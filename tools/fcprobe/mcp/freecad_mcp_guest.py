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
    ctype = (params.get("type") or "Coincident").capitalize()
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


def _add_closed_profile(sketch: Any, pts: List[List[float]], doc: Any,
                        Sketcher: Any, Part: Any, App: Any) -> None:
    """Add pts as a polygon of line segments + coincident constraints, closing
    the loop so the wire is a valid Pad/Pocket profile."""
    edges = []
    from FreeCAD import Vector
    for i in range(len(pts)):
        p0, p1 = pts[i], pts[(i + 1) % len(pts)]
        edges.append(Part.LineSegment(Vector(p0[0], p0[1], 0),
                                      Vector(p1[0], p1[1], 0)))
    for g in edges:
        sketch.addGeometry(g, False)
    n = len(edges)
    for i in range(n):
        a = (i, 2)
        b = ((i + 1) % n, 1)
        sketch.addConstraint(Sketcher.Constraint("Coincident", a[0], a[1], b[0], b[1]))


def _add_open_polyline(sketch: Any, pts: List[List[float]], doc: Any,
                       Sketcher: Any, Part: Any, App: Any) -> None:
    from FreeCAD import Vector
    edges = [Part.LineSegment(Vector(pts[i][0], pts[i][1], 0),
                              Vector(pts[i + 1][0], pts[i + 1][1], 0))
             for i in range(len(pts) - 1)]
    for g in edges:
        sketch.addGeometry(g, False)
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
    _add_closed_profile(sketch, pts, doc, Sketcher, Part, App)
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
    _add_closed_profile(sketch, pts, doc, Sketcher, Part, App)
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
    _add_open_polyline(sketch, pts, doc, Sketcher, Part, App)
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
    sketch.addGeometry(g, False)
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
    sketch.addGeometry(c, False)
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
    sketch.addGeometry(arc, False)
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
    sketch.addGeometry(bs, False)
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


def run_command(params: Dict[str, Any]) -> Dict[str, Any]:
    try:
        import FreeCADGui
    except Exception:
        raise RuntimeError("run_command requires the FreeCAD GUI")
    cmd = params.get("command") or params.get("name")
    FreeCADGui.runCommand(cmd)
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
    # GUI / workbench / view
    "run_command": run_command,
    "set_workbench": set_workbench,
    "set_view": set_view,
    "fit_view": fit_view,
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

