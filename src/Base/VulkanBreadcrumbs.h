// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Vulkan render/pick diagnostic breadcrumbs.
//
// A single variadic helper that appends formatted lines to a log file.  The
// file is truncated by the first call in each process, so launching FreeCAD
// starts a fresh log.  Useful for correlating mouse-position, pick-ray and
// highlight events when the app runs under a different display (Vulkan) than
// the one handling input (the hidden OpenGL viewer).
//
// The VK_BREADCRUMB macros guard each call site with an environment check so
// the log volume stays zero unless FC_VULKAN_BREADCRUMBS is set.  The check is
// cached per call site (see the macros below), so enabled or not the per-call
// cost is a single predictable branch:
//
//   VK_BREADCRUMB(fmt, ...)          log every call
//   VK_BREADCRUMB_ONCE(fmt, ...)     log only the first call at this site
//   VK_BREADCRUMB_LIMITED(n, ...)    log at most n calls at this site
//
// The rate-limited variants keep their counters in a function-local static,
// so the limit applies per call site, not globally.
//
// Shipping policy: the trace machinery (the file open/truncate, the mutex and
// the formatted writes) is compiled only when FREECAD_VULKAN_DEBUG_HOOKS is
// defined.  That macro is set by the build for Debug configurations and for
// research builds configured with -DFREECAD_USE_VULKAN_DEBUG_HOOKS=ON (see the
// top-level CMakeLists.txt), so a product Release build carries neither the
// /tmp log nor the instrumentation cost.  In that configuration the macros
// expand to a no-op sink with the same signature, keeping the call sites
// warning-clean.

#pragma once

#include <cstdlib>
#include <cstring>

namespace Base {

//! Single choke point for every FC_VULKAN_* / FC_GUI_* environment read in the
//! Gui/Base side, mirroring SoVulkanShared's helpers inside Coin.  The two
//! libraries are independent (Base is built before Coin and cannot include its
//! headers), so the policy is duplicated by necessity but kept identical.
//!
//! These helpers are always compiled: they also drive user-facing runtime
//! options (e.g. FC_VULKAN_VALIDATION, FC_VULKAN_PERSISTENT_RESOURCES) that are
//! not debug scaffolding.
//!
//! NOTE: these functions deliberately do NOT cache in a function-local
//! `static`: such a static is initialized once for the whole program, so the
//! first name passed would be returned for every later, different name.  The
//! VK_BREADCRUMB macros cache the result at each call site instead.

//! True when \a name is present in the environment (any value, including "0").
inline bool envFlagEnabled(const char* name)
{
    return std::getenv(name) != nullptr;
}

//! Like envFlagEnabled() but treats the values "0", "false" and "off" as
//! disabled, matching the boolean switch convention of the FC_* env vars.
//! Returns \a defaultValue when the variable is unset or empty.
inline bool envFlagTruthy(const char* name, bool defaultValue = false)
{
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return defaultValue;
    }
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0
        && std::strcmp(value, "off") != 0;
}

//! Raw value (or nullptr); \a name set to any value counts as set.
inline const char* envString(const char* name)
{
    return std::getenv(name);
}

//! Integer value, or \a defaultValue when unset/empty.
inline int envInt(const char* name, int defaultValue = 0)
{
    const char* value = std::getenv(name);
    return value ? std::atoi(value) : defaultValue;
}

//! Floating-point value, or \a defaultValue when unset/empty.
inline float envFloat(const char* name, float defaultValue = 0.0f)
{
    const char* value = std::getenv(name);
    return value ? static_cast<float>(std::atof(value)) : defaultValue;
}

}  // namespace Base

#ifdef FREECAD_VULKAN_DEBUG_HOOKS

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include <Base/FileInfo.h>

namespace Base {

//! Append a formatted breadcrumb to the trace log.
//!
//! The destination file is created/truncated on the first call of each
//! process, then appended to afterwards.  Override the path with the
//! FC_VULKAN_TRACE_FILE environment variable (default:
//! /tmp/freecad_vulkan_trace.log).  Calls are serialized so concurrent GUI
//! and render threads cannot interleave lines; the log is flushed per call.
inline void vulkanBreadcrumb(const char* fmt, ...)
{
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    static FILE* log = []() -> FILE* {
        const char* path = std::getenv("FC_VULKAN_TRACE_FILE");
        std::string fullPath;
        if (path && *path) {
            fullPath = path;
        }
        else {
            // Use the OS temp dir (Base::FileInfo::getTempPath) rather than a
            // hardcoded /tmp so the log lands somewhere writeable on
            // Windows/macOS too.
            fullPath = Base::FileInfo::getTempPath() + "freecad_vulkan_trace.log";
        }
        // Create/truncate the log on the first call of each process, so a
        // fresh FreeCAD run starts with an empty file.  Fall back to the
        // user's home directory if the temp path is not writable.
        FILE* f = std::fopen(fullPath.c_str(), "w");
        if (!f) {
            const char* home = std::getenv("HOME");
            if (home && *home) {
                static char fallback[512];
                std::snprintf(fallback, sizeof(fallback), "%s/freecad_vulkan_trace.log", home);
                f = std::fopen(fallback, "w");
            }
        }
        if (!f) {
            std::fprintf(stderr,
                         "[VK-TRACE] vulkanBreadcrumb: cannot create log file "
                         "'%s'\n",
                         fullPath.c_str());
        }
        return f;
    }();
    if (!log) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(log, fmt, args);
    va_end(args);
    std::fflush(log);
}

}  // namespace Base

// The gate is resolved once per call site (not per call): the environment is
// fixed for the process lifetime, and these macros sit on per-frame / per-event
// hot paths.
#define VK_BREADCRUMB(...)                                                     \
    do {                                                                       \
        static const bool vk_breadcrumbs_enabled_ =                            \
            ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS");                   \
        if (vk_breadcrumbs_enabled_) {                                         \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_ONCE(...)                                                \
    do {                                                                       \
        static const bool vk_breadcrumbs_enabled_ =                            \
            ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS");                   \
        static bool logged_ = false;                                           \
        if (!logged_ && vk_breadcrumbs_enabled_) {                             \
            logged_ = true;                                                    \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_LIMITED(limit, ...)                                      \
    do {                                                                       \
        static const bool vk_breadcrumbs_enabled_ =                            \
            ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS");                   \
        static int logged_ = 0;                                                \
        if (logged_ < (limit) && vk_breadcrumbs_enabled_) {                    \
            ++logged_;                                                         \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

// Log every stride-th call at this site.  For per-event hot paths (e.g. mouse
// motion) this keeps a representative, evenly spread sample instead of one line
// per event or only the first N calls.  The counter is per call site.
#define VK_BREADCRUMB_SAMPLED(stride, ...)                                     \
    do {                                                                       \
        static const bool vk_breadcrumbs_enabled_ =                            \
            ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS");                   \
        static int count_ = 0;                                                 \
        if ((++count_ % (stride)) == 0 && vk_breadcrumbs_enabled_) {           \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#else  // !FREECAD_VULKAN_DEBUG_HOOKS

namespace Base {

//! Product Release build without the Vulkan debug hooks: a no-op sink with the
//! same signature.  Keeping it a real function (instead of a macro that drops
//! its arguments) means the call sites' format arguments are still referenced,
//! so they compile cleanly under -Wunused, while no FILE, /tmp path, mutex or
//! formatted write exists in the binary.  The optimizer removes the calls.
inline void vulkanBreadcrumb(const char*, ...) noexcept
{
}

}  // namespace Base

#define VK_BREADCRUMB(...)                                                     \
    ::Base::vulkanBreadcrumb(__VA_ARGS__)
#define VK_BREADCRUMB_ONCE(...)                                                \
    ::Base::vulkanBreadcrumb(__VA_ARGS__)
#define VK_BREADCRUMB_LIMITED(limit, ...)                                      \
    ::Base::vulkanBreadcrumb(__VA_ARGS__)
#define VK_BREADCRUMB_SAMPLED(stride, ...)                                     \
    ::Base::vulkanBreadcrumb(__VA_ARGS__)

#endif  // FREECAD_VULKAN_DEBUG_HOOKS

// Document the available variants: VK_BREADCRUMB (every call), VK_BREADCRUMB_ONCE
// (first call), VK_BREADCRUMB_LIMITED(n) (first n calls), VK_BREADCRUMB_SAMPLED(n)
// (every nth call).
