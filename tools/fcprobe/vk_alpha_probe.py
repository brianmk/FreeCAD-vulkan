#!/usr/bin/env python3
"""Verify a semi-transparent material survives the RT path-tracing pipeline.

Adds an opaque floor and a semi-transparent cube (ViewObject.Transparency=50)
carrying an alpha in its material, then path-traces it.  The opacity-micro-map /
alpha-masked-triangle path (VK_EXT_opacity_micromap, probed in rt-phase0) is what
this drives.  The host check asserts PT actually accumulated frames, frame dumps
exist, and the transparency made it onto the object (the alpha path was engaged)
-- a smoke regression that catches crashes/breakage in the alpha path.

Usage:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 \\
      FC_VULKAN_DUMP_END=400 FreeCAD vk_alpha_probe.py
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
    print("ALPHA " + msg, file=sys.stderr)


s = Session(name="alpha")
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
        s.set_pref(VIEW, "VulkanPathTracingBounces", 3)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 2)
        FreeCADGui.activateWorkbench("PartWorkbench")

        doc = FreeCAD.newDocument("Alpha")
        floor = doc.addObject("Part::Box", "Floor")
        floor.Length = 20
        floor.Width = 20
        floor.Height = 1
        fm = FreeCAD.Material()
        fm.AmbientColor = (0.0, 0.0, 0.0)
        fm.DiffuseColor = (0.6, 0.6, 0.6)
        fm.SpecularColor = (0.1, 0.1, 0.1)
        fm.Shininess = 0.1
        floor.ViewObject.ShapeAppearance = fm

        cube = doc.addObject("Part::Box", "Glass")
        cube.Length = 3
        cube.Width = 3
        cube.Height = 3
        cube.Placement.Base = FreeCAD.Vector(0, 0, 3)
        cm = FreeCAD.Material()
        cm.AmbientColor = (0.0, 0.0, 0.0)
        cm.DiffuseColor = (0.4, 0.7, 0.9)
        cm.SpecularColor = (0.2, 0.2, 0.2)
        cm.Shininess = 0.4
        cube.ViewObject.ShapeAppearance = cm
        cube.ViewObject.Transparency = 50
        doc.recompute()

        view = FreeCADGui.ActiveDocument.ActiveView
        view.setAnimationEnabled(False)
        view.viewTop()
        cam = view.getCameraNode()
        cam.position.setValue(6.0, 0.0, 8.0)
        cam.height.setValue(14.0)
        s.frame_phase("alpha")
        log("phase=alpha material Transparency=50.0 (opacity path)")
    elif k == 12:
        FreeCADGui.updateGui()
    elif k == 20:
        log("snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
