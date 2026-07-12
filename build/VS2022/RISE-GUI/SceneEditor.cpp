//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.cpp - Scene editor panel implementation.
//
//  Ported from the Mac app's SceneEditorWindow.swift.
//
//////////////////////////////////////////////////////////////////////

#include "SceneEditor.h"
#include "RISESyntaxHighlighter.h"
#include "SceneTextEdit.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QFontDatabase>
#include <QFile>
#include <QFrame>
#include <QPalette>
#include <QTextDocument>
#include <QTextStream>
#include <QMessageBox>

namespace {

// Compact bordered pill button shared by Revert/Save/Save&Reload/close —
// mirrors the design comp's header affordances.
QPushButton* makeHeaderPill(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setFont(Theme::sans(11));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: 1px solid %2; border-radius: 6px; padding: 3px 10px; background: transparent; }"
        "QPushButton:disabled { color: %3; border-color: %4; }"
        "QPushButton:hover:!disabled { border-color: %5; }")
        .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderStrong),
             Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight),
             Theme::hex(Theme::borderHover)));
    return btn;
}

} // namespace

SceneEditor::SceneEditor(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Header strip: "Scene file" + dirty dot, Revert/Save/Save&Reload
    // pills, close.  Theme::bgHeader background, matching the comp.
    auto* header = new QWidget(this);
    header->setFixedHeight(38);
    header->setAutoFillBackground(true);
    {
        QPalette pal = header->palette();
        pal.setColor(QPalette::Window, Theme::bgHeader);
        header->setPalette(pal);
    }
    header->setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 10, 0);
    headerLayout->setSpacing(8);

    auto* titleLabel = new QLabel(QStringLiteral("Scene file"), header);
    titleLabel->setFont(Theme::sans(11, QFont::DemiBold));
    titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    headerLayout->addWidget(titleLabel);

    m_dirtyDot = new QLabel(QString::fromUtf8("\xE2\x97\x8F"), header);   // U+25CF BLACK CIRCLE
    m_dirtyDot->setStyleSheet(QStringLiteral("color: %1; font-size: 7px;").arg(Theme::hex(Theme::dirty)));
    m_dirtyDot->hide();
    headerLayout->addWidget(m_dirtyDot);

    headerLayout->addStretch();

    m_revertBtn = makeHeaderPill(tr("Revert"), header);
    m_saveBtn = makeHeaderPill(tr("Save"), header);
    m_saveReloadBtn = makeHeaderPill(tr("Save && Reload"), header);
    m_revertBtn->setEnabled(false);
    m_saveBtn->setEnabled(false);
    m_saveReloadBtn->setEnabled(false);
    headerLayout->addWidget(m_revertBtn);
    headerLayout->addWidget(m_saveBtn);
    headerLayout->addWidget(m_saveReloadBtn);

    m_closeBtn = makeHeaderPill(QString::fromUtf8("\xE2\x9C\x95"), header);   // ✕
    m_closeBtn->setToolTip(tr("Back to the Agent tab"));
    headerLayout->addWidget(m_closeBtn);

    layout->addWidget(header);

    // Editor — SceneTextEdit adds right-click suggestions pulled from the
    // library's SceneEditorSuggestions::SuggestionEngine.  Theme::mono(12)
    // with a dark bg/selection palette so the editor commits to the same
    // fixed dark surface as the rest of the redesigned chrome.
    m_editor = new SceneTextEdit();
    m_editor->setFont(Theme::mono(12));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4 * m_editor->fontMetrics().horizontalAdvance(' '));
    m_editor->setFrameShape(QFrame::NoFrame);
    {
        QPalette pal = m_editor->palette();
        pal.setColor(QPalette::Base, Theme::bgPanel);
        pal.setColor(QPalette::Text, Theme::textPrimary);
        pal.setColor(QPalette::Highlight, Theme::accent);
        pal.setColor(QPalette::HighlightedText, QColor(0x0d, 0x11, 0x16));
        m_editor->setPalette(pal);
    }
    layout->addWidget(m_editor, 1);

    // Syntax highlighter
    m_highlighter = new RISESyntaxHighlighter(m_editor->document());

    // ---- Bottom status bar: save state + honest client-side line/char
    // counts.  NO Ctrl+S shortcut on the Save button here — the Mac
    // client removed the ⌘S-equivalent for a collision with the
    // production-render shortcut; mirrored here by simply never binding
    // one (m_saveBtn is a plain click target only).
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(28);
    statusBar->setAutoFillBackground(true);
    {
        QPalette pal = statusBar->palette();
        pal.setColor(QPalette::Window, Theme::bgHeader);
        statusBar->setPalette(pal);
    }
    statusBar->setStyleSheet(QStringLiteral("border-top: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 0, 12, 0);
    statusLayout->setSpacing(10);

    m_saveStateLabel = new QLabel(statusBar);
    m_saveStateLabel->setFont(Theme::mono(10));
    statusLayout->addWidget(m_saveStateLabel);
    statusLayout->addStretch();

    m_countsLabel = new QLabel(statusBar);
    m_countsLabel->setFont(Theme::mono(10));
    m_countsLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    statusLayout->addWidget(m_countsLabel);

    layout->addWidget(statusBar);

    // Connections
    connect(m_closeBtn, &QPushButton::clicked, this, &SceneEditor::closeRequested);
    connect(m_revertBtn, &QPushButton::clicked, this, &SceneEditor::revert);
    connect(m_saveBtn, &QPushButton::clicked, this, &SceneEditor::save);
    connect(m_saveReloadBtn, &QPushButton::clicked, this, &SceneEditor::saveAndReload);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SceneEditor::onTextChanged);

    updateDirtyState();
}

void SceneEditor::loadFile(const QString& filePath)
{
    m_filePath = filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Could not open file: " + filePath);
        return;
    }

    QTextStream in(&file);
    QString content = in.readAll();
    file.close();

    m_originalText = content;
    m_editor->setPlainText(content);
    updateDirtyState();
}

void SceneEditor::refreshFromDisk()
{
    if (!m_filePath.isEmpty()) {
        loadFile(m_filePath);
    }
}

bool SceneEditor::isDirty() const
{
    return m_editor->toPlainText() != m_originalText;
}

void SceneEditor::save()
{
    if (m_filePath.isEmpty()) return;

    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Could not save file: " + m_filePath);
        return;
    }

    QTextStream out(&file);
    QString text = m_editor->toPlainText();
    out << text;
    file.close();

    m_originalText = text;
    updateDirtyState();
}

void SceneEditor::revert()
{
    if (isDirty()) {
        auto result = QMessageBox::question(this, "Revert",
            "Discard changes and revert to saved version?",
            QMessageBox::Yes | QMessageBox::No);
        if (result != QMessageBox::Yes) return;
    }

    m_editor->setPlainText(m_originalText);
    updateDirtyState();
}

void SceneEditor::saveAndReload()
{
    save();
    emit saveAndReloadRequested(m_filePath);
}

void SceneEditor::onTextChanged()
{
    updateDirtyState();
}

void SceneEditor::updateDirtyState()
{
    const bool dirty = isDirty();
    m_dirtyDot->setVisible(dirty);
    m_revertBtn->setEnabled(dirty);
    m_saveBtn->setEnabled(dirty);
    m_saveReloadBtn->setEnabled(dirty);

    if (dirty) {
        m_saveStateLabel->setText(QString::fromUtf8("\xE2\x97\x8F unsaved edits"));   // ● unsaved edits
        m_saveStateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::dirty)));
    } else {
        m_saveStateLabel->setText(QString::fromUtf8("\xE2\x9C\x93 saved"));   // ✓ saved
        m_saveStateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::success)));
    }

    // Honest client-side counts — the editor's own document, no
    // fabricated numbers.
    const QString text = m_editor->toPlainText();
    const int lineCount = m_editor->document()->blockCount();
    const int charCount = text.length();
    m_countsLabel->setText(tr("%1 lines \xC2\xB7 %2 chars").arg(lineCount).arg(charCount));
}
