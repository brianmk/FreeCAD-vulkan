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

#include "CrossSectionsBase.h"

#include <limits>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGroupBox>
#include <QKeyEvent>
#include <QPixmap>
#include <QRadioButton>
#include <QSpinBox>

#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoSeparator.h>

#include <Base/UnitsApi.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewProvider.h>


using namespace PartGui;

namespace PartGui
{
class CrossSectionsViewProvider: public Gui::ViewProvider
{
public:
    CrossSectionsViewProvider()
    {
        coords = new SoCoordinate3();
        coords->ref();
        planes = new SoLineSet();
        planes->ref();
        SoBaseColor* color = new SoBaseColor();
        color->rgb.setValue(1.0f, 0.447059f, 0.337255f);
        SoDrawStyle* style = new SoDrawStyle();
        style->lineWidth.setValue(2.0f);
        this->pcRoot->addChild(color);
        this->pcRoot->addChild(style);
        this->pcRoot->addChild(coords);
        this->pcRoot->addChild(planes);
    }
    ~CrossSectionsViewProvider() override
    {
        coords->unref();
        planes->unref();
    }
    void updateData(const App::Property*) override
    {}
    const char* getDefaultDisplayMode() const override
    {
        return "";
    }
    std::vector<std::string> getDisplayModes() const override
    {
        return {};
    }
    void setCoords(const std::vector<Base::Vector3f>& v)
    {
        coords->point.setNum(v.size());
        SbVec3f* p = coords->point.startEditing();
        for (unsigned int i = 0; i < v.size(); i++) {
            const Base::Vector3f& pt = v[i];
            p[i].setValue(pt.x, pt.y, pt.z);
        }
        coords->point.finishEditing();
        unsigned int count = v.size() / 5;
        planes->numVertices.setNum(count);
        int32_t* l = planes->numVertices.startEditing();
        for (unsigned int i = 0; i < count; i++) {
            l[i] = 5;
        }
        planes->numVertices.finishEditing();
    }

private:
    SoCoordinate3* coords;
    SoLineSet* planes;
};
}  // namespace PartGui

CrossSectionsBase::CrossSectionsBase(const Base::BoundBox3d& bb, QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , bbox(bb)
{}

CrossSectionsBase::~CrossSectionsBase()
{
    if (view) {
        view->getViewer()->removeViewProvider(vp);
    }
    delete vp;
}

void CrossSectionsBase::initCommon(
    Gui::QuantitySpinBox* position,
    Gui::QuantitySpinBox* distance,
    QSpinBox* countSections,
    QCheckBox* checkBothSides,
    QGroupBox* sectionsBox,
    QRadioButton* xyPlane,
    QRadioButton* xzPlane,
    QRadioButton* yzPlane
)
{
    this->position = position;
    this->distance = distance;
    this->countSections = countSections;
    this->checkBothSides = checkBothSides;
    this->sectionsBox = sectionsBox;
    this->xyPlane = xyPlane;
    this->xzPlane = xzPlane;
    this->yzPlane = yzPlane;

    setupConnections();

    constexpr double max = std::numeric_limits<double>::max();
    position->setRange(-max, max);
    position->setUnit(Base::Unit::Length);
    distance->setRange(0, max);
    distance->setUnit(Base::Unit::Length);
    vp = new CrossSectionsViewProvider();

    Base::Vector3d c = bbox.GetCenter();
    calcPlane(CrossSectionsBase::XY, c.z);
    position->setValue(c.z);

    Gui::Document* doc = Gui::Application::Instance->activeDocument();
    view = qobject_cast<Gui::View3DInventor*>(doc->getActiveView());
    if (view) {
        view->getViewer()->addViewProvider(vp);
    }
}

void CrossSectionsBase::setupConnections()
{
    connect(xyPlane, &QRadioButton::clicked, this, &CrossSectionsBase::xyPlaneClicked);
    connect(xzPlane, &QRadioButton::clicked, this, &CrossSectionsBase::xzPlaneClicked);
    connect(yzPlane, &QRadioButton::clicked, this, &CrossSectionsBase::yzPlaneClicked);
    connect(
        position,
        qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this,
        &CrossSectionsBase::positionValueChanged
    );
    connect(
        distance,
        qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        this,
        &CrossSectionsBase::distanceValueChanged
    );
    connect(
        countSections,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        &CrossSectionsBase::countSectionsValueChanged
    );
    connect(checkBothSides, &QCheckBox::toggled, this, &CrossSectionsBase::checkBothSidesToggled);
    connect(sectionsBox, &QGroupBox::toggled, this, &CrossSectionsBase::sectionsBoxToggled);
}

CrossSectionsBase::Plane CrossSectionsBase::plane() const
{
    if (xyPlane->isChecked()) {
        return CrossSectionsBase::XY;
    }
    else if (xzPlane->isChecked()) {
        return CrossSectionsBase::XZ;
    }
    else {
        return CrossSectionsBase::YZ;
    }
}

void CrossSectionsBase::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    else {
        QDialog::changeEvent(e);
    }
}

void CrossSectionsBase::keyPressEvent(QKeyEvent* ke)
{
    // The cross-sections dialog is embedded into a task panel
    // which is a parent widget and will handle the event
    ke->ignore();
}

void CrossSectionsBase::accept()
{
    if (apply()) {
        QDialog::accept();
    }
}

void CrossSectionsBase::xyPlaneClicked()
{
    Base::Vector3d c = bbox.GetCenter();
    position->setValue(c.z);
    if (!sectionsBox->isChecked()) {
        calcPlane(CrossSectionsBase::XY, c.z);
    }
    else {
        double dist = bbox.LengthZ() / countSections->value();
        if (!checkBothSides->isChecked()) {
            dist *= 0.5f;
        }
        distance->setValue(dist);
        calcPlanes(CrossSectionsBase::XY);
    }
}

void CrossSectionsBase::xzPlaneClicked()
{
    Base::Vector3d c = bbox.GetCenter();
    position->setValue(c.y);
    if (!sectionsBox->isChecked()) {
        calcPlane(CrossSectionsBase::XZ, c.y);
    }
    else {
        double dist = bbox.LengthY() / countSections->value();
        if (!checkBothSides->isChecked()) {
            dist *= 0.5f;
        }
        distance->setValue(dist);
        calcPlanes(CrossSectionsBase::XZ);
    }
}

void CrossSectionsBase::yzPlaneClicked()
{
    Base::Vector3d c = bbox.GetCenter();
    position->setValue(c.x);
    if (!sectionsBox->isChecked()) {
        calcPlane(CrossSectionsBase::YZ, c.x);
    }
    else {
        double dist = bbox.LengthX() / countSections->value();
        if (!checkBothSides->isChecked()) {
            dist *= 0.5f;
        }
        distance->setValue(dist);
        calcPlanes(CrossSectionsBase::YZ);
    }
}

void CrossSectionsBase::positionValueChanged(double v)
{
    if (!sectionsBox->isChecked()) {
        calcPlane(plane(), v);
    }
    else {
        calcPlanes(plane());
    }
}

void CrossSectionsBase::sectionsBoxToggled(bool b)
{
    if (b) {
        countSectionsValueChanged(countSections->value());
    }
    else {
        CrossSectionsBase::Plane type = plane();
        Base::Vector3d c = bbox.GetCenter();
        double value = 0;
        switch (type) {
            case CrossSectionsBase::XY:
                value = c.z;
                break;
            case CrossSectionsBase::XZ:
                value = c.y;
                break;
            case CrossSectionsBase::YZ:
                value = c.x;
                break;
        }

        position->setValue(value);
        calcPlane(type, value);
    }
}

void CrossSectionsBase::checkBothSidesToggled(bool b)
{
    double d = distance->value().getValue();
    d = b ? 2.0 * d : 0.5 * d;
    distance->setValue(d);
    calcPlanes(plane());
}

void CrossSectionsBase::countSectionsValueChanged(int v)
{
    CrossSectionsBase::Plane type = plane();
    double dist = 0;
    switch (type) {
        case CrossSectionsBase::XY:
            dist = bbox.LengthZ() / v;
            break;
        case CrossSectionsBase::XZ:
            dist = bbox.LengthY() / v;
            break;
        case CrossSectionsBase::YZ:
            dist = bbox.LengthX() / v;
            break;
    }
    if (!checkBothSides->isChecked()) {
        dist *= 0.5f;
    }
    distance->setValue(dist);
    calcPlanes(type);
}

void CrossSectionsBase::distanceValueChanged(double)
{
    calcPlanes(plane());
}

void CrossSectionsBase::calcPlane(Plane type, double pos)
{
    double bound[4];
    switch (type) {
        case XY:
            bound[0] = bbox.MinX;
            bound[1] = bbox.MaxX;
            bound[2] = bbox.MinY;
            bound[3] = bbox.MaxY;
            break;
        case XZ:
            bound[0] = bbox.MinX;
            bound[1] = bbox.MaxX;
            bound[2] = bbox.MinZ;
            bound[3] = bbox.MaxZ;
            break;
        case YZ:
            bound[0] = bbox.MinY;
            bound[1] = bbox.MaxY;
            bound[2] = bbox.MinZ;
            bound[3] = bbox.MaxZ;
            break;
    }

    std::vector<double> d;
    d.push_back(pos);
    makePlanes(type, d, bound);
}

void CrossSectionsBase::calcPlanes(Plane type)
{
    double bound[4];
    switch (type) {
        case XY:
            bound[0] = bbox.MinX;
            bound[1] = bbox.MaxX;
            bound[2] = bbox.MinY;
            bound[3] = bbox.MaxY;
            break;
        case XZ:
            bound[0] = bbox.MinX;
            bound[1] = bbox.MaxX;
            bound[2] = bbox.MinZ;
            bound[3] = bbox.MaxZ;
            break;
        case YZ:
            bound[0] = bbox.MinY;
            bound[1] = bbox.MaxY;
            bound[2] = bbox.MinZ;
            bound[3] = bbox.MaxZ;
            break;
    }

    std::vector<double> d = getPlanes();
    makePlanes(type, d, bound);
}

std::vector<double> CrossSectionsBase::getPlanes() const
{
    int count = countSections->value();
    double pos = position->value().getValue();
    double stp = distance->value().getValue();
    bool both = checkBothSides->isChecked();

    std::vector<double> d;
    if (both) {
        double start = pos - 0.5f * (count - 1) * stp;
        for (int i = 0; i < count; i++) {
            d.push_back(start + i * stp);
        }
    }
    else {
        for (int i = 0; i < count; i++) {
            d.push_back(pos + i * stp);
        }
    }
    return d;
}

std::vector<double> CrossSectionsBase::getSectionDistances() const
{
    std::vector<double> d;
    if (sectionsBox->isChecked()) {
        d = getPlanes();
    }
    else {
        d.push_back(position->value().getValue());
    }
    return d;
}

void CrossSectionsBase::getPlaneNormal(double& a, double& b, double& c) const
{
    a = b = c = 0.0;
    switch (plane()) {
        case CrossSectionsBase::XY:
            c = 1.0;
            break;
        case CrossSectionsBase::XZ:
            b = 1.0;
            break;
        case CrossSectionsBase::YZ:
            a = 1.0;
            break;
    }
}

void CrossSectionsBase::makePlanes(Plane type, const std::vector<double>& d, double bound[4])
{
    std::vector<Base::Vector3f> points;
    for (double it : d) {
        Base::Vector3f v[4];
        switch (type) {
            case XY:
                v[0].Set(bound[0], bound[2], it);
                v[1].Set(bound[1], bound[2], it);
                v[2].Set(bound[1], bound[3], it);
                v[3].Set(bound[0], bound[3], it);
                break;
            case XZ:
                v[0].Set(bound[0], it, bound[2]);
                v[1].Set(bound[1], it, bound[2]);
                v[2].Set(bound[1], it, bound[3]);
                v[3].Set(bound[0], it, bound[3]);
                break;
            case YZ:
                v[0].Set(it, bound[0], bound[2]);
                v[1].Set(it, bound[1], bound[2]);
                v[2].Set(it, bound[1], bound[3]);
                v[3].Set(it, bound[0], bound[3]);
                break;
        }

        points.push_back(v[0]);
        points.push_back(v[1]);
        points.push_back(v[2]);
        points.push_back(v[3]);
        points.push_back(v[0]);
    }
    vp->setCoords(points);
}

CrossSectionsTaskDialog::CrossSectionsTaskDialog(
    CrossSectionsBase* widget,
    const QPixmap& icon,
    bool expandable
)
    : widget(widget)
{
    addTaskBox(icon, widget, expandable);
}

bool CrossSectionsTaskDialog::accept()
{
    widget->accept();
    return (widget->result() == QDialog::Accepted);
}

void CrossSectionsTaskDialog::clicked(int id)
{
    if (id == QDialogButtonBox::Apply) {
        widget->apply();
    }
}
