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
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
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

private:
    std::function<void()> m_onClick;
};

} // namespace

StartWidget::StartWidget(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);

    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgCenter);
        setPalette(pal);
    }

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
    auto* bannerIcon = new QLabel(QString::fromUtf8("\xE2\x9A\xA0"), m_errorBanner);   // ⚠
    bannerIcon->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::error)));
    bannerLayout->addWidget(bannerIcon);
    m_errorLabel = new QLabel(m_errorBanner);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setFont(Theme::sans(12));
    m_errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    bannerLayout->addWidget(m_errorLabel, 1);
    m_errorBanner->setStyleSheet(QStringLiteral(
        "background-color: rgba(%1, %2, %3, 20); border: 1px solid rgba(%1, %2, %3, 90); border-radius: %4px;")
        .arg(Theme::error.red()).arg(Theme::error.green()).arg(Theme::error.blue())
        .arg(Theme::radiusMedium));
    m_errorBanner->hide();
    containerLayout->addWidget(m_errorBanner);
    containerLayout->addSpacing(18);

    // ---- Title + subtitle ----------------------------------------------
    auto* title = new QLabel(tr("Start a session"), container);
    title->setFont(Theme::sans(20, QFont::Medium));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    containerLayout->addWidget(title);

    auto* subtitle = new QLabel(
        tr("Open a scene you've worked on, browse for a file, or describe one for the agent to build."),
        container);
    subtitle->setWordWrap(true);
    subtitle->setFont(Theme::sans(12));
    subtitle->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    containerLayout->addWidget(subtitle);
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

    auto* header = new QLabel(tr("Recent scenes"), col);
    header->setFont(Theme::sans(11, QFont::DemiBold));
    header->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    colLayout->addWidget(header);

    m_recentsContainer = new QFrame(col);
    m_recentsContainer->setFrameShape(QFrame::NoFrame);
    m_recentsContainer->setStyleSheet(QStringLiteral(
        "QFrame { border: 1px solid %1; border-radius: %2px; }")
        .arg(Theme::hex(Theme::borderHairline)).arg(Theme::radiusMedium));
    m_recentsLayout = new QVBoxLayout(m_recentsContainer);
    m_recentsLayout->setContentsMargins(0, 0, 0, 0);
    m_recentsLayout->setSpacing(0);
    colLayout->addWidget(m_recentsContainer);

    auto* browseBtn = new QPushButton(tr("Browse files\xE2\x80\xA6"), col);
    browseBtn->setFont(Theme::sans(12, QFont::DemiBold));
    browseBtn->setCursor(Qt::PointingHandCursor);
    browseBtn->setFlat(true);
    browseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 8px; background: transparent; }"
        "QPushButton:hover { border-color: %4; }")
        .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::borderLight))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::borderHover)));
    connect(browseBtn, &QPushButton::clicked, this, [this]() { emit browseRequested(); });
    colLayout->addWidget(browseBtn);

    auto* dropHint = new QLabel(tr("or drop a .RISEscene here"), col);
    dropHint->setFont(Theme::sans(10));
    dropHint->setAlignment(Qt::AlignHCenter);
    dropHint->setStyleSheet(QStringLiteral("color: %1; margin-top: 4px;").arg(Theme::hex(Theme::textMuted)));
    colLayout->addWidget(dropHint);

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
    rowLayout->setContentsMargins(11, 8, 11, 8);
    rowLayout->setSpacing(10);

    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(1);

    auto* nameLabel = new QLabel(name, row);
    nameLabel->setFont(Theme::sans(12));
    nameLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(exists ? Theme::textPrimary : Theme::textSecondary)));
    textCol->addWidget(nameLabel);

    auto* subLabel = new QLabel(row);
    subLabel->setFont(Theme::sans(10));
    if (exists) {
        subLabel->setText(folder);
        subLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textMuted)));
    } else {
        subLabel->setText(tr("file not found"));
        subLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::error)));
    }
    textCol->addWidget(subLabel);

    rowLayout->addLayout(textCol, 1);

    if (exists) {
        auto* timeLabel = new QLabel(row);
        timeLabel->setFont(Theme::sans(10));
        timeLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textMuted)));
        // Absence of meta (old installs, or a path added before this
        // feature) renders the row without a time rather than migrating
        // -- spec §5.4.
        timeLabel->setText(epochSeconds > 0 ? relativeTimeString(epochSeconds) : QString());
        rowLayout->addWidget(timeLabel);
    } else {
        auto* removeBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"), row);   // ✕
        removeBtn->setFlat(true);
        removeBtn->setCursor(Qt::PointingHandCursor);
        removeBtn->setToolTip(tr("Remove from recent scenes"));
        removeBtn->setFixedSize(20, 20);
        removeBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: none; background: transparent; }"
            "QPushButton:hover { color: %2; }")
            .arg(Theme::hex(Theme::textMuted), Theme::hex(Theme::textPrimary)));
        connect(removeBtn, &QPushButton::clicked, this, [this, path]() { emit removeRecentRequested(path); });
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
        empty->setStyleSheet(QStringLiteral("color: %1; padding: 26px 12px;").arg(Theme::hex(Theme::textMuted)));
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
    auto* header = new QLabel(tr("Create with the agent"), col);
    header->setFont(Theme::sans(11, QFont::DemiBold));
    header->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    headerRow->addWidget(header);
    headerRow->addStretch(1);

    m_providerChip = new QLabel(col);
    m_providerChip->setFont(Theme::sans(10));
    m_providerChip->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background-color: rgba(%2, %3, %4, 31); border-radius: 8px; padding: 2px 8px; }")
        .arg(Theme::hex(Theme::accentLight))
        .arg(Theme::accent.red()).arg(Theme::accent.green()).arg(Theme::accent.blue()));
    m_providerChip->hide();
    headerRow->addWidget(m_providerChip);

    // Always-visible settings affordance (user feedback 2026-07-16):
    // switch models / add keys from the start screen even when already
    // configured -- routes to the same place the unconfigured Set up...
    // button does (the Agent tab's inline provider/model/key row).
    auto* configBtn = new QPushButton(QString::fromUtf8("\xE2\x9A\x99"), col);   // gear (house idiom, see ViewportProperties)
    configBtn->setFont(Theme::sans(11));
    configBtn->setCursor(Qt::PointingHandCursor);
    configBtn->setFlat(true);
    configBtn->setToolTip(tr("Provider, model, and API-key settings"));
    configBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: none; background: transparent; padding: 0 2px; }"
        "QPushButton:hover { color: %2; }")
        .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::textPrimary)));
    connect(configBtn, &QPushButton::clicked, this, [this]() { emit setupAgentRequested(); });
    headerRow->addWidget(configBtn);
    colLayout->addLayout(headerRow);

    m_promptEdit = new QPlainTextEdit(col);
    m_promptEdit->setPlaceholderText(
        tr("A brushed-gold watch dial on black velvet, dramatic side light, shallow depth of field."));
    m_promptEdit->setFont(Theme::sans(12));
    m_promptEdit->setFixedHeight(96);
    m_promptEdit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { color: %1; background-color: %2; border: 1px solid %3; border-radius: %4px; padding: 6px; }")
        .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::bgWell), Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusMedium));
    colLayout->addWidget(m_promptEdit);

    m_createBtn = new QPushButton(tr("\xE2\x9C\xA6 Create scene"), col);   // ✦ Create scene
    m_createBtn->setFont(Theme::sans(12, QFont::DemiBold));
    m_createBtn->setCursor(Qt::PointingHandCursor);
    m_createBtn->setFlat(true);
    m_createBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid rgba(%2, %3, %4, 140); border-radius: %5px; padding: 8px; background: transparent; }"
        "QPushButton:disabled { color: %6; border-color: %7; }")
        .arg(Theme::hex(Theme::accentLight))
        .arg(Theme::accent.red()).arg(Theme::accent.green()).arg(Theme::accent.blue())
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    connect(m_createBtn, &QPushButton::clicked, this, &StartWidget::onCreateClicked);
    colLayout->addWidget(m_createBtn);

    m_createFootnote = new QLabel(
        tr("Loads a blank stage, then the agent builds your scene from the description."), col);
    m_createFootnote->setWordWrap(true);
    m_createFootnote->setFont(Theme::sans(10));
    m_createFootnote->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textMuted)));
    colLayout->addWidget(m_createFootnote);

    // Unconfigured state (spec §6): path 3 must never dead-end.
    m_unconfiguredContainer = new QWidget(col);
    auto* uLayout = new QVBoxLayout(m_unconfiguredContainer);
    uLayout->setContentsMargins(0, 2, 0, 0);
    uLayout->setSpacing(8);

    auto* connectLabel = new QLabel(
        tr("Connect an agent to create scenes from a prompt."), m_unconfiguredContainer);
    connectLabel->setWordWrap(true);
    connectLabel->setFont(Theme::sans(11));
    connectLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
    uLayout->addWidget(connectLabel);

    auto* setupBtn = new QPushButton(tr("Set up\xE2\x80\xA6"), m_unconfiguredContainer);
    setupBtn->setFont(Theme::sans(12, QFont::DemiBold));
    setupBtn->setCursor(Qt::PointingHandCursor);
    setupBtn->setFlat(true);
    setupBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 6px 14px; background: transparent; }"
        "QPushButton:hover { border-color: %4; }")
        .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::borderLight))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::borderHover)));
    connect(setupBtn, &QPushButton::clicked, this, [this]() { emit setupAgentRequested(); });
    uLayout->addWidget(setupBtn, 0, Qt::AlignLeft);

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
        : QString::fromUtf8("\xE2\x9C\xA6 Create scene"));
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
