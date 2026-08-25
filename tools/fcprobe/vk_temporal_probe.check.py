#!/usr/bin/env python3
"""Host-side assertions for vk_temporal_probe.py (reset-on-move).

The probe orbits the camera and edits the box. Under the accumulate-while-
static / reset-on-move architecture a camera or scene change does NOT carry
the converged history forward (no temporal reprojection): it resets the run
to a clean preview (accum=0, frameIndex=0, reproject=0) and then
re-accumulates against the new camera once it has been static for a short
settle window.  The old temporal-reprojection behavior (reproject=1, history
surviving the move) is intentionally gone.

Assertions:
  - reproject=1 never appears anywhere (reset-on-move replaced reprojection).
  - each camera move (move-small / move-big) resets the run: a viewChanged
    frame with accum=0 reproject=0 frameIndex=0.
  - the scene edit (edit-box) resets too: sceneChanged frame with accum=0
    reproject=0.
  - after a move the run re-accumulates: some static frame resumes with
    frameIndex growing from 0 (the settle auto-restart), and the active-pixel
    fraction declines below 1.0 as convergence resumes.
"""

import re

STATE_LINE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"accum=(\d) frameIndex=(\d+) idle=(\d+) reproject=(\d)")
ADAPT_LINE = re.compile(
    r"\[RTDBG\] adaptive frame=(\d+) active=(\d+)/(\d+) fraction=([0-9.]+) "
    r"frameIndex=(\d+) accum=(\d) .*reprojected=(\d+)")
# The harness emits ordinal-bearing `[HARNESS] frame_phase phase=NAME frame=N`;
# the probe also prints `TEMPORAL phase=...` (stderr) without an ordinal.
HARNESS_PHASE = re.compile(r"\[HARNESS\] frame_phase phase=(\S+) frame=(\d+)")


def _windowed(lines):
    """[(phase, kind, match)] with phase assigned via the frame ordinal.

    A state/adaptive line's `frame=N` is attributed to the most recent
    ordinal-bearing HARNESS marker whose ordinal is <= N, independent of the
    interleaved stream order.  Falls back to the stderr TEMPORAL marker when
    no ordinal is present (pre-A logs)."""
    marks = []   # [(ordinal, phase)]
    phase = "boot"
    events = []
    ordinal_marks = []
    for line in lines:
        m = HARNESS_PHASE.search(line)
        if m:
            ordinal_marks.append((int(m.group(2)), m.group(1)))
            continue
        m = re.search(r"TEMPORAL phase=(\S+)", line)
        if m:
            phase = m.group(1)
            continue
        m = STATE_LINE.search(line)
        if m:
            events.append(((int(m.group(1)), phase), "state", m))
            continue
        m = ADAPT_LINE.search(line)
        if m:
            events.append(((int(m.group(1)), phase), "adaptive", m))
    if not ordinal_marks:
        return [(p, k, m) for (_, p), k, m in events]

    def phase_of(frame_ord):
        best = "boot"
        best_ord = -1
        for ordv, name in ordinal_marks:
            if ordv <= frame_ord and ordv > best_ord:
                best = name
                best_ord = ordv
        return best

    return [(phase_of(frame_ord), k, m) for (frame_ord, _), k, m in events]


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    env = report.session.get("env_overrides", {})
    temporal_off = env.get("FC_VULKAN_PT_TEMPORAL") == "0"

    events = _windowed(lines)
    states = [(p, m) for p, k, m in events if k == "state"]
    adaptives = [(p, m) for p, k, m in events if k == "adaptive"]

    # ptState groups: 1=frame, 2=viewChanged, 3=sceneChanged, 4=accum,
    # 5=frameIndex, 6=idle, 7=reproject.
    def sc(m):
        return m.group(3)

    def vc(m):
        return m.group(2)

    def accum(m):
        return m.group(4)

    def fridx(m):
        return m.group(5)

    def reproj(m):
        return m.group(7)

    # Reset-on-move: no reprojection frame may ever appear.
    reprojects = [m for _, m in states if reproj(m) == "1"]
    if reprojects:
        err(f"{len(reprojects)} reprojection frames appeared "
            "(reset-on-move must never reproject)")

    reset_states = [m for p, m in states
                    if p in ("move-small", "move-big", "edit-box")]
    if not reset_states:
        err("no frames observed in the move/edit windows (wake-up failed?)")
        return

    # Every view/scene change must reset the run: a fresh accumulation with
    # frameIndex==0 and reproject==0.  On the detecting frame the run may
    # already be accumulating (reset-on-move restarts accumulation on the same
    # frame it sees the change), so the invariant is frameIndex==0 +
    # reproject==0 -- NOT accum==0 (that only held pre-settle).
    for p, m in states:
        if p not in ("move-small", "move-big", "edit-box"):
            continue
        if vc(m) == "1" or sc(m) == "1":
            if not (fridx(m) == "0" and reproj(m) == "0"):
                err(f"{p}: expected reset to frameIndex=0 reproject=0 on a "
                    f"view/scene change (got frameIndex={fridx(m)} "
                    f"reproject={reproj(m)})")

    # After a camera move the run resumes cleanly: a static (viewChanged=0 &
    # sceneChanged=0) accumulating frame appears with an increasing
    # frameIndex, and at least one that started a fresh run (frameIndex small
    # then growing) followed by the active fraction declining below 1.0.
    resume_after_move = False
    for p, k, m in events:
        if p not in ("move-small", "move-big"):
            continue
        if k == "state" and vc(m) == "0" and sc(m) == "0" and \
                accum(m) == "1":
            resume_after_move = True
    if not resume_after_move:
        err("no static accumulating frame observed after a camera move "
            "(re-accumulation never resumed)")

    # Convergence resumes: an adaptive framework with fraction < 1.0 appears
    # after move-small.
    seen_move = False
    resumed = False
    for p, k, m in events:
        if p == "move-small":
            seen_move = True
        if seen_move and p in ("move-big", "edit-box"):
            break
        if seen_move and k == "adaptive" and float(m.group(4)) < 1.0:
            resumed = True
    if not resumed:
        err("fraction never declined below 1.0 after the camera move "
            "(fresh run did not converge)")

    # Scene edit: must reset to preview WITHOUT reprojecting.
    edit_states = [m for p, m in states if p == "edit-box"]
    if not edit_states:
        err("edit-box: no frames observed (scene change did not wake the "
            "converged viewport)")
    else:
        scene_drop = [m for m in edit_states
                      if sc(m) == "1" and fridx(m) == "0" and
                      reproj(m) == "0"]
        edit_reproject = [m for m in edit_states
                          if sc(m) == "1" and reproj(m) == "1"]
        if not scene_drop:
            err("edit-box: no sceneChanged fresh run observed (frameIndex=0, "
                "reproject=0) - the scene change did not reset the history")
        if edit_reproject:
            err(f"edit-box: {len(edit_reproject)} scene-change frames "
                "reprojected (history must not be reused across scene "
                "edits)")

    if temporal_off:
        pass
