// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2011 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include "ShapeSelection.h"

#include <Precision.hxx>
#include <QTreeWidget>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_HSequenceOfShape.hxx>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/Selection/Selection.h>
#include <Gui/ViewProvider.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/ShapeAnalysis_FreeBoundsFix.h>


using namespace PartGui;

std::string PartGui::fillShapeSelector(QTreeWidget* available)
{
    App::Document* activeDoc = App::GetApplication().getActiveDocument();
    if (!activeDoc) {
        return {};
    }
    Gui::Document* activeGui = Gui::Application::Instance->getDocument(activeDoc);
    if (!activeGui) {
        return {};
    }
    std::string document = activeDoc->getName();

    std::vector<App::DocumentObject*> objs = activeDoc->getObjectsOfType<App::DocumentObject>();

    for (auto obj : objs) {
        Part::TopoShape topoShape = Part::Feature::getTopoShape(
            obj,
            Part::ShapeOption::ResolveLink | Part::ShapeOption::Transform
        );
        if (topoShape.isNull()) {
            continue;
        }
        TopoDS_Shape shape = topoShape.getShape();
        if (shape.IsNull()) {
            continue;
        }

        // also allow compounds with a single face, wire or vertex or
        // if there are only edges building one wire
        if (shape.ShapeType() == TopAbs_COMPOUND) {
            Handle(TopTools_HSequenceOfShape) hEdges = new TopTools_HSequenceOfShape();
            Handle(TopTools_HSequenceOfShape) hWires = new TopTools_HSequenceOfShape();

            TopoDS_Iterator it(shape);
            int numChilds = 0;
            TopoDS_Shape child;
            for (; it.More(); it.Next(), numChilds++) {
                if (!it.Value().IsNull()) {
                    child = it.Value();
                    if (child.ShapeType() == TopAbs_EDGE) {
                        hEdges->Append(child);
                    }
                }
            }

            // a single child
            if (numChilds == 1) {
                shape = child;
            }
            // or all children are edges
            else if (hEdges->Length() == numChilds) {
                Part::Fix_ShapeAnalysis_FreeBounds_ConnectEdgesToWires(
                    hEdges,
                    Precision::Confusion(),
                    Standard_False,
                    hWires
                );
                if (hWires->Length() == 1) {
                    shape = hWires->Value(1);
                }
            }
        }

        if (!shape.Infinite()
            && (shape.ShapeType() == TopAbs_FACE || shape.ShapeType() == TopAbs_WIRE
                || shape.ShapeType() == TopAbs_EDGE || shape.ShapeType() == TopAbs_VERTEX)) {
            QString label = QString::fromUtf8(obj->Label.getValue());
            QString name = QString::fromLatin1(obj->getNameInDocument());
            QTreeWidgetItem* child = new QTreeWidgetItem();
            child->setText(0, label);
            child->setToolTip(0, label);
            child->setData(0, Qt::UserRole, name);
            Gui::ViewProvider* vp = activeGui->getViewProvider(obj);
            if (vp) {
                child->setIcon(0, vp->getIcon());
            }
            available->addTopLevelItem(child);
        }
    }

    return document;
}

void PartGui::applyShapeTreeSelection(
    const std::string& document,
    QTreeWidgetItem* current,
    QTreeWidgetItem* previous
)
{
    if (previous) {
        Gui::Selection().rmvSelection(
            document.c_str(),
            (const char*)previous->data(0, Qt::UserRole).toByteArray()
        );
    }
    if (current) {
        Gui::Selection().addSelection(
            document.c_str(),
            (const char*)current->data(0, Qt::UserRole).toByteArray()
        );
    }
}
