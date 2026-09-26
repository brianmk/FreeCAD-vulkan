// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "QuarterVulkanWidget.h"
#include "devices/InputDevice.h"
#include "eventhandlers/EventFilter.h"
#include "QuarterWidget.h"
#include "VulkanFrameDumper.h"
#include <Base/VulkanBreadcrumbs.h>

#ifdef FREECAD_USE_VULKAN

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
#include "VulkanDebugEnv.h"

#include <cstdarg>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace SIM::Coin3D::Quarter;
#include "QuarterVulkanRenderer.h"

namespace {

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
    // Timeline semaphores (Vulkan 1.2 core) let the async-compute queue signal
    // the graphics queue -- and vice versa -- without a CPU-stalling fence /
    // vkQueueWaitIdle.  Structured like the RT feature structs; chained onto
    // the VkPhysicalDeviceFeatures2 pNext when the device is currently 1.2+.
    VkPhysicalDeviceTimelineSemaphoreFeatures rtTimelineSemaphore {};
    bool rtTimelineSemaphoreAvailable = false;

    // Dedicated async-compute queue requested at device creation via
    // setQueueCreateInfoModifier().  The modifier (in QuarterVulkanWidget::
    // configureDeviceFeatures) scans the family properties for a
    // VK_QUEUE_COMPUTE_BIT family -- preferring a compute-only one -- appends
    // or extends a VkDeviceQueueCreateInfo, and records the chosen family
    // and queue index here so the renderer can later vkGetDeviceQueue() it.
    // computeQueueFamily is UINT32_MAX when no compute queue was requested.
    uint32_t computeQueueFamily = ~0u;
    uint32_t computeQueueIndex = 0;
    bool hasComputeQueueRequest = false;
    // Priorities array referenced from the appended/modified queue create
    // info; must remain valid until vkCreateDevice (which happens after the
    // modifier callback returns), so it is a long-lived member, not a lambda
    // stack local.
    float computeQueuePriorities[2] = {1.0f, 1.0f};
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
    // Whether the selected device supports VK_KHR_external_memory_fd.  The
    // CUDA/OptiX denoiser exports Vulkan memory as FDs through it, but unlike
    // the ray-tracing extensions it is not implied by VK_KHR_acceleration_
    // structure, so it must be probed too: requesting an extension the device
    // does not expose fails vkCreateDevice and would take down the whole
    // viewport, not just the denoiser.
    bool rtExternalMemoryFdAvailable = false;
    // Core features the raster path relies on.  Indices are uint32 (large
    // BRep tessellations exceed 65535 vertices) and the blend factor mapping
    // can select the SRC1_* factors; both require the corresponding feature
    // to be enabled.  Query the physical device and request them only when
    // present -- enabling an unsupported core feature also fails device
    // creation, exactly like the fillModeNonSolid case below.
    bool fullDrawIndexUint32 = false;
    bool dualSrcBlend = false;
    // Whether the selected device advertises the optional capability
    // extensions the RTX backend uses to improve the path tracer.  These are
    // strictly optional (the backend falls back gracefully), so they are
    // queried and only requested when present.  vkCreateDevice fails if an
    // unknown/unsupported extension is requested, so each must be gated.
    bool rtPositionFetchAvailable = false;
    bool rtOpacityMicromapAvailable = false;
    bool rtNvClusterAvailable = false;
    bool rtNvPartitionedAvailable = false;
    bool rtNvLinearSweptSpheresAvailable = false;
    // VK_KHR_synchronization2 (Vulkan 1.3 core) is a hard dependency of
    // VK_EXT_opacity_micromap; enabled alongside it when present.
    bool synchronization2Available = false;
    // Whether the device advertises VK_KHR_synchronization2 as an extension
    // (rather than the core Vulkan 1.3 feature).  Only then may the name be
    // added to the device extension list.
    bool synchronization2ExtensionAvailable = false;
    // Descriptor-indexing update-after-bind: lets the RT backend legally
    // rewrite a descriptor set an in-flight command buffer still references.
    bool descriptorIndexingAvailable = false;
    // VK_EXT_pipeline_creation_feedback: lets the backend log pipeline-cache
    // hits and creation cost (FC_VULKAN_PIPELINE_FEEDBACK).
    bool rtPipelineCreationFeedbackAvailable = false;
    // VK_EXT_debug_printf (+ VK_KHR_shader_non_semantic_info): lets shaders
    // compiled with COIN_ENABLE_DEBUG_PRINTF emit diagnostics
    // (FC_VULKAN_DEBUG_PRINTF).
    bool rtDebugPrintfAvailable = false;
    // Feature structs behind the optional extensions above.  They live on the
    // window object (not the modifier lambda) because QVulkanWindowPrivate::
    // init() reads the pNext chain after the callback returns.
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR rtPositionFetch {};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT rtOpacityMicromap {};
    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing {};
    // VK_KHR_synchronization2 / Vulkan 1.3 core: the renderer's barriers and
    // submits use the *2 entry points when this feature is enabled.
    VkPhysicalDeviceSynchronization2Features rtSynchronization2 {};

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
    //! scRGB HDR output requested (see setHdrOutputEnabled).  The actual
    //! swapchain format is reported by isHdrOutputActive() once the window is
    //! shown.
    bool hdrRequested = false;
    //! HDR was refused because the GPU driver is on the known-unsafe list
    //! (see hdrDriverBlocked); the SDR swapchain is used instead.
    bool hdrDriverBlocked = false;
    //! Receives tablet/touch/context-menu events the Coin input devices do not
    //! translate.  The InteractionController relays them to the surface that
    //! owns FreeCAD's gesture/tablet devices.
    std::function<bool(QEvent*)> rawEventSink;
    //! Translates mouse/wheel/keyboard events into Coin events (the widget is
    //! an InputDeviceHost) and delivers them through eventSink.
    EventFilter* eventFilter = nullptr;
    std::function<bool(const SoEvent*)> eventSink;

    // Debug-only synthetic mouse injector state (see pollInjectFile()).
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    QString injectPath;
    int injectConsumed = 0;
#endif
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
    if (const char * injectPath = Base::envString("FC_VULKAN_INJECT_PY")) {
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
        // Ray tracing requires Vulkan 1.2+ (acceleration-structure and
        // ray-tracing-pipeline APIs are core-adjacent KHR extensions
        // promoted to 1.2); advertise 1.2 so the device can expose them.
        g_sharedVulkanInstance.instance->setApiVersion(QVersionNumber(1, 2, 0));
        // The validation layer is opt-in (FC_VULKAN_VALIDATION): it costs
        // real CPU per draw and must not ship enabled by default.
        if (Gui::VkDebug::validation()) {
            g_sharedVulkanInstance.instance->setLayers(
                {QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        }
        // External memory interop with CUDA (for the RTX denoiser) needs the
        // instance capability extensions in addition to the device one, so the
        // FD export is usable.  Enabling them is free on 1.2+ and harmless if
        // the loader/driver lacks them (Qt tolerates unsupported instance
        // extensions in its setExtensions list).
        // VK_EXT_debug_utils is enabled unconditionally: it is the instance
        // half of the Coin renderer's optional object names / command-buffer
        // labels (FC_VULKAN_DEBUG_UTILS), and an unused extension is free.
        g_sharedVulkanInstance.instance->setExtensions({
            QByteArrayLiteral("VK_KHR_external_memory_capabilities"),
            QByteArrayLiteral("VK_KHR_external_memory"),
            QByteArrayLiteral("VK_KHR_external_semaphore_capabilities"),
            QByteArrayLiteral("VK_KHR_external_semaphore"),
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

    // One physical-device capability probe.  All of the widget's feature /
    // extension gates are filled from a single pass so (a) each device's
    // extension list is enumerated once instead of re-enumerated per queried
    // name, and (b) the per-device optional flags are recorded for the *chosen*
    // device rather than for whichever device happened to be probed last.
    // The capability block is shared with the renderer (SoVulkanDeviceCaps in
    // SoVulkanRenderTarget.h), so the extension-name list and the probe result
    // have one definition and the renderer does not re-enumerate the device.
    // (VK_KHR_synchronization2 is a hard dependency of VK_EXT_opacity_micromap;
    // requesting the latter without it fails vkCreateDevice validation,
    // VUID-ppEnabledExtensionNames-01387.)
    using DeviceCaps = SoVulkanDeviceCaps;
    auto queryCaps = [f](VkPhysicalDevice dev) {
        DeviceCaps caps;
        VkPhysicalDeviceFeatures feats {};
        f->vkGetPhysicalDeviceFeatures(dev, &feats);
        caps.fillModeNonSolid = feats.fillModeNonSolid != 0;
        caps.fullDrawIndexUint32 = feats.fullDrawIndexUint32 != 0;
        caps.dualSrcBlend = feats.dualSrcBlend != 0;
        // Descriptor-indexing update-after-bind bits: the RT backend's
        // descriptor ring cannot always keep a set out of an in-flight
        // command buffer, so it prefers the UPDATE_AFTER_BIND binding flags
        // when the device supports (and the widget enables) them.
        VkPhysicalDeviceDescriptorIndexingFeatures di {};
        di.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceFeatures2 feats2 {};
        feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        feats2.pNext = &di;
        f->vkGetPhysicalDeviceFeatures2(dev, &feats2);
        caps.descriptorIndexingUpdateAfterBind =
            di.descriptorBindingSampledImageUpdateAfterBind &&
            di.descriptorBindingStorageImageUpdateAfterBind &&
            di.descriptorBindingUniformBufferUpdateAfterBind &&
            di.descriptorBindingStorageBufferUpdateAfterBind;
        VkPhysicalDeviceProperties props {};
        f->vkGetPhysicalDeviceProperties(dev, &props);
        // Timeline semaphores are a Vulkan 1.2 core feature; the RT backend
        // already requires 1.2+, so a 1.2+ device supports them.
        caps.timelineSemaphore = props.apiVersion >= VK_API_VERSION_1_2;
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
        caps.externalSemaphoreFd = hasExt("VK_KHR_external_semaphore_fd");
        caps.externalMemoryFd = hasExt("VK_KHR_external_memory_fd");
        caps.positionFetch = hasExt("VK_KHR_ray_tracing_position_fetch");
        caps.opacityMicromap = hasExt("VK_EXT_opacity_micromap");
        caps.pipelineCreationFeedback =
            hasExt("VK_EXT_pipeline_creation_feedback");
        caps.debugPrintf = hasExt("VK_EXT_debug_printf") &&
            hasExt("VK_KHR_shader_non_semantic_info");
        caps.nvCluster = hasExt("VK_NV_cluster_acceleration_structure");
        caps.nvPartitioned = hasExt("VK_NV_partitioned_acceleration_structure");
        caps.nvLinearSweptSpheres = hasExt("VK_NV_ray_tracing_linear_swept_spheres");
        caps.synchronization2 = caps.synchronization2
            || hasExt("VK_KHR_synchronization2");
        caps.synchronization2Extension = hasExt("VK_KHR_synchronization2");
        caps.rayTracing = hasExt("VK_KHR_acceleration_structure")
            && hasExt("VK_KHR_ray_tracing_pipeline")
            && hasExt("VK_KHR_ray_query");
        return caps;
    };

    int bestIndex = 0;
    int bestScore = -1;
    DeviceCaps best;
    VkPhysicalDeviceProperties bestProps {};
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
        const int score = typeScore + (caps.fillModeNonSolid ? 20 : 0)
            + (caps.rayTracing ? 40 : 0);
        vkLog("QuarterVulkanWidget: device %d '%s' type=%d "
              "fillModeNonSolid=%d rayTracing=%d score=%d",
              static_cast<int>(i), props.deviceName,
              static_cast<int>(props.deviceType), caps.fillModeNonSolid ? 1 : 0,
              caps.rayTracing ? 1 : 0, score);
        if (score > bestScore) {
            bestScore = score;
            bestIndex = static_cast<int>(i);
            best = caps;
            bestProps = props;
        }
    }

    d->vulkanWindow->fillModeNonSolid = best.fillModeNonSolid;
    d->vulkanWindow->rtRayTracingAvailable = best.rayTracing;
    d->vulkanWindow->rtExternalSemaphoreFdAvailable = best.externalSemaphoreFd;
    d->vulkanWindow->rtExternalMemoryFdAvailable = best.externalMemoryFd;
    d->vulkanWindow->fullDrawIndexUint32 = best.fullDrawIndexUint32;
    d->vulkanWindow->dualSrcBlend = best.dualSrcBlend;
    d->vulkanWindow->rtTimelineSemaphoreAvailable = best.timelineSemaphore;
    d->vulkanWindow->rtPositionFetchAvailable = best.positionFetch;
    d->vulkanWindow->rtOpacityMicromapAvailable = best.opacityMicromap;
    d->vulkanWindow->rtPipelineCreationFeedbackAvailable =
        best.pipelineCreationFeedback;
    d->vulkanWindow->rtDebugPrintfAvailable = best.debugPrintf;
    d->vulkanWindow->rtNvClusterAvailable = best.nvCluster;
    d->vulkanWindow->rtNvPartitionedAvailable = best.nvPartitioned;
    d->vulkanWindow->rtNvLinearSweptSpheresAvailable = best.nvLinearSweptSpheres;
    d->vulkanWindow->synchronization2Available = best.synchronization2;
    d->vulkanWindow->synchronization2ExtensionAvailable =
        best.synchronization2Extension;
    d->vulkanWindow->descriptorIndexingAvailable =
        best.descriptorIndexingUpdateAfterBind;
    // Report the optional-capability probe once per device selection.  These
    // caps gate which extensions/features are requested below, so a run that
    // reports them absent is the signal that the selected device is a fallback
    // (e.g. RADV) or that the extension names changed.  Gated on
    // FC_VULKAN_RT_DEBUG like the backend's [RTDBG] lines; the Phase-0 probe
    // (vk_rt_phase0_probe) parses it.
    if (Gui::VkDebug::rtDebug()) {
        std::fprintf(stderr,
                     "[RTDBG] caps positionFetch=%d opacityMicromap=%d "
                     "nvCluster=%d nvPartitioned=%d nvLinearSweptSpheres=%d\n",
                     best.positionFetch ? 1 : 0, best.opacityMicromap ? 1 : 0,
                     best.nvCluster ? 1 : 0, best.nvPartitioned ? 1 : 0,
                     best.nvLinearSweptSpheres ? 1 : 0);
    }
    if (!best.externalMemoryFd) {
        vkWarn("QuarterVulkanWidget: the selected device lacks "
               "VK_KHR_external_memory_fd; the CUDA/OptiX denoiser cannot "
               "import Vulkan memory and will fall back to the other denoisers.");
    }
    // Pin QVulkanWindow to the GPU we probed, so the feature/extensions
    // requested by configureDeviceFeatures() are guaranteed to be supported
    // by the device that is actually created.
    d->window->setPhysicalDeviceIndex(bestIndex);
    // The selection log above is Base::Console().log (not captured by the
    // fcprobe harness).  Mirror the SELECTED device on the captured stderr
    // channel -- not the breadcrumb file, which several translation units
    // independently truncate -- so the CPU-only and device-profile probes can
    // assert which device was selected (a software device for the CPU test, a
    // simulated lower-tier device for the rtx-2060 profile).  Gated on the
    // breadcrumbs flag so it stays a harness-only diagnostic.
    if (Gui::VkDebug::breadcrumbs()) {
        std::fprintf(stderr,
                     "[VK-DEVICE] selected index=%d name='%s' type=%d "
                     "maxMemAlloc=%u\n",
                     bestIndex,
                     bestProps.deviceName,
                     static_cast<int>(bestProps.deviceType),
                     static_cast<unsigned>(
                         bestProps.limits.maxMemoryAllocationCount));
    }
    // Hand the probe result to the renderer so it can skip its own extension
    // enumeration (see SoVulkanDeviceContext::caps).
    if (d->renderer) {
        d->renderer->setDeviceCaps(best);
    }
    vkLog("QuarterVulkanWidget: selected physical device %d "
          "(fillModeNonSolid=%d rayTracing=%d)",
          bestIndex, best.fillModeNonSolid ? 1 : 0, best.rayTracing ? 1 : 0);
}

// Request the device extensions and feature structs for ray tracing (when
// the chosen device supports them) plus fillModeNonSolid for the
// wireframe/points overlays, before the window is first shown:
// QVulkanWindow creates the device on first expose.
//
// The ray-tracing feature set is requested whenever the device advertises it.
// That keeps the RTX backend always available so path tracing can be toggled
// live (raster <-> RT) with a preference change, instead of forcing a document
// reopen.
void QuarterVulkanWidget::configureDeviceFeatures()
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
        vkLog("QuarterVulkanWidget: the selected device does not advertise "
              "VK_KHR_ray_tracing_pipeline / VK_KHR_acceleration_structure; "
              "path tracing is unavailable and the raster backend is used");
        // Still request fillModeNonSolid for the wireframe/points overlay
        // pipelines.
        d->window->setEnabledFeaturesModifier(
          [this](VkPhysicalDeviceFeatures2 & features) {
            this->applyBaseDeviceFeatures(features);
          });
        return;
    }

    QList<QByteArray> deviceExt {
        QByteArrayLiteral("VK_KHR_acceleration_structure"),
        QByteArrayLiteral("VK_KHR_ray_tracing_pipeline"),
        QByteArrayLiteral("VK_KHR_ray_query"),
        // VK_KHR_acceleration_structure requires VK_KHR_deferred_host_operations
        // as a dependency extension, so it is implied by the ray-tracing probe.
        QByteArrayLiteral("VK_KHR_deferred_host_operations"),
    };
    // VK_KHR_external_memory_fd is NOT implied by the ray-tracing extension
    // set: only request it when the device actually exposes it, or vkCreateDevice
    // fails and the viewport never comes up (the denoiser already degrades via
    // the vkGetMemoryFdKHR entry-point probe).
    if (d->vulkanWindow->rtExternalMemoryFdAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_external_memory_fd");
    }
    if (d->vulkanWindow->rtExternalSemaphoreFdAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_external_semaphore")
                  << QByteArrayLiteral("VK_KHR_external_semaphore_fd");
    }
    // Optional capability extensions (queried above).  Requesting an
    // extension a device does not expose would fail device creation, so each
    // is gated on the probe result.
    if (d->vulkanWindow->rtPositionFetchAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_ray_tracing_position_fetch");
    }
    // VK_EXT_opacity_micromap requires VK_KHR_synchronization2 (or Vulkan 1.3
    // core); request both together, gated on the dependency being present, or
    // vkCreateDevice fails validation (VUID-ppEnabledExtensionNames-01387).
    if (d->vulkanWindow->rtOpacityMicromapAvailable
        && d->vulkanWindow->synchronization2Available) {
        deviceExt << QByteArrayLiteral("VK_EXT_opacity_micromap");
    }
    // VK_KHR_synchronization2: add the extension name only when the device
    // advertises it as an extension.  On a Vulkan 1.3+ device the feature is
    // core and the name may not be listed, in which case adding it would fail
    // device creation.
    if (d->vulkanWindow->synchronization2ExtensionAvailable) {
        deviceExt << QByteArrayLiteral("VK_KHR_synchronization2");
    }
    if (d->vulkanWindow->rtPipelineCreationFeedbackAvailable) {
        deviceExt << QByteArrayLiteral("VK_EXT_pipeline_creation_feedback");
    }
    // Shader-side diagnostics (debugPrintfEXT) are opt-in: the extension is
    // only requested when FC_VULKAN_DEBUG_PRINTF is set, since it changes the
    // SPIR-V the shaders must have been compiled with.
    if (d->vulkanWindow->rtDebugPrintfAvailable &&
        Gui::VkDebug::debugPrintf()) {
        deviceExt << QByteArrayLiteral("VK_EXT_debug_printf")
                  << QByteArrayLiteral("VK_KHR_shader_non_semantic_info");
    }
    if (d->vulkanWindow->rtNvClusterAvailable) {
        deviceExt << QByteArrayLiteral("VK_NV_cluster_acceleration_structure");
    }
    if (d->vulkanWindow->rtNvPartitionedAvailable) {
        deviceExt << QByteArrayLiteral("VK_NV_partitioned_acceleration_structure");
    }
    if (d->vulkanWindow->rtNvLinearSweptSpheresAvailable) {
        deviceExt << QByteArrayLiteral("VK_NV_ray_tracing_linear_swept_spheres");
    }
    d->window->setDeviceExtensions(deviceExt);
    // Request a dedicated async-compute queue at device creation so the RT
    // backend can overlap denoise/trace work on the compute queue while the
    // graphics queue runs the next frame (no vkQueueWaitIdle stall).  Qt picks
    // a single graphics queue by itself; QueueCreateInfoModifier is the
    // official hook to inject extra VkDeviceQueueCreateInfo (Qt 5.15+).
    d->window->setQueueCreateInfoModifier(
      [this](const VkQueueFamilyProperties * families, uint32_t familyCount,
             QVector<VkDeviceQueueCreateInfo> & queueCreateInfos) {
        // Prefer a compute-ONLY family: its queues are statically dedicated to
        // transfer/compute and overlap freely with the graphics queues.
        int computeFamily = -1;
        for (uint32_t i = 0; i < familyCount; ++i) {
          if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
              !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            computeFamily = static_cast<int>(i);
            break;
          }
        }
        if (computeFamily < 0) {
          // No compute-only family: extend the graphics-family request Qt is
          // already issuing if that family also computes and has a spare queue.
          for (int i = 0; i < queueCreateInfos.size(); ++i) {
            const VkDeviceQueueCreateInfo & info = queueCreateInfos[i];
            if (info.queueFamilyIndex >= familyCount) continue;
            const VkQueueFamilyProperties & fp = families[info.queueFamilyIndex];
            if ((fp.queueFlags & VK_QUEUE_COMPUTE_BIT) && fp.queueCount >= 2) {
              computeFamily = static_cast<int>(info.queueFamilyIndex);
              break;
            }
          }
        }
        if (computeFamily < 0) {
          vkLog("QuarterVulkanWidget: no compute-capable queue family found; "
                "async-compute unavailable");
          return;
        }
        bool extended = false;
        for (VkDeviceQueueCreateInfo & info : queueCreateInfos) {
          if (info.queueFamilyIndex == static_cast<uint32_t>(computeFamily)) {
            if (info.queueCount < 2) {
              info.queueCount = 2;
              info.pQueuePriorities = d->vulkanWindow->computeQueuePriorities;
            }
            d->vulkanWindow->computeQueueFamily =
              static_cast<uint32_t>(computeFamily);
            d->vulkanWindow->computeQueueIndex = 1;  // leave index 0 as graphics
            d->vulkanWindow->hasComputeQueueRequest = true;
            extended = true;
            break;
          }
        }
        if (!extended) {
          VkDeviceQueueCreateInfo info {};
          info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
          info.queueFamilyIndex = static_cast<uint32_t>(computeFamily);
          info.queueCount = 1;
          info.pQueuePriorities = d->vulkanWindow->computeQueuePriorities;
          queueCreateInfos.append(info);
          d->vulkanWindow->computeQueueFamily =
            static_cast<uint32_t>(computeFamily);
          d->vulkanWindow->computeQueueIndex = 0;
          d->vulkanWindow->hasComputeQueueRequest = true;
        }
        vkLog("QuarterVulkanWidget: request async-compute queue family=%d "
              "index=%u", computeFamily, d->vulkanWindow->computeQueueIndex);
      });
    vkLog("QuarterVulkanWidget: request device ext: "
          "accel_structure, ray_tracing_pipeline, ray_query, "
          "deferred_host_ops%s%s",
          d->vulkanWindow->rtExternalMemoryFdAvailable
            ? ", external_memory_fd"
            : "",
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
    d->vulkanWindow->rtTimelineSemaphore.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    d->vulkanWindow->rtTimelineSemaphore.timelineSemaphore = VK_TRUE;
    d->window->setEnabledFeaturesModifier(
      [this](VkPhysicalDeviceFeatures2 & features) {
        this->applyBaseDeviceFeatures(features);
        d->vulkanWindow->rtRayQuery.pNext = features.pNext;
        d->vulkanWindow->rtRayTracingPipeline.pNext =
          &d->vulkanWindow->rtRayQuery;
        d->vulkanWindow->rtAccelerationStructure.pNext =
          &d->vulkanWindow->rtRayTracingPipeline;
        d->vulkanWindow->rtBufferDeviceAddress.pNext =
          &d->vulkanWindow->rtAccelerationStructure;
        // Insert an optional feature struct directly after
        // VkPhysicalDeviceBufferDeviceAddressFeatures in the pNext chain.
        // Every Vulkan feature struct begins with sType + pNext, so the chain
        // link can be manipulated through VkBaseOutStructure; the driver walks
        // the whole chain, so the order is cosmetic only.
        auto chainAfterBda = [this](VkBaseOutStructure * node) {
            node->pNext = static_cast<VkBaseOutStructure *>(
                d->vulkanWindow->rtBufferDeviceAddress.pNext);
            d->vulkanWindow->rtBufferDeviceAddress.pNext = node;
        };
        // Timeline semaphores for the async-compute path (see the queue
        // modifier): only requested when the 1.2+ device reports them.
        if (d->vulkanWindow->rtTimelineSemaphoreAvailable) {
            chainAfterBda(reinterpret_cast<VkBaseOutStructure *>(
                &d->vulkanWindow->rtTimelineSemaphore));
        }
        // Chain the optional capability feature structs (each gated on its
        // extension being present) so the device enables them.
        if (d->vulkanWindow->rtPositionFetchAvailable) {
            d->vulkanWindow->rtPositionFetch.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;
            d->vulkanWindow->rtPositionFetch.rayTracingPositionFetch = VK_TRUE;
            chainAfterBda(reinterpret_cast<VkBaseOutStructure *>(
                &d->vulkanWindow->rtPositionFetch));
        }
        if (d->vulkanWindow->rtOpacityMicromapAvailable) {
            d->vulkanWindow->rtOpacityMicromap.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
            d->vulkanWindow->rtOpacityMicromap.micromap = VK_TRUE;
            chainAfterBda(reinterpret_cast<VkBaseOutStructure *>(
                &d->vulkanWindow->rtOpacityMicromap));
        }
        // Descriptor-indexing update-after-bind: the RT backend's descriptor
        // sets must be updatable while a caller-owned frame is still pending
        // (VUID-vkUpdateDescriptorSets-None-03047).
        if (d->vulkanWindow->descriptorIndexingAvailable) {
            VkPhysicalDeviceDescriptorIndexingFeatures & di =
                d->vulkanWindow->descriptorIndexing;
            di.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            di.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            di.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            di.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            di.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            chainAfterBda(reinterpret_cast<VkBaseOutStructure *>(&di));
        }
        features.pNext = &d->vulkanWindow->rtBufferDeviceAddress;
      });
    vkLog("QuarterVulkanWidget: requested ray tracing pipeline device "
          "extensions");
}

void QuarterVulkanWidget::applyBaseDeviceFeatures(
    VkPhysicalDeviceFeatures2 & features) const
{
    // Always-on features shared by the raster and RT feature-modifier paths.
    // fillModeNonSolid drives the wireframe/points overlay pipelines;
    // fullDrawIndexUint32 and dualSrcBlend are the other optional core
    // features the backend may use.  Each is gated on the probed device
    // capability recorded on the window.
    features.features.fillModeNonSolid =
        d->vulkanWindow->fillModeNonSolid ? VK_TRUE : VK_FALSE;
    features.features.fullDrawIndexUint32 =
        d->vulkanWindow->fullDrawIndexUint32 ? VK_TRUE : VK_FALSE;
    features.features.dualSrcBlend =
        d->vulkanWindow->dualSrcBlend ? VK_TRUE : VK_FALSE;
    // synchronization2: the renderer's barriers and submits take the *2 entry
    // points whenever this feature is enabled (Vulkan 1.3 core or the
    // extension).  Chained here so both the raster and RT feature-modifier
    // paths request it.  The struct lives on the window object because
    // QVulkanWindowPrivate::init() reads the pNext chain after the modifier
    // callback returns.
    if (d->vulkanWindow->synchronization2Available) {
        d->vulkanWindow->rtSynchronization2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        d->vulkanWindow->rtSynchronization2.synchronization2 = VK_TRUE;
        d->vulkanWindow->rtSynchronization2.pNext = features.pNext;
        features.pNext = &d->vulkanWindow->rtSynchronization2;
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
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setBackgroundGradient "
                  "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                  enabled ? 1 : 0, topColor[0], topColor[1], topColor[2],
                  bottomColor[0], bottomColor[1], bottomColor[2]);
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

void QuarterVulkanWidget::setRawEventSink(std::function<bool(QEvent *)> sink)
{
    d->rawEventSink = std::move(sink);
}

void QuarterVulkanWidget::setEventSink(std::function<bool(const SoEvent *)> sink)
{
    d->eventSink = std::move(sink);
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
    // the Coin input devices, so hand them to the raw-event sink (the
    // InteractionController relays them to the surface owning FreeCAD's
    // gesture/tablet devices).
    if (!d->rawEventSink) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease) {
        const auto* me = static_cast<const QMouseEvent*>(event);
        const QWidget* container = d->container;
        VK_BREADCRUMB_SAMPLED(32, "[VK-TRACE] eventFilter watched=%s type=%d pos=(%.1f,%.1f) "
                      "global=(%.1f,%.1f) | container rect=(%d,%d %dx%d) dpr=%.2f\n",
                      watched == d->container ? "container"
                      : (watched == static_cast<QObject*>(d->window) ? "window"
                                                                     : "other"),
                      static_cast<int>(event->type()),
                      me->position().x(), me->position().y(),
                      me->globalPosition().x(), me->globalPosition().y(),
                      container->x(), container->y(), container->width(),
                      container->height(), container->devicePixelRatioF());
    }

    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletMove:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::ContextMenu:
        if (d->rawEventSink(event)) {
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

void QuarterVulkanWidget::setPresentMode(int mode)
{
#ifdef FREECAD_USE_QT_PRIVATE_PRESENT_MODE
    vkLog("setPresentMode: requesting mode %d", mode);
    // Stored on the renderer because QVulkanWindow only creates the surface
    // (needed to know which present modes are supported) at first expose;
    // preInitResources() applies it before the swapchain is created.
    if (d->renderer) {
        d->renderer->setRequestedPresentMode(mode);
    }
#else
    // Product build without the Qt private present-mode hook: the preference
    // has no effect and the swapchain stays on Qt's FIFO default.  Log it so
    // the intent (and the reason) is visible in the trace.
    if (mode != 0) {
        vkLog("setPresentMode: ignoring mode %d; this build has no Qt private "
              "present-mode support (FIFO only)",
              mode);
    }
#endif
}

void QuarterVulkanWidget::setHdrOutputEnabled(bool enabled)
{
    if (enabled && hdrDriverBlocked(d->window)) {
        vkErr("HDR output requested but disabled: the NVIDIA 610.x driver series "
              "is known to forward invalid HDR10 luminance metadata to the "
              "compositor. Set FREECAD_VULKAN_HDR_ALLOW_UNSAFE_DRIVER=1 to "
              "override.");
        d->hdrDriverBlocked = true;
        enabled = false;
    }
    d->hdrRequested = enabled;
    if (!enabled) {
        vkLog("setHdrOutputEnabled: off (SDR swapchain)");
        return;
    }
    // Ask for an FP16 swapchain image, keeping the 8-bit format as a fallback
    // so a surface without FP16 still comes up (in SDR).  QVulkanWindow picks
    // the first requested format present in the surface's format list; on
    // Wayland it then uses VK_COLOR_SPACE_PASS_THROUGH_EXT and the color
    // mapping is carried by Qt's wp_image_description instead.
    d->window->setPreferredColorFormats(QList<VkFormat>()
        << VK_FORMAT_R16G16B16A16_SFLOAT
        << VK_FORMAT_B8G8R8A8_UNORM);

    // Tag the window's surface format with an extended-linear sRGB (scRGB)
    // color space.  Qt's Wayland platform reads this in
    // QWaylandWindow::initializeColorSpace() and attaches a
    // wp_color_manager_v1 image description with the extended-linear transfer
    // function and sRGB primaries (the description Blender uses for Wayland
    // HDR); on other platforms it is ignored.  Diffuse white is then 1.0 and
    // the compositor maps the >1.0 highlights onto the output.  Must be set
    // before the window is first shown.
    QSurfaceFormat fmt = d->window->format();
    fmt.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    d->window->setFormat(fmt);
    VK_BREADCRUMB("[VK-HDR] setHdrOutputEnabled: requested scRGB FP16 swapchain + "
                  "extended-linear sRGB\n");
}

bool QuarterVulkanWidget::isHdrOutputRequested() const
{
    return d->hdrRequested;
}

bool QuarterVulkanWidget::isHdrOutputActive() const
{
    if (!d->window || d->hdrDriverBlocked) {
        return false;
    }
    const VkFormat fmt = d->window->colorFormat();
    return fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
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

bool QuarterVulkanWidget::pickRay(const float origin[3],
                                  const float direction[3], float tMax,
                                  VulkanPickHit & out) const
{
    out = VulkanPickHit {};
    if (!d->renderer) {
        return false;
    }
    return d->renderer->pickRay(origin, direction, tMax, out);
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

bool QuarterVulkanWidget::hasAsyncComputeQueue() const
{
    return d->vulkanWindow && d->vulkanWindow->hasComputeQueueRequest;
}

uint32_t QuarterVulkanWidget::asyncComputeQueueFamilyIndex() const
{
    return d->vulkanWindow ? d->vulkanWindow->computeQueueFamily : ~0u;
}

uint32_t QuarterVulkanWidget::asyncComputeQueueIndex() const
{
    return d->vulkanWindow ? d->vulkanWindow->computeQueueIndex : 0;
}

void QuarterVulkanWidget::setViewMode(SoVulkanViewMode mode)
{
    if (!d->renderer) {
        return;
    }
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setViewMode mode=%d\n",
                  static_cast<int>(mode));
    d->renderer->setViewMode(mode);
    redraw();
}

SoVulkanViewMode QuarterVulkanWidget::getViewMode() const
{
    if (!d->renderer) {
        return SoVulkanViewMode::RtxModeOff;
    }
    return d->renderer->getViewMode();
}

void QuarterVulkanWidget::setViewSettings(const SoVulkanViewSettings & settings)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setViewSettings(settings);
    redraw();
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

void QuarterVulkanWidget::setSceneLights(const SoLightingData & lighting)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setSceneLights(lighting);
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

void QuarterVulkanWidget::setInteractionLod(bool active)
{
    if (!d->renderer) {
        return;
    }
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setInteractionLod active=%d\n",
                  active ? 1 : 0);
    d->renderer->setInteractionLod(active);
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

void QuarterVulkanWidget::setPathTracingDenoiserScale(float scale)
{
    if (!d->renderer) {
        return;
    }
    d->renderer->setPathTracingDenoiserScale(scale);
    redraw();
}

QWidget * QuarterVulkanWidget::getNativeWidget()
{
    return d->container;
}

#endif // FREECAD_USE_VULKAN

