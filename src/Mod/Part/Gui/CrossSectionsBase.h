// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2010 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <vector>

#include <QDialog>
#include <QPointer>

#include <Base/BoundBox.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Mod/Part/PartGlobal.h>

class QCheckBox;
class QEvent;
class QGroupBox;
class QKeyEvent;
class QPixmap;
class QRadioButton;
class QSpinBox;

namespace Gui
{
class QuantitySpinBox;
class View3DInventor;
}  // namespace Gui

namespace PartGui
{

class CrossSectionsViewProvider;

/** Shared base class of the Part and MeshPart cross-sections dialogs.
 *
 * It implements the plane selection, the live preview of the section planes
 * and the handling of single or multiple sections. The derived classes only
 * provide the module specific widgets and the actual section computation.
 */
class PartGuiExport CrossSectionsBase: public QDialog
{
public:
    enum Plane
    {
        XY,
        XZ,
        YZ
    };

    explicit CrossSectionsBase(
        const Base::BoundBox3d& bb,
        QWidget* parent = nullptr,
        Qt::WindowFlags fl = Qt::WindowFlags()
    );
    ~CrossSectionsBase() override;

    void accept() override;
    virtual bool apply() = 0;

protected:
    void initCommon(
        Gui::QuantitySpinBox* position,
        Gui::QuantitySpinBox* distance,
        QSpinBox* countSections,
        QCheckBox* checkBothSides,
        QGroupBox* sectionsBox,
        QRadioButton* xyPlane,
        QRadioButton* xzPlane,
        QRadioButton* yzPlane
    );

    void changeEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent*) override;

    virtual void retranslateUi() = 0;

    Plane plane() const;
    std::vector<double> getPlanes() const;
    std::vector<double> getSectionDistances() const;
    void getPlaneNormal(double& a, double& b, double& c) const;

private:
    void setupConnections();
    void xyPlaneClicked();
    void xzPlaneClicked();
    void yzPlaneClicked();
    void positionValueChanged(double);
    void distanceValueChanged(double);
    void countSectionsValueChanged(int);
    void checkBothSidesToggled(bool);
    void sectionsBoxToggled(bool);

    void calcPlane(Plane, double);
    void calcPlanes(Plane);
    void makePlanes(Plane, const std::vector<double>&, double[4]);

    Gui::QuantitySpinBox* position = nullptr;
    Gui::QuantitySpinBox* distance = nullptr;
    QSpinBox* countSections = nullptr;
    QCheckBox* checkBothSides = nullptr;
    QGroupBox* sectionsBox = nullptr;
    QRadioButton* xyPlane = nullptr;
    QRadioButton* xzPlane = nullptr;
    QRadioButton* yzPlane = nullptr;

    Base::BoundBox3d bbox;
    CrossSectionsViewProvider* vp = nullptr;
    QPointer<Gui::View3DInventor> view;
};

/** Task panel wrapping a cross-sections dialog.
 *
 * The concrete dialog is passed in so the same task handling can be reused by
 * the Part and MeshPart modules.
 */
class PartGuiExport CrossSectionsTaskDialog: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    CrossSectionsTaskDialog(CrossSectionsBase* widget, const QPixmap& icon, bool expandable = true);

    bool accept() override;
    void clicked(int id) override;

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel;
    }

private:
    CrossSectionsBase* widget;
};

}  // namespace PartGui
