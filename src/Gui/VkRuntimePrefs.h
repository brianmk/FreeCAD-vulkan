// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <App/Application.h>
#include <Base/Parameter.h>

namespace Gui
{

/** Runtime-behaviour knobs of the 3D viewport.
 *
 *  These used to be developer environment variables
 *  (`FC_VULKAN_PERSISTENT_RESOURCES`, `FC_VULKAN_PICK_THROTTLE_PX`,
 *  `FC_VULKAN_PICK_THROTTLE_MS`) read with `getenv()` at scattered call
 *  sites.  They change user-visible runtime behaviour (resource lifetime,
 *  hover-pick responsiveness), so they belong in the persisted View
 *  preferences rather than in a restart-time environment.
 *
 *  They are read once per process, exactly like the environment variables they
 *  replace: the consumers treat them as startup tuning, not live settings.
 *  Developer/CI-only diagnostics stay environment variables on purpose -- see
 *  `Base/VulkanBreadcrumbs.h` and the `FC_VULKAN_*` debug hooks -- because they
 *  are not user preferences and must not be persisted into a user config.
 */
namespace VkRuntimePrefs
{

//! Keep the Vulkan backend alive across expose/hide cycles
//! (QVulkanWindow::PersistentResources).  On by default; turning it off makes
//! every document restore re-initialize the backend.
inline bool persistentResources()
{
    static const bool enabled = [] {
        auto hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View");
        return hGrp->GetBool("VulkanPersistentResources", true);
    }();
    return enabled;
}

//! Minimum cursor travel (device px) before a hover preselection pick is
//! re-run.  0 disables the cursor-delta throttle.
inline int pickThrottleDeltaPx()
{
    static const int value = [] {
        auto hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View");
        return static_cast<int>(hGrp->GetInt("VulkanPickThrottlePx", 3));
    }();
    return value;
}

//! Minimum interval (ms) between hover preselection picks.  0 disables the
//! rate throttle.
inline int pickThrottleIntervalMs()
{
    static const int value = [] {
        auto hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View");
        return static_cast<int>(hGrp->GetInt("VulkanPickThrottleMs", 12));
    }();
    return value;
}

}  // namespace VkRuntimePrefs
}  // namespace Gui
