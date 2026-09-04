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
// the log volume stays zero unless FC_VULKAN_BREADCRUMBS is set.  The check
// itself is cached on first use, so enabled or not the per-call cost is a
// single predictable branch:
//
//   VK_BREADCRUMB(fmt, ...)          log every call
//   VK_BREADCRUMB_ONCE(fmt, ...)     log only the first call at this site
//   VK_BREADCRUMB_LIMITED(n, ...)    log at most n calls at this site
//
// The rate-limited variants keep their counters in a function-local static,
// so the limit applies per call site, not globally.

#pragma once

// MSVC marks the standard CRT <cstdlib>/<cstdio> functions (getenv, fopen,
// snprintf) as "unsafe" and with /WX this C4996 ends the build.  The code
// intentionally uses the portable standard functions everywhere rather than
// the MSVC-only _s variants, so silence the deprecation for this header.
#ifdef _MSC_VER
#    pragma warning(disable : 4996)
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Base {

//! True when \a name is present in the environment.  The lookup is cached on
//! first use: these helpers sit on hot paths (per-frame setup, per-mouse-event
//! filtering) and the environment does not change during a process lifetime.
inline bool envFlagEnabled(const char* name)
{
    static const bool enabled = std::getenv(name) != nullptr;
    return enabled;
}

//! Like envFlagEnabled() but treats the values "0", "false" and "off" as
//! disabled, matching the boolean switch convention of the FC_* env vars.
inline bool envFlagTruthy(const char* name)
{
    static const bool enabled = [name]() {
        const char* value = std::getenv(name);
        if (!value || !*value) {
            return false;
        }
        return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0
            && std::strcmp(value, "off") != 0;
    }();
    return enabled;
}

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
        if (!path || !*path) {
            path = "/tmp/freecad_vulkan_trace.log";
        }
        // Create/truncate the log on the first call of each process, so a
        // fresh FreeCAD run starts with an empty file.  Fall back to the
        // user's home directory if the default path is not writable.
        FILE* f = std::fopen(path, "w");
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
                         path);
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

#define VK_BREADCRUMB(...)                                                     \
    do {                                                                       \
        if (::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS")) {                 \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_ONCE(...)                                                \
    do {                                                                       \
        static bool logged_ = false;                                           \
        if (!logged_ && ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS")) {     \
            logged_ = true;                                                    \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_LIMITED(limit, ...)                                      \
    do {                                                                       \
        static int logged_ = 0;                                                \
        if (logged_ < (limit)                                                  \
            && ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS")) {              \
            ++logged_;                                                         \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

// Log every stride-th call at this site.  For per-event hot paths (e.g. mouse
// motion) this keeps a representative, evenly spread sample instead of one line
// per event or only the first N calls.  The counter is per call site.
#define VK_BREADCRUMB_SAMPLED(stride, ...)                                     \
    do {                                                                       \
        static int count_ = 0;                                                 \
        if ((++count_ % (stride)) == 0                                         \
            && ::Base::envFlagEnabled("FC_VULKAN_BREADCRUMBS")) {              \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

// Document the available variants: VK_BREADCRUMB (every call), VK_BREADCRUMB_ONCE
// (first call), VK_BREADCRUMB_LIMITED(n) (first n calls), VK_BREADCRUMB_SAMPLED(n)
// (every nth call).
