// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCGlobal.h>

#include <Base/Type.h>

#include <Inventor/SbViewportRegion.h>

#include "InteractionHost.h"

#include <memory>

class SoCamera;
class SoEvent;
class SoEventManager;

namespace Gui
{

class InteractionSurface;
class NavigationStyle;
class NavigationAnimation;

/** Owns the interaction pipeline for one render surface.
 *
 *  Phase 1 gave `NavigationStyle` an `InteractionHost` seam; this is the
 *  collaborator that implements it.  The controller owns the navigation style
 *  and the `processSoEvent` dispatch (NaviCube overlay, redirect-to-scene-graph
 *  and keyboard filtering), and forwards the surface-independent camera/scene/
 *  viewport/cursor/redraw queries to the `InteractionSurface` it was built for.
 *
 *  Both the GL `View3DInventorViewer` and `VulkanViewportAdapter` implement
 *  `InteractionSurface`, so the controller (and therefore navigation and
 *  picking) runs against whichever surface is current without depending on the
 *  hidden GL viewer.
 */
class GuiExport InteractionController: public InteractionHost
{
public:
    explicit InteractionController(InteractionSurface* surface);
    ~InteractionController() override;

    InteractionController(const InteractionController&) = delete;
    InteractionController& operator=(const InteractionController&) = delete;

    //! Dispatch one event through the NaviCube/navigation/base pipeline.
    //! Moved out of `View3DInventorViewer::processSoEvent()`.
    bool processSoEvent(const SoEvent* ev);

    //! Replace the active navigation style.  Moved out of the viewer.
    void setNavigationType(Base::Type type);
    //! The active navigation style (never null after construction).
    NavigationStyle* navigationStyle() const;

    //! The surface this controller drives.
    InteractionSurface* surface() const
    {
        return _surface;
    }

    /** Swap the active render surface.
     *
     *  Used by the Vulkan adapter to make itself the surface while its page is
     *  current, so camera/scene/viewport and surface-presentation calls
     *  (`getGLWidget()`, `scheduleRedraw()`, cursor) reach the visible Vulkan
     *  surface.  The `SoEventManager` is *not* re-injected: it stays installed
     *  on the GL viewer (the base event-dispatch authority) and is answered by
     *  the controller itself.  Pass nullptr to detach (used on teardown).
     */
    void setSurface(InteractionSurface* surface);

    /** Set the canonical viewport region / device pixel ratio.
     *
     *  A surface that is not the one owning the Coin render manager (the Vulkan
     *  viewport) reports its size here.  Picking/navigation then use this
     *  single region instead of the hidden GL viewer's hand-synced copy; the
     *  controller also pushes it into the event manager so
     *  `SoHandleEventAction` picks in the same space.
     */
    void setViewportRegion(const SbViewportRegion& region, float devicePixelRatio);
    //! The canonical device pixel ratio (1.0 until a surface reports one).
    float devicePixelRatio() const
    {
        return _devicePixelRatio;
    }
    //! True once a surface has reported a canonical region.
    bool hasViewportRegion() const
    {
        return _hasViewportRegion;
    }

    //! @name InteractionHost (surface-independent navigation view)
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
    //@}

private:
    InteractionSurface* _surface {nullptr};
    //! The Coin interaction authority: owns the event pipeline (scene-graph
    //! event dispatch + picking).  Moved out of QuarterWidget; the surface
    //! borrows it via surfaceSetEventManager().
    SoEventManager* _eventManager {nullptr};
    NavigationStyle* _navigation {nullptr};
    //! Canonical viewport region/DPR reported by a non-GL surface (Vulkan).
    SbViewportRegion _viewportRegion;
    float _devicePixelRatio {1.0F};
    bool _hasViewportRegion {false};
};

}  // namespace Gui
