// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "VulkanViewportAdapter.h"

#include "Quarter/QuarterVulkanWidget.h"
#include "Quarter/QuarterWidget.h"
#include "Application.h"
#include "InteractionController.h"
#include "View3DInventor.h"
#include "View3DInventorViewer.h"

#include "GpuPickService.h"

#include <Base/VulkanBreadcrumbs.h>

#include <Inventor/SbColor.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/SbRotation.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoEventManager.h>
#include <Inventor/nodes/SoAnnotation.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoDirectionalLight.h>
#include <Inventor/nodes/SoEnvironment.h>
#include <Inventor/nodes/SoPointLight.h>
#include <Inventor/nodes/SoSpotLight.h>
#include <Inventor/SoRenderManager.h>
#include <Inventor/rendering/SoRenderIR.h>
#include <Inventor/rendering/SoVulkanViewSettings.h>
#include <Inventor/sensors/SoNodeSensor.h>
#include <Inventor/sensors/SoSensor.h>

#include <QEvent>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#ifdef FREECAD_USE_VULKAN
#include <vulkan/vulkan.h>
#endif

using namespace Gui;

VulkanViewportAdapter::VulkanViewportAdapter(QStackedWidget* stack,
                                             View3DInventorViewer* viewer,
                                             QObject* parent)
    : QObject(parent)
    , _viewer(viewer)
    , _glSurface(viewer)
    , _interactionTimer(new QTimer(this))
{
    // The GL viewer is the controller's base surface: it owns the camera,
    // scene graph and Coin event manager.  When the Vulkan page is current the
    // controller is swapped to this adapter (see useVulkanViewport), which
    // forwards everything here except getGLWidget()/scheduleRedraw().
    // Interaction LOD: a camera move engages a single-bounce ray-traced
    // preview; the timer disengages it once the camera has been still for its
    // interval, which requests the full-quality restart.  Created for every
    // build (the setter it drives is a no-op without a Vulkan widget).
    _interactionTimer->setSingleShot(true);
    // Long enough that a continuous drag never re-enables full quality
    // mid-gesture, short enough that the refined image appears promptly on
    // release.
    _interactionTimer->setInterval(200);
    connect(_interactionTimer, &QTimer::timeout, this, [this] {
        setInteractionLod(false);
    });
#ifdef FREECAD_USE_VULKAN
    if (!_viewer) {
        return;
    }
    _vulkanViewer = new SIM::Coin3D::Quarter::QuarterVulkanWidget(stack);
    _vulkanViewer->setSampleCount(View3DInventorViewer::getNumSamples());
    // QVulkanWindow::grab() only converts 8-bit swapchain formats;
    // request B8G8R8A8_UNORM so screenshot tests read exact pixels.
    _vulkanViewer->setPreferredColorFormat(VK_FORMAT_B8G8R8A8_UNORM);
    // HDR output is a window/swapchain property: the preferred format and the
    // surface color space must be chosen before QVulkanWindow creates the
    // surface, so the preference is read here rather than in pushSettings().
    // Seed the single-source settings first (applyVulkanSettings() is
    // idempotent and its change signal has no listeners yet).
    _viewer->applyVulkanSettings();
    if (_viewer->getVulkanViewSettings().hdrEnabled) {
        _vulkanViewer->setHdrOutputEnabled(true);
    }
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
                if (!_vulkanViewer) {
                    return;
                }
                this->resyncCameraAndDecorations();
                _vulkanViewer->redraw();
            });
    // The viewer owns the Vulkan display options; re-apply them to the
    // Vulkan widget whenever preferences change.
    connect(_viewer, &View3DInventorViewer::vulkanSettingsChanged,
            this, [this] { pushSettings(); });
    // Do NOT make the Vulkan page current here.  QVulkanWindow creates its
    // VkDevice on first expose, so leaving the hidden GL page current keeps the
    // device out of the document-open path: a view opened in RasterCoin mode
    // never creates a device, and a RasterVulkan view creates it on the first
    // useVulkanViewport(true) below (deferred one event-loop turn).  setRenderMode()
    // calls useVulkanViewport() right after construction, so the intended page
    // is selected there.
    // The Vulkan widget translates its own mouse/wheel/keyboard input (it is
    // an InputDeviceHost) and delivers Coin events to this sink, which drives
    // the InteractionController.  Navigation and picking therefore run on the
    // shared controller without the hidden GL viewer's event manager.  Tablet/
    // touch/context-menu events are not translated by the Coin devices, so
    // relay them to the hidden GL viewer that owns FreeCAD's gesture devices.
    _vulkanViewer->setRawEventTarget(_viewer->getWidget());
    _vulkanViewer->setEventSink([this](const SoEvent* ev) {
        auto* controller = _viewer ? _viewer->getInteractionController() : nullptr;
        return controller && controller->processSoEvent(ev);
    });
    // Navigation's cursor shapes are routed to the visible surface by
    // View3DInventorViewer::setCursorTarget() (set in useVulkanViewport), so
    // no cursor mirroring is needed.  The event filter on the GL widget only
    // re-imposes the surface viewport region after a hidden-widget resize.
    _viewer->getWidget()->installEventFilter(this);
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
    // Re-point the camera + axis-cross decorations and re-attach the change
    // sensors (shared with the camera-changed fast path).
    this->resyncCameraAndDecorations();
    // The background (solid color + gradient + environment preset) is pushed
    // by pushSettings(), the single source of truth derived from the hidden
    // GL viewer.  syncViewer() only re-seeds scene/camera/overlays here and
    // lets pushSettings() refresh the background, so a background change is
    // pushed in exactly one place.
    pushSettings();
    _vulkanViewer->redraw();
#endif
}

void VulkanViewportAdapter::resyncCameraAndDecorations()
{
#ifdef FREECAD_USE_VULKAN
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
    // The camera node may have just been replaced; re-point the camera change
    // sensor at the new node so pose changes keep waking the frame.
    attachSensors();
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
    _wantVulkanViewport = vulkan;
    // Register (or drop) the GPU pick bridge: while the Vulkan page is
    // displayed, SoBrepFaceSet::rayPick consults the RTX backend's TLAS instead
    // of generating every triangle on the CPU.  The picker returns false until
    // hardware ray tracing is actually active, so a raster Vulkan view keeps
    // the CPU path, and the GL page clears it so GL picking is unchanged.
    if (vulkan) {
        Gui::GpuPickService::instance().setPicker(
            this,
            [this](const float origin[3], const float direction[3], float tMax,
                   Gui::GpuPickResult& out) {
                if (!_vulkanViewer) {
                    return false;
                }
                // The service is process-global: with more than one Vulkan view
                // open the picker belongs to whichever adapter registered last.
                // A non-active view must answer false so its own scene graph
                // falls back to the CPU pick -- otherwise SoBrepFaceSet trusts
                // a GPU "usable" result that names a shape from another view and
                // silently skips its own primitive traversal (hover then misses).
                auto* active = dynamic_cast<Gui::View3DInventor*>(
                    Gui::Application::Instance->activeView());
                if (!active || active->getViewer() != _viewer) {
                    return false;
                }
                SIM::Coin3D::Quarter::QuarterVulkanWidget::VulkanPickHit hit;
                if (!_vulkanViewer->pickRay(origin, direction, tMax, hit)) {
                    return false;
                }
                out.hit = hit.hit;
                out.shape = hit.userData;
                out.primitiveId = hit.primitiveId;
                out.primitiveOffset = hit.primitiveOffset;
                out.worldPos[0] = hit.worldPos[0];
                out.worldPos[1] = hit.worldPos[1];
                out.worldPos[2] = hit.worldPos[2];
                return true;
            });
    }
    else {
        Gui::GpuPickService::instance().clearPicker(this);
    }
    // The hidden GL viewer drives picking/navigation, but its own geometry is
    // unreliable (it is never shown, so it keeps a stale/default size).  The
    // render-manager viewport region is the single source of truth and is
    // pinned to the Vulkan surface (device pixels) by applySurfaceViewportToGL,
    // so tell the event/DPR conversion to normalize cursor positions against
    // that region (effectiveWindowSize) instead of the widget's own size.
    // Without this the Y-flip used the stale hidden-widget height and hover/
    // click picks landed far off the cursor.  In the classic GL page the
    // region tracks the visible widget, so the flag is cleared and the cached
    // logical size is used (upstream behavior).
    _viewer->setVulkanDevicePixels(vulkan);
    // Route navigation's cursor shapes to the visible surface: the Vulkan
    // container while the Vulkan page is current, the GL widget otherwise.
    _viewer->setCursorTarget(
        vulkan ? _vulkanViewer->getNativeWidget() : nullptr);
    // Swap the controller's surface so surface-presentation calls
    // (getGLWidget for context menus, scheduleRedraw) reach the visible
    // surface.  Everything else is delegated back to the GL viewer.
    if (auto* controller = _viewer->getInteractionController()) {
        controller->setSurface(vulkan ? static_cast<InteractionSurface*>(this)
                                      : _glSurface);
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
        if (!_vulkanActivated) {
            // First activation: defer making the page current by one event-loop
            // turn.  QVulkanWindow creates its VkDevice on first expose, so this
            // keeps the one-time device creation off the document-open critical
            // path (the surface paints a moment later instead).  `this` is the
            // timer context, so a destroyed adapter cancels the show.
            _vulkanActivated = true;
            QTimer::singleShot(0, this, [this] {
                if (!_wantVulkanViewport || !_vulkanViewer || !_viewer) {
                    return;
                }
                auto* h = qobject_cast<QStackedWidget*>(
                    _vulkanViewer->parentWidget());
                if (!h) {
                    return;
                }
                h->setCurrentWidget(_vulkanViewer);
                // Making the Vulkan page current re-lays-out the stacked
                // widget, which resets the hidden GL viewer's viewport region
                // to its own default size (the pick/navigation authority).
                // Re-impose the surface size now instead of waiting for the
                // first frame's size notification, so a freshly created view
                // is pickable immediately.
                if (QWidget* c = _vulkanViewer->getNativeWidget()) {
                    applySurfaceViewportToGL(c->size());
                }
                _vulkanViewer->redraw();
            });
            return;
        }
    }
    host->setCurrentWidget(target);
    if (vulkan) {
        if (QWidget* c = _vulkanViewer->getNativeWidget()) {
            applySurfaceViewportToGL(c->size());
        }
        _vulkanViewer->redraw();
    }
#else
    Q_UNUSED(vulkan);
#endif
}

void VulkanViewportAdapter::resyncViewport()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    // The hidden GL viewer is the pick/navigation authority; its viewport
    // region is the source for SoRayPickAction's normalized coordinates.  It
    // is reset to the GL widget's own size on every re-layout (a new document
    // or the attachment task panel), so re-impose the visible surface size
    // before re-pushing the scene.  Without this a freshly created view can
    // keep picking against a stale region until the render mode is re-applied.
    if (QWidget* container = _vulkanViewer->getNativeWidget()) {
        applySurfaceViewportToGL(container->size());
    }
    // SoRayPickAction derives its ray depth range from the shared camera's
    // near/far planes.  A GL render auto-fits those planes to the scene, but
    // the display-only Vulkan path never renders through the GL viewer, so on
    // a scene that appeared after the camera was last framed (e.g. the origin
    // planes the sketch attachment editor enlarges over a brand-new empty
    // document) the planes fall outside [near, far] and hover/click picks miss
    // until a render-mode round-trip runs the GL auto-clip.  Refresh the planes
    // here, from the same scene bounding box the GL render would use.
    if (SoRenderManager* rm = _viewer->getSoRenderManager()) {
        rm->updateClippingPlanes();
    }
    syncViewer();
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
    // ray tracing or the denoiser, even when the persisted preferences asked
    // for them -- the mode is the authority.
    const bool raster = settings.rasterOnly();

    // Memoised push: the preferences signal that drives pushSettings() fires
    // repeatedly with identical values (applyVulkanSettings() re-emits
    // vulkanSettingsChanged on every call, not only on change).  Only re-apply
    // to the renderer and emit the [VK-SET] diagnostic when the effective
    // values actually differ; otherwise this is no-op and keeps the log quiet.
    //
    // The edge/point (wireframe) overlay is the one raster-only feature: it is
    // drawn by the raster backend's overlay fill-mode re-draw (see
    // SoVulkanRenderBackendFrame), which only the raster path applies -- the
    // RTX backend never consumes it.  So it must be ALLOWED in the raster modes
    // and disabled in the ray-traced modes.  Gating it by `raster` the other
    // way round (like path tracing / the denoiser) made the VulkanWireframe /
    // VulkanShowPoints preferences unreachable.
    const bool effWireframe = raster ? settings.wireframe : false;
    const bool effPoints = raster ? settings.showPoints : false;

    // Background is a single view of truth derived here from the hidden GL
    // viewer (render-manager solid color + pcBackGround gradient) and pushed
    // in one place along with the environment preset.  syncViewer() no longer
    // sets the background directly; it only re-seeds scene/camera/overlays and
    // lets pushSettings() refresh the background, so there is one push path
    // and the solid/gradient/env state cannot drift between the two callers.
    SbColor4f bgColor = SbColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    float bgTop[3] = {0.0f, 0.0f, 0.0f};
    float bgBottom[3] = {0.0f, 0.0f, 0.0f};
    bool bgGradient = false;
    if (SoRenderManager* rm = _viewer->getSoRenderManager()) {
        bgColor = rm->getBackgroundColor();
        const View3DInventorViewer::Background gradient =
            _viewer->getGradientBackground();
        bgGradient = (gradient != View3DInventorViewer::Background::NoGradient);
        if (bgGradient) {
            SbColor from;
            SbColor to;
            _viewer->getGradientBackgroundColor(from, to);
            bgTop[0] = from[0]; bgTop[1] = from[1]; bgTop[2] = from[2];
            bgBottom[0] = to[0]; bgBottom[1] = to[1]; bgBottom[2] = to[2];
        }
    }

    // One settings blob for the whole display/tuning state.  The render
    // manager diffs it and re-applies only on change, so this is safe to call
    // repeatedly (the preferences signal that drives pushSettings() fires
    // often with identical values).  The stateful path-tracing enable/start
    // latch remains a separate call.
    SoVulkanViewSettings vs;
    vs.viewMode = viewRenderModeToWidgetMode(
        static_cast<ViewRenderMode>(settings.renderMode));
    vs.envMap = settings.envMap;
    // Denoising is required for path tracing, so it tracks the raster gate.
    vs.pathTracingDenoise = !raster;
    vs.pathTracingBounces = settings.pathTracingBounces;
    vs.pathTracingSettleFrames = settings.pathTracingSettleFrames;
    vs.pathTracingMaxSamples = settings.pathTracingMaxSamples;
    vs.pathTracingDenoiser = settings.pathTracingDenoiser;
    vs.pathTracingDenoiserScale = settings.pathTracingDenoiserScale;
    vs.pathTracingGlassIor = settings.pathTracingGlassIor;
    vs.pathTracingGlassAbsorption = settings.pathTracingGlassAbsorption;
    // Background and environment preset travel together: both drive the
    // frame's sky/miss radiance (see SoRenderParams::background* and
    // SoRTXRenderBackend::setEnvMap), so one push keeps raster and ray-traced
    // backgrounds in agreement.
    vs.backgroundColor = bgColor;
    vs.backgroundGradient = bgGradient;
    vs.backgroundTop = SbColor4f(bgTop[0], bgTop[1], bgTop[2], 1.0f);
    vs.backgroundBottom = SbColor4f(bgBottom[0], bgBottom[1], bgBottom[2], 1.0f);
    vs.wireframeOverlay = effWireframe;
    vs.pointsOverlay = effPoints;
    vs.edgeColor = settings.edgeColor;
    // HDR output: encode only when the preference asked for it AND the live
    // swapchain actually came up with an HDR format (the window chooses the
    // format before the settings can be pushed).  Otherwise the output stays
    // SDR, matching the 8-bit surface.
    vs.hdrOutput = settings.hdrEnabled && _vulkanViewer->isHdrOutputActive();
    // Map scene-white to the user's reference white (see VulkanViewSettings).
    vs.hdrExposure = settings.hdrExposure;
    // Highlight rolloff: 0 = clip, 1 = Reinhard, 2 = ACES, 3 = Hable.
    vs.hdrToneMap = settings.hdrToneMap;

    if (Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG")) {
        Base::Console().message(
            "[VK-SET] pushSettings raster=%d wireframe=%d points=%d "
            "edgeColor=(%.2f,%.2f,%.2f,%.2f) pt=%d bounces=%d settle=%d "
            "hdr=%d hdrExposure=%.4f hdrToneMap=%d "
            "(prefWireframe=%d prefPoints=%d)\n",
            raster ? 1 : 0, effWireframe ? 1 : 0, effPoints ? 1 : 0,
            settings.edgeColor[0], settings.edgeColor[1],
            settings.edgeColor[2], settings.edgeColor[3], !raster ? 1 : 0,
            settings.pathTracingBounces, settings.pathTracingSettleFrames,
            vs.hdrOutput ? 1 : 0, vs.hdrExposure, vs.hdrToneMap,
            settings.wireframe ? 1 : 0, settings.showPoints ? 1 : 0);
    }
    _vulkanViewer->setViewSettings(vs);
    // Enable/disable the ray tracer (stateful backend lifecycle).
    _vulkanViewer->setPathTracingEnabled(!raster);
    // The RTX backend is always brought up when the device supports it, so
    // path tracing can be toggled live with the preference: no document reopen
    // needed.  The only case
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

    // A preference change can disable interaction LOD mid-gesture; drop the
    // engaged state so the full-quality render resumes immediately.
    if (!settings.interactionLod) {
        setInteractionLod(false);
    }

    // GL-authoritative scene lighting -> both Vulkan backends.  The IR
    // draw-list lighting capture (SoLightElement::getLights) is unreliable on
    // the retained/replayed path tracer (the captured light count can drop to
    // zero, rendering surfaces at ambient-only/near-black).  Gather the viewer
    // three-point lights and push the camera-anchored world-space set so both
    // backends always have the real, view-following lights.
    this->pushSceneLights();
#endif
}

void
VulkanViewportAdapter::pushSceneLights()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }

    // World-space light set.  The GL viewer's three-point lighting is
    // VIEW-RELATIVE: the headlight and backlight are traversed before the
    // camera node (so glLightfv sees an identity modelview and their raw
    // direction is applied in eye space), and the fill light hangs under an
    // SoRotation connected to the camera orientation, which also makes it
    // eye-space-fixed.  Reproduce that here by taking each light's eye-space
    // travel direction (the negated direction field) and rotating it into
    // world space by the current camera orientation, so the backends -- which
    // shade in world space -- keep the highlights following the camera exactly
    // as Coin GL does.  Without the camera rotation the head/back lights would
    // stay world-fixed in Vulkan and the reflections would not track the view.
    SoLightingData lighting;

    SoRenderManager* rm = _viewer->getSoRenderManager();
    SbRotation camRot;
    // World <- eye rotation for the current camera: the camera orientation is
    // the inverse of the view rotation, i.e. exactly what SoRenderIR::
    // lightToWorld() expects.  The eye<->world convention lives in SoRenderIR
    // instead of being re-derived here.
    SbMatrix eyeToWorld;
    if (SoCamera* cam = rm ? rm->getCamera() : nullptr) {
        camRot = cam->orientation.getValue();
        camRot.getValue(eyeToWorld);
    }

    // Scene ambient from the viewer's environment node (so a scene lit purely
    // by ambient still reads non-black).
    if (SoEnvironment* env = _viewer->getEnvironment()) {
        const SbColor& ac = env->ambientColor.getValue();
        float ai = env->ambientIntensity.getValue();
        lighting.ambient = SbVec3f(ac[0] * ai, ac[1] * ai, ac[2] * ai);
    }

    // Push each enabled directional light with the same world-space
    // convention the raster IR uses (headlight + backlight + fill, matching
    // the GL viewer's three-point lighting), but anchored to the camera so
    // the Vulkan backends follow the view like Coin GL.
    lighting.lights.reserve(3);
    const SoDirectionalLight* lightsL[] = {
        _viewer->getHeadlight(), _viewer->getBacklight(), _viewer->getFillLight()};
    for (const SoDirectionalLight* light : lightsL) {
        if (!light || !light->on.getValue()) {
            continue;
        }
        SoLightData l;
        l.type = SO_LIGHT_DIRECTIONAL;
        const SbVec3f c = light->color.getValue();
        float i = light->intensity.getValue();
        l.color = SbVec3f(c[0] * i, c[1] * i, c[2] * i);
        SbVec3f eyeDir = -light->direction.getValue();
        if (eyeDir.normalize() == 0.0f) {
            eyeDir = SbVec3f(0.0f, 0.0f, 1.0f);
        }
        l.direction = eyeDir;
        lighting.lights.push_back(SoRenderIR::lightToWorld(l, eyeToWorld));
    }

    if (Base::envFlagEnabled("FC_LIGHT_TRACE")) {
        static int _n = 0;
        if (_n++ < 400) {
            SoCamera* cam = rm ? rm->getCamera() : nullptr;
            fprintf(stderr,
                    "[LTRACE] pushSceneLights n=%d cam=%p rot=(%.4f,%.4f,%.4f,%.4f) nlights=%zu\n",
                    _n, static_cast<void*>(cam), camRot[0], camRot[1], camRot[2], camRot[3],
                    lighting.lights.size());
            for (size_t i = 0; i < lighting.lights.size(); ++i) {
                fprintf(stderr, "[LTRACE]   light[%zu] dir=(%.4f,%.4f,%.4f) col=(%.3f,%.3f,%.3f)\n",
                        i, lighting.lights[i].direction[0], lighting.lights[i].direction[1],
                        lighting.lights[i].direction[2], lighting.lights[i].color[0],
                        lighting.lights[i].color[1], lighting.lights[i].color[2]);
            }
        }
    }

    _vulkanViewer->setSceneLights(lighting);
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
        // Refresh the authoritative light set first: the viewer lights are
        // camera-anchored (see pushSceneLights), so a camera move must
        // re-derive their world directions before the frame is drawn.
        pushSceneLights();
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
    if (Base::envFlagEnabled("FC_LIGHT_TRACE")) {
        static int _n = 0;
        if (_n++ < 400) {
            fprintf(stderr, "[LTRACE] cameraChangedCB n=%d\n", _n);
        }
    }
    auto* self = static_cast<VulkanViewportAdapter*>(data);
    self->noteCameraMoved();
    self->requestVulkanFrame();
}

void VulkanViewportAdapter::noteCameraMoved()
{
#ifdef FREECAD_USE_VULKAN
    if (!_viewer) {
        return;
    }
    const VulkanViewSettings& settings = _viewer->getVulkanViewSettings();
    // Interaction LOD is opt-in via the VulkanInteractionLod preference
    // (default on).  It applies to every Vulkan mode: a ray-traced mode lowers
    // the RT bounce count, and a raster mode draws wide lines as plain 1px GPU
    // lines instead of expanding every edge segment into quads on the CPU (the
    // dominant navigation cost on large edge sets).  When disabled, make sure
    // any previously engaged state is dropped.
    if (!settings.interactionLod) {
        setInteractionLod(false);
        return;
    }
    setInteractionLod(true);
    if (_interactionTimer) {
        _interactionTimer->start();
    }
#endif
}

void VulkanViewportAdapter::noteUserCameraMoved()
{
    _userCameraMoved = true;
}

void VulkanViewportAdapter::setInteractionLod(bool active)
{
    if (_interactionLod == active) {
        return;
    }
    _interactionLod = active;
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        // The widget's setter requests the frame that shows the new quality.
        _vulkanViewer->setInteractionLod(active);
    }
#endif
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

void VulkanViewportAdapter::setViewMode(SoVulkanViewMode mode)
{
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        _vulkanViewer->setViewMode(mode);
    }
#else
    Q_UNUSED(mode);
#endif
}

SoVulkanViewMode VulkanViewportAdapter::getViewMode() const
{
#ifdef FREECAD_USE_VULKAN
    return _vulkanViewer ? _vulkanViewer->getViewMode()
                         : SoVulkanViewMode::RtxModeOff;
#else
    return SoVulkanViewMode::RtxModeOff;
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

void VulkanViewportAdapter::applySurfaceViewportToGL(const QSize& surfaceSize)
{
    // In a non-Vulkan build the whole viewport is a no-op and calling
    // _vulkanViewer->getNativeWidget() here would pull an undefined symbol
    // (QuarterVulkanWidget.cpp is excluded from the build), so the body is
    // compiled only with the Vulkan renderer.
    Q_UNUSED(surfaceSize);
#ifdef FREECAD_USE_VULKAN
    // The hidden GL viewer is the picking/navigation authority, but it is
    // never shown, so QuarterWidget::resizeEvent() resets its render/event
    // manager viewport region to the GL widget's own (typically default
    // 400x400) size whenever the widget is re-laid-out (e.g. a document is
    // created or opened).  SoRayPickAction's normalized coordinates are
    // computed from that region, so a stale 400x400 region makes every pick
    // miss.  The Vulkan surface is the single source of truth, so re-impose
    // its size here.  Called on every surface size change and on any GL
    // widget resize (see eventFilter).
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

    // Event positions reach the hidden GL viewer already scaled to device
    // pixels: EventFilter::trackPointerPosition() runs
    // InputDevice::toDevicePixelPosition(), which multiplies the logical Qt
    // position by the widget's device pixel ratio, and QuarterWidget::
    // resizeEvent() sets the region to dpr * size.  The viewport region must
    // therefore be in the same device-pixel space for SoRayPickAction's
    // normalized coordinates to match the ray; using the logical size would
    // shift hover picking and navigation by the DPI factor.
    SbViewportRegion vp(static_cast<short>(pw), static_cast<short>(ph));
    _viewer->getSoRenderManager()->setViewportRegion(vp);

    // The interaction controller owns the canonical region now: picking and
    // navigation read it instead of the hidden GL viewer's render-manager copy,
    // and the controller pushes it into the (controller-owned) event manager.
    // The render-manager region above is kept only for the GL/IR render path
    // (line widths / point sizes).
    if (auto* controller = _viewer->getInteractionController()) {
        controller->setViewportRegion(vp, static_cast<float>(dpr));
    }

    // The viewport region is in device pixels (dpr * logical), so tell the
    // render manager the real device-pixel ratio.  This propagates to the
    // CoSoDevicePixelRatioElement and the render backend's params
    // (.devicePixelRatio), which the GL and Vulkan backends use to scale
    // logical SoDrawStyle line widths / point sizes into device pixels.
    // Without it the ratio stayed 1.0, so on a fractional-scaling display
    // (e.g. 1.25) lines and points rendered 1/dpr too thin and, for the
    // NaviCube overlay, its edge/axis strokes and dots drifted off the cube.
    _viewer->getSoRenderManager()->setDevicePixelRatio(static_cast<float>(dpr));

    // Keep the hidden GL widget sized to the visible Vulkan container so its
    // own resizeEvent computes the same device-pixel region (rather than the
    // default 400x400).  The swapchain size is NOT used directly: resizing a
    // non-current QStackedWidget page changes the stack's sizeHint, which
    // feeds back into the window and, in turn, the swapchain (this produced
    // an oscillating surface size).  Size the hidden viewer to the container.
    if (glWidget->sizePolicy()
        != QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored)) {
        glWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
    if (glWidget->size() != logical) {
        glWidget->resize(logical);
    }
#endif
}

void VulkanViewportAdapter::onSurfaceSizeChanged(const QSize& surfaceSize)
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    applySurfaceViewportToGL(surfaceSize);

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
    QWidget* container = _vulkanViewer->getNativeWidget();
    if (container && container->width() > 1 && container->height() > 1
        && !_initialVulkanFitDone && !_userCameraMoved) {
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
#else
    Q_UNUSED(surfaceSize);
#endif
}

VulkanViewportAdapter::~VulkanViewportAdapter()
{
    // If this adapter is the controller's active surface, hand it back to the
    // GL viewer before we are destroyed.  The viewer (and its controller) still
    // exist here -- View3DInventor deletes the adapter before _viewer -- so a
    // later controller call would otherwise reach freed memory.
    if (_viewer) {
        auto* controller = _viewer->getInteractionController();
        if (controller && controller->surface() == this) {
            controller->setSurface(_glSurface);
        }
    }
#ifdef FREECAD_USE_VULKAN
    // Drop the GPU pick bridge before the widget goes away: its callback
    // captures this adapter and dereferences _vulkanViewer.
    Gui::GpuPickService::instance().clearPicker(this);
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
        // Stop delivering input to the (soon-dead) GL viewer/controller.
        _vulkanViewer->setEventSink(nullptr);
        _vulkanViewer->setRawEventTarget(nullptr);
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
    // The hidden GL widget's resizeEvent resets its render-manager viewport
    // region to the GL widget's own size (default 400x400 after a document
    // re-layout).  Re-impose the surface size so picking/navigation stay
    // calibrated (see applySurfaceViewportToGL).
    if (_vulkanViewer && _viewer && event->type() == QEvent::Resize) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget == _viewer->getWidget()) {
            applySurfaceViewportToGL(QSize());
        }
    }
#else
    Q_UNUSED(watched);
    Q_UNUSED(event);
#endif
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// InteractionSurface: this adapter acting as the controller's active surface.
//
// Only used while the Vulkan page is current (useVulkanViewport).  The GL
// viewer stays the base surface for everything surface-independent; only
// getGLWidget() and scheduleRedraw() target the visible Vulkan surface, and
// surfaceSetEventManager() must stay on the GL viewer (it owns the manager).
// ---------------------------------------------------------------------------

SoCamera* VulkanViewportAdapter::getCamera() const
{
    return _glSurface ? _glSurface->getCamera() : nullptr;
}

SoNode* VulkanViewportAdapter::getSceneGraph() const
{
    return _glSurface ? _glSurface->getSceneGraph() : nullptr;
}

const SbViewportRegion& VulkanViewportAdapter::getViewportRegion() const
{
    if (_glSurface) {
        return _glSurface->getViewportRegion();
    }
    static const SbViewportRegion empty;
    return empty;
}

SoRenderManager* VulkanViewportAdapter::getSoRenderManager() const
{
    return _glSurface ? _glSurface->getSoRenderManager() : nullptr;
}

SoEventManager* VulkanViewportAdapter::getSoEventManager() const
{
    return _glSurface ? _glSurface->getSoEventManager() : nullptr;
}

SbVec3f VulkanViewportAdapter::getFocalPoint() const
{
    return _glSurface ? _glSurface->getFocalPoint() : SbVec3f();
}

float VulkanViewportAdapter::getPickRadius() const
{
    return _glSurface ? _glSurface->getPickRadius() : 0.0F;
}

QWidget* VulkanViewportAdapter::getGLWidget() const
{
    // Presentation difference: navigation parents its context menu to the
    // surface on screen, which is the Vulkan container, not the hidden GL
    // widget.
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        return _vulkanViewer->getNativeWidget();
    }
#endif
    return _glSurface ? _glSurface->getGLWidget() : nullptr;
}

bool VulkanViewportAdapter::isEditing() const
{
    return _glSurface && _glSurface->isEditing();
}

bool VulkanViewportAdapter::isEditingViewProvider() const
{
    return _glSurface && _glSurface->isEditingViewProvider();
}

bool VulkanViewportAdapter::isSelectionEnabled() const
{
    return _glSurface && _glSurface->isSelectionEnabled();
}

bool VulkanViewportAdapter::isViewing() const
{
    return _glSurface && _glSurface->isViewing();
}

bool VulkanViewportAdapter::isSeekMode() const
{
    return _glSurface && _glSurface->isSeekMode();
}

void VulkanViewportAdapter::setViewing(bool enable)
{
    if (_glSurface) {
        _glSurface->setViewing(enable);
    }
}

void VulkanViewportAdapter::setSeekMode(bool enable)
{
    if (_glSurface) {
        _glSurface->setSeekMode(enable);
    }
}

bool VulkanViewportAdapter::seekToPoint(const SbVec2s& screenpos)
{
    return _glSurface && _glSurface->seekToPoint(screenpos);
}

void VulkanViewportAdapter::seekToPoint(const SbVec3f& scenepos)
{
    if (_glSurface) {
        _glSurface->seekToPoint(scenepos);
    }
}

bool VulkanViewportAdapter::processSoEventBase(const SoEvent* ev)
{
    return _glSurface && _glSurface->processSoEventBase(ev);
}

void VulkanViewportAdapter::interactiveCountInc()
{
    if (_glSurface) {
        _glSurface->interactiveCountInc();
    }
}

void VulkanViewportAdapter::interactiveCountDec()
{
    if (_glSurface) {
        _glSurface->interactiveCountDec();
    }
}

int VulkanViewportAdapter::getInteractiveCount() const
{
    return _glSurface ? _glSurface->getInteractiveCount() : 0;
}

std::shared_ptr<NavigationAnimation> VulkanViewportAdapter::setCameraOrientation(
    const SbRotation& orientation,
    bool moveToCenter
) const
{
    return _glSurface ? _glSurface->setCameraOrientation(orientation, moveToCenter)
                      : std::shared_ptr<NavigationAnimation>();
}

std::shared_ptr<NavigationAnimation> VulkanViewportAdapter::startAnimation(
    const SbRotation& orientation,
    const SbVec3f& rotationCenter,
    const SbVec3f& translation,
    int duration,
    bool wait
) const
{
    return _glSurface ? _glSurface->startAnimation(
               orientation,
               rotationCenter,
               translation,
               duration,
               wait
           )
                      : std::shared_ptr<NavigationAnimation>();
}

void VulkanViewportAdapter::startSpinningAnimation(const SbVec3f& axis, float velocity)
{
    if (_glSurface) {
        _glSurface->startSpinningAnimation(axis, velocity);
    }
}

void VulkanViewportAdapter::viewAll()
{
    if (_glSurface) {
        _glSurface->viewAll();
    }
}

void VulkanViewportAdapter::showRotationCenter(bool show)
{
    if (_glSurface) {
        _glSurface->showRotationCenter(show);
    }
}

void VulkanViewportAdapter::changeRotationCenterPosition(const SbVec3f& newCenter)
{
    if (_glSurface) {
        _glSurface->changeRotationCenterPosition(newCenter);
    }
}

SbVec2s VulkanViewportAdapter::getPointOnViewport(const SbVec3f& point) const
{
    return _glSurface ? _glSurface->getPointOnViewport(point) : SbVec2s();
}

void VulkanViewportAdapter::setCursorRepresentation(int mode)
{
    // The GL viewer applies this to its cursorTarget, which useVulkanViewport()
    // points at the visible Vulkan container.
    if (_glSurface) {
        _glSurface->setCursorRepresentation(mode);
    }
}

void VulkanViewportAdapter::scheduleRedraw()
{
    // Presentation difference: wake the visible Vulkan surface, not the hidden
    // GL render manager.
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        requestVulkanFrame();
        return;
    }
#endif
    if (_glSurface) {
        _glSurface->scheduleRedraw();
    }
}

SoGroup* VulkanViewportAdapter::getObjectGroup() const
{
    return _glSurface ? _glSurface->getObjectGroup() : nullptr;
}

SoSeparator* VulkanViewportAdapter::getForegroundRoot() const
{
    return _glSurface ? _glSurface->getForegroundRoot() : nullptr;
}

void VulkanViewportAdapter::bindMouseSelection(AbstractMouseSelection* selection)
{
    if (_glSurface) {
        _glSurface->bindMouseSelection(selection);
    }
}

bool VulkanViewportAdapter::surfaceNaviCubeEnabled() const
{
    return _glSurface && _glSurface->surfaceNaviCubeEnabled();
}

bool VulkanViewportAdapter::surfaceProcessNaviCubeEvent(const SoEvent* ev)
{
    return _glSurface && _glSurface->surfaceProcessNaviCubeEvent(ev);
}

bool VulkanViewportAdapter::surfaceIsRedirectedToSceneGraph() const
{
    return _glSurface && _glSurface->surfaceIsRedirectedToSceneGraph();
}

void VulkanViewportAdapter::surfaceNotifyCameraMoved()
{
    // Let the GL viewer run its bookkeeping; the adapter's camera sensor is
    // what actually requests the Vulkan frame.
    if (_glSurface) {
        _glSurface->surfaceNotifyCameraMoved();
    }
}

void VulkanViewportAdapter::surfaceSetEventManager(SoEventManager* manager)
{
    // The Coin event manager stays on the GL viewer (the base dispatch
    // authority); never install it on the Vulkan widget.
    if (_glSurface) {
        _glSurface->surfaceSetEventManager(manager);
    }
}
