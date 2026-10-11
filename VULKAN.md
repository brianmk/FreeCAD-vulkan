# Vulkan viewport (experimental)

FreeCAD's bundled Coin carries a retained-IR **raster Vulkan** renderer, driven by the fork's
`QuarterVulkanWidget`. It is off by default; the OpenGL path stays the default and the fully
supported fallback.

This file documents the build switch, the preferences, and every debug/tuning environment
variable the viewport reads. If you add a knob, add it here (the earlier state of this codebase had
33 readable flags documented nowhere).

## Build

```sh
cmake -S . -B build -DFREECAD_USE_VULKAN=ON
```

- Requires the **bundled** Coin/Pivy (`FREECAD_USE_EXTERNAL_COIN_PIVY` must be OFF — configure
  fails otherwise) plus Vulkan headers, the Vulkan Memory Allocator and `glslangValidator` (the
  SPIR-V build step hard-fails without it). On pixi/conda, `libvulkan-headers` and
  `vulkan-memory-allocator` are declared for `linux-64`/`win-64`; the other platforms need them
  added.
- `-DFREECAD_USE_VULKAN_DEBUG_HOOKS=ON` additionally builds the test hooks (synthetic mouse
  injector, presented-frame capture). It is auto-enabled for `Debug` builds.

## Preferences (`BaseApp/Preferences/View`)

| key | type | meaning |
|---|---|---|
| `UseVulkanRenderer` | bool (false) | a *new* 3D view opens on the Vulkan page |
| `VulkanRenderMode` | int (1) | 0 = Interactive (raster Coin/GL), 1 = Interactive (raster Vulkan), 2 = Wireframe; owned by `View3DInventor`, mirrored by the status-bar selector |
| `VulkanWireframe` | bool (true) | model B-Rep feature-edge overlay |
| `VulkanShowPoints` | bool (false) | point-marker overlay |
| `VulkanInteractionLod` | bool (true) | reduce geometric detail while navigating |
| `VulkanEdgeColor` | unsigned 0xRRGGBBAA | edge-overlay colour; defaults to `DefaultShapeLineColor` |

## Environment variables

Set to `1` to enable (unless noted). `0`, `false` and `off` disable — except the couple of raw
`getenv` reads called out below, which only test presence.

### Viewport / widget (`FC_*`)

| variable | effect |
|---|---|
| `FC_VULKAN_VALIDATION` | request the `VK_LAYER_KHRONOS_validation` instance layer |
| `FC_VULKAN_PERSISTENT_RESOURCES` | keep backend resources alive across frames (default on; set `0` to opt out) |
| `FC_VULKAN_LOD_TRACE` | log the interaction-LOD gate decisions (pointer/camera pose) |
| `FC_VULKAN_INJECT_PY=FILE` | *(debug hooks)* poll `FILE` for `move/press/release x y` lines and post real platform mouse events to the embedded window |
| `FC_VULKAN_DUMP_FRAME` | *(debug hooks)* write the presented frame to `/tmp/vk_frame_<n>.png`, the naming the fcprobe harness collects |

### Renderer diagnostics and tuning (`COIN_VULKAN_*`)

| variable | effect |
|---|---|
| `COIN_VULKAN_BACKEND_DEBUG` | per-frame backend diagnostics (`[RTDBG]`, `[SEC]`, draw-list counters, the restamp/annotation invariants) |
| `COIN_VULKAN_TRACE` | verbose backend record tracing |
| `COIN_VULKAN_FRAME_TIMING` | wall-clock frame timing (`[RTDBG] cpuTiming`) |
| `COIN_VULKAN_GPU_TIMING` | per-pass GPU timestamps |
| `COIN_VULKAN_DEBUG_UTILS` | `VK_EXT_debug_utils` object names / command-buffer labels |
| `COIN_VULKAN_DEBUG_PRINTF` | `VK_EXT_debug_printf` shader-side diagnostics (read raw: `=0` still requests it) |
| `COIN_VULKAN_PIPELINE_FEEDBACK` | `VK_EXT_pipeline_creation_feedback` (pipeline-cache hits / creation cost) |
| `COIN_VULKAN_IR_REPLAY` | retained-IR replay; `0` forces a full re-traversal every frame |
| `COIN_VULKAN_RP_CLEAR` | control the render-pass attachment clear |
| `COIN_VULKAN_PARALLEL_RECORD` | record secondary command buffers in parallel |
| `COIN_VULKAN_RECORD_WORKERS` | worker count for the above |
| `COIN_VULKAN_EXTERNAL_SECONDARY` | external/secondary-buffer integration (RT composite path) |
| `COIN_VULKAN_RASTER_DECAL` | scale `SoPolygonOffset` units for float depth (overlay decals) |
| `COIN_VULKAN_WIREFRAME` | force the wireframe overlay |
| `COIN_VULKAN_POINTS` | force the point overlay |
| `COIN_VULKAN_TESS` | force the tessellation overlay |
| `COIN_VULKAN_EDGE_COLOR` | force the uniform B-Rep edge colour |
| `COIN_VULKAN_WLINE_CPU` | expand wide lines on the CPU instead of the GPU-instanced path |
| `COIN_VULKAN_WLINE_SERIAL` | serialise the wide-line expansion |
| `COIN_VULKAN_GEOM_LOD` | enable the geometric-LOD pre-pass |
| `COIN_VULKAN_GEOM_LOD_ALWAYS` | engage LOD regardless of motion |
| `COIN_VULKAN_GEOM_LOD_STATS` | log LOD statistics |
| `COIN_VULKAN_GEOM_LOD_PIXELS` | sub-pixel threshold for the LOD cull |
| `COIN_VULKAN_GEOM_LOD_MAX_INDEX` | maximum index count eligible for LOD |
| `COIN_VULKAN_GEOM_LOD_MIN_PRIMS` | minimum primitive count eligible for LOD |

Adjacent, likewise undocumented before: `FC_RASTER_OVERLAY_OFF` (the unregistered
`SoRasterOverlay` node), `FC_LIGHT_TRACE` (light diagnosis in the adapter),
`FC_SKIP_UNSAVED_PROMPT` (close-without-saving prompt).

## Tests

- Renderer/UI probes live in the **FreeCAD-DevTools** repository (`fcprobe/`), not here. The raster
  regression suite is `fcprobe/vk_ci_raster_suite.json`; the ray-tracing/feature suite is
  `fcprobe/vk_suite.json`. Run one with:

  ```sh
  HOME=<scratch> python3 fcprobe/freecad_probe.py run fcprobe/vk_annotation_restamp_probe.py \
      --binary <build>/bin/FreeCAD --profile vulkan --out <dir> --name <name> \
      --env COIN_VULKAN_BACKEND_DEBUG=1
  ```

  A probe's assertions live in the sibling `<probe>.check.py`.
- In-tree, `tests/perf/` measures pick/navigation frames against the GL and upstream builds, and
  `src/Mod/Test/TestCoinNodeSnapshots.py` compares offscreen node renders against the baselines in
  `tests/visual/baselines/coin-nodes/` (GL only today).
