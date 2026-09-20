"""Assistant - preferences (FreeCAD parameter group with defaults).

Layered config, lowest precedence first:
    env  DEEPSEEK_API_KEY            -> the API key if set
    pref User parameter:BaseApp/Preferences/Mod/Assistant/*  -> everything else
"""

import json
import os

_GROUP = "BaseApp/Preferences/Mod/Assistant"


class PreferencesPage:
    """Preferences page (Edit > Preferences > Assistant).

    FreeCAD instantiates it and calls ``loadSettings``/``saveSettings`` around
    the dialog.  The MCP checkbox is the persistent form of the toolbar LED
    button's state."""

    def __init__(self, parent=None):
        from PySide import QtGui

        self.form = QtGui.QWidget()
        self.form.setWindowTitle("Assistant")

        layout = QtGui.QVBoxLayout(self.form)
        self._mcp = QtGui.QCheckBox("Enable the MCP server for external clients")
        self._mcp.setToolTip(
            "Expose FreeCAD's MCP tool socket while this session runs, so an "
            "external MCP client can drive it. The LED button in the Assistant "
            "toolbar toggles the same setting at runtime."
        )
        layout.addWidget(self._mcp)
        layout.addStretch(1)
        self.loadSettings()

    def loadSettings(self):
        self._mcp.setChecked(mcp_enabled())

    def saveSettings(self):
        set_mcp_enabled(self._mcp.isChecked())
        # Apply immediately (the dialog's OK may be the only interaction).
        try:
            import McpControl
            McpControl.apply_setting()
        except Exception:  # noqa: BLE001
            pass

MODE_AUTO = "auto"
MODE_APPROVE = "approve"
MODE_DRY = "dry-run"

_DEFAULTS = {
    "Endpoint": "https://api.deepseek.com",
    "Model": "deepseek-v4-flash",
    "VisionModel": "deepseek-v4-flash-vision-exp",
    "ApiKey": "",
    "Mode": MODE_APPROVE,
    "MaxTurns": 12,
    "Timeout": 120,
    "Temperature": 0.2,
    "ToolAllowList": json.dumps([]),   # ["run_python", ...] or [] = all allowed
    "ToolBlockList": json.dumps([]),   # deny-listed tools
    "IncludeContext": True,
    "IncludeSelection": True,
    "IncludeLogs": True,
    "VisionEnabled": True,
    "Debug": False,
    "McpEnabled": True,
}

# Model names that deepseek no longer accepts (legacy defaults we shipped).  If a
# stale preference still holds one of these we ignore it and use the valid default.
_BAD_MODELS = {"deepseek-chat", "deepseek-vl2", "deepseek-reasoner", "deepseek-v3"}


def _get():
    import FreeCAD as App
    return App.ParamGet("User parameter:" + _GROUP)


def get(key, default=None):
    p = _get()
    fallback = _DEFAULTS.get(key, default)
    if isinstance(fallback, bool):
        return bool(p.GetBool(key, fallback))
    if isinstance(fallback, int):
        return int(p.GetInt(key, fallback))
    if isinstance(fallback, float):
        return float(p.GetFloat(key, fallback))
    return p.GetString(key, str(fallback if fallback is not None else ""))


def set(key, value):
    p = _get()
    if isinstance(value, bool):
        p.SetBool(key, value)
    elif isinstance(value, int):
        p.SetInt(key, value)
    elif isinstance(value, float):
        p.SetFloat(key, value)
    else:
        p.SetString(key, str(value))
    return value


def api_key():
    """Resolve the API key: DEEPSEEK_API_KEY env -> pref -> JSON store, sanitised
    to a single token (no whitespace/newlines, which would break the auth header)."""
    k = os.environ.get("DEEPSEEK_API_KEY") or get("ApiKey", "") or _read_key_file()
    return _clean_key(k)


def set_api_key(key):
    """Persist the key to BOTH the pref and a JSON file (the file survives a
    SIGKILL, unlike the user.cfg flush, so the key is remembered across runs)."""
    k = _clean_key(key)
    set("ApiKey", k)
    _write_key_file(k)
    return k


def _clean_key(k):
    k = (k or "").strip()
    return "".join(k.split())   # drop spaces/newlines -> a single token


def _key_file():
    try:
        import FreeCAD as App
        base = App.getUserAppDataDir()
    except Exception:
        base = os.path.join(os.path.expanduser("~"), ".local", "share", "FreeCAD")
    return os.path.join(base, "assistant_key.json")


def _read_key_file():
    try:
        with open(_key_file(), "r", encoding="utf-8") as fh:
            return (json.load(fh) or {}).get("api_key", "")
    except Exception:
        return ""


def _write_key_file(k):
    try:
        path = _key_file()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"api_key": k}, fh)
        os.chmod(path, 0o600)
    except Exception:
        pass


def endpoint():
    return get("Endpoint", _DEFAULTS["Endpoint"])


def model():
    m = get("Model", _DEFAULTS["Model"])
    return _DEFAULTS["Model"] if m in _BAD_MODELS else m


def vision_model():
    m = get("VisionModel", _DEFAULTS["VisionModel"])
    return _DEFAULTS["VisionModel"] if m in _BAD_MODELS else m


def mode():
    return get("Mode", _DEFAULTS["Mode"])


def max_turns():
    return int(get("MaxTurns", _DEFAULTS["MaxTurns"]))


def timeout():
    return int(get("Timeout", _DEFAULTS["Timeout"]))


def temperature():
    return float(get("Temperature", _DEFAULTS["Temperature"]))


def tool_allowlist():
    try:
        return json.loads(get("ToolAllowList") or "[]")
    except Exception:
        return []


def tool_blocklist():
    try:
        return json.loads(get("ToolBlockList") or "[]")
    except Exception:
        return []


def include_context():
    return bool(get("IncludeContext", True))


def include_selection():
    return bool(get("IncludeSelection", True))


def include_logs():
    return bool(get("IncludeLogs", True))


def vision_enabled():
    return bool(get("VisionEnabled", True))


def debug():
    return bool(get("Debug", False))


def mcp_enabled():
    """Whether the MCP socket server should be running (persisted setting)."""
    return bool(get("McpEnabled", True))


def set_mcp_enabled(value):
    return set("McpEnabled", bool(value))
