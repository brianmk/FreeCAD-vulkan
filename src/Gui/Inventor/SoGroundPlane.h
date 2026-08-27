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
#include <Inventor/SbVec3f.h>
#include <Inventor/fields/SoSFBool.h>
#include <Inventor/fields/SoSFFloat.h>
#include <Inventor/nodes/SoSeparator.h>

class SoLineSet;
class SoMaterial;
class SoSensor;
class SoState;
class SoVertexProperty;
class SoFieldSensor;

namespace Gui
{

/**
 * World-space "ground plane" grid sitting on the world XY plane (Z = 0).
 *
 * Unlike SoDrawingGrid (which is clipped to screen space and fills the whole
 * viewport), this node lives in the model scene and draws a set of grid lines
 * on the Z = 0 plane. The grid is *adaptive*: on every render it recomputes the
 * visible rectangular footprint of the current camera's view volume projected
 * onto Z = 0 and regenerates evenly spaced lines using a "nice" step, so the
 * grid always looks like a drafting/reference ground similar to the one used
 * by Autodesk Inventor without becoming too dense or too sparse.
 *
 * The grid is intentionally faint by default (high transparency) and exposes a
 * @ref transparency field so the user can dial it from barely-visible to solid.
 * The two origin-crossing lines (X and Y axes) are drawn darker and thicker.
 */
class GuiExport SoGroundPlane: public SoSeparator
{
    using inherited = SoSeparator;

    SO_NODE_HEADER(SoGroundPlane);

public:
    static void initClass();
    SoGroundPlane();

    //! Master visibility switch. When FALSE the node renders nothing.
    SoSFBool enabled;

    //! 0.0 = fully opaque, 1.0 = fully transparent (invisible grid).
    SoSFFloat transparency;

    //! Scale factor applied on top of the view-derived extent. Increasing this
    //! draws the grid further past the visible area.
    SoSFFloat extentFactor;

protected:
    ~SoGroundPlane() override;

    void GLRender(SoGLRenderAction* action) override;
#ifdef HAVE_COIN_IR_RENDER_ACTION
    void IRRender(SoIRRenderAction* action) override;
#endif

private:
    void updateGrid(SoState* state);
    void applyTransparency();
    static void transparencyChanged(void* data, SoSensor* sensor);
    static float niceStep(float raw);

    SoMaterial* m_centerLineMaterial = nullptr;
    SoMaterial* m_minorLineMaterial = nullptr;
    SoMaterial* m_majorLineMaterial = nullptr;
    SoLineSet* m_centerLineSet = nullptr;
    SoLineSet* m_minorLineSet = nullptr;
    SoLineSet* m_majorLineSet = nullptr;
    SoVertexProperty* m_centerVertexProperty = nullptr;
    SoVertexProperty* m_minorVertexProperty = nullptr;
    SoVertexProperty* m_majorVertexProperty = nullptr;
    SoFieldSensor* m_transparencySensor = nullptr;
};

}  // namespace Gui
