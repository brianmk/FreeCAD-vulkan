// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "VulkanViewSettings.h"

#include <algorithm>
#include <cstring>

namespace Gui {

//! Load the entire Vulkan viewport display preference set from a View group.
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
    this->envMap = hGrp->GetInt("VulkanEnvironmentMap", -1);

    this->showEdges = hGrp->GetBool("VulkanShowEdges", false);
    this->showPoints = hGrp->GetBool("VulkanShowPoints", false);
    // Colors are stored as Unsigned (0xAABBGGRR) to survive INT_MAX; the
    // alpha is pinned to 1 (the edge overlay is opaque).
    const unsigned long color = hGrp->GetUnsigned("VulkanEdgeColor", 0x050505FFUL);
    this->edgeColor = SbColor4f(
        static_cast<float>((color >> 24) & 0xff) / 255.0f,
        static_cast<float>((color >> 16) & 0xff) / 255.0f,
        static_cast<float>((color >> 8) & 0xff) / 255.0f,
        1.0f);

    this->pathTracing = hGrp->GetBool("VulkanPathTracing", false);
    this->pathTracingBounces = std::clamp(
        static_cast<int>(hGrp->GetInt("VulkanPathTracingBounces", 4)), 1, 16);
    this->pathTracingSettleFrames = std::clamp(
        static_cast<int>(hGrp->GetInt("VulkanPathTracingSettle", 6)), 1, 120);
    this->pathTracingMaxSamples = std::clamp(
        static_cast<int>(hGrp->GetInt("VulkanPathTracingMaxSamples", 256)),
        1, 4096);
    // Denoiser backend, stored as the combo index (0=RTX, 1=OIDN, 2=FSR,
    // 3=None); map to the backend name the RT renderer expects.
    switch (hGrp->GetInt("VulkanPathTracingDenoiser", 0)) {
        case 1:
            this->pathTracingDenoiser = "oidn";
            break;
        case 2:
            this->pathTracingDenoiser = "fsr";
            break;
        case 3:
            this->pathTracingDenoiser = "none";
            break;
        default:
            this->pathTracingDenoiser = "rtx";
            break;
    }
    this->pathTracingDenoiserScale = std::clamp(
        static_cast<float>(hGrp->GetFloat("VulkanPathTracingDenoiserScale", 1.0f)),
        1.0f, 8.0f);
}

//! True when \a reason names a "Vulkan*" display preference.
//! All of the viewport display + path-tracing tuning prefs are prefixed
//! "Vulkan"; the backend-choice prefs are "UseVulkan*", so a single prefix
//! test is an exact, future-proof trigger for re-applying the settings.
bool
VulkanViewSettings::isDisplayPref(const char * reason)
{
    return reason && std::strncmp(reason, "Vulkan", 6) == 0;
}

} // namespace Gui
