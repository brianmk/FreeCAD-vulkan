// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "ViewState.h"

#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoSeparator.h>

using namespace Gui;

ViewState::ViewState()
{
    // The per-frame decoration root is owned here and shared by both backends.
    // The active camera is kept as its first child so the root is
    // self-contained: a standalone traversal (the GL decoration pass) applies
    // the camera and renders the camera-coupled decorations without inheriting
    // the main scene's camera state.
    _decorationRoot = new SoSeparator;
    _decorationRoot->ref();
    _decorationRoot->setName("viewStateDecorationRoot");
}

ViewState::~ViewState()
{
    if (_sceneRoot) {
        _sceneRoot->unref();
    }
    if (_objectGroup) {
        _objectGroup->unref();
    }
    if (_foregroundRoot) {
        _foregroundRoot->unref();
    }
    if (_decorationRoot) {
        if (_camera && _decorationRoot->findChild(_camera) >= 0) {
            _decorationRoot->removeChild(_camera);
        }
        _decorationRoot->unref();
    }
    if (_camera) {
        _camera->unref();
    }
}

ViewState::CallbackId ViewState::addChangeCallback(ChangeCallback callback)
{
    const CallbackId id = _nextCallbackId++;
    _callbacks.emplace_back(id, std::move(callback));
    return id;
}

void ViewState::removeChangeCallback(CallbackId id)
{
    for (auto it = _callbacks.begin(); it != _callbacks.end(); ++it) {
        if (it->first == id) {
            _callbacks.erase(it);
            return;
        }
    }
}

void ViewState::notify(Change change)
{
    // Iterate over a copy: a callback may remove itself.
    const auto callbacks = _callbacks;
    for (const auto& entry : callbacks) {
        entry.second(change);
    }
}

void ViewState::setSceneRoot(SoSeparator* root)
{
    if (_sceneRoot == root) {
        return;
    }
    if (_sceneRoot) {
        _sceneRoot->unref();
    }
    _sceneRoot = root;
    if (_sceneRoot) {
        _sceneRoot->ref();
    }
    notify(Change::SceneRoot);
}

SoSeparator* ViewState::sceneRoot() const
{
    return _sceneRoot;
}

void ViewState::setCamera(SoCamera* camera)
{
    if (_camera == camera) {
        return;
    }
    if (_camera) {
        _camera->unref();
    }
    _camera = camera;
    if (_camera) {
        _camera->ref();
    }
    syncDecorationCamera();
    notify(Change::Camera);
}

SoCamera* ViewState::camera() const
{
    return _camera;
}

void ViewState::syncDecorationCamera()
{
    if (!_decorationRoot) {
        return;
    }
    // Drop a previous camera child, then place the current one first so it
    // frames any camera-coupled child (the ground grid) added after it.
    for (int i = _decorationRoot->getNumChildren() - 1; i >= 0; --i) {
        SoNode* child = _decorationRoot->getChild(i);
        if (child && child->isOfType(SoCamera::getClassTypeId())) {
            _decorationRoot->removeChild(i);
        }
    }
    if (_camera) {
        _decorationRoot->insertChild(_camera, 0);
    }
}

void ViewState::setObjectGroup(SoGroup* group)
{
    if (_objectGroup == group) {
        return;
    }
    if (_objectGroup) {
        _objectGroup->unref();
    }
    _objectGroup = group;
    if (_objectGroup) {
        _objectGroup->ref();
    }
}

SoGroup* ViewState::objectGroup() const
{
    return _objectGroup;
}

void ViewState::setForegroundRoot(SoSeparator* root)
{
    if (_foregroundRoot == root) {
        return;
    }
    if (_foregroundRoot) {
        _foregroundRoot->unref();
    }
    _foregroundRoot = root;
    if (_foregroundRoot) {
        _foregroundRoot->ref();
    }
}

SoSeparator* ViewState::foregroundRoot() const
{
    return _foregroundRoot;
}

void ViewState::setDecorationRoot(SoSeparator* root)
{
    if (_decorationRoot == root) {
        return;
    }
    if (_decorationRoot) {
        if (_camera && _decorationRoot->findChild(_camera) >= 0) {
            _decorationRoot->removeChild(_camera);
        }
        _decorationRoot->unref();
    }
    _decorationRoot = root;
    if (_decorationRoot) {
        _decorationRoot->ref();
    }
    syncDecorationCamera();
}

SoSeparator* ViewState::decorationRoot() const
{
    return _decorationRoot;
}

void ViewState::setViewportRegion(const SbViewportRegion& region)
{
    _viewportRegion = region;
    notify(Change::Viewport);
}

const SbViewportRegion& ViewState::viewportRegion() const
{
    return _viewportRegion;
}

void ViewState::setDevicePixelRatio(float ratio)
{
    if (ratio <= 0.0F) {
        ratio = 1.0F;
    }
    if (_devicePixelRatio == ratio) {
        return;
    }
    _devicePixelRatio = ratio;
    notify(Change::Viewport);
}

float ViewState::devicePixelRatio() const
{
    return _devicePixelRatio;
}
