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
  ├─ InteractionController ── interaction authority ├─ SoIRRenderAction ──► SoDrawList (retained IR)
  │    (navigation + controller-owned SoEventManager)  ├─ SoVulkanRenderBackend  (raster)
  ├─ hidden GL viewer (RasterCoin fallback + IR path)  └─ SoRTXRenderBackend     (path tracing, lazy)
  └─ VulkanViewportAdapter (QStackedWidget)
       └─ QuarterVulkanWidget (QWidget, InputDeviceHost) ── drives the controller
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
     - **HDR branch** — when `isHdrRasterActive()` (HDR requested *and* a 10-bit
       swapchain is live *and* the mode is raster), the widget instead calls
       `m_manager.renderExternalHdr(cb, cb-pass, cb-framebuffer)` and does *not*
       begin Qt's pass: the manager renders into a backend-owned linear RGBA16F
       intermediate and runs the output pass into the caller's framebuffer.  See
       §6 "HDR output" for the pipeline;
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

### HDR output (HDR10 / PQ)

The Vulkan viewport can present in high dynamic range.  Two preferences drive it
(`View` group, `Vulkan*` prefix, so `VulkanViewSettings::isDisplayPref` re-applies
them on change):

| Pref | Meaning |
|---|---|
| `VulkanHDR` (bool) | Request HDR10 output.  Ineffective unless the session/capability gate below passes. |
| `VulkanHDRExposure` (float, default `0.02`) | Linear gain applied to scene radiance before the PQ encode.  Scene-white (radiance `1.0`) is presented at `exposure × 10000 cd/m²`, so `0.02` maps diffuse white to the ~200 cd/m² SDR reference white while highlights still exceed it. |

**Pipeline (raster).**  `recordScenePass()` takes the HDR branch (see §2) and
`SoVulkanRenderBackend::renderExternalHdr()` renders the scene into a
backend-owned **linear RGBA16F** intermediate (its own render pass + framebuffer
in the render-pass cache), barriers it to `SHADER_READ_ONLY`, then runs a
fullscreen output pass (`data/shaders/vulkan/output/Output*.glsl`) into the
caller's swapchain framebuffer.  That pass applies `exposure` and the SMPTE
ST 2084 (PQ) inverse-EOTF **once, after all geometry/transparency has blended in
linear light** — which is what makes it color-correct; blending and MSAA resolve
must happen on linear radiance, never on PQ-encoded values.  A scene tone map is
deliberately *not* applied: it would compress exactly the highlights HDR exists
to preserve.  Path tracing keeps its own present pass, which applies the same
exposure + PQ encode (`rt/PresentFragment.glsl`); the two paths coexist because
the RT backend owns its present pass while the raster path borrows the caller's.

**Capability gate.**  HDR is *requested* by the pref but only *active* when
`isHdrOutputActive()` is true, i.e. the live swapchain came up 10-bit
(`VK_FORMAT_A2B10G10R10_UNORM_PACK32`, or FP16 scRGB).  Qt on Wayland selects
`VK_COLOR_SPACE_PASS_THROUGH_EXT` and carries the BT.2020 + ST 2084 mapping in a
`wp_image_description` derived from the window's `QColorSpace`
(`QColorSpace::Bt2100Pq`, set before the window is shown).  The widget logs the
surface's HDR formats (`[VK-HDR]` breadcrumbs, `logHdrSurfaceFormats`).  When any
of this is unavailable the viewport silently stays SDR.

**Driver guard.**  `hdrDriverBlocked()` refuses HDR on the NVIDIA 610.x series:
that driver forwards invalid HDR10 mastering metadata
(`min_luminance >= max_luminance`) to the compositor unfiltered, and KWin then
raises `wp_color_manager_v1` `invalid_luminance` and kills the client (Mesa's WSI
sanitizes the same values).  Qt's color-management path does not call
`vkSetHdrMetadataEXT`, so FreeCAD is not expected to trigger it, but a
compositor-side client kill is not an acceptable default.  Override with
`FREECAD_VULKAN_HDR_ALLOW_UNSAFE_DRIVER=1`; `FREECAD_VULKAN_HDR_FORCE_BLOCK_DRIVER=<major>`
exercises the gate.  No NVIDIA driver release had fixed this as of the last
report (610.57.04).

**Limitations.**  HDR output is native-Wayland only (the compositor
color-management protocol); X11 and non-HDR outputs fall back to SDR.  The raster
HDR intermediate is single-sample, so HDR mode drops MSAA until a resolve step is
added.  When HDR is off the SDR path is byte-identical to the pre-HDR renderer
(the manager uses `renderExternal()` and never allocates the intermediate).

## 7. Verification

| Harness | What it covers | Command |
|---|---|---|
| `tools/rendering/verify_renderer.sh` | ABI sentinel + ctest subset (+ optional preci/suite) | `tools/rendering/verify_renderer.sh` |
| Coin testsuite (`src/3rdParty/coin/testsuite/vulkan`) | 22 headless backend tests (lifecycle, depth, culling, textures, parallel recording, …) | `cmake -DCOIN_BUILD_TESTS=ON` then ctest |
| fcprobe (`tools/fcprobe/freecad_probe.py`) | GUI probes, regression suite `vk_suite.json`, parity matrix, soak | `python3 tools/fcprobe/freecad_probe.py suite` |
| Frame dumper (`FC_VULKAN_DUMP_FRAME=1`, `DUMP_START/END`) | golden-frame A/B; proves a refactor pixel-identical | `run --baseline DIR` |
| ABI sentinel (`tools/rendering/abi_sentinel.py`) | exported symbol set vs `abi_baseline.json`; refresh with `--capture` when intentional | part of `verify_renderer.sh` |

## 8. Known structural debt

Tracked in the renderer architecture cleanup:

- **Dual frame paths** — `render()` and `renderExternal()` now share a single
  `FramePlan` (`beginFramePlan()` / `recordFramePlan()` in
  `SoVulkanRenderBackendFrame.cpp`): target validation, frame matrices, the
  frame boundary, lighting setup, the geometry-cache update and the composite
  slot reservation are resolved once, and the record step is dispatched from
  the same decision.  What still differs is the pass/framebuffer/command-buffer
  lifecycle (the internal path owns it, the external path borrows the caller's)
  and the texture-upload step (`SoVulkanTextureCache::recordPending()` inline
  versus the external pre-pass).
- **God class** — `SoVulkanRenderBackend` holds the frame pump, caches, the
  worker pool and the CPU wide-line expander in one class; splitting into
  collaborating parts is the keystone refactor.  Extracted so far:
  `SoVulkanGpuTimers`, `SoVulkanPipelineCache`, `SoVulkanRenderPassCache`,
  `SoVulkanSamplerCache`, `SoVulkanStagingPool`, `SoVulkanBufferFactory`,
  `SoVulkanFrameRing` (primary command buffers + fences),
  `SoVulkanTextureCache` (per-command entries, staging/upload path, eviction
  sweep, white fallback), `SoVulkanGeometryArena` (the shared vertex/index
  blocks the cache carves per-command ranges out of) and
  `SoVulkanGeometryCache` (the per-command entries + lookup map, the
  per-command and shared-arena upload paths, and the generation/composite
  eviction sweep).  The texture cache moved the generation sweep and index
  re-resolution out of `updateGeometryCache()`, so the geometry loop now just
  calls `prepareCommand()`/`sweep()`; it borrows the shared set-1 descriptor
  pool and the deferred-destruction ring from the backend via callbacks.  The
  geometry cache similarly borrows the arena, the buffer factory and the
  deferred ring, and `updateGeometryCache()` is now the orchestrator that
  drives it plus the texture cache.  The secondary command buffers/pools and
  the record pool stay in the backend (the parallel-recording subsystem, not
  the frame ring).  The device handles themselves (`device`/`physicalDevice`/
  `vmaAllocator`/…) are still backend members used directly in ~100+ places, so
  the buffer factory, the arena and the geometry cache *borrow* them rather
  than owning them; a true `SoVulkanDevice` that owns the handles is a later,
  higher-churn step.
- **Two geometry stacks** — the raster and RT backends each upload meshes in
  their own format with their own cache and memory policy.  While ray tracing
  is active the raster backend is only compositing overlays/residue and
  releases the traced triangle geometry
  (`SoVulkanRenderBackend::setOverlayCompositeMode`), so the two stacks do not
  hold the same meshes resident at once; the formats and caches are still
  separate.
- **Hidden GL viewer as interaction authority** — resolved by the interaction
  controller: navigation, `processSoEvent` dispatch and the Coin `SoEventManager`
  now live in `Gui::InteractionController`, and the Vulkan surface translates
  its own input (`InputDeviceHost`) and drives that controller, so picking/
  navigation no longer depend on the hidden GL viewer's event manager.  Cursor
  mirroring and the render-manager region/DPR sync remain because navigation
  still runs on the GL `InteractionSurface`; see
  [`INTERACTION_AUTHORITY.md`](INTERACTION_AUTHORITY.md) §9.
