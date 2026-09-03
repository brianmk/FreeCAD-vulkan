#!/usr/bin/env python3
"""Host-side assertions for vk_alpha_probe.py (transparent material through PT).

This is a smoke regression for the opacity-micro-map / alpha-masked-triangle
path.  Asserts:

  - path tracing actually accumulated frames ([RTDBG] ptState with accum=1).
  - the transparency was set on the object (a `ALPHA phase=alpha
    material Transparency=...` record proves the alpha path was engaged).
  - at least one frame dump was produced (the alpha surface rendered).

Requires FC_VULKAN_RT_DEBUG=1 (ptState) and FC_VULKAN_DUMP_FRAME=1.
"""

import glob
import os
import re

PTSTATE = re.compile(
    r"\[RTDBG\] ptState frame=(\d+) viewChanged=(\d) sceneChanged=(\d) "
    r"accum=(\d) frameIndex=(\d+) idle=(\d+) reproject=(\d)")
ALPHA_ENGAGED = re.compile(r"ALPHA phase=alpha material Transparency=([0-9.]+)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    acc = [int(m.group(1)) for line in lines
           for m in [PTSTATE.search(line)] if m and m.group(4) == "1"]
    if not acc:
        err("no accumulating ptState frames (path tracing never accumulated)")
        return
    if len(acc) < 5:
        err(f"path tracing only accumulated {len(acc)} frames (< 5)")

    if not any(ALPHA_ENGAGED.search(line) for line in lines):
        err("no 'ALPHA phase=alpha material Transparency' record; the "
            "transparency was never applied to the object")

    frames = glob.glob(os.path.join(report.artifact_dir, "frames", "*.png"))
    if not frames:
        err("no frame dumps produced; the alpha surface rendered nothing")
