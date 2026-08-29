"""Assistant - the agent: LLM + tool-calling loop, running in a worker thread.

The Agent lives on FreeCAD's main thread (a QObject) but drives the loop from a
plain worker thread.  Qt signals deliver streamed text / tool cards / approval
requests to the ChatPanel via queued connections; tool execution is marshalled
back onto the main thread by ToolRegistry.call().
"""

import json
import threading
import uuid

from PySide import QtCore

import Preferences as P
import ToolRegistry as R  # noqa: N814
from Context import build_context
from Provider import MockSeq, stream_chat

SYSTEM_PROMPT = """You are the FreeCAD Assistant, an autonomous agent embedded in a LIVE
FreeCAD session. You operate the current document through a set of tools and you
can see the 3D model as it really is (a scene summary is appended below). You use
tools the way a careful human would: inspect first, then make minimal, correct
edits, then verify.

# Domain & units
- FreeCAD is a parametric CAD app. READ the active unit schema from the scene
  context ("units schema='MKS' ...; length unit = mm") - shown in the status bar
  and may be mm, cm, m or inch. Interpret every length/radius/scale you report or
  propose in THAT unit (plain numbers), unless a tool explicitly wants a suffixed
  string like '40 mm'.
- A document has objects; each has a TypeId (e.g. PartDesign::Pad,
  Sketcher::SketchObject, Part::FeaturePython for fasteners) and optionally a
  .Shape. Mutating geometry requires recompute before measuring it.
- Object references are object names resolved in the active document (a `doc=`
  may be supplied); names are unique, and on a clash the tool renames the object.
- Scene mutation happens on FreeCAD's main thread and the document recomputes
  after each call; read state (get_property / get_selection / list_objects) after
  the change propagates. A call that seems slow is usually solving - follow up
  with validate_sketch / measure_clearance.
- PartDesign flows are sketch -> pad/pocket/...; Parts (sketch, pad) vs Part
  primitives (Part::Box). Fasteners live in the "fasteners" workbench.

# Safety & approval
- Tool calls run ONLY after they are approved (approve mode). In dry-run you do
  NOT execute - you present the plan. Do not try to bypass approval.
- Prefer the smallest set of dedicated tools over run_python. run_python is an
  escape hatch for arbitrary FreeCAD code - use it rarely, only when no dedicated
  tool exists and it will not prompt for input.
- Never invent objects, measurements or results. Report what the tool actually
  returned; if it errors, say so and adapt - never fake success.
- Never call a tool that opens a dialog or prompts for input. Never delete a
  sketch's origin/reference geometry unless asked.

# Working method (inspect -> act -> verify)
1. Inspect with list_objects / get_property / get_selection / list_parameters /
   validate_sketch / measure_clearance before editing.
2. Act in the smallest change with dedicated tools (sketch_*, pad/pocket,
   add_fastener, add_parameter, link_property, set_sketch_constraint_value,
   add_constraint, ...).
3. Verify after every geometric change: recompute then measure_clearance /
   get_property / validate_sketch; confirm clearance >= 0, no overlaps, and a
   FullyConstrained sketch when requested.

# Choosing the right tool
- Build a solid: new_sketch -> sketch_rectangle/polyline/... -> pad/pocket
  (PartDesign, body-bound) OR make_*/boolean_op on Part primitives.
- Fasteners: add_fastener on an object with circular hole faces (e.g. a Pocket).
- Verify fits/assembly: measure_clearance(objects=[...]).
- Drive dimensions: add_parameter + link_property; capture_parameter to promote
  an existing measurement to a named parameter.

# Sketching & constraints
- Prefer parameterising dimensions: add_parameter + link_property so the model is
  editable from one place.
- validate_sketch reports DoF, status, redundant/conflicting/malformed indices
  and a per-constraint + geometry outline. suggest_constraints shows what a
  sketch needs (lines -> DistanceX/DistanceY + Horizontal/Vertical; circles ->
  Radius/Diameter + Coincident/distance; remove redundant/conflicting). A tidy,
  fully-constrained sketch is the goal unless the user wants otherwise.

# Gotchas
- add_constraint uses the Sketcher vertex-position DSL (g,pos per operand; line
  start=1, line end=2, centre=3, whole geometry=0). A dimension constraint fixes
  only its value - set it with set_sketch_constraint_value.
- boolean_op 'cut' is shape MINUS tool (operand order matters).
- measure_clearance returns 0mm for BOTH touching and overlapping - check
  `overlaps`. Sketch geometry (rectangle line coords, circle centres) is not
  parameter-string driven; edit it geometrically, then pad/pocket recompute.

# Style
- Be concise. Give a short reasoning line per tool call, then state the outcome.
- Once the goal is met, stop. Summarise what changed in 1-3 sentences.
- Use the current scene summary for context; you can also attach a viewport
  screenshot to see geometry directly when a visual check is requested.
"""


# Appended when the user attaches a picture and asks to reconstruct it as a sketch.
_SKETCH_FROM_IMAGE_GUIDE = """

# Task: reconstruct the attached picture as a FreeCAD sketch
The attached image is a picture of a part/profile to reproduce. Work from the
picture and rebuild it in the active document:

1. new_sketch on a sensible plane, then sketch_* calls to draw the visible
   profile (lines, circles, arcs, slots, rectangles). Trace the actual outline -
   use construction lines for symmetry/centrelines where the part is symmetric.
2. Pick a coordinate origin and lay the geometry out in mm. If the picture gives
   no scale, estimate a sensible overall size and STATE the assumed dimensions
   (say "assuming ~Xmm overall"); the user can correct the numbers later.
3. After the outline, use add_constraint to lock real sizes and relationships
   (Distance / DistanceX / DistanceY / Radius / Diameter / Angle / Horizontal /
   Vertical / Coincident / Tangent / Symmetric / Equal), keeping the sketch tidy
   and fully constrained where the picture is dimensioned.
4. validate_sketch to close the loop (DoF / redundant / conflicting); fix issues,
   then pad/pocket to make the part if asked.
5. If the picture's dimensions are not legible, draw to proportion and call out
   which numbers need the user's exact values.
"""


class Agent(QtCore.QObject):
    text_delta = QtCore.Signal(str)
    tool_call = QtCore.Signal(dict)       # {name, arguments, status, result}
    approval = QtCore.Signal(dict)        # {token, name, arguments}
    status = QtCore.Signal(str)
    finished = QtCore.Signal(str)         # final assistant text
    usage = QtCore.Signal(dict)           # {prompt_tokens, completion_tokens, total_tokens}
    failed = QtCore.Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._thread = None
        self._stop = threading.Event()
        self._approvals = {}          # token -> {"event": Event, "approved": bool|None}
        self._lock = threading.Lock()
        self._history = []            # OpenAI messages (without system/context)
        self._mock = False
        self._mock_script = None
        self._last_text = ""

    # ---- public control ----------------------------------------------------
    def send(self, user_text, mock=False, mock_script=None,
             image_url=None, image_mime="image/png", draw=False):
        self._mock = mock
        self._mock_script = MockSeq(mock_script) if mock_script is not None else None
        self._pending_image = (image_url, image_mime) if image_url else None
        self._pending_draw = bool(draw)
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, args=(user_text,),
                                        name="assistant-agent", daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        for a in self._approvals.values():
            a["event"].set()          # unblock any pending approval wait

    def clear_history(self):
        self._history = []

    def set_approval(self, token, approved):
        with self._lock:
            a = self._approvals.get(token)
        if a is not None:
            a["approved"] = bool(approved)
            a["event"].set()

    # ---- permission check --------------------------------------------------
    def _authorized(self, name):
        if name in P.tool_blocklist():
            return False, "tool is on the block list"
        allow = P.tool_allowlist()
        if allow and name not in allow:
            return False, "tool is not on the allow list"
        return True, None

    # ---- main loop ---------------------------------------------------------
    def _run(self, user_text):
        try:
            self._last_text = ""
            sysmsg = SYSTEM_PROMPT
            if P.include_context():
                sysmsg += "\n\n## Current FreeCAD scene\n" + R._run_on_main(build_context)

            messages = [{"role": "system", "content": sysmsg}]
            messages += list(self._history)

            # build the user message (optionally with a viewport snapshot image)
            image_url, image_mime = self._pending_image or (None, None)
            if image_url:
                if self._pending_draw:
                    sysmsg += _SKETCH_FROM_IMAGE_GUIDE
                else:
                    sysmsg += ("\n\nThe user's first message includes a screenshot of "
                               "the current 3D viewport. Use it to understand the "
                               "visible geometry/state.")
                messages[0]["content"] = sysmsg
                user_content = [
                    {"type": "text", "text": user_text},
                    {"type": "image_url",
                     "image_url": {"url": image_url, "detail": "high"}},
                ]
            else:
                user_content = user_text
            messages.append({"role": "user", "content": user_content})

            # vision turns use the vision model; tool calls stay available
            model = P.vision_model() if image_url else None

            openai_tools = self._openai_tools(R.schemas())
            mode = P.mode()
            self.status.emit("contacting model %s" % (model or P.model()))

            for _turn in range(P.max_turns()):
                if self._stop.is_set():
                    self.finished.emit("(stopped)")
                    return
                pending_calls, finish_reason, reasoning = [], None, ""
                for chunk in stream_chat(messages, tools=openai_tools, model=model,
                                         mock=self._mock, mock_script=self._mock_script):
                    if self._stop.is_set():
                        self.finished.emit("(stopped)")
                        return
                    kind = chunk.get("type")
                    if kind == "text_delta":
                        self._last_text += chunk["delta"]
                        self.text_delta.emit(chunk["delta"])
                    elif kind == "tool_calls":
                        pending_calls.extend(chunk.get("calls", []))
                    elif kind == "done":
                        finish_reason = chunk.get("finish_reason")
                        reasoning = chunk.get("reasoning_content", "")
                        u = chunk.get("usage")
                        if isinstance(u, dict):
                            self.usage.emit(u)
                    elif kind == "error":
                        self.failed.emit(str(chunk.get("error")))
                        return

                if finish_reason != "tool_calls" and not pending_calls:
                    break

                asst = {
                    "role": "assistant", "content": None,
                    "tool_calls": [
                        {"id": f"call_{i}", "type": "function",
                         "function": {"name": c.get("name"),
                                      "arguments": json.dumps(c.get("arguments", {}))}}
                        for i, c in enumerate(pending_calls)
                    ],
                }
                if reasoning:
                    asst["reasoning_content"] = reasoning
                messages.append(asst)

                for i, c in enumerate(pending_calls):
                    name, arguments = c.get("name"), c.get("arguments", {})
                    self.tool_call.emit({"name": name, "arguments": arguments, "status": "pending"})

                    authorized, why = self._authorized(name)
                    result = self._exec(name, arguments, mode, authorized, why)
                    self._history_tool = result
                    messages.append({"role": "tool", "tool_call_id": f"call_{i}",
                                     "content": str(result)})

            self._history = messages[1:]  # drop the system message
            self.finished.emit(self._last_text)
        except Exception as exc:  # noqa: BLE001
            self.failed.emit(f"{type(exc).__name__}: {exc}")

    def _exec(self, name, arguments, mode, authorized, why):
        """Run (or skip) one tool call and return its result text."""
        if not authorized:
            self.tool_call.emit({"name": name, "arguments": arguments,
                                 "status": "rejected", "result": why})
            return f"skipped: {why}"

        if mode == P.MODE_DRY:
            self.tool_call.emit({"name": name, "arguments": arguments,
                                 "status": "dry", "result": "(dry-run)"})
            return "(dry-run - not executed)"

        if mode == P.MODE_APPROVE:
            token = uuid.uuid4().hex
            ev = threading.Event()
            with self._lock:
                self._approvals[token] = {"event": ev, "approved": None}
            self.approval.emit({"token": token, "name": name, "arguments": arguments})
            ev.wait()
            with self._lock:
                approved = (self._approvals.pop(token, None) or {}).get("approved")
            if not approved:
                self.tool_call.emit({"name": name, "arguments": arguments,
                                     "status": "rejected", "result": "user rejected the call"})
                return "user rejected the call"

        self.tool_call.emit({"name": name, "arguments": arguments, "status": "running"})
        try:
            data = R.call(name, arguments)
            text = json.dumps(data, default=str)
            self.tool_call.emit({"name": name, "arguments": arguments,
                                 "status": "ran", "result": text})
            return text
        except Exception as exc:  # noqa: BLE001
            self.tool_call.emit({"name": name, "arguments": arguments,
                                 "status": "error", "result": str(exc)})
            return f"error: {exc}"

    @staticmethod
    def _openai_tools(schemas):
        out = []
        for t in schemas:
            out.append({"type": "function",
                        "function": {"name": t["name"],
                                     "description": t.get("description", ""),
                                     "parameters": t.get("inputSchema", {})}})
        return out
