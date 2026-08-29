// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "QuarterVulkanWidget.h"
#include "devices/InputDevice.h"
#include "QuarterWidget.h"
#include "VulkanFrameDumper.h"
#include <Base/VulkanBreadcrumbs.h>

#ifdef FREECAD_USE_VULKAN

#include <Base/Console.h>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoNode.h>
#include <Inventor/rendering/SoVulkanRenderManager.h>
#include <Inventor/rendering/SoVulkanRenderTarget.h>

#include <vulkan/vulkan.h>

#include <QVulkanInstance>
#include <QVulkanDeviceFunctions>
#include <QVulkanWindow>
#include <QVersionNumber>
#include <QVBoxLayout>

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QMutex>
#include <QMouseEvent>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>
#include <QtGui/6.11.2/QtGui/qpa/qwindowsysteminterface.h>

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SIM::Coin3D::Quarter;

namespace {

#define VK_TAG "[Vulkan] "

static void vkLog(const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Base::Console().log("%s%s\n", VK_TAG, buf);
}

static void vkWarn(const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Base::Console().warning("%s%s\n", VK_TAG, buf);
}

static void vkErr(const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Base::Console().error("%s%s\n", VK_TAG, buf);
}

// Format a VkPhysicalDevice API/driver version (VK_MAKE_API_VERSION layout).
static QByteArray vkVersionStr(uint32_t v)
{
    return QByteArray::number(VK_API_VERSION_MAJOR(v)) + '.' +
           QByteArray::number(VK_API_VERSION_MINOR(v)) + '.' +
           QByteArray::number(VK_API_VERSION_PATCH(v));
}


class QuarterVulkanRenderer;

class QuarterVulkanRenderer final : public QVulkanWindowRenderer
{
public:
    QuarterVulkanRenderer(QVulkanInstance * instance,
                          QVulkanWindow * window,
                          SoNode * scene,
                          SoCamera * camera,
                          QuarterVulkanWidget * owner,
                          bool rayTracing)
        : m_instance(instance)
        , m_scene(scene)
        , m_camera(camera)
        , m_window(window)
        , m_owner(owner)
        , m_rayTracing(rayTracing)
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
    void setBackgroundColor(const SbColor4f & color)
    {
        QMutexLocker locker(&m_stateMutex);
        m_background = color;
    }
    SbColor4f getBackgroundColor() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_background;
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
        m_backgroundGradient = enabled;
        m_backgroundTop = top;
        m_backgroundBottom = bottom;
    }
    void setWireframeOverlay(bool enabled)
    {
        QMutexLocker locker(&m_stateMutex);
        m_wireframeOverlay = enabled;
    }
    void setPointsOverlay(bool enabled)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pointsOverlay = enabled;
    }
    void setEdgeColor(const SbColor4f & color)
    {
        QMutexLocker locker(&m_stateMutex);
        m_edgeColor = color;
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
        m_pathTracingBounces = std::clamp(bounces, 1, 16);
    }
    void setPathTracingSettleFrames(int frames)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingSettleFrames = std::clamp(frames, 1, 120);
    }
    void setPathTracingMaxSamples(int samples)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingMaxSamples = std::clamp(samples, 1, 4096);
    }
    void setPathTracingDenoiser(const std::string & denoiser)
    {
        QMutexLocker locker(&m_stateMutex);
        m_pathTracingDenoiser = denoiser;
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

    void setViewMode(int mode)
    {
        QMutexLocker locker(&m_stateMutex);
        m_viewMode = mode;
    }
    int getViewMode() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_viewMode;
    }

    void setEnvMap(int index)
    {
        QMutexLocker locker(&m_stateMutex);
        m_envMap = index;
    }
    int getEnvMap() const
    {
        QMutexLocker locker(&m_stateMutex);
        return m_envMap;
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

    void preInitResources() override {}

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
        if (props) {
            m_initContext.apiVersion = props->apiVersion;
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

        m_dumper.initSwapChainResources();
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
        recordScenePass(cb, size, frame.background, multisample);

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
        SbColor4f background;
        SbColor4f backgroundTop;
        SbColor4f backgroundBottom;
        SbColor4f edgeColor;
        bool backgroundGradient = false;
        bool wireframeOverlay = false;
        bool pointsOverlay = false;
        bool pathTracingEnabled = false;
        int pathTracingBounces = 4;
        int pathTracingSettleFrames = 6;
        bool pathTracingDenoise = true;
    };

    // Apply a path-tracing setting to the manager only when it changed since
    // the last frame, and record it as applied.  Manager access is kept at
    // frame setup (see snapshotFrameState()); this just removes the repeated
    // diff-and-apply ceremony around the many scalar path-tracing settings.
    // When `force` is true the setting is pushed unconditionally (even when it
    // already matches `applied`): used right after a fresh RTX engine is
    // created, since a newly built engine starts from its own defaults (e.g.
    // ptDenoise is ON) and any setting that the manager previously "ignored"
    // (not-initialized) was marked applied without reaching the engine.
    template <typename T, typename Setter>
    void applyPathTracingSetting(const T& value, T& applied, Setter&& setter,
                                 bool force = false)
    {
        if (force || value != applied) {
            setter(value);
            applied = value;
        }
    }

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
        frame.background = m_background;
        frame.backgroundTop = m_backgroundTop;
        frame.backgroundBottom = m_backgroundBottom;
        frame.edgeColor = m_edgeColor;
        frame.backgroundGradient = m_backgroundGradient;
        frame.wireframeOverlay = m_wireframeOverlay;
        frame.pointsOverlay = m_pointsOverlay;
        frame.pathTracingEnabled = m_pathTracingEnabled;
        frame.pathTracingBounces = m_pathTracingBounces;
        frame.pathTracingSettleFrames = m_pathTracingSettleFrames;

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
        // defaults (ptDenoise is ON), so re-push every path-tracing setting
        // exactly once.  Guard on m_rtxBackendBuilt so a raster-only view (no
        // RT backend) never spams the "not initialized" manager warnings.
        const bool reapplyPT = m_rtxBackendBuilt
            && (m_reapplyPathTracingSettings || !rtxBefore);
        m_reapplyPathTracingSettings = false;
        applyPathTracingSetting(m_pathTracingBounces, m_appliedPathTracingBounces,
            [this](int v) {
                m_manager.setPathTracingBounces(static_cast<uint32_t>(v));
            },
            reapplyPT);
        applyPathTracingSetting(m_pathTracingSettleFrames, m_appliedPathTracingSettleFrames,
            [this](int v) {
                m_manager.setPathTracingSettleFrames(static_cast<uint32_t>(v));
            },
            reapplyPT);
        applyPathTracingSetting(m_pathTracingMaxSamples, m_appliedPathTracingMaxSamples,
            [this](int v) {
                m_manager.setPathTracingMaxSamples(static_cast<uint32_t>(v));
            },
            reapplyPT);
        // Denoising is required for path tracing, so it always runs when the
        // path tracer is active -- it is not an independent toggle the user
        // must remember to flip.  The Denoiser selector (below) only picks the
        // filter; "None" disables the filter and shows raw radiance.
        const bool effDenoise = m_pathTracingEnabled;
        frame.pathTracingDenoise = effDenoise;
        applyPathTracingSetting(effDenoise, m_appliedPathTracingDenoise,
            [this](bool v) {
                m_manager.setPathTracingDenoiseEnabled(v ? TRUE : FALSE);
            },
            reapplyPT);
        applyPathTracingSetting(m_pathTracingDenoiser, m_appliedPathTracingDenoiser,
            [this](const std::string& v) {
                m_manager.setPathTracingDenoiser(v.empty() ? nullptr : v.c_str());
            },
            reapplyPT);
        // Apply the ray-traced view mode (Interactive/AO/PathTracing) when it
        // changed, so the manager (and the shader's u_state.y) picks AO vs
        // multi-bounce.  The RT backend must be initialized first; the enable
        // toggle above builds it lazily when path tracing was requested.
        // `reapplyPT` re-pushes even when the request matches the recorded
        // baseline: a freshly (re)built engine starts from its own defaults,
        // so a mode chosen while the backend was down would otherwise be
        // swallowed by the equality check and never reach the new engine.
        if (reapplyPT || m_viewMode != m_appliedViewMode) {
            if (m_rtxBackendBuilt) {
                m_manager.setViewMode(m_viewMode);
            }
            m_appliedViewMode = m_viewMode;
        }
        // Environment "cubemap" preset: apply to the manager when it changed,
        // so the environment-lit view (and the path-tracer sky) use the
        // selected sky instead of the viewport gradient.  Index -1 (both
        // members' default) means "no preset": in Vulkan the engine then
        // renders the sky from the viewport background gradient, and the
        // freshly built RTX engine itself already defaults to -1, so forcing
        // a -1 re-push onto it early-returns in setEnvMap() unchanged.
        if (reapplyPT || m_envMap != m_appliedEnvMap) {
            if (m_rtxBackendBuilt) {
                m_manager.setEnvMap(m_envMap);
            }
            m_appliedEnvMap = m_envMap;
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
        m_manager.setBackgroundColor(frame.background);
        VK_BREADCRUMB_ONCE("[VK-TRACE] startNextFrame: setBackgroundGradient "
                           "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                           frame.backgroundGradient ? 1 : 0,
                           frame.backgroundTop[0], frame.backgroundTop[1],
                           frame.backgroundTop[2],
                           frame.backgroundBottom[0], frame.backgroundBottom[1],
                           frame.backgroundBottom[2]);
        m_manager.setBackgroundGradient(frame.backgroundGradient,
                                        frame.backgroundTop,
                                        frame.backgroundBottom);
        m_manager.setWireframeOverlay(frame.wireframeOverlay);
        m_manager.setPointsOverlay(frame.pointsOverlay);
        m_manager.setEdgeColor(frame.edgeColor);
        if (Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG")) {
            static int syncLog = 0;
            if (syncLog++ < 3) {
                Base::Console().message("[VK-SET] startNextFrame wire=%d points=%d "
                                       "edge=(%.2f,%.2f,%.2f,%.2f)\n",
                                       frame.wireframeOverlay ? 1 : 0,
                                    frame.pointsOverlay ? 1 : 0,
                                    frame.edgeColor[0], frame.edgeColor[1],
                                    frame.edgeColor[2], frame.edgeColor[3]);
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
        vkdf->vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        // QVulkanWindow's default render pass already clears color and depth
        // with the values in rpBegin above, so the backend must not issue
        // its own full-frame clear attachments (a second clear per frame).
        // The overlay block's sub-rect depth clear is unaffected.
        const SbBool ok = m_manager.renderExternal(false, false,
                                                   cb,
                                                   m_window->defaultRenderPass());
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
    QSize m_lastSurfaceSize;
    SbColor4f m_background = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool m_backgroundGradient = false;
    SbColor4f m_backgroundTop = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    SbColor4f m_backgroundBottom = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool m_wireframeOverlay = false;
    bool m_pointsOverlay = false;
    SbColor4f m_edgeColor = SbColor4f(0.05f, 0.05f, 0.05f, 1.0f);
    bool m_initialized = false;
    bool m_rayTracing = false;
    // Path tracing state mirrored here: requested values are written from
    // the widget API, startNextFrame() applies them to the manager during
    // frame setup and reports the active status back.
    bool m_pathTracingEnabled = false;
    bool m_pathTracingStart = false;
    bool m_pathTracingActive = false;
    bool m_pathTracingRefining = false;
    // Ray-traced view mode: Interactive (raster/off), AmbientOcclusion
    // (single-sample AO preview) or PathTracing (accumulating).  Stage from
    // the widget API and apply to the manager each frame, like the other
    // path-tracing settings.
    int m_viewMode = 0;   // 0=Interactive 1=AmbientOcclusion 2=PathTracing
    int m_appliedViewMode = -1;   // mirror so a change is seen exactly once
    // "Cubemap" environment preset (see setEnvMap); staged from the widget
    // API and applied to the manager each frame like the view mode.  -1 =
    // use the viewport background gradient.
    int m_envMap = -1;
    int m_appliedEnvMap = -1;
    bool m_appliedPathTracingEnabled = false;
    int m_pathTracingBounces = 4;
    int m_pathTracingSettleFrames = 6;
    int m_pathTracingMaxSamples = 256;
    std::string m_pathTracingDenoiser;   // "" = default (env/backend)
    int m_appliedPathTracingBounces = 0;
    int m_appliedPathTracingSettleFrames = 0;
    int m_appliedPathTracingMaxSamples = 0;
    // The denoiser baseline must reflect the backend's REAL initial state, which
    // is ON (SoRTXRenderBackend::ptDenoise defaults to TRUE every engine create).
    // Initializing it to false made applyPathTracingSetting(false, false,
    // ...) a no-op, so a fresh view ("denoiser off", the raster default, or an
    // RT view with the denoiser disabled) never pushed ptDenoise to FALSE and
    // the backend kept its default-on.  With the baseline TRUE the FIRST frame
    // always pushes the requested (usually off) state once.
    bool m_appliedPathTracingDenoise = true;
    std::string m_appliedPathTracingDenoiser;
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

/*!
  \brief Bridges QVulkanWindow's swapchain to SoVulkanRenderManager.

  Records the retained scene into QVulkanWindow's own command buffer and
  render pass via SoVulkanRenderManager::renderExternal().  QVulkanWindow owns
  the render-pass begin/end, submission, and the present-layout transition, so
  the backend never submits to the queue on this path.
*/
class QuarterVulkanWindow : public QVulkanWindow
{
public:
    QuarterVulkanWindow(QVulkanInstance * instance,
                        SoNode * scene,
                        SoCamera * camera,
                        QuarterVulkanWidget * owner,
                        bool rayTracing)
        : m_renderer(new QuarterVulkanRenderer(instance, this, scene, camera, owner, rayTracing))
    {
    }

    QVulkanWindowRenderer * createRenderer() override { return m_renderer; }

    QuarterVulkanRenderer * renderer() const { return m_renderer; }

    // Ray tracing feature structs referenced by the enabled-features
    // modifier (see QuarterVulkanWidget).  They must outlive the modifier
    // call: QVulkanWindowPrivate::init() uses the populated VkPhysical-
    // DeviceFeatures2 (with this pNext chain) when it later calls
    // vkCreateDevice, so structs on the modifier lambda's stack would
    // dangle.
    VkPhysicalDeviceBufferDeviceAddressFeatures rtBufferDeviceAddress {};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR rtAccelerationStructure {};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtRayTracingPipeline {};
    VkPhysicalDeviceRayQueryFeaturesKHR rtRayQuery {};
    // Whether the device supports VK_POLYGON_MODE_LINE/POINT
    // (fillModeNonSolid); the wireframe/points overlay pipelines need it
    // and requesting an unsupported core feature would fail device
    // creation, so it is queried up front and only then requested.
    bool fillModeNonSolid = false;
    // Whether the selected device advertises the ray-tracing extension set
    // (VK_KHR_acceleration_structure / ray_tracing_pipeline / ray_query).
    // Determined by the physical-device probe so the feature request below
    // matches the device QVulkanWindow actually creates.
    bool rtRayTracingAvailable = false;
    // Whether the selected device supports VK_KHR_external_semaphore_fd.
    // The CUDA/OptiX denoiser interop imports Vulkan FD semaphores, but
    // requesting the extension on a device without it would fail device
    // creation, so query it before adding it to the device extension set.
    bool rtExternalSemaphoreFdAvailable = false;

private:
    QuarterVulkanRenderer * m_renderer;
};

// Process-wide shared QVulkanInstance (Qt intends a single app-wide instance;
// the app's N 3D views all use it).  Owned here so a view's destructor can
// release it and reset the pointer; if the pointer were left dangling a later
// view (e.g. close a document then reopen it, which destroys and re-creates
// the view) would reuse a freed instance and crash inside
// selectPhysicalDevice()/vkEnumeratePhysicalDevices().
struct SharedVulkanInstance {
    QMutex mutex;
    QVulkanInstance * instance = nullptr;
    int refs = 0;
};
static SharedVulkanInstance g_sharedVulkanInstance;

} // namespace

class SIM::Coin3D::Quarter::QuarterVulkanWidgetPrivate
{
public:
    QVulkanInstance * instance = nullptr;
    // Shared-instance bookkeeping (see the constructor): the last widget
    // destroys the process-wide QVulkanInstance.
    QMutex * instanceMutex = nullptr;
    int * instanceRefs = nullptr;
    QVulkanWindow * window = nullptr;
    QuarterVulkanWindow * vulkanWindow = nullptr;
    QWidget * container = nullptr;
    QuarterVulkanRenderer * renderer = nullptr;
    SoNode * scene = nullptr;
    SoNode * overlayScene = nullptr;
    SoNode * decorationScene = nullptr;
    SoCamera * camera = nullptr;
    bool rayTracing = false;
    // Auto-nulled when the forwarded widget is destroyed, so the event
    // filter below can never dereference a dangling pointer.
    QPointer<QWidget> forwardTarget;

    // Debug-only synthetic mouse injector state (see pollInjectFile()).
    QString injectPath;
    int injectConsumed = 0;
};

QuarterVulkanWidget::QuarterVulkanWidget(QWidget * parent, bool rayTracing)
    : QWidget(parent)
    , d(new QuarterVulkanWidgetPrivate)
{
    vkLog("QuarterVulkanWidget: constructing%s",
          rayTracing ? " (ray tracing requested)" : "");
    d->rayTracing = rayTracing;

    this->ensureSharedInstance();

    d->vulkanWindow = new QuarterVulkanWindow(d->instance, d->scene, d->camera,
                                              this, rayTracing);
    d->window = d->vulkanWindow;
    d->window->setVulkanInstance(d->instance);
    d->renderer = d->vulkanWindow->renderer();

    this->selectPhysicalDevice();
    this->configureDeviceFeatures(rayTracing);
    this->logSupportedSampleCounts();

    d->container = QWidget::createWindowContainer(d->window, this);
    d->container->setFocusPolicy(Qt::StrongFocus);
    auto * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->container);

    // Forward input events arriving on the Vulkan surface to the hidden
    // OpenGL viewer (configured via setEventForwardTarget()) so navigation,
    // picking and other viewport interaction keep working.
    d->container->installEventFilter(this);
    d->window->installEventFilter(this);

    // Debug-only synthetic mouse injector.  A QWindowContainer swallows
    // QCoreApplication::sendEvent() input, so the only way a test probe can
    // drive the real event filter is a genuine platform event posted to the
    // embedded window.  Enabling this is zero-cost unless the env var names a
    // file to poll (see pollInjectFile()).
    if (const char * injectPath = ::getenv("FC_VULKAN_INJECT_PY")) {
        d->injectPath = injectPath;
        injectTimer = new QTimer(this);
        injectTimer->setInterval(10);
        QObject::connect(injectTimer, &QTimer::timeout, this,
                         &QuarterVulkanWidget::pollInjectFile);
        injectTimer->start();
    }
}

// One QVulkanInstance is shared across every 3D view (Qt intends a single
// app-wide instance; N views no longer allocate N instances).  Refcounted
// so the last widget tears it down.  Tearing it down resets the shared
// pointer to nullptr so the next widget (e.g. closing a document and
// reopening it, which destroys and re-creates the view) allocates a fresh
// instance instead of reusing a freed one.
void QuarterVulkanWidget::ensureSharedInstance()
{
    QMutexLocker locker(&g_sharedVulkanInstance.mutex);
    if (g_sharedVulkanInstance.instance &&
        !g_sharedVulkanInstance.instance->isValid() &&
        g_sharedVulkanInstance.refs == 0) {
        // A previous failed or abandoned creation can leave a sticky invalid
        // pointer behind.  Drop it when nobody references it so reopening a
        // view gets a fresh attempt instead of reusing the invalid instance.
        delete g_sharedVulkanInstance.instance;
        g_sharedVulkanInstance.instance = nullptr;
    }
    if (!g_sharedVulkanInstance.instance) {
        g_sharedVulkanInstance.instance = new QVulkanInstance;
        // Ray tracing requires Vulkan 1.2+ (acceleration-structure and
        // ray-tracing-pipeline APIs are core-adjacent KHR extensions
        // promoted to 1.2); advertise 1.2 so the device can expose them.
        g_sharedVulkanInstance.instance->setApiVersion(QVersionNumber(1, 2, 0));
        // The validation layer is opt-in (FC_VULKAN_VALIDATION): it costs
        // real CPU per draw and must not ship enabled by default.
        if (Base::envFlagEnabled("FC_VULKAN_VALIDATION")) {
            g_sharedVulkanInstance.instance->setLayers(
                {QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        }
        // External memory interop with CUDA (for the RTX denoiser) needs the
        // instance capability extensions in addition to the device one, so the
        // FD export is usable.  Enabling them is free on 1.2+ and harmless if
        // the loader/driver lacks them (Qt tolerates unsupported instance
        // extensions in its setExtensions list).
        g_sharedVulkanInstance.instance->setExtensions({
            QByteArrayLiteral("VK_KHR_external_memory_capabilities"),
            QByteArrayLiteral("VK_KHR_external_memory"),
            QByteArrayLiteral("VK_KHR_external_semaphore_capabilities"),
            QByteArrayLiteral("VK_KHR_external_semaphore"),
        });
        if (!g_sharedVulkanInstance.instance->create()) {
            vkWarn("QuarterVulkanWidget: could not create instance with "
                   "validation layer (error %d), retrying without layers",
                   static_cast<int>(
                       g_sharedVulkanInstance.instance->errorCode()));
            g_sharedVulkanInstance.instance->setLayers({});
            g_sharedVulkanInstance.instance->create();
        }

        if (g_sharedVulkanInstance.instance->isValid()) {
            const QVersionNumber api =
                g_sharedVulkanInstance.instance->supportedApiVersion();
            vkLog("QuarterVulkanWidget: instance created (Vulkan %d.%d.%d)",
                  api.majorVersion(), api.minorVersion(), api.microVersion());
            const auto layers = g_sharedVulkanInstance.instance->layers();
            for (const QByteArray & l : layers) {
                vkLog("  enabled layer: %s", l.constData());
            }
            const auto extensions =
                g_sharedVulkanInstance.instance->extensions();
            for (const QByteArray & e : extensions) {
                vkLog("  enabled extension: %s", e.constData());
            }
        }
        else {
            vkErr("QuarterVulkanWidget: Vulkan instance creation FAILED "
                  "(error %d)",
                  static_cast<int>(
                      g_sharedVulkanInstance.instance->errorCode()));
        }
    }
    ++g_sharedVulkanInstance.refs;
    d->instance = g_sharedVulkanInstance.instance;
    d->instanceRefs = &g_sharedVulkanInstance.refs;
    d->instanceMutex = &g_sharedVulkanInstance.mutex;
}

// Drop a widget's reference on the shared instance; the last widget destroys
// the instance AND resets the shared pointer, so a later view creates a fresh
// one.
void QuarterVulkanWidget::releaseSharedInstance()
{
    QMutexLocker locker(&g_sharedVulkanInstance.mutex);
    if (--g_sharedVulkanInstance.refs == 0) {
        delete g_sharedVulkanInstance.instance;
        g_sharedVulkanInstance.instance = nullptr;
    }
}

// Select the physical device and force QVulkanWindow to use it.
//
// QVulkanWindow would otherwise default to physical device 0, which on
// multi-GPU systems is frequently the integrated (or virtual) GPU rather
// than the discrete one.  Enumerate every device, score by device type
// (discrete ranks highest), and use the feature support the overlays and
// ray tracing actually need as a tie-breaker, then pin the selection with
// setPhysicalDeviceIndex() so configureDeviceFeatures()' requests always
// match the device that is created.  The score weights keep a discrete GPU
// ahead of any integrated GPU even when the discrete one lacks a secondary
// feature (fillModeNonSolid or ray tracing), so the dedicated GPU is always
// preferred; a warning is logged when the chosen device cannot satisfy the
// requested mode so the caller can fall back.
//
// The wireframe/points overlay pipelines use VK_POLYGON_MODE_LINE/POINT,
// which require the fillModeNonSolid device feature (not requested by Qt by
// default).  Requesting an unsupported core feature fails device creation,
// so the feature is probed per device and only requested when present.
void QuarterVulkanWidget::selectPhysicalDevice()
{
    // The instance may have failed to create (no driver/loader on this
    // machine -- ensureSharedInstance() logs the failure and the constructor
    // continues so the widget can still fall back gracefully).  In that state
    // QVulkanInstance::functions() carries null entry points, and calling
    // vkEnumeratePhysicalDevices through them crashes on construction.  Bail
    // out instead; Qt will refuse to initialize the window with the invalid
    // instance and the view stays empty rather than taking the process down.
    if (!d->instance || !d->instance->isValid()) {
        vkErr("QuarterVulkanWidget: Vulkan instance is invalid; skipping "
              "physical device selection");
        return;
    }
    auto * f = d->instance->functions();
    uint32_t devCount = 0;
    f->vkEnumeratePhysicalDevices(d->instance->vkInstance(), &devCount,
                                  nullptr);
    if (devCount == 0) {
        vkErr("QuarterVulkanWidget: no Vulkan physical devices available");
        return;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    f->vkEnumeratePhysicalDevices(d->instance->vkInstance(), &devCount,
                                  devs.data());

    int bestIndex = 0;
    int bestScore = -1;
    bool bestFillMode = false;
    bool bestRt = false;
    bool bestExternalSemaphoreFd = false;
    for (uint32_t i = 0; i < devCount; ++i) {
        VkPhysicalDeviceProperties props {};
        f->vkGetPhysicalDeviceProperties(devs[i], &props);
        int typeScore = 5;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            typeScore = 100;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            typeScore = 50;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            typeScore = 30;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            typeScore = 15;
            break;
        default:
            typeScore = 5;
            break;
        }
        VkPhysicalDeviceFeatures devFeatures {};
        f->vkGetPhysicalDeviceFeatures(devs[i], &devFeatures);
        const bool fillMode = devFeatures.fillModeNonSolid ? true : false;
        const bool rtReady = this->deviceSupportsRayTracing(devs[i]);
        const bool extSemFd =
            this->deviceSupportsExtension(devs[i], "VK_KHR_external_semaphore_fd");
        // Tie-breakers stay below the discrete/integrated type gap (50) so a
        // dedicated GPU is always preferred over an integrated one that
        // happens to have more secondary features.
        const int score =
            typeScore + (fillMode ? 20 : 0) + (rtReady ? 40 : 0);
        vkLog("QuarterVulkanWidget: device %d '%s' type=%d "
              "fillModeNonSolid=%d rayTracing=%d score=%d",
              static_cast<int>(i), props.deviceName,
              static_cast<int>(props.deviceType), fillMode ? 1 : 0,
              rtReady ? 1 : 0, score);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = static_cast<int>(i);
            bestFillMode = fillMode;
            bestRt = rtReady;
            bestExternalSemaphoreFd = extSemFd;
        }
    }

    d->vulkanWindow->fillModeNonSolid = bestFillMode;
    d->vulkanWindow->rtRayTracingAvailable = bestRt;
    d->vulkanWindow->rtExternalSemaphoreFdAvailable = bestExternalSemaphoreFd;
    // Pin QVulkanWindow to the GPU we probed, so the feature/extensions
    // requested by configureDeviceFeatures() are guaranteed to be supported
    // by the device that is actually created.
    d->window->setPhysicalDeviceIndex(bestIndex);
    vkLog("QuarterVulkanWidget: selected physical device %d "
          "(fillModeNonSolid=%d rayTracing=%d)",
          bestIndex, bestFillMode ? 1 : 0, bestRt ? 1 : 0);
    if (d->rayTracing && !bestRt) {
        vkWarn("QuarterVulkanWidget: the selected device does not support "
               "ray tracing; falling back to the raster backend.");
    }
}

// True when \a device advertises the ray-tracing extension set
// (VK_KHR_acceleration_structure, VK_KHR_ray_tracing_pipeline,
// VK_KHR_ray_query) that SoRTXRenderBackend requires.
bool QuarterVulkanWidget::deviceSupportsRayTracing(VkPhysicalDevice device)
{
    auto * f = d->instance->functions();
    uint32_t extCount = 0;
    f->vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount,
                                            nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount > 0) {
        f->vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount,
                                                exts.data());
    }
    bool haveAS = false;
    bool haveRTPipeline = false;
    bool haveRayQuery = false;
    for (const auto & ext : exts) {
        if (std::strcmp(ext.extensionName, "VK_KHR_acceleration_structure")
            == 0) {
            haveAS = true;
        }
        else if (std::strcmp(ext.extensionName,
                             "VK_KHR_ray_tracing_pipeline")
                 == 0) {
            haveRTPipeline = true;
        }
        else if (std::strcmp(ext.extensionName, "VK_KHR_ray_query") == 0) {
            haveRayQuery = true;
        }
    }
    return haveAS && haveRTPipeline && haveRayQuery;
}

bool QuarterVulkanWidget::deviceSupportsExtension(VkPhysicalDevice device,
                                                  const char * name)
{
    if (!d->instance || !d->instance->functions() || !name) {
        return false;
    }
    auto * f = d->instance->functions();
    uint32_t extCount = 0;
    f->vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount,
                                            nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount > 0) {
        f->vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount,
                                                exts.data());
    }
    for (const auto & ext : exts) {
        if (std::strcmp(ext.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// Request the device extensions and feature structs for ray tracing (when
// the chosen device supports them) plus fillModeNonSolid for the
// wireframe/points overlays, before the window is first shown:
// QVulkanWindow creates the device on first expose.
//
// The ray-tracing feature set is requested whenever the device advertises
// it, NOT only when the view was created with UseVulkanRayTracing.  That
// keeps the RTX backend always available so path tracing can be toggled live
// (raster <-> RT) with a preference change, instead of forcing a document
// reopen.  The construction `rayTracing` flag only influences the log below.
void QuarterVulkanWidget::configureDeviceFeatures(bool rayTracing)
{
    // Tell the renderer whether the selected device advertises the
    // ray-tracing extension set, so isRayTracingAvailable() reflects hardware
    // capability (not whether the backend has been built yet).  This must run
    // before the first initResources() so an RT-capable device opened in
    // raster mode does not report "unavailable".
    if (d->renderer) {
        d->renderer->setRayTracingDeviceSupported(
            d->vulkanWindow->rtRayTracingAvailable);
    }

    // The probe selected the physical device and recorded whether it
    // advertises the ray-tracing extension set; requesting extensions a
    // device does not support would fail device creation.
    if (!d->vulkanWindow->rtRayTracingAvailable) {
        if (rayTracing) {
            vkWarn("QuarterVulkanWidget: ray tracing requested but the selected "
                   "device does not advertise VK_KHR_ray_tracing_pipeline / "
                   "VK_KHR_acceleration_structure; falling back to raster");
        }
        // Still request fillModeNonSolid for the wireframe/points overlay
        // pipelines.
        d->window->setEnabledFeaturesModifier(
          [this](VkPhysicalDeviceFeatures2 & features) {
            features.features.fillModeNonSolid =
              d->vulkanWindow->fillModeNonSolid ? VK_TRUE : VK_FALSE;
          });
        return;
    }

    QList<QByteArray> deviceExt {
        QByteArrayLiteral("VK_KHR_acceleration_structure"),
        QByteArrayLiteral("VK_KHR_ray_tracing_pipeline"),
        QByteArrayLiteral("VK_KHR_ray_query"),
        QByteArrayLiteral("VK_KHR_deferred_host_operations"),
        QByteArrayLiteral("VK_KHR_external_memory_fd"),
    };
    if (d->vulkanWindow->rtExternalSemaphoreFdAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_external_semaphore")
                  << QByteArrayLiteral("VK_KHR_external_semaphore_fd");
    }
    d->window->setDeviceExtensions(deviceExt);
    vkLog("QuarterVulkanWidget: request device ext: "
          "accel_structure, ray_tracing_pipeline, ray_query, "
          "deferred_host_ops, external_memory_fd%s",
          d->vulkanWindow->rtExternalSemaphoreFdAvailable
            ? ", external_semaphore, external_semaphore_fd"
            : "");
    // Enable the device features behind those extensions.  The modifier
    // receives VkPhysicalDeviceFeatures2 after Qt has populated it; chain
    // the RT feature structs onto pNext.
    //
    // VK_KHR_acceleration_structure requires the core bufferDeviceAddress
    // feature: every BLAS/TLAS is referenced by device address and the RTX
    // backend calls vkGetBufferDeviceAddress() unconditionally.  Without
    // this feature the addresses are zero and the acceleration structure
    // builds are invalid (validation error or device lost on strict
    // drivers).
    //
    // The path tracer runs as a VK_KHR_ray_tracing_pipeline with a
    // five-group shader binding table, so the ray-tracing-pipeline feature
    // is required in addition.
    //
    // The feature structs live on the window object (not the modifier
    // lambda's stack): QVulkanWindowPrivate::init() reads the pNext chain
    // after the callback returns.
    d->vulkanWindow->rtBufferDeviceAddress.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    d->vulkanWindow->rtBufferDeviceAddress.bufferDeviceAddress = VK_TRUE;
    d->vulkanWindow->rtAccelerationStructure.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    d->vulkanWindow->rtAccelerationStructure.accelerationStructure = VK_TRUE;
    d->vulkanWindow->rtRayTracingPipeline.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    d->vulkanWindow->rtRayTracingPipeline.rayTracingPipeline = VK_TRUE;
    // The default dispatch mode is a ray-query compute path tracer
    // (FC_VULKAN_RT_SBT=1 opts into the ray tracing pipeline), so the
    // ray-query feature is required in addition.
    d->vulkanWindow->rtRayQuery.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    d->vulkanWindow->rtRayQuery.rayQuery = VK_TRUE;
    d->window->setEnabledFeaturesModifier(
      [this](VkPhysicalDeviceFeatures2 & features) {
        features.features.fillModeNonSolid =
          d->vulkanWindow->fillModeNonSolid ? VK_TRUE : VK_FALSE;
        d->vulkanWindow->rtRayQuery.pNext = features.pNext;
        d->vulkanWindow->rtRayTracingPipeline.pNext =
          &d->vulkanWindow->rtRayQuery;
        d->vulkanWindow->rtAccelerationStructure.pNext =
          &d->vulkanWindow->rtRayTracingPipeline;
        d->vulkanWindow->rtBufferDeviceAddress.pNext =
          &d->vulkanWindow->rtAccelerationStructure;
        features.pNext = &d->vulkanWindow->rtBufferDeviceAddress;
      });
    vkLog("QuarterVulkanWidget: requested ray tracing pipeline device "
          "extensions");
}

void QuarterVulkanWidget::logSupportedSampleCounts()
{
    const QList<int> samples = d->window->supportedSampleCounts();
    QStringList parts;
    parts.reserve(samples.size());
    for (int s : samples) {
        parts << QString::number(s);
    }
    QByteArray samplesStr = parts.join(QLatin1Char(',')).toUtf8();
    vkLog("QuarterVulkanWidget: supported sample counts: %s",
          samplesStr.isEmpty() ? "(none)" : samplesStr.constData());
}

QuarterVulkanWidget::~QuarterVulkanWidget()
{
    vkLog("QuarterVulkanWidget: destroying");
    // Stop forwarding events before anything is freed: deferred events
    // delivered to the container/window after `d` is gone would otherwise
    // hit the event filter with a dangling private pointer.
    if (d->container) {
        d->container->removeEventFilter(this);
    }
    if (d->window) {
        d->window->removeEventFilter(this);
    }
    // The container owns the QVulkanWindow child; destroying it destroys
    // the window and the renderer it owns while the QVulkanInstance is
    // still alive (the renderer shutdown needs the device/queue).
    delete d->container;
    d->container = nullptr;
    d->window = nullptr;
    d->vulkanWindow = nullptr;
    d->renderer = nullptr;
    // QVulkanWindow::setVulkanInstance() does not take ownership; release
    // our reference on the shared instance (the last widget destroys it and
    // resets the shared pointer so a later view creates a fresh instance).
    this->releaseSharedInstance();
    d->instance = nullptr;
    delete d;
}

void QuarterVulkanWidget::setSceneGraph(SoNode * root)
{
    d->scene = root;
    d->renderer->setScene(root);
    redraw();
}

SoNode * QuarterVulkanWidget::getSceneGraph() const
{
    return d->scene;
}

void QuarterVulkanWidget::setOverlaySceneGraph(SoNode * root)
{
    d->overlayScene = root;
    d->renderer->setOverlayScene(root);
    redraw();
}

SoNode * QuarterVulkanWidget::getOverlaySceneGraph() const
{
    return d->overlayScene;
}

void QuarterVulkanWidget::setDecorationSceneGraph(SoNode * root)
{
    d->decorationScene = root;
    d->renderer->setDecorationScene(root);
    redraw();
}

SoNode * QuarterVulkanWidget::getDecorationSceneGraph() const
{
    return d->decorationScene;
}

void QuarterVulkanWidget::setCamera(SoCamera * camera)
{
    d->camera = camera;
    d->renderer->setCamera(camera);
    redraw();
}

SoCamera * QuarterVulkanWidget::getCamera() const
{
    return d->camera;
}

void QuarterVulkanWidget::setBackgroundColor(const SbColor4f & color)
{
    d->renderer->setBackgroundColor(color);
    redraw();
}

void QuarterVulkanWidget::setBackgroundGradient(bool enabled,
                                                const SbColor4f & topColor,
                                                const SbColor4f & bottomColor)
{
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setBackgroundGradient "
                  "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                  enabled ? 1 : 0, topColor[0], topColor[1], topColor[2],
                  bottomColor[0], bottomColor[1], bottomColor[2]);
    d->renderer->setBackgroundGradient(enabled, topColor, bottomColor);
    redraw();
}

void QuarterVulkanWidget::setWireframeOverlay(bool enabled)
{
    d->renderer->setWireframeOverlay(enabled);
    redraw();
}

void QuarterVulkanWidget::setPointsOverlay(bool enabled)
{
    d->renderer->setPointsOverlay(enabled);
    redraw();
}

void QuarterVulkanWidget::setEdgeColor(const SbColor4f & color)
{
    d->renderer->setEdgeColor(color);
    redraw();
}

void QuarterVulkanWidget::setEventForwardTarget(QWidget * target,
                                                qreal targetDevicePixelRatio)
{
    // The per-event scale is derived from both widgets' *live* device pixel
    // ratios by InputDevice::crossWidgetPositionScale(); the ratio argument
    // is kept only for API compatibility.  targetDevicePixelRatio is unused:
    // reading a stale snapshot here is exactly what caused the forwarded pick
    // point to be rescaled by 1/dpr on fractional-scaling displays.
    Q_UNUSED(targetDevicePixelRatio);
    d->forwardTarget = target;
}

void QuarterVulkanWidget::pollInjectFile()
{
    if (d->injectPath.isEmpty() || !d->window) {
        return;
    }
    QFile f(d->injectPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) {
        return;
    }
    const QList<QByteArray> lines = bytes.split('\n');
    if (d->injectConsumed >= lines.size()) {
        // Nothing new: notice a trailing blank line so a subsequent append
        // (the probe re-writes the file) re-triggers a read next poll.
        if (lines.size() == 1 && lines[0].isEmpty()) {
            d->injectConsumed = 0;
        }
        return;
    }
    for (int i = d->injectConsumed; i < lines.size(); ++i) {
        const QList<QByteArray> tok = lines[i].trimmed().split(' ');
        if (tok.size() < 3) {
            continue;
        }
        const QEvent::Type type = [&tok]() {
            const QByteArray & t = tok[0];
            if (t == "move") return QEvent::MouseMove;
            if (t == "press") return QEvent::MouseButtonPress;
            if (t == "release") return QEvent::MouseButtonRelease;
            return QEvent::None;
        }();
        if (type == QEvent::None) {
            continue;
        }
        const QPointF local(tok[1].toDouble(), tok[2].toDouble());
        const QPointF global = d->container->mapToGlobal(
            QPoint(int(local.x()), int(local.y())));
        Qt::MouseButton btn = Qt::NoButton;
        Qt::MouseButtons state = Qt::NoButton;
        if (type == QEvent::MouseButtonPress) {
            btn = Qt::LeftButton;
            state = Qt::LeftButton;
        }
        else if (type == QEvent::MouseButtonRelease) {
            btn = Qt::LeftButton;
        }
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            d->window, local, QPointF(global.x(), global.y()), state, btn,
            type);
        d->injectConsumed = i + 1;
    }
}

bool QuarterVulkanWidget::eventFilter(QObject * watched, QEvent * event)
{
    Q_UNUSED(watched);
    if (!d->forwardTarget) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease) {
        const auto* me = static_cast<const QMouseEvent*>(event);
        const QWidget* gl = d->forwardTarget;
        const QWidget* container = d->container;
        VK_BREADCRUMB_SAMPLED(32, "[VK-TRACE] eventFilter watched=%s type=%d pos=(%.1f,%.1f) "
                      "global=(%.1f,%.1f) | container rect=(%d,%d %dx%d) dpr=%.2f "
                      "| glWidget rect=(%d,%d %dx%d) dpr=%.2f\n",
                      watched == d->container ? "container"
                      : (watched == static_cast<QObject*>(d->window) ? "window"
                                                                     : "other"),
                      static_cast<int>(event->type()),
                      me->position().x(), me->position().y(),
                      me->globalPosition().x(), me->globalPosition().y(),
                      container->x(), container->y(), container->width(),
                      container->height(), container->devicePixelRatioF(),
                      gl->x(), gl->y(), gl->width(), gl->height(),
                      gl->devicePixelRatioF());
    }

    // Forward input events from the visible Vulkan container to the hidden
    // OpenGL viewer (set via setEventForwardTarget()).  The two widgets share
    // the same window/screen and therefore the same (possibly fractional)
    // system device pixel ratio, so the container-to-viewer position scale is
    // exactly 1.0 on any OS (Windows 100-200%, macOS Retina 2.0, Linux 1.25,
    // ...): the position passes through unscaled and the GL side applies its
    // own live ratio in InputDevice::toDevicePixelPosition().  The scale is
    // taken from the *live* devicePixelRatioF() of both widgets (single source
    // of truth), never a cached/pre-rounded ratio, so fractional scales cannot
    // rescaled the forwarded point by 1/dpr and drift hovering/picking off
    // center towards the origin.
    const qreal dprScale = InputDevice::crossWidgetPositionScale(
        d->container, d->forwardTarget.data());

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: {
        const auto * me = static_cast<const QMouseEvent *>(event);
        if (qFuzzyCompare(dprScale, 1.0)) {
            if (QCoreApplication::sendEvent(d->forwardTarget, event)) {
                return true;
            }
            break;
        }
        QMouseEvent scaled(me->type(), me->position() * dprScale,
                           me->globalPosition(), me->button(), me->buttons(),
                           me->modifiers(), me->pointingDevice());
        if (QCoreApplication::sendEvent(d->forwardTarget, &scaled)) {
            return true;
        }
        break;
    }
    case QEvent::Wheel: {
        const auto * we = static_cast<const QWheelEvent *>(event);
        if (qFuzzyCompare(dprScale, 1.0)) {
            if (QCoreApplication::sendEvent(d->forwardTarget, event)) {
                return true;
            }
            break;
        }
        QWheelEvent scaled(we->position() * dprScale, we->globalPosition(),
                           we->pixelDelta(), we->angleDelta(), we->buttons(),
                           we->modifiers(), we->phase(), we->inverted());
        if (QCoreApplication::sendEvent(d->forwardTarget, &scaled)) {
            return true;
        }
        break;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletMove:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::ContextMenu:
        if (QCoreApplication::sendEvent(d->forwardTarget, event)) {
            return true;
        }
        break;
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

SbColor4f QuarterVulkanWidget::getBackgroundColor() const
{
    return d->renderer->getBackgroundColor();
}

void QuarterVulkanWidget::setClearEnabled(bool clearwindow, bool clearzbuffer)
{
    // QVulkanWindow's default render pass always clears its attachments
    // (LOAD_OP_CLEAR), so frame clears cannot be disabled on the Vulkan
    // path.  Kept for API parity with QuarterWidget; warn once if a caller
    // requests anything else than the fixed behavior.
    Q_UNUSED(clearwindow)
    Q_UNUSED(clearzbuffer)
    static bool warned = false;
    if (!warned) {
        warned = true;
        vkWarn("setClearEnabled: QVulkanWindow's render pass always clears "
               "color and depth; request ignored");
    }
}

void QuarterVulkanWidget::setSampleCount(int samples)
{
    vkLog("setSampleCount: requesting %d samples", samples);
    d->window->setSampleCount(samples);
}

int QuarterVulkanWidget::getSampleCount() const
{
    return static_cast<int>(d->window->sampleCountFlagBits());
}

void QuarterVulkanWidget::setPreferredColorFormat(int vkFormat)
{
    vkLog("setPreferredColorFormat: requesting VkFormat %d", vkFormat);
    d->window->setPreferredColorFormats(
        QList<VkFormat>() << static_cast<VkFormat>(vkFormat));
}

void QuarterVulkanWidget::redraw()
{
    d->window->requestUpdate();
}

bool QuarterVulkanWidget::supportsGrab() const
{
    return d->window->supportsGrab();
}

QImage QuarterVulkanWidget::grab() const
{
    return d->window->grab();
}

bool QuarterVulkanWidget::isRayTracingActive() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getRayTracingActive();
}

uint32_t QuarterVulkanWidget::getRenderFrameCount() const
{
    if (!d->renderer) {
        return 0;
    }
    return d->renderer->getRenderFrameCount();
}

bool QuarterVulkanWidget::isRayTracingAvailable() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getRayTracingAvailable();
}

bool QuarterVulkanWidget::isRayTracingProbed() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getRayTracingProbed();
}

void QuarterVulkanWidget::setPathTracingEnabled(bool enabled)
{
    if (!d->renderer) {
        return;
    }
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setPathTracingEnabled enabled=%d\n",
                  enabled ? 1 : 0);
    d->renderer->setPathTracingEnabled(enabled);
    redraw();
}

bool QuarterVulkanWidget::getPathTracingEnabled() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getPathTracingEnabled();
}

void QuarterVulkanWidget::setViewMode(RtxViewMode mode)
{
    if (!d->renderer) {
        return;
    }
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setViewMode mode=%d\n",
                  static_cast<int>(mode));
    d->renderer->setViewMode(static_cast<int>(mode));
    redraw();
}

QuarterVulkanWidget::RtxViewMode QuarterVulkanWidget::getViewMode() const
{
    if (!d->renderer) {
        return RtxViewMode::Interactive;
    }
    return static_cast<RtxViewMode>(d->renderer->getViewMode());
}

void QuarterVulkanWidget::setEnvMap(int index)
{
    if (!d->renderer) {
        return;
    }
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setEnvMap index=%d\n",
                  index);
    d->renderer->setEnvMap(index);
    redraw();
}

int QuarterVulkanWidget::getEnvMap() const
{
    if (!d->renderer) {
        return -1;
    }
    return d->renderer->getEnvMap();
}

int QuarterVulkanWidget::getEnvMapCount()
{
    return SoVulkanRenderManager::getEnvMapCount();
}

const char * QuarterVulkanWidget::getEnvMapName(int index)
{
    return SoVulkanRenderManager::getEnvMapName(index);
}

void QuarterVulkanWidget::setPathTracingStart(bool start)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingStart(start);
    redraw();
}

bool QuarterVulkanWidget::getPathTracingActive() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getPathTracingActive();
}

void QuarterVulkanWidget::setPathTracingBounces(int bounces)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingBounces(bounces);
    redraw();
}

void QuarterVulkanWidget::setPathTracingSettleFrames(int frames)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingSettleFrames(frames);
    redraw();
}

void QuarterVulkanWidget::setPathTracingMaxSamples(int samples)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingMaxSamples(samples);
    redraw();
}

void QuarterVulkanWidget::setPathTracingDenoiser(const std::string & denoiser)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingDenoiser(denoiser);
    redraw();
}

QWidget * QuarterVulkanWidget::getNativeWidget()
{
    return d->container;
}

#endif // FREECAD_USE_VULKAN
