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
#include <Inventor/actions/SoIRRenderAction.h>
#include <Inventor/rendering/SoRenderIR.h>
#include <Gui/Inventor/SoOverlayPromotion.h>
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
    if (getenv("FC_RASTER_OVERLAY_OFF")) {
        this->enabled.setValue(FALSE);
    }
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

    // The overlay backend ignores unscissored overlays, so the promoted
    // commands are scoped to the full viewport and switched to the OVERLAY
    // pass, where the subtree is rasterized on top of the main scene.
    Gui::promoteCommandsToOverlay(list, firstCommand, action->getState());
}
#endif
