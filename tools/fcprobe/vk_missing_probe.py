#!/usr/bin/env python3
"""Render a 20x20 grid in path-tracing and dump the viewport to PNG.

Diagnostic for "centre boxes not rendered": build the wave grid, settle the
path tracer, then dump the actual rendered pixels so we can see WHICH boxes are
absent (vs merely occluded in the iso view).  Output path: FC_PROFILE_OUT.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
N = int(os.environ.get("FC_PROFILE_N", "20"))
OUT = os.environ.get("FC_PROFILE_OUT", "/tmp/opencode/missing.png")


def log(msg):
    print("MISSING n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="missing")
steps = [0]


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRayTracing", False)
        s.set_pref(VIEW, "VulkanPathTracing", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 12)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Missing")
        stepSize = 12
        for i in range(N):
            for j in range(N):
                b = doc.addObject("Part::Box", "b%d_%d" % (i, j))
                b.Length = 4
                b.Width = 4
                b.Height = 4
                b.Placement = FreeCAD.Placement(
                    FreeCAD.Vector(i * stepSize - N * stepSize / 2.0,
                                   j * stepSize - N * stepSize / 2.0,
                                   6.0 * (1.0 + (i % 5) / 4.0)),
                    FreeCAD.Rotation())
        doc.recompute()
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        log("built n=%d" % N)
    elif k == 20:
        view = FreeCADGui.ActiveDocument.ActiveView
        path = OUT
        try:
            view.viewport().grab().save(path)
            log("wrote live viewport grab %s" % path)
        except Exception as e:
            log("viewport grab failed: %s" % e)
            try:
                view.saveImage(path)
                log("fell back to saveImage")
            except Exception as e2:
                log("saveImage failed too: %s" % e2)
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(500, step)


QtCore.QTimer.singleShot(500, step)
