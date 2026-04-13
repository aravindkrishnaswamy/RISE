//////////////////////////////////////////////////////////////////////
//
//  ControlsWidget.cpp - Controls panel implementation.
//
//  Button enable/disable logic ported from Mac app's ContentView.swift.
//
//////////////////////////////////////////////////////////////////////

#include "ControlsWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>

ControlsWidget::ControlsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // Section label
    auto* titleLabel = new QLabel("Controls");
    titleLabel->setStyleSheet("font-weight: bold; color: gray;");
    mainLayout->addWidget(titleLabel);

    // Scene actions
    auto* sceneLayout = new QHBoxLayout();
    m_openBtn = new QPushButton("Open Scene");
    m_editBtn = new QPushButton("Edit");
    m_clearBtn = new QPushButton("Clear");
    sceneLayout->addWidget(m_openBtn);
    sceneLayout->addWidget(m_editBtn);
    sceneLayout->addWidget(m_clearBtn);
    mainLayout->addLayout(sceneLayout);

    // Render actions
    auto* renderLayout = new QHBoxLayout();
    m_renderBtn = new QPushButton("Render");
    m_renderAnimBtn = new QPushButton("Render Animation");
    m_cancelBtn = new QPushButton("Cancel");
    renderLayout->addWidget(m_renderBtn);
    renderLayout->addWidget(m_renderAnimBtn);
    renderLayout->addWidget(m_cancelBtn);
    mainLayout->addLayout(renderLayout);

    // Cancelling indicator
    m_cancellingLabel = new QLabel("Cancelling \u2014 waiting for active block...");
    m_cancellingLabel->setStyleSheet("color: orange; font-style: italic;");
    m_cancellingLabel->hide();
    mainLayout->addWidget(m_cancellingLabel);

    // Progress group
    m_progressGroup = new QWidget();
    auto* progressLayout = new QVBoxLayout(m_progressGroup);
    progressLayout->setContentsMargins(0, 4, 0, 0);
    progressLayout->setSpacing(2);

    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 1000);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(false);
    progressLayout->addWidget(m_progressBar);

    auto* progressInfoLayout = new QHBoxLayout();
    m_progressPercent = new QLabel("0%");
    m_progressPercent->setStyleSheet("font-family: monospace;");
    m_progressTitle = new QLabel("");
    m_progressTitle->setStyleSheet("color: gray;");
    m_elapsedLabel = new QLabel("");
    m_elapsedLabel->setStyleSheet("font-family: monospace; color: gray;");
    m_elapsedLabel->setAlignment(Qt::AlignRight);

    progressInfoLayout->addWidget(m_progressPercent);
    progressInfoLayout->addWidget(m_progressTitle, 1);
    progressInfoLayout->addWidget(m_elapsedLabel);
    progressLayout->addLayout(progressInfoLayout);

    m_progressGroup->hide();
    mainLayout->addWidget(m_progressGroup);

    mainLayout->addStretch();

    // Connections
    connect(m_openBtn, &QPushButton::clicked, this, &ControlsWidget::openSceneClicked);
    connect(m_editBtn, &QPushButton::clicked, this, &ControlsWidget::editClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &ControlsWidget::clearClicked);
    connect(m_renderBtn, &QPushButton::clicked, this, &ControlsWidget::renderClicked);
    connect(m_renderAnimBtn, &QPushButton::clicked, this, &ControlsWidget::renderAnimationClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ControlsWidget::cancelClicked);

    updateButtonStates();
}

void ControlsWidget::setRenderState(RenderEngine::State state)
{
    m_state = state;
    updateButtonStates();

    bool isRendering = (state == RenderEngine::Rendering || state == RenderEngine::Cancelling);
    m_progressGroup->setVisible(isRendering);
    m_cancellingLabel->setVisible(state == RenderEngine::Cancelling);

    if (!isRendering) {
        m_progressBar->setValue(0);
        m_progressPercent->setText("0%");
        m_progressTitle->clear();
        m_elapsedLabel->clear();
    }
}

void ControlsWidget::setHasAnimation(bool hasAnimation)
{
    m_hasAnimation = hasAnimation;
    updateButtonStates();
}

void ControlsWidget::setHasScene(bool hasScene)
{
    m_hasScene = hasScene;
    updateButtonStates();
}

void ControlsWidget::updateProgress(double fraction, const QString& title)
{
    m_progressBar->setValue(static_cast<int>(fraction * 1000));
    m_progressPercent->setText(QString("%1%").arg(fraction * 100, 0, 'f', 1));
    m_progressTitle->setText(title);
}

void ControlsWidget::updateElapsedTime(double seconds)
{
    int mins = static_cast<int>(seconds) / 60;
    double secs = seconds - mins * 60;
    if (mins > 0) {
        m_elapsedLabel->setText(QString("%1m %2s").arg(mins).arg(secs, 0, 'f', 1));
    } else {
        m_elapsedLabel->setText(QString("%1s").arg(secs, 0, 'f', 1));
    }
}

void ControlsWidget::updateButtonStates()
{
    using S = RenderEngine::State;

    bool isActive = (m_state == S::Rendering || m_state == S::Cancelling || m_state == S::Loading);
    bool canRender = (m_state == S::SceneLoaded || m_state == S::Completed || m_state == S::Cancelled);

    m_openBtn->setEnabled(!isActive);
    m_editBtn->setEnabled(m_hasScene);
    m_clearBtn->setEnabled(!isActive && m_state != S::Idle);
    m_renderBtn->setEnabled(canRender);
    m_renderAnimBtn->setEnabled(canRender && m_hasAnimation);
    m_cancelBtn->setEnabled(m_state == S::Rendering);
}
