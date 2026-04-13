//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.cpp - Scene editor panel implementation.
//
//  Ported from the Mac app's SceneEditorWindow.swift.
//
//////////////////////////////////////////////////////////////////////

#include "SceneEditor.h"
#include "RISESyntaxHighlighter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QFontDatabase>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>

SceneEditor::SceneEditor(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Header row
    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel("Scene Editor");
    titleLabel->setStyleSheet("font-weight: bold;");
    headerLayout->addWidget(titleLabel);

    m_modifiedBadge = new QLabel("Modified");
    m_modifiedBadge->setStyleSheet(
        "color: white; background-color: orange; border-radius: 4px; "
        "padding: 1px 6px; font-size: 11px;");
    m_modifiedBadge->hide();
    headerLayout->addWidget(m_modifiedBadge);

    headerLayout->addStretch();

    m_closeBtn = new QPushButton("X");
    m_closeBtn->setFixedSize(24, 24);
    m_closeBtn->setFlat(true);
    headerLayout->addWidget(m_closeBtn);

    layout->addLayout(headerLayout);

    // Toolbar
    auto* toolbarLayout = new QHBoxLayout();
    m_revertBtn = new QPushButton("Revert");
    m_saveBtn = new QPushButton("Save");
    m_saveReloadBtn = new QPushButton("Save && Reload");
    m_revertBtn->setEnabled(false);
    m_saveBtn->setEnabled(false);
    m_saveReloadBtn->setEnabled(false);
    toolbarLayout->addWidget(m_revertBtn);
    toolbarLayout->addWidget(m_saveBtn);
    toolbarLayout->addWidget(m_saveReloadBtn);
    toolbarLayout->addStretch();
    layout->addLayout(toolbarLayout);

    // Editor
    m_editor = new QPlainTextEdit();
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(12);
    m_editor->setFont(monoFont);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4 * m_editor->fontMetrics().horizontalAdvance(' '));
    layout->addWidget(m_editor);

    // Syntax highlighter
    m_highlighter = new RISESyntaxHighlighter(m_editor->document());

    // Connections
    connect(m_closeBtn, &QPushButton::clicked, this, &SceneEditor::closeRequested);
    connect(m_revertBtn, &QPushButton::clicked, this, &SceneEditor::revert);
    connect(m_saveBtn, &QPushButton::clicked, this, &SceneEditor::save);
    connect(m_saveReloadBtn, &QPushButton::clicked, this, &SceneEditor::saveAndReload);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SceneEditor::onTextChanged);
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
    bool dirty = isDirty();
    m_modifiedBadge->setVisible(dirty);
    m_revertBtn->setEnabled(dirty);
    m_saveBtn->setEnabled(dirty);
    m_saveReloadBtn->setEnabled(dirty);
}
