# VULKAN ↔ GL DECOUPLING PLAN (TEMPORARY)

> **TEMPORARY working document.** Delete before the branch is proposed for
> merge. It exists only to coordinate the refactor; it is not user
> documentation.

Branch: `feat/vulkan-decouple-gl`.
Base: **`feat/vulkan-present-mode` @ `6cd24cd6c9`** (the integration branch).

## What changed since this plan was first written

The plan was originally scoped against `pr/freecad-tooling` @ `3773f50a66`.
Since then the integration branch advanced and three PRs merged into
`feat/vulkan-present-mode`:

- **#94 Tooling** — `tools/fcprobe` + the MCP server moved out of tree to
  `FreeCAD-DevTools`; CI now checks that repo out at `devtools/`.
- **#96 Gui/Vulkan: shared interaction** — landed the
  `InteractionController` / `InteractionHost` / `InteractionSurface` seam, the
  NaviCube / overlay / navigation / view-settings consolidation, **and**
  retargeted the sources at the reorganized Coin header paths. Notably
  `InteractionHost` no longer exposes `getSoRenderManager()` and `Navigation/*`
  no longer reaches the render manager, so **most of step 7 is already on the
  base**.
- **#97 Material** — physical/PBR material + embedded texture mapping.

CI fixes that also landed on the base: the `runPythonTests` report-dir fix, the
Coin MSVC `C2129` fix (`SoVulkanRenderManagerP.h`), and the material branch's
`Inventor/rendering/vulkan/...` include-path fix. Coin is pinned at
**`df50c0363`**; its Vulkan headers now live under
`include/Inventor/rendering/vulkan/` (`SoVulkanViewMode.h`,
`SoVulkanViewSettings.h`, `SoVulkanRenderManager.h`, `SoVulkanRenderTarget.h`,
`SoVulkanImageCopy.h`).

## Goal

Stop the Vulkan viewport being a parasite on the hidden OpenGL
`View3DInventorViewer`. One surface- and backend-agnostic **view model**
(`Gui::ViewState`) owns the scene roots, camera, viewport+DPR, picking, events,
lights and overlay subtrees; GL and Vulkan become pure consumers that observe
it. The endgame is a Vulkan-only view that never constructs a GL
`SoRenderManager`/`View3DInventorViewer`.

## Principle

`InteractionHost` / `InteractionSurface` (`src/Gui/InteractionHost.h`,
`InteractionSurface.h`) is the seam and is already on the base. The remaining
work is to give the state it hands out a neutral owner and delete the last GL
reads.

## Current state on this branch

**Steps 1, 2, 6 are applied; `make -j$(nproc) FreeCADGui` is green.** They are
staged, not yet committed, and **not yet runtime-verified**. Changes:

- New `src/Gui/ViewState.{h,cpp}`: neutral owner of scene root, camera, object
  group, foreground root, per-frame decoration root, viewport region + DPR,
  with change callbacks.
- `View3DInventorViewer` owns a `ViewState`, feeds the *same* scene-root pointer
  into its GL `SoRenderManager`, and returns scene/camera/viewport from the
  `ViewState` (`getViewState()` exposed). `setGroundPlaneDecorationScene()` is
  deleted; the grid is parented on `ViewState::decorationRoot()`.
- `VulkanViewportAdapter` sources scene/camera from `ViewState` (not
  `rm->getSceneGraph()/getCamera()`); `updateViewportAuthority()` writes the
  surface size/DPR into `ViewState` + the controller and **no longer writes the
  GL render-manager region**; `applySurfaceViewportToGL()` and the GL-widget
  event filter are gone.

The work was authored **before #96** and re-applied onto the current base. The
only conflicts were in `VulkanViewportAdapter.{h,cpp}` (resolved: kept #96's
`InteractionSurface` doc comment + added the ViewState-authority sentence and
dropped the GL render-manager write; kept `_surfaceViewport` as the reported
mirror).

### Residual couplings still present
- `VulkanViewportAdapter.cpp:445` reads `rm->getBackgroundColor()` in
  `pushSettings()` — **step 5**.
- `pushSceneLights()` gathers the viewer lights and pushes them — **step 5**.
- `_vulkanViewer->setRawEventTarget(_viewer->getWidget())` (`:150`) still relays
  tablet/touch/context-menu to the hidden GL widget — **step 4**.
- Picking/selection still run through the viewer's scene graph, not a neutral
  pick service — **step 3**.
- Coin `SoVulkanRenderManager` still hand-mirrors the GL scene-manager API —
  **step 8**.
- Step 6 deviation: the ground grid is on `ViewState::decorationRoot()`, but the
  axis-cross overlay still physically lives in the viewer's IR decoration
  wrapper (`getDecorationRoot()` composes them for Vulkan).

## Baseline / environment (this box)

- Debug build: `build/debug` (Unix Makefiles, `FREECAD_USE_VULKAN=ON`), binary
  `build/debug/bin/FreeCAD`. Non-Vulkan: `build/release` (ninja).
- Single-TU compile: parse `build/debug/compile_commands.json` and run the
  entry's `command`.
- Full link: `cd build/debug && make -j$(nproc) FreeCADGui` (then `FreeCAD`).
  Adding `ViewState.cpp` to `CMakeLists.txt` makes `make` re-run cmake.
- Never pipe `make` through `head`/`tail` (SIGPIPE); redirect to a log.
- Test harness is **out of tree**:
  `/home/phantom/dev/FreeCAD-DevTools/fcprobe/freecad_probe.py`; probes are
  `vk_*_probe.py` beside it. Example:
  `python3 /home/phantom/dev/FreeCAD-DevTools/fcprobe/freecad_probe.py run <probe.py> --binary build/debug/bin/FreeCAD --profile vulkan`.
- Qt env for probes: `QT_STYLE_OVERRIDE=fusion QT_QPA_PLATFORM=wayland`, plus
  `FC_SKIP_UNSAVED_PROMPT=1`.
- **Coin/pivy ABI trap:** a Coin header change or submodule-pin change requires
  rebuilding all consumers. A stale `Mod/pivy/_coin.so` / `PartGui.so` shows up
  as `undefined symbol` during probes. Run a full `make -j$(nproc)` (or at least
  the affected modules) before probing — this blocked the first WS-CORE run.

## Steps

Status legend: `[x]` committed + verified; `[~]` implemented, final
verification pending; `[ ]` not started.

| # | Step | Owns (files) | Status |
|---|------|--------------|--------|
| 1 | **Invert viewport authority**: one neutral `viewportRegion`+`dpr`; delete `applySurfaceViewportToGL` + GL-widget event filter. | `ViewState.*`, `View3DInventorViewer.*`, `VulkanViewportAdapter.*` | `[x]` `194c59a6d5` |
| 2 | **Lift scene+camera off `SoRenderManager`**: neutral owner holds scene root + camera; hosts return those; GL manager references them. | `ViewState.*`, `View3DInventorViewer.*`, `VulkanViewportAdapter.*` | `[x]` `194c59a6d5` |
| 3 | **Picking as a service**: neutral `pick(ray)` (scene+cam+viewport); selection calls it; Vulkan picks its own pixels. | `ViewState.*` + call sites | `[x]` `23fd7b4397` |
| 4 | **Own events in the controller**: event manager + gesture devices bound to the current surface; drop the hidden-GL relay (`setRawEventTarget`). | `InteractionController.*`, `InteractionSurface.h`, `View3DInventorViewer.*` | `[~]` `e7a82a54ab`; devices owned per surface, tablet/touch relay + gesture grab left |
| 5 | **Lights + background as view-model state**: camera-anchored headlight/backlight/fill and background on the view model; delete the GL gather (`pushSceneLights`, `getBackgroundColor`). | `ViewState.*`, `VulkanViewportAdapter.*`, `View3DInventor.*` | `[x]` `23fd7b4397` |
| 6 | **Overlays as scene subtrees**: navi-cube/axis-cross/ground-grid roots on the view model; drop `setGroundPlaneDecorationScene`. | `ViewState.*`, `View3DInventorViewer.*`, `VulkanViewportAdapter.*` | `[x]` `194c59a6d5` (axis-cross still in IR wrapper) |
| 7 | **Drop `getSoRenderManager()` from the neutral interface.** | `InteractionHost.h`, `Navigation/*` | `[~]` done by #96; only GL-only consumers keep it |
| 8 | **Coin: backend-agnostic scene manager**: common base for `SoRenderManager` + `SoVulkanRenderManager`. | `src/3rdParty/coin/**` | `[ ]` patch ready: coin `21aa55b32`, not integrated |
| 9 | **Endgame**: Vulkan-only view builds controller+view model without `View3DInventorViewer`/`SoRenderManager`. | `View3DInventor.*`, `VulkanViewportAdapter.*` | `[ ]` design in `/tmp/opencode/step9_design.md` |

## Workstream split

- **WS-CORE (steps 1,2,6)** — *done*: committed `194c59a6d5`, verified
  (full relink green; Vulkan raster+RT pick PASS; GL presel `(44,99,144)`).
- **WS-INTERACT (steps 3,4,5)** — *mostly done*: steps 3+5 committed
  `23fd7b4397`; the step-4 device-ownership change is `e7a82a54ab` (syntax-clean,
  runtime verification pending). Step 4's tablet/touch relay and gesture grab
  move to WS-CLEANUP.
- **WS-COIN (step 8)** — independent; owns `src/3rdParty/coin/**`. Can run in
  parallel with WS-INTERACT (disjoint tree) once build configuration is frozen.
- **WS-CLEANUP (step 9)** — after WS-INTERACT: Vulkan-only session path (step 7
  is largely satisfied already).
- **WS-VERIFY** — probe suite + parity after each stage.

Steps 1–6 all funnel through `View3DInventorViewer.*` +
`VulkanViewportAdapter.*`, so they are serialized within the FreeCAD workstream;
step 8 is the only genuinely parallel one.

## Verification gates

1. Per change: single-TU compile of every touched `.cpp` (rc=0).
2. Per stage: `make -j$(nproc) FreeCADGui` (debug) green.
3. Before probing after a Coin/header/pin change: full `make -j$(nproc)` so
   `pivy`/module consumers relink (avoid the `undefined symbol` trap).
4. End: non-Vulkan `build/release` `ninja FreeCADGui` green.
5. Probes (from `FreeCAD-DevTools/fcprobe`): Vulkan raster pick + path-tracing
   pick + GL preselection parity; the `vk_*` regression family.

## Status

- `[x]` Step 1 · `[x]` Step 2 · `[x]` Step 3 · `[~]` Step 4 · `[x]` Step 5
- `[x]` Step 6 · `[~]` Step 7 · `[ ]` Step 8 · `[ ]` Step 9

Progress: `194c59a6d5` (1/2/6) → `23fd7b4397` (3/5) → `e7a82a54ab` (4, partial).

Next actions:

1. Once the concurrent session's benchmark finishes, run a full relink + the
   pick/presel probes to verify the step-4 device-ownership change.
2. Integrate WS-COIN step 8 (`21aa55b32178f90a5deac14b0953946ba92dc9be`, patch
   `/tmp/opencode/coin-step8.patch`): land it on the coin fork, bump the
   `src/3rdParty/coin` gitlink, full relink, GL/Vulkan parity probes.
3. WS-CLEANUP step 9 (design in `/tmp/opencode/step9_design.md`): the Vulkan-only
   session; also retire the step-4 tablet/touch relay and gesture grab.
4. Delete this document before the branch is proposed for merge.
