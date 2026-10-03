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

#pragma once

#include <Mod/Import/ImportGlobal.h>
#include <Base/FileInfo.h>
#include <Message_ProgressRange.hxx>
#include <Resource_FormatType.hxx>
#include <TDocStd_Document.hxx>
#include <StepData_StepModel.hxx>
#include <Standard_Version.hxx>

namespace Import
{

class ImportExport ReaderStep
{
public:
    explicit ReaderStep(const Base::FileInfo& file);
    void setCodePage(Resource_FormatType cp)
    {
        codePage = cp;
    }
    void read(
        Handle(TDocStd_Document) hDoc,
        const Message_ProgressRange& theProgress = Message_ProgressRange()
    );

    /**
     * Tries to load the STEP conversion from the import cache. On success the OCAF
     * document in @p outDoc contains the fully transferred model and true is
     * returned. On a miss (or when caching is disabled) false is returned and
     * @p outDoc stays null.
     */
    bool tryReadFromCache(Handle(TDocStd_Document)& outDoc) const;

    /**
     * Stores the converted OCAF document in the import cache so that the next
     * import of the same, unchanged file can skip the STEP parsing and transfer.
     * This is a best effort operation: failures are reported but never abort
     * the import.
     */
    void writeCache(Handle(TDocStd_Document) hDoc) const;

private:
    Base::FileInfo file;
    Resource_FormatType codePage {};
};

}  // namespace Import
