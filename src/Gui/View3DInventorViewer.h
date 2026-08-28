// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2004 Jürgen Riegel <juergen.riegel@web.de>
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the     *
 *   License, or (at your option) any later version.                          *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful, but           *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of               *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *   GNU Lesser General Public License for more details.                      *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD.  If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                         *
 *                                                                            *
 ******************************************************************************/

#pragma once

#include <list>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <QCursor>
#include <QImage>
#include <QLabel>

#include <Inventor/SbRotation.h>
#include <Inventor/SbTime.h>
#include <Inventor/SbColor4f.h>
#include <Inventor/nodes/SoEnvironment.h>
#include <Inventor/nodes/SoEventCallback.h>
#include <Inventor/nodes/SoRotation.h>
#include <Inventor/nodes/SoSwitch.h>

#include <FCConfig.h>

#ifdef FC_OS_MACOSX
# include <OpenGL/gl.h>
#else
# ifdef FC_OS_WIN32
#  include <windows.h>
# endif  // FC_OS_WIN32
# include <GL/gl.h>
#endif  // FC_OS_MACOSX

#include <Base/BoundBox.h>
#include <Base/Placement.h>

#include "Namespace.h"
#include "Selection/Selection.h"

#include "CornerCrossLetters.h"
#include "View3DInventorSelection.h"
#include "Quarter/SoQTQuarterAdaptor.h"

class QOpenGLFramebufferObject;
class QOpenGLWidget;
class QSurfaceFormat;
class QTimer;

class SoTranslation;
class SoTransform;
class SoText2;
class SoAnnotation;

class SoSeparator;
class SoShapeHints;
class SoMaterial;
class SoRotationXYZ;
class SbSphereSheetProjector;
class SoEventCallback;  // NOLINT
class SbBox2s;
class SoVectorizeAction;
class QImage;
class SoGroup;  // NOLINT
class SoGLRenderAction;
class SoPickStyle;
class NaviCube;
class SoClipPlane;
class SoTimerSensor;
class SoSensor;
class SbBox3f;

namespace Quarter = SIM::Coin3D::Quarter;

namespace Base
{
class BoundBox2d;
}

namespace Gui
{
class NavigationAnimation;
class View3DInventor;
class ViewProvider;
class SoFCBackgroundGradient;
class NavigationStyle;
class SoFCUnifiedSelection;
class Document;
class GLGraphicsItem;
class RubberbandOverlay;
class SoGroundPlane;
class SoShapeScale;
class ViewerEventFilter;

/** Vulkan view render settings -- the single source of truth for the viewport.
 *
 *  This is the canonical in-memory blob for every Vulkan render option: the
 *  render mode (raster Coin / raster Vulkan / wireframe / AO / path tracing /
 *  environment), the cubemap environment preset, and the display + path-tracing
 *  tuning.  View3DInventorViewer loads it from the user preferences in
 *  applyVulkanSettings() (emitting vulkanSettingsChanged), and
 *  VulkanViewportAdapter::pushSettings() is the single applier that reads it to
 *  drive the backends.  Consumers must read the mode / raster gate from here,
 *  never from a second copy.
 */
struct VulkanViewSettings
{
    // Render mode: Gui::ViewRenderMode as int (0 RasterCoin, 1 RasterVulkan,
    // 2 Wireframe, 3 AmbientOcclusion, 4 RayTracing, 5 Environment).
    // Defaults to the Vulkan raster viewport.
    int renderMode = 1;
    // Cubemap environment preset index (-1 = viewport gradient/background).
    int envMap = -1;

    // True when the mode is a pure-raster mode (RasterCoin/RasterVulkan/
    // Wireframe).  The raster gate derived here tells the backends to keep
    // path tracing, ray tracing, the denoiser and the edge/point overlays
    // off regardless of any persisted tuning.
    bool rasterOnly() const { return renderMode >= 0 && renderMode <= 2; }

    bool showEdges = false;
    bool showPoints = false;
    SbColor4f edgeColor = SbColor4f(0.05f, 0.05f, 0.05f, 1.0f);
    bool pathTracing = false;
    // Path-tracing tuning (see the View preferences dialog).
    int pathTracingBounces = 4;
    int pathTracingSettleFrames = 6;
    int pathTracingMaxSamples = 256;
    // Denoiser backend name ("rtx", "oidn", "fsr", "none"); empty = default.
    // Denoising itself is required for path tracing and is enabled automatically
    // by the renderer; only the filter is configurable.
    std::string pathTracingDenoiser;
};

/** GUI view into a 3D scene provided by View3DInventor
 *
 */
class GuiExport View3DInventorViewer: public Quarter::SoQTQuarterAdaptor, public SelectionObserver
{
    using inherited = Quarter::SoQTQuarterAdaptor;
    Q_OBJECT

public:
    /// Pick modes for picking points in the scene
    enum SelectionMode
    {
        Lasso = 0,      /**< Select objects using a lasso. */
        Rectangle = 1,  /**< Select objects using a rectangle. */
        Rubberband = 2, /**< Select objects using a rubberband. */
        BoxZoom = 3,    /**< Perform a box zoom. */
        Clip = 4,       /**< Clip objects using a lasso. */
    };
    /** @name Modus handling of the viewer
     * Here you can switch several features on/off
     * and modes of the Viewer
     */
    //@{
    enum ViewerMod
    {
        ShowCoord = 1,        /**< Enables the Coordinate system in the corner. */
        ShowFPS = 2,          /**< Enables the Frames Per Second counter. */
        SimpleBackground = 4, /**< switch to a simple background. */
        DisallowRotation = 8, /**< switch off the rotation. */
        DisallowPanning = 16, /**< switch off the panning. */
        DisallowZooming = 32, /**< switch off the zooming. */
    };
    //@}

    /// Declares why the viewer scene is being traversed so screen-only
    /// decorations can be excluded from capture and export paths.
    enum class RenderIntent
    {
        /// Interactive viewport traversal including viewer decorations.
        LiveInteractive,
        /// Fresh raster output excluding screen-only viewer decorations.
        RasterCapture,
        /// Vector output excluding screen-only viewer decorations.
        VectorExport
    };

    /** @name Render mode
     */
    //@{
    enum RenderType
    {
        Native,
        Framebuffer,
        Image
    };
    //@}

    /** @name Background
     */
    //@{
    enum Background
    {
        NoGradient,
        LinearGradient,
        RadialGradient
    };
    //@}

    explicit View3DInventorViewer(QWidget* parent, const QOpenGLWidget* sharewidget = nullptr);
    View3DInventorViewer(
        const QSurfaceFormat& format,
        QWidget* parent,
        const QOpenGLWidget* sharewidget = nullptr
    );
    ~View3DInventorViewer() override;

    void init();

    /// Observer message from the Selection
    void onSelectionChanged(const SelectionChanges& Reason) override;

    SoDirectionalLight* getBacklight() const;
    void setBacklightEnabled(bool on);
    bool isBacklightEnabled() const;

    SoDirectionalLight* getFillLight() const;
    void setFillLightEnabled(bool on);
    bool isFillLightEnabled() const;

    SoEnvironment* getEnvironment() const;

    void setSceneGraph(SoNode* root) override;
    bool searchNode(SoNode*) const;

    void setAnimationEnabled(bool enable);
    void setSpinningAnimationEnabled(bool enable);
    bool isAnimationEnabled() const;
    bool isSpinningAnimationEnabled() const;
    bool isAnimating() const;
    bool isSpinning() const;
    std::shared_ptr<NavigationAnimation> startAnimation(
        const SbRotation& orientation,
        const SbVec3f& rotationCenter,
        const SbVec3f& translation,
        int duration = -1,
        bool wait = false
    ) const;
    void startSpinningAnimation(const SbVec3f& axis, float velocity);
    void stopAnimating();

    void setPopupMenuEnabled(bool on);
    bool isPopupMenuEnabled() const;

    void setFeedbackVisibility(bool enable);
    bool isFeedbackVisible() const;

    void setFeedbackSize(int size);
    int getFeedbackSize() const;

    /// Get the preferred samples from the user settings
    static int getNumSamples();
    void setRenderType(RenderType type);
    RenderType getRenderType() const;

    /** Options for rendering the scene into a fresh image. */
    struct RenderImageOptions
    {
        int width = 0;
        int height = 0;
        int samples = -1;
        QColor background;
        RenderIntent intent = RenderIntent::RasterCapture;
        bool includeViewerLighting = true;
    };

    /** Render the scene into a new image using the requested capture policy. */
    QImage renderToImage(const RenderImageOptions& options);

    /** Capture the live viewport framebuffer as a raster-oriented image. */
    QImage grabFramebuffer();

    void setViewing(bool enable) override;
    virtual void setCursorEnabled(bool enable);

    void addGraphicsItem(GLGraphicsItem*);
    void removeGraphicsItem(GLGraphicsItem*);
    std::list<GLGraphicsItem*> getGraphicsItems() const;
    std::list<GLGraphicsItem*> getGraphicsItemsOfType(const Base::Type&) const;
    void clearGraphicsItems();

    RubberbandOverlay& rubberbandOverlay();

    /** @name Handling of view providers */
    //@{
    /// Checks if the view provider is a top-level object of the scene
    bool hasViewProvider(ViewProvider*) const;
    /// Checks if the view provider is part of the scene.
    /// In contrast to hasViewProvider() this method also checks if the view
    /// provider is a child of another view provider
    bool containsViewProvider(const ViewProvider*) const;
    /// adds an ViewProvider to the view, e.g. from a feature
    void addViewProvider(ViewProvider*);
    /// remove a ViewProvider
    void removeViewProvider(ViewProvider*);
    /// get view provider by path
    ViewProvider* getViewProviderByPath(SoPath*) const;
    ViewProvider* getViewProviderByPathFromTail(SoPath*) const;
    /// get all view providers of given type
    std::vector<ViewProvider*> getViewProvidersOfType(const Base::Type& typeId) const;
    /// set the ViewProvider in special edit mode
    void setEditingViewProvider(Gui::ViewProvider* vp, int ModNum);
    /// return whether a view provider is edited
    bool isEditingViewProvider() const;
    /// return currently editing view provider
    ViewProvider* getEditingViewProvider() const;
    /// reset from edit mode
    void resetEditingViewProvider();
    SoNode* getEditingRoot() const;
    void setupEditingRoot(SoNode* node = nullptr, const Base::Matrix4D* mat = nullptr);
    void resetEditingRoot(bool updateLinks = true);
    void setEditingTransform(const Base::Matrix4D& mat);
    /** Helper method to get picked entities while editing.
     * It's in the responsibility of the caller to delete the returned instance.
     */
    SoPickedPoint* getPointOnRay(const SbVec2s& pos, const ViewProvider* vp) const;
    /** Helper method to get picked entities while editing.
     * It's in the responsibility of the caller to delete the returned instance.
     */
    SoPickedPoint* getPointOnRay(const SbVec3f& pos, const SbVec3f& dir, const ViewProvider* vp) const;
    /// display override mode
    void setOverrideMode(const std::string& mode);
    void updateOverrideMode(const std::string& mode);
    std::string getOverrideMode() const
    {
        return overrideMode;
    }
    //@}

    /** @name Making pictures */
    //@{
    /**
     * Creates an image with width \a width and height \a height of the current scene graph
     * using a multi-sampling of \a sample and exports the rendered scenegraph to an image.
     */
    void savePicture(
        int width,
        int height,
        int sample,
        const QColor& bg,
        QImage& img,
        RenderIntent intent = RenderIntent::LiveInteractive
    ) const;
    void saveGraphic(
        int pagesize,
        const QColor&,
        SoVectorizeAction* va,
        RenderIntent intent = RenderIntent::VectorExport
    ) const;
    //@}
    /**
     * Writes the current scenegraph to an Inventor file, either in ascii or binary.
     */
    bool dumpToFile(SoNode* node, const char* filename, bool binary) const;

    /** @name Selection methods */
    //@{
    void startSelection(SelectionMode = Lasso);
    void abortSelection();
    void stopSelection();
    bool isSelecting() const;
    std::vector<SbVec2f> getGLPolygon(SelectionRole* role = nullptr) const;
    std::vector<SbVec2f> getGLPolygon(const std::vector<SbVec2s>&) const;
    const std::vector<SbVec2s>& getPolygon(SelectionRole* role = nullptr) const;
    void setSelectionEnabled(bool enable);
    bool isSelectionEnabled() const;
    //@}

    /// Returns the screen coordinates of the origin of the path's tail object
    /*! Return value is in floating-point pixels, origin at bottom-left. */
    SbVec2f screenCoordsOfPath(SoPath* path) const;

    /** @name Edit methods */
    //@{
    void setEditing(bool edit);
    bool isEditing() const
    {
        return this->editing;
    }
    void setEditingCursor(const QCursor& cursor);
    void setComponentCursor(const QCursor& cursor);
    void setRedirectToSceneGraph(bool redirect)
    {
        this->redirected = redirect;
    }
    bool isRedirectedToSceneGraph() const
    {
        return this->redirected;
    }
    void setRedirectToSceneGraphEnabled(bool enable)
    {
        this->allowredir = enable;
    }
    bool isRedirectToSceneGraphEnabled() const
    {
        return this->allowredir;
    }
    //@}

    /** @name Pick actions */
    //@{
    // calls a PickAction on the scene graph
    bool pickPoint(const SbVec2s& pos, SbVec3f& point, SbVec3f& norm) const;
    SoPickedPoint* pickPoint(const SbVec2s& pos) const;
    const SoPickedPoint* getPickedPoint(SoEventCallback* n) const;
    bool pubSeekToPoint(const SbVec2s& pos);
    void pubSeekToPoint(const SbVec3f& pos);
    //@}

    /**
     * Set up a callback function \a cb which will be invoked for the given eventtype.
     * \a userdata will be given as the first argument to the callback function.
     */
    void addEventCallback(SoType eventtype, SoEventCallbackCB* cb, void* userdata = nullptr);
    /**
     * Unregister the given callback function \a cb.
     */
    void removeEventCallback(SoType eventtype, SoEventCallbackCB* cb, void* userdata = nullptr);

    /** @name Clipping plane, near and far plane */
    //@{
    /** Returns the view direction from the user's eye point in direction to the
     * viewport which is actually the negative normal of the near plane.
     * The vector is normalized to length of 1.
     */
    SbVec3f getViewDirection() const;
    void setViewDirection(SbVec3f);
    /** Returns the up direction */
    SbVec3f getUpDirection() const;

    /** Returns the orientation of the camera. */
    SbRotation getCameraOrientation() const;

    /** Returns the 3d point on the focal plane to the given 2d point. */
    SbVec3f getPointOnFocalPlane(const SbVec2s&) const;

    /** Returns the 3d point on a line to the given 2d point. */
    SbVec3f getPointOnLine(const SbVec2s&, const SbVec3f& axisCenter, const SbVec3f& axis) const;

    /** Returns the 3d point on the XY plane of a placement to the given 2d point. */
    SbVec3f getPointOnXYPlaneOfPlacement(const SbVec2s&, const Base::Placement&) const;

    /** Returns the bounding box on the XY plane of a placement to the given 2d point. */
    Base::BoundBox2d getViewportOnXYPlaneOfPlacement(Base::Placement plc) const;

    /** Returns the 2d coordinates on the viewport to the given 3d point. */
    SbVec2s getPointOnViewport(const SbVec3f&) const;

    /** Returns the per-axis scale between viewport-region pixels and widget
     * pixels (region size / widget size).  The hidden GL viewer's viewport
     * region may be sized in device pixels (Vulkan mode) or logical pixels
     * (classic GL mode); dividing region-space coordinates by this scale
     * yields widget-space coordinates in both cases. */
    SbVec2f viewportPixelScale() const;

    /** Converts Inventor coordinates into Qt coordinates.
     * The conversion takes the device pixel ratio into account.
     */
    QPoint toQPoint(const SbVec2s&) const;

    /** Converts Qt coordinates into Inventor coordinates.
     * The conversion takes the device pixel ratio into account.
     */
    SbVec2s fromQPoint(const QPoint&) const;

    /** Returns the near plane represented by its normal and base point. */
    void getNearPlane(SbVec3f& rcPt, SbVec3f& rcNormal) const;

    /** Returns the far plane represented by its normal and base point. */
    void getFarPlane(SbVec3f& rcPt, SbVec3f& rcNormal) const;

    /** Adds or remove a manipulator to/from the scenegraph. */
    void toggleClippingPlane(
        int toggle = -1,
        bool beforeEditing = false,
        bool noManip = false,
        const Base::Placement& pla = Base::Placement()
    );

    /** Checks whether a clipping plane is set or not. */
    bool hasClippingPlane() const;

    /** Project the given normalized 2d point onto the near plane */
    SbVec3f projectOnNearPlane(const SbVec2f&) const;

    /** Project the given normalized 2d point onto the far plane */
    SbVec3f projectOnFarPlane(const SbVec2f&) const;

    /** Project the given 2d point to a line */
    void projectPointToLine(const SbVec2s&, SbVec3f& pt1, SbVec3f& pt2) const;

    /** Get the normalized position of the 2d point. */
    SbVec2f getNormalizedPosition(const SbVec2s&) const;
    //@}

    /** @name Dimension controls
     * the "turn*" functions are wired up to parameter groups through view3dinventor.
     * don't call them directly. instead set the parameter groups.
     * @see TaskDimension
     */
    //@{
    void turnAllDimensionsOn();
    void turnAllDimensionsOff();
    void turn3dDimensionsOn();
    void turn3dDimensionsOff();
    void turnDeltaDimensionsOn();
    void turnDeltaDimensionsOff();
    void eraseAllDimensions();
    void addDimension3d(SoNode* node);
    void addDimensionDelta(SoNode* node);
    //@}

    /**
     * Set the camera's orientation. If isAnimationEnabled() returns
     * \a true the reorientation is animated and the animation is returned, otherwise its directly
     * set.
     */
    std::shared_ptr<NavigationAnimation> setCameraOrientation(
        const SbRotation& orientation,
        bool moveToCenter = false
    ) const;
    void setCameraType(SoType type) override;
    bool setCamera(const char* pCamera);
    void moveCameraTo(const SbRotation& orientation, const SbVec3f& position, int duration = -1);
    /**
     * Zooms the viewport to the size of the bounding box.
     */
    void boxZoom(const SbBox2s&);
    /**
     * Scale the viewport by a linear amount
     */
    void scale(float factor);
    /**
     * Move the camera to the configured home orientation and fit the scene.
     */
    void viewHome();
    /**
     * Reposition the current camera so we can see the complete scene.
     */
    void viewAll() override;
    void viewAll(float factor);
    void viewBoundBox(const SbBox3f& box);

    /// Breaks out a VR window for a Rift
    void viewVR();

    /**
     * Returns the bounding box of the scene graph.
     */
    SbBox3f getBoundingBox() const;

    /**
     * Reposition the current camera so we can see all selected objects.
     *
     * @param extend: Whether to extend the current view (zoom out if
     * necessary) to include the selection, or zoom in the camera to view only
     * the selection.
     */
    void viewSelection(bool extend = false);

    /** Reposition the current camera so we can see the given objects
     *
     * @param objs: viewing objects
     *
     * @param extend: Whether to extend the current view (zoom out if
     * necessary) to include the objects, or zoom in the camera to view only
     * the given objects.
     */
    void viewObjects(const std::vector<App::SubObjectT>& objs, bool extend = false);


    void alignToSelection();

    void setGradientBackground(Background);
    Background getGradientBackground() const;
    void getGradientBackgroundColor(SbColor& fromColor, SbColor& toColor) const;
    void setGradientBackgroundColor(const SbColor& fromColor, const SbColor& toColor);
    void setGradientBackgroundColor(
        const SbColor& fromColor,
        const SbColor& toColor,
        const SbColor& midColor
    );
    void setNavigationType(Base::Type);

    void setAxisLetterColor(const SbColor& color);
    void setAxisCross(bool on);
    bool hasAxisCross();

    void setGroundPlane(bool on);
    bool hasGroundPlane();
    void setGroundPlaneOpacity(float opacity);

    void showRotationCenter(bool show);
    void changeRotationCenterPosition(const SbVec3f& newCenter);

    void setEnabledFPSCounter(bool on);
    void setEnabledNaviCube(bool on);
    bool isEnabledNaviCube() const;
    void setNaviCubeCorner(int);
    NaviCube* getNaviCube() const;
    //! The annotation group holding the nav cube coin node (empty when hidden).
    SoAnnotation* getNaviCubeAnnotation() const;
    //! The axis cross overlay container for the IR (Vulkan) render path:
    //! a SoAxisCrossOverlay scoping the shared axis/letter graphs to the
    //! bottom-right corner viewport in the overlay render pass.
    SoNode* getAxisCrossOverlay();
    //! Refresh the axis cross overlay nodes (transforms, colors, letters)
    //! without issuing any GL rendering; called by drawAxisCross() and by
    //! the Vulkan viewport sync so the hidden GL viewer's frame loop is not
    //! required for the IR render path.
    void updateAxisCrossNodes();
    void setEnabledVBO(bool on);
    bool isEnabledVBO() const;
    void setRenderCache(int);

    //! Update colors of axis in corner to match preferences
    void updateColors();

    void getDimensions(float& fHeight, float& fWidth) const;
    float getMaxDimension() const;
    SbVec3f getFocalPoint() const;

    NavigationStyle* navigationStyle() const;

    void setDocument(Gui::Document* pcDocument);
    Gui::Document* getDocument();

    virtual PyObject* getPyObject();

    bool getSceneBoundBox(SbBox3f& box) const;
    bool getSceneBoundBox(Base::BoundBox3d& box) const;

    //! Vulkan-only display options owned by the viewer.  They mirror the
    //! OpenGL equivalents (draw style, vertex visibility) but only the
    //! Vulkan backend honors them.  Read them with getVulkanViewSettings();
    //! applyVulkanSettings() reloads them from the preferences and emits
    //! vulkanSettingsChanged().
    const VulkanViewSettings& getVulkanViewSettings() const
    {
        return vulkanSettings_;
    }
    void applyVulkanSettings();

Q_SIGNALS:
    void cameraChanged();
    //! Emitted when navigation mutated the active camera pose (rotate/pan/
    //! zoom) in place, i.e. without replacing the camera node.  The
    //! display-only Vulkan viewport owns no Coin sensors and goes converged-
    //! idle once the path tracer finishes, so it would not otherwise re-render
    //! on a camera move; this lets the adapter request a frame so the moved
    //! camera is shown (the backend then sees it as a reset-on-move and
    //! restarts the accumulation).
    void cameraMoved();
    //! Emitted after applyVulkanSettings() reloaded the Vulkan options from
    //! the preferences.
    void vulkanSettingsChanged();
    //! Emitted after the document selection/preselection context changed
    //! (a face was selected/preselected/cleared).  The Vulkan viewport is
    //! display-only and owns no Coin sensors, so a pure scene-graph colour
    //! mutation is not enough to schedule a frame; this lets the adapter
    //! request a redraw so the highlight appears even after path tracing
    //! has converged and the continuous refine loop has gone idle.
    void selectionChanged();

protected:
    static GLenum getInternalTextureFormat();
    void renderScene();
    void renderRubberbandOverlay();
    void renderFramebuffer();
    void renderGLImage();
    void animatedViewAll(const SbBox3f& bbox, int steps, int ms);
    void actualRedraw() override;
    void setSeekMode(bool on) override;
    void afterRealizeHook() override;
    bool processSoEvent(const SoEvent* ev) override;
    void dropEvent(QDropEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* ev) override;
    void dragMoveEvent(QDragMoveEvent* ev) override;
    void dragLeaveEvent(QDragLeaveEvent* ev) override;
    bool processSoEventBase(const SoEvent* const ev);
    void printDimension() const;
    void selectAll();

    static void onViewFitTimer(void*, SoSensor*);

private:
    static void setViewportCB(void* userdata, SoAction* action);
    static void clearBufferCB(void* userdata, SoAction* action);
    static void setGLWidgetCB(void* userdata, SoAction* action);
    static void handleEventCB(void* userdata, SoEventCallback* n);
    static void interactionStartCB(void* data, Quarter::SoQTQuarterAdaptor* viewer);
    static void interactionFinishCB(void* data, Quarter::SoQTQuarterAdaptor* viewer);
    static void interactionLoggerCB(void* ud, SoAction* action);

private:
    class ScopedRenderIntent;
    static void selectCB(void* viewer, SoPath* path);
    // A small intent stack lets nested export/capture code paths temporarily
    // override the default live-view traversal behavior.
    void pushRenderIntentOverride(RenderIntent intent) const;
    void popRenderIntentOverride() const;
    RenderIntent currentRenderIntent() const;
    static bool shouldRenderDecorations(RenderIntent intent);

    static void deselectCB(void* viewer, SoPath* path);
    static SoPath* pickFilterCB(void* viewer, const SoPickedPoint* pp);
    void initialize();
    void syncNaviCubeVisibility();
    void drawAxisCross();
    void drawSingleBackground(const QColor&);
    void recoverFromRenderMemoryException();
    void renderDelayedAnnotations(SoGLRenderAction* glra);
    void renderGLActionScene(const QColor& backgroundColor, SoGLRenderAction* glra);
    bool renderToFramebuffer(QOpenGLFramebufferObject*, bool includeViewerLighting = true);
    void setCursorRepresentation(int mode);
    void aboutToDestroyGLContext();
    void createStandardCursors();
    bool applyCameraState(const SoCamera& camera);

private:
    NaviCube* naviCube;
    SoAnnotation* naviCubeAnnotation;
    std::set<ViewProvider*> _ViewProviderSet;
    std::map<SoSeparator*, ViewProvider*> _ViewProviderMap;
    std::list<GLGraphicsItem*> graphicsItems;
    std::unique_ptr<RubberbandOverlay> rubberbandOverlayRenderer;
    ViewProvider* editViewProvider;
    VulkanViewSettings vulkanSettings_;
    SoFCBackgroundGradient* pcBackGround;
    SoSeparator* backgroundroot;
    SoSeparator* foregroundroot;
    // Dedicated root for viewer-owned HUD/decorations that should not be
    // treated as model content during capture/export traversals.
    SoSeparator* decorationroot;

    SoDirectionalLight* backlight;
    SoDirectionalLight* fillLight;
    SoEnvironment* environment;
    SoGroup* viewerLightingRoot;
    SoSeparator* viewerSceneRoot;

    SoRotation* lightRotation;

    // Scene graph root
    SoSeparator* pcViewProviderRoot;
    // Child group in the scene graph that contains view providers related to the physical object
    SoGroup* objectGroup;

    std::unique_ptr<View3DInventorSelection> inventorSelection;

    SoSeparator* pcEditingRoot;
    SoTransform* pcEditingTransform;
    bool restoreEditingRoot;
    SoEventCallback* pEventCallback;
    NavigationStyle* navigation;
    SoFCUnifiedSelection* selectionRoot;

    SoClipPlane* pcClipPlane;

    RenderType renderType;
    QOpenGLFramebufferObject* framebuffer;
    QImage glImage;
    bool shading;
    SoSwitch* dimensionRoot;

    // small axis cross in the corner
    bool axiscrossEnabled;
    int axiscrossSize;
    // big one in the middle
    SoShapeScale* axisCross;
    SoGroup* axisGroup;

    // ground plane grid on the world XY plane (Z=0)
    SoSeparator* groundPlaneGroup;
    SoGroundPlane* groundPlane;
    float groundPlaneOpacity;

    SoGroup* rotationCenterGroup;

    // stuff needed to draw the fps counter
    bool fpsEnabled;
    QLabel* fpsCounter = nullptr;
    QTimer* fpsUpdateTimer = nullptr;
    unsigned long previousAxisLetterColor = 0;
    bool vboEnabled;
    bool naviCubeEnabled;

    // Screen-only viewer decorations such as the navicube are rendered only
    // when the active render intent allows them.
    mutable std::vector<RenderIntent> renderIntentOverrideStack;

    Base::Color m_xColor;
    Base::Color m_yColor;
    Base::Color m_zColor;

    bool editing;
    QCursor editCursor, zoomCursor, panCursor, spinCursor;
    bool redirected;
    bool allowredir;

    bool viewFitting;
    SbTime viewFitTime;
    SoTimerSensor* viewFitTimer;

    std::string overrideMode;
    Gui::Document* guiDocument = nullptr;

    ViewerEventFilter* viewerEventFilter;

    PyObject* _viewerPy;

    static unsigned char XPM_pixel_data[YPM_WIDTH * YPM_HEIGHT * YPM_BYTES_PER_PIXEL + 1];
    static unsigned char YPM_pixel_data[YPM_WIDTH * YPM_HEIGHT * YPM_BYTES_PER_PIXEL + 1];
    static unsigned char ZPM_pixel_data[ZPM_WIDTH * ZPM_HEIGHT * ZPM_BYTES_PER_PIXEL + 1];

private Q_SLOTS:
    void updateFPSLabel();
    //! Recompute the effective pick radius (resolution + zoom) and push it into
    //! the Coin event manager so hover/preselection stays zoom-compensated.
    void updatePickRadius();

    // friends
    friend class NavigationStyle;
    friend class GLPainter;
    friend class ViewerEventFilter;
};

}  // namespace Gui
