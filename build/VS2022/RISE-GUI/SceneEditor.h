//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.h - Collapsible scene editor panel with syntax
//  highlighting, dirty detection, and Save/Revert/Save & Reload.
//
//  Ported from the Mac app's SceneEditorWindow.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENEEDITOR_H
#define SCENEEDITOR_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>

class RISESyntaxHighlighter;
class SceneTextEdit;

class SceneEditor : public QWidget
{
    Q_OBJECT

public:
    explicit SceneEditor(QWidget* parent = nullptr);

    void loadFile(const QString& filePath);
    void refreshFromDisk();
    bool isDirty() const;
    QString filePath() const { return m_filePath; }

signals:
    void closeRequested();
    void saveAndReloadRequested(const QString& filePath);

public slots:
    void save();
    void revert();
    void saveAndReload();

private slots:
    void onTextChanged();

private:
    SceneTextEdit* m_editor = nullptr;
    RISESyntaxHighlighter* m_highlighter = nullptr;
    QPushButton* m_revertBtn = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_saveReloadBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QLabel* m_modifiedBadge = nullptr;

    QString m_filePath;
    QString m_originalText;

    void updateDirtyState();
};

#endif // SCENEEDITOR_H
