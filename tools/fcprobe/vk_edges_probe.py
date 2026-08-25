#!/usr/bin/env python3
"""Reproduce the missing Vulkan edge lines.

Renders a Part::Box with edges/points overlays on and dumps the IR drawlist so
we can see what topology/pass the black edge lines land in under Vulkan.
"""

import os
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("EDGES " + msg, flush=True)


s = Session(name="edges")
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
        s.set_pref(VIEW, "VulkanShowEdges", True)
        s.set_pref(VIEW, "VulkanEdgeColor", 0xFF0000FF)
        build_scene()
        s.frame_phase("edges")
        log("edges enabled; snapshot")
        s.snapshot()
    elif n == 4:
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
