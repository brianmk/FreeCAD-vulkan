#!/usr/bin/env python3
"""freecad_mcp_server - Model Context Protocol server for a *live* FreeCAD.

The server itself runs on the HOST Python (it is the MCP stdio process that an
MCP client - Claude, Cursor, OpenCode, ... - launches).  Every tool call is
forwarded over a Unix-domain socket to a FreeCAD process running
``freecad_mcp_guest.py`` so it can drive the live model: build sketches of any
shape, extrude/pad/pocket, thread holes, mirror features, read the selection
and read/move the viewport cursor.

Lifecycle / connection:
    * The guest must be running inside a FreeCAD process
      (``FreeCAD freecad_mcp_guest.py`` for the GUI, or
      ``FreeCADCmd freecad_mcp_guest.py`` headless).
    * The server connects on demand (one connection per tool call).  If no
      guest is listening and ``--spawn`` is passed, the server launches the
      FreeCAD GUI with the guest embedded and waits for its socket.

Configuration (environment / CLI):
    FC_MCP_SOCKET   Unix socket path (default /tmp/opencode/freecad_mcp.sock)
    FREECAD_BIN     FreeCAD binary (default build/debug/bin/FreeCAD)
    --spawn         launch FreeCAD on startup if no guest is running
    --headless      spawn FreeCADCmd instead of the GUI
    --env K=V       extra env for a spawned FreeCAD (repeatable)
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
GUEST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "freecad_mcp_guest.py")
DEFAULT_BIN = os.path.join(ROOT, "build/debug/bin/FreeCAD")
SOCKET_PATH = os.environ.get("FC_MCP_SOCKET", "/tmp/opencode/freecad_mcp.sock")

# A spawned FreeCAD needs the harness display/background env; the boost lib path
# is required by this debug build.  Writes are kept as pure passthrough of the
# host env plus these defaults (callers can override via --env).
SPAWN_ENV = {
    "QT_STYLE_OVERRIDE": "fusion",
    "QT_QPA_PLATFORM": "xcb",
    "LD_LIBRARY_PATH": "/tmp/opencode/boost91",
    "FC_SKIP_UNSAVED_PROMPT": "1",
}

MCP_PARENT = os.path.dirname(os.path.abspath(__file__))
if MCP_PARENT not in sys.path:
    sys.path.insert(0, MCP_PARENT)


class FreeCADConnectionError(RuntimeError):
    pass


class GuestClient:
    """Thin line-delimited JSON RPC client to the in-FreeCAD guest agent."""

    def __init__(self, sock_path: str = SOCKET_PATH, timeout: float = 30.0):
        self.sock_path = sock_path
        self.timeout = timeout

    def call(self, method: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        if not os.path.exists(self.sock_path):
            raise FreeCADConnectionError(
                f"no live FreeCAD guest at {self.sock_path} - start it with "
                f"'FreeCAD {GUEST}' or run this server with --spawn")
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.settimeout(self.timeout)
        try:
            conn.connect(self.sock_path)
            req = {"id": 0, "method": method, "params": params or {}}
            conn.sendall((json.dumps(req) + "\n").encode("utf-8"))
            buf = b""
            while b"\n" not in buf:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
            line = buf.split(b"\n", 1)[0]
            resp = json.loads(line.decode("utf-8"))
        except (OSError, socket.timeout) as exc:
            raise FreeCADConnectionError(
                f"could not reach FreeCAD guest ({exc}); is the guest running "
                f"and the socket at {self.sock_path}?") from exc
        finally:
            try:
                conn.close()
            except OSError:
                pass
        result = resp.get("result", {})
        if not result.get("ok", False):
            raise RuntimeError(result.get("error", "unknown error"))
        return result.get("data") or {}

    def is_live(self) -> bool:
        try:
            self.call("active_document")
            return True
        except Exception:
            return False


_client = GuestClient()


def _call(method: str, **params: Any) -> Dict[str, Any]:
    data = _client.call(method, params)
    return data


def _fmt(data: Any) -> str:
    return json.dumps(data, ensure_ascii=False, default=str)


def _error(message: str) -> str:
    return json.dumps({"ok": False, "error": message}, ensure_ascii=False)


# ---------------------------------------------------------------------------
# FreeCAD process management (optional --spawn)
# ---------------------------------------------------------------------------
_spawned: Optional[subprocess.Popen] = None


def _merge_env(overrides: Dict[str, str]) -> Dict[str, str]:
    env = dict(os.environ)
    env.update(SPAWN_ENV)
    if not env.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = SPAWN_ENV["LD_LIBRARY_PATH"]
    env["FC_MCP_SOCKET"] = SOCKET_PATH
    env.update({k: v for k, v in overrides.items() if v is not None})
    return env


def spawn_freecad(headless: bool = False,
                  env_overrides: Optional[Dict[str, str]] = None,
                  binary: Optional[str] = None,
                  wait: int = 30) -> bool:
    """Launch FreeCAD (with the guest embedded) and wait for its socket."""
    global _spawned
    try:
        os.unlink(SOCKET_PATH)
    except OSError:
        pass
    bincmd = binary or (os.environ.get("FREECAD_BIN") or DEFAULT_BIN)
    if not os.path.exists(bincmd):
        if headless:
            bincmd = DEFAULT_BIN.replace("FreeCAD", "FreeCADCmd")
        else:
            bincmd = shutil.which("FreeCAD") or bincmd
    env = _merge_env(env_overrides or {})
    _spawned = subprocess.Popen([bincmd, GUEST], env=env,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.STDOUT)
    t0 = time.time()
    while time.time() - t0 < wait:
        if _client.is_live():
            return True
        if _spawned.poll() is not None:
            raise FreeCADConnectionError(
                f"FreeCAD exited during startup with code {_spawned.returncode}")
        time.sleep(0.25)
    raise FreeCADConnectionError("timed out waiting for FreeCAD guest")


# ---------------------------------------------------------------------------
# MCP tools
# ---------------------------------------------------------------------------
try:
    from mcp.server.fastmcp import FastMCP
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit(
        "This MCP server needs the 'mcp' Python package (v1.x FastMCP). "
        "Install it in a venv, e.g.:  pip install 'mcp<2'\n"
        f"Original error: {exc}") from exc

mcp = FastMCP("freecad")


@mcp.tool()
def status() -> str:
    """Report the live FreeCAD instance: active document, object count,
    selection, GUI availability, and the socket in use."""
    data = _call("list_objects")
    try:
        sel = _call("get_selection")
    except Exception:
        sel = {"selection": [], "gui_available": False}
    try:
        snap = _call("snapshot")
    except Exception:
        snap = {}
    return _fmt({"doc": data.get("doc"), "object_count": data.get("count"),
                 "selection": sel.get("selection", []),
                 "socket": SOCKET_PATH, "snapshot": snap})


@mcp.tool()
def new_document(name: str) -> str:
    """Create a new empty document (e.g. "Part") and return its name."""
    return _fmt(_call("new_document", name=name))


@mcp.tool()
def open_document(path: str) -> str:
    """Open an existing FreeCAD document (.FCStd, .step, .brep, ...) and
    return its name."""
    return _fmt(_call("open_document", path=path))


@mcp.tool()
def active_document() -> str:
    """Return the name of the currently active document, or null."""
    return _fmt(_call("active_document"))


@mcp.tool()
def set_active_document(name: str) -> str:
    """Make the document named `name` the active document."""
    return _fmt(_call("set_active_document", name=name))


@mcp.tool()
def list_objects() -> str:
    """List every object in the active document (name, label, TypeId,
    visibility, state)."""
    return _fmt(_call("list_objects"))


@mcp.tool()
def delete_object(name: str) -> str:
    """Delete the object named `name` from the active document."""
    return _fmt(_call("delete_object", name=name))


@mcp.tool()
def recompute() -> str:
    """Recompute the active document after a model change."""
    return _fmt(_call("recompute"))


@mcp.tool()
def get_selection() -> str:
    """Read the current FreeCAD selection: objects and their sub-elements
    (faces/edges/vertices). Empty when no GUI is attached."""
    return _fmt(_call("get_selection"))


@mcp.tool()
def select_objects(names: List[str]) -> str:
    """Programmatically select the named objects (replaces the selection)."""
    return _fmt(_call("select_objects", names=names))


@mcp.tool()
def clear_selection() -> str:
    """Clear the current selection."""
    return _fmt(_call("clear_selection"))


@mcp.tool()
def get_cursor() -> str:
    """Read the cursor position in the 3D viewport (global px, viewport
    logical px, device px, viewport size, dpr) plus the hovered object."""
    return _fmt(_call("get_cursor"))


@mcp.tool()
def move_cursor(x: float, y: float) -> str:
    """Move the mouse cursor to a logical viewport position (x, y in px) and
    return the sampled position + hovered object."""
    return _fmt(_call("move_cursor", x=x, y=y))


@mcp.tool()
def click(x: float, y: float) -> str:
    """Synthetic left-click at a logical viewport position (x, y px).
    Returns whether something was hit and the resulting selection."""
    return _fmt(_call("click", x=x, y=y))


@mcp.tool()
def snapshot() -> str:
    """Return viewport + document + selection + camera state (a live probe
    snapshot)."""
    return _fmt(_call("snapshot"))


@mcp.tool()
def get_log(limit: int = 200, tail: bool = True) -> str:
    """Return recent FreeCAD log output (stdout/stderr of the process, plus
    every C++/Qt/Python console message). `limit` = up to N lines; `tail=True`
    returns the most recent lines (default) or the oldest."""
    return _fmt(_call("get_log", limit=limit, tail=tail))


@mcp.tool()
def log(level: str = "message", message: str = "") -> str:
    """Write a line into the FreeCAD console/log. `level` is message/warning/
    error/log."""
    return _fmt(_call("log", level=level, message=message))


@mcp.tool()
def clear_log() -> str:
    """Clear the captured log buffer."""
    return _fmt(_call("clear_log"))


@mcp.tool()
def get_placement(name: str) -> str:
    """Return the Placement (base + rotation) of the named object."""
    return _fmt(_call("get_placement", name=name))


@mcp.tool()
def set_placement(name: str, base: List[float]) -> str:
    """Set the base position of the named object to `base` (keep rotation)."""
    return _fmt(_call("set_placement", name=name, base=base))


@mcp.tool()
def new_sketch(name: str = "Sketch", plane: str = "XY",
               body: str = "auto") -> str:
    """Create a new sketch. `plane` is XY/XZ/YZ. `body` is 'auto' (create or
    reuse a PartDesign::Body), a body name, or 'None' for a free sketch.
    Returns the sketch name, its body and plane."""
    return _fmt(_call("new_sketch", name=name, plane=plane, body=body))


@mcp.tool()
def sketch_rectangle(sketch: str, x0: float = 0.0, y0: float = 0.0,
                     x1: float = 10.0, y1: float = 10.0,
                     construction: bool = False) -> str:
    """Add a closed rectangle to `sketch` (corner-to-corner). Set
    `construction=True` for a reference line that does not drive the solid."""
    return _fmt(_call("sketch_rectangle", sketch=sketch, x0=x0, y0=y0,
                      x1=x1, y1=y1, construction=construction))


@mcp.tool()
def sketch_polygon(sketch: str, radius: float = 10.0, sides: int = 6,
                   center: Optional[List[float]] = None,
                   construction: bool = False) -> str:
    """Add a regular closed N-gon to `sketch` (center=[x,y]). `construction`
    toggles reference (non-solid-driving) geometry."""
    return _fmt(_call("sketch_polygon", sketch=sketch, radius=radius,
                      sides=sides, center=center or [0, 0],
                      construction=construction))


@mcp.tool()
def sketch_polyline(sketch: str, points: List[List[float]],
                    closed: bool = True, construction: bool = False) -> str:
    """Add a polyline to `sketch`. `points` is [[x,y], ...]; set closed=True
    to link the last point back to the first. `construction` toggles reference
    (non-solid-driving) geometry."""
    return _fmt(_call("sketch_polyline", sketch=sketch, points=points,
                      closed=closed, construction=construction))


@mcp.tool()
def sketch_line(sketch: str, p0: List[float], p1: List[float],
                construction: bool = False) -> str:
    """Add a single line segment to `sketch` from p0 to p1 ([x,y]).
    `construction=True` makes it a reference line (used for symmetry/helpers,
    not part of the solid)."""
    return _fmt(_call("sketch_line", sketch=sketch, p0=p0, p1=p1,
                      construction=construction))


@mcp.tool()
def sketch_circle(sketch: str, radius: float = 10.0,
                  center: Optional[List[float]] = None,
                  construction: bool = False) -> str:
    """Add a full circle to `sketch` (center=[x,y]). `construction=True` makes
    it a reference circle (e.g. a bolt circle, not part of the solid)."""
    return _fmt(_call("sketch_circle", sketch=sketch, radius=radius,
                      center=center or [0, 0], construction=construction))


@mcp.tool()
def sketch_arc(sketch: str, radius: float = 10.0,
               center: Optional[List[float]] = None,
               start_angle: float = 0.0, end_angle: float = 90.0,
               construction: bool = False) -> str:
    """Add an arc to `sketch` (center=[x,y], angles in degrees).
    `construction=True` makes it a reference arc."""
    return _fmt(_call("sketch_arc", sketch=sketch, radius=radius,
                      center=center or [0, 0], start_angle=start_angle,
                      end_angle=end_angle, construction=construction))


@mcp.tool()
def sketch_spline(sketch: str, points: List[List[float]],
                  construction: bool = False) -> str:
    """Add a spline to `sketch` through the given [[x,y], ...] points.
    `construction=True` makes it a reference spline."""
    return _fmt(_call("sketch_spline", sketch=sketch, points=points,
                      construction=construction))


@mcp.tool()
def add_constraint(sketch: str, type: str = "Coincident",
                   args: Optional[List[int]] = None) -> str:
    """Add a Sketcher constraint to `sketch`.

`type` is the constraint name (case-insensitive): Coincident, Horizontal,
Vertical, DistanceX, DistanceY, Distance, Radius, Diameter, Block, Symmetric,
Parallel, Perpendicular, Tangent, Equal, PointOnObject, Angle, ...

`args` is the Sketcher DSL (geometry index + vertex position per operand). Vertex
positions: line start=1, line end=2, circle/arc centre=3, whole geometry=0.

Arg counts by type:
  args=[g,p]            DistanceX / DistanceY (that point's horizontal/vertical
                        offset from the origin; 2 args) or Radius/Diameter on g
  args=[g1,p1,g2,p2]    Coincident, Distance (between two points), Parallel,
                        Perpendicular, Tangent, Symmetric, collinear
  args=[g]              Block, Horizontal, Vertical (whole geometry)

Examples:
  args=[0,2,1,2]        Coincident: geo0 end <-> geo1 end
  args=[0,3]            DistanceX of circle-0 centre from origin
  args=[0]              Radius on circle geo0

A dimension constraint fixes only its value; set the value separately with
set_sketch_constraint_value (returns the constraint index). After adding, recompute
then validate_sketch to catch any redundant/conflicting result."""
    return _fmt(_call("add_constraint", sketch=sketch, type=type,
                      args=args or []))


@mcp.tool()
def pad(sketch: str, length: float = 10.0, name: str = "Pad",
        symmetric: bool = False, reversed: bool = False) -> str:
    """Extrude the `sketch` (a closed, non-self-intersecting profile) into a solid
    and add it as a PartDesign::Pad in the Body. `length` is in mm; `symmetric`
    extrudes both ways about the sketch plane; `reversed` flips the direction.
    The sketch must be closed/coincident to form a valid profile - check it with
    validate_sketch first. After recompute, verify with get_property(Body,...,'Length')
    or inspect the result."""
    return _fmt(_call("pad", sketch=sketch, length=length, name=name,
                      symmetric=symmetric, reversed=reversed))


@mcp.tool()
def pocket(sketch: str, length: float = 10.0, name: str = "Pocket",
           symmetric: bool = False, reversed: bool = False) -> str:
    """Cut the `sketch`'s profile out of the solid (PartDesign::Pocket). `length`
    is the cut depth in mm (larger than the body if you want a through cut);
    `symmetric` cuts both ways; `reversed` flips the cut direction. The profile
    must lie on a face/plane of the body. After recompute, verify with
    get_property or measure_clearance."""
    return _fmt(_call("pocket", sketch=sketch, length=length, name=name,
                      symmetric=symmetric, reversed=reversed))


@mcp.tool()
def extrude(sketch: str, length: float = 10.0, dir: Optional[List[float]] = None,
            name: str = "Extrusion", solid: bool = True) -> str:
    """Extrude a sketch/shape with the Part workbench (not body-bound).
    Returns the Part::Extrusion solid with its bounding box."""
    return _fmt(_call("extrude", sketch=sketch, length=length,
                      dir=dir or [0, 0, 1], name=name, solid=solid))


@mcp.tool()
def add_hole(sketch: str, diameter: float = 5.0, depth: float = 10.0,
             threaded: bool = True, model_thread: bool = False,
             throughall: bool = False, drill_point: str = "flat",
             hole_cut: str = "none", name: str = "Hole") -> str:
    """Add a threaded hole (PartDesign::Hole) from a circle sketch. `threaded`
    toggles the cosmetic thread; `model_thread` generates real thread geometry;
    `throughall` drills through; `hole_cut` is none/counterbore/countersink/
    counterdrill; `drill_point` is flat/angled."""
    return _fmt(_call("add_hole", sketch=sketch, diameter=diameter, depth=depth,
                      threaded=threaded, model_thread=model_thread,
                      throughall=throughall, drill_point=drill_point,
                      hole_cut=hole_cut, name=name))


@mcp.tool()
def mirror(feature: Optional[str] = None, plane: str = "XY",
           name: str = "Mirror") -> str:
    """Mirror the previous (or named `feature`) about a body plane (XY/XZ/YZ)
    using PartDesign::Mirrored."""
    return _fmt(_call("mirror", feature=feature, plane=plane, name=name))


@mcp.tool()
def mirror_object(source: str, normal: Optional[List[float]] = None,
                  base: Optional[List[float]] = None,
                  name: str = "Mirroring") -> str:
    """Mirror a whole Part object about a plane at `base` along `normal`
    (Part workbench mirroring, e.g. a box about the YZ plane)."""
    return _fmt(_call("mirror_object", source=source,
                      normal=normal or [0, 1, 0], base=base or [0, 0, 0],
                      name=name))


@mcp.tool()
def pattern(feature: Optional[str] = None, style: str = "polar",
            occurrences: int = 3, axis: str = "Z", angle: float = 360.0,
            length: float = 10.0, mode: str = "whole",
            reversed: bool = False, name: Optional[str] = None) -> str:
    """Repeat an existing PartDesign feature (pad/pocket) as a pattern.

`style`:
  polar        -> PartDesign PolarPattern about origin `axis` (X/Y/Z). `mode`=
                  'whole' (Extent) sweeps `angle` as the total span across all
                  occurrences; 'spacing' or 'single' sets `angle` as the gap
                  between consecutive copies. Use occurrences+angle e.g. to make
                  a ring of holes about a hole.
  rectangular  -> PartDesign LinearPattern along origin `axis`, step `length`,
                  `occurrences` copies.

Call only AFTER the source feature exists (pad/pocket it first); `feature`
defaults to the body's tip. Then recompute + validate_sketch/measure_clearance to
check the pattern nests without overlaps."""
    return _fmt(_call("pattern", feature=feature, style=style,
                      occurrences=occurrences, axis=axis, angle=angle,
                      length=length, mode=mode, reversed=bool(reversed),
                      name=name))


@mcp.tool()
def run_command(command: str) -> str:
    """Run a FreeCAD GUI command by name (e.g. 'Std_New', 'PartDesign_Pad')."""
    return _fmt(_call("run_command", command=command))


@mcp.tool()
def set_workbench(name: str = "PartDesignWorkbench") -> str:
    """Activate a FreeCAD workbench by name."""
    return _fmt(_call("set_workbench", name=name))


@mcp.tool()
def set_view(name: str = "top") -> str:
    """Set the 3D view orientation: top, front, right, isometric, home."""
    return _fmt(_call("set_view", name=name))


@mcp.tool()
def fit_view() -> str:
    """Zoom/pan the view to fit all objects on screen."""
    return _fmt(_call("fit_view"))


@mcp.tool()
def control_camera(action: str = "fit", direction: Optional[List[float]] = None) -> str:
    """Drive the 3D viewport camera. `action` is one of: fit | isometric | top |
    front | right | rear | bottom | left | dimetric | trimetric | axometric |
    zoom_in | zoom_out | rotate_left | rotate_right | set_direction."""
    return _fmt(_call("control_camera", action=action, direction=direction))


@mcp.tool()
def screenshot(path: str = "/tmp/opencode/freecad_mcp_shot.png",
               format: str = "PNG") -> str:
    """Save a static image of the viewport. `format` is e.g. PNG."""
    return _fmt(_call("screenshot", path=path, format=format))


@mcp.tool()
def run_python(code: str, mode: str = "exec") -> str:
    """Run a FreeCAD Python snippet. mode='exec' for statements, 'eval' for a
    single expression. Returns any printed stdout and the eval result."""
    return _fmt(_call("run_python", code=code, mode=mode))


# --- extended sketch coverage ---

@mcp.tool()
def sketch_ellipse(sketch: str, center: Optional[List[float]] = None,
                   major_radius: float = 20.0, minor_radius: float = 10.0,
                   construction: bool = False) -> str:
    """Add an ellipse to `sketch` (center=[x,y]). `construction=True` makes it
    a reference ellipse."""
    return _fmt(_call("sketch_ellipse", sketch=sketch, center=center or [0, 0],
                      major_radius=major_radius, minor_radius=minor_radius,
                      construction=construction))


@mcp.tool()
def sketch_point(sketch: str, position: Optional[List[float]] = None) -> str:
    """Add a point (vertex) to `sketch` (position=[x,y])."""
    return _fmt(_call("sketch_point", sketch=sketch, position=position or [0, 0]))


@mcp.tool()
def sketch_slot(sketch: str, center: Optional[List[float]] = None,
                length: float = 20.0, width: float = 10.0,
                construction: bool = False) -> str:
    """Add a rounded slot (stadium) to `sketch` (center=[x,y]).
    `construction=True` makes it a reference slot."""
    return _fmt(_call("sketch_slot", sketch=sketch, center=center or [0, 0],
                      length=length, width=width, construction=construction))


# --- extended part coverage ---

@mcp.tool()
def fillet(feature: str, edges: List[str], radius: float = 1.0,
           name: str = "Fillet") -> str:
    """Add a PartDesign fillet to `edges` (e.g. ['Edge1','Edge2']) of `feature`."""
    return _fmt(_call("fillet", feature=feature, edges=list(edges),
                      radius=radius, name=name))


@mcp.tool()
def chamfer(feature: str, edges: List[str], size: float = 1.0,
            name: str = "Chamfer") -> str:
    """Add a PartDesign chamfer to `edges` of `feature`."""
    return _fmt(_call("chamfer", feature=feature, edges=list(edges),
                      size=size, name=name))


@mcp.tool()
def revolve(sketch: str, angle: float = 360.0, axis_object: Optional[str] = None,
            axis_element: str = "V_Axis", name: str = "Revolution") -> str:
    """Revolve `sketch` (in a body) about an axis (default the body's V_Axis)."""
    return _fmt(_call("revolve", sketch=sketch, angle=angle,
                      axis_object=axis_object, axis_element=axis_element, name=name))


@mcp.tool()
def loft(sections: List[str], name: str = "Loft") -> str:
    """Loft through `sections` (list of sketch names, >= 2)."""
    return _fmt(_call("loft", sections=list(sections), name=name))


@mcp.tool()
def boolean_op(shape: str, tool: str, mode: str = "fuse",
               name: Optional[str] = None) -> str:
    """Boolean of two objects. `shape` and `tool` are object names; `mode` is
    'fuse' (union), 'cut' (shape - tool; order matters), or 'common' (intersection).
    Result is created as a Part::Boolean with `name`. Both inputs must be solids
    with a Shape; verify with measure_clearance if you expect them to mate."""
    return _fmt(_call("boolean_op", shape=shape, tool=tool, mode=mode, name=name))


@mcp.tool()
def make_box(length: float = 10.0, width: float = 10.0, height: float = 10.0,
             name: str = "Box", position: Optional[List[float]] = None) -> str:
    """Create a Part::Box."""
    return _fmt(_call("make_box", length=length, width=width, height=height,
                      name=name, position=position))


@mcp.tool()
def make_cylinder(radius: float = 5.0, height: float = 10.0,
                  name: str = "Cylinder", position: Optional[List[float]] = None) -> str:
    """Create a Part::Cylinder."""
    return _fmt(_call("make_cylinder", radius=radius, height=height,
                      name=name, position=position))


@mcp.tool()
def make_sphere(radius: float = 5.0, name: str = "Sphere",
                position: Optional[List[float]] = None) -> str:
    """Create a Part::Sphere."""
    return _fmt(_call("make_sphere", radius=radius, name=name, position=position))


@mcp.tool()
def make_cone(radius1: float = 5.0, radius2: float = 0.0, height: float = 10.0,
              name: str = "Cone", position: Optional[List[float]] = None) -> str:
    """Create a Part::Cone."""
    return _fmt(_call("make_cone", radius1=radius1, radius2=radius2,
                      height=height, name=name, position=position))


@mcp.tool()
def make_torus(radius1: float = 10.0, radius2: float = 2.0, name: str = "Torus",
               position: Optional[List[float]] = None) -> str:
    """Create a Part::Torus."""
    return _fmt(_call("make_torus", radius1=radius1, radius2=radius2,
                      name=name, position=position))


@mcp.tool()
def add_fastener(object_name: str = "Pocket", screw_type: str = "ISO4014",
                 centers: Optional[List[List[float]]] = None,
                 diameter: Optional[float] = None, flip: bool = True) -> str:
    """Add Fasteners screws/bolts to the circular hole faces of `object_name`.
    Restrict to `centers=[[x,y],..]` (hole centre on the top face) and/or
    `diameter`; screw size derives from the matching hole. `screw_type` is the
    uppercase Fasteners code, e.g. 'ISO4014' (hex), 'ISO4762' (socket cap),
    'DIN933' (hex, full thread). `flip=True` sets Invert so the head sits on the
    top face (head-up). Returns the created screws and the hole edges used."""
    return _fmt(_call("add_fastener", object_name=object_name, screw_type=screw_type,
                      centers=centers or [], diameter=diameter, flip=flip))


@mcp.tool()
def export_objects(path: str, objects: Optional[List[str]] = None) -> str:
    """Export objects to `path` (extension selects STEP/STL/IGES...). Use
    `objects=[names]` or export everything with a Shape."""
    return _fmt(_call("export_objects", path=path, objects=objects))


@mcp.tool()
def set_render_mode(mode: str = "opengl") -> str:
    """Switch the 3D viewport renderer backend. mode='opengl'|'vulkan'|'raytracing'.
    Sets the preference and recreates the active 3D view."""
    return _fmt(_call("set_render_mode", mode=mode))


@mcp.tool()
def measure_clearance(a: Optional[str] = None, b: Optional[str] = None,
                      objects: Optional[List[str]] = None) -> str:
    """Validate clearance between objects (mm). Give `a`+`b` for one pair, or
    `objects=[names]` for all pairs. Returns `min_distance_mm` (min surface gap;
    0 when touching/overlapping) and `overlaps`/`common_volume_mm3` when the
    shapes' intersection has positive volume (a real collision). Use after any
    geometric change to confirm parts do not interpenetrate."""
    return _fmt(_call("measure_clearance", a=a, b=b, objects=objects))


@mcp.tool()
def add_parameter(name: str, value: Any, unit: Optional[str] = None) -> str:
    """Create/redefine a named parameter stored as a Spreadsheet alias. `value` is
    a number or a '40 mm' string; pass `unit` to append a unit to a number."""
    return _fmt(_call("add_parameter", name=name, value=value, unit=unit))


@mcp.tool()
def set_parameter(name: str, value: Any, unit: Optional[str] = None) -> str:
    """Update a named parameter's value (creates it if missing). `value` is a
    number or a '40 mm' string; `unit` appends a unit to a numeric value. Changing
    a value propagates to any property linked via link_property."""
    return _fmt(_call("set_parameter", name=name, value=value, unit=unit))


@mcp.tool()
def get_parameter(name: str) -> str:
    """Read a parameter's value."""
    return _fmt(_call("get_parameter", name=name))


@mcp.tool()
def list_parameters() -> str:
    """List all parameters (spreadsheet aliases) and their values."""
    return _fmt(_call("list_parameters"))


@mcp.tool()
def get_property(object: str, property: str) -> str:
    """Read a property from an object (e.g. Pad.Length)."""
    return _fmt(_call("get_property", object=object, property=property))


@mcp.tool()
def set_property(object: str, property: str, value: str) -> str:
    """Set a property on an object directly (e.g. Pad.Length='36 mm')."""
    return _fmt(_call("set_property", object=object, property=property, value=value))


@mcp.tool()
def link_property(object: str, property: str, parameter: str) -> str:
    """Bind `object.property` to a parameter via an Expression (=Params.<parameter>)
    so the dimension is driven by the spreadsheet cell. `object` is an object name
    (e.g. 'Pad'), `property` a numeric property (e.g. 'Length'), `parameter` a name
    already created by add_parameter. Works on feature properties (Pad.Length,
    Pocket.Length); it does NOT drive raw sketch geometry coordinates."""
    return _fmt(_call("link_property", object=object, property=property, parameter=parameter))


@mcp.tool()
def capture_parameter(object: str, property: str, parameter: str) -> str:
    """Convert a measurement into a parameter: reads the current `object.property`
    value, stores it as the `parameter`, then binds the property to it so the
    dimension is now driven by (and editable via) the parameter."""
    return _fmt(_call("capture_parameter", object=object, property=property, parameter=parameter))


@mcp.tool()
def get_sketch_constraints(sketch: str) -> str:
    """List a sketch's constraints (index, type, value) — for dimension introspection."""
    return _fmt(_call("get_sketch_constraints", sketch=sketch))


@mcp.tool()
def set_sketch_constraint_value(sketch: str, index: int, value: str) -> str:
    """Set a sketch constraint's datum value (e.g. '40 mm')."""
    return _fmt(_call("set_sketch_constraint_value", sketch=sketch, index=index, value=value))


@mcp.tool()
def validate_sketch(sketch: str) -> str:
    """Solver diagnostics for a sketch: degrees of freedom (DoF), FullyConstrained
    flag, status string, redundant/conflicting/malformed constraint indices, a
    per-constraint breakdown and a geometry outline."""
    return _fmt(_call("validate_sketch", sketch=sketch))


@mcp.tool()
def suggest_constraints(sketch: str, mode: str = "analyze") -> str:
    """Suggest what to do to a sketch: remove redundant/conflicting constraints,
    FreeCAD's missing-constraint hints, and a reason about remaining DOF.
    mode='apply' runs FreeCAD's autoconstraint to add missing constraints."""
    return _fmt(_call("suggest_constraints", sketch=sketch, mode=mode))


# ---------------------------------------------------------------------------
def main(argv: Optional[List[str]] = None) -> int:
    import argparse
    argv = list(sys.argv[1:] if argv is None else argv)
    p = argparse.ArgumentParser(prog="freecad_mcp_server",
                                description="MCP server for a live FreeCAD")
    p.add_argument("--spawn", action="store_true",
                   help="launch FreeCAD with the guest on startup if none is running")
    p.add_argument("--headless", action="store_true",
                   help="with --spawn, use FreeCADCmd instead of the GUI")
    p.add_argument("--binary", default=None, help="FreeCAD binary to spawn")
    p.add_argument("--env", action="append", default=[], metavar="K=V",
                   help="extra env for a spawned FreeCAD (repeatable)")
    p.add_argument("--socket", default=None, help="Unix socket path")
    p.add_argument("--timeout", type=int, default=30,
                   help="seconds to wait for a spawned guest")
    args = p.parse_args(argv)

    global SOCKET_PATH, _client
    if args.socket:
        SOCKET_PATH = args.socket
        _client = GuestClient(sock_path=args.socket)

    if args.spawn:
        try:
            spawn_freecad(headless=args.headless, binary=args.binary,
                          env_overrides=_parse_env(args.env), wait=args.timeout)
        except FreeCADConnectionError as exc:
            print(f"[spawn] {exc}", file=sys.stderr)
            return 1
    elif not _client.is_live():
        # Informative, not fatal: the server still starts (an MCP client may
        # attach the guest later); status/tools will surface the connection error.
        print(f"[mcp] no live FreeCAD guest at {SOCKET_PATH}; tools will fail "
              f"until the guest starts (see README)", file=sys.stderr)

    mcp.run(transport="stdio")
    return 0


def _parse_env(pairs: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for item in pairs:
        k, _, v = item.partition("=")
        out[k] = v
    return out


if __name__ == "__main__":
    sys.exit(main())
