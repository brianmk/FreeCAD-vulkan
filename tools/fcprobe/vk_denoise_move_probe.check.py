#!/usr/bin/env python3
"""Host-side assertions for vk_denoise_move_probe.py (denoise-after-move).

Correlates the renderer's FC_VULKAN_PT_DENOISE_TIMING [DENOISE-STATE] lines
with the probe's [HARNESS] frame_phase markers by presented-frame ordinal:

  [DENOISE-STATE] ord=<presented> frame=<sampleIdx> accum=<0/1> pend=<0/1>
                  ready=<0/1> denoise=<0/1> kind=<int>
  [HARNESS] frame_phase phase=moved frame=<presented>

The regression guarded: after a camera MOVE (AS-skip holds, no rebuild) a
fresh denoise must still be *triggered* (pend=1 at the sample cap) and
*published* (ready=1).  In a broken build the post-move segment shows no
pend/ready at all; the denoiser only responds to a rebuild trigger (hover).
"""

import re

PHASE = re.compile(r"\[HARNESS\] frame_phase phase=(\w+) frame=(\d+)")
STATE = re.compile(
    r"\[DENOISE-STATE\] ord=(\d+) frame=(\d+) accum=(\d) pend=(\d) "
    r"ready=(\d) denoise=(\d) kind=(\d)")


def _states(lines):
    rows = []
    for line in lines:
        m = STATE.search(line)
        if m:
            rows.append(dict(ord=int(m.group(1)), frame=int(m.group(2)),
                             accum=int(m.group(3)), pend=int(m.group(4)),
                             ready=int(m.group(5)), denoise=int(m.group(6)),
                             kind=int(m.group(7))))
    return rows


def _move_ordinal(lines):
    """Return the presented ordinal stamped at the 'moved' phase, or None."""
    for line in lines:
        m = PHASE.search(line)
        if m and m.group(1) == "moved":
            return int(m.group(2))
    return None


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    rows = _states(lines)
    if not rows:
        err("no [DENOISE-STATE] lines seen (FC_VULKAN_PT_DENOISE_TIMING "
            "unset or the RT viewport never presented)")
        return

    move_ord = _move_ordinal(lines)
    if move_ord is None:
        err("no frame_phase phase=moved marker found (probe never moved)")
        return

    pre = [r for r in rows if r["ord"] <= move_ord]
    post = [r for r in rows if r["ord"] > move_ord]

    def triggered(seg):
        return any(r["pend"] == 1 for r in seg)

    def published(seg):
        return any(r["ready"] == 1 for r in seg)

    bas_pend = triggered(pre)
    bas_ready = published(pre)
    if not (bas_pend or bas_ready):
        err("baseline denoise never ran before the move (pend=%s ready=%s) "
            "-> denoiser not functional, cannot test the move path"
            % (bas_pend, bas_ready))
        return

    post_pend = triggered(post)
    post_ready = published(post)
    if not post_pend:
        err("denoise-after-move BROKEN: no denoise was triggered (pend=1) "
            "after the move ordinal %d (post-move states: %d)" % (
                move_ord, len(post)))
        return
    if not post_ready:
        err("denoise-after-move BROKEN: a fresh denoise was triggered after "
            "the move (pend=1, ord=%d) but never published (ready=1) in the "
            "post-move window (%d states)" % (move_ord, len(post)))
        return

    restart = any(r["accum"] == 1 and r["frame"] <= 8 for r in post)
    cap = max(r["frame"] for r in post if r["pend"] == 1)
    report.log_event("denoise-move", "ok",
                     move_ord=move_ord,
                     pre_states=len(pre), post_states=len(post),
                     restart_after_move=restart,
                     post_pend_frame=cap,
                     post_ready=post_ready)
