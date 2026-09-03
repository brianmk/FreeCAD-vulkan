#!/usr/bin/env python3
"""Verify Vulkan point rendering (VulkanShowPoints) and the round-point glyph
path (FC_VULKAN_ROUND_POINTS=1, SO_POINT_SHAPE_ROUND).

Renders a tessellated Part::Sphere in a raster Vulkan view so vertex points are
numerous, then toggles VulkanShowPoints on.  The VulkanEdgeColor is pinned to red
so the frame dump pixel counts are deterministic; the host-side check asserts the
applyVulkanSettings breadcrumb recorded the points=0->1 transition and that the
run produced frame dumps (the round-point discard path ran without crashing).

Usage:
  FC_VULKAN_ROUND_POINTS=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 \\
      FC_VULKAN_DUMP_END=400 FC_VULKAN_BACKEND_DEBUG=1 FreeCAD vk_points_probe.py
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
    print("POINTS " + msg, flush=True)


s = Session(name="points")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("Points")
    sphere = doc.addObject("Part::Sphere", "Sphere")
    sphere.Radius = 6
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
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        # Force raster mode (1): the point overlay is only drawn by the raster
        # backend, not the RTX path tracer.
        s.set_pref(VIEW, "VulkanRenderMode", 1)
        # Edges off in BOTH phases so only points contribute edge-colored
        # (red) pixels -> a clean baseline (0) vs points (>0) comparison.
        s.set_pref(VIEW, "VulkanShowEdges", False)
        s.set_pref(VIEW, "VulkanShowPoints", False)
        s.set_pref(VIEW, "VulkanEdgeColor", 0xFF0000FF)
        build_scene()
        s.frame_phase("baseline")
        log("phase=baseline (points off)")
    elif k == 3:
        s.set_pref(VIEW, "VulkanShowPoints", True)
        s.frame_phase("points")
        log("phase=points (VulkanShowPoints on, round glyph path)")
    elif k == 5:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
