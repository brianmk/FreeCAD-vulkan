// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

/** Central home for the Vulkan viewport's type definitions.
 *
 *  The render mode and the single-source Vulkan display settings blob were
 *  embedded inline in the "regular" FreeCAD view classes (View3DInventor,
 *  View3DInventorViewer).  Extracting them here gives the render mode and the
 *  settings one definition shared by the view, the status bar selector and
 *  VulkanViewportAdapter.  The enum is only meaningful when the Vulkan renderer
 *  is built (FREECAD_USE_VULKAN); the settings struct is always defined so the
 *  non-Vulkan build keeps a no-op settings blob.
 */

#include <string>

#include <Base/Parameter.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/rendering/SoVulkanViewMode.h>

namespace Gui {

#ifdef FREECAD_USE_VULKAN
/// View render mode for a 3D view.  This port carries the raster Vulkan
/// renderer only: 0 = Interactive (raster Coin) -- the classic Coin/GL raster
/// view; 1 = Interactive (raster Vulkan) -- the Vulkan raster viewport;
/// 2 = Wireframe (raster).  Mirrored by the status-bar selector in the main
/// window; each view keeps its own mode.
enum class ViewRenderMode : int {
    RasterCoin = 0,     // Interactive (raster Coin): classic Coin/GL raster
    RasterVulkan = 1,   // Interactive (raster Vulkan): Vulkan raster viewport
    Wireframe = 2,
};

//! Raster render-mode categorization.  The render mode is stored both as a
//! ViewRenderMode enum (View3DInventor) and as VulkanViewSettings::renderMode
//! (an int); this single encoding of the boundary is anchored to the enum.
constexpr bool isRasterMode(int renderMode) noexcept
{
    return renderMode >= 0
        && renderMode <= static_cast<int>(ViewRenderMode::Wireframe);
}

//! Map a ViewRenderMode to the renderer's SoVulkanViewMode.  The application
//! enum distinguishes the two raster backends and the wireframe override; this
//! single place relates the two so callers pass the result to the renderer with
//! no magic ints.
constexpr SoVulkanViewMode viewRenderModeToWidgetMode(ViewRenderMode mode) noexcept
{
    (void)mode;
    return SoVulkanViewMode::Raster;
}
#endif // FREECAD_USE_VULKAN

/** Vulkan view render settings -- the single source of truth for the viewport.
 *
 *  This is the canonical in-memory blob for the Vulkan render options: the
 *  render mode (raster Coin / raster Vulkan / wireframe) and the display
 *  tuning.  View3DInventorViewer loads it from the user preferences in
 *  applyVulkanSettings() (emitting vulkanSettingsChanged), and
 *  VulkanViewportAdapter::pushSettings() is the single applier that reads it to
 *  drive the backend.  Consumers must read the mode from here, never from a
 *  second copy.
 */
struct VulkanViewSettings
{
    // Render mode: Gui::ViewRenderMode as int (0 RasterCoin, 1 RasterVulkan,
    // 2 Wireframe).  Defaults to the Vulkan raster viewport.
    int renderMode = 1;

    // True when the mode is a pure-raster mode (RasterCoin/RasterVulkan/
    // Wireframe).  Delegates to the single isRasterMode() categorization so the
    // mode boundary has one definition.
    bool rasterOnly() const
    {
#ifdef FREECAD_USE_VULKAN
        return isRasterMode(renderMode);
#else
        return false;
#endif
    }

    //! Model feature-edge visibility (the black BRep edge lines) and the
    //! point-marker overlay.
    bool edgeOverlay = true;
    bool showPoints = false;
    //! Interaction LOD: while the camera is navigating, reduce per-frame work
    //! so an orbit/pan of a heavy scene stays responsive, then restore full
    //! quality once the camera settles.  In raster mode this draws wide lines
    //! as plain 1px GPU lines instead of expanding every edge segment into
    //! quads on the CPU (the dominant navigation cost on large edge sets).
    bool interactionLod = true;
    SbColor4f edgeColor = SbColor4f(0.05f, 0.05f, 0.05f, 1.0f);

    // Load the whole Vulkan display preference set from the View preferences
    // group.  Single home for the "which pref key + which type" mapping so the
    // struct fields and the preference names cannot drift.
    void load(const ParameterGrp::handle & hGrp);

    // True when \a reason names a Vulkan viewport display preference (any
    // "Vulkan*" key).  The backend-choice pref ("UseVulkanRenderer") is
    // deliberately excluded by the prefix, so View3DSettings::OnChange can
    // route any "Vulkan*" change straight to applyVulkanSettings() without
    // enumerating every key.
    static bool isDisplayPref(const char * reason);
};

} // namespace Gui
