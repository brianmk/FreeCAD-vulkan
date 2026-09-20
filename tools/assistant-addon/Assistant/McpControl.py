"""Assistant - MCP socket server control.

Starts/stops the *vendored* MCP guest's socket listener (the same module the
ToolRegistry dispatches tool calls through) so an external MCP client can drive
this live FreeCAD session.  The persisted setting lives in Preferences
(``McpEnabled``); the Assistant toolbar LED button toggles it at runtime.
"""

import Preferences as P
import ToolRegistry


def _guest():
    # Same module instance ToolRegistry uses, so the socket listener and the
    # in-process handlers share one registry/state.
    return ToolRegistry._load_guest()


def is_running():
    try:
        return bool(_guest().guest_running())
    except Exception:  # noqa: BLE001
        return False


def start():
    g = _guest()
    if not g.guest_running():
        g.start_guest()
    return g.guest_running()


def stop():
    g = _guest()
    if g.guest_running():
        g.stop_guest()
    return g.guest_running()


def enabled():
    """The persisted setting (independent of the live state)."""
    return P.mcp_enabled()


def set_enabled(value):
    """Persist the setting and bring the server up/down to match."""
    value = bool(value)
    P.set_mcp_enabled(value)
    try:
        return start() if value else stop()
    except Exception:  # noqa: BLE001
        return is_running()


def apply_setting():
    """Reconcile the live server with the persisted setting (startup / prefs OK)."""
    return set_enabled(enabled())


def toggle():
    """Flip both the live state and the setting; returns the new state."""
    return set_enabled(not is_running())
