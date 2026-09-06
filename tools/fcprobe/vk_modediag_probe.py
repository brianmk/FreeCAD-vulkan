#!/usr/bin/env python3
"""Diagnose the status-bar render-mode combo + viewport stack identity."""

import os
import sys

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freecad_probe import Session  # noqa: E402

VIEW = "User parameter:BaseApp/Preferences/View"


def log(msg):
    print("DIAG " + msg, file=sys.stderr)


s = Session(name="diag")
steps = [0]


def dump_combos(tag):
    mw = FreeCADGui.getMainWindow()
    if mw is None:
        log(f"[{tag}] MainWindow=None")
        return
    # The status bar selector is a QComboBox added via addStatusBarItem.
    sb = mw.statusBar()
    log(f"[{tag}] statusbar={'present' if sb else 'None'}")
    if sb:
        for w in sb.findChildren(QtWidgets.QComboBox):
            log(f"[{tag}] statusbar combo object='{w.objectName()}' count={w.count()} "
                f"texts={[w.itemText(i) for i in range(w.count())]}")
    # All combos anywhere under the main window.
    for w in mw.findChildren(QtWidgets.QComboBox):
        log(f"[{tag}] any combo object='{w.objectName()}' visible={w.isVisible()} "
            f"enabled={w.isEnabled()} count={w.count()}")


def dump_stacks(tag):
    mw = FreeCADGui.getMainWindow()
    if mw is None:
        return
    for st in mw.findChildren(QtWidgets.QStackedWidget):
        log(f"[{tag}] stack object='{st.objectName()}' pages={st.count()} "
            f"cur={st.currentIndex()} curCls={st.currentWidget().__class__.__name__ if st.currentWidget() else 'none'}")
        for i in range(st.count()):
            w = st.widget(i)
            wn = w.__class__.__name__
            wigname = getattr(w, "windowTitle", lambda: "")() or w.objectName()
            log(f"[{tag}]   page[{i}] cls={wn} obj='{w.objectName()}' title='{wigname}'")


def step():
    steps[0] += 1
    k = steps[0]
    if k == 1:
        FreeCADGui.activateWorkbench("PartWorkbench")
        FreeCADGui.updateGui()
        s.set_pref(VIEW, "UseVulkanRenderer", True)
        s.set_pref(VIEW, "VulkanRenderMode", 0)
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        doc = FreeCAD.newDocument("D")
        box = doc.addObject("Part::Box", "Box")
        box.Length = 50
        box.Width = 50
        box.Height = 50
        doc.recompute()
        FreeCADGui.updateGui()
        v = FreeCADGui.ActiveDocument.ActiveView
        v.viewIsometric()
        v.fitAll()
        FreeCADGui.updateGui()
        log("built+view created")
    elif k == 10:
        dump_combos("t10")
        dump_stacks("t10")
    elif k == 14:
        s.finish("combo/stack diagnostics")
        FreeCADGui.getMainWindow().close()
        return
    QtCore.QTimer.singleShot(350, step)


QtCore.QTimer.singleShot(700, step)
