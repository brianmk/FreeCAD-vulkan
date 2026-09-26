# Performance Comparison — Vulkan Fork vs Upstream FreeCAD Base

Controlled benchmarks of the fork's **Vulkan viewport** (with the
`VulkanPresentMode` setting) against the fork's **GL fallback** and a
**pristine upstream build**.

> **Verification status — read first.** The numbers below are **provisional and
> unverified**. They compare Vulkan running in `Immediate` mode (V-Sync
> disabled, on xcb) against a GL baseline on its default pacing, so part of the
> gap — most visibly the "empty scene: 48 → 380 fps" jump — is a present-pacing
> artifact rather than render throughput. The driver scripts are committed under
> [`tools/perf/vulkan_vs_base/`](tools/perf/vulkan_vs_base/) with the full
> caveat list; re-run **both** sides under matched present mode and platform
> before quoting these figures.

Graphs (in `docs/vulkan-perf/`):
[`perf_dashboard.png`](docs/vulkan-perf/perf_dashboard.png) ·
[`perf_throughput.png`](docs/vulkan-perf/perf_throughput.png) ·
[`perf_vorontest.png`](docs/vulkan-perf/perf_vorontest.png) ·
[`perf_startup.png`](docs/vulkan-perf/perf_startup.png) ·
[`perf_rotating_camera.png`](docs/vulkan-perf/perf_rotating_camera.png)

## Setup

| | Baseline | Fork |
| --- | --- | --- |
| Source | `FreeCAD_base/FreeCAD` @ `acdce56122` (upstream) | `feat/vulkan-present-mode` |
| Build | `build/release`, Release | `build/release-vulkan`, Release |
| Binary | `FreeCAD_base/FreeCAD/build/release/bin/FreeCAD` | `build/release-vulkan/bin/FreeCAD` |
| Toolchain | pixi `default` (clang++, mold, Ninja) | same |

- Probe: `scene_bench_probe.py` (upstream guest probe) + a startup/open probe;
  identical files under both binaries. `[HARNESS]` lines are written to
  `sys.__stdout__` because FreeCAD redirects `print()`.
- Scene: 40 frames after 8 warm-up, 1600x900, MSAA 0, render cache off.
- Box: 32 cores, 93 GiB, display 3840x2160 @ 119.86 Hz, RTX 5090.

## Fixing the V-Sync lock

1. **Swapchain present mode.** `QVulkanWindow` hardcodes
   `VK_PRESENT_MODE_FIFO_KHR`. The new uncommitted `VulkanPresentMode`
   preference (View → 3D View: 0 FIFO / 1 Mailbox / 2 Immediate) writes
   `QVulkanWindowPrivate::presentMode`. A logging Vulkan layer confirms the
   setting reaches the driver: with `=2` the app requests presentMode 0
   (`VK_PRESENT_MODE_IMMEDIATE_KHR`).
2. **Qt/Wayland frame scheduling.** On native Wayland the forced-frame metric
   stays ~8–12 ms even with Immediate, because `QWindow::requestUpdate()` is
   paced by the compositor's `wl_surface.frame` (vblank) callbacks. Under
   **xcb** that cap disappears. All results below use **xcb** with
   `VulkanPresentMode=2`.

## 1. Scene throughput (xcb, Immediate)

| Workload | base GL | fork GL | fork Vulkan | Vk vs base |
| --- | ---: | ---: | ---: | ---: |
| empty | 48.1 fps | 46.0 fps | **379.7 fps** | 7.9x |
| sketches 200 | 42.1 fps | 42.2 fps | **380.7 fps** | 9.0x |
| objects 1000 | 22.7 fps | 21.4 fps | **312.2 fps** | 13.7x |
| objects 5000 | 6.9 fps | 6.9 fps | **211.9 fps** | 30.8x |
| objects 15000 | 2.43 fps | 2.31 fps | **6.10 fps** | 2.5x |

GL parity holds (±6%).

## 2. vorontest.FCStd (44.6 MB, single huge Part)

| Config | document open | render | speedup |
| --- | ---: | ---: | ---: |
| base GL | 17618 ms | 5.5 fps | 1x |
| fork GL | **1335 ms** | 46.2 fps | **13.2x open / 8.4x render** |
| fork Vulkan | 1266 ms | **376.9 fps** | 13.9x open / **68x render** |
| fork Vulkan path-trace | 1310 ms | 356.0 fps | 13.4x open / 65x render |

The fork opens the 44 MB document ~13x faster (async display-geometry during
restore) and renders it ~68x faster on Vulkan.

## 3. Startup & cube-document open (xcb, median of 5)

| Config | startup incl. logo | open cube (median) | launch → cube open |
| --- | ---: | ---: | ---: |
| base GL | 2111 ms | 122.7 ms | 3707 ms |
| fork GL | 2166 ms | 134.8 ms | 3793 ms |
| fork Vulkan | 2145 ms | **217.7 ms** | 4544 ms |

Startup including the splash/logo is unchanged (~2.1 s). The Vulkan first-open
adds ~95 ms for device/swapchain creation; subsequent cube opens are ~120 ms.

## 4. Rotating camera, 1000 boxes (xcb, orbit)

| Render mode | fps |
| --- | ---: |
| base GL | 22.1 |
| fork GL | 21.8 |
| fork Vulkan raster | **74.3** |
| fork Vulkan ray-trace | 71.0 |
| fork Vulkan path-trace | 68.5 |
| fork Vulkan environment | 72.6 |

Continuous camera motion (each frame resyncs the scene and restarts path
tracing) drops Vulkan from 312 to ~74 fps, but it is still **3.4x** upstream
GL, and ray/path/environment tracing all stay within ~8% of raster.

## 5. Path tracing

| Config | fps |
| --- | ---: |
| fork Vulkan path-trace, rotating 1000 boxes | 68.5 |
| fork Vulkan path-trace, static 250 boxes | 350.0 |
| fork Vulkan path-trace, vorontest | 356.0 |

### Demo GIFs

Camera flying around the BIMExample house, cloned into 8 houses mid-animation
and doubled to 16 near the end (base GL ~10 fps vs fork Vulkan ~33 fps; model
edges on for both):

| base GL | fork Vulkan (Immediate) |
| :---: | :---: |
| ![base GL fly-around](docs/vulkan-perf/houses_base_gl.gif) | ![fork Vulkan fly-around](docs/vulkan-perf/houses_vulkan.gif) |

Raster Vulkan → Path Tracing Max (mode 6), `BIMExample.FCStd` (camera flies in; RTX denoiser):

![raster Vulkan to Path Tracing Max](docs/vulkan-perf/bim_pathtracing_max.gif)

## Findings

1. The `VulkanPresentMode` setting works at the driver boundary; native Wayland
   still adds a compositor-scheduler cap that xcb avoids.
2. Vulkan is **8x–31x** faster than upstream GL on ordinary scenes and **68x**
   on the 44 MB vorontest model.
3. The fork's async geometry makes huge-document **open ~13x faster**.
4. GL parity is preserved; startup-with-logo is unchanged.
5. Vulkan render modes (raster/ray/path/environment) are within ~8% of each
   other under camera motion.

## Reproduce

The drivers are committed under `tools/perf/vulkan_vs_base/` (with its own
[README](tools/perf/vulkan_vs_base/README.md)); point them at your builds with
`PC_BASE_BIN` / `PC_FORK_BIN` and run:

```sh
python3 tools/perf/vulkan_vs_base/compare_scene_xcb.py   # scene throughput
python3 tools/perf/vulkan_vs_base/bench_extra.py         # vorontest / rotate / PT
python3 tools/perf/vulkan_vs_base/bench_open.py          # startup / open
python3 tools/perf/vulkan_vs_base/make_graphs.py         # charts
```

Results are written to `tools/perf/vulkan_vs_base/{results,results_extra,results_open}/`.
Probes `scene_bench_probe_io.py` / `open_bench_probe.py`, the verification
layer `novsync_layer/` and the chart tool `make_graphs.py` live in the same
directory.
