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
#include <Inventor/actions/SoGetBoundingBoxAction.h>
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
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Gui;

namespace {

//! A convex half-space: "inside" is dot(normal, p - point) >= 0.
struct GridHalfSpace
{
    SbVec3f normal;  //!< points into the view volume
    SbVec3f point;   //!< a point on the boundary plane
};

//! Build the six inward-facing half-spaces of the view volume from its eight
//! corners (four on the near plane, four on the far plane, matching NDC order).
std::array<GridHalfSpace, 6> viewVolumeHalfSpaces(const SbVec3f (&nearC)[4],
                                                  const SbVec3f (&farC)[4])
{
    SbVec3f center(0.0F, 0.0F, 0.0F);
    for (int i = 0; i < 4; ++i) {
        center += nearC[i];
        center += farC[i];
    }
    center *= 0.125F;

    std::array<GridHalfSpace, 6> planes {};
    auto setPlane = [&center](GridHalfSpace& hs, const SbVec3f& a, const SbVec3f& b,
                              const SbVec3f& c) {
        // Orient the face normal outward (away from the volume centre), then
        // expose the inward normal for the half-space test.
        SbVec3f outward = (b - a).cross(c - a);
        if (outward.dot(a - center) < 0.0F) {
            outward = -outward;
        }
        const float len = outward.length();
        if (len > 0.0F) {
            outward /= len;
        }
        hs.normal = -outward;
        hs.point = a;
    };

    setPlane(planes[0], nearC[0], nearC[1], nearC[2]);  // near
    setPlane(planes[1], farC[0], farC[1], farC[2]);     // far
    for (int i = 0; i < 4; ++i) {                       // sides
        const int j = (i + 1) % 4;
        setPlane(planes[2 + i], nearC[i], nearC[j], farC[j]);
    }
    return planes;
}

//! Clip segment [a, b] to the convex volume.  Returns false if it is entirely
//! outside; otherwise a/b are narrowed to the visible portion.
bool clipSegmentToVolume(SbVec3f& a, SbVec3f& b,
                         const std::array<GridHalfSpace, 6>& planes)
{
    const SbVec3f orig = a;
    const SbVec3f dir = b - a;
    float t0 = 0.0F;
    float t1 = 1.0F;
    for (const GridHalfSpace& hs : planes) {
        const float da = hs.normal.dot(a - hs.point);
        const float db = hs.normal.dot(b - hs.point);
        if (da >= 0.0F && db >= 0.0F) {
            continue;
        }
        if (da < 0.0F && db < 0.0F) {
            return false;
        }
        const float t = da / (da - db);
        if (da < 0.0F) {
            t0 = std::max(t0, t);
        }
        else {
            t1 = std::min(t1, t);
        }
        if (t0 > t1) {
            return false;
        }
    }
    a = orig + dir * t0;
    b = orig + dir * t1;
    return true;
}

}  // namespace

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

void SoGroundPlane::GLRenderBelowPath(SoGLRenderAction* action)
{
    if (!enabled.getValue()) {
        return;
    }
    // Regenerate geometry before the separator traverses its line sets; the
    // child traversal reaches this method, not GLRender().
    updateGrid(action->getState());
    inherited::GLRenderBelowPath(action);
}

void SoGroundPlane::GLRenderInPath(SoGLRenderAction* action)
{
    if (!enabled.getValue()) {
        return;
    }
    updateGrid(action->getState());
    inherited::GLRenderInPath(action);
}

void SoGroundPlane::getBoundingBox(SoGetBoundingBoxAction* action)
{
    // The grid is camera-coupled: updateGrid() sizes it from the current view
    // volume.  It must NOT contribute to the *main* scene bounding box, or the
    // Vulkan viewport (which derives near/far from that bbox and publishes them
    // onto the camera) would close a loop that compounds near/far ~3x per frame
    // until the coordinates blow up.  That is guaranteed structurally instead:
    // the grid lives in the per-frame decoration scene (getDecorationRoot()),
    // not the retained main scene, so it is not part of that bbox.  Report the
    // real (bounded) child bounds here -- returning nothing makes the parent
    // separator's bounding box empty, and Coin then frustum-culls the grid away
    // in the classic Coin/GL viewport.
    inherited::getBoundingBox(action);
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

    const SbVec2f ndc[4] = {
        SbVec2f(-1.0F, -1.0F),
        SbVec2f(1.0F, -1.0F),
        SbVec2f(1.0F, 1.0F),
        SbVec2f(-1.0F, 1.0F),
    };
    const float nearDepth = viewVolume.getNearDist();

    // Size the footprint from the camera's lateral view extent, NOT from the
    // camera's far plane.  The clip-range computation includes this grid's
    // bounds (so the far plane reaches the drawn ground); a far-dependent
    // footprint would close the loop far -> footprint -> far and diverge ~3x
    // per frame.  One view size past the near plane is far enough to cover the
    // visible ground at any tilt while keeping the extent stable.
    const float viewSize = std::max(viewVolume.getWidth(), viewVolume.getHeight());
    const float farDepth = nearDepth
        + (viewSize > 0.0F ? viewSize : std::max(viewVolume.getDepth(), 1.0F));

    SbVec3f nearC[4];
    SbVec3f farC[4];
    for (int i = 0; i < 4; ++i) {
        nearC[i] = viewVolume.getPlanePoint(nearDepth, ndc[i]);
        farC[i] = viewVolume.getPlanePoint(farDepth, ndc[i]);
    }

    // The clipped grid is a pure function of the view volume.  Skip the
    // (notifying) buffer rewrite when it is unchanged -- see the header.
    if (m_gridValid) {
        bool unchanged = true;
        for (int i = 0; i < 4 && unchanged; ++i) {
            unchanged = nearC[i] == m_gridNearCorners[i]
                && farC[i] == m_gridFarCorners[i];
        }
        if (unchanged) {
            return;
        }
    }

    const std::array<GridHalfSpace, 6> planes = viewVolumeHalfSpaces(nearC, farC);

    // Lateral generation footprint on Z = 0.  The visible region is within the
    // convex hull of the frustum corners, so their XY bounds contain it.
    float minX = FLT_MAX;
    float minY = FLT_MAX;
    float maxX = -FLT_MAX;
    float maxY = -FLT_MAX;
    for (int i = 0; i < 4; ++i) {
        const SbVec3f corners[2] = {nearC[i], farC[i]};
        for (const SbVec3f& c : corners) {
            minX = std::min(minX, c[0]);
            maxX = std::max(maxX, c[0]);
            minY = std::min(minY, c[1]);
            maxY = std::max(maxY, c[1]);
        }
    }

    float centerX = 0.5F * (minX + maxX);
    float centerY = 0.5F * (minY + maxY);
    float halfWidth = 0.5F * (maxX - minX);
    float halfHeight = 0.5F * (maxY - minY);

    // Extend a little past the visible area so lines reach the viewport edge.
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

    // Clip every generated line segment to the view volume (near/far + sides).
    // This is what keeps the grid inside the depth range the camera renders:
    // an axis-aligned grid sized from the footprint alone stretches far beyond
    // [near, far] on a tilted view and would be cut away by the clip planes.
    auto emitLine = [&planes](std::vector<SbVec3f>& out, float x0, float y0, float x1,
                              float y1) {
        SbVec3f a(x0, y0, 0.0F);
        SbVec3f b(x1, y1, 0.0F);
        if (clipSegmentToVolume(a, b, planes)) {
            out.push_back(a);
            out.push_back(b);
        }
    };

    auto snapFloor = [step](float value) {
        return std::floor(value / step) * step;
    };
    auto snapCeil = [step](float value) {
        return std::ceil(value / step) * step;
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
        std::vector<SbVec3f>& target =
            isCenter ? centerVertices : (isMajor ? majorVertices : minorVertices);
        emitLine(target, lineX, startY, lineX, endY);
    }

    // Horizontal lines: constant Y, running along X.
    for (float y = startY; y <= endY + step * 0.5F; y += step) {
        const float lineY = roundf(y / step) * step;
        const bool isCenter = std::abs(lineY) < step * 0.001F;
        const int lineIndex = static_cast<int>(std::round(lineY / step));
        const bool isMajor = (lineIndex % 5) == 0;
        std::vector<SbVec3f>& target =
            isCenter ? centerVertices : (isMajor ? majorVertices : minorVertices);
        emitLine(target, startX, lineY, endX, lineY);
    }

    m_gridValid = true;
    for (int i = 0; i < 4; ++i) {
        m_gridNearCorners[i] = nearC[i];
        m_gridFarCorners[i] = farC[i];
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
