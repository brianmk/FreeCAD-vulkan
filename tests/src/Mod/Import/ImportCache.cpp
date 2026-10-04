// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Correctness tests for the STEP import conversion cache
 *   (Import::ReaderStep::tryReadFromCache / writeCache).
 *
 *   The cache skips the STEP parse + XCAF transfer on a repeated import and is
 *   keyed by the source file's path, size and mtime. These tests verify the
 *   observable contract:
 *     - a miss before anything was cached,
 *     - a hit after writeCache(),
 *     - the cached document matches the freshly transferred one,
 *     - touching the source file invalidates a previously written entry.
 **************************************************************************/

#include <gtest/gtest.h>

#include <src/App/InitApplication.h>

#include <App/Application.h>
#include <Base/FileInfo.h>
#include <Mod/Import/App/ReaderStep.h>
#include <Mod/Import/App/WriterStep.h>
#include <Mod/Part/App/OCAF/ImportExportSettings.h>

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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

#include <chrono>
#include <filesystem>
#include <string>

namespace
{

Handle(TDocStd_Document) newDocument()
{
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument(
        TCollection_ExtendedString("MDTV-CAF"),
        doc
    );
    return doc;
}

TopoDS_Shape makeShape()
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(0.0, 0.0, 0.0), 10.0, 10.0, 10.0));
    builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(20.0, 0.0, 0.0), 5.0, 5.0, 5.0));
    return compound;
}

void addShape(const Handle(TDocStd_Document)& doc, const TopoDS_Shape& shape)
{
    Handle(XCAFDoc_ShapeTool) tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    tool->AddShape(shape, Standard_False);
}

long countFaces(const Handle(TDocStd_Document)& doc)
{
    long faces = 0;
    Handle(XCAFDoc_ShapeTool) tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    TDF_LabelSequence labels;
    tool->GetFreeShapes(labels);
    for (Standard_Integer i = 1; i <= labels.Length(); ++i) {
        for (TopExp_Explorer exp(tool->GetShape(labels.Value(i)), TopAbs_FACE); exp.More();
             exp.Next()) {
            ++faces;
        }
    }
    return faces;
}

}  // namespace

class ImportCacheTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        // Make the cache path explicit so a local user config cannot disable it.
        Part::OCAF::ImportExportSettings().setUseStepImportCache(true);
    }

    void SetUp() override
    {
        static long counter = 0;
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto dir = std::filesystem::temp_directory_path() / "freecad_import_cache_test";
        std::filesystem::create_directories(dir);
        stepPath_ = dir
            / ("cache_" + std::to_string(unique) + "_" + std::to_string(counter++) + ".step");

        Handle(TDocStd_Document) source = newDocument();
        addShape(source, makeShape());
        Import::WriterStep writer(Base::FileInfo(stepPath_.string()));
        writer.write(source);
        XCAFApp_Application::GetApplication()->Close(source);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(stepPath_, ec);
    }

    std::filesystem::path stepPath_;
};

TEST_F(ImportCacheTest, missBeforeWriteThenHitAfter)
{
    Import::ReaderStep reader(Base::FileInfo(stepPath_.string()));

    Handle(TDocStd_Document) early;
    EXPECT_FALSE(reader.tryReadFromCache(early));
    EXPECT_TRUE(early.IsNull());

    Handle(TDocStd_Document) cold = newDocument();
    reader.read(cold);
    reader.writeCache(cold);

    Handle(TDocStd_Document) warm;
    EXPECT_TRUE(reader.tryReadFromCache(warm));
    ASSERT_FALSE(warm.IsNull());
}

TEST_F(ImportCacheTest, cachedDocumentMatchesFreshTransfer)
{
    Import::ReaderStep reader(Base::FileInfo(stepPath_.string()));

    Handle(TDocStd_Document) cold = newDocument();
    reader.read(cold);
    const long coldFaces = countFaces(cold);
    // The source is a compound of two boxes: 2 * 6 faces.
    ASSERT_EQ(coldFaces, 12L);

    reader.writeCache(cold);

    Handle(TDocStd_Document) warm;
    ASSERT_TRUE(reader.tryReadFromCache(warm));
    EXPECT_EQ(countFaces(warm), coldFaces);
}

TEST_F(ImportCacheTest, disabledCacheAlwaysMisses)
{
    // Control for the hit tests: with the preference off, nothing is written
    // and every lookup misses.
    Part::OCAF::ImportExportSettings settings;
    settings.setUseStepImportCache(false);

    Import::ReaderStep reader(Base::FileInfo(stepPath_.string()));
    Handle(TDocStd_Document) cold = newDocument();
    reader.read(cold);
    reader.writeCache(cold);

    Handle(TDocStd_Document) warm;
    EXPECT_FALSE(reader.tryReadFromCache(warm));

    settings.setUseStepImportCache(true);
}

TEST_F(ImportCacheTest, changingTheSourceInvalidatesTheCache)
{
    Import::ReaderStep reader(Base::FileInfo(stepPath_.string()));

    Handle(TDocStd_Document) cold = newDocument();
    reader.read(cold);
    reader.writeCache(cold);

    Handle(TDocStd_Document) warm;
    ASSERT_TRUE(reader.tryReadFromCache(warm));

    // The cache key includes the mtime, so bumping it must miss again.
    const auto future = std::filesystem::file_time_type::clock::now()
        + std::chrono::seconds(120);
    std::filesystem::last_write_time(stepPath_, future);

    Handle(TDocStd_Document) stale;
    EXPECT_FALSE(reader.tryReadFromCache(stale));
}
