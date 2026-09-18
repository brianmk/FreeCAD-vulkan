#!/usr/bin/env python3
"""Verify the raster geometry LOD runs on real document geometry.

A fresh document's Vulkan main draw list usually contains only the hidden nav
cube (~36 vertices), so a nav-cube-only run makes the geometry LOD look like it
works while never exercising the document path.  This probe builds a real,
finely tessellated Part shape and forces the LOD pre-pass on, so the host-side
check can assert that

  * ``[DRAWLIST] mainMaxVc`` reports the shape's real vertex count (not 36),
    i.e. the document geometry actually reached the main pass; and
  * ``[GEOMLOD] prepass`` compacted that command (compacted>0, maxPrims>0),
    i.e. the LOD actually ran on it.

Requires FC_VULKAN_BACKEND_DEBUG=1 (the [DRAWLIST]/[GEOMLOD] lines) and
FC_VULKAN_GEOM_LOD_ALWAYS=1 (force the pre-pass on a static frame).

Usage:
  FC_VULKAN_BACKEND_DEBUG=1 FC_VULKAN_GEOM_LOD_ALWAYS=1 \\
      FreeCAD vk_geomlod_probe.py
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
    print("GEOMLOD_PROBE " + msg, flush=True)


s = Session(name="geomlod")
steps = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("GeomLod")
    sphere = doc.addObject("Part::Sphere", "Sphere")
    sphere.Radius = 10
    doc.recompute()
    # Fine tessellation so the SoBrepFaceSet is unmistakably real geometry
    # (well above the nav cube's 36 vertices), independent of the machine's
    # default deflection.
    try:
        sphere.ViewObject.Deviation = 0.05
    except Exception:
        pass
    doc.recompute()
    FreeCADGui.updateGui()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewIsometric()
    view.fitAll()


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        # Raster backend (1); the geometry LOD is raster-only.
        s.set_pref(VIEW, "VulkanRenderMode", 1)
        s.set_pref(VIEW, "VulkanInteractionLod", True)
        build_scene()
        s.frame_phase("scene")
        log("phase=scene")
    elif k == 3:
        # Force a few frames so the pre-pass runs and the draw list settles
        # with the document geometry in it.
        for _ in range(6):
            s.vulkan_render()
        s.frame_phase("lod")
        log("phase=lod")
    elif k == 5:
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
