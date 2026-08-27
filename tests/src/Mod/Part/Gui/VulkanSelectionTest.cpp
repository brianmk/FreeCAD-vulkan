// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Regression tests for sub-element (face/edge/point) selection on the Vulkan
// (SoIRRenderAction) rendering path.  The OpenGL and Vulkan viewports share
// the same scene graph and selection contexts, but the OpenGL path reads them
// from SoGLRenderAction while the Vulkan path reads them from SoIRRenderAction.
//
// These tests apply SoHighlightElementAction / SoSelectionElementAction to a
// PartGui Brep node exactly as the view provider does after a pick, traverse
// the scene with SoIRRenderAction, and assert that the retained draw list
// gains the overlay draw commands that carry the selection/highlight color.

#include <gtest/gtest.h>

#include <cmath>

#include <src/App/InitApplication.h>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/actions/SoIRRenderAction.h>
#include <Inventor/details/SoFaceDetail.h>
#include <Inventor/details/SoLineDetail.h>
#include <Inventor/misc/SoTempPath.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/rendering/SoRenderIR.h>

#include <Gui/Selection/SoFCSelection.h>
#include <Gui/Selection/SoFCUnifiedSelection.h>
#include <Gui/SoFCDB.h>

#include <Mod/Part/Gui/SoBrepEdgeSet.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>
#include <Mod/Part/Gui/SoBrepPointSet.h>

namespace
{

constexpr float kFaceR = 0.0F;
constexpr float kFaceG = 0.6F;
constexpr float kFaceB = 0.0F;

constexpr float kSelectionR = 0.0F;
constexpr float kSelectionG = 0.6F;
constexpr float kSelectionB = 0.0F;

constexpr float kHighlightR = 0.8F;
constexpr float kHighlightG = 0.2F;
constexpr float kHighlightB = 0.0F;

Gui::SoFCSelectionRoot* makeFaceScene(PartGui::SoBrepFaceSet*& faceOut)
{
    auto* root = new Gui::SoFCSelectionRoot;
    root->ref();

    auto* coords = new SoCoordinate3;
    // One quad (two triangles).
    coords->point.set1Value(0, SbVec3f(0.0F, 0.0F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, 0.0F, 0.0F));
    coords->point.set1Value(2, SbVec3f(1.0F, 1.0F, 0.0F));
    coords->point.set1Value(3, SbVec3f(0.0F, 1.0F, 0.0F));

    auto* face = new PartGui::SoBrepFaceSet;
    faceOut = face;
    face->partIndex.setNum(1);
    face->partIndex.set1Value(0, 2);  // two triangles in part 0
    static const int32_t kCoordIndex[] = {0, 1, 2, -1, 0, 2, 3, -1};
    face->coordIndex.setValues(0, 8, kCoordIndex);

    root->addChild(coords);
    root->addChild(face);
    return root;
}

// Mirror the view-provider preselection path: a highlight action carrying a
// SoFaceDetail that names part 0 of the face set, applied along the full path
// (selection root -> face node) so the context is keyed against the root.
void highlightFace(Gui::SoFCSelectionRoot* root, PartGui::SoBrepFaceSet* face)
{
    Gui::SoHighlightElementAction action;
    action.setHighlighted(true);
    action.setColor(SbColor(kFaceR, kFaceG, kFaceB));

    auto* detail = new SoFaceDetail;
    detail->setPartIndex(0);
    action.setElement(detail);

    auto* path = new SoTempPath(2);
    path->ref();
    path->append(root);
    path->append(face);
    action.apply(path);
    path->unref();

    delete detail;
}

bool hasEmissive(const SoRenderCommand& cmd, float r, float g, float b)
{
    const auto& e = cmd.material.emissive;
    constexpr float kEps = 1e-4F;
    return std::fabs(e[0] - r) < kEps && std::fabs(e[1] - g) < kEps
        && std::fabs(e[2] - b) < kEps;
}

bool hasDiffuse(const SoRenderCommand& cmd, float r, float g, float b)
{
    const auto& d = cmd.material.diffuse;
    constexpr float kEps = 1e-4F;
    return std::fabs(d[0] - r) < kEps && std::fabs(d[1] - g) < kEps
        && std::fabs(d[2] - b) < kEps;
}

// The highlight/selection color must appear in the recorded commands.  With
// GL parity the color is baked into the base pass's per-face colors
// (diffuse) via the material remap; the explicit emissive overlay only
// appears when the remap cannot be expressed through Coin state.
bool hasFaceColor(const SoRenderCommand& cmd, float r, float g, float b)
{
    return hasEmissive(cmd, r, g, b) || hasDiffuse(cmd, r, g, b);
}

// Mirror the view-provider click path: a selection action carrying a
// SoFaceDetail that names part 0 of the face set.
void selectFace(Gui::SoFCSelectionRoot* root, PartGui::SoBrepFaceSet* face)
{
    Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Append);
    action.setColor(SbColor(kSelectionR, kSelectionG, kSelectionB));

    auto* detail = new SoFaceDetail;
    detail->setPartIndex(0);
    action.setElement(detail);

    auto* path = new SoTempPath(2);
    path->ref();
    path->append(root);
    path->append(face);
    action.apply(path);
    path->unref();

    delete detail;
}

class VulkanPartSelectionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        if (!Gui::SoFCDB::isInitialized()) {
            Gui::SoFCDB::init();
        }
        PartGui::SoBrepFaceSet::initClass();
        PartGui::SoBrepEdgeSet::initClass();
        PartGui::SoBrepPointSet::initClass();
    }
};

}  // namespace

TEST_F(VulkanPartSelectionTest, highlightedFaceRecordsOverlayCommandInIR)
{
    PartGui::SoBrepFaceSet* face = nullptr;
    Gui::SoFCSelectionRoot* root = makeFaceScene(face);
    ASSERT_NE(face, nullptr);
    highlightFace(root, face);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    // The highlighted part must carry the highlight color (baked into the
    // per-face diffuse colors, or as an emissive overlay fallback).
    bool found = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        if (hasFaceColor(list.getCommand(i), kFaceR, kFaceG, kFaceB)) {
            found = true;
            break;
        }
    }

    root->unref();
    EXPECT_TRUE(found) << "no IR draw command carried the face-highlight color";
}

TEST_F(VulkanPartSelectionTest, selectedFaceRecordsOverlayCommandInIR)
{
    PartGui::SoBrepFaceSet* face = nullptr;
    Gui::SoFCSelectionRoot* root = makeFaceScene(face);
    ASSERT_NE(face, nullptr);
    selectFace(root, face);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    bool found = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        if (hasFaceColor(list.getCommand(i), kSelectionR, kSelectionG, kSelectionB)) {
            found = true;
            break;
        }
    }

    root->unref();
    EXPECT_TRUE(found) << "no IR draw command carried the face-selection color";
}

Gui::SoFCSelectionRoot* makeEdgeScene(PartGui::SoBrepEdgeSet*& edgeOut)
{
    auto* root = new Gui::SoFCSelectionRoot;
    root->ref();

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(0.0F, 0.0F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, 0.0F, 0.0F));

    auto* edge = new PartGui::SoBrepEdgeSet;
    edgeOut = edge;
    static const int32_t kCoordIndex[] = {0, 1, -1};
    edge->coordIndex.setValues(0, 3, kCoordIndex);

    root->addChild(coords);
    root->addChild(edge);
    return root;
}

// Two independent line segments (edge 0 and edge 1) so a highlight and a
// selection on DIFFERENT edges can coexist.
Gui::SoFCSelectionRoot* makeTwoEdgeScene(PartGui::SoBrepEdgeSet*& edgeOut)
{
    auto* root = new Gui::SoFCSelectionRoot;
    root->ref();

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(0.0F, 0.0F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, 0.0F, 0.0F));
    coords->point.set1Value(2, SbVec3f(0.0F, 1.0F, 0.0F));
    coords->point.set1Value(3, SbVec3f(1.0F, 1.0F, 0.0F));

    auto* edge = new PartGui::SoBrepEdgeSet;
    edgeOut = edge;
    // Two sections: {0,1,-1} and {2,3,-1} => line index 0 and 1.
    static const int32_t kCoordIndex[] = {0, 1, -1, 2, 3, -1};
    edge->coordIndex.setValues(0, 6, kCoordIndex);

    root->addChild(coords);
    root->addChild(edge);
    return root;
}

void selectEdge(Gui::SoFCSelectionRoot* root, PartGui::SoBrepEdgeSet* edge, int lineIndex)
{
    Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::Append);
    action.setColor(SbColor(kSelectionR, kSelectionG, kSelectionB));

    auto* detail = new SoLineDetail;
    detail->setLineIndex(lineIndex);
    action.setElement(detail);

    auto* path = new SoTempPath(2);
    path->ref();
    path->append(root);
    path->append(edge);
    action.apply(path);
    path->unref();

    delete detail;
}

Gui::SoFCSelectionRoot* makePointScene(PartGui::SoBrepPointSet*& pointOut)
{
    auto* root = new Gui::SoFCSelectionRoot;
    root->ref();

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(0.0F, 0.0F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, 0.0F, 0.0F));
    coords->point.set1Value(2, SbVec3f(0.0F, 1.0F, 0.0F));

    auto* points = new PartGui::SoBrepPointSet;
    pointOut = points;
    points->numPoints.setValue(3);

    root->addChild(coords);
    root->addChild(points);
    return root;
}

void highlightEdge(Gui::SoFCSelectionRoot* root, PartGui::SoBrepEdgeSet* edge)
{
    Gui::SoHighlightElementAction action;
    action.setHighlighted(true);
    action.setColor(SbColor(kHighlightR, kHighlightG, kHighlightB));

    auto* detail = new SoLineDetail;
    detail->setLineIndex(0);
    action.setElement(detail);

    auto* path = new SoTempPath(2);
    path->ref();
    path->append(root);
    path->append(edge);
    action.apply(path);
    path->unref();

    delete detail;
}

void highlightPoint(Gui::SoFCSelectionRoot* root, PartGui::SoBrepPointSet* points)
{
    Gui::SoHighlightElementAction action;
    action.setHighlighted(true);
    action.setColor(SbColor(kHighlightR, kHighlightG, kHighlightB));

    auto* path = new SoTempPath(2);
    path->ref();
    path->append(root);
    path->append(points);
    action.apply(path);
    path->unref();
}

TEST_F(VulkanPartSelectionTest, highlightedEdgeRecordsOverlayCommandInIR)
{
    PartGui::SoBrepEdgeSet* edge = nullptr;
    Gui::SoFCSelectionRoot* root = makeEdgeScene(edge);
    ASSERT_NE(edge, nullptr);
    highlightEdge(root, edge);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    bool found = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        if (hasEmissive(list.getCommand(i), kHighlightR, kHighlightG, kHighlightB)) {
            found = true;
            break;
        }
    }

    root->unref();
    EXPECT_TRUE(found) << "no IR draw command carried the edge-highlight emissive color";
}

// Regression for "hover-highlight another edge while one edge is selected".
// GL renders both the hovered-edge highlight and the selected-edge selection.
// This checks the IR draw list carries BOTH the selection color and the
// highlight color when they target DIFFERENT edges of the same edge set.
TEST_F(VulkanPartSelectionTest, bothSelectedAndHoveredEdgesRecordBothColorsInIR)
{
    PartGui::SoBrepEdgeSet* edge = nullptr;
    Gui::SoFCSelectionRoot* root = makeTwoEdgeScene(edge);
    ASSERT_NE(edge, nullptr);

    // Select edge 0 (committed) and hover edge 1 (preselect) at the same time.
    selectEdge(root, edge, 0);

    // Hover edge 1 (different from the selected edge 0): mirror preselect.
    {
        Gui::SoHighlightElementAction action;
        action.setHighlighted(true);
        action.setColor(SbColor(kHighlightR, kHighlightG, kHighlightB));
        auto* detail = new SoLineDetail;
        detail->setLineIndex(1);
        action.setElement(detail);
        auto* path = new SoTempPath(2);
        path->ref();
        path->append(root);
        path->append(edge);
        action.apply(path);
        path->unref();
        delete detail;
    }

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    bool selFound = false;
    bool hlFound = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        const SoRenderCommand& cmd = list.getCommand(i);
        if (hasEmissive(cmd, kSelectionR, kSelectionG, kSelectionB)
            || hasDiffuse(cmd, kSelectionR, kSelectionG, kSelectionB)) {
            selFound = true;
        }
        if (hasEmissive(cmd, kHighlightR, kHighlightG, kHighlightB)
            || hasDiffuse(cmd, kHighlightR, kHighlightG, kHighlightB)) {
            hlFound = true;
        }
    }

    root->unref();
    EXPECT_TRUE(selFound) << "no IR command carried the selected-edge color";
    EXPECT_TRUE(hlFound) << "no IR command carried the hovered-edge highlight color";
}

TEST_F(VulkanPartSelectionTest, highlightedPointRecordsOverlayCommandInIR)
{
    PartGui::SoBrepPointSet* points = nullptr;
    Gui::SoFCSelectionRoot* root = makePointScene(points);
    ASSERT_NE(points, nullptr);
    highlightPoint(root, points);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    bool found = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        if (hasEmissive(list.getCommand(i), kHighlightR, kHighlightG, kHighlightB)) {
            found = true;
            break;
        }
    }

    root->unref();
    EXPECT_TRUE(found) << "no IR draw command carried the point-highlight emissive color";
}

TEST_F(VulkanPartSelectionTest, unhighlightedFaceHasNoHighlightCommand)
{
    PartGui::SoBrepFaceSet* face = nullptr;
    Gui::SoFCSelectionRoot* root = makeFaceScene(face);
    ASSERT_NE(face, nullptr);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    for (int i = 0; i < list.getNumCommands(); ++i) {
        EXPECT_FALSE(hasEmissive(list.getCommand(i), kFaceR, kFaceG, kFaceB));
    }

    root->unref();
}
