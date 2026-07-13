//////////////////////////////////////////////////////////////////////
//
//  LogWidget.cpp - Restyled log output implementation.  See header
//    for the design-comp cross reference.
//
//  Color scheme: Event=default, Info=muted, Warning=amber,
//  Error/Fatal=red -- warn/error rows additionally get a subtle
//  tinted row background.
//
//////////////////////////////////////////////////////////////////////

#include "LogWidget.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QScrollBar>
#include <QFrame>
#include <QPalette>
#include <QTextCharFormat>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QPlainTextEdit>
#include <QFont>
#include <QTime>
#include <QRegularExpression>
#include <QMouseEvent>

#include <functional>
#include <utility>

namespace {

// A row that reports a plain left-click via a std::function callback.
// No Q_OBJECT / signals -- same pattern used in OutlinerWidget.cpp /
// ViewportProperties.cpp.  Used here for the collapsed status strip.
class ClickableStrip : public QWidget
{
public:
    using ClickFn = std::function<void()>;

    explicit ClickableStrip(ClickFn onClick, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_onClick(std::move(onClick))
    {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && m_onClick) {
            m_onClick();
        }
        QWidget::mouseReleaseEvent(e);
    }

private:
    ClickFn m_onClick;
};

struct SeverityDef {
    const char* label;
    int         mask;   // 0 = All
};
// Mask values mirror ILog.h's bitmask exactly (Event=1, Info=2,
// Warning=4, Error=8, Fatal=16 -- see LogWidget::kLevel* in the header,
// duplicated here as literals since this table lives at namespace
// scope, outside the class).
const SeverityDef kSeverities[] = {
    { "All",   0 },
    { "Info",  2      },
    { "Warn",  4      },
    { "Error", 8 | 16 },
};

} // namespace

LogWidget::LogWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_stack = new QStackedWidget(this);

    // ================================================================
    // Full view: header (32px) + body
    // ================================================================
    auto* fullView = new QWidget(m_stack);
    auto* fullLayout = new QVBoxLayout(fullView);
    fullLayout->setContentsMargins(0, 0, 0, 0);
    fullLayout->setSpacing(0);

    auto* header = new QWidget(fullView);
    header->setFixedHeight(32);
    header->setAutoFillBackground(true);
    {
        QPalette pal = header->palette();
        pal.setColor(QPalette::Window, Theme::bgHeader);
        header->setPalette(pal);
    }
    header->setStyleSheet(QStringLiteral(
        "QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 3px 9px; }"
        "QToolButton:checked { background-color: %1; color: %2; }")
        .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::textPrimary)));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 0, 10, 0);
    headerLayout->setSpacing(10);

    auto* title = new QLabel(tr("Log"), header);
    title->setFont(Theme::sans(11, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    headerLayout->addWidget(title);

    // Severity filter pills.
    auto* severityGroup = new QButtonGroup(this);
    severityGroup->setExclusive(true);
    for (const SeverityDef& def : kSeverities) {
        auto* btn = new QToolButton(header);
        btn->setText(QString::fromUtf8(def.label));
        btn->setFont(Theme::mono(9));
        btn->setCheckable(true);
        btn->setChecked(def.mask == 0);
        btn->setProperty("severityMask", def.mask);
        btn->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
        severityGroup->addButton(btn);
        connect(btn, &QToolButton::clicked, this, &LogWidget::onSeverityFilterClicked);
        headerLayout->addWidget(btn);
        m_severityButtons.append(btn);
    }

    m_warnCountChip = new QLabel(header);
    m_warnCountChip->setFont(Theme::mono(9));
    headerLayout->addWidget(m_warnCountChip);

    m_errCountChip = new QLabel(header);
    m_errCountChip->setFont(Theme::mono(9));
    m_errCountChip->setStyleSheet(QStringLiteral(
        "color: %1; border: 1px solid %2; border-radius: 4px; padding: 2px 6px;")
        .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::borderLight)));
    headerLayout->addWidget(m_errCountChip);

    m_filterEdit = new QLineEdit(header);
    m_filterEdit->setPlaceholderText(QString::fromUtf8("\xE2\x8C\x95 filter\xE2\x80\xA6"));
    m_filterEdit->setFont(Theme::mono(10));
    m_filterEdit->setMaximumWidth(200);
    m_filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background-color: %1; border: 1px solid %2; border-radius: 6px; padding: 4px 9px; color: %3; }")
        .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight), Theme::hex(Theme::textSecondary)));
    connect(m_filterEdit, &QLineEdit::textChanged, this, &LogWidget::onTextFilterChanged);
    headerLayout->addWidget(m_filterEdit, 1);

    m_followButton = new QToolButton(header);
    m_followButton->setCheckable(true);
    m_followButton->setChecked(true);
    m_followButton->setFont(Theme::mono(10));
    m_followButton->setText(tr("follow \xE2\x9C\x93"));
    m_followButton->setToolTip(tr("Auto-scroll to newest"));
    connect(m_followButton, &QToolButton::clicked, this, &LogWidget::onFollowToggled);
    headerLayout->addWidget(m_followButton);

    m_clearBtn = new QPushButton(tr("clear"), header);
    m_clearBtn->setFlat(true);
    m_clearBtn->setFont(Theme::sans(10));
    m_clearBtn->setEnabled(false);
    m_clearBtn->setStyleSheet(QStringLiteral("QPushButton { color: %1; border: none; }"
        "QPushButton:disabled { color: %2; }")
        .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::textDisabled)));
    headerLayout->addWidget(m_clearBtn);

    m_collapseBtn = new QToolButton(header);
    m_collapseBtn->setText(QString::fromUtf8("\xE2\x8C\x84"));   // ⌄
    m_collapseBtn->setToolTip(tr("Collapse to status strip (Alt+L)"));
    m_collapseBtn->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    connect(m_collapseBtn, &QToolButton::clicked, this, &LogWidget::onCollapseToggled);
    headerLayout->addWidget(m_collapseBtn);

    fullLayout->addWidget(header);

    // Body.
    m_textEdit = new QPlainTextEdit(fullView);
    m_textEdit->setReadOnly(true);
    m_textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_textEdit->setFont(Theme::mono(10));
    m_textEdit->setFrameShape(QFrame::NoFrame);
    m_textEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    {
        QPalette pal = m_textEdit->palette();
        pal.setColor(QPalette::Base, Theme::bgWell);
        pal.setColor(QPalette::Text, Theme::textMuted);
        m_textEdit->setPalette(pal);
    }
    fullLayout->addWidget(m_textEdit, 1);

    m_stack->addWidget(fullView);   // index 0

    // ================================================================
    // Collapsed strip (28px)
    // ================================================================
    auto* strip = new ClickableStrip([this]() { setCollapsed(false); }, m_stack);
    strip->setFixedHeight(28);
    strip->setAutoFillBackground(true);
    {
        QPalette pal = strip->palette();
        pal.setColor(QPalette::Window, Theme::bgWell);
        strip->setPalette(pal);
    }
    auto* stripLayout = new QHBoxLayout(strip);
    stripLayout->setContentsMargins(10, 0, 10, 0);
    stripLayout->setSpacing(10);

    auto* stripTitle = new QLabel(tr("Log"), strip);
    stripTitle->setFont(Theme::sans(11, QFont::DemiBold));
    stripTitle->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    stripLayout->addWidget(stripTitle);

    m_collapsedText = new QLabel(strip);
    m_collapsedText->setFont(Theme::mono(10));
    m_collapsedText->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textMuted)));
    m_collapsedText->setMaximumWidth(640);
    stripLayout->addWidget(m_collapsedText, 1);

    m_collapsedWarnChip = new QLabel(strip);
    m_collapsedWarnChip->setFont(Theme::mono(9));
    m_collapsedWarnChip->setStyleSheet(QStringLiteral(
        "color: %1; border: 1px solid %2; border-radius: 4px; padding: 1px 6px;")
        .arg(Theme::hex(Theme::warn), Theme::rgba(QColor(Theme::warn.red(), Theme::warn.green(), Theme::warn.blue(), static_cast<int>(0.35 * 255)))));
    stripLayout->addWidget(m_collapsedWarnChip);

    auto* stripChevron = new QLabel(QString::fromUtf8("\xE2\x8C\x83"), strip);   // ⌃
    stripChevron->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    stripLayout->addWidget(stripChevron);

    m_stack->addWidget(strip);   // index 1

    root->addWidget(m_stack);

    updateCounters();
    updateCollapsedStrip();
}

QColor LogWidget::colorForLevel(int level) const
{
    if (level & (kLevelFatal | kLevelError)) return Theme::error;
    if (level & kLevelWarning) return Theme::warn;
    if (level & kLevelInfo) return Theme::textMuted;
    return Theme::textSecondary;   // Event / default
}

bool LogWidget::passesFilter(const LogEntry& e) const
{
    if (m_severityFilter != 0 && !(e.level & m_severityFilter)) return false;
    if (!m_textFilter.isEmpty() && !e.message.contains(m_textFilter, Qt::CaseInsensitive)) return false;
    return true;
}

void LogWidget::appendEntryIfVisible(const LogEntry& e)
{
    if (!passesFilter(e)) return;

    QTextCursor cursor(m_textEdit->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat charFmt;
    charFmt.setForeground(colorForLevel(e.level));

    QTextBlockFormat blockFmt;
    if (e.level & (kLevelWarning | kLevelError | kLevelFatal)) {
        QColor bg = (e.level & kLevelWarning) ? Theme::warn : Theme::error;
        bg.setAlpha(16);
        blockFmt.setBackground(bg);
    }

    if (!m_textEdit->document()->isEmpty()) {
        cursor.insertBlock(blockFmt, charFmt);
    } else {
        cursor.setBlockFormat(blockFmt);
        cursor.setCharFormat(charFmt);
    }
    cursor.insertText(e.message, charFmt);

    if (m_follow) {
        if (QScrollBar* bar = m_textEdit->verticalScrollBar()) {
            bar->setValue(bar->maximum());
        }
    }
}

void LogWidget::rebuildVisibleText()
{
    m_textEdit->clear();
    for (const LogEntry& e : m_entries) {
        appendEntryIfVisible(e);
    }
}

void LogWidget::updateCounters()
{
    m_warnCountChip->setText(tr("WARN %1").arg(m_warnCount));
    m_warnCountChip->setStyleSheet(QStringLiteral(
        "color: %1; background-color: %2; border: 1px solid %3; border-radius: 4px; padding: 2px 6px;")
        .arg(Theme::hex(m_warnCount > 0 ? Theme::warn : Theme::textDim),
             m_warnCount > 0
                 ? Theme::rgba(QColor(Theme::warn.red(), Theme::warn.green(), Theme::warn.blue(), static_cast<int>(0.1 * 255)))
                 : QStringLiteral("transparent"),
             Theme::hex(m_warnCount > 0 ? Theme::warn : Theme::borderLight)));
    m_errCountChip->setText(tr("ERR %1").arg(m_errCount));
}

void LogWidget::updateCollapsedStrip()
{
    m_collapsedText->setText(m_entries.isEmpty() ? tr("No messages yet") : m_entries.last().message);
    m_collapsedWarnChip->setText(tr("WARN %1").arg(m_warnCount));
    m_collapsedWarnChip->setVisible(m_warnCount > 0);
}

void LogWidget::appendLog(int level, const QString& message)
{
    static const QRegularExpression tsRegex(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}"));
    QString text = message;
    if (!tsRegex.match(text).hasMatch()) {
        text = QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + text;
    }

    LogEntry e;
    e.level = level;
    e.message = text;

    bool droppedOldest = false;
    if (m_entries.size() >= MAX_LOG_MESSAGES) {
        m_entries.removeFirst();
        droppedOldest = true;
    }
    m_entries.append(e);

    if (level & kLevelWarning) ++m_warnCount;
    if (level & (kLevelError | kLevelFatal)) ++m_errCount;
    updateCounters();

    if (droppedOldest) {
        rebuildVisibleText();
    } else {
        appendEntryIfVisible(e);
    }
    updateCollapsedStrip();
    m_clearBtn->setEnabled(true);
}

void LogWidget::clearLog()
{
    m_entries.clear();
    m_warnCount = 0;
    m_errCount = 0;
    m_textEdit->clear();
    m_clearBtn->setEnabled(false);
    updateCounters();
    updateCollapsedStrip();
}

void LogWidget::onSeverityFilterClicked()
{
    auto* btn = qobject_cast<QToolButton*>(sender());
    if (!btn) return;
    m_severityFilter = btn->property("severityMask").toInt();
    rebuildVisibleText();
}

void LogWidget::onTextFilterChanged(const QString& text)
{
    m_textFilter = text;
    rebuildVisibleText();
}

void LogWidget::onFollowToggled()
{
    m_follow = m_followButton->isChecked();
    m_followButton->setText(m_follow ? tr("follow \xE2\x9C\x93") : tr("follow"));
    if (m_follow) {
        if (QScrollBar* bar = m_textEdit->verticalScrollBar()) {
            bar->setValue(bar->maximum());
        }
    }
}

void LogWidget::onCollapseToggled()
{
    setCollapsed(!m_collapsed);
}

void LogWidget::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    if (m_stack) m_stack->setCurrentIndex(collapsed ? 1 : 0);
}
