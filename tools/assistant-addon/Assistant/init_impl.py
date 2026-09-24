"""Assistant - workbench/command definitions (imported normally, not exec'd).

FreeCAD exec()s the addon's ``InitGui.py`` with quirky scope, so all real logic
lives here in a normally-imported module where ``__file__`` and module scope are
available.  ``InitGui.py`` simply imports this and registers.
"""

import os

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
        _ensure_mcp_statusbar_button(mw)
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


_mcp_leds = []


def _led_icon(on):
    """A small filled circle: green when the MCP server is up, grey when down."""
    pm = QtGui.QPixmap(16, 16)
    pm.fill(QtCore.Qt.transparent)
    p = QtGui.QPainter(pm)
    p.setRenderHint(QtGui.QPainter.Antialiasing)
    p.setPen(QtGui.QPen(QtGui.QColor("#0b0b0b"), 1))
    p.setBrush(QtGui.QColor("#2ecc40" if on else "#6b6b6b"))
    p.drawEllipse(3, 3, 10, 10)
    p.end()
    return QtGui.QIcon(pm)


def _refresh_mcp_led():
    if not _mcp_leds:
        return
    try:
        import McpControl
        on = McpControl.is_running()
    except Exception:
        on = False
    icon = _led_icon(on)
    tip = "MCP server: {} (click to turn {})".format(
        "ON" if on else "OFF", "off" if on else "on"
    )
    for led in list(_mcp_leds):
        try:
            led.setIcon(icon)
            led.setToolTip(tip)
        except RuntimeError:
            # Underlying C++ widget was deleted (toolbar rebuilt).
            _mcp_leds.remove(led)


def _make_led_button(parent, objname):
    btn = QtWidgets.QToolButton(parent)
    btn.setObjectName(objname)
    btn.setAutoRaise(True)
    btn.setCursor(QtCore.Qt.PointingHandCursor)
    btn.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
    btn.setText("MCP")
    btn.clicked.connect(toggle_mcp)
    _mcp_leds.append(btn)
    return btn


def toggle_mcp():
    """Flip the MCP server on/off and refresh the LED."""
    try:
        import McpControl
        McpControl.toggle()
    except Exception:
        pass
    _refresh_mcp_led()


def _install_mcp_button():
    """Insert the LED button into the Assistant toolbar, before its first action.

    Retries because the toolbar is built after the workbench initializes."""
    mw = FreeCADGui.getMainWindow()
    if mw is None:
        return False
    for tb in mw.findChildren(QtWidgets.QToolBar):
        for act in tb.actions():
            if act.objectName() == "Assistant_Toggle":
                if tb.findChild(QtWidgets.QToolButton, "AssistantMcpLed") is None:
                    tb.insertWidget(act, _make_led_button(tb, "AssistantMcpLed"))
                _refresh_mcp_led()
                return True
    return False


def _ensure_mcp_statusbar_button(mw):
    """A persistent LED next to the chat button: survives workbench switches, so
    the MCP state stays visible even when the Assistant workbench isn't active."""
    sb = mw.statusBar()
    if sb is None:
        return
    if sb.findChild(QtWidgets.QToolButton, "AssistantMcpStatusLed") is None:
        sb.addPermanentWidget(_make_led_button(sb, "AssistantMcpStatusLed"))
    _refresh_mcp_led()


def _schedule_mcp_button():
    retries = {"left": 40}

    def _try():
        try:
            if _install_mcp_button():
                return
        except Exception:
            pass
        if retries["left"] > 0:
            retries["left"] -= 1
            QtCore.QTimer.singleShot(300, _try)

    QtCore.QTimer.singleShot(0, _try)


def _start_mcp_from_pref():
    """Bring the MCP server up/down to match the persisted setting at startup."""
    try:
        import McpControl
        McpControl.apply_setting()
    except Exception:
        pass


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
        _schedule_mcp_button()

    def Activated(self):
        # Re-add the LED if the toolbar was rebuilt on a workbench switch.
        _schedule_mcp_button()

    def GetClassName(self):
        # Must be the Python workbench type: FreeCAD injects ``__Workbench__``
        # (needed by appendToolbar/appendMenu) only when GetClassName() names a
        # class derived from Gui::PythonBaseWorkbench.  "Gui::Workbench" is not,
        # so the toolbar calls raised "'AssistantWorkbench' object has no
        # attribute '__Workbench__'" and the Assistant toolbar never appeared.
        return "Gui::PythonWorkbench"


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


class AssistantMcpToggleCmd:
    """Start/stop the MCP socket server (the toolbar LED button does the same)."""
    def GetResources(self):
        return {"Pixmap": _icon(), "MenuText": "Toggle MCP Server",
                "ToolTip": "Start or stop the MCP server for external clients"}

    def IsActive(self):
        return True

    def Activated(self):
        toggle_mcp()


# Dock the assistant chat by default (tabified under FreeCAD's Tasks panel).
_schedule_default_layout()

# Install the toolbar LED if the Assistant toolbar already exists, and keep the
# persistent status-bar LED in sync.
_schedule_mcp_button()

# Bring the MCP server up (or down) to match the persisted setting.
_start_mcp_from_pref()
