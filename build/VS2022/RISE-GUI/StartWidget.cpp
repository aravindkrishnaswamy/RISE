//////////////////////////////////////////////////////////////////////
//
//  StartWidget.cpp - Windows Qt implementation of the start screen.
//
//////////////////////////////////////////////////////////////////////

#include "StartWidget.h"
#include "Theme.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSize>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

// A recent-file row: the whole row is clickable for an EXISTING file
// (opens it); a missing file has no click handler at all (spec §4.1 --
// "clicking a missing row does nothing (no error dialog)", its ✕ is the
// only affordance).  Plain QWidget (no Q_OBJECT/signals) so it needs no
// moc registration -- mousePressEvent is a virtual override, not a slot.
class RecentRow : public QWidget
{
public:
    RecentRow(std::function<void()> onClick, QWidget* parent = nullptr)
        : QWidget(parent), m_onClick(std::move(onClick))
    {
        setCursor(m_onClick ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_onClick) m_onClick();
        QWidget::mousePressEvent(event);
    }

    // Option-B restyle (2026-07-16): hover fill tint so the row visibly
    // reacts.  Palette toggle rather than QSS -- a non-Q_OBJECT class has
    // metaObject className "QWidget", so a QSS type selector would style
    // every child widget too.  Clickable rows only.
    void enterEvent(QEnterEvent* event) override
    {
        if (m_onClick) {
            QPalette pal = palette();
            pal.setColor(QPalette::Window, Theme::fillHover);
            setPalette(pal);
            setAutoFillBackground(true);
        }
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override
    {
        setAutoFillBackground(false);
        QWidget::leaveEvent(event);
    }

private:
    std::function<void()> m_onClick;
};

} // namespace

StartWidget::StartWidget(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);

    // Window fill: styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT
    // (Theme.h). setAutoFillBackground(true) is structural (not token-
    // dependent) and stays here; the color itself is applied once by the
    // ctor-end restyleTheme() call below, which runs before this widget
    // is ever shown.
    setAutoFillBackground(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* hcenter = new QHBoxLayout();
    hcenter->addStretch(1);

    auto* container = new QWidget(this);
    container->setMinimumWidth(680);
    container->setMaximumWidth(860);
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(32, 32, 32, 32);
    containerLayout->setSpacing(0);

    // ---- Error banner (spec §8: a failed load lands back here) --------
    m_errorBanner = new QWidget(container);
    auto* bannerLayout = new QHBoxLayout(m_errorBanner);
    bannerLayout->setContentsMargins(12, 8, 12, 8);
    bannerLayout->setSpacing(8);
    // Icon-system upgrade: mirrors StartView.swift's errorBanner
    // (Image(systemName: "exclamationmark.triangle"), size 12,
    // Theme.error) -- Lucide "triangle-alert" tinted error.
    auto* bannerIcon = new QLabel(m_errorBanner);
    // Theme::bindIconLabel keeps this pixmap correct across a mixed-DPI
    // monitor drag (plain setPixmap(iconPixmap(..., devicePixelRatioF()))
    // captures DPR once at construction and goes stale) AND across a
    // live theme switch -- the PROVIDER overload is required here since
    // Theme::error is a Theme:: token (Theme.h's bindIconLabel doc); the
    // fixed-color overload would silently go stale on the next switch.
    Theme::bindIconLabel(bannerIcon, QStringLiteral("triangle-alert"), 14, []() { return Theme::error; });
    bannerLayout->addWidget(bannerIcon);
    m_errorLabel = new QLabel(m_errorBanner);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setFont(Theme::sans(12));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    bannerLayout->addWidget(m_errorLabel, 1);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_errorBanner->hide();
    containerLayout->addWidget(m_errorBanner);
    containerLayout->addSpacing(18);

    // ---- Title + subtitle ----------------------------------------------
    m_titleLabel = new QLabel(tr("Start a session"), container);
    m_titleLabel->setFont(Theme::sans(20, QFont::Medium));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    containerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel(
        tr("Open a scene you've worked on, browse for a file, or describe one for the agent to build."),
        container);
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->setFont(Theme::sans(12));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    containerLayout->addWidget(m_subtitleLabel);
    containerLayout->addSpacing(22);

    // ---- Two-column row (decision #1: grouped 2-column, not 3 equal) --
    auto* columns = new QHBoxLayout();
    columns->setSpacing(22);
    columns->addWidget(buildOpenColumn(), 1);
    columns->addWidget(buildCreateColumn(), 1);
    containerLayout->addLayout(columns);

    hcenter->addWidget(container);
    hcenter->addStretch(1);
    outer->addLayout(hcenter);
    outer->addStretch(1);

    rebuildRecentsList();
    updateReadinessUI();
    m_themeReady = true;
    restyleTheme();
}

void StartWidget::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void StartWidget::restyleTheme()
{
    // LIVE THEME-SWITCH CONTRACT (Theme.h): every one of StartWidget's
    // own token-dependent styling sites, re-applied from the CURRENT
    // Theme:: token values. Called once at the end of the constructor
    // and again from changeEvent() on every QEvent::PaletteChange.
    // Idempotent.
    m_themeEpochSeen = Theme::paletteEpoch();

    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgCenter);
        setPalette(pal);
    }

    if (m_errorBanner) {
        m_errorBanner->setStyleSheet(QStringLiteral(
            "background-color: rgba(%1, %2, %3, 20); border: 1px solid rgba(%1, %2, %3, 90); border-radius: %4px;")
            .arg(Theme::error.red()).arg(Theme::error.green()).arg(Theme::error.blue())
            .arg(Theme::radiusMedium));
    }
    if (m_errorLabel) {
        m_errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    }

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_subtitleLabel) {
        m_subtitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    }

    // ---- Left column (open) --------------------------------------------
    if (m_openColumnHeader) {
        m_openColumnHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    }
    if (m_recentsContainer) {
        // P1 fix (2026-07-23 review): scoped to #recentsCard, not a bare
        // "QFrame { ... }" type selector -- QLabel derives from QFrame, so
        // an unscoped rule here boxed every child label inside the recent-
        // rows list (nameLabel/subLabel/iconLabel/chevron) with this card's
        // background+border+radius. The HLine separators built in
        // rebuildRecentsList() are unaffected either way -- they always set
        // their own more-specific inline stylesheet (border:none, a flat
        // borderHairline background), which already wins over any ambient
        // QFrame rule regardless of scoping.
        m_recentsContainer->setStyleSheet(QStringLiteral(
            "QFrame#recentsCard { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
            .arg(Theme::hex(Theme::bgCard), Theme::hex(Theme::borderLight)).arg(Theme::radiusMedium));
    }
    if (m_browseBtn) {
        m_browseBtn->setIcon(Theme::icon(QStringLiteral("folder"), 14, Theme::textPrimary));
        m_browseBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 8px; background: transparent; }"
            "QPushButton:hover { border-color: %4; }")
            .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::borderLight))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::borderHover)));
    }
    if (m_dropHintLabel) {
        m_dropHintLabel->setStyleSheet(QStringLiteral("color: %1; margin-top: 4px;").arg(Theme::hex(Theme::textDim)));
    }
    // Recent rows are entirely data-driven (rebuilt from
    // m_recentPaths/m_recentMeta) -- rebuildRecentsList() tears them
    // down and recreates them from the CURRENT Theme:: tokens, which is
    // cheap at <=10 rows.  See this method's declaration doc in the
    // header for why this is a deliberate, documented exception to the
    // general restyleTheme() "creates no widgets" rule.
    rebuildRecentsList();

    // ---- Right column (create) -------------------------------------------
    if (m_createColumnHeader) {
        m_createColumnHeader->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    }
    if (m_providerChip) {
        m_providerChip->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; background-color: rgba(%2, %3, %4, 31); border-radius: 8px; padding: 2px 8px; }")
            .arg(Theme::hex(Theme::accentLight))
            .arg(Theme::accent.red()).arg(Theme::accent.green()).arg(Theme::accent.blue()));
    }
    if (m_configBtn) {
        m_configBtn->setIcon(Theme::icon(QStringLiteral("cog"), 13, Theme::textDim, Theme::textDisabled, Theme::textPrimary));
        // Stylesheet has no Theme:: tokens (border: none / transparent
        // only) -- nothing to re-apply there.
    }
    if (m_promptEdit) {
        m_promptEdit->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { color: %1; background-color: %2; border: 1px solid %3; border-radius: %4px; padding: 6px; }")
            .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::bgWell), Theme::hex(Theme::borderHairline))
            .arg(Theme::radiusMedium));
    }
    if (m_createBtn) {
        m_createBtn->setIcon(Theme::icon(QStringLiteral("sparkles"), 14, Theme::accentLight, Theme::textDisabled));
        m_createBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: 1px solid rgba(%2, %3, %4, 140); border-radius: %5px; padding: 8px; background: transparent; }"
            "QPushButton:disabled { color: %6; border-color: %7; }")
            .arg(Theme::hex(Theme::accentLight))
            .arg(Theme::accent.red()).arg(Theme::accent.green()).arg(Theme::accent.blue())
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    }
    if (m_createFootnote) {
        m_createFootnote->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }
    if (m_connectLabel) {
        m_connectLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    }
    if (m_setupBtn) {
        m_setupBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 6px 14px; background: transparent; }"
            "QPushButton:hover { border-color: %4; }")
            .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::borderLight))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::borderHover)));
    }
}

// ============================================================
// Left column: open an existing scene
// ============================================================

QWidget* StartWidget::buildOpenColumn()
{
    auto* col = new QWidget(this);
    auto* colLayout = new QVBoxLayout(col);
    colLayout->setContentsMargins(0, 0, 0, 0);
    colLayout->setSpacing(10);

    m_openColumnHeader = new QLabel(tr("Recent scenes"), col);
    m_openColumnHeader->setFont(Theme::sans(12, QFont::DemiBold));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    colLayout->addWidget(m_openColumnHeader);

    m_recentsContainer = new QFrame(col);
    m_recentsContainer->setFrameShape(QFrame::NoFrame);
    // objectName so restyleTheme()'s QSS can scope to THIS frame only
    // (#recentsCard) rather than an unscoped "QFrame { ... }" type
    // selector, which would also match every descendant QLabel (QLabel
    // derives from QFrame) -- see restyleTheme()'s comment on this same
    // rule for the P1 fix this avoided (2026-07-23 review).
    m_recentsContainer->setObjectName(QStringLiteral("recentsCard"));
    // Option-B restyle: the list is a RAISED surface (bgCard) with a
    // visible border -- controls sit ON it instead of text floating on the
    // window background (contrast feedback 2026-07-16). Styled in
    // restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_recentsLayout = new QVBoxLayout(m_recentsContainer);
    m_recentsLayout->setContentsMargins(0, 0, 0, 0);
    m_recentsLayout->setSpacing(0);
    colLayout->addWidget(m_recentsContainer);

    m_browseBtn = new QPushButton(tr("Browse files\xE2\x80\xA6"), col);
    m_browseBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_browseBtn->setCursor(Qt::PointingHandCursor);
    m_browseBtn->setFlat(true);
    // Icon-system upgrade: mirrors StartView.swift's "Browse files…"
    // button (Image(systemName: "folder"), size 12, Theme.textPrimary)
    // -- Lucide "folder" tinted textPrimary.
    m_browseBtn->setIconSize(QSize(14, 14));
    // Icon + stylesheet: styled in restyleTheme() -- LIVE THEME-SWITCH
    // CONTRACT (Theme.h).
    connect(m_browseBtn, &QPushButton::clicked, this, [this]() { emit browseRequested(); });
    colLayout->addWidget(m_browseBtn);

    m_dropHintLabel = new QLabel(tr("or drop a .RISEscene here"), col);
    m_dropHintLabel->setFont(Theme::sans(11));
    m_dropHintLabel->setAlignment(Qt::AlignHCenter);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    colLayout->addWidget(m_dropHintLabel);

    colLayout->addStretch(1);
    return col;
}

QWidget* StartWidget::buildRecentRow(const QString& path, qint64 epochSeconds)
{
    // Existence checked per render of the screen (spec §4.1) -- cheap at
    // <= 10 rows, and it means an entry deleted/moved outside RISE shows
    // honestly as "file not found" instead of failing on click.
    const bool exists = QFileInfo::exists(path);
    const QFileInfo fi(path);
    const QString name = fi.completeBaseName();
    const QString folder = fi.absolutePath();

    auto onClick = exists
        ? std::function<void()>([this, path]() { emit openPathRequested(path); })
        : std::function<void()>();
    auto* row = new RecentRow(std::move(onClick), m_recentsContainer);
    row->setAutoFillBackground(false);

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(11, 9, 11, 9);
    rowLayout->setSpacing(10);

    // Option-B restyle (2026-07-16): affordance visible AT REST -- accent
    // scene glyph, medium-weight primary name, ONE textSecondary meta line
    // ("Tests/VCM · 2m ago"; full path in the tooltip), and an always-
    // visible accent chevron.
    // Scene glyph: tinted SVG icon (mirrors StartView.swift's SF `cube` /
    // `cube.transparent` recent-row symbol; "box" is the Lucide analogue).
    auto* iconLabel = new QLabel(row);
    // Theme::bindIconLabel keeps this pixmap correct across a mixed-DPI
    // monitor drag (plain setPixmap(iconPixmap(..., devicePixelRatioF()))
    // captures DPR once at construction and goes stale) AND across a
    // live theme switch -- PROVIDER overload since both colors are
    // Theme:: tokens.  (Belt-and-suspenders: this whole row is also torn
    // down and rebuilt by restyleTheme()'s rebuildRecentsList() call on
    // every theme switch, so this row never actually survives to see a
    // stale repaint -- but binding it correctly costs nothing and keeps
    // the pattern consistent everywhere it's used.)
    Theme::bindIconLabel(iconLabel, QStringLiteral("box"), 12,
        [exists]() { return exists ? Theme::accentLight : Theme::textMuted; });
    iconLabel->setFixedWidth(16);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setStyleSheet(QStringLiteral("background: transparent;"));
    rowLayout->addWidget(iconLabel);

    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(1);

    auto* nameLabel = new QLabel(name, row);
    nameLabel->setFont(Theme::sans(12, QFont::Medium));
    nameLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
        .arg(Theme::hex(exists ? Theme::textPrimary : Theme::textSecondary)));
    textCol->addWidget(nameLabel);

    auto* subLabel = new QLabel(row);
    subLabel->setFont(Theme::sans(10));
    if (exists) {
        // Folder TAIL (last two components) + relative time, one line.
        const QStringList parts = folder.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QString shortFolder = parts.size() >= 2
            ? parts.at(parts.size() - 2) + QLatin1Char('/') + parts.last()
            : (parts.isEmpty() ? folder : parts.last());
        const QString timeStr = epochSeconds > 0 ? relativeTimeString(epochSeconds) : QString();
        subLabel->setText(timeStr.isEmpty()
            ? shortFolder
            : shortFolder + QString::fromUtf8(" \xC2\xB7 ") + timeStr);   // ·
        subLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
            .arg(Theme::hex(Theme::textSecondary)));
        row->setToolTip(tr("Open %1").arg(path));
    } else {
        subLabel->setText(tr("file not found"));
        subLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
            .arg(Theme::hex(Theme::error)));
    }
    textCol->addWidget(subLabel);

    rowLayout->addLayout(textCol, 1);

    if (exists) {
        // Icon-system upgrade: mirrors StartView.swift's recentRow
        // (Image(systemName: "chevron.right"), size 11 semibold,
        // Theme.accentLight) -- Lucide "chevron-right" tinted accentLight.
        auto* chevron = new QLabel(row);
        // Theme::bindIconLabel keeps this pixmap correct across a mixed-DPI
        // monitor drag (plain setPixmap(iconPixmap(..., devicePixelRatioF()))
        // captures DPR once at construction and goes stale) AND across a
        // live theme switch -- PROVIDER overload since Theme::accentLight
        // is a Theme:: token (see the iconLabel comment above for why
        // this row is also rebuilt wholesale on every switch anyway).
        Theme::bindIconLabel(chevron, QStringLiteral("chevron-right"), 13, []() { return Theme::accentLight; });
        chevron->setStyleSheet(QStringLiteral("background: transparent;"));
        rowLayout->addWidget(chevron);
    } else {
        // QToolButton (converted from QPushButton) so autoRaise + hover
        // maps to QIcon::Active: QCommonStyle::CE_ToolButtonLabel promotes
        // an autoRaise button's State_MouseOver to Active, which restores
        // the textPrimary hover tint the old QSS `:hover { color: ... }`
        // rule provided before this button became icon-only (QPushButton
        // hover does not drive QIcon::Active, only State_HasFocus does).
        auto* removeBtn = new QToolButton(row);
        removeBtn->setAutoRaise(true);
        removeBtn->setCursor(Qt::PointingHandCursor);
        removeBtn->setToolTip(tr("Remove from recent scenes"));
        removeBtn->setFixedSize(20, 20);
        // Icon-system upgrade: mirrors StartView.swift's recentRow
        // (Image(systemName: "xmark"), size 10 medium, Theme.textMuted)
        // -- Lucide "x" tinted textMuted, textPrimary on hover.
        removeBtn->setIconSize(QSize(11, 11));
        removeBtn->setIcon(Theme::icon(QStringLiteral("x"), 11, Theme::textMuted, Theme::textDisabled, Theme::textPrimary));
        removeBtn->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; }"));
        connect(removeBtn, &QToolButton::clicked, this, [this, path]() { emit removeRecentRequested(path); });
        rowLayout->addWidget(removeBtn);
    }

    return row;
}

void StartWidget::clearRecentsLayout()
{
    QLayoutItem* item;
    while ((item = m_recentsLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
}

void StartWidget::rebuildRecentsList()
{
    if (!m_recentsLayout) return;
    clearRecentsLayout();

    if (m_recentPaths.isEmpty()) {
        auto* empty = new QLabel(tr("Scenes you open appear here."), m_recentsContainer);
        empty->setFont(Theme::sans(12));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty->setStyleSheet(QStringLiteral("color: %1; padding: 26px 12px; background: transparent;").arg(Theme::hex(Theme::textSecondary)));
        m_recentsLayout->addWidget(empty);
        return;
    }

    bool first = true;
    for (const QString& path : m_recentPaths) {
        if (!first) {
            auto* sep = new QFrame(m_recentsContainer);
            sep->setFrameShape(QFrame::HLine);
            sep->setFixedHeight(1);
            sep->setStyleSheet(QStringLiteral("background-color: %1; border: none;")
                .arg(Theme::hex(Theme::borderHairline)));
            m_recentsLayout->addWidget(sep);
        }
        first = false;
        m_recentsLayout->addWidget(buildRecentRow(path, m_recentMeta.value(path, 0)));
    }
}

QString StartWidget::relativeTimeString(qint64 epochSeconds)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 diff = now - epochSeconds;
    if (diff < 0) diff = 0;
    if (diff < 60)        return tr("just now");
    if (diff < 3600)      return tr("%1m ago").arg(diff / 60);
    if (diff < 86400)     return tr("%1h ago").arg(diff / 3600);
    if (diff < 7 * 86400) return tr("%1d ago").arg(diff / 86400);
    // Older than a week: a short absolute date rather than an ever-
    // growing "N weeks ago" -- close enough to
    // RelativeDateTimeFormatter's .abbreviated tapering on the Mac side,
    // no exact-parity requirement.
    return QDateTime::fromSecsSinceEpoch(epochSeconds).date().toString(Qt::ISODate);
}

// ============================================================
// Right column: create with the agent
// ============================================================

QWidget* StartWidget::buildCreateColumn()
{
    auto* col = new QWidget(this);
    auto* colLayout = new QVBoxLayout(col);
    colLayout->setContentsMargins(0, 0, 0, 0);
    colLayout->setSpacing(10);

    auto* headerRow = new QHBoxLayout();
    m_createColumnHeader = new QLabel(tr("Create with the agent"), col);
    m_createColumnHeader->setFont(Theme::sans(12, QFont::DemiBold));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    headerRow->addWidget(m_createColumnHeader);
    headerRow->addStretch(1);

    m_providerChip = new QLabel(col);
    m_providerChip->setFont(Theme::sans(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_providerChip->hide();
    headerRow->addWidget(m_providerChip);

    // Always-visible settings affordance (user feedback 2026-07-16):
    // switch models / add keys from the start screen even when already
    // configured -- routes to the same place the unconfigured Set up...
    // button does (the Agent tab's inline provider/model/key row).
    // Icon-system upgrade: mirrors StartView.swift's header settings
    // button (Image(systemName: "gearshape"), size 11, Theme.textDim)
    // -- Lucide "cog" (closest visual match to the plain gearshape
    // outline) tinted textDim.
    // QToolButton (converted from QPushButton) so autoRaise + hover maps
    // to QIcon::Active: QCommonStyle::CE_ToolButtonLabel promotes an
    // autoRaise button's State_MouseOver to Active, which restores the
    // textPrimary hover tint the old QSS `:hover { color: ... }` rule
    // provided before this button became icon-only (QPushButton hover
    // does not drive QIcon::Active, only State_HasFocus does).
    m_configBtn = new QToolButton(col);
    m_configBtn->setAutoRaise(true);
    m_configBtn->setCursor(Qt::PointingHandCursor);
    m_configBtn->setToolTip(tr("Provider, model, and API-key settings"));
    m_configBtn->setIconSize(QSize(13, 13));
    // Icon: styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT
    // (Theme.h). The stylesheet has no Theme:: tokens (border:none /
    // transparent only), so it's set once here and never revisited.
    m_configBtn->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; background: transparent; padding: 0 2px; }"));
    connect(m_configBtn, &QToolButton::clicked, this, [this]() { emit setupAgentRequested(); });
    headerRow->addWidget(m_configBtn);
    colLayout->addLayout(headerRow);

    m_promptEdit = new QPlainTextEdit(col);
    m_promptEdit->setPlaceholderText(
        tr("A brushed-gold watch dial on black velvet, dramatic side light, shallow depth of field."));
    m_promptEdit->setFont(Theme::sans(12));
    m_promptEdit->setFixedHeight(96);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    colLayout->addWidget(m_promptEdit);

    m_createBtn = new QPushButton(tr("Create scene"), col);
    m_createBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_createBtn->setCursor(Qt::PointingHandCursor);
    m_createBtn->setFlat(true);
    // Icon-system upgrade: mirrors StartView.swift's "Create scene"
    // button (Image(systemName: "sparkles"), size 12, Theme.accentLight,
    // dimmed to textDisabled while disabled) -- Lucide "sparkles".
    m_createBtn->setIconSize(QSize(14, 14));
    // Icon + stylesheet: styled in restyleTheme() -- LIVE THEME-SWITCH
    // CONTRACT (Theme.h).
    connect(m_createBtn, &QPushButton::clicked, this, &StartWidget::onCreateClicked);
    colLayout->addWidget(m_createBtn);

    m_createFootnote = new QLabel(
        tr("Loads a blank stage, then the agent builds your scene from the description."), col);
    m_createFootnote->setWordWrap(true);
    m_createFootnote->setFont(Theme::sans(11));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    colLayout->addWidget(m_createFootnote);

    // Unconfigured state (spec §6): path 3 must never dead-end.
    m_unconfiguredContainer = new QWidget(col);
    auto* uLayout = new QVBoxLayout(m_unconfiguredContainer);
    uLayout->setContentsMargins(0, 2, 0, 0);
    uLayout->setSpacing(8);

    m_connectLabel = new QLabel(
        tr("Connect an agent to create scenes from a prompt."), m_unconfiguredContainer);
    m_connectLabel->setWordWrap(true);
    m_connectLabel->setFont(Theme::sans(11));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    uLayout->addWidget(m_connectLabel);

    m_setupBtn = new QPushButton(tr("Set up\xE2\x80\xA6"), m_unconfiguredContainer);
    m_setupBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_setupBtn->setCursor(Qt::PointingHandCursor);
    m_setupBtn->setFlat(true);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    connect(m_setupBtn, &QPushButton::clicked, this, [this]() { emit setupAgentRequested(); });
    uLayout->addWidget(m_setupBtn, 0, Qt::AlignLeft);

    colLayout->addWidget(m_unconfiguredContainer);
    m_unconfiguredContainer->hide();

    colLayout->addStretch(1);
    return col;
}

void StartWidget::onCreateClicked()
{
    const QString text = m_promptEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        // Enabled-with-feedback (spec §4.3): an empty prompt focuses the
        // box rather than silently ignoring the click.
        m_promptEdit->setFocus();
        return;
    }
    if (!m_agentConfigured || m_createInFlight) return;   // defensive; button is hidden/disabled then
    setCreateInFlight(true);
    emit createRequested(text);
}

// ============================================================
// Public slots
// ============================================================

void StartWidget::setRecents(const QStringList& paths, const QMap<QString, qint64>& meta)
{
    m_recentPaths = paths;
    m_recentMeta = meta;
    rebuildRecentsList();
}

void StartWidget::setAgentReadiness(bool configured, const QString& providerDisplayName)
{
    m_agentConfigured = configured;
    m_providerName = providerDisplayName;
    updateReadinessUI();
}

void StartWidget::setErrorMessage(const QString& message)
{
    if (!m_errorBanner || !m_errorLabel) return;
    m_errorLabel->setText(message);
    m_errorBanner->setVisible(!message.isEmpty());
}

void StartWidget::setCreateInFlight(bool inFlight)
{
    m_createInFlight = inFlight;
    updateReadinessUI();
}

void StartWidget::updateReadinessUI()
{
    if (!m_promptEdit || !m_createBtn || !m_unconfiguredContainer) return;

    m_promptEdit->setEnabled(m_agentConfigured);
    m_createBtn->setVisible(m_agentConfigured);
    m_createFootnote->setVisible(m_agentConfigured);
    m_unconfiguredContainer->setVisible(!m_agentConfigured);

    m_createBtn->setText(m_createInFlight
        ? tr("Creating\xE2\x80\xA6")
        : tr("Create scene"));
    m_createBtn->setEnabled(!m_createInFlight);

    const bool showChip = m_agentConfigured && !m_providerName.isEmpty();
    m_providerChip->setVisible(showChip);
    if (showChip) m_providerChip->setText(m_providerName);
}

// ============================================================
// Drag-and-drop (spec §4.1: drop a .RISEscene anywhere on this widget)
// ============================================================

void StartWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(
                QStringLiteral(".RISEscene"), Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void StartWidget::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (path.endsWith(QStringLiteral(".RISEscene"), Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            emit openPathRequested(path);
            return;
        }
    }
}
