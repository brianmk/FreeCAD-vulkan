// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "QuarterVulkanWidget.h"
#include "devices/InputDevice.h"
#include "eventhandlers/EventFilter.h"
#include "QuarterWidget.h"

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
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QMutex>
#include <QMouseEvent>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>
#ifdef HAVE_QT6_GUI_PRIVATE
#include <QtGui/qpa/qwindowsysteminterface.h>
#endif

#include "Selection.h"

#include <cstdarg>
#include <cstdlib>
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
    // On by default; honors the conventional 0/false/off opt-out.
    static const bool enabled = []() {
        const char * value = std::getenv("FC_VULKAN_PERSISTENT_RESOURCES");
        if (!value || !*value) {
            return true;
        }
        return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0
            && std::strcmp(value, "off") != 0;
    }();
    return enabled;
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

    //! Interaction LOD is a runtime navigation state, not a persisted
    //! setting, so it is staged on its own rather than in m_viewSettings.
    void setInteractionLod(bool active)
    {
        QMutexLocker locker(&m_stateMutex);
        m_interactionLod = active;
    }
    // Staged authoritative scene lighting (GL host -> raster backend).  Applied
    // at the next startNextFrame() so every m_manager call stays inside frame
    // setup.  The set is camera-anchored world-space data, re-pushed by the
    // adapter on every camera move, so the dirty flag drives a fresh apply per
    // frame while the camera moves.
    void setSceneLights(const SoLightingData & lighting)
    {
        QMutexLocker locker(&m_stateMutex);
        m_sceneLighting = lighting;
        m_sceneLightsDirty = true;
    }

    //! Ordinal of the last presented frame (see SoVulkanRenderManager::
    //! getRenderFrameCount).  The same value is copied into that frame's
    //! SoRenderParams::frame so frame dumps can be correlated to this ordinal
    //! by the probe/checker layer.
    uint32_t getRenderFrameCount() const
    {
        return m_manager.getRenderFrameCount();
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
        m_initialized = m_manager.initialize(&m_initContext);
        if (m_initialized) {
            // Mirror the GL viewer (QuarterWidget sets
            // SoRenderManager::VARIABLE_NEAR_PLANE): re-fit the camera
            // near/far to the scene bounding box every frame so zooming and
            // orbiting never clip the model at the near/far planes.  The
            // hidden GL viewer never renders, so its own auto-clipping would
            // never run.
            m_manager.setAutoClipping(SoVulkanRenderManager::VARIABLE_NEAR_PLANE);
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
    }

    void releaseSwapChainResources() override
    {
        vkLog("releaseSwapChainResources");
        m_manager.setRenderTarget(nullptr);
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
    }

    void logicalDeviceLost() override
    {
        vkErr("logicalDeviceLost: VK_ERROR_DEVICE_LOST");
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

        m_window->frameReady();

        // The surface is display-only and owns no Coin sensors, so every
        // change must arrive as an explicit wake:
        //   - widget setters call redraw();
        //   - the VulkanViewportAdapter requests a frame on a document update,
        //     a selection/preselection change, a camera-node swap or an
        //     interactive camera-pose navigation.
        // No continuous refine loop is needed for the raster path.
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
        //! Interaction LOD engaged while the camera moves.
        bool interactionLod = false;
    };

    // Qt 6 invokes startNextFrame() on the GUI thread, like every other
    // access to these state members; the setters and redraw sensors run on
    // the same thread.  Snapshot everything under the mutex and use the
    // result for the rest of the frame so one frame always sees a
    // consistent set of values.
    FrameState snapshotFrameState()
    {
        FrameState frame;
        QMutexLocker locker(&m_stateMutex);
        frame.scene = m_scene;
        frame.overlayScene = m_overlayScene;
        frame.decorationScene = m_decorationScene;
        frame.camera = m_camera;
        frame.viewSettings = m_viewSettings;
        frame.interactionLod = m_interactionLod;

        // Push the GL-authoritative scene lighting to the render manager: the
        // raster executor keeps the view's view-relative lights following the
        // camera like Coin GL.
        if (m_sceneLightsDirty) {
            m_manager.setSceneLights(m_sceneLighting);
            m_sceneLightsDirty = false;
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
        // One call applies the whole display/tuning blob (the manager diffs it).
        m_manager.setViewSettings(frame.viewSettings);
        // Interaction LOD is a separate runtime state (not part of the diffed
        // settings blob).  The manager forwards it to the raster backend and is
        // idempotent, so applying it every frame is cheap.
        m_manager.setInteractionLod(frame.interactionLod ? TRUE : FALSE);
        if (std::getenv("COIN_VULKAN_BACKEND_DEBUG") != nullptr) {
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
        // vkCmdResetQueryPool must not be recorded inside a render pass, so
        // reset the GPU-timer query pool here, before the pass begins; the
        // backend's scopes then write into the reset range during renderExternal.
        m_manager.resetExternalGpuQueries(cb);
        vkdf->vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

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

    QVulkanInstance * m_instance = nullptr;
    SoNode * m_scene = nullptr;
    SoNode * m_overlayScene = nullptr;
    SoNode * m_decorationScene = nullptr;
    SoCamera * m_camera = nullptr;
    QVulkanWindow * m_window = nullptr;
    QuarterVulkanWidget * m_owner = nullptr;
    QSize m_lastSurfaceSize;
    //! Display/tuning settings as one blob (see SoVulkanViewSettings).  The
    //! manager diffs and applies the whole blob, so the renderer keeps no
    //! per-field "last applied" mirrors of its own.
    SoVulkanViewSettings m_viewSettings;
    // Capabilities probed by selectPhysicalDevice() (see setDeviceCaps).
    SoVulkanDeviceCaps m_deviceCaps {};
    bool m_deviceCapsValid = false;
    bool m_initialized = false;
    //! Interaction LOD runtime state (engaged while navigating).
    bool m_interactionLod = false;
    // Staged authoritative scene lighting (GL host -> raster backend).  The
    // eye-space data is camera-independent, so only a dirty flag (set on push /
    // on an empty reset) triggers a re-push to the manager.
    SoLightingData m_sceneLighting;
    bool m_sceneLightsDirty = false;
    // The device context handed to SoVulkanRenderManager::initialize().  The
    // manager retains the POINTER (documented: the application must keep it
    // alive until shutdown), so a stack local would dangle once initResources()
    // returns.
    SoVulkanDeviceContext m_initContext;
    // Guards the frame-state members below, which are written from the
    // widget API (and redraw sensors) and snapshotted by startNextFrame().
    // Qt 6 runs both on the GUI thread, so the lock documents the snapshot
    // contract rather than preventing data races; it also future-proofs the
    // code if rendering ever moves to a dedicated thread.
    mutable QMutex m_stateMutex;
    SoVulkanRenderManager m_manager;
    SoVulkanRenderTarget m_target;
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
                        QuarterVulkanWidget * owner)
        : m_renderer(new QuarterVulkanRenderer(instance, this, scene, camera, owner))
    {
    }

    QVulkanWindowRenderer * createRenderer() override { return m_renderer; }

    QuarterVulkanRenderer * renderer() const { return m_renderer; }

public:
    // Core features the raster path relies on.  Indices are uint32 (large
    // BRep tessellations exceed 65535 vertices) and the blend factor mapping
    // can select the SRC1_* factors; both require the corresponding feature
    // to be enabled.  Query the physical device and request them only when
    // present -- enabling an unsupported core feature fails device creation.
    // Whether the device supports VK_POLYGON_MODE_LINE/POINT
    // (fillModeNonSolid); the wireframe/points overlay pipelines need it.
    bool fillModeNonSolid = false;
    bool fullDrawIndexUint32 = false;
    bool dualSrcBlend = false;
    // VK_KHR_synchronization2 (Vulkan 1.3 core): the renderer's barriers and
    // submits use the *2 entry points when this feature is enabled.
    bool synchronization2Available = false;
    // Whether the device advertises VK_KHR_synchronization2 as an extension
    // (rather than the core Vulkan 1.3 feature).  Only then may the name be
    // added to the device extension list.
    bool synchronization2ExtensionAvailable = false;
    // VK_EXT_pipeline_creation_feedback: lets the backend log pipeline-cache
    // hits and creation cost (COIN_VULKAN_PIPELINE_FEEDBACK).
    bool pipelineCreationFeedbackAvailable = false;
    // VK_EXT_debug_printf (+ VK_KHR_shader_non_semantic_info): lets shaders
    // compiled with COIN_ENABLE_DEBUG_PRINTF emit diagnostics
    // (COIN_VULKAN_DEBUG_PRINTF).
    bool debugPrintfAvailable = false;
    // VK_KHR_synchronization2 / Vulkan 1.3 core feature struct.  It lives on
    // the window object (not the modifier lambda) because QVulkanWindowPrivate::
    // init() reads the pNext chain after the callback returns.
    VkPhysicalDeviceSynchronization2Features synchronization2Feature {};

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
    // Auto-nulled when the raw-event widget is destroyed, so the event
    // filter below can never dereference a dangling pointer.
    QPointer<QWidget> rawEventTarget;
    //! Translates mouse/wheel/keyboard events into Coin events (the widget is
    //! an InputDeviceHost) and delivers them through eventSink.
    EventFilter* eventFilter = nullptr;
    std::function<bool(const SoEvent*)> eventSink;
    //! Reports whether an active tool/edit handler consumes the events (see
    //! setEventGrabProbe).
    std::function<bool()> eventGrabProbe;

    // Debug-only synthetic mouse injector state (see pollInjectFile()).
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    QString injectPath;
    int injectConsumed = 0;
#endif
    // When the Vulkan surface is not the visible page (the view is on the
    // classic Coin/OpenGL raster), redraw() must be a no-op: the scene is
    // shared and re-synced on switch-back (useVulkanViewport(true) ->
    // syncViewer), so re-rendering the hidden surface on every GL frame is
    // pure waste. The adapter sets this in useVulkanViewport().
    bool redrawEnabled = true;
};

QuarterVulkanWidget::QuarterVulkanWidget(QWidget * parent)
    : QWidget(parent)
    , d(new QuarterVulkanWidgetPrivate)
{
    vkLog("QuarterVulkanWidget: constructing");

    this->ensureSharedInstance();

    d->vulkanWindow = new QuarterVulkanWindow(d->instance, d->scene, d->camera,
                                               this);
    d->window = d->vulkanWindow;
    d->window->setVulkanInstance(d->instance);

    // Keep the Coin Vulkan backend alive across expose/hide cycles.  Without
    // this, a document restore makes Qt reset the window's renderer resources
    // repeatedly (hidden -> shown -> resize), which re-initializes the backend
    // and runs multiple expensive scene passes during updateGui().  It must be
    // set before the window is initialized/visible.
    if (vulkanPersistentResourcesEnabled()) {
        d->window->setFlags(d->window->flags() | QVulkanWindow::PersistentResources);
    }
    d->renderer = d->vulkanWindow->renderer();

    this->selectPhysicalDevice();
    this->configureDeviceFeatures();
    this->logSupportedSampleCounts();

    d->container = QWidget::createWindowContainer(d->window, this);
    d->container->setFocusPolicy(Qt::StrongFocus);
    auto * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->container);

    // The widget is an InputDeviceHost: translate mouse/wheel/keyboard events
    // itself (via a Coin EventFilter) and deliver them to the event sink the
    // viewport adapter wires to the InteractionController.  Events this filter
    // does not handle (tablet/touch/context-menu) fall through to the widget's
    // own event filter below, which relays them to the raw-event target.
    d->eventFilter = new EventFilter(this);
    d->container->installEventFilter(d->eventFilter);
    d->window->installEventFilter(d->eventFilter);

    d->container->installEventFilter(this);
    d->window->installEventFilter(this);

    // Debug-only synthetic mouse injector.  A QWindowContainer swallows
    // QCoreApplication::sendEvent() input, so the only way a test probe can
    // drive the real event filter is a genuine platform event posted to the
    // embedded window.  Enabling this is zero-cost unless the env var names a
    // file to poll (see pollInjectFile()).  Compiled out of release builds
    // unless FREECAD_USE_VULKAN_DEBUG_HOOKS is set.
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    if (const char * injectPath = std::getenv("FC_VULKAN_INJECT_PY")) {
        d->injectPath = injectPath;
        injectTimer = new QTimer(this);
        injectTimer->setInterval(10);
        QObject::connect(injectTimer, &QTimer::timeout, this,
                         &QuarterVulkanWidget::pollInjectFile);
        injectTimer->start();
    }
#endif
}

// One QVulkanInstance is shared across every 3D view (Qt intends a single
// app-wide instance; N views no longer allocate N instances).  Refcounted
// so the last widget tears it down.  Tearing it down resets the shared
// pointer to nullptr so the next widget (e.g. closing a document and
// reopening it, which destroys and re-creates the view) allocates a fresh
// instance instead of reusing a freed one.
// Create (if needed) and return the process-wide QVulkanInstance.  The caller
// must already hold g_sharedVulkanInstance.mutex.  Split out from
// ensureSharedInstance() so prewarmSharedInstance() can create the instance
// during startup without tying it to a widget.
static QVulkanInstance * ensureSharedVulkanInstanceLocked()
{
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
        // Advertise Vulkan 1.2.  The raster backend's memory allocator (VMA)
        // and the optional synchronization2 path are built against it.
        g_sharedVulkanInstance.instance->setApiVersion(QVersionNumber(1, 2, 0));
        // The validation layer is opt-in (FC_VULKAN_VALIDATION): it costs
        // real CPU per draw and must not ship enabled by default.
        if (std::getenv("FC_VULKAN_VALIDATION") != nullptr) {
            g_sharedVulkanInstance.instance->setLayers(
                {QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        }
        // VK_EXT_debug_utils is enabled unconditionally: it is the instance
        // half of the Coin renderer's optional object names / command-buffer
        // labels (COIN_VULKAN_DEBUG_UTILS), and an unused extension is free.
        g_sharedVulkanInstance.instance->setExtensions({
            QByteArrayLiteral("VK_EXT_debug_utils"),
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
    return g_sharedVulkanInstance.instance;
}

void QuarterVulkanWidget::ensureSharedInstance()
{
    QMutexLocker locker(&g_sharedVulkanInstance.mutex);
    ensureSharedVulkanInstanceLocked();
    ++g_sharedVulkanInstance.refs;
    d->instance = g_sharedVulkanInstance.instance;
    d->instanceRefs = &g_sharedVulkanInstance.refs;
    d->instanceMutex = &g_sharedVulkanInstance.mutex;
}

// Startup warm-up: create the shared instance (and thereby load the Vulkan
// loader/ICD + GPU driver) ahead of the first 3D view, so opening the first
// document does not pay the one-time driver initialization.  Idempotent; a
// failed creation is logged and the views fall back exactly as before.
void QuarterVulkanWidget::prewarmSharedInstance()
{
    QMutexLocker locker(&g_sharedVulkanInstance.mutex);
    ensureSharedVulkanInstanceLocked();
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
// (discrete ranks highest) with fillModeNonSolid as a tie-breaker, then pin
// the selection with setPhysicalDeviceIndex() so configureDeviceFeatures()'
// requests always match the device that is created.  The score weights keep a
// discrete GPU ahead of any integrated GPU even when the discrete one lacks a
// secondary feature, so the dedicated GPU is always preferred.
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

    // One physical-device capability probe.  All of the widget's feature /
    // extension gates are filled from a single pass so (a) each device's
    // extension list is enumerated once instead of re-enumerated per queried
    // name, and (b) the per-device optional flags are recorded for the *chosen*
    // device rather than for whichever device happened to be probed last.
    // The capability block is shared with the renderer (SoVulkanDeviceCaps in
    // SoVulkanRenderTarget.h), so the extension-name list and the probe result
    // have one definition and the renderer does not re-enumerate the device.
    using DeviceCaps = SoVulkanDeviceCaps;
    auto queryCaps = [f](VkPhysicalDevice dev) {
        DeviceCaps caps;
        VkPhysicalDeviceFeatures feats {};
        f->vkGetPhysicalDeviceFeatures(dev, &feats);
        caps.fillModeNonSolid = feats.fillModeNonSolid != 0;
        caps.fullDrawIndexUint32 = feats.fullDrawIndexUint32 != 0;
        caps.dualSrcBlend = feats.dualSrcBlend != 0;
        VkPhysicalDeviceProperties props {};
        f->vkGetPhysicalDeviceProperties(dev, &props);
        caps.synchronization2 = props.apiVersion >= VK_API_VERSION_1_3;
        uint32_t extCount = 0;
        f->vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount > 0) {
            f->vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount,
                                                    exts.data());
        }
        auto hasExt = [&exts](const char * name) {
            for (const auto & ext : exts) {
                if (std::strcmp(ext.extensionName, name) == 0) {
                    return true;
                }
            }
            return false;
        };
        caps.pipelineCreationFeedback =
            hasExt("VK_EXT_pipeline_creation_feedback");
        caps.debugPrintf = hasExt("VK_EXT_debug_printf") &&
            hasExt("VK_KHR_shader_non_semantic_info");
        caps.synchronization2 = caps.synchronization2
            || hasExt("VK_KHR_synchronization2");
        caps.synchronization2Extension = hasExt("VK_KHR_synchronization2");
        return caps;
    };

    int bestIndex = 0;
    int bestScore = -1;
    DeviceCaps best;
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
        const DeviceCaps caps = queryCaps(devs[i]);
        // Tie-breakers stay below the discrete/integrated type gap (50) so a
        // dedicated GPU is always preferred over an integrated one that
        // happens to have more secondary features.
        const int score = typeScore + (caps.fillModeNonSolid ? 20 : 0);
        vkLog("QuarterVulkanWidget: device %d '%s' type=%d "
              "fillModeNonSolid=%d score=%d",
              static_cast<int>(i), props.deviceName,
              static_cast<int>(props.deviceType), caps.fillModeNonSolid ? 1 : 0,
              score);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = static_cast<int>(i);
            best = caps;
        }
    }

    d->vulkanWindow->fillModeNonSolid = best.fillModeNonSolid;
    d->vulkanWindow->fullDrawIndexUint32 = best.fullDrawIndexUint32;
    d->vulkanWindow->dualSrcBlend = best.dualSrcBlend;
    d->vulkanWindow->pipelineCreationFeedbackAvailable =
        best.pipelineCreationFeedback;
    d->vulkanWindow->debugPrintfAvailable = best.debugPrintf;
    d->vulkanWindow->synchronization2Available = best.synchronization2;
    d->vulkanWindow->synchronization2ExtensionAvailable =
        best.synchronization2Extension;
    // Pin QVulkanWindow to the GPU we probed, so the feature/extensions
    // requested by configureDeviceFeatures() are guaranteed to be supported
    // by the device that is actually created.
    d->window->setPhysicalDeviceIndex(bestIndex);
    // Hand the probe result to the renderer so it can skip its own extension
    // enumeration (see SoVulkanDeviceContext::caps).
    if (d->renderer) {
        d->renderer->setDeviceCaps(best);
    }
    vkLog("QuarterVulkanWidget: selected physical device %d "
          "(fillModeNonSolid=%d)",
          bestIndex, best.fillModeNonSolid ? 1 : 0);
}

// Request the device extensions and features the raster renderer needs, plus
// fillModeNonSolid for the wireframe/points overlays, before the window is
// first shown: QVulkanWindow creates the device on first expose.
void QuarterVulkanWidget::configureDeviceFeatures()
{
    // Only request extensions the probed device actually exposes; requesting
    // an unsupported extension fails vkCreateDevice and would take down the
    // viewport.
    QList<QByteArray> deviceExt;
    // VK_KHR_synchronization2: add the extension name only when the device
    // advertises it as an extension.  On a Vulkan 1.3+ device the feature is
    // core and the name may not be listed, in which case adding it would fail
    // device creation.
    if (d->vulkanWindow->synchronization2ExtensionAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_synchronization2");
    }
    // VK_EXT_pipeline_creation_feedback lets the backend log pipeline-cache
    // hits and creation cost (COIN_VULKAN_PIPELINE_FEEDBACK).
    if (d->vulkanWindow->pipelineCreationFeedbackAvailable) {
        deviceExt << QByteArrayLiteral("VK_EXT_pipeline_creation_feedback");
    }
    // Shader-side diagnostics (debugPrintfEXT) are opt-in: the extension is
    // only requested when COIN_VULKAN_DEBUG_PRINTF is set, since it changes the
    // SPIR-V the shaders must have been compiled with.
    if (d->vulkanWindow->debugPrintfAvailable
        && std::getenv("COIN_VULKAN_DEBUG_PRINTF") != nullptr) {
        deviceExt << QByteArrayLiteral("VK_EXT_debug_printf")
                  << QByteArrayLiteral("VK_KHR_shader_non_semantic_info");
    }
    d->window->setDeviceExtensions(deviceExt);
    vkLog("QuarterVulkanWidget: requested raster device extensions");

    // Enable the core features the raster path uses.  The modifier receives
    // VkPhysicalDeviceFeatures2 after Qt has populated it;
    // applyBaseDeviceFeatures fills the always-on core features and chains the
    // synchronization2 struct.
    d->window->setEnabledFeaturesModifier(
      [this](VkPhysicalDeviceFeatures2 & features) {
        this->applyBaseDeviceFeatures(features);
      });
}

void QuarterVulkanWidget::applyBaseDeviceFeatures(
    VkPhysicalDeviceFeatures2 & features) const
{
    // Always-on core features for the raster backend.  fillModeNonSolid drives
    // the wireframe/points overlay pipelines; fullDrawIndexUint32 and
    // dualSrcBlend are the other optional core features the backend may use.
    // Each is gated on the probed device capability recorded on the window.
    features.features.fillModeNonSolid =
        d->vulkanWindow->fillModeNonSolid ? VK_TRUE : VK_FALSE;
    features.features.fullDrawIndexUint32 =
        d->vulkanWindow->fullDrawIndexUint32 ? VK_TRUE : VK_FALSE;
    features.features.dualSrcBlend =
        d->vulkanWindow->dualSrcBlend ? VK_TRUE : VK_FALSE;
    // synchronization2: the renderer's barriers and submits take the *2 entry
    // points whenever this feature is enabled (Vulkan 1.3 core or the
    // extension).  The struct lives on the window object because
    // QVulkanWindowPrivate::init() reads the pNext chain after the modifier
    // callback returns.
    if (d->vulkanWindow->synchronization2Available) {
        d->vulkanWindow->synchronization2Feature.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        d->vulkanWindow->synchronization2Feature.synchronization2 = VK_TRUE;
        d->vulkanWindow->synchronization2Feature.pNext = features.pNext;
        features.pNext = &d->vulkanWindow->synchronization2Feature;
    }
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
    // Stop translating/forwarding events before anything is freed: deferred
    // events delivered to the container/window after `d` is gone would
    // otherwise hit the event filter with a dangling private pointer.
    if (d->container) {
        d->container->removeEventFilter(this);
    }
    if (d->window) {
        d->window->removeEventFilter(this);
    }
    delete d->eventFilter;
    d->eventFilter = nullptr;
    d->eventSink = nullptr;
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
    d->renderer->setBackgroundGradient(enabled, topColor, bottomColor);
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

void QuarterVulkanWidget::setRawEventTarget(QWidget * target)
{
    d->rawEventTarget = target;
}

void QuarterVulkanWidget::setEventSink(std::function<bool(const SoEvent *)> sink)
{
    d->eventSink = std::move(sink);
}

void QuarterVulkanWidget::setEventGrabProbe(std::function<bool()> probe)
{
    d->eventGrabProbe = std::move(probe);
}

qreal QuarterVulkanWidget::devicePixelRatio() const
{
    return QWidget::devicePixelRatio();
}

bool QuarterVulkanWidget::vulkanDevicePixels() const
{
    // The Vulkan surface region is reported to the InteractionController in
    // device pixels (see VulkanViewportAdapter::applySurfaceViewportToGL).
    return true;
}

QSize QuarterVulkanWidget::inputSize() const
{
    return d->container ? d->container->size() : this->size();
}

SbVec2s QuarterVulkanWidget::inputWindowSize() const
{
    const QSize logical = inputSize();
    return SbVec2s(static_cast<short>(logical.width()),
                   static_cast<short>(logical.height()));
}

bool QuarterVulkanWidget::processSoEvent(const SoEvent * event)
{
    return d->eventSink ? d->eventSink(event) : false;
}

bool QuarterVulkanWidget::eventsGrabbed() const
{
    return d->eventGrabProbe ? d->eventGrabProbe() : false;
}

#ifdef FREECAD_VULKAN_DEBUG_HOOKS
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
#ifdef HAVE_QT6_GUI_PRIVATE
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
#endif
        d->injectConsumed = i + 1;
    }
}
#endif

bool QuarterVulkanWidget::eventFilter(QObject * watched, QEvent * event)
{
    Q_UNUSED(watched);

    // Mouse, wheel and keyboard are translated by d->eventFilter (this widget
    // is an InputDeviceHost) and delivered to the InteractionController.  The
    // remaining events -- tablet, touch and context menu -- are not handled by
    // the Coin input devices, so relay them to the raw-event target (the
    // hidden GL viewer that owns FreeCAD's gesture/tablet devices).
    if (!d->rawEventTarget) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletMove:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::ContextMenu:
        if (QCoreApplication::sendEvent(d->rawEventTarget, event)) {
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
    // Hidden-surface guard: while the view shows the classic GL raster, the
    // Vulkan window is not the visible page; rendering to it on every scene
    // change is wasted work (the surface is re-synced on switch-back).
    if (!d->redrawEnabled) {
        return;
    }
    d->window->requestUpdate();
}

void QuarterVulkanWidget::setRedrawEnabled(bool enabled)
{
    d->redrawEnabled = enabled;
}

bool QuarterVulkanWidget::supportsGrab() const
{
    return d->window->supportsGrab();
}

QImage QuarterVulkanWidget::grab() const
{
    return d->window->grab();
}

uint32_t QuarterVulkanWidget::getRenderFrameCount() const
{
    if (!d->renderer) {
        return 0;
    }
    return d->renderer->getRenderFrameCount();
}

void QuarterVulkanWidget::setViewSettings(const SoVulkanViewSettings & settings)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setViewSettings(settings);
    redraw();
}

void QuarterVulkanWidget::setSceneLights(const SoLightingData & lighting)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setSceneLights(lighting);
    redraw();
}

void QuarterVulkanWidget::setInteractionLod(bool active)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setInteractionLod(active);
    redraw();
}

QWidget * QuarterVulkanWidget::getNativeWidget()
{
    return d->container;
}

#endif // FREECAD_USE_VULKAN
