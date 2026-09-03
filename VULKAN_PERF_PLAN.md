# Vulkan Renderer Performance Improvements — Implementation Plan

> **TRANSIENT DOCUMENT.** This is a working plan for implementing the
> Vulkan performance features researched and agreed on. **Delete this file
> once the implementation is complete** (see "Deletion" at the end).

## 0. Progress & blockers (updated 2026-09-03)

### 2026-09-03 — AS-skip (GPU win) + denoise-after-move regression + probe

- **AS-skip (skip per-frame acceleration-structure rebuild on a static
  scene)** — implemented in `recordAccelerationStructures`:
  `asDirty = cacheChanged || asTransformChanged`. When false the
  `buildNeePool`/`updateMaterials`/`buildTlas`/`updateDescriptors`/AS-barrier
  block is skipped. `asTransformChanged` is fed by `RTXCachedGeometry::
  transformSignal` (hashed in `updateGeometryCache`), so a pure camera orbit
  (world-space AS, uses `command.modelMatrix`, not view/proj) never dirties it.
  Validated: `[RTDBG] frameTiming asRecord` = **74.68 ms** on the first (build)
  frame, then **0.09–0.22 ms** on subsequent static frames (record skipped);
  `asGpu` stays steady ~5.5–5.9 ms (the trace is GPU-bound and recorded in the
  AS-phase cmd). `profile` + `fullresolve` both still PASS.
- **Black-flash regression fix** — root cause: `updateDescriptors()` only
  populated `set[descriptorSetIndex]`; after gating on `asDirty`, the trace
  alternated between a valid set (full trace ~5.3 ms) and a never-initialized
  set (0.18 ms black). Fix: the `descriptorSetIndex = (descriptorSetIndex+1)&1`
  alternation moved from the top of `recordAccelerationStructures` into the
  `asDirty` block so non-build frames keep binding the last-populated set.
- **Denoise-after-move regression (fixed + validated)** — symptom: after a
  camera move the denoiser never fired until a hover forced a rebuild. Root
  cause: the background/environment-change wake (`backgroundChanged`) compared
  floats with exact `!=`; a tiny FP re-derivation of the viewport gradient read
  as a background change, reset the run at the sample ceiling, and **cleared the
  pending `ptDenoisePending`** so the denoiser never published. Fix: epsilon
  compare (`fne`, 1e-4) so only a *real* background change restarts the run.
  Validated by new probe `vk_denoise_move_probe` (suite case `denoise-move`,
  `FC_VULKAN_PT_MAXSAMPLES=32`): baseline denoise publishes (ready=1) before
  the move, then after a 30° orbit `accum` restarts, re-reaches the cap
  (`pend=1`), and publishes `ready=1` again — check log:
  `move_ord=540 restart_after_move=true post_pend_frame=528 post_ready=true`.
- **Mode-at-creation harness finding (corrected)** — the RT probes are
  *self-sufficient*: `set_pref("VulkanRenderMode",4)` writes the live ParamGet
  (in-memory, inert for immediate application), and each probe closes existing
  docs, sets the pref, then creates a new document — so the fresh
  `View3DInventor` construction reads the pref and opens in RayTracing mode.
  No `user.cfg` seed is needed; verified with a fully clean config (`profile`
  and `denoise-move` both render RT and PASS). The earlier "view created
  RasterVulkan / set_pref is inert" dead-end only applied to setting the mode on
  an *already-created* view (combo path), not the re-create-after-pref order the
  probes use. Do NOT rely on a persisted seed.

Implemented + runtime-validated (prior session):
- **Item 1** — TLAS instance culling (`FC_VULKAN_TLAS_CULL`, `FC_VULKAN_TLAS_PIX`):
  object-space AABB + camera-frustum projection, sub-pixel cull that never drops a
  near-plane-straddling box; refit/UPDATE now requires an identical instance set.
  Probe `vk_tlascull_probe` on/off both pass.
- **Item 4a** — 16-bit AS vertex format (`FC_VULKAN_AS_PACK`): `buildBlas`/`refitBlas`
  choose `R16G16B16_SFLOAT` (half) when the gate is on and coords fit the half
  range, else `R32G32B32_SFLOAT`. Probe `vk_aspack_probe` on/off both pass.
- **Item 4b** — AS compaction (`FC_VULKAN_AS_COMPACT`): after the frame that built a
  BLAS completes, the AS compacted size is read back via a query pool and the AS is
  copied into a smaller buffer; the original is deferred-destroyed and `devAddr`
  swapped. Measured 4224→2944 bytes (≈30%) on this box; compacted BLASes are rebuilt
  (not refitted) on edit. Probe `vk_ascompact_probe` passes.
- **Profile (FC_VULKAN_FRAME_TIMING)** — added `[RTDBG] frameTiming` phase
  breakdown (interval / asRecord / asGpu / denoise / traceRecord) to
  renderExternal, plus `vk_profile_probe` (suite case `profile`). On a 3-object
  scene at 3 bounces (54 frames): interval ~6.3ms (~159 fps); **asGpu ~1.65ms
  (26%)**; trace + present + vsync ~4.6ms (74%). The path-trace kernel dominates.
  Enabling Item 1/4a/4b leaves asGpu flat (~1.65ms), so on this tiny scene the AS
  build is fixed-overhead dominated (per-frame TLAS rebuild), NOT geometry-size
  bound -- those wins appear on large scenes (many instances/vertices).
  Conclusion: materials/BDA work (Item 3) will not move frame time; the trace
  kernel is the target (or measure AS behaviour on a large scene).

- **Interactive PT "faint/transparent boxes" fix (option 1)** — root cause: the
  adaptive sampler (PathTrace.glsl:652) freezes a pixel once `nPrev >=
  ptAdaptiveMinSamples` (4) AND its rel-variance is low; small/distant boxes
  whose first few samples are background-dominated freeze at a thin
  background-leaning mean, the active fraction collapses below
  `ptAdaptiveStopFraction` (0.05), and the run idles on a faint/edge-only image
  in ~9 frames.  Fix = force a **full-resolve after every camera/scene move**:
  - new member `ptForceFullResolve` (SoRTXRenderBackend.h); latched TRUE on
    start / scene-change / view-change reset (PathTracing.cpp:130,145,165).
  - while latched, `adaptive[3]` (the per-pixel freeze gate) is forced 0 so the
    shader samples EVERY pixel each frame (fraction stays ~1.0), and
    `adaptivelyConverged` is gated off (`!ptForceFullResolve`) so the run can
    only idle once `ptFrameIndex >= ptMaxSamples` (PathTracing.cpp:183,624).
  - verified: `[RTDBG] adaptive ... fraction=1.0000 frameIndex=.. maxSamp=.. fill=1`
    for frames 0..maxSamps-1, then `fill=0` at the cap.  The run no longer
    idles on a 4-sample freeze; every pixel reaches the sample cap.
  - NOTE: runtime `ptMaxSamples` was **64** (the user's saved
    `VulkanPathTracingMaxSamples`), not the 256 default — low cap; raise the
    pref for cleaner convergence.
  - The far/small boxes STILL render as a diamond lattice (aliasing moiré) even
    with 64 full samples and `FC_VULKAN_PT_DENOISER=none`: each box is ~1px in
    a fit-all iso view, so jittered samples alternate hit/miss.  This is a
    SEPARATE geometric-aliasing artifact (near/far halving / AGS), not the
    under-sampling freeze; needs its own AA work.
  - **SSAA (deferred)** — the fix for the lattice is supersampling AA, gated
    `FC_VULKAN_PT_AA` (default 1 = unchanged).  All three routes require risky
    plumbing and were NOT taken in-session: (1) buffer-scale SSAA scales the
    trace buffers + storage image + dispatch grid + denoiser readback
    (`denoiseWidth` follows the buffer size) and needs a box-downsample in the
    present pass; (2) in-shader primary-ray loop wraps the ~200-line monolithic
    path-trace and re-seeds per subsample; (3) host re-dispatch N frames needs a
    per-subsample frame-index (the frame rides in a host-mapped UBO read at
    submit time, so per-dispatch variation needs device-side updates between
    dispatches).  NOTE: the far boxes are genuinely sub-pixel in a fit-all
    grid, so even perfect AA makes them clean faint dots, not big cubes;
    "selection goes crazy" is a downstream effect of picking the aliased image
    (the Coin pick is correct against the true geometry).  Recommend a focused
    session or accepting sub-pixel far boxes.

- **Item 2a** — hardware capability check (`probeComputeQueue` + `[RTDBG] computeCaps`):
  report queue-family compute support/count and acquire a second compute-capable queue
  when the family has >= 2 queues. Here it confirms the family supports compute and
  16 queues but Qt created only 1 (so `computeQueue=0`).
- **Item 2b (device-creation queue request)** — Qt EMBEDDING workaround: instead of
  reworking the device, `QuarterVulkanWidget` calls `QVulkanWindow::
  setQueueCreateInfoModifier` (Qt 5.15+ official hook to inject `VkDeviceQueueCreateInfo`)
  to request a dedicated `VK_QUEUE_COMPUTE_BIT` family at device creation. Verified:
  `[RTDBG] computeCaps family=2 idx=0 req=1 computeQueue=1 computeCount=8 flags=0xe`
  (family 2 is compute-only, 8 queues, and we now hold a real queue handle). The
  `FC_VULKAN_ASYNC_COMPUTE` denoiser-copy path dispatches on this queue. Timeline
  semaphores (Vulkan 1.2 core) are probed + enabled at device creation so the sync can
  be made non-blocking (replacing the current fence wait).
  **Item 2b limit found (honest):** the RT frame loop is synchronous (`vkQueueWaitIdle`
  on the graphics queue at the end of every frame, and the denoiser runs right after
  that wait), so a Vulkan compute queue cannot yet overlap with the present until the
  per-frame `vkQueueWaitIdle` is removed AND the present (Qt-submitted, no hook to add
  a semaphore wait) is made to wait on the compute timeline. On this NVIDIA GPU the
  denoiser chooses RTX/OptiX (kind=2), which already overlaps via CUDA interop
  semaphores, so `submitDenoiseCopy` (the OIDN CPU-worker publish path, now wired to
  the timeline semaphore) is only exercised on non-RTX denoiser hardware.



Blocked by architecture (device is created by Qt's `QVulkanWindow`; the enabled
feature chain is `bufferDeviceAddress → accelerationStructure → rayTracingPipeline → rayQuery`):
- **Item 2** — async compute: **no compute queue** (`SoVulkanDeviceContext` exposes only
  `graphicsQueue`), no timeline semaphores; needs Qt device/queue-create rework.
- **Item 3** — bindless descriptor arrays: `bufferDeviceAddress` is enabled, but
  `VK_EXT_descriptor_indexing` is **not** in the enabled feature chain (needs embedding change).
- **Item 5** — pipeline library + FSR: `VK_KHR_pipeline_library` **not** enabled (needs embedding change).

Remaining in-item work: Item 4b (AS compaction + per-instance push-constant transforms).
Parked suite follow-ups (pre-existing renderer bugs surfaced by RT now running, not the
perf items): RTX edge/point overlay (prefs/points), mis-on NEE glow, rt-phase0 live toggle,
pick mouse injection.


## 1. Context & goal

The FreeCAD debug build renders the 3D viewport on a Vulkan backend that
mixes rasterization with an RTX **path tracer** (accumulation, adaptive
sampling, NEE/MIS, temporal accumulate-on-static/reset-on-move, OIDN/RTX
denoising). The research pass identified a prioritized set of Vulkan features
that can cut frame time in a CAD-style viewport (many small/static meshes,
camera-and-edit-driven, path-traced fill). This document is the concrete
implementation plan, in the agreed order:

1. TLAS instance culling + group geometry per BLAS
2. Async compute: OIDN + accumulation + AS build overlap
3. Bindless descriptors + buffer device address (hit groups / materials)
4. 16-bit AS vertex formats + AS compaction + per-instance push-constant transforms
5. FSR 2 upscaling + pipeline library (explicit pipeline precompile)

Sources for the technique rationale: the Khronos "Vulkan Ray Tracing Best
Practices for Hybrid Rendering" post (TLAS instance culling, geometry-grouping
per BLAS, 60% BLAS-count saving, 16-bit AS formats, traversal flags/Tmax,
buffer device address, bindless, pipeline library) and NVIDIA's
"Device-Generated Commands" post (occlusion culling, sorting, LOD), plus
AMD's mesh-shader best practices (with the `VK_EXT_mesh_shader` vs
`VK_NV_mesh_shader` caveat) and AMD FSR as the open upscaler.

## 2. Current architecture snapshot (evidence from the source)

Renderer lives in the **Coin submodule**:
`src/3rdParty/coin/src/rendering/SoRTXRenderBackend/` (split TUs), class
`SoRTXRenderBackend`, private helpers in `SoRTXRenderBackendP.h`, shaders in
`src/3rdParty/coin/data/shaders/vulkan/rt/`.

Key facts that drive the plan:

- **One graphics queue.** `this->queue` /
  `this->queueFamilyIndex = graphicsQueueFamilyIndex`
  (`SoRTXRenderBackendCore.cpp:380`). Every submission is synchronous:
  `vkQueueSubmit(...); vkQueueWaitIdle(...)` (denoise, core, geometry). No
  fences, no timeline semaphores, no compute queue. → Item 2.
- **BLAS build** (`buildBlas`/`refitBlas`, `SoRTXRenderBackendGeometry.cpp`):
  `triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT` (32-bit), flags
  `PREFER_FAST_TRACE | ALLOW_UPDATE`, `mode` BUILD vs UPDATE. No AS
  compaction. → Items 1 & 4.
- **TLAS build** (`buildTlas`, `SoRTXRenderBackendGeometry.cpp`): builds one
  `VkAccelerationStructureInstanceKHR` per cache entry (per draw command),
  `instanceCount = instances.size()`; a `[RTDBG] tlas` debug line already
  prints drawlist commands vs instance count. No angular-size culling. → Item 1.
- **Geometry cache**: `geometryCache[]` + `commandToCache` keyed by
  `SoRenderCommand`; `updateGeometryCache()` dedups identical content by hash
  and handles refit-pending vs new vs reused. → Items 1 & 4.
- **Two dispatch modes**: ray-query **compute pipeline** (default) or
  **SBT ray-tracing pipeline** (`useSbtPipeline`). → relevant to every item.
- **Descriptor sets:** `rtSetLayout`/`presentSetLayout`, double-buffered
  `rtDescriptorSets[2]`/`presentDescriptorSets[2]`, `descriptorSetIndex`.
  `RTMaterial[]` is one record per draw command, indexed by the instance
  **custom index** (command index). A single frame-block UBO + raygen push
  constant block (`RTXFrameBlock`, `RTXRaygenPush` in `SoRTXRenderBackendP.h`).
  → Items 3 & 4.
- **Denoiser**: OIDN (`COIN_BUILD_OIDN`) / RTX / "fsr" filter names via
  `setDenoiserFilter`. Denoise pass currently submits + waits on the main
  queue. → Items 2 & 5.
- **Temporal / adaptive / NEE**: `updatePathTracingState()`,
  `swapPathTracingHistory()`, `updateAdaptiveStats()`, `recordTraceAndPresent()`.
  Frame block carries `temporal`, `adaptive`, `nee` vec4 fields.
  → Item 5 (upscale) and measurement.

### 2.1 Important environment caveat (read before patching)

The backend is **mid-refactor**: the submodule is carrying uncommitted renames
(`SoVulkanRenderBackend…` ↔ `SoRTXRenderBackend…`), and several identifier
occurrences appear aliased to single letters (`l`, `n`) that resolve to the real
names via preprocessing. **Patch against the real, readable symbol names**
(`buildBlas`, `refitBlas`, `buildTlas`, `updateGeometryCache`, class
`SoRTXRenderBackend`) and treat any `l`/`n` you meet as that symbol. Expect
rebase churn if the rename lands before you finish; keep each patch small and
compile-checked (below) so re-applying is mechanical.

## 3. Implementation plan (in order)

### Item 1 — TLAS instance culling + group geometry per BLAS
**Why.** The TLAS currently holds one instance per draw command even when a
thousands-of-triangles object projects to a few pixels. Khronos: "the applica­
tion should never hold the entire scene in the TLAS" — cull by angular size and
group static geometries to cut BLAS count (reported 60% saving → faster
traversal).

**Change (TLAS culling).** In `buildTlas`, before appending an instance, project
the cache entry's world AABB into camera space and compute its angular size;
skip instances whose angular size falls below a threshold (configurable via
pref/env, e.g. `FC_VULKAN_TLAS_ANGLE_MIN`). Keep instance mapping (custom index)
stable so the SBT/`RTMaterial` index still matches the draw command — culling a
TLAS instance must NOT shift the material indexing (a skipped instance is simply
absent from the TLAS, not renumbered).

**Change (group geometry per BLAS).** Instead of one BLAS per command, co-bucket
static cache entries that share a `SoRenderCommand` pass/material into one BLAS
and reference them via `gl_GeometryIndex`/per-geometry SBT. This is the larger
sub-task; do it only after culling is landed and measured, behind
`FC_VULKAN_BLAS_GROUP=1`.

**Measure.** Extend the `[RTDBG] tlas` debug line with `instances= culled=`
(frames present a cullable instance count); add a `[RTDBG] blas` `mem=`
(compaction/grouping). Add a probe `vk_tlascull_probe.py` + `.check.py` that
renders N small far objects and asserts `culled>0` while a front, large object
still traces (no visible loss). Register in `vk_suite.json`.

**Risk.** Culling a historically-visible instance causes pop-in; gate strictly
and expose a pref. Rebuild `Coin FreeCADGui PartGui` (Coin render structs are
touched only if `SoRenderCommand`/geometry descriptors change — culling at the
TLAS layer does not).

### Item 2 — Async compute: OIDN + accumulation + AS build overlap
**Why.** Today the whole frame is `submit + waitIdle`: the denoiser (OIDN), the
accumulation/trace, and the AS builds serialize. On a 3D-queue-capable GPU these
can overlap on a compute queue with timeline semaphores, raising occupancy and
hiding the denoiser under the next frame's raster.

**Change.**
- Request a **compute queue** at device create
  (`deviceContext->computeQueueFamilyIndex` if present; else fall back to the
  graphics queue with a priority hint). Store `computeQueue`/`computeFamilyIndex`.
- Switch the **denoise** pass (`SoRTXRenderBackendDenoise.cpp`) and the
  **AS-build** command recording to run logically after the main submit on the
  compute queue, synchronized with a timeline semaphore (`VK_KHR_timeline_sema­
  phore`, enable the feature) instead of `vkQueueWaitIdle`.
- Keep a single-frame-latency model: submit raster/trace on graphics, then
  denoise on compute while the graphics queue starts the next frame.

**Measure.** `[RTDBG]` microseconds per stage (`waitsMs`, `denoiseMs`,
`buildMs`) so a probe can assert the compute work overlaps
(`denoiseMs` while still hitting a target frame count). Probe
`vk_async_probe.py` (+ `.check.py`) asserting the denoiser runs against a target
frame deadline; register in the suite.

**Risk.** Complexity + correctness (lifetime of buffers vs compute). Gate behind
`FC_VULKAN_ASYNC_COMPUTE=1`; the current single-queue synchronous path stays as
the default host/present path until proven. Only the compute-step is async; the
present path must keep its ordering guarantees. Widest VUID risk is
submission-time synchronization — the Khronos `synchronization2`/timeline doc is
the reference.

### Item 3 — Bindless descriptors + buffer device address
**Why.** RT hit groups indexed by a `RTMaterial` per command can hit binding
churn and deserialized descriptor updates; ray tracing wants a single scene
descriptor set where materials/textures/geometry are referenced by address.
`VK_KHR_buffer_device_address` + `VK_EXT_descriptor_indexing` enable pointer-like
access in hit shaders.

**Change.** Enable `bufferDeviceAddress` and `descriptorIndexing`
(and `runtimeDescriptorArray`/`shaderSampledImageArrayNonUniformIndexing`).
Rebuild the `rtSetLayout` scatterment: keep the accelerate-structure binding-0,
but fold `RTMaterial[]`, textures and vertex/index buffers into a small set of
bindless arrays referenced via `[[vk::ext_nonuniform]]`-style non-uniform
indexing (`VK_EXT_descriptor_indexing`). Read VB/IB through
`PhysicalStorageBuffer` (buffer device address) in the chit shaders so no
per-instance VB/IB binding is needed.

**Measure.** `[RTDBG]` descriptor-update count per frame; assert it drops to a
constant after bindless. Probe: many-material scene, assert
`descriptorUpdates` does not scale with draw count.

**Risk.** Shader rewrite (Raygen/ClosestHit/PathTrace/ShadowChit), SPIR-V layout
match with the `std430` `RTMaterial`. This is the highest-shader-touch item; do
it after 1–2 (culling + async) land so the rework is based on a stable frame.
Handle the `VK_EXT_descriptor_indexing` `NON_UNIFORM_INDEXING` requirement and
the pipeline layout/descriptor-set-layout ABI (rebuild `Coin FreeCADGui`).

### Item 4 — 16-bit AS formats + compaction + per-instance push constants
**Why.** `R32G32B32_SFLOAT` positions + no compaction: static CAD meshes can use
16-bit SNORM positions and 16-bit indices to halve AS memory and speed traversal;
compaction shrinks residency after build; per-instance transforms can ride a push
constant instead of descriptor reads. (Per-frame arrays already ride the raygen
push constant block / frame-block UBO.)

**Change.** In `buildBlas`/`refitBlas`, when `FC_VULKAN_AS_PACK=1` and geometry
qualifies (static, small coords), upload `VK_FORMAT_R16G16B16_SNORM` positions
and `VK_INDEX_TYPE_UINT16`; else keep 32-bit. Enable compaction
(`VK_ACCELERATION_STRUCTURE_COMPACTION` / `VK_KHR_acceleration_structure`,
`vkCmdBuildAccelerationStructuresIndirect`/copy compacted) for rebuilt ASes.
Move per-instance object-to-world into the push constant path where the ray-gen
block allows, keeping the transpose convention (SbMatrix is row-vector; use
`m[c][r]`, see build skill "transpose gotcha").

**Measure.** `[RTDBG] blas ... mem=<bytes>` and format counters (`packed=`);
a probe asserts `packed>0` for a static box and `mem` drops vs the 32-bit run
(compare two runs).

**Risk.** SNORM precision for far/moving geometry (keep 32-bit fallback),
compaction + `ALLOW_UPDATE` interplay (can't compact an update-built AS; run
compaction on the initial build), ABI (rebuild `Coin FreeCADGui PartGui` if any
`SoGeometryDesc`/buffer struct changes).

### Item 5 — FSR 2 upscaling + pipeline library
**Why.** Path-traced accumulation is the cost driver in a large viewport; FSR 2
(open source, AMD, Vulkan) temporal upscales a lower-resolution accumulation to
presentation, and FSR 3 does frame interpolation. Pipeline library + a warm
pipeline cache stop shader-compile stutter on first paint.

**Change.** Behind `FC_VULKAN_FSR=1`, render/trace+accumulate at
`scaleFactor` (e.g. 0.7) and run an FSR 2 upsample before present (a new
`Present`/post stage after the denoiser; reuse the "fsr" denoiser filter name
space carefully — FSR2-*upscale* is distinct from the FSR *denoise* filter).
Precompile the RT + present pipelines at engine init (store a
`VkPipelineCache`, and `VK_KHR_pipeline_library` for the present/RT group) so
the first render doesn't hitch.

**Measure.** Frame-dump pixels at the original resolution must match the
full-res baseline within tolerance (golden/parity probe), while
`[RTDBG]` reports the scaled resolution + upsample cost. A probe asserts the
upsampled frame converges to the reference (parity) within the existing frame
compare tolerances.

**Risk.** Visual fidelity (parity vs full-res); ordering with temporal
accumulation (FSR2 needs its own motion/history — it composes with the existing
reset-on-move); a new post stage after denoise. Highest-risk share of visual
regression; land last and behind `FC_VULKAN_FSR=1`. Uses AMD FSR (open), so no
proprietary runtime door on Linux. Verify with the parity frames probe + a
`vk_fsr_probe.py`.

## 4. Build & verification workflow

Use the `freecad-build` skill's rules. After **any** change:

- **Compile check (one TU):** run the TU's recorded command from
  `build/debug/compile_commands.json`; only `error:` lines matter — but a
  compile pass is *not* enough.
- **Full build:** `make -j$(nproc) Coin FreeCADGui PartGui > log 2>&1`, then
  `grep -iE 'error:|undefined reference' log`. Rebuild these as a unit whenever a
  Coin render/geometry struct changes (ABI trap). Never pipe `make` to `head`.
- **Runtime probe:** `env LD_LIBRARY_PATH=/tmp/opencode/boost91
  QT_STYLE_OVERRIDE=fusion QT_QPA_PLATFORM=xcb build/debug/bin/FreeCAD
  tools/fcprobe/vk_<probe>.py`. With `FC_VULKAN_DUMP_FRAME=1`,
  `FC_VULKAN_RT_DEBUG=1`, `FC_VULKAN_BREADCRUMBS=1` as needed.
- **Harness gate:** add/verify a `vk_<feat>_probe.py` + `.check.py` and a
  `vk_suite.json` entry (env = the flags that gate the feature). Every perf
  feature needs (a) a `[RTDBG]` counter it asserts on and (b) an on/off pair
  where the "off" is the control.
- **Regression baseline:** run `tools/fcprobe/vk_*.py` lint (clean) and
  `python3 tools/fcprobe/freecad_probe.py suite` to confirm the existing
  `blas*`, `adaptive-*`, `temporal-*`, `mis-*`, etc. still pass (no visual/blas
  regressions).

## 5. New measurement counters (to add alongside each item)

Extend the `[RTDBG]` debug lines the harness already parses, so probes can
assert improvements:

- Item 1 → `[RTDBG] tlas ... instances=<N> culled=<M>` (add to existing tlas line).
- Item 2 → stage timings `waitsMs= denoiseMs= buildMs=` (new line or fields).
- Item 3 → `descriptorUpdates=` (count per frame).
- Item 4 → `[RTDBG] blas ... packed=<n> mem=<bytes>` (extend existing line).
- Item 5 → `scale=<f> upsampleMs=<ms>` + the frames parity comparison.

Keep them behind `FC_VULKAN_RT_DEBUG=1` so release renders don't pay for it.

## 6. Cross-cutting risks

- **Mid-refactor renames** (§2.1): patch against real names; expect rebase churn.
- **ABI / rebuild trap:** any change to `SoRenderCommand`, `SoGeometryDesc`, the
  IR command headers, or a descriptor/layout struct requires rebuilding
  `Coin FreeCADGui PartGui` (and other render-emitting Gui libs).
- **Single-queue synchronous baseline:** keep the current path as the default;
  feature-gate everything (`FC_VULKAN_TLAS_CULL`, `FC_VULKAN_ASYNC_COMPUTE`,
  `FC_VULKAN_AS_PACK`, `FC_VULKAN_FSR`) so any regression is bisectable and the
  on/off probe pairs pass.
- **No on-GPU validation in CI here:** each item must be verified with the
  runtime probes (parity/pick/blas) — "compiles" is not a pass.
- **VUID churn:** new extension/feature enables and barrier/queue transitions can
  add Vulkan validation diagnostics; extend the suite `allow_vuid` list only for
  diagnostics owned by FreeCAD/Qt, never to hide a real new error (the harness
  surfaces non-allow-listed VUIDs as failures).

## 7. Deletion

When all five items are implemented, verified (build + suite + parity probes)
and the docs/skills note the new env flags and counters, **delete this file**:
`VULKAN_PERF_PLAN.md` (from the repo root). It must not remain in the tree.
