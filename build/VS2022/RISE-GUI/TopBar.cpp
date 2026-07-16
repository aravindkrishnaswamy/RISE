//////////////////////////////////////////////////////////////////////
//
//  TopBar.cpp - Implementation.  See TopBar.h for the design-brief
//    cross reference.
//
//////////////////////////////////////////////////////////////////////

#include "TopBar.h"
#include "RenderEngine.h"
#include "ViewportBridge.h"
#include "ChatPanel.h"
#include "Theme.h"

#include "Utilities/RenderETAEstimator.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QTimer>
#include <QPainter>
#include <QLinearGradient>
#include <QStyle>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <cmath>
#include <algorithm>

// =====================================================================
// TopBarLogoSwatch — the 20x20 rounded-rect spectral-gradient logo
// mark, left of the "RISE" wordmark.  Mirrors TopBar.swift's
// `logoGradient` RoundedRectangle.  No Q_OBJECT: pure paint, no
// signals/slots, so it needs no moc registration of its own (moc
// only runs on TopBar.h, which is enough to pull in every QObject
// declared there — this class isn't one).
// =====================================================================
class TopBarLogoSwatch : public QWidget
{
public:
    explicit TopBarLogoSwatch(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(20, 20);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient grad(QPointF(0, 0), QPointF(width(), height()));
        Theme::applySpectralStops(grad);
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(rect(), Theme::radiusSmall, Theme::radiusSmall);
    }
};

// =====================================================================
// TopBarProgressStrip — the 150x3 spectral-gradient mini progress bar
// in the render-status readout.  Mirrors TopBar.swift's ZStack of two
// RoundedRectangles (trough + gradient fill).  No Q_OBJECT — see
// TopBarLogoSwatch's note above.
// =====================================================================
class TopBarProgressStrip : public QWidget
{
public:
    explicit TopBarProgressStrip(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(150, 3);
    }

    void setFraction(double f)
    {
        f = std::max(0.0, std::min(1.0, f));
        if (std::abs(f - m_fraction) < 1e-6) return;
        m_fraction = f;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        p.setBrush(Theme::fillTrough);
        p.drawRoundedRect(rect(), 2, 2);

        const int fillW = static_cast<int>(width() * m_fraction + 0.5);
        if (fillW > 0) {
            QRect fillRect(0, 0, fillW, height());
            QLinearGradient grad(QPointF(0, 0), QPointF(width(), 0));
            Theme::applySpectralStops(grad);
            p.setBrush(grad);
            p.drawRoundedRect(fillRect, 2, 2);
        }
    }

private:
    double m_fraction = 0.0;
};

// =====================================================================
// Refinement-status text mapping — ported verbatim from the macOS
// RefinementStatusFormatter.swift so the two readouts (this bar; a
// future viewport pill in slice B) can never disagree at a glance.
// =====================================================================
namespace {

struct RefinementStatus {
    QString text;
    QString label;
    double  fraction = 0.0;
};

int RefinementRung(unsigned int scaleDivisor)
{
    const unsigned int d = std::max(1u, scaleDivisor);
    const int log2d = static_cast<int>(std::round(std::log2(static_cast<double>(d))));
    return std::max(1, std::min(6, 6 - log2d));
}

RefinementStatus ComputeRefinementStatus(int phase, unsigned int scaleDivisor,
                                          bool isProduction, bool isCancelling,
                                          double productionProgress,
                                          bool isProductionPaused = false)
{
    // Label policy (user feedback 2026-07-16, mirrors the Mac formatter):
    // the small tracked label shows ONLY when it adds information the
    // primary text doesn't carry -- empty means the consumer hides it.
    // Pre-fix most states echoed themselves ("Settled SETTLED").
    if (isCancelling) {
        return { QStringLiteral("Cancelling…"), QString(), productionProgress };
    }
    if (isProduction) {
        const int pct = static_cast<int>(productionProgress * 100);
        // Item 4: workers parked at RenderEngine's pause gate --
        // progress honestly frozen at the pause point.  Mirrors
        // RefinementStatusFormatter.swift's isProductionPaused branch.
        if (isProductionPaused) {
            // The label adds WHAT is paused (a production render).
            return { QStringLiteral("Paused %1%").arg(pct),
                     QStringLiteral("PRODUCTION"),
                     productionProgress };
        }
        return { QStringLiteral("Production %1%").arg(pct),
                 QString(),
                 productionProgress };
    }
    const int r = RefinementRung(scaleDivisor);
    switch (phase) {
    case 4: return { QStringLiteral("Paused"), QString(), r / 6.0 };
    case 2: return { QStringLiteral("Refining · rung %1/6").arg(r), QString(), r / 6.0 };
    case 1: return { QStringLiteral("Rendering"), QString(), r / 6.0 };
    case 3: return { QStringLiteral("Polishing"), QStringLiteral("DENOISED — NOT FINAL"), 1.0 };
    case 0: return { QStringLiteral("Settled"), QString(), 1.0 };
    default: return { QStringLiteral("—"), QString(), 0.0 };
    }
}

} // namespace

// =====================================================================
// TopBar
// =====================================================================

TopBar::TopBar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(44);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->setSpacing(14);

    // ---- Left: scene identity --------------------------------------
    auto* identity = new QWidget(this);
    auto* idLayout = new QHBoxLayout(identity);
    idLayout->setContentsMargins(0, 0, 0, 0);
    idLayout->setSpacing(9);

    m_logoSwatch = new TopBarLogoSwatch(identity);
    idLayout->addWidget(m_logoSwatch);

    auto* wordmark = new QLabel(QStringLiteral("RISE"), identity);
    wordmark->setFont(Theme::sans(13, QFont::Bold));
    wordmark->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    idLayout->addWidget(wordmark);

    auto* sep1 = new QFrame(identity);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFixedHeight(20);
    sep1->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    idLayout->addWidget(sep1);

    m_sceneFileLabel = new QLabel(QStringLiteral("No scene"), identity);
    m_sceneFileLabel->setFont(Theme::mono(12));
    m_sceneFileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    idLayout->addWidget(m_sceneFileLabel);

    m_dirtyDotLabel = new QLabel(QString::fromUtf8("\xE2\x97\x8F"), identity);   // U+25CF BLACK CIRCLE
    m_dirtyDotLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 6px;").arg(Theme::hex(Theme::dirty)));
    m_dirtyDotLabel->hide();
    idLayout->addWidget(m_dirtyDotLabel);

    m_dirtyTextLabel = new QLabel(QStringLiteral("edited"), identity);
    m_dirtyTextLabel->setFont(Theme::sans(10));
    m_dirtyTextLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::dirty)));
    m_dirtyTextLabel->hide();
    idLayout->addWidget(m_dirtyTextLabel);

    layout->addWidget(identity);
    layout->addStretch(1);

    // ---- Center: render-status cluster -------------------------------
    auto* cluster = new QWidget(this);
    cluster->setObjectName(QStringLiteral("topBarCluster"));
    cluster->setStyleSheet(QStringLiteral(
        "#topBarCluster { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
        .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight))
        .arg(Theme::radiusLarge));
    auto* clusterLayout = new QHBoxLayout(cluster);
    clusterLayout->setContentsMargins(6, 5, 6, 5);
    clusterLayout->setSpacing(10);

    // (The refinement pause/resume tool button was removed by user
    // request -- interactive refinement restarts on every edit anyway,
    // so pausing it was a niche control.  Production pause lives on the
    // transport pill; the restart button below remains.  Mirrors
    // TopBar.swift.)

    m_restartBtn = new QToolButton(cluster);
    m_restartBtn->setFixedSize(26, 26);
    m_restartBtn->setToolTip(QStringLiteral("Restart refinement"));
    m_restartBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(m_restartBtn, &QToolButton::clicked, this, &TopBar::onRestartClicked);
    clusterLayout->addWidget(m_restartBtn);

    m_readoutContainer = new QWidget(cluster);
    auto* readoutLayout = new QVBoxLayout(m_readoutContainer);
    readoutLayout->setContentsMargins(0, 0, 4, 0);
    readoutLayout->setSpacing(3);

    auto* readoutRow = new QWidget(m_readoutContainer);
    auto* readoutRowLayout = new QHBoxLayout(readoutRow);
    readoutRowLayout->setContentsMargins(0, 0, 0, 0);
    readoutRowLayout->setSpacing(8);

    m_statusRowLabel = new QLabel(QStringLiteral("—"), readoutRow);
    m_statusRowLabel->setFont(Theme::mono(11));
    m_statusRowLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    readoutRowLayout->addWidget(m_statusRowLabel);

    m_statusTagLabel = new QLabel(QStringLiteral("—"), readoutRow);
    m_statusTagLabel->setFont(Theme::sans(9));
    m_statusTagLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    readoutRowLayout->addWidget(m_statusTagLabel);
    readoutRowLayout->addStretch(1);

    readoutLayout->addWidget(readoutRow);

    m_progressStrip = new TopBarProgressStrip(m_readoutContainer);
    readoutLayout->addWidget(m_progressStrip);

    clusterLayout->addWidget(m_readoutContainer);

    m_integratorSep = new QFrame(cluster);
    m_integratorSep->setFrameShape(QFrame::VLine);
    m_integratorSep->setFixedHeight(22);
    m_integratorSep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    clusterLayout->addWidget(m_integratorSep);

    // P2 fix: placeholder text only -- both this label AND
    // m_integratorSep get their real text/visibility from the
    // updateIntegratorChip() call at the end of this constructor
    // (before the widget is ever shown), which hides both unless a
    // real resolved integrator is available.
    m_integratorChip = new QLabel(QStringLiteral("AUTO"), cluster);
    m_integratorChip->setFont(Theme::mono(10));
    m_integratorChip->setStyleSheet(QStringLiteral("color: %1; padding: 0 4px;").arg(Theme::hex(Theme::textDim)));
    clusterLayout->addWidget(m_integratorChip);

    layout->addWidget(cluster);
    layout->addStretch(1);

    // ---- Right: render transport (Render -> Pause -> Resume) ------------
    // One slot that morphs with the production render's lifecycle:
    // idle -> "Render" (kicks a production render); rendering -> "Pause"
    // (parks the workers) + the Cancel pill beside it; paused -> "Resume"
    // + Cancel.  Cancelling shows a disabled label (no Cancel pill --
    // there's nothing left to cancel).  See updateTransportButton() for
    // the per-state text/style and TopBar.h's doc on
    // renderTransportClicked() / setCanStartProductionRender() for the
    // MainWindow wiring contract.  Font/weight/style match the macOS
    // transportPill (TopBar.swift).
    m_transportBtn = new QPushButton(this);
    m_transportBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_transportBtn->setFlat(true);
    m_transportBtn->setFixedHeight(28);
    m_transportOpacity = new QGraphicsOpacityEffect(m_transportBtn);
    m_transportOpacity->setOpacity(1.0);
    m_transportBtn->setGraphicsEffect(m_transportOpacity);
    connect(m_transportBtn, &QPushButton::clicked, this, &TopBar::onRenderTransportClicked);
    layout->addWidget(m_transportBtn);

    // ---- Right: Cancel pill --------------------------------------------
    // Shown beside m_transportBtn only while Rendering (error-tinted
    // outline, mirrors TopBar.swift's cancelPill); hidden the rest of
    // the time by updateTransportButton().  Same font/height as the
    // transport pill it sits next to.
    m_cancelBtn = new QPushButton(this);
    m_cancelBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_cancelBtn->setFlat(true);
    m_cancelBtn->setFixedHeight(28);
    m_cancelBtn->hide();
    connect(m_cancelBtn, &QPushButton::clicked, this, &TopBar::onCancelClicked);
    layout->addWidget(m_cancelBtn);

    // ---- Right: save-state chip ----------------------------------------
    // Explicit-save-only (user decision 2026-07-12): UI edits never
    // auto-save to disk -- this chip is the ONLY way a save gets
    // triggered from this bar.  Dirty -> an ACTIVE bordered "Save" pill
    // (click -> saveClicked()); clean -> a passive "✓ saved" label; no
    // loaded path -> hidden.  Kept as a QPushButton (flat) so the
    // existing click -> saveClicked() wiring stays intact; enabled only
    // while dirty, so a stray click on the passive "saved" text is a
    // no-op.  See updateSaveChip() for the per-state text/color/style.
    m_saveBtn = new QPushButton(this);
    m_saveBtn->setFont(Theme::mono(10));
    m_saveBtn->setFlat(true);
    m_saveBtn->setFixedHeight(28);
    m_saveBtn->setEnabled(false);
    m_saveBtn->hide();
    connect(m_saveBtn, &QPushButton::clicked, this, &TopBar::saveClicked);
    layout->addWidget(m_saveBtn);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(500);
    connect(m_pollTimer, &QTimer::timeout, this, &TopBar::pollRefinementState);

    updateReadout();
    updateIntegratorChip();
    updateControlsEnabled();
    updateSaveChip();
}

void TopBar::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::bgTopBar);
    p.setPen(Theme::borderHairline);
    p.drawLine(0, height() - 1, width(), height() - 1);
    QWidget::paintEvent(event);
}

void TopBar::setEngine(RenderEngine* engine)
{
    if (m_engine == engine) return;
    m_engine = engine;
    if (!m_engine) return;

    connect(m_engine, &RenderEngine::stateChanged, this, &TopBar::onEngineStateChanged);
    connect(m_engine, &RenderEngine::progressUpdated, this, &TopBar::onEngineProgress);
    // P1-3 fix: surface Error state in the persistent readout, not just
    // MainWindow's transient 5s status-bar message.
    connect(m_engine, &RenderEngine::errorOccurred, this, &TopBar::onEngineError);

    m_engineState = static_cast<int>(m_engine->state());
    updateReadout();
    updateIntegratorChip();
    updateControlsEnabled();
}

void TopBar::setViewportBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    if (m_bridge) {
        pollRefinementState();
        m_pollTimer->start();
    } else {
        m_pollTimer->stop();
        m_refinementPhase = -1;
        m_refinementScaleDivisor = 1;
        updateReadout();
    }
    updateControlsEnabled();
    updateSaveChip();
}

void TopBar::setLoadedFilePath(const QString& path)
{
    m_loadedFilePath = path;
    updateSceneIdentity();
    updateSaveChip();
}

void TopBar::setChatPanel(ChatPanel* chatPanel)
{
    m_chatPanel = chatPanel;
    updateControlsEnabled();
}

void TopBar::onChatRenderOutstandingChanged()
{
    updateControlsEnabled();
}

void TopBar::setSceneEditsDirty(bool dirty)
{
    if (m_dirty == dirty) return;
    m_dirty = dirty;
    updateSceneIdentity();
    updateSaveChip();
}

void TopBar::updateSaveChip()
{
    if (!m_saveBtn) return;
    // Untitled scenes (loadedFilePath empty but a bridge exists -- start-
    // screen create path, docs/gui/START_SCREEN.md §5.2) get the SAME
    // save affordance: MainWindow::onSaveScene() routes to Save-As for
    // them, so the pill never leads to a silent no-op.
    if (m_loadedFilePath.isEmpty() && !m_bridge) {
        m_saveBtn->hide();
        m_saveBtn->setEnabled(false);
        return;
    }
    m_saveBtn->show();

    if (m_dirty) {
        // Active, bordered "Save" pill -- mirrors the macOS TopBar.swift
        // saveStateChip's dirty branch (sans 12 demibold, textPrimary,
        // borderStrong border, fillHover background).
        m_saveBtn->setFont(Theme::sans(12, QFont::DemiBold));
        m_saveBtn->setText(tr("Save"));
        m_saveBtn->setToolTip(tr("Write the scene file to disk (Ctrl+Alt+S) — edits are never auto-saved"));
        m_saveBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; border: 1px solid %2; border-radius: %3px; "
            "color: %4; padding: 6px 13px; }")
            .arg(Theme::hex(Theme::fillHover), Theme::hex(Theme::borderStrong))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textPrimary)));
        m_saveBtn->setEnabled(true);
        m_saveBtn->setCursor(Qt::PointingHandCursor);
    } else {
        // Passive, non-interactive "saved" readout.
        m_saveBtn->setFont(Theme::mono(10));
        m_saveBtn->setText(QString::fromUtf8("\xE2\x9C\x93 saved"));   // ✓ saved
        m_saveBtn->setToolTip(QString());
        m_saveBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; padding: 0; }")
            .arg(Theme::hex(Theme::textDim)));
        m_saveBtn->setEnabled(false);
        m_saveBtn->setCursor(Qt::ArrowCursor);
    }
}

void TopBar::updateElapsedTime(double seconds)
{
    m_lastElapsed = seconds;
    updateReadoutTooltip();
}

void TopBar::updateRemainingTime(double seconds, bool hasEstimate)
{
    m_lastRemaining = seconds;
    m_haveRemainingEstimate = hasEstimate;
    updateReadoutTooltip();
}

void TopBar::updateReadoutTooltip()
{
    if (!m_readoutContainer) return;

    // P1-3 / P2 fix (mirrors TopBar.swift's `readoutHelp`): priority
    // order is (1) "why is this in an error state" -- the single most
    // important thing to surface and the only one that can explain a
    // render that silently stopped progressing; (2) the current
    // progress-callback title (P2 fix -- previously discarded by
    // onEngineProgress); (3) the elapsed/remaining-time caption this
    // tooltip has always shown.  Each tier is a no-op fallthrough to
    // the next, matching the Swift `if let ... return` chain.
    if (m_engineState == RenderEngine::Error && !m_lastErrorMessage.isEmpty()) {
        m_readoutContainer->setToolTip(m_lastErrorMessage);
        return;
    }

    QString tip = QStringLiteral("Elapsed: %1")
        .arg(QString::fromStdString(RISE::RenderETAEstimator::FormatDuration(m_lastElapsed)));
    if (m_haveRemainingEstimate) {
        tip += QStringLiteral("  ·  Remaining: ~%1")
            .arg(QString::fromStdString(RISE::RenderETAEstimator::FormatDuration(m_lastRemaining)));
    } else {
        tip += QStringLiteral("  ·  Remaining: estimating…");
    }
    if (!m_progressTitle.isEmpty()) {
        tip += QStringLiteral("  ·  %1").arg(m_progressTitle);
    }
    m_readoutContainer->setToolTip(tip);
}

void TopBar::updateSceneIdentity()
{
    if (m_loadedFilePath.isEmpty()) {
        if (m_bridge) {
            // Untitled scene (start-screen create path,
            // docs/gui/START_SCREEN.md §5.2): loaded from the bundled
            // starter template, not yet on disk.  Gains a real name via
            // Save-As.
            m_sceneFileLabel->setText(QStringLiteral("Untitled"));
            m_sceneFileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
            m_dirtyDotLabel->setVisible(m_dirty);
            m_dirtyTextLabel->setVisible(m_dirty);
            return;
        }
        m_sceneFileLabel->setText(QStringLiteral("No scene"));
        m_sceneFileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        m_dirtyDotLabel->hide();
        m_dirtyTextLabel->hide();
        return;
    }
    m_sceneFileLabel->setText(QFileInfo(m_loadedFilePath).fileName());
    m_sceneFileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    m_dirtyDotLabel->setVisible(m_dirty);
    m_dirtyTextLabel->setVisible(m_dirty);
}

// (onPauseResumeClicked -- the center-cluster refinement pause/resume
// click handler -- was removed by user request; see the tombstone
// comment on its declaration in TopBar.h and in the constructor above.
// The Render menu's "Pause Render"/"Resume Render" action is now
// production-only too -- see MainWindow.cpp's m_pauseResumeAction.)

void TopBar::onRenderTransportClicked()
{
    // While Rendering, this pill pauses/resumes THE RENDER -- workers
    // park at RenderEngine's ProgressCallbackAdapter pause gate (mirrors
    // macOS's transportPill Resume/Pause branches).  Always reachable
    // while Rendering (the Cancelling state disables the button
    // entirely, per updateTransportButton).
    if (m_engineState == RenderEngine::Rendering) {
        if (m_engine) {
            m_engine->setProductionRenderPaused(!m_engine->isProductionRenderPaused());
        }
        updateReadout();
        updateTransportButton();
        return;
    }

    // Cancelling: the button is disabled (see updateTransportButton),
    // so this shouldn't be reachable -- guard anyway rather than fall
    // through to a render start mid-cancel.
    if (m_engineState == RenderEngine::Cancelling) return;

    // Idle / SceneLoaded / Completed / Cancelled / Loading / Error:
    // click-time re-check mirroring the pause/restart buttons' Round-3
    // P2 pattern -- the button is ALSO disabled via updateTransportButton
    // whenever m_canStartProductionRender is false, but a click racing a
    // state flip must refuse rather than kick a render MainWindow's own
    // predicate would have refused.  MainWindow::onRender (connected to
    // renderTransportClicked) owns all the actual pre-render bookkeeping.
    if (!m_canStartProductionRender) return;
    emit renderTransportClicked();
}

void TopBar::onCancelClicked()
{
    // Reuses RenderEngine::cancelRender() -- the exact same single-line
    // body as MainWindow::onCancel() (wired to the Render menu's Cancel
    // action / Ctrl+.) -- so there is exactly one cancel code path, not
    // a duplicated one.  Click-time re-check mirroring the other
    // transport handlers' Round-3 P2 pattern: the button is ALSO hidden
    // via updateTransportButton whenever the engine isn't Rendering,
    // but a click racing a state flip (e.g. the render just completed)
    // must refuse rather than cancel nothing.  Works while paused too --
    // the pause gate observes cancel.
    if (m_engineState != RenderEngine::Rendering) return;
    if (m_engine) m_engine->cancelRender();
}

void TopBar::onRestartClicked()
{
    // Round-3 P2: same click-time re-check pattern as the other
    // transport handlers (onRenderTransportClicked / onCancelClicked).
    if (!m_bridge) return;
    const bool disabled = (m_engineState == RenderEngine::Rendering
                         || m_engineState == RenderEngine::Cancelling
                         || m_engineState == RenderEngine::Loading
                         || (m_chatPanel && m_chatPanel->isChatRenderOutstanding()));
    if (disabled) return;
    m_bridge->stop();
    m_bridge->start();
    pollRefinementState();
}

void TopBar::pollRefinementState()
{
    if (!m_bridge) {
        m_refinementPhase = -1;
        m_refinementScaleDivisor = 1;
    } else {
        unsigned int sd = 1;
        m_refinementPhase = m_bridge->refinementPhase(&sd);
        m_refinementScaleDivisor = sd;
    }
    updateReadout();
    updateControlsEnabled();
}

void TopBar::onEngineStateChanged(int newState)
{
    m_engineState = newState;
    updateReadout();
    updateIntegratorChip();
    updateControlsEnabled();
}

void TopBar::onEngineProgress(double fraction, const QString& title)
{
    m_productionProgress = fraction;
    // P2 fix: title was previously discarded -- store it so
    // updateReadoutTooltip() (called by updateReadout(), below) can
    // surface it (mirrors Mac's TopBar.swift `readoutHelp` falling
    // back to `viewModel.progressTitle`).
    m_progressTitle = title;
    updateReadout();
}

void TopBar::onEngineError(const QString& message)
{
    // P1-3 fix: stash for updateReadout()/updateReadoutTooltip() (the
    // latter called by the former) -- both short-circuit on
    // m_engineState == Error, which onEngineStateChanged's own
    // updateReadout() call (fired for the SAME state transition) will
    // already have picked up by the time this arrives, but re-running
    // here is cheap and covers the signal-ordering edge case where
    // errorOccurred is emitted before stateChanged reaches this slot.
    m_lastErrorMessage = message;
    updateReadout();
}

void TopBar::updateReadout()
{
    // P1-3 fix (mirrors TopBar.swift's `status` computed property):
    // Error has no representation in ComputeRefinementStatus's
    // (phase, isProduction, isCancelling) input space -- falling
    // through to it would render an error state as whatever the
    // refinement phase happened to be cached at before the failure
    // (typically "Settled"), hiding a real failure behind a look
    // identical to "everything's fine".  Checked BEFORE the shared
    // formatter so an error always wins regardless of stale phase state.
    if (m_engineState == RenderEngine::Error) {
        m_statusRowLabel->setText(QStringLiteral("Error"));
        m_statusTagLabel->setText(QStringLiteral("ERROR"));
        m_statusTagLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::error)));
        m_lastNonPausedFraction = 0.0;
        if (m_progressStrip) m_progressStrip->setFraction(0.0);
        updateReadoutTooltip();
        return;
    }

    const bool isProduction = (m_engineState == RenderEngine::Rendering);
    const bool isCancelling = (m_engineState == RenderEngine::Cancelling);
    const bool isProductionPaused = isProduction && m_engine && m_engine->isProductionRenderPaused();

    const RefinementStatus s = ComputeRefinementStatus(
        m_refinementPhase, m_refinementScaleDivisor, isProduction, isCancelling, m_productionProgress,
        isProductionPaused);

    m_statusRowLabel->setText(s.text);
    m_statusTagLabel->setText(s.label);
    // Empty label = nothing to add beside the text (see the label-policy
    // comment in RefinementStatus above) -- hide so no stray gap renders.
    m_statusTagLabel->setVisible(!s.label.isEmpty());

    QColor tagColor = Theme::success;
    if (isCancelling) {
        tagColor = Theme::warn;
    } else if (isProduction) {
        tagColor = Theme::success;
    } else {
        switch (m_refinementPhase) {
        case 4:  tagColor = Theme::warn;    break;
        case -1: tagColor = Theme::textDim; break;
        default: tagColor = Theme::success; break;
        }
    }
    m_statusTagLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(tagColor)));

    // Freeze the progress fraction while paused (and not mid-
    // production) instead of collapsing it to whatever a -1/0
    // phase would compute — mirrors TopBar.swift's
    // `progressFraction` / `syncLastFraction`.
    const bool freeze = (m_refinementPhase == 4 && !isProduction);
    const double fraction = freeze ? m_lastNonPausedFraction : s.fraction;
    if (!freeze) {
        m_lastNonPausedFraction = s.fraction;
    }
    if (m_progressStrip) m_progressStrip->setFraction(fraction);
    updateReadoutTooltip();
}

void TopBar::updateIntegratorChip()
{
    QString integ, reason;
    if (m_engine && m_engineState == RenderEngine::Completed) {
        integ = m_engine->autoResolvedIntegrator();
        reason = m_engine->autoResolveReason();
    }
    // P2 fix (mirrors the Mac fix -- TopBar.swift's `if let integ =
    // viewModel.resolvedIntegrator`): when there's no resolved
    // integrator to show (not using the auto-dispatcher, or no render
    // yet), hide BOTH the chip AND its divider entirely rather than
    // rendering a misleading static "AUTO" placeholder next to a
    // divider that now separates the readout from nothing meaningful.
    const bool hasIntegrator = !integ.isEmpty();
    if (m_integratorSep) m_integratorSep->setVisible(hasIntegrator);
    m_integratorChip->setVisible(hasIntegrator);
    if (hasIntegrator) {
        m_integratorChip->setText(QStringLiteral("AUTO → %1").arg(integ.toUpper()));
        m_integratorChip->setStyleSheet(QStringLiteral("color: %1; padding: 0 4px;").arg(Theme::hex(Theme::success)));
        m_integratorChip->setToolTip(reason);
    } else {
        m_integratorChip->setToolTip(QString());
    }
}

void TopBar::updateControlsEnabled()
{
    // Mirrors TopBar.swift's `refinementControlsDisabled`: true while
    // no bridge is attached, a production render owns the scene, or
    // (P1-2 fix, mirrors canUseSceneTransport) a chat-driven render
    // owns it instead — the C++ controller's start+stop no-op in that
    // state anyway; disabling here just keeps the UI honest about it.
    // m_restartBtn is the only remaining consumer of this predicate now
    // that the center-cluster refinement pause button is gone (production
    // pause lives on the transport pill -- see updateTransportButton).
    const bool refinementDisabled = !m_bridge
        || m_engineState == RenderEngine::Rendering
        || m_engineState == RenderEngine::Cancelling
        || (m_chatPanel && m_chatPanel->isChatRenderOutstanding());
    m_restartBtn->setEnabled(!refinementDisabled);

    updateTransportButton();
}

void TopBar::setCanStartProductionRender(bool canStart)
{
    if (m_canStartProductionRender == canStart) return;
    m_canStartProductionRender = canStart;
    updateTransportButton();
}

void TopBar::updateTransportButton()
{
    if (!m_transportBtn) return;

    const bool isProduction = (m_engineState == RenderEngine::Rendering);
    const bool isCancelling = (m_engineState == RenderEngine::Cancelling);

    // Shared "filled" pill style (accent bg, near-black text) --
    // used for the idle "Render" state and the paused "Resume" state.
    // Mirrors TopBar.swift's transportPill(filled: true).
    const QString filledStyle = QStringLiteral(
        "QPushButton { background-color: %1; color: rgba(0,0,0,0.92); border: none;"
        " border-radius: %2px; padding: 6px 15px; }")
        .arg(Theme::hex(Theme::accent))
        .arg(Theme::radiusMedium);
    // Shared "outlined" pill style (warn border/text, transparent bg) --
    // used for the Rendering-and-not-paused "Pause" state.  Mirrors
    // transportPill(filled: false).
    const QString outlinedStyle = QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
        " border-radius: %3px; padding: 6px 15px; }")
        .arg(Theme::hex(Theme::warn))
        .arg(Theme::rgba(QColor(Theme::warn.red(), Theme::warn.green(), Theme::warn.blue(),
                                 static_cast<int>(0.5 * 255))))
        .arg(Theme::radiusMedium);
    // Plain disabled text, no chrome -- mirrors the Mac's disabled
    // "Cancelling…" Text (no pill/border at all in that state).
    const QString cancellingStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none; color: %1;"
        " padding: 6px 15px; }")
        .arg(Theme::hex(Theme::textDisabled));
    // Error-tinted outline pill for the Cancel button -- shown beside
    // Pause/Resume only while a production render is actually in
    // flight.  Mirrors TopBar.swift's cancelPill.
    const QString cancelStyle = QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
        " border-radius: %3px; padding: 6px 13px; }")
        .arg(Theme::hex(Theme::error))
        .arg(Theme::rgba(QColor(Theme::error.red(), Theme::error.green(), Theme::error.blue(),
                                 static_cast<int>(0.5 * 255))))
        .arg(Theme::radiusMedium);

    if (isCancelling) {
        m_transportBtn->setText(QStringLiteral("Cancelling\xE2\x80\xA6"));
        m_transportBtn->setStyleSheet(cancellingStyle);
        m_transportBtn->setEnabled(false);
        m_transportBtn->setCursor(Qt::ArrowCursor);
        m_transportBtn->setToolTip(QString());
        if (m_transportOpacity) m_transportOpacity->setOpacity(1.0);
        // No Cancel pill while already cancelling -- nothing left to
        // cancel (mirrors TopBar.swift: the cancelPill lives only in
        // the `isProduction` branch, not the `isCancelling` one).
        if (m_cancelBtn) m_cancelBtn->hide();
        return;
    }

    if (isProduction) {
        const bool paused = m_engine && m_engine->isProductionRenderPaused();
        m_transportBtn->setEnabled(true);
        m_transportBtn->setCursor(Qt::PointingHandCursor);
        if (m_transportOpacity) m_transportOpacity->setOpacity(1.0);
        if (paused) {
            m_transportBtn->setText(QStringLiteral("Resume"));
            m_transportBtn->setStyleSheet(filledStyle);
            m_transportBtn->setToolTip(tr("Resume the render where it left off"));
        } else {
            m_transportBtn->setText(QStringLiteral("Pause"));
            m_transportBtn->setStyleSheet(outlinedStyle);
            m_transportBtn->setToolTip(tr("Pause the render \xE2\x80\x94 workers park, CPU goes quiet"));
        }
        // Cancel pill beside Pause/Resume -- enabled unconditionally
        // while Rendering, whether paused or not (the pause gate
        // observes cancel, so cancelling a paused render already works).
        if (m_cancelBtn) {
            m_cancelBtn->show();
            m_cancelBtn->setStyleSheet(cancelStyle);
            m_cancelBtn->setEnabled(true);
            m_cancelBtn->setCursor(Qt::PointingHandCursor);
            m_cancelBtn->setText(QStringLiteral("Cancel"));
            m_cancelBtn->setToolTip(tr("Cancel the render (Ctrl+.)"));
        }
        return;
    }

    // Idle / SceneLoaded / Completed / Cancelled / Loading / Error:
    // the "Render" pill, gated on m_canStartProductionRender (pushed in
    // from MainWindow -- see setCanStartProductionRender's doc).  No
    // Cancel pill in this state either -- there's no render in flight.
    if (m_cancelBtn) m_cancelBtn->hide();
    m_transportBtn->setText(QStringLiteral("Render"));
    m_transportBtn->setStyleSheet(filledStyle);
    m_transportBtn->setEnabled(m_canStartProductionRender);
    m_transportBtn->setCursor(m_canStartProductionRender ? Qt::PointingHandCursor : Qt::ArrowCursor);
    // Mirrors TopBar.swift's `.opacity(viewModel.canStartProductionRender
    // ? 1.0 : 0.4)` -- QSS alone can't dim background AND text together
    // as one "whole pill" opacity, hence the QGraphicsOpacityEffect.
    if (m_transportOpacity) m_transportOpacity->setOpacity(m_canStartProductionRender ? 1.0 : 0.4);
    m_transportBtn->setToolTip(tr("Start a production render (Ctrl+R)"));
}
