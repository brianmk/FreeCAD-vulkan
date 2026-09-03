#!/usr/bin/env python3
"""freecad_mcp_guest - live-control agent that runs INSIDE a FreeCAD process.

This script is launched as ``FreeCAD freecad_mcp_guest.py`` (GUI) or
``FreeCADCmd freecad_mcp_guest.py`` (headless).  It opens a Unix-domain socket
and serves a small line-delimited JSON-RPC surface.  The host-side
``freecad_mcp_server.py`` (an MCP server) connects to this socket and forwards
tool calls here, so an MCP client can drive a *live* FreeCAD: build sketches of
any shape, extrude/pad, thread holes, mirror features, inspect the selection and
read/move the viewport cursor ... all while the GUI stays up and the user
watches.

Protocol (line-delimited JSON over a Unix socket; one request per connection):

    ->  {"id": 1, "method": "<tool>", "params": {..}}
    <-  {"id": 1, "result": {"ok": true,  "data": {..}}}
        {"id": 1, "result": {"ok": false, "error": "message"}}

All methods are executed on FreeCAD's main thread (via a Qt QTimer pump in the
GUI, or a blocking drain loop when headless), so scene mutations are serialized
and never race the viewport.  Communication is one-request-then-reply per
connection; the listener thread does the socket I/O and hands each job to the
main-thread executor.

Configuration (environment):
    FC_MCP_SOCKET   path for the Unix socket (default /tmp/opencode/freecad_mcp.sock)

Nothing harness/FULL-CAD specific fires except inside this process; the agent is
purely additive to a normal interactive FreeCAD session.
"""

from __future__ import annotations

import collections
import json
import os
import socket
import sys
import threading
import queue
import traceback
from typing import Any, Callable, Dict, List, Optional

# ---------------------------------------------------------------------------
# Path / environment
# ---------------------------------------------------------------------------

MCP_PARENT = os.path.dirname(os.path.abspath(__file__))
# Make the fcprobe harness importable so we can reuse its proven Session
# (viewport discovery + synthetic mouse input) for cursor/click tools.
FCPROBE_DIR = os.path.dirname(MCP_PARENT)
if FCPROBE_DIR not in sys.path:
    sys.path.insert(0, FCPROBE_DIR)

SOCKET_PATH = os.environ.get("FC_MCP_SOCKET", "/tmp/opencode/freecad_mcp.sock")

# ---------------------------------------------------------------------------
# Process output capture (the "logs")
# ---------------------------------------------------------------------------
# We capture the whole stdout/stderr stream of the FreeCAD process at the FD
# level (dup2 onto a pipe) so every C++/Qt/Python message lands in an in-memory
# ring buffer that the MCP `get_log` tool can read.  The captured bytes are also
# echoed back to the original stdout so a user's terminal still sees them.
_LOG_CAPACITY = int(os.environ.get("FC_MCP_LOG_LINES", "8000"))
_LOG = collections.deque(maxlen=_LOG_CAPACITY)   # complete lines (striped)
_log_partial = b""                                 # partial next line
_log_meta = {"captured": False, "started": 0.0, "bytes": 0}
_capture_started = threading.Event()


def _ingest_chunk(chunk: bytes) -> None:
    global _log_partial
    _log_meta["bytes"] += len(chunk)
    data = _log_partial + chunk
    parts = data.split(b"\n")
    _log_partial = parts[-1]
    for raw in parts[:-1]:
        _LOG.append(raw.decode("utf-8", "replace"))


def _capture_reader(rfd: int, echo_fd: int) -> None:
    """Drain the capture pipe into the ring buffer and echo to the original fd."""
    try:
        f = os.fdopen(os.dup(rfd), "rb", buffering=0)
    except OSError:
        return
    while True:
        try:
            chunk = f.read(4096)
        except OSError:
            break
        if not chunk:
            break
        _ingest_chunk(chunk)
        if echo_fd is not None:
            try:
                os.write(echo_fd, chunk)
            except OSError:
                pass
    try:
        f.close()
    except OSError:
        pass


def install_output_capture() -> None:
    """Redirect the process's stdout/stderr FDs onto a pipe we can read.

    Must run before any model work so later diagnostics are captured.  Safe to
    call once; the reader thread keeps echoing to the original output FD so
    nothing the user sees is lost.
    """
    if _log_meta["captured"]:
        return
    try:
        import fcntl
        orig_out = os.dup(1)
        orig_err = os.dup(2)
        r, w = os.pipe()
        os.dup2(w, 1)
        os.dup2(w, 2)
        os.close(w)
        # Echo to stdout (stderr was merged into the same pipe; the original
        # stderr fd is left untouched for the OS-level error reporting).
        threading.Thread(target=_capture_reader, args=(r, orig_out),
                         daemon=True).start()
        _log_meta.update({"captured": True, "started": 0.0})
    except Exception:
        # If FD redirection is unavailable, fall back to a Python-level tee.
        _install_python_tee()


class _Tee:
    def __init__(self, stream):
        self.stream = stream
        self.sentinel = ("",)
    def write(self, data):
        _ingest_chunk(data.encode("utf-8", "replace"))
        try:
            return self.stream.write(data)
        except Exception:
            return 0
    def flush(self):
        try:
            self.stream.flush()
        except Exception:
            pass


def _install_python_tee() -> None:
    try:
        if not isinstance(sys.stdout, _Tee):
            sys.stdout = _Tee(sys.stdout)
        if not isinstance(sys.stderr, _Tee):
            sys.stderr = _Tee(sys.stderr)
        _log_meta["captured"] = True
    except Exception:
        pass


def get_log(params: Dict[str, Any]) -> Dict[str, Any]:
    limit = max(1, min(int(params.get("limit", 200)), _LOG_CAPACITY))
    tail = bool(params.get("tail", True))
    lines = list(_LOG)
    if tail:
        lines = lines[-limit:]
    else:
        lines = lines[:limit]
    return {"lines": lines, "count": len(lines), "total": len(_LOG),
            "byte_count": _log_meta["bytes"], "captured": _log_meta["captured"],
            "capacity": _LOG_CAPACITY}


def clear_log(params: Dict[str, Any]) -> Dict[str, Any]:
    _LOG.clear()
    return {"cleared": True}


def log(params: Dict[str, Any]) -> Dict[str, Any]:
    """Write a line into the FreeCAD console (and thus the captured log)."""
    level = (params.get("level") or "message").strip().lower()
    msg = str(params.get("message", params.get("msg", "")))
    if not msg.endswith("\n"):
        msg += "\n"
    # Write straight to the captured fd1 so the line is always in the buffer,
    # then also push it through FreeCAD's own console for the ReportView.
    try:
        os.write(1, msg.encode("utf-8", "replace"))
    except OSError:
        pass
    try:
        import FreeCAD
        con = FreeCAD.Console
        fn = {"error": con.PrintError, "warning": con.PrintWarning,
              "log": con.PrintLog, "message": con.PrintMessage}.get(level, con.PrintMessage)
        fn(msg)
    except Exception:
        pass
    return {"level": level, "message": msg.rstrip("\n")}


def _app_modules():
    """Late import of the FreeCAD App-side modules (work in GUI and FreeCADCmd)."""
    import FreeCAD as App
    try:
        import Part
    except Exception:  # pragma: no cover - startup order
        import Part  # noqa: F811
    import Sketcher
    return App, Part, Sketcher


# ---------------------------------------------------------------------------
# Modeling helpers
# ---------------------------------------------------------------------------

def _doc(doc: Any, optional: bool = False) -> Any:
    App, _, _ = _app_modules()
    if doc in (None, "", "active"):
        d = App.ActiveDocument
        if d is None:
            if optional:
                return None
            raise RuntimeError("no active document (create one first)")
        return d
    if isinstance(doc, str):
        d = App.getDocument(doc)
        if d is None:
            raise RuntimeError(f"no document named {doc!r}")
        return d
    return doc


def _obj(doc: Any, name: str) -> Any:
    o = doc.getObject(name)
    if o is None:
        raise RuntimeError(f"no object named {name!r}")
    return o


def _active_body(doc: Any) -> Optional[Any]:
    bodies = [o for o in doc.Objects if o.TypeId == "PartDesign::Body"]
    if len(bodies) == 1:
        return bodies[0]
    for b in bodies:
        try:
            if b.isTip():
                return b
        except Exception:
            pass
    return bodies[0] if bodies else None


def _ensure_body(doc: Any, name: str = "Body") -> Any:
    body = _active_body(doc)
    if body is not None:
        return body
    body = doc.addObject("PartDesign::Body", name)
    doc.recompute()
    return body


def _plane_feature(body: Any, plane: str) -> Any:
    """Return the origin-plane feature of `body` matching `plane` (XY/XZ/YZ).

    OriginFeatures index order is not reliable across builds, so we match on the
    plane's Name first, then fall back to a positional guess for XY=0, XZ=2,
    YZ=1 (the common PartDesign order), and finally to the first plane that is
    not a base feature.
    """
    feats = list(body.Origin.OriginFeatures)
    want = plane.upper()
    # A valid Plane's Name looks like "XY_Plane"/"XZ_Plane"/"YZ_Plane" (the
    # datum planes) or carries the plane in its Role.
    for f in feats:
        label = f"{getattr(f, 'Name', '')} {getattr(f, 'Label', '')} {getattr(f, 'Role', '')}".upper()
        if want in label:
            return f
    guess = {"XY": 0, "XZ": 2, "YZ": 1}.get(want)
    if guess is not None and 0 <= guess < len(feats):
        return feats[guess]
    return feats[0]


def _axis_feature(body: Any, axis: str) -> Any:
    """Return the body-origin axis (a PartDesign datum line) for `axis` (X/Y/Z)."""
    feats = list(body.Origin.OriginFeatures)
    want = axis.strip().upper()
    if want not in {"X", "Y", "Z"}:
        raise RuntimeError(f"unknown origin axis {axis!r}")

    # Origin datum lines are named "X_Axis"/"Y_Axis"/"Z_Axis".  Match only the
    # requested axis so a missing datum cannot silently pattern about another
    # coordinate direction.
    expected = f"{want}_AXIS"
    for f in feats:
        label = (
            f"{getattr(f, 'Name', '')} {getattr(f, 'Label', '')} "
            f"{getattr(f, 'Role', '')}"
        ).upper().replace(" ", "_")
        is_axis = (
            getattr(f, "TypeId", "") == "PartDesign::Line"
            or "AXIS" in label
        )
        if is_axis and (expected in label or f"{want}AXIS" in label):
            return f
    raise RuntimeError(f"no {want} axis in body origin")


def _sketch_plane(body: Any, plane: str, sketch: Any) -> None:
    """Attach `sketch` to the requested origin plane of `body`."""
    import FreeCAD as App
    from FreeCAD import Placement, Rotation, Vector
    if body is None:
        # standalone sketch: place on the requested plane via its placement
        rot = {
            "XY": Rotation(Vector(0, 0, 1), 0),
            "XZ": Rotation(Vector(1, 0, 0), 90),
            "YZ": Rotation(Vector(0, 1, 0), 90),
        }.get(plane.upper(), Rotation(0, 0, 0))
        sketch.Placement = Placement(Vector(0, 0, 0), rot)
        return
    try:
        feats = list(body.Origin.OriginFeatures)
        feat = _plane_feature(body, plane)
        sketch.Support = (feat,)
        sketch.MapMode = "FlatFace"
        sketch.AddReversed = False
    except Exception:
        # the map-mode path can fail before the origin exists; offset placement
        sketch.Placement = sketch.Placement


def new_sketch(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    name = params.get("name", "Sketch")
    plane = params.get("plane", "XY").upper()
    body_name = params.get("body", "auto")
    if body_name in ("auto", None):
        # inside a PartDesign body when one exists, else create one
        body = _active_body(doc)
        if body is None and params.get("insolid", True):
            body = _ensure_body(doc)
    elif body_name == "None":
        body = None
    else:
        body = _obj(doc, body_name)
        if body.TypeId != "PartDesign::Body":
            raise RuntimeError(f"{body_name!r} is not a PartDesign::Body")
    sketch = doc.addObject("Sketcher::SketchObject", name)
    position = params.get("position") or params.get("offset")
    if position:
        try:
            sketch.Placement = App.Placement(
                App.Vector(*position), sketch.Placement.Rotation)
        except Exception:
            pass
    if body is not None:
        body.addObject(sketch)
    _sketch_plane(body, plane, sketch)
    doc.recompute()
    return {"name": sketch.Name, "body": body.Name if body else None, "plane": plane,
            "geometry_count": len(sketch.Geometry), "constraint_count": len(sketch.Constraints)}


def _get_sketch(sketch: Any, doc: Any) -> Any:
    if isinstance(sketch, str):
        return _obj(doc, sketch)
    return sketch


def add_constraint(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _get_sketch(params.get("sketch") or params.get("name"), doc)
    ctype = _canon_constraint_type(params.get("type"))
    # args come as either an explicit `args`/`params` list or the a/a_point/b/b_point
    # convenience form; any string arg is resolved as a geometry index by name.
    args = list(params.get("args") or params.get("params") or [])
    if "a" in params or "b" in params:
        args = [params.get("a"), params.get("a_point", 1),
                params.get("b"), params.get("b_point", 1)]

    def _idx(v: Any) -> Any:
        if isinstance(v, str):
            for i, g in enumerate(sketch.Geometry):
                if v in f"{g.TypeId}":
                    pass
            # `v` is expected to be a named object or an int; ints pass through
            try:
                return int(v)
            except (TypeError, ValueError):
                return v
        return v

    con = Sketcher.Constraint(ctype, *[_idx(a) for a in args])
    sketch.addConstraint(con)
    doc.recompute()
    return {"added": str(con), "constraint_count": len(sketch.Constraints)}


# Sketcher constraint names are PascalCase (e.g. DistanceY, not distancey).  Map a
# case-insensitive model value to the canonical name so "DistanceY" / "distancey"
# both work.  Unknown names pass through so the solver reports them accurately.
_CONSTRAINT_TYPES = {
    "coincident": "Coincident", "horizontal": "Horizontal", "vertical": "Vertical",
    "parallel": "Parallel", "perpendicular": "Perpendicular", "tangent": "Tangent",
    "distancex": "DistanceX", "distancey": "DistanceY", "distance": "Distance",
    "radius": "Radius", "diameter": "Diameter", "angle": "Angle", "equal": "Equal",
    "pointonobject": "PointOnObject", "pointonline": "PointOnLine",
    "pointonplane": "PointOnPlane", "symmetric": "Symmetric", "block": "Block",
    "collinear": "Collinear", "midpoint": "MidPoint", "smooth": "Smooth",
    "snellslaw": "SnellsLaw", "internallignment": "InternalAlignment",
    "equal_distance": "EqualDistance", "auxbuilt": "AuxBuilt", "spacing": "Spacing",
}


def _canon_constraint_type(t: Any) -> str:
    t = (t or "Coincident").strip()
    return _CONSTRAINT_TYPES.get(t.lower(), t)


def _add_closed_profile(sketch: Any, pts: List[List[float]], doc: Any,
                        Sketcher: Any, Part: Any, App: Any, construction: bool = False) -> None:
    """Add pts as a polygon of line segments + coincident constraints, closing
    the loop so the wire is a valid Pad/Pocket profile."""
    edges = []
    from FreeCAD import Vector
    for i in range(len(pts)):
        p0, p1 = pts[i], pts[(i + 1) % len(pts)]
        edges.append(Part.LineSegment(Vector(p0[0], p0[1], 0),
                                      Vector(p1[0], p1[1], 0)))
    for g in edges:
        sketch.addGeometry(g, construction)
    n = len(edges)
    for i in range(n):
        a = (i, 2)
        b = ((i + 1) % n, 1)
        sketch.addConstraint(Sketcher.Constraint("Coincident", a[0], a[1], b[0], b[1]))


def _add_open_polyline(sketch: Any, pts: List[List[float]], doc: Any,
                       Sketcher: Any, Part: Any, App: Any, construction: bool = False) -> None:
    from FreeCAD import Vector
    edges = [Part.LineSegment(Vector(pts[i][0], pts[i][1], 0),
                              Vector(pts[i + 1][0], pts[i + 1][1], 0))
             for i in range(len(pts) - 1)]
    for g in edges:
        sketch.addGeometry(g, construction)
    for i in range(len(edges) - 1):
        sketch.addConstraint(Sketcher.Constraint("Coincident", i, 2, i + 1, 1))


def _resolve_sketch_params(doc: Any, params: Dict[str, Any]):
    sketch = _get_sketch(params.get("sketch") or params.get("name"), doc)
    return sketch


def sketch_rectangle(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    x0, y0 = params.get("x0", 0.0), params.get("y0", 0.0)
    x1, y1 = params.get("x1", 10.0), params.get("y1", 10.0)
    pts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    _add_closed_profile(sketch, pts, doc, Sketcher, Part, App,
                        construction=bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "constraint_count": len(sketch.Constraints),
            "edges": len(pts)}


def sketch_polygon(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    cx, cy = params.get("center", [0, 0])
    r = params.get("radius", 10.0)
    sides = int(params.get("sides", 6))
    import math
    pts = [[cx + r * math.cos(2 * math.pi * i / sides),
            cy + r * math.sin(2 * math.pi * i / sides)] for i in range(sides)]
    _add_closed_profile(sketch, pts, doc, Sketcher, Part, App,
                        construction=bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "constraint_count": len(sketch.Constraints),
            "sides": sides, "radius": r}


def sketch_polyline(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    pts = params.get("points")
    if not pts or len(pts) < 2:
        raise RuntimeError("polyline needs >= 2 points")
    closed = bool(params.get("closed", False))
    if closed:
        pts = pts + [pts[0]]
    _add_open_polyline(sketch, pts, doc, Sketcher, Part, App,
                       construction=bool(params.get("construction", False)))
    if closed:
        n = len(sketch.Geometry)
        if n >= 2:
            k = len(pts) - 2  # index of last line segment
            sketch.addConstraint(Sketcher.Constraint("Coincident", k, 2, 0, 1))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "constraint_count": len(sketch.Constraints),
            "points": len(pts)}


def sketch_line(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    a = params.get("p0")
    b = params.get("p1")
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        g = Part.LineSegment(Vector(a[0], a[1], 0), Vector(b[0], b[1], 0))
    else:
        g = Part.LineSegment(Vector(params.get("x0", 0), params.get("y0", 0), 0),
                             Vector(params.get("x1", 10), params.get("y1", 10), 0))
    sketch.addGeometry(g, bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry)}


def sketch_circle(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    cx, cy = params.get("center", [0, 0])
    r = params.get("radius", 10.0)
    c = Part.Circle(Vector(cx, cy, 0), Vector(0, 0, 1), r)
    sketch.addGeometry(c, bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "center": [cx, cy], "radius": r}


def sketch_arc(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    import math
    cx, cy = params.get("center", [0, 0])
    r = params.get("radius", 10.0)
    a0 = params.get("start_angle", 0.0)
    a1 = params.get("end_angle", 90.0)
    circle = Part.Circle(Vector(cx, cy, 0), Vector(0, 0, 1), r)
    arc = Part.ArcOfCircle(circle, math.radians(a0), math.radians(a1))
    sketch.addGeometry(arc, bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "start_angle": a0, "end_angle": a1}


def sketch_spline(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    pts = params.get("points")
    if not pts or len(pts) < 2:
        raise RuntimeError("spline needs >= 2 points")
    bs = Part.BSplineCurve([Vector(p[0], p[1], 0) for p in pts])
    sketch.addGeometry(bs, bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "points": len(pts)}


def _set_feature_props(feature: Any, props: Dict[str, Any]) -> List[str]:
    """Best-effort set of model properties; skip ones the build lacks."""
    skipped = []
    for k, v in props.items():
        try:
            setattr(feature, k, v)
        except Exception:
            skipped.append(k)
    return skipped


def _last_feature(doc: Any, body: Any) -> Optional[Any]:
    """Return the last PartDesign profile feature in the body's feature chain."""
    group = list(getattr(body, "Group", []))
    # skip the sketch origin/datum plane members; keep only profile features
    for o in reversed(group):
        if o.TypeId.startswith("PartDesign::") and o.TypeId not in (
                "PartDesign::Body", "PartDesign::Plane", "PartDesign::SubShapeBinder"):
            return o
    return None


def pad(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    body = _active_body(doc)
    if body is None or sketch not in body.Group:
        raise RuntimeError(
            "pad needs a sketch that is a member of a PartDesign::Body "
            "(create the sketch with new_sketch first)")
    length = params.get("length", params.get("depth", 10.0))
    pad = doc.addObject("PartDesign::Pad", params.get("name", "Pad"))
    body.addObject(pad)
    pad.Profile = sketch
    pad.Length = length
    props = {"Type": "Dimension"}
    if params.get("symmetric", False) or params.get("midplane", False):
        props["MidPlane"] = True
    if params.get("reversed", False):
        props["Reversed"] = True
    if params.get("offset") is not None:
        props["Offset"] = params["offset"]
    _set_feature_props(pad, props)
    doc.recompute()
    return _feature_result(doc, pad)


def pocket(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    body = _active_body(doc)
    if body is None or sketch not in body.Group:
        raise RuntimeError("pocket needs a sketch in a PartDesign::Body")
    length = params.get("length", params.get("depth", 10.0))
    p = doc.addObject("PartDesign::Pocket", params.get("name", "Pocket"))
    body.addObject(p)
    p.Profile = sketch
    p.Length = length
    props = {"Type": "Dimension"}
    if params.get("symmetric", False):
        props["MidPlane"] = True
    if params.get("reversed", False):
        props["Reversed"] = True
    _set_feature_props(p, props)
    doc.recompute()
    return _feature_result(doc, p)


def _feature_result(doc: Any, feature: Any) -> Dict[str, Any]:
    bb = None
    try:
        b = feature.Shape.BoundBox
        bb = {"xmin": b.XMin, "ymin": b.YMin, "zmin": b.ZMin,
              "xmax": b.XMax, "ymax": b.YMax, "zmax": b.ZMax}
    except Exception:
        bb = None
    return {"name": feature.Name, "type_id": feature.TypeId, "label": feature.Label,
            "bounding_box": bb, "is_valid": bool(getattr(feature, "isValid", True)),
            "state": getattr(feature, "State", None)}


def extrude(params: Dict[str, Any]) -> Dict[str, Any]:
    """Part workbench extrusion of a sketch or a Part::Box-like solid shape.",
    """
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    src = params.get("source") or params.get("sketch") or params.get("name")
    obj = _obj(doc, src) if isinstance(src, str) else src
    length = params.get("length", 10.0)
    e = doc.addObject("Part::Extrusion", params.get("name", "Extrusion"))
    e.Base = obj
    e.Dir = App.Vector(*params.get("dir", [0, 0, 1]))
    e.LengthFwd = abs(length)
    e.LengthRev = 0.0
    e.Solid = bool(params.get("solid", True))
    if params.get("dir_mode") in ("Normal", "Along", "Custom"):
        e.DirMode = params["dir_mode"]
    if params.get("lengthmode", "Along").lower() in ("along", "custom"):
        pass
    taper = params.get("taper")
    if taper is not None:
        try:
            e.TaperAngle = taper
        except Exception:
            pass
    doc.recompute()
    return _feature_result(doc, e)


def add_hole(params: Dict[str, Any]) -> Dict[str, Any]:
    """Create a PartDesign::Hole with optional cosmetic/model threads.

    Profile should be a closed circle sketch belonging to the body (the hole is
    cut from the body's material).  Threads in FreeCAD are a projection of the
    drill: ``CosmeticThread`` draws the thread on the hole wall, ``ModelThread``
    produces the actual helical thread geometry.
    """
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    profile = params.get("profile") or params.get("sketch") or params.get("name")
    sketch = _obj(doc, profile) if isinstance(profile, str) else profile
    body = None
    for o in doc.Objects:
        if o.TypeId == "PartDesign::Body" and sketch in getattr(o, "Group", []):
            body = o
            break
    diameter = params.get("diameter", params.get("radius", 5.0) * 2)
    depth = params.get("depth", params.get("length", 10.0))
    threaded = bool(params.get("threaded", params.get("thread", True)))
    h = doc.addObject("PartDesign::Hole", params.get("name", "Hole"))
    if body is not None:
        body.addObject(h)
    h.Profile = sketch
    h.Diameter = diameter
    props: Dict[str, Any] = {}
    if params.get("throughall", params.get("through_all", False)):
        props["DepthType"] = "ThroughAll"
    else:
        props["DepthType"] = "Dimension"
        props["Depth"] = depth
    if threaded:
        # "set threads": cosmetic (drawn) and optionally real model threads.
        props["CosmeticThread"] = True
        props["ModelThread"] = bool(params.get("model_thread", params.get("thread_geometry", False)))
        if params.get("thread_pitch"):
            props["CustomThreadClearance"] = params["thread_pitch"]
    # drill tip style
    drill_point = (params.get("drill_point", "flat") or "flat").capitalize()
    if drill_point in ("Flat", "Angled"):
        props["DrillPoint"] = drill_point
    if params.get("drill_point_angle") is not None:
        props["DrillPointAngle"] = params["drill_point_angle"]
    # countersink / counterbore / counterdrill
    cut = params.get("hole_cut", params.get("cut", "none")).capitalize()
    if cut in ("Counterbore", "Countersink", "Counterdrill"):
        props["HoleCutType"] = cut
        if params.get("hole_cut_diameter") is not None:
            props["HoleCutDiameter"] = params["hole_cut_diameter"]
        if params.get("hole_cut_depth") is not None:
            props["HoleCutDepth"] = params["hole_cut_depth"]
    if params.get("midplane", False):
        props["Midplane"] = True
    if params.get("reversed", False):
        props["Reversed"] = True
    if params.get("refine", True):
        props["Refine"] = True
    skipped = _set_feature_props(h, {k: v for k, v in props.items() if v is not None})
    doc.recompute()
    return {**_feature_result(doc, h), "skipped_props": skipped, "threaded": threaded}


def mirror(params: Dict[str, Any]) -> Dict[str, Any]:
    """PartDesign::Mirrored of a feature about a body datum/origin plane."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    body = _active_body(doc)
    if body is None:
        raise RuntimeError("mirror needs a PartDesign::Body")
    feature = params.get("feature") or params.get("source")
    if feature is None:
        feature = (getattr(body, "Tip", None)
                   or _last_feature(doc, body))
        if feature is None:
            raise RuntimeError("no feature to mirror (build a pad/pocket first)")
    if isinstance(feature, str):
        feature = _obj(doc, feature)
    plane = params.get("plane", "XY").upper()
    m = _obj(doc, params.get("mirror", "MirrorPlane")) if params.get("mirror") else None
    if m is None:
        m = _plane_feature(body, plane)
    mirror = doc.addObject("PartDesign::Mirrored", params.get("name", "Mirror"))
    body.addObject(mirror)
    mirror.Originals = [feature]
    mirror.MirrorPlane = m
    doc.recompute()
    return _feature_result(doc, mirror)


def mirror_object(params: Dict[str, Any]) -> Dict[str, Any]:
    """Mirror a whole Part object with the Part workbench mirroring.",
    """
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("source") or params.get("name"))
    base = params.get("base", [0, 0, 0])
    normal = params.get("normal", [0, 0, 1])
    mm = doc.addObject("Part::Mirroring", params.get("name", "Mirroring"))
    mm.Source = obj
    mm.Base = App.Vector(*base)
    mm.Normal = App.Vector(*normal)
    mm.Label = params.get("label", "Mirroring")
    doc.recompute()
    return _feature_result(doc, mm)


def pattern(params: Dict[str, Any]) -> Dict[str, Any]:
    """PartDesign pattern of an existing feature: polar (about an axis) or
    rectangular/linear (along an axis)."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    body = _active_body(doc)
    if body is None:
        raise RuntimeError("pattern needs a PartDesign::Body")
    feature = params.get("feature") or params.get("source") or params.get("name")
    if feature is None:
        feature = getattr(body, "Tip", None) or _last_feature(doc, body)
        if feature is None:
            raise RuntimeError("no feature to pattern (build a pad/pocket first)")
    if isinstance(feature, str):
        feature = _obj(doc, feature)
    style = (params.get("style") or params.get("type") or "polar").lower()
    occurrences = int(params.get("occurrences", 3))
    if occurrences < 1:
        raise RuntimeError("occurrences must be >= 1")
    if style == "polar":
        obj = doc.addObject("PartDesign::PolarPattern", params.get("name", "PolarPattern"))
        body.addObject(obj)
        obj.Originals = [feature]
        obj.Axis = (_axis_feature(body, params.get("axis", "Z")), [""])
        mode = (params.get("mode") or "whole").lower()
        obj.Mode = "Spacing" if mode in ("half", "single", "spacing") else "Extent"
        obj.Angle = float(params.get("angle", 360.0))
        obj.Occurrences = occurrences
        if params.get("reversed"):
            obj.Reversed = True
    elif style in ("rectangular", "linear"):
        obj = doc.addObject("PartDesign::LinearPattern", params.get("name", "LinearPattern"))
        body.addObject(obj)
        obj.Originals = [feature]
        obj.Direction = (_axis_feature(body, params.get("axis", "X")), [""])
        obj.Length = float(params.get("length", 10.0))
        obj.Occurrences = occurrences
        if params.get("reversed"):
            obj.Reversed = True
    else:
        raise RuntimeError(f"unknown pattern style {style!r}")
    doc.recompute()
    return _feature_result(doc, obj)


# ---------------------------------------------------------------------------
# Document / object / selection helpers
# ---------------------------------------------------------------------------

def list_objects(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"), optional=True)
    if doc is None:
        return {"doc": None, "objects": [], "count": 0}
    out = []
    for o in doc.Objects:
        out.append({
            "name": o.Name, "label": o.Label, "type_id": o.TypeId,
            "visible": bool(getattr(o, "Visibility", True)),
            "internal": bool(getattr(o, "isValid", True)),
            "state": getattr(o, "State", None),
        })
    return {"doc": doc.Name, "objects": out, "count": len(out)}


def new_document(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    name = params.get("name", "Unnamed")
    d = App.newDocument(name)
    return {"name": d.Name}


def open_document(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    path = params.get("path")
    d = App.openDocument(path)
    return {"name": d.Name, "path": path}


def active_document(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    d = App.ActiveDocument
    if d is None:
        return {"name": None}
    return {"name": d.Name}


def set_active_document(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    name = params.get("name")
    d = App.getDocument(name)
    if d is None:
        raise RuntimeError(f"no document named {name!r}")
    App.setActiveDocument(d.Name)
    return {"name": d.Name}


def recompute(params: Dict[str, Any]) -> Dict[str, Any]:
    doc = _doc(params.get("doc", "active"))
    doc.recompute()
    return {"recomputed": doc.Name}


def delete_object(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    name = params.get("name")
    if isinstance(name, str):
        names = [name]
    else:
        names = list(name or [])
    removed = []
    for n in names:
        o = _obj(doc, n)
        doc.removeObject(n)
        removed.append(n)
    return {"removed": removed}


def get_placement(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    o = _obj(doc, params.get("name"))
    pl = o.Placement
    return {"name": o.Name, "base": list(pl.Base), "rotation": [*pl.Rotation.Q],
            "label": o.Label}


def set_placement(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    o = _obj(doc, params.get("name"))
    base = params.get("base")
    if base is not None:
        pos = App.Placement(App.Vector(*base), o.Placement.Rotation)
        o.Placement = pos
    doc.recompute()
    pl = o.Placement
    return {"name": o.Name, "base": list(pl.Base)}


def get_selection(params: Dict[str, Any]) -> Dict[str, Any]:
    """Return the current FreeCADGui selection (empty when headless)."""
    try:
        import FreeCADGui
        sel = FreeCADGui.Selection.getSelectionEx()
        out = []
        for s in sel:
            out.append({"name": s.ObjectName, "label": s.Object.Label,
                        "subs": list(s.SubElementNames)})
        return {"selection": out, "count": len(out)}
    except Exception:
        App, _, _ = _app_modules()
        doc = _doc(params.get("doc", "active"))
        try:
            sel = (getattr(doc, "Selection", []) or [])
        except Exception:
            sel = []
        return {"selection": [{"name": o, "subs": []} for o in sel], "count": len(sel),
                "gui_available": False}


def select_objects(params: Dict[str, Any]) -> Dict[str, Any]:
    try:
        import FreeCADGui
    except Exception:
        raise RuntimeError("selection requires the FreeCAD GUI")
    doc = _doc(params.get("doc", "active"))
    names = params.get("names") or params.get("name")
    if isinstance(names, str):
        names = [names]
    FreeCADGui.Selection.clearSelection()
    sel = []
    for n in names:
        o = doc.getObject(n)
        if o is None:
            continue
        FreeCADGui.Selection.addSelection(doc.Name, o.Name)
        sel.append(n)
    return {"selected": sel, "count": len(sel)}


def clear_selection(params: Dict[str, Any]) -> Dict[str, Any]:
    try:
        import FreeCADGui
        FreeCADGui.Selection.clearSelection()
        return {"cleared": True}
    except Exception:
        return {"cleared": True, "gui_available": False}


_USER_INPUT_COMMANDS = frozenset({
    "Std_Open", "Std_Save", "Std_SaveAs", "Std_Import", "Std_Export",
    "Std_DlgPreferences", "Std_DlgMacroExecute", "Std_Print", "Std_OpenRecent",
})


def _cmd_dialog_timeout_ms() -> int:
    try:
        return int(os.environ.get("FC_CMD_DIALOG_TIMEOUT_MS", "1500"))
    except (TypeError, ValueError):
        return 1500


def _dialog_watchdog():
    """Arm a QTimer to auto-dismiss any modal dialog a command opens, so a
    user-input command (e.g. Std_Open) cannot block the agent.  Returns the
    QTimer so the caller can stop() it if no dialog appeared."""
    import PySide.QtCore as QtCore
    import PySide.QtWidgets as QtWidgets

    def cancel():
        w = QtWidgets.QApplication.activeModalWidget()
        if w is not None:
            try:
                w.reject()
            except Exception:
                try:
                    w.close()
                except Exception:
                    pass

    timer = QtCore.QTimer()
    timer.setSingleShot(True)
    timer.setInterval(_cmd_dialog_timeout_ms())
    timer.timeout.connect(cancel)
    timer.start()
    return timer


def run_command(params: Dict[str, Any]) -> Dict[str, Any]:
    try:
        import FreeCADGui
    except Exception:
        raise RuntimeError("run_command requires the FreeCAD GUI")
    cmd = params.get("command") or params.get("name")
    watch = _dialog_watchdog() if cmd in _USER_INPUT_COMMANDS else None
    try:
        FreeCADGui.runCommand(cmd)
    finally:
        if watch is not None:
            watch.stop()
    return {"command": cmd}


def set_workbench(params: Dict[str, Any]) -> Dict[str, Any]:
    try:
        import FreeCADGui
    except Exception:
        raise RuntimeError("set_workbench requires the FreeCAD GUI")
    FreeCADGui.activateWorkbench(params.get("name", "PartDesignWorkbench"))
    return {"workbench": params.get("name")}


def run_python(params: Dict[str, Any]) -> Dict[str, Any]:
    """Eval/exec a FreeCAD Python snippet (power tool, use with care)."""
    import io
    from contextlib import redirect_stdout, redirect_stderr
    App, _, _ = _app_modules()
    g = {"__name__": "__mcp_guest__", "FreeCAD": App}
    buf = io.StringIO()
    result = None
    try:
        with redirect_stdout(buf), redirect_stderr(buf):
            if params.get("mode", "exec") == "eval":
                result = eval(params.get("code", ""), g)
            else:
                exec(params.get("code", ""), g)
                d = App.ActiveDocument
                if d is not None:
                    d.recompute()
    except Exception as exc:
        return {"result": None, "stdout": buf.getvalue(), "error": f"{type(exc).__name__}: {exc}"}
    return {"result": str(result) if result is not None else None,
            "stdout": buf.getvalue()}


# ---------------------------------------------------------------------------
# GUI / viewport / cursor (requires the live GUI; reuses harness Session)
# ---------------------------------------------------------------------------

_session = None
_session_lock = threading.Lock()


def _ensure_session():
    """Build the harness Session once for viewport discovery + synthetic input."""
    global _session
    with _session_lock:
        if _session is None:
            from freecad_probe import Session
            _session = Session(name="mcp")
    return _session


def _gui_ctx():
    import FreeCADGui
    import FreeCAD
    return FreeCAD, FreeCADGui


def snapshot(params: Dict[str, Any]) -> Dict[str, Any]:
    """Viewport + document + selection + camera state (mirrors probe snapshot)."""
    App, _, _ = _app_modules()
    state: Dict[str, Any] = {"doc": (App.ActiveDocument.Name
                                     if App.ActiveDocument else None)}
    try:
        import FreeCADGui
        view = FreeCADGui.activeView()
        state["camera_type"] = view.getCameraType() if view else None
        sel = FreeCADGui.Selection.getSelectionEx()
        state["selection"] = [{"name": s.ObjectName, "subs": list(s.SubElementNames)}
                              for s in sel]
    except Exception:
        state["selection"] = []
        state["gui_available"] = False
    try:
        s = _ensure_session()
        s._relocate_viewport()
        state["viewport"] = {"w": s.width, "h": s.height, "dpr": s.dpr}
        state["gui_available"] = True
    except Exception as exc:
        state["gui"] = str(exc)
    return state


def _viewport():
    """Return the harness Session container if it can see one."""
    return _ensure_session()


def _leave_cursor_sampler():
    """Sample the current cursor position/hover through the live viewport."""
    import FreeCADGui
    from PySide import QtWidgets, QtGui as QG
    s = _ensure_session()
    s._relocate_viewport()
    container = s.container
    if container is None:
        raise RuntimeError(
            "no 3D viewport container (cursor tools need the FreeCAD GUI with "
            "an open view; run the guest as a GUI FreeCAD, not headless)")
    pos = QG.QCursor.pos()
    local = container.mapFromGlobal(pos)
    hover = None
    try:
        pre = FreeCADGui.Selection.getPreselection()
        if pre and getattr(pre, "ObjectName", None):
            hover = {"obj": pre.ObjectName, "subs": list(getattr(pre, "SubElementNames", []) or [])}
    except Exception:
        hover = None
    return {
        "global": {"x": pos.x(), "y": pos.y()},
        "viewport": {"x": local.x(), "y": local.y()},
        "device": {"x": int(round(local.x() * s.dpr)), "y": int(round(local.y() * s.dpr))},
        "viewport_size": {"w": s.width, "h": s.height},
        "dpr": s.dpr,
        "hover": hover,
    }


def get_cursor(params: Dict[str, Any]) -> Dict[str, Any]:
    return _leave_cursor_sampler()


def move_cursor(params: Dict[str, Any]) -> Dict[str, Any]:
    """Move the cursor to a logical viewport position; returns sampled state."""
    import FreeCADGui
    from PySide import QtCore, QtGui as QG
    x = params.get("x")
    y = params.get("y")
    if x is None or y is None:
        raise RuntimeError("move_cursor needs x,y (logical viewport px)")
    s = _ensure_session()
    s.move(float(x), float(y))
    # Optionally warp the real OS cursor to the viewport point so `get_cursor`
    # reflects the move (not just the synthetic in-viewport event).
    if params.get("warp", True) and s.container is not None:
        try:
            pos = QtCore.QPoint(int(x), int(y))
            qpos = s.container.mapToGlobal(pos)
            QG.QCursor.setPos(qpos)
            # deliver a real move to the container so hover updates
            s.move(float(x), float(y))
        except Exception:
            pass
    return _leave_cursor_sampler()


def click(params: Dict[str, Any]) -> Dict[str, Any]:
    """Synthetic click at a logical viewport position (press+release)."""
    import FreeCADGui
    x = params.get("x")
    y = params.get("y")
    if x is None or y is None:
        raise RuntimeError("click needs x,y (logical viewport px)")
    s = _ensure_session()
    hit = s.click(float(x), float(y))
    sel = FreeCADGui.Selection.getSelectionEx()
    return {"hit": bool(hit),
            "selection": [{"name": s.ObjectName, "subs": list(s.SubElementNames)}
                          for s in sel]}


def set_view(params: Dict[str, Any]) -> Dict[str, Any]:
    import FreeCADGui
    view = FreeCADGui.activeView()
    if view is None:
        raise RuntimeError("no active view")
    name = params.get("name", "top").lower()
    method = {"top": "viewTop", "front": "viewFront", "right": "viewRight",
              "isometric": "viewIsometric", "home": "viewHome"}.get(name)
    if not method:
        raise RuntimeError(f"unknown view {name!r}")
    try:
        getattr(view, method)()
    except Exception:
        pass
    return {"view": name}


def fit_view(params: Dict[str, Any]) -> Dict[str, Any]:
    import FreeCADGui
    view = FreeCADGui.activeView()
    if view is None:
        raise RuntimeError("no active view")
    view.fitAll()
    return {"fit": True}


def control_camera(params: Dict[str, Any]) -> Dict[str, Any]:
    """Drive the 3D viewport camera. `action` is one of:
       fit | isometric | top | front | right | rear | bottom | left | dimetric |
       trimetric | axometric | zoom_in | zoom_out | rotate_left | rotate_right |
       set_direction (needs `direction=[x,y,z]`)."""
    import FreeCAD
    import FreeCADGui
    view = FreeCADGui.activeView()
    if view is None:
        raise RuntimeError("no active view")
    action = (params.get("action") or "fit").lower()
    standard = {"isometric": "viewIsometric", "top": "viewTop", "front": "viewFront",
                "right": "viewRight", "rear": "viewRear", "bottom": "viewBottom",
                "left": "viewLeft", "dimetric": "viewDimetric",
                "trimetric": "viewTrimetric", "axometric": "viewAxometric"}
    if action == "fit":
        view.fitAll()
    elif action in standard:
        getattr(view, standard[action])()
    elif action == "zoom_in":
        view.zoomIn()
    elif action == "zoom_out":
        view.zoomOut()
    elif action == "rotate_left":
        view.viewRotateLeft()
    elif action == "rotate_right":
        view.viewRotateRight()
    elif action == "set_direction":
        d = params.get("direction", [0, 0, 1])
        view.setViewDirection(FreeCAD.Vector(float(d[0]), float(d[1]), float(d[2])))
    else:
        raise RuntimeError(f"unknown camera action {action!r}")
    return {"action": action, "view_direction": str(view.getViewDirection())}


def screenshot(params: Dict[str, Any]) -> Dict[str, Any]:
    import FreeCADGui
    view = FreeCADGui.activeView()
    if view is None:
        raise RuntimeError("no active view")
    path = params.get("path", "/tmp/opencode/freecad_mcp_shot.png")
    fmt = params.get("format", path.rsplit(".", 1)[-1] if "." in path else "PNG").upper()
    w = int(params.get("width", 0) or 0)
    h = int(params.get("height", 0) or 0)
    try:
        view.saveImage(path, w, h, fmt)
        return {"path": path, "format": fmt, "ok": True}
    except Exception as exc:
        return {"path": path, "ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Extended modeling tools (full sketch / constraint / part coverage)
# ---------------------------------------------------------------------------

def sketch_ellipse(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    cx, cy = params.get("center", [0, 0])
    rx = params.get("major_radius", params.get("radius", 20.0))
    ry = params.get("minor_radius", rx / 2.0)
    geom = Part.Ellipse(Vector(cx, cy, 0), Vector(cx + rx, cy, 0), Vector(cx, cy + ry, 0))
    sketch.addGeometry(geom, bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "center": [cx, cy],
            "major_radius": rx, "minor_radius": ry}


def sketch_point(params: Dict[str, Any]) -> Dict[str, Any]:
    App, Part, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    x, y = params.get("position", [0, 0])
    sketch.addGeometry(Part.Point(Vector(x, y, 0)), bool(params.get("construction", False)))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "position": [x, y]}


def sketch_slot(params: Dict[str, Any]) -> Dict[str, Any]:
    """Rounded slot: two arcs + two lines, each corner coincident so the wire
    is a valid Pad/Pocket profile.  center = slot midpoint, length = centre
    distance, width = slot width."""
    App, Part, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    from FreeCAD import Vector
    import math
    cx, cy = params.get("center", [0, 0])
    half = params.get("length", 20.0) / 2.0
    width = params.get("width", 10.0)
    r = width / 2.0
    z = Vector(0, 0, 1)
    pA = Vector(cx - half, cy, 0)
    pB = Vector(cx + half, cy, 0)
    cA = Part.Circle(pA, z, r)
    cB = Part.Circle(pB, z, r)
    arcA = Part.ArcOfCircle(cA, math.radians(90), math.radians(270))
    arcB = Part.ArcOfCircle(cB, math.radians(270), math.radians(90))
    for g in (arcA, Part.LineSegment(pA - Vector(0, r, 0), pB - Vector(0, r, 0)),
              arcB, Part.LineSegment(pB + Vector(0, r, 0), pA + Vector(0, r, 0))):
        sketch.addGeometry(g, bool(params.get("construction", False)))
    n = len(sketch.Geometry) - len(sketch.Geometry)  # noop
    start = len(sketch.Geometry) - 4
    for k in range(4):
        a = (start + k, 2)
        b = (start + (k + 1) % 4, 1)
        sketch.addConstraint(Sketcher.Constraint("Coincident", a[0], a[1], b[0], b[1]))
    doc.recompute()
    return {"geometry_count": len(sketch.Geometry), "center": [cx, cy],
            "length": 2 * half, "width": width}


def fillet(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    feature = _obj(doc, params.get("feature") or params.get("object") or params.get("source"))
    body = _active_body(doc)
    if body is None:
        raise RuntimeError("fillet needs a PartDesign::Body")
    edges = params.get("edges") or params.get("edge") or []
    if isinstance(edges, str):
        edges = [edges]
    if not edges:
        raise RuntimeError("fillet needs an 'edges' list (e.g. ['Edge1','Edge2'])")
    f = doc.addObject("PartDesign::Fillet", params.get("name", "Fillet"))
    body.addObject(f)
    f.Base = (feature, list(edges))
    f.Radius = params.get("radius", 1.0)
    doc.recompute()
    return _feature_result(doc, f)


def chamfer(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    feature = _obj(doc, params.get("feature") or params.get("object") or params.get("source"))
    body = _active_body(doc)
    if body is None:
        raise RuntimeError("chamfer needs a PartDesign::Body")
    edges = params.get("edges") or params.get("edge") or []
    if isinstance(edges, str):
        edges = [edges]
    if not edges:
        raise RuntimeError("chamfer needs an 'edges' list (e.g. ['Edge1'])")
    f = doc.addObject("PartDesign::Chamfer", params.get("name", "Chamfer"))
    body.addObject(f)
    f.Base = (feature, list(edges))
    f.Size = params.get("size", params.get("radius", 1.0))
    doc.recompute()
    return _feature_result(doc, f)


def revolve(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sketch = _resolve_sketch_params(doc, params)
    body = _active_body(doc)
    if body is None or sketch not in body.Group:
        raise RuntimeError("revolve needs a sketch in a PartDesign::Body")
    angle = params.get("angle", 360.0)
    axis_obj = _obj(doc, params.get("axis_object") or body.Name)
    axis_elem = params.get("axis_element", "V_Axis")
    r = doc.addObject("PartDesign::Revolution", params.get("name", "Revolution"))
    body.addObject(r)
    r.Profile = sketch
    r.Angle = angle
    try:
        r.Axis = (axis_obj, [axis_elem])
    except Exception:
        r.AxisReference = (params.get("axis_ref") or [None, True])
    doc.recompute()
    return _feature_result(doc, r)


def loft(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    body = _active_body(doc)
    if body is None:
        raise RuntimeError("loft needs a PartDesign::Body")
    sections = [_resolve_sketch_params(doc, {"sketch": s})
                for s in (params.get("sections") or [])]
    if len(sections) < 2:
        raise RuntimeError("loft needs >= 2 'sections' (sketches)")
    l = doc.addObject("PartDesign::AdditiveLoft", params.get("name", "Loft"))
    body.addObject(l)
    l.Sections = sections
    doc.recompute()
    return _feature_result(doc, l)


def boolean_op(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    a = _obj(doc, params.get("shape") or params.get("a") or params.get("base"))
    b = _obj(doc, params.get("tool") or params.get("b"))
    mode = (params.get("mode") or "fuse").lower()
    out = doc.addObject("Part::Feature", params.get("name") or mode.capitalize())
    if mode in ("fuse", "union", "add"):
        out.Shape = a.Shape.fuse(b.Shape)
    elif mode in ("cut", "subtract", "sub"):
        out.Shape = a.Shape.cut(b.Shape)
    elif mode in ("common", "intersect", "inter"):
        out.Shape = a.Shape.common(b.Shape)
    else:
        raise RuntimeError(f"unknown boolean mode {mode!r}")
    doc.recompute()
    return _feature_result(doc, out)


def _place(obj: Any, params: Dict[str, Any], App: Any) -> None:
    pos = params.get("position") or params.get("base") or params.get("center")
    if pos:
        obj.Placement = App.Placement(App.Vector(*pos), obj.Placement.Rotation)


def make_box(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    b = doc.addObject("Part::Box", params.get("name", "Box"))
    b.Length = params.get("length", 10.0)
    b.Width = params.get("width", 10.0)
    b.Height = params.get("height", 10.0)
    _place(b, params, App)
    doc.recompute()
    return _feature_result(doc, b)


def make_cylinder(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    c = doc.addObject("Part::Cylinder", params.get("name", "Cylinder"))
    c.Radius = params.get("radius", 5.0)
    c.Height = params.get("height", 10.0)
    _place(c, params, App)
    doc.recompute()
    return _feature_result(doc, c)


def make_sphere(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    s = doc.addObject("Part::Sphere", params.get("name", "Sphere"))
    s.Radius = params.get("radius", 5.0)
    _place(s, params, App)
    doc.recompute()
    return _feature_result(doc, s)


def make_cone(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    c = doc.addObject("Part::Cone", params.get("name", "Cone"))
    c.Radius1 = params.get("radius1", 5.0)
    c.Radius2 = params.get("radius2", 0.0)
    c.Height = params.get("height", 10.0)
    _place(c, params, App)
    doc.recompute()
    return _feature_result(doc, c)


def make_torus(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    t = doc.addObject("Part::Torus", params.get("name", "Torus"))
    t.Radius1 = params.get("radius1", 10.0)
    t.Radius2 = params.get("radius2", 2.0)
    _place(t, params, App)
    doc.recompute()
    return _feature_result(doc, t)


def add_fastener(params: Dict[str, Any]) -> Dict[str, Any]:
    """Add screws/bolts to circular hole faces of `object` using the Fasteners
    workbench.  `screw_type` e.g. 'iso4014'.  Restrict to `centers=[[x,y],..]`
    (on the top face) and/or `diameter`.  Size/length derive from the hole."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("object") or params.get("object_name") or "Pocket")
    stype = (params.get("screw_type") or params.get("type") or "ISO4014").upper()
    centers = params.get("centers") or []
    diameter = params.get("diameter")
    shape = obj.Shape
    ztop = shape.BoundBox.ZMax
    import FreeCADGui as Gui
    top_circ = []
    for i, e in enumerate(shape.Edges):
        c = e.Curve
        if c.TypeId == "Part::GeomCircle":
            ctr = c.Center
            if abs(ctr[2] - ztop) < 0.05:
                if diameter is not None and abs(c.Radius - diameter / 2.0) > 0.5:
                    continue
                if centers:
                    hit = any(abs(ctr[0] - sx) < 0.5 and abs(ctr[1] - sy) < 0.5
                              for sx, sy in centers)
                    if not hit:
                        continue
                top_circ.append(("Edge%d" % (i + 1), ctr))
    if not top_circ:
        raise RuntimeError("no matching circular hole edges found on the top face")
    Gui.Selection.clearSelection()
    for ename, _ in top_circ:
        Gui.Selection.addSelection(doc.Name, obj.Name, ename)
    import FastenersCmd
    import FastenerBase
    sels = FastenerBase.FSGetAttachableSelections()
    made = []
    for selObj in sels:
        a = doc.addObject("Part::FeaturePython", "Screw")
        FastenersCmd.FSScrewObject(a, stype, selObj)
        a.Label = a.Proxy.familyType if getattr(a.Proxy, "familyType", None) else a.Label
        FastenersCmd.FSViewProviderTree(a.ViewObject)
        flip = params.get("flip")
        if flip is not None and hasattr(a, "Invert"):
            a.Invert = bool(flip)
        made.append(a.Name)
    doc.recompute()
    return {"screws": made, "edges": [e for e, _ in top_circ], "count": len(made)}


def export_objects(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    path = params.get("path")
    if not path:
        raise RuntimeError("export needs a 'path' (extension selects the format)")
    names = params.get("objects") or params.get("names")
    sel = [doc.getObject(n) for n in names] if names else list(doc.Objects)
    sel = [o for o in sel if o is not None and hasattr(o, "Shape")]
    if not sel:
        raise RuntimeError("export found no shapable objects to write")
    import Import
    Import.export(sel, path)
    return {"path": path, "objects": [o.Name for o in sel], "count": len(sel)}


def measure_clearance(params: Dict[str, Any]) -> Dict[str, Any]:
    """Validate clearance between objects.  ``min_distance_mm`` is the OCCT
    minimum gap between the two shapes (0 when they touch/overlap);
    ``overlaps`` is True when their intersection has positive volume (= clipping)."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))

    def _shape(name: Any) -> Any:
        obj = name if not isinstance(name, str) else _obj(doc, name)
        if not hasattr(obj, "Shape"):
            raise RuntimeError(f"{getattr(obj, 'Name', name)!r} has no Shape")
        return obj, obj.Shape

    def _pair(n1: Any, n2: Any) -> Dict[str, Any]:
        _, s1 = _shape(n1)
        _, s2 = _shape(n2)
        try:
            dist = s1.distToShape(s2)[0]
        except Exception:
            dist = None
        try:
            common = s1.common(s2).Volume
        except Exception:
            common = 0.0
        name1 = n1 if isinstance(n1, str) else n1.Name
        name2 = n2 if isinstance(n2, str) else n2.Name
        return {"a": name1, "b": name2, "min_distance_mm": dist,
                "overlaps": bool(common and common > 1e-6),
                "common_volume_mm3": round(common, 3)}

    res: List[Dict[str, Any]] = []
    a = params.get("a")
    b = params.get("b")
    names = params.get("objects")
    if a and b:
        pairs = [(a, b)]
    elif names:
        pairs = [(names[i], names[j]) for i in range(len(names))
                 for j in range(i + 1, len(names))]
    else:
        raise RuntimeError("measure_clearance needs 'a'+'b' or an 'objects' list")
    res = [_pair(n1, n2) for n1, n2 in pairs]
    return {"pairs": res, "overlaps": [r for r in res if r["overlaps"]]}


PARAM_SHEET_NAME = "Params"


def _param_sheet(doc: Any) -> Any:
    s = doc.getObject(PARAM_SHEET_NAME)
    if s is None:
        s = doc.addObject("Spreadsheet::Sheet", PARAM_SHEET_NAME)
        doc.recompute()
    return s


def _param_cell(sheet: Any, name: str) -> Optional[str]:
    for r in range(1, 1000):
        cell = "A%d" % r
        try:
            if (sheet.getAlias(cell) or "") == name:
                return cell
        except Exception:
            continue
    return None


def _freecell(sheet: Any) -> str:
    r = 1
    while True:
        cell = "A%d" % r
        if not sheet.getContents(cell):
            return cell
        r += 1
        if r > 10000:
            raise RuntimeError("spreadsheet full")


def _value_str(value: Any, unit: Optional[str]) -> str:
    if isinstance(value, str):
        return value
    if unit:
        return "%s %s" % (value, unit)
    return str(value)


def add_parameter(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    name = params.get("name")
    value = params.get("value")
    if not name:
        raise RuntimeError("add_parameter needs a 'name'")
    if value is None:
        raise RuntimeError("add_parameter needs a 'value'")
    unit = params.get("unit")
    sheet = _param_sheet(doc)
    cell = _param_cell(sheet, name) or _freecell(sheet)
    text = _value_str(value, unit)
    sheet.set(cell, text)
    sheet.setAlias(cell, name)
    doc.recompute()
    return {"parameter": name, "cell": cell, "value": text}


def set_parameter(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    name = params.get("name")
    value = params.get("value")
    if not name or value is None:
        raise RuntimeError("set_parameter needs a 'name' and a 'value'")
    unit = params.get("unit")
    return add_parameter({"name": name, "value": value, "unit": unit, "doc": doc.Name})


def get_parameter(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    name = params.get("name")
    if not name:
        raise RuntimeError("get_parameter needs a 'name'")
    sheet = doc.getObject(PARAM_SHEET_NAME)
    if sheet is None:
        return {"parameter": name, "found": False}
    cell = _param_cell(sheet, name)
    if cell is None:
        return {"parameter": name, "found": False}
    return {"parameter": name, "cell": cell, "value": sheet.getContents(cell), "found": True}


def list_parameters(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sheet = doc.getObject(PARAM_SHEET_NAME)
    if sheet is None:
        return {"parameters": {}}
    out: Dict[str, str] = {}
    for r in range(1, 1000):
        cell = "A%d" % r
        try:
            alias = sheet.getAlias(cell) or ""
        except Exception:
            alias = ""
        if alias:
            out[alias] = sheet.getContents(cell)
    return {"parameters": out}


def get_property(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("object"))
    prop = params.get("property")
    if not prop:
        raise RuntimeError("get_property needs 'object' and 'property'")
    return {"object": obj.Name, "property": prop, "value": str(getattr(obj, prop))}


def set_property(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("object"))
    prop = params.get("property")
    value = params.get("value")
    if not prop or value is None:
        raise RuntimeError("set_property needs 'object', 'property', 'value'")
    setattr(obj, prop, value)
    doc.recompute()
    return {"object": obj.Name, "property": prop, "value": str(getattr(obj, prop))}


def link_property(params: Dict[str, Any]) -> Dict[str, Any]:
    """Bind ``object.property`` to a parameter via an OCCT expression
    (``Params.<parameter>``) so the dimension is driven by the spreadsheet cell."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("object"))
    prop = params.get("property")
    name = params.get("parameter")
    if not prop or not name:
        raise RuntimeError("link_property needs 'object', 'property', 'parameter'")
    if doc.getObject(PARAM_SHEET_NAME) is None:
        raise RuntimeError("no parameters yet (call add_parameter first)")
    expr = params.get("expression") or "%s.%s" % (PARAM_SHEET_NAME, name)
    obj.setExpression(prop, expr)
    doc.recompute()
    return {"object": obj.Name, "property": prop, "expression": expr,
            "value_after": str(getattr(obj, prop, None))}


def capture_parameter(params: Dict[str, Any]) -> Dict[str, Any]:
    """Turn an existing measurement into a named parameter: read the current
    ``object.property`` value, store it as the parameter, then bind the property
    to it so the dimension is now driven by (and editable via) the parameter."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    obj = _obj(doc, params.get("object"))
    prop = params.get("property")
    name = params.get("parameter")
    if not prop or not name:
        raise RuntimeError("capture_parameter needs 'object', 'property', 'parameter'")
    val = getattr(obj, prop)
    num = val.Value if hasattr(val, "Value") else float(val)
    unit = params.get("unit")
    created = add_parameter({"name": name, "value": num, "unit": unit, "doc": doc.Name})
    linked = link_property({"object": obj.Name, "property": prop, "parameter": name, "doc": doc.Name})
    return {"parameter": name, "object": obj.Name, "property": prop,
            "value": num, "param_cell": created["cell"],
            "expression": linked["expression"], "value_after": linked["value_after"]}


def get_sketch_constraints(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sk = _obj(doc, params.get("sketch"))
    out = []
    for i, c in enumerate(sk.Constraints):
        out.append({"index": i, "type": c.Type, "value": c.Value})
    return {"sketch": sk.Name, "n_constraints": len(sk.Constraints),
            "constraints": out}


def set_sketch_constraint_value(params: Dict[str, Any]) -> Dict[str, Any]:
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sk = _obj(doc, params.get("sketch"))
    idx = int(params.get("index"))
    value = params.get("value")
    if value is None:
        raise RuntimeError("set_sketch_constraint_value needs 'sketch', 'index', 'value'")
    sk.setDatum(idx, App.Units.Quantity(str(value)))
    doc.recompute()
    return {"sketch": sk.Name, "index": idx, "value": str(value)}


def _sketch_geom_outline(sk: Any) -> List[Dict[str, Any]]:
    """Compact geometry outline (with coordinates) so the agent can reason about
    which constraints a sketch still needs."""
    out = []
    for i, g in enumerate(sk.Geometry):
        t = g.TypeId
        item: Dict[str, Any] = {"index": i, "type": t.split("::")[-1]}
        try:
            if hasattr(g, "StartPoint") and hasattr(g, "EndPoint"):
                item["start"] = [round(g.StartPoint.x, 3), round(g.StartPoint.y, 3)]
                item["end"] = [round(g.EndPoint.x, 3), round(g.EndPoint.y, 3)]
            elif hasattr(g, "Center"):
                item["center"] = [round(g.Center.x, 3), round(g.Center.y, 3)]
                if hasattr(g, "Radius"):
                    item["radius"] = round(g.Radius, 3)
            elif t == "Part::Point":
                item["position"] = [round(g.X, 3), round(g.Y, 3)] if hasattr(g, "X") else None
        except Exception:
            pass
        out.append(item)
    return out


def _sketch_constraint_detail(sk: Any) -> List[Dict[str, Any]]:
    out = []
    for i, c in enumerate(sk.Constraints):
        out.append({
            "index": i, "type": c.Type, "value": getattr(c, "Value", None),
            "first": getattr(c, "First", None), "first_pos": getattr(c, "FirstPos", None),
            "second": getattr(c, "Second", None), "second_pos": getattr(c, "SecondPos", None),
            "third": getattr(c, "Third", None),
            "driving": bool(getattr(c, "Driving", True)),
            "active": bool(getattr(c, "IsActive", True)),
        })
    return out


def validate_sketch(params: Dict[str, Any]) -> Dict[str, Any]:
    """Solver diagnostics for a sketch: DoF, full-constraint state, status string,
    redundant / conflicting / malformed constraint indices, a per-constraint
    breakdown, and a geometry outline the agent can reason about."""
    App, _, _ = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sk = _get_sketch(params.get("sketch") or params.get("name"), doc)

    def _ints(x):
        try:
            return [int(v) for v in (x or [])]
        except Exception:
            return []

    return {
        "sketch": sk.Name,
        "dof": int(sk.DoF),
        "fully_constrained": bool(sk.FullyConstrained),
        "status": str(sk.getStatusString()),
        "redundant": _ints(sk.RedundantConstraints),
        "partially_redundant": _ints(sk.PartiallyRedundantConstraints),
        "conflicting": _ints(sk.ConflictingConstraints),
        "malformed": _ints(sk.MalformedConstraints),
        "missing_equality": _ints(sk.MissingLineEqualityConstraints),
        "missing_point_on_point": _ints(sk.MissingPointOnPointConstraints),
        "missing_radius": _ints(sk.MissingRadiusConstraints),
        "missing_hv": _ints(sk.MissingVerticalHorizontalConstraints),
        "constraints": _sketch_constraint_detail(sk),
        "geometry": _sketch_geom_outline(sk),
        "geometry_count": len(sk.Geometry),
    }


def suggest_constraints(params: Dict[str, Any]) -> Dict[str, Any]:
    """Suggest what to do to a sketch based on the solver:
      - remove redundant/conflicting constraints (with indices),
      - FreeCAD's own missing-constraint hints,
      - a reason about the remaining degrees of freedom + geometry type.
    ``mode='apply'`` additionally runs FreeCAD's ``autoconstraint`` to let the
    solver add the missing 'common' constraints itself."""
    App, _, Sketcher = _app_modules()
    doc = _doc(params.get("doc", "active"))
    sk = _get_sketch(params.get("sketch") or params.get("name"), doc)
    mode = (params.get("mode") or "analyze").lower()

    suggestions: List[Dict[str, Any]] = []

    # fixes first: redundant / conflicting / malformed
    for i in list(getattr(sk, "RedundantConstraints", []) or []):
        suggestions.append({"action": "remove", "constraint": int(i),
                            "why": "redundant"})
    for i in list(getattr(sk, "PartiallyRedundantConstraints", []) or []):
        suggestions.append({"action": "remove", "constraint": int(i),
                            "why": "partially redundant"})
    for i in list(getattr(sk, "ConflictingConstraints", []) or []):
        suggestions.append({"action": "resolve_conflict", "constraint": int(i),
                            "why": "conflicting"})
    for i in list(getattr(sk, "MalformedConstraints", []) or []):
        suggestions.append({"action": "fix", "constraint": int(i),
                            "why": "malformed"})

    # FreeCAD's detectMissing* hints (did not mutate the sketch)
    for attr, kind in (("detectMissingVerticalHorizontalConstraints", "vertical/horizontal"),
                       ("detectMissingPointOnPointConstraints", "point-on-point"),
                       ("detectMissingEqualityConstraints", "equality"),
                       ("detectMissingRadiusConstraints", "radius")):
        if hasattr(sk, attr):
            try:
                res = getattr(sk, attr)()
                for r in (res or []):
                    suggestions.append({"action": "add", "type": kind, "hint": list(r)
                                        if isinstance(r, (list, tuple)) else r})
            except Exception:
                pass

    # reason about remaining DOF + geometry
    dof = int(sk.DoF)
    types = [g.TypeId for g in sk.Geometry]
    n_lines = sum(1 for t in types if "LineSegment" in t)
    n_circ = sum(1 for t in types if "Circle" in t and "Arc" not in t)
    n_arc = sum(1 for t in types if "Arc" in t)
    if dof > 0:
        note = (f"{dof} degree(s) of freedom remain; the sketch is under-constrained. "
                f"Add dimension/position constraints (e.g. Coincident to origin, "
                f"DistanceX/DistanceY, Horizontal/Vertical, Block) to reach "
                f"FullyConstrained. Current geometry: {n_lines} line(s), "
                f"{n_circ} circle(s), {n_arc} arc(s).")
        suggestions.append({"action": "dimension", "why": "underconstrained", "note": note})
    if n_circ:
        suggestions.append({"action": "add", "type": "radius"
                            if not sk.FullyConstrained else None,
                            "hint": "each circle typically wants a Radius/Diameter + a "
                                    "Coincident/distance to position it",
                            "why": "circles"})
    elif n_lines > 0 and dof > 0:
        suggestions.append({"action": "add", "type": "distance",
                            "hint": "a rectangle/profile of lines typically wants "
                                    "DistanceX/DistanceY (or Horizontal/Vertical + "
                                    "distances) to fix its size and position",
                            "why": "lines"})

    if mode == "apply" and hasattr(sk, "autoconstraint"):
        try:
            added = sk.autoconstraint()
            doc.recompute()
            suggestions.append({"action": "applied", "count": int(added),
                                "note": "FreeCAD autoconstraint ran"})
        except Exception as exc:
            suggestions.append({"action": "applied", "count": 0,
                                "note": f"autoconstraint failed: {exc}"})

    return {"sketch": sk.Name, "dof": dof, "fully_constrained": bool(sk.FullyConstrained),
            "suggestions": suggestions}




def set_render_mode(params: Dict[str, Any]) -> Dict[str, Any]:
    """Switch the 3D viewport renderer backend.  The backend is chosen when the
    3D view is created, so we set the preference and recreate the view."""
    App, _, _ = _app_modules()
    mode = (params.get("mode") or "opengl").lower()
    pgrp = App.ParamGet("User parameter:BaseApp/Preferences/View")
    vulkan = mode in ("vulkan", "vk", "ray", "raytracing")
    rt = mode in ("raytracing", "vulkan+rt")
    pgrp.SetBool("UseVulkanRenderer", vulkan)
    pgrp.SetBool("UseVulkanRayTracing", rt)
    recreated = None
    try:
        import FreeCADGui as Gui
        md = Gui.ActiveDocument
        if md is not None:
            mdi = getattr(md, "ActiveView", None)
            clone = getattr(mdi, "clone", None) if mdi is not None else None
            if callable(clone):
                clone()
                recreated = True
    except Exception as exc:
        recreated = str(exc)
    return {"mode": mode, "use_vulkan": vulkan, "ray_tracing": rt,
            "view_recreated": recreated}


# ---------------------------------------------------------------------------
# RPC dispatch table
# ---------------------------------------------------------------------------

HANDLERS: Dict[str, Callable[[Dict[str, Any]], Any]] = {
    # documents / state
    "new_document": new_document,
    "open_document": open_document,
    "active_document": active_document,
    "set_active_document": set_active_document,
    "list_objects": list_objects,
    "delete_object": delete_object,
    "recompute": recompute,
    "snapshot": snapshot,
    "run_python": run_python,
    # logs
    "get_log": get_log,
    "clear_log": clear_log,
    "log": log,
    # placement
    "get_placement": get_placement,
    "set_placement": set_placement,
    # selection
    "get_selection": get_selection,
    "select_objects": select_objects,
    "clear_selection": clear_selection,
    # sketches
    "new_sketch": new_sketch,
    "sketch_rectangle": sketch_rectangle,
    "sketch_polygon": sketch_polygon,
    "sketch_polyline": sketch_polyline,
    "sketch_line": sketch_line,
    "sketch_circle": sketch_circle,
    "sketch_arc": sketch_arc,
    "sketch_spline": sketch_spline,
    "add_constraint": add_constraint,
    # modeling
    "pad": pad,
    "pocket": pocket,
    "extrude": extrude,
    "add_hole": add_hole,
    "mirror": mirror,
    "mirror_object": mirror_object,
    "pattern": pattern,
    # extended sketch coverage
    "sketch_ellipse": sketch_ellipse,
    "sketch_point": sketch_point,
    "sketch_slot": sketch_slot,
    # extended part coverage
    "fillet": fillet,
    "chamfer": chamfer,
    "revolve": revolve,
    "loft": loft,
    "boolean_op": boolean_op,
    "make_box": make_box,
    "make_cylinder": make_cylinder,
    "make_sphere": make_sphere,
    "make_cone": make_cone,
    "make_torus": make_torus,
    "add_fastener": add_fastener,
    "export_objects": export_objects,
    "measure_clearance": measure_clearance,
    # parameters (spreadsheet-backed)
    "add_parameter": add_parameter,
    "set_parameter": set_parameter,
    "get_parameter": get_parameter,
    "list_parameters": list_parameters,
    "get_property": get_property,
    "set_property": set_property,
    "link_property": link_property,
    "capture_parameter": capture_parameter,
    "get_sketch_constraints": get_sketch_constraints,
    "set_sketch_constraint_value": set_sketch_constraint_value,
    "validate_sketch": validate_sketch,
    "suggest_constraints": suggest_constraints,
    "set_render_mode": set_render_mode,
    # GUI / workbench / view
    "run_command": run_command,
    "set_workbench": set_workbench,
    "set_view": set_view,
    "fit_view": fit_view,
    "control_camera": control_camera,
    "screenshot": screenshot,
    # cursor / synthetic input
    "get_cursor": get_cursor,
    "move_cursor": move_cursor,
    "click": click,
}


def handle_request(req: Dict[str, Any]) -> Dict[str, Any]:
    method = req.get("method")
    params = req.get("params") or {}
    handler = HANDLERS.get(method)
    if handler is None:
        return {"ok": False, "error": f"unknown method {method!r}",
                "available": sorted(HANDLERS)}
    try:
        data = handler(params)
        return {"ok": True, "data": data}
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}",
                "traceback": "".join(traceback.format_exc()).splitlines()[-12:]}


# ---------------------------------------------------------------------------
# Socket listener + main-thread executor
# ---------------------------------------------------------------------------

_request_queue: "queue.Queue[tuple[socket.socket, dict]]" = queue.Queue()


def _listener(sock: socket.socket) -> None:
    """Accept connections, read one request line, hand it to the executor."""
    while True:
        try:
            conn, _ = sock.accept()
        except OSError:
            return
        try:
            data = b""
            while b"\n" not in data:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                data += chunk
            line = data.split(b"\n", 1)[0]
            req = json.loads(line.decode("utf-8"))
        except Exception:
            conn.close()
            continue
        _request_queue.put((conn, req))


def _respond(conn: socket.socket, req: Dict[str, Any], result: Dict[str, Any]) -> None:
    try:
        out = {"id": req.get("id"), "result": result}
        conn.sendall((json.dumps(out) + "\n").encode("utf-8"))
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    finally:
        conn.close()


def _process_one() -> bool:
    """Run one queued request (if any) and reply.  Returns True if ran one."""
    try:
        conn, req = _request_queue.get_nowait()
    except queue.Empty:
        return False
    try:
        result = handle_request(req)
    except Exception:  # safety net
        result = {"ok": False, "error": "internal: " + traceback.format_exc()}
    _respond(conn, req, result)
    return True


def _drain_headless() -> None:
    """Blocking executor for FreeCADCmd (no Qt event loop): drain forever."""
    while True:
        if not _process_one():
            # modest idle wait so we don't spin on an empty socket queue
            import time
            time.sleep(0.02)


_keep_alive: List[Any] = []


def _drain_gui() -> None:
    """Qt pump for the GUI mode: process queued requests on the Qt main thread.

    We take over the running Qt loop (FreeCAD's own main loop keeps spinning),
    so we only install a QTimer that drains the RPC queue every tick and keep a
    strong reference to it so it is not garbage-collected.
    """
    from PySide import QtCore
    timer = QtCore.QTimer()
    timer.timeout.connect(lambda: _process_one())
    timer.start(10)
    _keep_alive.append(timer)


def _is_gui() -> bool:
    try:
        import FreeCAD
        return bool(getattr(FreeCAD, "GuiUp", False))
    except Exception:
        return False


def run_guest() -> None:
    """Open the socket, serve until killed.  Called at module import (FreeCAD
    executes this script as top-level code)."""
    install_output_capture()
    try:
        os.unlink(SOCKET_PATH)
    except OSError:
        pass
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(SOCKET_PATH)
    sock.listen(8)
    # Listener thread does the socket I/O; the main thread executes each job so
    # scene/UI mutations always happen on FreeCAD's main thread (never a race).
    threading.Thread(target=_listener, args=(sock,), daemon=True).start()
    if _is_gui():
        _drain_gui()
    else:
        # headless: block here so FreeCADCmd stays alive
        try:
            _drain_headless()
        except KeyboardInterrupt:
            pass


if "FreeCAD" in sys.modules:
    # We are being executed as a script *inside* FreeCAD: the embedded
    # interpreter has the FreeCAD module loaded before it runs the file, which
    # is the reliable signal (FreeCAD does not set __name__ to __main__ for a
    # script).  Never auto-start when merely imported on the host Python (where
    # FreeCAD is absent), so the module stays importable for testing.
    run_guest()

