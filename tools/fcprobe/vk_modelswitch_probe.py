#!/usr/bin/env python3
"""Probe: Coin renderer -> Vulkan mode switch.

Reproduces the user's report "can't change from coin renderer to vulkan".

The probe:
  * Opens a document with a box (PartWorkbench).
  * Handles the active-view carefully (view may exist only after a doc+window).
  * Mirrors the status-bar mode combo: uses the ViewRenderingMode combo if
    present, else calls view.setRenderMode() directly.
  * Steps through RasterCoin(0) -> RasterVulkan(1) -> back -> forward,
    logging the effective renderMode and the QStackedWidget currentWidget.
  * Verdict PASS if after selecting RasterVulkan the effective renderMode is
    RasterVulkan AND the visible surface is the Vulkan widget.

Run:
  FreeCAD vk_modelswitch_probe.py
"""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("MODESW " + msg, file=sys.stderr)


s = Session(name="modelswitch")
steps = [0]


def combo():
    mw = FreeCADGui.getMainWindow()
    if mw is None:
        return None
    return mw.findChild(QtWidgets.QComboBox, "ViewRenderingMode")


def active_view():
    doc = FreeCADGui.ActiveDocument
    if doc is None:
        return None
    try:
        return doc.ActiveView
    except Exception:
        return None


def report(tag):
    view = active_view()
    if view is None:
        log(f"[{tag}] no active view")
        return
    mode = int(view.getRenderMode())
    frame = 0
    try:
        frame = view.getVulkanFrameCount()
    except Exception:
        pass
    mw = FreeCADGui.getMainWindow()
    stack = mw.findChild(QtWidgets.QStackedWidget) if mw else None
    cur = "none"
    if stack and stack.currentWidget():
        cur = stack.currentWidget().__class__.__name__
    c = combo()
    cstr = f"combo={'present' if c is not None else 'ABSENT'}"
    if c is not None:
        cstr += f"(idx={c.currentIndex()})"
    log(f"[{tag}] renderMode={mode} frame={frame} stackCur={cur} {cstr}")


def build():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    doc = FreeCAD.newDocument("MSwitch")
    box = doc.addObject("Part::Box", "Box")
    box.Length = 50
    box.Width = 50
    box.Height = 50
    doc.recompute()
    FreeCADGui.updateGui()
    FreeCADGui.Selection.clearSelection()
    view = FreeCADGui.ActiveDocument.ActiveView
    view.setAnimationEnabled(False)
    view.viewIsometric()
    view.fitAll()
    FreeCADGui.updateGui()
    return view


def set_mode(mode):
    """Switch render mode via the status-bar combo if present, else direct."""
    c = combo()
    if c is not None:
        c.setCurrentIndex(mode)
        FreeCADGui.updateGui()
        QtCore.QCoreApplication.processEvents()
        log(f"via combo index={mode}")
    else:
        view = active_view()
        if view is not None:
            view.setRenderMode(mode)
            FreeCADGui.updateGui()
            QtCore.QCoreApplication.processEvents()
            log(f"via setRenderMode mode={mode}")


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        FreeCADGui.activateWorkbench("PartWorkbench")
        FreeCADGui.updateGui()
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "VulkanRenderMode", 0)
        s.set_pref(VIEW, "VulkanPathTracing", False)
        build()
        # Explicitly start in coin after the view exists.
        v = active_view()
        if v is not None:
            v.setRenderMode(0)
            FreeCADGui.updateGui()
        report("init-coin")
    elif k == 12:
        set_mode(1)
        report("coin->vulkan")
    elif k == 24:
        set_mode(0)
        report("vulkan->coin")
    elif k == 36:
        set_mode(1)
        report("coin->vulkan-2")
    elif k == 48:
        view = active_view()
        mode = int(view.getRenderMode()) if view is not None else -1
        log(f"final effective renderMode={mode}")
        s.snapshot()
        s.finish("effective renderMode after coin->vulkan")
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(350, step)


QtCore.QTimer.singleShot(600, step)
