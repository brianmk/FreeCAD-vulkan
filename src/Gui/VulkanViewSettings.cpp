// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "VulkanViewSettings.h"

#include <cstring>

namespace Gui {

//! Load the Vulkan viewport display preference set from a View group.
//! This is the single source of truth for the pref-key/type mapping, kept
//! beside the struct fields so an added setting is exactly one field above
//! plus one line here (and is automatically re-applied via isDisplayPref()).
void
VulkanViewSettings::load(const ParameterGrp::handle & hGrp)
{
    if (!hGrp) {
        return;
    }
    this->renderMode = hGrp->GetInt("VulkanRenderMode", 1);

    // VulkanWireframe backs the status-bar "show model edges" toggle: true
    // (the historical default look) shows the BRep feature edges, false hides
    // them.  Default true so an unset preference keeps the edges visible.
    this->edgeOverlay = hGrp->GetBool("VulkanWireframe", true);
    this->showPoints = hGrp->GetBool("VulkanShowPoints", false);
    this->interactionLod = hGrp->GetBool("VulkanInteractionLod", true);
    // Colors are stored as Unsigned (0xRRGGBBAA) to survive INT_MAX; the
    // alpha is pinned to 1 (the edge overlay is opaque).
    //
    // The edge overlay draws every edge with a single uniform color, so unless
    // VulkanEdgeColor is set it defaults to the Coin/OpenGL default shape line
    // color (DefaultShapeLineColor), keeping the two viewports' edge color the
    // same out of the box.
    const unsigned long glEdgeColor =
        hGrp->GetUnsigned("DefaultShapeLineColor", 255UL);
    const unsigned long color = hGrp->GetUnsigned("VulkanEdgeColor", glEdgeColor);
    this->edgeColor = SbColor4f(
        static_cast<float>((color >> 24) & 0xff) / 255.0f,
        static_cast<float>((color >> 16) & 0xff) / 255.0f,
        static_cast<float>((color >> 8) & 0xff) / 255.0f,
        1.0f);
}

//! True when \a reason names a "Vulkan*" display preference.
//! All of the viewport display prefs are prefixed "Vulkan"; the backend-choice
//! prefs are "UseVulkan*", so a single prefix test is an exact, future-proof
//! trigger for re-applying the settings.
bool
VulkanViewSettings::isDisplayPref(const char * reason)
{
    return reason && std::strncmp(reason, "Vulkan", 6) == 0;
}

} // namespace Gui
