# Vulkan raster vs base OpenGL — scene throughput

Controlled comparison of the fork's **Vulkan raster** viewport against a pristine
**upstream OpenGL** build. Reproducible with the committed harness under
[`tools/perf/vulkan_vs_base/`](../../tools/perf/vulkan_vs_base/).

> **Read the caveat first.** The Vulkan side is measured with V-Sync disabled
> (present mode `Immediate`) on xcb, the GL side on its default pacing, so the
> ratios overstate pure rasterizer throughput (see [Caveats](#caveats)). The
> numbers are for the two backends on one box, not an apples-to-apples GPU
> benchmark.

## Test

The same in-FreeCAD probe (`scene_bench_probe_io.py`) runs under both binaries:
it builds a deterministic scene, then times `FC_BENCH_FRAMES` forced frames
after `FC_BENCH_WARMUP` warm-up frames and reports `[HARNESS] scene_bench` fps.
The camera is **static** (no orbit), so the metric is the steady per-frame cost
of the scene as built, not navigation.

| | Value |
| --- | --- |
| Workloads | `empty`, `sketches` x200, `objects` x1000 / 5000 / 15000 |
| Viewport | 1600x900 |
| Frames / warm-up | 40 / 8 |
| Platform / present | xcb; Vulkan `VulkanPresentMode=2` (Immediate), GL default |
| GPU | NVIDIA RTX 5090, driver 615.71.09 |
| CPU | 32 cores |

Backends: baseline is upstream OpenGL (`RasterCoin`, `VulkanRenderMode=0`); fork
is `RasterVulkan` (`VulkanRenderMode=1`).

### Builds

| | Baseline | Fork |
| --- | --- | --- |
| Source | `FreeCAD_base/FreeCAD` @ `acdce56122` (upstream) | this tree @ `fix/vulkan-geomlod-min-prims` |
| Build | `build/release`, Release, Vulkan **OFF** | `build/release-vulkan`, Release, Vulkan **ON** |
| Bundled Coin | — | `593350efc` (camera cache, fingerprint skip, same-root guard, scoped pre-pass fence, small-command LOD gate) |

## Results

Frames per second (higher is better). Base GL and Vulkan raster only.

| Workload | base GL | Vulkan raster | Vulkan vs base |
| --- | ---: | ---: | ---: |
| empty | 49.8 | **388.6** | **7.8x** |
| sketches 200 | 43.8 | **382.7** | **8.7x** |
| objects 1000 | 22.7 | **358.7** | **15.8x** |
| objects 5000 | 7.2 | **200.3** | **27.8x** |
| objects 15000 | 2.5 | **100.8** | **41.0x** |

Against the previous fork build (`PERF_COMPARISON.md`), the small scenes are
unchanged but the heavy scene improves sharply — the retained-IR
graph-fingerprint walk no longer runs every frame:

| Workload | prior Vulkan | this Vulkan | delta |
| --- | ---: | ---: | ---: |
| objects 1000 | 312.2 | 358.7 | +15% |
| objects 5000 | 211.9 | 200.3 | ~noise |
| objects 15000 | 6.10 | 100.8 | **16.5x** |

The base-GL figures are stable run-to-run, so the heavy-scene jump is the
Vulkan-side manager work (`setSceneGraph` same-root guard + fingerprint skip),
not a machine change.

## How to reproduce

```sh
# 1. Baseline: upstream checkout, Release, Vulkan OFF.
#    (configure BUILD_TYPE=Release with FREECAD_USE_VULKAN=OFF)
cd FreeCAD_base/FreeCAD && ninja -C build/release

# 2. Fork: this tree, Release, Vulkan ON. Rebuilds bundled Coin with the
#    renderer changes (the pixi clang toolchain is recorded in the build dir).
ninja -C build/release-vulkan -j"$(nproc)"

# 3. Run the harness (defaults to the two builds above).
python3 tools/perf/vulkan_vs_base/compare_scene_xcb.py

#    Or point it at other builds:
PC_BASE_BIN=/path/to/base/bin/FreeCAD \
PC_FORK_BIN=/path/to/fork/bin/FreeCAD \
  python3 tools/perf/vulkan_vs_base/compare_scene_xcb.py

# Result JSON + per-config logs:
#   tools/perf/vulkan_vs_base/results/scene_results_novsync.json
#   tools/perf/vulkan_vs_base/results/<config>.log
```

To run a single workload, pass its config name(s), e.g.:

```sh
python3 tools/perf/vulkan_vs_base/compare_scene_xcb.py \
    base_gl_x_objects15000 fork_vk_x_objects15000
```

Knobs: `PC_FRAMES`, `PC_WARMUP`, `PC_VIEWPORT` (host), and `FC_BENCH_*` (set by
the harness). See `tools/perf/vulkan_vs_base/README.md`.

## Caveats

1. **Mismatched present pacing.** Vulkan runs on xcb with
   `VulkanPresentMode=2` (`VK_PRESENT_MODE_IMMEDIATE_KHR`, V-Sync off); base GL
   is on its default pacing. Part of the gap is present pacing, not render
   throughput.
2. **Frame metric.** The probe counts frames the engine is willing to produce,
   not completed presented frames; an uncapped swapchain inflates the Vulkan
   side.
3. **Static camera.** The scenes do not orbit, so these numbers do not capture
   interaction-time costs (camera resolution, the geometry-LOD pre-pass) — those
   are covered in [`navigation-frame-fix.md`](navigation-frame-fix.md).
4. **Single box / single GPU.** No repeat across drivers or hardware.

For an apples-to-apples rerun, measure both sides under matched present pacing
or use GPU-timestamp/offscreen timing, and repeat with
`FC_BENCH_MOTION=orbit` for the interaction case.
