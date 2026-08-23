// SPDX-License-Identifier: LGPL-2.1-or-later
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

#include "SoAxisCrossOverlay.h"

#include <Inventor/actions/SoGLRenderAction.h>
#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/actions/SoIRRenderAction.h>
#endif
#include <Inventor/elements/SoLazyElement.h>
#include <Inventor/elements/SoShapeStyleElement.h>
#include <Inventor/elements/SoViewportRegionElement.h>
#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <Inventor/rendering/SoRenderIR.h>
#endif

using namespace Gui;

SO_NODE_SOURCE(SoAxisCrossOverlay);

void SoAxisCrossOverlay::initClass()
{
    SO_NODE_INIT_CLASS(SoAxisCrossOverlay, SoSeparator, "Separator");
}

SoAxisCrossOverlay::SoAxisCrossOverlay()
{
    SO_NODE_CONSTRUCTOR(SoAxisCrossOverlay);
    // Default matches View3DInventorViewer::axiscrossSize (10%); the viewer
    // re-syncs this field before the first render anyway.
    SO_NODE_ADD_FIELD(sizeFraction, (10.0F));
    SO_NODE_ADD_FIELD(enabled, (TRUE));
}

SoAxisCrossOverlay::~SoAxisCrossOverlay() = default;

#ifdef HAVE_COIN_IR_RENDER_ACTION
void SoAxisCrossOverlay::IRRender(SoIRRenderAction* action)
{
    if (!this->enabled.getValue()) {
        return;
    }
    SoState* state = action->getState();
    if (!state) {
        inherited::IRRender(action);
        return;
    }

    // Bottom-right corner viewport, sized like drawAxisCross(): a square
    // whose side is sizeFraction percent of the smaller viewport dimension.
    const SbViewportRegion vp = SoViewportRegionElement::get(state);
    const SbVec2s size = vp.getViewportSizePixels();
    const int pixelarea = static_cast<int>(
        this->sizeFraction.getValue() / 100.0F * std::min(size[0], size[1]));
    if (pixelarea <= 0) {
        inherited::IRRender(action);
        return;
    }
    const int viewportX = size[0] - pixelarea;
    const int viewportY = 0;

    SoDrawList& list = action->getMutableDrawList();
    const int firstCommand = list.getNumCommands();

    state->push();

    SbViewportRegion corner = vp;
    corner.setViewportPixels(viewportX, viewportY, pixelarea, pixelarea);
    SoViewportRegionElement::set(state, corner);

    // GL draws the axis cross with the action transparency set to BLEND so
    // the textured glyph edges composite smoothly; mirror that here.
    SoShapeStyleElement::setTransparencyType(state, SoGLRenderAction::BLEND);
    SoLazyElement::setTransparencyType(state, SoGLRenderAction::BLEND);

    inherited::IRRender(action);

    // Promote the recorded commands to the overlay pass and scope them to
    // the corner rect: the backend draws the pass last, clearing the rect's
    // depth first, exactly like the navigation cube overlay.
    const int count = list.getNumCommands();
    if (getenv("FC_VULKAN_AXIS_DEBUG")) {
        fprintf(stderr,
                "[AXIS] IRRender viewport=%dx%d corner=%d,%d %dx%d cmds=%d\n",
                size[0], size[1], viewportX, viewportY, pixelarea, pixelarea,
                count - firstCommand);
    }
    for (int i = firstCommand; i < count; ++i) {
        SoRenderCommand& cmd = list.getCommand(i);
        cmd.pass = SO_RENDERPASS_OVERLAY;
        cmd.state.raster.viewportEnabled = TRUE;
        cmd.state.raster.viewportX = viewportX;
        cmd.state.raster.viewportY = viewportY;
        cmd.state.raster.viewportWidth = pixelarea;
        cmd.state.raster.viewportHeight = pixelarea;
        cmd.state.raster.scissorEnabled = TRUE;
        cmd.state.raster.scissorX = viewportX;
        cmd.state.raster.scissorY = viewportY;
        cmd.state.raster.scissorWidth = pixelarea;
        cmd.state.raster.scissorHeight = pixelarea;
    }

    state->pop();
}
#endif
