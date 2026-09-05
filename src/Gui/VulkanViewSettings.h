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

namespace Gui {

#ifdef FREECAD_USE_VULKAN
/// Ray-traced view render mode for a 3D view.
/// 0 = Interactive (raster Coin) -- the default raster rendering, no ray
/// tracing; 1 = Interactive (raster Vulkan) -- Vulkan raster viewport;
/// 2 = Wireframe (raster); 3 = Ray Tracing (single-sample ray preview, no
/// progressive accumulation -- the cheapest RT mode); 4 = Path Tracing
/// (multi-bounce GI with progressive accumulation + denoising); 5 =
/// Environment (single-sample IBL preview).  Mirrored by the status-bar
/// selector in the main window; each view keeps its own mode.  The two
/// "Interactive" raster modes never enable path tracing, ray tracing, the
/// denoiser or the edge/point overlays.
enum class ViewRenderMode : int {
    RasterCoin = 0,     // Interactive (raster Coin): classic Coin/GL raster
    RasterVulkan = 1,   // Interactive (raster Vulkan): Vulkan raster viewport
    Wireframe = 2,
    RayTracing = 3,     // single-sample ray preview (AO-style), no accumulate
    PathTracing = 4,    // full path tracer: progressive accumulation + denoise
    Environment = 5,
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

//! Map a ViewRenderMode to the Vulkan viewport widget's own ray-traced
//! view-mode int (0 = raster, 1 = RayTracing/AO, 2 = PathTracing, 3 =
//! Environment).  The two enums are intentionally distinct (the widget mode is
//! a subset), so this single mapping replaces the magic ints that used to be
//! hardcoded in each setRenderMode() branch.
constexpr int viewRenderModeToWidgetMode(ViewRenderMode mode) noexcept
{
    switch (mode) {
        case ViewRenderMode::RayTracing:  return 1;  // widget AO shader
        case ViewRenderMode::PathTracing: return 2;
        case ViewRenderMode::Environment: return 3;
        default:                          return 0;  // RasterCoin/RasterVulkan/Wireframe
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
    // 2 Wireframe, 3 RayTracing, 4 PathTracing, 5 Environment).
    // Defaults to the Vulkan raster viewport.
    int renderMode = 1;
    // Cubemap environment preset index (-1 = viewport gradient/background).
    int envMap = -1;

    // True when the mode is a pure-raster mode (RasterCoin/RasterVulkan/
    // Wireframe).  The raster gate derived here tells the backends to keep
    // path tracing, ray tracing, the denoiser and the edge/point overlays
    // off regardless of any persisted tuning.  Delegates to the single
    // isRasterMode() categorization so the mode boundary has one definition.
    bool rasterOnly() const
    {
#ifdef FREECAD_USE_VULKAN
        return isRasterMode(renderMode);
#else
        return false;
#endif
    }

    bool showEdges = false;
    bool showPoints = false;
    SbColor4f edgeColor = SbColor4f(0.05f, 0.05f, 0.05f, 1.0f);
    bool pathTracing = false;
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

    // Load the whole Vulkan display preference set from the View preferences
    // group.  Single home for the "which pref key + which type" mapping so the
    // struct fields and the preference names cannot drift: every consumer
    // (applyVulkanSettings, the pref-change observer, the initial-apply path)
    // funnels through here instead of re-enumerating the keys.  Adding a field
    // is one line here plus one field above.
    void load(const ParameterGrp::handle & hGrp);

    // True when \a reason names a Vulkan viewport display preference (any
    // "Vulkan*" key).  The backend-choice prefs ("UseVulkanRenderer",
    // "UseVulkanRayTracing") are deliberately excluded by the prefix, so
    // View3DSettings::OnChange can route any "Vulkan*" change straight to
    // applyVulkanSettings() without enumerating every key -- a new display pref
    // is picked up automatically.
    static bool isDisplayPref(const char * reason);
};

} // namespace Gui
