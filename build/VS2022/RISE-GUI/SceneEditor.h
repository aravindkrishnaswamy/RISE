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

    /// CST <-> scene-file live sync (UI refinement item 1): mirror the
    /// live CST's serialized text into the buffer, resetting the dirty
    /// baseline so the buffer reads as clean immediately afterward.
    /// Caller (MainWindow's poll) only calls this when !isDirty() --
    /// this method does NOT itself check that, so it must never be
    /// called while the user has unsaved edits (that would silently
    /// discard them).
    void refreshFromLiveScene(const QString& text);

    /// Amber "scene changed elsewhere" warning: true when the live CST
    /// has moved on while the buffer still has unsaved edits (set by
    /// MainWindow's poll; only ever true alongside isDirty() -- see
    /// that poll's doc).  Cleared by refreshFromLiveScene() directly,
    /// and every poll tick MainWindow observes the buffer as clean
    /// again (this class does not watch its own dirty transitions to
    /// auto-clear the flag; the poll drives it explicitly either way).
    void setBehindLiveScene(bool behind);

private slots:
    void onTextChanged();

private:
    SceneTextEdit* m_editor = nullptr;
    RISESyntaxHighlighter* m_highlighter = nullptr;
    QPushButton* m_revertBtn = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_saveReloadBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QLabel* m_dirtyDot = nullptr;

    // Bottom status bar (RISE UI redesign): save-state label ("● unsaved
    // edits" amber / "✓ saved" green) + honest client-side line/char
    // counts.  Mirrors the design comp's Scene file tab footer strip.
    QLabel* m_saveStateLabel = nullptr;
    // CST <-> scene-file live sync (item 1): amber "buffer is stale"
    // warning, shown BESIDE m_saveStateLabel (not replacing it) when
    // m_behindLiveScene is true -- mirrors SceneEditorWindow.swift's
    // statusBar, which shows both facts at once ("unsaved edits" AND
    // "scene changed elsewhere").
    QLabel* m_staleWarningLabel = nullptr;
    QLabel* m_countsLabel = nullptr;

    QString m_filePath;
    QString m_originalText;
    bool m_behindLiveScene = false;

    void updateDirtyState();
};

#endif // SCENEEDITOR_H
