// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QObject>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/rendering/vulkan/SoVulkanViewMode.h>

#include "InteractionSurface.h"

#include <memory>
#include <string>

class QStackedWidget;
class QTimer;
class SoNodeSensor;
class SoSensor;

namespace SIM::Coin3D::Quarter { class QuarterVulkanWidget; }

namespace Gui
{

class View3DInventorViewer;

/** Owns the Vulkan viewport integration of a 3D view.
 *
 *  The Vulkan widget is the visible display surface and, while its page is
 *  current, this adapter is the `InteractionController`'s active
 *  `InteractionSurface`: it answers the camera/scene/viewport queries and the
 *  presentation hooks (redraw, cursor target, native widget) from the Vulkan
 *  side, so navigation and picking no longer need the hidden OpenGL viewer's
 *  render manager.  The controller's event manager stays installed on the GL
 *  viewer (the base dispatch authority), so input delivered by the Vulkan
 *  widget runs through the same controller; preference handling and the
 *  view/document state that is not surface presentation (editing/selection
 *  state, seek/viewing mode, object/foreground roots, NaviCube) still live on
 *  the GL viewer.  The adapter keeps the two sides in sync: it pushes scene-
 *  graph/camera/background state into the Vulkan widget and keeps the surface
 *  viewport region calibrated -- the visible Vulkan surface is the single
 *  viewport authority, its size is written into the viewer's neutral
 *  ViewState, never back into the hidden GL render manager.  All wiring is
 *  done in the constructor; the connections use this object as context, so
 *  they are torn down automatically when the view is destroyed.
 *
 *  Like Coin's SoRenderManager (which watches the scene with a root
 *  SoNodeSensor whose callback calls scheduleRedraw()), the adapter installs
 *  change sensors on the camera node and the scene graph so that any change
 *  -- interactive navigation, a navcube/view-home animation tick, a program-
 *  matic camera move, a geometry/sketcher edit -- wakes exactly one coalesced
 *  Vulkan frame.  The hidden GL viewer's frame loop never runs and the
 *  display-only Vulkan widget owns no sensors of its own, so without these a
 *  change would leave the surface frozen (most visibly during a camera
 *  animation, which mutates the camera on a timer with no SoEvent).
 *
 *  Only compiled and used when FREECAD_USE_VULKAN is enabled; every method
 *  is a no-op otherwise.
 */
class VulkanViewportAdapter : public QObject, public InteractionSurface
{
    Q_OBJECT

public:
    VulkanViewportAdapter(QStackedWidget* stack,
                          View3DInventorViewer* viewer,
                          QObject* parent);

    /// Stop the Vulkan widget and detach it from the viewer before the viewer
    /// (and the document scene graph) is destroyed.  Without this the
    /// QuarterVulkanWidget -- a QObject child of the stack that outlives this
    /// adapter -- keeps wiring to the freed viewer, tearing down the scene
    /// twice and SIGSEGVing in the Vulkan instance teardown on document close.
    ~VulkanViewportAdapter() override;

    /// Push scene graph, camera, background and overlays to the Vulkan
    /// widget (no-op without one).
    void syncViewer();

    /// Push the viewer-owned Vulkan display options to the Vulkan widget.
    void pushSettings();

    /// Request a frame from the Vulkan widget (no-op without one).  Used to
    /// surface scene-graph mutations -- e.g. selection/preselection highlight
    /// -- that do not go through a full syncViewer().
    void redraw();

    /// Mark that the user has interactively moved the camera.  The one-time
    /// initial re-fit (which corrects a viewAll() that ran before the surface
    /// had a real size) is skipped afterwards so it can never snap the camera
    /// back while the user is navigating.
    void noteUserCameraMoved();

    void setPathTracingEnabled(bool enabled);
    void setPathTracingStart(bool start);
    bool isPathTracingEnabled() const;
    bool isPathTracingActive() const;
    /// Whether the Vulkan device advertises the hardware ray-tracing extension
    /// set (VK_KHR_acceleration_structure / ray_tracing_pipeline / ray_query).
    /// Only trustworthy once isRayTracingProbed() is true.  Mirrors
    /// QuarterVulkanWidget::isRayTracingAvailable.
    bool isRayTracingAvailable() const;
    /// Whether the renderer has probed the device and settled ray-tracing
    /// availability.  Mirrors QuarterVulkanWidget::isRayTracingProbed.
    bool isRayTracingProbed() const;
    /// Show the Vulkan viewport (\a vulkan = true) or the classic Coin/OpenGL
    /// viewer (\a vulkan = false) in the view's stacked widget.  Only has an
    /// effect when a Vulkan renderer is active; re-syncs the Vulkan surface
    /// (scene/camera/background) before it is brought back on top.
    void useVulkanViewport(bool vulkan);

    /// Re-impose the visible surface size as the neutral view state's viewport
    /// authority and re-push scene, camera and background.  A view created for
    /// a brand-new document can hold a stale viewport region or a stale scene
    /// binding until the render mode is re-applied -- the same effect a manual
    /// renderer switch has -- which leaves hover preselection dead until then.
    /// Idempotent; safe to call whenever the layout or scene may have changed.
    void resyncViewport();

Q_SIGNALS:
    /// Re-emitted from QuarterVulkanWidget::rayTracingUnavailable: a
    /// path-tracing request was dropped because the ray-tracing backend could
    /// not be brought up on this device.  The view should fall back to a
    /// raster render mode.
    void rayTracingUnavailable();

public:
    /// Set the ray-traced view mode.  Mirrors QuarterVulkanWidget::setViewMode.
    void setViewMode(SoVulkanViewMode mode);
    /// Set the "cubemap" environment preset (-1 = viewport background).
    /// Mirrors QuarterVulkanWidget::setEnvMap.
    void setEnvMap(int index);

    /// Ordinal of the last presented frame (see
    /// QuarterVulkanWidget::getRenderFrameCount).  0 when there is no Vulkan
    /// widget yet.  Exposed so scripts/probes can key frame dumps and backend
    /// traces (which carry the same ordinal) to a single monotonic value.
    uint32_t getRenderFrameCount() const;

    /// Force a single Vulkan frame regardless of whether the viewport is
    /// converged-idle.  Used by scripted probes after a scene/camera edit:
    /// the demand-driven widget only re-renders on redraw() or refining, and
    /// the harness's doc.recompute()/updateGui() bypasses the normal
    /// Application::onUpdate() route that would otherwise request one.
    void requestVulkanRender();

    /// Request one Vulkan frame from the widget (coalesced by Qt).  This is the
    /// single wake-up used by the change sensors and by interactive camera
    /// moves: it re-derives the camera-anchored scene lights before redrawing,
    /// so a navigation that only mutates the camera node still updates the
    /// lighting (a bare redraw() would reuse the previous frame's lights).
    void requestVulkanFrame();

    //! @name InteractionSurface (active only while the Vulkan page is current)
    //!
    //! The adapter is the genuine surface for navigation: camera, scene graph
    //! and viewport region are answered from the Vulkan widget (which shares
    //! the same camera/scene nodes as the view) and from this surface's own
    //! region, so navigation and picking no longer need a GL render manager.
    //! Only view-state queries that are not surface presentation -- editing and
    //! selection state, seek/viewing mode, object/foreground roots, NaviCube
    //! and mouse-selection binding -- still forward to the GL viewer, which owns
    //! that document/view state.  `getGLWidget()`/`scheduleRedraw()` wake the
    //! visible Vulkan surface, and `surfaceSetEventManager()` stays on the GL
    //! viewer, where the controller's event manager is installed.
    //@{
    SoCamera* getCamera() const override;
    SoNode* getSceneGraph() const override;
    const SbViewportRegion& getViewportRegion() const override;
    SoEventManager* getSoEventManager() const override;
    SbVec3f getFocalPoint() const override;
    float getPickRadius() const override;
    QWidget* getGLWidget() const override;

    bool isEditing() const override;
    bool isEditingViewProvider() const override;
    bool isSelectionEnabled() const override;
    bool isViewing() const override;
    bool isSeekMode() const override;
    void setViewing(bool enable) override;
    void setSeekMode(bool enable) override;
    bool seekToPoint(const SbVec2s& screenpos) override;
    void seekToPoint(const SbVec3f& scenepos) override;

    bool processSoEventBase(const SoEvent* ev) override;
    void interactiveCountInc() override;
    void interactiveCountDec() override;
    int getInteractiveCount() const override;

    std::shared_ptr<NavigationAnimation> setCameraOrientation(
        const SbRotation& orientation,
        bool moveToCenter = false
    ) const override;
    std::shared_ptr<NavigationAnimation> startAnimation(
        const SbRotation& orientation,
        const SbVec3f& rotationCenter,
        const SbVec3f& translation,
        int duration = -1,
        bool wait = false
    ) const override;
    void startSpinningAnimation(const SbVec3f& axis, float velocity) override;
    void viewAll() override;
    void showRotationCenter(bool show) override;
    void changeRotationCenterPosition(const SbVec3f& newCenter) override;
    SbVec2s getPointOnViewport(const SbVec3f&) const override;

    void setCursorRepresentation(int mode) override;
    void scheduleRedraw() override;
    SoGroup* getObjectGroup() const override;
    SoSeparator* getForegroundRoot() const override;
    void bindMouseSelection(AbstractMouseSelection* selection) override;

    bool surfaceNaviCubeEnabled() const override;
    bool surfaceProcessNaviCubeEvent(const SoEvent* ev) override;
    bool surfaceIsRedirectedToSceneGraph() const override;
    QWidget* surfaceRawEventTarget() const override;
    void surfaceNotifyCameraMoved() override;
    void surfaceSetEventManager(SoEventManager* manager) override;
    //@}

private:
    void onSurfaceSizeChanged(const QSize& surfaceSize);

    /// Re-impose the visible Vulkan surface size as the neutral view state's
    /// viewport region and device pixel ratio (device pixels) and hand the same
    /// region to the interaction controller so picking/navigation normalize
    /// against the visible surface.  The Vulkan surface is the single viewport
    /// authority; nothing is written back into the hidden GL render manager.
    void updateViewportAuthority(const QSize& surfaceSize);

    /// (Re)attach the scene-graph and camera change sensors to the view
    /// state's scene root and camera.  Safe to call repeatedly; re-attaches
    /// only when the tracked nodes changed.
    void attachSensors();

    /// Re-point the Vulkan widget at the viewer's current camera and axis-cross
    /// decoration nodes and (re)attach the change sensors.  Shared by the
    /// camera-changed fast path and the full syncViewer() resync so the two
    /// stay in step.
    void resyncCameraAndDecorations();

    /// Gather the GL viewer's authoritative scene lighting (headlight,
    /// backlight, fill light) and push it to the Vulkan backends.  The IR
    /// draw-list lighting capture can drop to zero on the retained/replayed
    /// path tracer, so this provides a reliable light source to keep the
    /// scene lit.  The viewer lights are camera-anchored (Coin GL applies
    /// them in eye space), so their directions are re-derived from the
    /// current camera orientation on every call to follow the view.
    void pushSceneLights();

    /// Engage interaction LOD after a camera change and (re)arm the idle
    /// timer that disengages it once the camera has been still for a short
    /// window.  Driven from the camera sensor, so it covers an interactive
    /// drag, a seek animation and the navcube/view-home animation alike
    /// without depending on the GL interaction callbacks (which never fire
    /// for the hidden display-only GL viewer).  Honours the
    /// VulkanInteractionLod preference.
    void noteCameraMoved();

    /// Forward the interaction-LOD state to the Vulkan widget (only on a
    /// change).  A no-op without a Vulkan widget.
    void setInteractionLod(bool active);

    /// SoNodeSensor callbacks (Coin's auditor mechanism fires these on any
    /// field write / structural change of the tracked node).
    static void sceneChangedCB(void* data, SoSensor* sensor);
    static void cameraChangedCB(void* data, SoSensor* sensor);

    View3DInventorViewer* _viewer = nullptr;
    //! The controller's base surface: the GL viewer, which owns the
    //! camera/scene/event manager.  When this adapter is the controller's
    //! active surface it forwards every non-presentation call here.
    InteractionSurface* _glSurface = nullptr;
    SIM::Coin3D::Quarter::QuarterVulkanWidget* _vulkanViewer = nullptr;
    //! Canonical surface viewport region (device pixels), reported by
    //! applySurfaceViewportToGL() from the visible Vulkan surface size.  This
    //! is the region the InteractionHost contract exposes, so navigation reads
    //! it without reaching through the hidden GL viewer's render manager.
    SbViewportRegion _surfaceViewport;
    //! True once the Vulkan page has been made current at least once.  The
    //! first activation is deferred one event-loop turn so the one-time Vulkan
    //! device creation it triggers is not paid on the document-open critical
    //! path.  A view that never activates the Vulkan viewport (RasterCoin mode)
    //! therefore never creates a device at all.
    bool _vulkanActivated = false;
    //! Whether the Vulkan viewport is currently the desired page.  Guards the
    //! deferred first activation against a switch back to RasterCoin before the
    //! queued show runs.
    bool _wantVulkanViewport = false;
    bool _initialVulkanFitDone = false;
    //! Set once the user navigates the camera; suppresses the initial re-fit.
    bool _userCameraMoved = false;
    bool _pathTracingRtMismatchWarned = false;
    // No memoised settings signature: the render manager now diffs the whole
    // settings blob (SoVulkanRenderManager::setViewSettings), so re-pushing
    // identical values from the repeatedly-firing preferences signal is a
    // cheap no-op there.
    std::unique_ptr<SoNodeSensor> _sceneSensor;
    std::unique_ptr<SoNodeSensor> _cameraSensor;
    //! Single-shot idle timer: armed on every camera change while interaction
    //! LOD is engaged, it disengages the LOD once the camera has been still
    //! for its interval.
    QTimer* _interactionTimer = nullptr;
    //! Current interaction-LOD state forwarded to the Vulkan widget.
    bool _interactionLod = false;
};

}  // namespace Gui
