// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Headless regression tests for SoRayPickAction::setRay().
//
// These exercise the CPU picking math directly against a minimal Inventor
// scene graph.  No GL context, Qt widget, or FreeCAD UI is required: the pick
// action only needs an SbViewportRegion plus a camera so the pick radius can
// be related from screen pixels to world units.
//
// The fix under test is the WS_RAY_SET branch of computeWorldSpaceRay(), which
// previously computed an effectively zero pick radius (min(viewvolume) *
// FLT_EPSILON) and therefore never intersected lines/points from a manually
// set ray.  It now derives the radius from the same view volume/viewport
// relationship as the cursor-based setPoint() path.

#include <gtest/gtest.h>

#include <cmath>

#include <Inventor/SbLine.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoDB.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/actions/SoRayPickAction.h>
#include <Inventor/details/SoFaceDetail.h>
#include <Inventor/details/SoLineDetail.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoSeparator.h>

namespace
{

constexpr float kRadiusPixels = 5.0F;
constexpr float kEps = 1e-3F;

// Camera looking down -Z at the origin from (0, 0, 10).
SoPerspectiveCamera* makeCamera()
{
    auto* cam = new SoPerspectiveCamera;
    cam->position.setValue(0.0F, 0.0F, 10.0F);
    cam->pointAt(SbVec3f(0.0F, 0.0F, 0.0F));
    cam->aspectRatio = 1.0F;
    cam->nearDistance = 0.1F;
    cam->farDistance = 100.0F;
    return cam;
}

// A single segment on the X axis at y=offset, from x=-1 to x=1.
SoSeparator* makeLineRoot(float yOffset)
{
    auto* root = new SoSeparator;

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(-1.0F, yOffset, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, yOffset, 0.0F));

    auto* line = new SoIndexedLineSet;
    line->coordIndex.set1Value(0, 0);
    line->coordIndex.set1Value(1, 1);
    line->coordIndex.set1Value(2, -1);

    root->addChild(makeCamera());
    root->addChild(coords);
    root->addChild(line);
    return root;
}

// A single triangle in the XY plane around the origin.
SoSeparator* makeTriangleRoot()
{
    auto* root = new SoSeparator;

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(-1.0F, -1.0F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, -1.0F, 0.0F));
    coords->point.set1Value(2, SbVec3f(0.0F, 1.0F, 0.0F));

    auto* face = new SoIndexedFaceSet;
    face->coordIndex.set1Value(0, 0);
    face->coordIndex.set1Value(1, 1);
    face->coordIndex.set1Value(2, 2);
    face->coordIndex.set1Value(3, -1);

    root->addChild(makeCamera());
    root->addChild(coords);
    root->addChild(face);
    return root;
}

// Two parallel X-axis segments at y=0 and y=0.003.  Line 1 sits at z=0.5,
// closer to the camera (which looks down -Z), so it is nearer in ray depth
// than line 0 at z=0.
SoSeparator* makeTwoLineRoot()
{
    auto* root = new SoSeparator;

    auto* coords = new SoCoordinate3;
    coords->point.set1Value(0, SbVec3f(-1.0F, 0.000F, 0.0F));
    coords->point.set1Value(1, SbVec3f(1.0F, 0.000F, 0.0F));
    coords->point.set1Value(2, SbVec3f(-1.0F, 0.003F, 0.5F));
    coords->point.set1Value(3, SbVec3f(1.0F, 0.003F, 0.5F));

    auto* line = new SoIndexedLineSet;
    line->coordIndex.set1Value(0, 0);
    line->coordIndex.set1Value(1, 1);
    line->coordIndex.set1Value(2, -1);
    line->coordIndex.set1Value(3, 2);
    line->coordIndex.set1Value(4, 3);
    line->coordIndex.set1Value(5, -1);

    root->addChild(makeCamera());
    root->addChild(coords);
    root->addChild(line);
    return root;
}

class SoRayPickActionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        SoDB::init();
    }
};

}  // namespace

TEST_F(SoRayPickActionTest, setRayPicksLineWithinRadius)
{
    SoSeparator* root = makeLineRoot(0.002F);
    root->ref();

    SoRayPickAction action(SbViewportRegion(100, 100));
    action.setRadius(kRadiusPixels);
    action.setRay(SbVec3f(0.0F, 0.0F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F), 0.1F);
    action.apply(root);

    SoPickedPoint* pp = action.getPickedPoint();
    ASSERT_NE(pp, nullptr);

    auto* detail = static_cast<const SoLineDetail*>(pp->getDetail());
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->getLineIndex(), 0);

    // Closest point on the line is the origin.
    EXPECT_TRUE(pp->getPoint().equals(SbVec3f(0.0F, 0.0F, 0.0F), kEps));

    root->unref();
}

TEST_F(SoRayPickActionTest, setRayMissesLineBeyondRadius)
{
    SoSeparator* root = makeLineRoot(0.02F);  // ~24px away at the near plane
    root->ref();

    SoRayPickAction action(SbViewportRegion(100, 100));
    action.setRadius(kRadiusPixels);
    action.setRay(SbVec3f(0.0F, 0.0F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F), 0.1F);
    action.apply(root);

    EXPECT_EQ(action.getPickedPoint(), nullptr);

    root->unref();
}

TEST_F(SoRayPickActionTest, setRayPicksClosestOfTwoLines)
{
    SoSeparator* root = makeTwoLineRoot();
    root->ref();

    // Ray passes 0.002 world units from line 0 and 0.001 from line 1; both
    // are inside the pick radius, but line 1 is nearer in ray depth (its z
    // is closer to the camera), so it must win.
    SoRayPickAction action(SbViewportRegion(100, 100));
    action.setRadius(kRadiusPixels);
    action.setRay(SbVec3f(0.0F, 0.002F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F), 0.1F);
    action.apply(root);

    SoPickedPoint* pp = action.getPickedPoint();
    ASSERT_NE(pp, nullptr);

    auto* detail = static_cast<const SoLineDetail*>(pp->getDetail());
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->getLineIndex(), 1);

    // Closest point on line 1 is (0, 0.003, 0.5).
    EXPECT_TRUE(pp->getPoint().equals(SbVec3f(0.0F, 0.003F, 0.5F), kEps));

    root->unref();
}

TEST_F(SoRayPickActionTest, setRayPicksTriangle)
{
    SoSeparator* root = makeTriangleRoot();
    root->ref();

    SoRayPickAction action(SbViewportRegion(100, 100));
    action.setRay(SbVec3f(0.0F, 0.0F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F), 0.1F);
    action.apply(root);

    SoPickedPoint* pp = action.getPickedPoint();
    ASSERT_NE(pp, nullptr);

    auto* detail = static_cast<const SoFaceDetail*>(pp->getDetail());
    ASSERT_NE(detail, nullptr);
    EXPECT_EQ(detail->getFaceIndex(), 0);

    EXPECT_TRUE(pp->getPoint().equals(SbVec3f(0.0F, 0.0F, 0.0F), kEps));

    // The face lies in the XY plane, so its normal is +/-Z.
    const SbVec3f n = pp->getObjectNormal();
    EXPECT_NEAR(std::fabs(n.dot(SbVec3f(0.0F, 0.0F, 1.0F))), 1.0F, kEps);

    root->unref();
}

TEST_F(SoRayPickActionTest, setRayHonorsNearFarClipping)
{
    SoSeparator* root = makeLineRoot(0.0F);
    root->ref();

    // Line is 10 world units from the ray start.
    {
        SoRayPickAction action(SbViewportRegion(100, 100));
        action.setRadius(kRadiusPixels);
        action.setRay(SbVec3f(0.0F, 0.0F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F),
                      1.0F, 15.0F);
        action.apply(root);
        EXPECT_NE(action.getPickedPoint(), nullptr);
    }
    {
        // Near plane is beyond the line, so nothing may be picked.
        SoRayPickAction action(SbViewportRegion(100, 100));
        action.setRadius(kRadiusPixels);
        action.setRay(SbVec3f(0.0F, 0.0F, 10.0F), SbVec3f(0.0F, 0.0F, -1.0F),
                      11.0F, 15.0F);
        action.apply(root);
        EXPECT_EQ(action.getPickedPoint(), nullptr);
    }

    root->unref();
}

TEST_F(SoRayPickActionTest, setPointMatchesSetRayForSameLine)
{
    // A line offset from the view center by an amount that is inside the
    // pixel radius (5px) but larger than the old setRay() radius (epsilon),
    // so this also guards the radius regression directly.
    SoSeparator* root = makeLineRoot(0.002F);
    root->ref();

    SoPickedPoint* pointPick = nullptr;
    {
        SoRayPickAction action(SbViewportRegion(100, 100));
        action.setRadius(kRadiusPixels);
        action.setPoint(SbVec2s(50, 50));
        action.apply(root);
        pointPick = action.getPickedPoint();
    }
    ASSERT_NE(pointPick, nullptr);

    SoPickedPoint* rayPick = nullptr;
    {
        SoRayPickAction action(SbViewportRegion(100, 100));
        action.setRadius(kRadiusPixels);
        const SbLine line = action.getLine();
        const SbVec3f start = line.getPosition();
        const SbVec3f dir = line.getDirection();
        action.setRay(start, dir, 0.0F);
        action.apply(root);
        rayPick = action.getPickedPoint();
    }
    ASSERT_NE(rayPick, nullptr);

    EXPECT_TRUE(rayPick->getPoint().equals(pointPick->getPoint(), kEps));

    root->unref();
}
