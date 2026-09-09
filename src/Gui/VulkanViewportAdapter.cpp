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
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoEventManager.h>
#include <Inventor/actions/SoSearchAction.h>
#include <Inventor/nodes/SoAnnotation.h>
#include <Inventor/actions/SoGetMatrixAction.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoDirectionalLight.h>
#include <Inventor/nodes/SoEnvironment.h>
#include <Inventor/nodes/SoPointLight.h>
#include <Inventor/nodes/SoSpotLight.h>
#include <Inventor/SoRenderManager.h>
#include <Inventor/rendering/SoRenderIR.h>
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
    VK_BREADCRUMB("[VKFLOW] ctor enter viewer=%p useRayTracing=%d\n",
                  static_cast<void*>(_viewer), int(useRayTracing));
    _vulkanViewer = new SIM::Coin3D::Quarter::QuarterVulkanWidget(stack, useRayTracing);
    VK_BREADCRUMB("[VKFLOW] ctor QuarterVulkanWidget created ptr=%p\n",
                  static_cast<void*>(_vulkanViewer));
    _vulkanViewer->setSampleCount(View3DInventorViewer::getNumSamples());
    // QVulkanWindow::grab() only converts 8-bit swapchain formats;
    // request B8G8R8A8_UNORM so screenshot tests read exact pixels.
    _vulkanViewer->setPreferredColorFormat(VK_FORMAT_B8G8R8A8_UNORM);
    stack->addWidget(_vulkanViewer);
    VK_BREADCRUMB("[VKFLOW] ctor widget added to stack\n");
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
    VK_BREADCRUMB("[VKFLOW] ctor current widget set to Vulkan viewport\n");
    // The GL widget is the picking/navigation authority; at startup it is
    // already Vulkan-driven, so mark it device-pixel (see useVulkanViewport
    // and applySurfaceViewportToGL for the runtime/switching case).
    if (auto* quarter = dynamic_cast<Quarter::QuarterWidget*>(_viewer->getWidget())) {
        quarter->setVulkanDevicePixels(true);
        VK_BREADCRUMB("[VKFLOW] ctor GL widget marked vulkan-device-pixel\n");
    }
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
    VK_BREADCRUMB("[VKFLOW] ctor connect surfaceSizeChanged<->onSurfaceSizeChanged\n");
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
    VK_BREADCRUMB("[VKFLOW] syncViewer enter glScene=%p glCam=%p\n",
                  static_cast<void*>(rm->getSceneGraph()),
                  static_cast<void*>(rm->getCamera()));
    _vulkanViewer->setSceneGraph(rm->getSceneGraph());
    _vulkanViewer->setOverlaySceneGraph(_viewer->getNaviCubeAnnotation());
    VK_BREADCRUMB("[VKFLOW] syncViewer scene=%p overlay=(navcube=%p) set\n",
                  static_cast<void*>(rm->getSceneGraph()),
                  static_cast<void*>(_viewer->getNaviCubeAnnotation()));
    // The hidden GL viewer's frame loop never runs, so the axis cross
    // overlay nodes are refreshed here for the IR (Vulkan) render path.
    _viewer->updateAxisCrossNodes();
    _vulkanViewer->setDecorationSceneGraph(_viewer->getAxisCrossOverlay());
    VK_BREADCRUMB("[VKFLOW] syncViewer decoration(axis-cross)=%p set\n",
                  static_cast<void*>(_viewer->getAxisCrossOverlay()));
    _vulkanViewer->setCamera(rm->getCamera());
    VK_BREADCRUMB("[VKFLOW] syncViewer camera=%p set\n",
                  static_cast<void*>(rm->getCamera()));
    // The background (solid color + gradient + environment preset) is pushed
    // by pushSettings(), the single source of truth derived from the hidden
    // GL viewer.  syncViewer() only re-seeds scene/camera/overlays here and
    // lets pushSettings() refresh the background, so a background change is
    // pushed in exactly one place.
    pushSettings();
    VK_BREADCRUMB("[VKFLOW] syncViewer pushSettings() done\n");
    // Track the scene/camera we just pushed so subsequent changes on the
    // (possibly new) nodes wake the Vulkan frame (idempotent).
    attachSensors();
    VK_BREADCRUMB("[VKFLOW] syncViewer attachSensors() done\n");
    _vulkanViewer->redraw();
    VK_BREADCRUMB("[VKFLOW] syncViewer redraw() done (init complete)\n");
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
    VK_BREADCRUMB("[VKFLOW] useVulkanViewport(toggle) -> %s\n", vulkan ? "vulkan" : "gl");
    // The GL viewer drives navigation/picking and is the scene-graph authority;
    // before the Vulkan surface is shown again, push its current
    // scene/camera/background back in so the switch does not leave a stale
    // frame on top.
    if (vulkan) {
        syncViewer();
    }
    host->setCurrentWidget(target);
    VK_BREADCRUMB("[VKFLOW] useVulkanViewport setCurrentWidget(%s)\n",
                  vulkan ? "vulkan" : "gl");
    // The GL widget is the picking/navigation authority in every mode, so its
    // event handling must know which pixel space its viewport region is in:
    // device pixels (Vulkan surface active) or logical pixels (classic GL).
    // Set it synchronously here so a switch to Vulkan doesn't leave a stale
    // flag and drop hover/select picking (the surfaceSizeChanged/GL-resize
    // driven applySurfaceViewportToGL may lag or not fire if the surface is
    // already sized).
    if (auto* quarter = dynamic_cast<Quarter::QuarterWidget*>(_viewer->getWidget())) {
        quarter->setVulkanDevicePixels(vulkan);
    }
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
    VK_BREADCRUMB("[VKFLOW] pushSettings enter r=1 e=%d p=%d\n",
                  settings.showEdges ? 1 : 0, settings.showPoints ? 1 : 0);
    // The raster gate is DERIVED here from the single-source settings
    // struct (VulkanViewSettings::rasterOnly()), not passed in separately.
    // In a raster render mode the viewport must never enable path tracing,
    // ray tracing or the denoiser, even when the persisted preferences asked
    // for them -- the mode is the authority and this gate keeps the
    // accumulate/trace path from leaking back into Interactive.  The edge
    // overlay is NOT gated: it renders in every mode (matching the OpenGL
    // viewport, whose shapes always draw their edges), only its color/style
    // comes from the settings.
    const bool raster = settings.rasterOnly();

    // Memoised push: the preferences signal that drives pushSettings() fires
    // repeatedly with identical values (applyVulkanSettings() re-emits
    // vulkanSettingsChanged on every call, not only on change).  Only re-apply
    // to the renderer and emit the [VK-SET] diagnostic when the effective
    // values actually differ; otherwise this is no-op and keeps the log quiet.
    const bool effEdges = settings.showEdges;
    const bool effPoints = raster ? false : settings.showPoints;
    const bool effTess = settings.showTessEdges;

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

    char sig[512];
    std::snprintf(sig, sizeof(sig), "r=%d e=%d p=%d t=%d c=%.3g,%.3g,%.3g,%.3g "
                                    "em=%d pt=%d bo=%d se=%d ms=%d dn=%s ds=%.3g "
                                    "bg=%d %.3g,%.3g,%.3g %.3g,%.3g,%.3g "
                                    "%.3g,%.3g,%.3g",
                  raster ? 1 : 0, effEdges ? 1 : 0, effPoints ? 1 : 0,
                  effTess ? 1 : 0,
                  settings.edgeColor[0], settings.edgeColor[1],
                  settings.edgeColor[2], settings.edgeColor[3],
                  settings.envMap, !raster ? 1 : 0,
                  settings.pathTracingBounces, settings.pathTracingSettleFrames,
                  settings.pathTracingMaxSamples,
                  settings.pathTracingDenoiser.c_str(),
                  settings.pathTracingDenoiserScale,
                  bgGradient ? 1 : 0,
                  bgColor[0], bgColor[1], bgColor[2],
                  bgTop[0], bgTop[1], bgTop[2],
                  bgBottom[0], bgBottom[1], bgBottom[2]);
    const bool changed = (sig != this->_pushedSettingsSig);
    this->_pushedSettingsSig = sig;

    if (changed) {
        if (Base::envFlagEnabled("FC_VULKAN_BACKEND_DEBUG")) {
            Base::Console().message("[VK-SET] pushSettings raster=%d edges=%d points=%d tess=%d "
                                    "edgeColor=(%.2f,%.2f,%.2f,%.2f) pt=%d "
                                    "bounces=%d settle=%d "
                                    "(prefEdges=%d prefPoints=%d)\n",
                                    raster ? 1 : 0, effEdges ? 1 : 0,
                                    effPoints ? 1 : 0, effTess ? 1 : 0,
                                    settings.edgeColor[0], settings.edgeColor[1],
                                    settings.edgeColor[2], settings.edgeColor[3],
                                    !raster ? 1 : 0,
                                    settings.pathTracingBounces,
                                    settings.pathTracingSettleFrames,
                                    settings.showEdges ? 1 : 0,
                                    settings.showPoints ? 1 : 0);
        }
        _vulkanViewer->setWireframeOverlay(effEdges);
        _vulkanViewer->setPointsOverlay(effPoints);
        _vulkanViewer->setTessellationOverlay(effTess);
        _vulkanViewer->setEdgeColor(settings.edgeColor);
        // Background and environment preset are pushed together: both drive
        // the frame's sky/miss radiance (see SoRenderParams::background* and
        // SoRTXRenderBackend::setEnvMap), so updating them in one place keeps
        // raster and ray-traced backgrounds in agreement.
        _vulkanViewer->setBackgroundColor(bgColor);
        _vulkanViewer->setBackgroundGradient(
            bgGradient,
            SbColor4f(bgTop[0], bgTop[1], bgTop[2], 1.0f),
            SbColor4f(bgBottom[0], bgBottom[1], bgBottom[2], 1.0f));
        _vulkanViewer->setEnvMap(settings.envMap);

        _vulkanViewer->setPathTracingEnabled(!raster);
        _vulkanViewer->setPathTracingBounces(settings.pathTracingBounces);
        _vulkanViewer->setPathTracingSettleFrames(settings.pathTracingSettleFrames);
        _vulkanViewer->setPathTracingMaxSamples(settings.pathTracingMaxSamples);
        if (!settings.pathTracingDenoiser.empty()) {
            _vulkanViewer->setPathTracingDenoiser(settings.pathTracingDenoiser);
        }
        _vulkanViewer->setPathTracingDenoiserScale(
            settings.pathTracingDenoiserScale);
        VK_BREADCRUMB("[VKFLOW] pushSettings applied: bg=%d edges=%d points=%d pt=%d\n",
                      bgGradient ? 1 : 0, effEdges ? 1 : 0, effPoints ? 1 : 0,
                      !raster ? 1 : 0);
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

    // GL-authoritative scene lighting -> RT backend.  The IR draw-list
    // lighting capture (SoLightElement::getLights) is unreliable on the
    // retained/replayed path tracer (the captured light count can drop to
    // zero, rendering surfaces at ambient-only/near-black).  Gather the
    // viewer headlight plus any document SoLight nodes and push the eye-space
    // set so the RT backend always has the real lights.
    this->pushSceneLights();
#endif
}
namespace {

// Evaluate \a light's world-space "travel" direction (pointing TOWARD the
// surfaces it lights, i.e. the negated SoDirectionalLight::direction field),
// transformed by the light node's world/scene matrix the same way
// SoRenderIR::fillLightingFromState does.  When the light cannot be located
// in the scene graph it is treated as world-fixed (its raw direction used
// directly), which is the convention for the viewer headlight/backlight.
// The search action and the path it yields are kept in this scope: the path
// is owned by the search action and freed when it goes out of scope, so it
// must not be returned or used outside it.
SbVec3f
lightWorldDirection(SoRenderManager* rm, const SoNode* light)
{
    SbVec3f dir(0.0f, 0.0f, 0.0f);
    if (light->isOfType(SoDirectionalLight::getClassTypeId())) {
        const SbVec3f d =
            static_cast<const SoDirectionalLight*>(light)->direction.getValue();
        dir = SbVec3f(-d[0], -d[1], -d[2]);
    }
    if (rm) {
        if (SoNode* root = rm->getSceneGraph()) {
            SoSearchAction sa;
            sa.setNode(const_cast<SoNode*>(light));
            sa.setInterest(SoSearchAction::FIRST);
            sa.apply(root);
            if (SoPath* path = sa.getPath()) {
                SoGetMatrixAction matrixAction(SbViewportRegion(1, 1));
                matrixAction.apply(path);
                matrixAction.getMatrix().multDirMatrix(dir, dir);
            }
        }
    }
    return dir;
}

} // namespace

void
VulkanViewportAdapter::pushSceneLights()
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }

    // World-space light set, matching the SoRenderIR::fillLightingFromState
    // convention exactly: each directional light's world direction is its
    // negated direction field transformed by the light node's scene matrix.
    // The viewer headlight and backlight sit at the scene root (world-fixed),
    // while the fill light hangs under an SoRotation connected to the camera
    // orientation, so it follows the camera.  The RT shaders consume the
    // fields directly in world space, giving raster/path-tracer parity.
    std::vector<SoLightData> lights;
    lights.reserve(3);
    SbVec3f ambient(0.2f, 0.2f, 0.2f);

    SoRenderManager* rm = _viewer->getSoRenderManager();
    SoNode* root = rm ? rm->getSceneGraph() : nullptr;

    // Scene ambient from the environment node (so a scene lit purely by
    // ambient still reads non-black).
    if (root) {
        SoSearchAction sa;
        sa.setType(SoEnvironment::getClassTypeId());
        sa.setInterest(SoSearchAction::FIRST);
        sa.apply(root);
        if (SoEnvironment* env = static_cast<SoEnvironment*>(
                sa.getPath() ? sa.getPath()->getTail() : nullptr)) {
            const SbColor& ac = env->ambientColor.getValue();
            float ai = env->ambientIntensity.getValue();
            ambient = SbVec3f(ac[0] * ai, ac[1] * ai, ac[2] * ai);
        }
    }

    // Push each enabled directional light with the same world-space
    // convention the raster IR uses (headlight + backlight + fill, matching
    // the GL viewer's three-point lighting).
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
        SbVec3f dir = lightWorldDirection(rm, light);
        if (dir.normalize() == 0.0f) {
            dir = SbVec3f(0.0f, 0.0f, 1.0f);
        }
        l.direction = dir;
        lights.push_back(l);
    }

    _vulkanViewer->setSceneLights(lights, ambient);
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
    VK_BREADCRUMB_SAMPLED(50, "[VKFLOW] requestVulkanFrame (redraw)\n");
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
    VK_BREADCRUMB("[VKFLOW] sceneChangedCB fired\n");
    static_cast<VulkanViewportAdapter*>(data)->requestVulkanFrame();
}

void VulkanViewportAdapter::cameraChangedCB(void* data, SoSensor* /*sensor*/)
{
    VK_BREADCRUMB_SAMPLED(10, "[VKFLOW] cameraChangedCB fired\n");
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
    VK_BREADCRUMB("[VKFLOW] attachSensors sceneSensor->%p cameraSensor->%p\n",
                  _sceneSensor ? static_cast<void*>(_sceneSensor->getAttachedNode()) : nullptr,
                  _cameraSensor ? static_cast<void*>(_cameraSensor->getAttachedNode()) : nullptr);
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
        _vulkanViewer->setViewMode(mode);
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
    // In the Vulkan render modes the Vulkan surface is the display and the
    // single source of truth for size; the hidden GL viewer only drives
    // picking/navigation, so it is re-imposed to the surface size.  But in the
    // classic RasterCoin mode the GL widget itself is the visible page (the
    // Vulkan container is hidden and carries a stale/tiny size), so the GL
    // widget must drive its own render-manager viewport.  Using the Vulkan
    // container there made the main scene render into the tiny stale rect --
    // only the self-sizing NavCube showed -- and resizing the visible GL
    // widget to it squeezed the whole viewport into a small rectangle.
    auto* stack = qobject_cast<QStackedWidget*>(_vulkanViewer->parentWidget());
    const bool vulkanActive = stack && stack->currentWidget() == _vulkanViewer;
    // Tell the GL widget which pixel space its viewport region uses so the event
    // handling in Mouse/EventFilter converts cursor positions in the matching
    // space (live ratio + region normalized for Vulkan; cached ratio + the
    // widget's own logical size for classic GL).
    if (auto* quarter = qobject_cast<Quarter::QuarterWidget*>(glWidget)) {
        quarter->setVulkanDevicePixels(vulkanActive);
    }
    QWidget* source = vulkanActive ? container : glWidget;
    const QSize logical = source->size();
    if (logical.width() <= 0 || logical.height() <= 0) {
        return;
    }
    const qreal dpr = glWidget->devicePixelRatioF();
    const int pw = qMax(1, qRound(logical.width() * dpr));
    const int ph = qMax(1, qRound(logical.height() * dpr));
    const SbVec2s glSize =
        _viewer->getSoRenderManager()->getViewportRegion().getViewportSizePixels();
    VK_BREADCRUMB("[VK-TRACE] surfaceSizeChanged surface=%dx%d "
                  "source=%dx%d container=%dx%d glViewport(before)=%dx%d "
                  "glWidgetSize=%dx%d dpr=%.3f vulkan=%d\n",
                  surfaceSize.width(), surfaceSize.height(),
                  source->width(), source->height(),
                  container->width(), container->height(),
                  glSize[0], glSize[1],
                  glWidget->width(), glWidget->height(), dpr, vulkanActive ? 1 : 0);

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

    // Only in the Vulkan render modes keep the hidden GL widget calibrated to
    // the Vulkan container so its own resizeEvent computes the same device-
    // pixel region (rather than a default 400x400).  The swapchain size is NOT
    // used directly: resizing a non-current QStackedWidget page changes the
    // stack's sizeHint, which feeds back into the window and, in turn, the
    // swapchain (this produced an oscillating surface size).  In RasterCoin
    // mode the GL widget is the visible page and must keep its own layout and
    // size policy -- forcing Ignored and resizing it to the hidden container
    // collapses the whole viewport into the small rectangle.
    if (vulkanActive) {
        if (glWidget->sizePolicy()
            != QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored)) {
            glWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        }
        if (glWidget->size() != logical) {
            glWidget->resize(logical);
        }
    }
#endif
}

void VulkanViewportAdapter::onSurfaceSizeChanged(const QSize& surfaceSize)
{
#ifdef FREECAD_USE_VULKAN
    if (!_vulkanViewer || !_viewer) {
        return;
    }
    VK_BREADCRUMB("[VKFLOW] onSurfaceSizeChanged size=%dx%d\n",
                  surfaceSize.width(), surfaceSize.height());
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
        && !_initialVulkanFitDone) {
        _initialVulkanFitDone = true;
        const bool animation = _viewer->isAnimationEnabled();
        if (animation) {
            _viewer->setAnimationEnabled(false);
        }
        VK_BREADCRUMB("[VKFLOW] onSurfaceSizeChanged first stable size -> viewAll()\n");
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
#endif
    return QObject::eventFilter(watched, event);
}
