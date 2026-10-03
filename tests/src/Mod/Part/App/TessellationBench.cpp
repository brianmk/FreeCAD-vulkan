// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   A benchmark for the OCCT triangulation -> vertex-array conversion that
 *   ViewProviderPartExt::setupCoinGeometry() performs, compared against a
 *   direct Poly_Triangulation -> interleaved vertex buffer pack.
 *
 *   It is registered with CTest (PartTessellationBench) but is a plain
 *   benchmark: it always succeeds and only reports timings. By default it
 *   tessellates a synthetic compound of spheres, so it is self-contained and
 *   fast; set PART_TESS_BENCH_MODEL to a .brep file to benchmark a real model,
 *   and PART_TESS_BENCH_GRID to change the synthetic grid size.
 *
 *   Note: the "coin-style" pass produces smooth (cross-product accumulated and
 *   normalized) normals like setupCoinGeometry(), while the direct pass writes
 *   flat normals; the direct path could reuse Poly_Triangulation::Normal() when
 *   the mesh carries normals. The comparison is about the data layout and the
 *   number of passes over the mesh, not a like-for-like shading algorithm.
 **************************************************************************/

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <IMeshTools_Parameters.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

TopoDS_Shape makeSynthetic(int grid)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    const double radius = 4.0;
    const double step = 10.0;
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(i * step, j * step, 0.0), radius);
            builder.Add(compound, sphere);
        }
    }
    return compound;
}

bool loadShape(const char* path, TopoDS_Shape& shape)
{
    BRep_Builder builder;
    return BRepTools::Read(shape, path, builder);
}

struct MeshStats
{
    long nodes = 0;
    long triangles = 0;
};

// The per-face iteration shared by both conversion passes.
template<typename FaceFn>
void forEachTriangle(
    const std::vector<TopoDS_Face>& faces,
    const FaceFn& fn
)
{
    for (const auto& face : faces) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, loc);
        if (mesh.IsNull()) {
            continue;
        }
        gp_Trsf trsf;
        const bool identity = loc.IsIdentity();
        if (!identity) {
            trsf = loc.Transformation();
        }
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        fn(face, mesh, loc, trsf, identity, reversed);
    }
}

}  // namespace

int main()
{
    const int grid = std::getenv("PART_TESS_BENCH_GRID") != nullptr
        ? std::max(1, std::atoi(std::getenv("PART_TESS_BENCH_GRID")))
        : 4;

    TopoDS_Shape shape;
    const char* model = std::getenv("PART_TESS_BENCH_MODEL");
    if (model != nullptr && *model != '\0') {
        if (!loadShape(model, shape)) {
            std::fprintf(stderr, "failed to read model '%s'\n", model);
            return 1;
        }
        std::printf("model: %s\n", model);
    }
    else {
        shape = makeSynthetic(grid);
        std::printf("model: synthetic %dx%d sphere grid\n", grid, grid);
    }

    const double deflection = 0.1;
    const double angle = 0.0349;

    auto t0 = Clock::now();
    BRepTools::Clean(shape, Standard_True);
    IMeshTools_Parameters params;
    params.Deflection = deflection;
    params.Relative = Standard_False;
    params.Angle = angle;
    params.InParallel = Standard_True;
    BRepMesh_IncrementalMesh(shape, params);
    auto t1 = Clock::now();

    std::vector<TopoDS_Face> faces;
    MeshStats stats;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face face = TopoDS::Face(exp.Current());
        faces.push_back(face);
        TopLoc_Location loc;
        Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, loc);
        if (!mesh.IsNull()) {
            stats.nodes += mesh->NbNodes();
            stats.triangles += mesh->NbTriangles();
        }
    }

    std::printf(
        "faces=%zu nodes=%ld triangles=%ld  mesh=%.1f ms\n",
        faces.size(),
        stats.nodes,
        stats.triangles,
        elapsedMs(t0, t1)
    );
    if (stats.triangles == 0) {
        std::printf("no triangulation produced\n");
        return 0;
    }

    // Pass A: the SoCoordinate3 + SoNormal layout (smooth, accumulated normals).
    std::vector<float> coords(static_cast<size_t>(stats.nodes) * 3);
    std::vector<float> normals(static_cast<size_t>(stats.nodes) * 3, 0.0f);
    std::vector<int32_t> index(static_cast<size_t>(stats.triangles) * 4);

    auto a0 = Clock::now();
    {
        long nodeOffset = 0;
        long triOffset = 0;
        forEachTriangle(faces, [&](const TopoDS_Face&,
                                   const Handle(Poly_Triangulation)& mesh,
                                   const TopLoc_Location&,
                                   const gp_Trsf& trsf,
                                   bool identity,
                                   bool reversed) {
            for (int g = 1; g <= mesh->NbTriangles(); ++g) {
                Standard_Integer n1 = 0, n2 = 0, n3 = 0;
                mesh->Triangle(g).Get(n1, n2, n3);
                if (reversed) {
                    std::swap(n2, n3);
                }
                gp_Pnt points[3] = {mesh->Node(n1), mesh->Node(n2), mesh->Node(n3)};
                if (!identity) {
                    points[0].Transform(trsf);
                    points[1].Transform(trsf);
                    points[2].Transform(trsf);
                }
                gp_Vec faceNormal = gp_Vec(points[0], points[1]).Crossed(gp_Vec(points[0], points[2]));
                if (faceNormal.SquareMagnitude() > 0.0) {
                    faceNormal.Normalize();
                }
                const int base = static_cast<int>(nodeOffset);
                const int nodes[3] = {n1, n2, n3};
                for (int k = 0; k < 3; ++k) {
                    coords[3 * (base + nodes[k] - 1)] = float(points[k].X());
                    coords[3 * (base + nodes[k] - 1) + 1] = float(points[k].Y());
                    coords[3 * (base + nodes[k] - 1) + 2] = float(points[k].Z());
                    normals[3 * (base + nodes[k] - 1)] += float(faceNormal.X());
                    normals[3 * (base + nodes[k] - 1) + 1] += float(faceNormal.Y());
                    normals[3 * (base + nodes[k] - 1) + 2] += float(faceNormal.Z());
                }
                index[4 * (triOffset + g - 1)] = base + n1 - 1;
                index[4 * (triOffset + g - 1) + 1] = base + n2 - 1;
                index[4 * (triOffset + g - 1) + 2] = base + n3 - 1;
                index[4 * (triOffset + g - 1) + 3] = -1;
            }
            nodeOffset += mesh->NbNodes();
            triOffset += mesh->NbTriangles();
        });
        for (size_t i = 0; i < normals.size(); i += 3) {
            const float length = std::sqrt(
                normals[i] * normals[i] + normals[i + 1] * normals[i + 1]
                + normals[i + 2] * normals[i + 2]
            );
            if (length > 0.0f) {
                normals[i] /= length;
                normals[i + 1] /= length;
                normals[i + 2] /= length;
            }
        }
    }
    auto a1 = Clock::now();

    // Pass B: direct Poly_Triangulation -> 32-byte interleaved vertex buffer
    // (position f32x3, normal f32x3, packed color RGBA8, uv RG16).
    constexpr size_t vertexStride = 32;
    std::vector<uint8_t> interleaved(static_cast<size_t>(stats.nodes) * vertexStride);

    auto b0 = Clock::now();
    {
        long nodeOffset = 0;
        forEachTriangle(faces, [&](const TopoDS_Face&,
                                   const Handle(Poly_Triangulation)& mesh,
                                   const TopLoc_Location&,
                                   const gp_Trsf& trsf,
                                   bool identity,
                                   bool) {
            for (int i = 1; i <= mesh->NbNodes(); ++i) {
                gp_Pnt p = mesh->Node(i);
                if (!identity) {
                    p.Transform(trsf);
                }
                uint8_t* out = &interleaved[(nodeOffset + i - 1) * vertexStride];
                const float pos[3] = {float(p.X()), float(p.Y()), float(p.Z())};
                std::memcpy(out, pos, sizeof(pos));
                const float nrm[3] = {0.0f, 0.0f, 1.0f};
                std::memcpy(out + 12, nrm, sizeof(nrm));
                const uint32_t color = 0xffffffffu;
                std::memcpy(out + 24, &color, sizeof(color));
                const uint32_t uv = 0;
                std::memcpy(out + 28, &uv, sizeof(uv));
            }
            nodeOffset += mesh->NbNodes();
        });
    }
    auto b1 = Clock::now();

    const double setupMs = elapsedMs(a0, a1);
    const double directMs = elapsedMs(b0, b1);
    std::printf("coin-style (coords+normals, smooth): %8.2f ms\n", setupMs);
    std::printf("direct interleaved (flat):           %8.2f ms\n", directMs);
    std::printf("speedup: %.2fx\n", setupMs / directMs);
    return 0;
}
