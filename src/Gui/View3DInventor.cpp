/***************************************************************************
 *   Copyright (c) 2004 Jürgen Riegel <juergen.riegel@web.de>              *
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


#include <string>
#include <cstring>

#include <Base/VulkanBreadcrumbs.h>
#include <QApplication>
#include <QKeyEvent>
#include <QEvent>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QLayout>
#include <QMdiSubWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPrinter>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QStackedWidget>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <Inventor/actions/SoGetPrimitiveCountAction.h>
#include <Inventor/fields/SoSFString.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/SoPickedPoint.h>


#include <App/Application.h>
#include <App/Document.h>
#include <App/GeoFeature.h>
#include <Base/Builder3D.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>

#include <Gui/PreferencePages/DlgSettingsPDF.h>

#include "View3DInventor.h"
#include "Base/Exception.h"
#include "View3DSettings.h"
#include "Application.h"
#include "BitmapFactory.h"
#include "Camera.h"
#include "Document.h"
#include "FileDialog.h"
#include "MainWindow.h"
#include "NaviCube.h"
#include "Navigation/NavigationStyle.h"
#include "SoFCDB.h"
#include "SoFCSelectionAction.h"
#include "SoFCVectorizeSVGAction.h"
#include "View3DInventorViewer.h"
#include "VulkanViewportAdapter.h"
#include "View3DPy.h"
#include "ViewParams.h"
#include "ViewProvider.h"
#include "ViewProviderDocumentObject.h"
#include "WaitCursor.h"

#include "Utilities.h"

using namespace Gui;

void GLOverlayWidget::paintEvent(QPaintEvent*)
{
    QPainter paint(this);
    paint.drawImage(0, 0, image);
    paint.end();
}

/* TRANSLATOR Gui::View3DInventor */

TYPESYSTEM_SOURCE_ABSTRACT(Gui::View3DInventor, Gui::MDIViewWithCamera)

View3DInventor::View3DInventor(
    Gui::Document* pcDocument,
    QWidget* parent,
    const QOpenGLWidget* sharewidget,
    Qt::WindowFlags wflags
)
    : MDIViewWithCamera(pcDocument, parent, wflags)
    , _viewerPy(nullptr)
{
    stack = new QStackedWidget(this);
    // important for highlighting
    setMouseTracking(true);
    // accept drops on the window, get handled in dropEvent, dragEnterEvent
    setAcceptDrops(true);

    // anti-aliasing settings
    bool smoothing = false;
    bool glformat = false;
    int samples = View3DInventorViewer::getNumSamples();
    QSurfaceFormat f;

    if (samples > 1) {
        glformat = true;
        f.setSamples(samples);
    }
    else if (samples > 0) {
        smoothing = true;
    }

    if (glformat) {
        _viewer = new View3DInventorViewer(f, this, sharewidget);
    }
    else {
        _viewer = new View3DInventorViewer(this, sharewidget);
    }

    if (smoothing) {
        _viewer->getSoRenderManager()->getGLRenderAction()->setSmoothing(true);
    }

    // create the inventor widget and set the defaults
    _viewer->setDocument(this->_pcDocument);
    stack->addWidget(_viewer->getWidget());
    // https://forum.freecad.org/viewtopic.php?f=3&t=6055&sid=150ed90cbefba50f1e2ad4b4e6684eba
    // describes a minor error but trying to fix it leads to a major issue
    // https://forum.freecad.org/viewtopic.php?f=3&t=6085&sid=3f4bcab8007b96aaf31928b564190fd7
    // so the change is commented out
    // By default, the wheel events are processed by the 3d view AND the mdi area.
    //_viewer->getGLWidget()->setAttribute(Qt::WA_NoMousePropagation);
    setCentralWidget(stack);

    // apply the user settings
    applySettings();

#ifdef FREECAD_USE_VULKAN
    VK_BREADCRUMB("[VK-TRACE] View3DInventor: UseVulkanRenderer=%d\n",
                  App::GetApplication()
                          .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
                          ->GetBool("UseVulkanRenderer", false) ? 1 : 0);
    if (_viewer && App::GetApplication()
                           .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
                           ->GetBool("UseVulkanRenderer", false)) {
        const bool useRayTracing =
            Base::envFlagTruthy("FC_VULKAN_RAYTRACING") ||
            App::GetApplication()
                    .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
                    ->GetBool("UseVulkanRayTracing", false);
        VK_BREADCRUMB("[VK-TRACE] View3DInventor: UseVulkanRayTracing=%d\n",
                      useRayTracing ? 1 : 0);
        // The adapter owns the Vulkan widget and all GL<->Vulkan sync
        // wiring (scene/camera state, input forwarding, cursor mirroring,
        // viewport sizing).
        _vulkanAdapter = new VulkanViewportAdapter(stack, _viewer, useRayTracing, this);
        // Reopen consistency: if the persisted VulkanPathTracing pref enabled
        // the path tracer, the view should come back in the Ray Tracing mode
        // (matching setPathTracingEnabled persisting that flag).  The status-
        // bar selector then reflects whatever the user last chose.
        if (App::GetApplication()
                .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
                ->GetBool("VulkanPathTracing", false)) {
            _renderMode = ViewRenderMode::RayTracing;
        }
        // Reopen consistency for the environment/cubemap preset: restore the
        // persisted choice (-1 = viewport background gradient) and push it to
        // the adapter so the sky matches what the user last selected.
        const int persistedEnvMap = App::GetApplication()
            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
            ->GetInt("VulkanEnvironmentMap", -1);
        _envMap = persistedEnvMap;
        if (_envMap >= 0 && _vulkanAdapter) {
            _vulkanAdapter->setEnvMap(_envMap);
        }
        // A selection/preselection change mutates the shared scene graph but
        // never reaches the display-only Vulkan widget on its own (it owns no
        // Coin sensors).  Ask the adapter to redraw so the highlight shows up
        // even after the path tracer has converged and gone idle.
        connect(_viewer, &View3DInventorViewer::selectionChanged,
                _vulkanAdapter, [this] {
                    if (_vulkanAdapter) {
                        _vulkanAdapter->redraw();
                    }
                });
    }
#endif

    stopSpinTimer = new QTimer(this);
    connect(stopSpinTimer, &QTimer::timeout, this, &View3DInventor::stopAnimating);

    setWindowIcon(Gui::BitmapFactory().pixmap("Document"));
}

View3DInventor::~View3DInventor()
{
    if (_pcDocument) {
        SoCamera* Cam = _viewer->getSoRenderManager()->getCamera();
        if (Cam) {
            _pcDocument->saveCameraSettings(SoFCDB::writeNodesToString(Cam).c_str());
        }
    }

    viewSettings.reset();

    // If we destroy this viewer by calling 'delete' directly the focus proxy widget which is
    // defined by a widget in SoQtViewer isn't reset. This widget becomes a dangling pointer and
    // makes the application crash. (Probably it's better to destroy this viewer by calling
    // close().) See also Gui::Document::~Document().
    QWidget* foc = qApp->focusWidget();
    if (foc) {
        QWidget* par = foc->parentWidget();
        while (par) {
            if (par == this) {
                foc->setFocusProxy(nullptr);
                foc->clearFocus();
                break;
            }
            par = par->parentWidget();
        }
    }

    // Tear down the Vulkan viewport adapter BEFORE the hidden OpenGL viewer.
    // The adapter is a QObject child of this view, so Qt would destroy it
    // only when `this` itself is destroyed -- after _viewer is already gone.
    // Its QuarterVulkanWidget wires signals to _viewer (cameraChanged,
    // vulkanSettingsChanged, surfaceSizeChanged) and to the document's
    // scene graph; leaving it alive past `delete _viewer` lets deferred
    // syncViewer()/pushSettings() re-fire against a freed _viewer and scene,
    // which tears down the same separator twice (SoGroup::removeChild errors)
    // and SIGSEGVs in QVulkanInstance::functions() during the document-tab
    // close (the 'shutdown'/'initialized' backend cycling).
    delete _vulkanAdapter;
    _vulkanAdapter = nullptr;

    if (_viewerPy) {
        Base::PyGILStateLocker lock;
        Py_DECREF(_viewerPy);
    }

    // here is from time to time trouble!!!
    delete _viewer;
}

void View3DInventor::deleteSelf()
{
    _viewer->setSceneGraph(nullptr);
    _viewer->setDocument(nullptr);
    // Drop the Vulkan viewport now, while _viewer and its scene graph are
    // still valid.  The adapter is a QObject child destroyed only when `this`
    // is, which is after _viewer; leaving it connected lets its Vulkan widget
    // re-sync against the freed viewer/scene during this close (the backend
    // shutdown/initialized cycling and the QVulkanInstance::functions() crash).
    delete _vulkanAdapter;
    _vulkanAdapter = nullptr;
    MDIViewWithCamera::deleteSelf();
}

View3DInventor* View3DInventor::clone()
{
    auto mdiView = _pcDocument->createView(getClassTypeId(), CreateViewMode::Clone);
    auto view3D = static_cast<View3DInventor*>(mdiView);

    view3D->cloneFrom(*this);
    view3D->getViewer()->setAxisCross(getViewer()->hasAxisCross());
    view3D->getViewer()->setGroundPlane(getViewer()->hasGroundPlane());

    // FIXME: Add parameter to define behaviour by the calling instance
    // View provider editing

    int editMode;
    ViewProvider* editViewProvider = _pcDocument->getInEdit(nullptr, nullptr, &editMode);
    if (editViewProvider) {
        getViewer()->resetEditingViewProvider();
        view3D->getViewer()->setEditingViewProvider(editViewProvider, editMode);
    }

    return view3D;
}

PyObject* View3DInventor::getPyObject()
{
    if (!_viewerPy) {
        _viewerPy = new View3DInventorPy(this);
    }

    Py_INCREF(_viewerPy);
    return _viewerPy;
}

void View3DInventor::applySettings()
{
    viewSettings = std::make_unique<View3DSettings>(
        App::GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/View"),
        _viewer
    );
    naviSettings = std::make_unique<NaviCubeSettings>(
        App::GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/NaviCube"),
        _viewer
    );
    viewSettings->applySettings();
    naviSettings->applySettings();
}

void View3DInventor::setPathTracingEnabled(bool enabled)
{
    if (_vulkanAdapter) {
        _vulkanAdapter->setPathTracingEnabled(enabled);
    }
    // Persist the choice as the VulkanPathTracing preference so it survives a
    // view reopen (the Vulkan settings are read from it on construction).
    if (auto grp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View")) {
        grp->SetBool("VulkanPathTracing", enabled);
    }
}

void View3DInventor::setPathTracingStart(bool start)
{
    if (_vulkanAdapter) {
        _vulkanAdapter->setPathTracingStart(start);
    }
}

bool View3DInventor::isPathTracingEnabled() const
{
    return _vulkanAdapter && _vulkanAdapter->isPathTracingEnabled();
}

bool View3DInventor::isPathTracingActive() const
{
    return _vulkanAdapter && _vulkanAdapter->isPathTracingActive();
}

uint32_t View3DInventor::getVulkanFrameCount() const
{
    return _vulkanAdapter ? _vulkanAdapter->getRenderFrameCount() : 0;
}

void View3DInventor::requestVulkanRender()
{
    if (_vulkanAdapter) {
        _vulkanAdapter->requestVulkanRender();
    }
}

Gui::ViewRenderMode View3DInventor::getRenderMode() const
{
    return _renderMode;
}

void View3DInventor::setRenderMode(ViewRenderMode mode)
{
    if (_renderMode == mode) {
        return;
    }
    _renderMode = mode;
#ifdef FREECAD_USE_VULKAN
    switch (mode) {
        case ViewRenderMode::Interactive:
        case ViewRenderMode::Wireframe:
            // Raster modes: path tracing is off.  The wireframe view style is
            // set on the GL viewer (the Vulkan widget mirrors the scene graph,
            // and the raster Vulkan backend renders it with the same draw
            // style, so the wireframe override propagates through it).
            if (_vulkanAdapter) {
                _vulkanAdapter->setPathTracingEnabled(false);
            }
            break;
        case ViewRenderMode::AmbientOcclusion:
            // Realtime single-sample ray AO: enable the RT backend (so the
            // ray-query compute tracer renders) but never accumulate.  The
            // view-mode value selects the AO shader path on the backend.
            if (_vulkanAdapter) {
                _vulkanAdapter->setPathTracingEnabled(true);
                _vulkanAdapter->setViewMode(1);  // widget AmbientOcclusion
                _vulkanAdapter->setPathTracingStart(false);
            }
            break;
        case ViewRenderMode::RayTracing:
            // Full accumulating path tracer.
            if (_vulkanAdapter) {
                _vulkanAdapter->setPathTracingEnabled(true);
                _vulkanAdapter->setViewMode(2);  // widget PathTracing
                _vulkanAdapter->setPathTracingStart(true);
            }
            break;
        case ViewRenderMode::Environment:
            // Realtime single-sample procedural IBL preview: enable the RT
            // backend but never accumulate (like AO).  The view-mode value
            // selects the environment shader path on the backend.
            if (_vulkanAdapter) {
                _vulkanAdapter->setPathTracingEnabled(true);
                _vulkanAdapter->setViewMode(3);  // widget Environment
                _vulkanAdapter->setPathTracingStart(false);
            }
            break;
    }
#else
    (void)mode;
#endif
    // Raster draw-style override for the Interactive vs Wireframe modes.
    if (_viewer) {
        switch (mode) {
            case ViewRenderMode::Interactive:
                _viewer->setOverrideMode("As Is");
                break;
            case ViewRenderMode::Wireframe:
                _viewer->setOverrideMode("Wireframe");
                break;
            case ViewRenderMode::AmbientOcclusion:
            case ViewRenderMode::RayTracing:
            case ViewRenderMode::Environment:
                break;  // ray tracer ignores the raster draw style
        }
    }
    if (_vulkanAdapter) {
        _vulkanAdapter->redraw();
    }
}

int View3DInventor::getEnvMap() const
{
    return _envMap;
}

void View3DInventor::setEnvMap(int index)
{
    if (_envMap == index) {
        return;
    }
    _envMap = index;
    if (_vulkanAdapter) {
        _vulkanAdapter->setEnvMap(index);
        _vulkanAdapter->redraw();
    }
    // Persist the choice as the VulkanEnvironmentMap preference so it survives
    // a view reopen (see the reopen-consistency init in the constructor).
    if (auto grp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View")) {
        grp->SetInt("VulkanEnvironmentMap", index);
    }
}

bool View3DInventor::getShowEdges() const
{
    return _viewer && _viewer->getVulkanViewSettings().showEdges;
}

void View3DInventor::setShowEdges(bool enabled)
{
    if (getShowEdges() == enabled) {
        return;
    }
    // The edge overlay is driven by the VulkanShowEdges preference; update the
    // preference then reload the settings so the viewer emits
    // vulkanSettingsChanged(), which the adapter re-pushes to the renderer.
    if (auto grp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View")) {
        grp->SetBool("VulkanShowEdges", enabled);
    }
    if (_viewer) {
        _viewer->applyVulkanSettings();
    }
}

void View3DInventor::onRename(Gui::Document* pDoc)
{
    SoSFString name;
    name.setValue(pDoc->getDocument()->getName());
    SoFCDocumentAction cAct(name);
    cAct.apply(_viewer->getSceneGraph());
}

void View3DInventor::onUpdate()
{
#ifdef FC_LOGUPDATECHAIN
    Base::Console().log("Acti: Gui::View3DInventor::onUpdate()");
#endif
    update();
    _viewer->redraw();
    if (_vulkanAdapter) {
        // The Vulkan viewport is display-only and owns no Coin sensors, so a
        // scene edit never schedules a frame on its own (only selection and
        // API-driven redraws do).  Ask it to redraw so the edited scene is
        // reflected even after the path tracer has converged and gone idle;
        // otherwise keep-alive/BLAS-overlay updates are dropped until the user
        // touches the view.  syncViewer() pushes the (possibly changed) scene
        // graph and camera; the redraw() then requests exactly one boundary
        // frame, which the render loop otherwise suppresses when refining is
        // false (converged-idle).
        _vulkanAdapter->syncViewer();
        _vulkanAdapter->redraw();
    }
}

void View3DInventor::viewAll()
{
    _viewer->viewAll();
}

const char* View3DInventor::getName() const
{
    return "View3DInventor";
}

void View3DInventor::print()
{
    QPrinter printer(QPrinter::ScreenResolution);
    printer.setFullPage(true);
    restorePrinterSettings(&printer);

    QPrintDialog dlg(&printer, this);
    if (dlg.exec() == QDialog::Accepted) {
        Gui::WaitCursor wc;
        print(&printer);
        savePrinterSettings(&printer);
    }
}

void View3DInventor::printPdf()
{
    QString filename = FileDialog::getSaveFileName(
        this,
        tr("Export PDF"),
        QString(),
        FileDialog::FilterList {{QStringLiteral("PDF"), {"*.pdf"}}}
    );
    if (!filename.isEmpty()) {
        Gui::WaitCursor wc;
        QPrinter printer(QPrinter::ScreenResolution);
        // setPdfVersion sets the printed PDF Version to what is chosen in
        // Preferences/Import-Export/PDF more details under:
        // https://www.kdab.com/creating-pdfa-documents-qt/
        printer.setPdfVersion(Gui::Dialog::DlgSettingsPDF::evaluatePDFVersion());
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setPageOrientation(QPageLayout::Landscape);
        printer.setOutputFileName(filename);
        printer.setCreator(QString::fromStdString(App::Application::getNameWithVersion()));
        print(&printer);
    }
}

void View3DInventor::printPreview()
{
    QPrinter printer(QPrinter::ScreenResolution);
    printer.setFullPage(true);
    restorePrinterSettings(&printer);

    QPrintPreviewDialog dlg(&printer, this);
    connect(
        &dlg,
        &QPrintPreviewDialog::paintRequested,
        this,
        qOverload<QPrinter*>(&View3DInventor::print)
    );
    dlg.exec();
    savePrinterSettings(&printer);
}

void View3DInventor::print(QPrinter* printer)
{
    QPainter p(printer);
    p.setRenderHints(QPainter::Antialiasing);
    if (!p.isActive() && !printer->outputFileName().isEmpty()) {
        qApp->setOverrideCursor(Qt::ArrowCursor);
        QMessageBox::critical(
            this,
            tr("Opening file failed"),
            tr("Can't open file '%1' for writing.").arg(printer->outputFileName())
        );
        qApp->restoreOverrideCursor();
        return;
    }

    QRect rect = printer->pageLayout().paintRectPixels(printer->resolution());
    View3DInventorViewer::RenderImageOptions options;
    options.width = rect.width();
    options.height = rect.height();
    options.samples = 8;
    options.background = QColor(255, 255, 255);
    options.intent = View3DInventorViewer::RenderIntent::RasterCapture;
    QImage img = _viewer->renderToImage(options);
    p.drawImage(0, 0, img);
    p.end();
}

bool View3DInventor::containsViewProvider(const ViewProvider* vp) const
{
    return _viewer->containsViewProvider(vp);
}

// **********************************************************************************

bool View3DInventor::onMsg(const char* pMsg)
{
    if (strcmp("ViewFit", pMsg) == 0) {
        _viewer->viewAll();
        return true;
    }
    else if (strcmp("ViewHome", pMsg) == 0) {
        _viewer->viewHome();
        return true;
    }
    else if (strcmp("ViewVR", pMsg) == 0) {
        // call the VR portion of the viewer
        _viewer->viewVR();
        return true;
    }
    else if (strcmp("ViewSelection", pMsg) == 0) {
        _viewer->viewSelection(ViewParams::instance()->getViewSelectionExtend());
        return true;
    }
    else if (strcmp("ViewSelectionExtend", pMsg) == 0) {
        _viewer->viewSelection(true);
        return true;
    }
    else if (strncmp("Dump", pMsg, 4) == 0) {
        dump(pMsg + 5);
        return true;
    }
    else if (strcmp("ViewBottom", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Bottom));
        return true;
    }
    else if (strcmp("ViewFront", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Front));
        return true;
    }
    else if (strcmp("ViewLeft", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Left));
        return true;
    }
    else if (strcmp("ViewRear", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Rear));
        return true;
    }
    else if (strcmp("ViewRight", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Right));
        return true;
    }
    else if (strcmp("ViewTop", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Top));
        return true;
    }
    else if (strcmp("ViewAxo", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Isometric));
        return true;
    }
    else if (strcmp("ViewDimetric", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Dimetric));
        return true;
    }
    else if (strcmp("ViewTrimetric", pMsg) == 0) {
        _viewer->setCameraOrientation(Camera::rotation(Camera::Trimetric));
        return true;
    }
    else if (strcmp("OrthographicCamera", pMsg) == 0) {
        _viewer->setCameraType(SoOrthographicCamera::getClassTypeId());
        if (_vulkanAdapter) {
        _vulkanAdapter->syncViewer();
    }
        return true;
    }
    else if (strcmp("PerspectiveCamera", pMsg) == 0) {
        _viewer->setCameraType(SoPerspectiveCamera::getClassTypeId());
        if (_vulkanAdapter) {
        _vulkanAdapter->syncViewer();
    }
        return true;
    }
    else if (strcmp("Undo", pMsg) == 0) {
        getGuiDocument()->undo(1);
        return true;
    }
    else if (strcmp("Redo", pMsg) == 0) {
        getGuiDocument()->redo(1);
        return true;
    }
    else if (strcmp("Save", pMsg) == 0) {
        getGuiDocument()->save();
        return true;
    }
    else if (strcmp("SaveAs", pMsg) == 0) {
        getGuiDocument()->saveAs();
        return true;
    }
    else if (strcmp("SaveCopy", pMsg) == 0) {
        getGuiDocument()->saveCopy();
        return true;
    }
    else if (strcmp("ZoomIn", pMsg) == 0) {
        _viewer->navigationStyle()->zoomIn();
        return true;
    }
    else if (strcmp("ZoomOut", pMsg) == 0) {
        _viewer->navigationStyle()->zoomOut();
        return true;
    }
    else if (strcmp("StoreWorkingView", pMsg) == 0) {
        _viewer->saveHomePosition();
        return true;
    }
    else if (strcmp("RecallWorkingView", pMsg) == 0) {
        if (_viewer->hasHomePosition()) {
            _viewer->resetToHomePosition();
        }
        return true;
    }

    return false;
}

bool View3DInventor::onHasMsg(const char* pMsg) const
{
    if (strcmp("CanPan", pMsg) == 0) {
        return true;
    }
    else if (strcmp("Save", pMsg) == 0) {
        return true;
    }
    else if (strcmp("SaveAs", pMsg) == 0) {
        return true;
    }
    else if (strcmp("SaveCopy", pMsg) == 0) {
        return true;
    }
    else if (strcmp("Undo", pMsg) == 0) {
        App::Document* doc = getAppDocument();
        return doc && doc->getAvailableUndos() > 0;
    }
    else if (strcmp("Redo", pMsg) == 0) {
        App::Document* doc = getAppDocument();
        return doc && doc->getAvailableRedos() > 0;
    }
    else if (strcmp("Print", pMsg) == 0) {
        return true;
    }
    else if (strcmp("PrintPreview", pMsg) == 0) {
        return true;
    }
    else if (strcmp("PrintPdf", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewFit", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewHome", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewVR", pMsg) == 0) {
#ifdef BUILD_VR
        return true;
#else
        return false;
#endif
    }
    else if (strcmp("ViewSelection", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewBottom", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewFront", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewLeft", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewRear", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewRight", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewTop", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewAxo", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewDimetric", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ViewTrimetric", pMsg) == 0) {
        return true;
    }
    else if (strcmp("OrthographicCamera", pMsg) == 0) {
        return true;
    }
    else if (strcmp("PerspectiveCamera", pMsg) == 0) {
        return true;
    }
    else if (strncmp("Dump", pMsg, 4) == 0) {
        return true;
    }
    else if (strcmp("ZoomIn", pMsg) == 0) {
        return true;
    }
    else if (strcmp("ZoomOut", pMsg) == 0) {
        return true;
    }
    else if (strcmp("AllowsOverlayOnHover", pMsg) == 0) {
        return true;
    }
    else if (strcmp("StoreWorkingView", pMsg) == 0) {
        return true;
    }
    else if (strcmp("RecallWorkingView", pMsg) == 0) {
        return _viewer->hasHomePosition();
    }

    return false;
}

const std::string& View3DInventor::getCamera() const
{
    SoCamera* Cam = _viewer->getSoRenderManager()->getCamera();
    if (!Cam) {
        throw Base::RuntimeError("Could not find reference to 3D View camera");
    }
    return SoFCDB::writeNodesToString(Cam);
}

bool View3DInventor::setCamera(const char* pCamera)
{
    return _viewer->setCamera(pCamera);
}

void View3DInventor::toggleClippingPlane()
{
    _viewer->toggleClippingPlane();
}

bool View3DInventor::hasClippingPlane() const
{
    return _viewer->hasClippingPlane();
}

void View3DInventor::setOverlayWidget(QWidget* widget)
{
    removeOverlayWidget();
    stack->addWidget(widget);
    stack->setCurrentIndex(1);
}

void View3DInventor::removeOverlayWidget()
{
    stack->setCurrentIndex(0);
    QWidget* overlay = stack->widget(1);
    if (overlay) {
        stack->removeWidget(overlay);
    }
}

void View3DInventor::setOverrideCursor(const QCursor& aCursor)
{
    _viewer->getWidget()->setCursor(aCursor);
}

void View3DInventor::restoreOverrideCursor()
{
    _viewer->getWidget()->setCursor(QCursor(Qt::ArrowCursor));
}

// defined in SoFCDB.cpp
extern SoNode* replaceSwitchesInSceneGraph(SoNode*);

void View3DInventor::dump(const char* filename, bool onlyVisible)
{
    SoGetPrimitiveCountAction action;
    action.setCanApproximate(true);
    action.apply(_viewer->getSceneGraph());

    SoNode* node = _viewer->getSceneGraph();
    if (onlyVisible) {
        node = replaceSwitchesInSceneGraph(node);
        node->ref();
    }

    if (action.getTriangleCount() > 100000 || action.getPointCount() > 30000
        || action.getLineCount() > 10000) {
        _viewer->dumpToFile(node, filename, true);
    }
    else {
        _viewer->dumpToFile(node, filename, false);
    }

    if (onlyVisible) {
        node->unref();
    }
}

void View3DInventor::windowStateChanged(QWidget* view)
{
    bool canStartTimer = false;
    if (this != view) {
        // If both views are child widgets of the workspace and view is maximized this view
        // must be hidden, hence we can start the timer.
        // Note: If view is top-level or fullscreen it doesn't necessarily hide the other view
        // e.g. if it is on a second monitor.
        canStartTimer = (!this->isWindow() && !view->isWindow() && view->isMaximized());
    }
    else if (isMinimized()) {
        // I am the active view but minimized
        canStartTimer = true;
    }

    if (canStartTimer) {
        int msecs = viewSettings->stopAnimatingIfDeactivated();
        if (!stopSpinTimer->isActive() && msecs >= 0) {  // if < 0 do not stop rotation
            stopSpinTimer->setSingleShot(true);
            stopSpinTimer->start(msecs);
        }
    }
    else if (stopSpinTimer->isActive()) {
        // If this view may be visible again we can stop the timer
        stopSpinTimer->stop();
    }
}

void View3DInventor::stopAnimating()
{
    _viewer->stopAnimating();
}

/**
 * Drops the event \a e and writes the right Python command.
 */
void View3DInventor::dropEvent(QDropEvent* e)
{
    const QMimeData* data = e->mimeData();
    if (data->hasUrls()) {
        getMainWindow()->loadUrls(getAppDocument(), data->urls());
    }
    else {
        MDIViewWithCamera::dropEvent(e);
    }
}

void View3DInventor::dragEnterEvent(QDragEnterEvent* e)
{
    // Here we must allow uri drags and check them in dropEvent
    const QMimeData* data = e->mimeData();
    if (data->hasUrls()) {
        e->accept();
    }
    else {
        e->ignore();
    }
}

void View3DInventor::setCurrentViewMode(ViewMode mode)
{
    ViewMode oldmode = currentViewMode();
    if (mode == oldmode) {
        return;
    }

    if (mode == Child) {
        // Fix in two steps:
        // The mdi view got a QWindow when it became a top-level widget and when resetting it to a
        // child widget the QWindow must be deleted because it has an impact on resize events and
        // may break the layout of mdi view inside the QMdiSubWindow. In the second step below the
        // layout must be invalidated after it's again a child widget to make sure the mdi view fits
        // into the QMdiSubWindow.
        QWindow* winHandle = this->windowHandle();
        if (winHandle) {
            winHandle->destroy();
        }
    }

    MDIViewWithCamera::setCurrentViewMode(mode);

    // This widget becomes the focus proxy of the embedded GL widget if we leave
    // the 'Child' mode. If we reenter 'Child' mode the focus proxy is reset to 0.
    // If we change from 'TopLevel' mode to 'Fullscreen' mode or vice versa nothing
    // happens.
    // Grabbing keyboard when leaving 'Child' mode (as done in a recent version) should
    // be avoided because when two or more windows are either in 'TopLevel' or 'Fullscreen'
    // mode only the last window gets all key event even if it is not the active one.
    //
    // It is important to set the focus proxy to get all key events otherwise we would lose
    // control after redirecting the first key event to the GL widget.

    if (oldmode == Child) {
        _viewer->getGLWidget()->setFocusProxy(this);
    }
    else if (mode == Child) {
        _viewer->getGLWidget()->setFocusProxy(nullptr);

        // Step two
        auto mdi = qobject_cast<QMdiSubWindow*>(parentWidget());
        if (mdi && mdi->layout()) {
            mdi->layout()->invalidate();
        }
    }
}

RayPickInfo View3DInventor::getObjInfoRay(Base::Vector3d* startvec, Base::Vector3d* dirvec)
{
    double vsx, vsy, vsz;
    double vdx, vdy, vdz;
    vsx = startvec->x;
    vsy = startvec->y;
    vsz = startvec->z;
    vdx = dirvec->x;
    vdy = dirvec->y;
    vdz = dirvec->z;
    // near plane clipping is required to avoid false intersections
    float nearClippingPlane = 0.1F;

    RayPickInfo ret = {false, Base::Vector3d(), "", "", std::nullopt, std::nullopt, std::nullopt};
    SoRayPickAction action(getViewer()->getSoRenderManager()->getViewportRegion());
    action.setRay(SbVec3f(vsx, vsy, vsz), SbVec3f(vdx, vdy, vdz), nearClippingPlane);
    action.apply(getViewer()->getSoRenderManager()->getSceneGraph());
    SoPickedPoint* Point = action.getPickedPoint();

    if (!Point) {
        return ret;
    }

    ret.point = Base::convertTo<Base::Vector3d>(Point->getPoint());
    ViewProvider* vp = getViewer()->getViewProviderByPath(Point->getPath());
    if (vp && vp->isDerivedFrom<ViewProviderDocumentObject>()) {
        if (!vp->isSelectable()) {
            return ret;
        }
        auto vpd = static_cast<ViewProviderDocumentObject*>(vp);
        if (vp->useNewSelectionModel()) {
            std::string subname;
            if (!vp->getElementPicked(Point, subname)) {
                return ret;
            }
            auto obj = vpd->getObject();
            if (!obj) {
                return ret;
            }
            if (!subname.empty()) {
                App::ElementNamePair elementName;
                auto sobj = App::GeoFeature::resolveElement(obj, subname.c_str(), elementName);
                if (!sobj) {
                    return ret;
                }
                if (sobj != obj) {
                    ret.parentObject = obj->getExportName();
                    ret.subName = subname;
                    obj = sobj;
                }
                subname = !elementName.oldName.empty() ? elementName.oldName : elementName.newName;
            }
            ret.document = obj->getDocument()->getName();
            ret.object = obj->getNameInDocument();
            ret.component = subname;
            ret.isValid = true;
        }
        else {
            ret.document = vpd->getObject()->getDocument()->getName();
            ret.object = vpd->getObject()->getNameInDocument();
            // search for a SoFCSelection node
            SoFCDocumentObjectAction objaction;
            objaction.apply(Point->getPath());
            if (objaction.isHandled()) {
                ret.component = objaction.componentName.getString();
            }
        }
        // ok, found the node of interest
        ret.isValid = true;
    }
    else {
        // custom nodes not in a VP: search for a SoFCSelection node
        SoFCDocumentObjectAction objaction;
        objaction.apply(Point->getPath());
        if (objaction.isHandled()) {
            ret.document = objaction.documentName.getString();
            ret.object = objaction.objectName.getString();
            ret.component = objaction.componentName.getString();
            // ok, found the node of interest
            ret.isValid = true;
        }
    }
    return ret;
}

void View3DInventor::keyPressEvent(QKeyEvent* e)
{
    // See StdViewDockUndockFullscreen::activated()
    // With Qt5 one cannot directly use 'setCurrentViewMode'
    // of an MDI view because it causes rendering problems.
    // The only reliable solution is to clone the MDI view,
    // set its view mode and close the original MDI view.

    QMainWindow::keyPressEvent(e);
}

void View3DInventor::keyReleaseEvent(QKeyEvent* e)
{
    QMainWindow::keyReleaseEvent(e);
}

void View3DInventor::focusInEvent(QFocusEvent*)
{
    _viewer->getGLWidget()->setFocus();
}

bool View3DInventor::eventFilter(QObject* watched, QEvent* event)
{
    Q_UNUSED(watched);
    Q_UNUSED(event);
    return MDIView::eventFilter(watched, event);
}

void View3DInventor::contextMenuEvent(QContextMenuEvent* e)
{
    MDIViewWithCamera::contextMenuEvent(e);
}

void View3DInventor::customEvent(QEvent* e)
{
    if (e->type() == QEvent::User) {
        auto se = static_cast<NavigationStyleEvent*>(e);
        ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/View"
        );
        if (hGrp->GetBool("SameStyleForAllViews", true)) {
            hGrp->SetASCII("NavigationStyle", std::string {se->style().getName()}.c_str());
        }
        else {
            _viewer->setNavigationType(se->style());
        }
    }
}


#include "moc_View3DInventor.cpp"
