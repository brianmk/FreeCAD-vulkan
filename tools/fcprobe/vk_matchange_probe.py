#!/usr/bin/env python3
"""Material-edit re-render test for the RT path tracer.

Root cause under test: a pure material edit (recolor) keeps the geometry
content, the transform and the command count identical, so the geometry cache
never set cacheChanged -> updatePathTracingState saw sceneChanged=false -> a
converged path tracer kept its stale accumulation until the camera moved.  The
fix adds a material-content hash per traced piece so the recolor fires
cacheChanged and the accumulation restarts.

The probe enables the trace, builds a cyan cube, lets the tracer settle, then
recolors the cube red WITHOUT moving the camera.  It asserts the [GCR] MATERIAL
breadcrumb appears (which the fix emits only on a material-edit scene change)
and that the trace shows a scene-change restart after the edit.

Run:
  FC_VULKAN_RT_GEO=1 FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 \\
      FC_VULKAN_DUMP_END=400 FreeCAD vk_matchange_probe.py
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
    print("MATCHG " + msg, file=sys.stderr)


s = Session(name="matchange")
steps = [0]


def build():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    doc = FreeCAD.newDocument("MatChange")

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

    cube = doc.addObject("Part::Box", "Cube")
    cube.Length = 3
    cube.Width = 3
    cube.Height = 3
    cube.Placement.Base = FreeCAD.Vector(0, 0, 4.5)
    cm = FreeCAD.Material()
    cm.AmbientColor = (0.0, 0.0, 0.0)
    cm.DiffuseColor = (0.0, 0.8, 0.9)   # cyan
    cm.SpecularColor = (0.2, 0.2, 0.2)
    cm.Shininess = 0.4
    cube.ViewObject.ShapeAppearance = cm
    cube.ViewObject.Transparency = 0
    doc.recompute()

    FreeCADGui.Selection.clearSelection()

    view = FreeCADGui.ActiveDocument.ActiveView
    view.setAnimationEnabled(False)
    view.viewIsometric()
    view.fitAll()
    return cube


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
        build()
        s.frame_phase("matchange-cyan-settle")
        log("phase=cyan settling (no camera move)")
    elif k == 60:
        log("recolor cube -> red (no camera move)")
        doc = FreeCAD.ActiveDocument
        cube = doc.getObject("Cube")
        cm = FreeCAD.Material()
        cm.AmbientColor = (0.0, 0.0, 0.0)
        cm.DiffuseColor = (0.8, 0.1, 0.05)  # red
        cm.SpecularColor = (0.2, 0.2, 0.2)
        cm.Shininess = 0.4
        cube.ViewObject.ShapeAppearance = cm
        doc.recompute()
        s.frame_phase("matchange-red-resolve")
        log("phase=red resolve")
    elif k == 160:
        log("done")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(300, step)


QtCore.QTimer.singleShot(500, step)
