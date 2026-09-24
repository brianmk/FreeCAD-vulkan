#!/usr/bin/env python3
"""Regression: the Create-sketch camera rotation animation.

When the sketch attachment editor reveals the (temporarily enlarged) origin
planes over an empty body, the view rotates with an eased animation to the
axonometric view; the duration is the NewSketchViewAnimationDuration
preference (seconds, 0.6 by default).  The rotation must not happen when an
object/face is already selected to attach to.

The probe drives the real command twice and samples the camera view direction:

  phase=unselected  nothing selected -> the view sweeps to isometric over the
                    configured duration, with intermediate orientations (i.e.
                    it animates, it does not snap);
  phase=selected    a single origin plane selected -> the view stays put.

Asserts the same invariants in-process (s.error -> s.finish verdict) and in
the script-adjacent host check (vk_sketch_view_anim_probe.check.py), which
re-parses the emitted lines.

Run standalone or via `freecad_probe.py suite` (vk_suite.json).
"""

import math
import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore
from pivy import coin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"
PD = "User parameter:BaseApp/Preferences/Mod/PartDesign"
DURATION = 1.0  # seconds used for the unselected phase

# Camera::rotation(Isometric) looks from (1,-1,1) towards the origin, so the
# view direction is its negation normalised.
ISO_DIR = (-1.0 / math.sqrt(3.0), 1.0 / math.sqrt(3.0), -1.0 / math.sqrt(3.0))

s = Session(name="sketch-view-anim")


def settle(ms):
    t = QtCore.QElapsedTimer()
    t.start()
    while t.elapsed() < ms:
        QtCore.QCoreApplication.processEvents()
        QtCore.QThread.msleep(5)


def viewdir(cam):
    out = cam.orientation.getValue().multVec(coin.SbVec3f(0.0, 0.0, -1.0))
    return (out[0], out[1], out[2])


def angle(a, b):
    dot = max(-1.0, min(1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))
    return math.degrees(math.acos(dot))


def sweep(seconds):
    """Sample the view direction for `seconds` -> (start_dir, rows[(ms, deg)])."""
    cam = FreeCADGui.activeView().getCameraNode()
    start = viewdir(cam)
    t = QtCore.QElapsedTimer()
    t.start()
    rows = []
    while t.elapsed() < seconds * 1000.0:
        QtCore.QCoreApplication.processEvents()
        QtCore.QThread.msleep(50)
        rows.append((t.elapsed(), angle(start, viewdir(cam))))
    return start, rows


def motion_metrics(rows):
    """(motion_span_s, intermediate_sample_count) for a sweep trace.

    The span is first-sample-above-2%-to-first-sample-above-98% of the total
    sweep; the count is samples strictly between 5% and 95% (a snap yields 0).
    """
    total = max((deg for _, deg in rows), default=0.0)
    if total <= 0.0:
        return 0.0, 0
    t_start = next((ms for ms, deg in rows if deg > 0.02 * total), None)
    t_end = next((ms for ms, deg in rows if deg >= 0.98 * total), None)
    if t_start is None or t_end is None:
        return 0.0, 0
    intermediate = sum(1 for _, deg in rows if 0.05 * total < deg < 0.95 * total)
    return (t_end - t_start) / 1000.0, intermediate


def set_vulkan_raster():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    p = FreeCAD.ParamGet(VIEW)
    p.SetBool("UseVulkanRenderer", True)
    p.SetInt("VulkanRenderMode", 1)  # RasterVulkan
    FreeCADGui.activateWorkbench("PartDesignWorkbench")


def new_empty_body(name):
    doc = FreeCAD.newDocument(name)
    doc.addObject("PartDesign::Body", "Body")
    doc.recompute()
    FreeCADGui.updateGui()
    return doc


def restore_prefs():
    FreeCAD.ParamGet(PD).RemFloat("NewSketchViewAnimationDuration")
    FreeCAD.ParamGet(PD).SetBool("NewSketchUseAttachmentDialog", False)


def close_dialog_and_docs():
    try:
        FreeCADGui.Control.closeDialog()
    except Exception:
        pass
    settle(200)
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    settle(100)


def step_unselected():
    FreeCAD.ParamGet(PD).SetFloat("NewSketchViewAnimationDuration", DURATION)
    doc = new_empty_body("SketchViewAnim")
    view = FreeCADGui.activeView()
    view.setAnimationEnabled(False)
    view.viewFront()
    view.setAnimationEnabled(True)
    settle(400)
    FreeCADGui.Selection.clearSelection()
    s.emit("sketchviewanim", phase="unselected", dur=DURATION)
    ok = s.command("PartDesign_NewSketch")
    _, rows = sweep(1.8)
    span, intermediate = motion_metrics(rows)
    final = viewdir(view.getCameraNode())
    from_iso = angle(final, ISO_DIR)
    s.emit(
        "sketchviewanim_result",
        phase="unselected",
        command_ok=int(ok),
        from_iso="%.2f" % from_iso,
        intermediate=intermediate,
        span="%.2f" % span,
    )
    if not ok:
        s.error("PartDesign_NewSketch did not run")
    if from_iso > 3.0:
        s.error("view did not settle on isometric (%.2f deg off)" % from_iso)
    if intermediate < 3:
        s.error("no intermediate orientations: the view snapped, it did not animate")
    if not 0.4 <= span <= 1.8:
        s.error("rotation span %.2fs is not near the configured %.1fs" % (span, DURATION))
    close_dialog_and_docs()


def step_selected():
    # Force the attachment editor for a single plane selection (otherwise the
    # fast path creates the sketch without ever showing the origin planes).
    FreeCAD.ParamGet(PD).SetBool("NewSketchUseAttachmentDialog", True)
    doc = new_empty_body("SketchViewAnimSel")
    view = FreeCADGui.activeView()
    view.setAnimationEnabled(False)
    view.viewFront()
    view.setAnimationEnabled(True)
    settle(400)
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(doc.Name, "XY_Plane")
    settle(150)
    s.emit("sketchviewanim", phase="selected",
           selected=len(FreeCADGui.Selection.getSelectionEx()))
    s.command("PartDesign_NewSketch")
    _, rows = sweep(1.2)
    max_angle = max((deg for _, deg in rows), default=0.0)
    s.emit("sketchviewanim_result", phase="selected", max_angle="%.2f" % max_angle)
    if max_angle > 3.0:
        s.error("view rotated %.2f deg despite a selected plane" % max_angle)


def step_done():
    restore_prefs()
    close_dialog_and_docs()
    s.finish()
    FreeCADGui.getMainWindow().close()


STEP = [0]


def run_step():
    STEP[0] += 1
    try:
        if STEP[0] == 1:
            set_vulkan_raster()
        elif STEP[0] == 2:
            step_unselected()
        elif STEP[0] == 3:
            step_selected()
        elif STEP[0] == 4:
            step_done()
            return
    except Exception:
        import traceback

        traceback.print_exc()
        restore_prefs()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(400, run_step)


QtCore.QTimer.singleShot(400, run_step)
