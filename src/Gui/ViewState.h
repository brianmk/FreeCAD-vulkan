// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCGlobal.h>

#include <Inventor/SbViewportRegion.h>

#include <functional>
#include <utility>
#include <vector>

class SoCamera;
class SoGroup;
class SoSeparator;

namespace Gui
{

/** Backend-agnostic owner of a 3D view's authoritative state.
 *
 *  A view's scene roots, camera, viewport region and device pixel ratio used
 *  to live on the OpenGL `SoRenderManager` (via the Quarter adaptor) and the
 *  Vulkan viewport read them back from there.  `ViewState` lifts that state
 *  into one neutral owner so both the GL and Vulkan backends become consumers:
 *  the GL `SoRenderManager` merely references the same nodes, and the Vulkan
 *  adapter reads scene/camera/viewport from here instead of from the render
 *  manager.
 *
 *  Deliberately a plain class (no `QObject`, no Qt) so it can be created and
 *  queried without a `QApplication`.  Changes to the camera, scene root or
 *  viewport are announced to registered callbacks.
 */
class GuiExport ViewState
{
public:
    enum class Change
    {
        SceneRoot,
        Camera,
        Viewport,
    };

    using ChangeCallback = std::function<void(Change)>;
    using CallbackId = unsigned int;

    ViewState();
    ~ViewState();

    ViewState(const ViewState&) = delete;
    ViewState& operator=(const ViewState&) = delete;

    //! @name Change notification
    //@{
    CallbackId addChangeCallback(ChangeCallback callback);
    void removeChangeCallback(CallbackId id);
    //@}

    //! @name Scene roots and camera
    //@{
    //! The main scene root (the node the GL render manager renders, wrapped in
    //! its superscene, and the Vulkan adapter pushes to its renderer).
    void setSceneRoot(SoSeparator* root);
    SoSeparator* sceneRoot() const;

    //! The active camera node.  Replaceable (projection changes swap the node);
    //! the previous camera is released and the decoration root re-pointed.
    void setCamera(SoCamera* camera);
    SoCamera* camera() const;

    void setObjectGroup(SoGroup* group);
    SoGroup* objectGroup() const;

    void setForegroundRoot(SoSeparator* root);
    SoSeparator* foregroundRoot() const;
    //@}

    //! @name Per-frame decorations
    //@{
    //! The per-frame (camera-coupled) decoration root shared by both
    //! backends; the ground grid lives here.  The active camera is kept as its
    //! first child so the root is self-contained: a standalone traversal (the
    //! GL decoration pass) applies the camera before the grid.  The viewer's IR
    //! decoration wrapper composes this with the axis-cross overlay for the
    //! Vulkan path (see View3DInventorViewer::getDecorationRoot()).
    void setDecorationRoot(SoSeparator* root);
    SoSeparator* decorationRoot() const;
    //@}

    //! @name Viewport
    //@{
    void setViewportRegion(const SbViewportRegion& region);
    const SbViewportRegion& viewportRegion() const;

    void setDevicePixelRatio(float ratio);
    float devicePixelRatio() const;
    //@}

private:
    void notify(Change change);
    void syncDecorationCamera();

    SoSeparator* _sceneRoot {nullptr};
    SoCamera* _camera {nullptr};
    SoGroup* _objectGroup {nullptr};
    SoSeparator* _foregroundRoot {nullptr};
    SoSeparator* _decorationRoot {nullptr};
    SbViewportRegion _viewportRegion;
    float _devicePixelRatio {1.0F};
    std::vector<std::pair<CallbackId, ChangeCallback>> _callbacks;
    CallbackId _nextCallbackId {1};
};

}  // namespace Gui
