#!/usr/bin/env python3
"""Host-side assertions for vk_denoise_move_probe.py (denoise-after-move).

Correlates the renderer's FC_VULKAN_PT_DENOISE_TIMING [DENOISE-STATE] lines
with the probe's [HARNESS] frame_phase markers by presented-frame ordinal:

  [DENOISE-STATE] ord=<presented> frame=<sampleIdx> accum=<0/1> pend=<0/1>
                  ready=<0/1> denoise=<0/1> kind=<int>
  [HARNESS] frame_phase phase=moved frame=<presented>

The regression guarded: after a camera MOVE (AS-skip holds, no rebuild) a
fresh denoise must still be *published* (ready=1) once the run re-reaches the
sample cap.  In a broken build the post-move segment never publishes; the
denoiser only responds to a rebuild trigger (hover).

NOTE on pend: this check does NOT use the pend field.  [DENOISE-STATE] is
emitted from recordTraceAndPresent(), and the external-render path
(SoRTXRenderBackend::renderExternal) calls updateDenoise() *before* it, so
updateDenoise has already consumed ptDenoisePending by the time the line is
printed -- pend is always 0 in the GUI path.  A fresh publish is instead
observable as a ready 0->1 edge: a camera move resets denoiseResultReady, so
any ready=1 after the move is necessarily a newly published denoise.
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

    def published(seg):
        return any(r["ready"] == 1 for r in seg)

    def fresh_publish(seg):
        """First ready=1 that follows a ready=0 within seg (a 0->1 edge).

        The camera move resets denoiseResultReady, so a 0->1 edge after the
        move is necessarily a freshly published denoise (not a pre-move
        leftover).  Requiring the ready=0 first also keeps a stale ready=1 in
        the first post-move line from counting as a fresh publish.
        """
        seen_zero = False
        for r in seg:
            if not seen_zero:
                if r["ready"] == 0:
                    seen_zero = True
                continue
            if r["ready"] == 1:
                return r
        return None

    if not published(pre):
        err("baseline denoise never published before the move (ready=0) "
            "-> denoiser not functional, cannot test the move path")
        return

    fresh = fresh_publish(post)
    if fresh is None:
        err("denoise-after-move BROKEN: no fresh denoise was published "
            "(no ready 0->1 edge) after the move ordinal %d "
            "(post-move states: %d)" % (move_ord, len(post)))
        return
    if not any(r["accum"] == 1 for r in post):
        err("denoise-after-move BROKEN: the run never re-accumulated after "
            "the move (no accum=1 in %d post-move states)" % len(post))
        return

    restart = any(r["accum"] == 1 and r["frame"] <= 8 for r in post)
    before_publish = [r for r in post if r["ord"] < fresh["ord"]]
    cap = max((r["frame"] for r in before_publish), default=0)
    report.log_event("denoise-move", "ok",
                     move_ord=move_ord,
                     pre_states=len(pre), post_states=len(post),
                     restart_after_move=restart,
                     post_publish_ord=fresh["ord"],
                     post_publish_frame=fresh["frame"],
                     post_cap_frame=cap,
                     post_ready=fresh["ready"])
