// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Regression test for the Vulkan (SoIRRenderAction) selection path.
//
// The OpenGL viewport uses SoGLRenderAction while the Vulkan viewport renders
// the very same scene graph through SoIRRenderAction. Selection and
// preselection state lives in SoFCSelectionRoot contexts that are written by
// SoHighlightElementAction / SoSelectionElementAction and then consumed by the
// renderer. These tests exercise that consumption directly: they build a
// minimal selection scene, apply the whole-object selection/highlight actions
// exactly like the view providers do, traverse the scene with
// SoIRRenderAction, and assert that the retained draw list carries the
// selection/highlight color as an emissive override.
//
// They do not create a Vulkan instance or a QVulkanWindow: the bug being
// guarded here is in the FreeCAD selection nodes, not the backend.

#include <gtest/gtest.h>

#include <cmath>

#include <src/App/InitApplication.h>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/actions/SoIRRenderAction.h>
#include <Inventor/nodes/SoCube.h>
#include <Inventor/rendering/SoRenderIR.h>

#include <Gui/Selection/SoFCSelection.h>
#include <Gui/Selection/SoFCUnifiedSelection.h>
#include <Gui/SoFCDB.h>

namespace
{

constexpr float kSelectionR = 0.0F;
constexpr float kSelectionG = 0.6F;
constexpr float kSelectionB = 0.0F;

constexpr float kHighlightR = 0.8F;
constexpr float kHighlightG = 0.2F;
constexpr float kHighlightB = 0.0F;

// A minimal scene: one selection root wrapping one selectable cube.
Gui::SoFCSelectionRoot* makeSelectionScene()
{
    auto* root = new Gui::SoFCSelectionRoot;
    root->ref();

    auto* sel = new Gui::SoFCSelection;
    sel->style = Gui::SoFCSelection::EMISSIVE;
    sel->selectionMode = Gui::SoFCSelection::SEL_ON;
    sel->preselectionMode = Gui::SoFCSelection::AUTO;
    sel->useNewSelection = false;

    auto* cube = new SoCube;
    cube->width = 2.0F;
    cube->height = 2.0F;
    cube->depth = 2.0F;

    sel->addChild(cube);
    root->addChild(sel);
    return root;
}

// Apply whole-object selection exactly like ViewProvider::setSelection does:
// a SoSelectionElementAction(All) applied to the selection root.
void selectWholeObject(Gui::SoFCSelectionRoot* root)
{
    Gui::SoSelectionElementAction action(Gui::SoSelectionElementAction::All);
    action.setColor(SbColor(kSelectionR, kSelectionG, kSelectionB));
    action.apply(root);
}

void highlightWholeObject(Gui::SoFCSelectionRoot* root)
{
    Gui::SoHighlightElementAction action;
    action.setHighlighted(true);
    action.setColor(SbColor(kHighlightR, kHighlightG, kHighlightB));
    action.apply(root);
}

bool hasEmissive(const SoRenderCommand& cmd, float r, float g, float b)
{
    const auto& e = cmd.material.emissive;
    constexpr float kEps = 1e-4F;
    return std::fabs(e[0] - r) < kEps && std::fabs(e[1] - g) < kEps
        && std::fabs(e[2] - b) < kEps;
}

class VulkanSelectionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        if (!Gui::SoFCDB::isInitialized()) {
            Gui::SoFCDB::init();
        }
    }
};

}  // namespace

TEST_F(VulkanSelectionTest, selectedObjectEmitsEmissiveOverrideInIR)
{
    Gui::SoFCSelectionRoot* root = makeSelectionScene();
    selectWholeObject(root);

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    bool found = false;
    for (int i = 0; i < list.getNumCommands(); ++i) {
        if (hasEmissive(list.getCommand(i), kSelectionR, kSelectionG, kSelectionB)) {
            found = true;
            break;
        }
    }

    root->unref();
    EXPECT_TRUE(found) << "no IR draw command carried the selection emissive color";
}

TEST_F(VulkanSelectionTest, highlightedObjectEmitsEmissiveOverrideInIR)
{
    Gui::SoFCSelectionRoot* root = makeSelectionScene();
    highlightWholeObject(root);

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
    EXPECT_TRUE(found) << "no IR draw command carried the highlight emissive color";
}

TEST_F(VulkanSelectionTest, unselectedObjectHasNoSelectionEmissive)
{
    Gui::SoFCSelectionRoot* root = makeSelectionScene();

    SbViewportRegion vp(512, 512);
    SoIRRenderAction action(vp);
    action.apply(root);

    const SoDrawList& list = action.getDrawList();
    ASSERT_GT(list.getNumCommands(), 0);

    for (int i = 0; i < list.getNumCommands(); ++i) {
        EXPECT_FALSE(hasEmissive(list.getCommand(i), kSelectionR, kSelectionG, kSelectionB));
    }

    root->unref();
}
