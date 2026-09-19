# FreeCAD Vulkan renderer — architecture

Living reference for the fork's Vulkan renderer (raster + ray tracing).  It
describes the layering, the frame flow, the synchronization and caching model,
the configuration surface and the verification harnesses.  Update it in the
same change that alters any of those.

Path-tracing status and roadmap live in [`RTX_STATUS.md`](../../RTX_STATUS.md).

## 1. Layering and ownership

```
FreeCAD Gui (Qt)                                Coin (Qt-free, submodule)
────────────────────────────────────────────    ─────────────────────────────────────────────
View3DInventorViewer                             SoVulkanRenderManager (pimpl)
  ├─ hidden GL viewer  ── interaction authority    ├─ SoIRRenderAction ──► SoDrawList (retained IR)
  └─ VulkanViewportAdapter (QStackedWidget)        ├─ SoVulkanRenderBackend  (raster)
       └─ QuarterVulkanWidget (QWidget)            └─ SoRTXRenderBackend     (path tracing, lazy)
            └─ QVulkanWindow
                 └─ QuarterVulkanRenderer
                      └─ SoVulkanRenderManager
```

- **The application owns the Vulkan device.**  `QuarterVulkanWidget` creates a
  process-wide refcounted `QVulkanInstance` and QVulkanWindow creates one
  `VkDevice` per 3D view.  Coin borrows instance/device/queue through
  `SoVulkanDeviceContext` (`include/Inventor/rendering/SoVulkanRenderTarget.h`)
  and a per-frame `SoVulkanRenderTarget`; Coin never creates a surface, a
  swapchain, a render pass, or a submission on the GUI path.
- **The manager owns the backends.**  `SoVulkanRenderManager` traverses the
  scene graph (or replays a retained draw list), then dispatches to the raster
  backend, or to the RT backend plus a raster overlay composite.
- **`SoVulkanRenderBackend` and `SoRTXRenderBackend`** implement
  `SoRenderBackend` (`src/rendering/SoRenderBackend.h`).  The interface is the
  Vulkan backend contract, not a promise that every backend is interchangeable.

## 2. Frame flow (external path — the only GUI path)

`QuarterVulkanRenderer` (`src/Gui/Quarter/QuarterVulkanWidget.cpp`):

1. `initResources()` — build `SoVulkanDeviceContext`, set the persistent
   pipeline-cache path, `m_manager.initialize(&context)`.
2. `initSwapChainResources()` — set the render target and
   `setMaxFramesInFlight(swapChainImageCount() + 1)`.
3. `startNextFrame()`:
   - snapshot the frame state (scene/camera/settings under a mutex);
   - `setupRenderTarget()` — color = MSAA image or swapchain image, depth =
     QVulkanWindow's depth/stencil;
   - `pushSceneState()` — push scene/overlay/decoration graphs, camera,
     viewport, DPR and the `SoVulkanViewSettings` blob;
   - `recordScenePass(cb)`:
     - `vkCmdBeginRenderPass(defaultRenderPass, currentFramebuffer)` — Qt's
       LOAD pass, whose clear values are filled by the widget;
     - `m_manager.renderExternal(cb, pass, framebuffer)`:
       - `prepareRenderParams()` — refresh the active camera, auto-clipping,
         traverse (`SoIRRenderAction`) or replay, sort, build `SoRenderParams`;
       - `backend.prepareExternalFrame()` — `beginFrame()` (advance the frame
         ring, flush deferred destroys), lighting setup, `updateGeometryCache()`
         (stage changed geometry/textures);
       - `beginExternalPrepass()` — record the pending texture copies and the
         sub-pixel LOD compaction dispatches into a backend-owned transient
         command buffer (Vulkan forbids transfer/compute inside a render pass);
       - `recordFrame()` — opaque pass (secondary buffers, optionally parallel
         workers), transparent pass, on-top annotations, overlay block;
       - `submitExternalPrepass()` — submit the transient buffer and wait, so
         the copies and compaction are visible before the caller's submission;
     - `vkCmdEndRenderPass(cb)`;
   - `frameReady()` (Qt submits/presents); `requestUpdate()` while path
     tracing is refining.
4. `releaseSwapChainResources()` / `releaseResources()` — `m_manager.shutdown()`
   while the device is still valid.

**Own-queue path.**  `render()` / `renderOverlaysOnly()` drive a complete
offscreen frame (own render pass, framebuffer cache, submission) and are used by
the Coin testsuite and `SoFCOffscreenRenderer`, not by the GUI.

## 3. Retained IR and replay

- `SoIRRenderAction` walks the scene and emits a `SoDrawList` of
  `SoRenderCommand`s (geometry, material, state, pass).  The raster and RT
  backends both consume that one list.
- A **camera-only frame** replays the previous draw list: the manager compares a
  fingerprint of the main scene (`computeGraphFingerprint`) and skips
  traversal.  `SoRenderParams::geometryContentUnchanged` tells the backend the
  main geometry is bit-identical, so it skips content re-hashing.
- Application-side state the graph fingerprint cannot see (FreeCAD selection /
  preselection rendered by `SoFCSelectionRoot`) is published through
  `SoVulkanRenderManager::setExternalRevision()`; any bump forces a
  re-traversal.
- `SoRenderParams::interactionLod` is set while the camera moves.  Backends may
  drop work invisible in motion (raster: wide lines become 1px; RT: single
  bounce) and restore quality when the camera stops.

## 4. Synchronization

- **Frame slots.**  `maxFramesInFlight` sizes the per-frame command
  buffers/fences, the lighting/draw UBO ring and the deferred-destruction ring.
  The GUI sets it to `swapchain images + 1`, the margin being what makes slot
  reuse safe when the caller (QVulkanWindow) owns submission: an image is only
  released after its present completes, so a slot is not reused until the
  swapchain has cycled.
- **Deferred destruction.**  Resources replaced while a frame may still
  reference them are queued (`SoVulkanShared::PendingDestroys`) and released
  `maxFramesInFlight` frames later (ring-slot style for the raster backend,
  current-slot for the RT backend).
- **Transient pre-pass.**  The external pre-pass command buffer is submitted and
  host-waited before the caller submits its pass (no semaphore can be threaded
  through the caller's submission).  Recording happens before `recordFrame()`
  and submission after, so the CPU frame recording overlaps the previous GPU
  frame.
- The own-queue path signals a fence per frame slot; the external path relies on
  Qt's present semaphores and never signals those fences.

## 5. Caches and invalidation

| Cache | Key | Invalidated by | Notes |
|---|---|---|---|
| Geometry (`gpuCache`) | `SoRenderCommand*` + content hash | content-hash change, cache eviction (unvisited generation) | packed 32-byte interleaved vertex stream + optional index buffer; shared blocks for retained batches |
| Texture (`textureCache`) | command + content hash | content-hash change, eviction | images sub-allocated from the staging pool; shared `VkSampler` cache |
| Pipeline (`pipelineCache`) | `PipelineKey` (render pass, topology, fill/cull, depth, blend, stencil, samples, wide-line variant) | never during a session; persisted to disk (A4) | `resolvedKey`/`resolvedPipeline` on the cache entry is the steady-state fast path |
| Render pass / framebuffer | attachment identity (formats, samples, layouts, load ops) + target images | target change | render passes survive swapchain image cycling; the framebuffer is recreated on any target change |
| Sub-pixel LOD slots | per command + content hash | content-hash change, cache eviction | `maxFramesInFlight` compacted index buffers; built only while interaction LOD is active |
| Wide-line slots | per command + `maxFramesInFlight` | buffer too small | CPU quad expansion; the GPU-instanced path (non-stippled `LINE_LIST`) needs no per-frame buffer |
| Pipeline cache (disk) | `pipelineCacheUUID` (driver-validated) | device/driver change | `<CacheLocation>/vulkan/pipeline_cache.bin`, temp-file + rename |

## 6. Configuration

`SoVulkanConfig` (`src/rendering/SoVulkanConfig.h`) is the typed, enumerable
home for renderer knobs.  The environment is resolved once, on first use, and is
immutable for the process lifetime.  `SoVulkanConfig::dump()` prints the
resolved set; it runs at device init when `FC_VULKAN_BACKEND_DEBUG=1`.

Boolean flags honor the `"0"`/`"false"`/`"off"` opt-out unless documented
presence-only.  The remaining direct `getenv` sites are debug/timing flags
(`FC_VULKAN_BACKEND_DEBUG`, `FC_VULKAN_FRAME_TIMING`, `FC_VULKAN_CLIP_DEBUG`,
…) and the RT behavioral/material flags; they migrate when their subsystem is
next touched.

## 7. Verification

| Harness | What it covers | Command |
|---|---|---|
| `tools/rendering/verify_renderer.sh` | ABI sentinel + ctest subset (+ optional preci/suite) | `tools/rendering/verify_renderer.sh` |
| Coin testsuite (`src/3rdParty/coin/testsuite/vulkan`) | 23 headless backend tests (lifecycle, depth, culling, textures, parallel recording, …) | `cmake -DCOIN_BUILD_TESTS=ON` then ctest |
| fcprobe (`tools/fcprobe/freecad_probe.py`) | GUI probes, regression suite `vk_suite.json`, parity matrix, soak | `python3 tools/fcprobe/freecad_probe.py suite` |
| Frame dumper (`FC_VULKAN_DUMP_FRAME=1`, `DUMP_START/END`) | golden-frame A/B; proves a refactor pixel-identical | `run --baseline DIR` |
| ABI sentinel (`tools/rendering/abi_sentinel.py`) | exported symbol set vs `abi_baseline.json`; refresh with `--capture` when intentional | part of `verify_renderer.sh` |

## 8. Known structural debt

Tracked in the renderer architecture cleanup:

- **Dual frame paths** — `render()` and `renderExternal()` duplicate pass/
  framebuffer/clear/texture-upload handling.
- **God class** — `SoVulkanRenderBackend` holds the frame pump, three caches,
  the worker pool and the CPU wide-line expander in one class; splitting into
  collaborating parts is the keystone refactor.
- **Two geometry stacks** — the raster and RT backends each upload the same
  meshes in their own format with their own cache and memory policy.
- **Hidden GL viewer as interaction authority** — navigation/picking still run
  on the never-rendered OpenGL viewer, so viewport features must work in both
  stacks and coordinate systems are hand-synced.
