#!/usr/bin/env python3
"""Host-side assertions for vk_hover_settle_probe.py (hover must not restart PT).

Correlates the renderer's [RTDBG] ptState lines with the probe's
[HARNESS] frame_phase markers by the phase they fall under:

  [HARNESS] frame_phase phase=<name> frame=<presented>
  [RTDBG] ptState frame=<presented> viewChanged=<0/1> sceneChanged=<0/1>
          matChanged=<0/1> bgChanged=<0/1> latch=<0/1> accum=<0/1>
          frameIndex=<n> idle=<n> reproject=<0/1>

The regression guarded: while the cursor sweeps faces (phase "converged",
after the initial build has settled), the geometry cache must report NO scene
change.  Before the fix every hover reorder tripped sceneChanged=1 (a stale
pointer-map slot read as a geometry edit) and reset frameIndex to 0, so the
denoiser never re-converged.  The build phase legitimately has sceneChanged=1
(the first frames / initial scene upload), so only the converged window is
asserted.

A run where the logging is absent (FC_VULKAN_RT_DEBUG unset) or the run never
accumulated is an error, not a pass, so the guard cannot silently no-op.
"""

import re

PHASE = re.compile(r"\[HARNESS\] frame_phase phase=(\w+) frame=(\d+)")
PTSTATE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"matChanged=(\d) bgChanged=(\d) latch=(\d) accum=(\d) "
    r"frameIndex=(\d+) idle=(\d+)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    # Tag each ptState row with the probe phase it falls under (phase markers
    # are emitted before the sweep and are ordered with the frames).
    phase = "pre"
    by_phase = {}
    for line in lines:
        m = PHASE.search(line)
        if m:
            phase = m.group(1)
        s = PTSTATE.search(line)
        if s:
            row = dict(
                frame=int(s.group(1)), viewChanged=int(s.group(2)),
                sceneChanged=int(s.group(3)), matChanged=int(s.group(4)),
                bgChanged=int(s.group(5)), latch=int(s.group(6)),
                accum=int(s.group(7)), frameIndex=int(s.group(8)),
                idle=int(s.group(9)))
            by_phase.setdefault(phase, []).append(row)

    if not by_phase:
        err("no [RTDBG] ptState lines captured (FC_VULKAN_RT_DEBUG unset, or "
            "the PathTracing viewport never presented)")
        return

    build = by_phase.get("build", [])
    hover = by_phase.get("converged", [])
    if not hover:
        err("no ptState frames during the 'converged' hover phase (the probe "
            "never swept, or settled before any frame was logged)")
        return

    # The test is only meaningful if the run actually accumulated + converged
    # before/at the start of the hover window.
    if not any(r["frameIndex"] > 0 for r in build + hover):
        err("the run never accumulated a single sample (frameIndex stayed 0); "
            "cannot tell a reset from a dead viewport")
        return

    resets = [r for r in hover if r["sceneChanged"] == 1]
    if resets:
        err("hover restarted the accumulation: %d sceneChanged=1 frame(s) "
            "during the hover sweep (first: %s)"
            % (len(resets), [r["frame"] for r in resets[:10]]))
        return

    # A reset also drops frameIndex back to 0; assert the run stayed converged.
    max_running = 0
    for r in hover:
        if r["frameIndex"] == 0 and max_running > 0:
            err("hover reset the accumulation: frameIndex fell back to 0 "
                "(frame=%d) after reaching %d" % (r["frame"], max_running))
            return
        max_running = max(max_running, r["frameIndex"])

    report.log_event(
        "hover-settle", "ok",
        build_states=len(build), hover_states=len(hover),
        hover_resets=0, hover_max_frame_index=max_running,
        hover_idle_min=min(r["idle"] for r in hover))
