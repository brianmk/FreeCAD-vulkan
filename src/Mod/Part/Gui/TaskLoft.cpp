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


#include <Mod/Part/App/ShapeAnalysis_FreeBoundsFix.h>
#include <Precision.hxx>
#include <QMessageBox>
#include <QTextStream>
#include <QTreeWidget>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_HSequenceOfShape.hxx>


#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/Selection/Selection.h>
#include <Gui/ViewProvider.h>

#include <Mod/Part/App/PartFeature.h>

#include "TaskLoft.h"
#include "ShapeSelection.h"
#include "ui_TaskLoft.h"


using namespace PartGui;

class LoftWidget::Private
{
public:
    Ui_TaskLoft ui;
    std::string document;
    Private() = default;
    ~Private() = default;
};

/* TRANSLATOR PartGui::LoftWidget */

LoftWidget::LoftWidget(QWidget* parent)
    : d(new Private())
{
    Q_UNUSED(parent);
    Gui::Command::runCommand(Gui::Command::App, "from FreeCAD import Base");
    Gui::Command::runCommand(Gui::Command::App, "import Part");

    d->ui.setupUi(this);
    d->ui.selector->setAvailableLabel(tr("Available profiles"));
    d->ui.selector->setSelectedLabel(tr("Selected profiles"));

    // clang-format off
    connect(d->ui.selector->availableTreeWidget(), &QTreeWidget::currentItemChanged,
            this, &LoftWidget::onCurrentItemChanged);
    connect(d->ui.selector->selectedTreeWidget(), &QTreeWidget::currentItemChanged,
            this, &LoftWidget::onCurrentItemChanged);
    // clang-format on

    findShapes();
}

LoftWidget::~LoftWidget()
{
    delete d;
}

void LoftWidget::findShapes()
{
    d->document = fillShapeSelector(d->ui.selector->availableTreeWidget());
}

bool LoftWidget::accept()
{
    QString list, solid, ruled, closed;
    if (d->ui.checkSolid->isChecked()) {
        solid = QStringLiteral("True");
    }
    else {
        solid = QStringLiteral("False");
    }

    if (d->ui.checkRuledSurface->isChecked()) {
        ruled = QStringLiteral("True");
    }
    else {
        ruled = QStringLiteral("False");
    }

    if (d->ui.checkClosed->isChecked()) {
        closed = QStringLiteral("True");
    }
    else {
        closed = QStringLiteral("False");
    }

    QTextStream str(&list);

    int count = d->ui.selector->selectedTreeWidget()->topLevelItemCount();
    if (count < 2) {
        QMessageBox::critical(
            this,
            tr("Too Few Elements"),
            tr("At least 2 vertices, edges, wires, or faces are required.")
        );
        return false;
    }
    for (int i = 0; i < count; i++) {
        QTreeWidgetItem* child = d->ui.selector->selectedTreeWidget()->topLevelItem(i);
        QString name = child->data(0, Qt::UserRole).toString();
        str << "App.getDocument('" << d->document.c_str() << "')." << name << ", ";
    }

    QString cmd;
    cmd = QStringLiteral(
              "App.getDocument('%5').addObject('Part::Loft','Loft')\n"
              "App.getDocument('%5').ActiveObject.Sections=[%1]\n"
              "App.getDocument('%5').ActiveObject.Solid=%2\n"
              "App.getDocument('%5').ActiveObject.Ruled=%3\n"
              "App.getDocument('%5').ActiveObject.Closed=%4\n"
    )
              .arg(list, solid, ruled, closed, d->document.c_str());

    return executeShapeCommand(this, d->document, cmd, QT_TRANSLATE_NOOP("Command", "Loft"));
}

bool LoftWidget::reject()
{
    return true;
}

void LoftWidget::onCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
    applyShapeTreeSelection(d->document, current, previous);
}

void LoftWidget::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange) {
        d->ui.retranslateUi(this);
        d->ui.selector->setAvailableLabel(tr("Vertex/Edge/Wire/Face"));
        d->ui.selector->setSelectedLabel(tr("Loft"));
    }
}


/* TRANSLATOR PartGui::TaskLoft */

TaskLoft::TaskLoft()
{
    widget = new LoftWidget();
    addTaskBox(Gui::BitmapFactory().pixmap("Part_Loft"), widget);
}

TaskLoft::~TaskLoft() = default;

void TaskLoft::open()
{}

void TaskLoft::clicked(int)
{}

bool TaskLoft::accept()
{
    return widget->accept();
}

bool TaskLoft::reject()
{
    return widget->reject();
}

#include "moc_TaskLoft.cpp"
