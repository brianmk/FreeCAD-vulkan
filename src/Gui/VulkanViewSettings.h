// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

/** Central home for the Vulkan viewport's type definitions.
 *
 *  The ray-trace render mode and the single-source Vulkan display settings
 *  blob were embedded inline in the "regular" FreeCAD view classes
 *  (View3DInventor, View3DInventorViewer).  Extracting them here gives the
 *  render mode and the settings one definition shared by the view, the status
 *  bar selector and VulkanViewportAdapter, instead of scattering the Vulkan
 *  model through the view code.  The enum is only meaningful when the Vulkan
 *  renderer is built (FREECAD_USE_VULKAN); the settings struct is always
 *  defined so the non-Vulkan build keeps a no-op settings blob.
 */

#include <string>

#include <Base/Parameter.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/rendering/vulkan/SoVulkanViewMode.h>

namespace Gui {

#ifdef FREECAD_USE_VULKAN
/// Ray-traced view render mode for a 3D view.
/// 0 = Interactive (raster Coin) -- the default raster rendering, no ray
/// tracing; 1 = Interactive (raster Vulkan) -- Vulkan raster viewport;
/// 2 = Wireframe (raster); 3 = Ray Tracing (single-sample ray preview, no
/// progressive accumulation -- the cheapest RT mode); 4 = Path Tracing
/// (multi-bounce GI with progressive accumulation + denoising); 5 =
/// Environment (single-sample IBL preview); 6 = Path Tracing Max (the
/// accumulating path tracer extended with physically-based dielectric glass:
/// Fresnel + Snell refraction + total internal reflection + Beer-Lambert
/// absorption).  Mirrored by the status-bar selector in the main window; each
/// view keeps its own mode.  The two "Interactive" raster modes never enable
/// path tracing, ray tracing or the denoiser; the point overlay is likewise
/// raster-only, while the model edge overlay is honoured in every mode.
enum class ViewRenderMode : int {
    RasterCoin = 0,     // Interactive (raster Coin): classic Coin/GL raster
    RasterVulkan = 1,   // Interactive (raster Vulkan): Vulkan raster viewport
    Wireframe = 2,
    RayTracing = 3,     // single-sample ray preview (AO-style), no accumulate
    PathTracing = 4,    // full path tracer: progressive accumulation + denoise
    Environment = 5,
    PathTracingMax = 6, // path tracer + physically-based dielectric glass
};

//! Raster vs ray-traced render-mode categorization.
//! The render mode is stored both as a ViewRenderMode enum (View3DInventor)
//! and as VulkanViewSettings::renderMode (an int); this single encoding of the
//! raster boundary is anchored to the enum so the three raster mode names are
//! listed once instead of being re-derived by every caller (the old three-way
//! scattering of rasterOnly() plus the local "raster"/"rayTraced" lists in
//! setRenderMode).
constexpr bool isRasterMode(int renderMode) noexcept
{
    return renderMode >= 0
        && renderMode <= static_cast<int>(ViewRenderMode::Wireframe);
}
constexpr bool isRayTracedMode(int renderMode) noexcept
{
    return !isRasterMode(renderMode);
}

//! Map a ViewRenderMode to the renderer's SoVulkanViewMode.  The application
//! enum is a superset (it distinguishes the two raster backends and the
//! wireframe override), so this is the single place the two are related --
//! callers pass the result straight to the renderer with no magic ints.
constexpr SoVulkanViewMode viewRenderModeToWidgetMode(ViewRenderMode mode) noexcept
{
    switch (mode) {
        case ViewRenderMode::RayTracing:  return SoVulkanViewMode::RtxModeAmbientOcclusion;
        case ViewRenderMode::PathTracing: return SoVulkanViewMode::RtxModePathTrace;
        case ViewRenderMode::PathTracingMax:
            return SoVulkanViewMode::RtxModePathTraceMax;
        case ViewRenderMode::Environment: return SoVulkanViewMode::RtxModeEnvironment;
        default:                          return SoVulkanViewMode::RtxModeOff;
    }
}
#endif // FREECAD_USE_VULKAN

/** Vulkan view render settings -- the single source of truth for the viewport.
 *
 *  This is the canonical in-memory blob for every Vulkan render option: the
 *  render mode (raster Coin / raster Vulkan / wireframe / AO / path tracing /
 *  environment), the cubemap environment preset, and the display + path-tracing
 *  tuning.  View3DInventorViewer loads it from the user preferences in
 *  applyVulkanSettings() (emitting vulkanSettingsChanged), and
 *  VulkanViewportAdapter::pushSettings() is the single applier that reads it to
 *  drive the backends.  Consumers must read the mode / raster gate from here,
 *  never from a second copy.
 */
struct VulkanViewSettings
{
    // Render mode: Gui::ViewRenderMode as int (0 RasterCoin, 1 RasterVulkan,
    // 2 Wireframe, 3 RayTracing, 4 PathTracing, 5 Environment, 6
    // PathTracingMax).
    // Defaults to the Vulkan raster viewport.
    int renderMode = 1;
    // Cubemap environment preset index (-1 = viewport gradient/background).
    int envMap = -1;

    // True when the mode is a pure-raster mode (RasterCoin/RasterVulkan/
    // Wireframe).  The raster gate derived here tells the backends to keep
    // path tracing, ray tracing, the denoiser and the point overlay off
    // regardless of any persisted tuning (the model edge overlay is not gated:
    // it is honoured in every mode).  Delegates to the single isRasterMode()
    // categorization so the mode boundary has one definition.
    bool rasterOnly() const
    {
#ifdef FREECAD_USE_VULKAN
        return isRasterMode(renderMode);
#else
        return false;
#endif
    }

    //! Model feature-edge visibility (the black BRep edge lines) and the
    //! point-marker overlay.  The edge overlay is honoured in every Vulkan
    //! mode -- the raster main pass and the ray-tracing composite both gate
    //! their line residue on it; the point overlay stays raster-only.
    bool edgeOverlay = true;
    bool showPoints = false;
    //! HDR output for the Vulkan viewport, presented as scRGB: an FP16
    //! extended-linear sRGB swapchain (the representation Blender and gamescope
    //! use on Wayland) that the compositor color-manages through the
    //! wp_color_manager_v1 protocol.  Diffuse white is linear 1.0 (the
    //! compositor's reference white) and values above 1.0 carry HDR highlights.
    //! Only effective on a native Wayland session whose compositor exposes the
    //! color-management protocol and whose output is actually in an HDR mode;
    //! the viewport falls back to SDR otherwise.  Ignored by the classic
    //! Coin/OpenGL viewport.
    bool hdrEnabled = false;
    //! Diffuse-white gain for the scRGB output.  1.0 presents scene white
    //! (linear 1.0) at the compositor's reference white; > 1 brightens the
    //! whole viewport, < 1 darkens it.  HDR highlights remain above 1.0 and are
    //! mapped by the compositor/display.
    float hdrExposure = 1.0f;
    //! Highlight compression for the scene-linear ray-traced path (the raster
    //! path is display-referred and never tone-maps): 0 = clip (no tone map,
    //! highlights pass through above 1.0), 1 = Reinhard, 2 = ACES, 3 = Hable.
    //! The filmic operators compress the result into [0,1].  Ignored when HDR
    //! is off.
    int hdrToneMap = 0;
    //! Interaction LOD: while the camera is navigating, reduce per-frame work
    //! so an orbit/pan of a heavy scene stays responsive, then restore full
    //! quality once the camera settles.  Applies to every Vulkan mode: a
    //! ray-traced mode drops to a single-bounce preview, and a raster mode
    //! draws wide lines as plain 1px GPU lines instead of expanding every edge
    //! segment into quads on the CPU (the dominant navigation cost on large
    //! edge sets).  The classic Coin/GL viewport is unaffected.
    bool interactionLod = true;
    //! Swapchain present mode (V-Sync): 0 = FIFO (V-Sync, always supported --
    //! the historical default), 1 = Mailbox (V-Sync, no tearing, lower
    //! latency; frames may be dropped under load), 2 = Immediate (no V-Sync,
    //! may tear, uncapped frame rate).  A window/swapchain property, so it is
    //! applied when a view is created and an already-open view keeps its mode
    //! until reopened.  Falls back to FIFO when the surface does not advertise
    //! the requested mode.  Ignored by the classic Coin/OpenGL viewport.
    int presentMode = 0;
    SbColor4f edgeColor = SbColor4f(0.05f, 0.05f, 0.05f, 1.0f);
    // Path-tracing tuning (see the View preferences dialog).
    int pathTracingBounces = 4;
    int pathTracingSettleFrames = 6;
    int pathTracingMaxSamples = 256;
    // Denoiser backend name ("rtx", "oidn", "fsr", "none"); empty = default.
    // Denoising itself is required for path tracing and is enabled automatically
    // by the renderer; only the filter is configurable.
    std::string pathTracingDenoiser;
    // Denoiser upscale factor (>= 1).  A factor > 1 runs the host-side
    // denoiser at reduced resolution and the present pass upscales it back.
    float pathTracingDenoiserScale = 1.0f;
    // Physically-based glass parameters, used only by Path Tracing Max.  Any
    // material with transparency (alpha < 1) is treated as a smooth dielectric
    // with this index of refraction (1.5 = window glass); Beer-Lambert
    // absorption is derived from the material colour scaled by
    // pathTracingGlassAbsorption (0 = perfectly clear).
    float pathTracingGlassIor = 1.5f;
    float pathTracingGlassAbsorption = 0.1f;

    // Load the whole Vulkan display preference set from the View preferences
    // group.  Single home for the "which pref key + which type" mapping so the
    // struct fields and the preference names cannot drift: every consumer
    // (applyVulkanSettings, the pref-change observer, the initial-apply path)
    // funnels through here instead of re-enumerating the keys.  Adding a field
    // is one line here plus one field above.
    void load(const ParameterGrp::handle & hGrp);

    // True when \a reason names a Vulkan viewport display preference (any
    // "Vulkan*" key).  The backend-choice pref ("UseVulkanRenderer") is
    // deliberately excluded by the prefix, so
    // View3DSettings::OnChange can route any "Vulkan*" change straight to
    // applyVulkanSettings() without enumerating every key -- a new display pref
    // is picked up automatically.
    static bool isDisplayPref(const char * reason);
};

} // namespace Gui
