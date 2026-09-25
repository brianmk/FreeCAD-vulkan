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
class SoGetBoundingBoxAction;

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
    // Coin's optimized SoSeparator child traversal calls GLRenderBelowPath()
    // (and GLRenderInPath() when the node is on the current path), NOT
    // GLRender(), on each child.  Because this node derives from SoSeparator,
    // omitting these would dispatch to the inherited SoSeparator implementations
    // and skip updateGrid() entirely -- the grid then renders with empty vertex
    // buffers in the classic Coin/GL viewport.
    void GLRenderBelowPath(SoGLRenderAction* action) override;
    void GLRenderInPath(SoGLRenderAction* action) override;
#ifdef HAVE_COIN_IR_RENDER_ACTION
    void IRRender(SoIRRenderAction* action) override;
#endif

    //! Reports the real (bounded) child bounds.  An empty bound would let the
    //! parent separator's frustum culling drop the grid before updateGrid()
    //! ever produces geometry.  The grid is kept out of the *main* scene
    //! bounding box structurally, by living in the per-frame decoration scene
    //! rather than the retained main scene (see View3DInventorViewer).
    void getBoundingBox(SoGetBoundingBoxAction* action) override;

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

    //! Cache of the view volume (its near/far corner points) from the last
    //! buffer write.  SoMFVec3f/SoMFInt32::setValues() notifies the field's
    //! auditors even when the values are identical, and the node lives in the
    //! scene graph, so an unconditional rewrite on every render would wake the
    //! viewport's scene redraw sensor from inside the frame -- a redraw feedback
    //! loop that keeps rendering at full rate while completely idle.  Skipping
    //! the rewrite while the view volume is unchanged breaks that loop.  The
    //! corners are the complete input to the grid: the clipped line set depends
    //! on the whole frustum (position, orientation, near/far, aspect), not just
    //! a footprint.
    bool m_gridValid = false;
    SbVec3f m_gridNearCorners[4];
    SbVec3f m_gridFarCorners[4];
};

}  // namespace Gui
