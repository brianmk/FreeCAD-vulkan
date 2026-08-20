// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "QuarterVulkanWidget.h"
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
#include <QKeyEvent>
#include <QMutex>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cstdarg>
#include <cstdio>
#include <cstring>

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

//! Env-gated frame-dump helper (FC_VULKAN_DUMP_FRAME).
//!
//! When disabled every method is a no-op and no Vulkan resources are
//! allocated.  When enabled it keeps a host-visible staging buffer sized to
//! the swapchain, records a copy of the presented color image into it inside
//! the frame's command buffer, and writes the mapped pixels to
//! /tmp/vk_frame_<n>.png after submission.  This captures the exact pixels
//! the backend rasterized (QVulkanWindow::grab() renders its own frame).
//! The dump window is controlled with FC_VULKAN_DUMP_START /
//! FC_VULKAN_DUMP_END (defaults 240-245).
class VulkanFrameDumper
{
public:
    VulkanFrameDumper(QVulkanInstance * instance, QVulkanWindow * window)
        : m_instance(instance)
        , m_window(window)
        , m_enabled(qEnvironmentVariableIsSet("FC_VULKAN_DUMP_FRAME"))
    {
        if (m_enabled) {
            m_dumpStart = qEnvironmentVariableIntValue("FC_VULKAN_DUMP_START");
            m_dumpEnd = qEnvironmentVariableIntValue("FC_VULKAN_DUMP_END");
            if (m_dumpStart < 0) {
                m_dumpStart = 240;
            }
            if (m_dumpEnd <= 0) {
                m_dumpEnd = 246;
            }
        }
    }

    void initSwapChainResources()
    {
        if (!m_enabled || m_buffer != VK_NULL_HANDLE) {
            return;
        }
        const QSize imgSize = m_window->swapChainImageSize();
        if (imgSize.width() <= 0 || imgSize.height() <= 0) {
            return;
        }

        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        VkBufferCreateInfo bci {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = static_cast<VkDeviceSize>(imgSize.width())
            * static_cast<VkDeviceSize>(imgSize.height()) * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkdf->vkCreateBuffer(m_window->device(), &bci, nullptr, &buf)
            != VK_SUCCESS) {
            vkErr("frame dump staging buffer creation failed");
            return;
        }

        VkMemoryRequirements memReq {};
        vkdf->vkGetBufferMemoryRequirements(m_window->device(), buf, &memReq);
        VkMemoryAllocateInfo mai {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = findMemoryType(
            memReq,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            vkErr("frame dump: no host-visible memory type available");
            vkdf->vkDestroyBuffer(m_window->device(), buf, nullptr);
            return;
        }
        if (vkdf->vkAllocateMemory(m_window->device(), &mai, nullptr, &mem)
                != VK_SUCCESS
            || vkdf->vkBindBufferMemory(m_window->device(), buf, mem, 0)
                != VK_SUCCESS) {
            vkErr("frame dump staging buffer alloc failed");
            if (mem != VK_NULL_HANDLE) {
                vkdf->vkFreeMemory(m_window->device(), mem, nullptr);
            }
            vkdf->vkDestroyBuffer(m_window->device(), buf, nullptr);
            return;
        }

        m_buffer = buf;
        m_memory = mem;
        m_size = imgSize;
        vkLog("frame dump staging buffer: %dx%d (%llu bytes)",
              imgSize.width(), imgSize.height(),
              static_cast<unsigned long long>(bci.size));
    }

    void releaseSwapChainResources()
    {
        if (m_buffer == VK_NULL_HANDLE) {
            return;
        }
        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        vkdf->vkDestroyBuffer(m_window->device(), m_buffer, nullptr);
        if (m_memory != VK_NULL_HANDLE) {
            vkdf->vkFreeMemory(m_window->device(), m_memory, nullptr);
        }
        m_buffer = VK_NULL_HANDLE;
        m_memory = VK_NULL_HANDLE;
    }

    //! Record the copy of the presented color image into the staging buffer
    //! inside \a cb (no-op when disabled, outside the dump window, or
    //! without a staging buffer).
    void recordFrameCopy(VkCommandBuffer cb, int swapchainIndex, const QSize & size)
    {
        if (!m_enabled || m_buffer == VK_NULL_HANDLE) {
            return;
        }
        m_frameCount++;
        if (m_frameCount < m_dumpStart || m_frameCount >= m_dumpEnd) {
            return;
        }
        m_dumpCount++;

        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        VkImage srcImage = m_window->swapChainImage(swapchainIndex);
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = srcImage;
        barrier.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkdf->vkCmdPipelineBarrier(
            cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            1, &barrier);

        VkBufferImageCopy region {};
        region.imageSubresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(size.width()),
                              static_cast<uint32_t>(size.height()), 1};
        vkdf->vkCmdCopyImageToBuffer(cb, srcImage,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     m_buffer, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkdf->vkCmdPipelineBarrier(
            cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &barrier);
    }

    //! Write the staging buffer to a PNG after frame submission (no-op when
    //! disabled, without a buffer, or after the dump window).
    void saveFrame()
    {
        const int windowFrames = m_dumpEnd - m_dumpStart + 1;
        if (!m_enabled || m_buffer == VK_NULL_HANDLE || m_dumpCount <= 0
            || m_dumpCount > windowFrames) {
            return;
        }
        QVulkanDeviceFunctions * vkdf =
            m_instance->deviceFunctions(m_window->device());
        vkdf->vkQueueWaitIdle(m_window->graphicsQueue());
        void * data = nullptr;
        if (vkdf->vkMapMemory(m_window->device(), m_memory, 0,
                              VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) {
            return;
        }
        const QImage img(static_cast<const uchar *>(data),
                         m_size.width(), m_size.height(),
                         static_cast<qsizetype>(m_size.width()) * 4,
                         QImage::Format_ARGB32);
        const QString path =
            QStringLiteral("/tmp/vk_frame_%1.png").arg(m_dumpCount);
        if (!img.isNull() && img.save(path)) {
            vkLog("frame dump %d: %dx%d -> %s", m_dumpCount, img.width(),
                  img.height(), qPrintable(path));
        }
        else {
            vkErr("frame dump %d: image save failed", m_dumpCount);
        }
        vkdf->vkUnmapMemory(m_window->device(), m_memory);
    }

private:
    // Returns UINT32_MAX when no memory type matches, so callers can fail
    // with a diagnostic instead of silently falling back to type 0.
    uint32_t findMemoryType(const VkMemoryRequirements & memReq,
                            uint32_t props)
    {
        // Pick a memory type supporting the requested properties.  The
        // physical device memory properties are fetched with the vulkan
        // loader directly (QVulkanWindow does not expose them).
        VkPhysicalDeviceMemoryProperties memProps {};
        vkGetPhysicalDeviceMemoryProperties(m_window->physicalDevice(),
                                            &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1u << i))
                && (memProps.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    QVulkanInstance * m_instance = nullptr;
    QVulkanWindow * m_window = nullptr;
    bool m_enabled = false;
    int m_dumpStart = 240;
    int m_dumpEnd = 246;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    QSize m_size;
    int m_dumpCount = 0;
    int m_frameCount = 0;
};

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
    void setCamera(SoCamera * camera)
    {
        QMutexLocker locker(&m_stateMutex);
        m_camera = camera;
    }
    void setClearEnabled(bool window, bool depth)
    {
        QMutexLocker locker(&m_stateMutex);
        m_clearWindow = window;
        m_clearDepth = depth;
    }
    void setBackgroundColor(const SbColor4f & color)
    {
        QMutexLocker locker(&m_stateMutex);
        m_background = color;
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

    // Path tracing state is applied to the manager on the render thread
    // (startNextFrame) instead of being called into the manager from the
    // GUI thread, keeping every manager access on the render thread.
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

    SoVulkanRenderManager * getManager() { return &m_manager; }

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

        SoVulkanDeviceContext context;
        context.instance = m_instance->vkInstance();
        context.physicalDevice = m_window->physicalDevice();
        context.device = m_window->device();
        context.graphicsQueue = m_window->graphicsQueue();
        context.graphicsQueueFamilyIndex = m_window->graphicsQueueFamilyIndex();
        if (props) {
            context.apiVersion = props->apiVersion;
        }
        m_initialized = m_manager.initialize(&context);
        if (m_initialized) {
            // Mirror the GL viewer (QuarterWidget sets
            // SoRenderManager::VARIABLE_NEAR_PLANE): re-fit the camera
            // near/far to the scene bounding box every frame so zooming and
            // orbiting never clip the model at the near/far planes.  The
            // hidden GL viewer never renders, so its own auto-clipping would
            // never run.
            m_manager.setAutoClipping(SoVulkanRenderManager::VARIABLE_NEAR_PLANE);
            m_manager.setRayTracing(m_rayTracing);
            if (m_rayTracing) {
                if (m_manager.getRayTracingActive()) {
                    vkLog("initResources: ray tracing active");
                }
                else {
                    vkWarn("initResources: ray tracing requested but "
                           "unavailable; using raster Vulkan backend");
                }
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
    }

    void logicalDeviceLost() override
    {
        vkErr("logicalDeviceLost: VK_ERROR_DEVICE_LOST");
    }

    void startNextFrame() override
    {
        // The state members below are written by the GUI thread (through the
        // setters) while this function runs on QVulkanWindow's render
        // thread.  Snapshot everything under the mutex and use the locals
        // for the rest of the frame; pending path-tracing requests are
        // applied to the manager here on the render thread.
        SoNode * scene = nullptr;
        SoNode * overlayScene = nullptr;
        SoCamera * camera = nullptr;
        SbColor4f background;
        SbColor4f backgroundTop;
        SbColor4f backgroundBottom;
        SbColor4f edgeColor;
        bool backgroundGradient = false;
        bool wireframeOverlay = false;
        bool pointsOverlay = false;
        bool clearWindow = true;
        bool clearDepth = true;
        {
            QMutexLocker locker(&m_stateMutex);
            scene = m_scene;
            overlayScene = m_overlayScene;
            camera = m_camera;
            background = m_background;
            backgroundTop = m_backgroundTop;
            backgroundBottom = m_backgroundBottom;
            edgeColor = m_edgeColor;
            backgroundGradient = m_backgroundGradient;
            wireframeOverlay = m_wireframeOverlay;
            pointsOverlay = m_pointsOverlay;
            clearWindow = m_clearWindow;
            clearDepth = m_clearDepth;

            if (m_pathTracingStart) {
                m_manager.setPathTracingStart(TRUE);
                m_pathTracingStart = false;
            }
            if (m_pathTracingEnabled != m_appliedPathTracingEnabled) {
                m_manager.setPathTracingEnabled(m_pathTracingEnabled ? TRUE
                                                                     : FALSE);
                m_appliedPathTracingEnabled = m_pathTracingEnabled;
            }
        }

        if (!m_initialized || !scene) {
            if (!m_initialized) {
                vkWarn("startNextFrame: backend not initialized, skipping");
            }
            else {
                vkWarn("startNextFrame: no scene graph set, skipping");
            }
            return;
        }

        const int index = m_window->currentSwapChainImageIndex();
        const QSize size = m_window->swapChainImageSize();

        const VkSampleCountFlagBits samples = m_window->sampleCountFlagBits();
        const bool multisample = samples != VK_SAMPLE_COUNT_1_BIT;
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

        m_manager.setSceneGraph(scene);
        m_manager.setOverlaySceneGraph(overlayScene);
        m_manager.setCamera(camera);

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
        m_manager.setBackgroundColor(background);
        VK_BREADCRUMB_ONCE("[VK-TRACE] startNextFrame: setBackgroundGradient "
                           "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                           backgroundGradient ? 1 : 0,
                           backgroundTop[0], backgroundTop[1], backgroundTop[2],
                           backgroundBottom[0], backgroundBottom[1], backgroundBottom[2]);
        m_manager.setBackgroundGradient(backgroundGradient,
                                        backgroundTop,
                                        backgroundBottom);
        m_manager.setWireframeOverlay(wireframeOverlay);
        m_manager.setPointsOverlay(pointsOverlay);
        m_manager.setEdgeColor(edgeColor);
        m_manager.setClearEnabled(clearWindow, clearDepth);
        m_manager.setRenderTarget(&m_target);

        // Log once per swapchain recreation (not per frame) to avoid console
        // spam now that the renderer requests a new frame continuously.
        static QSize lastLoggedSize;
        static uint32_t lastLoggedSamples = 0;
        if (size != lastLoggedSize || samples != lastLoggedSamples) {
            lastLoggedSize = size;
            lastLoggedSamples = samples;
            vkLog("startNextFrame: swapchainImage=%d extent=%dx%d samples=%d",
                  index, size.width(), size.height(),
                  static_cast<int>(samples));
        }

        // Notify the GUI thread so the hidden OpenGL viewer (which owns
        // navigation/picking) can keep its viewport region in sync with the
        // visible Vulkan surface size.
        if (size != m_lastSurfaceSize) {
            m_lastSurfaceSize = size;
            QMetaObject::invokeMethod(m_owner, "surfaceSizeChanged",
                                      Qt::QueuedConnection,
                                      Q_ARG(QSize, size));
        }

        // QVulkanWindow does not begin/end the render pass for us; the
        // renderer is expected to do it in startNextFrame() using
        // defaultRenderPass() and currentFramebuffer().  Record the scene
        // into QVulkanWindow's own command buffer between a begin/end pair,
        // then signal frameReady().  The backend must not begin/end a render
        // pass or submit to the queue on this path.
        VkCommandBuffer cb = m_window->currentCommandBuffer();
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

        QVulkanDeviceFunctions * vkdf = m_instance->deviceFunctions(m_window->device());
        vkdf->vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        const SbBool ok = m_manager.renderExternal(clearWindow, clearDepth,
                                                   cb,
                                                   m_window->defaultRenderPass());
        if (!ok) {
            vkErr("startNextFrame: renderExternal FAILED");
        }

        vkdf->vkCmdEndRenderPass(cb);

        // Env-gated frame dump (see VulkanFrameDumper): copy the swapchain
        // color image into a staging buffer inside the same command buffer,
        // then read it back after submission and write a PNG.
        m_dumper.recordFrameCopy(cb, index, size);

        m_window->frameReady();

        // Report the path-tracing status back to the GUI thread (one frame
        // of latency is acceptable for a status getter).
        {
            QMutexLocker locker(&m_stateMutex);
            m_pathTracingActive = m_manager.getPathTracingActive() ? true
                                                                   : false;
        }

        m_dumper.saveFrame();
        // Re-render every frame.  The Vulkan surface is display-only: camera
        // navigation, selection and scene-graph edits all happen on the
        // (hidden) OpenGL viewer that owns the same SoCamera and scene-graph
        // nodes this renderer references.  Those mutations do not emit
        // signals the Vulkan side can cheaply subscribe to, so request the
        // next frame unconditionally.  Without this the image stays frozen
        // after the first frame (camera moves, highlight and picking updates
        // never appear).
        m_window->requestUpdate();
    }

private:
    QVulkanInstance * m_instance = nullptr;
    SoNode * m_scene = nullptr;
    SoNode * m_overlayScene = nullptr;
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
    bool m_clearWindow = true;
    bool m_clearDepth = true;
    bool m_initialized = false;
    bool m_rayTracing = false;
    // Path tracing state mirrored here: the GUI thread writes the
    // requested values, startNextFrame applies them to the manager on the
    // render thread and reports the active status back.
    bool m_pathTracingEnabled = false;
    bool m_pathTracingStart = false;
    bool m_pathTracingActive = false;
    bool m_appliedPathTracingEnabled = false;
    // Guards every state member written from the GUI thread and read by
    // the render thread (startNextFrame snapshots under this lock).
    mutable QMutex m_stateMutex;
    SoVulkanRenderManager m_manager;
    SoVulkanRenderTarget m_target;
    VulkanFrameDumper m_dumper;
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

private:
    QuarterVulkanRenderer * m_renderer;
};

} // namespace

class SIM::Coin3D::Quarter::QuarterVulkanWidgetPrivate
{
public:
    QVulkanInstance * instance = nullptr;
    QVulkanWindow * window = nullptr;
    QuarterVulkanWindow * vulkanWindow = nullptr;
    QWidget * container = nullptr;
    QuarterVulkanRenderer * renderer = nullptr;
    SoNode * scene = nullptr;
    SoNode * overlayScene = nullptr;
    SoCamera * camera = nullptr;
    SbColor4f background = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool backgroundGradient = false;
    SbColor4f backgroundTop = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    SbColor4f backgroundBottom = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool clearWindow = true;
    bool clearDepth = true;
    bool rayTracing = false;
    QWidget * forwardTarget = nullptr;
};

QuarterVulkanWidget::QuarterVulkanWidget(QWidget * parent, bool rayTracing)
    : QWidget(parent)
    , d(new QuarterVulkanWidgetPrivate)
{
    vkLog("QuarterVulkanWidget: constructing%s",
          rayTracing ? " (ray tracing requested)" : "");
    d->rayTracing = rayTracing;

    d->instance = new QVulkanInstance;
    // Ray tracing requires Vulkan 1.2+ (acceleration-structure and
    // ray-tracing-pipeline APIs are core-adjacent KHR extensions promoted to
    // 1.2); advertise 1.2 so the device can expose them.
    d->instance->setApiVersion(QVersionNumber(1, 2, 0));
    d->instance->setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    if (!d->instance->create()) {
        vkWarn("QuarterVulkanWidget: could not create instance with validation "
               "layer (error %d), retrying without layers",
               static_cast<int>(d->instance->errorCode()));
        d->instance->setLayers({});
        d->instance->create();
    }

    if (d->instance->isValid()) {
        const QVersionNumber api = d->instance->supportedApiVersion();
        vkLog("QuarterVulkanWidget: instance created (Vulkan %d.%d.%d)",
              api.majorVersion(), api.minorVersion(), api.microVersion());
        const auto layers = d->instance->layers();
        for (const QByteArray & l : layers) {
            vkLog("  enabled layer: %s", l.constData());
        }
        const auto extensions = d->instance->extensions();
        for (const QByteArray & e : extensions) {
            vkLog("  enabled extension: %s", e.constData());
        }
    }
    else {
        vkErr("QuarterVulkanWidget: Vulkan instance creation FAILED (error %d)",
              static_cast<int>(d->instance->errorCode()));
    }

    d->vulkanWindow = new QuarterVulkanWindow(d->instance, d->scene, d->camera,
                                              this, rayTracing);
    d->window = d->vulkanWindow;
    d->window->setVulkanInstance(d->instance);
    d->renderer = d->vulkanWindow->renderer();

    if (rayTracing) {
        // Request the RT device extensions; QVulkanWindow creates the device
        // on first expose, so this must happen before the window is shown.
        const auto supported = d->window->supportedDeviceExtensions();
        bool haveAS = false;
        bool haveRTPipeline = false;
        bool haveRayQuery = false;
        for (const auto & ext : supported) {
            if (ext.name == "VK_KHR_acceleration_structure") haveAS = true;
            if (ext.name == "VK_KHR_ray_tracing_pipeline") haveRTPipeline = true;
            if (ext.name == "VK_KHR_ray_query") haveRayQuery = true;
        }
        if (haveAS && haveRTPipeline && haveRayQuery) {
            d->window->setDeviceExtensions({
                QByteArrayLiteral("VK_KHR_acceleration_structure"),
                QByteArrayLiteral("VK_KHR_ray_tracing_pipeline"),
                QByteArrayLiteral("VK_KHR_ray_query"),
                QByteArrayLiteral("VK_KHR_deferred_host_operations"),
            });
            // Enable the device features behind those extensions.  The
            // modifier receives VkPhysicalDeviceFeatures2 after Qt has
            // populated it; chain the RT feature structs onto pNext.
            //
            // VK_KHR_acceleration_structure requires the core
            // bufferDeviceAddress feature: every BLAS/TLAS is referenced by
            // device address and the RTX backend calls
            // vkGetBufferDeviceAddress() unconditionally.  Without this
            // feature the addresses are zero and the acceleration structure
            // builds are invalid (validation error or device lost on strict
            // drivers).
            //
            // The path tracer runs as a VK_KHR_ray_tracing_pipeline with a
            // five-group shader binding table, so the ray-tracing-pipeline
            // feature is required in addition.
            //
            // The feature structs live on the window object (not the
            // modifier lambda's stack): QVulkanWindowPrivate::init() reads
            // the pNext chain after the callback returns.
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
            // (FC_VULKAN_RT_SBT=1 opts into the ray tracing pipeline), so
            // the ray-query feature is required in addition.
            d->vulkanWindow->rtRayQuery.sType =
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            d->vulkanWindow->rtRayQuery.rayQuery = VK_TRUE;
            d->window->setEnabledFeaturesModifier(
              [this](VkPhysicalDeviceFeatures2 & features) {
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
        else {
            vkWarn("QuarterVulkanWidget: ray tracing requested but the device "
                   "does not advertise VK_KHR_ray_tracing_pipeline / "
                   "VK_KHR_acceleration_structure; falling back to raster");
        }
    }

    const QList<int> samples = d->window->supportedSampleCounts();
    QByteArray samplesStr;
    for (int s : samples) {
        if (!samplesStr.isEmpty()) {
            samplesStr += ',';
        }
        samplesStr += QByteArray::number(s);
    }
    vkLog("QuarterVulkanWidget: supported sample counts: %s",
          samplesStr.isEmpty() ? "(none)" : samplesStr.constData());

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
    // the instance we created.
    delete d->instance;
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
    d->background = color;
    d->renderer->setBackgroundColor(color);
}

void QuarterVulkanWidget::setBackgroundGradient(bool enabled,
                                                const SbColor4f & topColor,
                                                const SbColor4f & bottomColor)
{
    VK_BREADCRUMB("[VK-TRACE] QuarterVulkanWidget::setBackgroundGradient "
                  "enabled=%d top=(%.3f,%.3f,%.3f) bottom=(%.3f,%.3f,%.3f)\n",
                  enabled ? 1 : 0, topColor[0], topColor[1], topColor[2],
                  bottomColor[0], bottomColor[1], bottomColor[2]);
    d->backgroundGradient = enabled;
    d->backgroundTop = topColor;
    d->backgroundBottom = bottomColor;
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

void QuarterVulkanWidget::setEventForwardTarget(QWidget * target)
{
    d->forwardTarget = target;
}

bool QuarterVulkanWidget::eventFilter(QObject * watched, QEvent * event)
{
    Q_UNUSED(watched);
    if (!d->forwardTarget) {
        return QWidget::eventFilter(watched, event);
    }

    if (getenv("FC_VULKAN_BREADCRUMBS")) {
        if (event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease) {
            const auto* me = static_cast<const QMouseEvent*>(event);
            const QWidget* gl = d->forwardTarget;
            const QWidget* container = d->container;
            Base::vulkanBreadcrumb(
                    "[VK-TRACE] eventFilter watched=%s type=%d pos=(%.1f,%.1f) "
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
    }

    // Only forward input events.  The coordinates are left untouched: the
    // Vulkan container and the forward target occupy the same stacked-widget
    // area, so positions map 1:1.
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
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

const SbColor4f & QuarterVulkanWidget::getBackgroundColor() const
{
    return d->background;
}

void QuarterVulkanWidget::setClearEnabled(bool clearwindow, bool clearzbuffer)
{
    d->clearWindow = clearwindow;
    d->clearDepth = clearzbuffer;
    d->renderer->setClearEnabled(clearwindow, clearzbuffer);
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

SoVulkanRenderManager * QuarterVulkanWidget::getRenderManager() const
{
    return d->renderer->getManager();
}

bool QuarterVulkanWidget::isRayTracingActive() const
{
    if (!d->renderer) {
        return false;
    }
    return d->renderer->getManager()->getRayTracingActive();
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

QWidget * QuarterVulkanWidget::getNativeWidget()
{
    return d->container;
}

#endif // FREECAD_USE_VULKAN
