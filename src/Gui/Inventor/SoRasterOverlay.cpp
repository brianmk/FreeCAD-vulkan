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
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the             *
 *   GNU Lesser General Public License for more details.                      *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see                                  *
 *   <https://www.gnu.org/licenses/>.                                         *
 *                                                                            *
 ******************************************************************************/

#include "PreCompiled.h"

#include "SoRasterOverlay.h"

#include <cstdlib>

#ifdef HAVE_COIN_IR_RENDER_ACTION
#include <algorithm>
#include <Inventor/actions/SoIRRenderAction.h>
#include <Inventor/elements/SoViewportRegionElement.h>
#include <Inventor/rendering/SoRenderIR.h>
#endif

using namespace Gui;

SO_NODE_SOURCE(SoRasterOverlay);

void SoRasterOverlay::initClass()
{
    SO_NODE_INIT_CLASS(SoRasterOverlay, SoSeparator, "Separator");
}

SoRasterOverlay::SoRasterOverlay()
{
    SO_NODE_CONSTRUCTOR(SoRasterOverlay);
    SO_NODE_ADD_FIELD(enabled, (TRUE));
#ifdef FREECAD_VULKAN_DEBUG_HOOKS
    if (getenv("FC_RASTER_OVERLAY_OFF")) {
        this->enabled.setValue(FALSE);
    }
#endif
}

SoRasterOverlay::~SoRasterOverlay() = default;

#ifdef HAVE_COIN_IR_RENDER_ACTION
void SoRasterOverlay::IRRender(SoIRRenderAction* action)
{
    if (!this->enabled.getValue()) {
        inherited::IRRender(action);
        return;
    }

    SoDrawList& list = action->getMutableDrawList();
    const int firstCommand = list.getNumCommands();

    inherited::IRRender(action);

    // The overlay backend ignores unscissored overlays, so scope the promoted
    // commands to the full viewport (mirroring SoFCSelection's highlight
    // promotion). The path tracer skips OVERLAY commands, so the subtree is
    // rastered on top of the traced image rather than traced itself.
    SoState* state = action->getState();
    const SbViewportRegion vp = SoViewportRegionElement::get(state);
    const short vx = std::max(0, static_cast<int>(vp.getViewportOriginPixels()[0]));
    const short vy = std::max(0, static_cast<int>(vp.getViewportOriginPixels()[1]));
    const short vw = std::max(1, static_cast<int>(vp.getViewportSizePixels()[0]));
    const short vh = std::max(1, static_cast<int>(vp.getViewportSizePixels()[1]));

    const int count = list.getNumCommands();
    for (int i = firstCommand; i < count; ++i) {
        SoRenderCommand& cmd = list.getCommand(i);
        cmd.pass = SO_RENDERPASS_OVERLAY;
        cmd.state.raster.scissorEnabled = TRUE;
        cmd.state.raster.scissorX = vx;
        cmd.state.raster.scissorY = vy;
        cmd.state.raster.scissorWidth = vw;
        cmd.state.raster.scissorHeight = vh;
    }
}
#endif
