#!/usr/bin/env python3
"""Host-side assertions for vk_blas_probe.py (Phase 7: BLAS refit/reuse).

Parses the real-time interleaved stderr stream (the probe's phase markers
print to stderr, so `BLAS phase=...` and `[RTDBG] blas ...` lines are in
true chronological order) and the collected frame dumps.

Checks:
  - per-phase BLAS counters (refit on width edit, reuse on transform,
    build on new geometry, no rebuild storm on identical re-added content)
  - refit correctness: the frame right before the width edit and the first
    frame after it must differ strongly (the refit actually moved pixels).
"""

import glob
import os
import re
import sys

BLAS_LINE = re.compile(r"\[RTDBG\] blas built=(\d+) refit=(\d+) reused=(\d+) "
                       r"cache=(\d+)")
PHASE_LINE = re.compile(r"BLAS phase=(\S+)")


def _blas_events(lines):
    """(line_index, (built, refit, reused, cache), phase) in log order."""
    events = []
    phase = "boot"
    for i, line in enumerate(lines):
        m = PHASE_LINE.search(line)
        if m:
            phase = m.group(1)
            continue
        m = BLAS_LINE.search(line)
        if m:
            events.append((i, tuple(int(g) for g in m.groups()), phase))
    return events


def _phase_windows(lines):
    """Map phase name -> (start_line, end_line) from the stderr markers."""
    marks = [(i, m.group(1)) for i, line in enumerate(lines)
             for m in [PHASE_LINE.search(line)] if m]
    windows = {}
    for idx, (i, name) in enumerate(marks):
        end = marks[idx + 1][0] if idx + 1 < len(marks) else None
        windows[name] = (i, end)
    return windows


def _check(events, name, pred, what):
    hits = [e for e in events if e[2] == name and pred(e[1])]
    if not hits:
        return [f"phase {name}: no frame with {what}"]
    return []


def check(lines, report):
    def fail(msg):
        report.add_error(msg)

    events = _blas_events(lines)
    if not events:
        fail("no [RTDBG] blas lines found (renderer never ran?)")
        return
    windows = _phase_windows(lines)

    # -- per-phase counters ------------------------------------------------
    for name in ("refit-box", "reuse-transform", "build-cylinder",
                 "add-identical-box"):
        if name not in windows:
            fail(f"phase {name}: missing marker in log")
            continue
    phase_events = {name: [e for e in events if e[2] == name]
                    for name in windows}

    # The width edit's refit frame can render up to a step after its
    # marker (the marker prints before the frames of its step), so search
    # from the refit marker to the end of the log: the run contains exactly
    # one position-only edit, so any refit frame after the marker is it.
    refit_marker = windows.get("refit-box", (None, None))[0]
    if refit_marker is None or not any(
            i > refit_marker and c[1] >= 1 and c[0] == 0
            for i, c, _ in events):
        fail("no refit>=1 built==0 frame after the refit-box marker "
             "(in-place width edit never refit)")
    for msg in _check(events, "reuse-transform",
                      lambda c: c[2] >= 1 and c[1] == 0 and c[0] == 0,
                      "reused>=1 refit==0 built==0 (transform-only move)"):
        fail(msg)
    for msg in _check(events, "build-cylinder",
                      lambda c: c[0] >= 1 and c[3] >= 3,
                      "built>=1 cache>=3 (new geometry)"):
        fail(msg)
    # Re-adding identical content must not rebuild the existing entries:
    # only the newly inserted command (Box2) may build.
    bad = [e for e in phase_events.get("add-identical-box", [])
           if e[1][0] > 1]
    if bad:
        fail(f"add-identical-box: {len(bad)} frames rebuilt more than one "
             "BLAS (content re-key failed)")
    if not any(e[1][2] >= 2
               for e in phase_events.get("add-identical-box", [])):
        fail("add-identical-box: no frame reused>=2 (existing BLASes lost)")

    # -- refit pixel correctness -------------------------------------------
    frames_dir = os.path.join(report.artifact_dir, "frames")
    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")),
                    key=lambda p: int(re.search(r"(\d+)", os.path.basename(p))
                                      .group(1)))
    if not frames:
        fail("refit pixel check: no frame dumps collected")
        return
    refit_start = windows.get("refit-box", (None, None))[0]
    if refit_start is None:
        return
    pre_idx = None
    post_idx = None
    for ordinal, (i, counts, phase) in enumerate(events):
        if i < refit_start:
            pre_idx = ordinal
        if i > refit_start and post_idx is None:
            post_idx = ordinal
    if pre_idx is None or post_idx is None:
        fail("refit pixel check: could not find frames around the edit")
        return
    if post_idx >= len(frames) or pre_idx >= len(frames):
        fail("refit pixel check: dump/blas-line count mismatch "
             f"(dumps={len(frames)}, need {post_idx})")
        return
    try:
        from PIL import Image
    except ImportError:
        fail("refit pixel check: PIL not available")
        return
    a = Image.open(frames[pre_idx]).convert("RGB")
    b = Image.open(frames[post_idx]).convert("RGB")
    if a.size != b.size:
        fail("refit pixel check: frame sizes differ")
        return
    pa, pb = a.load(), b.load()
    w, h = a.size
    total = 0.0
    changed = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            d = sum(abs(pa[x, y][c] - pb[x, y][c]) for c in range(3)) / 3.0
            total += d
            if d > 8.0:
                changed += 1
    samples = (h // 2) * (w // 2)
    mean = total / samples
    if mean < 0.5 or changed < samples * 0.01:
        fail(f"refit pixel check: frame diff too small (mean={mean:.3f}, "
             f"changed={changed}/{samples}) - the in-place refit did not "
             "update the rendered geometry")
    else:
        sys.stderr.write(f"[CHECK] blas refit pixel diff mean={mean:.3f} "
                         f"changed={changed}/{samples}\n")
