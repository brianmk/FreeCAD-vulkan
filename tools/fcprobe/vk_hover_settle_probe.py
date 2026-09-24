#!/usr/bin/env python3
"""Regression: hovering faces must not restart the path-tracing accumulation.

Guards the geometry-cache identity fix (SoRTXRenderBackendGeometry.cpp).  The
draw list is a per-frame arena, so a hover highlight that reorders commands
while keeping the command count (and same-object face commands that share a
model matrix) let the traced path trust a stale pointer-map slot: it was
misread as a geometry *content* edit, overwriting another object's cache entry
(the "other planes change colour on hover" symptom) and setting cacheChanged,
which reset the accumulation/denoiser on every hover frame.  A raw-memcmp
transform detector compounded it: a z=0 placement can flip a translation
component between -0.0f and +0.0f (same value, different bits), read as a move.

The probe opens the repo's BIMExample (the scene that reproduces it), enters
PathTracing, lets it converge, then sweeps the cursor across the model so the
preselection highlight reorders the draw list.  Run it with FC_VULKAN_RT_DEBUG=1
so the [RTDBG] ptState lines carry sceneChanged/frameIndex, and
FC_VULKAN_RT_GEO=1 for the [GCR] geometry-cache churn trace.  The adjacent
vk_hover_settle_probe.check.py asserts there is no sceneChanged=1 after the run
has settled, i.e. the accumulation survives the whole hover sweep.
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
DOC = os.path.join(REPO, "data", "examples", "BIMExample.FCStd")

# One step == one 250 ms timer tick.  SETTLE must comfortably exceed the
# sample cap + settle idle so the build resets are over before the sweep.
SETTLE = int(os.environ.get("FC_HOVER_SETTLE", "70"))
GRID = int(os.environ.get("FC_HOVER_GRID", "8"))
DONE = SETTLE + GRID * GRID + 2


def log(msg):
    print("HOVER-SETTLE " + msg, file=sys.stderr)


s = Session(name="hover-settle")
steps = [0]


def hover_point(i):
    """Serpentine grid over the middle of the viewport (logical px)."""
    a = i % GRID
    b = i // GRID
    if b % 2:
        a = GRID - 1 - a
    return (int(s.width * (0.10 + 0.80 * a / max(GRID - 1, 1))),
            int(s.height * (0.12 + 0.66 * b / max(GRID - 1, 1))))


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "VulkanRenderMode", 4)  # PathTracing
        s.set_pref(VIEW, "VulkanPathTracingBounces", 2)
        s.set_pref(VIEW, "VulkanPathTracingSettle", 4)
        s.set_pref(VIEW, "VulkanPathTracingMaxSamples", 32)
        s.set_pref(VIEW, "VulkanPathTracingDenoiser", 1)  # oidn
        FreeCADGui.activateWorkbench("PartWorkbench")
        FreeCAD.openDocument(DOC)
        FreeCADGui.updateGui()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        s.frame_phase("build")
        log("phase=build doc=%s" % DOC)
    elif k == SETTLE:
        s.frame_phase("converged")
        log("phase=converged")
    elif SETTLE < k <= SETTLE + GRID * GRID:
        x, y = hover_point(k - SETTLE - 1)
        s.move(x, y)
    elif k == DONE:
        pre = FreeCADGui.Selection.getPreselection()
        log("hover_done preselect=%s"
            % (list(pre.SubElementNames) if pre else None))
        s.frame_phase("hover_done")
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(250, step)


QtCore.QTimer.singleShot(400, step)
