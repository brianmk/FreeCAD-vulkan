#!/usr/bin/env python3
"""Dump the viewport via view.saveImage() while RT is active.

Resolution: if the output contains the nav-cube HUD / flat Coin shading it is
the COIN raster view; if it looks path-traced (no HUD, accumulative shading)
it is the RAY-TRACED storage image.  Output path: FC_PROFILE_OUT.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
N = int(os.environ.get("FC_PROFILE_N", "12"))
OUT = os.environ.get("FC_PROFILE_OUT", "/tmp/opencode/save.png")


def log(msg):
    print("SAVE n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="save")
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
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        FreeCADGui.activateWorkbench("PartWorkbench")
        doc = FreeCAD.newDocument("Save")
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
    elif k in (6, 12, 20):
        view = FreeCADGui.ActiveDocument.ActiveView
        try:
            view.saveImage(OUT)
            log("saveImage wrote %s (phase k=%d)" % (OUT, k))
        except Exception as e:
            log("saveImage failed k=%d: %s" % (k, e))
    elif k == 24:
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(400, step)


QtCore.QTimer.singleShot(400, step)
