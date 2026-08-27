#!/usr/bin/env python3
"""Verify live raster <-> path-tracing switching (no document reopen).

Opens a view in raster mode (UseVulkanRayTracing=False), verifies the RTX
backend is nonetheless available on the device (rtxBackendAvailable=1
breadcrumb), then toggles VulkanPathTracing on/off and expects the renderer
to flip its backend live (rtBackendToggle=1/0 breadcrumbs) without the
"reopen the document" warning.

Usage:
  FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 FC_VULKAN_DUMP_END=400 \\
      FreeCAD vk_rt_toggle_probe.py

Phases: raster-open -> pt-on -> pt-off -> pt-on-again.
Exit: [VERDICT] rt-toggle PASS only if every phase completes without error.
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
    print("RTTOGGLE " + msg, flush=True)


s = Session(name="rt-toggle")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("RtToggle")
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
        # Start from a raster-only view: close the startup document, force
        # UseVulkanRayTracing off, then reopen so the new view is created
        # without the RTX request flag (the reported "reopen" scenario).
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        build_scene()
        s.frame_phase("raster-open")
        log("phase=raster-open (view created without UseVulkanRayTracing)")
    elif k == 3:
        log("phase=pt-on (toggle path tracing live)")
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.frame_phase("pt-on")
    elif k == 5:
        log("phase=pt-off (back to raster)")
        s.set_pref(VIEW, "VulkanPathTracing", False)
        s.frame_phase("pt-off")
    elif k == 7:
        log("phase=pt-on-again (re-enter path tracing)")
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.frame_phase("pt-on-again")
    elif k == 9:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
