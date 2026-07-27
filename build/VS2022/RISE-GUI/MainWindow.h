//////////////////////////////////////////////////////////////////////
//
//  MainWindow.h - Main application window.
//
//  UI redesign (mirrors the macOS ContentView.swift + RISEApp.swift):
//  central widget = TopBar over a 3-column workspace —
//    left panel (user-resizable via QSplitter, 360-900px, 520px default,
//      persisted to QSettings "leftPanelWidth"; Agent / Scene file tabs)
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
#include <QMap>
#include <QStringList>
#include <QtGlobal>

#include "Theme.h"

class QAction;
class QTimer;
class QActionGroup;
class QMenu;
class QLabel;
class QStackedWidget;
class QVBoxLayout;
class QToolButton;
class QSplitter;
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
class EnvironmentPanel;
class StartWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Shutdown-order fix (2026-07-24): QObject::deleteChildren destroys
    // children in CREATION order, and m_engine (created first, in the
    // constructor) precedes m_viewportBridge (created on scene load) in
    // that list -- so the default teardown freed the RenderEngine before
    // the bridge's destructor ran its detach sequence against it.  The
    // destructor uses the ordered viewport teardown to detach every bridge
    // borrower before deleting the bridge while the engine is still alive.
    ~MainWindow() override;

private slots:
    void onOpenScene();
    void onOpenRecentScene(const QString& filePath);
    void onClearRecentFiles();
    void onClear();
    void onRender();
    void onRenderActiveRegion();
    void onRenderAnimation();
    void onCancel();
    void onSaveScene();

    void onStateChanged(int newState);
    void onSceneSizeDetected(int width, int height);
    void onSaveAndReload(const QString& filePath);
    void onSceneSavedToPath(const QString& path, bool wasSaveAs);

    // ---- Start screen (docs/gui/START_SCREEN.md) ----------------------
    // StartWidget owns no scene state itself; every one of its signals
    // routes to the exact code path the equivalent menu/button already
    // uses (see each slot's doc below).
    /// The ✕ on a missing recent row, or File > Open Recent's own
    /// missing-file handling (onOpenRecentScene) -- unified here so both
    /// surfaces persist the removal (the menu-only prior behavior did
    /// not write QSettings, so a stale entry reappeared on next launch).
    void removeRecentFile(const QString& filePath);
    /// Create clicked with a non-empty, ready prompt (StartWidget already
    /// validated both -- see StartWidget::createRequested's doc).
    void onCreateSceneFromPrompt(const QString& prompt);

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

    // Live-scene UI sync -- ~2 Hz poll while a viewport bridge is attached,
    // mirroring macOS RenderViewModel.pollRefinementState.  It mirrors CST
    // text into the editor and refreshes animation presence/options so an
    // agent-added first timeline appears without rebuilding the viewport.
    // Started/stopped alongside the bridge in rebuildViewportForLoadedScene /
    // teardownViewport.  Explicit-save-only (user decision 2026-07-12): no
    // branch here writes the .RISEscene to disk; writes happen only via
    // performSceneSave(), triggered by the TopBar or File menu.
    void onCstSyncTick();

protected:
    // L5b — re-probe HDR availability on window screen change.
    // The HDRRenderWidget's own event() handler covers the case
    // where it's the currently-shown widget; MainWindow's handler
    // covers the SDR-mode case where the HDR widget is hidden in
    // QStackedWidget and won't receive ScreenChangeInternal events.
    bool event(QEvent* ev) override;

    // LIVE THEME-SWITCH CONTRACT (Theme.h): every widget class with
    // token-dependent styling hooks QEvent::PaletteChange here and
    // calls its own restyleTheme(). MainWindow is the reference
    // implementation the per-panel conversions cite.
    void changeEvent(QEvent* e) override;

private:
    void createMenuBar();
    void createStatusBar();
    void updateStatusBar();
    void updateWindowTitle();
    void updateRecentFilesMenu();
    void addToRecentFiles(const QString& filePath);
    /// Single writer for both the recentSceneFiles path list and the
    /// sibling recentSceneMeta (path -> last-opened epoch seconds, start
    /// screen spec §5.4) to QSettings -- addToRecentFiles/removeRecentFile
    /// both funnel through this so the two keys can never drift out of
    /// sync with each other.
    void persistRecents();
    /// `untitled` (start-screen create path, spec §5.2): loads through the
    /// SAME path a normal Open uses (dirty-editor guard, already-loaded
    /// confirm, RenderEngine::loadScene, SceneEditor initial read) except
    /// it is never added to recents.  See RenderEngine::loadScene's doc
    /// for how `untitled` propagates to loadedFilePath().  Returns false
    /// if the user cancelled one of the confirm dialogs (unsaved editor
    /// changes, or "a scene is already loaded") -- onCreateSceneFromPrompt
    /// uses this to reset the Create button rather than leaving it stuck
    /// on "Creating…" for a load that never happened.
    bool loadSceneFile(const QString& filePath, bool untitled = false);
    /// Resolves the bundled starter template (start screen create path,
    /// spec §5.1/§5.3): the build-output copy the .vcxproj's post-build
    /// step places at $(OutDir)Templates\, falling back to the
    /// repo-relative canonical path for a dev run from an unusual working
    /// directory.  Empty return means neither exists -- surfaced as an
    /// honest error banner rather than a silent no-op.
    QString resolveStarterScenePath() const;
    /// Single point that decides which widget the center viewport stack
    /// shows: the interactive viewport pane while a bridge is attached,
    /// the passive RenderWidget's loading overlay while a load is in
    /// flight with no bridge yet, and StartWidget otherwise (fresh
    /// launch, after Close Scene, or a failed load -- mirrors macOS
    /// ContentView's `viewportBridge == nil` gate).  Refreshes
    /// StartWidget's recents snapshot immediately before showing it, so
    /// missing-file rows are always current ("verified per render of the
    /// screen", spec §4.1).  Called from every place the bridge or engine
    /// state can change.
    void updateCenterViewStack();

    QWidget* buildLeftPanel();
    QWidget* buildCenterColumn();
    QWidget* buildRightPanel();
    QWidget* buildLogDrawer();
    void     setLeftTab(int index);
    /// Refreshes just the Agent/Scene-file tab buttons' font weight +
    /// border-underline stylesheet from the CURRENTLY checked button and
    /// the CURRENT Theme:: token values -- factored out of setLeftTab()
    /// so restyleTheme() can re-apply it on a theme switch WITHOUT
    /// re-triggering setLeftTab()'s scene-file-reload side effect.
    void     updateTabButtonStyles();

    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies every one of
    // MainWindow's own token-dependent styling sites (splitter handle,
    // left/right panel border stylesheets + QPalette::Window fills, the
    // tab-strip border, the tab buttons' active/inactive stylesheets,
    // the center column's QPalette::Window fill) from the CURRENT
    // Theme:: token values. Called once at the end of the constructor
    // and again from changeEvent() on QEvent::PaletteChange. Must stay
    // idempotent and must not create widgets -- see the contract.
    void     restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // m_themeReady gates changeEvent()'s restyleTheme() call until the
    // ctor has actually reached its own restyleTheme() call (member
    // pointers touched by restyleTheme() may still be null before that).
    // m_themeEpochSeen dedupes so a setStyleSheet-triggered synchronous
    // re-entrant PaletteChange (delivered mid-restyleTheme()) can't
    // recurse. Uniform across every changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    // Single gate for every menu-action enable/label state that depends
    // on render state and/or viewport-bridge presence.  Driven from the
    // real state-change points (onStateChanged, dirtyChanged) rather
    // than solely from each menu's aboutToShow — QAction::setEnabled
    // also gates the action's global KEYBOARD SHORTCUT, so an action
    // that only got its enabled state refreshed when its menu opens
    // would leave shortcuts like Ctrl+Space / Ctrl+R inert until the
    // user visited that menu at least once.  Each menu's aboutToShow
    // still calls this too, purely to guarantee the visible LABEL text
    // (e.g. "Undo Translate", "Resume Render") is fresh at the
    // instant the menu is seen — isProductionRenderPaused() / undo-redo
    // labels can change without any Qt signal firing into MainWindow.
    void updateMenuActionStates();

    /// Round-3 P2: the ONE click-time gate for scene-transport actions
    /// (undo/redo, restart refinement) -- mirrors Mac's
    /// RenderViewModel.canUseSceneTransport so every caller inherits
    /// the production-render AND chat-render-outstanding clauses.  The
    /// setEnabled() plumbing uses the same terms; this closes the
    /// click-vs-disable race window for keyboard shortcuts and racing
    /// clicks.
    bool canUseSceneTransport() const;

    /// Reverse source traceability: THE single gate for "Select in Inspector"
    /// -- the scene transport is free (no render owns the controller) AND the
    /// editor buffer is clean (the click offset resolves against the live CST
    /// serialization, which the buffer only matches when clean).  Used both to
    /// enable/grey the menu item (SceneTextEdit predicate) and to re-check in
    /// the selectEntityAtByteOffset slot -- one source, no drift.  Mirrors
    /// Mac's RenderViewModel.canReverseSelect.
    bool canReverseSelect() const;

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

    // CST <-> scene-file live sync (UI refinement item 1).  Cheap
    // change-detector pair from ViewportBridge::getSceneTextVersion.
    // uuid is fresh per load; revision bumps iff content changed.
    // `valid` stands in for macOS's `SceneTextVersion?` optional --
    // false means "no version observed yet" (never compares equal to
    // itself while false, matching Swift's `nil != nil` being false
    // but every comparison here first null-checks `valid`).
    struct SceneTextVersion {
        quint64 uuid = 0;
        quint64 revision = 0;
        bool    valid = false;
        bool operator==(const SceneTextVersion& o) const {
            return valid && o.valid && uuid == o.uuid && revision == o.revision;
        }
        bool operator!=(const SceneTextVersion& o) const { return !(*this == o); }
    };

    /// The version last mirrored into the SceneEditor buffer (item 1),
    /// or set at bridge-attach time so the file-on-disk bytes and the
    /// live CST start in agreement.  Invalid before any bridge attaches.
    SceneTextVersion m_lastSyncedSceneTextVersion;
    /// ~2 Hz live-scene poll driving onCstSyncTick.  Started in
    /// rebuildViewportForLoadedScene, stopped in teardownViewport.
    QTimer* m_cstSyncTimer = nullptr;

    /// Shared implementation behind File > Save Scene and the TopBar's
    /// Save pill for an ALREADY-NAMED scene (in-place, at the currently
    /// loaded path).  Mirrors macOS RenderViewModel.saveScene().
    /// Explicit-save-only (user decision 2026-07-12): this is the ONLY
    /// place a .RISEscene write ever happens -- there is no debounced
    /// auto-save path to distinguish from, so every outcome (including
    /// refusal / I/O failure) always alerts the user.  Returns true on
    /// a successful (or no-op) save, false on refusal/failure.  Callers
    /// must first check loadedFilePath().isEmpty() and route to
    /// performSceneSaveAs() instead -- see onSaveScene().
    bool performSceneSave();

    /// Start-screen create path (spec §5.2): the untitled-scene Save-As
    /// fallback -- onSaveScene() routes here whenever the loaded scene
    /// has no path yet.  Also reachable any time a normal Save-As is
    /// wanted (mirrors macOS RenderViewModel.saveSceneAs()).  Opens a
    /// native picker, then reuses the SAME ViewportBridge::saveSceneTo /
    /// SaveStatus handling as performSceneSave().  On success the scene
    /// gains a path (ViewportBridge::saveSceneTo already re-anchors
    /// RenderEngine::loadedFilePath internally) and joins Recent Scenes
    /// via onSceneSavedToPath(path, /*wasSaveAs=*/true).  Returns true on
    /// a successful (or no-op) save, false on cancel/refusal/failure.
    bool performSceneSaveAs();

    /// The single unsaved-work gate for destructive scene transitions
    /// (Close Scene, load-over) -- mirrors macOS promptToSaveUnsavedWork
    /// (rounds 2+3): both dirt kinds, the both-dirty divergent-text case,
    /// untitled Save-As routing.  True = proceed, false = abort.
    bool promptToSaveUnsavedWork(const QString& action);

    /// Save the raw editor text to a user-chosen path (always dialogs;
    /// never writes to the scene's own file).  False on cancel/IO error.
    bool saveEditorTextViaDialog();

    /// "Reveal in scene file" (item 3): resolve (category, name) via
    /// the bridge, switch to the Scene-file tab, and scroll/select/
    /// flash the resolved line in the SceneEditor.  Mirrors macOS
    /// RenderViewModel.revealEntityInSceneText.  `category` uses the
    /// SAME numbering as SceneEditCategory_* / ViewportBridge::Category
    /// (kept as a raw int here rather than the bridge's nested enum type,
    /// so this header doesn't need to pull in ViewportBridge.h -- matches
    /// every other cross-widget slot in this class; the two call sites in
    /// MainWindow.cpp adapt via a small lambda).  Switches tabs even when
    /// the SceneEditor buffer has unsaved edits (the stale-buffer warning
    /// is already showing then) but SKIPS the scroll in that case -- the
    /// offset addresses the LIVE text and could land on the wrong line.
    /// A no-op (no tab switch, no scroll) when there's no bridge, the
    /// scene isn't editable (canUseSceneTransport() gate, same as every
    /// other serializedSceneText()-family call), or the entity doesn't
    /// resolve.
    void revealEntityInSceneText(int category, const QString& name);

    /// Source traceability (param + Environment granularity): resolve a
    /// specific UI element's EXACT scene-file span via
    /// ViewportBridge::resolveSourceSpan, switch to the Scene-file tab, and
    /// select/flash [offset, offset+length) in the SceneEditor.  `param`
    /// empty falls back to a whole-chunk (line) reveal.  Same
    /// canUseSceneTransport() gate + stale-buffer (isDirty) skip as
    /// revealEntityInSceneText -- a reveal must be right or absent, never
    /// misleading.  `category` uses the SAME raw-int convention as
    /// revealEntityInSceneText above (ViewportBridge::Category numbering).
    void revealSourceSpan(int category, const QString& name,
                           const QString& param, int occurrence);

    /// Reverse source traceability (text -> UI select): resolve the scene
    /// entity whose source contains UTF-8 `byteOffset` (a right-click in the
    /// SceneEditor) via ViewportBridge::sourceRefAtByteOffset, then select it
    /// so the outliner + properties panel land on it.  Same
    /// canUseSceneTransport() gate as the forward reveals; additionally
    /// requires !m_sceneEditor->isDirty() because the offset comes from the
    /// buffer but resolves against the live CST serialization -- the two only
    /// agree when the buffer is clean.  A no-resolve (inter-chunk trivia, a
    /// non-entity chunk, a dirty buffer) is a silent no-op.
    void selectEntityAtByteOffset(quint64 byteOffset);

    // Entity creation + painter CRUD (entity-creation slice) -- mirrors
    // macOS RenderViewModel.addEntity / duplicateSelectedOrNamed /
    // removeEntity.  `category` uses the SAME raw-int convention as
    // revealEntityInSceneText above (ViewportBridge::Category numbering)
    // so this header stays free of the bridge's nested enum type; the
    // Insert menu and the outliner's "+"/context-menu signals all adapt
    // via a small lambda at their connect site.  All three are gated on
    // canUseSceneTransport() (re-checked AFTER any confirm dialog, since
    // a chat-driven render can start or finish while a modal is up) and
    // surface the core's honest refusal message via QMessageBox on
    // failure.  Success re-selects the affected entity (add/duplicate)
    // so the properties panel follows it; the outliner's own refresh is
    // NOT poked explicitly here -- the core bumps the controller's
    // sceneEpoch on every landed chunk CRUD, and OutlinerWidget::refresh()
    // already runs on every ViewportBridge::imageUpdated frame, so the
    // epoch-gated re-pull picks up the change on its own (no per-platform
    // refresh workaround needed, matching the design brief).
    void addEntity(int category, unsigned int templateIndex);
    void duplicateEntity(int category, const QString& name);
    void removeEntity(int category, const QString& name);

    // UI refinement item 3: File > Theme handler.  No-op if `newMode`
    // is already active (also guards the redundant re-click of an
    // already-checked radio item in the File > Theme submenu).
    void onThemeModeChanged(Theme::ThemeMode newMode);

    // Recent files
    QStringList m_recentFiles;
    // Start-screen spec §5.4: SIBLING map to m_recentFiles, path -> last-
    // opened epoch seconds, for the start screen's "2m ago" labels.
    // Deliberately a separate QSettings key ("recentSceneMeta") from the
    // shared "recentSceneFiles" path list, so old installs and a path
    // added before this feature simply render without a time.
    QMap<QString, qint64> m_recentFilesMeta;
    QMenu* m_recentFilesMenu = nullptr;
    static constexpr int MAX_RECENT_FILES = 10;

    RenderEngine* m_engine = nullptr;
    RenderWidget* m_renderWidget = nullptr;
    HDRRenderWidget* m_hdrRenderWidget = nullptr;  // L5b — Windows HDR display
    QStackedWidget* m_productionPaneStack = nullptr;  // SDR / HDR within production
    // Start screen (docs/gui/START_SCREEN.md): the no-scene launch
    // surface, persistent like TopBar/ChatPanel -- built once, shown/
    // hidden by updateCenterViewStack() rather than recreated per scene
    // load/close.
    StartWidget*    m_startWidget = nullptr;
    QAction*        m_hdrToggleAction = nullptr;       // View > HDR Preview
    QAction*        m_saveImageAction = nullptr;       // L5d — File > Save Rendered Image…
    QMenu*          m_toneCurveMenu = nullptr;         // L5e — View > Tone Curve
    QActionGroup*   m_toneCurveGroup = nullptr;        // L5e — exclusive group for tone-curve radio
    // View > Left Panel toggle -- promoted from a createMenuBar()-local
    // variable so the start screen's Create path (which force-shows the
    // left panel to reveal the Agent tab, spec §4.3 step 4) can keep this
    // checkbox in sync; otherwise the next click would incorrectly HIDE
    // a panel the action itself believes is still closed.
    QAction*        m_leftPanelAction = nullptr;

    // UI redesign: menu actions whose enable state / label text is
    // driven live by updateMenuActionStates() (see its doc above).
    QAction* m_saveSceneAction = nullptr;
    QAction* m_closeSceneAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_pauseResumeAction = nullptr;
    QAction* m_restartAction = nullptr;
    QAction* m_drawRegionAction = nullptr;
    QAction* m_renderAction = nullptr;
    QAction* m_renderRegionAction = nullptr;
    QAction* m_renderAnimAction = nullptr;
    QAction* m_cancelAction = nullptr;

    TopBar*      m_topBar = nullptr;
    ChatPanel*   m_chatPanel = nullptr;
    LogWidget*   m_logWidget = nullptr;
    SceneEditor* m_sceneEditor = nullptr;

    // Left panel: slim Agent / Scene-file tab strip over a QStackedWidget.
    // User-resizable (wider, resizable left panel refinement): m_leftPanel
    // + the center column live inside m_leftSplitter (a QSplitter) so the
    // user can drag the boundary; m_leftSplitter's own size is what
    // buildLeftPanel/the constructor persist to QSettings ("leftPanelWidth"),
    // not m_leftPanel's fixed width (there isn't one anymore).
    QWidget*      m_leftPanel = nullptr;
    QSplitter*    m_leftSplitter = nullptr;
    QStackedWidget* m_leftPanelStack = nullptr;
    QToolButton*  m_agentTabBtn = nullptr;
    QToolButton*  m_sceneTabBtn = nullptr;
    // Agent/Scene-file tab strip container -- stored (rather than kept
    // as a buildLeftPanel()-local) so restyleTheme() can re-apply its
    // border-bottom stylesheet on a live theme switch.
    QWidget*      m_leftTabStrip = nullptr;

    // Right panel: fixed-width host for the outliner (persistent, built
    // once) over the per-scene ViewportProperties (rebuilt per scene).
    QWidget*         m_rightPanel = nullptr;
    QVBoxLayout*     m_rightPanelLayout = nullptr;
    OutlinerWidget*  m_outlinerWidget = nullptr;
    // Environment / IBL section, persistent (built once, like the
    // outliner) between the outliner and the per-scene ViewportProperties.
    // Shows nothing until setBridge() gives it a live scene; hides itself
    // entirely when the scene has no active rasterizer.
    EnvironmentPanel* m_environmentPanel = nullptr;

    // Center column: persistent host for the view stack / timeline /
    // log drawer.  m_viewportTimeline is inserted/removed at index 1
    // by rebuildViewportForLoadedScene / teardownViewport (it's
    // created/destroyed alongside the per-scene viewport bridge).
    QVBoxLayout* m_centerColumnLayout = nullptr;
    QWidget*     m_logDrawerContainer = nullptr;
    // Center column container itself -- stored (rather than kept as a
    // constructor-local) so restyleTheme() can re-apply its
    // QPalette::Window (bgCenter) fill on a live theme switch.
    QWidget*     m_centerColumn = nullptr;


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
