//////////////////////////////////////////////////////////////////////
//
//  ProposalCard.cpp - Diff-card implementation.  See header for the
//    macOS ProposalDiffCard cross-reference.
//
//////////////////////////////////////////////////////////////////////

#include "ProposalCard.h"
#include "Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace {

// Returns `c` with its alpha channel replaced by `alphaFraction` (0..1) --
// a small live-token-friendly stand-in for the QColor(r,g,b,alpha)
// reconstructions this file used to do inline at every alpha-blended
// fill, so each one is always computed from whatever Theme:: token value
// is CURRENT at call time (never a value frozen at some earlier point).
QColor withAlpha(const QColor& c, double alphaFraction)
{
    QColor result = c;
    result.setAlphaF(alphaFraction);
    return result;
}

// LIVE THEME-SWITCH CONTRACT (Theme.h): every one of these free
// functions returns the stylesheet string for one styling site, so both
// the constructor's build*() methods AND ProposalCard::restyleTheme()
// can call the SAME formula from the CURRENT token values instead of
// two copies that can drift out of sync.

QString cardOuterStyleSheet()
{
    return QStringLiteral(
        "ProposalCard { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
        .arg(Theme::hex(Theme::bgCardDeep))
        .arg(Theme::rgba(withAlpha(Theme::accent, 0.32)))
        .arg(Theme::radiusCard);
}

QString hairlineStyleSheet()
{
    return QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderHairline));
}

QString countChipStyleSheet(const QColor& fg, const QColor& bg)
{
    return QStringLiteral(
        "QLabel { color: %1; background-color: %2; border-radius: 4px; padding: 2px 6px; }")
        .arg(Theme::hex(fg), Theme::rgba(bg));
}

QString diffLineStyleSheet(bool isAddition)
{
    const QColor fg = isAddition ? Theme::successLight : Theme::error;
    const QColor bg = isAddition ? withAlpha(Theme::success, 0.1) : withAlpha(Theme::errorStrong, 0.1);
    return QStringLiteral("QLabel { color: %1; background-color: %2; padding: 1px 12px; }")
        .arg(Theme::hex(fg), Theme::rgba(bg));
}

// The Apply button's text sits directly on a SOLID Theme::accent fill --
// Theme::textOnAccent (see Theme.h) is the token added for exactly this,
// mirroring macOS ProposalCard.swift's unconditional `Color(hex:
// 0x0d1116)` on its own matching button.  The disabled background used
// to be a bare Theme::whiteAlpha(14%) -- always white regardless of
// mode, wrong in Light -- now Theme::fillActive, the theme-aware
// "visible-but-faint fill" token (whiteAlpha(31) ~12% in Dark,
// blackAlpha(26) ~10% in Light), the closest existing token to the old
// hardcoded 14% white's visual weight.
QString applyButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: none; border-radius: %3px; padding: 8px 0; }"
        "QPushButton:disabled { background-color: %4; color: %5; }")
        .arg(Theme::hex(Theme::accent))
        .arg(Theme::hex(Theme::textOnAccent))
        .arg(Theme::radiusMedium)
        .arg(Theme::rgba(Theme::fillActive))
        .arg(Theme::hex(Theme::textDisabled));
}

QString rejectButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2; border-radius: %3px; padding: 8px 15px; }"
        "QPushButton:disabled { color: %4; border-color: %5; }")
        .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderStrong))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight));
}

QString undoButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2; border-radius: 6px; padding: 4px 10px; }")
        .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderStrong));
}

QLabel* makeCountChip(const QString& text, const QColor& fg, const QColor& bg)
{
    auto* lbl = new QLabel(text);
    lbl->setFont(Theme::mono(9));
    lbl->setStyleSheet(countChipStyleSheet(fg, bg));
    return lbl;
}

} // namespace

ProposalCard::ProposalCard(const ProposalEntry& proposal, const QString& sceneFileName, QWidget* parent)
    : QWidget(parent)
    , m_proposal(proposal)
    , m_sceneFileName(sceneFileName)
{
    setStyleSheet(cardOuterStyleSheet());
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildHeader(root);

    m_sep1 = new QFrame(this);
    m_sep1->setFrameShape(QFrame::HLine);
    m_sep1->setStyleSheet(hairlineStyleSheet());
    m_sep1->setFixedHeight(1);
    root->addWidget(m_sep1);

    buildBody(root);

    m_sep2 = new QFrame(this);
    m_sep2->setFrameShape(QFrame::HLine);
    m_sep2->setStyleSheet(hairlineStyleSheet());
    m_sep2->setFixedHeight(1);
    root->addWidget(m_sep2);

    buildFooter(root);

    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies every token-
    // dependent style above from the CURRENT Theme:: values (a harmless
    // redundant re-write at construction time, since the widgets above
    // were already built from those same CURRENT values via the shared
    // *StyleSheet() helpers) and installs this as the single source of
    // truth changeEvent() re-invokes on every QEvent::PaletteChange.
    m_themeReady = true;
    restyleTheme();
}

// ============================================================
// Header: "Proposed edit" + scene file + −N/+N count chips
// ============================================================

void ProposalCard::buildHeader(QVBoxLayout* root)
{
    auto* row = new QHBoxLayout();
    row->setContentsMargins(12, 9, 12, 9);
    row->setSpacing(8);

    m_titleLabel = new QLabel(tr("Proposed edit"));
    m_titleLabel->setFont(Theme::sans(12, QFont::DemiBold));
    m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    row->addWidget(m_titleLabel);

    if (!m_sceneFileName.isEmpty()) {
        m_fileLabel = new QLabel(m_sceneFileName);
        m_fileLabel->setFont(Theme::mono(10));
        m_fileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        row->addWidget(m_fileLabel);
    }

    row->addStretch(1);

    const int minus = minusCount();
    const int plus = plusCount();
    if (minus > 0) {
        m_minusChip = makeCountChip(QStringLiteral("-%1").arg(minus), Theme::error,
                                     withAlpha(Theme::errorStrong, 0.1));
        row->addWidget(m_minusChip);
    }
    if (plus > 0) {
        m_plusChip = makeCountChip(QStringLiteral("+%1").arg(plus), Theme::successLight,
                                    withAlpha(Theme::success, 0.12));
        row->addWidget(m_plusChip);
    }

    root->addLayout(row);
}

int ProposalCard::plusCount() const
{
    if (m_proposal.kind == QLatin1String("insert_chunk")) {
        int n = 0;
        const QStringList lines = m_proposal.chunkText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        for (const QString& l : lines) {
            if (!l.trimmed().isEmpty()) ++n;
        }
        return n;
    }
    if (m_proposal.kind == QLatin1String("remove_chunk")) return 0;
    return 1;   // param_edit
}

int ProposalCard::minusCount() const
{
    return m_proposal.kind == QLatin1String("remove_chunk") ? 1 : 0;
}

// ============================================================
// Body: diff-styled, keyed by kind
// ============================================================

QWidget* ProposalCard::makeBlockLabelRow(const QString& text, QWidget** outDot, QLabel** outLabel) const
{
    auto* container = new QWidget;
    auto* row = new QHBoxLayout(container);
    row->setContentsMargins(12, 2, 12, 2);
    row->setSpacing(7);

    auto* dot = new QLabel;
    dot->setFixedSize(5, 5);
    dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 2px;")
        .arg(Theme::hex(Theme::accentSoft)));
    row->addWidget(dot);

    auto* lbl = new QLabel(text);
    lbl->setFont(Theme::mono(10));
    lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::accentSoft)));
    row->addWidget(lbl, 1);

    if (outDot) *outDot = dot;
    if (outLabel) *outLabel = lbl;
    return container;
}

QLabel* ProposalCard::makeDiffLine(const QString& sign, const QString& text, bool isAddition)
{
    auto* lbl = new QLabel(QStringLiteral("%1 %2").arg(sign, text));
    lbl->setFont(Theme::mono(11));
    lbl->setWordWrap(true);
    lbl->setStyleSheet(diffLineStyleSheet(isAddition));
    return lbl;
}

void ProposalCard::buildBody(QVBoxLayout* root)
{
    auto* body = new QVBoxLayout();
    body->setContentsMargins(0, 6, 0, 6);
    body->setSpacing(2);

    if (m_proposal.kind == QLatin1String("insert_chunk")) {
        static const int kMaxLinesShown = 12;
        const QStringList allLines = m_proposal.chunkText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        QString label = m_proposal.target;
        if (label.isEmpty()) {
            for (const QString& l : allLines) {
                if (!l.trimmed().isEmpty()) { label = l.trimmed(); break; }
            }
            if (label.isEmpty()) label = QStringLiteral("block");
        }
        body->addWidget(makeBlockLabelRow(tr("new block \xC2\xB7 %1").arg(label), &m_blockDot, &m_blockLabel));

        const int shown = std::min<int>(kMaxLinesShown, allLines.size());
        for (int i = 0; i < shown; ++i) {
            QLabel* line = makeDiffLine(QStringLiteral("+"), allLines.at(i), true);
            body->addWidget(line);
            m_diffAdditionLines.append(line);
        }
        const int remaining = allLines.size() - shown;
        if (remaining > 0) {
            m_moreLabel = new QLabel(tr("\xE2\x80\xA6 %1 more").arg(remaining));
            m_moreLabel->setFont(Theme::mono(10));
            m_moreLabel->setStyleSheet(QStringLiteral("color: %1; padding: 2px 12px 0 12px;")
                .arg(Theme::hex(Theme::textDim)));
            body->addWidget(m_moreLabel);
        }
    } else if (m_proposal.kind == QLatin1String("remove_chunk")) {
        const QString text = m_proposal.entityKind.isEmpty()
            ? m_proposal.target
            : QStringLiteral("%1 (%2)").arg(m_proposal.target, m_proposal.entityKind);
        QLabel* line = makeDiffLine(QStringLiteral("\xE2\x88\x92"), text, false);
        body->addWidget(line);
        m_diffRemovalLines.append(line);
    } else {
        // param_edit (default -- also the fallback for any unknown kind,
        // matching the Mac card's `default:` branch).
        body->addWidget(makeBlockLabelRow(tr("param edit \xC2\xB7 %1").arg(m_proposal.target), &m_blockDot, &m_blockLabel));
        QLabel* line = makeDiffLine(QStringLiteral("+"),
            QStringLiteral("%1: %2").arg(m_proposal.param, m_proposal.value), true);
        body->addWidget(line);
        m_diffAdditionLines.append(line);
    }

    root->addLayout(body);
}

// ============================================================
// Footer: keyed by status
// ============================================================

void ProposalCard::buildFooter(QVBoxLayout* root)
{
    auto* row = new QHBoxLayout();
    row->setContentsMargins(10, 10, 10, 10);
    row->setSpacing(8);

    if (m_proposal.status == QLatin1String("pending")) {
        m_applyBtn = new QPushButton(tr("Apply \xE2\x80\x94 restarts refinement"));
        m_applyBtn->setFont(Theme::sans(12, QFont::DemiBold));
        m_applyBtn->setCursor(Qt::PointingHandCursor);
        m_applyBtn->setFlat(true);
        m_applyBtn->setStyleSheet(applyButtonStyleSheet());
        connect(m_applyBtn, &QPushButton::clicked, this, [this]() { emit applyClicked(m_proposal.id); });
        row->addWidget(m_applyBtn, 1);

        m_rejectBtn = new QPushButton(tr("Reject"));
        m_rejectBtn->setFont(Theme::sans(12));
        m_rejectBtn->setCursor(Qt::PointingHandCursor);
        m_rejectBtn->setFlat(true);
        m_rejectBtn->setStyleSheet(rejectButtonStyleSheet());
        connect(m_rejectBtn, &QPushButton::clicked, this, [this]() { emit rejectClicked(m_proposal.id); });
        row->addWidget(m_rejectBtn);
    } else if (m_proposal.status == QLatin1String("applied")) {
        m_footerStatusColor = FooterStatusColor::Success;
        m_footerStatusLabel = new QLabel(tr("\xE2\x9C\x93 Applied \xC2\xB7 one undo step"));
        m_footerStatusLabel->setFont(Theme::sans(12));
        m_footerStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::successLight)));
        row->addWidget(m_footerStatusLabel);
        row->addStretch(1);

        m_undoBtn = new QPushButton(tr("\xE2\x86\xBA Undo"));
        m_undoBtn->setFont(Theme::sans(11));
        m_undoBtn->setCursor(Qt::PointingHandCursor);
        m_undoBtn->setFlat(true);
        m_undoBtn->setStyleSheet(undoButtonStyleSheet());
        connect(m_undoBtn, &QPushButton::clicked, this, [this]() { emit undoClicked(); });
        row->addWidget(m_undoBtn);
    } else if (m_proposal.status == QLatin1String("rejected")) {
        m_footerStatusColor = FooterStatusColor::Faint;
        m_footerStatusLabel = new QLabel(tr("\xE2\x9C\x95 Rejected \xE2\x80\x94 scene unchanged"));
        m_footerStatusLabel->setFont(Theme::sans(12));
        m_footerStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
        row->addWidget(m_footerStatusLabel);
        row->addStretch(1);
        // No "restore proposal" affordance: the controller does not
        // expose re-staging a rejected proposal (matches the Mac card's
        // "no fake affordances" note) -- plain label only.
    } else if (m_proposal.status == QLatin1String("conflict")) {
        m_footerStatusColor = FooterStatusColor::Warn;
        m_footerStatusLabel = new QLabel(tr("\xE2\x9A\xA0 Conflict \xE2\x80\x94 scene changed since this was proposed"));
        m_footerStatusLabel->setFont(Theme::sans(12));
        m_footerStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::warn)));
        row->addWidget(m_footerStatusLabel);
        row->addStretch(1);
    } else {
        // Unknown status -- render nothing (mirrors the Mac card's
        // `default: EmptyView()`); the row stays empty rather than
        // fabricating an affordance for a state we don't understand.
    }

    m_footerWidget = new QWidget;
    m_footerWidget->setLayout(row);
    m_footerWidget->setStyleSheet(QStringLiteral("background-color: %1;").arg(Theme::hex(Theme::bgCardDeep)));
    root->addWidget(m_footerWidget);
}

void ProposalCard::setActionsEnabled(bool enabled)
{
    if (m_applyBtn) {
        m_applyBtn->setEnabled(enabled);
        m_applyBtn->setToolTip(enabled ? QString() : tr("Unavailable while a render is in flight"));
    }
    if (m_rejectBtn) {
        m_rejectBtn->setEnabled(enabled);
        m_rejectBtn->setToolTip(enabled ? QString() : tr("Unavailable while a render is in flight"));
    }
    // Round-2 P1: the applied-state Undo drives the same scene-mutating
    // path as Apply/Reject and gets the same gate.
    if (m_undoBtn) {
        m_undoBtn->setEnabled(enabled);
        m_undoBtn->setToolTip(enabled ? QString() : tr("Unavailable while a render is in flight"));
    }
}

// ============================================================
// LIVE THEME-SWITCH CONTRACT (Theme.h) -- see MainWindow::restyleTheme
// / ChatPanel::restyleTheme for the reference implementations this
// mirrors.
// ============================================================

void ProposalCard::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ProposalCard::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();
    setStyleSheet(cardOuterStyleSheet());

    const QString hairline = hairlineStyleSheet();
    if (m_sep1) m_sep1->setStyleSheet(hairline);
    if (m_sep2) m_sep2->setStyleSheet(hairline);

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_fileLabel) {
        m_fileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }
    if (m_minusChip) {
        m_minusChip->setStyleSheet(countChipStyleSheet(Theme::error, withAlpha(Theme::errorStrong, 0.1)));
    }
    if (m_plusChip) {
        m_plusChip->setStyleSheet(countChipStyleSheet(Theme::successLight, withAlpha(Theme::success, 0.12)));
    }

    if (m_blockDot) {
        m_blockDot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 2px;")
            .arg(Theme::hex(Theme::accentSoft)));
    }
    if (m_blockLabel) {
        m_blockLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::accentSoft)));
    }
    for (QLabel* line : std::as_const(m_diffAdditionLines)) {
        if (line) line->setStyleSheet(diffLineStyleSheet(true));
    }
    for (QLabel* line : std::as_const(m_diffRemovalLines)) {
        if (line) line->setStyleSheet(diffLineStyleSheet(false));
    }
    if (m_moreLabel) {
        m_moreLabel->setStyleSheet(QStringLiteral("color: %1; padding: 2px 12px 0 12px;")
            .arg(Theme::hex(Theme::textDim)));
    }

    if (m_applyBtn)  m_applyBtn->setStyleSheet(applyButtonStyleSheet());
    if (m_rejectBtn) m_rejectBtn->setStyleSheet(rejectButtonStyleSheet());
    if (m_undoBtn)   m_undoBtn->setStyleSheet(undoButtonStyleSheet());

    if (m_footerStatusLabel && m_footerStatusColor != FooterStatusColor::None) {
        QColor c = Theme::textFaint;
        switch (m_footerStatusColor) {
        case FooterStatusColor::Success: c = Theme::successLight; break;
        case FooterStatusColor::Faint:   c = Theme::textFaint;    break;
        case FooterStatusColor::Warn:    c = Theme::warn;         break;
        case FooterStatusColor::None:    break;
        }
        m_footerStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(c)));
    }
    if (m_footerWidget) {
        m_footerWidget->setStyleSheet(QStringLiteral("background-color: %1;").arg(Theme::hex(Theme::bgCardDeep)));
    }
}
