// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "InteractionController.h"

#include "InteractionSurface.h"
#include "Navigation/NavigationStyle.h"

#include <FCConfig.h>

#include <Inventor/SoEventManager.h>
#include <Inventor/errors/SoDebugError.h>
#include <Inventor/events/SoKeyboardEvent.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Inventor/SbRotation.h>
#include <Inventor/SbVec2s.h>
#include <Inventor/SbVec3f.h>

using namespace Gui;

InteractionController::InteractionController(InteractionSurface* surface)
    : _surface(surface)
{
    // Own the Coin interaction authority and lend it to the surface.  This is
    // the SoEventManager QuarterWidget used to create and own itself; moving it
    // here is what lets a second surface (the Vulkan viewport) drive the same
    // picking/navigation pipeline.
    _eventManager = new SoEventManager;
    _eventManager->setNavigationState(SoEventManager::MIXED_NAVIGATION);
    if (_surface) {
        _surface->surfaceSetEventManager(_eventManager);
    }

    setNavigationType(CADNavigationStyle::getClassTypeId());
}

InteractionController::~InteractionController()
{
    // Detach first: the surface may outlive this controller (base destructors
    // run after the derived one) and must not reach a freed event manager.
    if (_surface) {
        _surface->surfaceSetEventManager(nullptr);
    }
    delete _eventManager;

    delete _navigation;
}

void InteractionController::setNavigationType(Base::Type type)
{
    if (_navigation && _navigation->getTypeId() == type) {
        return;  // nothing to do
    }

    Base::Type navtype
        = Base::Type::getTypeIfDerivedFrom(type.getName(), NavigationStyle::getClassTypeId());
    auto ns = static_cast<NavigationStyle*>(navtype.createInstance());
    // createInstance could return a null pointer
    if (!ns) {
#if FC_DEBUG
        SoDebugError::postWarning(
            "InteractionController::setNavigationType",
            "Navigation object must be of type NavigationStyle."
        );
#endif
        return;
    }

    if (_navigation) {
        ns->operator=(*_navigation);
        delete _navigation;
    }
    _navigation = ns;
    _navigation->setViewer(this);
}

NavigationStyle* InteractionController::navigationStyle() const
{
    return _navigation;
}

void InteractionController::setSurface(InteractionSurface* surface)
{
    // The SoEventManager stays on the GL viewer (the base dispatch authority
    // for `processSoEventBase()`), so swapping the surface does not re-install
    // it.  The controller answers `getSoEventManager()` from its own
    // `_eventManager`, and the canonical viewport region/DPR is reported by the
    // surface that owns the visible swapchain.
    _surface = surface;
}

bool InteractionController::processSoEvent(const SoEvent* ev)
{
    if (!_surface) {
        return false;
    }

    // Snapshot the camera pose before the event so a navigation/event that
    // rotates, pans or zooms (which mutates the shared camera node in place)
    // can be detected afterwards.  The display-only Vulkan widget owns no Coin
    // sensors and, once the path tracer has converged, runs no continuous
    // refine loop, so without this a camera move would never re-render:
    // surfaceNotifyCameraMoved() lets the surface request one frame.
    SoCamera* cam = getCamera();
    const SbVec3f camPosBefore = cam ? cam->position.getValue() : SbVec3f();
    const SbRotation camOriBefore = cam ? cam->orientation.getValue() : SbRotation();

    bool result = false;
    if (_surface->surfaceNaviCubeEnabled() && _surface->surfaceProcessNaviCubeEvent(ev)) {
        return true;
    }
    if (!_navigation) {
        return _surface->processSoEventBase(ev);
    }
    if (_surface->surfaceIsRedirectedToSceneGraph()) {
        result = _surface->processSoEventBase(ev);

        if (!result) {
            result = _navigation->processEvent(ev);
        }
    }
    else if (ev->getTypeId().isDerivedFrom(SoKeyboardEvent::getClassTypeId())) {
        // filter out 'Q' and 'ESC' keys
        const auto ke = static_cast<const SoKeyboardEvent*>(ev);  // NOLINT

        switch (ke->getKey()) {
            case SoKeyboardEvent::ESCAPE:
            case SoKeyboardEvent::Q:  // ignore 'Q' keys (to prevent app from being closed)
                return _surface->processSoEventBase(ev);
            default:
                result = _navigation->processEvent(ev);
                break;
        }
    }
    else {
        result = _navigation->processEvent(ev);
    }

    if (cam && cam == getCamera()
        && (cam->position.getValue() != camPosBefore
            || cam->orientation.getValue() != camOriBefore)) {
        _surface->surfaceNotifyCameraMoved();
    }
    return result;
}

SoCamera* InteractionController::getCamera() const
{
    return _surface->getCamera();
}

SoNode* InteractionController::getSceneGraph() const
{
    return _surface->getSceneGraph();
}

const SbViewportRegion& InteractionController::getViewportRegion() const
{
    if (_hasViewportRegion) {
        return _viewportRegion;
    }
    return _surface->getViewportRegion();
}

void InteractionController::setViewportRegion(
    const SbViewportRegion& region,
    float devicePixelRatio
)
{
    _viewportRegion = region;
    _devicePixelRatio = devicePixelRatio > 0.0F ? devicePixelRatio : 1.0F;
    _hasViewportRegion = true;
    if (_eventManager) {
        _eventManager->setViewportRegion(_viewportRegion);
    }
}

SoEventManager* InteractionController::getSoEventManager() const
{
    // The controller owns the Coin interaction authority; it is the same
    // manager the surface borrows through surfaceSetEventManager().  Answering
    // from here means navigation does not need a GL render manager (or any
    // surface) to query the active event/grabber state.
    return _eventManager;
}

SbVec3f InteractionController::getFocalPoint() const
{
    return _surface->getFocalPoint();
}

float InteractionController::getPickRadius() const
{
    return _surface->getPickRadius();
}

QWidget* InteractionController::getGLWidget() const
{
    return _surface->getGLWidget();
}

bool InteractionController::isEditing() const
{
    return _surface->isEditing();
}

bool InteractionController::isEditingViewProvider() const
{
    return _surface->isEditingViewProvider();
}

bool InteractionController::isSelectionEnabled() const
{
    return _surface->isSelectionEnabled();
}

bool InteractionController::isViewing() const
{
    return _surface->isViewing();
}

bool InteractionController::isSeekMode() const
{
    return _surface->isSeekMode();
}

void InteractionController::setViewing(bool enable)
{
    _surface->setViewing(enable);
}

void InteractionController::setSeekMode(bool enable)
{
    _surface->setSeekMode(enable);
}

bool InteractionController::seekToPoint(const SbVec2s& screenpos)
{
    return _surface->seekToPoint(screenpos);
}

void InteractionController::seekToPoint(const SbVec3f& scenepos)
{
    _surface->seekToPoint(scenepos);
}

bool InteractionController::processSoEventBase(const SoEvent* ev)
{
    return _surface->processSoEventBase(ev);
}

void InteractionController::interactiveCountInc()
{
    _surface->interactiveCountInc();
}

void InteractionController::interactiveCountDec()
{
    _surface->interactiveCountDec();
}

int InteractionController::getInteractiveCount() const
{
    return _surface->getInteractiveCount();
}

std::shared_ptr<NavigationAnimation> InteractionController::setCameraOrientation(
    const SbRotation& orientation,
    bool moveToCenter
) const
{
    return _surface->setCameraOrientation(orientation, moveToCenter);
}

std::shared_ptr<NavigationAnimation> InteractionController::startAnimation(
    const SbRotation& orientation,
    const SbVec3f& rotationCenter,
    const SbVec3f& translation,
    int duration,
    bool wait
) const
{
    return _surface->startAnimation(orientation, rotationCenter, translation, duration, wait);
}

void InteractionController::startSpinningAnimation(const SbVec3f& axis, float velocity)
{
    _surface->startSpinningAnimation(axis, velocity);
}

void InteractionController::viewAll()
{
    _surface->viewAll();
}

void InteractionController::showRotationCenter(bool show)
{
    _surface->showRotationCenter(show);
}

void InteractionController::changeRotationCenterPosition(const SbVec3f& newCenter)
{
    _surface->changeRotationCenterPosition(newCenter);
}

SbVec2s InteractionController::getPointOnViewport(const SbVec3f& point) const
{
    return _surface->getPointOnViewport(point);
}

void InteractionController::setCursorRepresentation(int mode)
{
    _surface->setCursorRepresentation(mode);
}

void InteractionController::scheduleRedraw()
{
    _surface->scheduleRedraw();
}

SoGroup* InteractionController::getObjectGroup() const
{
    return _surface->getObjectGroup();
}

SoSeparator* InteractionController::getForegroundRoot() const
{
    return _surface->getForegroundRoot();
}

void InteractionController::bindMouseSelection(AbstractMouseSelection* selection)
{
    _surface->bindMouseSelection(selection);
}
