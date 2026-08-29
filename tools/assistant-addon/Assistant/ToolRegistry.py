"""Assistant - in-process FreeCAD tool registry.

Loads the *vendored* MCP guest (the same HANDLERS / handle_request used by the
freecad_mcp_server, imported directly with no socket) plus the tool JSON Schemas
generated offline.  Tool execution is marshalled onto FreeCAD's main thread (Qt
queued invoke) so scene/UI mutations never race the viewport.
"""

import json
import os
import sys
import threading

from PySide import QtCore

VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
_SCHEMA = os.path.join(VENDOR, "tool_schemas.json")

_GUEST = None
_SCHEMAS = None


def _load_guest():
    global _GUEST, _SCHEMAS
    if _GUEST is None:
        import importlib.util
        path = os.path.join(VENDOR, "freecad_mcp_guest.py")
        spec = importlib.util.spec_from_file_location("_assistant_guest", path)
        mod = importlib.util.module_from_spec(spec)
        sys.modules["_assistant_guest"] = mod
        spec.loader.exec_module(mod)
        _GUEST = mod
    if _SCHEMAS is None:
        with open(_SCHEMA, "r", encoding="utf-8") as fh:
            _SCHEMAS = json.load(fh)
    return _GUEST


class ToolError(RuntimeError):
    def __init__(self, msg, tool=None):
        self.tool = tool
        super().__init__(msg or f"tool {tool!r} failed")


class _MainExecutor(QtCore.QObject):
    """Lives on FreeCAD's main thread; executes callables queued from workers."""
    _dispatch = QtCore.Signal(object)

    def __init__(self):
        super().__init__()
        self._dispatch.connect(self._do, QtCore.Qt.QueuedConnection)

    def _do(self, payload):
        fn, holder, done = payload
        try:
            holder["value"] = fn()
        except Exception as exc:  # noqa: BLE001
            holder["error"] = exc
        done.set()


_executor = None


def _get_executor():
    global _executor
    if _executor is None:
        from PySide import QtWidgets
        _executor = _MainExecutor()
        app = QtWidgets.QApplication.instance()
        if app is not None and QtCore.QThread.currentThread() != app.thread():
            _executor.moveToThread(app.thread())
    return _executor


def _run_on_main(fn, timeout=180.0):
    """Run `fn` on FreeCAD's main thread and return its result (or raise)."""
    from PySide import QtCore, QtWidgets

    app = QtWidgets.QApplication.instance()
    if app is None:
        return fn()

    # Already on FreeCAD's main thread -> run synchronously (a queued invoke would
    # deadlock, since we are currently inside the event loop's callback).
    if QtCore.QThread.currentThread() == app.thread():
        return fn()

    holder, done = {}, threading.Event()
    _get_executor()._dispatch.emit((fn, holder, done))
    if not done.wait(timeout):
        raise ToolError("tool timed out waiting for FreeCAD's main thread")
    if "error" in holder:
        raise holder["error"]
    return holder["value"]


def schemas():
    """LLM tool definitions (name/description/inputSchema) for function calling.
    Only tools that actually have a guest handler are offered - the MCP server
    has a few wrappers (e.g. `status`) that the guest cannot dispatch."""
    _load_guest()
    return [t for t in _SCHEMAS if t["name"] in _GUEST.HANDLERS]


def names():
    return [t["name"] for t in schemas()]


def call(name, params=None):
    """Dispatch a tool call in-process.  `params` is a dict of keyword args."""
    _load_guest()
    req = {"method": name, "params": params or {}}
    res = _run_on_main(lambda: _GUEST.handle_request(req))
    if not isinstance(res, dict) or not res.get("ok"):
        err = (res or {}).get("error") if isinstance(res, dict) else None
        raise ToolError(err, name)
    return res.get("data")


def availability():
    """name -> True when a matching guest handler exists (schemas can outpace
    the registry)."""
    g = _load_guest()
    return {t["name"]: (t["name"] in g.HANDLERS) for t in _SCHEMAS}
