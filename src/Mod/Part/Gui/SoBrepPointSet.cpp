// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2011 Werner Mayer <wmayer[at]users.sourceforge.net>
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
#include <Base/VulkanBreadcrumbs.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <Inventor/actions/SoGetBoundingBoxAction.h>
#include <Inventor/actions/SoGLRenderAction.h>
#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/actions/SoIRRenderAction.h>
#endif
#include <Inventor/details/SoPointDetail.h>
#include <Inventor/elements/SoCoordinateElement.h>
#include <Inventor/elements/SoDepthBufferElement.h>
#include <Inventor/elements/SoLazyElement.h>
#include <Inventor/elements/SoMaterialBindingElement.h>
#include <Inventor/elements/SoOverrideElement.h>
#include <Inventor/elements/SoPointSizeElement.h>
#include <Inventor/elements/SoTextureEnabledElement.h>
#include <Inventor/errors/SoDebugError.h>
#include <Inventor/misc/SoState.h>
#include <Inventor/nodes/SoIndexedPointSet.h>

#include <Gui/Selection/SoFCUnifiedSelection.h>
#include <Gui/Inventor/So3DAnnotation.h>

#include "ViewProviderExt.h"
#include "SoBrepOverlayHelpers.h"
#include "SoBrepPointSet.h"


using namespace PartGui;

SO_NODE_SOURCE(SoBrepPointSet)

void SoBrepPointSet::initClass()
{
    SO_NODE_INIT_CLASS(SoBrepPointSet, SoPointSet, "PointSet");
}

SoBrepPointSet::SoBrepPointSet()
    : selContext(std::make_shared<SelContext>())
    , selContext2(std::make_shared<SelContext>())
{
    SO_NODE_CONSTRUCTOR(SoBrepPointSet);
    SO_NODE_ADD_FIELD(highlightCoordIndex, (0));
    SO_NODE_ADD_FIELD(selectionCoordIndex, (0));
    SO_NODE_ADD_FIELD(highlightColor, (SbColor(1.0f, 0.0f, 0.0f)));
    SO_NODE_ADD_FIELD(selectionColor, (SbColor(0.0f, 0.6f, 0.0f)));

    highlightCoordIndex.setNum(0);
    selectionCoordIndex.setNum(0);
    overlayPointSet = new SoIndexedPointSet;
    overlayPointSet->ref();
}

SoBrepPointSet::~SoBrepPointSet()
{
    if (overlayPointSet) {
        overlayPointSet->unref();
        overlayPointSet = nullptr;
    }
}

void SoBrepPointSet::GLRender(SoGLRenderAction* action)
{
    auto state = action->getState();
    selCounter.checkRenderCache(state);

    const SoCoordinateElement* coords = SoCoordinateElement::getInstance(state);
    int num = coords->getNum() - this->startIndex.getValue();
    if (num < 0) {
        // Fixes: #0000545: Undo revolve causes crash 'illegal storage'
        return;
    }
    SelContextPtr ctx2;
    SelContextPtr ctx = Gui::SoFCSelectionRoot::getRenderContext<SelContext>(this, selContext, ctx2);
    if (ctx2 && ctx2->selectionIndex.empty()) {
        return;
    }
    if (selContext2->checkGlobal(ctx)) {
        ctx = selContext2;
    }


    bool hasContextHighlight = ctx && ctx->isHighlighted() && !ctx->isHighlightAll()
        && ctx->highlightIndex >= 0;
    // for clarifyselection, add this node to delayed path if it is highlighted and render it on
    // top of everything else (highest priority)
    if (Gui::Selection().isClarifySelectionActive() && hasContextHighlight
        && !Gui::SoDelayedAnnotationsElement::isProcessingDelayedPaths) {
        if (viewProvider) {
            viewProvider->setFaceHighlightActive(true);
        }
        Gui::SoDelayedAnnotationsElement::addDelayedPath(
            action->getState(),
            action->getCurPath()->copy(),
            300
        );
        return;
    }

    if (ctx && ctx->highlightIndex == std::numeric_limits<int>::max() && !ctx->isSelectAll()) {
        if (ctx->selectionIndex.empty()) {
            if (ctx2) {
                ctx2->selectionColor = ctx->highlightColor;
                renderSelection(action, ctx2);
            }
            else {
                renderHighlight(action, ctx);
            }
        }
        else {
            if (!action->isRenderingDelayedPaths()) {
                renderSelection(action, ctx);
            }
            if (ctx2) {
                ctx2->selectionColor = ctx->highlightColor;
                renderSelection(action, ctx2);
            }
            else {
                renderHighlight(action, ctx);
            }
            if (action->isRenderingDelayedPaths()) {
                renderSelection(action, ctx);
            }
        }
        return;
    }

    if (!action->isRenderingDelayedPaths()) {
        renderHighlight(action, ctx);
    }
    if (ctx && !ctx->selectionIndex.empty()) {
        if (ctx->isSelectAll()) {
            if (ctx2 && !ctx2->selectionIndex.empty()) {
                ctx2->selectionColor = ctx->selectionColor;
                renderSelection(action, ctx2);
            }
            else {
                renderSelection(action, ctx);
            }
            if (action->isRenderingDelayedPaths()) {
                renderHighlight(action, ctx);
            }
            return;
        }
        if (!action->isRenderingDelayedPaths()) {
            renderSelection(action, ctx);
        }
    }
    if (ctx2 && !ctx2->selectionIndex.empty()) {
        renderSelection(action, ctx2, false);
    }
    else if (Gui::SoDelayedAnnotationsElement::isProcessingDelayedPaths) {
        state->push();
        SoDepthBufferElement::set(state, FALSE, FALSE, SoDepthBufferElement::ALWAYS, SbVec2f(0.0f, 1.0f));
        inherited::GLRender(action);
        state->pop();
    }
    else {
        inherited::GLRender(action);
    }

    // Workaround for #0000433
    // #if !defined(FC_OS_WIN32)
    if (!action->isRenderingDelayedPaths()) {
        renderHighlight(action, ctx);
    }
    if (ctx && !ctx->selectionIndex.empty()) {
        renderSelection(action, ctx);
    }
    if (action->isRenderingDelayedPaths()) {
        renderHighlight(action, ctx);
    }
    // #endif

    // Optional overlay rendering for deterministic tests (and programmatic usage).
    const int hlNum = highlightCoordIndex.getNum();
    if (hlNum > 0) {
        renderOverlayPoints(
            action,
            overlayPointSet,
            highlightCoordIndex.getValues(0),
            hlNum,
            highlightColor.getValue(),
            OverlayDepthMode::DrawOnTop
        );
    }
    const int selNum = selectionCoordIndex.getNum();
    if (selNum > 0) {
        renderOverlayPoints(
            action,
            overlayPointSet,
            selectionCoordIndex.getValues(0),
            selNum,
            selectionColor.getValue(),
            OverlayDepthMode::DrawOnTop
        );
    }
}

#ifdef HAVE_COIN_IR_RENDER_ACTION
void SoBrepPointSet::IRRender(SoIRRenderAction* action)
{
    VK_BREADCRUMB_LIMITED(30, "[VK-TRACE] SoBrepPointSet::IRRender this=%p\n", this);
    auto state = action->getState();
    selCounter.checkRenderCache(state);

    const SoCoordinateElement* coords = SoCoordinateElement::getInstance(state);
    int num = coords->getNum() - this->startIndex.getValue();
    if (num < 0) {
        return;
    }

    SelContextPtr ctx2;
    SelContextPtr ctx = Gui::SoFCSelectionRoot::getRenderContext<SelContext>(this, selContext, ctx2);
    copyIRRenderContexts(ctx, ctx2);
    VK_BREADCRUMB_LIMITED(30,
                          "[VK-TRACE] SoBrepPointSet::IRRender ctx=%p hlIdx=%d selIdx=%zu "
                          "ctx2=%p ctx2selIdx=%zu\n",
                          ctx.get(), ctx ? ctx->highlightIndex : -2,
                          ctx ? ctx->selectionIndex.size() : (size_t)-1,
                          ctx2.get(), ctx2 ? ctx2->selectionIndex.size() : (size_t)-1);
    // Parity with GLRender(): an existing-but-empty secondary context
    // inhibits drawing of the base geometry in partial-render scenarios.
    if (ctx2 && ctx2->selectionIndex.empty()) {
        return;
    }
    // Clarify selection: render the highlighted points on top of everything
    // else.  GL registers the node for the delayed-annotations pass; the IR
    // path records the points inline with the depth test off and the
    // backend draws such commands last.
    const bool hasContextHighlight = ctx && ctx->isHighlighted() && !ctx->isHighlightAll()
        && ctx->highlightIndex >= 0;
    if (Gui::Selection().isClarifySelectionActive() && hasContextHighlight) {
        renderClarifySelectionIR(action,
                                 ctx,
                                 viewProvider,
                                 &SoBrepPointSet::renderHighlightIR,
                                 &SoPointSet::IRRender,
                                 this);
        return;
    }
    SelContextPtr globalCtx = selContext2
        ? std::dynamic_pointer_cast<SelContext>(selContext2->copy())
        : SelContextPtr();
    if (globalCtx && globalCtx->checkGlobal(ctx)) {
        ctx = globalCtx;
    }

    inherited::IRRender(action);

    if (ctx && ctx->highlightIndex == std::numeric_limits<int>::max() && !ctx->isSelectAll()) {
        if (ctx->selectionIndex.empty()) {
            if (ctx2) {
                ctx2->selectionColor = ctx->highlightColor;
                renderSelectionIR(action, ctx2);
            }
            else {
                renderHighlightIR(action, ctx);
            }
        }
        else {
            renderSelectionIR(action, ctx);
            if (ctx2) {
                ctx2->selectionColor = ctx->highlightColor;
                renderSelectionIR(action, ctx2);
            }
            else {
                renderHighlightIR(action, ctx);
            }
        }
        return;
    }

    renderHighlightIR(action, ctx);
    if (ctx && !ctx->selectionIndex.empty()) {
        if (ctx->isSelectAll()) {
            if (ctx2 && !ctx2->selectionIndex.empty()) {
                ctx2->selectionColor = ctx->selectionColor;
                renderSelectionIR(action, ctx2);
            }
            else {
                renderSelectionIR(action, ctx);
            }
            return;
        }
        renderSelectionIR(action, ctx);
    }
    if (ctx2 && !ctx2->selectionIndex.empty()) {
        renderSelectionIR(action, ctx2);
    }

    // Optional overlay rendering for deterministic tests (and programmatic usage).
    const int hlNum = highlightCoordIndex.getNum();
    if (hlNum > 0) {
        renderOverlayPoints(
            action,
            overlayPointSet,
            highlightCoordIndex.getValues(0),
            hlNum,
            highlightColor.getValue(),
            OverlayDepthMode::DrawOnTop
        );
    }
    const int selNum = selectionCoordIndex.getNum();
    if (selNum > 0) {
        renderOverlayPoints(
            action,
            overlayPointSet,
            selectionCoordIndex.getValues(0),
            selNum,
            selectionColor.getValue(),
            OverlayDepthMode::DrawOnTop
        );
    }
}
#endif

void SoBrepPointSet::GLRenderBelowPath(SoGLRenderAction* action)
{
    inherited::GLRenderBelowPath(action);
}

void SoBrepPointSet::getBoundingBox(SoGetBoundingBoxAction* action)
{

    SelContextPtr ctx2 = Gui::SoFCSelectionRoot::getSecondaryActionContext<SelContext>(action, this);
    if (!ctx2 || ctx2->isSelectAll()) {
        inherited::getBoundingBox(action);
        return;
    }

    if (ctx2->selectionIndex.empty()) {
        return;
    }

    auto state = action->getState();
    auto coords = SoCoordinateElement::getInstance(state);
    const SbVec3f* coords3d = coords->getArrayPtr3();
    int numverts = coords->getNum();
    int startIndex = this->startIndex.getValue();

    SbBox3f bbox;
    for (auto idx : ctx2->selectionIndex) {
        if (idx >= startIndex && idx < numverts) {
            bbox.extendBy(coords3d[idx]);
        }
    }

    if (!bbox.isEmpty()) {
        action->extendBy(bbox);
    }
}

//! Shared decision logic for renderHighlight()/renderHighlightIR(): which
//! point indices the highlight covers.
static bool collectHighlightPoints(const SoBrepPointSet* node,
                                   SoState* state,
                                   Gui::SoFCSelectionContextPtr ctx,
                                   std::vector<int32_t>& pointIndices)
{
    if (!ctx || ctx->highlightIndex < 0) {
        return false;
    }

    const SoCoordinateElement* coords = SoCoordinateElement::getInstance(state);
    if (!coords) {
        return false;
    }

    const int startIndex = node->startIndex.getValue();
    const int id = ctx->highlightIndex;
    if (id == std::numeric_limits<int>::max()) {
        pointIndices.reserve(coords->getNum() - startIndex);
        for (int idx = startIndex; idx < coords->getNum(); ++idx) {
            pointIndices.push_back(static_cast<int32_t>(idx));
        }
        return true;
    }
    if (id < startIndex || id >= coords->getNum()) {
        SoDebugError::postWarning("SoBrepPointSet::collectHighlightPoints",
                                  "highlightIndex out of range");
        return false;
    }
    pointIndices.push_back(static_cast<int32_t>(id));
    return true;
}

//! Shared decision logic for renderSelection()/renderSelectionIR().
static bool collectSelectionPoints(const SoBrepPointSet* node,
                                   SoState* state,
                                   Gui::SoFCSelectionContextPtr ctx,
                                   std::vector<int32_t>& pointIndices)
{
    if (!ctx) {
        return false;
    }

    const SoCoordinateElement* coords = SoCoordinateElement::getInstance(state);
    if (!coords) {
        return false;
    }

    const int startIndex = node->startIndex.getValue();
    if (ctx->isSelectAll()) {
        pointIndices.reserve(coords->getNum() - startIndex);
        for (int idx = startIndex; idx < coords->getNum(); ++idx) {
            pointIndices.push_back(static_cast<int32_t>(idx));
        }
        return true;
    }

    pointIndices.reserve(ctx->selectionIndex.size());
    bool warn = false;
    for (auto idx : ctx->selectionIndex) {
        if (idx >= startIndex && idx < coords->getNum()) {
            pointIndices.push_back(static_cast<int32_t>(idx));
        }
        else {
            warn = true;
        }
    }
    if (warn) {
        SoDebugError::postWarning("SoBrepPointSet::collectSelectionPoints",
                                  "selectionIndex out of range");
    }
    return true;
}

void SoBrepPointSet::renderHighlight(SoGLRenderAction* action, SelContextPtr ctx)
{
    std::vector<int32_t> pointIndices;
    if (!collectHighlightPoints(this, action->getState(), ctx, pointIndices)) {
        return;
    }
    renderOverlayPoints(action, overlayPointSet, pointIndices.data(),
                        static_cast<int>(pointIndices.size()),
                        ctx->highlightColor, OverlayDepthMode::DrawOnTop);
}

void SoBrepPointSet::renderSelection(SoGLRenderAction* action, SelContextPtr ctx, bool /*push*/)
{
    std::vector<int32_t> pointIndices;
    if (!collectSelectionPoints(this, action->getState(), ctx, pointIndices)) {
        return;
    }
    renderOverlayPoints(action, overlayPointSet, pointIndices.data(),
                        static_cast<int>(pointIndices.size()),
                        ctx->selectionColor, OverlayDepthMode::RespectDepth);
}

#ifdef HAVE_COIN_IR_RENDER_ACTION
void SoBrepPointSet::renderHighlightIR(SoIRRenderAction* action, SelContextPtr ctx)
{
    std::vector<int32_t> pointIndices;
    if (!collectHighlightPoints(this, action->getState(), ctx, pointIndices)) {
        return;
    }
    renderOverlayPoints(action, overlayPointSet, pointIndices.data(),
                        static_cast<int>(pointIndices.size()),
                        ctx->highlightColor, OverlayDepthMode::DrawOnTop);
}

void SoBrepPointSet::renderSelectionIR(SoIRRenderAction* action, SelContextPtr ctx)
{
    std::vector<int32_t> pointIndices;
    if (!collectSelectionPoints(this, action->getState(), ctx, pointIndices)) {
        return;
    }
    renderOverlayPoints(action, overlayPointSet, pointIndices.data(),
                        static_cast<int>(pointIndices.size()),
                        ctx->selectionColor, OverlayDepthMode::RespectDepth);
}
#endif

void SoBrepPointSet::doAction(SoAction* action)
{
    if (action->getTypeId() == Gui::SoHighlightElementAction::getClassTypeId()) {
        Gui::SoHighlightElementAction* hlaction = static_cast<Gui::SoHighlightElementAction*>(action);
        selCounter.checkAction(hlaction);
        VK_BREADCRUMB_LIMITED(30,
                              "[VK-TRACE] SoBrepPointSet::doAction HL this=%p highlighted=%d detail=%p\n",
                              this, hlaction->isHighlighted() ? 1 : 0,
                              (const void*)hlaction->getElement());
        if (!hlaction->isHighlighted()) {
            SelContextPtr ctx
                = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext, false);
            if (ctx) {
                ctx->highlightIndex = -1;
                touch();
            }
            return;
        }
        SelContextPtr ctx = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext);
        const SoDetail* detail = hlaction->getElement();
        if (!detail) {
            ctx->highlightIndex = std::numeric_limits<int>::max();
            ctx->highlightColor = hlaction->getColor();
            touch();
            return;
        }
        else if (!detail->isOfType(SoPointDetail::getClassTypeId())) {
            ctx->highlightIndex = -1;
            touch();
            return;
        }

        int index = static_cast<const SoPointDetail*>(detail)->getCoordinateIndex();
        if (index != ctx->highlightIndex) {
            ctx->highlightIndex = index;
            ctx->highlightColor = hlaction->getColor();
            touch();
        }
        return;
    }
    else if (action->getTypeId() == Gui::SoSelectionElementAction::getClassTypeId()) {
        Gui::SoSelectionElementAction* selaction = static_cast<Gui::SoSelectionElementAction*>(action);
        switch (selaction->getType()) {
            case Gui::SoSelectionElementAction::All: {
                SelContextPtr ctx = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext);
                selCounter.checkAction(selaction, ctx);
                ctx->selectionColor = selaction->getColor();
                ctx->selectionIndex.clear();
                ctx->selectionIndex.insert(-1);
                touch();
                return;
            }
            case Gui::SoSelectionElementAction::None:
                if (selaction->isSecondary()) {
                    if (Gui::SoFCSelectionRoot::removeActionContext(action, this)) {
                        touch();
                    }
                }
                else {
                    SelContextPtr ctx
                        = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext, false);
                    if (ctx) {
                        ctx->selectionIndex.clear();
                        touch();
                    }
                }
                return;
            case Gui::SoSelectionElementAction::Remove:
            case Gui::SoSelectionElementAction::Append: {
                const SoDetail* detail = selaction->getElement();
                if (!detail || !detail->isOfType(SoPointDetail::getClassTypeId())) {
                    if (selaction->isSecondary()) {
                        // For secondary context, a detail of different type means
                        // the user may want to partial render only other type of
                        // geometry. So we call below to obtain a action context.
                        // If no secondary context exist, it will create an empty
                        // one, and an empty secondary context inhibites drawing
                        // here.
                        auto ctx = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext);
                        selCounter.checkAction(selaction, ctx);
                        touch();
                    }
                    return;
                }
                int index = static_cast<const SoPointDetail*>(detail)->getCoordinateIndex();
                if (selaction->getType() == Gui::SoSelectionElementAction::Append) {
                    SelContextPtr ctx
                        = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext);
                    selCounter.checkAction(selaction, ctx);
                    ctx->selectionColor = selaction->getColor();
                    if (ctx->isSelectAll()) {
                        ctx->selectionIndex.clear();
                    }
                    if (ctx->selectionIndex.insert(index).second) {
                        touch();
                    }
                }
                else {
                    SelContextPtr ctx
                        = Gui::SoFCSelectionRoot::getActionContext(action, this, selContext, false);
                    if (ctx && ctx->removeIndex(index)) {
                        touch();
                    }
                }
                break;
            }
            default:
                break;
        }
        return;
    }

    inherited::doAction(action);
}
