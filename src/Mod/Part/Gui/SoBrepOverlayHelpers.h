// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

#include <FCConfig.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <Base/Color.h>

#include <Inventor/actions/SoGLRenderAction.h>
#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/actions/SoIRRenderAction.h>
#endif
#include <Inventor/elements/SoDepthBufferElement.h>
#include <Inventor/elements/SoLazyElement.h>
#include <Inventor/elements/SoMaterialBindingElement.h>
#include <Inventor/elements/SoOverrideElement.h>
#include <Inventor/elements/SoPointSizeElement.h>
#include <Inventor/elements/SoPolygonOffsetElement.h>
#include <Inventor/elements/SoShapeStyleElement.h>
#include <Inventor/elements/SoTextureEnabledElement.h>
#include <Inventor/misc/SoState.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoIndexedPointSet.h>

namespace PartGui
{

/// Controls how B-rep overlay primitives interact with the scene depth buffer.
enum class OverlayDepthMode
{
    /// Keep normal occlusion so committed selection does not expose hidden geometry.
    RespectDepth,
    /// Render above model geometry for hover and preselection feedback.
    DrawOnTop,
};

//! State shared by all overlay primitive rendering: unlit, untextured,
//! material-bound OVERALL with the binding override forced on.
static inline void applyOverlayPrimitiveState(SoState* state, SoNode* node)
{
    if (!state || !node) {
        return;
    }

    SoLazyElement::setLightModel(state, SoLazyElement::BASE_COLOR);
    SoTextureEnabledElement::set(state, node, false);
    SoMaterialBindingElement::set(state, SoMaterialBindingElement::OVERALL);
    SoOverrideElement::setMaterialBindingOverride(state, node, true);
}

//! Depth interaction shared by all overlay primitive rendering.
static inline void applyOverlayDepthState(SoState* state, OverlayDepthMode depthMode)
{
    switch (depthMode) {
        case OverlayDepthMode::DrawOnTop:
            SoDepthBufferElement::set(
                state,
                FALSE,
                FALSE,
                SoDepthBufferElement::ALWAYS,
                SbVec2f(0.0f, 1.0f)
            );
            return;
        case OverlayDepthMode::RespectDepth:
            SoDepthBufferElement::set(
                state,
                TRUE,
                FALSE,
                SoDepthBufferElement::LEQUAL,
                SbVec2f(0.0f, 1.0f)
            );
            return;
    }
}

//! Render-dispatch: the overlay helpers are templated on the action type;
//! these overloads forward to the matching node render entry point so the
//! GL and IR paths share a single implementation.
#ifdef HAVE_COIN_IR_RENDER_ACTION
static inline void renderOverlayNode(SoIndexedPointSet* node, SoIRRenderAction* action)
{
    node->IRRender(action);
}
static inline void renderOverlayNode(SoIndexedLineSet* node, SoIRRenderAction* action)
{
    node->IRRender(action);
}
static inline void renderOverlayNode(SoIndexedFaceSet* node, SoIRRenderAction* action)
{
    node->IRRender(action);
}
#endif
static inline void renderOverlayNode(SoIndexedPointSet* node, SoGLRenderAction* action)
{
    node->GLRender(action);
}
static inline void renderOverlayNode(SoIndexedLineSet* node, SoGLRenderAction* action)
{
    node->GLRender(action);
}
static inline void renderOverlayNode(SoIndexedFaceSet* node, SoGLRenderAction* action)
{
    node->GLRender(action);
}

//! Records/draws the overlay point set.  Templated on the render action so
//! GLRender and IRRender stay compile-time identical.
template <typename Action>
static void renderOverlayPoints(
    Action* action,
    SoIndexedPointSet* pointSet,
    const int32_t* indices,
    int numIndices,
    const SbColor& color,
    OverlayDepthMode depthMode
)
{
    if (!action || !pointSet || !indices || numIndices <= 0) {
        return;
    }

    std::vector<int32_t> pointIndices;
    pointIndices.reserve(static_cast<size_t>(numIndices) + 1);

    for (int i = 0; i < numIndices; i++) {
        const int32_t idx = indices[i];
        if (idx >= 0) {
            pointIndices.push_back(idx);
        }
    }
    pointIndices.push_back(-1);

    if (pointIndices.size() <= 1) {
        return;
    }

    auto state = action->getState();
    state->push();

    applyOverlayPrimitiveState(state, pointSet);
    applyOverlayDepthState(state, depthMode);

    SoLazyElement::setEmissive(state, &color);
    uint32_t packedColor = color.getPackedValue(0.0);
    SoLazyElement::setPacked(state, pointSet, 1, &packedColor, false);

    float ps = SoPointSizeElement::get(state);
    if (ps < 4.0f) {
        SoPointSizeElement::set(state, pointSet, 4.0f);
    }

    // setValues() does not shrink the field, so rewrite the overlay index array
    // to the exact size to avoid stale points from the previous overlay render.
    pointSet->coordIndex.setNum(static_cast<int>(pointIndices.size()));
    int32_t* coordIndex = pointSet->coordIndex.startEditing();
    std::copy(pointIndices.begin(), pointIndices.end(), coordIndex);
    pointSet->coordIndex.finishEditing();
    renderOverlayNode(pointSet, action);

    state->pop();
}

//! Records/draws the overlay line set.  Templated on the render action so
//! GLRender and IRRender stay compile-time identical.
template <typename Action>
static void renderOverlayLines(
    Action* action,
    SoIndexedLineSet* lineSet,
    const int32_t* indices,
    int numIndices,
    const Base::Color& color,
    OverlayDepthMode depthMode
)
{
    if (!action || !lineSet || !indices || numIndices <= 0) {
        return;
    }

    // Match the legacy GL path by drawing each edge segment independently.
    std::vector<int32_t> lineIndices;
    lineIndices.reserve(static_cast<size_t>(numIndices) * 3);

    int32_t previous = -1;
    for (int i = 0; i < numIndices; i++) {
        const int32_t current = indices[i];
        if (current < 0) {
            previous = -1;
            continue;
        }
        if (previous >= 0) {
            lineIndices.push_back(previous);
            lineIndices.push_back(current);
            lineIndices.push_back(-1);
        }
        previous = current;
    }

    if (lineIndices.empty()) {
        return;
    }

    auto state = action->getState();
    state->push();

    applyOverlayPrimitiveState(state, lineSet);
    applyOverlayDepthState(state, depthMode);

    const SbColor sbColor(color.r, color.g, color.b);
    const float transparency = std::max(0.0f, 1.0f - color.a);
    const bool hasTransparency = transparency > 0.0f;
    if (hasTransparency) {
        SoShapeStyleElement::setTransparencyType(state, SoGLRenderAction::BLEND);
        SoLazyElement::setTransparencyType(state, SoGLRenderAction::BLEND);
    }

    SoLazyElement::setEmissive(state, &sbColor);
    uint32_t packedColor = sbColor.getPackedValue(transparency);
    SoLazyElement::setPacked(state, lineSet, 1, &packedColor, hasTransparency);

    // setValues() does not shrink the field, so rewrite the overlay index
    // array to the exact size to avoid stale segments from the previous
    // highlight.
    lineSet->coordIndex.setNum(static_cast<int>(lineIndices.size()));
    int32_t* coordIndex = lineSet->coordIndex.startEditing();
    std::copy(lineIndices.begin(), lineIndices.end(), coordIndex);
    lineSet->coordIndex.finishEditing();
    renderOverlayNode(lineSet, action);

    state->pop();
}

//! SbColor overload forwarding to the Base::Color implementation.
template <typename Action>
static void renderOverlayLines(
    Action* action,
    SoIndexedLineSet* lineSet,
    const int32_t* indices,
    int numIndices,
    const SbColor& color,
    OverlayDepthMode depthMode
)
{
    renderOverlayLines(
        action,
        lineSet,
        indices,
        numIndices,
        Base::Color(color[0], color[1], color[2], 1.0f),
        depthMode
    );
}

//! Groups per-line color overrides and records/draws them as on-top overlay
//! segments.  Templated on the render action for GL/IR parity.
template <typename Action>
static void renderColorOverrides(
    Action* action,
    SoIndexedLineSet* lineSet,
    const int32_t* indices,
    int numIndices,
    const std::map<int, Base::Color>& colors
)
{
    if (!action || !lineSet || !indices || numIndices <= 0 || colors.empty()) {
        return;
    }

    struct ColorGroup
    {
        Base::Color color;
        std::vector<int32_t> indices;
    };

    std::map<uint32_t, ColorGroup> colorGroups;
    const auto wildcard = colors.find(-1);

    int lineIndex = 0;
    for (int i = 0; i < numIndices; ++lineIndex) {
        const int sectionStart = i;
        while (i < numIndices && indices[i] >= 0) {
            ++i;
        }

        const Base::Color* color = nullptr;
        auto it = colors.find(lineIndex);
        if (it != colors.end()) {
            color = &it->second;
        }
        else if (wildcard != colors.end()) {
            color = &wildcard->second;
        }

        if (color) {
            const SbColor sbColor(color->r, color->g, color->b);
            const uint32_t key = sbColor.getPackedValue(std::max(0.0f, 1.0f - color->a));
            auto& group = colorGroups[key];
            if (group.indices.empty()) {
                group.color = *color;
            }
            group.indices.insert(group.indices.end(), indices + sectionStart, indices + i);
            group.indices.push_back(-1);
        }

        if (i < numIndices && indices[i] < 0) {
            ++i;
        }
    }

    for (const auto& [_, group] : colorGroups) {
        renderOverlayLines(
            action,
            lineSet,
            group.indices.data(),
            static_cast<int>(group.indices.size()),
            group.color,
            OverlayDepthMode::DrawOnTop
        );
    }
}

//! Records/draws the overlay face set.  Templated on the render action so
//! GLRender and IRRender stay compile-time identical.
template <typename Action>
static void renderOverlayFaces(
    Action* action,
    SoIndexedFaceSet* faceSet,
    const std::vector<int32_t>& coordIndex,
    const SbColor& color,
    bool onTop
)
{
    if (!action || !faceSet || coordIndex.empty()) {
        return;
    }

    auto state = action->getState();
    state->push();

    applyOverlayPrimitiveState(state, faceSet);

    if (onTop) {
        applyOverlayDepthState(state, OverlayDepthMode::DrawOnTop);
        SoShapeStyleElement::setTransparencyType(state, SoGLRenderAction::BLEND);
        SoLazyElement::setTransparencyType(state, SoGLRenderAction::BLEND);
    }
    else {
        SoPolygonOffsetElement::set(
            state, faceSet, -0.00001f, -1.0f, SoPolygonOffsetElement::FILLED, TRUE
        );
        applyOverlayDepthState(state, OverlayDepthMode::RespectDepth);
    }

    SoLazyElement::setEmissive(state, &color);
    const uint32_t packed = color.getPackedValue(0.0f);
    SoLazyElement::setPacked(state, faceSet, 1, &packed, false);

    faceSet->coordIndex.setValues(0, static_cast<int32_t>(coordIndex.size()), coordIndex.data());
    renderOverlayNode(faceSet, action);

    state->pop();
}

//! Copy the render contexts so an IR traversal never mutates the shared
//! selection contexts.  Qt 6 runs the traversal on the GUI thread, but the
//! contexts are shared with GLRender() and the selection logic, and the IR
//! path works on throwaway copies that only need to be consistent within
//! the traversal.
template <typename CtxPtr>
static inline void copyIRRenderContexts(CtxPtr& ctx, CtxPtr& ctx2)
{
    if (ctx) {
        ctx = std::dynamic_pointer_cast<typename CtxPtr::element_type>(ctx->copy());
    }
    if (ctx2) {
        ctx2 = std::dynamic_pointer_cast<typename CtxPtr::element_type>(ctx2->copy());
    }
}

//! Clarify-selection on-top pass shared by the IR (Vulkan) renderers: pushes
//! the depth-off state, records the inherited geometry plus the highlight
//! overlay, and pops the state.  GL defers the highlighted path to the
//! delayed-annotations pass; the IR path records the same geometry inline
//! with the depth test off and the backend draws such commands last.  The
//! two render entry points are passed as lambdas so callers can forward to
//! their (private) member functions without exposing them.
#ifdef HAVE_COIN_IR_RENDER_ACTION
template <typename CtxPtr, typename VP, typename FnHighlight, typename FnInherited>
static inline void renderClarifySelectionIR(
    SoIRRenderAction* action,
    CtxPtr ctx,
    VP* viewProvider,
    FnHighlight&& renderHighlightIR,
    FnInherited&& renderInheritedIR)
{
    if (viewProvider) {
        viewProvider->setFaceHighlightActive(true);
    }
    SoState* state = action->getState();
    state->push();
    SoDepthBufferElement::set(
        state, FALSE, FALSE, SoDepthBufferElement::ALWAYS, SbVec2f(0.0f, 1.0f)
    );
    renderInheritedIR(action);
    renderHighlightIR(action, ctx);
    state->pop();
}
#endif

}  // namespace PartGui
