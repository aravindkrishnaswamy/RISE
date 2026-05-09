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

#include "Utilities/RenderETAEstimator.h"

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

    // Separate "Remaining: ~M:SS" line below the progress info so the
    // estimate is readable without squeezing the existing layout.
    m_remainingLabel = new QLabel("");
    m_remainingLabel->setStyleSheet("font-family: monospace; color: gray;");
    m_remainingLabel->setAlignment(Qt::AlignRight);
    progressLayout->addWidget(m_remainingLabel);

    m_progressGroup->hide();
    mainLayout->addWidget(m_progressGroup);

    // L5e — Exposure slider.  Lives in the Controls panel (under
    // the Render group) so it's visually distinct from the
    // animation-time slider in the viewport pane.  Header row is
    // "Exposure  +0.0 EV" so the function is obvious at a glance;
    // double-click anywhere on the slider track resets to 0 EV
    // (ExposureSlider subclass overrides mouseDoubleClickEvent).
    auto* exposureGroup = new QWidget();
    auto* exposureLayout = new QVBoxLayout(exposureGroup);
    exposureLayout->setContentsMargins(0, 4, 0, 0);
    exposureLayout->setSpacing(2);

    auto* exposureHeader = new QHBoxLayout();
    auto* exposureTitle = new QLabel("Exposure");
    exposureTitle->setStyleSheet("color: gray;");
    m_exposureLabel = new QLabel("0.0 EV");
    m_exposureLabel->setStyleSheet("font-family: monospace; color: gray;");
    m_exposureLabel->setAlignment(Qt::AlignRight);
    exposureHeader->addWidget(exposureTitle);
    exposureHeader->addStretch();
    exposureHeader->addWidget(m_exposureLabel);
    exposureLayout->addLayout(exposureHeader);

    m_exposureSlider = new ExposureSlider();
    m_exposureSlider->setRange(kExposureSliderMin, kExposureSliderMax);
    m_exposureSlider->setValue(0);
    m_exposureSlider->setSingleStep(1);   //  0.1 EV
    m_exposureSlider->setPageStep(10);    //  1.0 EV
    m_exposureSlider->setToolTip(
        "Display exposure in EV stops.  Double-click the slider to reset to 0.");
    exposureLayout->addWidget(m_exposureSlider);

    mainLayout->addWidget(exposureGroup);

    mainLayout->addStretch();

    // Connections
    connect(m_openBtn, &QPushButton::clicked, this, &ControlsWidget::openSceneClicked);
    connect(m_editBtn, &QPushButton::clicked, this, &ControlsWidget::editClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &ControlsWidget::clearClicked);
    connect(m_renderBtn, &QPushButton::clicked, this, &ControlsWidget::renderClicked);
    connect(m_renderAnimBtn, &QPushButton::clicked, this, &ControlsWidget::renderAnimationClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ControlsWidget::cancelClicked);

    // L5e — exposure-slider wiring.  valueChanged → forward as
    // double EV; resetRequested (double-click) → snap to 0 +
    // forward.  The valueChanged signal also fires when we
    // programmatically setValue(0), so there's a single consumer
    // path for both gestures.
    connect(m_exposureSlider, &QSlider::valueChanged,
            this, &ControlsWidget::onExposureSliderChanged);
    connect(m_exposureSlider, &ExposureSlider::resetRequested,
            this, &ControlsWidget::onExposureResetRequested);

    updateButtonStates();
}

void ControlsWidget::setExposureEV(double ev)
{
    // External programmatic setter (e.g. on scene load).  Block
    // signals so we don't echo back to the engine.
    const int v = qBound(kExposureSliderMin, static_cast<int>(ev * 10.0), kExposureSliderMax);
    QSignalBlocker blocker(m_exposureSlider);
    m_exposureSlider->setValue(v);
    m_exposureLabel->setText(QString("%1%2 EV")
        .arg(ev > 0 ? "+" : "")
        .arg(ev, 0, 'f', 1));
}

void ControlsWidget::onExposureSliderChanged(int value)
{
    const double ev = value / 10.0;
    m_exposureLabel->setText(QString("%1%2 EV")
        .arg(ev > 0 ? "+" : "")
        .arg(ev, 0, 'f', 1));
    emit exposureChanged(ev);
}

void ControlsWidget::onExposureResetRequested()
{
    // Snap-to-zero.  setValue triggers valueChanged → label
    // refresh + exposureChanged emit through the normal path.
    m_exposureSlider->setValue(0);
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
        m_remainingLabel->clear();
        m_lastElapsed = 0.0;
        m_lastRemaining = 0.0;
        m_haveRemainingEstimate = false;
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
    m_lastElapsed = seconds;
    refreshTimeLabels();
}

void ControlsWidget::updateRemainingTime(double seconds, bool hasEstimate)
{
    m_lastRemaining = seconds;
    m_haveRemainingEstimate = hasEstimate;
    refreshTimeLabels();
}

void ControlsWidget::refreshTimeLabels()
{
    m_elapsedLabel->setText(
        QString("Elapsed: %1")
            .arg(QString::fromStdString(
                RISE::RenderETAEstimator::FormatDuration(m_lastElapsed))));
    if (m_haveRemainingEstimate) {
        m_remainingLabel->setText(
            QString("Remaining: ~%1")
                .arg(QString::fromStdString(
                    RISE::RenderETAEstimator::FormatDuration(m_lastRemaining))));
    } else {
        m_remainingLabel->setText("Remaining: estimating\u2026");
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
