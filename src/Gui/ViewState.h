// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCGlobal.h>

#include <Inventor/SbColor.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/SbVec2s.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/rendering/SoRenderIR.h>

#include <functional>
#include <utility>
#include <vector>

class SoCamera;
class SoDirectionalLight;
class SoEnvironment;
class SoGroup;
class SoPickedPoint;
class SoRayPickAction;
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

    //! @name Picking
    //@{
    /** Apply \a action to this view's neutral pick scene.
     *
     *  The action is run against a transient root that applies the owned
     *  camera before the owned scene root, so a pick needs only the view state
     *  (camera + scene + viewport) and never the GL render manager -- or its
     *  superscene -- even though that is what the classic GL path picks.  The
     *  wrapper is transient; only the camera and scene references are shared.
     *  Configure \a action (point/ray/radius/pickAll) before calling.
     */
    void applyPick(SoRayPickAction& action) const;

    /** Pick the nearest scene point under viewport pixel \a pos.
     *
     *  Returns an owned copy (the caller deletes it) or nullptr.
     */
    SoPickedPoint* pickPoint(const SbVec2s& pos, float radius = 0.0F) const;
    //@}

    //! @name View lighting
    //@{
    /** Set the (non-owning) viewer lights whose camera-anchored setup the
     *  Vulkan backends consume.  The viewer keeps ownership. */
    void setLights(
        SoDirectionalLight* headlight,
        SoDirectionalLight* backlight,
        SoDirectionalLight* fillLight,
        SoEnvironment* environment
    );
    SoDirectionalLight* headlight() const;
    SoDirectionalLight* backlight() const;
    SoDirectionalLight* fillLight() const;
    SoEnvironment* environment() const;

    /** World-space, camera-anchored light set for a backend that shades in
     *  world space (the Vulkan raster/ray-traced backends).
     *
     *  The viewer's three-point lighting is view-relative: each light's raw
     *  travel direction is applied in eye space (the head/back lights are
     *  traversed before the camera, the fill light hangs under a rotation
     *  connected to the camera orientation).  This reproduces that by rotating
     *  each enabled light from eye space into world space with the current
     *  camera orientation, so the highlights follow the view exactly as Coin
     *  GL does.  This is the neutral owner of the derivation the Vulkan
     *  adapter used to gather from the GL viewer.
     */
    SoLightingData sceneLights() const;
    //@}

    //! @name Background
    //@{
    void setBackgroundColor(const SbColor4f& color);
    const SbColor4f& backgroundColor() const;

    void setBackgroundGradientEnabled(bool enabled);
    bool hasBackgroundGradient() const;
    void setBackgroundGradientColors(const SbColor& top, const SbColor& bottom);
    const SbColor& backgroundTop() const;
    const SbColor& backgroundBottom() const;
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

    //! Viewer-owned lights (not ref-counted here; the viewer owns them).
    SoDirectionalLight* _headlight {nullptr};
    SoDirectionalLight* _backlight {nullptr};
    SoDirectionalLight* _fillLight {nullptr};
    SoEnvironment* _environment {nullptr};

    //! Background view-model (solid clear colour plus optional gradient).
    SbColor4f _backgroundColor {0.0F, 0.0F, 0.0F, 1.0F};
    bool _backgroundGradient {false};
    SbColor _backgroundTop;
    SbColor _backgroundBottom;

    std::vector<std::pair<CallbackId, ChangeCallback>> _callbacks;
    CallbackId _nextCallbackId {1};
};

}  // namespace Gui
