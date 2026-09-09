#!/usr/bin/env python3
"""Scene-building probe: exercises the harness's document authoring helpers.

The Session can now create a document and populate it with Part primitives in
one call (``build_document``), or open a saved ``.FCStd`` (``open_document``).
This probe builds a small populated scene, snapshots the viewport, and exits.

Usage:
  FreeCAD vk_scene_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session

s = Session("SCENEPROBE")


def step():
    s.build_document(
        "SceneProbe",
        shapes=[
            {"type": "Box", "name": "Base", "Length": 10, "Width": 8, "Height": 2,
             "Base": (0, 0, 0)},
            {"type": "Cylinder", "name": "Pin", "Radius": 2, "Height": 6,
             "Base": (5, 1, 0)},
            {"type": "Sphere", "name": "Ball", "Radius": 3, "Base": (-5, 0, 4)},
            "Torus",
        ],
        workbench="PartWorkbench",
        view="viewTop",
        fit=True,
    )
    st = s.snapshot()
    vp = st.get("viewport", {})
    s.emit("scene_built", objects=len(s.active_document().Objects),
           viewport="%dx%d" % (vp.get("w", 0), vp.get("h", 0)))
    print("SCENEPROBE built %d objects" % len(s.active_document().Objects), flush=True)
    s.verdict(True)
    s.finish()
    FreeCADGui.getMainWindow().close()


QtCore.QTimer.singleShot(500, step)
