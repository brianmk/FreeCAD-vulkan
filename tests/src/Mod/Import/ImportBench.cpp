// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   A benchmark for the STEP import pipeline phases:
 *
 *     cold import  = STEPCAFControl_Reader parse + XCAF transfer
 *                    (Import::ReaderStep::read)
 *     cache write  = serialise the transferred document to the import cache
 *                    (Import::ReaderStep::writeCache)
 *     cache hit    = load the converted document from the cache
 *                    (Import::ReaderStep::tryReadFromCache)
 *
 *   The cache-hit path is what the import work added: it skips STEP parsing
 *   and transfer entirely, so the benchmark is a regression guard for that
 *   speedup.
 *
 *   It is registered with CTest (ImportBench) but is a plain benchmark: it
 *   always succeeds and only reports timings. By default it builds a synthetic
 *   compound of boxes, writes it to a temporary .step file and round-trips it,
 *   so it is self-contained; set PART_IMPORT_BENCH_MODEL to an existing STEP
 *   file to benchmark a real model instead and PART_IMPORT_BENCH_GRID to
 *   change the synthetic grid size.
 **************************************************************************/

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <Base/FileInfo.h>
#include <Mod/Import/App/ReaderStep.h>
#include <Mod/Import/App/WriterStep.h>
#include <Mod/Part/App/OCAF/ImportExportSettings.h>

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopAbs.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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

    const double step = 10.0;
    const double size = 6.0;
    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            builder.Add(
                compound,
                BRepPrimAPI_MakeBox(gp_Pnt(i * step, j * step, 0.0), size, size, size)
            );
        }
    }
    return compound;
}

Handle(TDocStd_Document) newDocument()
{
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument(
        TCollection_ExtendedString("MDTV-CAF"),
        doc
    );
    return doc;
}

void addShape(const Handle(TDocStd_Document)& doc, const TopoDS_Shape& shape)
{
    Handle(XCAFDoc_ShapeTool) tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    tool->AddShape(shape, Standard_False);
}

struct MeshStats
{
    long faces = 0;
    long solids = 0;
};

MeshStats shapeStats(const Handle(TDocStd_Document)& doc)
{
    MeshStats stats;
    Handle(XCAFDoc_ShapeTool) tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    TDF_LabelSequence labels;
    tool->GetFreeShapes(labels);
    for (Standard_Integer i = 1; i <= labels.Length(); ++i) {
        const TopoDS_Shape shape = tool->GetShape(labels.Value(i));
        for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
            ++stats.faces;
        }
        for (TopExp_Explorer exp(shape, TopAbs_SOLID); exp.More(); exp.Next()) {
            ++stats.solids;
        }
    }
    return stats;
}

}  // namespace

int main()
{
    tests::initApplication();

    // The cache is on by default; make it explicit so the benchmark always
    // exercises the cache-hit path even if a local user config disabled it.
    Part::OCAF::ImportExportSettings settings;
    settings.setUseStepImportCache(true);

    const int grid = std::getenv("PART_IMPORT_BENCH_GRID") != nullptr
        ? std::max(1, std::atoi(std::getenv("PART_IMPORT_BENCH_GRID")))
        : 8;

    const char* model = std::getenv("PART_IMPORT_BENCH_MODEL");
    const bool synthetic = (model == nullptr || *model == '\0');

    std::filesystem::path stepPath;
    if (synthetic) {
        stepPath = std::filesystem::temp_directory_path() / "freecad_import_bench.step";
        std::printf("model: synthetic %dx%d box grid -> %s\n", grid, grid, stepPath.c_str());
    }
    else {
        stepPath = model;
        std::printf("model: %s\n", stepPath.c_str());
    }

    Base::FileInfo file(stepPath.string());

    try {
        if (synthetic) {
            Handle(TDocStd_Document) source = newDocument();
            addShape(source, makeSynthetic(grid));
            Import::WriterStep writer(file);
            writer.write(source);
            XCAFApp_Application::GetApplication()->Close(source);
        }

        if (!file.exists()) {
            std::fprintf(stderr, "model '%s' does not exist\n", file.filePath().c_str());
            return 1;
        }

        Import::ReaderStep reader(file);

        // Warm up OCCT's lazy initialization so the first timed read is fair.
        {
            Handle(TDocStd_Document) warmup = newDocument();
            reader.read(warmup);
            XCAFApp_Application::GetApplication()->Close(warmup);
        }

        auto c0 = Clock::now();
        Handle(TDocStd_Document) cold = newDocument();
        reader.read(cold);
        auto c1 = Clock::now();

        const MeshStats stats = shapeStats(cold);

        auto w0 = Clock::now();
        reader.writeCache(cold);
        auto w1 = Clock::now();

        Handle(TDocStd_Document) warm;
        auto h0 = Clock::now();
        const bool hit = reader.tryReadFromCache(warm);
        auto h1 = Clock::now();

        const double coldMs = elapsedMs(c0, c1);
        const double writeMs = elapsedMs(w0, w1);
        const double warmMs = elapsedMs(h0, h1);

        std::printf(
            "faces=%ld solids=%ld\n"
            "parse+transfer: %8.2f ms\n"
            "cache write:    %8.2f ms\n",
            stats.faces,
            stats.solids,
            coldMs,
            writeMs
        );
        if (hit) {
            std::printf("cache hit:      %8.2f ms  speedup=%.2fx\n", warmMs, coldMs / warmMs);
        }
        else {
            std::printf("cache hit:      MISS (no speedup measured)\n");
        }
        std::printf("cold total (parse+transfer+write): %8.2f ms\n", coldMs + writeMs);

        XCAFApp_Application::GetApplication()->Close(cold);
    }
    catch (const Standard_Failure& e) {
        std::fprintf(stderr, "import failed: %s\n", e.GetMessageString());
        return 1;
    }

    return 0;
}
