//////////////////////////////////////////////////////////////////////
//
//  MainWindow.h - Main application window.
//
//  UI redesign (mirrors the macOS ContentView.swift + RISEApp.swift):
//  central widget = TopBar over a 3-column workspace —
//    left panel (404px fixed, Agent / Scene file tabs)
//    center column (viewport stack / timeline / log drawer)
//    right panel (344px fixed, ViewportProperties)
//  The retired ControlsWidget's functions are rehoused: Open/Recent
//  stay in menus, Render/Render Animation/Cancel move to the Render
//  menu (+ pause/resume/restart refinement), the exposure slider lives
//  in the viewport toolbar's EV chip popup (slice B), and the progress
//  display moves to the TopBar cluster.
//
//////////////////////////////////////////////////////////////////////

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>

class QAction;
class QActionGroup;
class QMenu;
class QLabel;
class QStackedWidget;
class QVBoxLayout;
class QToolButton;
class RenderEngine;
class RenderWidget;
class HDRRenderWidget;
class TopBar;
class ChatPanel;
class LogWidget;
class SceneEditor;
class ViewportBridge;
class ViewportWidget;
class ViewportToolbar;
class ViewportTimeline;
class ViewportProperties;
class OutlinerWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenScene();
    void onOpenRecentScene(const QString& filePath);
    void onClearRecentFiles();
    void onClear();
    void onRender();
    void onRenderAnimation();
    void onCancel();
    void onSaveScene();

    void onStateChanged(int newState);
    void onSceneSizeDetected(int width, int height);
    void onSaveAndReload(const QString& filePath);
    void onSceneSavedToPath(const QString& path);

    // L5e — exposure slider.  UI redesign slice B: the slider widget
    // itself now lives inside the per-scene ViewportToolbar's EV chip
    // popup (see ViewportToolbar::exposureSlider()); these slots stay
    // on MainWindow because they own the RenderEngine business logic
    // (setViewExposureEV), connected fresh in rebuildViewportForLoadedScene.
    void onExposureSliderChanged(int value);
    void onExposureResetRequested();

    // L5b — HDR display path.  `onHDRToggled` flips the engine's
    // HDR mode AND the QStackedWidget's currently-shown production
    // pane between RenderWidget (SDR) and HDRRenderWidget (HDR).
    // `onHDRAvailabilityChanged` updates the toggle's enabled state
    // when the user drags the window between an HDR-capable monitor
    // and an SDR monitor (driven by HDRRenderWidget's screen-change
    // probe).
    void onHDRToggled(bool checked);
    void onHDRAvailabilityChanged(bool available);

    // L5d — File > Save Rendered Image…  Opens a QFileDialog with
    // EXR-default / PNG / TIFF filters, infers the format from the
    // chosen extension, dispatches via RenderEngine::saveAs.  No-op
    // if the engine hasn't started a render yet (action is disabled
    // until then).
    void onSaveRenderedImage();

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

    QWidget* buildLeftPanel();
    QWidget* buildCenterColumn();
    QWidget* buildRightPanel();
    QWidget* buildLogDrawer();
    void     setLeftTab(int index);

    // Single gate for every menu-action enable/label state that depends
    // on render state and/or viewport-bridge presence.  Driven from the
    // real state-change points (onStateChanged, dirtyChanged) rather
    // than solely from each menu's aboutToShow — QAction::setEnabled
    // also gates the action's global KEYBOARD SHORTCUT, so an action
    // that only got its enabled state refreshed when its menu opens
    // would leave shortcuts like Ctrl+Space / Ctrl+R inert until the
    // user visited that menu at least once.  Each menu's aboutToShow
    // still calls this too, purely to guarantee the visible LABEL text
    // (e.g. "Undo Translate", "Resume Refinement") is fresh at the
    // instant the menu is seen — isRefinementPaused() / undo-redo
    // labels can change without any Qt signal firing into MainWindow.
    void updateMenuActionStates();

    /// Round-3 P2: the ONE click-time gate for scene-transport actions
    /// (undo/redo, pause/resume/restart refinement) -- mirrors Mac's
    /// RenderViewModel.canUseSceneTransport so every caller inherits
    /// the production-render AND chat-render-outstanding clauses.  The
    /// setEnabled() plumbing uses the same terms; this closes the
    /// click-vs-disable race window for keyboard shortcuts and racing
    /// clicks.
    bool canUseSceneTransport() const;

    // P2 fix (mirrors Mac's ContentView.swift:366 exposure-popover gate
    // `renderState != .idle && !edrEnabled`): re-derives the EV chip's
    // enabled state from BOTH inputs together instead of only the HDR
    // interlock (the previous `setEvEnabled(!checked)` call sites never
    // consulted engine state at all).  In practice `m_viewportToolbar`
    // only exists between SceneLoaded and the next Idle transition (see
    // rebuildViewportForLoadedScene / teardownViewport), which already
    // approximates "not idle" structurally -- this makes that intent
    // explicit and keeps the two call sites (onHDRToggled,
    // onStateChanged) from being able to drift out of sync with each
    // other the way two independent inline `setEvEnabled` calls could.
    void updateEvEnabledState();

    // Recent files
    QStringList m_recentFiles;
    QMenu* m_recentFilesMenu = nullptr;
    static constexpr int MAX_RECENT_FILES = 10;

    RenderEngine* m_engine = nullptr;
    RenderWidget* m_renderWidget = nullptr;
    HDRRenderWidget* m_hdrRenderWidget = nullptr;  // L5b — Windows HDR display
    QStackedWidget* m_productionPaneStack = nullptr;  // SDR / HDR within production
    QAction*        m_hdrToggleAction = nullptr;       // View > HDR Preview
    QAction*        m_saveImageAction = nullptr;       // L5d — File > Save Rendered Image…
    QMenu*          m_toneCurveMenu = nullptr;         // L5e — View > Tone Curve
    QActionGroup*   m_toneCurveGroup = nullptr;        // L5e — exclusive group for tone-curve radio

    // UI redesign: menu actions whose enable state / label text is
    // driven live by updateMenuActionStates() (see its doc above).
    QAction* m_saveSceneAction = nullptr;
    QAction* m_closeSceneAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_pauseResumeAction = nullptr;
    QAction* m_restartAction = nullptr;
    QAction* m_renderAction = nullptr;
    QAction* m_renderAnimAction = nullptr;
    QAction* m_cancelAction = nullptr;

    TopBar*      m_topBar = nullptr;
    ChatPanel*   m_chatPanel = nullptr;
    LogWidget*   m_logWidget = nullptr;
    SceneEditor* m_sceneEditor = nullptr;

    // Left panel: slim Agent / Scene-file tab strip over a QStackedWidget.
    QWidget*      m_leftPanel = nullptr;
    QStackedWidget* m_leftPanelStack = nullptr;
    QToolButton*  m_agentTabBtn = nullptr;
    QToolButton*  m_sceneTabBtn = nullptr;

    // Right panel: fixed-width host for the outliner (persistent, built
    // once) over the per-scene ViewportProperties (rebuilt per scene).
    QWidget*         m_rightPanel = nullptr;
    QVBoxLayout*     m_rightPanelLayout = nullptr;
    OutlinerWidget*  m_outlinerWidget = nullptr;

    // Center column: persistent host for the view stack / timeline /
    // log drawer.  m_viewportTimeline is inserted/removed at index 1
    // by rebuildViewportForLoadedScene / teardownViewport (it's
    // created/destroyed alongside the per-scene viewport bridge).
    QVBoxLayout* m_centerColumnLayout = nullptr;
    QWidget*     m_logDrawerContainer = nullptr;


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

    void rebuildViewportForLoadedScene();
    void teardownViewport();
};

#endif // MAINWINDOW_H
