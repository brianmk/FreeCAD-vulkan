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

#include "ImportExportSettingsBase.h"

#include <App/Application.h>


using namespace Part;

ImportExportSettingsBase::ImportExportSettingsBase(const char* groupPath)
{
    pGroup = App::GetApplication().GetParameterGroupByPath(groupPath);
}

Interface::Unit ImportExportSettingsBase::getUnit() const
{
    return static_cast<Interface::Unit>(pGroup->GetInt("Unit", 0));
}

void ImportExportSettingsBase::setUnit(Interface::Unit unit)
{
    pGroup->SetInt("Unit", static_cast<long>(unit));
    applyUnit(unit);
}

std::string ImportExportSettingsBase::getCompany() const
{
    return pGroup->GetASCII("Company", defaultCompany().c_str());
}

void ImportExportSettingsBase::setCompany(const char* name)
{
    pGroup->SetASCII("Company", name);
    applyCompany(name);
}

std::string ImportExportSettingsBase::getAuthor() const
{
    return pGroup->GetASCII("Author", defaultAuthor().c_str());
}

void ImportExportSettingsBase::setAuthor(const char* name)
{
    pGroup->SetASCII("Author", name);
    applyAuthor(name);
}

std::string ImportExportSettingsBase::getProductName() const
{
    return defaultProductName();
}

void ImportExportSettingsBase::setProductName(const char* name)
{
    applyProductName(name);
}

std::string ImportExportSettingsBase::defaultCompany() const
{
    return {};
}

void ImportExportSettingsBase::applyCompany(const char*)
{}

std::string ImportExportSettingsBase::defaultAuthor() const
{
    return {};
}

void ImportExportSettingsBase::applyAuthor(const char*)
{}
