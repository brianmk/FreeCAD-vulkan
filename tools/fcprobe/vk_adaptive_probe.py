#!/usr/bin/env python3
"""Verify adaptive sampling: converged pixels stop tracing, the active-pixel
fraction declines over the progressive run, and the accumulation auto-stops
below the sample cap.

Run twice and compare:
  - default (adaptive ON): [RTDBG] adaptive fraction declines below 1.0 and
    the run stops below FC_VULKAN_PT_MAXSAMPLES.
  - FC_VULKAN_PT_ADAPTIVE=0: fraction stays 1.0 on every accumulating frame
    and the run never stops below the cap.

The thresholds are deliberately loose so convergence is reachable within
the probe's short lifetime:
  FC_VULKAN_PT_MIN_SAMPLES=2  (variance test starts at 2 spp)
  FC_VULKAN_PT_THRESHOLD=0.8  (80% relative error is "converged")
  FC_VULKAN_PT_STOP_FRACTION=0.5 (stop once <50% of pixels are active)

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_PT_MAXSAMPLES=64 \\
      FreeCAD vk_adaptive_probe.py
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
    print("ADAPTIVE " + msg, file=sys.stderr)


s = Session(name="adaptive")
steps = [0]


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # 4=RayTracing: the real RT gate
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Adaptive")
        box = doc.addObject("Part::Box", "Box")
        box.Length = 10
        box.Width = 10
        box.Height = 10
        doc.recompute()
        view = FreeCADGui.ActiveDocument.ActiveView
        # viewTop() is instant; fitAll() animates the camera and would
        # keep the view "changed" for many frames, resetting accumulation.
        view.viewTop()
        s.frame_phase("setup")
        log("phase=setup (path tracing on, camera static)")
    elif k == 20:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
