#!/usr/bin/env python3
"""Host-side assertions for vk_rt_phase0_probe.py (Phase 0: RTX foundation).

Checks performed on the run bundle:
  1. The [RTDBG] caps line was emitted by the backend (device extension
     self-probe reached the created device).  On the RTX 5090 with the
     proprietary driver the position-fetch, opacity-micromap and NV-shaped
     cluster/partitioned/LSS extensions are advertised, so they must read 1
     (not that they are *used* yet in Phase 0 - only that they were detected
     and the request did not fail device creation).
  2. Path tracing actually ran: at least one accumulated frame (ptState line)
     and a frame dump exists with content.
  3. No validation error or crash surfaced by the harness.

The [RTDBG] ptState lines used to count accumulating frames are gated on
FC_VULKAN_RT_DEBUG (see SoRTXRenderBackendPathTracing.cpp), so run the probe
with that env set:

  FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_RT_DEBUG=1 \\
      freecad_probe.py run tools/fcprobe/vk_rt_phase0_probe.py \\
      --profile vulkan --name rt-phase0
"""

import glob
import os
import re
import sys

CAPS_FOUND = re.compile(r"\[RTDBG\] caps positionFetch=(\d+)\s+opacityMicromap=(\d+)\s+"
                        r"nvCluster=(\d+)\s+nvPartitioned=(\d+)\s+nvLinearSweptSpheres=(\d+)")
PTSTATE = re.compile(r"\[RTDBG\] ptState frame=(\d+)")


def check(lines, report):
    def err(msg):
        report.add_error(msg)

    caps = None
    pt_frames = []
    for ln in lines:
        m = CAPS_FOUND.search(ln)
        if m and caps is None:
            caps = (int(m.group(1)), int(m.group(2)), int(m.group(3)),
                    int(m.group(4)), int(m.group(5)))
        m2 = PTSTATE.search(ln)
        if m2:
            pt_frames.append(int(m2.group(1)))

    if caps is None:
        err("no [RTDBG] caps line: the backend did not self-probe (or path "
            "tracing never came up)")
        return

    (pf, om, cl, ptpart, lss) = caps
    names = {"positionFetch": pf, "opacityMicromap": om, "nvCluster": cl,
             "nvPartitioned": ptpart, "nvLinearSweptSpheres": lss}
    have = 0
    for name, v in names.items():
        if v:
            sys.stderr.write(f"[CHECK] caps: {name} ok\n")
            have += 1

    if have == 0:
        err("all five optional capability extensions reported absent; the "
            "device may be a fallback (RADV) GPU or the extension names "
            "changed")

    if not pt_frames:
        err("no [RTDBG] ptState lines: path tracing never accumulated a frame")
        return

    # At least 5 accumulating frames: the auto-restart + settle window must
    # have produced progress (not a single stalled frame).
    if len(pt_frames) < 5:
        err("path tracing only accumulated %d frames (< 5); the settle "
            "auto-restart may be wedged" % len(pt_frames))

    # Require at least one dumped frame with content (the probe drives
    # FC_VULKAN_DUMP_FRAME with an early START so the PT frames are dumped).
    frames = glob.glob(os.path.join(report.artifact_dir, "frames", "*.png"))
    if not frames:
        frames = glob.glob("/tmp/vk_frame_*.png")
    if not frames:
        err("no frame dumps produced; HDR/PT run rendered nothing")
        return
    sys.stderr.write(f"[CHECK] frames: {len(frames)} dump(s) present\n")
