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
#include <functional>

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

    /// Reverse source traceability: forward an enablement predicate to the
    /// embedded SceneTextEdit's "Select in Inspector" item.  MainWindow sets
    /// this (folding in its render-transport gate + this editor's dirty
    /// state) so the item greys out instead of silently no-opping.
    void setCanSelectEntityPredicate(std::function<bool()> pred);

signals:
    void closeRequested();
    void saveAndReloadRequested(const QString& filePath);

    /// Reverse source traceability: relayed from the embedded SceneTextEdit
    /// when the user picks "Select in Inspector"; carries the UTF-8 byte
    /// offset of the click.  MainWindow resolves it to a scene entity and
    /// selects it.  (Forward direction: the revealAt slots above.)
    void selectEntityAtByteOffsetRequested(quint64 byteOffset);

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

    /// "Reveal in scene file" (item 3): scroll to the line containing
    /// UTF-8 byte offset `byteOffset` in the editor's CURRENT buffer,
    /// select the whole line, and briefly flash it.  ASSUMPTION
    /// (documented per the design brief): the buffer is byte-identical
    /// to the CST serialization `byteOffset` was resolved against --
    /// MainWindow::revealEntityInSceneText only calls this when
    /// `!isDirty()`, so the buffer's last mirror (refreshFromLiveScene)
    /// is exactly what the bridge resolved the offset against.  A
    /// stale/out-of-range offset bails out silently rather than
    /// crashing or landing on the wrong line.
    void revealAt(quint64 byteOffset);

    /// Source-traceability overload: when `byteLength > 0`, select+flash
    /// EXACTLY [byteOffset, byteOffset+byteLength) (a param's `role value`
    /// run) instead of the whole line; `byteLength == 0` falls back to the
    /// line-selecting behaviour of `revealAt(byteOffset)` above.  Both byte
    /// offsets are converted to UTF-16 char indices the same way (the
    /// inverse of SceneTextEdit::cursorByteOffsetUtf8).  Same stale/out-of-
    /// range guard: an offset (or offset+length) past the buffer end is a
    /// silent no-op, and a range that would end before it starts falls back
    /// to the line rather than selecting nothing.
    void revealAt(quint64 byteOffset, quint64 byteLength);

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
