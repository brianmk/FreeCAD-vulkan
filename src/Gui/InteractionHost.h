// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCGlobal.h>

#include <Inventor/SbRotation.h>
#include <Inventor/SbVec2s.h>
#include <Inventor/SbVec3f.h>

#include <memory>

class QWidget;
class SbViewportRegion;
class SoCamera;
class SoEvent;
class SoEventManager;
class SoGroup;
class SoNode;
class SoSeparator;

namespace Gui
{

class AbstractMouseSelection;
class NavigationAnimation;

/** Surface-independent host for a navigation style.
 *
 *  `NavigationStyle` and its subclasses used to reach directly into
 *  `View3DInventorViewer` for the camera, scene graph, viewport region, event
 *  manager, editing state and cursor/redraw.  That coupling is what forced
 *  navigation (and picking) to run on the hidden GL viewer even when the
 *  Vulkan surface was the one on screen.
 *
 *  `InteractionHost` is the seam that replaces that coupling.  It deliberately
 *  exposes no `SoRenderManager`: camera, scene graph and viewport region are
 *  all the renderer-independent state navigation and picking need, so a
 *  non-GL surface (the Vulkan viewport) can satisfy the contract without a GL
 *  render manager.  `InteractionController` implements it for the active
 *  surface, and every surface reports its own camera/scene/region, so the same
 *  navigation styles drive whichever surface is current.
 */
class GuiExport InteractionHost
{
public:
    virtual ~InteractionHost() = default;

    //! @name Scene / camera / viewport
    //@{
    virtual SoCamera* getCamera() const = 0;
    virtual SoNode* getSceneGraph() const = 0;
    virtual const SbViewportRegion& getViewportRegion() const = 0;
    virtual SoEventManager* getSoEventManager() const = 0;
    virtual SbVec3f getFocalPoint() const = 0;
    virtual float getPickRadius() const = 0;
    virtual QWidget* getGLWidget() const = 0;
    //@}

    //! @name Interaction / editing state
    //@{
    virtual bool isEditing() const = 0;
    virtual bool isEditingViewProvider() const = 0;
    virtual bool isSelectionEnabled() const = 0;
    virtual bool isViewing() const = 0;
    virtual bool isSeekMode() const = 0;
    virtual void setViewing(bool enable) = 0;
    virtual void setSeekMode(bool enable) = 0;
    virtual bool seekToPoint(const SbVec2s& screenpos) = 0;
    virtual void seekToPoint(const SbVec3f& scenepos) = 0;
    //@}

    //! @name Event dispatch
    //@{
    virtual bool processSoEventBase(const SoEvent* ev) = 0;
    virtual void interactiveCountInc() = 0;
    virtual void interactiveCountDec() = 0;
    virtual int getInteractiveCount() const = 0;
    //@}

    //! @name Camera / view control
    //@{
    virtual std::shared_ptr<NavigationAnimation> setCameraOrientation(
        const SbRotation& orientation,
        bool moveToCenter = false
    ) const = 0;
    virtual std::shared_ptr<NavigationAnimation> startAnimation(
        const SbRotation& orientation,
        const SbVec3f& rotationCenter,
        const SbVec3f& translation,
        int duration = -1,
        bool wait = false
    ) const = 0;
    virtual void startSpinningAnimation(const SbVec3f& axis, float velocity) = 0;
    virtual void viewAll() = 0;
    virtual void showRotationCenter(bool show) = 0;
    virtual void changeRotationCenterPosition(const SbVec3f& newCenter) = 0;
    virtual SbVec2s getPointOnViewport(const SbVec3f&) const = 0;
    //@}

    //! @name Cursor / redraw / scene roots
    //@{
    virtual void setCursorRepresentation(int mode) = 0;
    virtual void scheduleRedraw() = 0;
    virtual SoGroup* getObjectGroup() const = 0;
    virtual SoSeparator* getForegroundRoot() const = 0;
    //@}

    //! @name Selection binding
    //@{
    /** Bind a mouse selection model to this surface.  The GL host wires it to
     *  the viewer for overlay drawing; a Vulkan host will bind it to its own
     *  overlay.  Keeps the `View3DInventorViewer`-typed `grabMouseModel()` out
     *  of `NavigationStyle`. */
    virtual void bindMouseSelection(AbstractMouseSelection* selection) = 0;
    //@}
};

}  // namespace Gui
