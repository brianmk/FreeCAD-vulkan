// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "QuarterVulkanWidget.h"

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

class QuarterVulkanRenderer;

class QuarterVulkanRenderer final : public QVulkanWindowRenderer
{
public:
    QuarterVulkanRenderer(QVulkanInstance * instance,
                          QVulkanWindow * window,
                          SoNode * scene,
                          SoCamera * camera)
        : m_instance(instance)
        , m_scene(scene)
        , m_camera(camera)
        , m_window(window)
    {
    }

    void setScene(SoNode * scene) { m_scene = scene; }
    void setCamera(SoCamera * camera) { m_camera = camera; }
    void setViewportRegion(const SbViewportRegion & vp) { m_viewport = vp; }
    void setClearEnabled(bool window, bool depth)
    {
        m_clearWindow = window;
        m_clearDepth = depth;
    }
    void setBackgroundColor(const SbColor4f & color) { m_background = color; }

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
        m_initialized = m_manager.initialize(&context);
        if (m_initialized) {
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
        if (!m_initialized || !m_scene) {
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

        m_manager.setSceneGraph(m_scene);
        m_manager.setCamera(m_camera);

        // If no viewport region has been configured yet, fall back to the
        // full swapchain extent so the frame renders correctly on first show.
        SbViewportRegion vp = m_viewport;
        if (vp.getViewportSizePixels() == SbVec2s(0, 0)) {
            vp.setWindowSize(static_cast<short>(size.width()),
                             static_cast<short>(size.height()));
            vp.setViewportPixels(0, 0,
                                 static_cast<short>(size.width()),
                                 static_cast<short>(size.height()));
        }
        m_manager.setViewportRegion(vp);
        m_manager.setBackgroundColor(m_background);
        m_manager.setClearEnabled(m_clearWindow, m_clearDepth);
        m_manager.setRenderTarget(&m_target);

        vkLog("startNextFrame: frame=%d swapchainImage=%d extent=%dx%d samples=%d",
              m_window->currentFrame(), index, size.width(), size.height(),
              static_cast<int>(samples));

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
        rpBegin.clearValueCount = 0;
        rpBegin.pClearValues = nullptr;

        QVulkanDeviceFunctions * vkdf = m_instance->deviceFunctions(m_window->device());
        vkdf->vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        const SbBool ok = m_manager.renderExternal(m_clearWindow, m_clearDepth,
                                                   cb,
                                                   m_window->defaultRenderPass());
        if (!ok) {
            vkErr("startNextFrame: renderExternal FAILED");
        }

        vkdf->vkCmdEndRenderPass(cb);
        m_window->frameReady();
    }

private:
    QVulkanInstance * m_instance = nullptr;
    SoNode * m_scene = nullptr;
    SoCamera * m_camera = nullptr;
    QVulkanWindow * m_window = nullptr;
    SbViewportRegion m_viewport;
    SbColor4f m_background = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool m_clearWindow = true;
    bool m_clearDepth = true;
    bool m_initialized = false;
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
                        SoCamera * camera)
        : m_renderer(new QuarterVulkanRenderer(instance, this, scene, camera))
    {
    }

    QVulkanWindowRenderer * createRenderer() override { return m_renderer; }

    QuarterVulkanRenderer * renderer() const { return m_renderer; }

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
    SoCamera * camera = nullptr;
    SbViewportRegion viewport;
    SbColor4f background = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    bool clearWindow = true;
    bool clearDepth = true;
};

QuarterVulkanWidget::QuarterVulkanWidget(QWidget * parent)
    : QWidget(parent)
    , d(new QuarterVulkanWidgetPrivate)
{
    vkLog("QuarterVulkanWidget: constructing");

    d->instance = new QVulkanInstance;
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

    d->vulkanWindow = new QuarterVulkanWindow(d->instance, d->scene, d->camera);
    d->window = d->vulkanWindow;
    d->window->setVulkanInstance(d->instance);
    d->renderer = d->vulkanWindow->renderer();

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
    auto * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->container);
}

QuarterVulkanWidget::~QuarterVulkanWidget()
{
    vkLog("QuarterVulkanWidget: destroying");
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

void QuarterVulkanWidget::setViewportRegion(const SbViewportRegion & region)
{
    d->viewport = region;
    d->renderer->setViewportRegion(region);
}

const SbViewportRegion & QuarterVulkanWidget::getViewportRegion() const
{
    return d->viewport;
}

void QuarterVulkanWidget::setBackgroundColor(const SbColor4f & color)
{
    d->background = color;
    d->renderer->setBackgroundColor(color);
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

void QuarterVulkanWidget::redraw()
{
    d->window->requestUpdate();
}

SoVulkanRenderManager * QuarterVulkanWidget::getRenderManager() const
{
    return d->renderer->getManager();
}

QWidget * QuarterVulkanWidget::getNativeWidget()
{
    return d->container;
}

void QuarterVulkanWidget::viewAll()
{
    if (d->camera) {
        vkLog("viewAll: fitting camera to viewport");
        d->camera->viewAll(static_cast<SoNode *>(nullptr), d->viewport);
        redraw();
    }
}

#endif // FREECAD_USE_VULKAN
