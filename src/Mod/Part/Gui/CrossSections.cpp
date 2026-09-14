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

#include <memory>

#include <QFuture>
#include <QMessageBox>

#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <App/Document.h>
#include <Base/Interpreter.h>
#include <Base/Sequencer.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/MainWindow.h>
#include <Mod/Part/App/CrossSection.h>
#include <Mod/Part/App/PartFeature.h>

#include "CrossSections.h"
#include "ui_CrossSections.h"


using namespace PartGui;
#undef CS_FUTURE  // multi-threading causes some problems

CrossSections::CrossSections(const Base::BoundBox3d& bb, QWidget* parent, Qt::WindowFlags fl)
    : CrossSectionsBase(bb, parent, fl)
    , ui(std::make_unique<Ui_CrossSections>())
{
    ui->setupUi(this);
    initCommon(
        ui->position,
        ui->distance,
        ui->countSections,
        ui->checkBothSides,
        ui->sectionsBox,
        ui->xyPlane,
        ui->xzPlane,
        ui->yzPlane
    );
}

CrossSections::~CrossSections() = default;

void CrossSections::retranslateUi()
{
    ui->retranslateUi(this);
}

bool CrossSections::apply()
{
    std::vector<App::DocumentObject*> docobjs = Gui::Selection().getObjectsOfType(
        App::DocumentObject::getClassTypeId()
    );
    std::vector<App::DocumentObject*> obj;
    for (auto it : docobjs) {
        if (!Part::Feature::getTopoShape(it, Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform)
                 .isNull()) {
            obj.push_back(it);
        }
    }

    std::vector<double> d = getSectionDistances();
    double a = 0, b = 0, c = 0;
    getPlaneNormal(a, b, c);

#ifdef CS_FUTURE
    Standard::SetReentrant(Standard_True);
    for (std::vector<App::DocumentObject*>::iterator it = obj.begin(); it != obj.end(); ++it) {
        Part::CrossSection cs(a, b, c, static_cast<Part::Feature*>(*it)->Shape.getValue());
        QFuture<std::list<TopoDS_Wire>> future
            = QtConcurrent::mapped(d, std::bind(&Part::CrossSection::section, &cs, sp::_1));
        future.waitForFinished();
        QFuture<std::list<TopoDS_Wire>>::const_iterator ft;
        TopoDS_Compound comp;
        BRep_Builder builder;
        builder.MakeCompound(comp);

        for (ft = future.begin(); ft != future.end(); ++ft) {
            const std::list<TopoDS_Wire>& w = *ft;
            for (std::list<TopoDS_Wire>::const_iterator wt = w.begin(); wt != w.end(); ++wt) {
                if (!wt->IsNull()) {
                    builder.Add(comp, *wt);
                }
            }
        }

        App::Document* doc = (*it)->getDocument();
        std::string s = (*it)->getNameInDocument();
        s += "_cs";
        auto* section = doc->addObject<Part::Feature>(s.c_str());
        section->Shape.setValue(comp);
        section->purgeTouched();
    }
#else
    Base::SequencerLauncher seq("Cross-sections…", obj.size() * (d.size() + 1));
    try {
        Gui::Command::runCommand(Gui::Command::App, "import Part\n");
        Gui::Command::runCommand(Gui::Command::App, "from FreeCAD import Base\n");
        for (auto it : obj) {
            App::Document* doc = it->getDocument();
            std::string s = it->getNameInDocument();
            s += "_cs";
            Gui::Command::runCommand(
                Gui::Command::App,
                QStringLiteral(
                    "wires=list()\n"
                    "shape=FreeCAD.getDocument(\"%1\").%2.Shape\n"
                )
                    .arg(QLatin1String(doc->getName()), QLatin1String(it->getNameInDocument()))
                    .toLatin1()
            );

            for (double jt : d) {
                Gui::Command::runCommand(
                    Gui::Command::App,
                    QStringLiteral(
                        "for i in shape.slice(Base.Vector(%1,%2,%3),%4):\n"
                        "    wires.append(i)\n"
                    )
                        .arg(a)
                        .arg(b)
                        .arg(c)
                        .arg(jt)
                        .toLatin1()
                );
                seq.next();
            }

            Gui::Command::runCommand(
                Gui::Command::App,
                QStringLiteral(
                    "comp=Part.makeCompound(wires)\n"
                    "slice=FreeCAD.getDocument(\"%1\").addObject(\"Part::Feature\",\"%2\")\n"
                    "slice.Shape=comp\n"
                    "slice.purgeTouched()\n"
                    "del slice,comp,wires,shape"
                )
                    .arg(QLatin1String(doc->getName()), QLatin1String(s.c_str()))
                    .toLatin1()
            );
        }
        seq.next();
    }
    catch (Base::Exception& e) {
        e.reportException();
        QMessageBox::critical(
            Gui::getMainWindow(),
            tr("Cannot compute cross-sections"),
            QString::fromStdString(e.getMessage())
        );
        return false;
    }

    seq.next();
#endif

    return true;
}

// ---------------------------------------

TaskCrossSections::TaskCrossSections(const Base::BoundBox3d& bb)
    : CrossSectionsTaskDialog(
          new CrossSections(bb),
          Gui::BitmapFactory().pixmap("Part_CrossSections")
      )
{}

#include "moc_CrossSections.cpp"
