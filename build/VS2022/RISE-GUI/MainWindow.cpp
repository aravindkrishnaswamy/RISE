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

#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QApplication>
#include <QScreen>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("RISE - Realistic Image Synthesis Engine");
    setMinimumSize(900, 600);

    // Create the engine
    m_engine = new RenderEngine(this);

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

    // Right splitter: render view | bottom panel (220px fixed height)
    m_rightSplitter = new QSplitter(Qt::Vertical);
    m_rightSplitter->addWidget(m_renderWidget);
    m_rightSplitter->addWidget(m_bottomSplitter);
    m_rightSplitter->setSizes({500, 220});
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
    statusBar()->showMessage(QString("RISE %1 — Ready").arg(m_engine->versionString()));
}

void MainWindow::createMenuBar()
{
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* openAction = fileMenu->addAction("&Open Scene...", this, &MainWindow::onOpenScene);
    openAction->setShortcut(QKeySequence::Open);

    auto* saveAction = fileMenu->addAction("&Save Scene", [this]() {
        if (m_sceneEditor->isVisible()) m_sceneEditor->save();
    });
    saveAction->setShortcut(QKeySequence::Save);

    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction("E&xit", this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    auto* editMenu = menuBar()->addMenu("&Edit");
    auto* undoAction = editMenu->addAction("&Undo");
    undoAction->setShortcut(QKeySequence::Undo);
    auto* redoAction = editMenu->addAction("&Redo");
    redoAction->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    auto* findAction = editMenu->addAction("&Find...");
    findAction->setShortcut(QKeySequence::Find);

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

void MainWindow::onOpenScene()
{
    // Check for unsaved editor changes
    if (m_sceneEditor->isVisible() && m_sceneEditor->isDirty()) {
        auto result = QMessageBox::question(this, "Unsaved Changes",
            "The editor has unsaved changes. Save before opening a new scene?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (result == QMessageBox::Cancel) return;
        if (result == QMessageBox::Save) m_sceneEditor->save();
    }

    QString filePath = QFileDialog::getOpenFileName(
        this, "Open Scene", QString(),
        "RISE Scene Files (*.RISEscene);;All Files (*)");

    if (filePath.isEmpty()) return;

    // Clear previous scene
    m_engine->clearScene();

    m_engine->loadScene(filePath);
}

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

void MainWindow::onClear()
{
    m_engine->clearScene();
    m_controlsWidget->setHasScene(false);
    updateStatusBar();
}

void MainWindow::onRender()
{
    m_engine->startRender();
}

void MainWindow::onRenderAnimation()
{
    // Derive video output path from scene file
    QString scenePath = m_engine->loadedFilePath();
    QString videoPath = scenePath;
    videoPath.replace(".RISEscene", ".mp4");

    m_engine->startAnimationRender(videoPath);
}

void MainWindow::onCancel()
{
    m_engine->cancelRender();
}

void MainWindow::onStateChanged(int newState)
{
    auto state = static_cast<RenderEngine::State>(newState);

    m_renderWidget->setRenderState(state);
    m_controlsWidget->setRenderState(state);

    bool hasScene = (state != RenderEngine::Idle);
    m_controlsWidget->setHasScene(hasScene);

    updateStatusBar();
}

void MainWindow::onSceneSizeDetected(int width, int height)
{
    // Auto-resize window to fit scene, matching Mac app behavior
    int bottomHeight = 220;
    int menuHeight = menuBar()->height();
    int statusHeight = statusBar()->height();

    int targetW = width + (m_editorVisible ? 600 : 0) + 40;
    int targetH = height + bottomHeight + menuHeight + statusHeight + 40;

    // Clamp to screen size
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        QRect available = screen->availableGeometry();
        targetW = qMin(targetW, available.width() - 50);
        targetH = qMin(targetH, available.height() - 50);
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
