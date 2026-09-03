#!/usr/bin/env python3
"""Verify Vulkan display preferences are read and applied.

Cycles the Vulkan-only View prefs (VulkanShowEdges / VulkanShowPoints /
VulkanEdgeColor) while rendering a Part::Box in the Vulkan viewport.  Each phase
resets a pref via `Session.set_pref` and emits a `[HARNESS] pref` record; the
parameter writes fire View3DSettings::OnChange -> applyVulkanSettings, whose
breadcrumb (trace.log) plus a frame dump let the host assert that the value the
code READ is the value that was RENDERED.

Usage:
  FC_VULKAN_DUMP_FRAME=1 FC_VULKAN_DUMP_START=0 FC_VULKAN_DUMP_END=400 \\
      FreeCAD vk_prefs_probe.py

Phases: baseline (edges/points off) -> edges (red) -> edges+points.
Exit: [VERDICT] prefs PASS only if every phase completes without error.
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
    print("PREFS " + msg, flush=True)


s = Session(name="prefs")
PHASES = [
    ("baseline", {"VulkanShowEdges": False, "VulkanShowPoints": False}),
    ("edges", {"VulkanShowEdges": True, "VulkanShowPoints": False,
               "VulkanEdgeColor": 0xFF0000FF}),          # opaque red
    ("points", {"VulkanShowEdges": True, "VulkanShowPoints": True}),
]
steps = [0]
phase_idx = [0]


def apply_phase(idx):
    name, prefs = PHASES[idx]
    for key, value in prefs.items():
        s.set_pref(VIEW, key, value)
    s.frame_phase(name)
    log(f"phase={name} edges={prefs.get('VulkanShowEdges', '?')} "
        f"points={prefs.get('VulkanShowPoints', '?')}")


def build_scene():
    FreeCADGui.activateWorkbench("PartWorkbench")
    doc = FreeCAD.newDocument("Prefs")
    box = doc.addObject("Part::Box", "Box")
    box.Label = "PrefBox"
    box.Length = 10
    box.Width = 10
    box.Height = 10
    doc.recompute()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.viewTop()
    view.fitAll()


def step():
    steps[0] += 1
    k = steps[0]
    if k == 2:
        # reset the overlays to a clean baseline BEFORE the box renders.
        # Force raster mode (1): the edge/point overlay is only drawn by the
        # raster backend, not the RTX path tracer.
        s.set_pref(VIEW, "VulkanRenderMode", 1)
        apply_phase(0)
        build_scene()
    elif k == 3:
        log("settled baseline; turning edges on")
        phase_idx[0] = 1
        apply_phase(1)
    elif k == 5:
        log("edges on; turning points on")
        phase_idx[0] = 2
        apply_phase(2)
    elif k == 7:
        log("points on; snapshot + finish")
        s.snapshot()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(650, step)


QtCore.QTimer.singleShot(500, step)
