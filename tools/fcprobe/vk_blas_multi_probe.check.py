#!/usr/bin/env python3
"""Host-side assertions for vk_blas_multi_probe.py (multi-object BLAS scaling).

Attributes each blob `[RTDBG] blas frame=F built=N refit=N reused=N cache=N`
line to a phase via the ordinal-bearing `[HARNESS] frame_phase phase=NAME frame=N`
marker (same scheme vk_blas_probe.check.py uses).  Asserts that cache scales with
object count, distinct objects build, transform-only edits reuse, width edits
refit, and an identical re-add does not rebuild the whole stack.

Requires FC_VULKAN_RT_DEBUG=1 (the blas line is gated on it).
"""

import re

BLAS_LINE = re.compile(
    r"\[RTDBG\] blas frame=(\d+) built=(\d+) refit=(\d+) reused=(\d+) "
    r"cache=(\d+)")
PHASE_LINE = re.compile(r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")


def _blas_events(lines):
    return [(int(m.group(1)), tuple(int(m.group(i)) for i in range(2, 6)))
            for line in lines for m in [BLAS_LINE.search(line)] if m]


def _marks(lines):
    return [(int(m.group(2)), m.group(1))
            for line in lines for m in [PHASE_LINE.search(line)] if m]


def _phase_of(marks, frame_ord):
    best, best_ord = "boot", -1
    for ordv, name in marks:
        if ordv <= frame_ord and ordv > best_ord:
            best, best_ord = name, ordv
    if best_ord < 0 and marks:
        # The renderer records the very first frame (the initial BLAS build)
        # before the probe's first frame_phase marker reaches the log, so a
        # pre-marker event belongs to the first phase, not "boot".
        best = marks[0][1]
    return best


EXPECTED = 4  # B1, B2, B3 all cached, then B4 identical to B1 = still 4


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    events = _blas_events(lines)
    if not events:
        err("no [RTDBG] blas lines found (renderer never ran?)")
        return
    marks = _marks(lines)
    if not marks:
        err("no ordinal-bearing [HARNESS] frame_phase markers found")
        return
    logged = [(ordv, counts, _phase_of(marks, ordv)) for ordv, counts in events]
    by_phase = {}
    for ordv, counts, phase in logged:
        by_phase.setdefault(phase, []).append(counts)

    # Cache must scale up to all four geometries (B1/B2/B3 + B4 dedup-safe).
    if max(counts[3] for _, counts, _ in logged) < EXPECTED:
        err("cache never reached %d geometries (Box/Cyl dedup expected)" % EXPECTED)

    # Each distinct object build must report built>=1 in its build phase.
    for name in ("build-1", "build-2", "build-3"):
        if not any(c[0] >= 1 for c in by_phase.get(name, ())):
            err(f"{name}: no built>=1 frame (new geometry never built)")

    # Transform-only edit: reuse, no built, no refit.
    if not any(c[2] >= 1 and c[0] == 0 and c[1] == 0
               for c in by_phase.get("reuse", ())):
        err("reuse: no reused>=1 built==0 refit==0 frame (transform-only move)")

    # Same-topology width edit: refit, no built.
    if not any(c[1] >= 1 and c[0] == 0 for c in by_phase.get("refit", ())):
        err("refit: no refit>=1 built==0 frame (in-place width edit never refit)")

    # Identical re-add must NOT rebuild the whole stack (content-key dedup).
    add = by_phase.get("add-identical", ())
    if add and max(c[0] for c in add) > 1:
        err(f"add-identical: {max(c[0] for c in add)} BLASes rebuilt "
            "on an identical re-add (content re-key failed)")
    if add and any(c[3] >= EXPECTED for c in add) is False:
        err("add-identical: cache never held all geometries after the re-add")
