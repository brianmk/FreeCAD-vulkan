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

#include <string>

#include <Base/Parameter.h>
#include <Mod/Part/App/Interface.h>
#include <Mod/Part/PartGlobal.h>

namespace Part
{

/** Common base of the STEP and IGES import/export settings.
 *
 * It stores the settings in a parameter group and leaves the format specific
 * handling of the unit, company, author and product name to the derived class.
 */
class PartExport ImportExportSettingsBase
{
public:
    virtual ~ImportExportSettingsBase() = default;

    Interface::Unit getUnit() const;
    void setUnit(Interface::Unit);

    std::string getCompany() const;
    void setCompany(const char*);

    std::string getAuthor() const;
    void setAuthor(const char*);

    std::string getProductName() const;
    void setProductName(const char*);

protected:
    explicit ImportExportSettingsBase(const char* groupPath);

    virtual void applyUnit(Interface::Unit) = 0;

    virtual std::string defaultCompany() const;
    virtual void applyCompany(const char*);

    virtual std::string defaultAuthor() const;
    virtual void applyAuthor(const char*);

    virtual std::string defaultProductName() const = 0;
    virtual void applyProductName(const char*) = 0;

    ParameterGrp::handle pGroup;
};

}  // namespace Part
