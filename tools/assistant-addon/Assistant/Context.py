"""Assistant - context assembly for the agent's system prompt.

Must be called on FreeCAD's main thread (the Agent wraps it via
ToolRegistry._run_on_main).  Produces a compact textual snapshot of the live
scene: active doc, object inventory, selection, workbench, parameters and the
recent log tail.
"""

import json

MAX_OBJECTS = 250

# FreeCAD unit schemas (index -> primary length unit), mirroring the C++ schemas.
# listSchemas(): Internal, MKS, Imperial, ImperialDecimal, Centimeter,
#                ImperialBuilding, MmMin, ImperialCivil, FEM, MeterDecimal
_SCHEMA_LENGTH = {
    "Internal": "mm", "MKS": "mm", "Centimeter": "cm", "MmMin": "mm",
    "FEM": "mm", "MeterDecimal": "m",
    "Imperial": "in (imperial fraction)", "ImperialDecimal": "in (decimal)",
    "ImperialBuilding": "ft/in", "ImperialCivil": "in/ft",
}


def unit_summary():
    """Report the active unit schema (the one shown in FreeCAD's status bar)."""
    try:
        import FreeCAD as App
        u = App.Units
        idx = int(u.getSchema())
        try:
            names = list(u.listSchemas()) or []
            name = names[idx] if 0 <= idx < len(names) else str(idx)
        except Exception:
            name = str(idx)
        length = _SCHEMA_LENGTH.get(name, "mm")
        li = _SCHEMA_LENGTH.get(name, "mm")[:2].strip()
        try:
            p = App.ParamGet("User parameter:BaseApp/Units")
            decimals = int(p.GetInt("Decimals", 2)) if hasattr(p, "GetInt") else 2
        except Exception:
            decimals = 2
        angle = "deg"
        return (f"units schema='{name}' (index {idx}); length unit = {length}; "
                f"angle = {angle}; decimals = {decimals}")
    except Exception:
        return "units schema unknown (defaulting to mm/deg)"


def capture_snapshot(width=900, height=650, fmt="PNG"):
    """Capture the active 3D viewport to a base64 PNG (or None if no view).

    Returns (data_url, mime) where data_url is a ``data:image/png;base64,...``
    string ready to embed as an OpenAI ``image_url`` content part.
    """
    try:
        import base64
        import os
        import tempfile
        import FreeCADGui as Gui
        view = Gui.activeView()
        if view is None:
            return None
        fd, path = tempfile.mkstemp(suffix=".png")
        os.close(fd)
        try:
            view.saveImage(path, int(width), int(height), fmt)
            with open(path, "rb") as fh:
                data = base64.b64encode(fh.read()).decode("ascii")
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
        return (f"data:image/{fmt.lower()};base64,{data}", f"image/{fmt.lower()}")
    except Exception:
        return None


def _params_summary(doc):
    out = {}
    try:
        s = doc.getObject("Params")
        if s is not None:
            for r in range(1, 200):
                cell = "A%d" % r
                try:
                    alias = (s.getAlias(cell) or "").strip()
                except Exception:
                    alias = ""
                if alias:
                    out[alias] = s.getContents(cell)
    except Exception:
        pass
    return out


def build_context():
    import FreeCAD as App
    doc = App.ActiveDocument
    lines = []
    if doc is None:
        return "No active document."
    lines.append(f"Document: {doc.Name}  ({doc.FileName or 'unsaved'})")
    lines.append(unit_summary())

    objs = [o for o in doc.Objects]
    lines.append(f"Objects ({len(objs)}):")
    for o in objs[:MAX_OBJECTS]:
        has_shape = hasattr(o, "Shape")
        lines.append(f"  - {o.Name} <{o.TypeId}> {'[shape]' if has_shape else ''}")
    if len(objs) > MAX_OBJECTS:
        lines.append(f"  ... and {len(objs) - MAX_OBJECTS} more")

    try:
        import FreeCADGui as Gui
        sel = [o.Name for o in Gui.Selection.getSelection()]
        if sel:
            lines.append(f"Selection: {', '.join(sel)}")
        wb = Gui.activeWorkbench()
        if wb is not None:
            lines.append(f"Workbench: {getattr(wb, 'MenuText', wb)}")
    except Exception:
        pass

    params = _params_summary(doc)
    if params:
        lines.append("Parameters:")
        for k, v in params.items():
            lines.append(f"  {k} = {v}")

    try:
        log = App.Console.GetStatusMsg if hasattr(App.Console, "GetStatusMsg") else None
        if log:
            tail = App.Console.GetStatusMsg()  # may not exist; guarded
            lines.append("Log tail: " + (tail[-300:] if tail else ""))
    except Exception:
        pass

    return "\n".join(lines)
