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

#pragma once

#include "SoNaviCube.h"

class SoIRRenderAction;
class SoOrthographicCamera;
class SoPerspectiveCamera;
class SoTransform;
class SoSwitch;
class SoSeparator;
class SoMaterial;
class SoDrawStyle;
class SoIndexedFaceSet;
class SoIndexedLineSet;
class SoPointSet;
class SoVertexProperty;
class SoFaceSet;
class SoTexture2;
class SoPolygonOffset;
class SoShapeHints;
class SoDepthBuffer;

namespace Gui
{

/**
 * Vulkan-only navigation cube node.
 *
 * The base SoNaviCube is kept byte-for-byte upstream (a pure OpenGL node) so
 * the fork stays portable: the GL renderer draws it exactly as upstream does.
 * This class is a fully self-contained re-implementation of the navcube for the
 * Vulkan/IR (retained-render) pipeline.  It builds its OWN scene graph (cube,
 * edges, three coloured axis arrows), drives it from the inherited public state
 * fields, and records it into the overlay draw list scoped to the navcube rect.
 */
class GuiExport SoNaviCubeVulkan: public SoNaviCube
{
    using inherited = SoNaviCube;

    SO_NODE_HEADER(SoNaviCubeVulkan);

public:
    static void initClass();
    SoNaviCubeVulkan();

    //! Vulkan/IR retained-render entry point.  Owns the whole Vulkan scene:
    //! builds/updates the navcube scene graph and records it into the overlay
    //! pass scoped to the navcube corner viewport.
    void IRRender(SoIRRenderAction* action) override;

protected:
    ~SoNaviCubeVulkan() override;

private:
    //! Applies a multiplicative scale to the overlay viewport size (Vulkan-only).
    //! The base Coin geometry is never scaled; 1.0 = neutral.
    void setVulkanScale(float scale);

    //! Lazily build the navcube scene graph (once).
    void ensureScene() const;
    //! Refresh camera, transform, materials and highlight from the inherited
    //! public state fields, and touch the retained geometry so the axes (and the
    //! per-face highlight) re-record with the current camera transform.
    void updateScene() const;
    //! Scope the scene to the corner viewport, force BASE_COLOR lighting, record
    //! sceneRoot, and promote the recorded commands to the overlay pass.
    void recordOverlay(SoIRRenderAction* action);
    //! Map a PickId (as int) to the cube's 6-face material index, or -1.
    static int faceIndex(int pickId);

    // AxisNodes / LabelNodes / ButtonNodes are inherited from the base
    // SoNaviCube (protected) to avoid duplicating the node-group structs.

    //! Build the face-label quads (textured) into a labelsSep under the group.
    void buildLabels(SoSeparator* cubeGroup) const;
    //! Clear the label node references (children removal).
    void resetLabels() const;
    //! Build the screen-space navigation buttons (arrows / home / backside /
    //! view menu) into a buttonsSep under sceneRoot.  They sit in front of the
    //! cube and are UI controls: depth test is off and they do not cull.
    void buildButtons() const;
    //! Refresh the button fill/outline materials (hover highlight + opacity) and
    //! touch the retained geometry so a camera-only frame still re-records it.
    void updateButtons(float op, int hilitePick) const;

    mutable SoSeparator* sceneRoot {nullptr};
    mutable SoSeparator* labelsSep {nullptr};
    mutable SoSeparator* buttonsSep {nullptr};
    mutable SoSwitch* cameraSwitch {nullptr};
    mutable SoOrthographicCamera* orthoCamera {nullptr};
    mutable SoPerspectiveCamera* perspCamera {nullptr};
    mutable SoTransform* rootTransform {nullptr};

    mutable SoIndexedFaceSet* cubeFaces {nullptr};
    mutable SoVertexProperty* cubeVertexProperty {nullptr};
    mutable SoMaterial* cubeMaterial {nullptr};

    mutable SoIndexedLineSet* edges {nullptr};
    mutable SoVertexProperty* edgeVertexProperty {nullptr};
    mutable SoMaterial* edgeMaterial {nullptr};
    mutable SoDrawStyle* edgeDrawStyle {nullptr};

    mutable std::array<AxisNodes, 3> axisNodes;
    //! One label node per face label pick id (Front/Top/Right/Rear/Bottom/Left).
    mutable std::array<LabelNodes, static_cast<std::size_t>(
        SoNaviCube::PickId::Left) + 1> faceLabels;
    //! One entry per navigation button, indexed by PickId.
    mutable std::array<ButtonNodes, static_cast<std::size_t>(
        SoNaviCube::PickId::ViewMenu) + 1> buttonNodes;

    float vulkanScale {1.0F};
    mutable bool sceneBuilt {false};
};

}  // namespace Gui
