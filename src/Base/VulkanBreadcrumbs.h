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
// the overhead and log volume stay zero unless FC_VULKAN_BREADCRUMBS is set:
//
//   VK_BREADCRUMB(fmt, ...)          log every call
//   VK_BREADCRUMB_ONCE(fmt, ...)     log only the first call at this site
//   VK_BREADCRUMB_LIMITED(n, ...)    log at most n calls at this site
//
// The rate-limited variants keep their counters in a function-local static,
// so the limit applies per call site, not globally.

#ifndef BASE_VULKANBREADCRUMBS_H
#define BASE_VULKANBREADCRUMBS_H

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace Base {

//! Append a formatted breadcrumb to the trace log.
//!
//! The destination file is created/truncated on the first call of each
//! process, then appended to afterwards.  Override the path with the
//! FC_VULKAN_TRACE_FILE environment variable (default:
//! /tmp/freecad_vulkan_trace.log).
inline void vulkanBreadcrumb(const char* fmt, ...)
{
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
        if (std::getenv("FC_VULKAN_BREADCRUMBS")) {                            \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_ONCE(...)                                                \
    do {                                                                       \
        static bool logged_ = false;                                           \
        if (!logged_ && std::getenv("FC_VULKAN_BREADCRUMBS")) {                \
            logged_ = true;                                                    \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#define VK_BREADCRUMB_LIMITED(limit, ...)                                      \
    do {                                                                       \
        static int logged_ = 0;                                                \
        if (logged_ < (limit) && std::getenv("FC_VULKAN_BREADCRUMBS")) {       \
            ++logged_;                                                         \
            ::Base::vulkanBreadcrumb(__VA_ARGS__);                             \
        }                                                                      \
    } while (0)

#endif  // BASE_VULKANBREADCRUMBS_H
