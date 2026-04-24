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

class QCompleter;
class QStringListModel;

class SceneTextEdit : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit SceneTextEdit(QWidget* parent = nullptr);
    ~SceneTextEdit() override;

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
};

#endif // SCENETEXTEDIT_H
