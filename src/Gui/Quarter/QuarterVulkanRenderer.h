// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

//! Private implementation header for QuarterVulkanWidget: the
//! QVulkanWindowRenderer that bridges QVulkanWindow to
//! SoVulkanRenderManager, plus the internal Vulkan helpers it shares with
//! the widget/window implementation.  Included only by
//! QuarterVulkanWidget.cpp; the contents keep internal linkage through the
//! file-scope anonymous namespace, exactly as before this split.

#ifdef FREECAD_USE_VULKAN

#include "QuarterVulkanWidget.h"
#include "devices/InputDevice.h"
#include "eventhandlers/EventFilter.h"
#include "QuarterWidget.h"
#include "VulkanFrameDumper.h"
#include <Base/VulkanBreadcrumbs.h>

#include <Base/Console.h>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoNode.h>
#include <Inventor/rendering/vulkan/SoVulkanRenderManager.h>
#include <Inventor/rendering/vulkan/SoVulkanRenderTarget.h>

#include <vulkan/vulkan.h>

#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>
#include <QVulkanWindow>
#include <QVersionNumber>
#include <QVBoxLayout>

#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QMutex>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>
#ifdef HAVE_QT6_GUI_PRIVATE
#include <QtGui/qpa/qwindowsysteminterface.h>
#endif
#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
// QVulkanWindowPrivate is the only way to reach the swapchain present mode:
// QVulkanWindow hardcodes VK_PRESENT_MODE_FIFO_KHR and exposes no setter.
// This is a deliberate opt-in (see src/Gui/CMakeLists.txt) so a product build
// can avoid depending on Qt's private ABI altogether.
#include <QtGui/private/qvulkanwindow_p.h>
#endif

#include "Selection.h"
#include "VkRuntimePrefs.h"

#include <cstdarg>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace SIM::Coin3D::Quarter;

namespace {

// Fold the live Gui selection model into a revision number for the Vulkan
// manager's retained-drawlist replay: FreeCAD renders selection and
// preselection through SoFCSelectionRoot without guaranteeing a node-field
// change, so the graph fingerprint alone would keep drawing a stale
// highlight after a click or hover move.  Any change to the selection set
// or the current preselection produces a different value.
uint64_t mixRevision(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t hashCString(uint64_t h, const char * s)
{
    for (; s && *s; ++s) {
        h = mixRevision(h, static_cast<unsigned char>(*s));
    }
    return mixRevision(h, 0);
}

uint64_t selectionRevision()
{
    uint64_t h = 0xcbf29ce484222325ULL;
    const Gui::SelectionSingleton & sel = Gui::Selection();
    h = mixRevision(h, sel.hasSelection() ? 1u : 0u);
    h = mixRevision(h, sel.size());
    h = mixRevision(h, sel.hasPreselection() ? 1u : 0u);
    if (sel.hasPreselection()) {
        const Gui::SelectionChanges & pre = sel.getPreselection();
        h = hashCString(h, pre.pObjectName);
        h = hashCString(h, pre.pSubName);
    }
    if (sel.hasSelection() && sel.size() <= 64u) {
        const auto objs = sel.getSelection();
        for (const Gui::SelectionSingleton::SelObj & o : objs) {
            h = mixRevision(h, reinterpret_cast<uintptr_t>(o.pObject));
            h = hashCString(h, o.SubName);
        }
    }
    return h;
}

#define VK_TAG "[Vulkan] "

static bool vulkanPersistentResourcesEnabled()
{
    // Runtime behaviour knob, persisted as the View preference
    // VulkanPersistentResources (migrated from the
    // FC_VULKAN_PERSISTENT_RESOURCES developer env var).
    return Gui::VkRuntimePrefs::persistentResources();
}

enum class VkLogLevel {
    Log,
    Warning,
    Error
};

// Single formatting/emission path for the three Vulkan log severities.  The
// only difference between them is the Base::Console() channel, so the
// va_list formatting and the "[Vulkan] " tag live here once.
static void vkMessage(VkLogLevel level, const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Console() takes std::format placeholders (not printf), so pass the
    // already-formatted tag/body as arguments rather than into the format.
    switch (level) {
        case VkLogLevel::Log:
            Base::Console().log("{}{}\n", VK_TAG, buf);
            break;
        case VkLogLevel::Warning:
            Base::Console().warning("{}{}\n", VK_TAG, buf);
            break;
        case VkLogLevel::Error:
            Base::Console().error("{}{}\n", VK_TAG, buf);
            break;
    }
}

#define vkLog(...) vkMessage(VkLogLevel::Log, __VA_ARGS__)
#define vkWarn(...) vkMessage(VkLogLevel::Warning, __VA_ARGS__)
#define vkErr(...) vkMessage(VkLogLevel::Error, __VA_ARGS__)

// Format a VkPhysicalDevice API/driver version (VK_MAKE_API_VERSION layout).
static QByteArray vkVersionStr(uint32_t v)
{
    return QByteArray::number(VK_API_VERSION_MAJOR(v)) + '.' +
           QByteArray::number(VK_API_VERSION_MINOR(v)) + '.' +
           QByteArray::number(VK_API_VERSION_PATCH(v));
}

#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
//! Map the widget-level present-mode request to a VkPresentModeKHR.
//! 0 = FIFO (V-Sync), 1 = Mailbox (V-Sync, low latency), 2 = Immediate
//! (no V-Sync); any other value means FIFO.  FIFO is the only mode the Vulkan
//! spec guarantees, so it is the fallback for an unsupported request.
static VkPresentModeKHR requestedPresentModeKhr(int requested)
{
    switch (requested) {
        case 1: return VK_PRESENT_MODE_MAILBOX_KHR;
        case 2: return VK_PRESENT_MODE_IMMEDIATE_KHR;
        default: return VK_PRESENT_MODE_FIFO_KHR;
    }
}

static const char * presentModeName(VkPresentModeKHR mode)
{
    switch (mode) {
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        default: return "fifo";
    }
}

//! Resolve the swapchain present mode for \a requested against the surface's
//! advertised modes, falling back to FIFO when the request is unsupported.
//! Requires a live surface (QVulkanWindow created it in init(), before the
//! renderer's preInitResources()), so Qt's FIFO default is the only safe
//! fallback otherwise.
static VkPresentModeKHR choosePresentMode(QVulkanWindow * window, int requested)
{
    const VkPresentModeKHR wanted = requestedPresentModeKhr(requested);
    if (wanted == VK_PRESENT_MODE_FIFO_KHR) {
        return wanted;
    }

    QVulkanInstance * instance = window->vulkanInstance();
    VkPhysicalDevice physDev = window->physicalDevice();
    VkSurfaceKHR surface =
        instance ? QVulkanInstance::surfaceForWindow(window) : VK_NULL_HANDLE;
    if (instance && surface != VK_NULL_HANDLE && physDev != VK_NULL_HANDLE) {
        auto fn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            instance->getInstanceProcAddr(
                "vkGetPhysicalDeviceSurfacePresentModesKHR"));
        if (fn) {
            uint32_t count = 0;
            if (fn(physDev, surface, &count, nullptr) == VK_SUCCESS && count > 0) {
                std::vector<VkPresentModeKHR> modes(count);
                if (fn(physDev, surface, &count, modes.data()) == VK_SUCCESS
                    && std::find(modes.begin(), modes.end(), wanted) != modes.end()) {
                    return wanted;
                }
            }
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

//! Apply the user's present-mode request to QVulkanWindow's private swapchain
//! configuration.  QVulkanWindow keeps a single presentMode member (FIFO) that
//! recreateSwapChain() copies into VkSwapchainCreateInfoKHR; writing it here is
//! the only hook.  Must run after the surface exists and before the first
//! recreateSwapChain() (i.e. from the renderer's preInitResources()).
static void applyPresentMode(QVulkanWindow * window, int requested)
{
    const VkPresentModeKHR mode = choosePresentMode(window, requested);
    auto * priv = static_cast<QVulkanWindowPrivate *>(QWindowPrivate::get(window));
    // Tripwire for private-ABI drift: QVulkanWindow initialises presentMode to
    // FIFO and only this function changes it, so anything else means the field
    // offset no longer matches the Qt build this was written against.  Refuse
    // to write through a possibly-wrong offset rather than corrupt the struct.
    if (priv->presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        vkWarn("present mode: Qt private layout mismatch (presentMode=%d); "
               "keeping Qt's default",
               static_cast<int>(priv->presentMode));
        return;
    }
    priv->presentMode = mode;
    // The vkLog line below goes to Base::Console().log, which the fcprobe harness
    // cannot capture.  Mirror the resolved swapchain mode as a breadcrumb so the
    // present-mode regression probe can assert the requested mode was applied
    // (or fell back to FIFO).
    VK_BREADCRUMB("[VK-PRESENT] requested=%d using=%s\n",
                  requested,
                  presentModeName(mode));
    vkLog("present mode: requested %d, using %s", requested, presentModeName(mode));
    if (mode != requestedPresentModeKhr(requested)) {
        vkWarn("present mode %s not supported by the surface; using fifo (vsync)",
               presentModeName(requestedPresentModeKhr(requested)));
    }
}
#endif // FREECAD_USE_QT_PRIVATE_PRESENT_MODE

//! Log the swapchain surface formats relevant to HDR output.
//!
//! The HDR output path renders into an FP16 scRGB
//! (VK_FORMAT_R16G16B16A16_SFLOAT) swapchain image; the 10-bit HDR10
//! (VK_FORMAT_A2B10G10R10_UNORM_PACK32) capability is logged too so a surface
//! that only offers the latter is visible in the trace.  Whether the
//! driver/surface exposes these is independent of the color-management protocol,
//! so it is enumerated here (read-only; QVulkanWindow still chooses the actual
//! swapchain format from the preferred-format list).  Call it once the surface
//! exists (initSwapChainResources).
static void logHdrSurfaceFormats(QVulkanInstance * instance, QVulkanWindow * window)
{
    if (!instance || !window) {
        return;
    }
    auto getFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        instance->getInstanceProcAddr("vkGetPhysicalDeviceSurfaceFormatsKHR"));
    if (!getFormats) {
        return;
    }
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(window);
    if (surface == VK_NULL_HANDLE) {
        return;
    }
    VkPhysicalDevice physDev = window->physicalDevice();
    uint32_t count = 0;
    if (getFormats(physDev, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
        return;
    }
    QList<VkSurfaceFormatKHR> formats(static_cast<int>(count));
    if (getFormats(physDev, surface, &count, formats.data()) != VK_SUCCESS) {
        return;
    }
    const bool hdr10 = std::any_of(
        formats.cbegin(), formats.cend(), [](const VkSurfaceFormatKHR & f) {
            return f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        });
    const bool scrgb = std::any_of(
        formats.cbegin(), formats.cend(), [](const VkSurfaceFormatKHR & f) {
            return f.format == VK_FORMAT_R16G16B16A16_SFLOAT;
        });
    vkLog("HDR surface capabilities: %u format(s), HDR10(10-bit)=%d, scRGB(FP16)=%d",
          count, hdr10 ? 1 : 0, scrgb ? 1 : 0);
    for (const VkSurfaceFormatKHR & f : formats) {
        vkLog("  surface format: %d colorSpace: %d",
              static_cast<int>(f.format), static_cast<int>(f.colorSpace));
    }
    // The breadcrumb channel is the one captured to the trace file (Console().log
    // is report-view only), so mirror the summary there for headless runs.
    VK_BREADCRUMB("[VK-HDR] surface formats=%u hdr10_10bit=%d scrgb_fp16=%d\n",
                  count, hdr10 ? 1 : 0, scrgb ? 1 : 0);
    for (const VkSurfaceFormatKHR & f : formats) {
        VK_BREADCRUMB("[VK-HDR]   format=%d colorSpace=%d\n",
                      static_cast<int>(f.format), static_cast<int>(f.colorSpace));
    }
}

//! Known-unsafe NVIDIA driver series for HDR10 output.
//!
//! An application that calls vkSetHdrMetadataEXT (VK_EXT_hdr_metadata) with
//! invalid mastering metadata (min_luminance >= max_luminance, e.g. all-zero)
//! has that metadata forwarded unfiltered by NVIDIA's Wayland WSI; KWin then
//! raises wp_color_manager_v1 invalid_luminance and tears the client down.
//! Mesa's WSI sanitizes the same values (is_hdr_metadata_legal), so only
//! NVIDIA is affected.  Reported against 610.43.02 and still reproduced on
//! 610.57.04 (Fedora/Plasma 6.7.4, Sep 2026); the fix belongs in the WSI layer
//! (NVIDIA) or the caller, and no driver release has fixed it yet.
//!
//! Qt's Wayland path drives HDR through wp_color_manager_v1 / QColorSpace and
//! does NOT call vkSetHdrMetadataEXT, so FreeCAD is not expected to trigger the
//! bug (and HDR output runs clean on the 615.71.09 driver here).  HDR output is
//! still experimental, though, and a compositor-side client kill is not an
//! acceptable default, so the known-bad 610 series is gated.  Set
//! FREECAD_VULKAN_HDR_ALLOW_UNSAFE_DRIVER=1 to override (and, in a debug build,
//! FREECAD_VULKAN_HDR_FORCE_BLOCK_DRIVER=<major> to exercise the gate).
//!
//! The check is scoped to the platform the bug exists on (NVIDIA on Wayland)
//! and reads the driver version from the Vulkan physical-device properties
//! whenever they are available, so it is not Linux- or /proc-specific.  The
//! NVIDIA kernel-module file is only a fallback for the brief window before
//! QVulkanWindow has selected the physical device.
static bool hdrDriverBlocked(QVulkanWindow * window)
{
    constexpr uint32_t kNvidiaVendorId = 0x10DE;
    constexpr uint32_t kKnownBadDriverMajor = 610;

    if (qEnvironmentVariableIsSet("FREECAD_VULKAN_HDR_ALLOW_UNSAFE_DRIVER")) {
        return false;
    }

#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    // Test hook: force a driver major version without touching the real driver.
    bool forcedOk = false;
    const int forced = qEnvironmentVariableIntValue(
        "FREECAD_VULKAN_HDR_FORCE_BLOCK_DRIVER", &forcedOk);
    if (forcedOk) {
        return static_cast<uint32_t>(forced) == kKnownBadDriverMajor;
    }
#endif

    // The bug is in NVIDIA's Wayland WSI only (see above): Mesa sanitises the
    // metadata and the Windows/X11 paths never reach this WSI, so leaving HDR
    // enabled there is safe.
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        return false;
    }

    // Prefer the Vulkan driver properties: portable across OSes and reports the
    // exact driver backing the selected device.  They are only populated once
    // QVulkanWindow has picked the physical device, so fall back to the kernel
    // module file before the window is first exposed.
    if (window) {
        if (const VkPhysicalDeviceProperties * props =
                window->physicalDeviceProperties()) {
            if (props->vendorID != kNvidiaVendorId) {
                return false;
            }
            // NVIDIA encodes driverVersion as VK_MAKE_VERSION(major, minor,
            // patch); vulkaninfo reports it the same way (e.g. 615.71.9.0).
            return VK_VERSION_MAJOR(props->driverVersion) == kKnownBadDriverMajor;
        }
    }

    // Fallback: NVIDIA only, so the file is absent (or unreadable) for every
    // other driver and on non-Linux systems.
    QFile f(QStringLiteral("/proc/driver/nvidia/version"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    // "NVRM version: NVIDIA UNIX x86_64 Kernel Module  615.71.09  ..."
    const QString line = QString::fromLocal8Bit(f.readLine());
    static const QRegularExpression re(QStringLiteral("(\\d+)\\.\\d+\\.\\d+"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) {
        return false;
    }
    const uint32_t major = static_cast<uint32_t>(m.captured(1).toUInt());
    return major == kKnownBadDriverMajor;
}


class QuarterVulkanRenderer;

class QuarterVulkanRenderer final : public QVulkanWindowRenderer
{
public:
    QuarterVulkanRenderer(QVulkanInstance * instance,
                          QVulkanWindow * window,
                          SoNode * scene,
                          SoCamera * camera,
                          QuarterVulkanWidget * owner)
        : m_instance(instance)
        , m_scene(scene)
        , m_camera(camera)
        , m_window(window)
        , m_owner(owner)
        , m_dumper(instance, window)
    {
    }

    void setScene(SoNode * scene)
    {
        QMutexLocker locker(&m_stateMutex);
        m_scene = scene;
    }
    void setOverlayScene(SoNode * scene)
    {
        QMutexLocker locker(&m_stateMutex);
        m_overlayScene = scene;
    }
    void setDecorationScene(SoNode * scene)
    {
        QMutexLocker locker(&m_stateMutex);
        m_decorationScene = scene;
    }
    void setCamera(SoCamera * camera)
    {
        QMutexLocker locker(&m_stateMutex);
        m_camera = camera;
    }
    //! Display/tuning settings as one blob (see SoVulkanViewSettings).  The
    //! render manager diffs and applies the whole blob, so the renderer keeps
    //! no per-field "last applied" mirrors of its own.  The individual setters
    //! are thin struct-field writers kept for the existing widget API.
    void setViewSettings(const SoVulkanViewSettings & settings)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings = settings;
    }
    void setBackgroundColor(const SbColor4f & color)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.backgroundColor = color;
    }
    SbColor4f getBackgroundColor() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_viewSettings.backgroundColor;
    }
    void setBackgroundGradient(bool enabled,
                               const SbColor4f & top,
                               const SbColor4f & bottom)
    {
        VK_BREADCRUMB("[VK-TRACE] QuarterVulkanRenderer::setBackgroundGradient "
                      "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                      enabled ? 1 : 0, top[0], top[1], top[2],
                      bottom[0], bottom[1], bottom[2]);
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.backgroundGradient = enabled;
        m_viewSettings.backgroundTop = top;
        m_viewSettings.backgroundBottom = bottom;
    }
    void setPointsOverlay(bool enabled)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pointsOverlay = enabled;
    }
    void setEdgeColor(const SbColor4f & color)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.edgeColor = color;
    }

    // Capabilities probed by the widget's physical-device selection, handed to
    // the render manager through SoVulkanDeviceContext::caps so the renderer
    // does not re-enumerate the device extension list.
    void setDeviceCaps(const SoVulkanDeviceCaps & caps)
    {
        QMutexLocker locker(&m_stateMutex);
        m_deviceCaps = caps;
        m_deviceCapsValid = true;
    }

    // Path tracing state is staged here and applied to the manager at the
    // next startNextFrame() instead of being called into the manager from
    // arbitrary widget-API call sites, keeping every manager access inside
    // frame setup.  Qt 6 invokes startNextFrame() on the GUI thread.
    void setPathTracingEnabled(bool enabled)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingEnabled = enabled;
    }
    void setPathTracingStart(bool start)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingStart = start;
    }
    void setPathTracingBounces(int bounces)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pathTracingBounces = std::clamp(bounces, 1, 16);
    }
    void setPathTracingSettleFrames(int frames)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pathTracingSettleFrames = std::clamp(frames, 1, 120);
    }
    //! Interaction LOD is a runtime navigation state, not a persisted
    //! setting, so it is staged on its own rather than in m_viewSettings.
    void setInteractionLod(bool active)
    {
        QMutexLocker locker(&m_stateMutex);
        m_interactionLod = active;
    }
    void setPathTracingMaxSamples(int samples)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pathTracingMaxSamples = std::clamp(samples, 1, 4096);
    }
    void setPathTracingDenoiser(const std::string & denoiser)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pathTracingDenoiser = denoiser;
    }
    void setPathTracingDenoiserScale(float scale)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.pathTracingDenoiserScale = std::clamp(scale, 1.0f, 8.0f);
    }
    bool getPathTracingEnabled() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_pathTracingEnabled;
    }
    bool getPathTracingActive() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_pathTracingActive;
    }

    void setViewMode(SoVulkanViewMode mode)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.viewMode = mode;
    }
    SoVulkanViewMode getViewMode() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_viewSettings.viewMode;
    }

    void setEnvMap(int index)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewSettings.envMap = index;
    }
    int getEnvMap() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_viewSettings.envMap;
    }

    // Staged authoritative scene lighting (GL host -> both backends).  Like
    // the env map, applied at the next startNextFrame() so every m_manager call
    // stays inside frame setup.  The set is camera-anchored world-space data,
    // re-pushed by the adapter on every camera move, so the dirty flag drives a
    // fresh apply per frame while the camera moves.
    void setSceneLights(const SoLightingData & lighting)
    {
        QMutexLocker locker(&m_stateMutex);
        m_sceneLighting = lighting;
        m_sceneLightsDirty = true;
    }

    // Ray-tracing status mirrored from the manager (which only re-evaluates
    // device support during frame setup) so callers can query the cached
    // value from anywhere.
    bool getRayTracingActive() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_rayTracingActive;
    }

    //! Ordinal of the last presented frame (see SoVulkanRenderManager::
    //! getRenderFrameCount).  The same value is copied into that frame's
    //! SoRenderParams::frame, so [RTDBG] lines and frame dumps can be
    //! correlated to this ordinal by the probe/checker layer.
    uint32_t getRenderFrameCount() const
    {
        return m_manager.getRenderFrameCount();
    }

    //! Cast one world-space ray against the RT backend's TLAS (see
    //! QuarterVulkanWidget::pickRay).  Returns false when ray tracing is not
    //! active or no TLAS has been built yet.
    bool pickRay(const float origin[3], const float direction[3], float tMax,
                 QuarterVulkanWidget::VulkanPickHit & out) const
    {
        SoVulkanRenderManager::VulkanPickHit hit;
        if (!m_manager.pickRay(origin, direction, tMax, hit)) {
            return false;
        }
        out.hit = hit.hit;
        out.t = hit.t;
        out.worldPos[0] = hit.worldPos[0];
        out.worldPos[1] = hit.worldPos[1];
        out.worldPos[2] = hit.worldPos[2];
        out.commandIndex = hit.commandIndex;
        out.primitiveId = hit.primitiveId;
        out.userData = hit.userData;
        out.primitiveOffset = hit.primitiveOffset;
        return true;
    }

    // Whether the RTX backend actually initialized (the device supports
    // hardware ray tracing and it came up).  Two distinct concepts are kept
    // separate: device support (m_rtxBackendAvailable) is a capability the
    // device advertises and is knowable before/without building the backend;
    // m_rtxBackendBuilt is whether the backend actually came up (so the
    // raster-only path never pays for building it until path tracing is
    // on).  When availability is false, path tracing can never run on this
    // device.
    bool getRayTracingAvailable() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_rtxBackendAvailable;
    }

    // True once initResources() has run: device support is settled and
    // availability can be judged.  Before that the adapter must not warn about
    // "no hardware ray tracing" on an unprobed renderer.  This is independent
    // of whether path tracing was actually requested.
    bool getRayTracingProbed() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_rtxBackendProbed;
    }

    // Device ray-tracing capability is determined by the physical-device probe
    // in the widget (selectPhysicalDevice/configureDeviceFeatures) and pushed
    // here, so getRayTracingAvailable() reflects hardware support even before
    // the RTX backend has been built (path tracing off at startup).  It is the
    // source of truth the adapter uses to decide whether a path-tracing
    // request can ever succeed.
    void setRayTracingDeviceSupported(bool supported)
    {
        QMutexLocker locker(&m_stateMutex);
        m_rtxBackendAvailable = supported;
    }

    // Demand-driven redraws: the surface re-renders only when something
    // changed.  Widget setters call redraw() from the GUI thread, and
    // camera/scene changes arrive through VulkanViewportAdapter::syncViewer
    // (FreeCAD routes every document update through Application::onUpdate()
    // and every camera swap through View3DInventorViewer::cameraChanged;
    // both end in the adapter's redraw()).  The Vulkan widget deliberately
    // owns no Coin sensors: the hidden GL viewer's render loop never runs,
    // so Coin's sensor delay queue is never processed and freshly attached
    // sensors never activate.

#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
    //! Record the widget-level present-mode request (0 FIFO, 1 Mailbox, 2
    //! Immediate).  Applied in preInitResources(), once QVulkanWindow has
    //! created the surface that decides which modes are actually supported.
    void setRequestedPresentMode(int mode) { m_requestedPresentMode = mode; }
#endif

    void preInitResources() override
    {
#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
        applyPresentMode(m_window, m_requestedPresentMode);
#endif
    }

    void initResources() override
    {
        vkLog("initResources: creating Vulkan backend");
        const VkPhysicalDeviceProperties * props = m_window->physicalDeviceProperties();
        if (props) {
            vkLog("  physical device: %s", props->deviceName);
            vkLog("  Vulkan API version: %s", vkVersionStr(props->apiVersion).constData());
            vkLog("  driver version: 0x%08x", props->driverVersion);
        }
        vkLog("  queue family index: %u", m_window->graphicsQueueFamilyIndex());
        vkLog("  graphics queue: %p", static_cast<void*>(m_window->graphicsQueue()));

        m_initContext = {};
        m_initContext.instance = m_instance->vkInstance();
        m_initContext.physicalDevice = m_window->physicalDevice();
        m_initContext.device = m_window->device();
        m_initContext.graphicsQueue = m_window->graphicsQueue();
        m_initContext.graphicsQueueFamilyIndex =
            m_window->graphicsQueueFamilyIndex();
        // The async-compute queue requested at device creation (see
        // configureDeviceFeatures).  The backend retrieves the handle itself
        // with vkGetDeviceQueue() from this family+index.
        if (m_owner->hasAsyncComputeQueue()) {
            m_initContext.computeQueueFamilyIndex =
                m_owner->asyncComputeQueueFamilyIndex();
            m_initContext.computeQueueIndex =
                m_owner->asyncComputeQueueIndex();
        }
        if (props) {
            m_initContext.apiVersion = props->apiVersion;
        }
        // Pass the capabilities probed by the widget so the renderer does not
        // re-enumerate the device extension list.
        m_initContext.caps = m_deviceCaps;
        m_initContext.capsValid = m_deviceCapsValid;
        // Persistent pipeline cache: without it every process start recompiles
        // the many lazily-created pipeline variants (topology/fill/depth/
        // blend/stencil/sample-count combinations) on first appearance, which
        // stutters the first frames of a scene.  The blob is per-device (the
        // driver's cache header carries the pipelineCacheUUID), so every view
        // on one GPU shares the file and a stale file is ignored by the
        // backend.  Best-effort: if the cache directory cannot be created the
        // renderer simply runs without persistence.
        {
            const QString cacheDir =
                QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                + QStringLiteral("/vulkan");
            QDir().mkpath(cacheDir);
            m_manager.setPipelineCachePath(
                (cacheDir + QStringLiteral("/pipeline_cache.bin")).toStdString());
        }
        // Request the ray-tracing backend BEFORE initialize() only when path
        // tracing is (or will be) used.  Bringing it up unconditionally
        // rebuilds the whole RT stack (acceleration structures + RT
        // pipelines) on EVERY window reset -- QVulkanWindow tears down and
        // re-creates the backend on Expose/Hide/Resize/Move events -- which
        // is the dominant cost when the user orbits in the raster-only
        // path.  When a view opens with path tracing off, we build only the
        // raster backend; ensureRayTracing() brings the RT backend up lazily
        // the first time path tracing is toggled on, with no re-open needed.
        m_manager.setRayTracing(m_pathTracingEnabled ? TRUE : FALSE);
        m_initialized = m_manager.initialize(&m_initContext);
        // Whether the RTX backend actually built during this initialize().
        // This is NOT device support: when path tracing was off at startup the
        // RT backend is skipped, so this is false even on an RT-capable one.
        // Device capability lives in m_rtxBackendAvailable (set by the device
        // probe in setRayTracingDeviceSupported).
        m_rtxBackendBuilt = m_manager.getRayTracingBackend() ? true : false;
        // availability is now settled: initResources ran.  Distinct from any
        // path-tracing request.
        m_rtxBackendProbed = true;
        // A freshly (re)initialized RTX engine starts from its own defaults
        // (ptDenoise is ON, see SoRTXRenderBackend::ptDenoise), and a prior
        // frame may have marked the denoise/bounce settings "applied" while the
        // backend was not yet initialized (the manager logged "setting
        // ignored").  Re-apply the user's path-tracing settings once the next
        // frame runs against a built backend, so a startup window-reset (or
        // raster-only open) never leaves the denoiser leaked on.
        m_reapplyPathTracingSettings = m_rtxBackendBuilt;
        VK_BREADCRUMB("[VK-TRACE] QuarterVulkanRenderer::initResources "
                      "pathTracing=%d rtxBuilt=%d\n",
                      m_pathTracingEnabled ? 1 : 0,
                      m_rtxBackendBuilt ? 1 : 0);
        if (m_initialized) {
            // Mirror the GL viewer (QuarterWidget sets
            // SoRenderManager::VARIABLE_NEAR_PLANE): re-fit the camera
            // near/far to the scene bounding box every frame so zooming and
            // orbiting never clip the model at the near/far planes.  The
            // hidden GL viewer never renders, so its own auto-clipping would
            // never run.
            m_manager.setAutoClipping(SoVulkanRenderManager::VARIABLE_NEAR_PLANE);
            if (m_rtxBackendBuilt) {
                vkLog("initResources: ray tracing backend built (device "
                      "support=%d)",
                      m_rtxBackendAvailable ? 1 : 0);
            }
            else {
                vkLog("initResources: ray tracing backend not built "
                      "(device support=%d); using raster Vulkan backend",
                      m_rtxBackendAvailable ? 1 : 0);
            }
            vkLog("initResources: backend initialized OK");
        }
        else {
            vkErr("initResources: backend initialize FAILED");
        }
    }

    void initSwapChainResources() override
    {
        VK_BREADCRUMB("[VK-HDR] initSwapChainResources colorFormat=%d\n",
                      static_cast<int>(m_window->colorFormat()));
        m_manager.setRenderTarget(&m_target);
        // QVulkanWindow may keep up to its swapchain image count frames in
        // flight; give the backend one extra ring slot of margin.
        m_manager.setMaxFramesInFlight(
            static_cast<uint32_t>(m_window->swapChainImageCount()) + 1u);
        const VkSampleCountFlagBits samples = m_window->sampleCountFlagBits();
        vkLog("initSwapChainResources:");
        vkLog("  image size: %dx%d", m_window->swapChainImageSize().width(),
              m_window->swapChainImageSize().height());
        vkLog("  color format: %d", static_cast<int>(m_window->colorFormat()));
        vkLog("  depth/stencil format: %d",
              static_cast<int>(m_window->depthStencilFormat()));
        vkLog("  sample count: %d", static_cast<int>(samples));
        vkLog("  swapchain images: %d", m_window->swapChainImageCount());
        logHdrSurfaceFormats(m_instance, m_window);

        m_dumper.initSwapChainResources();

        // The swapchain color format is now known: notify the owner on the GUI
        // thread so it can re-push its render settings with the real HDR output
        // state (see the signal's documentation).  Queued because this runs on
        // the render thread during QVulkanWindow's swapchain (re)initialization.
        QMetaObject::invokeMethod(m_owner, "swapChainChanged",
                                  Qt::QueuedConnection);
    }

    void releaseSwapChainResources() override
    {
        vkLog("releaseSwapChainResources");
        m_manager.setRenderTarget(nullptr);
        m_dumper.releaseSwapChainResources();
    }

    void releaseResources() override
    {
        // Shut down the backend while the Vulkan device/queue are still
        // valid; the manager destructor runs too late to wait on the queue.
        vkLog("releaseResources: shutting down backend");
        m_manager.shutdown();
        m_initialized = false;
    }

    void physicalDeviceLost() override
    {
        vkErr("physicalDeviceLost: VK_ERROR_DEVICE_LOST");
        this->dropToRaster("physical device lost");
    }

    void logicalDeviceLost() override
    {
        vkErr("logicalDeviceLost: VK_ERROR_DEVICE_LOST");
        this->dropToRaster("logical device lost");
    }

    // Hard fall-back to the raster backend (Autodesk-style): when the device is
    // lost -- commonly an NVIDIA TDR timeout while a path-traced sample takes
    // too long -- any further path-tracing request would be futile and would
    // busy-loop the renderer against a dead device.  Drop the request so the
    // next initResources() (after Qt recreates the swapchain) comes up raster
    // only, and the adapter's availability check reports the true state.  The
    // device-lost callbacks fire outside startNextFrame()'s frame loop, so we
    // mutate the request state under the accepted lock.
    void dropToRaster(const char * reason)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingEnabled = false;
        m_appliedPathTracingEnabled = false;
        m_rtxBackendBuilt = false;
        m_rayTracingActive = false;
        // Reset the manager side too, not just our request state, so it is not
        // left believing ray tracing is still live if Qt does not recreate the
        // swapchain after the loss.  setRayTracing(FALSE) is a pure request-flag
        // set and is always safe; disable path tracing on the RTX backend only
        // when it is actually live so we do not trip setPathTracingEnabled()'s
        // "backend not initialized" warning.
        m_manager.setRayTracing(FALSE);
        if (m_manager.getRayTracingActive()) {
            m_manager.setPathTracingEnabled(FALSE);
        }
        vkWarn("path tracing disabled after %s; falling back to the raster "
               "Vulkan backend.", reason);
    }

    void startNextFrame() override
    {
        const FrameState frame = snapshotFrameState();

        if (!m_initialized || !frame.scene) {
            if (!m_initialized) {
                vkWarn("startNextFrame: backend not initialized, skipping");
            }
            else {
                vkWarn("startNextFrame: no scene graph set, skipping");
            }
            // QVulkanWindow expects frameReady() exactly once per
            // startNextFrame().  When the backend is up but no scene is set
            // yet, signal it with the (empty) command buffer so the present
            // pipeline never stalls waiting for a frame that will not come.
            // A failed backend initResources() must release the frame too:
            // Qt still drives startNextFrame() after init (the swapchain is
            // its own, independent of our backend), and skipping frameReady()
            // there parks the present loop forever -- a frozen viewport that
            // never recovers, because Qt schedules no further frame while one
            // is pending.
            m_window->frameReady();
            return;
        }

        const int index = m_window->currentSwapChainImageIndex();
        const QSize size = m_window->swapChainImageSize();
        const VkSampleCountFlagBits samples = m_window->sampleCountFlagBits();
        const bool multisample = samples != VK_SAMPLE_COUNT_1_BIT;

        setupRenderTarget(index, size, samples, multisample);
        pushSceneState(frame, size);
        logSwapchainState(index, size, samples);
        notifySurfaceSize(size);

        VkCommandBuffer cb = m_window->currentCommandBuffer();
        recordScenePass(cb, size, frame.viewSettings.backgroundColor,
                        multisample);

        // Env-gated frame dump (see Detail::VulkanFrameDumper): copy the
        // swapchain color image into a staging buffer inside the same command
        // buffer, then read it back after submission and write a PNG.  The
        // PNG is named by the manager's per-frame ordinal -- the SAME ordinal
        // the RT backend prints in its [RTDBG] blas/ptState lines -- so a
        // frame dump can be correlated to the backend trace that produced it
        // even when the two arrive out of order.
        m_dumper.recordFrameCopy(cb, index, size,
                                 m_manager.getRenderFrameCount());

        m_window->frameReady();

        reportStatus();

        m_dumper.saveFrame();
    // Progressive path tracing accumulates samples across frames and
    // therefore needs continuous re-renders while it is working toward a
    // converged image: while accumulating AND during the short post-move
    // settle window in which the backend counts idle frames before
    // auto-restarting (m_pathTracingRefining).  Once converged the flag goes
    // false and the surface can go idle.
    //
    // The surface is display-only and owns no Coin sensors, so every other
    // change must arrive as an explicit wake:
    //   - widget setters call redraw();
    //   - the VulkanViewportAdapter requests a frame on:
    //       - document update        (onUpdate),
    //       - selection/preselection (selectionChanged),
    //       - camera-node swap       (cameraChanged),
    //       - camera-pose navigation (cameraMoved).
    //
    // Without the refining gate the Vulkan surface used to busy-loop
    // requestUpdate() at full swapchain rate.
    if (frame.pathTracingEnabled && m_pathTracingRefining) {
        m_window->requestUpdate();
    }
    }

private:
    //! Everything startNextFrame() reads from the widget API.  Snapshotted
    //! under m_stateMutex so one frame always sees a consistent set of
    //! values.
    struct FrameState
    {
        SoNode * scene = nullptr;
        SoNode * overlayScene = nullptr;
        SoNode * decorationScene = nullptr;
        SoCamera * camera = nullptr;
        //! Display/tuning settings snapshot (one blob; the manager diffs it).
        SoVulkanViewSettings viewSettings;
        bool pathTracingEnabled = false;
        //! Interaction LOD (single-bounce preview while the camera moves).
        bool interactionLod = false;
    };

    // Qt 6 invokes startNextFrame() on the GUI thread, like every other
    // access to these state members; the setters and redraw sensors run on
    // the same thread.  Snapshot everything under the mutex and use the
    // result for the rest of the frame so one frame always sees a
    // consistent set of values.  Pending path-tracing requests are applied
    // to the manager here, at frame setup.
    FrameState snapshotFrameState()
    {
        FrameState frame;
        QMutexLocker locker(&m_stateMutex);
        // Whether the RTX backend was up entering this frame; the toggle block
        // below may build it lazily (raster -> path-tracing) or leave it alone.
        const bool rtxBefore = m_rtxBackendBuilt;
        frame.scene = m_scene;
        frame.overlayScene = m_overlayScene;
        frame.decorationScene = m_decorationScene;
        frame.camera = m_camera;
        frame.viewSettings = m_viewSettings;
        frame.pathTracingEnabled = m_pathTracingEnabled;
        frame.interactionLod = m_interactionLod;
        // Denoising is required for path tracing, so it always runs while the
        // path tracer is active; the denoiser selector only picks the filter.
        frame.viewSettings.pathTracingDenoise = m_pathTracingEnabled;

        // Enable before raising the start latch: the RT backend drops the
        // latch if path tracing is not yet enabled (setPathTracingStart
        // ignores requests while ptEnabled is false).
        if (m_pathTracingEnabled != m_appliedPathTracingEnabled) {
            // Live backend switch as ONE cohesive manager call: requestRayTracing()
            // sets the dispatch request, lazily builds the RTX backend when it
            // was skipped at startup (so the raster-only path never pays for
            // it and a runtime toggle needs no window re-initialization), and
            // enables/disables path tracing.  requestRayTracing() returns the
            // effective active state, which the hard-fallback path below uses.
            VK_BREADCRUMB("[VK-TRACE] QuarterVulkanRenderer::startNextFrame "
                          "rtBackendToggle=%d rtxBuilt=%d\n",
                          m_pathTracingEnabled ? 1 : 0,
                          m_rtxBackendBuilt ? 1 : 0);
            const bool rtActive = m_manager.requestRayTracing(
                m_pathTracingEnabled ? TRUE : FALSE);
            m_rtxBackendBuilt = rtActive;
            if (m_pathTracingEnabled && !rtActive) {
                // Hard fallback: the request asked for path tracing but the
                // RTX backend could not be brought up (device lacks ray
                // tracing, or the lazy build failed).  Do not keep requesting
                // it every frame; drop the request, revert to raster, and let
                // the adapter warn.  This mirrors Autodesk's fallback from GPU
                // ray tracing back to the Realistic viewport.
                m_pathTracingEnabled = false;
                m_appliedPathTracingEnabled = false;
                // Keep this frame's snapshot in agreement with the request we
                // just dropped so the refining gate below does not keep the
                // surface spinning on a dead trace path.
                frame.pathTracingEnabled = false;
                vkWarn("requestRayTracing: path tracing requested but the "
                       "ray-tracing backend is unavailable; falling back to "
                       "the raster Vulkan backend.");
                // Feature detection: tell the owner the ray tracer cannot run
                // here so it can revert to a raster render mode (the hardware
                // may advertise the extensions but still fail to build the
                // backend, e.g. on a device below Vulkan 1.2).  Emitted through
                // the owner on a QUEUED connection (same pattern as
                // notifySurfaceSize) since this runs inside snapshotFrameState
                // while the state mutex is held; the slot must not run inline.
                QMetaObject::invokeMethod(m_owner, "rayTracingUnavailable",
                                          Qt::QueuedConnection);
            }
            else {
                m_appliedPathTracingEnabled = m_pathTracingEnabled;
            }
        }
        // A fresh RTX engine -- lazily built just above, or re-created by the
        // window-init reset flagged in initResources() -- starts from its own
        // defaults (ptDenoise is ON), so force the manager to re-apply the
        // whole settings blob to the fresh engine.  Guarded on m_rtxBackendBuilt
        // so a raster-only view (no RT backend) never spams the "not
        // initialized" manager warnings.  The manager does the per-field diff
        // for the steady state (see SoVulkanRenderManager::setViewSettings).
        const bool reapplyPT = m_rtxBackendBuilt
            && (m_reapplyPathTracingSettings || !rtxBefore);
        m_reapplyPathTracingSettings = false;
        if (reapplyPT) {
            m_manager.invalidateViewSettings();
        }
        // Push the GL-authoritative scene lighting to the render manager,
        // which fans it out to BOTH backends: the raster executor (so the
        // raster view's view-relative lights follow the camera like Coin GL)
        // and the RT backend (whose IR capture can drop to zero lights on the
        // retained/replayed path tracer).  Not gated on the RT backend being
        // built -- the raster path needs the set too.
        if (m_sceneLightsDirty) {
            m_manager.setSceneLights(m_sceneLighting);
            m_sceneLightsDirty = false;
        }
        if (m_pathTracingStart) {
            m_manager.setPathTracingStart(TRUE);
            m_pathTracingStart = false;
        }
        return frame;
    }

    void setupRenderTarget(int index, const QSize & size,
                           VkSampleCountFlagBits samples, bool multisample)
    {
        m_target.colorImage = multisample
            ? m_window->msaaColorImage(index)
            : m_window->swapChainImage(index);
        m_target.colorImageView = multisample
            ? m_window->msaaColorImageView(index)
            : m_window->swapChainImageView(index);
        m_target.colorFormat = m_window->colorFormat();
        m_target.depthImage = m_window->depthStencilImage();
        m_target.depthImageView = m_window->depthStencilImageView();
        m_target.depthFormat = m_window->depthStencilFormat();
        m_target.extent = {static_cast<uint32_t>(size.width()),
                           static_cast<uint32_t>(size.height())};
        m_target.sampleCount = samples;
    }

    void pushSceneState(const FrameState & frame, const QSize & size)
    {
        m_manager.setSceneGraph(frame.scene);
        m_manager.setOverlaySceneGraph(frame.overlayScene);
        m_manager.setDecorationSceneGraph(frame.decorationScene);
        m_manager.setCamera(frame.camera);
        // Publish the selection revision the graph fingerprint cannot see
        // (see selectionRevision()).
        m_manager.setExternalRevision(selectionRevision());

        // The hidden GL viewer's viewport region is not authoritative: the
        // Vulkan surface always covers the entire stacked-widget area, while
        // the GL viewer may still report a stale or default size (it is never
        // shown).  Using the GL-derived region produced a small rendering
        // window in a corner of an otherwise clear-colored surface.  Always
        // drive the Vulkan viewport/projection from the swapchain extent.
        SbViewportRegion vp(static_cast<short>(size.width()),
                            static_cast<short>(size.height()));
        vp.setViewportPixels(0, 0,
                             static_cast<short>(size.width()),
                             static_cast<short>(size.height()));
        m_manager.setViewportRegion(vp);
        // The swapchain is in device pixels; record the widget's device-pixel
        // ratio so the render backend scales logical SoDrawStyle line widths
        // / point sizes correctly (see SoVulkanRenderBackend).  Without it the
        // ratio stayed 1.0 and overlay strokes (NaviCube edges/axes/service
        // dots) rendered 1/dpr too thin on a fractional-scaling display.
        m_manager.setDevicePixelRatio(static_cast<float>(m_owner->devicePixelRatioF()));
        VK_BREADCRUMB_ONCE("[VK-TRACE] startNextFrame: setViewSettings "
                           "bgGradient=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                           frame.viewSettings.backgroundGradient ? 1 : 0,
                           frame.viewSettings.backgroundTop[0],
                           frame.viewSettings.backgroundTop[1],
                           frame.viewSettings.backgroundTop[2],
                           frame.viewSettings.backgroundBottom[0],
                           frame.viewSettings.backgroundBottom[1],
                           frame.viewSettings.backgroundBottom[2]);
        // One call applies the whole display/tuning blob (the manager diffs it).
        m_manager.setViewSettings(frame.viewSettings);
        // Interaction LOD is a separate runtime state (not part of the diffed
        // settings blob).  The manager forwards it to the RT backend and is
        // idempotent, so applying it every frame is cheap.
        m_manager.setInteractionLod(frame.interactionLod ? TRUE : FALSE);
        if (Gui::VkDebug::backendDebug()) {
            static int syncLog = 0;
            if (syncLog++ < 3) {
                Base::Console().message(
                    "[VK-SET] startNextFrame wire={} points={} "
                    "edge=({:.2f},{:.2f},{:.2f},{:.2f})\n",
                    frame.viewSettings.wireframeOverlay ? 1 : 0,
                    frame.viewSettings.pointsOverlay ? 1 : 0,
                    frame.viewSettings.edgeColor[0],
                    frame.viewSettings.edgeColor[1],
                    frame.viewSettings.edgeColor[2],
                    frame.viewSettings.edgeColor[3]);
            }
        }
        // The external path relies on QVulkanWindow's default render pass
        // clear (see recordScenePass); the backend must never issue its own
        // full-frame clear attachments into that pass.
        m_manager.setClearEnabled(false, false);
        m_manager.setRenderTarget(&m_target);
    }

    // Log once per swapchain recreation (not per frame) to avoid console
    // spam now that the renderer requests a new frame continuously.
    void logSwapchainState(int index, const QSize & size,
                           VkSampleCountFlagBits samples)
    {
        static QSize lastLoggedSize;
        static uint32_t lastLoggedSamples = 0;
        if (size != lastLoggedSize || samples != lastLoggedSamples) {
            lastLoggedSize = size;
            lastLoggedSamples = samples;
            vkLog("startNextFrame: swapchainImage=%d extent=%dx%d samples=%d",
                  index, size.width(), size.height(),
                  static_cast<int>(samples));
        }
    }

    // Notify the GUI thread so the hidden OpenGL viewer (which owns
    // navigation/picking) can keep its viewport region in sync with the
    // visible Vulkan surface size.
    void notifySurfaceSize(const QSize & size)
    {
        if (size != m_lastSurfaceSize) {
            m_lastSurfaceSize = size;
            QMetaObject::invokeMethod(m_owner, "surfaceSizeChanged",
                                      Qt::QueuedConnection,
                                      Q_ARG(QSize, size));
        }
    }

    // QVulkanWindow does not begin/end the render pass for us; the renderer
    // is expected to do it in startNextFrame() using defaultRenderPass() and
    // currentFramebuffer().  Record the scene into QVulkanWindow's own
    // command buffer between a begin/end pair, then signal frameReady().
    // The backend must not begin/end a render pass or submit to the queue on
    // this path.
    void recordScenePass(VkCommandBuffer cb, const QSize & size,
                         const SbColor4f & background, bool multisample)
    {
        // HDR raster path: the manager owns the whole pass lifecycle (offscreen
        // RGBA16F pass, barrier, output/scRGB pass into Qt's framebuffer), so
        // do NOT begin Qt's default render pass here.
        if (m_manager.isHdrRasterActive()) {
            const SbBool hdrOk = m_manager.renderExternalHdr(
                false, false, cb, m_window->defaultRenderPass(),
                m_window->currentFramebuffer());
            if (!hdrOk) {
                vkErr("startNextFrame: renderExternalHdr FAILED");
            }
            return;
        }

        VkRenderPassBeginInfo rpBegin {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_window->defaultRenderPass();
        rpBegin.framebuffer = m_window->currentFramebuffer();
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = {
            static_cast<uint32_t>(size.width()),
            static_cast<uint32_t>(size.height())};

        // QVulkanWindow's default render pass is created with
        // LOAD_OP_CLEAR on the color, depth and (when MSAA is active) MSAA
        // color attachments, so the begin info must carry one clear value
        // per cleared attachment.  With multisampling Qt's pass clears
        // attachment 0 (swapchain resolve target), 1 (depth) and 2 (MSAA
        // color); without it only attachments 0 and 1 are cleared.  Passing
        // fewer clear values than cleared attachments is a spec violation
        // (VUID-VkRenderPassBeginInfo-clearValueCount-00902) and leaves the
        // MSAA color attachment uninitialized, which resolves to an
        // undefined (white) frame.  Clear color to the configured viewport
        // background and depth to 1.0 (the conventional far value) to match
        // the legacy clear behavior; the backend additionally issues clear
        // attachments only when its clear flags request it.
        VkClearValue clearValues[3] {};
        clearValues[0].color.float32[0] = background[0];
        clearValues[0].color.float32[1] = background[1];
        clearValues[0].color.float32[2] = background[2];
        clearValues[0].color.float32[3] = background[3];
        clearValues[1].depthStencil.depth = 1.0f;
        clearValues[1].depthStencil.stencil = 0;
        if (multisample) {
            // MSAA color attachment: same clear color as the resolve target.
            clearValues[2] = clearValues[0];
        }
        rpBegin.clearValueCount = multisample ? 3u : 2u;
        rpBegin.pClearValues = clearValues;

        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        // When the device has VK_EXT_nested_command_buffer enabled the Coin
        // backend records its opaque pass into secondary command buffers and
        // replays them with vkCmdExecuteCommands().  That is only legal in a
        // subpass begun with INLINE_AND_SECONDARY contents, and this pass is
        // caller-owned (the backend does not begin it), so mirror the backend's
        // choice here; with plain INLINE the backend records inline instead.
        const VkSubpassContents subpassContents =
            m_manager.nestedCommandBuffersEnabled()
                ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_EXT
                : VK_SUBPASS_CONTENTS_INLINE;
        vkdf->vkCmdBeginRenderPass(cb, &rpBegin, subpassContents);

        // QVulkanWindow's default render pass already clears color and depth
        // with the values in rpBegin above, so the backend must not issue
        // its own full-frame clear attachments (a second clear per frame).
        // The overlay block's sub-rect depth clear is unaffected.
        //
        // renderExternal() also owns the GPU geometry-LOD pre-pass: because
        // Vulkan forbids compute inside a render pass and the pass is already
        // begun here, the backend records the sub-pixel compaction into a
        // transient command buffer it submits ahead of this pass's submission.
        // No caller coordination is needed.
        //
        // NOTE: this only compacts what the manager's draw list actually
        // contains.  A fresh document's main region is usually just the hidden
        // nav cube, so "the LOD runs" here says nothing about real document
        // geometry - see the VALIDATION NOTE in
        // SoVulkanRenderBackendGeometryLod.cpp before trusting a nav-cube-only
        // result.
        const SbBool ok = m_manager.renderExternal(false, false,
                                                   cb,
                                                   m_window->defaultRenderPass(),
                                                   m_window->currentFramebuffer());
        if (!ok) {
            vkErr("startNextFrame: renderExternal FAILED");
        }

        vkdf->vkCmdEndRenderPass(cb);
    }

    // Report the path-tracing and ray-tracing status back to the GUI thread
    // (one frame of latency is acceptable for status getters).
    void reportStatus()
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingActive = m_manager.getPathTracingActive() ? true : false;
        m_pathTracingRefining =
            m_manager.getPathTracingRefining() ? true : false;
        m_rayTracingActive = m_manager.getRayTracingActive() ? true : false;
    }

    QVulkanInstance * m_instance = nullptr;
    SoNode * m_scene = nullptr;
    SoNode * m_overlayScene = nullptr;
    SoNode * m_decorationScene = nullptr;
    SoCamera * m_camera = nullptr;
    QVulkanWindow * m_window = nullptr;
    QuarterVulkanWidget * m_owner = nullptr;
#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
    //! Swapchain present mode requested through the widget API (0 FIFO default,
    //! 1 Mailbox, 2 Immediate); read once by preInitResources().
    int m_requestedPresentMode = 0;
#endif
    QSize m_lastSurfaceSize;
    //! Display/tuning settings as one blob (see SoVulkanViewSettings).  The
    //! manager diffs and applies the whole blob, so the renderer keeps no
    //! per-field "last applied" mirrors of its own.
    SoVulkanViewSettings m_viewSettings;
    // Capabilities probed by selectPhysicalDevice() (see setDeviceCaps).
    SoVulkanDeviceCaps m_deviceCaps {};
    bool m_deviceCapsValid = false;
    bool m_initialized = false;
    // Path tracing state mirrored here: requested values are written from
    // the widget API, startNextFrame() applies them to the manager during
    // frame setup and reports the active status back.
    bool m_pathTracingEnabled = false;
    bool m_pathTracingStart = false;
    bool m_pathTracingActive = false;
    bool m_pathTracingRefining = false;
    //! Interaction LOD runtime state (single-bounce preview while navigating).
    bool m_interactionLod = false;
    // Staged authoritative scene lighting (GL host -> RT backend).  The eye-
    // space data is camera-independent, so only a dirty flag (set on push /
    // on an empty reset) triggers a re-push to the manager.
    SoLightingData m_sceneLighting;
    bool m_sceneLightsDirty = false;
    bool m_appliedPathTracingEnabled = false;
    // Set when the RTX engine was (re)created (initResources()/lazy build) so
    // the next frame re-pushes every path-tracing setting to the fresh engine
    // instead of trusting the stale "applied" baselines.  Consumed once per
    // frame by snapshotFrameState().
    bool m_reapplyPathTracingSettings = false;
    bool m_rayTracingActive = false;
    // Device ray-tracing capability (does the physical device advertise the
    // KHR extension set?), known from the device probe regardless of whether
    // the backend is built.  This is what getRayTracingAvailable() reports and
    // what the adapter uses to decide whether a path-tracing request can ever
    // succeed on this GP.
    bool m_rtxBackendAvailable = false;
    // Whether the RTX backend is actually built and active right now.  Kept
    // distinct from device capability: a raster-first view (path tracing off)
    // never builds the RT backend, yet an RT-capable device reports
    // m_rtxBackendAvailable = true while m_rtxBackendBuilt = false.  The toggle
    // uses this to know whether a path-tracing request took effect.
    bool m_rtxBackendBuilt = false;
    // True once the renderer has determined availability (the device probe ran
    // or initResources() completed); before that the adapter must not warn
    // about missing hardware ray tracing.
    bool m_rtxBackendProbed = false;
    // The device context handed to SoVulkanRenderManager::initialize().  The
    // manager retains the POINTER (documented: the application must keep it
    // alive until shutdown) so ensureRayTracing() can lazily build the RTX
    // backend later; a stack local would dangle once initResources() returns
    // and the lazy RT build would read freed memory (garbage apiVersion ->
    // "requires a Vulkan 1.2+ device" on an RT-capable GPU, and a repeating
    // path-tracing fallback).
    SoVulkanDeviceContext m_initContext;
    // Guards the frame-state members below, which are written from the
    // widget API (and redraw sensors) and snapshotted by startNextFrame().
    // Qt 6 runs both on the GUI thread, so the lock documents the snapshot
    // contract rather than preventing data races; it also future-proofs the
    // code if rendering ever moves to a dedicated thread.
    mutable QMutex m_stateMutex;
    SoVulkanRenderManager m_manager;
    SoVulkanRenderTarget m_target;
    Detail::VulkanFrameDumper m_dumper;
};

}  // namespace

#endif  // FREECAD_USE_VULKAN
