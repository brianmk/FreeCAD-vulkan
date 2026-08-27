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

#pragma once

#include <FCGlobal.h>
#include <Inventor/fields/SoSFBool.h>
#include <Inventor/fields/SoSFFloat.h>
#include <Inventor/nodes/SoSeparator.h>

class SoIRRenderAction;

namespace Gui
{

/**
 * Screen-space container for the viewer's axis cross overlay.
 *
 * Children (the axis arrow group and the letter glyph group built by
 * View3DInventorViewer::drawAxisCross()) carry their own cameras, so this
 * node only has to scope them to the bottom-right corner viewport and
 * promote their draw commands to the overlay render pass -- the Vulkan
 * backend then draws them last with the corner's depth cleared, mirroring
 * the GL overlay rendering in drawAxisCross().
 */
class GuiExport SoAxisCrossOverlay: public SoSeparator
{
    using inherited = SoSeparator;

    SO_NODE_HEADER(SoAxisCrossOverlay);

public:
    static void initClass();
    SoAxisCrossOverlay();

    //! Axis cross size as a percentage of the smaller viewport dimension.
    SoSFFloat sizeFraction;

    //! Master visibility switch, mirrored from the viewer's feedback
    //! visibility so the IR path matches GL exactly (GL simply never calls
    //! drawAxisCross() when the axis cross is disabled).
    SoSFBool enabled;

protected:
    ~SoAxisCrossOverlay() override;

#ifdef HAVE_COIN_IR_RENDER_ACTION
    void IRRender(SoIRRenderAction* action) override;
#endif
};

}  // namespace Gui

