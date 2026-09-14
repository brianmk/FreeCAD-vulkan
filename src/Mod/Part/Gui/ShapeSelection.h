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

#pragma once

#include <string>

#include <QCoreApplication>
#include <QMessageBox>
#include <QString>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Document.h>

class QTreeWidget;
class QTreeWidgetItem;

namespace PartGui
{

/// Fills the available-shapes tree of an ActionSelector with all selectable
/// shapes of the active document. Returns the document name, or an empty
/// string if there is no active document or its GUI document.
std::string fillShapeSelector(QTreeWidget* available);

/// Mirrors the selection of a shape tree item into the 3D view selection.
void applyShapeTreeSelection(
    const std::string& document,
    QTreeWidgetItem* current,
    QTreeWidgetItem* previous
);

/// Runs a Part feature creation command inside a transaction and recomputes the
/// document. On failure the transaction is aborted and an error dialog shown.
template<class W>
bool executeShapeCommand(
    W* parent,
    const std::string& document,
    const QString& cmd,
    const char* commandName
)
{
    try {
        Gui::Document* doc = Gui::Application::Instance->getDocument(document.c_str());
        if (!doc) {
            throw Base::RuntimeError("Document doesn't exist anymore");
        }
        doc->openCommand(commandName);
        Gui::Command::runCommand(Gui::Command::App, cmd.toUtf8());
        doc->getDocument()->recompute();
        App::DocumentObject* obj = doc->getDocument()->getActiveObject();
        if (obj && !obj->isValid()) {
            std::string msg = obj->getStatusString();
            doc->abortCommand();
            throw Base::RuntimeError(msg);
        }
        doc->commitCommand();
    }
    catch (const Base::Exception& e) {
        QMessageBox::warning(
            parent,
            W::tr("Input error"),
            QCoreApplication::translate("Exception", e.what())
        );
        return false;
    }

    return true;
}

}  // namespace PartGui
