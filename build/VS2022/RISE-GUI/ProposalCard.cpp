//////////////////////////////////////////////////////////////////////
//
//  ProposalCard.cpp - Diff-card implementation.  See header for the
//    macOS ProposalDiffCard cross-reference.
//
//////////////////////////////////////////////////////////////////////

#include "ProposalCard.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QStringList>

#include <algorithm>

namespace {

QWidget* makeCountChip(const QString& text, const QColor& fg, const QColor& bg)
{
    auto* lbl = new QLabel(text);
    lbl->setFont(Theme::mono(9));
    lbl->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background-color: %2; border-radius: 4px; padding: 2px 6px; }")
        .arg(Theme::hex(fg), Theme::rgba(bg)));
    return lbl;
}

} // namespace

ProposalCard::ProposalCard(const ProposalEntry& proposal, const QString& sceneFileName, QWidget* parent)
    : QWidget(parent)
    , m_proposal(proposal)
    , m_sceneFileName(sceneFileName)
{
    setStyleSheet(QStringLiteral(
        "ProposalCard { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
        .arg(Theme::hex(Theme::bgCardDeep))
        .arg(Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(),
                                 static_cast<int>(0.32 * 255))))
        .arg(Theme::radiusCard));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    buildHeader(root);

    auto* sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderHairline)));
    sep1->setFixedHeight(1);
    root->addWidget(sep1);

    buildBody(root);

    auto* sep2 = new QFrame(this);
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderHairline)));
    sep2->setFixedHeight(1);
    root->addWidget(sep2);

    buildFooter(root);
}

// ============================================================
// Header: "Proposed edit" + scene file + −N/+N count chips
// ============================================================

void ProposalCard::buildHeader(QVBoxLayout* root)
{
    auto* row = new QHBoxLayout();
    row->setContentsMargins(12, 9, 12, 9);
    row->setSpacing(8);

    auto* title = new QLabel(tr("Proposed edit"));
    title->setFont(Theme::sans(12, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    row->addWidget(title);

    if (!m_sceneFileName.isEmpty()) {
        auto* file = new QLabel(m_sceneFileName);
        file->setFont(Theme::mono(10));
        file->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        row->addWidget(file);
    }

    row->addStretch(1);

    const int minus = minusCount();
    const int plus = plusCount();
    if (minus > 0) {
        row->addWidget(makeCountChip(QStringLiteral("-%1").arg(minus), Theme::error,
                                      QColor(Theme::errorStrong.red(), Theme::errorStrong.green(),
                                             Theme::errorStrong.blue(), static_cast<int>(0.1 * 255))));
    }
    if (plus > 0) {
        row->addWidget(makeCountChip(QStringLiteral("+%1").arg(plus), Theme::successLight,
                                      QColor(Theme::success.red(), Theme::success.green(),
                                             Theme::success.blue(), static_cast<int>(0.12 * 255))));
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

QWidget* ProposalCard::makeBlockLabelRow(const QString& text) const
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

    return container;
}

QWidget* ProposalCard::makeDiffLine(const QString& sign, const QString& text, bool isAddition) const
{
    auto* lbl = new QLabel(QStringLiteral("%1 %2").arg(sign, text));
    lbl->setFont(Theme::mono(11));
    lbl->setWordWrap(true);
    const QColor fg = isAddition ? Theme::successLight : Theme::error;
    const QColor bg = isAddition
        ? QColor(Theme::success.red(), Theme::success.green(), Theme::success.blue(), static_cast<int>(0.1 * 255))
        : QColor(Theme::errorStrong.red(), Theme::errorStrong.green(), Theme::errorStrong.blue(), static_cast<int>(0.1 * 255));
    lbl->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background-color: %2; padding: 1px 12px; }")
        .arg(Theme::hex(fg), Theme::rgba(bg)));
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
        body->addWidget(makeBlockLabelRow(tr("new block \xC2\xB7 %1").arg(label)));

        const int shown = std::min<int>(kMaxLinesShown, allLines.size());
        for (int i = 0; i < shown; ++i) {
            body->addWidget(makeDiffLine(QStringLiteral("+"), allLines.at(i), true));
        }
        const int remaining = allLines.size() - shown;
        if (remaining > 0) {
            auto* more = new QLabel(tr("\xE2\x80\xA6 %1 more").arg(remaining));
            more->setFont(Theme::mono(10));
            more->setStyleSheet(QStringLiteral("color: %1; padding: 2px 12px 0 12px;")
                .arg(Theme::hex(Theme::textDim)));
            body->addWidget(more);
        }
    } else if (m_proposal.kind == QLatin1String("remove_chunk")) {
        const QString text = m_proposal.entityKind.isEmpty()
            ? m_proposal.target
            : QStringLiteral("%1 (%2)").arg(m_proposal.target, m_proposal.entityKind);
        body->addWidget(makeDiffLine(QStringLiteral("\xE2\x88\x92"), text, false));
    } else {
        // param_edit (default -- also the fallback for any unknown kind,
        // matching the Mac card's `default:` branch).
        body->addWidget(makeBlockLabelRow(tr("param edit \xC2\xB7 %1").arg(m_proposal.target)));
        body->addWidget(makeDiffLine(QStringLiteral("+"),
            QStringLiteral("%1: %2").arg(m_proposal.param, m_proposal.value), true));
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
        m_applyBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; color: #0d1116; border: none; border-radius: %2px; padding: 8px 0; }"
            "QPushButton:disabled { background-color: %3; color: %4; }")
            .arg(Theme::hex(Theme::accent))
            .arg(Theme::radiusMedium)
            .arg(Theme::rgba(Theme::whiteAlpha(int(0.14 * 255))))
            .arg(Theme::hex(Theme::textDisabled)));
        connect(m_applyBtn, &QPushButton::clicked, this, [this]() { emit applyClicked(m_proposal.id); });
        row->addWidget(m_applyBtn, 1);

        m_rejectBtn = new QPushButton(tr("Reject"));
        m_rejectBtn->setFont(Theme::sans(12));
        m_rejectBtn->setCursor(Qt::PointingHandCursor);
        m_rejectBtn->setFlat(true);
        m_rejectBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2; border-radius: %3px; padding: 8px 15px; }"
            "QPushButton:disabled { color: %4; border-color: %5; }")
            .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderStrong))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
        connect(m_rejectBtn, &QPushButton::clicked, this, [this]() { emit rejectClicked(m_proposal.id); });
        row->addWidget(m_rejectBtn);
    } else if (m_proposal.status == QLatin1String("applied")) {
        auto* lbl = new QLabel(tr("\xE2\x9C\x93 Applied \xC2\xB7 one undo step"));
        lbl->setFont(Theme::sans(12));
        lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::successLight)));
        row->addWidget(lbl);
        row->addStretch(1);

        m_undoBtn = new QPushButton(tr("\xE2\x86\xBA Undo"));
        m_undoBtn->setFont(Theme::sans(11));
        m_undoBtn->setCursor(Qt::PointingHandCursor);
        m_undoBtn->setFlat(true);
        m_undoBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2; border-radius: 6px; padding: 4px 10px; }")
            .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderStrong)));
        connect(m_undoBtn, &QPushButton::clicked, this, [this]() { emit undoClicked(); });
        row->addWidget(m_undoBtn);
    } else if (m_proposal.status == QLatin1String("rejected")) {
        auto* lbl = new QLabel(tr("\xE2\x9C\x95 Rejected \xE2\x80\x94 scene unchanged"));
        lbl->setFont(Theme::sans(12));
        lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
        row->addWidget(lbl);
        row->addStretch(1);
        // No "restore proposal" affordance: the controller does not
        // expose re-staging a rejected proposal (matches the Mac card's
        // "no fake affordances" note) -- plain label only.
    } else if (m_proposal.status == QLatin1String("conflict")) {
        auto* lbl = new QLabel(tr("\xE2\x9A\xA0 Conflict \xE2\x80\x94 scene changed since this was proposed"));
        lbl->setFont(Theme::sans(12));
        lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::warn)));
        row->addWidget(lbl);
        row->addStretch(1);
    } else {
        // Unknown status -- render nothing (mirrors the Mac card's
        // `default: EmptyView()`); the row stays empty rather than
        // fabricating an affordance for a state we don't understand.
    }

    auto* footerWidget = new QWidget;
    footerWidget->setLayout(row);
    footerWidget->setStyleSheet(QStringLiteral("background-color: %1;").arg(Theme::hex(Theme::bgCardDeep)));
    root->addWidget(footerWidget);
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
