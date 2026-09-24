// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <Inventor/SbBox3f.h>
#include <Inventor/SbMatrix.h>
#include <Inventor/SbRotation.h>
#include <Inventor/SbSphere.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewVolume.h>
#include <Inventor/SoDB.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Base/Converter.h>
#include <Base/Tools.h>
#include <Gui/Camera.h>
#include <Gui/Utilities.h>
#include <Gui/ViewProvider.h>

#include <src/App/InitApplication.h>


/*
 This comment was previously used to get the hard coded axonometric view quaternions
 in the Camera class.
 This has since been replaced with unit tests that verify the correctness of the
 quaternion calculations.

 The old code is kept for reference and to show how the quaternions were calculated.

 ---

 Formulas to get quaternion for axonometric views:

 \code
from math import sqrt, degrees, asin, atan
p1=App.Rotation(App.Vector(1,0,0),90)
p2=App.Rotation(App.Vector(0,0,1),alpha)
p3=App.Rotation(p2.multVec(App.Vector(1,0,0)),beta)
p4=p3.multiply(p2).multiply(p1)

from pivy import coin
c=Gui.ActiveDocument.ActiveView.getCameraNode()
c.orientation.setValue(*p4.Q)
 \endcode

 The angles alpha and beta depend on the type of axonometry
 Isometric:
 \code
alpha=45
beta=degrees(asin(-sqrt(1.0/3.0)))
 \endcode

 Dimetric:
 \code
alpha=degrees(asin(sqrt(1.0/8.0)))
beta=degrees(-asin(1.0/3.0))
 \endcode

 Trimetric:
 \code
alpha=30.0
beta=-35.0
 \endcode

 Verification code that the axonomtries are correct:

 \code
from pivy import coin
c=Gui.ActiveDocument.ActiveView.getCameraNode()
vo=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,0,0)).getValue())
vx=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(10,0,0)).getValue())
vy=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,10,0)).getValue())
vz=App.Vector(c.getViewVolume().getMatrix().multVecMatrix(coin.SbVec3f(0,0,10)).getValue())
(vx-vo).Length
(vy-vo).Length
(vz-vo).Length

# Projection
vo.z=0
vx.z=0
vy.z=0
vz.z=0

(vx-vo).Length
(vy-vo).Length
(vz-vo).Length
 \endcode

 See also:
 http://www.mathematik.uni-marburg.de/~thormae/lectures/graphics1/graphics_6_2_ger_web.html#1
 http://www.mathematik.uni-marburg.de/~thormae/lectures/graphics1/code_v2/Axonometric/qt/Axonometric.cpp
 https://de.wikipedia.org/wiki/Arkussinus_und_Arkuskosinus
*/

using Base::convertTo;
using Base::Rotation;
using Base::toRadians;
using Base::Vector3d;

namespace
{

Rotation buildAxonometricRotation(double alphaRad, double betaRad)
{
    const auto p1 = Rotation(Vector3d::UnitX, toRadians<float>(90.0));
    const auto p2 = Rotation(Vector3d::UnitZ, alphaRad);
    const auto p3 = Rotation(p2.multVec(Vector3d::UnitX), betaRad);
    const auto p4 = p3 * p2 * p1;

    return p4;
}

// Returns a tuple of 2D lengths of X, Y, Z unit vectors after applying rotation
std::array<double, 3> getProjectedLengths(const SbRotation& rot)
{
    // Set up a simple view volume to test the projection of the unit vectors.
    // The actual values don't matter much, as we are only interested in the
    // relative lengths of the projected vectors.
    SbViewVolume volume;
    // left, right, bottom, top, near, far
    volume.ortho(-10, 10, -10, 10, -10, 10);

    volume.rotateCamera(rot);
    const auto matrix = volume.getMatrix();

    // Get the transformed unit vectors
    SbVec3f vo, vx, vy, vz;
    matrix.multVecMatrix(SbVec3f(0, 0, 0), vo);
    matrix.multVecMatrix(SbVec3f(10, 0, 0), vx);
    matrix.multVecMatrix(SbVec3f(0, 10, 0), vy);
    matrix.multVecMatrix(SbVec3f(0, 0, 10), vz);

    // Project to XY plane by setting Z to 0
    vo[2] = 0;
    vx[2] = 0;
    vy[2] = 0;
    vz[2] = 0;

    // Return the lengths of the projected vectors
    return {(vx - vo).length(), (vy - vo).length(), (vz - vo).length()};
}

SbVec3f furthestNormalizedCorner(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    SbBox3f normalized = box;
    normalized.transform(camera.getViewVolume(aspect).getMatrix());

    const SbVec3f low = normalized.getMin();
    const SbVec3f high = normalized.getMax();
    return {
        std::max(std::abs(low[0]), std::abs(high[0])),
        std::max(std::abs(low[1]), std::abs(high[1])),
        std::max(std::abs(low[2]), std::abs(high[2])),
    };
}

float largestNormalizedExtent(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    const SbVec3f furthest = furthestNormalizedCorner(camera, box, aspect);
    return std::max(furthest[0], furthest[1]);
}

float largestNormalizedDepth(const SoOrthographicCamera& camera, const SbBox3f& box, float aspect)
{
    return furthestNormalizedCorner(camera, box, aspect)[2];
}

/// Expect the fit to decline @p box, leaving a known starting pose untouched.
void expectFitDeclines(SoOrthographicCamera& camera, const SbBox3f& box)
{
    camera.orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    camera.position.setValue(7.0F, 8.0F, 9.0F);
    camera.height.setValue(42.0F);

    Gui::Camera::fitToBox(camera, box, 1.0F);

    EXPECT_FLOAT_EQ(camera.height.getValue(), 42.0F);

    const SbVec3f position = camera.position.getValue();
    EXPECT_FLOAT_EQ(position[0], 7.0F);
    EXPECT_FLOAT_EQ(position[1], 8.0F);
    EXPECT_FLOAT_EQ(position[2], 9.0F);
}

}  // namespace

class CameraPrecalculatedQuaternions: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(CameraPrecalculatedQuaternions, testIsometric)
{
    // Use the formula to get the isometric rotation
    double alpha = toRadians(45.0f);
    double beta = std::asin(-std::sqrt(1.0 / 3.0));

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::isometric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}

TEST_F(CameraPrecalculatedQuaternions, testDimetric)
{
    // Use the formula to get the dimetric rotation
    double alpha = std::asin(std::sqrt(1.0 / 8.0));
    double beta = -std::asin(1.0 / 3.0);

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::dimetric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}

TEST_F(CameraPrecalculatedQuaternions, testTrimetric)
{
    // Use the formula to get the trimetric rotation
    double alpha = toRadians(30.0);
    double beta = toRadians(-35.0);

    const Rotation actual = buildAxonometricRotation(alpha, beta);
    const Rotation expected = convertTo<Rotation>(Gui::Camera::trimetric());

    EXPECT_TRUE(actual.isSame(expected, 1e-6));
}


class CameraRotation: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(CameraRotation, testIsometricProjection)
{
    auto rot = Gui::Camera::isometric();
    auto lengths = getProjectedLengths(rot);

    // In isometric, expect all lengths to be roughly equal
    EXPECT_NEAR(lengths[0], lengths[1], 1e-6);  // X == Y
    EXPECT_NEAR(lengths[0], lengths[2], 1e-6);  // X == Z
    EXPECT_NEAR(lengths[1], lengths[2], 1e-6);  // Y == Z
}

TEST_F(CameraRotation, testDimetricProjection)
{
    const auto rot = Gui::Camera::dimetric();
    const auto lengths = getProjectedLengths(rot);

    // In dimetric, expect two lengths to be roughly equal, one different
    const std::initializer_list<std::pair<double, double>> pairs = {
        {lengths[0], lengths[1]},
        {lengths[1], lengths[2]},
        {lengths[0], lengths[2]},
    };

    constexpr double tolerance = 1e-6;
    const auto isSimilar = [&](std::pair<double, double> lengths) -> bool {
        return std::abs(lengths.first - lengths.second) < tolerance;
    };

    unsigned similarCount = std::ranges::count_if(pairs, isSimilar);

    EXPECT_EQ(similarCount, 1);  // Exactly two are equal
}

TEST_F(CameraRotation, testTrimetricProjection)
{
    auto rot = Gui::Camera::trimetric();
    auto lengths = getProjectedLengths(rot);

    // In trimetric, all should differ significantly
    EXPECT_GT(std::abs(lengths[0] - lengths[1]), 1e-3);
    EXPECT_GT(std::abs(lengths[1] - lengths[2]), 1e-3);
    EXPECT_GT(std::abs(lengths[0] - lengths[2]), 1e-3);
}


class CameraHelpers: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        // The helpers exercise Coin math (SbRotation from/to) that can post
        // diagnostics; initialize Coin the same way the other Coin tests do.
        SoDB::init();
    }
};

TEST_F(CameraHelpers, rotationFromBasisIdentity)
{
    // The canonical basis is the identity rotation (Top view).
    EXPECT_TRUE(Gui::Camera::rotationsMatch(
        Gui::Camera::rotationFromBasis({1, 0, 0}, {0, 1, 0}, {0, 0, 1}),
        Gui::Camera::top(),
        1e-12F));
}

TEST_F(CameraHelpers, alignZAxisNoop)
{
    // An identity camera already looks with +Z along (0,0,1), so aligning to
    // (0,0,1) must not change the orientation.
    SbRotation identity;
    EXPECT_TRUE(Gui::Camera::rotationsMatch(
        Gui::Camera::alignZAxis(identity, {0, 0, 1}), identity, 1e-12F));
}

TEST_F(CameraHelpers, alignZAxisPointsZAtTarget)
{
    // After aligning an identity camera to (1,0,0), its +Z axis points at (1,0,0).
    SbRotation identity;
    const SbRotation result = Gui::Camera::alignZAxis(identity, {1, 0, 0});
    SbVec3f z;
    result.multVec(SbVec3f(0, 0, 1), z);
    EXPECT_NEAR(z[0], 1.0f, 1e-5f);
    EXPECT_NEAR(z[1], 0.0f, 1e-5f);
    EXPECT_NEAR(z[2], 0.0f, 1e-5f);
}

TEST_F(CameraHelpers, naviCubeMainFacesMatchStandardViews)
{
    // These (x, z) face frames are the ones NaviCube::getFaceRotation maps its
    // six main faces to. They must equal the standard camera views, so that a
    // face click yields the same orientation as the corresponding View menu
    // command. This pins the equivalence the NaviCube refactor relies on.
    struct Face
    {
        SbVec3f x;
        SbVec3f z;
        Gui::Camera::Orientation view;
    };
    const Face faces[] = {
        {{1, 0, 0}, {0, 0, 1}, Gui::Camera::Top},
        {{1, 0, 0}, {0, -1, 0}, Gui::Camera::Front},
        {{0, -1, 0}, {-1, 0, 0}, Gui::Camera::Left},
        {{-1, 0, 0}, {0, 1, 0}, Gui::Camera::Rear},
        {{0, 1, 0}, {1, 0, 0}, Gui::Camera::Right},
        {{1, 0, 0}, {0, 0, -1}, Gui::Camera::Bottom},
    };

    for (const auto& f : faces) {
        const SbRotation face = Gui::Camera::faceOrientation(f.x, f.z);
        const SbRotation view = Gui::Camera::rotation(f.view);
        EXPECT_TRUE(Gui::Camera::rotationsMatch(face, view, 1e-9F))
            << "face frame x=(" << f.x[0] << ", " << f.x[1] << ", " << f.x[2]
            << ") z=(" << f.z[0] << ", " << f.z[1] << ", " << f.z[2]
            << ") face=" << face.getValue()[0] << " " << face.getValue()[1]
            << " " << face.getValue()[2] << " " << face.getValue()[3]
            << " view=" << view.getValue()[0] << " " << view.getValue()[1]
            << " " << view.getValue()[2] << " " << view.getValue()[3];
    }
}


class CameraFitToBox: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        SoDB::init();  // idempotent; constructing a Coin node needs the type database
    }

    void SetUp() override
    {
        camera.reset(new SoOrthographicCamera);
    }

    Gui::CoinPtr<SoOrthographicCamera> camera;
};

TEST_F(CameraFitToBox, aCubeIsFramedWithTheMargin)
{
    // The default orientation looks down -Z, so camera space is world space.
    const SbBox3f box(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, box, 1.0F);

    EXPECT_FLOAT_EQ(camera->height.getValue(), 2.0F * Gui::Camera::fitMargin);

    const SbVec3f position = camera->position.getValue();
    EXPECT_FLOAT_EQ(position[0], 0.0F);
    EXPECT_FLOAT_EQ(position[1], 0.0F);
    EXPECT_FLOAT_EQ(position[2], std::sqrt(3.0F));
}

TEST_F(CameraFitToBox, noCornerIsClipped)
{
    // A different orientation and a non-square frame from theContentReachesTheMargin below.
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Dimetric));
    const SbBox3f box(-30.0F, -8.0F, -12.0F, 70.0F, 22.0F, 5.0F);

    Gui::Camera::fitToBox(*camera, box, 1.6F);

    EXPECT_LE(largestNormalizedExtent(*camera, box, 1.6F), 1.0F);
}

TEST_F(CameraFitToBox, theClippingPlanesClearTheScene)
{
    // The isometric direction runs along a cube's body diagonal, so a cubic box puts its nearest
    // and farthest corners hard against tangent clipping planes.
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, box, 1.0F);

    EXPECT_LT(largestNormalizedDepth(*camera, box, 1.0F), 1.0F);
}

TEST_F(CameraFitToBox, theContentReachesTheMargin)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-30.0F, -8.0F, -12.0F, 70.0F, 22.0F, 5.0F);

    Gui::Camera::fitToBox(*camera, box, 1.0F);

    // A circumscribing-sphere fit leaves this well short of the margin.
    EXPECT_NEAR(largestNormalizedExtent(*camera, box, 1.0F), 1.0F / Gui::Camera::fitMargin, 1e-4F);
}

TEST_F(CameraFitToBox, aTranslatedBoxFramesIdentically)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f box(-1.0F, -2.0F, -3.0F, 4.0F, 5.0F, 6.0F);
    const SbVec3f offset(100.0F, -50.0F, 25.0F);
    const SbBox3f moved(box.getMin() + offset, box.getMax() + offset);

    Gui::Camera::fitToBox(*camera, box, 1.0F);
    const float heightAtOrigin = camera->height.getValue();
    const SbVec3f positionAtOrigin = camera->position.getValue();

    Gui::Camera::fitToBox(*camera, moved, 1.0F);
    const SbVec3f movedPosition = camera->position.getValue();

    EXPECT_FLOAT_EQ(camera->height.getValue(), heightAtOrigin);
    EXPECT_NEAR(movedPosition[0], positionAtOrigin[0] + offset[0], 1e-3F);
    EXPECT_NEAR(movedPosition[1], positionAtOrigin[1] + offset[1], 1e-3F);
    EXPECT_NEAR(movedPosition[2], positionAtOrigin[2] + offset[2], 1e-3F);
}

TEST_F(CameraFitToBox, anElongatedRodBeatsTheSphereFit)
{
    camera->orientation.setValue(Gui::Camera::rotation(Gui::Camera::Isometric));
    const SbBox3f rod(-0.5F, -0.5F, -50.0F, 0.5F, 0.5F, 50.0F);

    Gui::Camera::fitToBox(*camera, rod, 1.0F);

    SbSphere circumscribed;
    circumscribed.circumscribe(rod);

    // Coin's viewBoundingBox would hand the rod a frame of 2 * radius and leave it swimming.
    EXPECT_LT(camera->height.getValue(), 2.0F * circumscribed.getRadius());
    EXPECT_NEAR(largestNormalizedExtent(*camera, rod, 1.0F), 1.0F / Gui::Camera::fitMargin, 1e-4F);
}

TEST_F(CameraFitToBox, theAspectRatioDecidesWhichExtentConstrains)
{
    // Half extents are 2 across and 1 up in camera space.
    const SbBox3f box(-2.0F, -1.0F, -1.0F, 2.0F, 1.0F, 1.0F);

    Gui::Camera::fitToBox(*camera, box, 1.0F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 4.0F * Gui::Camera::fitMargin);

    Gui::Camera::fitToBox(*camera, box, 2.0F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 2.0F * Gui::Camera::fitMargin);

    Gui::Camera::fitToBox(*camera, box, 0.5F);
    EXPECT_FLOAT_EQ(camera->height.getValue(), 8.0F * Gui::Camera::fitMargin);
}

TEST_F(CameraFitToBox, anEmptyBoxLeavesTheCameraAlone)
{
    expectFitDeclines(*camera, SbBox3f());
}

TEST_F(CameraFitToBox, aPointBoxLeavesTheCameraAlone)
{
    // A single-point scene is a valid bounding box, not an empty one, so it reaches the fit.
    expectFitDeclines(*camera, SbBox3f(3.0F, 4.0F, 5.0F, 3.0F, 4.0F, 5.0F));
}
