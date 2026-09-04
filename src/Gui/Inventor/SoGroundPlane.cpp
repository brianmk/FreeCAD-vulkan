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

#include "PreCompiled.h"

#include "SoGroundPlane.h"

#include <Inventor/SbViewVolume.h>
#include <Inventor/actions/SoGLRenderAction.h>
#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/actions/SoIRRenderAction.h>
#endif
#include <Inventor/elements/SoViewVolumeElement.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoTransparencyType.h>
#include <Inventor/nodes/SoVertexProperty.h>
#include <Inventor/sensors/SoFieldSensor.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Gui;

SO_NODE_SOURCE(SoGroundPlane);

void SoGroundPlane::initClass()
{
    SO_NODE_INIT_CLASS(SoGroundPlane, SoSeparator, "Separator");
}

SoGroundPlane::SoGroundPlane()
{
    SO_NODE_CONSTRUCTOR(SoGroundPlane);

    SO_NODE_ADD_FIELD(enabled, (TRUE));
    SO_NODE_ADD_FIELD(transparency, (0.85F));
    SO_NODE_ADD_FIELD(extentFactor, (1.5F));

    const SbColor centerColor(0.55F, 0.55F, 0.65F);
    const SbColor majorColor(0.55F, 0.55F, 0.62F);
    const SbColor minorColor(0.58F, 0.58F, 0.64F);

    auto* centerStyle = new SoDrawStyle;
    centerStyle->lineWidth = 2.0F;
    auto* majorStyle = new SoDrawStyle;
    majorStyle->lineWidth = 1.0F;
    auto* minorStyle = new SoDrawStyle;
    minorStyle->lineWidth = 1.0F;

    m_centerLineMaterial = new SoMaterial;
    m_centerLineMaterial->diffuseColor.setValue(centerColor);
    m_minorLineMaterial = new SoMaterial;
    m_minorLineMaterial->diffuseColor.setValue(minorColor);
    m_majorLineMaterial = new SoMaterial;
    m_majorLineMaterial->diffuseColor.setValue(majorColor);

    m_centerVertexProperty = new SoVertexProperty;
    m_centerLineSet = new SoLineSet;
    m_centerLineSet->vertexProperty.setValue(m_centerVertexProperty);

    m_minorVertexProperty = new SoVertexProperty;
    m_minorLineSet = new SoLineSet;
    m_minorLineSet->vertexProperty.setValue(m_minorVertexProperty);

    m_majorVertexProperty = new SoVertexProperty;
    m_majorLineSet = new SoLineSet;
    m_majorLineSet->vertexProperty.setValue(m_majorVertexProperty);

    auto* transparencyType = new SoTransparencyType;
    transparencyType->value = SoTransparencyType::BLEND;

    addChild(transparencyType);
    addChild(centerStyle);
    addChild(m_centerLineMaterial);
    addChild(m_centerLineSet);
    addChild(majorStyle);
    addChild(m_majorLineMaterial);
    addChild(m_majorLineSet);
    addChild(minorStyle);
    addChild(m_minorLineMaterial);
    addChild(m_minorLineSet);

    m_transparencySensor = new SoFieldSensor(SoGroundPlane::transparencyChanged, this);
    m_transparencySensor->attach(&transparency);
    applyTransparency();
}

SoGroundPlane::~SoGroundPlane()
{
    m_transparencySensor->detach();
    delete m_transparencySensor;
    m_transparencySensor = nullptr;
}

void SoGroundPlane::transparencyChanged(
    void* data,
    SoSensor* // sensor
)
{
    if (auto* self = static_cast<SoGroundPlane*>(data)) {
        self->applyTransparency();
    }
}

void SoGroundPlane::applyTransparency()
{
    const float t = std::clamp(transparency.getValue(), 0.0F, 1.0F);
    m_centerLineMaterial->transparency.setValue(t);
    m_majorLineMaterial->transparency.setValue(std::min(1.0F, t * 1.05F));
    m_minorLineMaterial->transparency.setValue(std::min(1.0F, t + 0.05F));
}

void SoGroundPlane::GLRender(SoGLRenderAction* action)
{
    if (!enabled.getValue()) {
        return;
    }
    updateGrid(action->getState());
    inherited::GLRender(action);
}

void SoGroundPlane::updateGrid(SoState* state)
{
    if (!state) {
        return;
    }

    const SbViewVolume& viewVolume = SoViewVolumeElement::get(state);
    if (viewVolume.getDepth() <= 0.0F) {
        return;
    }

    // Project the camera's view volume onto the Z = 0 plane to determine the
    // rectangular footprint the ground grid needs to cover. Use both near and
    // far corners so the grid fills the whole visible area independently of
    // the camera position.
    std::vector<SbVec3f> worldCorners;
    worldCorners.reserve(16);

    const float nearDepth = viewVolume.getNearDist();
    const float farDepth = nearDepth + viewVolume.getDepth();

    auto collectCorners = [&](float depth) {
        if (depth <= 0.0F) {
            return;
        }
        // Sample the four normalized view corners lying on this depth plane
        // instead of relying on the (private) getPlaneRectangle.
        const SbVec2f normCorners[] = {
            SbVec2f(-1.0F, -1.0F),
            SbVec2f(1.0F, -1.0F),
            SbVec2f(1.0F, 1.0F),
            SbVec2f(-1.0F, 1.0F),
        };
        for (const SbVec2f& corner : normCorners) {
            worldCorners.push_back(viewVolume.getPlanePoint(depth, corner));
        }
    };

    collectCorners(nearDepth);
    collectCorners(farDepth);

    if (worldCorners.empty()) {
        return;
    }

    // Gather the XY footprint of all sampled corners (grid lies on Z = 0).
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    float maxX = -FLT_MAX;
    float maxY = -FLT_MAX;
    for (const SbVec3f& c : worldCorners) {
        minX = std::min(minX, c[0]);
        maxX = std::max(maxX, c[0]);
        minY = std::min(minY, c[1]);
        maxY = std::max(maxY, c[1]);
    }

    float centerX = 0.5F * (minX + maxX);
    float centerY = 0.5F * (minY + maxY);
    float halfWidth = 0.5F * (maxX - minX);
    float halfHeight = 0.5F * (maxY - minY);

    // Apply the extent factor so the grid extends a bit past the visible area.
    const float factor = std::max(extentFactor.getValue(), 0.1F);
    halfWidth *= factor;
    halfHeight *= factor;

    const float maxHalf = std::max(halfWidth, halfHeight);
    if (maxHalf <= 0.0F) {
        return;
    }

    // Choose a "nice" step so we get roughly gridDivisions divisions across the
    // widest dimension regardless of zoom distance.
    constexpr int gridDivisions = 20;
    const float rawStep = (2.0F * maxHalf) / static_cast<float>(gridDivisions);
    const float step = niceStep(rawStep);
    if (step <= 0.0F) {
        return;
    }

    // Build line segments for the grid. Minor lines sit on each "nice" multiple;
    // major lines are drawn every 5th line to help orientation; the two lines
    // crossing the origin are drawn thick to mimic the X/Y axes.
    std::vector<SbVec3f> minorVertices;
    std::vector<SbVec3f> majorVertices;
    std::vector<SbVec3f> centerVertices;

    auto snapFloor = [step](float value) {
        return std::floor(value / step) * step;
    };
    auto snapCeil = [step](float value) {
        return std::ceil(value / step) * step;
    };

    auto addLine = [](std::vector<SbVec3f>& out,
                      float x0,
                      float y0,
                          float x1,
                          float y1,
                          bool crossesOriginX,
                          bool crossesOriginY) {
        // A constant coordinate that is exactly a multiple of step may not be
        // aligned with the origin; only the zero line is treated as a center one.
        (void)crossesOriginX;
        (void)crossesOriginY;
        out.emplace_back(x0, y0, 0.0F);
        out.emplace_back(x1, y1, 0.0F);
    };

    const float startX = snapFloor(centerX - halfWidth);
    const float endX = snapCeil(centerX + halfWidth);
    const float startY = snapFloor(centerY - halfHeight);
    const float endY = snapCeil(centerY + halfHeight);

    // Vertical lines: constant X, running along Y.
    for (float x = startX; x <= endX + step * 0.5F; x += step) {
        const float lineX = roundf(x / step) * step;
        const bool isCenter = std::abs(lineX) < step * 0.001F;
        const int lineIndex = static_cast<int>(std::round(lineX / step));
        const bool isMajor = (lineIndex % 5) == 0;
        if (isCenter) {
            addLine(centerVertices, lineX, startY, lineX, endY, true, false);
        }
        else if (isMajor) {
            addLine(majorVertices, lineX, startY, lineX, endY, false, false);
        }
        else {
            addLine(minorVertices, lineX, startY, lineX, endY, false, false);
        }
    }

    // Horizontal lines: constant Y, running along X.
    for (float y = startY; y <= endY + step * 0.5F; y += step) {
        const float lineY = roundf(y / step) * step;
        const bool isCenter = std::abs(lineY) < step * 0.001F;
        const int lineIndex = static_cast<int>(std::round(lineY / step));
        const bool isMajor = (lineIndex % 5) == 0;
        if (isCenter) {
            addLine(centerVertices, startX, lineY, endX, lineY, false, true);
        }
        else if (isMajor) {
            addLine(majorVertices, startX, lineY, endX, lineY, false, false);
        }
        else {
            addLine(minorVertices, startX, lineY, endX, lineY, false, false);
        }
    }

    auto fillLineSet = [](SoLineSet* lineSet,
                          SoVertexProperty* vp,
                          std::vector<SbVec3f>& vertices) {
        const std::size_t count = vertices.size();
        const std::size_t lineCount = count / 2;
        vp->vertex.setValues(0, static_cast<int>(count), vertices.data());
        std::vector<int32_t> numVertices(lineCount, 2);
        lineSet->numVertices.setValues(0, static_cast<int>(lineCount), numVertices.data());
    };

    fillLineSet(m_centerLineSet, m_centerVertexProperty, centerVertices);
    fillLineSet(m_majorLineSet, m_majorVertexProperty, majorVertices);
    fillLineSet(m_minorLineSet, m_minorVertexProperty, minorVertices);
}

float SoGroundPlane::niceStep(float raw)
{
    if (raw <= 0.0F) {
        return 1.0F;
    }
    // Round to the nearest 1/2/5 * 10^k for a natural drafting-grid step.
    const float exponent = std::floor(std::log10(raw));
    const float base = std::pow(10.0F, exponent);
    const float normalized = raw / base;
    float nice;
    if (normalized < 1.5F) {
        nice = 1.0F;
    }
    else if (normalized < 3.5F) {
        nice = 2.5F;
    }
    else if (normalized < 7.5F) {
        nice = 5.0F;
    }
    else {
        nice = 10.0F;
    }
    return nice * base;
}

#ifdef HAVE_COIN_IR_RENDER_ACTION
void SoGroundPlane::IRRender(SoIRRenderAction* action)
{
    if (!enabled.getValue()) {
        return;
    }
    // Regenerate geometry from the state before the IR traversal records the
    // draw list, so the recorded commands stay in sync with the camera.
    updateGrid(action->getState());
    inherited::IRRender(action);
}
#endif
