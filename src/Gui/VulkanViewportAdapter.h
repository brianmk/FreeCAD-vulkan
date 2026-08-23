// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <QObject>

class QStackedWidget;

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
 *  Only compiled and used when FREECAD_USE_VULKAN is enabled; every method
 *  is a no-op otherwise.
 */
class VulkanViewportAdapter : public QObject
{
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

    void setPathTracingEnabled(bool enabled);
    void setPathTracingStart(bool start);
    bool isPathTracingEnabled() const;
    bool isPathTracingActive() const;

private:
    void onSurfaceSizeChanged(const QSize& surfaceSize);
    bool eventFilter(QObject* watched, QEvent* event) override;

    View3DInventorViewer* _viewer = nullptr;
    SIM::Coin3D::Quarter::QuarterVulkanWidget* _vulkanViewer = nullptr;
    bool _initialVulkanFitDone = false;
    bool _pathTracingRtMismatchWarned = false;
};

}  // namespace Gui
