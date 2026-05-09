//////////////////////////////////////////////////////////////////////
//
//  MainWindow.h - Main application window with 3-panel layout.
//
//  Ported from the Mac app's ContentView.swift + RISEApp.swift.
//  Layout: [optional editor | [render view / [controls | log]]]
//
//////////////////////////////////////////////////////////////////////

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QStringList>

class QAction;
class QMenu;
class QStackedWidget;
class RenderEngine;
class RenderWidget;
class HDRRenderWidget;
class ControlsWidget;
class LogWidget;
class SceneEditor;
class ViewportBridge;
class ViewportWidget;
class ViewportToolbar;
class ViewportTimeline;
class ViewportProperties;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenScene();
    void onOpenRecentScene(const QString& filePath);
    void onClearRecentFiles();
    void onEditToggle();
    void onClear();
    void onRender();
    void onRenderAnimation();
    void onCancel();

    void onStateChanged(int newState);
    void onSceneSizeDetected(int width, int height);
    void onSaveAndReload(const QString& filePath);

    // L5b — HDR display path.  `onHDRToggled` flips the engine's
    // HDR mode AND the QStackedWidget's currently-shown production
    // pane between RenderWidget (SDR) and HDRRenderWidget (HDR).
    // `onHDRAvailabilityChanged` updates the toggle's enabled state
    // when the user drags the window between an HDR-capable monitor
    // and an SDR monitor (driven by HDRRenderWidget's screen-change
    // probe).
    void onHDRToggled(bool checked);
    void onHDRAvailabilityChanged(bool available);

protected:
    // L5b — re-probe HDR availability on window screen change.
    // The HDRRenderWidget's own event() handler covers the case
    // where it's the currently-shown widget; MainWindow's handler
    // covers the SDR-mode case where the HDR widget is hidden in
    // QStackedWidget and won't receive ScreenChangeInternal events.
    bool event(QEvent* ev) override;

private:
    void createMenuBar();
    void createStatusBar();
    void updateStatusBar();
    void updateWindowTitle();
    void updateRecentFilesMenu();
    void addToRecentFiles(const QString& filePath);
    void loadSceneFile(const QString& filePath);

    // Recent files
    QStringList m_recentFiles;
    QMenu* m_recentFilesMenu = nullptr;
    static constexpr int MAX_RECENT_FILES = 10;

    RenderEngine* m_engine = nullptr;
    RenderWidget* m_renderWidget = nullptr;
    HDRRenderWidget* m_hdrRenderWidget = nullptr;  // L5b — Windows HDR display
    QStackedWidget* m_productionPaneStack = nullptr;  // SDR / HDR within production
    QAction*        m_hdrToggleAction = nullptr;       // View > HDR Preview
    ControlsWidget* m_controlsWidget = nullptr;
    LogWidget* m_logWidget = nullptr;
    SceneEditor* m_sceneEditor = nullptr;

    // Interactive viewport — created lazily on scene load.  No more
    // toggle: viewport is always present once a scene is loaded.
    // Render stops the viewport's render thread first, runs the
    // production rasterizer, then restarts the viewport.
    ViewportBridge*     m_viewportBridge = nullptr;
    QWidget*            m_viewportPane = nullptr;
    ViewportWidget*     m_viewportWidget = nullptr;
    ViewportToolbar*    m_viewportToolbar = nullptr;
    ViewportTimeline*   m_viewportTimeline = nullptr;
    ViewportProperties* m_viewportProps = nullptr;
    QStackedWidget*     m_viewStack = nullptr;

    QSplitter* m_mainSplitter = nullptr;     // horizontal: editor | right
    QSplitter* m_rightSplitter = nullptr;    // vertical: render-or-viewport | bottom
    QSplitter* m_bottomSplitter = nullptr;   // horizontal: controls | log

    bool m_editorVisible = false;

    void rebuildViewportForLoadedScene();
    void teardownViewport();
};

#endif // MAINWINDOW_H
