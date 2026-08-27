#!/usr/bin/env python3
"""Force pure-raster Vulkan and check box edges render."""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("EDGES " + msg, flush=True)


s = Session(name="edgesraster")
k = [0]


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("Edges")
    box = doc.addObject("Part::Box", "Box")
    box.Label = "EdgeBox"
    box.Length = 10
    box.Width = 10
    box.Height = 10
    doc.recompute()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewTop()
    view.fitAll()


def step():
    k[0] += 1
    n = k[0]
    if n == 2:
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        build_scene()
        s.frame_phase("raster")
        log("raster forced; edges on")
        s.set_pref(VIEW, "VulkanShowEdges", True)
    elif n == 4:
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
