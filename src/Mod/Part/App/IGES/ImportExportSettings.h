// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2022 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#pragma once

#include <Mod/Part/App/ImportExportSettingsBase.h>

namespace Part
{

namespace IGES
{

class PartExport ImportExportSettings: public ImportExportSettingsBase
{
public:
    ImportExportSettings();

    bool getSkipBlankEntities() const;
    void setSkipBlankEntities(bool) const;

    bool getBRepMode() const;
    void setBRepMode(bool) const;

private:
    void applyUnit(Interface::Unit) override;

    std::string defaultCompany() const override;
    void applyCompany(const char*) override;

    std::string defaultAuthor() const override;
    void applyAuthor(const char*) override;

    std::string defaultProductName() const override;
    void applyProductName(const char*) override;
};

}  // namespace IGES
}  // namespace Part
