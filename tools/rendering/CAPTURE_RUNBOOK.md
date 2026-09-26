# Vulkan capture & device-simulation runbook

External tooling around the fcprobe harness: frame capture/replay, GPU
profiling, and device simulation.  All commands run from the repo root with the
debug build (`build/debug/bin/FreeCAD`).

Prerequisites (Arch names): `gfxreconstruct`, `renderdoc`, `nsys` (from
`nsight-systems`), `vulkan-profiles`.  `renderdoccmd`, `nsys`,
`gfxrecon-replay` and `vulkaninfo` must be on `PATH`.

## 1. GFXReconstruct — record once, replay forever

Record a FreeCAD run into a `.gfxr`, then replay it without FreeCAD:

```sh
# Capture frames 1-30 of a probe (the layer appends the frame range to the
# requested name, so the harness discovers the real file).
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_diag_probe.py \
    --profile vulkan --gfxreconstruct --gfxreconstruct-frames 1-30 --name gfxr

# Replay headless and dump one PNG per frame into the artifact dir.
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py replay \
    /tmp/opencode/runs/gfxr-*/capture_frames_1_through_30.gfxr \
    --screenshots 1-30 --name gfxr-replay
```

**Gotcha — WSI.** FreeCAD presents through a Wayland surface, so
`--wsi headless` (the default) fails with
`vkGetPhysicalDeviceSurfaceFormatsKHR ... VK_ERROR_EXTENSION_NOT_PRESENT`.
Replay with the capture's own WSI instead:

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py replay <capture.gfxr> --wsi wayland
```

`--wsi xcb` works too when the capture was made on the XCB platform (see
RenderDoc below).  A replay that writes non-black PNGs (check
`magick identify -format '%[fx:mean]' frame.png`, expect ~0.2-0.4) has
reproduced the frames.

## 2. RenderDoc — frame debugging with object names

`renderdoccmd capture` injects the layer but has **no CLI frame trigger**, so
the harness sets `FC_PROBE_RENDERDOC=1` and the probe calls
`RENDERDOC_GetAPI` from inside the process, queues a capture, and forces a
frame to present:

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_diag_probe.py \
    --profile vulkan --renderdoc --name rdc
# -> <artifact>/renderdoc_frameNN.rdc
qrenderdoc <artifact>/renderdoc_frameNN.rdc
```

Notes:

- The harness enables `QT_XCB_GL_INTEGRATION=xcb_egl` for RenderDoc runs.
  RenderDoc's GLX hooks cannot create a context against the NVIDIA driver
  under XWayland, which otherwise floods the log with
  `QOpenGLWidget: Failed to make context current` /
  `QRhiGles2: Failed to make context current`.  EGL avoids the broken GLX path;
  the Vulkan capture is unaffected.
- Object names and pass labels come from `VK_EXT_debug_utils`
  (`FC_VULKAN_DEBUG_UTILS`); run with it set to see `Coin raster VkDevice`,
  `draw UBO ring`, `lighting UBO ring`, `TLAS`/`BLAS`, etc. in the pipeline
  state pane.
- The capture is written when a frame presents **after** the trigger; probes
  that call `Session.finish()` should not close the window first (the harness's
  `renderdoc_capture()` pumps frames for exactly this reason).

## 3. Nsight Systems — where the CPU time goes

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_diag_probe.py \
    --profile vulkan --nsys --name nsys
nsys stats --report vulkan_api_sum --format table <artifact>/nsys.nsys-rep
nsys stats --report cuda_gpu_kern_sum --format table <artifact>/nsys.nsys-rep
```

Symbol resolution and auto-stats are disabled by the harness to keep runs
fast; enable them by invoking `nsys profile` directly if needed.

## 4. Device simulation — the profiles fallback matrix

`VK_LAYER_LUNARG_device_simulation` is not packaged; the installed
`VK_LAYER_KHRONOS_profiles` layer is the fallback.  Profiles live in
`tools/rendering/device_profiles/*.json`:

| Profile | Effect |
| ------- | ------ |
| `no-ray-tracing` | drops `VK_KHR_ray_query` / `ray_tracing_pipeline` / `acceleration_structure` / `position_fetch`; keeps real limits |
| `no-descriptor-indexing` | drops `VK_EXT_descriptor_indexing` / `descriptor_buffer` |
| `portability` | forces `VK_KHR_portability_subset` emulation |
| `desktop-baseline-2022` | full `VP_LUNARG_desktop_baseline_2022` device (Vulkan 1.1, minimal limits) |
| `minimum-1-2` | full `VP_LUNARG_minimum_requirements_1_2` device |
| `roadmap-2022` | full `VP_KHR_roadmap_2022` device |

Single run:

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py run ../FreeCAD-DevTools/fcprobe/vk_diag_probe.py \
    --profile vulkan --device-profile no-ray-tracing --name devprof
```

Matrix (runs the Vulkan profile once per device and asserts each passes):

```sh
python3 ../FreeCAD-DevTools/fcprobe/freecad_probe.py matrix ../FreeCAD-DevTools/fcprobe/vk_diag_probe.py \
    --device-profile no-ray-tracing \
    --device-profile no-descriptor-indexing \
    --device-profile portability --name devmatrix
```

The layer's notifications are captured in `stdout.log`; look for
`Overriding device capabilities with the '...' profile` or
`exclude_device_extensions: ...` to confirm the simulation took effect.

**Push-constant portability.** The strict full-device profiles
(`desktop-baseline-2022`, `minimum-1-2`, `roadmap-2022`) set
`maxPushConstantsSize` to the Vulkan minimum (128 B).  The visual push-constant
block is 112 B because the projection matrix lives in the per-draw `DrawBlock`
UBO, not in the push block, so all profiles render.  If you shrink the UBO or
move data back into the push block, keep `sizeof(VulkanPushConstants) <= 128`
(the static_assert in `SoVulkanRenderBackendP.h` enforces it) or
`createPipelineLayout()` will reject minimum-spec devices again.

## 5. Ray-tracing validation layer

`VK_LAYER_NV_ray_tracing_validation` ships with newer NVIDIA drivers.  Enable
it with `--rt-validation`; the harness adds the layer when its manifest is
installed and records `rt_validation: unavailable` otherwise (a documented
N/A, not a failure).  On driver 615.71.09 the layer is absent.

## 6. Validation-layer profiles

See `tools/rendering/vk_layer_settings/README.md` for the
`VK_LAYER_KHRONOS_validation` check profiles (`--validation-profile`).
