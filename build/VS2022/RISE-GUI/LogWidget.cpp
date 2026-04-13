//////////////////////////////////////////////////////////////////////
//
//  LogWidget.cpp - Color-coded log output implementation.
//
//  Ported from the Mac app's LogOutputView.swift.
//  Color scheme: Event=default, Info=gray, Warning=orange,
//  Error/Fatal=red.
//
//////////////////////////////////////////////////////////////////////

#include "LogWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QFont>
#include <QFontDatabase>

// Log level values from ILog.h
static constexpr int eLog_Event   = 1;
static constexpr int eLog_Info    = 2;
static constexpr int eLog_Warning = 4;
static constexpr int eLog_Error   = 8;
static constexpr int eLog_Fatal   = 16;

LogWidget::LogWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    // Header
    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel("Log Output");
    titleLabel->setStyleSheet("font-weight: bold; color: gray;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    m_clearBtn = new QPushButton("Clear");
    m_clearBtn->setFlat(true);
    m_clearBtn->setEnabled(false);
    headerLayout->addWidget(m_clearBtn);
    layout->addLayout(headerLayout);

    // Log text area
    m_textEdit = new QPlainTextEdit();
    m_textEdit->setReadOnly(true);
    m_textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);

    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(11);
    m_textEdit->setFont(monoFont);

    // Make text selectable
    m_textEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);

    layout->addWidget(m_textEdit);

    connect(m_clearBtn, &QPushButton::clicked, this, &LogWidget::clearLog);
}

void LogWidget::appendLog(int level, const QString& message)
{
    // Cap at MAX_LOG_MESSAGES
    if (m_messageCount >= MAX_LOG_MESSAGES) {
        // Remove the first line
        QTextCursor cursor = m_textEdit->textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 1);
        cursor.removeSelectedText();
        cursor.deleteChar(); // Remove the newline
        m_messageCount--;
    }

    // Determine color
    QColor color;
    if (level & eLog_Fatal) {
        color = QColor(220, 50, 50);   // Red
    } else if (level & eLog_Error) {
        color = QColor(220, 50, 50);   // Red
    } else if (level & eLog_Warning) {
        color = QColor(230, 160, 0);   // Orange
    } else if (level & eLog_Info) {
        color = QColor(128, 128, 128); // Gray
    } else {
        color = palette().color(QPalette::Text); // Default (Event)
    }

    // Append with color
    QTextCursor cursor(m_textEdit->document());
    cursor.movePosition(QTextCursor::End);

    QTextCharFormat fmt;
    fmt.setForeground(color);
    cursor.setCharFormat(fmt);

    if (m_messageCount > 0) {
        cursor.insertText("\n");
    }
    cursor.insertText(message);

    m_messageCount++;
    m_clearBtn->setEnabled(true);

    // Auto-scroll to bottom
    m_textEdit->verticalScrollBar()->setValue(m_textEdit->verticalScrollBar()->maximum());
}

void LogWidget::clearLog()
{
    m_textEdit->clear();
    m_messageCount = 0;
    m_clearBtn->setEnabled(false);
}
