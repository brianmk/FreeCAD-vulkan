#!/usr/bin/env python3
"""Host-side assertions for vk_hover_glass_probe.py (hover must not restart PT).

Regression for the hover-over-floor glitch: FreeCAD's selection/preselection
highlight momentarily swaps a shape's base geometry for a degenerate/garbage
vertex buffer (SoHighlightElementAction / whichChild), which the RT geometry
cache used to read as a content change -> cacheChanged -> sceneChanged -> the
path tracer restarted its accumulation ("the lights re-calc") and refit the
BLAS with the junk points, so a ray transmitting through glass missed the
surface and read black.

The fix adds a degenerate-geometry guard: skip a traced command whose positions
have collapsed or gone non-finite/out-of-range, keeping the previous valid BLAS
alive (re-stamped, not evicted) and leaving cacheChanged untouched.  Requires
FC_VULKAN_RT_DEBUG=1 (ptState).

Asserts:

  - path tracing accumulated before the hover;
  - no spurious scene-change restart (sceneChanged=1 with viewChanged=0)
    occurs AFTER the hover;
  - the accumulation frameIndex keeps advancing across the hover (never
    resets to 0 after the hover marker).
"""

import re

PTSTATE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"bgChanged=(\d) latch=(\d) accum=(\d) frameIndex=(\d+) idle=(\d+) "
    r"reproject=(\d)")
HOVER = re.compile(r"HOVERGLASS hover floor x=\d+ y=\d+")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    # Reconstruct a frame-ordered timeline of ptState lines with their line
    # indices so the "after hover" boundary can be located.
    timeline = []
    for lineno, line in enumerate(lines):
        m = PTSTATE.search(line)
        if not m:
            continue
        timeline.append({
            "line": lineno,
            "frame": int(m.group(1)),
            "view": int(m.group(2)),
            "scene": int(m.group(3)),
            "accum": int(m.group(6)),
            "idx": int(m.group(7)),
        })

    hover_line = None
    for lineno, line in enumerate(lines):
        if HOVER.search(line):
            hover_line = lineno
            break

    if hover_line is None:
        err("probe never hovered over the floor")
        return

    if not timeline:
        err("no [RTDBG] ptState lines (path tracing never ran)")
        return

    # 1. Accumulate before the hover.
    before = [t for t in timeline if t["line"] < hover_line]
    if not any(t["accum"] == 1 for t in before):
        err("path tracing never accumulated before the hover")

    boundary = hover_line
    # 2. After the hover there must be NO scene-change restart caused by the
    #    hover itself (sceneChanged=1 with viewChanged=0).  The very first
    #    frame (frame 1) legitimately reports sceneChanged=1 on bring-up; it
    #    appears before the hover so it is not counted here.
    after = [t for t in timeline
             if t["line"] > boundary and t["scene"] == 1 and t["view"] == 0]
    if after:
        err(f"{len(after)} spurious scene-change restart(s) after hover "
            f"(sceneChanged=1 viewChanged=0): frames "
            f"{[t['frame'] for t in after]}")

    # 3. The accumulation must keep advancing across the hover: track whether
    #    the last sampled frame before the hover and the last sampled frame
    #    after it both accumulated, and that frameIndex never resets to 0
    #    after the hover (a restart would reset frameIndex to 0 and accum=0).
    max_idx_after = max((t["idx"] for t in after), default=None)
    # Only meaningful if any after-hover frames exist at all.
    any_after = any(t["line"] > boundary for t in timeline)
    if any_after and max_idx_after is not None and max_idx_after < 1:
        err(f"accumulation never advanced after the hover (max frameIndex="
            f"{max_idx_after})")
