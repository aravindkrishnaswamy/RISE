//////////////////////////////////////////////////////////////////////
//
//  MainWindow.cpp - Main window implementation.
//
//  Ported from the Mac app's ContentView.swift + RISEApp.swift.
//
//////////////////////////////////////////////////////////////////////

#include "MainWindow.h"
#include "RenderEngine.h"
#include "RenderWidget.h"
#include "ControlsWidget.h"
#include "LogWidget.h"
#include "SceneEditor.h"
#include "ViewportBridge.h"
#include "ViewportWidget.h"
#include "ViewportToolbar.h"
#include "ViewportTimeline.h"
#include "ViewportProperties.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QFileInfo>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setMinimumSize(900, 600);

    // Load recent files from settings
    QSettings settings;
    m_recentFiles = settings.value("recentSceneFiles").toStringList();

    // Create the engine
    m_engine = new RenderEngine(this);

    updateWindowTitle();

    // Create widgets
    m_renderWidget = new RenderWidget();
    m_controlsWidget = new ControlsWidget();
    m_logWidget = new LogWidget();
    m_sceneEditor = new SceneEditor();

    // Bottom splitter: controls (260px) | log
    m_bottomSplitter = new QSplitter(Qt::Horizontal);
    m_bottomSplitter->addWidget(m_controlsWidget);
    m_bottomSplitter->addWidget(m_logWidget);
    m_bottomSplitter->setSizes({260, 600});
    m_bottomSplitter->setStretchFactor(0, 0);
    m_bottomSplitter->setStretchFactor(1, 1);

    // Stacked widget toggles between the passive render view and the
    // interactive viewport pane.  The viewport pane is built lazily
    // when a scene loads (see rebuildViewportForLoadedScene).
    m_viewStack = new QStackedWidget();
    m_viewStack->addWidget(m_renderWidget);   // index 0

    // Right splitter: stack | bottom panel (280px fixed height)
    m_rightSplitter = new QSplitter(Qt::Vertical);
    m_rightSplitter->addWidget(m_viewStack);
    m_rightSplitter->addWidget(m_bottomSplitter);
    m_rightSplitter->setSizes({500, 280});
    m_rightSplitter->setStretchFactor(0, 1);
    m_rightSplitter->setStretchFactor(1, 0);

    // Main splitter: editor (hidden) | right content
    m_mainSplitter = new QSplitter(Qt::Horizontal);
    m_mainSplitter->addWidget(m_sceneEditor);
    m_mainSplitter->addWidget(m_rightSplitter);
    m_mainSplitter->setSizes({0, 1024});
    m_sceneEditor->hide();

    setCentralWidget(m_mainSplitter);

    createMenuBar();
    createStatusBar();

    // Connect engine signals
    connect(m_engine, &RenderEngine::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_engine, &RenderEngine::progressUpdated, m_controlsWidget, &ControlsWidget::updateProgress);
    connect(m_engine, &RenderEngine::progressUpdated, m_renderWidget, [this](double fraction, const QString&) {
        m_renderWidget->setProgress(fraction);
    });
    connect(m_engine, &RenderEngine::imageUpdated, m_renderWidget, &RenderWidget::updateImage);
    connect(m_engine, &RenderEngine::sceneSizeDetected, this, &MainWindow::onSceneSizeDetected);
    connect(m_engine, &RenderEngine::logMessage, m_logWidget, &LogWidget::appendLog);
    connect(m_engine, &RenderEngine::elapsedTimeUpdated, m_controlsWidget, &ControlsWidget::updateElapsedTime);
    connect(m_engine, &RenderEngine::remainingTimeUpdated, m_controlsWidget, &ControlsWidget::updateRemainingTime);
    connect(m_engine, &RenderEngine::hasAnimationChanged, m_controlsWidget, &ControlsWidget::setHasAnimation);
    connect(m_engine, &RenderEngine::errorOccurred, this, [this](const QString& msg) {
        statusBar()->showMessage("Error: " + msg, 5000);
    });

    // Connect controls signals
    connect(m_controlsWidget, &ControlsWidget::openSceneClicked, this, &MainWindow::onOpenScene);
    connect(m_controlsWidget, &ControlsWidget::editClicked, this, &MainWindow::onEditToggle);
    connect(m_controlsWidget, &ControlsWidget::clearClicked, this, &MainWindow::onClear);
    connect(m_controlsWidget, &ControlsWidget::renderClicked, this, &MainWindow::onRender);
    connect(m_controlsWidget, &ControlsWidget::renderAnimationClicked, this, &MainWindow::onRenderAnimation);
    connect(m_controlsWidget, &ControlsWidget::cancelClicked, this, &MainWindow::onCancel);

    // Connect editor signals
    connect(m_sceneEditor, &SceneEditor::closeRequested, this, &MainWindow::onEditToggle);
    connect(m_sceneEditor, &SceneEditor::saveAndReloadRequested, this, &MainWindow::onSaveAndReload);

    // Set initial status
    statusBar()->showMessage(QString("RISE %1 \u2014 Ready").arg(m_engine->versionString()));
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

    auto* saveAction = fileMenu->addAction("&Save Scene", [this]() {
        if (m_sceneEditor->isVisible()) m_sceneEditor->save();
    });
    saveAction->setShortcut(QKeySequence::Save);

    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction("E&xit", this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    // --- Edit menu ---
    auto* editMenu = menuBar()->addMenu("&Edit");
    auto* undoAction = editMenu->addAction("&Undo");
    undoAction->setShortcut(QKeySequence::Undo);
    auto* redoAction = editMenu->addAction("&Redo");
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    auto* findAction = editMenu->addAction("&Find...");
    findAction->setShortcut(QKeySequence::Find);

    // --- Render menu ---
    auto* renderMenu = menuBar()->addMenu("&Render");
    renderMenu->addAction("&Render", this, &MainWindow::onRender);
    renderMenu->addAction("Render &Animation", this, &MainWindow::onRenderAnimation);
    renderMenu->addSeparator();
    renderMenu->addAction("&Cancel", this, &MainWindow::onCancel);
}

void MainWindow::createStatusBar()
{
    statusBar()->setSizeGripEnabled(true);
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
    if (m_sceneEditor->isVisible() && m_sceneEditor->isDirty()) {
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

    // Auto-open editor when scene loads (matching Mac app behavior)
    if (!m_editorVisible) {
        onEditToggle();
    } else {
        // Refresh editor contents if already visible
        m_sceneEditor->loadFile(filePath);
    }
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
// Editor
// ============================================================

void MainWindow::onEditToggle()
{
    m_editorVisible = !m_editorVisible;

    if (m_editorVisible) {
        // Load file into editor if we have a scene
        if (!m_engine->loadedFilePath().isEmpty()) {
            m_sceneEditor->loadFile(m_engine->loadedFilePath());
        }
        m_sceneEditor->show();
        m_mainSplitter->setSizes({600, m_mainSplitter->width() - 600});
    } else {
        m_sceneEditor->hide();
        m_mainSplitter->setSizes({0, m_mainSplitter->width()});
    }
}

// ============================================================
// Render controls
// ============================================================

void MainWindow::onClear()
{
    teardownViewport();
    m_engine->clearScene();
    m_controlsWidget->setHasScene(false);
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::onRender()
{
    // Stop the viewport's render thread BEFORE the production
    // rasterizer runs — they'd race against the same scene state
    // otherwise.  The viewport restarts in onStateChanged when
    // production transitions back to Completed/Cancelled/Error.
    if (m_viewportBridge) m_viewportBridge->stop();

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
    // Derive video output path from scene file
    QString scenePath = m_engine->loadedFilePath();
    QString videoPath = scenePath;
    videoPath.replace(".RISEscene", ".mp4");

    if (m_viewportBridge) m_viewportBridge->stop();
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
    m_controlsWidget->setRenderState(state);

    bool hasScene = (state != RenderEngine::Idle);
    m_controlsWidget->setHasScene(hasScene);

    // When the engine has just finished loading a scene, build the
    // viewport bridge over it and switch to the viewport pane.  When
    // the scene is cleared, tear it down.  No "interact mode" toggle:
    // the viewport is always visible once a scene is loaded.
    if (state == RenderEngine::SceneLoaded && !m_viewportBridge) {
        rebuildViewportForLoadedScene();
        if (m_viewportPane) m_viewStack->setCurrentWidget(m_viewportPane);
        if (m_viewportBridge) {
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
    // can keep editing on the freshly-rendered scene state.  Suppress
    // the very next preview frame at the sink layer so the production
    // image stays on screen until the user actually starts dragging —
    // otherwise the bridge's initial render would flash a half-rendered
    // preview right after a clean production result.
    const bool renderEnded = (state == RenderEngine::Completed
                          || state == RenderEngine::Cancelled
                          || state == RenderEngine::Error);
    if (renderEnded && m_viewportBridge && !m_viewportBridge->isRunning()) {
        m_viewportBridge->suppressNextFrame();
        m_viewportBridge->start();
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

    // Disable Open Recent during active operations
    m_recentFilesMenu->setEnabled(interacting);

    updateStatusBar();
    updateWindowTitle();
}

void MainWindow::onSceneSizeDetected(int width, int height)
{
    // Auto-resize window to fit scene, matching Mac app behavior
    int bottomHeight = 280;
    int statusHeight = 30;
    int menuHeight = menuBar()->height();

    int targetW = width + (m_editorVisible ? 600 : 0) + 40;
    int targetH = height + bottomHeight + menuHeight + statusHeight + 40;

    // Clamp to screen size with smart scaling
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        QRect available = screen->availableGeometry();
        int maxW = available.width() - 50;
        int maxH = available.height() - 50;

        if (targetW > maxW || targetH > maxH) {
            // Scale down while preserving aspect ratio of render area
            double scaleW = static_cast<double>(maxW - (m_editorVisible ? 600 : 0) - 40) / width;
            double scaleH = static_cast<double>(maxH - bottomHeight - menuHeight - statusHeight - 40) / height;
            double scale = qMin(scaleW, scaleH);
            scale = qMin(scale, 1.0); // Don't upscale

            targetW = static_cast<int>(width * scale) + (m_editorVisible ? 600 : 0) + 40;
            targetH = static_cast<int>(height * scale) + bottomHeight + menuHeight + statusHeight + 40;
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
    title += QString(" \u2014 RISE %1").arg(m_engine->versionString());
    setWindowTitle(title);
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

    statusBar()->showMessage(QString("RISE %1 \u2014 %2")
        .arg(m_engine->versionString(), stateText));
}

// ============================================================
// Interactive viewport
// ============================================================

void MainWindow::rebuildViewportForLoadedScene()
{
    teardownViewport();

    m_viewportBridge = new ViewportBridge(m_engine, this);

    m_viewportToolbar  = new ViewportToolbar();
    m_viewportWidget   = new ViewportWidget(m_viewportBridge);
    m_viewportTimeline = new ViewportTimeline();
    m_viewportProps    = new ViewportProperties(m_viewportBridge);
    m_viewportTimeline->setVisible(m_engine->hasAnimation());

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
        }
    }

    // Compose the pane: VBox{ toolbar, viewport, timeline } | properties
    auto* col = new QVBoxLayout;
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(m_viewportToolbar);
    col->addWidget(m_viewportWidget, 1);
    col->addWidget(m_viewportTimeline);
    auto* leftSide = new QWidget;
    leftSide->setLayout(col);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(leftSide, 1);
    row->addWidget(m_viewportProps);
    m_viewportPane = new QWidget;
    m_viewportPane->setLayout(row);
    m_viewStack->addWidget(m_viewportPane);   // index 1

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
    connect(m_viewportToolbar,  &ViewportToolbar::undoClicked,
            m_viewportBridge,   &ViewportBridge::undo);
    connect(m_viewportToolbar,  &ViewportToolbar::redoClicked,
            m_viewportBridge,   &ViewportBridge::redo);

    connect(m_viewportTimeline, &ViewportTimeline::scrubBegin,
            m_viewportBridge,   &ViewportBridge::scrubTimeBegin);
    connect(m_viewportTimeline, &ViewportTimeline::scrubEnd,
            m_viewportBridge,   &ViewportBridge::scrubTimeEnd);
    connect(m_viewportTimeline, &ViewportTimeline::timeChanged,
            m_viewportBridge,   &ViewportBridge::scrubTime);

    // Live-preview frames from the bridge \u2192 viewport widget + props refresh.
    connect(m_viewportBridge, &ViewportBridge::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
    connect(m_viewportBridge, &ViewportBridge::imageUpdated,
            m_viewportProps,  &ViewportProperties::refresh);

    // Production-render frames from the engine \u2192 also flow to the
    // viewport widget so clicking "Render" updates the live view.
    connect(m_engine, &RenderEngine::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
}

void MainWindow::teardownViewport()
{
    if (!m_viewportBridge) return;

    // Stop the render thread BEFORE the bridge dies.
    m_viewportBridge->stop();

    if (m_viewportPane) {
        m_viewStack->removeWidget(m_viewportPane);
        delete m_viewportPane;
        m_viewportPane = nullptr;
        m_viewportToolbar = nullptr;
        m_viewportWidget = nullptr;
        m_viewportTimeline = nullptr;
        m_viewportProps = nullptr;
    }
    delete m_viewportBridge;
    m_viewportBridge = nullptr;
    // Fall back to the passive RenderWidget when no scene is loaded.
    m_viewStack->setCurrentIndex(0);
}

