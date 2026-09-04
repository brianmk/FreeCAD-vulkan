// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QObject>

#include <memory>

class QStackedWidget;
class SoNodeSensor;
class SoSensor;

namespace SIM::Coin3D::Quarter { class QuarterVulkanWidget; }

namespace Gui
{

class View3DInventorViewer;

/** Owns the Vulkan viewport integration of a 3D view.
 *
 *  The Vulkan widget is display-only: navigation, picking and preference
 *  handling stay on the (hidden) OpenGL viewer.  This adapter keeps the two
 *  sides in sync: it pushes scene-graph/camera/background state into the
 *  Vulkan widget, forwards its input events to the GL viewer, mirrors the
 *  GL viewer's cursor shape onto the visible Vulkan surface and keeps the
 *  GL viewport region in sync with the Vulkan swapchain size.  All wiring
 *  is done in the constructor; the connections use this object as context,
 *  so they are torn down automatically when the view is destroyed.
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
class VulkanViewportAdapter : public QObject
{
    Q_OBJECT

public:
    VulkanViewportAdapter(QStackedWidget* stack,
                          View3DInventorViewer* viewer,
                          bool useRayTracing,
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

Q_SIGNALS:
    /// Re-emitted from QuarterVulkanWidget::rayTracingUnavailable: a
    /// path-tracing request was dropped because the ray-tracing backend could
    /// not be brought up on this device.  The view should fall back to a
    /// raster render mode.
    void rayTracingUnavailable();

public:
    /// Set the ray-traced view mode (0 = Interactive/raster, 1 = Ambient
    /// Occlusion, 2 = Path Tracing).  Mirrors QuarterVulkanWidget::setViewMode.
    void setViewMode(int mode);
    /// Current ray-traced view mode (see setViewMode).
    int getViewMode() const;
    /// Set the "cubemap" environment preset (-1 = viewport background).
    /// Mirrors QuarterVulkanWidget::setEnvMap.
    void setEnvMap(int index);
    /// Current environment/cubemap preset index (see setEnvMap).
    int getEnvMap() const;

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

private:
    void onSurfaceSizeChanged(const QSize& surfaceSize);
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// Request one Vulkan frame from the widget (coalesced by Qt).  This is the
    /// single wake-up used by the change sensors below.
    void requestVulkanFrame();
    /// (Re)attach the scene-graph and camera change sensors to the viewer's
    /// current render manager.  Safe to call repeatedly; re-attaches only when
    /// the tracked nodes changed.
    void attachSensors();

    /// SoNodeSensor callbacks (Coin's auditor mechanism fires these on any
    /// field write / structural change of the tracked node).
    static void sceneChangedCB(void* data, SoSensor* sensor);
    static void cameraChangedCB(void* data, SoSensor* sensor);

    View3DInventorViewer* _viewer = nullptr;
    SIM::Coin3D::Quarter::QuarterVulkanWidget* _vulkanViewer = nullptr;
    bool _initialVulkanFitDone = false;
    bool _pathTracingRtMismatchWarned = false;
    std::unique_ptr<SoNodeSensor> _sceneSensor;
    std::unique_ptr<SoNodeSensor> _cameraSensor;
};

}  // namespace Gui
