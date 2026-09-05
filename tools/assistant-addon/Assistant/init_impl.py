"""Assistant - workbench/command definitions (imported normally, not exec'd).

FreeCAD exec()s the addon's ``InitGui.py`` with quirky scope, so all real logic
lives here in a normally-imported module where ``__file__`` and module scope are
available.  ``InitGui.py`` simply imports this and registers.
"""

import os
import sys

import FreeCADGui

from PySide import QtCore, QtGui, QtWidgets

ADON_DIR = os.path.dirname(os.path.abspath(__file__))
_panel = None
_layout_installed = False


def _icon(name="assistant.svg"):
    p = os.path.join(ADON_DIR, "Resources", name)
    return p if os.path.exists(p) else None


def get_panel():
    """Return (creating on first use) the singleton assistant chat dock."""
    global _panel
    if _panel is None:
        from ChatPanel import ChatPanel
        _panel = ChatPanel()
    return _panel


def open_panel():
    """Show + focus the assistant chat (unlike toggle_panel, always opens)."""
    p = get_panel()
    p.show()
    p.raise_()
    if hasattr(p, "focus_input"):
        p.focus_input()


def install_default_layout():
    """Dock the assistant chat by default, tabified under FreeCAD's 'Tasks' panel.

    Idempotent. Returns True once it has tabified the panel into the window
    (or False if the main window / Tasks dock isn't ready yet, so the caller can
    retry after the startup winds down).
    """
    global _layout_installed
    if _layout_installed:
        return True
    mw = FreeCADGui.getMainWindow()
    if mw is None:
        return False
    try:
        p = get_panel()
        p.setFeatures(QtWidgets.QDockWidget.DockWidgetClosable)  # locked (no float/move)
        if p.parent() is None:
            mw.addDockWidget(QtCore.Qt.RightDockWidgetArea, p)
        tasks = mw.findChild(QtWidgets.QDockWidget, "Tasks")
        if tasks is not None:
            mw.tabifyDockWidget(tasks, p)
        p.show()
        p.raise_()
        _layout_installed = True
        _ensure_statusbar_button(mw)
        return True
    except Exception:
        return False


def _ensure_statusbar_button(mw):
    """Small 'open chat' button in the status bar, next to the units/system
    widgets at the right.  Status-bar permanent widgets persist across workbench
    switches (unlike workbench-owned toolbars/menus)."""
    sb = mw.statusBar()
    if sb is None:
        return
    if sb.findChild(QtWidgets.QToolButton, "AssistantChatButton") is not None:
        return
    btn = QtWidgets.QToolButton(sb)
    btn.setObjectName("AssistantChatButton")
    btn.setAutoRaise(True)
    btn.setCursor(QtCore.Qt.PointingHandCursor)
    btn.setToolTip("Open the Assistant chat")
    ic = os.path.join(ADON_DIR, "Resources", "assistant.svg")
    if os.path.exists(ic):
        btn.setIcon(QtGui.QIcon(ic))
    btn.setText("🗨")  # fallback glyph when the icon cannot be loaded
    btn.clicked.connect(lambda: open_panel())
    sb.addPermanentWidget(btn)


def _schedule_default_layout():
    """Defer dock install until the GUI event loop settles; retry until the Tasks
    dock exists (it appears once a workbench with a task panel is active)."""
    retries = {"left": 40}

    def _try():
        try:
            if install_default_layout():
                return
        except Exception:
            pass
        if retries["left"] > 0:
            retries["left"] -= 1
            QtCore.QTimer.singleShot(300, _try)

    QtCore.QTimer.singleShot(0, _try)


def toggle_panel():
    p = get_panel()
    if p.isVisible():
        p.hide()
    else:
        p.show()
        p.raise_()
        if hasattr(p, "focus_input"):
            p.focus_input()


class AssistantWorkbench(FreeCADGui.Workbench):
    MenuText = "Assistant"
    ToolTip = "AI chat dock that drives any FreeCAD tool"
    Icon = _icon()

    def Initialize(self):
        self.appendToolbar("Assistant", ["Assistant_Toggle", "Assistant_Clear"])
        self.appendMenu("Assistant", ["Assistant_Toggle", "Assistant_Clear"])

    def GetClassName(self):
        return "Gui::Workbench"


class AssistantToggleCmd:
    """Show/hide the assistant chat dock (global shortcut reopens it anywhere)."""
    def GetResources(self):
        return {"Pixmap": _icon(), "MenuText": "Assistant Chat",
                "ToolTip": "Open the AI chat dock",
                "Shortcut": "Ctrl+Shift+A"}

    def IsActive(self):
        return True

    def Activated(self):
        toggle_panel()


class AssistantClearCmd:
    def GetResources(self):
        return {"Pixmap": _icon("clear.svg"), "MenuText": "Clear Assistant Chat",
                "ToolTip": "Clear the current assistant conversation"}

    def IsActive(self):
        return True

    def Activated(self):
        p = get_panel()
        if hasattr(p, "clear_conversation"):
            p.clear_conversation()


# Dock the assistant chat by default (tabified under FreeCAD's Tasks panel).
_schedule_default_layout()
