#!/usr/bin/env python3
"""Reproduce the "far boxes render as edges-only" live-transient bug.

Build the 20x20 wave grid, settle PT, then ROTATE the camera and capture a
sequence of frames immediately after rotation (no settle) to catch the
reprojection transient where far/small boxes show as wireframe/edges-only.
Each frame is dumped to FC_PROFILE_DIR/f%03d.png.
"""

import math
import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
N = int(os.environ.get("FC_PROFILE_N", "20"))
OUTDIR = os.environ.get("FC_PROFILE_DIR", "/tmp/opencode/seq")


def log(msg):
    print("SEQ n=%d %s" % (N, msg), file=sys.stderr)


s = Session(name="seq")
steps = [0]


def dump(idx):
    os.makedirs(OUTDIR, exist_ok=True)
    view = FreeCADGui.ActiveDocument.ActiveView
    path = os.path.join(OUTDIR, "f%03d.png" % idx)
    try:
        view.saveImage(path)
        log("wrote %s" % path)
    except Exception as e:
        log("saveImage failed: %s" % e)


def orbit(degrees):
    """Rotate the camera around the world Z axis, keeping it looking at the
    origin (same pattern as vk_temporal_probe)."""
    from pivy import coin

    view = FreeCADGui.ActiveDocument.ActiveView
    cam = view.getCameraNode()
    pos = cam.position.getValue()
    angle = math.radians(degrees)
    x = pos[0] * math.cos(angle) - pos[1] * math.sin(angle)
    y = pos[0] * math.sin(angle) + pos[1] * math.cos(angle)
    cam.position.setValue(x, y, pos[2])
    cam.pointAt(coin.SbVec3f(0.0, 0.0, 0.0))
    log("orbited %d deg" % degrees)


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
        doc = FreeCAD.newDocument("Seq")
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
        log("built")
    elif k == 16:
        dump(0)
    elif k == 17:
        # Rotate slightly and capture immediately (reprojection refresh).
        orbit(15)
        dump(1)
    elif k == 18:
        dump(2)
    elif k == 19:
        dump(3)
    elif k == 21:
        orbit(-30)
        dump(4)
    elif k == 22:
        dump(5)
    elif k == 27:
        dump(9)
    elif k == 32:
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(400, step)


QtCore.QTimer.singleShot(400, step)
