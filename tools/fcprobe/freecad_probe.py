#!/usr/bin/env python3
"""freecad_probe - unified testing harness for the FreeCAD debug build.

This is the Tier-1 consolidation of the breadcrumb + pick-probe infrastructures
into one tool.  It has two roles in one module:

GUEST HARNESS
    Imported *inside* a probe script that runs within FreeCAD (launched via
    ``bin/FreeCAD probe.py``).  Provides viewport discovery, synthetic input,
    dpr-aware coordinate mapping, typed diagnostic emission, a snapshot helper,
    and a verdict accumulator -- replacing the duplicated helpers that have been
    copy-pasted across ~50 vk_*.py scripts.

HOST RUNNER
    When executed directly (``python3 freecad_probe.py run ...``) it launches
    FreeCAD with the right environment for a script/manifest, redirects the
    breadcrumb trace into an artifact bundle, collects stdout + frames, and
    writes a machine-readable ``report.json`` with a single exit code.

The FreeCAD/PySide imports are lazy so this module can be imported from plain
host Python too (for the parsers and report writer, which are pure Python).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterable, Iterator, List, Optional

# ---------------------------------------------------------------------------
# Unified diagnostic event schema
# ---------------------------------------------------------------------------
# Normalized event: {"source": ..., "kind": ..., "fields": {...}, "text": ...}
# Sources present in the FreeCAD logs:
#   PICKPROBE  ObjectIdentifier/hover-click diagnostics (SoFCUnifiedSelection)
#   VK-TRACE   Base::vulkanBreadcrumb stream (SoFCUnifiedSelection, Volume render)
#   VKBE       Vulkan backend debug (draw/UBO/PUSH/OVL summaries)
#   VK-VALIDATION  Khronos Vulkan validation layer messages (VUID- lines)
#   HARNESS    this module's own typed events
#
# The HARNESS line format is "kind NAME k1=v1 k2=v2 ..." and is meant to be a
# single, parseable record that mirrors the C-side log line conventions.

# Mapping of source -> known integer/float/quoted field names, used by the
# lenient key=value parser.  Any unrecognized token is kept as a raw field.
_PICKPROBE_FIELDS = {"event", "pos", "hit", "obj", "bbox", "local", "sub"}

# Substrings that identify an output line as a Khronos Vulkan validation-layer
# diagnostic (VUID = Vulkan Validation ID).  These are ranked by specificity so
# the scanner picks a stable classification; a line is classified as validation
# output when any non-empty marker matches.
_VALIDATION_MARKERS = (
    "VUID-",
    "UNASSIGNED-",
    "Validation Error",
    "Validation Warning",
    "vk",
)


def _split_kv(token: str) -> Optional[tuple[str, str]]:
    """Split ``key=value`` (key word chars only).  Returns None if not a kv."""
    if "=" not in token:
        return None
    key, _, value = token.partition("=")
    if not key or not (key[0].isalpha() or key[0] == "_") or not all(
        c.isalnum() or c in "_.-" for c in key
    ):
        return None
    return key, value


def parse_event(line: str) -> Optional[dict[str, Any]]:
    """Parse one log line into a normalized event dict, or None if not a known
    diagnostic source."""
    text = line.rstrip("\n")
    if text.startswith("[PICKPROBE]"):
        body = text[len("[PICKPROBE]") :].strip()
        ev = _parse_tagged("PICKPROBE", body, _PICKPROBE_FIELDS)
        # The PICKPROBE record's native "kind" is the `event=` field; surface it
        # so consumers can filter hover vs click uniformly.
        ev["kind"] = ev["fields"].get("event", "")
    elif text.startswith("[VK-TRACE]"):
        ev = {
            "source": "VK-TRACE",
            "kind": "breadcrumb",
            "fields": {},
            "text": text[len("[VK-TRACE]") :].strip(),
        }
    elif text.startswith("[VKBE]"):
        ev = {
            "source": "VKBE",
            "kind": "backend",
            "fields": {},
            "text": text[len("[VKBE]") :].strip(),
        }
    elif text.startswith("[VK-SET]"):
        ev = _parse_tagged("VK-SET", text[len("[VK-SET]") :].strip(), set())
    elif text.startswith("[OVL]"):
        ev = _parse_tagged("OVL", text[len("[OVL]") :].strip(), set())
    elif text.startswith("[PUSH]"):
        ev = _parse_tagged("PUSH", text[len("[PUSH]") :].strip(), set())
    elif text.startswith("[UBO]"):
        ev = _parse_tagged("UBO", text[len("[UBO]") :].strip(), set())
    elif text.startswith("[HARNESS]"):
        ev = _parse_tagged("HARNESS", text[len("[HARNESS]") :].strip(), set())
    elif text.startswith("[VERDICT]"):
        body = text[len("[VERDICT]") :].split()
        ev = {
            "source": "VERDICT",
            "kind": "verdict",
            "fields": {},
            "text": text[len("[VERDICT]") :].strip(),
        }
        if len(body) >= 2:
            ev["fields"] = {"name": body[0], "result": body[1]}
    elif _is_validation_line(text):
        ev = {
            "source": "VK-VALIDATION",
            "kind": "validation",
            "fields": _validation_fields(text),
            "text": text,
        }
    else:
        return None
    return ev


def _is_validation_line(text: str) -> bool:
    """True when a line looks like Khronos Vulkan validation-layer output."""
    # VUID identifiers are the definitive signal; "vk" alone is too greedy for
    # a general log, so only treat bare "vk..." as validation when the line also
    # mentions error/warning/validation.
    if "VUID-" in text or "UNASSIGNED-" in text:
        return True
    if "Validation Error" in text or "Validation Warning" in text:
        return True
    return False


def _validation_fields(text: str) -> dict[str, str]:
    import re

    fields: dict[str, str] = {}
    # Capture the VUID / UNASSIGNED identifier (alphanumerics, hyphens, dots).
    match = re.search(r"(?:VUID|UNASSIGNED)-[A-Za-z0-9_.-]+", text)
    if match:
        fields["vuid"] = match.group(0)
    if "Error" in text:
        fields["level"] = "ERROR"
    elif "Warning" in text:
        fields["level"] = "WARN"
    elif fields.get("vuid"):
        # A cited VUID (e.g. the "The Vulkan spec states:" tail of a validation
        # message) implies a spec violation even when the primary "Validation
        # Error" line was not captured — surface it as a warning, not INFO.
        fields["level"] = "WARN"
    else:
        fields["level"] = "INFO"
    return fields


def validation_summary(events: Iterable[dict[str, Any]]) -> dict[str, Any]:
    """Bucket validation diagnostic events by VUID code, returning per-VUID
    counts (the Khronos layer emits one message per violation, so the same
    VUID may repeat: here we aggregate instead of flooding the log)."""
    from collections import defaultdict
    sums: dict[str, dict[str, Any]] = defaultdict(
        lambda: {"count": 0, "levels": set(), "example": ""})
    for ev in events:
        f = ev.get("fields", {})
        vuid = f.get("vuid", "(unknown)")
        s = sums[vuid]
        s["count"] += 1
        s["levels"].add(f.get("level", "INFO"))
        if not s["example"]:
            s["example"] = ev.get("text", "")[:120]
    result = {}
    for vuid, s in sorted(sums.items(), key=lambda kv: -kv[1]["count"]):
        result[vuid] = {
            "count": s["count"],
            "levels": sorted(s["levels"]),
            "example": s["example"],
        }
    return result


def extract_validation(lines: Iterable[str]) -> list[dict[str, Any]]:
    """Return the Khronos VUID diagnostics found in `lines` as normalized events."""
    out = []
    for line in lines:
        ev = parse_event(line)
        if ev and ev["source"] == "VK-VALIDATION":
            out.append(ev)
    return out


# ---------------------------------------------------------------------------
# Tier 2 -- golden-frame, drawlist-hash and state-snapshot regression
# ---------------------------------------------------------------------------

def vkbe_lines(lines: Iterable[str]) -> list[dict[str, Any]]:
    """Return the Vulkan backend draw-command summary events ([VKBE] lines)."""
    return [ev for ev in iter_events(lines) if ev["source"] == "VKBE"]


def drawlist_digest(events: Iterable[dict[str, Any]]) -> str:
    """Canonical hash of a draw-command stream ([VKBE] records).

    The backend emits one [VKBE] line per recorded command with its material
    and push-constant state (draw/UBO/PUSH/OVL).  Canonicalizing those records
    and hashing them yields a fingerprint that is stable across identical runs
    and changes the instant any render state (geometry, material, overlay) does.
    """
    import hashlib
    h = hashlib.sha256()
    for ev in sorted(events, key=lambda e: e.get("text", "")):
        h.update((ev.get("text", "") + "\n").encode("utf-8", "replace"))
    return h.hexdigest()


def collect_frame_dumps(dest_dir: str, source_glob: str = "/tmp/vk_frame_*.png") -> list[str]:
    """Copy Vulkan frame dumps into `dest_dir/frames` and return the copied paths."""
    import glob
    import shutil
    frames_dir = os.path.join(dest_dir, "frames")
    os.makedirs(frames_dir, exist_ok=True)
    copied = []
    for src in sorted(glob.glob(source_glob)):
        dst = os.path.join(frames_dir, os.path.basename(src))
        shutil.copy2(src, dst)
        copied.append(dst)
    return copied


def file_sha256(path: str) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def frame_hashes(frames_dir: str) -> dict[str, str]:
    """Map frame filename -> content sha256 for every PNG in `frames_dir`."""
    import glob
    return {os.path.basename(p): file_sha256(p)
            for p in sorted(glob.glob(os.path.join(frames_dir, "*.png")))}


def count_color_pixels(path: str, rgb: tuple[int, int, int], tol: int = 12) -> int:
    """Count pixels within `tol` (max channel delta) of `rgb` in a PNG (RGB)."""
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(path).convert("RGB"), dtype=np.int32)
    target = np.array(rgb, dtype=np.int32)
    dist = np.abs(a - target).max(axis=2)
    return int((dist <= tol).sum())


def extract_vksett(events: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return the `[VK-SET] pushSettings ...` events (the settings-push record)."""
    return [ev for ev in events if ev.get("source") == "VK-SET"]


def _frame_num(path: str) -> int:
    import re
    m = re.search(r"(\d+)\.png", os.path.basename(path))
    return int(m.group(1)) if m else 0


def check_preferences(events: Iterable[dict[str, Any]],
                      frames_dir: str,
                      edge_rgb: tuple[int, int, int] = (255, 0, 0),
                      tol: int = 12, min_px: int = 50,
                      expected_edge_on: bool = True) -> list[str]:
    """Assert the Vulkan display prefs were read AND rendered.

    Code path: the ``applyVulkanSettings`` breadcrumb must have recorded the
    edges/points transitions.  Render: with the overlay on, at least one frame
    must contain enough ``edge_rgb`` pixels; with it off, some frame must have
    none.  Returns a list of error strings (empty == PASS).
    """
    import glob
    import os
    errors: list[str] = []
    evs = list(events)
    traces = [ev.get("text", "") for ev in evs
              if ev.get("source") == "VK-TRACE"
              and "applyVulkanSettings" in ev.get("text", "")]
    if not traces:
        errors.append("no applyVulkanSettings breadcrumb (code path never ran)")
    if expected_edge_on and not any("edges=1" in t for t in traces):
        errors.append("applyVulkanSettings never recorded edges=1")
    if any("edges=0" in t for t in traces) is False:
        errors.append("applyVulkanSettings never recorded edges=0 (baseline)")

    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")), key=_frame_num)
    if not frames:
        errors.append("no frame dumps to analyze")
        return errors
    counts = [count_color_pixels(f, edge_rgb, tol) for f in frames]
    if expected_edge_on:
        if max(counts) < min_px:
            errors.append(
                f"no frame renders edges in {edge_rgb}"
                f" (max {max(counts)} px, need >= {min_px})")
        if 0 not in counts:
            errors.append("no frame with 0 edge pixels (overlay may be stuck on)")
    return errors


def image_metrics(a_path: str, b_path: str) -> dict[str, Any]:
    """Pixel-diff two PNGs (RGB).  Returns mean abs diff + changed/area stats."""
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(a_path).convert("RGB"), dtype=np.int32)
    b = np.asarray(Image.open(b_path).convert("RGB"), dtype=np.int32)
    if a.shape != b.shape:
        return {"error": f"size mismatch {a.shape} vs {b.shape}"}
    diff = np.abs(a - b)
    maxd = diff.max(axis=2)
    return {
        "mean_abs": float(diff.mean()),
        "changed_frac": float((maxd > 0).mean()),
        "big_diff_pixels": int((maxd > 8).sum()),
        "max_delta": int(maxd.max()),
    }


def compare_frames(baseline_dir: str, frames_dir: str,
                   mean_threshold: float = 1.5,
                   big_threshold_px: int = 200) -> list[str]:
    """Compare a run's frame dumps against a baseline, returning error strings."""
    import glob
    base = sorted(glob.glob(os.path.join(baseline_dir, "*.png")))
    out = sorted(glob.glob(os.path.join(frames_dir, "*.png")))
    errors = []
    if not base and not out:
        return ["no frames to compare"]
    if len(base) != len(out):
        errors.append(
            f"frame count mismatch: baseline {len(base)} vs run {len(out)}"
        )
    for b, o in zip(base, out):
        m = image_metrics(b, o)
        if "error" in m:
            errors.append(f"{os.path.basename(o)}: {m['error']}")
        elif m["mean_abs"] > mean_threshold or m["big_diff_pixels"] > big_threshold_px:
            errors.append(
                f"{os.path.basename(o)}: mean_abs={m['mean_abs']:.2f} "
                f"(> {mean_threshold}) big_diff_pixels={m['big_diff_pixels']} "
                f"(> {big_threshold_px})"
            )
    return errors


def extract_snapshot(events: Iterable[dict[str, Any]]) -> Optional[dict[str, Any]]:
    """Rehydrate the first `[HARNESS] snapshot state=<json>` event into a dict."""
    for ev in events:
        if ev.get("source") == "HARNESS" and ev.get("kind") == "snapshot":
            raw = ev.get("fields", {}).get("state")
            if raw:
                try:
                    return json.loads(raw)
                except (ValueError, TypeError):
                    return {"_raw": raw}
    return None


# ---------------------------------------------------------------------------
# Tier 3 -- pick-trace comparison, run diffing and parity matrix
# ---------------------------------------------------------------------------

def _floats(value: str, n: int, default: float = 0.0) -> list[float]:
    """Parse a comma-separated list of numbers (e.g. pos='100,200', hit='1,2,3')."""
    out = []
    for part in str(value).split(","):
        try:
            out.append(float(part))
        except (TypeError, ValueError):
            out.append(default)
    while len(out) < n:
        out.append(default)
    return out[:n]


def pick_trace_from_log(lines: Iterable[str]) -> list[dict[str, Any]]:
    """Extract the structured [PICKPROBE] event fields from a log line stream."""
    return [ev["fields"] for ev in iter_events(lines) if ev["source"] == "PICKPROBE"]


def diff_pick_traces(a: list[dict[str, Any]], b: list[dict[str, Any]],
                     pos_tol: float = 1.0, hit_tol: float = 1e-3,
                     slack: int = 2) -> list[str]:
    """Compare two pick-probe traces, returning human-readable differences.

    Matches entries positionally (hover/click, in order) and compares the mouse
    position, event kind, picked object, and scene-space hit point within the
    given tolerances.  Because the sweep is deterministic this detects pick /
    hover boundary drift between two runs (e.g. Vulkan vs GL or before/after a
    fix).
    """
    errors: list[str] = []
    if len(a) != len(b):
        errors.append(f"pick event count differs: {len(a)} vs {len(b)}")
    n = min(len(a), len(b))
    for i in range(n):
        x, y = a[i], b[i]
        ka = x.get("event", ""); kb = y.get("event", "")
        pa = _floats(x.get("pos", ""), 2); pb = _floats(y.get("pos", ""), 2)
        oa = x.get("obj", "-"); ob = y.get("obj", "-")
        ha = _floats(x.get("hit", ""), 3); hb = _floats(y.get("hit", ""), 3)
        a_has_hit = x.get("hit") is not None
        b_has_hit = y.get("hit") is not None
        if ka != kb:
            errors.append(f"[{i}] event kind {ka} vs {kb}")
        if abs(pa[0] - pb[0]) > pos_tol or abs(pa[1] - pb[1]) > pos_tol:
            errors.append(f"[{i}] pos {pa} vs {pb}")
        if a_has_hit != b_has_hit:
            errors.append(f"[{i}] hit presence {a_has_hit} vs {b_has_hit} "
                          f"(obj {oa} vs {ob})")
        elif a_has_hit and oa != ob:
            errors.append(f"[{i}] object {oa} vs {ob}")
        elif a_has_hit:
            d = max(abs(ha[j] - hb[j]) for j in range(3))
            if d > hit_tol + slack * 0.0:
                errors.append(f"[{i}] hit {ha} vs {hb} (d={d:.4f})")
    return errors


def _load_report(artifact_dir: str) -> Optional[dict[str, Any]]:
    path = os.path.join(artifact_dir, "report.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def diff_runs(a_dir: str, b_dir: str,
              pick_pos_tol: float = 1.0, hit_tol: float = 1e-3) -> list[str]:
    """Diff two run bundles (report.json), including their pick-probe traces."""
    ra, rb = _load_report(a_dir), _load_report(b_dir)
    errors: list[str] = []
    if ra is None or rb is None:
        return ["missing report.json in one or both bundles"]
    if ra.get("verdict") != rb.get("verdict"):
        errors.append(f"verdict {ra.get('verdict')} vs {rb.get('verdict')}")
    sa, sb = ra.get("session", {}), rb.get("session", {})
    ha, hb = sa.get("drawlist_hash"), sb.get("drawlist_hash")
    if ha and hb and ha != hb:
        errors.append(f"drawlist hash {ha[:12]} vs {hb[:12]}")
    if sa.get("validation_count") != sb.get("validation_count"):
        errors.append(f"validation {sa.get('validation_count')} vs {sb.get('validation_count')}")
    if "state" in sa and "state" in sb and sa["state"] != sb["state"]:
        errors.append(f"state snapshot {sa['state']} vs {sb['state']}")

    # Compare pick traces extracted from each bundle's stdout.log.
    def _ta(d: str, report: dict) -> list[dict[str, Any]]:
        p = os.path.join(d, "stdout.log")
        if os.path.exists(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                return pick_trace_from_log(f)
        return report.get("events", []) and [
            {k: v for k, v in e.get("fields", {}).items()}
            for e in report["events"] if e.get("source") == "PICKPROBE"
        ]

    errors.extend(diff_pick_traces(_ta(a_dir, ra), _ta(b_dir, rb),
                                   pos_tol=pick_pos_tol, hit_tol=hit_tol))
    return errors


def run_matrix(script: str, profiles: Iterable[str] = ("vulkan", "gl"),
               out_dir: str = "/tmp/opencode/matrix",
               binary: Optional[str] = None,
               env_overrides: Optional[dict[str, str]] = None,
               timeout: int = 120, report_name: Optional[str] = None,
               validation: bool = False) -> dict[str, Any]:
    """Run `script` under several profiles and diff each pair (parity matrix).

    Returns {profiles: {profile: RunReport-ish}, pairs: [(a,b,errors)]}.  This
    is the Vulkan-vs-GL parity harness: the same deterministic probe is executed
    with `--profile vulkan` and `--profile gl` and their pick traces, drawlist
    hash, state and verdicts are compared.
    """
    binary = binary or _DEFAULT_FREECAD
    reports: dict[str, RunReport] = {}
    for prof in profiles:
        reports[prof] = run_case(
            script=script, binary=binary, profile=prof,
            env_overrides=env_overrides, out_dir=out_dir,
            timeout=timeout, report_name=report_name or f"{os.path.basename(script)}[{prof}]",
            validation=validation,
        )
    pairs: list[tuple[str, str, list[str]]] = []
    plist = list(profiles)
    for i in range(len(plist)):
        for j in range(i + 1, len(plist)):
            a, b = plist[i], plist[j]
            errors = diff_runs(reports[a].artifact_dir, reports[b].artifact_dir)
            pairs.append((a, b, errors))
    return {"reports": reports, "pairs": pairs}


def tally_events(events: Iterable[dict[str, Any]]) -> dict[str, int]:
    """Count normalized diagnostic events by (source, kind) — a crude coverage /
    activity tally of which diagnostics actually fired in a run."""
    from collections import Counter
    c: Counter = Counter()
    for ev in events:
        c[(ev.get("source", "?"), ev.get("kind", "?"))] += 1
    return {f"{s}:{k}": n for (s, k), n in sorted(c.items())}


def run_to_fail(script: str, max_runs: int = 10,
                out_dir: str = "/tmp/opencode/soak",
                binary: Optional[str] = None, profile: str = "vulkan",
                env_overrides: Optional[dict[str, str]] = None,
                timeout: int = 120, validation: bool = False,
                stop_on_fail: bool = True) -> dict[str, Any]:
    """Run `script` repeatedly until it stops passing (or a native crash).

    Each iteration launches a fresh FreeCAD process (fresh state), so this is
    the harness's soak / run-to-fail driver: it reproduces intermittent crashes
    and surfaces a signal/exit/verdict per run.  Returns a report dict with the
    per-run results and an overall verdict.
    """
    binary = binary or _DEFAULT_FREECAD
    base = os.path.splitext(os.path.basename(script))[0]
    runs: list[dict[str, Any]] = []
    crashes = 0
    for i in range(max_runs):
        rep = run_case(script, binary=binary, profile=profile,
                       env_overrides=env_overrides, out_dir=out_dir,
                       timeout=timeout, report_name=f"{base}-soak{i}",
                       validation=validation)
        signal = rep.session.get("exit_signal")
        runs.append({
            "run": i,
            "verdict": rep.verdict,
            "exit_code": rep.session.get("exit_code"),
            "signal": signal,
            "errors": list(rep.errors),
            "artifact_dir": rep.artifact_dir,
        })
        if signal:
            crashes += 1
        if stop_on_fail and rep.verdict != "PASS":
            break
    ok = all(r["verdict"] == "PASS" and not r["signal"] for r in runs)
    return {"runs": runs, "crashes": crashes, "total_runs": len(runs),
            "ok": ok, "max_runs": max_runs}


def _parse_tagged(source: str, body: str, known: set[str]) -> dict[str, Any]:
    """Parse `kind key=value ...` (and thus allow bare tokens to be grouped)."""
    ev: dict[str, Any] = {"source": source, "kind": "", "fields": {}, "text": body}
    tokens = body.split()
    if not tokens:
        return ev
    # First token(s) before the first k=v form the kind.  Keep it simple: the
    # leading free-form word is the kind; everything else is fields.
    idx = 0
    while idx < len(tokens) and not any("=" in t for t in tokens[idx : idx + 1]):
        idx += 1
    # Recompute: kind = everything before the first k=v token.
    first_kv = next((i for i, t in enumerate(tokens) if "=" in t), len(tokens))
    ev["kind"] = " ".join(tokens[:first_kv]) if first_kv else (tokens[0] if tokens else "")
    for token in tokens[first_kv:]:
        kv = _split_kv(token)
        if kv:
            ev["fields"][kv[0]] = kv[1]
        else:
            ev["fields"].setdefault("_extra", [])
            ev["fields"]["_extra"].append(token)  # type: ignore[union-attr]
    return ev


def iter_events(lines: Iterable[str]) -> Iterator[dict[str, Any]]:
    """Yield normalized events from an iterable of log lines."""
    for line in lines:
        ev = parse_event(line)
        if ev is not None:
            yield ev


def format_event(ev: dict[str, Any]) -> str:
    """Render a normalized event back to a canonical single-line record."""
    parts = []
    if ev.get("kind"):
        parts.append(str(ev["kind"]))
    for k, v in ev.get("fields", {}).items():
        if k == "_extra":
            parts.extend(str(x) for x in v)
        else:
            parts.append(f"{k}={v}")
    text = ev.get("text") or " ".join(parts)
    return f"[{ev['source']}] {text}"


# ---------------------------------------------------------------------------
# Artifact bundle + JSON report
# ---------------------------------------------------------------------------
@dataclass
class RunReport:
    """Collects the outcome of one harness run and writes it to an artifact dir."""

    name: str
    artifact_dir: str
    started: float = field(default_factory=time.time)
    verdict: str = "FAIL"  # PASS | FAIL | ERROR | TIMEOUT
    errors: List[str] = field(default_factory=list)
    events: List[dict[str, Any]] = field(default_factory=list)
    session: Dict[str, Any] = field(default_factory=dict)
    artifacts: List[str] = field(default_factory=list)
    # True once an explicit verdict was stamped (mark).  Used so a later
    # "probe printed PASS" decision at the end of run_case cannot clobber a
    # FAIL/ERROR already set by validation gating or a frame regression.
    _verdict_set: bool = field(default=False, repr=False)

    def log_event(self, source: str, kind: str, **fields: Any) -> None:
        self.events.append({"source": source, "kind": kind, "fields": fields})

    def add_error(self, fmt: str, *args: Any) -> None:
        self.errors.append(fmt % args if args else fmt)

    def mark(self, verdict: str) -> None:
        self.verdict = verdict
        self._verdict_set = True

    def register(self, path: str) -> None:
        """Record an artifact file (relative to artifact_dir)."""
        if os.path.isabs(path):
            path = os.path.relpath(path, self.artifact_dir)
        if path not in self.artifacts:
            self.artifacts.append(path)

    def write(self) -> str:
        """Write report.json and return its path."""
        if "report.json" not in self.artifacts:
            self.artifacts.append("report.json")
        payload = {
            "name": self.name,
            "started": self.started,
            "duration_ms": int((time.time() - self.started) * 1000),
            "verdict": self.verdict,
            "errors": self.errors,
            "events": self.events,
            "session": self.session,
            "artifacts": self.artifacts,
        }
        path = os.path.join(self.artifact_dir, "report.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        return path


def new_artifact_dir(parent: str, name: str) -> str:
    """Create a fresh artifact directory `parent/<name>-<ts>[-<rand>]`."""
    ts = time.strftime("%Y%m%d-%H%M%S")
    run_id = f"{name or 'run'}-{ts}-{uuid.uuid4().hex[:6]}"
    path = os.path.join(parent, run_id)
    os.makedirs(path, exist_ok=False)
    return path


# ---------------------------------------------------------------------------
# Host runner
# ---------------------------------------------------------------------------
# Default environment presets.  These match the working harness configuration
# for the Vulkan debug build on this machine.
_PROFILES = {
    "vulkan": {
        "QT_STYLE_OVERRIDE": "fusion",
        "QT_QPA_PLATFORM": "xcb",
        "LD_LIBRARY_PATH": "/tmp/opencode/boost91",
        "FC_SKIP_UNSAVED_PROMPT": "1",
        "FC_VULKAN_BREADCRUMBS": "1",
    },
    "gl": {
        "QT_STYLE_OVERRIDE": "fusion",
        "QT_QPA_PLATFORM": "xcb",
        "LD_LIBRARY_PATH": "/tmp/opencode/boost91",
        "FC_SKIP_UNSAVED_PROMPT": "1",
    },
}

_DEFAULT_FREECAD = "/home/phantom/dev/FreeCAD/build/debug/bin/FreeCAD"

# Set on every harness-launched FreeCAD process. Lets both the host runner and
# any guest-side probe logic know they are running under the test harness, so
# harness-only behaviors (like "close on probe error") can be gated and can
# never fire in an end user's normal interactive FreeCAD session.
_HARNESS_MARKER = "FC_HARNESS"


def _env_dict(env_list: Iterable[str]) -> dict[str, str]:
    """Parse `["K=V", ...]` (as passed by `--env`) into a dict."""
    out: dict[str, str] = {}
    for item in env_list:
        key, _, val = item.partition("=")
        out[key] = val
    return out


def _merge_env(
    profile: str,
    overrides: dict[str, str],
    trace_path: Optional[str],
    validation: bool = False,
    layer_path: Optional[str] = None,
) -> dict[str, str]:
    env = dict(os.environ)
    env.update(_PROFILES.get(profile, {}))
    env[_HARNESS_MARKER] = "1"  # mark this as a harness launch, never a user session
    if trace_path:
        env["FC_VULKAN_TRACE_FILE"] = trace_path
    if validation:
        # Qt (QuarterVulkanWidget) enables VK_LAYER_KHRONOS_validation when this
        # flag is set; the Khronos VUID diagnostics are emitted on stderr and are
        # captured by the runner for classification.
        env["FC_VULKAN_VALIDATION"] = "1"
    if layer_path:
        env["VK_LAYER_PATH"] = layer_path
    env.update({k: v for k, v in overrides.items() if v is not None})
    return env


def run_case(
    script: str,
    binary: str = _DEFAULT_FREECAD,
    profile: str = "vulkan",
    env_overrides: Optional[dict[str, str]] = None,
    out_dir: str = "/tmp/opencode/runs",
    timeout: int = 120,
    report_name: Optional[str] = None,
    validation: bool = False,
    layer_path: Optional[str] = None,
    fail_on_validation: bool = False,
    allow_vuid: Optional[Iterable[str]] = None,
    baseline_dir: Optional[str] = None,
    frame_mean_threshold: float = 1.5,
    frame_big_threshold_px: int = 200,
) -> RunReport:
    """Launch FreeCAD with `script`, collect artifacts, and return the report.

    This is the host-side entry point used by the `run` subcommand.  It does
    not require FreeCAD to be importable from host Python.

    Tier-2 behavior: collected frame dumps and the Vulkan draw-command stream
    ([VKBE]) are fingerprinted and placed in the report.  When ``baseline_dir``
    points at a previous run's ``frames/``, each frame is pixel-compared and any
    frame exceeding the thresholds is recorded as an error (golden regression).
    """
    script = os.path.abspath(script)
    name = report_name or os.path.splitext(os.path.basename(script))[0]
    artifact_dir = new_artifact_dir(out_dir, name)
    trace_path = os.path.join(artifact_dir, "trace.log")
    report = RunReport(name=name, artifact_dir=artifact_dir)
    report.session.update(
        {
            "script": script,
            "binary": binary,
            "profile": profile,
            "env_overrides": env_overrides or {},
            "validation": validation,
            "layer_path": layer_path,
            "fail_on_validation": fail_on_validation,
            "allow_vuid": sorted(set(allow_vuid or ())),
            "baseline_dir": baseline_dir,
        }
    )

    env = _merge_env(profile, env_overrides or {}, trace_path, validation, layer_path)
    report.register(trace_path)

    stdout_path = os.path.join(artifact_dir, "stdout.log")
    report.register(stdout_path)
    with open(stdout_path, "w", encoding="utf-8", errors="replace") as outf:
        proc = subprocess.Popen(
            [binary, script],
            cwd="/home/phantom/dev/FreeCAD",
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        # Stream to both the artifact file and memory for parsing.  A reader
        # thread feeds the log; the main loop enforces a hard deadline and
        # shuts FreeCAD down as soon as a terminal error appears so a dead
        # probe doesn't idle the GUI out the whole timeout.
        lines: List[str] = []
        terminal_reason: Optional[str] = None

        def reader() -> None:
            nonlocal terminal_reason
            for line in proc.stdout:  # type: ignore[union-attr]
                lines.append(line)
                outf.write(line)
                if terminal_reason is None and _is_terminal_error(line):
                    terminal_reason = line.strip()

        reader_thread = threading.Thread(target=reader, daemon=True)
        reader_thread.start()

        deadline = time.time() + timeout
        rc: Optional[int] = None
        probe_died = False
        while True:
            now = time.time()
            if terminal_reason and env.get(_HARNESS_MARKER) == "1":
                # Harness run only (never a user's interactive session): the
                # probe is dead, so close FreeCAD now so the tool can report
                # instead of idling the GUI out the whole timeout.
                probe_died = True
                try:
                    proc.terminate()
                    rc = proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    rc = proc.wait()
                break
            if not reader_thread.is_alive():
                rc = proc.poll()
                if rc is None:
                    # stdout closed but child lingers briefly; give a grace
                    try:
                        rc = proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        rc = proc.wait()
                break
            if now >= deadline:
                proc.kill()
                rc = proc.wait()
                report.add_error("timeout after %ss", timeout)
                report.mark("TIMEOUT")
                break
            time.sleep(0.1)
        reader_thread.join(timeout=10)

        # Parse the unified event stream from stdout for the report.
        for ev in iter_events(lines):
            report.events.append(ev)
        if rc and rc != 0 and not probe_died:
            report.add_error("process exited with code %s", rc)

    # Fold the breadcrumb trace events into the report too.
    if os.path.exists(trace_path):
        with open(trace_path, encoding="utf-8", errors="replace") as tf:
            for ev in iter_events(tf):
                report.events.append(ev)

    # Khronos Vulkan validation diagnostics (VUID- lines) are emitted on
    # stderr/stdout; classify them and surface them in the report.
    validation_events = extract_validation(lines)
    report.session["validation"] = validation
    report.session["validation_count"] = len(validation_events)
    report.session["validation_summary"] = validation_summary(validation_events)
    allow = set(str(v) for v in (allow_vuid or ()))
    # Diagnostics for allow-listed VUIDs are still reported but never fail the
    # run; e.g. FreeCAD's swapchain acquire/present is owned by Qt's
    # QVulkanWindow, so the Qt-side present/semaphore VUIDs there are expected
    # and must not gate on regressions in FreeCAD's own code.
    notable = [ev for ev in validation_events
               if ev.get("fields", {}).get("vuid", "") not in allow]
    report.session["validation_notable_count"] = len(notable)
    for ev in validation_events:
        report.events.append(ev)
    for ev in notable:
        level = ev.get("fields", {}).get("level", "INFO")
        if level == "ERROR":
            report.add_error("Vulkan validation: %s", ev.get("text", "")[:200])
    if fail_on_validation and notable:
        report.add_error("Vulkan validation produced %d diagnostics",
                         len(notable))
        report.mark("FAIL")

    # -- Tier 2: draw-command fingerprint (drawlist hash) ------------------
    vkbe = vkbe_lines(lines)
    report.session["vkbe_count"] = len(vkbe)
    report.session["drawlist_hash"] = drawlist_digest(vkbe)

    # -- Tier 2: frame dumps -> copy into bundle + fingerprint -------------
    frames = collect_frame_dumps(artifact_dir)
    if frames:
        reports_frames = os.path.join(artifact_dir, "frames")
        for f in frames:
            report.register(f)
        if os.path.isdir(reports_frames):
            report.session["frame_hashes"] = frame_hashes(reports_frames)
            if baseline_dir:
                base_frames = baseline_dir
                if not base_frames.endswith("/frames") and os.path.isdir(
                    os.path.join(base_frames, "frames")
                ):
                    base_frames = os.path.join(base_frames, "frames")
                frame_errors = compare_frames(
                    base_frames, reports_frames,
                    mean_threshold=frame_mean_threshold,
                    big_threshold_px=frame_big_threshold_px,
                )
                for fe in frame_errors:
                    report.add_error("frame regression: %s", fe)
                if frame_errors:
                    report.mark("FAIL")

    # -- Tier 2: state snapshot rehydrate ---------------------------------
    snap = extract_snapshot(report.events)
    if snap is not None:
        report.session["state"] = snap

    # -- crash / signal detection + verbose tail ---------------------------
    # On POSIX subprocess returns a negative returncode when the child died of
    # a signal (e.g. SIGSEGV = -11), which is exactly what a native crash looks
    # like.  Surface it and keep a short tail of stdout for diagnostics.
    # `probe_died` means WE terminated the child after a probe error, so its
    # negative rc is not a native crash.
    if rc < 0:
        if probe_died:
            report.session["exit_signal"] = None
        else:
            report.session["exit_signal"] = -rc
            report.add_error("process killed by signal -%d (native crash)",
                             -rc)
            report.mark("ERROR")
    else:
        report.session["exit_signal"] = None
    tail = "".join(lines[-60:])
    report.session["stdout_tail"] = tail[-20000:]

    report.session["exit_code"] = rc
    report.session["verdict_line"] = extract_verdict(lines)
    if extract_verdict(lines) == "PASS":
        if not report._verdict_set:
            report.mark("PASS")
    elif report.verdict not in ("ERROR", "TIMEOUT", "FAIL"):
        report.add_error("no VERDICT PASS line found")

    # -- console error capture (probe exception / FreeCAD error) -----------
    # The FreeCAD GUI writes probe load/run failures to its console
    # ("Exception while processing file: ...") and its Base::Console().error
    # output.  Surface these even when no [VERDICT] line is ever printed.
    console_errs = _console_errors(lines)
    if console_errs:
        for e in console_errs:
            report.add_error(e)
        if report.verdict not in ("ERROR", "TIMEOUT"):
            report.mark("FAIL")

    report.write()
    return report


def _is_terminal_error(line: str) -> bool:
    """True if a streamed line means the probe can no longer continue.

    FreeCAD prints ``Exception while processing file: <script>`` when a probe
    dies at load/exec time.  At that point the probe is dead, so the run is
    useless and the idle GUI should be closed immediately rather than wait out
    the whole timeout.
    """
    return any(m in line for m in ("Exception while processing file:",))


def _console_errors(lines: Iterable[str]) -> list[str]:
    """Extract error diagnostics the FreeCAD console writes to stdout/stderr.

    The most common is a probe that failed at load/start: FreeCAD reports
    ``Exception while processing file: <script>`` (or a Python ``Traceback``).
    We collect the marker, any indented traceback frames, and the trailing
    non-indented exception summary, stopping at the first blank line.
    (``Base::Console().error`` output also lands here.)
    """
    markers = ("Exception while processing file:", "Traceback (most recent call last):")
    result: list[str] = []
    collecting = False
    seen_frame = False
    for line in lines:
        s = line.rstrip("\n")
        if any(m in s for m in markers):
            collecting = True
            result.append(s)
            seen_frame = False
            continue
        if not collecting:
            continue
        if not s.strip():
            break
        if s[:1] in (" ", "\t"):
            result.append(s)
            seen_frame = True
        elif seen_frame:
            # non-indented line right after frames = the exception summary
            result.append(s)
            break
        else:
            break
    return result


def extract_verdict(lines: Iterable[str]) -> str:
    """Scan log lines for a verdict record and return the result ("PASS"/"FAIL").

    Recognizes both the unified ``[VERDICT] NAME PASS|FAIL`` record and the
    legacy probe style ``<PREFIX> VERDICT NAME PASS|FAIL`` (e.g.
    ``PICKHARNESS VERDICT PICKPROBE PASS``).  Returns "" if none found.
    """
    for line in lines:
        if line.startswith("[VERDICT]"):
            tokens = line.split()
            if len(tokens) >= 3:
                return tokens[-1]
        # legacy: a bare "VERDICT <...> PASS|FAIL" token anywhere in the line
        m = re.search(r"VERDICT\s+\S+\s+(PASS|FAIL)\b", line)
        if m:
            return m.group(1)
    return ""


def _cli(argv: List[str]) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.command == "run":
        report = run_case(
            script=args.script,
            binary=args.binary,
            profile=args.profile,
            env_overrides=_env_dict(args.env),
            out_dir=args.out,
            timeout=args.timeout,
            report_name=args.name,
            validation=args.validation,
            layer_path=args.layer_path,
            fail_on_validation=args.fail_on_validation,
            allow_vuid=args.allow_vuid,
            baseline_dir=args.baseline,
            frame_mean_threshold=args.frame_mean_threshold,
            frame_big_threshold_px=args.frame_big_threshold_px,
        )
        # -- self-passing regression: assert the Vulkan display prefs were
        #    both read (applyVulkanSettings breadcrumb) and rendered (frames).
        if args.check_preferences and hasattr(args, "edge_color"):
            edge_rgb = tuple(int(x) for x in args.edge_color.split(","))
            pferrs = check_preferences(
                report.events,
                os.path.join(report.artifact_dir, "frames"),
                edge_rgb=edge_rgb, min_px=args.min_edge_px)
            for e in pferrs:
                report.add_error(e)
            if pferrs:
                report.mark("FAIL")
                report.write()
        print(f"[RUN] artifact_dir={report.artifact_dir}")
        print(f"[RUN] verdict={report.verdict}")
        print(f"[RUN] drawlist_hash={report.session.get('drawlist_hash', '')[:16]}")
        print(f"[RUN] vkbe_count={report.session.get('vkbe_count')} "
              f"validation={report.session.get('validation_count')} "
              f"notable={report.session.get('validation_notable_count')}")
        allowed = set(args.allow_vuid or ())
        for vuid, s in (report.session.get("validation_summary") or {}).items():
            lv = ",".join(s.get("levels", []))
            tag = " (allowed)" if vuid in allowed else ""
            print(f"[RUN] VALIDATION {vuid} count={s['count']} level={lv}{tag}")
        for e in report.errors:
            print(f"[RUN] ERROR {e}")
        return 0 if report.verdict == "PASS" else 1
    if args.command == "report":
        return _cli_report(args)
    if args.command == "matrix":
        profiles = [p.strip() for p in args.profiles.split(",") if p.strip()]
        result = run_matrix(
            script=args.script, profiles=profiles, out_dir=args.out,
            binary=args.binary, env_overrides=_env_dict(args.env),
            timeout=args.timeout, report_name=args.name, validation=args.validation,
        )
        failed = False
        for a, b, errors in result["pairs"]:
            if errors:
                failed = True
            print(f"[MATRIX] {a} vs {b}: {'DIFF' if errors else 'MATCH'}")
            for e in errors:
                print(f"[MATRIX]   {e}")
        return 1 if failed else 0
    if args.command == "compare":
        errors = diff_runs(args.a, args.b, pick_pos_tol=args.pos_tol,
                           hit_tol=args.hit_tol)
        for e in errors:
            print(f"[compare] {e}")
        print(f"[compare] {'DIFF' if errors else 'MATCH'}")
        return 1 if errors else 0
    if args.command == "soak":
        result = run_to_fail(
            script=args.script, max_runs=args.max_runs, out_dir=args.out,
            binary=args.binary, profile=args.profile,
            env_overrides=_env_dict(args.env), timeout=args.timeout,
            validation=args.validation, stop_on_fail=not args.no_stop,
        )
        for r in result["runs"]:
            sig = f" signal={r['signal']}" if r["signal"] else ""
            print(f"[SOAK] run {r['run']} verdict={r['verdict']} "
                  f"exit={r['exit_code']}{sig}")
            for e in r["errors"]:
                print(f"[SOAK]   {e}")
        print(f"[SOAK] total={result['total_runs']} crashes={result['crashes']} "
              f"ok={result['ok']}")
        return 0 if result["ok"] else 1
    return 2


def _cli_report(args: Any) -> int:
    """Summarize one or more artifact bundle(s), optionally against a baseline."""
    if args.artifact_dir:
        _print_report(args.artifact_dir)
    return 0


def _print_report(artifact_dir: str) -> None:
    report_path = os.path.join(artifact_dir, "report.json")
    if not os.path.exists(report_path):
        print(f"[report] no report.json in {artifact_dir}")
        return
    with open(report_path, encoding="utf-8") as f:
        data = json.load(f)
    print(f"[report] name={data.get('name')} verdict={data.get('verdict')} "
          f"duration_ms={data.get('duration_ms')}")
    sess = data.get("session", {})
    if sess.get("drawlist_hash"):
        print(f"[report] drawlist_hash={sess['drawlist_hash'][:16]} "
              f"vkbe_count={sess.get('vkbe_count')}")
    if sess.get("validation_count"):
        print(f"[report] validation_count={sess['validation_count']} "
              f"notable={sess.get('validation_notable_count')}")
        allowed = set(sess.get("allow_vuid") or ())
        for vuid, s in (sess.get("validation_summary") or {}).items():
            tag = " (allowed)" if vuid in allowed else ""
            print(f"[report]   VALIDATION {vuid} count={s['count']} "
                  f"level={','.join(s['levels'])}{tag}")
    if "state" in sess:
        print(f"[report] state={json.dumps(sess['state'], separators=(',', ':'))}")
    if sess.get("frame_hashes"):
        print(f"[report] frames={len(sess['frame_hashes'])}")
    for e in data.get("errors", []):
        print(f"[report] ERROR {e}")


def _build_parser() -> Any:
    import argparse

    p = argparse.ArgumentParser(
        prog="freecad_probe",
        description="Unified FreeCAD testing harness (breadcrumbs + probes)",
    )
    sub = p.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="launch FreeCAD with a probe script/manifest")
    run.add_argument("script", help="path to a probe script (or .json manifest)")
    run.add_argument("--binary", default=_DEFAULT_FREECAD, help="FreeCAD binary")
    run.add_argument("--profile", default="vulkan", choices=sorted(_PROFILES))
    run.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="K=V",
        help="extra environment variable, repeatable",
    )
    run.add_argument("--out", default="/tmp/opencode/runs", help="artifact dir parent")
    run.add_argument("--name", default=None, help="run/artifact name")
    run.add_argument("--timeout", type=int, default=120, help="seconds before kill")
    run.add_argument(
        "--validation",
        action="store_true",
        help="enable the Khronos Vulkan validation layer (FC_VULKAN_VALIDATION)",
    )
    run.add_argument(
        "--layer-path",
        default=None,
        metavar="DIR",
        help="set VK_LAYER_PATH to the bundled validation layer",
    )
    run.add_argument(
        "--fail-on-validation",
        action="store_true",
        help="mark the run FAIL if any Vulkan validation diagnostic is emitted",
    )
    run.add_argument(
        "--allow-vuid",
        action="append",
        default=[],
        metavar="VUID",
        help="suppress a VUID diagnostic (does not fail the run); repeatable",
    )
    run.add_argument(
        "--baseline",
        default=None,
        metavar="DIR",
        help="a prior run's artifact dir (or its frames/) to golden-compare frames against",
    )
    run.add_argument(
        "--frame-mean-threshold",
        type=float,
        default=1.5,
        help="max mean abs pixel diff per frame before FAIL (default 1.5)",
    )
    run.add_argument(
        "--frame-big-threshold-px",
        type=int,
        default=200,
        help="max pixels differing by >8 per frame before FAIL (default 200)",
    )
    run.add_argument(
        "--check-preferences",
        action="store_true",
        help="assert Vulkan display prefs were read (applyVulkanSettings trace) "
             "and rendered (edge-colored pixels) — a self-passing regression",
    )
    run.add_argument(
        "--edge-color",
        default="255,0,0",
        metavar="R,G,B",
        help="edge/overlay color to look for with --check-preferences (default 255,0,0)",
    )
    run.add_argument(
        "--min-edge-px",
        type=int,
        default=50,
        help="minimum edge-colored pixels for --check-preferences (default 50)",
    )
    rep = sub.add_parser("report", help="summarize an artifact dir's report.json")
    rep.add_argument("artifact_dir", help="path to an artifact directory (run bundle)")

    mtx = sub.add_parser("matrix", help="run a probe across profiles and diff each pair")
    mtx.add_argument("script", help="path to a probe script")
    mtx.add_argument("--binary", default=_DEFAULT_FREECAD, help="FreeCAD binary")
    mtx.add_argument("--profiles", default="vulkan,gl",
                     help="comma-separated profiles to compare (default vulkan,gl)")
    mtx.add_argument("--out", default="/tmp/opencode/matrix", help="artifact dir parent")
    mtx.add_argument("--name", default=None, help="run/artifact name prefix")
    mtx.add_argument("--timeout", type=int, default=120, help="seconds per run")
    mtx.add_argument("--validation", action="store_true",
                     help="enable the Khronos Vulkan validation layer")
    mtx.add_argument(
        "--env", action="append", default=[], metavar="K=V",
        help="extra environment variable, repeatable",
    )

    cmp_ = sub.add_parser("compare", help="diff two run bundles (report.json + pick traces)")
    cmp_.add_argument("a", help="first artifact dir")
    cmp_.add_argument("b", help="second artifact dir")
    cmp_.add_argument("--pos-tol", type=float, default=1.0, help="pick position tolerance (px)")
    cmp_.add_argument("--hit-tol", type=float, default=1e-3, help="pick hit tolerance (mm)")

    soak = sub.add_parser("soak", help="run a probe until it stops passing or crashes")
    soak.add_argument("script", help="path to a probe script")
    soak.add_argument("--max-runs", type=int, default=10, help="max iterations (default 10)")
    soak.add_argument("--binary", default=_DEFAULT_FREECAD, help="FreeCAD binary")
    soak.add_argument("--profile", default="vulkan", choices=sorted(_PROFILES))
    soak.add_argument("--out", default="/tmp/opencode/soak", help="artifact parent dir")
    soak.add_argument("--timeout", type=int, default=120, help="seconds per run")
    soak.add_argument("--no-stop", action="store_true",
                      help="do not stop after the first failing run")
    soak.add_argument("--validation", action="store_true",
                      help="enable the Khronos Vulkan validation layer")
    soak.add_argument("--env", action="append", default=[], metavar="K=V",
                      help="extra environment variable, repeatable")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    return _cli(argv)


# ---------------------------------------------------------------------------
# Guest harness (runs *inside* FreeCAD)
# ---------------------------------------------------------------------------
# Everything below the line is only used by probe scripts that run within a
# FreeCAD process.  It lazily imports FreeCAD / FreeCADGui / PySide so that
# importing this module from host Python stays side-effect free.

class Session:
    """One probe session: viewport session + synthetic input + verdict.

    Typical guest usage::

        s = Session("pick")
        s.fit("Box")                      # activate Part workbench, fit view
        s.move(...); s.click(...); s.menu("Select All"); s.command("Std_New")
        s.verdict(ok)                     # -> [VERDICT] <name> PASS|FAIL
        s.close()
    """

    def __init__(self, name: str = "run", record: bool = False):
        self.name = name
        self.errors: List[str] = []
        self.record_enabled = record
        self.recorded: List[tuple[Any, ...]] = []
        self._login()

    # -- lazy imports ------------------------------------------------------
    def _login(self) -> None:
        import FreeCAD  # noqa: F401  (available only inside FreeCAD)
        import FreeCADGui
        import PySide.QtCore as QtCore
        import PySide.QtGui as QtGui
        import PySide.QtWidgets as QtWidgets

        self._FreeCAD = FreeCAD
        self._Gui = FreeCADGui
        self._QtCore = QtCore
        self._QtGui = QtGui
        self._QtWidgets = QtWidgets

        self.win, self.container, self.stack = self.find_viewport()
        self.available = self.container is not None
        self.dpr = self.container.devicePixelRatioF() if self.available else 1.0
        if self.available:
            self.width = self.container.width()
            self.height = self.container.height()
        else:
            self.width = self.height = 0

    def find_viewport(self) -> tuple[Any, Any, Any]:
        """Locate the 3D viewport container.  Returns (window, container, stack)."""
        QW = self._QtWidgets
        win = QW.QApplication.activeWindow()
        if win is None:
            for t in QW.QApplication.topLevelWidgets():
                if isinstance(t, QW.QMainWindow) and t.isVisible():
                    win = t
                    break
        if win is None:
            return None, None, None
        mdi = win.findChild(QW.QMdiArea)
        if mdi is None:
            return None, None, None
        sub = mdi.currentSubWindow() or (mdi.subWindowList() or [None])[0]
        if sub is None:
            return None, None, None
        view_widget = sub.widget()
        stack = view_widget.findChild(QW.QStackedWidget)
        container = stack.currentWidget() if stack else view_widget
        container.setMouseTracking(True)
        return win, container, stack

    # -- coordinate helpers ------------------------------------------------
    def device(self, x: float, y: float) -> tuple[int, int]:
        """Convert logical container coords to device pixels for the view API."""
        return (int(round(x * self.dpr)), int(round(y * self.dpr)))

    def get_object_info(self, x: float, y: float) -> Optional[dict]:
        view = self.active_view()
        if view is None:
            return None
        dx, dy = self.device(x, y)
        return view.getObjectInfo((dx, dy))

    def active_view(self) -> Any:
        try:
            return self._Gui.activeView()
        except Exception:
            return None

    # -- synthetic input ---------------------------------------------------
    def send_mouse(self, etype: Any, pos: Any, btn: Any, btns: Any) -> None:
        if not self.available:
            return
        QtCore = self._QtCore
        QtGui = self._QtGui
        QW = self._QtWidgets
        container = self.container
        # Deliver to the widget actually under the point; without
        # setMouseTracking(True) on the target synthetic moves are dropped.
        target = container.childAt(pos) or container
        target.setMouseTracking(True)
        tpos = target.mapFrom(container, pos)
        ev = QtGui.QMouseEvent(etype, tpos, target.mapToGlobal(tpos), btn,
                               btns, QtCore.Qt.NoModifier)
        QW.QApplication.sendEvent(target, ev)
        QW.QApplication.processEvents()

    def _mouse(self, kind: str, x: float, y: float) -> None:
        QtCore = self._QtCore
        QtGui = self._QtGui
        pos = QtCore.QPoint(int(x), int(y))
        if kind == "move":
            self.send_mouse(QtCore.QEvent.MouseMove, pos, QtCore.Qt.NoButton,
                            QtCore.Qt.NoButton)
        elif kind == "click":
            self.send_mouse(QtCore.QEvent.MouseButtonPress, pos,
                            QtCore.Qt.LeftButton, QtCore.Qt.LeftButton)
            self.send_mouse(QtCore.QEvent.MouseButtonRelease, pos,
                            QtCore.Qt.LeftButton, QtCore.Qt.NoButton)

    def move(self, x: float, y: float) -> None:
        self._record("move", x, y)
        self._mouse("move", x, y)

    def click(self, x: float, y: float) -> bool:
        self._record("click", x, y)
        self._mouse("click", x, y)
        sel = self._Gui.Selection.getSelectionEx()
        hit = bool(sel)
        self._Gui.Selection.clearSelection()
        self._QtCore.QCoreApplication.processEvents()
        return hit

    def key(self, key: Any, modifiers: Any = None) -> None:
        QtCore = self._QtCore
        QW = self._QtWidgets
        mods = self._QtCore.Qt.ControlModifier if modifiers == "ctrl" else (
            self._QtCore.Qt.NoModifier
        )
        ev = self._QtGui.QKeyEvent(QtCore.QEvent.KeyPress, key, mods)
        QW.QApplication.sendEvent(self.win, ev)

    def command(self, cmd: str, require: bool = False) -> bool:
        """Run a FreeCAD command.

        When ``require`` is True the call is guarded: if the command is not
        currently available (its own `isActive()` precondition is false, e.g. a
        tool that needs a sketch or selection that is not present), a dependency
        error is recorded and False is returned -- instead of the command being
        silently ignored by the GUI.  When False the command is fired verbatim
        and True is returned once dispatched (the caller verifies the outcome).
        """
        self._record("command", cmd)
        if require and not self.command_available(cmd):
            self.error("command %r is not available (missing prerequisite "
                       "object or selection)", cmd)
            return False
        try:
            self._Gui.runCommand(cmd)
        except Exception as exc:  # pragma: no cover - guest-only path
            self.error("command %r raised %r", cmd, exc)
            return False
        return True

    def command_available(self, name: str) -> bool:
        """Best-effort check of a command's precondition (its `isActive()`).

        FreeCAD exposes the static helper ``Gui.Command.isCmdActive(name)`` and
        an instance ``Gui.getCommand(name).isActive()``.  If neither can be
        probed the result is optimistic (True) so a genuinely valid command is
        never blocked; a wrong dependency is still caught by the outcome-based
        feature helpers below.
        """
        Gui = self._Gui
        cmd_cls = getattr(Gui, "Command", None)
        static = getattr(cmd_cls, "isCmdActive", None)
        if static is not None:
            try:
                return bool(static(name))
            except Exception:
                pass
        try:
            cmd = Gui.getCommand(name)
            return bool(cmd and cmd.isActive())
        except Exception:
            return True

    # -- workbench / document API -----------------------------------------
    # FreeCAD tools (workbenches, sketches, PartDesign features like Pad,
    # Part primitives, GDML, Mesh, CAM, TechDraw, ...) are all reachable either
    # through a command name (Gui.runCommand) or the Python API.  The helpers
    # below make the common cases one call; anything else is just `session.eval(...)`.

    def activate_workbench(self, name: str) -> None:
        self._record("workbench", name)
        self._Gui.activateWorkbench(name)

    def new_document(self, name: str) -> Any:
        return self._FreeCAD.newDocument(name)

    def active_document(self) -> Any:
        return self._FreeCAD.ActiveDocument

    def add_object(self, type_id: str, name: str) -> Any:
        doc = self.active_document()
        return doc.addObject(type_id, name)

    def recompute(self, doc: Any = None) -> None:
        (doc or self.active_document()).recompute()

    def eval(self, expr: str) -> Any:
        """Evaluate a FreeCAD Python expression and return the result."""
        return eval(expr, {"FreeCAD": self._FreeCAD, "Gui": self._Gui,
                           "session": self})

    def exec(self, code: str) -> None:
        """Run a block of FreeCAD Python (e.g. model/tooling setup)."""
        exec(code, {"FreeCAD": self._FreeCAD, "Gui": self._Gui, "session": self})

    def clear_selection(self) -> None:
        self._Gui.Selection.clearSelection()

    def set_preselection(self, obj: str, sub: str = "", x: int = 0, y: int = 0,
                         z: int = 0, w: int = 1) -> None:
        try:
            self._Gui.Selection.setPreselection(obj, sub, x, y, z, w)
        except Exception:
            self._Gui.Selection.setPreselection(obj, sub)

    # -- feature dependency orchestration ----------------------------------
    # PartDesign features (Pad, Pocket, ...) have a required dependency graph:
    # Body -> Sketch (closed profile) -> feature.  FreeCAD enables the tool
    # commands whenever a document is active, so a blind runCommand can silently
    # do nothing.  These helpers enforce the prerequisite chain and verify the
    # outcome, so a missing dependency becomes a recorded FAIL, not a silent PASS.

    def get_active_body(self) -> Any:
        """Return the active PartDesign::Body, or None.

        The active body is a view-side setting not cleanly exposed to Python, so
        we use a robust heuristic: the single Body, else the one with a current
        tip, else the first.
        """
        doc = self.active_document()
        if doc is None:
            return None
        bodies = [o for o in doc.Objects if o.TypeId == "PartDesign::Body"]
        if len(bodies) == 1:
            return bodies[0]
        for b in bodies:
            try:
                if b.isTip():
                    return b
            except Exception:
                pass
        return bodies[0] if bodies else None

    def ensure_body(self, name: str = "Body") -> Optional[Any]:
        """Return the active Body, creating one (and activating the workbench)
        if none exists.  This is the required first link of the feature chain."""
        body = self.get_active_body()
        if body is not None:
            return body
        doc = self.active_document()
        if doc is None:
            self.error("ensure_body: no active document")
            return None
        try:
            self._Gui.activateWorkbench("PartDesignWorkbench")
        except Exception:
            pass
        body = doc.addObject("PartDesign::Body", name)
        doc.recompute()
        return body

    def make_rect_sketch(self, x0: float, y0: float, x1: float, y1: float,
                         name: str = "Sketch") -> Optional[Any]:
        """Create a closed rectangle sketch in the active Body and attach it to
        the XY plane.  Returns the sketch, or None + error on failure.  This is
        the required dependency for profile features like Pad/Pocket."""
        import FreeCAD as App
        import Part
        import Sketcher

        doc = self.active_document()
        body = self.get_active_body()
        if body is None:
            self.error("make_rect_sketch: no PartDesign::Body (call ensure_body first)")
            return None
        sketch = doc.addObject("Sketcher::SketchObject", name)
        body.addObject(sketch)
        try:
            sketch.Support = (body.Origin.OriginFeatures[0],)
            sketch.MapMode = "FlatFace"
        except Exception:
            pass
        doc.recompute()
        edges = [
            Part.LineSegment(App.Vector(x0, y0, 0), App.Vector(x1, y0, 0)),
            Part.LineSegment(App.Vector(x1, y0, 0), App.Vector(x1, y1, 0)),
            Part.LineSegment(App.Vector(x1, y1, 0), App.Vector(x0, y1, 0)),
            Part.LineSegment(App.Vector(x0, y1, 0), App.Vector(x0, y0, 0)),
        ]
        for ln in edges:
            sketch.addGeometry(ln, False)
        # Tie the corners together (i.end -> (i+1).start), then close the loop.
        for a, b in ((0, 1), (1, 2), (2, 3)):
            sketch.addConstraint(Sketcher.Constraint("Coincident", a, 2, b, 1))
        sketch.addConstraint(Sketcher.Constraint("Coincident", 3, 2, 0, 1))
        doc.recompute()
        return sketch

    def add_partdesign_feature(self, cmd: str, type_id: str,
                               profile: Optional[Any] = None) -> Optional[Any]:
        """Run a profile-based PartDesign feature (`cmd`) with its prerequisites
        satisfied and verify it was actually created.

        Dependencies orchestrated here:
          * a PartDesign::Body must exist;
          * ``profile`` (e.g. a Sketch) must belong to that Body and is selected
            as the feature input;
          * the resulting object of ``type_id`` must appear -- otherwise the
            missing/prereq or silent no-op case is recorded as an error.

        Returns the created feature, or None + recorded error.
        """
        doc = self.active_document()
        if doc is None:
            self.error("add_partdesign_feature: no active document")
            return None
        body = self.get_active_body()
        if body is None:
            self.error("add_partdesign_feature: no PartDesign::Body (create one first)")
            return None
        if profile is not None and profile not in body.Group:
            self.error("add_partdesign_feature: %s is not a member of the body",
                       getattr(profile, "Label", profile))
            return None
        before = {o.Name for o in doc.Objects}
        if profile is not None:
            self._Gui.Selection.clearSelection()
            self._Gui.Selection.addSelection(doc.Name, profile.Name)
        if not self.command(cmd, require=True):
            return None
        doc.recompute()
        created = [o for o in doc.Objects
                   if o.Name not in before and o.TypeId.endswith(type_id)]
        if not created:
            self.error("add_partdesign_feature: %r produced no %s "
                       "(check the dependency chain)", cmd, type_id)
            return None
        return created[0]

    def has_object(self, name: str) -> bool:
        doc = self.active_document()
        return doc is not None and doc.getObject(name) is not None

    def menu(self, text: str) -> bool:
        """Trigger a real menu item whose label contains `text` (case-insensitive)."""
        self._record("menu", text)
        QW = self._QtWidgets
        mw = self._Gui.getMainWindow()
        needle = text.replace("&", "").lower()
        for menu in mw.findChildren(QW.QMenu):
            for act in menu.actions():
                lbl = (act.text() or "").replace("&", "").lower()
                if lbl and needle in lbl:
                    act.trigger()
                    return True
        return False

    def send_msg_to_view(self, msg: str) -> None:
        self._Gui.SendMsgToActiveView(msg)

    def fit(self, obj=None) -> None:
        """Activate the Part workbench (if needed), fit the view.  Runs deferred."""
        self.schedule(self._fit_now, 0)

    def _fit_now(self) -> None:
        try:
            self._Gui.activateWorkbench("PartWorkbench")
        except Exception:
            pass
        view = self.active_view()
        if view is not None:
            view.viewTop()
            view.fitAll()

    # -- diagnostics / verdict --------------------------------------------
    def _record(self, kind: str, *args: Any) -> None:
        if self.record_enabled:
            self.recorded.append((kind,) + args)

    def play(self, steps: Iterable[tuple[Any, ...]]) -> None:
        """Replay a recorded (or hand-built) step sequence."""
        for step in steps:
            kind, *rest = step
            if kind == "move":
                self.move(*rest)
            elif kind == "click":
                self.click(*rest)
            elif kind == "command":
                self.command(*rest)
            elif kind == "menu":
                self.menu(*rest)

    def emit(self, kind: str, **fields: Any) -> None:
        line = " ".join(
            [kind] + [f"{k}={v}" for k, v in fields.items()]
        )
        print(f"[HARNESS] {line}", flush=True)

    def frame_phase(self, name: str) -> None:
        """Mark a phase boundary so the host can correlate frame dumps to the
        pref state at the time they were rendered (``[HARNESS] frame_phase``)."""
        self.emit("frame_phase", phase=name)

    def set_pref(self, group: str, key: str, value: Any, emit: bool = True) -> None:
        """Set a FreeCAD parameter (group is a full path) and record it.

        Writing to a parameter group fires the ParameterObserver, so View prefs
        like ``VulkanShowEdges`` trigger ``View3DSettings::OnChange`` ->
        ``applyVulkanSettings`` automatically.  Emits a ``[HARNESS] pref`` record.
        """
        import FreeCAD
        hGrp = FreeCAD.ParamGet(group)
        if isinstance(value, bool):
            hGrp.SetBool(key, value)
        elif isinstance(value, int):
            # Colors are stored as Unsigned (0xAABBGGRR/0xRRGGBBAA); large
            # values like opaque red (0xFF0000FF) exceed INT_MAX and must be
            # written with SetUnsigned, matching the GetUnsigned read path.
            if value >= 0 and value > 2 ** 31 - 1 and hasattr(hGrp, "SetUnsigned"):
                hGrp.SetUnsigned(key, value)
            else:
                hGrp.SetInt(key, value)
        elif isinstance(value, float):
            hGrp.SetFloat(key, value)
        elif isinstance(value, str):
            hGrp.SetString(key, value)
        else:
            raise TypeError(f"set_pref: unsupported value type {type(value)!r}")
        if emit:
            self.emit("pref", group=group, key=key, value=f"{value}")

    def snapshot(self) -> dict[str, Any]:
        view = self.active_view()
        state: dict[str, Any] = {
            "viewport": {
                "w": self.width,
                "h": self.height,
                "dpr": self.dpr,
            },
        }
        if view is not None:
            try:
                cam = view.getCameraType()
                state["camera_type"] = cam
            except Exception:
                pass
        try:
            sel = self._Gui.Selection.getSelectionEx()
            state["selection"] = [o.getName() for o in sel]
        except Exception:
            state["selection"] = []
        self.emit("snapshot", state=json.dumps(state, separators=(",", ":")))
        return state

    def verdict(self, ok: bool, detail: str = "") -> None:
        result = "PASS" if ok else "FAIL"
        if not ok:
            self.errors.append(detail)
        print(f"[VERDICT] {self.name} {result}", flush=True)

    def finish(self, detail: str = "") -> None:
        """Print a verdict derived from all recorded errors/invariants so far —
        the ergonomic end-of-probe call: PASS unless anything failed."""
        self.verdict(not self.errors, detail or ("; ".join(self.errors)))

    def error(self, fmt: str, *args: Any) -> None:
        msg = fmt % args if args else fmt
        self.errors.append(msg)
        self.emit("error", msg=msg)

    # -- invariants + soak -------------------------------------------------
    def expect(self, name: str, cond: bool, detail: str = "") -> bool:
        """Record an invariant check: emits `[HARNESS] expect name=.. ok=..` and
        appends a session error when it fails.  Returns the condition so it can
        be used inline.  The aggregate verdict still comes from `verdict()`."""
        if not cond:
            self.errors.append(f"invariant {name} failed" + (f": {detail}" if detail else ""))
        self.emit("expect", name=name, ok=("1" if cond else "0"),
                  **({"detail": detail} if detail else {}))
        return cond

    def soak(self, n_ops: int = 64, seed: Optional[int] = None,
             bounds_inset: int = 20, axis="both") -> bool:
        """Randomized navigation + pick fuzz with invariants.

        Performs random orbit/pan, mouse move and click within the viewport and
        asserts small invariants after every step (camera intact, viewport rect
        unchanged, no runaway selection).  Useful for finding crashes / invariants
        that a deterministic sweep misses.  Returns True if all invariants held.
        """
        import random
        import FreeCADGui

        rng = random.Random(seed)
        view = self.active_view()
        ok = True
        x0, y0 = bounds_inset, bounds_inset
        w = max(1, self.width - 2 * bounds_inset)
        h = max(1, self.height - 2 * bounds_inset)
        # Preserve a camera/rotation baseline to verify the camera survives.
        cam0 = view.getCameraOrientation() if view is not None else None
        for i in range(n_ops):
            op = rng.random()
            if op < 0.35:
                # orbit the camera
                if view is not None:
                    view.viewOrbit(rng.uniform(-30, 30), rng.uniform(-30, 30))
            elif op < 0.60:
                x, y = rng.randint(x0, w), rng.randint(y0, h)
                self.move(x, y)
            else:
                x, y = rng.randint(x0, w), rng.randint(y0, h)
                self.click(x, y)
            if i % 8 == 0:
                ok = self.expect("soak.camera_alive",
                                 view is not None, "active view lost") and ok
                ok = self.expect("soak.viewport_stable",
                                 (self.width > 0 and self.height > 0),
                                 "viewport collapsed") and ok
        return ok

    def close(self) -> None:
        self.schedule(self._close_now, 0)

    def _close_now(self) -> None:
        self._Gui.getMainWindow().close()

    def schedule(self, fn: Callable[[], None], ms: int) -> None:
        self._QtCore.QTimer.singleShot(ms, fn)

    def run(self, main_fn: Callable[[], None], start_ms: int = 500,
            step_ms: int = 200, close_delay: int = 400) -> None:
        """Run `main_fn` deferred (avoids the startup crash on view ops) and
        close the window shortly afterwards."""

        def _go() -> None:
            try:
                main_fn()
            except Exception as exc:  # pragma: no cover - guest-only path
                self.error("probe raised %r", exc)
            self.schedule(self._close_now, close_delay)

        self.schedule(_go, start_ms)

    @classmethod
    def deferred(cls, name: str, run_fn: Callable[["Session"], None],
                 start_ms: int = 500, close_delay: int = 400) -> None:
        """Boot the session after the GUI is up, call ``run_fn(session)``, and
        close.  This is the ergonomic top-level entry point for a probe script:
        the session (and its GUI assumptions) are created inside a timer step,
        which avoids the startup race on view/camera operations."""

        def _boot() -> None:
            from PySide import QtCore
            try:
                session = cls(name)
            except Exception as exc:  # pragma: no cover - guest-only path
                print(f"[HARNESS] boot failed: {exc!r}", flush=True)
                return
            try:
                run_fn(session)
            except Exception as exc:  # pragma: no cover - guest-only path
                session.error("probe raised %r", exc)
                session.verdict(False)
            finally:
                session.schedule(session._close_now, close_delay)

        from PySide import QtCore
        QtCore.QTimer.singleShot(start_ms, _boot)


if __name__ == "__main__":
    sys.exit(main())
