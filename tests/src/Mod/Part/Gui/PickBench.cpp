// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   A benchmark for SoBrepFaceSet ray picking (the hover/preselection path).
 *
 *   A merged Part::Feature becomes a single SoBrepFaceSet with millions of
 *   triangles. This builds a synthetic mesh with many faces and many triangles
 *   per face and compares the default linear pick (no face bounding boxes) with
 *   the per-face-bbox culled pick, so a regression in the culling shows up as a
 *   measured slowdown here.
 *
 *   It is registered with CTest (PartPickBench) but is a plain benchmark: it
 *   always succeeds and only reports timings. Configure the synthetic mesh with
 *   PART_PICK_BENCH_FACES / PART_PICK_BENCH_TRIS / PART_PICK_BENCH_PICKS.
 **************************************************************************/

#include <Inventor/SbBox3f.h>
#include <Inventor/SbVec3f.h>
#include <Inventor/SbViewportRegion.h>
#include <Inventor/SoDB.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/actions/SoRayPickAction.h>
#include <Inventor/details/SoFaceDetail.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Inventor/details/SoLineDetail.h>

#include <Mod/Part/Gui/SoBrepEdgeSet.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

constexpr double pi = 3.14159265358979323846;

double elapsedMs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int envInt(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

//! A strip of `faces` triangle fans along +X, `trisPerFace` triangles each.
//! Face i covers the unit square centred at (i + 0.5, 0.5), so a ray through
//! the centre must hit exactly one face. With boxes the pick culls all other
//! faces; without boxes it tests every triangle.
struct Strip
{
    SoSeparator* root {nullptr};
    SoCoordinate3* coords {nullptr};
    PartGui::SoBrepFaceSet* faces {nullptr};

    Strip(int faceCount, int trisPerFace, bool withBoxes)
    {
        root = new SoSeparator;
        root->ref();
        coords = new SoCoordinate3;
        faces = new PartGui::SoBrepFaceSet;
        root->addChild(coords);
        root->addChild(faces);

        const int coordCount = faceCount * trisPerFace * 3;
        coords->point.setNum(coordCount);
        faces->coordIndex.setNum(faceCount * trisPerFace * 4);
        faces->partIndex.setNum(faceCount);

        SbVec3f* points = coords->point.startEditing();
        int32_t* indices = faces->coordIndex.startEditing();
        int32_t* parts = faces->partIndex.startEditing();

        std::vector<SbBox3f> boxes;
        boxes.reserve(faceCount);

        int coord = 0;
        int index = 0;
        for (int f = 0; f < faceCount; ++f) {
            const float cx = static_cast<float>(f) + 0.5f;
            const float cy = 0.5f;
            const SbVec3f apex(cx, cy, 0.0f);

            SbBox3f box;
            for (int t = 0; t < trisPerFace; ++t) {
                const double a0 = 2.0 * pi * t / trisPerFace;
                const double a1 = 2.0 * pi * (t + 1) / trisPerFace;
                const SbVec3f v1(
                    cx + 0.5f * static_cast<float>(std::cos(a0)),
                    cy + 0.5f * static_cast<float>(std::sin(a0)),
                    0.0f
                );
                const SbVec3f v2(
                    cx + 0.5f * static_cast<float>(std::cos(a1)),
                    cy + 0.5f * static_cast<float>(std::sin(a1)),
                    0.0f
                );

                points[coord] = apex;
                points[coord + 1] = v1;
                points[coord + 2] = v2;
                indices[index] = coord;
                indices[index + 1] = coord + 1;
                indices[index + 2] = coord + 2;
                indices[index + 3] = -1;
                coord += 3;
                index += 4;

                box.extendBy(apex);
                box.extendBy(v1);
                box.extendBy(v2);
            }
            parts[f] = trisPerFace;
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

    int pick(int face) const
    {
        SbViewportRegion viewport(256, 256);
        SoRayPickAction action(viewport);
        const float x = static_cast<float>(face) + 0.5f;
        action.setRay(SbVec3f(x, 0.5f, 10.0f), SbVec3f(0.0f, 0.0f, -1.0f));
        action.apply(root);
        const SoPickedPoint* pp = action.getPickedPoint();
        if (!pp) {
            return -1;
        }
        const auto* detail = dynamic_cast<const SoFaceDetail*>(pp->getDetail());
        return detail ? detail->getPartIndex() : -1;
    }
};

double benchPick(const Strip& strip, int faceCount, int picks)
{
    // Touch every face once so a representative spread of rays is measured.
    const auto start = Clock::now();
    for (int p = 0; p < picks; ++p) {
        strip.pick((p * 7 + 3) % faceCount);
    }
    return elapsedMs(start, Clock::now()) / std::max(picks, 1);
}

//! `edges` vertical polylines at x = i, each `segs` segments, y in [0, 1].
//! The SoBrepEdgeSet pick path is still a linear scan (no cull yet), so this
//! reports the baseline the hover cost is currently dominated by.
struct EdgeStrip
{
    SoSeparator* root {nullptr};
    SoCoordinate3* coords {nullptr};
    PartGui::SoBrepEdgeSet* edges {nullptr};

    EdgeStrip(int edgeCount, int segs)
    {
        root = new SoSeparator;
        root->ref();
        coords = new SoCoordinate3;
        edges = new PartGui::SoBrepEdgeSet;
        root->addChild(coords);
        root->addChild(edges);

        const int pointsPerEdge = segs + 1;
        coords->point.setNum(edgeCount * pointsPerEdge);
        edges->coordIndex.setNum(edgeCount * (pointsPerEdge + 1));

        SbVec3f* points = coords->point.startEditing();
        int32_t* indices = edges->coordIndex.startEditing();
        std::vector<int> mapping;
        mapping.reserve(edgeCount);

        int coord = 0;
        int index = 0;
        for (int e = 0; e < edgeCount; ++e) {
            const float x = static_cast<float>(e);
            for (int s = 0; s < pointsPerEdge; ++s) {
                points[coord] = SbVec3f(x, static_cast<float>(s) / segs, 0.0f);
                indices[index++] = coord++;
            }
            indices[index++] = -1;
            mapping.push_back(e);
        }
        coords->point.finishEditing();
        edges->coordIndex.finishEditing();
        edges->setEdgeMapping(std::move(mapping));
    }

    ~EdgeStrip()
    {
        root->unref();
    }

    EdgeStrip(const EdgeStrip&) = delete;
    EdgeStrip& operator=(const EdgeStrip&) = delete;

    int pick(int edge) const
    {
        return pickWithRadius(edge, -1.0f);
    }

    //! Pick with an explicit pick radius (the cone radius the line-segment test
    //! uses); a negative radius leaves Coin's default in place.
    int pickWithRadius(int edge, float radius) const
    {
        SbViewportRegion viewport(256, 256);
        SoRayPickAction action(viewport);
        if (radius >= 0.0f) {
            action.setRadius(radius);
        }
        action.setRay(
            SbVec3f(static_cast<float>(edge), 0.5f, 10.0f),
            SbVec3f(0.0f, 0.0f, -1.0f)
        );
        action.apply(root);
        const SoPickedPoint* pp = action.getPickedPoint();
        if (!pp) {
            return -1;
        }
        const auto* detail = dynamic_cast<const SoLineDetail*>(pp->getDetail());
        return detail ? detail->getPartIndex() : -1;
    }
};

double benchEdgePick(const EdgeStrip& strip, int edgeCount, int picks, float radius = -1.0f)
{
    const auto start = Clock::now();
    for (int p = 0; p < picks; ++p) {
        strip.pickWithRadius((p * 7 + 3) % edgeCount, radius);
    }
    return elapsedMs(start, Clock::now()) / std::max(picks, 1);
}

}  // namespace

int main()
{
    SoDB::init();
    if (PartGui::SoBrepFaceSet::getClassTypeId().isBad()) {
        PartGui::SoBrepFaceSet::initClass();
    }
    if (PartGui::SoBrepEdgeSet::getClassTypeId().isBad()) {
        PartGui::SoBrepEdgeSet::initClass();
    }

    const int faceCount = envInt("PART_PICK_BENCH_FACES", 4000);
    const int trisPerFace = envInt("PART_PICK_BENCH_TRIS", 64);
    const int picks = envInt("PART_PICK_BENCH_PICKS", 50);

    std::printf(
        "[PICKBENCH] faces=%d tris/face=%d total_tris=%d picks=%d\n",
        faceCount,
        trisPerFace,
        faceCount * trisPerFace,
        picks
    );

    const Strip unculled(faceCount, trisPerFace, false);
    const Strip culled(faceCount, trisPerFace, true);

    if (unculled.pick(faceCount / 2) != faceCount / 2
        || culled.pick(faceCount / 2) != faceCount / 2) {
        std::printf("[PICKBENCH] ERROR: pick returned the wrong face\n");
        return 1;
    }

    // Warm up, then measure.
    benchPick(unculled, faceCount, 3);
    benchPick(culled, faceCount, 3);

    const double unculledMs = benchPick(unculled, faceCount, picks);
    const double culledMs = benchPick(culled, faceCount, picks);

    std::printf("[PICKBENCH] unculled %.3f ms/pick\n", unculledMs);
    std::printf("[PICKBENCH] culled   %.3f ms/pick\n", culledMs);
    std::printf(
        "[PICKBENCH] speedup  %.1fx\n",
        culledMs > 0.0 ? unculledMs / culledMs : 0.0
    );

    // Optional scene-scale sweep: the culled pick should stay ~flat while the
    // unculled scan grows with the face count. Enabled with
    // PART_PICK_BENCH_SWEEP=1 so the default run stays fast.
    if (std::getenv("PART_PICK_BENCH_SWEEP")) {
        for (int scale : {500, 1000, 2000, 4000}) {
            const Strip scaleUnculled(scale, trisPerFace, false);
            const Strip scaleCulled(scale, trisPerFace, true);
            if (scaleUnculled.pick(scale / 2) != scale / 2
                || scaleCulled.pick(scale / 2) != scale / 2) {
                std::printf("[PICKBENCH] ERROR: scale pick returned the wrong face\n");
                return 1;
            }
            benchPick(scaleUnculled, scale, 3);
            benchPick(scaleCulled, scale, 3);
            const double scaleUnculledMs = benchPick(scaleUnculled, scale, picks);
            const double scaleCulledMs = benchPick(scaleCulled, scale, picks);
            std::printf(
                "[PICKBENCH] scale faces=%d unculled=%.3f culled=%.3f speedup=%.1fx\n",
                scale,
                scaleUnculledMs,
                scaleCulledMs,
                scaleCulledMs > 0.0 ? scaleUnculledMs / scaleCulledMs : 0.0
            );
        }
    }

    const int edgeCount = envInt("PART_PICK_BENCH_EDGES", 4000);
    const int edgeSegs = envInt("PART_PICK_BENCH_EDGE_SEGS", 8);
    const EdgeStrip edgeStrip(edgeCount, edgeSegs);
    const int gotEdge = edgeStrip.pick(edgeCount / 2);
    if (gotEdge != edgeCount / 2) {
        std::printf(
            "[PICKBENCH] ERROR: edge pick got %d expected %d\n",
            gotEdge,
            edgeCount / 2
        );
        return 1;
    }
    benchEdgePick(edgeStrip, edgeCount, 3);
    const double edgeMs = benchEdgePick(edgeStrip, edgeCount, picks);
    std::printf(
        "[PICKBENCH] edges=%d segs/edge=%d edge_culled=no %.3f ms/pick\n",
        edgeCount,
        edgeSegs,
        edgeMs
    );

    // The edge pick is still an unculled linear scan; show that its cost does
    // not depend on the pick (cone) radius, unlike a naive radius-inflated box
    // cull would.
    for (float radius : {2.0f, 10.0f, 40.0f}) {
        benchEdgePick(edgeStrip, edgeCount, 3, radius);
        const double radiusMs = benchEdgePick(edgeStrip, edgeCount, picks, radius);
        std::printf(
            "[PICKBENCH] edge radius=%.0fpx %.3f ms/pick\n",
            radius,
            radiusMs
        );
    }
    return 0;
}
