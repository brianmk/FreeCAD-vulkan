#!/usr/bin/env python3
"""Phase 0 (RTX foundation) verification.

Exercises the capability plumbing + HDR accumulation path-tracing changes
added in Phase 0:

  - The backend self-probes the optional capability extensions
    (VK_KHR_ray_tracing_position_fetch, VK_EXT_opacity_micromap,
    VK_NV_cluster_acceleration_structure, VK_NV_partitioned_acceleration_structure,
    VK_NV_ray_tracing_linear_swept_spheres) and emits an [RTDBG] caps line.
  - The widget requests those extensions/features from the device (gated on
    the probe result) so the device create does not fail on a driver that
    lacks them.
  - A path-tracing run accumulates HDR radiance (unclamped) and still
    converges + denoises without crashing or losing the image.

The probe drives path tracing on, let it settle/auto-restart, then finishes.
Host-side assertions (in vk_rt_phase0_probe.check.py) grep the [RTDBG] caps
line and require a settled frame dump to exist.  Scenes/lights kept minimal so
it runs fast.

Usage:
  FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 FC_VULKAN_DUMP_END=400 \\
      FreeCAD vk_rt_phase0_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("RTX0 " + msg, flush=True)


s = Session(name="rt-phase0")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("RtPhase0")
    box = doc.addObject("Part::Box", "Box")
    box.Label = "RtBox"
    box.Length = 10
    box.Width = 10
    box.Height = 10
    doc.recompute()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewTop()
    view.fitAll()


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        # Raster first, so the RTX backend is brought up lazily and the caps
        # line happens at the toggle (not at view creation).
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        s.set_pref(VIEW, "VulkanRenderMode", 1)  # 1=RasterVulkan: raster gate
        s.set_pref(VIEW, "VulkanPathTracingBounces", 3)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        s.set_pref(VIEW, "VulkanPathTracingMaxSamples", 64)
        build_scene()
        s.frame_phase("raster-open")
        log("phase=raster-open")
    elif k == 4:
        log("phase=pt-on (lazy RTX bring-up + caps probe)")
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4=RayTracing: the real RT gate
        s.frame_phase("pt-on")
    elif k in (7, 10, 13):
        # A few settle frames to let the accumulation auto-restart and
        # converge toward the sample cap.
        log("phase=pt-settle %d" % k)
        s.frame_phase("pt-settle-%d" % k)
    elif k == 16:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
