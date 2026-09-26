// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <Base/VulkanBreadcrumbs.h>

namespace Gui
{

/** Developer / CI diagnostics for the Vulkan viewport.
 *
 *  These are deliberately environment variables and NOT preferences: they are
 *  not user settings, produce developer-only output or behaviour, and must not
 *  be persisted into a user configuration.  They are the single place the
 *  Gui-side `FC_VULKAN_*` debug hook names live, so the namespace of developer
 *  switches can be seen (and documented) at a glance instead of being scraped
 *  out of scattered `getenv()` calls.
 *
 *  User-visible runtime behaviour belongs in the View preferences instead -- see
 *  Gui::VkRuntimePrefs (persistent resources, pick throttles).  Some hooks are
 *  additionally gated behind the `FREECAD_VULKAN_DEBUG_HOOKS` build option
 *  (auto-on for Debug builds), so release/product builds carry no injector.
 *
 *  The process-global `FC_SKIP_UNSAVED_PROMPT` (CI/test only) and the renderer's
 *  internal `FC_VULKAN_*` tuning overrides (consumed inside bundled Coin, which
 *  has no ParameterDB access) also stay environment variables by design.
 */
namespace VkDebug
{

//! `FC_VULKAN_BACKEND_DEBUG`: per-command draw/material/push-constant logs.
inline bool backendDebug()
{
    return Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG");
}

//! `FC_VULKAN_VALIDATION`: enable the Khronos Vulkan validation layers.
inline bool validation()
{
    return Base::envFlagEnabled("FC_VULKAN_VALIDATION");
}

//! `FC_VULKAN_RT_DEBUG`: ray-tracing backend diagnostics.
inline bool rtDebug()
{
    return Base::envFlagEnabled("FC_VULKAN_RT_DEBUG");
}

//! `FC_VULKAN_BREADCRUMBS`: mirror the GUI-side trace to the trace file.
inline bool breadcrumbs()
{
    return Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS");
}

//! `FC_VULKAN_DEBUG_PRINTF`: route renderer diagnostics through debug printf.
inline bool debugPrintf()
{
    return Base::envFlagEnabled("FC_VULKAN_DEBUG_PRINTF");
}

//! `FC_LIGHT_TRACE`: trace the GL-viewer scene light derivation.
inline bool lightTrace()
{
    return Base::envFlagEnabled("FC_LIGHT_TRACE");
}

//! `FC_VULKAN_AXIS_DEBUG`: axis-cross overlay diagnostics.
inline bool axisDebug()
{
    return Base::envFlagEnabled("FC_VULKAN_AXIS_DEBUG");
}

//! `FC_VULKAN_DUMP_FRAME`: dump presented swapchain frames to PNG.
inline bool dumpFrame()
{
    return Base::envFlagEnabled("FC_VULKAN_DUMP_FRAME");
}

}  // namespace VkDebug
}  // namespace Gui
