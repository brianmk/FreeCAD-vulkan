// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2023 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/


#include <filesystem>
#include <sys/stat.h>
#include <cstdio>
#include <cstdint>
#include <string>

#include <Standard_Version.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <PCDM_ReaderStatus.hxx>
#include <PCDM_StoreStatus.hxx>
#include <TCollection_ExtendedString.hxx>
#include <Transfer_TransientProcess.hxx>
#include <XCAFApp_Application.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>


#include "ReaderStep.h"
#include <App/Application.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Part/App/OCAF/ImportExportSettings.h>
#include <Mod/Part/App/encodeFilename.h>

using namespace Import;

ReaderStep::ReaderStep(const Base::FileInfo& file)  // NOLINT
    : file {file}
{
#if OCC_VERSION_HEX >= 0x070800
    codePage = Resource_FormatType_UTF8;
#endif
}

void ReaderStep::read(Handle(TDocStd_Document) hDoc, const Message_ProgressRange& theProgress)
{
    std::string utf8Name = file.filePath();
    std::string name8bit = Part::encodeFilename(utf8Name);
    STEPCAFControl_Reader aReader;
    aReader.SetColorMode(true);
    aReader.SetNameMode(true);
    aReader.SetLayerMode(true);
    aReader.SetSHUOMode(true);
#if OCC_VERSION_HEX < 0x070800
    if (aReader.ReadFile(name8bit.c_str()) != IFSelect_RetDone) {
#else
    Handle(StepData_StepModel) aStepModel = new StepData_StepModel;
    aStepModel->InternalParameters.InitFromStatic();
    aStepModel->SetSourceCodePage(codePage);
    if (aReader.ReadFile(name8bit.c_str(), aStepModel->InternalParameters) != IFSelect_RetDone) {
#endif
        throw Base::FileException("Cannot read STEP file", file);
    }

    aReader.Transfer(hDoc, theProgress);
}

namespace {

// Cache layout: <user cache dir>/step-ocaf/<fnv1a(path|size|mtime)>.xbf
// The key is derived from the source file, so a modified STEP file never hits a
// stale conversion. The entry is stored as a binary OCAF document (BinXCAF):
// the default MDTV-CAF format needs a PCDM storage plugin resource that is not
// shipped with the OCCT packaging, whereas BinXCAF/TKBinXCAF is always present.

uint64_t fnv1a(const std::string& data)
{
    uint64_t hash = 1469598103934665603ULL;
    for (char c : data) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string getCachePath(const Base::FileInfo& file)
{
    struct stat st {};
    if (::stat(file.filePath().c_str(), &st) != 0) {
        return {};
    }

    std::string key = file.filePath() + '\x1f'
        + std::to_string(static_cast<intmax_t>(st.st_size)) + '\x1f'
        + std::to_string(static_cast<long long>(st.st_mtime));

    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(fnv1a(key)));

    std::error_code ec;
    const std::string dir = App::Application::getUserCachePath() + "/step-ocaf";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return {};
    }

    return dir + '/' + buffer + ".xbf";
}

bool isCacheEnabled()
{
    return Part::OCAF::ImportExportSettings().getUseStepImportCache();
}

}  // anonymous namespace

bool ReaderStep::tryReadFromCache(Handle(TDocStd_Document)& outDoc) const
{
    outDoc.Nullify();
    if (!isCacheEnabled()) {
        return false;
    }

    const std::string path = getCachePath(file);
    if (path.empty() || !Base::FileInfo(path).exists()) {
        return false;
    }

    Handle(TDocStd_Document) doc;
    const PCDM_ReaderStatus status = XCAFApp_Application::GetApplication()
                                         ->Open(TCollection_ExtendedString(path.c_str()), doc, Message_ProgressRange());
    if (status != PCDM_RS_OK) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }

    Base::Console().message("STEP import cache hit: {}\n", path);
    outDoc = doc;
    return true;
}

void ReaderStep::writeCache(Handle(TDocStd_Document) hDoc) const
{
    if (hDoc.IsNull() || !isCacheEnabled()) {
        return;
    }

    const std::string path = getCachePath(file);
    if (path.empty()) {
        return;
    }

    try {
        // MDTV-CAF cannot be stored with this OCCT packaging (its PCDM storage
        // plugin resource is missing), so save the document as binary OCAF.
        hDoc->ChangeStorageFormat("BinXCAF");

        // Write to a temporary file and move it into place so a crashed write
        // never leaves a half-written cache entry behind. The name must keep
        // the .xbf suffix, otherwise SaveAs appends it a second time.
        const std::string tmpPath = path.substr(0, path.size() - 4) + ".tmp.xbf";
        const PCDM_StoreStatus status = XCAFApp_Application::GetApplication()
                                            ->SaveAs(hDoc, TCollection_ExtendedString(tmpPath.c_str()), Message_ProgressRange());
        std::error_code ec;
        if (status == PCDM_SS_OK) {
            std::filesystem::rename(tmpPath, path, ec);
            Base::Console().message("STEP import cache updated: {}\n", path);
        }
        else {
            std::filesystem::remove(tmpPath, ec);
        }
    }
    catch (const Standard_Failure&) {
        Base::Console().warning("Could not write the STEP import cache entry for {}\n", file.filePath());
    }
}
