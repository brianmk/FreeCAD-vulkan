// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#include "ViewState.h"

#include <Inventor/SoPickedPoint.h>
#include <Inventor/actions/SoRayPickAction.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoDirectionalLight.h>
#include <Inventor/nodes/SoEnvironment.h>
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

void ViewState::applyPick(SoRayPickAction& action) const
{
    // The classic GL path picks the render manager's superscene, which wraps
    // the scene root with the active camera (plus a headlight that is
    // irrelevant to ray picking).  Reproduce only the camera+scene part here so
    // picking needs neither the superscene nor the GL render manager: a
    // transient separator applies the owned camera before the shared scene
    // root.  Only the camera/scene references are shared; the wrapper is
    // discarded as soon as the action has run.
    SoSeparator* root = new SoSeparator;
    root->ref();
    if (_camera) {
        root->addChild(_camera);
    }
    if (_sceneRoot) {
        root->addChild(_sceneRoot);
    }
    action.apply(root);
    root->unref();
}

SoPickedPoint* ViewState::pickPoint(const SbVec2s& pos, float radius) const
{
    SoRayPickAction action(_viewportRegion);
    action.setPoint(pos);
    if (radius > 0.0F) {
        action.setRadius(radius);
    }
    applyPick(action);

    const SoPickedPoint* pick = action.getPickedPoint();
    return (pick ? new SoPickedPoint(*pick) : nullptr);
}

void ViewState::setLights(
    SoDirectionalLight* headlight,
    SoDirectionalLight* backlight,
    SoDirectionalLight* fillLight,
    SoEnvironment* environment
)
{
    _headlight = headlight;
    _backlight = backlight;
    _fillLight = fillLight;
    _environment = environment;
}

SoDirectionalLight* ViewState::headlight() const
{
    return _headlight;
}

SoDirectionalLight* ViewState::backlight() const
{
    return _backlight;
}

SoDirectionalLight* ViewState::fillLight() const
{
    return _fillLight;
}

SoEnvironment* ViewState::environment() const
{
    return _environment;
}

SoLightingData ViewState::sceneLights() const
{
    SoLightingData lighting;

    // World <- eye rotation for the current camera: the camera orientation is
    // the inverse of the view rotation, i.e. exactly what SoRenderIR::
    // lightToWorld() expects.  The eye<->world convention lives in SoRenderIR
    // rather than being re-derived here.
    SbMatrix eyeToWorld;
    if (_camera) {
        const SbRotation camRot = _camera->orientation.getValue();
        camRot.getValue(eyeToWorld);
    }

    // Scene ambient from the viewer's environment node (so a scene lit purely
    // by ambient still reads non-black).
    if (_environment) {
        const SbColor& ac = _environment->ambientColor.getValue();
        const float ai = _environment->ambientIntensity.getValue();
        lighting.ambient = SbVec3f(ac[0] * ai, ac[1] * ai, ac[2] * ai);
    }

    // Push each enabled directional light with the same world-space convention
    // the raster IR uses (headlight + backlight + fill, matching the viewer's
    // three-point lighting), but anchored to the camera so the Vulkan backends
    // follow the view like Coin GL.
    lighting.lights.reserve(3);
    const SoDirectionalLight* lights[] = {_headlight, _backlight, _fillLight};
    for (const SoDirectionalLight* light : lights) {
        if (!light || !light->on.getValue()) {
            continue;
        }
        SoLightData l;
        l.type = SO_LIGHT_DIRECTIONAL;
        const SbVec3f c = light->color.getValue();
        const float i = light->intensity.getValue();
        l.color = SbVec3f(c[0] * i, c[1] * i, c[2] * i);
        SbVec3f eyeDir = -light->direction.getValue();
        if (eyeDir.normalize() == 0.0F) {
            eyeDir = SbVec3f(0.0F, 0.0F, 1.0F);
        }
        l.direction = eyeDir;
        lighting.lights.push_back(SoRenderIR::lightToWorld(l, eyeToWorld));
    }

    return lighting;
}

void ViewState::setBackgroundColor(const SbColor4f& color)
{
    _backgroundColor = color;
}

const SbColor4f& ViewState::backgroundColor() const
{
    return _backgroundColor;
}

void ViewState::setBackgroundGradientEnabled(bool enabled)
{
    _backgroundGradient = enabled;
}

bool ViewState::hasBackgroundGradient() const
{
    return _backgroundGradient;
}

void ViewState::setBackgroundGradientColors(const SbColor& top, const SbColor& bottom)
{
    _backgroundTop = top;
    _backgroundBottom = bottom;
}

const SbColor& ViewState::backgroundTop() const
{
    return _backgroundTop;
}

const SbColor& ViewState::backgroundBottom() const
{
    return _backgroundBottom;
}
