// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include "InteractionHost.h"

class SoEvent;
class SoEventManager;

namespace Gui
{

/** Surface hooks the `InteractionController` needs beyond the navigation
 *  contract in `InteractionHost`.
 *
 *  A *surface* is the thing that renders and receives native events: the GL
 *  `View3DInventorViewer` today, the Vulkan widget later.  `InteractionHost`
 *  already exposes the camera/scene/viewport/event-manager the navigation
 *  styles reach through; this adds the surface-owned overlay/redirect hooks the
 *  controller's `processSoEvent` dispatch needs, so the same controller can run
 *  on either surface.
 *
 *  See `docs/vulkan/INTERACTION_AUTHORITY.md` (Phase 2).
 */
class GuiExport InteractionSurface: public InteractionHost
{
public:
    ~InteractionSurface() override = default;

    //! Whether the surface owns a visible NaviCube overlay.
    virtual bool surfaceNaviCubeEnabled() const = 0;
    //! Offer the event to the NaviCube overlay; true when it consumed it.
    virtual bool surfaceProcessNaviCubeEvent(const SoEvent* ev) = 0;
    //! Whether events should be redirected straight into the scene graph.
    virtual bool surfaceIsRedirectedToSceneGraph() const = 0;
    //! The active camera pose changed in place (navigation mutated the shared
    //! camera node); the surface wakes its renderer / emits cameraMoved().
    virtual void surfaceNotifyCameraMoved() = 0;

    //! Install (or, with nullptr, detach) the Coin event manager the surface
    //! should run its events through.  The controller owns it; the surface only
    //! borrows it, so detaching before the controller is destroyed keeps the
    //! surface from touching freed memory during teardown.
    virtual void surfaceSetEventManager(SoEventManager* manager) = 0;
};

}  // namespace Gui
