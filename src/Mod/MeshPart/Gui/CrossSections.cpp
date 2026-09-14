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

#include <memory>
#include <sstream>

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <QFuture>
#include <QMessageBox>
#include <QtConcurrentMap>

#include <App/Document.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Mod/Mesh/App/Core/Algorithm.h>
#include <Mod/Mesh/App/Core/Grid.h>
#include <Mod/Mesh/App/MeshFeature.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/Tools.h>

#include "CrossSections.h"
#include "ui_CrossSections.h"


using namespace MeshPartGui;
namespace sp = std::placeholders;

namespace MeshPartGui
{
class MeshCrossSection
{
public:
    MeshCrossSection(
        const MeshCore::MeshKernel& mesh,
        const MeshCore::MeshFacetGrid& grid,
        double x,
        double y,
        double z,
        bool connectEdges,
        double eps
    )
        : mesh(mesh)
        , grid(grid)
        , x(x)
        , y(y)
        , z(z)
        , connectEdges(connectEdges)
        , epsilon(eps)
    {}
    std::list<TopoDS_Wire> section(double d)
    {
        Mesh::MeshObject::TPolylines polylines;
        MeshCore::MeshAlgorithm algo(mesh);
        Base::Vector3f p(x * d, y * d, z * d);
        Base::Vector3f n(x, y, z);
        algo.CutWithPlane(p, n, grid, polylines, epsilon, connectEdges);

        std::list<TopoDS_Wire> wires;
        for (const auto& polyline : polylines) {
            BRepBuilderAPI_MakePolygon mkPoly;
            for (auto jt : polyline) {
                mkPoly.Add(Base::convertTo<gp_Pnt>(jt));
            }

            if (mkPoly.IsDone()) {
                wires.push_back(mkPoly.Wire());
            }
        }

        return wires;
    }

private:
    const MeshCore::MeshKernel& mesh;
    const MeshCore::MeshFacetGrid& grid;
    double x, y, z;
    bool connectEdges;
    double epsilon;
};
}  // namespace MeshPartGui

CrossSections::CrossSections(const Base::BoundBox3d& bb, QWidget* parent, Qt::WindowFlags fl)
    : PartGui::CrossSectionsBase(bb, parent, fl)
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
    ui->spinEpsilon->setMinimum(0.0001);
}

CrossSections::~CrossSections() = default;

void CrossSections::retranslateUi()
{
    ui->retranslateUi(this);
}

bool CrossSections::apply()
{
    std::vector<App::DocumentObject*> obj = Gui::Selection().getObjectsOfType(
        Mesh::Feature::getClassTypeId()
    );

    std::vector<double> d = getSectionDistances();
    double a = 0, b = 0, c = 0;
    getPlaneNormal(a, b, c);

    bool connectEdges = ui->checkBoxConnect->isChecked();
    double eps = ui->spinEpsilon->value();

#if 1  // multi-threaded sections
    for (auto it : obj) {
        const Mesh::MeshObject& mesh = static_cast<Mesh::Feature*>(it)->Mesh.getValue();

        MeshCore::MeshKernel kernel(mesh.getKernel());
        kernel.Transform(mesh.getTransform());

        MeshCore::MeshFacetGrid grid(kernel);

        // NOLINTBEGIN
        MeshCrossSection cs(kernel, grid, a, b, c, connectEdges, eps);
        QFuture<std::list<TopoDS_Wire>> future
            = QtConcurrent::mapped(d, std::bind(&MeshCrossSection::section, &cs, sp::_1));
        future.waitForFinished();
        // NOLINTEND

        TopoDS_Compound comp;
        BRep_Builder builder;
        builder.MakeCompound(comp);

        for (const auto& w : future) {
            for (const auto& wt : w) {
                if (!wt.IsNull()) {
                    builder.Add(comp, wt);
                }
            }
        }

        App::Document* doc = it->getDocument();
        std::string s = it->getNameInDocument();
        s += "_cs";
        Part::Feature* section = doc->addObject<Part::Feature>(s.c_str());
        section->Shape.setValue(comp);
        section->purgeTouched();
    }
#else
    try {
        Gui::Command::runCommand(Gui::Command::App, "import Mesh, Part\n");
        Gui::Command::runCommand(Gui::Command::App, "from FreeCAD import Base\n");

        std::stringstream str;
        str << "[";
        for (std::vector<double>::iterator jt = d.begin(); jt != d.end(); ++jt) {
            double d = *jt;
            str << "("
                << "App.Vector(" << a * d << ", " << b * d << ", " << c * d << "), "
                << "App.Vector(" << a << ", " << b << ", " << c << ")"
                << "), ";
        }
        str << "]";

        QString planes = QString::fromStdString(str.str());
        for (std::vector<App::DocumentObject*>::iterator it = obj.begin(); it != obj.end(); ++it) {
            App::Document* doc = (*it)->getDocument();
            std::string s = (*it)->getNameInDocument();
            s += "_cs";
            Gui::Command::runCommand(
                Gui::Command::App,
                QStringLiteral(
                    "points=FreeCAD.getDocument(\"%1\").%2.Mesh.crossSections(%3, %4, %5)\n"
                    "wires=[]\n"
                    "for i in points:\n"
                    "    wires.extend([Part.makePolygon(j) for j in i])\n"
                )
                    .arg(QLatin1String(doc->getName()))
                    .arg(QLatin1String((*it)->getNameInDocument()))
                    .arg(planes)
                    .arg(eps)
                    .arg(connectEdges ? QLatin1String("True") : QLatin1String("False"))
                    .toLatin1()
            );

            Gui::Command::runCommand(
                Gui::Command::App,
                QStringLiteral(
                    "comp=Part.Compound(wires)\n"
                    "slice=FreeCAD.getDocument(\"%1\").addObject(\"Part::Feature\",\"%2\")\n"
                    "slice.Shape=comp\n"
                    "slice.purgeTouched()\n"
                    "del slice,comp,wires,points"
                )
                    .arg(QLatin1String(doc->getName()))
                    .arg(QLatin1String(s.c_str()))
                    .toLatin1()
            );
        }
    }
    catch (const Base::Exception& e) {
        QMessageBox::critical(this, tr("Failure"), QString::fromLatin1(e.what()));
        return false;
    }
#endif

    return true;
}

// ---------------------------------------

TaskCrossSections::TaskCrossSections(const Base::BoundBox3d& bb)
    : CrossSectionsTaskDialog(
          new CrossSections(bb),
          Gui::BitmapFactory().pixmap("Mesh_CrossSections"),
          true
      )
{}

#include "moc_CrossSections.cpp"
