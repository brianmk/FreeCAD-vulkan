// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QSize>
#include <QWidget>
#include <string>

#include <Inventor/SbColor4f.h>

#ifdef FREECAD_USE_VULKAN
#include <vulkan/vulkan.h>
#endif

class QVulkanInstance;
class QVulkanWindow;
class QImage;

class SoCamera;
class SoNode;

namespace SIM {
namespace Coin3D {
namespace Quarter {

class QuarterVulkanWidgetPrivate;

/*!
  \brief Self-contained Vulkan viewport widget for FreeCAD's Quarter layer.

  Wraps a QVulkanWindow (through QWidget::createWindowContainer()) and drives a
  Qt-free SoVulkanRenderManager each frame.  This class intentionally mirrors
  the small slice of the SoQTQuarterAdaptor / QuarterWidget API the viewport
  needs, so it can be swapped in without depending on the legacy OpenGL
  render path.

  GL remains the default; this widget is only compiled and used when
  FREECAD_USE_VULKAN is enabled.
*/
class QuarterVulkanWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QuarterVulkanWidget(QWidget * parent = nullptr,
                                 bool rayTracing = false);
    ~QuarterVulkanWidget() override;

Q_SIGNALS:
    //! Emitted when the Vulkan swapchain size is known or changes.
    //! Delivered on the GUI thread.
    void surfaceSizeChanged(const QSize & size);

public:
    void setSceneGraph(SoNode * root);
    SoNode * getSceneGraph() const;

    /*!
      \brief Set an optional screen-space overlay scene graph (navigation cube).

      The overlay scene is traversed and drawn after the main scene each frame
      in its own viewport/scissor region (see SoVulkanRenderManager::
      setOverlaySceneGraph()).  Pass nullptr to disable it.
    */
    void setOverlaySceneGraph(SoNode * root);
    SoNode * getOverlaySceneGraph() const;

    /*!
      \brief Set an optional decoration scene graph (axis cross overlay).

      Traversed after the overlay scene graph each frame and drawn in the
      overlay pass on top of it (see SoVulkanRenderManager::
      setDecorationSceneGraph()).  Pass nullptr to disable it.
    */
    void setDecorationSceneGraph(SoNode * root);
    SoNode * getDecorationSceneGraph() const;

    void setCamera(SoCamera * camera);
    SoCamera * getCamera() const;

    void setBackgroundColor(const SbColor4f & color);
    SbColor4f getBackgroundColor() const;

    /*!
      \brief Configure a vertical screen-space background gradient.

      When \a enabled is TRUE the Vulkan surface is filled with a top-to-
      bottom gradient between \a topColor and \a bottomColor before geometry
      is drawn, instead of a flat clear color.
    */
    void setBackgroundGradient(bool enabled,
                               const SbColor4f & topColor,
                               const SbColor4f & bottomColor);

    /*!
      \brief Configure Vulkan-only display overlays (shaded-with-edges /
      show-vertices) and their edge color.

      These do not affect the hidden OpenGL viewer and are only honored by
      the Vulkan backend.
    */
    void setWireframeOverlay(bool enabled);
    void setPointsOverlay(bool enabled);
    void setEdgeColor(const SbColor4f & color);

    /*!
      \brief Forward viewport input events to another widget.

      The Vulkan widget is display-only: it has no navigation, picking or
      scene-graph event handling of its own.  Setting a forward target makes
      it relay mouse, wheel, keyboard, tablet and touch events to that widget
      (normally the hidden OpenGL viewer) so navigation and picking keep
      working while the Vulkan surface is on top.
    */
    //! Forward input events to \a target (typically the hidden GL viewer that
    //! owns navigation/picking).  \a targetDevicePixelRatio is the device
    //! pixel ratio the target uses to convert event positions; pass -1 to
    //! fall back to the target's current devicePixelRatioF().  Providing it
    //! explicitly avoids sniffing the target's type at event time.
    void setEventForwardTarget(QWidget * target, qreal targetDevicePixelRatio = -1.0);

    /*!
      \brief No-op kept for API parity with QuarterWidget.

      QVulkanWindow's default render pass always clears color and depth
      (LOAD_OP_CLEAR), so frame clears cannot be disabled on the Vulkan
      path.  Calls log a one-time warning.
    */
    void setClearEnabled(bool clearwindow, bool clearzbuffer);

    /*!
      \brief Configure MSAA sample count (1, 2, 4, 8...).

      Must be called before the window is first shown.  Values unsupported by
      the physical device fall back to QVulkanWindow's default of 1.
    */
    void setSampleCount(int samples);
    int getSampleCount() const;

    /*!
      \brief Request a preferred swapchain color format.

      Must be called before the window is first shown.  This matters for
      grab(): QVulkanWindow::grab() only performs an 8-bit conversion (and
      BGR<->RGB swap) when the swapchain format is VK_FORMAT_B8G8R8A8_UNORM.
      Other formats are read back as raw bits and produce garbled QImage
      contents.  Tests that verify pixels should request
      VK_FORMAT_B8G8R8A8_UNORM explicitly.
    */
    void setPreferredColorFormat(int vkFormat);

    //! Schedule a redraw on the Vulkan window (safe from any thread).
    void redraw();

    /*!
      \brief Whether the underlying QVulkanWindow supports grabbing a
      resolved frame back to the CPU.
    */
    bool supportsGrab() const;

    /*!
      \brief Grab the last presented frame as a QImage (empty if unsupported).

      Useful for smoke tests and offscreen verification of the MSAA resolve.
    */
    QImage grab() const;

    QWidget * getNativeWidget();

    /*!
      \brief Whether the ray-tracing backend is active.

      Ray tracing requires a Vulkan 1.2+ device with
      VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline enabled.
      The widget requests them at construction when \a rayTracing was true;
      when the device does not support them the raster backend is used and
      this returns FALSE.
    */
    bool isRayTracingActive() const;

    /*!
      \brief Whether hardware ray tracing is available on this device.

      Independent of the live raster/RT toggle: TRUE when the RTX backend
      initialized (the device advertises VK_KHR_acceleration_structure /
      VK_KHR_ray_tracing_pipeline / VK_KHR_ray_query and the widget enabled
      them).  When FALSE, path tracing can never run here.
    */
    bool isRayTracingAvailable() const;

    /*!
      \brief Whether availability has been determined (initResources ran).

      False before the renderer first initialized the Vulkan backend; a
      caller must not warn about missing hardware ray tracing until this
      returns TRUE.
    */
    bool isRayTracingProbed() const;

    /*!
      \brief Enable/disable path tracing on the ray-tracing backend.

      Path tracing renders multi-bounce global illumination with shadow
      rays, progressive per-pixel accumulation and an edge-stopping denoise
      pass.  The setting takes effect on the next frame; a no-op when the
      ray-tracing backend is not active.
    */
    void setPathTracingEnabled(bool enabled);

    //! True when path tracing is enabled (see setPathTracingEnabled()).
    bool getPathTracingEnabled() const;

    /*!
      \brief Start flag for progressive path-tracing refinement.

      Raising the flag starts a fresh progressive accumulation (one jittered
      sample per frame, denoised on display).  Any camera move or scene
      change automatically drops back to a single-sample preview until the
      flag is raised again.
    */
    void setPathTracingStart(bool start);

    //! True while a progressive accumulation is running.
    bool getPathTracingActive() const;

    /*!
      \brief Maximum path-tracing bounces (1..16).

      Higher bounce counts add more indirect-light transport at the cost of
      noisier early frames.  Forwarded to the ray-tracing backend.
    */
    void setPathTracingBounces(int bounces);

    /*!
      \brief Frames of a static camera before the accumulation auto-restarts
      (1..120).

      After a camera or scene change the renderer drops to a single-sample
      live preview; once the camera stays static for this many frames a fresh
      accumulation starts automatically.  Forwarded to the ray-tracing
      backend.
    */
    void setPathTracingSettleFrames(int frames);

    /*!
      \brief Enable/disable the edge-stopping denoise pass.

      When disabled the raw accumulated radiance is shown, making the
      Monte-Carlo noise of the early accumulation frames visible.  Forwarded
      to the ray-tracing backend.
    */
    void setPathTracingDenoise(bool enabled);

    /*!
      \brief Select the denoiser backend by name ("rtx", "oidn", "fsr",
      "none"); an empty string uses the default (backend env / built-in)
      choice.  Forwarded to the ray-tracing backend.
    */
    void setPathTracingDenoiser(const std::string & denoiser);

    //! Accumulated-sample cap (1..4096) before the run auto-stops; forwarded
    //! to the ray-tracing backend.
    void setPathTracingMaxSamples(int samples);

protected:
    bool eventFilter(QObject * watched, QEvent * event) override;

private:
    void ensureSharedInstance();
    void releaseSharedInstance();
    void selectPhysicalDevice();
    bool deviceSupportsRayTracing(VkPhysicalDevice device);
    void configureDeviceFeatures(bool rayTracing);
    void logSupportedSampleCounts();

    QuarterVulkanWidgetPrivate * d;
};

} // namespace Quarter
} // namespace Coin3D
} // namespace SIM
