#!/usr/bin/env python3
"""Host-side assertions for vk_blas_probe.py (Phase 7: BLAS refit/reuse).

The probe's phase markers (`[HARNESS] frame_phase phase=NAME frame=N`) and the
backend's `[RTDBG] blas frame=F built=/refit=/reused=/cache=` lines both carry
the SAME monotonic presented-frame ordinal (N/F).  So a blas line is attributed
to a phase by matching its ordinal against the phase markers' ordinals -- NOT by
relying on the interleaved stream order.  Frame dumps are named
`/tmp/vk_frame_<ordinal>.png`, so they correlate to a blas line by the ordinal
too.

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

BLAS_LINE = re.compile(
    r"\[RTDBG\] blas frame=(\d+) built=(\d+) refit=(\d+) reused=(\d+) "
    r"cache=(\d+)")
# The probe prints `BLAS phase=...` (stderr) and the harness prints
# `[HARNESS] frame_phase phase=... frame=<ordinal>` (stdout); both land in
# stdout.log.  The ordinal-bearing marker is the HARNESS one.
PHASE_LINE = re.compile(
    r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")
# Fallback for runs whose markers carry no ordinal (pre-A output): keep the
# plain stderr marker so the check can still produce a useful error.
PHASE_NOFRAME = re.compile(r"BLAS phase=(\S+)")


def _blas_events(lines):
    """[(frame_ordinal, (built, refit, reused, cache))] in log order."""
    events = []
    for line in lines:
        m = BLAS_LINE.search(line)
        if m:
            ev = tuple(int(g) for g in m.groups())
            events.append((ev[0], (ev[1], ev[2], ev[3], ev[4])))
    return events


def _phase_markers(lines):
    """[(frame_ordinal, name)] for every ordinal-bearing phase marker."""
    marks = []
    for line in lines:
        m = PHASE_LINE.search(line)
        if m:
            marks.append((int(m.group(2)), m.group(1)))
    return marks


def _phase_of(marks, frame_ord):
    """Attribute a frame ordinal to the most recent marker whose ordinal is
    <= it.  Markers are emitted right BEFORE a phase's action, so a phase's
    frames have ordinals strictly greater than its marker and <= the next
    marker's ordinal.  Frames before the first marker are 'boot'."""
    best = "boot"
    best_ord = -1
    for ordv, name in marks:
        if ordv <= frame_ord and ordv > best_ord:
            best = name
            best_ord = ordv
    return best


def _check(events, name, pred, what):
    hits = [e for e in events if e[2] == name and pred(e[1])]
    if not hits:
        return [f"phase {name}: no frame with {what}"]
    return []


def check(lines, report):
    def fail(msg):
        report.add_error(msg)

    raw_events = _blas_events(lines)
    if not raw_events:
        fail("no [RTDBG] blas lines found (renderer never ran?)")
        return
    marks = _phase_markers(lines)
    if not marks:
        fail("no ordinal-bearing [HARNESS] frame_phase markers found "
             "(probe did not call frame_phase, or the view exposes no "
             "getVulkanFrameCount)")
        return

    # Attach a phase to each blas event by ordinal.
    events = [(ordv, counts, _phase_of(marks, ordv))
              for ordv, counts in raw_events]

    # -- per-phase counters ------------------------------------------------
    for name in ("refit-box", "reuse-transform", "build-cylinder",
                 "add-identical-box"):
        if name not in [n for _, n in marks]:
            fail(f"phase {name}: missing marker in log")
    refit_marker_ord = next((ordv for ordv, n in marks if n == "refit-box"),
                            None)

    # The refit (width edit) is the only position-only edit with
    # refit>=1 built==0, so search that phase's window by ordinal.
    if refit_marker_ord is None or not any(
            e[2] == "refit-box" and e[1][1] >= 1 and e[1][0] == 0
            for e in events):
        fail("no refit>=1 built==0 frame in the refit-box window "
             "(in-place width edit never refit)")
    for msg in _check(events, "reuse-transform",
                      lambda c: c[2] >= 1 and c[1] == 0 and c[0] == 0,
                      "reused>=1 refit==0 built==0 (transform-only move)"):
        fail(msg)
    for msg in _check(events, "build-cylinder",
                      lambda c: c[0] >= 1 and c[3] >= 2,
                      "built>=1 cache>=2 (new cylinder geometry)"):
        fail(msg)
    # After the identical Box2 is added, the cache must hold all three
    # geometries (Box + Cyl + Box2).  The cylinder's own build frame reports
    # cache==2 (Box2 arrives later), so the >=3 check is done here rather
    # than on the cylinder-build frame.
    if not any(e[2] == "add-identical-box" and e[1][3] >= 3
               for e in events):
        fail("add-identical-box: cache never reached >=3 "
             "(Box + Cyl + Box2 not all cached)")
    bad = [e for e in events if e[2] == "add-identical-box" and e[1][0] > 1]
    if bad:
        fail(f"add-identical-box: {len(bad)} frames rebuilt more than one "
             "BLAS (content re-key failed)")
    if not any(e[2] == "add-identical-box" and e[1][2] >= 2
               for e in events):
        fail("add-identical-box: no frame reused>=2 (existing BLASes lost)")

    # -- refit pixel correctness ------------------------------------------
    # The refit-box marker was emitted immediately before the width edit, so
    # the frame right before the edit is the last dump with an ordinal <= the
    # marker's ordinal; the first frame after is the first dump with a
    # strictly greater ordinal.
    frames = sorted(glob.glob(os.path.join(report.artifact_dir, "frames",
                                           "*.png")),
                    key=lambda p: int(re.search(r"(\d+)", os.path.basename(p))
                                      .group(1)))
    if not frames:
        fail("refit pixel check: no frame dumps collected")
        return
    if refit_marker_ord is None:
        return
    pre = None
    post = None
    for p in frames:
        n = int(re.search(r"(\d+)", os.path.basename(p)).group(1))
        if n <= refit_marker_ord:
            pre = p
        elif post is None:
            post = p
    if pre is None or post is None:
        fail("refit pixel check: could not find frame dumps around the edit")
        return
    try:
        from PIL import Image
    except ImportError:
        fail("refit pixel check: PIL not available")
        return
    a = Image.open(pre).convert("RGB")
    b = Image.open(post).convert("RGB")
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
