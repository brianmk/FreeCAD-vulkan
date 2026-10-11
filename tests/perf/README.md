# Three-arm performance suite (`base` / `gl` / `vk`)

Runs the same workloads against the three standard arms and renders a
comparison graph, so a fork change can be judged against the real upstream
baseline (not only the fork's GL-vs-Vulkan):

| arm    | binary (default) | renderer |
|--------|------------------|----------|
| `base` | `$FCP_BASE_BIN` (no default; point it at an upstream FreeCAD) | upstream FreeCAD, **no** Vulkan (system Coin / GL) |
| `gl`   | `$FCP_GL_BUILD` / `../build-freecad-off` | fork, `FREECAD_USE_VULKAN=OFF` |
| `vk`   | `$FCP_VK_BUILD` / `../build-freecad` | fork, Vulkan raster |

The defaults are relative to `$FCP_REPO` (this checkout, by default), so the suite works from any
clone once the three builds exist; override `FCP_BASE_BIN`, `FCP_GL_BUILD` and `FCP_VK_BUILD` for
other layouts.

`base` has no fork-only API, so the *same* `probe.py` runs on all three arms
with everything fork-specific guarded by `hasattr` (`setRenderMode`,
`requestVulkanRender`, `getVulkanFrameCount`). The fork-only C++ microbenchmarks
(`PartPickBench`, `ImportBench`) are additionally run on the `gl`/`vk` arms and
shown as separate graph panels.

## Usage

```bash
# full run (defaults: 3 arms, step import + pick + box orbit/idle, 30 samples)
python3 tests/perf/run_suite.py

# fast smoke
python3 tests/perf/run_suite.py --quick

# pick arms/works/samples
python3 tests/perf/run_suite.py --arms base,vk --works box:orbit --samples 50
python3 tests/perf/run_suite.py --no-benches

# (re)render the graph from an existing results.json
python3 tests/perf/graph.py tests/perf/results/results.json tests/perf/results/graph.png
```

Work spec is `scene:work`:

- scenes: `box` (a `Part::Box`), `step` (opens `data/examples/Schenkel.stp`,
  override with `FCP_STEP`).
- works: `open` (scene-build/import wall time), `idle` (static frame),
  `orbit` (moving-camera frame), `pick` (`getObjectInfo` over a screen grid).

## Outputs (`--out`, default `tests/perf/results/`)

- `results.json` — all cases + fork bench numbers.
- `summary.md` — one table, metric per row, one column per arm.
- `graph.png` — 2x3 grouped bars (import, orbit, idle, pick, pick bench,
  import bench).
- `logs/` — per-case FreeCAD stdout/stderr; `raw/` — per-case probe JSON.

## Notes

- The C++ benches are best-of-3 and run **before** the GUI cases; the
  short culled-pick measurement is otherwise distorted by GUI teardown load.
- Frame timing is wall-clock from the probe. The `[RTDBG]` timing hooks are runtime-gated by
  `COIN_VULKAN_FRAME_TIMING` (not compiled out in Release), so a timing run must set it. `vk`
  forces a frame via
  `requestVulkanRender()`+`getVulkanFrameCount()`; `base`/`gl` use
  `redraw()`+`graphicsView().repaint()`.
- Each arm gets its own `FREECAD_USER_HOME` under `results/` so user config and
  the STEP import cache do not leak between arms.
- Wayland caps present to the refresh rate; runs use `QT_QPA_PLATFORM=xcb`.
