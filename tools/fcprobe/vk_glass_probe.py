#!/usr/bin/env python3
"""Decisive translucent-glass test for the RT path tracer.

Scene: a gray floor, a RED backdrop box far behind, and a cyan glass cube in
front (DiffuseColor (0.4,0.7,0.9)).  The camera is fitted to the cube and a
light is added so the glass's own surface is lit.  If thin-glass transmission
works, the cube reads as translucent cyan with the red backdrop visible
through it; toggling the cube's Transparency between 0 (opaque) and 50 must
change the same pixel from solid cyan toward a red/cyan blend.

Run:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 \\
      FC_VULKAN_DUMP_END=400 FreeCAD vk_glass_probe.py
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
    print("GLASS " + msg, file=sys.stderr)


s = Session(name="glass")
steps = [0]


def build(transparency):
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    doc = FreeCAD.newDocument("Glass")

    floor = doc.addObject("Part::Box", "Floor")
    floor.Length = 30
    floor.Width = 30
    floor.Height = 1
    fm = FreeCAD.Material()
    fm.AmbientColor = (0.0, 0.0, 0.0)
    fm.DiffuseColor = (0.5, 0.5, 0.5)
    fm.SpecularColor = (0.1, 0.1, 0.1)
    fm.Shininess = 0.1
    floor.ViewObject.ShapeAppearance = fm

    backdrop = doc.addObject("Part::Box", "Backdrop")
    backdrop.Length = 6
    backdrop.Width = 6
    backdrop.Height = 6
    backdrop.Placement.Base = FreeCAD.Vector(0, 12, 2)
    bm = FreeCAD.Material()
    bm.DiffuseColor = (0.8, 0.1, 0.1)  # red
    bm.AmbientColor = (0.0, 0.0, 0.0)
    backdrop.ViewObject.ShapeAppearance = bm

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
    cube.ViewObject.Transparency = transparency
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
        FreeCADGui.activateWorkbench("PartWorkbench")
        build(50)  # translucent
        s.frame_phase("glass-trans50")
        log("phase=glass Transparency=50 (translucent)")
    elif k == 30:
        log("snapshot partial")
        s.snapshot()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(700, step)


QtCore.QTimer.singleShot(500, step)
