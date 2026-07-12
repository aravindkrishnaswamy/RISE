//////////////////////////////////////////////////////////////////////
//
//  MainWindow.cpp - Main window implementation.
//
//  UI redesign — mirrors the macOS app's ContentView.swift +
//  RISEApp.swift.  See MainWindow.h for the layout / rehousing map.
//
//////////////////////////////////////////////////////////////////////

#include "MainWindow.h"
#include "RenderEngine.h"
#include "RenderWidget.h"
#include "HDRRenderWidget.h"
#include "TopBar.h"
#include "ChatPanel.h"
#include "LogWidget.h"
#include "SceneEditor.h"
#include "ViewportBridge.h"
#include "ViewportWidget.h"
#include "ViewportToolbar.h"
#include "ViewportTimeline.h"
#include "ViewportProperties.h"
#include "OutlinerWidget.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QToolButton>

#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QLabel>
#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QPalette>
#include <QSlider>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>

// CST <-> scene-file live sync + auto-save (item 1/2): eLog_Warning for
// the auto-save-refused/failed log line in performSceneSave.
#include "Interfaces/ILog.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    resize(1440, 900);
    setMinimumSize(1320, 760);

    // Load recent files from settings
    QSettings settings;
    m_recentFiles = settings.value("recentSceneFiles").toStringList();

    // Create the engine
    m_engine = new RenderEngine(this);

    updateWindowTitle();

    // Create widgets
    m_renderWidget = new RenderWidget();
    m_hdrRenderWidget = new HDRRenderWidget();  // L5b — Windows HDR

    // L5b — production-pane sub-stack: SDR widget at idx 0,
    // HDR widget at idx 1.  The View > HDR Preview action flips
    // between them.  Both connected to the engine; only the
    // currently-active one receives updates (the engine emits
    // either `imageUpdated` or `hdrImageUpdated` depending on its
    // own HDR-mode flag, which `onHDRToggled` keeps in sync with
    // the stack's current index).
    m_productionPaneStack = new QStackedWidget();
    m_productionPaneStack->addWidget(m_renderWidget);     // index 0 — SDR
    m_productionPaneStack->addWidget(m_hdrRenderWidget);  // index 1 — HDR
    m_productionPaneStack->setCurrentIndex(0);

    // Stacked widget toggles between the passive render view and the
    // interactive viewport pane.  The viewport pane is built lazily
    // when a scene loads (see rebuildViewportForLoadedScene).
    m_viewStack = new QStackedWidget();
    m_viewStack->addWidget(m_productionPaneStack);   // index 0

    m_topBar = new TopBar();
    m_topBar->setEngine(m_engine);

    m_chatPanel = new ChatPanel();
    m_logWidget = new LogWidget();
    m_sceneEditor = new SceneEditor();

    // ---- Central widget: TopBar over a 3-column workspace -----------
    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_topBar);

    auto* mainArea = new QWidget(central);
    auto* mainAreaLayout = new QHBoxLayout(mainArea);
    mainAreaLayout->setContentsMargins(0, 0, 0, 0);
    mainAreaLayout->setSpacing(0);

    m_leftPanel = buildLeftPanel();
    mainAreaLayout->addWidget(m_leftPanel);

    QWidget* centerColumn = buildCenterColumn();
    mainAreaLayout->addWidget(centerColumn, 1);

    m_rightPanel = buildRightPanel();
    mainAreaLayout->addWidget(m_rightPanel);

    centralLayout->addWidget(mainArea, 1);
    setCentralWidget(central);

    createMenuBar();
    createStatusBar();

    // Connect engine signals
    connect(m_engine, &RenderEngine::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_engine, &RenderEngine::progressUpdated, m_renderWidget, [this](double fraction, const QString&) {
        m_renderWidget->setProgress(fraction);
    });
    connect(m_engine, &RenderEngine::imageUpdated, m_renderWidget, &RenderWidget::updateImage);
    // L5b — HDR signal routes to the HDR widget.  Both are wired
    // unconditionally; the engine emits one or the other based on
    // its mode flag.
    connect(m_engine, &RenderEngine::hdrImageUpdated,
            m_hdrRenderWidget, &HDRRenderWidget::updateHDRImage);
    connect(m_hdrRenderWidget, &HDRRenderWidget::hdrAvailabilityChanged,
            this, &MainWindow::onHDRAvailabilityChanged);
    connect(m_engine, &RenderEngine::sceneSizeDetected, this, &MainWindow::onSceneSizeDetected);
    connect(m_engine, &RenderEngine::logMessage, m_logWidget, &LogWidget::appendLog);
    // UI redesign: elapsed/remaining now surface via the TopBar
    // readout's tooltip (see TopBar::updateReadoutTooltip) instead of
    // the retired ControlsWidget's dedicated labels.
    connect(m_engine, &RenderEngine::elapsedTimeUpdated, m_topBar, &TopBar::updateElapsedTime);
    connect(m_engine, &RenderEngine::remainingTimeUpdated, m_topBar, &TopBar::updateRemainingTime);
    connect(m_engine, &RenderEngine::errorOccurred, this, [this](const QString& msg) {
        statusBar()->showMessage("Error: " + msg, 5000);
    });

    connect(m_topBar, &TopBar::saveClicked, this, &MainWindow::onSaveScene);

    // P1-2 fix (mirrors Mac's canUseSceneTransport): while a chat-driven
    // `render` tool call owns the scene, undo/redo and refinement
    // transport must go stale-free even though nothing routes through
    // RenderEngine::stateChanged for that transition -- it's entirely
    // internal to ChatPanel's async poll timer.  Refresh both the menu
    // actions and TopBar's own control-enable state at every transition.
    m_topBar->setChatPanel(m_chatPanel);
    connect(m_chatPanel, &ChatPanel::chatRenderOutstandingChanged, this, &MainWindow::updateMenuActionStates);
    connect(m_chatPanel, &ChatPanel::chatRenderOutstandingChanged, m_topBar, &TopBar::onChatRenderOutstandingChanged);

    // CST <-> scene-file live sync + auto-save (item 1/2).  ~2 Hz, same
    // cadence as TopBar's own refinement-status poll.  Started/stopped
    // alongside the viewport bridge -- see rebuildViewportForLoadedScene
    // / teardownViewport.
    m_cstSyncTimer = new QTimer(this);
    m_cstSyncTimer->setInterval(500);
    connect(m_cstSyncTimer, &QTimer::timeout, this, &MainWindow::onCstSyncTick);

    // Connect editor signals.  UI redesign: the Scene file tab has no
    // "close" concept of its own (the tab strip always offers both
    // tabs) — the editor's inline "X" button now just flips back to
    // the Agent tab instead of hiding a splitter pane.
    connect(m_sceneEditor, &SceneEditor::closeRequested, this, [this]() { setLeftTab(0); });
    connect(m_sceneEditor, &SceneEditor::saveAndReloadRequested, this, &MainWindow::onSaveAndReload);

    // Set initial status
    statusBar()->showMessage(QString("RISE %1 — Ready").arg(m_engine->versionString()));

    // L5b — initial HDR-availability probe.  HDRRenderWidget is at
    // index 1 of the production sub-stack and so won't fire its own
    // Show event until the toggle is flipped — chicken-and-egg.
    // The static probe enumerates all DXGI outputs without needing
    // a swap chain.
    onHDRAvailabilityChanged(HDRRenderWidget::probeAnyAdapterHDRAvailable());
}

// ============================================================
// Workspace chrome construction (UI redesign)
// ============================================================

QWidget* MainWindow::buildLeftPanel()
{
    auto* panel = new QWidget();
    panel->setFixedWidth(404);
    panel->setAutoFillBackground(true);
    {
        QPalette pal = panel->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        panel->setPalette(pal);
    }
    panel->setStyleSheet(QStringLiteral("border-right: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Slim tab strip: "Agent" / "Scene file", spectral-underline on
    // the active tab.  Mirrors ContentView.swift's leftPanelTabStrip.
    auto* tabStrip = new QWidget(panel);
    tabStrip->setFixedHeight(42);
    tabStrip->setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    auto* tabStripLayout = new QHBoxLayout(tabStrip);
    tabStripLayout->setContentsMargins(15, 0, 15, 0);
    tabStripLayout->setSpacing(18);

    auto makeTabButton = [tabStrip](const QString& label) {
        auto* btn = new QToolButton(tabStrip);
        btn->setText(label);
        btn->setCheckable(true);
        btn->setAutoRaise(true);
        btn->setFixedHeight(40);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return btn;
    };

    m_agentTabBtn = makeTabButton(QStringLiteral("Agent"));
    m_sceneTabBtn = makeTabButton(QStringLiteral("Scene file"));

    auto* tabGroup = new QButtonGroup(tabStrip);
    tabGroup->setExclusive(true);
    tabGroup->addButton(m_agentTabBtn);
    tabGroup->addButton(m_sceneTabBtn);

    tabStripLayout->addWidget(m_agentTabBtn);
    tabStripLayout->addWidget(m_sceneTabBtn);
    tabStripLayout->addStretch(1);
    layout->addWidget(tabStrip);

    m_leftPanelStack = new QStackedWidget(panel);
    m_leftPanelStack->addWidget(m_chatPanel);     // index 0 — Agent
    m_leftPanelStack->addWidget(m_sceneEditor);   // index 1 — Scene file
    layout->addWidget(m_leftPanelStack, 1);

    connect(m_agentTabBtn, &QToolButton::clicked, this, [this]() { setLeftTab(0); });
    connect(m_sceneTabBtn, &QToolButton::clicked, this, [this]() { setLeftTab(1); });

    setLeftTab(0);   // Agent tab is active on launch

    return panel;
}

void MainWindow::setLeftTab(int index)
{
    if (!m_leftPanelStack) return;
    m_leftPanelStack->setCurrentIndex(index);

    const bool agentActive = (index == 0);
    m_agentTabBtn->setChecked(agentActive);
    m_sceneTabBtn->setChecked(!agentActive);
    m_agentTabBtn->setFont(Theme::sans(12, agentActive ? QFont::DemiBold : QFont::Normal));
    m_sceneTabBtn->setFont(Theme::sans(12, !agentActive ? QFont::DemiBold : QFont::Normal));

    const QString activeStyle = QStringLiteral(
        "QToolButton { color: %1; border: none; border-bottom: 2px solid %2; background: transparent; }")
        .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::accent));
    const QString inactiveStyle = QStringLiteral(
        "QToolButton { color: %1; border: none; border-bottom: 2px solid transparent; background: transparent; }")
        .arg(Theme::hex(Theme::textFaint));
    m_agentTabBtn->setStyleSheet(agentActive ? activeStyle : inactiveStyle);
    m_sceneTabBtn->setStyleSheet(!agentActive ? activeStyle : inactiveStyle);

    // Matches the pre-redesign Edit-toggle behavior: switching into
    // the Scene file tab (re-)loads the current file from disk.
    if (!agentActive && m_engine && !m_engine->loadedFilePath().isEmpty()) {
        m_sceneEditor->loadFile(m_engine->loadedFilePath());
    }
}

QWidget* MainWindow::buildCenterColumn()
{
    auto* col = new QWidget();
    col->setAutoFillBackground(true);
    {
        QPalette pal = col->palette();
        pal.setColor(QPalette::Window, Theme::bgCenter);
        col->setPalette(pal);
    }

    m_centerColumnLayout = new QVBoxLayout(col);
    m_centerColumnLayout->setContentsMargins(0, 0, 0, 0);
    m_centerColumnLayout->setSpacing(0);
    m_centerColumnLayout->addWidget(m_viewStack, 1);   // index 0 — grows
    // index 1 is reserved for m_viewportTimeline, inserted/removed by
    // rebuildViewportForLoadedScene / teardownViewport alongside the
    // per-scene viewport bridge.

    m_logDrawerContainer = buildLogDrawer();
    m_centerColumnLayout->addWidget(m_logDrawerContainer);   // index 1 (or 2 once timeline is present)

    return col;
}

QWidget* MainWindow::buildLogDrawer()
{
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 4, 10, 8);
    layout->setSpacing(4);

    // UI redesign slice B: the exposure slider moved to the viewport
    // toolbar's EV chip popup (ViewportToolbar::exposureSlider()) --
    // wired fresh in rebuildViewportForLoadedScene each time a scene
    // loads.  See MainWindow.h's header doc.
    layout->addWidget(m_logWidget, 1);

    return container;
}

QWidget* MainWindow::buildRightPanel()
{
    auto* panel = new QWidget();
    panel->setFixedWidth(344);
    panel->setAutoFillBackground(true);
    {
        QPalette pal = panel->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        panel->setPalette(pal);
    }
    panel->setStyleSheet(QStringLiteral("border-left: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    m_rightPanelLayout = new QVBoxLayout(panel);
    m_rightPanelLayout->setContentsMargins(0, 0, 0, 0);
    m_rightPanelLayout->setSpacing(0);

    // OutlinerWidget is persistent (built once, like TopBar/ChatPanel) --
    // it just shows nothing until setBridge() gives it a live scene.
    // ViewportProperties (below it) is per-scene: rebuildViewportForLoadedScene
    // inserts a fresh instance; teardownViewport removes it.  See
    // MainWindow.h's header doc.
    m_outlinerWidget = new OutlinerWidget(panel);
    m_rightPanelLayout->addWidget(m_outlinerWidget);

    return panel;
}

void MainWindow::onExposureSliderChanged(int value)
{
    // UI redesign slice B: the label lives inside ViewportToolbar's EV
    // chip now (self-updating from the same valueChanged signal) --
    // this slot's only remaining job is the RenderEngine business logic.
    const double ev = value / 10.0;
    if (m_engine) m_engine->setViewExposureEV(ev);
}

void MainWindow::onExposureResetRequested()
{
    if (m_viewportToolbar && m_viewportToolbar->exposureSlider()) {
        m_viewportToolbar->exposureSlider()->setValue(0);
    }
}

void MainWindow::createMenuBar()
{
    // --- File menu ---
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* openAction = fileMenu->addAction("&Open Scene...", this, &MainWindow::onOpenScene);
    openAction->setShortcut(QKeySequence::Open);

    // Open Recent submenu
    m_recentFilesMenu = fileMenu->addMenu("Open &Recent");
    updateRecentFilesMenu();

    fileMenu->addSeparator();

    // L5d — Save Rendered Image action (parity with macOS File >
    // Save Rendered Image…).  Disabled until the engine has at
    // least started a render; remains enabled across the rest of
    // the app's lifetime so the user can save the LAST rendered
    // result even after a cancel or scene reload's Render click.
    m_saveImageAction = fileMenu->addAction("Save Rendered &Image...",
        this, &MainWindow::onSaveRenderedImage);
    m_saveImageAction->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_saveImageAction->setEnabled(false);  // toggled by onStateChanged

    fileMenu->addSeparator();

    // UI redesign: "Save Scene" now performs the interactive
    // round-trip save via ViewportBridge (Phase 6.5) — the same
    // action as the TopBar's Save pill — rather than the raw
    // scene-editor text save.  The Scene file tab's own inline
    // Save / Revert / Save & Reload buttons still cover that lower-
    // level text-file editing workflow, unaffected by this item.
    m_saveSceneAction = fileMenu->addAction("&Save Scene", this, &MainWindow::onSaveScene);
    m_saveSceneAction->setShortcut(QKeySequence("Ctrl+Alt+S"));
    m_saveSceneAction->setEnabled(false);

    // Rehouses the retired ControlsWidget's "Clear" button.
    m_closeSceneAction = fileMenu->addAction("&Close Scene", this, &MainWindow::onClear);
    m_closeSceneAction->setShortcut(QKeySequence("Ctrl+Shift+W"));
    m_closeSceneAction->setEnabled(false);

    connect(fileMenu, &QMenu::aboutToShow, this, &MainWindow::updateMenuActionStates);

    fileMenu->addSeparator();

    // UI refinement item 3: light/dark theme switch (File > Theme).
    // Theme::setMode persists the choice via QSettings, reassigns every
    // Theme:: token global, and re-applies the QPalette + app font to
    // the running QApplication live -- see onThemeModeChanged for the
    // "some panels update after restarting" honest-gap notice this
    // mechanism implies (Theme.cpp's doc has the full inventory of
    // widgets that bake colors into a setStyleSheet string at
    // construction time and so don't repaint from this alone).
    auto* themeMenu = fileMenu->addMenu("&Theme");
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    auto* darkThemeAction = themeMenu->addAction("&Dark");
    darkThemeAction->setCheckable(true);
    auto* lightThemeAction = themeMenu->addAction("&Light");
    lightThemeAction->setCheckable(true);
    themeGroup->addAction(darkThemeAction);
    themeGroup->addAction(lightThemeAction);
    darkThemeAction->setChecked(Theme::mode() == Theme::ThemeMode::Dark);
    lightThemeAction->setChecked(Theme::mode() == Theme::ThemeMode::Light);
    connect(darkThemeAction, &QAction::triggered, this, [this]() { onThemeModeChanged(Theme::ThemeMode::Dark); });
    connect(lightThemeAction, &QAction::triggered, this, [this]() { onThemeModeChanged(Theme::ThemeMode::Light); });

    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction("E&xit", this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    // --- Edit menu ---
    // UI redesign: the retired ControlsWidget exposed Undo/Redo
    // nowhere explicit (only via the interactive viewport's own key
    // handling); the redesign surfaces them as first-class Edit-menu
    // items with the C++ controller's human-readable step label.
    auto* editMenu = menuBar()->addMenu("&Edit");
    m_undoAction = editMenu->addAction("Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this]() {
        if (!canUseSceneTransport()) return;
        m_viewportBridge->undo();
    });

    m_redoAction = editMenu->addAction("Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, [this]() {
        if (!canUseSceneTransport()) return;
        m_viewportBridge->redo();
    });

    editMenu->addSeparator();
    auto* findAction = editMenu->addAction("&Find...");
    findAction->setShortcut(QKeySequence::Find);

    connect(editMenu, &QMenu::aboutToShow, this, &MainWindow::updateMenuActionStates);

    // --- View menu ---
    // L5b — adds the HDR Preview toggle.  Disabled by default; the
    // HDRRenderWidget enables it once it confirms the active monitor
    // reports an HDR colorspace + max luminance > SDR.
    auto* viewMenu = menuBar()->addMenu("&View");
    m_hdrToggleAction = viewMenu->addAction("&HDR Preview");
    m_hdrToggleAction->setCheckable(true);
    m_hdrToggleAction->setChecked(false);
    m_hdrToggleAction->setEnabled(false);  // enabled by onHDRAvailabilityChanged
    connect(m_hdrToggleAction, &QAction::toggled,
            this, &MainWindow::onHDRToggled);

    // L5e — Tone Curve submenu.  Mirror of the macOS
    // View > Tone Curve picker.  Exclusive QActionGroup gives the
    // submenu radio-button semantics; engine default = 2 (ACES).
    // Greyed out when HDR Preview is on.
    auto* toneCurveMenu = viewMenu->addMenu("&Tone Curve");
    m_toneCurveGroup = new QActionGroup(this);
    m_toneCurveGroup->setExclusive(true);
    struct ToneCurveEntry { const char* label; int curve; };
    const ToneCurveEntry entries[] = {
        { "&None",      0 },
        { "&Reinhard",  1 },
        { "&ACES",      2 },
        { "Ag&X",       3 },
        { "&Hable",     4 },
    };
    for (const auto& e : entries) {
        auto* a = toneCurveMenu->addAction(e.label);
        a->setCheckable(true);
        a->setChecked(e.curve == 2);  // ACES default
        a->setData(e.curve);
        m_toneCurveGroup->addAction(a);
    }
    connect(m_toneCurveGroup, &QActionGroup::triggered,
            this, [this](QAction* a) {
                if (m_engine) m_engine->setViewToneCurve(a->data().toInt());
            });
    m_toneCurveMenu = toneCurveMenu;

    // UI redesign: show/hide the left panel (Agent / Scene file tabs)
    // and the bottom log drawer.  Both default on; toggling doesn't
    // persist across launches in slice 1.
    viewMenu->addSeparator();
    auto* leftPanelAction = viewMenu->addAction("Left Panel");
    leftPanelAction->setCheckable(true);
    leftPanelAction->setChecked(true);
    connect(leftPanelAction, &QAction::toggled, this, [this](bool on) {
        if (m_leftPanel) m_leftPanel->setVisible(on);
    });

    auto* logAction = viewMenu->addAction("Log");
    logAction->setCheckable(true);
    logAction->setChecked(true);
    logAction->setShortcut(QKeySequence("Alt+L"));
    connect(logAction, &QAction::toggled, this, [this](bool on) {
        if (m_logDrawerContainer) m_logDrawerContainer->setVisible(on);
    });

    // --- Render menu ---
    // UI redesign (design brief A2): refinement transport + production
    // render, rehousing the retired ControlsWidget's Render / Render
    // Animation / Cancel buttons.
    auto* renderMenu = menuBar()->addMenu("&Render");

    m_pauseResumeAction = renderMenu->addAction("Pause Refinement");
    m_pauseResumeAction->setShortcut(QKeySequence("Ctrl+Space"));
    connect(m_pauseResumeAction, &QAction::triggered, this, [this]() {
        // Item 4: while a production render is in flight, this item
        // pauses/resumes THE RENDER instead of interactive refinement --
        // mirrors the TopBar pause button's dual mode (TopBar::
        // onPauseResumeClicked) and macOS RISEApp.swift's pauseMenuTitle
        // / Render-menu Button.  Always available then (guarded only by
        // updateMenuActionStates keeping it disabled while Cancelling).
        if (m_engine->state() == RenderEngine::Rendering) {
            m_engine->setProductionRenderPaused(!m_engine->isProductionRenderPaused());
            updateMenuActionStates();
            return;
        }
        if (!canUseSceneTransport()) return;
        if (m_viewportBridge->isRefinementPaused()) m_viewportBridge->resumeRefinement();
        else m_viewportBridge->pauseRefinement();
        updateMenuActionStates();
    });

    m_restartAction = renderMenu->addAction("Restart Refinement");
    connect(m_restartAction, &QAction::triggered, this, [this]() {
        if (!canUseSceneTransport()) return;
        m_viewportBridge->stop();
        m_viewportBridge->start();
    });

    renderMenu->addSeparator();

    m_renderAction = renderMenu->addAction("&Render", this, &MainWindow::onRender);
    m_renderAction->setShortcut(QKeySequence("Ctrl+R"));

    m_renderAnimAction = renderMenu->addAction("Render &Animation", this, &MainWindow::onRenderAnimation);

    m_cancelAction = renderMenu->addAction("&Cancel", this, &MainWindow::onCancel);
    m_cancelAction->setShortcut(QKeySequence("Ctrl+."));

    connect(renderMenu, &QMenu::aboutToShow, this, &MainWindow::updateMenuActionStates);

    // Initial sync — gives every action above its correct
    // enabled/disabled state (and label) from the moment the window
    // opens, independent of whether the user has ever opened a menu.
    updateMenuActionStates();
}

bool MainWindow::canUseSceneTransport() const
{
    if (!m_viewportBridge) return false;
    const auto state = m_engine->state();
    const bool productionActive = (state == RenderEngine::Rendering
                                || state == RenderEngine::Cancelling
                                || state == RenderEngine::Loading);
    const bool chatOutstanding = m_chatPanel && m_chatPanel->isChatRenderOutstanding();
    return !productionActive && !chatOutstanding;
}

void MainWindow::updateMenuActionStates()
{
    const auto state = m_engine->state();
    const bool interacting = (state != RenderEngine::Rendering
                           && state != RenderEngine::Cancelling
                           && state != RenderEngine::Loading);

    if (m_saveSceneAction) {
        // CST <-> scene-file live sync + auto-save (item 1/2): manual
        // escape hatch, so enabled for both "there's something unsaved"
        // (though edits now auto-save on a debounce, this covers the
        // window before the next auto-save tick fires) AND
        // `m_autoSaveSuspended` (the file changed on disk externally
        // and auto-save backed off) -- clicking is exactly a manual
        // retry in that case.  Mirrors macOS RISEApp.swift's "Save
        // Scene" menu item gate.
        m_saveSceneAction->setEnabled(
            (m_viewportBridge && m_viewportBridge->hasUnsavedSceneChanges())
            || m_autoSaveSuspended);
    }
    if (m_closeSceneAction) {
        m_closeSceneAction->setEnabled(interacting && state != RenderEngine::Idle);
    }

    // P1-2 fix (mirrors Mac's canUseSceneTransport / TopBar.swift:103-113):
    // a chat-driven `render` tool call owns the scene for the duration of
    // its async job just as surely as a production render does -- undo/
    // redo and refinement transport must not run underneath it.  Scoped
    // to just these two clauses (NOT into the broader `interacting` term
    // above, which also gates viewport/outliner/properties enable state
    // that has no scene-transport hazard) to mirror the Mac split.
    const bool chatRenderOutstanding = m_chatPanel && m_chatPanel->isChatRenderOutstanding();

    const bool bridgeInteractingEnabled = (m_viewportBridge != nullptr) && interacting && !chatRenderOutstanding;
    if (m_undoAction) {
        const QString label = m_viewportBridge ? m_viewportBridge->undoActionLabel() : QString();
        m_undoAction->setText(label.isEmpty() ? QStringLiteral("Undo") : QStringLiteral("Undo %1").arg(label));
        m_undoAction->setEnabled(bridgeInteractingEnabled);
    }
    if (m_redoAction) {
        const QString label = m_viewportBridge ? m_viewportBridge->redoActionLabel() : QString();
        m_redoAction->setText(label.isEmpty() ? QStringLiteral("Redo") : QStringLiteral("Redo %1").arg(label));
        m_redoAction->setEnabled(bridgeInteractingEnabled);
    }
    // Round-2 P1: the viewport toolbar's undo/redo buttons are a second
    // affordance for the same scene-transport path -- gate them with the
    // IDENTICAL term so the two can never drift (the toolbar's broader
    // setEnabled(interacting) alone misses the chat-render clause).
    if (m_viewportToolbar) m_viewportToolbar->setUndoRedoEnabled(bridgeInteractingEnabled);

    const bool refinementDisabled = !m_viewportBridge
        || state == RenderEngine::Rendering
        || state == RenderEngine::Cancelling
        || chatRenderOutstanding;
    if (m_pauseResumeAction) {
        // Item 4 (mirrors macOS RISEApp.swift's pauseMenuTitle /
        // Button.disabled): while a production render is in flight,
        // this item always stays enabled (except while Cancelling,
        // still covered by `state == RenderEngine::Cancelling` below
        // since `refinementDisabled` folds that in) and its label/
        // action target THE RENDER instead of interactive refinement.
        if (state == RenderEngine::Rendering) {
            m_pauseResumeAction->setEnabled(true);
            m_pauseResumeAction->setText(
                m_engine->isProductionRenderPaused() ? "Resume Render" : "Pause Render");
        } else if (state == RenderEngine::Cancelling) {
            m_pauseResumeAction->setEnabled(false);
            m_pauseResumeAction->setText("Pause Render");
        } else {
            m_pauseResumeAction->setEnabled(!refinementDisabled);
            m_pauseResumeAction->setText(
                (m_viewportBridge && m_viewportBridge->isRefinementPaused())
                    ? "Resume Refinement" : "Pause Refinement");
        }
    }
    if (m_restartAction) {
        m_restartAction->setEnabled(!refinementDisabled);
    }

    const bool canRender = (state == RenderEngine::SceneLoaded
                         || state == RenderEngine::Completed
                         || state == RenderEngine::Cancelled);
    if (m_renderAction)     m_renderAction->setEnabled(canRender);
    const bool canRenderAnim = canRender && m_engine->hasAnimation();
    if (m_renderAnimAction) m_renderAnimAction->setEnabled(canRenderAnim);
    if (m_viewportTimeline) m_viewportTimeline->setRenderMovieEnabled(canRenderAnim);
    if (m_cancelAction)     m_cancelAction->setEnabled(state == RenderEngine::Rendering);
}

void MainWindow::updateEvEnabledState()
{
    if (!m_viewportToolbar) return;
    const bool notIdle = m_engine && m_engine->state() != RenderEngine::Idle;
    const bool hdrOff = !(m_hdrToggleAction && m_hdrToggleAction->isChecked());
    m_viewportToolbar->setEvEnabled(notIdle && hdrOff);
}

// ============================================================
// L5b — HDR toggle handlers
// ============================================================

void MainWindow::onHDRToggled(bool checked)
{
    // Engine flag drives the encode path (RGBA16F vs RGBA8 +
    // ForHDRDisplay vs ForLDRDisplay).  ProductionPaneStack flips
    // which widget is visible.  Order matters slightly: flip the
    // engine first so the immediate Repaint inside setHDREnabled
    // emits to the widget that's about to become visible.
    if (m_engine) m_engine->setHDREnabled(checked);
    if (m_productionPaneStack) {
        m_productionPaneStack->setCurrentIndex(checked ? 1 : 0);
    }
    // L5e — Tone curve is irrelevant on the HDR display path
    // (the OS compositor handles the display map).  Grey out
    // the submenu when HDR is on.
    if (m_toneCurveMenu) {
        m_toneCurveMenu->setEnabled(!checked);
    }
    // L5e round-2 / P2 fix — Same gate for the exposure slider (now the
    // viewport toolbar's EV chip), now combined with the "not idle"
    // clause (updateEvEnabledState) instead of just the HDR interlock.
    // Applying exposure on top of the HDR compositor's dynamic-range
    // map double-maps the radiance signal — flicker / hue shifts on
    // HDR-capable monitors.
    updateEvEnabledState();
    // UI redesign: the EDR chip mirrors this same action's checked
    // state -- keep it in sync regardless of which affordance the user
    // drove the toggle from (menu item vs chip).
    if (m_viewportToolbar) {
        m_viewportToolbar->setEdrChecked(checked);
    }
}

bool MainWindow::event(QEvent* ev)
{
    // L5b — top-level screen change (user dragged the window
    // between monitors).  Re-run the DXGI probe so the toggle
    // reflects the new active monitor's HDR capability.  The
    // probe is cheap (factory enumeration, no swap chain).
    if (ev->type() == QEvent::ScreenChangeInternal) {
        onHDRAvailabilityChanged(HDRRenderWidget::probeAnyAdapterHDRAvailable());
    }
    return QMainWindow::event(ev);
}

// ============================================================
// L5d — Save Rendered Image
// ============================================================

void MainWindow::onSaveRenderedImage()
{
    if (!m_engine) return;

    // QFileDialog filter syntax: "Description (*.ext);;Description (*.ext)"
    // The first filter is the default (HDR archival EXR).  PNG and
    // TIFF cover the LDR side; users who need HDR / RGBEA / TGA /
    // PPM can still type the extension and the engine's
    // extension-aware lookup will find the right encoder.
    const QString filter =
        "OpenEXR HDR Image (*.exr);;"
        "PNG Image (*.png);;"
        "TIFF Image (*.tif *.tiff);;"
        "Radiance HDR (*.hdr);;"
        "All Files (*)";

    // Default filename: scene basename + ".exr".  loadedFilePath
    // may contain a full path; strip directories and the
    // .RISEscene extension.
    QString defaultName = "rendered.exr";
    const QString scenePath = m_engine->loadedFilePath();
    if (!scenePath.isEmpty()) {
        QFileInfo info(scenePath);
        defaultName = info.completeBaseName() + ".exr";
    }

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this,
        "Save Rendered Image",
        defaultName,
        filter,
        &selectedFilter);
    if (path.isEmpty()) return;

    // If the user didn't type an extension, infer one from the
    // selected filter.  QFileDialog appends the filter's extension
    // automatically on most platforms but we belt-and-brace it.
    QFileInfo chosen(path);
    if (chosen.suffix().isEmpty()) {
        if      (selectedFilter.contains("*.exr"))  path += ".exr";
        else if (selectedFilter.contains("*.png"))  path += ".png";
        else if (selectedFilter.contains("*.tif"))  path += ".tiff";
        else if (selectedFilter.contains("*.hdr"))  path += ".hdr";
        else                                        path += ".exr";
    }

    // Map the final extension → bridge format name (case-insensitive).
    const QString ext = QFileInfo(path).suffix().toLower();
    QString formatName;
    if      (ext == "exr")                formatName = "EXR";
    else if (ext == "png")                formatName = "PNG";
    else if (ext == "tif" || ext == "tiff") formatName = "TIFF";
    else if (ext == "hdr")                formatName = "HDR";
    else if (ext == "rgbea")              formatName = "RGBEA";
    else if (ext == "tga")                formatName = "TGA";
    else if (ext == "ppm")                formatName = "PPM";
    else                                  formatName = "EXR";  // safe default

    const bool ok = m_engine->saveAs(path, formatName, /*ev=*/0.0);
    if (ok) {
        statusBar()->showMessage(
            QString("Saved %1 (%2)").arg(QFileInfo(path).fileName(), formatName),
            5000);
    } else {
        QMessageBox::warning(this, "Save Failed",
            QString("Could not write %1 as %2.  See the log for details.")
                .arg(QFileInfo(path).fileName(), formatName));
    }
}

void MainWindow::onHDRAvailabilityChanged(bool available)
{
    if (!m_hdrToggleAction) return;
    m_hdrToggleAction->setEnabled(available);
    if (!available && m_hdrToggleAction->isChecked()) {
        // Active monitor lost HDR capability (e.g. window dragged
        // back to an SDR display) — silently flip OFF.  Match the
        // macOS auto-disable behaviour from L5a round-7.
        m_hdrToggleAction->setChecked(false);
    } else if (available && !m_hdrToggleAction->isChecked()) {
        // Auto-enable on first detection (matches the macOS round-2
        // auto-enable when EDR becomes available).
        m_hdrToggleAction->setChecked(true);
    }
}

void MainWindow::createStatusBar()
{
    statusBar()->setSizeGripEnabled(true);
}

// ============================================================
// UI refinement item 3 — light/dark theme switch
// ============================================================

void MainWindow::onThemeModeChanged(Theme::ThemeMode newMode)
{
    if (newMode == Theme::mode()) return;
    Theme::setMode(newMode, *qApp);
    // Honest gap (see Theme.cpp's doc): most of this app's custom-
    // styled widgets bake Theme:: colors into a setStyleSheet() string
    // at construction time, not a live repaint -- those keep their
    // stale colors until reconstructed.  One-time notice per switch
    // rather than silently claiming a full live switch that isn't one.
    QMessageBox::information(this, tr("Theme Changed"),
        tr("Some panels update after restarting RISE."));
}

// ============================================================
// Scene-file save (interactive round-trip, Phase 6.5) +
// CST <-> scene-file live sync / auto-save (UI refinement item 1/2)
// ============================================================

void MainWindow::onSaveScene()
{
    // Manual entry point -- File > Save Scene and the TopBar chip's
    // suspended-state retry click both land here (see TopBar::saveClicked).
    performSceneSave(/*isAutoSave=*/false);
}

// Shared behind onSaveScene() (isAutoSave=false) and onCstSyncTick()'s
// debounced auto-save (isAutoSave=true).  Mirrors macOS
// RenderViewModel.saveScene(auto:) -- see that method's doc for the
// full "why": a manual save is always a fresh retry that clears any
// prior suspension up front; an auto-save failure logs one warning
// line and suspends further attempts instead of alert-spamming a
// modal on every 500ms poll tick; a manual retry (this method with
// isAutoSave=false) clears the suspension and keeps the original
// alert behavior.  Returns true on a successful (or no-op) save,
// false on refusal/I-O failure, so onCstSyncTick can decide whether
// to suspend further attempts.
bool MainWindow::performSceneSave(bool isAutoSave)
{
    // Note: no Save-As affordance here — matches the macOS TopBar's
    // Save pill.  Save-As lives in ViewportProperties (right panel).
    if (!m_viewportBridge) return false;
    const QString path = m_viewportBridge->loadedFilePath();
    if (path.isEmpty()) return false;

    if (!isAutoSave) {
        m_autoSaveSuspended = false;
        if (m_topBar) m_topBar->setAutoSaveSuspended(false);
    }

    QString errMsg;
    const ViewportBridge::SaveStatus status = m_viewportBridge->saveSceneTo(path, errMsg);
    switch (status) {
    case ViewportBridge::SaveStatus::Saved:
        m_autoSaveSuspended = false;
        if (m_topBar) m_topBar->setAutoSaveSuspended(false);
        // Manual save: pull the just-written bytes into the scene-
        // editor pane (unless it has its own unsaved text edits) and
        // refresh the window title.  An auto-save success stays
        // silent (mirrors macOS item 2: "nothing else, dirty flips
        // false via the existing push") -- the next onCstSyncTick's
        // item-1 sync mirrors the editor once the CST version bumps,
        // so there's no need to duplicate that here via a disk re-read.
        if (!isAutoSave) {
            onSceneSavedToPath(path);
        }
        return true;
    case ViewportBridge::SaveStatus::NoOp:
        return true;   // Silent success — file unchanged on disk.
    case ViewportBridge::SaveStatus::Refused: {
        const QString message = errMsg.isEmpty()
            ? tr("The save engine declined to write this file.") : errMsg;
        if (isAutoSave) {
            m_autoSaveSuspended = true;
            if (m_topBar) m_topBar->setAutoSaveSuspended(true);
            if (m_logWidget) m_logWidget->appendLog(RISE::eLog_Warning, tr("Auto-save refused: %1").arg(message));
        } else {
            QMessageBox::warning(this, "Save Refused", message);
        }
        return false;
    }
    case ViewportBridge::SaveStatus::Failed: {
        const QString message = errMsg.isEmpty()
            ? tr("An I/O error occurred while saving the file.") : errMsg;
        if (isAutoSave) {
            m_autoSaveSuspended = true;
            if (m_topBar) m_topBar->setAutoSaveSuspended(true);
            if (m_logWidget) m_logWidget->appendLog(RISE::eLog_Warning, tr("Auto-save failed: %1").arg(message));
        } else {
            QMessageBox::warning(this, "Save Failed", message);
        }
        return false;
    }
    case ViewportBridge::SaveStatus::Error: {
        const QString message = tr("Unexpected save state (%1).").arg(errMsg);
        if (isAutoSave) {
            m_autoSaveSuspended = true;
            if (m_topBar) m_topBar->setAutoSaveSuspended(true);
            if (m_logWidget) m_logWidget->appendLog(RISE::eLog_Warning, tr("Auto-save failed: %1").arg(message));
        } else {
            QMessageBox::warning(this, "Save Failed", message);
        }
        return false;
    }
    }
    return false;
}

// CST <-> scene-file live sync (item 1) + debounced auto-save (item 2).
// Driven at ~2 Hz by m_cstSyncTimer while a viewport bridge is attached
// (started in rebuildViewportForLoadedScene, stopped in
// teardownViewport).  Mirrors macOS RenderViewModel.pollRefinementState's
// items 1+2.
void MainWindow::onCstSyncTick()
{
    // Same gate as ChatPanel's chat-render poll (see its
    // refreshProposals doc): serializedSceneText()/getSceneTextVersion()
    // both take the controller's commit mutex, which a production
    // render OR an outstanding chat-driven render holds for its
    // duration -- calling either here would wedge this poll (and so
    // the GUI thread) against the render.  canUseSceneTransport()
    // already folds in the bridge-presence check.
    if (!canUseSceneTransport()) return;

    quint64 uuid = 0, revision = 0;
    if (!m_viewportBridge->getSceneTextVersion(&uuid, &revision)) return;
    const SceneTextVersion current{ uuid, revision, true };

    // Item 1 — mirror the live CST into the SceneEditor buffer.
    if (m_sceneEditor) {
        if (!m_sceneEditor->isDirty()) {
            // No unsaved buffer edits: safe to pull fresh text, and
            // this is also how the "buffer is stale" warning clears
            // once the user's buffer catches back up (e.g. after
            // Revert) without waiting for a new sync.
            if (current != m_lastSyncedSceneTextVersion) {
                m_sceneEditor->refreshFromLiveScene(m_viewportBridge->serializedSceneText());
                m_lastSyncedSceneTextVersion = current;
            }
            m_sceneEditor->setBehindLiveScene(false);
        } else if (current != m_lastSyncedSceneTextVersion) {
            // The user has unsaved buffer edits AND the live scene has
            // moved on since the last sync -- don't clobber the
            // buffer; just flag that it's stale.
            m_sceneEditor->setBehindLiveScene(true);
        }
    }

    // Item 2 — debounced auto-save.  hasUnsavedSceneChanges() mirrors
    // the CONTROLLER's dirty flag (agent/GUI edits pending a write),
    // independent of the SceneEditor text-editor buffer above.
    // Debounce: only save once the version has been stable across one
    // full poll tick, so a burst of edits coalesces into a single
    // write instead of saving mid-burst.  Never runs with no loaded
    // path or while suspended.
    if (m_viewportBridge->hasUnsavedSceneChanges()
        && !m_autoSaveSuspended
        && !m_engine->loadedFilePath().isEmpty()
        && m_previousPollSceneTextVersion.valid
        && m_previousPollSceneTextVersion == current
        && m_lastAutoSaveAttemptVersion != current) {
        m_lastAutoSaveAttemptVersion = current;
        performSceneSave(/*isAutoSave=*/true);
    }
    m_previousPollSceneTextVersion = current;
}

void MainWindow::onSceneSavedToPath(const QString& path)
{
    // Shared by TopBar's Save pill (onSaveScene above), the File >
    // Save Scene menu item, and ViewportProperties' own Save / Save
    // As… buttons (connected below in rebuildViewportForLoadedScene).
    updateWindowTitle();
    if (!m_sceneEditor) return;
    if (!m_sceneEditor->isDirty()) {
        m_sceneEditor->loadFile(path);
    } else {
        QMessageBox::warning(
            this,
            tr("Scene editor has unsaved text changes"),
            tr("Your interactive edits were saved to %1.  "
               "The scene editor pane still shows the "
               "pre-save text plus your unsaved edits.  "
               "Clicking Save in the scene editor will "
               "overwrite the just-saved interactive "
               "changes — use Revert in the scene editor "
               "to discard your text edits and pull the "
               "new file content.").arg(path));
    }
}

// ============================================================
// Recent Files
// ============================================================

void MainWindow::addToRecentFiles(const QString& filePath)
{
    // Remove if already present, then insert at front
    m_recentFiles.removeAll(filePath);
    m_recentFiles.prepend(filePath);

    // Cap at MAX_RECENT_FILES
    while (m_recentFiles.size() > MAX_RECENT_FILES) {
        m_recentFiles.removeLast();
    }

    // Persist
    QSettings settings;
    settings.setValue("recentSceneFiles", m_recentFiles);

    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    m_recentFilesMenu->clear();

    if (m_recentFiles.isEmpty()) {
        auto* emptyAction = m_recentFilesMenu->addAction("No Recent Scenes");
        emptyAction->setEnabled(false);
    } else {
        // Remove stale entries (files that no longer exist)
        QStringList validFiles;
        for (const QString& path : m_recentFiles) {
            if (QFileInfo::exists(path)) {
                validFiles.append(path);
            }
        }
        m_recentFiles = validFiles;

        for (const QString& path : m_recentFiles) {
            QString label = QFileInfo(path).fileName();
            auto* action = m_recentFilesMenu->addAction(label, [this, path]() {
                onOpenRecentScene(path);
            });
            action->setToolTip(path);
        }

        m_recentFilesMenu->addSeparator();
        m_recentFilesMenu->addAction("Clear Recent", this, &MainWindow::onClearRecentFiles);
    }
}

void MainWindow::onOpenRecentScene(const QString& filePath)
{
    if (!QFileInfo::exists(filePath)) {
        QMessageBox::warning(this, "File Not Found",
            QString("The file no longer exists:\n%1").arg(filePath));
        m_recentFiles.removeAll(filePath);
        updateRecentFilesMenu();
        return;
    }

    loadSceneFile(filePath);
}

void MainWindow::onClearRecentFiles()
{
    m_recentFiles.clear();

    QSettings settings;
    settings.setValue("recentSceneFiles", m_recentFiles);

    updateRecentFilesMenu();
}

// ============================================================
// Scene Loading
// ============================================================

void MainWindow::loadSceneFile(const QString& filePath)
{
    // Check for unsaved editor changes
    if (m_sceneEditor->isDirty()) {
        auto result = QMessageBox::question(this, "Unsaved Changes",
            "The editor has unsaved changes. Save before opening a new scene?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (result == QMessageBox::Cancel) return;
        if (result == QMessageBox::Save) m_sceneEditor->save();
    }

    // If a scene is already loaded, ask whether to clear or merge
    if (m_engine->state() != RenderEngine::Idle) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Scene Already Loaded");
        msgBox.setText("A scene is already loaded. How would you like to proceed?");
        auto* clearBtn = msgBox.addButton("Clear && Load", QMessageBox::AcceptRole);
        auto* mergeBtn = msgBox.addButton("Merge", QMessageBox::ActionRole);
        msgBox.addButton(QMessageBox::Cancel);
        msgBox.exec();

        if (msgBox.clickedButton() == clearBtn) {
            m_engine->clearScene();
        } else if (msgBox.clickedButton() == mergeBtn) {
            // Don't clear — merge on top of existing scene
        } else {
            return; // Cancel
        }
    }

    addToRecentFiles(filePath);
    m_engine->loadScene(filePath);
    updateWindowTitle();

    // UI redesign: the Scene file tab is always embedded (no Edit
    // toggle) — just refresh its contents from the freshly-loaded
    // file, whichever tab happens to be frontmost.
    m_sceneEditor->loadFile(filePath);
}

void MainWindow::onOpenScene()
{
    // Remember last directory
    QSettings settings;
    QString lastDir = settings.value("lastOpenDirectory").toString();

    QString filePath = QFileDialog::getOpenFileName(
        this, "Open Scene", lastDir,
        "RISE Scene Files (*.RISEscene);;All Files (*)");

    if (filePath.isEmpty()) return;

    // Save the directory for next time
    settings.setValue("lastOpenDirectory", QFileInfo(filePath).absolutePath());

    loadSceneFile(filePath);
}

// ============================================================
// Render controls
// ============================================================

void MainWindow::onClear()
{
    teardownViewport();
    m_engine->clearScene();
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::onRender()
{
    // P2-4: explicit gate mirroring the Mac 80d7e0f8 productionRenderStarting.
    // Cancels any in-flight chat turn (and an outstanding async chat-render
    // job) BEFORE the production rasterizer runs.  Do not remove this even
    // though onStateChanged's setSceneEditable(false) also disables the chat
    // panel below — that disable only takes effect on the NEXT event-loop
    // turn, which is too late to stop a chat turn already suspended in an
    // HTTP await or a render_wait poll from resuming into a production
    // render that is now reading Scene state off-main.
    if (m_chatPanel) m_chatPanel->productionRenderStarting();

    // Stop the viewport's render thread BEFORE the production
    // rasterizer runs — they'd race against the same scene state
    // otherwise.  The viewport restarts in onStateChanged when
    // production transitions back to Completed/Cancelled/Error.
    //
    // First halt any running preview-play QTimer: disabling the
    // timeline widget (done in onStateChanged once Rendering starts)
    // does NOT stop a live QTimer, so a tick could still fire a scrub
    // into the scene the production rasterizer is about to read.
    if (m_viewportTimeline) m_viewportTimeline->stopPlayback();
    if (m_viewportBridge) m_viewportBridge->stop();

    // UI redesign (A4 region refinement): a region "armed" for a
    // not-yet-drawn drag can no longer land against Scene state the
    // production rasterizer is about to read off-main — clear it so
    // the REGION chip doesn't lie about being armed through a render.
    if (m_viewportToolbar) m_viewportToolbar->cancelRegionArm();

    // Advance scene state to the canonical scrubbed time AND
    // regenerate photon maps before the production rasterizer fires.
    // The viewport's scrub path calls SetSceneTimeForPreview, which
    // advances the animator but skips photon regen for
    // responsiveness; without this full SetSceneTime, hitting Render
    // after scrubbing renders the right object positions but
    // caustics frozen at the pre-scrub time.
    //
    // We prefer the controller's lastSceneTime over
    // m_viewportTimeline->currentTime because Undo / Redo can change
    // scene time without touching the slider — passing the slider's
    // local value in that window would roll the scene back to a
    // stale time.  Fall back to the slider when the viewport bridge
    // is absent (no scene loaded, no scrubs possible).
    double sceneTime = m_viewportTimeline ? m_viewportTimeline->currentTime() : 0.0;
    if (m_viewportBridge) sceneTime = m_viewportBridge->lastSceneTime();
    m_engine->setSceneTime(sceneTime);

    m_engine->startRender();
}

void MainWindow::onRenderAnimation()
{
    // P2-4: same explicit gate as onRender() above (mirrors the Mac
    // 80d7e0f8 productionRenderStarting) — keep even though onStateChanged
    // also disables the chat panel; see onRender()'s comment for why.
    if (m_chatPanel) m_chatPanel->productionRenderStarting();

    // Derive video output path from scene file.  The animation export is
    // a ProRes 4444 (HDR10) master, which must live in a QuickTime (.mov)
    // container — so derive a .mov name directly.  (VideoEncoder also
    // force-rewrites any non-.mov path to .mov as a backstop.)
    QString scenePath = m_engine->loadedFilePath();
    QString videoPath = scenePath;
    videoPath.replace(".RISEscene", ".mov");

    // Halt a running preview-play QTimer before the production
    // animation render begins (see onRender for why widget-disable
    // alone is insufficient).
    if (m_viewportTimeline) m_viewportTimeline->stopPlayback();
    if (m_viewportBridge) m_viewportBridge->stop();
    if (m_viewportToolbar) m_viewportToolbar->cancelRegionArm();
    m_engine->startAnimationRender(videoPath);
}

void MainWindow::onCancel()
{
    m_engine->cancelRender();
}

// ============================================================
// State & UI updates
// ============================================================

void MainWindow::onStateChanged(int newState)
{
    auto state = static_cast<RenderEngine::State>(newState);

    m_renderWidget->setRenderState(state);

    // L5d — gate File > Save Rendered Image.  The production VFS's
    // FrameStore exists once the rasterizer has emitted at least
    // one OutputImage; that happens any time we transition through
    // Rendering / Cancelling.  Completed / Cancelled retain the
    // last contents (`bridge.clearAll` doesn't free the VFS, per
    // L4 §7.5).  Re-loading a scene transitions back through
    // Loading → SceneLoaded which has no fresh output yet — gate
    // off until the next render starts.
    if (m_saveImageAction) {
        const bool canSave =
            state == RenderEngine::Rendering
         || state == RenderEngine::Cancelling
         || state == RenderEngine::Completed
         || state == RenderEngine::Cancelled;
        m_saveImageAction->setEnabled(canSave);
    }

    // When the engine has just finished loading a scene, build the
    // viewport bridge over it and switch to the viewport pane.  When
    // the scene is cleared, tear it down.  No "interact mode" toggle:
    // the viewport is always visible once a scene is loaded.
    if (state == RenderEngine::SceneLoaded && !m_viewportBridge) {
        rebuildViewportForLoadedScene();
        if (m_viewportPane) m_viewStack->setCurrentWidget(m_viewportPane);
        if (m_viewportBridge) {
            // Override the scene's authored Film with a screen-appropriate
            // size for the INTERACTIVE preview.  The available render
            // surface is approximated by the screen's work area; the long
            // edge is capped at 800 px so we never burn cycles pushing
            // pixels the viewport widget would just downscale.  Must run
            // BEFORE start() so the first render pass picks up the new
            // dims.  Note: production renders launched from this app
            // (RequestProductionRender) will also use these dims — author
            // a larger value in the Output Settings panel to override.
            if (QScreen* screen = QApplication::primaryScreen()) {
                const QRect avail = screen->availableGeometry();
                m_viewportBridge->scaleFilmToFit(avail.width(), avail.height(), 800);
            }
            m_viewportBridge->start();
            // The new controller defaults to Select internally; if the
            // user had a camera tool selected on the previous scene,
            // re-push the toolbar's persisted selection so the toolbar
            // and the controller agree.
            if (m_viewportToolbar) {
                m_viewportBridge->setTool(m_viewportToolbar->currentTool());
            }
        }
        if (m_viewportProps)  m_viewportProps->refresh();
    } else if (state == RenderEngine::Idle && m_viewportBridge) {
        teardownViewport();
    }

    // Restart the viewport when production rendering ends so the user
    // can keep editing on the freshly-rendered scene state — but WITHOUT
    // its initial render pass, so the just-finished production image
    // stays on screen until the user actually interacts.  Without this
    // the interactive rasterizer's first pass would overwrite the
    // production result (visible as the render "flashing then flipping
    // back to the live preview").  The render thread stays parked until
    // the first edit / gesture; this works for both the LDR QImage path
    // and the HDR observer path, which the old sink-level frame-drop did
    // not (the HDR frame reaches the screen through the FrameStore
    // observer, bypassing the sink entirely).
    const bool renderEnded = (state == RenderEngine::Completed
                          || state == RenderEngine::Cancelled
                          || state == RenderEngine::Error);
    if (renderEnded && m_viewportBridge && !m_viewportBridge->isRunning()) {
        m_viewportBridge->startSuppressingInitialRender();
    }

    // While production is in flight, disable viewport interaction so
    // edits don't race the rasterizer.  The greyed-out widgets make
    // it visually obvious that the user must Cancel to interact.
    const bool interacting = (state != RenderEngine::Rendering
                          && state != RenderEngine::Cancelling
                          && state != RenderEngine::Loading);
    if (m_viewportToolbar)  m_viewportToolbar->setEnabled(interacting);
    if (m_viewportWidget)   m_viewportWidget->setEnabled(interacting);
    if (m_viewportTimeline) m_viewportTimeline->setEnabled(interacting);
    if (m_viewportProps)    m_viewportProps->setEnabled(interacting);
    if (m_outlinerWidget)   m_outlinerWidget->setEnabled(interacting);
    if (m_chatPanel)        m_chatPanel->setSceneEditable(interacting && m_viewportBridge != nullptr);

    // P2 fix: re-derive the EV chip's "not idle" half on every state
    // transition (rebuildViewportForLoadedScene/onHDRToggled already
    // cover the other two triggers) -- a no-op while m_viewportToolbar
    // is null (no scene loaded yet).
    updateEvEnabledState();

    // Gizmo overlay gate — narrower than `interacting` (which also
    // hides during `.loading` / `.cancelling` transitions).  Only
    // suppress the overlay while the production rasterizer is
    // actually running, so the user keeps the widgets between
    // renders and during brief state-machine transitions.  Mirrors
    // macOS `isProductionRendering`.
    if (m_viewportWidget) {
        m_viewportWidget->setProductionRendering(state == RenderEngine::Rendering);
    }

    // Disable Open Recent during active operations
    m_recentFilesMenu->setEnabled(interacting);

    // Covers both axes updateMenuActionStates() depends on: render
    // state (just changed) and viewport-bridge presence (rebuilt /
    // torn down synchronously above, if this transition triggered it).
    updateMenuActionStates();

    updateStatusBar();
    updateWindowTitle();
}

void MainWindow::onSceneSizeDetected(int width, int height)
{
    // Auto-resize window to fit scene, matching Mac app behavior.
    // UI redesign: the left/right panels are now fixed-width chrome
    // (404 + 344) rather than a togglable editor splitter, so the
    // width budget is constant regardless of which left-panel tab is
    // frontmost.
    const int kLeftPanelWidth = 404;
    const int kRightPanelWidth = 344;
    const int kTopBarHeight = 44;
    const int kBottomHeight = 190;   // timeline + log drawer, approx
    int statusHeight = 30;
    int menuHeight = menuBar()->height();

    int targetW = width + kLeftPanelWidth + kRightPanelWidth + 40;
    int targetH = height + kBottomHeight + kTopBarHeight + menuHeight + statusHeight + 40;

    // Clamp to screen size with smart scaling
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        QRect available = screen->availableGeometry();
        int maxW = available.width() - 50;
        int maxH = available.height() - 50;

        if (targetW > maxW || targetH > maxH) {
            // Scale down while preserving aspect ratio of render area
            double scaleW = static_cast<double>(maxW - kLeftPanelWidth - kRightPanelWidth - 40) / width;
            double scaleH = static_cast<double>(maxH - kBottomHeight - kTopBarHeight - menuHeight - statusHeight - 40) / height;
            double scale = qMin(scaleW, scaleH);
            scale = qMin(scale, 1.0); // Don't upscale

            targetW = static_cast<int>(width * scale) + kLeftPanelWidth + kRightPanelWidth + 40;
            targetH = static_cast<int>(height * scale) + kBottomHeight + kTopBarHeight + menuHeight + statusHeight + 40;
        }
    }

    // Only resize if larger than minimum
    if (targetW > minimumWidth() && targetH > minimumHeight()) {
        resize(targetW, targetH);
    }
}

void MainWindow::onSaveAndReload(const QString& filePath)
{
    m_engine->clearScene();
    m_engine->loadScene(filePath);
}

void MainWindow::updateWindowTitle()
{
    QString title;
    if (!m_engine->loadedFilePath().isEmpty()) {
        title = QFileInfo(m_engine->loadedFilePath()).fileName();
    } else {
        title = "RISE";
    }
    title += QString(" — RISE %1").arg(m_engine->versionString());
    setWindowTitle(title);

    if (m_topBar) m_topBar->setLoadedFilePath(m_engine->loadedFilePath());
}

void MainWindow::updateStatusBar()
{
    QString stateText;
    switch (m_engine->state()) {
    case RenderEngine::Idle:       stateText = "Ready"; break;
    case RenderEngine::Loading:    stateText = "Loading..."; break;
    case RenderEngine::SceneLoaded: stateText = "Scene loaded"; break;
    case RenderEngine::Rendering:  stateText = "Rendering..."; break;
    case RenderEngine::Cancelling: stateText = "Cancelling..."; break;
    case RenderEngine::Completed:  stateText = "Completed"; break;
    case RenderEngine::Cancelled:  stateText = "Cancelled"; break;
    case RenderEngine::Error:      stateText = "Error"; break;
    }

    // UI redesign: the resolved-integrator display now lives in the
    // TopBar cluster's "AUTO → X" chip (see TopBar::updateIntegratorChip).
    // The animation-output summary isn't covered by the TopBar, so it
    // stays here rather than being dropped.
    if (m_engine->state() == RenderEngine::Completed) {
        const QString animOut = m_engine->lastAnimationOutputsSummary();
        if (!animOut.isEmpty()) {
            stateText += QString(" · %1").arg(animOut);
        }
    }

    statusBar()->showMessage(QString("RISE %1 — %2")
        .arg(m_engine->versionString(), stateText));
}

// ============================================================
// Interactive viewport
// ============================================================

void MainWindow::rebuildViewportForLoadedScene()
{
    teardownViewport();

    m_viewportBridge = new ViewportBridge(m_engine, this);
    if (m_chatPanel) {
        m_chatPanel->setViewportBridge(m_viewportBridge);
        m_chatPanel->setSceneEditable(true);
    }
    if (m_topBar) {
        m_topBar->setViewportBridge(m_viewportBridge);
    }
    // Phase 6.5: mirror the dirty-state transition into the TopBar's
    // Save-pill enable state + dirty dot, AND the File > Save Scene
    // menu item's enable state (updateMenuActionStates reads
    // hasUnsavedSceneChanges()).  QueuedConnection-emitted by the
    // bridge's C trampoline (see ViewportBridge ctor), so a direct
    // Qt::AutoConnection here delivers on the GUI thread.
    connect(m_viewportBridge, &ViewportBridge::dirtyChanged, m_topBar, &TopBar::setSceneEditsDirty);
    connect(m_viewportBridge, &ViewportBridge::dirtyChanged, this, [this](bool) { updateMenuActionStates(); });

    // CST <-> scene-file live sync + auto-save (item 1/2): a freshly-
    // attached bridge's current version becomes the sync baseline --
    // the bytes that were just loaded/merged and the live CST agree at
    // this instant, so there is nothing to pull into the editor yet.
    // Only a LATER agent/GUI edit that bumps the revision should
    // overwrite the SceneEditor buffer.  Also resets the per-scene
    // auto-save/stale-warning state so it doesn't leak across scene
    // loads.  Mirrors macOS RISEViewportBridge's `viewportBridge`
    // didSet baseline-setting.  Gated on canUseSceneTransport() like
    // every other getSceneTextVersion()/serializedSceneText() call
    // site (see onCstSyncTick's doc) -- every caller of this method
    // only reaches it once renderState has already left Loading, so
    // this is always true in practice, but the gate is kept anyway
    // rather than assuming that invariant holds forever.
    {
        quint64 uuid = 0, revision = 0;
        m_lastSyncedSceneTextVersion =
            (canUseSceneTransport() && m_viewportBridge->getSceneTextVersion(&uuid, &revision))
            ? SceneTextVersion{ uuid, revision, true } : SceneTextVersion{};
    }
    m_previousPollSceneTextVersion = m_lastSyncedSceneTextVersion;
    m_lastAutoSaveAttemptVersion = SceneTextVersion{};
    m_autoSaveSuspended = false;
    if (m_topBar) m_topBar->setAutoSaveSuspended(false);
    if (m_sceneEditor) m_sceneEditor->setBehindLiveScene(false);
    if (m_cstSyncTimer) m_cstSyncTimer->start();

    m_viewportToolbar  = new ViewportToolbar();
    m_viewportToolbar->setBridge(m_viewportBridge);
    m_viewportWidget   = new ViewportWidget(m_viewportBridge);
    m_viewportTimeline = new ViewportTimeline();
    m_viewportProps    = new ViewportProperties(m_viewportBridge);
    m_viewportTimeline->setVisible(m_engine->hasAnimation());

    // UI redesign slice B — EV chip: the exposure slider widget lives
    // inside the fresh per-scene ViewportToolbar; re-establish the
    // RenderEngine-facing connections and seed the HDR interlock from
    // the CURRENT View > HDR Preview state (a scene can be (re)loaded
    // while HDR Preview is already on).
    connect(m_viewportToolbar->exposureSlider(), &QSlider::valueChanged,
            this, &MainWindow::onExposureSliderChanged);
    connect(m_viewportToolbar->exposureSlider(), &ExposureSlider::resetRequested,
            this, &MainWindow::onExposureResetRequested);
    const bool hdrOn = m_hdrToggleAction && m_hdrToggleAction->isChecked();
    updateEvEnabledState();
    m_viewportToolbar->setEdrChecked(hdrOn);
    m_viewportToolbar->setEdrEnabled(m_hdrToggleAction && m_hdrToggleAction->isEnabled());

    // EDR chip <-> View > HDR Preview: single source of truth is the
    // QAction; the chip forwards a click to toggle() it (which then
    // fires onHDRToggled, which mirrors the new checked state back
    // onto the chip) rather than driving RenderEngine state directly.
    connect(m_viewportToolbar, &ViewportToolbar::edrToggleClicked, this, [this]() {
        if (m_hdrToggleAction && m_hdrToggleAction->isEnabled()) m_hdrToggleAction->toggle();
    });

    // REGION arm/drag hand-off between the toolbar chip and the
    // viewport surface -- both directions, see each signal's doc.
    connect(m_viewportToolbar, &ViewportToolbar::regionArmedChanged,
            m_viewportWidget, &ViewportWidget::setRegionArmed);
    connect(m_viewportWidget, &ViewportWidget::regionArmCancelled,
            m_viewportToolbar, &ViewportToolbar::cancelRegionArm);

    if (m_outlinerWidget) {
        m_outlinerWidget->setBridge(m_viewportBridge);
        // A pick in the outliner (category open/close, entity select)
        // doesn't itself touch the interactive rasterizer, so there's
        // no imageUpdated to ride -- follow it explicitly so the
        // single-entity inspector below updates immediately.
        connect(m_outlinerWidget, &OutlinerWidget::selectionActivated,
                m_viewportProps, &ViewportProperties::refresh);
    }

    // Pull the timeline range from the scene's animation_options
    // chunk via the bridge.  Defaults are (0, 1) when the scene
    // declares no animation_options; we keep the slider's max above
    // 0 so a 0-length scene still produces a visible (if useless)
    // slider — the user sees that no animation is wired up rather
    // than a clamped slider that always reads t=0.
    {
        double t0 = 0, t1 = 0;
        unsigned int nf = 0;
        if (m_viewportBridge->animationOptions(t0, t1, nf) && t1 > t0) {
            m_viewportTimeline->setRange(t0, t1);
            m_viewportTimeline->setAnimationFrameCount(nf);
        }
    }

    // The scene's named animations are surfaced by the ViewportProperties
    // panel's "Animation" accordion category (constructed just above) —
    // it pulls the list from the bridge's generic categoryEntities() and
    // activates a pick via setSelection(Category::Animation, …).  The
    // timeline still follows the active animation via animationOptions().

    // Viewport pane: VBox{ toolbar, viewport widget } — hosted inside
    // m_viewStack (the center column's persistent top slot).  The
    // timeline and properties panel are no longer nested here; they're
    // inserted into the persistent center-column / right-panel chrome
    // below instead (see MainWindow.h's header doc) — their C++
    // lifetime is still scene-scoped, created here and destroyed by
    // teardownViewport, only their PARENT LAYOUT changed.
    auto* col = new QVBoxLayout;
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(m_viewportToolbar);
    col->addWidget(m_viewportWidget, 1);
    m_viewportPane = new QWidget;
    m_viewportPane->setLayout(col);
    m_viewStack->addWidget(m_viewportPane);   // index 1

    m_centerColumnLayout->insertWidget(1, m_viewportTimeline);
    m_rightPanelLayout->addWidget(m_viewportProps);

    // Wire signals.
    connect(m_viewportToolbar,  &ViewportToolbar::toolChanged,
            m_viewportBridge,   &ViewportBridge::setTool);
    connect(m_viewportToolbar,  &ViewportToolbar::toolChanged,
            m_viewportWidget,   &ViewportWidget::setActiveTool);
    // Tool change may have flipped the panel mode (Camera vs None vs
    // Object) — refresh the props panel so it switches contents /
    // header accordingly.
    connect(m_viewportToolbar,  &ViewportToolbar::toolChanged,
            m_viewportProps,    [this](ViewportTool) {
                if (m_viewportProps) m_viewportProps->refresh();
            });
    // Round-2 P1 belt-and-suspenders: route through a gate that
    // re-checks at click time (the buttons are ALSO disabled via
    // setUndoRedoEnabled, but a click racing a state flip must refuse).
    connect(m_viewportToolbar,  &ViewportToolbar::undoClicked,
            this, [this]() {
                if (!canUseSceneTransport()) return;
                m_viewportBridge->undo();
            });
    connect(m_viewportToolbar,  &ViewportToolbar::redoClicked,
            this, [this]() {
                if (!canUseSceneTransport()) return;
                m_viewportBridge->redo();
            });

    connect(m_viewportTimeline, &ViewportTimeline::scrubBegin,
            m_viewportBridge,   &ViewportBridge::scrubTimeBegin);
    connect(m_viewportTimeline, &ViewportTimeline::scrubEnd,
            m_viewportBridge,   &ViewportBridge::scrubTimeEnd);
    connect(m_viewportTimeline, &ViewportTimeline::timeChanged,
            m_viewportBridge,   &ViewportBridge::scrubTime);
    // "Render movie…" chip -- same slot the Render > Render Animation
    // menu item drives; setRenderMovieEnabled above keeps it gated
    // identically to that menu item.
    connect(m_viewportTimeline, &ViewportTimeline::renderMovieClicked,
            this, &MainWindow::onRenderAnimation);
    m_viewportTimeline->setRenderMovieEnabled(
        m_renderAnimAction && m_renderAnimAction->isEnabled());

    // Live-preview frames from the bridge → viewport widget + props refresh.
    connect(m_viewportBridge, &ViewportBridge::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
    connect(m_viewportBridge, &ViewportBridge::imageUpdated,
            m_viewportProps,  &ViewportProperties::refresh);
    if (m_outlinerWidget) {
        connect(m_viewportBridge, &ViewportBridge::imageUpdated,
                m_outlinerWidget, &OutlinerWidget::refresh);
    }

    // Round-trip save success: pull the just-written bytes into the
    // SceneEditor text pane so it reflects the saved edits, refresh
    // the window title, and surface a warning if the user also has
    // unsaved text-editor edits.  Shared with TopBar's Save pill /
    // File > Save Scene via the onSceneSavedToPath slot.
    connect(m_viewportProps, &ViewportProperties::sceneSavedToPath,
            this, &MainWindow::onSceneSavedToPath);

    // Production-render frames from the engine → also flow to the
    // viewport widget so clicking "Render" updates the live view.
    connect(m_engine, &RenderEngine::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
}

void MainWindow::teardownViewport()
{
    if (!m_viewportBridge) return;

    // CST <-> scene-file live sync + auto-save (item 1/2): no bridge,
    // no sync/auto-save -- stop polling and drop all per-scene state so
    // it can't leak into the next scene load.
    if (m_cstSyncTimer) m_cstSyncTimer->stop();
    m_lastSyncedSceneTextVersion = SceneTextVersion{};
    m_previousPollSceneTextVersion = SceneTextVersion{};
    m_lastAutoSaveAttemptVersion = SceneTextVersion{};
    m_autoSaveSuspended = false;
    if (m_sceneEditor) m_sceneEditor->setBehindLiveScene(false);

    if (m_chatPanel) {
        m_chatPanel->setViewportBridge(nullptr);
    }
    if (m_topBar) {
        m_topBar->setViewportBridge(nullptr);
        m_topBar->setSceneEditsDirty(false);
        m_topBar->setAutoSaveSuspended(false);
    }
    if (m_outlinerWidget) {
        m_outlinerWidget->setBridge(nullptr);
    }

    // Close any looping preview-play (and its open scrub bracket) through the
    // normal path before the bridge it drives goes away — don't rely on
    // controller destruction to swallow an open composite.
    if (m_viewportTimeline) m_viewportTimeline->stopPlayback();

    // Stop the render thread BEFORE the bridge dies.
    m_viewportBridge->stop();

    if (m_viewportTimeline) {
        m_centerColumnLayout->removeWidget(m_viewportTimeline);
        delete m_viewportTimeline;
        m_viewportTimeline = nullptr;
    }
    if (m_viewportProps) {
        m_rightPanelLayout->removeWidget(m_viewportProps);
        delete m_viewportProps;
        m_viewportProps = nullptr;
    }
    if (m_viewportPane) {
        m_viewStack->removeWidget(m_viewportPane);
        delete m_viewportPane;
        m_viewportPane = nullptr;
        m_viewportToolbar = nullptr;
        m_viewportWidget = nullptr;
    }
    delete m_viewportBridge;
    m_viewportBridge = nullptr;
    // Fall back to the passive RenderWidget when no scene is loaded.
    m_viewStack->setCurrentIndex(0);
}
