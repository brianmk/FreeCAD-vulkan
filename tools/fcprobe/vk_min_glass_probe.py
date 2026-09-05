#!/usr/bin/env python3
"""MINIMAL translucent-glass probe: no floor, no backdrop, no color helpers.

The only geometry is a cyan cube at two alpha values.  Any non-background pixel
must be the glass's own shading.  Denoiser forced off by the host (FC_VULKAN_PT_DENOISER=none)
so the frame dump is the raw compute radiance.

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_PT_DENOISER=none FC_VULKAN_DUMP_FRAME=1 \\
      FC_VULKAN_DUMP_START=0 FC_VULKAN_DUMP_END=400 FreeCAD vk_min_glass_probe.py
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
    print("MINGLASS " + msg, file=sys.stderr)


s = Session(name="minglass")
steps = [0]


def build(alpha_frac):
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    doc = FreeCAD.newDocument("MinGlass")
    cube = doc.addObject("Part::Box", "Glass")
    cube.Length = 3
    cube.Width = 3
    cube.Height = 3
    cm = FreeCAD.Material()
    cm.AmbientColor = (0.0, 0.0, 0.0)
    cm.DiffuseColor = (0.4, 0.7, 0.9)
    cm.SpecularColor = (0.2, 0.2, 0.2)
    cm.Shininess = 0.4
    cube.ViewObject.ShapeAppearance = cm
    cube.ViewObject.Transparency = 50
    doc.recompute()
    FreeCADGui.Selection.clearSelection()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.setAnimationEnabled(False)
    view.viewIsometric()
    view.fitAll()
    return doc


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 3)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        s.set_pref(VIEW, "VulkanPathTracingMaxSamples", 64)
        FreeCADGui.activateWorkbench("PartWorkbench")
        build(50)
        s.frame_phase("min-glass-trans50")
        log("phase=min-glass Transparency=50")
    elif k == 40:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(600, step)


QtCore.QTimer.singleShot(500, step)
