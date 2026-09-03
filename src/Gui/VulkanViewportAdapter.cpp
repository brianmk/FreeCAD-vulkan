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
#include <Inventor/sensors/SoNodeSensor.h>
#include <Inventor/sensors/SoSensor.h>

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
    // document load).  The Vulkan render manager re-resolves the camera
    // from the scene-graph authority every frame (refreshActiveCamera /
    // resolveActiveCamera), so a full re-sync of the (unchanged) scene,
    // overlays, background and settings is not needed here.  Only re-point
    // the widget's camera member, re-orient the axis cross to the new node,
    // and request one frame; a stale camera would otherwise make the
    // auto-clipping update the wrong node's near/far planes while the view
    // used the new node.  syncViewer() (which also pushes the scene) is
    // left for real scene churn via onUpdate()/requestVulkanRender().
    connect(_viewer, &View3DInventorViewer::cameraChanged,
            this, [this] {
                if (!_vulkanViewer || !_viewer) {
                    return;
                }
                SoRenderManager* rm = _viewer->getSoRenderManager();
                if (!rm) {
                    return;
                }
                _vulkanViewer->setCamera(rm->getCamera());
                _viewer->updateAxisCrossNodes();
                _vulkanViewer->setDecorationSceneGraph(_viewer->getAxisCrossOverlay());
                // The camera node was just replaced; re-point the camera change
                // sensor at the new node so pose changes keep waking the frame.
                attachSensors();
                _vulkanViewer->redraw();
            });
    // The viewer owns the Vulkan display options; re-apply them to the
    // Vulkan widget whenever preferences change.
    connect(_viewer, &View3DInventorViewer::vulkanSettingsChanged,
            this, [this] { pushSettings(); });
    stack->setCurrentWidget(_vulkanViewer);
    // The Vulkan widget is display-only; relay its viewport input events
    // to the (hidden) OpenGL viewer so navigation and picking still work.
    // The container<->viewer coordinate scale is derived live from both
    // widgets' devicePixelRatioF() by InputDevice::crossWidgetPositionScale()
    // at event time (single source of truth, portable across 1.25/1.5/2.0
    // display scales); the ratio argument is unused, so pass the default.
    _vulkanViewer->setEventForwardTarget(_viewer->getWidget(), -1.0);
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
    // Relay a ray-tracing-unavailable drop so the view can fall back to a
    // raster render mode (feature detection for non path-tracing hardware).
    connect(_vulkanViewer,
            &SIM::Coin3D::Quarter::QuarterVulkanWidget::rayTracingUnavailable,
            this, &VulkanViewportAdapter::rayTracingUnavailable);
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
    // Track the scene/camera we just pushed so subsequent changes on the
    // (possibly new) nodes wake the Vulkan frame (idempotent).
    attachSensors();
    _vulkanViewer->redraw();
#endif
}

bool VulkanViewportAdapter::isRayTracingAvailable() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer && _vulkanViewer->isRayTracingAvailable();
#else
    return false;
#endif
}

bool VulkanViewportAdapter::isRayTracingProbed() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer && _vulkanViewer->isRayTracingProbed();
#else
    return false;
#endif
}

void VulkanViewportAdapter::useVulkanViewport(bool vulkan)
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    auto* host = qobject_cast<QStackedWidget*>(_vulkanViewer->parentWidget());
    if (!host) {
        return;
    }
    QWidget* target = vulkan ? static_cast<QWidget*>(_vulkanViewer)
                             : _viewer->getWidget();
    if (host->currentWidget() == target) {
        return;
    }
    // The GL viewer drives navigation/picking and is the scene-graph authority;
    // before the Vulkan surface is shown again, push its current
    // scene/camera/background back in so the switch does not leave a stale
    // frame on top.
    if (vulkan) {
        syncViewer();
    }
    host->setCurrentWidget(target);
    if (vulkan) {
        _vulkanViewer->redraw();
    }
#else
    Q_UNUSED(vulkan);
#endif
}

void VulkanViewportAdapter::pushSettings()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    const VulkanViewSettings& settings = _viewer->getVulkanViewSettings();
    // The raster gate is DERIVED here from the single-source settings
    // struct (VulkanViewSettings::rasterOnly()), not passed in separately.
    // In a raster render mode the viewport must never enable path tracing,
    // ray tracing, the denoiser or the edge/point overlays, even when the
    // persisted preferences asked for them -- the mode is the authority and
    // this gate keeps edges/path-tracing from leaking back into Interactive.
    const bool raster = settings.rasterOnly();
    if (Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG")) {
        const bool effEdges = raster ? false : settings.showEdges;
        const bool effPoints = raster ? false : settings.showPoints;
        Base::Console().message("[VK-SET] pushSettings raster=%d edges=%d points=%d "
                                "edgeColor=(%.2f,%.2f,%.2f,%.2f) pt=%d "
                                "bounces=%d settle=%d "
                                "(prefEdges=%d prefPoints=%d)\n",
                                raster ? 1 : 0, effEdges ? 1 : 0,
                                effPoints ? 1 : 0,
                                settings.edgeColor[0], settings.edgeColor[1],
                                settings.edgeColor[2], settings.edgeColor[3],
                                !raster ? 1 : 0,
                                settings.pathTracingBounces,
                                settings.pathTracingSettleFrames,
                                settings.showEdges ? 1 : 0,
                                settings.showPoints ? 1 : 0);
    }
    _vulkanViewer->setWireframeOverlay(raster ? false : settings.showEdges);
    _vulkanViewer->setPointsOverlay(raster ? false : settings.showPoints);
    _vulkanViewer->setEdgeColor(settings.edgeColor);
    // Cubemap environment preset (from the canonical settings struct).
    _vulkanViewer->setEnvMap(settings.envMap);

    // Path tracing toggle + tuning.  Enable only in a ray-traced mode; the
    // start latch (kicking off a progressive render) is raised by the mode
    // switch, not here -- camera moves reset to the live preview until the
    // camera settles, then the accumulation auto-restarts.
    _vulkanViewer->setPathTracingEnabled(!raster);
    _vulkanViewer->setPathTracingBounces(settings.pathTracingBounces);
    _vulkanViewer->setPathTracingSettleFrames(settings.pathTracingSettleFrames);
    _vulkanViewer->setPathTracingMaxSamples(settings.pathTracingMaxSamples);
    // Denoising is required for path tracing and is enabled automatically by
    // the renderer; only the denoiser filter is selectable here.
    if (!settings.pathTracingDenoiser.empty()) {
        _vulkanViewer->setPathTracingDenoiser(settings.pathTracingDenoiser);
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
        !raster && _vulkanViewer->isRayTracingProbed() &&
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
void
VulkanViewportAdapter::redraw()
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->redraw();
    }
#endif
}

void VulkanViewportAdapter::requestVulkanFrame()
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        // Qt coalesces repeated update()/redraw() requests into one repaint, so
        // a burst of sensor triggers (e.g. every animation tick) costs one frame
        // per shown frame rather than one per field write.
        _vulkanViewer->redraw();
    }
#endif
}

void VulkanViewportAdapter::sceneChangedCB(void* data, SoSensor* /*sensor*/)
{
    static_cast<VulkanViewportAdapter*>(data)->requestVulkanFrame();
}

void VulkanViewportAdapter::cameraChangedCB(void* data, SoSensor* /*sensor*/)
{
    static_cast<VulkanViewportAdapter*>(data)->requestVulkanFrame();
}

void VulkanViewportAdapter::attachSensors()
{
#ifdef FREECAD_USE_VULKAN
    if (!_viewer || !_vulkanViewer) {
        return;
    }
    SoRenderManager* rm = _viewer->getSoRenderManager();
    if (!rm) {
        return;
    }
    // Mirror Coin's SoRenderManager: a node sensor on the scene root redraws on
    // any geometry change (Sketcher edits, selection bake, recompute), and one
    // on the camera node redraws on any pose change (interactive navigation,
    // the navcube / view-home animation ticking the camera on a timer, and any
    // programmatic setCameraOrientation).  A node sensor is a node auditor, so
    // it fires on every field write of the tracked node.
    SoNode* root = rm->getSceneGraph();
    if (!_sceneSensor) {
        _sceneSensor = std::make_unique<SoNodeSensor>(&VulkanViewportAdapter::sceneChangedCB, this);
        _sceneSensor->setPriority(1);
    }
    if (_sceneSensor->getAttachedNode() != root) {
        _sceneSensor->detach();
        if (root) {
            _sceneSensor->attach(root);
        }
    }
    SoCamera* camera = rm->getCamera();
    if (!_cameraSensor) {
        _cameraSensor = std::make_unique<SoNodeSensor>(&VulkanViewportAdapter::cameraChangedCB, this);
        _cameraSensor->setPriority(1);
    }
    if (_cameraSensor->getAttachedNode() != camera) {
        _cameraSensor->detach();
        if (camera) {
            _cameraSensor->attach(camera);
        }
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

void VulkanViewportAdapter::setViewMode(int mode)
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->setViewMode(
            static_cast<SIM::Coin3D::Quarter::QuarterVulkanWidget::RtxViewMode>(mode));
    }
#else
    Q_UNUSED(mode);
#endif
}

int VulkanViewportAdapter::getViewMode() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer
        ? static_cast<int>(_vulkanViewer->getViewMode())
        : 0;
#else
    return 0;
#endif
}

void VulkanViewportAdapter::setEnvMap(int index)
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->setEnvMap(index);
    }
#else
    Q_UNUSED(index);
#endif
}

int VulkanViewportAdapter::getEnvMap() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer ? _vulkanViewer->getEnvMap() : -1;
#else
    return -1;
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

uint32_t VulkanViewportAdapter::getRenderFrameCount() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer ? _vulkanViewer->getRenderFrameCount() : 0;
#else
    return 0;
#endif
}

void VulkanViewportAdapter::requestVulkanRender()
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        // Push the current scene/camera (the probe's orbit/edit mutated the
        // GL viewer's camera node or the document, which the Vulkan widget
        // does not observe on its own) and then force exactly one frame, even
        // when the viewport is converged-idle.  Only the adapter owns both the
        // GL viewer (source of truth) and the Vulkan widget, so the sync must
        // happen here, not in the widget.
        syncViewer();
        _vulkanViewer->redraw();
    }
#else
    // no-op
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
    const int pw = qMax(1, qRound(logical.width() * dpr));
    const int ph = qMax(1, qRound(logical.height() * dpr));
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

    // The viewport region is in device pixels (dpr * logical), so tell the
    // render manager the real device-pixel ratio.  This propagates to the
    // CoSoDevicePixelRatioElement and the render backend's params
    // (.devicePixelRatio), which the GL and Vulkan backends use to scale
    // logical SoDrawStyle line widths / point sizes into device pixels.
    // Without it the ratio stayed 1.0, so on a fractional-scaling display
    // (e.g. 1.25) lines and points rendered 1/dpr too thin and, for the
    // NaviCube overlay, its edge/axis strokes and dots drifted off the cube.
    _viewer->getSoRenderManager()->setDevicePixelRatio(static_cast<float>(dpr));

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
        const bool animation = _viewer->isAnimationEnabled();
        if (animation) {
            _viewer->setAnimationEnabled(false);
        }
        _viewer->viewAll();
        if (animation) {
            _viewer->setAnimationEnabled(true);
        }
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
    // Detach the change sensors first: their callbacks call into _vulkanViewer,
    // so they must not fire once the widget / scene graph start going away.
    if (_cameraSensor) {
        _cameraSensor->detach();
    }
    if (_sceneSensor) {
        _sceneSensor->detach();
    }
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
