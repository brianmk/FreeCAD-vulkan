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

#pragma once

#include <FCGlobal.h>
#include <Inventor/fields/SoSFBool.h>
#include <Inventor/nodes/SoSeparator.h>

class SoIRRenderAction;

namespace Gui
{

/**
 * Marks its subtree as raster-only in the Vulkan retained renderer.
 *
 * Children are traversed normally, then every render command they recorded is
 * promoted to the screen-space OVERLAY pass. The path tracer never builds TLAS
 * geometry for overlay commands and skips them when tracing, so the subtree is
 * drawn by the raster backend on top of the traced image instead of being
 * path-traced.
 *
 * Used for annotation-style geometry such as the origin datum planes: they are
 * crisp, free of denoiser noise, and - because they follow the same view
 * transform as picking - their pickable area stays aligned with what is drawn.
 *
 * Has no effect on the OpenGL path (which renders through GLRender, not
 * IRRender).
 */
class GuiExport SoRasterOverlay: public SoSeparator
{
    using inherited = SoSeparator;

    SO_NODE_HEADER(SoRasterOverlay);

public:
    static void initClass();
    SoRasterOverlay();

    //! Master switch; when off the subtree renders through the normal (traced)
    //! path so a caller can opt out without rebuilding the scene graph.
    SoSFBool enabled;

protected:
    ~SoRasterOverlay() override;

#ifdef HAVE_COIN_IR_RENDER_ACTION
    void IRRender(SoIRRenderAction* action) override;
#endif
};

}  // namespace Gui
