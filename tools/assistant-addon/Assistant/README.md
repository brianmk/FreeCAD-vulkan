# FreeCAD Assistant

A VSCode-style **AI chat dock** for FreeCAD that can drive **any FreeCAD tool** through
LLM tool-calling. It ships as a self-contained addon (no FreeCAD core changes) and talks
to an OpenAI-compatible endpoint (DeepSeek by default, also works with OpenRouter,
Ollama, LM Studio, vLLM, ...).

```
 ┌───────────────────────────────┐        OpenAI-compatible chat/completions + SSE
 │  ChatPanel (QDockWidget)      │  ─────────────────────────────────────────────►  LLM
 │   ├─ messages (Markdown)      │  ◄── tool_calls / streamed text ─────────────
 │   ├─ tool-call cards          │
 │   ├─ Approve/Reject per tool  │
 │   └─ Settings dialog          │
 └───────────────────────────────┘
        │ Agent (worker thread)         in-process, no MCP server/socket
        ▼
   ToolRegistry ────────────────────►  vendored freecad_mcp_guest.HANDLERS
        │                               (executes on FreeCAD's main thread)
        ▼
   any FreeCAD tool: sketches, pad/pocket, fasteners, parameters, run_python, ...
```

## What it can do
- Stream replies as Markdown, with tool-call cards (name, args, result, status).
- **Approve each tool call** by default (VSCode-style), or auto-run / dry-run.
- Execute all of the 70 MCP tools in-process (the same registry that feeds the
  `freecad_mcp_server`) plus `run_python` as a general escape hatch — so it really
  can use *any* tool in FreeCAD.
- Auto-injects scene context (active doc, object inventory, selection, workbench,
  `Params` spreadsheet) into the prompt.
- Shows context-aware quick-start links: **New document** when nothing is open,
  and **Create a sketch** when the active document has no objects. Clicking a
  link runs the corresponding tool in-place.
- Persists preferences (endpoint/model/key/mode) in a FreeCAD parameter group.
- Tool execution is marshalled to FreeCAD's **main thread** (safe against the
  viewport/scene threading rules).

## Sketch validation & constraint suggestions
The agent can validate a sketch's constraint state and propose what to add:
- **`validate_sketch(sketch)`** — solver diagnostics: `DoF` (degrees of freedom
  remaining), `FullyConstrained`, `StatusString`, redundant / conflicting /
  malformed constraint indices, a per-constraint breakdown, and a geometry
  outline (endpoints / centers / radii).
- **`suggest_constraints(sketch, mode='analyze'|'apply')`** — reasons about the
  remaining DoF + geometry type (lines → DistanceX/Y + Horizontal/Vertical,
  circles → Radius/Diameter + position), reports FreeCAD's own missing-constraint
  hints, and flags redundant/conflicting constraints to remove. `mode='apply'`
  runs FreeCAD's `autoconstraint()`.

Typical agent flow: `validate_sketch` → read the solver state → propose
`add_constraint(...)` calls (approval-gated) → re-run `validate_sketch` to confirm
`FullyConstrained=True`.

## Install (dev)
```bash
scripts/install.sh            # symlink (needs a real dir for discovery: use --copy)
scripts/install.sh --copy     # copy a frozen snapshot
```
The installer resolves FreeCAD's *versioned* user data dir (e.g.
`~/.local/share/FreeCAD/v26-3/Mod`) via `FreeCADCmd`, then registers the addon in
that dir's `Mod/manifest.json` when needed. Restart FreeCAD and pick the **Assistant**
workbench (or run the `Assistant_Toggle` command).

> Note: FreeCAD 1.1+ only loads user addons whose directory is a real directory
> (symlinks are ignored) AND which appear in `Mod/manifest.json`. `install.sh --copy`
> handles both for you.

## First run
1. Open the Assistant dock (auto-appears; or run `Assistant_Toggle`).
2. Open **Settings** and set your **API key** (or set `DEEPSEEK_API_KEY` in the env),
   the endpoint (`https://api.deepseek.com`), the text model (`deepseek-v4-flash`) and the
   **vision model** (`deepseek-v4-flash-vision-exp`) you use for screenshots.
3. Ask something, e.g. *"make a 40x38 plate with 4 corner holes, then check clearance"*.
   Approve the tool calls as they appear.

## Reopening the chat (if you close it)
The dock is docked by default under the **Tasks** panel (right side) and is locked
(not floatable / draggable). To bring it back after closing:
- **Shortcut**: `Ctrl+Shift+A` (works in any workbench; rebindable in
  Edit → Preferences → General → Shortcuts).
- **Command**: switch to the **Assistant** workbench toolbar/menu → *Assistant Chat*.
- **Python console**: `FreeCADGui.runCommand('Assistant_Toggle')`.

## Vision
Tick the **View** checkbox next to Send, or enable *"Attach viewport snapshot"* in
Settings, then send a message. The addon captures the current 3D viewport as a PNG and
sends it as an `image_url` part (base64 data URL) to the configured **vision model**,
with the prompt alongside. E.g. *"what shape is in the view, and are any bolts
clipping?"* The message is built as:
```
{"role":"user","content":[{"type":"text","text":...},{"type":"image_url","image_url":{...}}]}
```
No view available → the agent tells you and sends text-only.

## Layout
```
Assistant/
├── Init.py / InitGui.py     # exec-safe shims (must not use __file__)
├── init_impl.py             # workbench + command definitions
├── ChatPanel.py             # dock UI (messages, tool cards, approval, settings)
├── Agent.py                 # LLM + tool-calling loop, approval gating
├── Provider.py              # OpenAI-compatible streaming client (requests)
├── ToolRegistry.py          # in-process dispatch + main-thread marshalling
├── Context.py               # scene context for the system prompt
├── Preferences.py           # ParameterGrp-backed settings
├── vendor/                  # bundled registry + tool schemas (see scripts/)
│   ├── freecad_mcp_guest.py
│   └── tool_schemas.json
├── Resources/               # icons
└── package.xml
scripts/
├── install.sh               # install / register the addon
├── sync_guest.py            # re-vendor tools/fcprobe/mcp/freecad_mcp_guest.py
└── gen_tool_schemas.py      # regenerate vendor/tool_schemas.json from the MCP server
```

## Keeping the vendored tools in sync
When the MCP tool surface at `tools/fcprobe/mcp` changes:
```bash
python3 scripts/sync_guest.py
/tmp/opencode/mcp-venv/bin/python scripts/gen_tool_schemas.py   # needs the mcp pkg
```
Then reinstall with `--copy`.

## Tests
- Headless probe: `tests/assistant_probe.py` (drives the panel with a mock LLM).
- A live E2E checklist is in the probe.

## Notes / limitations
- Requires FreeCAD's bundled Python to have `requests` (present via AddonManager).
- `mcp` package is **not** needed inside FreeCAD; tool schemas are generated offline.
- Vision uses the configured **vision model** (default `deepseek-v4-flash-vision-exp`); it should support
  image parts (OpenAI-compatible `image_url`) if tool-calling on the same turn matters.
- Session history is kept in-memory for the session.
