#!/usr/bin/env python3
"""Diagnostic: during a Vulkan render run, report which viewers/windows are
live and whether GL contexts are being made current (i.e. an invisible GL
render happening in the background)."""
import os
import sys
import time

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

USE_VULKAN = os.environ.get("FC_RENDERER", "vulkan") != "gl"
s = Session(name="vkscope")
steps = [0]
doc = [None]
viewer_ref = [None]


def log(m):
    print("[VKSCOPE] %s" % m, file=sys.stderr, flush=True)


def step():
    steps[0] += 1
    k = steps[0]
    try:
        if k == 1:
            from pivy import coin  # noqa: F401
            FreeCADGui.activateWorkbench("PartWorkbench")
            for name in list(FreeCAD.listDocuments()):
                FreeCAD.closeDocument(name)
        elif k == 2:
            doc[0] = FreeCAD.newDocument("Scope")
            b = doc[0].addObject("Part::Box", "box")
            b.Length = 10
            doc[0].recompute()
            FreeCADGui.SendMsgToActiveView("ViewFit")
        elif k == 3:
            from PySide import QtWidgets
            v = FreeCADGui.ActiveDocument.ActiveView
            viewer_ref[0] = v
            mw = FreeCADGui.getMainWindow()
            log("main window: %r" % type(mw).__name__)
            log("central widget: %r" % type(mw.centralWidget()).__name__)
            # Find the QStackedWidget and enumerate its children
            for w in mw.findChildren(QtWidgets.QStackedWidget):
                log("QStackedWidget found: %s children=%d current=%r" % (
                    w.objectName(), w.count(),
                    type(w.currentWidget()).__name__ if w.currentWidget() else None))
                for i in range(w.count()):
                    wd = w.widget(i)
                    log("  slot %d: %r visible=%s hidden=%s" % (
                        i, type(wd).__name__, wd.isVisible(), wd.isHidden()))
            # Enumerate all top-level windows
            log("top-level windows:")
            for tw in QtWidgets.QApplication.topLevelWidgets():
                log("  %r visible=%s type=%s" % (
                    type(tw).__name__, tw.isVisible(), tw.windowType()))
            log("mdi children:")
            for mc in mw.findChildren(QtWidgets.QMdiSubWindow):
                log("  subwindow %r inner=%r visible=%s" % (
                    type(mc).__name__,
                    type(mc.widget()).__name__ if mc.widget() else None,
                    mc.isVisible()))
    except Exception:
        import traceback
        traceback.print_exc()
        s.finish()
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(150, step)


QtCore.QTimer.singleShot(300, step)


def done():
    s.verdict("PASS")
    s.finish()
    FreeCADGui.getMainWindow().close()


QtCore.QTimer.singleShot(4000, done)
