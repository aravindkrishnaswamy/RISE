//////////////////////////////////////////////////////////////////////
//
//  SceneTextEdit.h - QPlainTextEdit subclass that hosts the scene
//    text and adds scene-aware right-click suggestions.  The
//    suggestion engine lives in src/Library/SceneEditorSuggestions;
//    this class is the thin Qt adapter.
//
//////////////////////////////////////////////////////////////////////

#ifndef SCENETEXTEDIT_H
#define SCENETEXTEDIT_H

#include <QPlainTextEdit>
#include <QTimer>
#include <functional>

class QCompleter;
class QStringListModel;

class SceneTextEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit SceneTextEdit(QWidget* parent = nullptr);
    ~SceneTextEdit() override;

    /// Reverse source traceability: predicate deciding whether "Select in
    /// Inspector" can act right now (a clean, editable scene buffer).  When
    /// it returns false the menu item is shown DISABLED rather than silently
    /// no-opping -- the affordance stays "right or absent, never misleading",
    /// matching the forward reveal affordances.  Evaluated at right-click
    /// time; unset == always enabled.  Set by MainWindow via SceneEditor
    /// (it folds in both the render-transport gate and the dirty check).
    void setCanSelectEntityPredicate(std::function<bool()> pred) {
        m_canSelectEntity = std::move(pred);
    }

signals:
    // Reverse source traceability (text -> UI select): the user picked
    // "Select in Inspector" from the right-click menu.  Carries the UTF-8
    // byte offset of the click position (the caret, which contextMenuEvent
    // moves to the click before emitting) so MainWindow can resolve it to
    // the containing entity via ViewportBridge::sourceRefAtByteOffset and
    // select that entity.  The forward direction is SceneEditor::revealAt.
    void selectEntityAtByteOffsetRequested(quint64 byteOffset);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private slots:
    void insertCompletion(const QString& completion);
    void onRequestCompletions();

private:
    // Converts the caret's UTF-16 code-unit offset (what QTextCursor::position
    // returns) into a UTF-8 byte offset suitable for the library's
    // SuggestionEngine, given the current document text.
    std::size_t cursorByteOffsetUtf8() const;

    // Current partial token under the caret (what the completer filters by).
    QString partialWordUnderCursor() const;

    QCompleter*       m_completer = nullptr;
    QStringListModel* m_completionModel = nullptr;
    QTimer            m_completionTimer; // debounces complete-trigger on keystrokes
    std::function<bool()> m_canSelectEntity; // reverse-traceability item enablement
};

#endif // SCENETEXTEDIT_H
