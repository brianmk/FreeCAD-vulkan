// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "VulkanViewportAdapter.h"

#include "Quarter/QuarterVulkanWidget.h"
#include "Quarter/QuarterWidget.h"
#include "Application.h"
#include "DisplayLuminance.h"
#include "InteractionController.h"
#include "VulkanDebugEnv.h"
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
#include <Inventor/rendering/vulkan/SoVulkanViewSettings.h>
#include <Inventor/sensors/SoNodeSensor.h>
#include <Inventor/sensors/SoSensor.h>

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QWidget>

#include <cmath>

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
    // Present mode (V-Sync) is a window/swapchain property too: QVulkanWindow
    // uses it when it creates the swapchain on first expose, so the preference
    // is read here rather than in pushSettings() (which must not recreate the
    // swapchain -- that would loop back through swapChainChanged).
    _vulkanViewer->setPresentMode(_viewer->getVulkanViewSettings().presentMode);
    if (_viewer->getVulkanViewSettings().hdrEnabled) {
        _vulkanViewer->setHdrOutputEnabled(true);
        // The output's HDR state gates the HDR encode (see pushSettings) and
        // supplies the reference white.  The probe is asynchronous, so
        // changed() re-pushes the settings once the compositor answers, and
        // again after an output reconfiguration.
        DisplayLuminance& luminance = DisplayLuminance::instance();
        QScreen* screen = _vulkanViewer->screen();
        luminance.query(screen ? screen : QGuiApplication::primaryScreen());
        connect(&luminance, &DisplayLuminance::changed, this, [this] {
            pushSettings();
        });
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
    // touch/context-menu events are not translated by the Coin devices, so hand
    // them to the controller, which owns the event/surface abstraction (and the
    // raw-event target) rather than the adapter reaching for the hidden GL
    // widget directly.
    _vulkanViewer->setRawEventSink([this](QEvent* ev) {
        auto* controller = _viewer ? _viewer->getInteractionController() : nullptr;
        return controller && controller->processRawEvent(ev);
    });
    _vulkanViewer->setEventSink([this](const SoEvent* ev) {
        auto* controller = _viewer ? _viewer->getInteractionController() : nullptr;
        return controller && controller->processSoEvent(ev);
    });
    // Navigation's cursor shapes are routed to the visible surface by
    // View3DInventorViewer::setCursorTarget() (set in useVulkanViewport), so
    // no cursor mirroring is needed.
    // The visible Vulkan surface is the single viewport authority: its size is
    // written into the viewer's neutral view state (and the controller) so
    // navigation (aspect/near-far) and ray picking use the visible surface size
    // rather than a stale default.  Nothing is written back into the hidden GL
    // render manager.
    connect(_vulkanViewer,
            &SIM::Coin3D::Quarter::QuarterVulkanWidget::surfaceSizeChanged,
            this, &VulkanViewportAdapter::onSurfaceSizeChanged);
    // The HDR encode must match the swapchain the surface actually came up on.
    // pushSettings() derives vs.hdrOutput from isHdrOutputActive(), which is
    // only valid once the swapchain exists; re-push when it is (re)created.
    // A view opened in Coin/GL mode and later switched to Vulkan otherwise
    // keeps hdrOutput=false as computed before the surface existed, and renders
    // SDR sRGB code values into the FP16 extended-linear (scRGB) swapchain --
    // a washed-out image.
    connect(_vulkanViewer,
            &SIM::Coin3D::Quarter::QuarterVulkanWidget::swapChainChanged,
            this, [this] { pushSettings(); });
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
    ViewState* state = _viewer->getViewState();
    if (!state) {
        return;
    }
    _vulkanViewer->setSceneGraph(state->sceneRoot());
    _vulkanViewer->setOverlaySceneGraph(_viewer->getNaviCubeAnnotation());
    // The ground grid lives on the view state's per-frame decoration root (see
    // View3DInventorViewer::setGroundPlane), so no backend-specific scene surgery
    // is needed to keep it camera-coupled on the retained Vulkan path.
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
    ViewState* state = _viewer->getViewState();
    if (!state) {
        return;
    }
    _vulkanViewer->setCamera(state->camera());
    _viewer->updateAxisCrossNodes();
    // Per-frame decoration scene: axis cross + the camera-coupled ground grid.
    // The grid must live here (not the retained main scene) so it re-records
    // every frame and tracks the view volume on zoom/rotate.
    _vulkanViewer->setDecorationSceneGraph(_viewer->getDecorationRoot());
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
    // visible Vulkan surface is the single viewport authority and feeds the
    // neutral view state (device pixels), so tell the event/DPR conversion to
    // normalize cursor positions against that region (effectiveWindowSize)
    // instead of the widget's own size.  Without this the Y-flip used the stale
    // hidden-widget height and hover/click picks landed far off the cursor.  In
    // the classic GL page the region tracks the visible widget, so the flag is
    // cleared and the cached logical size is used (upstream behavior).
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
    // The neutral view state (fed by the Vulkan surface while it is visible)
    // holds the camera/scene the controller drives; before the Vulkan surface
    // is shown again, push its current scene/camera/background in so the switch
    // does not leave a stale frame on top.
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
                // Re-impose the visible surface size as the viewport authority
                // now instead of waiting for the first frame's size
                // notification, so a freshly created view is pickable
                // immediately.
                if (QWidget* c = _vulkanViewer->getNativeWidget()) {
                    updateViewportAuthority(c->size());
                }
                _vulkanViewer->redraw();
            });
            return;
        }
    }
    host->setCurrentWidget(target);
    if (vulkan) {
        if (QWidget* c = _vulkanViewer->getNativeWidget()) {
            updateViewportAuthority(c->size());
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
    // The visible surface is the viewport authority; re-impose its size on the
    // neutral view state (and the controller) before re-pushing the scene.
    // Without this a freshly created view can keep picking against a stale
    // region until the render mode is re-applied.
    if (QWidget* container = _vulkanViewer->getNativeWidget()) {
        updateViewportAuthority(container->size());
    }
    // SoRayPickAction derives its ray depth range from the shared camera's
    // near/far planes.  A GL render auto-fits those planes to the scene, but
    // the display-only Vulkan path never renders through the GL viewer, so on
    // a scene that appeared after the camera was last framed (e.g. the origin
    // planes the sketch attachment editor enlarges over a brand-new empty
    // document) the planes fall outside [near, far] and hover/click picks miss
    // until a render-mode round-trip runs the GL auto-clip.  Refresh the planes
    // here, from the same scene bounding box the GL render would use.
    _viewer->refreshCameraClipping();
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
    // The model feature-edge overlay is a display option in every Vulkan mode:
    // the raster main pass skips the line commands and the ray-tracing
    // composite skips its line residue when it is off (see
    // SoVulkanRenderBackend::setEdgeOverlayVisible), so it is NOT gated by the
    // raster/ray-traced split.  The point-marker overlay remains raster-only
    // (the RTX path rasterizes no point residue).
    const bool effEdgeOverlay = settings.edgeOverlay;
    const bool effPoints = raster ? settings.showPoints : false;

    // Background is a single view of truth owned by the neutral view state
    // (solid colour + optional gradient) and pushed in one place along with the
    // environment preset.  syncViewer() no longer sets the background directly;
    // it only re-seeds scene/camera/overlays and lets pushSettings() refresh the
    // background, so there is one push path and the solid/gradient/env state
    // cannot drift between the two callers.  The viewer writes the state when it
    // sets the GL background, so this no longer reads the GL render manager.
    ViewState* state = _viewer->getViewState();
    const SbColor4f bgColor = state ? state->backgroundColor() : SbColor4f(0, 0, 0, 1);
    const bool bgGradient = state && state->hasBackgroundGradient();
    const SbColor from = state ? state->backgroundTop() : SbColor();
    const SbColor to = state ? state->backgroundBottom() : SbColor();
    const float bgTop[3] = {from[0], from[1], from[2]};
    const float bgBottom[3] = {to[0], to[1], to[2]};

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
    // The raster triangle-as-lines debug overlay is no longer driven by the
    // status-bar button (that toggles the real model edges via edgeOverlay);
    // it is forced only through the renderer's FC_VULKAN_WIREFRAME env hook.
    vs.pointsOverlay = effPoints;
    vs.edgeOverlay = effEdgeOverlay;
    vs.edgeColor = settings.edgeColor;
    // scRGB output is enabled whenever the preference asked for it AND the
    // window actually came up on the FP16 extended-linear swapchain (it chooses
    // the format before the settings can be pushed).  It is deliberately NOT
    // gated on the output currently being in HDR: an extended-linear surface is
    // correctly anchored by the compositor on an SDR output too, whereas
    // turning the encode off while the FP16 surface is live would write sRGB
    // code values into a surface the compositor reads as linear.  The detected
    // output state is logged below for diagnostics only.
    const DisplayLuminance& luminance = DisplayLuminance::instance();
    vs.hdrOutput = settings.hdrEnabled && _vulkanViewer->isHdrOutputActive();
    // scRGB white gain (1.0 = reference white) and the optional ray-traced
    // highlight rolloff (see VulkanViewSettings).
    vs.hdrExposure = settings.hdrExposure;
    vs.hdrToneMap = settings.hdrToneMap;

    if (VkDebug::backendDebug()) {
        Base::Console().message(
            "[VK-SET] pushSettings raster={} edgeOverlay={} points={} "
            "edgeColor=({:.2f},{:.2f},{:.2f},{:.2f}) pt={} bounces={} settle={} "
            "hdr={} hdrExposure={:.4f} hdrToneMap={} "
            "outHdr={} refNits={:.0f} maxNits={:.0f} "
            "(prefEdgeOverlay={} prefPoints={})\n",
            raster ? 1 : 0, effEdgeOverlay ? 1 : 0, effPoints ? 1 : 0,
            settings.edgeColor[0], settings.edgeColor[1],
            settings.edgeColor[2], settings.edgeColor[3], !raster ? 1 : 0,
            settings.pathTracingBounces, settings.pathTracingSettleFrames,
            vs.hdrOutput ? 1 : 0, vs.hdrExposure, vs.hdrToneMap,
            luminance.isHdrOutput() ? 1 : 0,
            luminance.referenceWhiteNits(), luminance.maxNits(),
            settings.edgeOverlay ? 1 : 0, settings.showPoints ? 1 : 0);
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

    // The world-space, camera-anchored light set is owned by the neutral view
    // state.  ViewState::sceneLights() performs the eye->world derivation that
    // used to be gathered here from the GL viewer's three lights and
    // environment, so the adapter no longer reaches back through the viewer.
    ViewState* state = _viewer->getViewState();
    if (!state) {
        return;
    }
    SoLightingData lighting = state->sceneLights();

#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    if (VkDebug::lightTrace()) {
        static int _n = 0;
        if (_n++ < 400) {
            SoCamera* cam = state->camera();
            const SbRotation camRot =
                cam ? cam->orientation.getValue() : SbRotation();
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
#endif

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
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    if (VkDebug::lightTrace()) {
        static int _n = 0;
        if (_n++ < 400) {
            fprintf(stderr, "[LTRACE] cameraChangedCB n=%d\n", _n);
        }
    }
#endif
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
    ViewState* state = _viewer->getViewState();
    if (!state) {
        return;
    }
    // Mirror Coin's SoRenderManager: a node sensor on the scene root redraws on
    // any geometry change (Sketcher edits, selection bake, recompute), and one
    // on the camera node redraws on any pose change (interactive navigation,
    // the navcube / view-home animation ticking the camera on a timer, and any
    // programmatic setCameraOrientation).  A node sensor is a node auditor, so
    // it fires on every field write of the tracked node.
    SoNode* root = state->sceneRoot();
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
    SoCamera* camera = state->camera();
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

void VulkanViewportAdapter::updateViewportAuthority(const QSize& surfaceSize)
{
    // In a non-Vulkan build the whole viewport is a no-op and calling
    // _vulkanViewer->getNativeWidget() here would pull an undefined symbol
    // (QuarterVulkanWidget.cpp is excluded from the build), so the body is
    // compiled only with the Vulkan renderer.
    Q_UNUSED(surfaceSize);
#ifdef FREECAD_USE_VULKAN
    if (!_viewer || !_vulkanViewer) {
        return;
    }
    ViewState* state = _viewer->getViewState();
    if (!state) {
        return;
    }
    // The Vulkan surface is the single viewport authority, but this must not
    // run while the classic Coin/GL page is the visible one: the Vulkan
    // container is then hidden and still has its stale pre-expose default
    // (100x30).  In that case the GL widget is sized by the layout and the GL
    // resize path feeds the view state.
    if (!_wantVulkanViewport) {
        return;
    }
    QWidget* container = _vulkanViewer->getNativeWidget();
    if (!container) {
        return;
    }
    const QSize logical = container->size();
    if (logical.width() <= 0 || logical.height() <= 0) {
        return;
    }
    const qreal dpr = container->devicePixelRatioF();
    const int pw = qMax(1, qRound(logical.width() * dpr));
    const int ph = qMax(1, qRound(logical.height() * dpr));
    const SbVec2s before = state->viewportRegion().getViewportSizePixels();
    VK_BREADCRUMB("[VK-TRACE] surfaceSizeChanged surface=%dx%d "
                  "container=%dx%d logical=%dx%d viewport(before)=%dx%d dpr=%.3f\n",
                  surfaceSize.width(), surfaceSize.height(),
                  container->width(), container->height(),
                  logical.width(), logical.height(),
                  before[0], before[1], dpr);

    // Event positions reach the viewer already scaled to device pixels:
    // EventFilter::trackPointerPosition() runs InputDevice::toDevicePixelPosition(),
    // which multiplies the logical Qt position by the widget's device pixel
    // ratio, and QuarterWidget::resizeEvent() sets the region to dpr * size.
    // The viewport region must therefore be in the same device-pixel space for
    // SoRayPickAction's normalized coordinates to match the ray; using the
    // logical size would shift hover picking and navigation by the DPI factor.
    SbViewportRegion vp(static_cast<short>(pw), static_cast<short>(ph));
    state->setViewportRegion(vp);
    // The viewport region is in device pixels (dpr * logical), so record the
    // real device-pixel ratio.  This reaches the render backends and scales
    // logical SoDrawStyle line widths / point sizes into device pixels.
    state->setDevicePixelRatio(static_cast<float>(dpr));
    // Mirror the region for the InteractionHost contract (getViewportRegion()).
    // The neutral ViewState is the authority; the hidden GL render manager is
    // deliberately left untouched (the surface is the single viewport source).
    _surfaceViewport = vp;

    // The interaction controller owns the canonical region for picking and
    // navigation and pushes it into the (controller-owned) event manager, so
    // keep it in step with the visible surface.
    if (auto* controller = _viewer->getInteractionController()) {
        controller->setViewportRegion(vp, static_cast<float>(dpr));
    }
#endif
}

void VulkanViewportAdapter::onSurfaceSizeChanged(const QSize& surfaceSize)
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    updateViewportAuthority(surfaceSize);

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
        _vulkanViewer->setRawEventSink(nullptr);
        QWidget* host = _vulkanViewer->parentWidget();
        if (auto* stack = qobject_cast<QStackedWidget*>(host)) {
            stack->removeWidget(_vulkanViewer);
        }
        delete _vulkanViewer;
        _vulkanViewer = nullptr;
    }
#endif
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
    // The Vulkan widget shares the view's camera node (syncViewer() points it
    // at the GL viewer's render-manager camera), so answer from the surface
    // actually on screen rather than delegating to the hidden GL viewer.
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        if (SoCamera* cam = _vulkanViewer->getCamera()) {
            return cam;
        }
    }
#endif
    return _glSurface ? _glSurface->getCamera() : nullptr;
}

SoNode* VulkanViewportAdapter::getSceneGraph() const
{
    // Same as getCamera(): the Vulkan widget renders the shared scene-graph
    // root (syncViewer() sets it from the GL viewer's render manager).
#ifdef FREECAD_USE_VULKAN
    if (_vulkanViewer) {
        if (SoNode* root = _vulkanViewer->getSceneGraph()) {
            return root;
        }
    }
#endif
    return _glSurface ? _glSurface->getSceneGraph() : nullptr;
}

const SbViewportRegion& VulkanViewportAdapter::getViewportRegion() const
{
    // The visible surface is the source of truth for the viewport region; it
    // is captured in applySurfaceViewportToGL().  Fall back to the hidden GL
    // viewer only before the surface has reported a real size.
    if (_surfaceViewport.getViewportSizePixels()[0] > 0
        && _surfaceViewport.getViewportSizePixels()[1] > 0) {
        return _surfaceViewport;
    }
    if (_glSurface) {
        return _glSurface->getViewportRegion();
    }
    static const SbViewportRegion empty;
    return empty;
}

SoEventManager* VulkanViewportAdapter::getSoEventManager() const
{
    // The Coin event manager is a view/controller authority, not surface
    // presentation: the controller owns it and installs it on the GL viewer, so
    // the adapter reports that shared instance rather than owning a second one.
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

QWidget* VulkanViewportAdapter::surfaceRawEventTarget() const
{
    // Tablet/touch/context-menu events are still owned by the base GL surface
    // (FreeCAD's gesture devices live there), so delegate the lookup instead of
    // naming the hidden GL widget here.
    return _glSurface ? _glSurface->surfaceRawEventTarget() : nullptr;
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
