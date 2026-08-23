// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "VulkanViewportAdapter.h"

#include "Quarter/QuarterVulkanWidget.h"
#include "Quarter/QuarterWidget.h"
#include "View3DInventorViewer.h"

#include <Base/VulkanBreadcrumbs.h>

#include <Inventor/SbColor.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoEventManager.h>
#include <Inventor/nodes/SoAnnotation.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/SoRenderManager.h>

#include <QEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QWidget>

#ifdef FREECAD_USE_VULKAN
#include <vulkan/vulkan.h>
#endif

using namespace Gui;

VulkanViewportAdapter::VulkanViewportAdapter(QStackedWidget* stack,
                                             View3DInventorViewer* viewer,
                                             bool useRayTracing,
                                             QObject* parent)
    : QObject(parent)
    , _viewer(viewer)
{
#ifdef FREECAD_USE_VULKAN
    if (!_viewer) {
        return;
    }
    _vulkanViewer = new SIM::Coin3D::Quarter::QuarterVulkanWidget(stack, useRayTracing);
    _vulkanViewer->setSampleCount(View3DInventorViewer::getNumSamples());
    // QVulkanWindow::grab() only converts 8-bit swapchain formats;
    // request B8G8R8A8_UNORM so screenshot tests read exact pixels.
    _vulkanViewer->setPreferredColorFormat(VK_FORMAT_B8G8R8A8_UNORM);
    stack->addWidget(_vulkanViewer);
    VK_BREADCRUMB("[VK-TRACE] View3DInventor: QuarterVulkanWidget created\n");
    syncViewer();
    // The viewer replaces its camera node whenever the projection type
    // changes (menu toggle, Python setCameraType, camera restore on
    // document load).  Re-sync the Vulkan widget immediately so its
    // render manager never keeps referencing the orphaned old camera:
    // a stale camera makes the auto-clipping update the wrong node's
    // near/far planes while the rendered view uses the new node, whose
    // planes stay at their defaults and cull everything beyond 10 units.
    connect(_viewer, &View3DInventorViewer::cameraChanged,
            this, [this] { syncViewer(); });
    // The viewer owns the Vulkan display options; re-apply them to the
    // Vulkan widget whenever preferences change.
    connect(_viewer, &View3DInventorViewer::vulkanSettingsChanged,
            this, [this] { pushSettings(); });
    stack->setCurrentWidget(_vulkanViewer);
    // The Vulkan widget is display-only; relay its viewport input events
    // to the (hidden) OpenGL viewer so navigation and picking still work.
    // Pass the ratio the GL side itself uses when converting event
    // positions (QuarterWidget caches devicePixelRatio(); that cached value
    // is what the relay must match, not devicePixelRatioF()), so the widget
    // does not have to guess it from the target's type at event time.
    qreal glDpr = _viewer->getWidget()->devicePixelRatioF();
    if (const auto * quarter = qobject_cast<
            const SIM::Coin3D::Quarter::QuarterWidget *>(
                _viewer->getWidget())) {
        glDpr = quarter->devicePixelRatio();
    }
    _vulkanViewer->setEventForwardTarget(_viewer->getWidget(), glDpr);
    // Navigation and picking run on the hidden OpenGL viewer, so cursor
    // shape changes land on its widget.  Mirror them onto the visible
    // Vulkan container (see eventFilter) and pick up the initial state.
    _viewer->getWidget()->installEventFilter(this);
    _vulkanViewer->setCursor(_viewer->getWidget()->cursor());
    // Keep the hidden GL viewer's viewport region in sync with the
    // Vulkan surface so navigation (aspect/near-far) and ray picking
    // use the visible surface size rather than a stale default.
    connect(_vulkanViewer,
            &SIM::Coin3D::Quarter::QuarterVulkanWidget::surfaceSizeChanged,
            this, &VulkanViewportAdapter::onSurfaceSizeChanged);
#else
    Q_UNUSED(stack);
    Q_UNUSED(viewer);
    Q_UNUSED(useRayTracing);
#endif
}

void VulkanViewportAdapter::syncViewer()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    SoRenderManager* rm = _viewer->getSoRenderManager();
    if (!rm) {
        return;
    }
    _vulkanViewer->setSceneGraph(rm->getSceneGraph());
    _vulkanViewer->setOverlaySceneGraph(_viewer->getNaviCubeAnnotation());
    // The hidden GL viewer's frame loop never runs, so the axis cross
    // overlay nodes are refreshed here for the IR (Vulkan) render path.
    _viewer->updateAxisCrossNodes();
    _vulkanViewer->setDecorationSceneGraph(_viewer->getAxisCrossOverlay());
    _vulkanViewer->setCamera(rm->getCamera());
    _vulkanViewer->setBackgroundColor(rm->getBackgroundColor());
    const View3DInventorViewer::Background gradient =
        _viewer->getGradientBackground();
    if (gradient != View3DInventorViewer::Background::NoGradient) {
        SbColor from;
        SbColor to;
        _viewer->getGradientBackgroundColor(from, to);
        _vulkanViewer->setBackgroundGradient(true,
                                             SbColor4f(from[0], from[1], from[2], 1.0f),
                                             SbColor4f(to[0], to[1], to[2], 1.0f));
    }
    else {
        _vulkanViewer->setBackgroundGradient(false,
                                             SbColor4f(0.0f, 0.0f, 0.0f, 1.0f),
                                             SbColor4f(0.0f, 0.0f, 0.0f, 1.0f));
    }
    pushSettings();
    _vulkanViewer->redraw();
#endif
}

void VulkanViewportAdapter::pushSettings()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    const VulkanViewSettings& settings = _viewer->getVulkanViewSettings();
    if (Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG")) {
        Base::Console().message("[VK-SET] pushSettings edges=%d points=%d "
                                "edgeColor=(%.2f,%.2f,%.2f,%.2f) pt=%d "
                                "bounces=%d settle=%d denoise=%d\n",
                                settings.showEdges ? 1 : 0,
                            settings.showPoints ? 1 : 0,
                            settings.edgeColor[0], settings.edgeColor[1],
                            settings.edgeColor[2], settings.edgeColor[3],
                            settings.pathTracing ? 1 : 0,
                            settings.pathTracingBounces,
                            settings.pathTracingSettleFrames,
                            settings.pathTracingDenoise ? 1 : 0);
    }
    _vulkanViewer->setWireframeOverlay(settings.showEdges);
    _vulkanViewer->setPointsOverlay(settings.showPoints);
    _vulkanViewer->setEdgeColor(settings.edgeColor);

    // Path tracing toggle + tuning (start flag: enabling kicks off a
    // progressive render; camera moves reset to the live preview until the
    // camera settles, then the accumulation auto-restarts).
    _vulkanViewer->setPathTracingEnabled(settings.pathTracing);
    _vulkanViewer->setPathTracingBounces(settings.pathTracingBounces);
    _vulkanViewer->setPathTracingSettleFrames(settings.pathTracingSettleFrames);
    _vulkanViewer->setPathTracingDenoise(settings.pathTracingDenoise);
    if (settings.pathTracing) {
        _vulkanViewer->setPathTracingStart(true);
    }
    // The RTX backend is always brought up when the device supports it
    // (independent of UseVulkanRayTracing), so path tracing can be toggled
    // live with the preference: no document reopen needed.  The only case
    // where a path-tracing request cannot be honored is hardware without
    // VK_KHR_acceleration_structure / ray_tracing_pipeline / ray_query; warn
    // once per transition (the preferences signal that drives pushSettings()
    // fires repeatedly) instead of spamming every tick.  The check is gated
    // on isRayTracingProbed(): before the renderer's first initResources()
    // availability is unknown and must not produce a spurious warning.
    const bool rtUnavailable =
        settings.pathTracing && _vulkanViewer->isRayTracingProbed() &&
        !_vulkanViewer->isRayTracingAvailable();
    if (rtUnavailable && !_pathTracingRtMismatchWarned) {
        Base::Console().warning(
            "[VK-SET] Path tracing is enabled in preferences, but this device "
            "does not support hardware ray tracing (VK_KHR_acceleration_structure "
            "/ VK_KHR_ray_tracing_pipeline), so the view renders with the "
            "raster Vulkan backend. Path tracing is unavailable on this "
            "hardware.\n");
        _pathTracingRtMismatchWarned = true;
    }
    else if (!rtUnavailable) {
        _pathTracingRtMismatchWarned = false;
    }
#endif
}

void VulkanViewportAdapter::setPathTracingEnabled(bool enabled)
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->setPathTracingEnabled(enabled);
    }
#else
    Q_UNUSED(enabled);
#endif
}

void VulkanViewportAdapter::setPathTracingStart(bool start)
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->setPathTracingStart(start);
    }
#else
    Q_UNUSED(start);
#endif
}

bool VulkanViewportAdapter::isPathTracingEnabled() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer && _vulkanViewer->getPathTracingEnabled();
#else
    return false;
#endif
}

bool VulkanViewportAdapter::isPathTracingActive() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer && _vulkanViewer->getPathTracingActive();
#else
    return false;
#endif
}

void VulkanViewportAdapter::onSurfaceSizeChanged(const QSize& surfaceSize)
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    // The Vulkan swapchain size is in device pixels and is not a
    // stable source for sizing the hidden GL widget: resizing a
    // non-current QStackedWidget page changes the stack's sizeHint,
    // which feeds back into the window and, in turn, the swapchain
    // (this produced an oscillating surface size).  Size the hidden
    // viewer to the visible Vulkan container instead.
    //
    // Event positions reach the hidden GL viewer already scaled to
    // device pixels: EventFilter::trackPointerPosition() runs
    // InputDevice::toDevicePixelPosition(), which multiplies the
    // logical Qt position by the widget's device pixel ratio, and
    // QuarterWidget::resizeEvent() sets the render/event manager
    // viewport region to dpr * size (device pixels).  The viewport
    // region must therefore be in the same device-pixel space for
    // SoRayPickAction's normalized coordinates to match the ray;
    // using the logical size would shift hover picking and
    // navigation by the DPI factor on high-density displays.
    QWidget* container = _vulkanViewer->getNativeWidget();
    QWidget* glWidget = _viewer->getWidget();
    if (!container || !glWidget) {
        return;
    }
    const QSize logical = container->size();
    if (logical.width() <= 0 || logical.height() <= 0) {
        return;
    }
    const qreal dpr = glWidget->devicePixelRatioF();
    const int pw = qMax(1, static_cast<int>(logical.width() * dpr));
    const int ph = qMax(1, static_cast<int>(logical.height() * dpr));
    const SbVec2s glSize =
        _viewer->getSoRenderManager()->getViewportRegion().getViewportSizePixels();
    VK_BREADCRUMB("[VK-TRACE] surfaceSizeChanged surface=%dx%d "
                  "container=%dx%d logical=%dx%d glViewport(before)=%dx%d "
                  "glWidgetSize=%dx%d dpr=%.3f\n",
                  surfaceSize.width(), surfaceSize.height(),
                  container->width(), container->height(),
                  logical.width(), logical.height(),
                  glSize[0], glSize[1],
                  glWidget->width(), glWidget->height(), dpr);

    SbViewportRegion vp(static_cast<short>(pw), static_cast<short>(ph));
    _viewer->getSoRenderManager()->setViewportRegion(vp);
    _viewer->getSoEventManager()->setViewportRegion(vp);

    // NOTE: Do NOT write the surface aspect into the shared camera's
    // aspectRatio field.  SoOrthographicCamera::getViewVolume() (and
    // SoPerspectiveCamera::getViewVolume()) apply the aspectRatio
    // FIELD, and FreeCAD-side math (the Sketcher's getProjectingLine,
    // navigation) already applies the VIEWPORT aspect itself.  With
    // the field also set, the aspect is applied twice and cursor
    // mapping drifts away from the cursor, growing with the distance
    // from the view center.  The Vulkan projection and viewAll()
    // framing use the viewport region (kept in sync above), matching
    // classic GL FreeCAD where the field stays at its default.

    // Re-frame once the surface has a real size.  At startup the
    // swapchain is created with a default size and only later
    // matches the window, so the first viewAll() ran against a
    // wrong viewport/aspect and framed the camera too close to the
    // scene.  Re-running it here (only on the first stable size)
    // repositions the camera outside the object.
    if (!_initialVulkanFitDone && pw > 1 && ph > 1) {
        _initialVulkanFitDone = true;
        _viewer->viewAll();
    }

    // Prevent the hidden page from affecting the stack's sizeHint so
    // this does not feed back into the window/swapchain size.
    if (glWidget->sizePolicy()
        != QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored)) {
        glWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
    if (glWidget->size() != logical) {
        glWidget->resize(logical);
    }
#else
    Q_UNUSED(surfaceSize);
#endif
}

VulkanViewportAdapter::~VulkanViewportAdapter()
{
#ifdef FREECAD_USE_VULKAN
    // The QuarterVulkanWidget is a QObject child of the stack, so it outlives
    // this adapter (and the hidden _viewer) unless we remove it now.  Left
    // connected, its QVulkanWindow keeps re-initializing against a dying
    // scene graph and its event filter / signal connections to _viewer fire
    // after _viewer is freed -> SoGroup::removeChild double-remove and a
    // QVulkanInstance::functions() SIGSEGV during document close.  Detach it
    // from the viewer and drop it from the stack so the QVulkanWindow shuts
    // down synchronously here, before _viewer is destroyed.
    if (_vulkanViewer) {
        // Stop forwarding input to the (soon-dead) GL viewer.
        _vulkanViewer->setEventForwardTarget(nullptr, 1.0);
        QWidget* host = _vulkanViewer->parentWidget();
        if (auto* stack = qobject_cast<QStackedWidget*>(host)) {
            stack->removeWidget(_vulkanViewer);
        }
        delete _vulkanViewer;
        _vulkanViewer = nullptr;
    }
#endif
}

bool VulkanViewportAdapter::eventFilter(QObject* watched, QEvent* event)
{
#ifdef FREECAD_USE_VULKAN
    // The hidden OpenGL viewer drives navigation and picking, and its widget
    // is where the navigation code sets cursor shapes.  Mirror them onto the
    // visible Vulkan container so modes like spin/zoom/pan show the right
    // pointer shape over the viewport.
    if (_vulkanViewer && event->type() == QEvent::CursorChange) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && _viewer && widget == _viewer->getWidget()) {
            _vulkanViewer->setCursor(widget->cursor());
        }
    }
#endif
    return QObject::eventFilter(watched, event);
}
