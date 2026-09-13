// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2019 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <memory>

#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Part/Gui/CrossSectionsBase.h>

namespace MeshPartGui
{

class Ui_CrossSections;

class CrossSections: public PartGui::CrossSectionsBase
{
    Q_OBJECT

public:
    explicit CrossSections(
        const Base::BoundBox3d& bb,
        QWidget* parent = nullptr,
        Qt::WindowFlags fl = Qt::WindowFlags()
    );

    ~CrossSections() override;

    bool apply() override;

protected:
    void retranslateUi() override;

private:
    std::unique_ptr<Ui_CrossSections> ui;
};

class TaskCrossSections: public PartGui::CrossSectionsTaskDialog
{
    Q_OBJECT

public:
    explicit TaskCrossSections(const Base::BoundBox3d& bb);
};

}  // namespace MeshPartGui
