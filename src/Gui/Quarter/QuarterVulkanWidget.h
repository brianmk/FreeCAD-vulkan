// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QWidget>

#include <Inventor/SbColor4f.h>

class QVulkanInstance;
class QVulkanWindow;
class QImage;

class SbViewportRegion;
class SoCamera;
class SoNode;
class SoVulkanRenderManager;

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
    explicit QuarterVulkanWidget(QWidget * parent = nullptr);
    ~QuarterVulkanWidget() override;

    void setSceneGraph(SoNode * root);
    SoNode * getSceneGraph() const;

    void setCamera(SoCamera * camera);
    SoCamera * getCamera() const;

    void setViewportRegion(const SbViewportRegion & region);
    const SbViewportRegion & getViewportRegion() const;

    void setBackgroundColor(const SbColor4f & color);
    const SbColor4f & getBackgroundColor() const;

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

    SoVulkanRenderManager * getRenderManager() const;

    QWidget * getNativeWidget();

public Q_SLOTS:
    void viewAll();

private:
    QuarterVulkanWidgetPrivate * d;
};

} // namespace Quarter
} // namespace Coin3D
} // namespace SIM
