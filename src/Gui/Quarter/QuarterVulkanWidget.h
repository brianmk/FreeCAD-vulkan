// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QSize>
#include <QWidget>
#include <functional>

#include <Inventor/SbColor4f.h>
#include <Inventor/rendering/SoRenderIR.h>
#include <Inventor/rendering/SoVulkanViewSettings.h>

#include <Quarter/InputDeviceHost.h>

#ifdef FREECAD_USE_VULKAN
#include <vulkan/vulkan.h>
#endif

class QVulkanInstance;
class QVulkanWindow;
class QImage;

class SoCamera;
class SoNode;
class SoEvent;

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

  This port carries the raster Vulkan renderer only.  GL remains the default;
  this widget is only compiled and used when FREECAD_USE_VULKAN is enabled.
*/
class QuarterVulkanWidget : public QWidget, public InputDeviceHost
{
    Q_OBJECT

public:
    explicit QuarterVulkanWidget(QWidget * parent = nullptr);
    ~QuarterVulkanWidget() override;

    /*!
      \brief Create the process-wide QVulkanInstance ahead of the first view.

      The shared QVulkanInstance (and the Vulkan loader/ICD and GPU driver it
      pulls in) is otherwise created lazily when the first 3D view is built, so
      the first document open pays that one-time driver initialization.  Calling
      this once during GUI startup moves the cost off the document-open path.
      Idempotent; a creation failure is logged and the views fall back exactly as
      before.  Only available in Vulkan builds.
    */
    static void prewarmSharedInstance();

Q_SIGNALS:
    //! Emitted when the Vulkan swapchain size is known or changes.
    //! Delivered on the GUI thread.
    void surfaceSizeChanged(const QSize & size);

public:
    void setSceneGraph(SoNode * root);
    SoNode * getSceneGraph() const;

    /*!
      \brief Set an optional screen-space overlay scene graph.

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
      \brief Configure Vulkan-only display overlays (show-vertices) and their
      edge color.

      These do not affect the hidden OpenGL viewer and are only honored by
      the Vulkan backend.
    */
    void setPointsOverlay(bool enabled);
    void setEdgeColor(const SbColor4f & color);

    /*!
      \brief Forward non-translated input events to another widget.

      Tablet, touch and context-menu events are not translated by the Coin
      input devices, so they are still relayed to \a target (the hidden OpenGL
      viewer that owns FreeCAD's gesture/tablet devices).  Mouse, wheel and
      keyboard events are translated by this widget's own InputDeviceHost and
      delivered through setEventSink().
    */
    void setRawEventTarget(QWidget * target);

    /*!
      \brief Set the sink that receives translated Coin events.

      The widget is now an InputDeviceHost: it translates mouse/wheel/keyboard
      events into SoEvents and hands them to this sink, which the viewport
      adapter wires to the InteractionController.  A null sink drops them.
    */
    void setEventSink(std::function<bool(const SoEvent *)> sink);

    //! @name InputDeviceHost
    //@{
    qreal devicePixelRatio() const override;
    bool vulkanDevicePixels() const override;
    QSize inputSize() const override;
    SbVec2s inputWindowSize() const override;
    bool processSoEvent(const SoEvent * event) override;
    //@}

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

    //! Ordinal of the last presented frame (1-based; 0 before the first
    //! render).  The same value is written into that frame's
    //! SoRenderParams::frame.  Probe phase markers and frame dumps key off it
    //! so the checker can correlate records independent of stream order.
    uint32_t getRenderFrameCount() const;

    /*!
      \brief Apply the whole Vulkan viewport display/tuning settings blob.

      The render manager diffs it and re-applies only on change, so this may be
      called repeatedly.  Structural state (scene, camera, viewport, render
      target) is separate.
    */
    void setViewSettings(const SoVulkanViewSettings & settings);

    /*!
      \brief Provide the authoritative scene lighting (GL host -> raster backend).

      \a lighting is the camera-anchored world-space viewer light set plus the
      intensity-scaled scene ambient.  Forwarded to SoVulkanRenderManager::
      setSceneLights so the raster backend uses the host's lights instead of
      the IR draw-list lighting capture, which can drop to zero on the retained/
      replayed frame and render surfaces near-black.
    */
    void setSceneLights(const SoLightingData & lighting);

    /*!
      \brief Enable/disable interaction LOD (quality reduction while the
      camera moves).

      While engaged the raster renderer draws wide lines as plain 1px GPU
      lines instead of expanding every edge segment into quads on the CPU,
      keeping an interactive orbit/pan of a heavy scene responsive.
      Forwarded to the raster backend; a no-op when it is not initialized.
    */
    void setInteractionLod(bool active);

protected:
    bool eventFilter(QObject * watched, QEvent * event) override;

#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    /*!
      \brief Debug-only synthetic mouse injector (FC_VULKAN_INJECT_PY).

      When the FC_VULKAN_INJECT_PY environment variable names a file, this
      widget polls it on a timer and, for every "type x y" line it has not
      yet consumed, posts a genuine platform mouse event
      (QWindowSystemInterface::handleMouseEvent) to the embedded QVulkanWindow.
      This is the only injection path that reaches the real event filter:
      QCoreApplication::sendEvent() to a QWindowContainer is swallowed by the
      container and never forwarded to the embedded window.  A per-poll
      consumed-line counter lets a probe append new events and have them picked
      up on the next poll.

      Compiled only when FREECAD_VULKAN_DEBUG_HOOKS is set (auto-on for Debug
      builds, see src/Gui/CMakeLists.txt), so release builds carry no injector.
    */
    void pollInjectFile();
#endif

private:
    void ensureSharedInstance();
    void releaseSharedInstance();
#ifdef FREECAD_USE_VULKAN
    void selectPhysicalDevice();
    void configureDeviceFeatures();
    //! Fill the always-on device features (wireframe/points overlays + full
    //! draw-index + dual-source blend) requested for the raster backend.
    void applyBaseDeviceFeatures(VkPhysicalDeviceFeatures2 & features) const;
    void logSupportedSampleCounts();
#endif

    QuarterVulkanWidgetPrivate * d;
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    QTimer * injectTimer = nullptr;
#endif
};

} // namespace Quarter
} // namespace Coin3D
} // namespace SIM
