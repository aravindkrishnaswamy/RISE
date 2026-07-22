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
#include "EnvironmentPanel.h"
#include "StartWidget.h"
#include "Theme.h"

#include <QAction>
#include <QtCore/qalgorithms.h>   // qDeleteAll (Insert-menu submenu teardown)
#include <QActionGroup>
#include <QButtonGroup>
#include <QToolButton>

#include <QMenuBar>
#include <QStatusBar>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QLabel>
#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QPalette>
#include <QSlider>
#include <QStackedWidget>
#include <QSplitter>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    resize(1440, 900);
    setMinimumSize(1320, 760);

    // Load recent files from settings
    QSettings settings;
    m_recentFiles = settings.value("recentSceneFiles").toStringList();
    // Start-screen spec §5.4: sibling recentSceneMeta key (path -> last-
    // opened epoch seconds).  A hand-edited/corrupted entry that doesn't
    // convert to a whole number reads as 0 -- rendered as "no time" by
    // StartWidget rather than a bogus date.
    {
        const QVariantMap meta = settings.value("recentSceneMeta").toMap();
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) {
            m_recentFilesMeta.insert(it.key(), it.value().toLongLong());
        }
    }

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

    // Stacked widget toggles between the passive render view, the start
    // screen, and the interactive viewport pane.  The viewport pane is
    // built lazily when a scene loads (see rebuildViewportForLoadedScene).
    // Which of the three is CURRENT is decided in exactly one place --
    // updateCenterViewStack() -- driven from every render-state / bridge
    // transition (mirrors macOS ContentView's declarative `viewportBridge
    // == nil` gate).
    m_viewStack = new QStackedWidget();
    m_viewStack->addWidget(m_productionPaneStack);   // index 0
    m_startWidget = new StartWidget();
    m_viewStack->addWidget(m_startWidget);           // index 1

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

    // Wider, resizable left panel: m_leftPanel + the center column live
    // inside a QSplitter so the user can drag the boundary between them.
    // The right panel stays a fixed-width sibling outside the splitter
    // (design brief only calls for the left edge to be resizable).
    m_leftPanel = buildLeftPanel();
    QWidget* centerColumn = buildCenterColumn();

    m_leftSplitter = new QSplitter(Qt::Horizontal, mainArea);
    m_leftSplitter->setHandleWidth(5);
    // Prevent a fast/careless drag from collapsing the left panel to 0 --
    // its own min/max width (set in buildLeftPanel) then becomes the real
    // drag bound instead of just a soft hint.
    m_leftSplitter->setChildrenCollapsible(false);
    m_leftSplitter->addWidget(m_leftPanel);
    m_leftSplitter->addWidget(centerColumn);
    m_leftSplitter->setStretchFactor(0, 0);   // left panel: user-sized
    m_leftSplitter->setStretchFactor(1, 1);   // center column: takes the rest
    m_leftSplitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background-color: %1; }"
        "QSplitter::handle:hover { background-color: %2; }")
        .arg(Theme::hex(Theme::borderHairline), Theme::hex(Theme::borderHover)));

    // Persist the left panel's width across launches (QSettings
    // "leftPanelWidth"), mirroring the macOS client's
    // @AppStorage("leftPanelWidth") in ContentView.swift.  Default 520 —
    // wider than the original 404px comp value so the scene file is
    // actually readable; clamp to the same [360, 700] bounds the drag
    // handle itself enforces, in case the persisted value predates this
    // refinement or was hand-edited in the registry/INI.
    {
        QSettings splitterSettings;
        const int savedLeftWidth = splitterSettings.value(QStringLiteral("leftPanelWidth"), 520).toInt();
        const int clampedLeftWidth = std::clamp(savedLeftWidth, 360, 700);
        m_leftSplitter->setSizes({ clampedLeftWidth, 10000 });
    }
    connect(m_leftSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (!m_leftSplitter) return;
        const QList<int> sizes = m_leftSplitter->sizes();
        if (sizes.isEmpty()) return;
        QSettings s;
        s.setValue(QStringLiteral("leftPanelWidth"), sizes.first());
    });

    mainAreaLayout->addWidget(m_leftSplitter, 1);

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
        // Start screen (spec §8): a failed LOAD (no bridge attached yet --
        // a render-time failure of an already-loaded scene leaves the
        // bridge in place and never shows StartWidget at all, see
        // updateCenterViewStack) lands the user back here with the error
        // as a banner, and re-enables Create for a retry.
        if (m_startWidget && !m_viewportBridge) {
            m_startWidget->setErrorMessage(msg);
            m_startWidget->setCreateInFlight(false);
            // A failed STARTER load must not leave its create-prompt staged
            // (ChatPanel::setViewportBridge never ran to consume it) -- the
            // next successful open would auto-send it into an unrelated
            // scene (review P1; exact mirror of the Mac e0c8d823 fix).
            if (m_chatPanel) m_chatPanel->setPendingFirstPrompt(QString());
        }
    });

    connect(m_topBar, &TopBar::saveClicked, this, &MainWindow::onSaveScene);
    // Render-transport pill (TopBar, right side): idle-state clicks route
    // through the SAME slot the Render menu action / Ctrl+R shortcut use,
    // so the pre-render bookkeeping (stopping the viewport, cancelling an
    // outstanding chat turn, advancing scene time) lives in exactly one
    // place.  Pause/resume during Rendering are handled locally by
    // TopBar via the engine's setProductionRenderPaused accessor and
    // never reach this signal -- see TopBar::onRenderTransportClicked.
    connect(m_topBar, &TopBar::renderTransportClicked, this, &MainWindow::onRender);

    // P1-2 fix (mirrors Mac's canUseSceneTransport): while a chat-driven
    // `render` tool call owns the scene, undo/redo and refinement
    // transport must go stale-free even though nothing routes through
    // RenderEngine::stateChanged for that transition -- it's entirely
    // internal to ChatPanel's async poll timer.  Refresh both the menu
    // actions and TopBar's own control-enable state at every transition.
    m_topBar->setChatPanel(m_chatPanel);
    connect(m_chatPanel, &ChatPanel::chatRenderOutstandingChanged, this, &MainWindow::updateMenuActionStates);
    connect(m_chatPanel, &ChatPanel::chatRenderOutstandingChanged, m_topBar, &TopBar::onChatRenderOutstandingChanged);

    // ---- Start screen (docs/gui/START_SCREEN.md) -----------------------
    // StartWidget owns no scene/agent state itself; every signal routes
    // to the exact code path the equivalent menu/button already uses.
    connect(m_startWidget, &StartWidget::openPathRequested, this, &MainWindow::onOpenRecentScene);
    connect(m_startWidget, &StartWidget::removeRecentRequested, this, &MainWindow::removeRecentFile);
    connect(m_startWidget, &StartWidget::browseRequested, this, &MainWindow::onOpenScene);
    connect(m_startWidget, &StartWidget::createRequested, this, &MainWindow::onCreateSceneFromPrompt);
    // "Set up…" (spec §6, unconfigured state): this platform has no
    // separate provider-settings popover -- the chat panel's own
    // provider/key row IS the settings surface -- so this just switches
    // to it, force-showing the left panel (and keeping the View > Left
    // Panel checkbox honest) in case the user had hidden it.
    connect(m_startWidget, &StartWidget::setupAgentRequested, this, [this]() {
        if (m_leftPanelAction) m_leftPanelAction->setChecked(true);
        if (m_leftPanel) m_leftPanel->setVisible(true);
        setLeftTab(0);
    });

    // Start screen readiness (spec §6): mirror ChatPanel's real provider
    // state into the Create column, live -- reusing ChatPanel's own
    // check (agentConfigured()) rather than a second source of truth.
    auto pushAgentReadiness = [this]() {
        if (m_startWidget && m_chatPanel) {
            m_startWidget->setAgentReadiness(m_chatPanel->agentConfigured(),
                                              m_chatPanel->currentProviderDisplayName());
        }
    };
    connect(m_chatPanel, &ChatPanel::agentConfiguredChanged, this, pushAgentReadiness);
    pushAgentReadiness();

    // CST <-> scene-file live sync (item 1).  ~2 Hz, same cadence as
    // TopBar's own refinement-status poll.  Started/stopped alongside
    // the viewport bridge -- see rebuildViewportForLoadedScene /
    // teardownViewport.
    m_cstSyncTimer = new QTimer(this);
    m_cstSyncTimer->setInterval(500);
    connect(m_cstSyncTimer, &QTimer::timeout, this, &MainWindow::onCstSyncTick);

    // Connect editor signals.  UI redesign: the Scene file tab has no
    // "close" concept of its own (the tab strip always offers both
    // tabs) — the editor's inline "X" button now just flips back to
    // the Agent tab instead of hiding a splitter pane.
    connect(m_sceneEditor, &SceneEditor::closeRequested, this, [this]() { setLeftTab(0); });
    connect(m_sceneEditor, &SceneEditor::saveAndReloadRequested, this, &MainWindow::onSaveAndReload);
    connect(m_sceneEditor, &SceneEditor::selectEntityAtByteOffsetRequested,
            this, &MainWindow::selectEntityAtByteOffset);
    // Reverse source traceability: gate the "Select in Inspector" item's
    // enablement on the SAME conditions selectEntityAtByteOffset checks, so
    // it greys out (rather than silently no-opping) on a dirty buffer or
    // while a render owns the scene.  Evaluated lazily at right-click time,
    // so it always reflects the current transport + dirty state.
    m_sceneEditor->setCanSelectEntityPredicate([this]() { return canReverseSelect(); });

    // Set initial status
    statusBar()->showMessage(QString("RISE %1 — Ready").arg(m_engine->versionString()));

    // L5b — initial HDR-availability probe.  HDRRenderWidget is at
    // index 1 of the production sub-stack and so won't fire its own
    // Show event until the toggle is flipped — chicken-and-egg.
    // The static probe enumerates all DXGI outputs without needing
    // a swap chain.
    onHDRAvailabilityChanged(HDRRenderWidget::probeAnyAdapterHDRAvailable());

    // Start screen: establish the initial center-stack widget (Idle, no
    // bridge -> StartWidget) -- every LATER transition is driven from
    // onStateChanged/teardownViewport, but nothing has fired yet at
    // construction time.
    updateCenterViewStack();
}

// ============================================================
// Workspace chrome construction (UI redesign)
// ============================================================

QWidget* MainWindow::buildLeftPanel()
{
    auto* panel = new QWidget();
    // Wider, resizable left panel: was setFixedWidth(404); now lives
    // inside m_leftSplitter (see the constructor), user-draggable
    // between these two bounds.  Initial/persisted width is set on the
    // splitter itself, not here.
    panel->setMinimumWidth(360);
    // Review P2: 700 max (not 900) — at the 1320px minimum window the
    // fixed 344px right panel + a 900px left panel would leave the
    // center viewport ~71px.  700 keeps the center >= ~270px worst-case.
    panel->setMaximumWidth(700);
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

    // "Reveal in scene file" (item 3): connected ONCE here (not in
    // rebuildViewportForLoadedScene) because m_outlinerWidget is
    // persistent but `this` (the receiver) is not recreated per scene
    // load -- connecting inside rebuildViewportForLoadedScene would
    // accumulate a duplicate connection on every scene reload.  The
    // signal carries ViewportBridge::Category; adapt to the int-typed
    // slot so MainWindow.h doesn't need to include ViewportBridge.h
    // (see revealEntityInSceneText's doc).
    connect(m_outlinerWidget, &OutlinerWidget::revealRequested, this,
            [this](ViewportBridge::Category category, const QString& name) {
                revealEntityInSceneText(static_cast<int>(category), name);
            });

    // Entity-creation slice: same "connect once, adapt the enum to a raw
    // int" pattern as revealRequested above -- m_outlinerWidget is
    // persistent, `this` is not recreated per scene load.
    connect(m_outlinerWidget, &OutlinerWidget::addEntityRequested, this,
            [this](ViewportBridge::Category category, unsigned int templateIndex) {
                addEntity(static_cast<int>(category), templateIndex);
            });
    connect(m_outlinerWidget, &OutlinerWidget::duplicateRequested, this,
            [this](ViewportBridge::Category category, const QString& name) {
                duplicateEntity(static_cast<int>(category), name);
            });
    connect(m_outlinerWidget, &OutlinerWidget::deleteRequested, this,
            [this](ViewportBridge::Category category, const QString& name) {
                removeEntity(static_cast<int>(category), name);
            });

    // Environment / IBL section, between the outliner and the per-scene
    // ViewportProperties (which rebuildViewportForLoadedScene appends
    // below it).  Persistent like the outliner -- setBridge() gives it a
    // live scene on load, nullptr on teardown; it hides itself when the
    // scene has no active rasterizer.  Mirrors the macOS EnvironmentPanel
    // sitting between OutlinerView and PropertiesPanel.
    m_environmentPanel = new EnvironmentPanel(panel);
    m_rightPanelLayout->addWidget(m_environmentPanel);

    // A landed environment edit changes the Painter list (Add/Remove) and
    // re-renders (live edits) -- refresh the outliner so it follows.  The
    // per-scene ViewportProperties is connected in
    // rebuildViewportForLoadedScene (same persistent-to-per-scene split as
    // the outliner's selectionActivated connect).  Both m_environmentPanel
    // and m_outlinerWidget are persistent, so this connects once.
    connect(m_environmentPanel, &EnvironmentPanel::environmentEdited,
            m_outlinerWidget, &OutlinerWidget::refresh);

    // Source traceability: a per-row reveal from the Environment section
    // resolves the backing param's exact span (the HDRI file on the bound
    // painter; radiance_* on the active rasterizer).  m_environmentPanel is
    // persistent, so this connects once.  occurrence 0 = the first (only)
    // matching param on the singleton chunk.
    connect(m_environmentPanel, &EnvironmentPanel::revealEnvParamRequested, this,
            [this](int category, const QString& name, const QString& param) {
                revealSourceSpan(category, name, param, 0);
            });

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
    m_closeSceneAction = fileMenu->addAction("C&lear Scene", this, &MainWindow::onClear);
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

    // --- Insert menu ---
    // Entity-creation slice: "Add Entity" templates, alongside the
    // outliner's per-category "+" affordance and context menu (same
    // addEntity() call). Mirrors macOS RISEApp.swift's CommandMenu
    // "Insert" -- rebuilt on aboutToShow (template counts/labels are
    // cheap, lock-free static lookups, safe to query during a render;
    // only the per-item Add action is gated on canUseSceneTransport()).
    // A category with no registered templates (Camera/Rasterizer/Film/
    // Animation/SceneVariant, or every category before a scene is
    // loaded) simply doesn't get a submenu.
    auto* insertMenu = menuBar()->addMenu("&Insert");
    connect(insertMenu, &QMenu::aboutToShow, this, [this, insertMenu]() {
        // QMenu::clear() does NOT free submenus added via addMenu(): a
        // submenu's own menuAction() is parented to the SUBMENU itself
        // (see QMenuPrivate::init), not to `insertMenu`, so clear()'s
        // "only delete actions this menu owns" rule silently skips it --
        // every menu-open would otherwise leak one QMenu (+ its QActions)
        // per populated category.  Delete the submenus explicitly first;
        // their destructor removes the now-dangling action from
        // insertMenu automatically, so the clear() below is just a
        // defensive mop-up.
        qDeleteAll(insertMenu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
        insertMenu->clear();
        if (!m_viewportBridge) return;
        struct InsertCategoryEntry { const char* title; ViewportBridge::Category category; };
        static const InsertCategoryEntry kEntries[] = {
            { "Object",   ViewportBridge::Category::Object },
            { "Light",    ViewportBridge::Category::Light },
            { "Material", ViewportBridge::Category::Material },
            { "Painter",  ViewportBridge::Category::Painter },
            { "Medium",   ViewportBridge::Category::Medium },
        };
        const bool canAdd = canUseSceneTransport();
        for (const auto& entry : kEntries) {
            const unsigned int count = m_viewportBridge->entityTemplateCount(entry.category);
            if (count == 0) continue;
            QMenu* sub = insertMenu->addMenu(tr(entry.title));
            sub->setEnabled(canAdd);
            const int catInt = static_cast<int>(entry.category);
            for (unsigned int i = 0; i < count; ++i) {
                const QString label = m_viewportBridge->entityTemplateLabel(entry.category, i);
                QAction* templateAction = sub->addAction(label);
                connect(templateAction, &QAction::triggered, this,
                        [this, catInt, i]() { addEntity(catInt, i); });
            }
        }
    });

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
    m_leftPanelAction = viewMenu->addAction("Left Panel");
    m_leftPanelAction->setCheckable(true);
    m_leftPanelAction->setChecked(true);
    connect(m_leftPanelAction, &QAction::toggled, this, [this](bool on) {
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

    // Production-only now: the center-cluster refinement pause/resume
    // button was removed from TopBar by user request (interactive
    // refinement restarts on every edit anyway, so pausing it was a
    // niche control -- see TopBar.cpp's tombstone comment on
    // onPauseResumeClicked), so this menu item no longer has a
    // refinement-side counterpart to defer to.  It now ONLY pauses/
    // resumes THE RENDER, mirrors macOS RISEApp.swift's pauseMenuTitle /
    // Render-menu Button, and is enabled only while Rendering (see
    // updateMenuActionStates) -- not the dual-mode affordance the prior
    // comment here described.
    m_pauseResumeAction = renderMenu->addAction("Pause Render");
    m_pauseResumeAction->setShortcut(QKeySequence("Ctrl+Space"));
    connect(m_pauseResumeAction, &QAction::triggered, this, [this]() {
        if (m_engine->state() == RenderEngine::Rendering) {
            m_engine->setProductionRenderPaused(!m_engine->isProductionRenderPaused());
            updateMenuActionStates();
        }
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

bool MainWindow::canReverseSelect() const
{
    return canUseSceneTransport() && m_sceneEditor && !m_sceneEditor->isDirty();
}

void MainWindow::updateMenuActionStates()
{
    const auto state = m_engine->state();
    const bool interacting = (state != RenderEngine::Rendering
                           && state != RenderEngine::Cancelling
                           && state != RenderEngine::Loading);

    if (m_saveSceneAction) {
        // Explicit-save-only (user decision 2026-07-12): enabled iff
        // there's something unsaved -- no auto-save-suspended state to
        // fold in anymore.  Mirrors macOS RISEApp.swift's "Save Scene"
        // menu item gate.
        m_saveSceneAction->setEnabled(
            m_viewportBridge && m_viewportBridge->hasUnsavedSceneChanges());
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
    // Layout selection also reaches the controller (and can otherwise block
    // on the whole-duration mutex held by an agent render).  Keep all viewport
    // toolbar controls on the same chat-inclusive ownership gate.
    if (m_viewportToolbar) m_viewportToolbar->setEnabled(bridgeInteractingEnabled);

    // "Reveal in scene file" (item 3): the properties panel's ⌗ chip
    // and the outliner's context-menu item both call
    // ViewportBridge::getEntitySourceLocation, which takes the SAME
    // commit mutex as every other serializedSceneText()-family call --
    // gate both on the IDENTICAL bridgeInteractingEnabled term used for
    // undo/redo above (mirrors Mac's isSceneEditableForAgents), so a
    // chat-driven render can never be wedged against by either affordance.
    if (m_viewportProps)  m_viewportProps->setSceneEditable(bridgeInteractingEnabled);
    if (m_outlinerWidget) m_outlinerWidget->setSceneEditable(bridgeInteractingEnabled);
    // Environment section's mutating controls gate on the IDENTICAL term:
    // each ViewportBridge setEnvironment* call takes the controller's
    // commit mutex a production / chat-driven render owns.
    if (m_environmentPanel) m_environmentPanel->setSceneEditable(bridgeInteractingEnabled);
    // The viewport's nav axis-ball reads isFreeFlyActive()/hasHomeView() and
    // its pointer* calls all take the controller commit mutex; gate them on the
    // SAME chat-inclusive term.  onStateChanged's setEnabled(interacting) is
    // render-state-only and misses the chat-render clause, so without this a
    // chat-driven render would wedge the viewport (nav overlay repaint on
    // Windows needs no gesture at all).
    if (m_viewportWidget) m_viewportWidget->setSceneEditable(bridgeInteractingEnabled);
    // The timeline scrub signal -> OnTimeScrub takes the controller mutex; its
    // onStateChanged setEnabled(interacting) is render-state-only and misses the
    // chat-render clause, so disable it on the chat-inclusive term here too.
    if (m_viewportTimeline) m_viewportTimeline->setEnabled(bridgeInteractingEnabled);

    const bool refinementDisabled = !m_viewportBridge
        || state == RenderEngine::Rendering
        || state == RenderEngine::Cancelling
        || chatRenderOutstanding;
    if (m_pauseResumeAction) {
        // Production-only now (mirrors macOS RISEApp.swift's
        // pauseMenuTitle / Button.disabled(viewModel.renderState !=
        // .rendering)): the center-cluster refinement pause button is
        // gone (see TopBar.cpp's tombstone comment), so this item has
        // no refinement-side mode left to fall back to.  Enabled ONLY
        // while Rendering; label/action always target THE RENDER.
        m_pauseResumeAction->setEnabled(state == RenderEngine::Rendering);
        m_pauseResumeAction->setText(
            (state == RenderEngine::Rendering && m_engine->isProductionRenderPaused())
                ? "Resume Render" : "Pause Render");
    }
    if (m_restartAction) {
        m_restartAction->setEnabled(!refinementDisabled);
    }

    const bool canRender = (state == RenderEngine::SceneLoaded
                         || state == RenderEngine::Completed
                         || state == RenderEngine::Cancelled);
    if (m_renderAction)     m_renderAction->setEnabled(canRender);
    // TopBar's render-transport pill (right side) mirrors this SAME
    // predicate for its idle "Render" state -- pushed in here rather
    // than re-derived in TopBar so the two enable rules can never
    // hand-copy-drift apart.  See TopBar::setCanStartProductionRender.
    if (m_topBar)            m_topBar->setCanStartProductionRender(canRender);
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
        // user-review P2#7: the same monitor move changes devicePixelRatio;
        // a fixed-size window fires no resizeEvent, so the N-up panes must be
        // told to re-push their device-pixel render dims for the new screen.
        if (m_viewportWidget) m_viewportWidget->refreshForDpiChange();
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
// CST <-> scene-file live sync (UI refinement item 1)
// ============================================================

void MainWindow::onSaveScene()
{
    // Entry point -- File > Save Scene and the TopBar's Save pill both
    // land here (see TopBar::saveClicked).
    // Untitled scene (start-screen create path, spec §5.2): nothing on
    // disk to save in-place -- the first Save IS Save-As.
    if (m_viewportBridge && m_viewportBridge->loadedFilePath().isEmpty()) {
        performSceneSaveAs();
        return;
    }
    performSceneSave();
}

// Shared behind onSaveScene() (File > Save Scene / the TopBar's Save
// pill) and ViewportProperties' own Save button.  Mirrors macOS
// RenderViewModel.saveScene().  Explicit-save-only (user decision
// 2026-07-12): this is the ONLY place a .RISEscene write ever happens
// from this method's callers -- UI edits mutate the live CST in memory
// and onCstSyncTick's item-1 mirror keeps the SceneEditor buffer
// following those edits live, but none of that touches disk.  Always
// alerts on refusal / I/O failure; there is no silent/suspended auto-
// save path to distinguish from.  Returns true on a successful (or
// no-op) save, false on refusal/failure.
bool MainWindow::performSceneSave()
{
    // Note: no Save-As affordance here — matches the macOS TopBar's
    // Save pill.  Save-As lives in ViewportProperties (right panel) and
    // in performSceneSaveAs() below (onSaveScene() routes there instead
    // of here when there's no loaded path).
    if (!m_viewportBridge) return false;
    const QString path = m_viewportBridge->loadedFilePath();
    if (path.isEmpty()) return false;

    QString errMsg;
    const ViewportBridge::SaveStatus status = m_viewportBridge->saveSceneTo(path, errMsg);
    switch (status) {
    case ViewportBridge::SaveStatus::Saved:
        // Pull the just-written bytes into the scene-editor pane
        // (unless it has its own unsaved text edits) and refresh the
        // window title.  Not a Save-As -- an in-place save doesn't
        // re-bump the scene's existing recents entry.
        onSceneSavedToPath(path, /*wasSaveAs=*/false);
        return true;
    case ViewportBridge::SaveStatus::NoOp:
        return true;   // Silent success — file unchanged on disk.
    case ViewportBridge::SaveStatus::Refused: {
        const QString message = errMsg.isEmpty()
            ? tr("The save engine declined to write this file.") : errMsg;
        QMessageBox::warning(this, "Save Refused", message);
        return false;
    }
    case ViewportBridge::SaveStatus::Failed: {
        const QString message = errMsg.isEmpty()
            ? tr("An I/O error occurred while saving the file.") : errMsg;
        QMessageBox::warning(this, "Save Failed", message);
        return false;
    }
    case ViewportBridge::SaveStatus::Error: {
        const QString message = tr("Unexpected save state (%1).").arg(errMsg);
        QMessageBox::warning(this, "Save Failed", message);
        return false;
    }
    }
    return false;
}

// Start-screen untitled-scene Save-As fallback (spec §5.2) + the general
// Save-As entry point onSaveScene() routes to whenever there's no loaded
// path.  Picks a destination via the native Save dialog, then reuses the
// SAME ViewportBridge::saveSceneTo/SaveStatus handling as
// performSceneSave() above -- saveSceneTo() re-anchors
// RenderEngine::loadedFilePath internally on success (see its doc), so
// every consumer (TopBar, updateWindowTitle, a subsequent in-place Save)
// picks up the new path with no further plumbing here.  Returns true on
// a successful (or no-op) save, false on cancel/refusal/failure.
bool MainWindow::performSceneSaveAs()
{
    if (!m_viewportBridge) return false;

    QSettings settings;
    QString startDir = settings.value("lastOpenDirectory").toString();
    QString defaultName = QStringLiteral("untitled.RISEscene");
    const QString loaded = m_viewportBridge->loadedFilePath();
    if (!loaded.isEmpty()) {
        const QFileInfo fi(loaded);
        startDir = fi.absolutePath();
        defaultName = fi.fileName();
    }
    const QString filePath = QFileDialog::getSaveFileName(
        this, "Save Scene As",
        startDir.isEmpty() ? defaultName : (startDir + QLatin1Char('/') + defaultName),
        "RISE Scene Files (*.RISEscene);;All Files (*)");
    if (filePath.isEmpty()) return false;   // user cancelled
    settings.setValue("lastOpenDirectory", QFileInfo(filePath).absolutePath());

    QString errMsg;
    const ViewportBridge::SaveStatus status = m_viewportBridge->saveSceneTo(filePath, errMsg);
    switch (status) {
    case ViewportBridge::SaveStatus::Saved:
        onSceneSavedToPath(filePath, /*wasSaveAs=*/true);
        // Media paths follow the scene to its new home -- vital for the
        // untitled create path, whose template load registered no project
        // root (review P2; mirrors the Mac registerMediaPaths-on-Save-As).
        m_engine->setupMediaPaths(filePath);
        return true;
    case ViewportBridge::SaveStatus::NoOp:
        return true;
    case ViewportBridge::SaveStatus::Refused: {
        const QString message = errMsg.isEmpty()
            ? tr("The save engine declined to write this file.") : errMsg;
        QMessageBox::warning(this, "Save Refused", message);
        return false;
    }
    case ViewportBridge::SaveStatus::Failed: {
        const QString message = errMsg.isEmpty()
            ? tr("An I/O error occurred while saving the file.") : errMsg;
        QMessageBox::warning(this, "Save Failed", message);
        return false;
    }
    case ViewportBridge::SaveStatus::Error: {
        const QString message = tr("Unexpected save state (%1).").arg(errMsg);
        QMessageBox::warning(this, "Save Failed", message);
        return false;
    }
    }
    return false;
}

// CST <-> scene-file live sync (item 1).  Driven at ~2 Hz by
// m_cstSyncTimer while a viewport bridge is attached (started in
// rebuildViewportForLoadedScene, stopped in teardownViewport).  Mirrors
// macOS RenderViewModel.pollRefinementState's item 1.  Explicit-save-
// only (user decision 2026-07-12): this mirrors the live CST into the
// SceneEditor buffer ONLY -- UI edits never write the .RISEscene to
// disk automatically; a write happens only from performSceneSave().
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
}

void MainWindow::onSceneSavedToPath(const QString& path, bool wasSaveAs)
{
    // Shared by TopBar's Save pill / performSceneSaveAs() (onSaveScene
    // above), the File > Save Scene menu item, and ViewportProperties'
    // own Save / Save As… buttons (connected below in
    // rebuildViewportForLoadedScene).
    if (wasSaveAs) {
        // Start-screen create path (spec §5.2) and any other explicit
        // Save-As: the scene gains a path and joins Recent Scenes.  An
        // in-place save of an already-open scene does NOT re-bump its
        // recents entry (mirrors macOS RenderViewModel's
        // saveSceneAs()/saveScene() split).
        addToRecentFiles(path);
    }
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
// "Reveal in scene file" (item 3)
// ============================================================

void MainWindow::revealEntityInSceneText(int category, const QString& name)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;

    quint64 offset = 0;
    quint32 line = 0;
    const auto cat = static_cast<ViewportBridge::Category>(category);
    if (!m_viewportBridge->getEntitySourceLocation(cat, name, &offset, &line)) return;

    setLeftTab(1);   // Scene file tab

    // The offset addresses the LIVE serialization.  When the editor
    // buffer has unsaved hand edits it may differ without being
    // shorter, so a scroll could land on the WRONG line while looking
    // authoritative.  Switch tabs only in that case (the stale-buffer
    // warning is already showing) -- a reveal must be right or absent,
    // never misleading.
    if (m_sceneEditor && !m_sceneEditor->isDirty()) {
        m_sceneEditor->revealAt(offset);
    }
}

void MainWindow::revealSourceSpan(int category, const QString& name,
                                  const QString& param, int occurrence)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;

    quint64 offset = 0;
    quint64 length = 0;
    quint32 line = 0;
    quint32 column = 0;
    const auto cat = static_cast<ViewportBridge::Category>(category);
    if (!m_viewportBridge->resolveSourceSpan(cat, name, param, occurrence,
                                             &offset, &length, &line, &column)) {
        return;
    }

    setLeftTab(1);   // Scene file tab

    // Same stale-buffer guard as revealEntityInSceneText: the offset
    // addresses the LIVE serialization, so skip the scroll (but still
    // switch tabs, where the stale-buffer warning is already showing)
    // when the editor buffer has unsaved hand edits -- a reveal must be
    // right or absent, never misleading.
    if (m_sceneEditor && !m_sceneEditor->isDirty()) {
        m_sceneEditor->revealAt(offset, length);
    }
}

void MainWindow::selectEntityAtByteOffset(quint64 byteOffset)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;

    // The offset comes FROM the buffer but resolves against the live CST
    // serialization; they only agree when the buffer is clean.  A dirty
    // buffer (unsaved hand edits) would resolve the wrong entity, so refuse
    // silently -- the inverse of the forward reveals' stale-buffer guard.
    if (!m_sceneEditor || m_sceneEditor->isDirty()) return;

    ViewportBridge::Category cat = ViewportBridge::Category::None;
    QString name;
    QString param;
    int occurrence = 0;
    if (!m_viewportBridge->sourceRefAtByteOffset(byteOffset, &cat, &name,
                                                 &param, &occurrence)) {
        return;   // inter-chunk trivia, non-entity chunk, or EOF
    }

    // Select the resolved entity.  A right-click in the text editor produces
    // no imageUpdated frame, so (unlike a render-driven refresh) we must
    // re-pull both panels explicitly -- mirrors the outliner's own selection
    // path.  Selection never mutates the document text, so this cannot dirty
    // the buffer or feed back into another resolve.  Refresh only if the
    // selection took (the resolver already validated addressability, so this
    // normally succeeds; gating avoids a stale re-snapshot on the rare miss).
    if (m_viewportBridge->setSelection(cat, name)) {
        if (m_viewportProps)    m_viewportProps->refresh();
        if (m_outlinerWidget)   m_outlinerWidget->refresh();
    }
}

// ============================================================
// Entity creation + painter CRUD (entity-creation slice)
// ============================================================
//
// All three mutating calls below take the controller's commit mutex
// (same reason as every other scene-edit call) -- gated on
// canUseSceneTransport() so a click during a production render (or an
// outstanding chat-driven render) refuses cleanly instead of wedging
// on the mutex.  Success re-selects the affected entity so the
// properties panel jumps to it; the outliner's own list refresh rides
// the next ViewportBridge::imageUpdated frame (epoch-gated), matching
// the design brief's "no per-platform refresh workaround" note.
// Failure surfaces the core's honest refusal message via QMessageBox,
// mirroring macOS RenderViewModel's presentEntityEditAlert.

void MainWindow::addEntity(int category, unsigned int templateIndex)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;
    const auto cat = static_cast<ViewportBridge::Category>(category);
    QString outName;
    QString outMessage;
    const bool applied = m_viewportBridge->instantiateEntityTemplate(cat, templateIndex, &outName, &outMessage);
    if (applied) {
        if (!outName.isEmpty()) m_viewportBridge->setSelection(cat, outName);
    } else {
        QMessageBox::warning(this, tr("Couldn't Add Entity"),
            outMessage.isEmpty() ? tr("The edit was refused.") : outMessage);
    }
}

void MainWindow::duplicateEntity(int category, const QString& name)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;
    const auto cat = static_cast<ViewportBridge::Category>(category);
    QString outName;
    QString outMessage;
    const bool applied = m_viewportBridge->duplicateEntity(cat, name, &outName, &outMessage);
    if (applied) {
        if (!outName.isEmpty()) m_viewportBridge->setSelection(cat, outName);
    } else {
        QMessageBox::warning(this, tr("Couldn't Duplicate \"%1\"").arg(name),
            outMessage.isEmpty() ? tr("The edit was refused.") : outMessage);
    }
}

void MainWindow::removeEntity(int category, const QString& name)
{
    if (!m_viewportBridge || !canUseSceneTransport()) return;
    const auto cat = static_cast<ViewportBridge::Category>(category);

    QMessageBox confirm(this);
    confirm.setWindowTitle(tr("Delete \"%1\"?").arg(name));
    confirm.setText(tr("This can be undone with Edit > Undo."));
    confirm.setIcon(QMessageBox::Warning);
    auto* deleteBtn = confirm.addButton(tr("Delete"), QMessageBox::AcceptRole);
    confirm.addButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != deleteBtn) return;

    // Re-check the edit gate AFTER the modal returns: the confirm
    // dialog blocks the event loop's caller but not Qt's event
    // processing (QMessageBox::exec spins its own loop), so a chat-
    // driven agent render can begin or end while it's up -- the gate
    // state checked at the top of this function may be stale by now.
    // Calling into the commit mutex while a render owns it would wedge
    // the UI, same hazard every other bridge call here guards against.
    if (!canUseSceneTransport()) return;

    QString outMessage;
    const bool applied = m_viewportBridge->removeEntity(cat, name, &outMessage);
    if (!applied) {
        QMessageBox::warning(this, tr("Couldn't Delete \"%1\"").arg(name),
            outMessage.isEmpty() ? tr("The edit was refused.") : outMessage);
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

    // Start-screen spec §5.4: stamp/refresh this path's last-opened time,
    // then prune meta for any path that just fell off the capped list so
    // the sibling key stays bounded too.
    m_recentFilesMeta.insert(filePath, QDateTime::currentSecsSinceEpoch());
    for (auto it = m_recentFilesMeta.begin(); it != m_recentFilesMeta.end(); ) {
        if (m_recentFiles.contains(it.key())) ++it;
        else it = m_recentFilesMeta.erase(it);
    }

    persistRecents();
    updateRecentFilesMenu();
    if (m_startWidget) m_startWidget->setRecents(m_recentFiles, m_recentFilesMeta);
}

void MainWindow::removeRecentFile(const QString& filePath)
{
    // The start screen's ✕ on a missing row, and onOpenRecentScene's own
    // missing-file handling below -- unified here so both surfaces
    // actually persist the removal (the menu-rebuild's prior in-memory-
    // only pruning did not write QSettings, so a stale entry reappeared
    // on the next launch).
    m_recentFiles.removeAll(filePath);
    m_recentFilesMeta.remove(filePath);
    persistRecents();
    updateRecentFilesMenu();
    if (m_startWidget) m_startWidget->setRecents(m_recentFiles, m_recentFilesMeta);
}

void MainWindow::persistRecents()
{
    QSettings settings;
    settings.setValue("recentSceneFiles", m_recentFiles);
    QVariantMap meta;
    for (auto it = m_recentFilesMeta.constBegin(); it != m_recentFilesMeta.constEnd(); ++it) {
        meta.insert(it.key(), it.value());
    }
    settings.setValue("recentSceneMeta", meta);
}

void MainWindow::updateRecentFilesMenu()
{
    m_recentFilesMenu->clear();

    if (m_recentFiles.isEmpty()) {
        auto* emptyAction = m_recentFilesMenu->addAction("No Recent Scenes");
        emptyAction->setEnabled(false);
    } else {
        // Existing files only, for the DROPDOWN's display -- unlike the
        // start screen's Recent scenes list (which shows missing entries
        // dimmed with a remove affordance, spec §4.1), this menu simply
        // omits them.  Does NOT mutate/persist m_recentFiles -- that list
        // is the canonical source both this menu and the start screen
        // read from; a stale entry stays until removeRecentFile() (the
        // start screen's ✕, or onOpenRecentScene's click-time removal
        // below) drops it.
        for (const QString& path : m_recentFiles) {
            if (!QFileInfo::exists(path)) continue;
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
    // Shared by File > Open Recent's per-item action and the start
    // screen's recent-row click (StartWidget::openPathRequested) /
    // drag-and-drop -- a dropped file that isn't already in recents still
    // exists by construction (just dragged from Explorer), so this guard
    // only ever fires for a recents entry whose file moved/was deleted
    // since the row was last built.
    if (!QFileInfo::exists(filePath)) {
        QMessageBox::warning(this, "File Not Found",
            QString("The file no longer exists:\n%1").arg(filePath));
        removeRecentFile(filePath);
        return;
    }

    loadSceneFile(filePath);
}

void MainWindow::onClearRecentFiles()
{
    m_recentFiles.clear();
    m_recentFilesMeta.clear();

    QSettings settings;
    settings.remove("recentSceneFiles");
    settings.remove("recentSceneMeta");

    updateRecentFilesMenu();
    if (m_startWidget) m_startWidget->setRecents(m_recentFiles, m_recentFilesMeta);
}

// ============================================================
// Scene Loading
// ============================================================

bool MainWindow::loadSceneFile(const QString& filePath, bool untitled)
{
    // A fresh attempt (Browse, a recent row, drag-drop, or Create) clears
    // any banner left over from a prior failure (spec §8).
    if (m_startWidget) m_startWidget->setErrorMessage(QString());

    // Unsaved-work gate (mirrors the Mac rounds 2+3 unified helper): covers
    // LIVE scene edits AND raw editor text, the both-dirty case, and
    // untitled scenes.  Cancel or a refused save aborts the open.
    if (!promptToSaveUnsavedWork(tr("loading a new scene"))) return false;

    // A NORMAL open must never inherit a stale create-prompt (async-failure
    // sibling of the errorOccurred clear above).
    if (!untitled && m_chatPanel) m_chatPanel->setPendingFirstPrompt(QString());

    // If a scene is already loaded, confirm the clear-and-load.
    //
    // The pre-CST "Merge" option is GONE (2026-07-12): every GUI load
    // routes through the canonical CST path, whose load-once guard
    // refuses a second load into a populated Job
    // (Job::LoadAsciiSceneViaCst: "this Job already loaded a scene;
    // re-load is not supported") -- the old Merge button could only ever
    // end in the Error state.  It was also doubly broken at the bridge
    // level even when the loader still allowed it: the merge-load
    // mutated Job/manager state under the live interactive render
    // thread, and the SceneLoaded bridge rebuild is gated on
    // !m_viewportBridge, so the old bridge's SceneEditController/CST
    // document never saw the merged chunks (and stayed permanently
    // parked after a merge-while-rendering).  If scene merging is ever
    // wanted, it is a CST-document-level feature, not a second parser
    // pass over the live Job.
    if (m_engine->state() != RenderEngine::Idle) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Scene Already Loaded");
        msgBox.setText("A scene is already loaded. Clear it and load the new scene?");
        auto* clearBtn = msgBox.addButton("Clear && Load", QMessageBox::AcceptRole);
        msgBox.addButton(QMessageBox::Cancel);
        msgBox.exec();

        if (msgBox.clickedButton() != clearBtn) {
            return false; // Cancel
        }
        // Cancel + join any in-flight render BEFORE tearing down the
        // viewport bridge: a coordinator-routed production render runs
        // through the bridge's SceneEditController
        // (RunProductionRenderThroughController), so destroying the
        // bridge first would rip that controller out from under the
        // render worker.  And the bridge must be torn down BEFORE
        // clearScene()'s ClearAll destroys the Job/manager state its
        // interactive render thread reads (mirrors macOS
        // continueClearAndLoad, whose comment records the pre-fix
        // crashes deep in IntegratePixel / DestroyContainers).
        m_engine->cancelAndJoinInFlightWork();
        teardownViewport();
        m_engine->clearScene();
    }

    // Start-screen create path (spec §5.2): an untitled scene never
    // enters recents -- there's no user-facing document path to reopen.
    if (!untitled) {
        addToRecentFiles(filePath);
    }
    if (m_chatPanel) m_chatPanel->setScenePath(filePath);   // E1: session-record scene identity
    m_engine->loadScene(filePath, untitled);
    updateWindowTitle();

    // UI redesign: the Scene file tab is always embedded (no Edit
    // toggle) — just refresh its contents from the freshly-loaded
    // file, whichever tab happens to be frontmost.
    m_sceneEditor->loadFile(filePath);
    // Untitled (start-screen create): the buffer came FROM the bundled
    // template, but the editor must never SAVE to it -- SceneEditor::save()
    // with the template's real path actively corrupted the shared asset
    // (review P1).  Clearing the editor's path makes saves guarded no-ops
    // and disables its pills with a Save-As hint.
    if (untitled) m_sceneEditor->markUntitled();
    return true;
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
// Start screen (docs/gui/START_SCREEN.md)
// ============================================================

void MainWindow::updateCenterViewStack()
{
    // Single point that decides which of {viewport pane, RenderWidget,
    // StartWidget} the center column shows -- mirrors macOS
    // ContentView's declarative `viewportBridge == nil` gate.  A bridge
    // means a scene is loaded (named or untitled); with no bridge, a
    // load in flight shows the passive loading overlay, and everything
    // else (fresh launch, after Close Scene, or a failed load) shows the
    // start screen.  Called from every place the bridge or engine state
    // can change (onStateChanged's tail, teardownViewport, the
    // constructor's initial sync).
    if (!m_viewStack) return;
    if (m_viewportBridge) {
        if (m_viewportPane) m_viewStack->setCurrentWidget(m_viewportPane);
        return;
    }
    if (m_engine && m_engine->state() == RenderEngine::Loading) {
        m_viewStack->setCurrentWidget(m_productionPaneStack);
        return;
    }
    if (m_startWidget) {
        // "Verified per render of the screen" (spec §4.1): re-stat every
        // recent path right before it becomes visible, so a file
        // deleted/moved while some OTHER screen was showing renders
        // honestly on return here.
        m_startWidget->setRecents(m_recentFiles, m_recentFilesMeta);
        m_viewStack->setCurrentWidget(m_startWidget);
    }
}

QString MainWindow::resolveStarterScenePath() const
{
    // Start-screen create path (spec §5.1/§5.3): the build-output copy
    // the .vcxproj's post-build step places at $(OutDir)Templates\ next
    // to the .exe.  Falls back to the repo-relative canonical path for a
    // dev run from an unusual working directory -- a convenience for
    // this platform only (there is no notion of a repo-relative fallback
    // on an end-user install; a missing bundled copy there is a real
    // packaging bug, surfaced as the error banner below rather than
    // silently reaching for a source tree that won't exist).
    const QString bundled = QCoreApplication::applicationDirPath()
        + QStringLiteral("/Templates/empty_starter.RISEscene");
    if (QFileInfo::exists(bundled)) return bundled;

    const QString repoRelative = QStringLiteral("scenes/Templates/empty_starter.RISEscene");
    if (QFileInfo::exists(repoRelative)) return repoRelative;

    return QString();
}

void MainWindow::onCreateSceneFromPrompt(const QString& prompt)
{
    // Start-screen create path (spec §4.3): resolve the bundled starter
    // template, stash the prompt on the chat panel for its NEXT fresh
    // bridge attach to consume exactly once (mirrors macOS
    // ChatViewModel.pendingFirstPrompt / sceneOpened -- see
    // ChatPanel::setPendingFirstPrompt's doc), switch to the Agent tab,
    // then load the template as an untitled scene through the SAME
    // loadSceneFile() path a normal Open uses.
    const QString templatePath = resolveStarterScenePath();
    if (templatePath.isEmpty()) {
        if (m_startWidget) {
            m_startWidget->setErrorMessage(
                tr("The starter scene template is missing from the app's install."));
            m_startWidget->setCreateInFlight(false);
        }
        return;
    }

    if (m_chatPanel) m_chatPanel->setPendingFirstPrompt(prompt);

    // Force the left panel + Agent tab visible so the user watches the
    // agent build (spec §4.3 step 4), keeping the View > Left Panel
    // checkbox honest about the panel's actual visibility.
    if (m_leftPanelAction) m_leftPanelAction->setChecked(true);
    if (m_leftPanel) m_leftPanel->setVisible(true);
    setLeftTab(0);

    // No explicit createInFlight reset on an ASYNC failure: a successful
    // load switches the center stack away from StartWidget entirely (see
    // updateCenterViewStack), and an async load failure re-enables
    // Create via the errorOccurred handler above.  A SYNC bail-out is
    // still possible, though -- Close Scene doesn't clear the SceneEditor
    // buffer, so a stray unsaved text edit left over from a scene closed
    // earlier this session can still trigger loadSceneFile()'s "Unsaved
    // Changes" confirm even with no scene currently loaded; Cancelling
    // that (or, in principle, the "already loaded" confirm) must reset
    // the button and drop the stash rather than leaving "Creating…"
    // stuck and a prompt armed to fire on some later, unrelated load.
    if (!loadSceneFile(templatePath, /*untitled=*/true)) {
        if (m_startWidget) m_startWidget->setCreateInFlight(false);
        if (m_chatPanel) m_chatPanel->setPendingFirstPrompt(QString());
    }
}

// ============================================================
// Render controls
// ============================================================

// The single unsaved-work gate for destructive scene transitions (Close
// Scene, load-over) -- mirrors macOS RenderViewModel.promptToSaveUnsavedWork
// (rounds 2+3): covers unsaved LIVE scene edits (Save routes untitled scenes
// to Save-As) AND unsaved RAW editor text, including the BOTH-dirty case
// where the editor buffer is a DIVERGENT variant of the just-saved scene --
// writing it to the same file would clobber that save, so it gets an
// explicit offer to save to a SEPARATE file.  Returns true to proceed with
// the transition, false to abort it.
bool MainWindow::promptToSaveUnsavedWork(const QString& action)
{
    const bool sceneDirty = m_viewportBridge
        && m_viewportBridge->hasUnsavedSceneChanges();
    const bool editorDirty = m_sceneEditor && m_sceneEditor->isDirty();
    if (!sceneDirty && !editorDirty) return true;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Unsaved Changes"));
    if (sceneDirty && editorDirty) {
        box.setText(tr("The scene has unsaved changes, and the scene editor "
                       "has separate unsaved text changes. Save the scene "
                       "before %1?").arg(action));
    } else if (sceneDirty) {
        box.setText(tr("The scene has unsaved changes. Save before %1?").arg(action));
    } else {
        box.setText(tr("The scene editor has unsaved text changes. Save before %1?").arg(action));
    }
    QPushButton* saveBtn = box.addButton(tr("Save"), QMessageBox::AcceptRole);
    box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    QPushButton* cancelBtn = box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == cancelBtn) return false;
    if (box.clickedButton() != saveBtn) return true;   // Discard

    if (sceneDirty) {
        const bool untitled = m_viewportBridge->loadedFilePath().isEmpty();
        const bool ok = untitled ? performSceneSaveAs() : performSceneSave();
        if (!ok) return false;
        if (m_sceneEditor && m_sceneEditor->isDirty()) {
            // Both-dirty follow-up (Mac round-3 parity): the raw editor text
            // was NOT part of the scene save and would overwrite it if
            // written to the same file -- offer a separate destination.
            QMessageBox follow(this);
            follow.setIcon(QMessageBox::Warning);
            follow.setWindowTitle(tr("Scene editor text not included"));
            follow.setText(tr("The scene editor's unsaved text changes were "
                              "not part of the scene save (writing them to "
                              "the same file would overwrite it). Save the "
                              "editor text to a separate file?"));
            QPushButton* saveAsBtn = follow.addButton(tr("Save As..."), QMessageBox::AcceptRole);
            follow.addButton(tr("Discard Text"), QMessageBox::DestructiveRole);
            QPushButton* followCancel = follow.addButton(QMessageBox::Cancel);
            follow.exec();
            if (follow.clickedButton() == followCancel) return false;
            if (follow.clickedButton() == saveAsBtn) {
                if (!saveEditorTextViaDialog()) return false;
            }
        }
        return true;
    }
    // Editor-only dirt.
    if (!m_sceneEditor->filePath().isEmpty()) {
        m_sceneEditor->save();
        return !m_sceneEditor->isDirty();   // save() alerts on IO failure
    }
    return saveEditorTextViaDialog();
}

// Save the raw editor text to a user-chosen path (always dialogs, never the
// scene's own file).  On success the editor adopts the written file via
// loadFile() so its dirty baseline resets.  False on cancel / IO error.
bool MainWindow::saveEditorTextViaDialog()
{
    const QString filePath = QFileDialog::getSaveFileName(
        this, tr("Save Scene As"), QStringLiteral("untitled.RISEscene"),
        tr("RISE Scene Files (*.RISEscene);;All Files (*)"));
    if (filePath.isEmpty()) return false;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Could not save file: %1").arg(filePath));
        return false;
    }
    QTextStream out(&file);
    out << m_sceneEditor->text();
    file.close();
    // Adopt so the buffer reads clean (the scene itself stays whatever it
    // was -- this is a raw-text export, not a scene re-anchor).
    m_sceneEditor->loadFile(filePath);
    return true;
}

void MainWindow::onClear()
{
    // Dirty-close prompt (spec §5.2; mirrors the Mac clearScene() fix —
    // Close previously discarded unsaved work silently for named AND
    // untitled scenes).  Both dirt kinds gate: unsaved LIVE scene edits
    // (save routes untitled scenes to Save-As) and unsaved RAW editor
    // text.  A cancelled prompt or a failed/refused save aborts the close.
    // Unsaved-work gate (spec §5.2; mirrors the Mac rounds 2+3 unified
    // helper -- covers both dirt kinds, the both-dirty case, and untitled
    // scenes).  Cancel or a refused save aborts the close.
    if (!promptToSaveUnsavedWork(tr("closing"))) return;

    // Close Scene is menu-gated off while Rendering/Cancelling, but make
    // the join-before-teardown ordering structural rather than
    // gate-dependent (see loadSceneFile's matching comment): the
    // teardown below destroys the SceneEditController a coordinator-
    // routed render would still be running through.  No-op when nothing
    // is in flight.
    m_engine->cancelAndJoinInFlightWork();
    teardownViewport();
    m_engine->clearScene();
    updateWindowTitle();
    updateStatusBar();
    // Close Scene returns to the start screen (spec §8) -- clear any
    // stale error banner / in-flight guard from a PRIOR visit so it
    // doesn't leak into this one.  updateCenterViewStack() (called from
    // teardownViewport()/the clearScene()-driven state change above)
    // already refreshes the recents snapshot.
    if (m_startWidget) {
        m_startWidget->setErrorMessage(QString());
        m_startWidget->setCreateInFlight(false);
    }
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

    // Capture the canonical scrubbed time for the production worker.
    // The worker advances scene state and regenerates photon maps inside
    // the coordinated render path, rather than blocking this UI handler.
    //
    // We prefer the controller's lastSceneTime over
    // m_viewportTimeline->currentTime because Undo / Redo can change
    // scene time without touching the slider — passing the slider's
    // local value in that window would roll the scene back to a
    // stale time.  Fall back to the slider when the viewport bridge
    // is absent (no scene loaded, no scrubs possible).
    double sceneTime = m_viewportTimeline ? m_viewportTimeline->currentTime() : 0.0;
    if (m_viewportBridge) sceneTime = m_viewportBridge->lastSceneTime();
    m_engine->startRender(sceneTime);
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
        // Which of {RenderWidget, StartWidget, the viewport pane} the
        // center stack shows is now decided in exactly one place --
        // updateCenterViewStack(), called at this function's tail.
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
    // Start screen: the single place that decides {viewport pane,
    // RenderWidget, StartWidget} -- covers every transition this
    // function can produce (bridge built/torn down above, or a bare
    // state change like Loading/Error with no bridge either way).
    updateCenterViewStack();
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
    // Same join-then-teardown ordering as loadSceneFile / onClear (see
    // loadSceneFile's comment): the bridge's interactive render thread
    // must be stopped before clearScene()'s ClearAll destroys the
    // manager state it reads, and any in-flight render must be joined
    // before the bridge's controller is destroyed.
    m_engine->cancelAndJoinInFlightWork();
    teardownViewport();
    m_engine->clearScene();
    m_engine->loadScene(filePath);
}

void MainWindow::updateWindowTitle()
{
    QString title;
    if (!m_engine->loadedFilePath().isEmpty()) {
        title = QFileInfo(m_engine->loadedFilePath()).fileName();
    } else if (m_viewportBridge) {
        // Untitled scene (start-screen create path, spec §5.2): a scene
        // is loaded (the bundled starter template) but has no path yet
        // -- Save routes to Save-As.  TopBar infers the same state from
        // (empty path, bridge present) below.
        title = "Untitled";
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

    // CST <-> scene-file live sync (item 1): a freshly-attached bridge's
    // current version becomes the sync baseline -- the bytes that were
    // just loaded and the live CST agree at this instant, so there is
    // nothing to pull into the editor yet.  Only a LATER agent/GUI edit
    // that bumps the revision should overwrite the SceneEditor buffer.
    // Also resets the per-scene stale-warning state so it doesn't leak
    // across scene loads.  Mirrors macOS RISEViewportBridge's
    // `viewportBridge` didSet baseline-setting.  Gated on
    // canUseSceneTransport() like every other getSceneTextVersion()/
    // serializedSceneText() call site (see onCstSyncTick's doc) -- every
    // caller of this method only reaches it once renderState has
    // already left Loading, so this is always true in practice, but the
    // gate is kept anyway rather than assuming that invariant holds
    // forever.
    {
        quint64 uuid = 0, revision = 0;
        m_lastSyncedSceneTextVersion =
            (canUseSceneTransport() && m_viewportBridge->getSceneTextVersion(&uuid, &revision))
            ? SceneTextVersion{ uuid, revision, true } : SceneTextVersion{};
    }
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

    // N-up multi-viewport (docs/gui/RENDER_MODES.md §7.5): the layout
    // picker lives in the toolbar; the pane grid it drives lives in the
    // viewport widget -- resync immediately on a click instead of waiting
    // for the widget's own 500ms poll.
    connect(m_viewportToolbar, &ViewportToolbar::layoutChanged,
            m_viewportWidget, &ViewportWidget::onViewportLayoutChanged);

    if (m_outlinerWidget) {
        m_outlinerWidget->setBridge(m_viewportBridge);
        // A pick in the outliner (category open/close, entity select)
        // doesn't itself touch the interactive rasterizer, so there's
        // no imageUpdated to ride -- follow it explicitly so the
        // single-entity inspector below updates immediately.
        connect(m_outlinerWidget, &OutlinerWidget::selectionActivated,
                m_viewportProps, &ViewportProperties::refresh);
    }

    if (m_environmentPanel) {
        m_environmentPanel->setBridge(m_viewportBridge);
        // An environment edit doesn't necessarily touch the PRIMARY
        // selection, but Add/Remove change the Painter list and a live
        // edit re-renders -- follow it so the single-entity inspector
        // stays in sync (same persistent-to-per-scene connect as the
        // outliner's selectionActivated above; m_viewportProps is
        // recreated per load so this reconnects fresh each time).
        connect(m_environmentPanel, &EnvironmentPanel::environmentEdited,
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
    // N-up multi-viewport (§7): secondary-pane (1..kViewportPaneCount-1)
    // frames arrive on this separate signal -- pane 0's keep riding
    // imageUpdated above (see ViewportBridge::paneImageUpdated's doc).
    connect(m_viewportBridge, &ViewportBridge::paneImageUpdated,
            m_viewportWidget, &ViewportWidget::setPaneImage);
    connect(m_viewportBridge, &ViewportBridge::imageUpdated,
            m_viewportProps,  &ViewportProperties::refresh);
    if (m_outlinerWidget) {
        connect(m_viewportBridge, &ViewportBridge::imageUpdated,
                m_outlinerWidget, &OutlinerWidget::refresh);
    }
    if (m_environmentPanel) {
        // Rides every preview frame like the outliner so a scene load /
        // active-rasterizer change / undone edit re-reads environmentInfo
        // (reload() self-skips while one of its own fields has focus, so a
        // frame can't blow away an in-progress edit).
        connect(m_viewportBridge, &ViewportBridge::imageUpdated,
                m_environmentPanel, &EnvironmentPanel::reload);
    }

    // Round-trip save success: pull the just-written bytes into the
    // SceneEditor text pane so it reflects the saved edits, refresh
    // the window title, and surface a warning if the user also has
    // unsaved text-editor edits.  Shared with TopBar's Save pill /
    // File > Save Scene via the onSceneSavedToPath slot.
    connect(m_viewportProps, &ViewportProperties::sceneSavedToPath,
            this, &MainWindow::onSceneSavedToPath);

    // "Reveal in scene file" (item 3): m_viewportProps is per-scene
    // (recreated above), so this connects fresh each load -- the OLD
    // instance's connection to `this` is auto-dropped when it's
    // destroyed by teardownViewport, matching the sceneSavedToPath
    // connect just above.  Adapts the Category-typed signal to the
    // int-typed slot for the same reason as the outliner's connect in
    // buildRightPanel (see revealEntityInSceneText's doc).
    connect(m_viewportProps, &ViewportProperties::revealRequested, this,
            [this](ViewportBridge::Category category, const QString& name) {
                revealEntityInSceneText(static_cast<int>(category), name);
            });

    // Per-param reveal (source traceability): a property row's context
    // menu resolves to the param's EXACT span.  Same Category-to-int adapt
    // as revealRequested above; occurrence 0 (the first matching param).
    connect(m_viewportProps, &ViewportProperties::revealParamRequested, this,
            [this](ViewportBridge::Category category, const QString& name, const QString& param) {
                revealSourceSpan(static_cast<int>(category), name, param, 0);
            });

    // Production-render frames from the engine → also flow to the
    // viewport widget so clicking "Render" updates the live view.
    connect(m_engine, &RenderEngine::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
}

void MainWindow::teardownViewport()
{
    if (!m_viewportBridge) return;

    // CST <-> scene-file live sync (item 1): no bridge, no sync -- stop
    // polling and drop all per-scene state so it can't leak into the
    // next scene load.
    if (m_cstSyncTimer) m_cstSyncTimer->stop();
    m_lastSyncedSceneTextVersion = SceneTextVersion{};
    if (m_sceneEditor) m_sceneEditor->setBehindLiveScene(false);

    if (m_chatPanel) {
        m_chatPanel->setViewportBridge(nullptr);
        // (master merge) clear the trajectory scene identity; the
        // pre-redesign hide()/m_chatVisible teardown does NOT apply —
        // the redesigned chat panel is a permanent left-panel tab.
        m_chatPanel->setScenePath(QString());
    }
    if (m_topBar) {
        m_topBar->setViewportBridge(nullptr);
        m_topBar->setSceneEditsDirty(false);
    }
    if (m_outlinerWidget) {
        m_outlinerWidget->setBridge(nullptr);
    }
    if (m_environmentPanel) {
        // Drops the bridge and hides the section (environmentInfo returns
        // false with no controller).  m_environmentPanel is persistent, so
        // it is NOT deleted here -- only its bridge borrow is cleared.
        m_environmentPanel->setBridge(nullptr);
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
    // Fall back to the loading overlay or the start screen, whichever
    // the current engine state calls for (start screen: no scene loaded
    // is the common case here -- fresh launch, Close Scene, a failed
    // load).
    updateCenterViewStack();
}
