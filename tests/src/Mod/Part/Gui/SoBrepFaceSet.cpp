// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SbBox3f.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoDB.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/actions/SoRayPickAction.h>
#include <Inventor/details/SoFaceDetail.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Mod/Part/Gui/SoBrepFaceSet.h>

#include <vector>

namespace
{

//! A flat strip of single-triangle faces laid out along +X. Face i spans
//! x in [i, i+1], so a ray at x = i + 0.5 must hit exactly face i. The strip
//! lets the pick test drive every face and compare the culled pick path
//! (faceBoxes present) with the unculled fallback (no faceBoxes).
struct Strip
{
    SoSeparator* root {nullptr};
    SoCoordinate3* coords {nullptr};
    PartGui::SoBrepFaceSet* faces {nullptr};

    Strip(int faceCount, bool withBoxes)
    {
        root = new SoSeparator;
        root->ref();
        coords = new SoCoordinate3;
        faces = new PartGui::SoBrepFaceSet;
        root->addChild(coords);
        root->addChild(faces);

        coords->point.setNum(faceCount * 3);
        faces->coordIndex.setNum(faceCount * 4);
        faces->partIndex.setNum(faceCount);

        SbVec3f* points = coords->point.startEditing();
        int32_t* indices = faces->coordIndex.startEditing();
        int32_t* parts = faces->partIndex.startEditing();

        std::vector<SbBox3f> boxes;
        boxes.reserve(faceCount);
        for (int i = 0; i < faceCount; ++i) {
            const SbVec3f a(static_cast<float>(i), 0.0f, 0.0f);
            const SbVec3f b(static_cast<float>(i + 1), 0.0f, 0.0f);
            const SbVec3f c(static_cast<float>(i) + 0.5f, 1.0f, 0.0f);
            points[3 * i] = a;
            points[3 * i + 1] = b;
            points[3 * i + 2] = c;
            indices[4 * i] = 3 * i;
            indices[4 * i + 1] = 3 * i + 1;
            indices[4 * i + 2] = 3 * i + 2;
            indices[4 * i + 3] = -1;
            parts[i] = 1;

            SbBox3f box;
            box.extendBy(a);
            box.extendBy(b);
            box.extendBy(c);
            boxes.push_back(box);
        }

        coords->point.finishEditing();
        faces->coordIndex.finishEditing();
        faces->partIndex.finishEditing();
        faces->setFaceBoxes(withBoxes ? std::move(boxes) : std::vector<SbBox3f> {});
    }

    ~Strip()
    {
        root->unref();
    }

    Strip(const Strip&) = delete;
    Strip& operator=(const Strip&) = delete;

    //! Returns the picked face (part) index, or -1 when nothing is hit.
    int pick(float x) const
    {
        SbViewportRegion viewport(200, 200);
        SoRayPickAction action(viewport);
        action.setRay(SbVec3f(x, 0.4f, 10.0f), SbVec3f(0.0f, 0.0f, -1.0f));
        action.apply(root);
        const SoPickedPoint* pp = action.getPickedPoint();
        if (!pp) {
            return -1;
        }
        const auto* detail = dynamic_cast<const SoFaceDetail*>(pp->getDetail());
        return detail ? detail->getPartIndex() : -1;
    }
};

class SoBrepFaceSetPickTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        SoDB::init();
        if (PartGui::SoBrepFaceSet::getClassTypeId().isBad()) {
            PartGui::SoBrepFaceSet::initClass();
        }
    }
};

TEST_F(SoBrepFaceSetPickTest, culledPickHitsTheFaceUnderTheRay)
{
    const Strip strip(20, true);

    EXPECT_EQ(strip.pick(0.5f), 0);
    EXPECT_EQ(strip.pick(4.5f), 4);
    EXPECT_EQ(strip.pick(19.5f), 19);
}

TEST_F(SoBrepFaceSetPickTest, unculledFallbackHitsTheFaceUnderTheRay)
{
    // With no faceBoxes the node must fall back to the inherited pick.
    const Strip strip(20, false);

    EXPECT_EQ(strip.pick(0.5f), 0);
    EXPECT_EQ(strip.pick(4.5f), 4);
    EXPECT_EQ(strip.pick(19.5f), 19);
}

TEST_F(SoBrepFaceSetPickTest, culledAndUnculledAgree)
{
    const Strip culled(64, true);
    const Strip unculled(64, false);

    for (int i = 0; i < 64; ++i) {
        const float x = static_cast<float>(i) + 0.5f;
        ASSERT_EQ(culled.pick(x), unculled.pick(x)) << "face " << i;
    }
}

TEST_F(SoBrepFaceSetPickTest, missEverythingReturnsNoPick)
{
    const Strip strip(20, true);

    EXPECT_EQ(strip.pick(-1.0f), -1);
    EXPECT_EQ(strip.pick(20.5f), -1);
    // Inside a face's x range but outside the triangle (y is part of the ray).
    EXPECT_EQ(strip.pick(0.9f), -1);
}

TEST_F(SoBrepFaceSetPickTest, staleBoxCacheFallsBackInsteadOfMisPicking)
{
    Strip strip(20, true);
    ASSERT_EQ(strip.pick(4.5f), 4);

    // A cache that does not match partIndex must be ignored (fallback), not
    // used to cull faces wrongly.
    strip.faces->partIndex.setNum(21);
    strip.faces->setFaceBoxes({SbBox3f()});
    EXPECT_EQ(strip.pick(4.5f), 4);
}

}  // namespace
