# Vulkan-vs-base benchmarks

Scripts used to produce [`PERF_COMPARISON.md`](../../../PERF_COMPARISON.md) and
the charts in `docs/vulkan-perf/`. They drive the same in-FreeCAD scene probe
under the *baseline* (upstream `FreeCAD_base`) and the *fork* binaries so the
two can be compared.

> **The reported numbers are unverified and not apples-to-apples.** See
> [Caveats](#caveats) below before quoting them.

## Layout

| File | Role |
| --- | --- |
| `compare_scene.py` | scene-throughput table (Wayland/FIFO reference run) |
| `compare_scene_xcb.py` | scene-throughput table on **xcb** with Vulkan `Immediate` |
| `bench_extra.py` | vorontest open/render, rotating camera, ray/path-trace modes |
| `bench_open.py` | startup + cube-open timing (median of `OB_REPS`) |
| `make_graphs.py` | renders the PNGs into `docs/vulkan-perf/` |
| `scene_bench_probe_io.py` | guest probe: builds the scene, reports `[HARNESS]` fps records |
| `open_bench_probe.py` | guest probe: startup / document-open timing |
| `record_probe.py`, `record_houses_probe.py`, `record_gifs.py`, `record_houses_gifs.py`, `relabel_gifs.py` | GIF capture for the demo clips |
| `novsync_layer/` | logging Vulkan layer used to confirm the present mode reaches the driver |
| `_paths.py` | shared path resolution (binaries + probe/result locations) |

## Prerequisites

- The `pixi` `default` environment (clang++, mold, Ninja) or an equivalent
  toolchain.
- Two builds:
  - baseline: upstream checkout at `FreeCAD_base/FreeCAD`, `build/release`,
    Vulkan **off**;
  - fork: this tree, `build/release-vulkan`, Vulkan **on**.
- A NVIDIA GPU and a display (the reported runs were 1600x900 on an RTX 5090).

## Running

Binaries default to the sibling paths above; override them for another layout:

```sh
export PC_BASE_BIN=/path/to/base/bin/FreeCAD
export PC_FORK_BIN=/path/to/fork/bin/FreeCAD
export PC_VORON=/path/to/vorontest.FCStd      # bench_extra.py only
# optional knobs: PC_FRAMES, PC_WARMUP, PC_VIEWPORT, OB_REPS

python3 tools/perf/vulkan_vs_base/compare_scene_xcb.py
python3 tools/perf/vulkan_vs_base/bench_extra.py
python3 tools/perf/vulkan_vs_base/bench_open.py
python3 tools/perf/vulkan_vs_base/make_graphs.py
```

Each driver writes JSON + printable tables under `results*/` next to itself and
loads the guest probe with `FreeCAD --no-focus`. The `[HARNESS]` lines are
written to `sys.__stdout__` because FreeCAD redirects `print()`.

## Caveats

These are the reasons the headline speedups (8x–68x) must be treated as
**unverified** rather than as render-throughput facts:

1. **Mismatched present pacing.** The Vulkan numbers are collected on **xcb**
   with `VulkanPresentMode=2` (`VK_PRESENT_MODE_IMMEDIATE_KHR`, i.e. V-Sync
   disabled), while the GL baseline is left on the default pacing. The
   "empty scene: 48 -> 380 fps" jump in particular is a tell that the two sides
   are present-limited differently, not that one rasterizer is 8x faster.
2. **Frame metric.** The probe measures frames the engine is willing to
   produce, not completed presented frames, so an uncapped swapchain inflates
   the Vulkan side.
3. **Single box / single GPU.** No repeat across drivers or hardware.
4. **The GIF demos are not measurements.**

To make the comparison honest, re-run **both** sides under the same present
mode and platform (or measure GPU time instead of frame rate). Until then,
`PERF_COMPARISON.md` labels these results as provisional.

## The `novsync_layer/` verification layer

`nvsync_layer.c` is a small Vulkan layer that logs the
`VkSwapchainCreateInfoKHR::presentMode` the driver is asked for. It was used to
confirm the `VulkanPresentMode` preference actually reaches the driver (with
the preference set to `2` the layer saw `VK_PRESENT_MODE_IMMEDIATE_KHR`). Build
it into a `.so`, point `VK_LAYER_PATH` at this directory and enable
`VK_LAYER_FORK_novsync` to reproduce that check.
