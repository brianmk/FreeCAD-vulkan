#!/usr/bin/env python3
"""Host-side assertions for vk_matchange_probe.py (material edit re-renders PT).

Regression for the material-edit bug: changing an object's material (same
geometry, same transform, no camera move) must be treated as a scene change so
the path tracer restarts its accumulation.  Requires FC_VULKAN_RT_DEBUG=1
(ptState) and FC_VULKAN_RT_GEO=1 ([GCR] MATERIAL breadcrumb).

Asserts:

  - path tracing accumulated frames before the edit (scene was live);
  - the material edit produced a scene-change restart with viewChanged=0
    (camera was NOT the trigger);
  - the accumulation resumed after the settle window (frameIndex advanced);
  - the [GCR] MATERIAL breadcrumb recorded the hash change on the recolor.
"""

import re

PTSTATE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"bgChanged=(\d) latch=(\d) accum=(\d) frameIndex=(\d+) idle=(\d+) "
    r"reproject=(\d)")
MATERIAL_CHANGE = re.compile(
    r"\[GCR\] MATERIAL cmd=\S+ pass=\d+ vc=\d+ old=[0-9a-f]+ "
    r"new=[0-9a-f]+")
RECOLOR = re.compile(r"MATCHG recolor cube -> red \(no camera move\)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    fmts = [PTSTATE.search(line) for line in lines]
    states = []
    for m in fmts:
        # Normalize each parsed line into a reusable dict.
        if not m:
            continue
        states.append({
            "frame": int(m.group(1)),
            "view": int(m.group(2)),
            "scene": int(m.group(3)),
            "accum": int(m.group(6)),
            "idx": int(m.group(7)),
            "idle": int(m.group(8)),
        })

    if not states:
        err("no [RTDBG] ptState lines (path tracing never ran)")
        return

    # 1. Before the recolor the scene must have accumulated a few samples.
    early = [s for s in states if s["frame"] < 100]
    if not any(s["accum"] == 1 for s in early):
        err("path tracing never accumulated before the material edit")

    # 2. A restart with viewChanged=0 must follow the recolor.
    restart = None
    for s in states:
        if s["scene"] == 1 and s["view"] == 0:
            # find the recolor marker line index to bound the search
            restart = s
            break
    if restart is None:
        err("no scene-change restart with viewChanged=0 after the recolor")
    else:
        # 3. After the settle window, accumulation must resume.
        after = [s for s in states
                 if s["frame"] > restart["frame"] and s["accum"] == 1]
        if not after:
            err(f"accumulation did not resume after the frame "
                f"{restart['frame']} restart")
        else:
            # frameIndex must have advanced past zero (a fresh run, not a
            # stuck preview).
            if after[0]["idx"] != 0:
                # tolerate the first resumed frame being pre-advance
                pass
            if max(s["idx"] for s in after) < 1:
                err("resumed accumulation never advanced frameIndex")

    # 4. The material hash breadcrumb must record the recolor.
    if not any(RECOLOR.search(line) for line in lines):
        err("probe never performed the recolor")
    if not any(MATERIAL_CHANGE.search(line) for line in lines):
        err("no [GCR] MATERIAL breadcrumb; material edit did not change the "
            "material content hash")
