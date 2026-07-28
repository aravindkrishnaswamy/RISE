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
#include <QComboBox>
#include <QEvent>
#include <QSignalBlocker>
#include <QSize>
#include <QTimer>
#include <QPainter>
#include <QLinearGradient>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QMenu>
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
                                          bool isProductionPaused = false,
                                          bool viewportRenderModeWantsDenoise = true)
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
    // Data view modes (normals/depth/facets/wireframe) never denoise --
    // InteractivePelRasterizer::ShouldDenoise is gated off while a
    // view-mode caster is installed -- so claiming DENOISED there would
    // lie.  BeautyVariant modes (deep_reflect/direct, GUI render modes
    // P2a) DO genuinely denoise (their own ephemeral pipeline's fixed
    // OIDN config), so the label keys off the registry's wantsDenoise
    // flag rather than a hardcoded mode-name comparison.  Mirrors
    // RefinementStatusFormatter.swift's identical gate.
    case 3: return { QStringLiteral("Polishing"),
                     viewportRenderModeWantsDenoise
                         ? QStringLiteral("DENOISED — NOT FINAL")
                         : QString(),
                     1.0 };
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

    m_wordmarkLabel = new QLabel(QStringLiteral("RISE"), identity);
    m_wordmarkLabel->setFont(Theme::sans(13, QFont::Bold));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    idLayout->addWidget(m_wordmarkLabel);

    m_identitySep = new QFrame(identity);
    m_identitySep->setFrameShape(QFrame::VLine);
    m_identitySep->setFixedHeight(20);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    idLayout->addWidget(m_identitySep);

    m_sceneFileLabel = new QLabel(QStringLiteral("No scene"), identity);
    m_sceneFileLabel->setFont(Theme::mono(12));
    // Styled by updateSceneIdentity() -- re-invoked from restyleTheme()
    // (LIVE THEME-SWITCH CONTRACT, Theme.h); this initial text/color is
    // the SAME "No scene"/textDim state updateSceneIdentity() computes
    // for an unloaded/unbridged TopBar, so this is overwritten (not
    // contradicted) the instant the ctor-end restyleTheme() call runs.
    idLayout->addWidget(m_sceneFileLabel);

    m_dirtyDotLabel = new QLabel(QString::fromUtf8("\xE2\x97\x8F"), identity);   // U+25CF BLACK CIRCLE
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_dirtyDotLabel->hide();
    idLayout->addWidget(m_dirtyDotLabel);

    m_dirtyTextLabel = new QLabel(QStringLiteral("edited"), identity);
    m_dirtyTextLabel->setFont(Theme::sans(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_dirtyTextLabel->hide();
    idLayout->addWidget(m_dirtyTextLabel);

    layout->addWidget(identity);
    layout->addStretch(1);

    // ---- Center: render-status cluster -------------------------------
    m_cluster = new QWidget(this);
    QWidget* cluster = m_cluster;
    cluster->setObjectName(QStringLiteral("topBarCluster"));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
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
    // Icon-system upgrade: mirrors TopBar.swift's restartButton (SF
    // Symbol "arrow.clockwise", size 12, Theme.textMuted, disabled by
    // refinementControlsDisabled) -- Lucide "refresh-cw" tinted
    // textMuted/textDisabled to match.
    m_restartBtn->setIconSize(QSize(12, 12));
    // Icon: styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT
    // (Theme.h).
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
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
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
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
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

    // P1 fix (2026-07-23 review): the render-mode combo and X-ray toggle
    // are free-standing viewport-chrome chips, parented to the TOP BAR
    // (`this`), NOT the #topBarCluster status card above -- they used to
    // live inside `cluster`'s own clusterLayout, sharing its single
    // bordered box with the restart button + readout.  Mac's TopBar.swift
    // renderStatusCluster (lines 97-118) boxes ONLY restart + readout (+
    // integrator chip); the mode combo and X-ray toggle are separate
    // viewport-chrome chips (ContentView.swift:574-629), never inside the
    // status card.  Windows keeps both controls IN the top bar rather
    // than moving them into the viewport chrome proper (ratified by
    // docs/gui/RENDER_MODES.md:162-163), but they now get their OWN
    // bordered-chip look here, each a standalone card, instead of being
    // boxed together with the render-status readout.  `layout`'s
    // setSpacing(14) (set at the top of this constructor) gives these
    // chips the bar's existing 14px rhythm automatically, same as every
    // other top-level item in this row.

    // P1 (docs/gui/RENDER_MODES.md §5): viewport render-mode combo --
    // "TopBar combobox" per §5's GUI bullet.  Always visible (never
    // hidden -- matches m_restartBtn's disable-not-hide convention, not
    // m_saveBtn's hide-when-nothing-loaded convention); enabled state
    // and current index are entirely owned by refreshRenderModeCombo().
    m_renderModeCombo = new QComboBox(this);
    m_renderModeCombo->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    // Fixed built-in mode set (RENDER_MODES.md decision 2) -- populated
    // ONCE from the controller-independent registry; refreshRenderModeCombo()
    // only ever moves the current-index pointer afterward, never touches
    // the item list.  Each item's UserRole data is the wire name
    // (RISE_API_SceneEditController_SetViewportRenderMode's `name` arg);
    // ToolTipRole is the mode's question string.
    for (const ViewportRenderModeInfo& info : ViewportBridge::viewportRenderModes()) {
        const int idx = m_renderModeCombo->count();
        m_renderModeCombo->addItem(info.title);
        m_renderModeCombo->setItemData(idx, info.name, Qt::UserRole);
        m_renderModeCombo->setItemData(idx, info.question, Qt::ToolTipRole);
        // P2a review fix: stash isVariant per item (Qt::UserRole + 1) so
        // refreshRenderModeCombo() can gate m_xrayBtn's enabled state on the
        // ACTIVE mode without a second registry round-trip per refresh.
        m_renderModeCombo->setItemData(idx, info.isVariant, Qt::UserRole + 1);
    }
    m_renderModeCombo->setEnabled(false);   // no bridge yet -- refreshRenderModeCombo() re-enables on scene load
    connect(m_renderModeCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &TopBar::onRenderModeComboChanged);
    layout->addWidget(m_renderModeCombo);

    // P1 (docs/gui/RENDER_MODES.md "X-ray axis"): compact checkable
    // toggle, right of the mode combo.  Applies to EVERY viewport render
    // mode, INCLUDING the shaded preview (caster-layer resolve, default
    // ON; user decision 2026-07-17) -- refreshRenderModeCombo() enables
    // it under the same gate as the mode combo itself (scene loaded,
    // render-owns-scene / bridge available), with no per-mode gating.
    // Style mirrors m_renderModeCombo's bordered chip look, with an
    // accent-tinted `:checked` state so the toggle reads clearly at a
    // glance (same pattern as LogWidget's severity filter pills).
    m_xrayBtn = new QToolButton(this);
    m_xrayBtn->setText(QStringLiteral("X-Ray"));
    m_xrayBtn->setFont(Theme::mono(10));
    m_xrayBtn->setCheckable(true);
    m_xrayBtn->setToolTip(QStringLiteral(
        "See through transmissive surfaces (straight-line) — applies to every view mode, on by default"));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_xrayBtn->setEnabled(false);   // no bridge yet -- refreshRenderModeCombo() re-enables on scene load
    connect(m_xrayBtn, &QToolButton::clicked, this, &TopBar::onXrayButtonToggled);
    layout->addWidget(m_xrayBtn);

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

    m_regionRenderBtn = new QToolButton(this);
    m_regionRenderBtn->setText(QStringLiteral("\xE2\x96\xBE"));
    m_regionRenderBtn->setFont(Theme::sans(11, QFont::DemiBold));
    m_regionRenderBtn->setFixedHeight(28);
    m_regionRenderBtn->setPopupMode(QToolButton::InstantPopup);
    auto* regionMenu = new QMenu(m_regionRenderBtn);
    QAction* regionAction = regionMenu->addAction(QStringLiteral("Render Active Region"));
    connect(regionAction, &QAction::triggered, this, &TopBar::renderRegionClicked);
    m_regionRenderBtn->setMenu(regionMenu);
    m_regionRenderBtn->hide();
    layout->addWidget(m_regionRenderBtn);

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
    m_themeReady = true;
    restyleTheme();
}

void TopBar::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::bgTopBar);
    p.setPen(Theme::borderHairline);
    p.drawLine(0, height() - 1, width(), height() - 1);
    QWidget::paintEvent(event);
}

void TopBar::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void TopBar::restyleTheme()
{
    // LIVE THEME-SWITCH CONTRACT (Theme.h): every one of TopBar's own
    // token-dependent styling sites, re-applied from the CURRENT Theme::
    // token values. Called once at the end of the constructor and again
    // from changeEvent() on every QEvent::PaletteChange. Idempotent,
    // creates no widgets.
    m_themeEpochSeen = Theme::paletteEpoch();
    //
    // Two kinds of site here:
    //  (1) chrome that's baked ONCE at construction and never touched
    //      again by any other code path (wordmark, separators, the
    //      cluster's objectName-scoped QSS, the combo/x-ray chip
    //      stylesheets, the restart icon's tint) -- re-applied directly
    //      below.
    //  (2) chrome that ALREADY has a dedicated update*() method invoked
    //      on every relevant state change (scene identity, the save
    //      chip, the status readout/tag, the integrator chip, the
    //      transport/cancel pills) -- those methods already read
    //      Theme:: tokens live at call time, so re-invoking them here is
    //      enough; no separate restyle logic is duplicated for them.

    if (m_wordmarkLabel) {
        m_wordmarkLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_identitySep) {
        m_identitySep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    }
    if (m_dirtyDotLabel) {
        m_dirtyDotLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 6px;").arg(Theme::hex(Theme::dirty)));
    }
    if (m_dirtyTextLabel) {
        m_dirtyTextLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::dirty)));
    }

    if (m_cluster) {
        m_cluster->setStyleSheet(QStringLiteral(
            "#topBarCluster { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
            .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight))
            .arg(Theme::radiusLarge));
    }

    if (m_restartBtn) {
        // Icon re-tint: Theme::icon() caches pixmaps keyed on (name,
        // size, color, dpr) -- see Theme.h's iconPixmap doc -- so this
        // is a cheap cache hit/insert, not a re-render from scratch.
        m_restartBtn->setIcon(Theme::icon(QStringLiteral("refresh-cw"), 12, Theme::textMuted, Theme::textDisabled));
    }

    if (m_statusRowLabel) {
        m_statusRowLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }

    if (m_integratorSep) {
        m_integratorSep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    }

    if (m_renderModeCombo) {
        m_renderModeCombo->setStyleSheet(QStringLiteral(
            "QComboBox { color: %1; background: transparent; border: 1px solid %2; border-radius: 5px; padding: 2px 6px; }")
            .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::borderLight)));
    }

    if (m_xrayBtn) {
        m_xrayBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background: transparent; border: 1px solid %2; border-radius: 5px; padding: 2px 6px; }"
            "QToolButton:checked { color: %3; border-color: %3; }")
            .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::borderLight), Theme::hex(Theme::accent)));
    }

    // (2) -- re-invoke the existing update*() methods so their
    // already-live-token-reading bodies repaint with the new palette.
    updateSceneIdentity();
    updateSaveChip();
    updateReadout();
    updateIntegratorChip();
    updateTransportButton();
    updateRegionRenderButton();

    // TopBarLogoSwatch and TopBarProgressStrip (m_logoSwatch,
    // m_progressStrip) are custom-painted widgets that read Theme::
    // tokens directly in paintEvent() every frame -- per the LIVE
    // THEME-SWITCH CONTRACT point 3, they need no code here; Qt's own
    // palette-change cascade already calls update() on them.
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
        // N-up multi-viewport (docs/gui/RENDER_MODES.md §7.5): the status
        // line follows the PRIMARY pane's refinement state rather than
        // always pane 0 -- for Single layout primaryPane() is always 0
        // (§7.2's fallback rule), so this is byte-identical to the
        // pre-N-up behaviour there; for N-up layouts it tracks whichever
        // pane the user actually made primary.
        int phase = -1;
        unsigned int sd = 1;
        if (m_bridge->getPaneRefinementStatus(m_bridge->primaryPane(), &phase, &sd)) {
            m_refinementPhase = phase;
            m_refinementScaleDivisor = sd;
        } else {
            m_refinementPhase = -1;
            m_refinementScaleDivisor = 1;
        }
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
        m_statusTagLabel->setText(m_engine && m_engine->isRegionProductionRender()
            ? QStringLiteral("REGION ERROR") : QStringLiteral("ERROR"));
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
        isProductionPaused,
        m_bridge ? m_bridge->viewportRenderModeWantsDenoise() : true);

    m_statusRowLabel->setText(s.text);
    QString statusLabel = s.label;
    if (m_engine && m_engine->isRegionProductionRender()) {
        statusLabel = (isCancelling || m_engineState == RenderEngine::Cancelled)
            ? QStringLiteral("REGION CANCELLED") : QStringLiteral("REGION FINAL");
    } else if (!isProduction && m_bridge) {
        unsigned int l = 0, t = 0, r = 0, b = 0;
        if (m_bridge->getInteractiveRegion(&l, &t, &r, &b)) {
            if (m_refinementScaleDivisor == 1) {
                const QSize dims = m_bridge->cameraSurfaceDimensions();
                const double framePixels = std::max(1.0,
                    static_cast<double>(dims.width()) * dims.height());
                const int percent = static_cast<int>(std::lround(
                    100.0 * static_cast<double>(r - l + 1) * (b - t + 1) / framePixels));
                statusLabel = tr("REGION %1%").arg(percent);
            } else {
                statusLabel = tr("FULL PREVIEW \xE2\x86\x92 REGION");
            }
        }
    }
    m_statusTagLabel->setText(statusLabel);
    // Empty label = nothing to add beside the text (see the label-policy
    // comment in RefinementStatus above) -- hide so no stray gap renders.
    m_statusTagLabel->setVisible(!statusLabel.isEmpty());

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

    // P1 (docs/gui/RENDER_MODES.md §5): this is the single choke point
    // updateControlsEnabled() already has for every render-owns-scene
    // transition (engine state changes, chat-render outstanding changes,
    // a bridge attach/detach) AND, via pollRefinementState()'s own tail
    // call to this method, the 500ms poll tick -- so the combo's current
    // index also gets re-read from the bridge on that cadence, which is
    // the safety net for a controller-side reset-to-"preview" that
    // doesn't route through setViewportBridge (e.g. a scene_variant
    // switch or CST full re-derive that reuses the SAME bridge/controller
    // instance -- see SceneEditController::RebindEditorToJob's doc).
    refreshRenderModeCombo();

    updateTransportButton();
    updateRegionRenderButton();
}

void TopBar::updateRegionRenderButton()
{
    if (!m_regionRenderBtn) return;
    unsigned int left = 0, top = 0, right = 0, bottom = 0;
    const bool hasRegion = m_bridge
        && m_bridge->getInteractiveRegion(&left, &top, &right, &bottom);
    const bool idle = m_engineState != RenderEngine::Rendering
        && m_engineState != RenderEngine::Cancelling
        && m_engineState != RenderEngine::Loading;
    const bool supported = m_engine && m_engine->productionRasterizerHonorsRegion();
    m_regionRenderBtn->setVisible(hasRegion && idle);
    m_regionRenderBtn->setEnabled(hasRegion && idle
        && m_canStartProductionRender && supported);
    m_regionRenderBtn->setToolTip(supported
        ? QStringLiteral("Render only the active region")
        : QStringLiteral("The production rasterizer does not support region rendering"));
    m_regionRenderBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background-color: %1; color: %2; border: none;"
        " border-radius: %3px; padding: 5px 9px; }")
        .arg(Theme::hex(Theme::accent), Theme::hex(Theme::textOnAccent))
        .arg(Theme::radiusMedium));
}

void TopBar::refreshRenderModeCombo()
{
    if (!m_renderModeCombo) return;

    // Same render-owns-scene / refinement-disabled predicate as
    // m_restartBtn just above -- SetViewportRenderMode itself refuses
    // under the identical guard (render-owns-scene) plus skeleton mode,
    // so disabling here just keeps the UI honest about a set that would
    // be refused anyway rather than inventing a second source of truth.
    const bool disabled = !m_bridge
        || m_engineState == RenderEngine::Rendering
        || m_engineState == RenderEngine::Cancelling
        || (m_chatPanel && m_chatPanel->isChatRenderOutstanding());
    m_renderModeCombo->setEnabled(!disabled);

    if (!m_bridge) {
        if (m_xrayBtn) m_xrayBtn->setEnabled(false);   // nothing to resync against either
        return;   // nothing to resync the current index against
    }

    // Re-read the ACTIVE mode -- never assume the combo's current index
    // still matches what the controller actually holds (see this
    // method's doc for the reset paths that don't go through
    // setViewportBridge).
    const QString activeName = m_bridge->viewportRenderMode();
    const int wantIndex = m_renderModeCombo->findData(activeName, Qt::UserRole);
    if (wantIndex >= 0 && wantIndex != m_renderModeCombo->currentIndex()) {
        // QSignalBlocker so this programmatic sync never re-enters
        // onRenderModeComboChanged (mirrors ChatPanel::revertProviderModelWidgets'
        // identical pattern for m_providerCombo).
        const QSignalBlocker blocker(m_renderModeCombo);
        m_renderModeCombo->setCurrentIndex(wantIndex);
    }
    // (wantIndex < 0 means an unknown name -- shouldn't happen, registry
    // mismatch -- and wantIndex == currentIndex() means already in sync;
    // either way the combo's index is left alone.)

    // X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): enabled under
    // X-ray applies to EVERY mode including the shaded preview
    // (caster-layer resolve, default ON) -- enabled whenever the
    // scene/render gating allows the mode combo itself.
    if (m_xrayBtn) {
        // X-ray applies to EVERY mode including the shaded preview
        // (caster-layer resolve, default ON; user decision 2026-07-17) --
        // no data-mode gating.  P2a review fix: EXCEPT the BeautyVariant
        // rows (deep_reflect/direct) -- those drive a wholly separate
        // ephemeral PT pipeline that never reads the x-ray flag/caster, so
        // toggling it there would silently no-op.  Read the ACTIVE combo
        // item's stashed isVariant flag (Qt::UserRole + 1, set once at
        // construction) rather than re-querying the registry here.
        const int activeIndex = m_renderModeCombo->currentIndex();
        const bool activeIsVariant = activeIndex >= 0
            && m_renderModeCombo->itemData(activeIndex, Qt::UserRole + 1).toBool();
        const bool xrayEnabled = !disabled && !activeIsVariant;
        m_xrayBtn->setEnabled(xrayEnabled);

        // Re-read the CURRENT flag -- never assume the button's checked
        // state still matches the controller (same reset-on-rebind
        // hazard as the mode combo; the "never blocks" C-ABI contract
        // means this is safe to call unconditionally, even mid-render).
        const bool activeXray = m_bridge->viewportXray();
        if (activeXray != m_xrayBtn->isChecked()) {
            // QSignalBlocker so this programmatic sync never re-enters
            // onXrayButtonToggled (same pattern as the combo above).
            const QSignalBlocker blocker(m_xrayBtn);
            m_xrayBtn->setChecked(activeXray);
        }
    }
}

void TopBar::onRenderModeComboChanged(int index)
{
    if (!m_bridge || !m_renderModeCombo) return;
    if (index < 0) return;
    const QString name = m_renderModeCombo->itemData(index, Qt::UserRole).toString();
    if (name.isEmpty()) return;
    // The set CAN fail (render-owns-scene, unknown/non-selectable name,
    // skeleton mode) -- never assume it took effect.  Re-read the
    // EFFECTIVE mode unconditionally so a refusal snaps the combo back
    // to whatever's actually active instead of showing the user's
    // rejected pick as if it had landed.
    m_bridge->setViewportRenderMode(name);
    refreshRenderModeCombo();
}

void TopBar::onXrayButtonToggled(bool checked)
{
    if (!m_bridge || !m_xrayBtn) return;
    // The set CAN fail (render-owns-scene, skeleton mode) -- never
    // assume it took effect.  Re-read the EFFECTIVE flag unconditionally
    // so a refusal snaps the button back to whatever's actually active
    // instead of showing the user's rejected click as if it had landed
    // (mirrors onRenderModeComboChanged's identical discipline).
    m_bridge->setViewportXray(checked);
    refreshRenderModeCombo();
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
    // Mirrors TopBar.swift's transportPill(filled: true).  P3 fix
    // (2026-07-23 review): Theme::textOnAccent, not a hardcoded
    // `rgba(0,0,0,0.92)` -- that literal is Dark-mode-correct (near-
    // black on the brighter Dark accent) but goes low-contrast against
    // Light's darkened accent, the same defect fix 3 already corrected
    // for Theme::textOnAccent itself (now white in Light, near-black in
    // Dark -- see Theme.cpp's LightPalette doc on that token).
    const QString filledStyle = QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: none;"
        " border-radius: %3px; padding: 6px 15px; }")
        .arg(Theme::hex(Theme::accent), Theme::hex(Theme::textOnAccent))
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
