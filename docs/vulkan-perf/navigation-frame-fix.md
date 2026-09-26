# Vulkan viewport: navigation-frame cost

Where the time went on a navigation (moving-camera) frame, and what fixed it.
The GPU pass costs ~0.02 ms and the backend record phase ~3 ms, so the frame
was dominated by CPU-side scene-graph bookkeeping that ran on every frame.

Measured on a deterministic scene of 1600 `Part::Box` objects (3230 draw
commands), RasterVulkan, 1600x900, debug build, xcb.
`FC_VULKAN_FRAME_TIMING=1` emits the `[RTDBG] cpuTiming ...` and
`[RTDBG] extPhase ...` lines; `FC_GUI_OPEN_BREADCRUMB=1` emits the
`[VKRENDER] <start> <phase> dur_us=...` lines (capped at 30 lines per run, so a
per-frame phase shows as a full cap).

## Manager frame cost

| Phase (per navigation frame) | Before | After |
| --- | ---: | ---: |
| `computeGraphFingerprint` (`[VKRENDER]`) | every frame, ~58 ms | 2 warm-up runs only |
| `setClippingPlanes` (`cpuTiming clip`) | 0.58 ms | **0.19 ms** |
| external pre-pass wait (`extPhase prepassSubmit`) | ~12 ms | ~12 ms |

### Why

1. **`setSceneGraph()` re-armed the dirty sensor on the same root.**
   `QuarterVulkanRenderer::pushSceneState()` pushes the unchanged scene pointer
   every frame, and the setter had no same-root guard, so
   `sceneGraphChanged()` set `sceneGraphDirty` on every navigation frame. That
   forced the O(nodes) `computeGraphFingerprint()` walk (~58 ms on this scene)
   and invalidated the scene-fingerprint / bbox caches in
   `setClippingPlanes()` for the same reason. An early return when the root
   pointer is unchanged restores the retained-IR replay fast path.

2. **The external pre-pass wait drained the whole graphics queue.**
   `submitExternalPrepass()` submitted the texture-copy / geometry-LOD pre-pass
   and then called `vkQueueWaitIdle()`. The wait must stay on the host (the
   caller owns the submit, so no semaphore can be threaded through it), but it
   now waits on a dedicated `VkFence` so it does not also block on the caller's
   swapchain acquire/present. Queue submission is in-order, so this scopes the
   wait rather than shortening it: `prepassSubmit` is unchanged (~12 ms).

## Geometry LOD pre-pass

The sub-pixel geometry LOD (`interactionLod`, or forced with
`FC_VULKAN_GEOM_LOD_ALWAYS=1`) issued one indirect-cursor fill + barrier +
compute dispatch per eligible triangle command. A scene of many small parts
makes that pure fixed overhead: this bench has 1601 triangle commands of at most
16 primitives, so 1601 dispatches were spent culling 16-triangle boxes.

`GeometryLod.minPrims` (`FC_VULKAN_GEOM_LOD_MIN_PRIMS`, default 256; `0`
disables) now rejects commands below the threshold -- they draw in full -- and
the external pre-pass is skipped entirely when it compacted nothing and has no
texture copies (`recordGeometryLodPrepass()` returns its count;
`beginExternalPrepass()` reports "no pre-pass").

| Phase (LD forced) | Before | After |
| --- | ---: | ---: |
| geometry LOD (`cpuTimingRaster lod`) | ~23 ms, `compacted=1601` | **0.03 ms, `compacted=0`** |

Large meshes are unaffected: a 265k-primitive sphere still compacts
(`compacted=1`, `maxPrims=265174`); `FC_VULKAN_GEOM_LOD_MIN_PRIMS=0` restores the
old behaviour.

## Verification

- `FC_VULKAN_VALIDATION=1` with `FC_VULKAN_GEOM_LOD_ALWAYS=1` (forces the
  pre-pass every frame): `VUID=0`, clean exit.
- Ray-tracing smoke (PathTracing mode): `VUID=0`, frames render.
- Frame dumps: moving / hiding / adding a shape still changes the render, and
  camera pose changes still update, while a settled camera replays.
