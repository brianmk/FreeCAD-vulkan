// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the     *
 *   License, or (at your option) any later version.                          *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful, but           *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of               *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *   GNU Lesser General Public License for more details.                      *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD.  If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                         *
 *                                                                            *
 ******************************************************************************/

#include <FCConfig.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <utility>

#include "SoNaviCubeVulkan.h"

#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/SbViewVolume.h>
#include <Inventor/actions/SoIRRenderAction.h>
#include <Inventor/elements/SoLightModelElement.h>
#include <Inventor/elements/SoLazyElement.h>
#include <Inventor/elements/SoShapeStyleElement.h>
#include <Inventor/elements/SoViewportRegionElement.h>
#include <Inventor/misc/SoState.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoDepthBuffer.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoPointSet.h>
#include <Inventor/nodes/SoPolygonOffset.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoShapeHints.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTexture2.h>
#include <Inventor/nodes/SoTransform.h>
#include <Inventor/nodes/SoVertexProperty.h>
#include <Inventor/rendering/SoRenderIR.h>
#endif

using namespace Gui;

namespace
{
constexpr float kOverlayNear = 0.1F;
constexpr float kOverlayFar = 10.1F;
constexpr float kOverlayOrthoExtent = 2.1F;
constexpr float kOverlayFovScale = 1.1F;
constexpr float kOverlayCubeZ = -5.1F;
constexpr float kOverlayButtonZ = -4.0F;  // in front of the cube (cube at ~-5.1)

float toTransparency(float alpha)
{
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    return 1.0F - alpha;
}

// The nine screen-space navigation buttons, in the base SoNaviCube's order.
constexpr std::array<SoNaviCube::PickId, 9> kButtonPickIds = {{
    SoNaviCube::PickId::ArrowNorth,
    SoNaviCube::PickId::ArrowSouth,
    SoNaviCube::PickId::ArrowEast,
    SoNaviCube::PickId::ArrowWest,
    SoNaviCube::PickId::ArrowRight,
    SoNaviCube::PickId::ArrowLeft,
    SoNaviCube::PickId::Backside,
    SoNaviCube::PickId::Home,
    SoNaviCube::PickId::ViewMenu,
}};
}  // namespace

SO_NODE_SOURCE(SoNaviCubeVulkan);

void SoNaviCubeVulkan::initClass()
{
    SO_NODE_INIT_CLASS(SoNaviCubeVulkan, SoNaviCube, "SoNaviCube");
}

SoNaviCubeVulkan::SoNaviCubeVulkan()
{
    SO_NODE_CONSTRUCTOR(SoNaviCubeVulkan);
}

SoNaviCubeVulkan::~SoNaviCubeVulkan()
{
    if (sceneRoot) {
        sceneRoot->unref();
        sceneRoot = nullptr;
    }
}

void SoNaviCubeVulkan::setVulkanScale(float scale)
{
    vulkanScale = std::clamp(scale, 0.1F, 10.0F);
}

int SoNaviCubeVulkan::faceIndex(int pickId)
{
    switch (static_cast<PickId>(pickId)) {
        case PickId::Front: return 0;
        case PickId::Rear: return 1;
        case PickId::Right: return 2;
        case PickId::Left: return 3;
        case PickId::Top: return 4;
        case PickId::Bottom: return 5;
        default: return -1;
    }
}

#ifdef HAVE_COIN_IR_RENDER_ACTION

void SoNaviCubeVulkan::ensureScene() const
{
    if (sceneBuilt) {
        return;
    }
    sceneBuilt = true;

    sceneRoot = new SoSeparator;
    sceneRoot->ref();

    // Force filled rendering for the overlay regardless of viewer draw style.
    {
        auto* drawStyle = new SoDrawStyle;
        drawStyle->style = SoDrawStyle::FILLED;
        sceneRoot->addChild(drawStyle);
    }
    // Explicit depth so the overlay self-occludes (the backend clears the
    // scoped rect's depth before drawing the overlay pass).
    {
        auto* depth = new SoDepthBuffer;
        depth->test = TRUE;
        depth->write = TRUE;
        depth->function = SoDepthBuffer::LEQUAL;
        depth->range = SbVec2f(0.0F, 1.0F);
        sceneRoot->addChild(depth);
    }

    // Camera (ortho preferred, persp fallback) -- fixed projection like the GL
    // overlay, independent of the viewport aspect ratio.
    cameraSwitch = new SoSwitch;
    sceneRoot->addChild(cameraSwitch);

    orthoCamera = new SoOrthographicCamera;
    perspCamera = new SoPerspectiveCamera;
    cameraSwitch->addChild(orthoCamera);
    cameraSwitch->addChild(perspCamera);

    SoCamera::ViewportMapping mapping = SoCamera::LEAVE_ALONE;
    orthoCamera->viewportMapping = mapping;
    perspCamera->viewportMapping = mapping;
    orthoCamera->aspectRatio = 1.0F;
    perspCamera->aspectRatio = 1.0F;
    orthoCamera->position = SbVec3f(0.0F, 0.0F, 0.0F);
    perspCamera->position = SbVec3f(0.0F, 0.0F, 0.0F);
    orthoCamera->orientation = SbRotation();
    perspCamera->orientation = SbRotation();
    orthoCamera->nearDistance = kOverlayNear;
    orthoCamera->farDistance = kOverlayFar;
    perspCamera->nearDistance = kOverlayNear;
    perspCamera->farDistance = kOverlayFar;
    orthoCamera->focalDistance = std::abs(kOverlayCubeZ);
    perspCamera->focalDistance = std::abs(kOverlayCubeZ);
    orthoCamera->height = 2.0F * kOverlayOrthoExtent;
    const float halfAngle = std::atan(std::tan(std::numbers::pi_v<float> / 8.0F)
                                      * kOverlayFovScale);
    perspCamera->heightAngle = 2.0F * halfAngle;
    cameraSwitch->whichChild = 0;

    // Root transform: rotates the (unit) cube against the camera and pushes it
    // out to the overlay depth so it never z-fights the main scene.  The axis,
    // cube and edge separators are added AFTER it inside cubeGroup so they
    // inherit (rotate with) its transform.
    auto* cubeGroup = new SoSeparator;
    sceneRoot->addChild(cubeGroup);

    rootTransform = new SoTransform;
    cubeGroup->addChild(rootTransform);

    // ---- Axis arrows ----
    {
        auto* axisSwitch = new SoSwitch;
        cubeGroup->addChild(axisSwitch);

        constexpr float a = -1.1F;
        constexpr float b = -1.05F;
        constexpr float c = 0.5F;
        const SbVec3f segments[3][2] = {
            {SbVec3f(b, a, a), SbVec3f(c, a, a)},
            {SbVec3f(a, b, a), SbVec3f(a, c, a)},
            {SbVec3f(a, a, b), SbVec3f(a, a, c)},
        };

        for (int axis = 0; axis < 3; ++axis) {
            AxisNodes nodes;
            nodes.sep = new SoSeparator;

            nodes.material = new SoMaterial;
            nodes.sep->addChild(nodes.material);

            nodes.drawStyle = new SoDrawStyle;
            nodes.sep->addChild(nodes.drawStyle);

            nodes.vertexProperty = new SoVertexProperty;
            nodes.vertexProperty->vertex.setNum(2);
            nodes.vertexProperty->vertex.set1Value(0, segments[axis][0]);
            nodes.vertexProperty->vertex.set1Value(1, segments[axis][1]);

            nodes.line = new SoIndexedLineSet;
            nodes.line->vertexProperty = nodes.vertexProperty;
            const std::int32_t lineIdx[3] = {0, 1, -1};
            nodes.line->coordIndex.setValues(0, 3, lineIdx);
            nodes.sep->addChild(nodes.line);

            nodes.points = new SoPointSet;
            nodes.points->vertexProperty = nodes.vertexProperty;
            nodes.points->numPoints = 2;
            nodes.sep->addChild(nodes.points);

            axisSwitch->addChild(nodes.sep);
            axisNodes[axis] = nodes;
        }
    }

    // ---- Cube fill (6 faces) ----
    {
        auto* cubeSep = new SoSeparator;
        cubeGroup->addChild(cubeSep);

        cubeMaterial = new SoMaterial;
        cubeSep->addChild(cubeMaterial);

        auto* binding = new SoMaterialBinding;
        binding->value = SoMaterialBinding::PER_FACE_INDEXED;
        cubeSep->addChild(binding);

        // No polygon offset on the fill: it must write its true depth so the
        // near (front) faces actually occlude the far faces' labels.  A +1
        // offset pushed the front fill to the same depth the labels' -1 offset
        // reaches, so LEQUAL let the far labels paint right through the shell.
        auto* offset = new SoPolygonOffset;
        offset->factor = 0.0F;
        offset->units = 0.0F;
        offset->styles = SoPolygonOffset::FILLED;
        offset->on = TRUE;
        cubeSep->addChild(offset);

        // The overlay camera is the inverse of the view camera, which together
        // with the navcube's own view matrix is an opposite-handed view volume:
        // the retained IR winding (outward, matching the GL navcube's
        // COUNTERCLOCKWISE) culls the WRONG faces under the Vulkan pipeline's
        // front-face convention.  Declare CLOCKWISE so SOLID culling keeps the
        // faces toward the camera and cleanly hides the far faces (and the far
        // faces' labels bleeding through the translucent shell).
        auto* hints = new SoShapeHints;
        hints->vertexOrdering = SoShapeHints::CLOCKWISE;
        hints->shapeType = SoShapeHints::SOLID;
        hints->faceType = SoShapeHints::CONVEX;
        cubeSep->addChild(hints);

        cubeFaces = new SoIndexedFaceSet;
        cubeSep->addChild(cubeFaces);

        cubeVertexProperty = new SoVertexProperty;
        cubeVertexProperty->materialBinding = SoVertexProperty::PER_FACE_INDEXED;
        cubeFaces->vertexProperty = cubeVertexProperty;

        // Unit cube corners.
        const SbVec3f corners[8] = {
            SbVec3f(-1.0F, -1.0F, -1.0F), SbVec3f(1.0F, -1.0F, -1.0F),
            SbVec3f(1.0F, 1.0F, -1.0F),  SbVec3f(-1.0F, 1.0F, -1.0F),
            SbVec3f(-1.0F, -1.0F, 1.0F), SbVec3f(1.0F, -1.0F, 1.0F),
            SbVec3f(1.0F, 1.0F, 1.0F),   SbVec3f(-1.0F, 1.0F, 1.0F),
        };
        cubeVertexProperty->vertex.setValues(0, 8, corners);

        // Base SoNaviCube face convention: Top=+Z, Bottom=-Z, Front=-Y,
        // Rear=+Y, Right=+X, Left=-X.  The base label quads, pickAt() and the
        // per-face highlight all use this frame, so the cube must be built in
        // the SAME Z-up frame (not Y-up) or every face's label lands on the
        // wrong face.  Winding is outward-CCW (cross product of consecutive
        // edges points at the outward normal), which the CLOCKWISE declaration
        // below (compensating the IR's opposite-handed view volume) keeps
        // front-facing.
        // Front(-Y), Rear(+Y), Right(+X), Left(-X), Top(+Z), Bottom(-Z).
        const std::int32_t faces[6][5] = {
            {0, 1, 5, 4, -1},  // -Y front
            {2, 3, 7, 6, -1},  // +Y rear
            {5, 1, 2, 6, -1},  // +X right
            {0, 4, 7, 3, -1},  // -X left
            {4, 5, 6, 7, -1},  // +Z top
            {1, 0, 3, 2, -1},  // -Z bottom
        };
        cubeFaces->coordIndex.setNum(6 * 5);
        for (int f = 0; f < 6; ++f) {
            cubeFaces->coordIndex.setValues(f * 5, 5, faces[f]);
        }
        // Per-face material binding into the single cubeMaterial array.
        const std::int32_t materialIndex[6] = {0, 1, 2, 3, 4, 5};
        cubeFaces->materialIndex.setValues(0, 6, materialIndex);
    }

    // ---- Cube edges ----
    {
        auto* edgeSep = new SoSeparator;
        cubeGroup->addChild(edgeSep);

        edgeMaterial = new SoMaterial;
        edgeSep->addChild(edgeMaterial);

        edgeDrawStyle = new SoDrawStyle;
        edgeSep->addChild(edgeDrawStyle);

        edges = new SoIndexedLineSet;
        edgeSep->addChild(edges);

        edgeVertexProperty = new SoVertexProperty;
        edgeVertexProperty->vertex.setNum(8);
        const SbVec3f corners[8] = {
            SbVec3f(-1.0F, -1.0F, -1.0F), SbVec3f(1.0F, -1.0F, -1.0F),
            SbVec3f(1.0F, 1.0F, -1.0F),  SbVec3f(-1.0F, 1.0F, -1.0F),
            SbVec3f(-1.0F, -1.0F, 1.0F), SbVec3f(1.0F, -1.0F, 1.0F),
            SbVec3f(1.0F, 1.0F, 1.0F),   SbVec3f(-1.0F, 1.0F, 1.0F),
        };
        edgeVertexProperty->vertex.setValues(0, 8, corners);
        edges->vertexProperty = edgeVertexProperty;

        // 12 edges of the cube.
        const std::int32_t edgeIdx[12][3] = {
            {0, 1, -1}, {1, 2, -1}, {2, 3, -1}, {3, 0, -1},
            {4, 5, -1}, {5, 6, -1}, {6, 7, -1}, {7, 4, -1},
            {0, 4, -1}, {1, 5, -1}, {2, 6, -1}, {3, 7, -1},
        };
        edges->coordIndex.setNum(12 * 3);
        for (int i = 0; i < 12; ++i) {
            edges->coordIndex.setValues(i * 3, 3, edgeIdx[i]);
        }
    }

    // ---- Face labels (textured quads reusing the base SoNaviCube textures) --
    buildLabels(cubeGroup);

    // ---- Screen-space navigation buttons (fixed, in front of the cube) ----
    buildButtons();

    sceneRoot->touch();
}

namespace
{
//! The six navigable face labels.  The second element is the index the base
//! SoNaviCube::pickIndex() maps a face PickId to; only used to index the
//! faceLabels array here.  Order matches NaviCubeImplementation's Qt texture
//! creation (Top/Front/Left/Rear/Right/Bottom).
constexpr std::array<SoNaviCube::PickId, 6> kFaceLabelOrder = {{
    SoNaviCube::PickId::Top,
    SoNaviCube::PickId::Front,
    SoNaviCube::PickId::Left,
    SoNaviCube::PickId::Rear,
    SoNaviCube::PickId::Right,
    SoNaviCube::PickId::Bottom,
}};
}  // namespace

void SoNaviCubeVulkan::buildLabels(SoSeparator* cubeGroup) const
{
    if (!cubeGroup) {
        return;
    }

    labelsSep = new SoSeparator;
    cubeGroup->addChild(labelsSep);

    // Labels sit just on top of the faces and should not be culled.
    auto* depth = new SoDepthBuffer;
    depth->test = TRUE;
    depth->write = FALSE;
    depth->function = SoDepthBuffer::LEQUAL;
    labelsSep->addChild(depth);

    auto* offset = new SoPolygonOffset;
    offset->factor = -1.0F;
    offset->units = -1.0F;
    offset->styles = SoPolygonOffset::FILLED;
    offset->on = TRUE;
    labelsSep->addChild(offset);

    auto* hints = new SoShapeHints;
    hints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
    hints->shapeType = SoShapeHints::UNKNOWN_SHAPE_TYPE;
    hints->faceType = SoShapeHints::UNKNOWN_FACE_TYPE;
    labelsSep->addChild(hints);

    for (SoNaviCube::PickId pickId : kFaceLabelOrder) {
        std::array<SbVec3f, 4> quad {};
        const SoTexture2* texture = nullptr;
        if (!this->getLabelQuad(pickId, quad, texture)) {
            continue;
        }
        auto nodes = LabelNodes {};
        nodes.sep = new SoSeparator;

        nodes.material = new SoMaterial;
        // Labels are white text in a modulated texture; tint with emphase.
        nodes.material->diffuseColor.setValue(emphaseColor.getValue());
        nodes.sep->addChild(nodes.material);

        nodes.texture = const_cast<SoTexture2*>(texture);
        nodes.sep->addChild(nodes.texture);

        nodes.face = new SoFaceSet;
        nodes.face->numVertices.set1Value(0, 4);
        nodes.vertexProperty = new SoVertexProperty;
        nodes.face->vertexProperty = nodes.vertexProperty;
        nodes.vertexProperty->vertex.setNum(4);
        nodes.vertexProperty->texCoord.setNum(4);
        // Handling the two flipped axes the GL/OpenGL pipeline and the
        // Vulkan/IR pipeline disagree on.  The base SoNaviCube stores the label
        // image vertically flipped for GL's bottom-left texture origin; IR reads
        // rows top-to-bottom and samples the quad with its own x-axis handedness,
        // so both U and V need to be inverted relative to the GL mapping for the
        // rendered text to read the right way around.
        nodes.vertexProperty->texCoord.set1Value(0, SbVec2f(0.0F, 0.0F));
        nodes.vertexProperty->texCoord.set1Value(1, SbVec2f(1.0F, 0.0F));
        nodes.vertexProperty->texCoord.set1Value(2, SbVec2f(1.0F, 1.0F));
        nodes.vertexProperty->texCoord.set1Value(3, SbVec2f(0.0F, 1.0F));
        for (int i = 0; i < 4; ++i) {
            // The base SoNaviCube declares its faces Z-up (Top=+Z, Bottom=-Z,
            // Front=-Y, Rear=+Y) and the cube faces below are built in the same
            // Z-up frame, so the label quads land on the matching faces as-is.
            nodes.vertexProperty->vertex.set1Value(
                i, quad[static_cast<std::size_t>(i)]);
        }
        nodes.sep->addChild(nodes.face);

        labelsSep->addChild(nodes.sep);
        faceLabels[static_cast<std::size_t>(pickId)] = nodes;
    }
}

void SoNaviCubeVulkan::buildButtons() const
{
    buttonsSep = new SoSeparator;
    sceneRoot->addChild(buttonsSep);

    // UI controls: never depth-tested against (or testing) the cube, and never
    // culled (some button meshes are concave or have inconsistent winding).
    {
        auto* depth = new SoDepthBuffer;
        depth->test = FALSE;
        depth->write = FALSE;
        depth->function = SoDepthBuffer::ALWAYS;
        depth->range = SbVec2f(0.0F, 1.0F);
        buttonsSep->addChild(depth);

        auto* hints = new SoShapeHints;
        hints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
        hints->shapeType = SoShapeHints::UNKNOWN_SHAPE_TYPE;
        hints->faceType = SoShapeHints::UNKNOWN_FACE_TYPE;
        buttonsSep->addChild(hints);
    }

    SbViewVolume ortho;
    ortho.ortho(-kOverlayOrthoExtent, kOverlayOrthoExtent,
                -kOverlayOrthoExtent, kOverlayOrthoExtent,
                kOverlayNear, kOverlayFar);
    SbViewVolume persp;
    const float dim = kOverlayNear
        * static_cast<float>(std::tan(std::numbers::pi_v<float> / 8.0F))
        * kOverlayFovScale;
    persp.frustum(-dim, dim, -dim, dim, kOverlayNear, kOverlayFar);

    const float buttonPlaneDist = std::abs(kOverlayButtonZ);

    for (SoNaviCube::PickId pickId : kButtonPickIds) {
        std::vector<SbVec3f> verts;
        std::vector<int> tris;
        std::vector<std::int32_t> outline;
        if (!this->getButtonGeom(pickId, verts, tris, outline)) {
            continue;
        }
        const size_t n = verts.size();
        if (n < 3) {
            continue;
        }

        ButtonNodes nodes;
        nodes.sep = new SoSeparator;
        buttonsSep->addChild(nodes.sep);

        nodes.fillMaterial = new SoMaterial;
        nodes.sep->addChild(nodes.fillMaterial);

        nodes.coordsSwitch = new SoSwitch;
        nodes.sep->addChild(nodes.coordsSwitch);

        nodes.vertexOrtho = new SoVertexProperty;
        nodes.vertexOrtho->vertex.setNum(static_cast<int>(n));
        nodes.coordsSwitch->addChild(nodes.vertexOrtho);

        nodes.vertexPersp = new SoVertexProperty;
        nodes.vertexPersp->vertex.setNum(static_cast<int>(n));
        nodes.coordsSwitch->addChild(nodes.vertexPersp);

        for (int i = 0; i < static_cast<int>(n); ++i) {
            const SbVec3f& overlayPoint = verts[static_cast<std::size_t>(i)];
            const SbVec2f normPoint(overlayPoint[0], 1.0F - overlayPoint[1]);
            nodes.vertexOrtho->vertex.set1Value(
                i, ortho.getPlanePoint(buttonPlaneDist, normPoint));
            nodes.vertexPersp->vertex.set1Value(
                i, persp.getPlanePoint(buttonPlaneDist, normPoint));
        }

        nodes.fill = new SoIndexedFaceSet;
        {
            std::vector<std::int32_t> idx;
            if (tris.size() >= 3) {
                idx.reserve(tris.size() + (tris.size() / 3));
                for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                    idx.push_back(tris[i]);
                    idx.push_back(tris[i + 1]);
                    idx.push_back(tris[i + 2]);
                    idx.push_back(-1);
                }
            }
            else {
                const std::int32_t n32 = static_cast<std::int32_t>(n);
                idx.reserve((n32 - 2) * 4);
                for (std::int32_t i = 1; i + 1 < n32; ++i) {
                    idx.push_back(0);
                    idx.push_back(i);
                    idx.push_back(i + 1);
                    idx.push_back(-1);
                }
            }
            nodes.fill->coordIndex.setValues(0, static_cast<int>(idx.size()), idx.data());
        }
        nodes.sep->addChild(nodes.fill);

        nodes.outlineMaterial = new SoMaterial;
        nodes.sep->addChild(nodes.outlineMaterial);

        nodes.outlineDrawStyle = new SoDrawStyle;
        nodes.sep->addChild(nodes.outlineDrawStyle);

        nodes.outline = new SoIndexedLineSet;
        {
            std::vector<std::int32_t> idx;
            if (!outline.empty()) {
                idx = outline;
            }
            else {
                idx.reserve(n + 2);
                for (size_t i = 0; i < n; ++i) {
                    idx.push_back(static_cast<std::int32_t>(i));
                }
                idx.push_back(0);
                idx.push_back(-1);
            }
            nodes.outline->coordIndex.setValues(0, static_cast<int>(idx.size()), idx.data());
        }
        nodes.sep->addChild(nodes.outline);

        buttonNodes[static_cast<std::size_t>(pickId)] = nodes;
    }
}

void SoNaviCubeVulkan::updateButtons(float op, int hilitePick) const
{
    if (!buttonsSep) {
        return;
    }

    const SbColor base = baseColor.getValue();
    const SbColor hilite = hiliteColor.getValue();
    const SbColor emph = emphaseColor.getValue();
    const float baseTr = toTransparency(baseAlpha.getValue() * op);
    const float hiliteTr = toTransparency(hiliteAlpha.getValue() * op);
    const float emphTr = toTransparency(emphaseAlpha.getValue() * op);
    const float bw = borderWidth.getValue();
    const int coordChild = cameraIsOrthographic.getValue() ? 0 : 1;

    for (SoNaviCube::PickId pickId : kButtonPickIds) {
        ButtonNodes& nodes = buttonNodes[static_cast<std::size_t>(pickId)];
        if (!nodes.sep) {
            continue;
        }
        const bool isHilite = (static_cast<int>(pickId) == hilitePick);
        if (nodes.coordsSwitch && nodes.coordsSwitch->whichChild.getValue() != coordChild) {
            nodes.coordsSwitch->whichChild = coordChild;
        }
        if (nodes.fillMaterial) {
            nodes.fillMaterial->diffuseColor.setValue(isHilite ? hilite : base);
            nodes.fillMaterial->transparency.setValue(isHilite ? hiliteTr : baseTr);
            nodes.fillMaterial->touch();
        }
        if (nodes.outlineMaterial) {
            nodes.outlineMaterial->diffuseColor.setValue(emph);
            nodes.outlineMaterial->transparency.setValue(emphTr);
            nodes.outlineMaterial->touch();
        }
        if (nodes.outlineDrawStyle) {
            nodes.outlineDrawStyle->lineWidth.setValue(bw);
            nodes.outlineDrawStyle->touch();
        }
        if (nodes.fill) {
            nodes.fill->touch();
        }
        if (nodes.outline) {
            nodes.outline->touch();
        }
        if (nodes.vertexOrtho) {
            nodes.vertexOrtho->touch();
        }
        if (nodes.vertexPersp) {
            nodes.vertexPersp->touch();
        }
    }
}

void SoNaviCubeVulkan::resetLabels() const
{
    for (auto& nodes : faceLabels) {
        nodes = {};
    }
    labelsSep = nullptr;
}

void SoNaviCubeVulkan::updateScene() const
{
    if (!sceneRoot) {
        return;
    }

    cameraSwitch->whichChild = cameraIsOrthographic.getValue() ? 0 : 1;
    const SbRotation cam = cameraOrientation.getValue();
    rootTransform->rotation = cam.inverse();
    rootTransform->translation = SbVec3f(0.0F, 0.0F, kOverlayCubeZ);

    const float op = opacity.getValue();

    // Cube face colours (base + hovered highlight).
    const SbColor base = baseColor.getValue();
    const SbColor hilite = hiliteColor.getValue();
    const float baseTr = toTransparency(baseAlpha.getValue() * op);
    const float hiliteTr = toTransparency(hiliteAlpha.getValue() * op);
    const int hi = faceIndex(hiliteId.getValue());
    if (cubeMaterial) {
        cubeMaterial->diffuseColor.setNum(6);
        cubeMaterial->transparency.setNum(6);
        for (int f = 0; f < 6; ++f) {
            const bool isHilite = (f == hi);
            cubeMaterial->diffuseColor.set1Value(f, isHilite ? hilite : base);
            cubeMaterial->transparency.set1Value(f, isHilite ? hiliteTr : baseTr);
        }
    }

    if (edgeMaterial) {
        edgeMaterial->diffuseColor.setValue(emphaseColor.getValue());
        edgeMaterial->transparency.setValue(toTransparency(emphaseAlpha.getValue() * op));
    }
    if (edgeDrawStyle) {
        edgeDrawStyle->lineWidth.setValue(borderWidth.getValue());
    }

    const SbColor axisColors[3] = {
        axisXColor.getValue(),
        axisYColor.getValue(),
        axisZColor.getValue(),
    };
    const float axisTr = toTransparency(op);
    const float axisWidth = borderWidth.getValue() * 2.0F;
    for (int axis = 0; axis < 3; ++axis) {
        AxisNodes& nodes = axisNodes[axis];
        if (!nodes.material) {
            continue;
        }
        nodes.material->diffuseColor.setValue(axisColors[axis]);
        nodes.material->transparency.setValue(axisTr);
        nodes.drawStyle->lineWidth.setValue(axisWidth);
        nodes.drawStyle->pointSize.setValue(axisWidth);
        // Retained cache: the axis line/point geometry is baked with the model
        // matrix at first record, so on camera-only frames it must be re-touched
        // to re-record with the new camera transform (otherwise the X/Y/Z origin
        // markers stay frozen while the cube rotates).
        nodes.line->touch();
        nodes.points->touch();
        nodes.material->touch();
    }

    // Re-record the cube fill so the per-face highlight reaches the GPU.
    if (cubeFaces) {
        cubeFaces->touch();
    }
    if (cubeMaterial) {
        cubeMaterial->touch();
    }
    // The edge line-set geometry is likewise baked with the model matrix at
    // first record, so it must be re-touched every frame or the cube edges
    // stay frozen while the faces keep rotating under the camera transform.
    if (edges) {
        edges->touch();
    }
    if (edgeVertexProperty) {
        edgeVertexProperty->touch();
    }
    if (edgeMaterial) {
        edgeMaterial->touch();
    }
    if (edgeDrawStyle) {
        edgeDrawStyle->touch();
    }
    // Re-record the face labels so they follow the rotating cube too.
    for (SoNaviCube::PickId pickId : kFaceLabelOrder) {
        LabelNodes& nodes = faceLabels[static_cast<std::size_t>(pickId)];
        if (nodes.face) {
            nodes.face->touch();
        }
        if (nodes.vertexProperty) {
            nodes.vertexProperty->touch();
        }
        if (nodes.material) {
            nodes.material->touch();
        }
    }
    // Refresh the screen-space navigation buttons (hover highlight + opacity).
    updateButtons(op, hiliteId.getValue());
}

void SoNaviCubeVulkan::recordOverlay(SoIRRenderAction* action)
{
    const SbVec4f& rect = viewportRect.getValue();
    const float scale = vulkanScale;
    const int viewportX = static_cast<int>(std::lround(rect[0]));
    const int viewportY = static_cast<int>(std::lround(rect[1]));
    const int viewportWidth = static_cast<int>(std::lround(rect[2] * scale));
    const int viewportHeight = static_cast<int>(std::lround(rect[3] * scale));
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }

    SoState* state = action->getState();
    if (!state) {
        return;
    }

    SoDrawList& list = action->getMutableDrawList();
    const int firstCommand = list.getNumCommands();

    state->push();

    SbViewportRegion vp = SoViewportRegionElement::get(state);
    vp.setViewportPixels(viewportX, viewportY, viewportWidth, viewportHeight);
    SoViewportRegionElement::set(state, vp);

    SoLightModelElement::set(state, this, SoLightModelElement::BASE_COLOR);
    SoShapeStyleElement::setLightModel(state, SoLazyElement::BASE_COLOR);
    SoLazyElement::setLightModel(state, SoLazyElement::BASE_COLOR);

    sceneRoot->IRRender(action);

    const int count = list.getNumCommands();
    for (int i = firstCommand; i < count; ++i) {
        SoRenderCommand& cmd = list.getCommand(i);
        cmd.pass = SO_RENDERPASS_OVERLAY;
        cmd.state.raster.viewportEnabled = TRUE;
        cmd.state.raster.viewportX = viewportX;
        cmd.state.raster.viewportY = viewportY;
        cmd.state.raster.viewportWidth = viewportWidth;
        cmd.state.raster.viewportHeight = viewportHeight;
        cmd.state.raster.scissorEnabled = TRUE;
        cmd.state.raster.scissorX = viewportX;
        cmd.state.raster.scissorY = viewportY;
        cmd.state.raster.scissorWidth = viewportWidth;
        cmd.state.raster.scissorHeight = viewportHeight;
    }

    state->pop();
}

void SoNaviCubeVulkan::IRRender(SoIRRenderAction* action)
{
    if (!action) {
        return;
    }
    ensureScene();
    updateScene();
    recordOverlay(action);
}

#endif  // HAVE_COIN_IR_RENDER_ACTION
