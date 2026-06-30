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
#include "HDRRenderWidget.h"
#include "ControlsWidget.h"
#include "LogWidget.h"
#include "SceneEditor.h"
#include "ViewportBridge.h"
#include "ViewportWidget.h"
#include "ViewportToolbar.h"
#include "ViewportTimeline.h"
#include "ViewportProperties.h"

#include <QAction>
#include <QActionGroup>

#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QFileInfo>
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
    m_hdrRenderWidget = new HDRRenderWidget();  // L5b — Windows HDR
    m_controlsWidget = new ControlsWidget();
    m_logWidget = new LogWidget();
    m_sceneEditor = new SceneEditor();

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
    // L5b: at index 0 we now host the production-pane sub-stack
    // (SDR | HDR) instead of the bare RenderWidget; the View > HDR
    // Preview toggle flips the inner stack.
    m_viewStack = new QStackedWidget();
    m_viewStack->addWidget(m_productionPaneStack);   // index 0

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
    // L5b — HDR signal routes to the HDR widget.  Both are wired
    // unconditionally; the engine emits one or the other based on
    // its mode flag.
    connect(m_engine, &RenderEngine::hdrImageUpdated,
            m_hdrRenderWidget, &HDRRenderWidget::updateHDRImage);
    connect(m_hdrRenderWidget, &HDRRenderWidget::hdrAvailabilityChanged,
            this, &MainWindow::onHDRAvailabilityChanged);
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

    // The active named animation is picked in the right-side panel's
    // "Animation" accordion category — no dropdown wiring needed here.

    // L5e — exposure slider drives engine.setViewExposureEV.
    // Engine is mid-render-safe (atomic + Repaint, no rasterizer
    // re-run), so even rapid drag updates settle in <1 frame.
    connect(m_controlsWidget, &ControlsWidget::exposureChanged,
            m_engine, &RenderEngine::setViewExposureEV);

    // Connect editor signals
    connect(m_sceneEditor, &SceneEditor::closeRequested, this, &MainWindow::onEditToggle);
    connect(m_sceneEditor, &SceneEditor::saveAndReloadRequested, this, &MainWindow::onSaveAndReload);

    // Set initial status
    statusBar()->showMessage(QString("RISE %1 \u2014 Ready").arg(m_engine->versionString()));

    // L5b \u2014 initial HDR-availability probe.  HDRRenderWidget is at
    // index 1 of the production sub-stack and so won't fire its own
    // Show event until the toggle is flipped \u2014 chicken-and-egg.
    // The static probe enumerates all DXGI outputs without needing
    // a swap chain.
    onHDRAvailabilityChanged(HDRRenderWidget::probeAnyAdapterHDRAvailable());
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

    // L5d — Save Rendered Image action (parity with macOS File >
    // Save Rendered Image…).  Disabled until the engine has at
    // least started a render; remains enabled across the rest of
    // the app's lifetime so the user can save the LAST rendered
    // result even after a cancel or scene reload's Render click.
    // The QFileDialog filter defaults to EXR (HDR archival) with
    // PNG / TIFF as LDR alternatives.
    m_saveImageAction = fileMenu->addAction("Save Rendered &Image...",
        this, &MainWindow::onSaveRenderedImage);
    m_saveImageAction->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_saveImageAction->setEnabled(false);  // toggled by onStateChanged

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

    // --- View menu ---
    // L5b — adds the HDR Preview toggle.  Disabled by default; the
    // HDRRenderWidget enables it once it confirms the active monitor
    // reports an HDR colorspace + max luminance > SDR (queried via
    // IDXGIOutput6::GetDesc1 on widget creation and on screen-change
    // events).  Same UX pattern as the macOS ContentView toggle
    // gated by NSScreen.maximumPotentialExtendedDynamicRangeColor-
    // ComponentValue.
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
    // Greyed out when HDR Preview is on (HDR display path is
    // by-construction tone-curve-free).
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

    // --- Render menu ---
    auto* renderMenu = menuBar()->addMenu("&Render");
    renderMenu->addAction("&Render", this, &MainWindow::onRender);
    renderMenu->addAction("Render &Animation", this, &MainWindow::onRenderAnimation);
    renderMenu->addSeparator();
    renderMenu->addAction("&Cancel", this, &MainWindow::onCancel);
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
    // L5e round-2 — Same gate for the exposure slider.  Applying
    // exposure on top of the HDR compositor's dynamic-range map
    // double-maps the radiance signal — flicker / hue shifts on
    // HDR-capable monitors.
    if (m_controlsWidget) {
        m_controlsWidget->setHDREnabled(checked);
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
    // The "Animation" accordion category is rebuilt with the viewport
    // (teardownViewport above destroys the old ViewportProperties); the
    // next scene load repopulates it.  Nothing animation-specific to
    // reset on the controls panel anymore.
    updateWindowTitle();
    updateStatusBar();
}

void MainWindow::onRender()
{
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

    // Phase 7c -- when the active rasterizer is the auto-dispatcher, surface the
    // concrete integrator it resolved to (mirrors the macOS status bar).
    if (m_engine->state() == RenderEngine::Completed) {
        const QString integ = m_engine->autoResolvedIntegrator();
        if (!integ.isEmpty()) {
            stateText += QString(" \u00b7 Auto \u2192 %1").arg(integ.toUpper());
        }
        // Animation export: surface the video file(s) written so the user
        // doesn't have to open the log to confirm the .mov / .mp4 landed.
        const QString animOut = m_engine->lastAnimationOutputsSummary();
        if (!animOut.isEmpty()) {
            stateText += QString(" \u00b7 %1").arg(animOut);
        }
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
    m_viewportToolbar->setBridge(m_viewportBridge);
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
            m_viewportTimeline->setAnimationFrameCount(nf);
        }
    }

    // The scene's named animations are surfaced by the ViewportProperties
    // panel's "Animation" accordion category (constructed just above) —
    // it pulls the list from the bridge's generic categoryEntities() and
    // activates a pick via setSelection(Category::Animation, …).  The
    // timeline still follows the active animation via animationOptions().

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

    // Round-trip save success: pull the just-written bytes into the
    // SceneEditor text pane so it reflects the saved edits, refresh
    // the window title (Save-As may have re-anchored loadedFilePath
    // inside ViewportBridge::saveSceneTo already, but title update is
    // a GUI concern this lambda owns), and surface a warning if the
    // user also has unsaved text-editor edits (clicking the editor's
    // own Save would otherwise overwrite the just-saved interactive
    // changes \u2014 adversarial-review round 1 P1).
    //
    // The Save-As path re-anchor of `m_engine->m_loadedFilePath` is
    // owned by ViewportBridge::saveSceneTo (Phase 6.5); we do NOT
    // re-anchor here to avoid a duplicate write \u2014 adversarial-review
    // round 1 P2.
    connect(m_viewportProps, &ViewportProperties::sceneSavedToPath,
            this, [this](const QString& path) {
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
                           "changes \u2014 use Revert in the scene editor "
                           "to discard your text edits and pull the "
                           "new file content.").arg(path));
                }
            });

    // Production-render frames from the engine \u2192 also flow to the
    // viewport widget so clicking "Render" updates the live view.
    connect(m_engine, &RenderEngine::imageUpdated,
            m_viewportWidget, &ViewportWidget::setImage);
}

void MainWindow::teardownViewport()
{
    if (!m_viewportBridge) return;

    // Close any looping preview-play (and its open scrub bracket) through the
    // normal path before the bridge it drives goes away — don't rely on
    // controller destruction to swallow an open composite.
    if (m_viewportTimeline) m_viewportTimeline->stopPlayback();

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

