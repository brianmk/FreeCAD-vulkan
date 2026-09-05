#!/usr/bin/env python3
"""Hover-over-floor causes glass to go black? trace it.

Scene: gray floor + translucent glass cube.  Render first, then move the mouse
onto the floor (preselect highlight promotes the floor's command pass?) and
read the resulting draw-list pass distribution / accumulation state.

Run:
  FC_VULKAN_RT_DEBUG=1 FC_VULKAN_RT_GEO=1 FreeCAD vk_hover_glass_probe.py
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
    print("HOVERGLASS " + msg, file=sys.stderr)


s = Session(name="hoverglass")
steps = [0]
last_x = 0.0
last_y = 0.0


def build():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    doc = FreeCAD.newDocument("HoverGlass")

    floor = doc.addObject("Part::Box", "Floor")
    floor.Length = 30
    floor.Width = 30
    floor.Height = 1
    fm = FreeCAD.Material()
    fm.AmbientColor = (0.0, 0.0, 0.0)
    fm.DiffuseColor = (0.55, 0.55, 0.55)
    fm.SpecularColor = (0.1, 0.1, 0.1)
    fm.Shininess = 0.1
    floor.ViewObject.ShapeAppearance = fm

    cube = doc.addObject("Part::Box", "Glass")
    cube.Length = 3
    cube.Width = 3
    cube.Height = 3
    cube.Placement.Base = FreeCAD.Vector(0, 0, 4.5)
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
    return cube


def step():
    global last_x, last_y
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
        s.frame_phase("hoverglass-settle")
        log("phase=settle (no hover)")
    elif k == 40:
        # Find a pixel that hits the floor (not the glass) by scanning, then
        # hover there so the preselection highlight promotes the floor's
        # command pass.
        view = FreeCADGui.ActiveDocument.ActiveView
        hit_x = hit_y = None
        for _ in range(60):
            gx = int(s.width * (0.15 + 0.70 * ((steps[0] * 7 + _ * 13) % 100) / 100.0))
            gy = int(s.height * (0.10 + 0.80 * ((_ * 29) % 100) / 100.0))
            info = s.get_object_info(gx, gy)
            if info and info.get("Component", ""):
                comp = info.get("Component", "")
                # prefer the floor (the large gray box); accept any hit that
                # reports the Floor object
                s.move(gx, gy)
                pre = FreeCADGui.Selection.getPreselection()
                if pre and pre.ObjectName == "Floor":
                    hit_x, hit_y = gx, gy
                    break
        if hit_x is None:
            log("no floor pixel found (cannot force highlight)")
        else:
            log("hover floor x=%d y=%d" % (hit_x, hit_y))
            s.move(hit_x, hit_y)
        s.frame_phase("hoverglass-hover-floor")
    elif k == 70:
        log("done")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(300, step)


QtCore.QTimer.singleShot(500, step)
