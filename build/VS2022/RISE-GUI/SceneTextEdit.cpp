//////////////////////////////////////////////////////////////////////
//
//  SceneTextEdit.cpp - Right-click builds a QMenu from the library's
//    SuggestionEngine and inserts the chosen suggestion at the caret.
//    Inline autocomplete (ghost text + popup) is deferred; this
//    implementation delivers the context-menu half of the feature.
//
//////////////////////////////////////////////////////////////////////

#include "SceneTextEdit.h"

#include <QAbstractItemView>
#include <QAction>
#include <QByteArray>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QString>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCursor>

#include <map>
#include <memory>

#include "SceneEditorSuggestions/SuggestionEngine.h"
#include "SceneEditorSuggestions/SceneGrammar.h"

SceneTextEdit::SceneTextEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseSensitive);  // scene grammar is case-sensitive
    connect(m_completer, QOverload<const QString&>::of(&QCompleter::activated),
            this, &SceneTextEdit::insertCompletion);

    // Debounce completion triggers so we don't re-query the engine on every
    // keystroke mid-word; 200 ms matches the Mac app's cadence.
    m_completionTimer.setSingleShot(true);
    m_completionTimer.setInterval(200);
    connect(&m_completionTimer, &QTimer::timeout, this, &SceneTextEdit::onRequestCompletions);
}

SceneTextEdit::~SceneTextEdit() = default;

std::size_t SceneTextEdit::cursorByteOffsetUtf8() const
{
    const QTextCursor cursor = textCursor();
    const int codeUnitPos = cursor.position();
    const QString upto = toPlainText().left(codeUnitPos);
    return static_cast<std::size_t>(upto.toUtf8().size());
}

void SceneTextEdit::contextMenuEvent(QContextMenuEvent* event)
{
    using namespace RISE::SceneEditorSuggestions;

    // Move the caret to the click position so suggestions reflect where
    // the user actually right-clicked rather than the prior selection.
    QTextCursor cur = cursorForPosition(event->pos());
    setTextCursor(cur);

    const QByteArray utf8 = toPlainText().toUtf8();
    const std::string buffer(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const std::size_t cursorByte = cursorByteOffsetUtf8();

    SuggestionEngine engine;
    const std::vector<Suggestion> sugs = engine.GetSuggestions(buffer, cursorByte, SuggestionMode::ContextMenu);

    if (sugs.empty()) {
        // Fall back to Qt's default context menu so the user can still cut/copy/paste.
        QMenu* def = createStandardContextMenu();
        def->exec(event->globalPos());
        delete def;
        return;
    }

    QMenu topMenu(this);

    // Group by category — useful at SceneRoot where there are ~126 entries
    // across 17 categories; flat for inside-a-block where the list is the
    // parameters of a single chunk.
    std::map<RISE::ChunkCategory, std::vector<const Suggestion*>> byCat;
    bool hasChunkKeyword = false;
    for (const auto& s : sugs) {
        byCat[s.category].push_back(&s);
        if (s.kind == SuggestionKind::ChunkKeyword) hasChunkKeyword = true;
    }

    auto insertSuggestionAction = [this](QMenu* m, const Suggestion& s) {
        QString text = QString::fromStdString(s.insertText);
        QString label = QString::fromStdString(s.displayText);
        QAction* act = m->addAction(label);
        if (!s.description.empty()) {
            act->setToolTip(QString::fromStdString(s.description));
        }
        connect(act, &QAction::triggered, this, [this, text]() {
            this->textCursor().insertText(text);
        });
    };

    if (hasChunkKeyword && byCat.size() > 1) {
        const auto& grammar = SceneGrammar::Instance();
        for (auto& kv : byCat) {
            QMenu* sub = topMenu.addMenu(QString::fromUtf8(grammar.CategoryDisplayName(kv.first)));
            for (const Suggestion* s : kv.second) {
                insertSuggestionAction(sub, *s);
            }
        }
    } else {
        for (const auto& s : sugs) {
            insertSuggestionAction(&topMenu, s);
        }
    }

    topMenu.addSeparator();
    // Parent the default edit actions (Cut/Copy/Paste/…) to topMenu so
    // they outlive the helper QMenu we pull them from.  Deleting stdMenu
    // while topMenu still references its QActions would leave dangling
    // pointers in topMenu's internal action list.
    std::unique_ptr<QMenu> stdMenu( createStandardContextMenu() );
    for (QAction* a : stdMenu->actions()) {
        a->setParent(&topMenu);
        topMenu.addAction(a);
    }
    topMenu.exec(event->globalPos());
}

QString SceneTextEdit::partialWordUnderCursor() const
{
    QTextCursor cur = textCursor();
    const int pos = cur.position();
    const QString block = cur.block().text();
    const int blockPos = pos - cur.block().position();
    // Walk left from the caret to find the start of the current token.
    int start = blockPos;
    while (start > 0) {
        const QChar c = block[start - 1];
        if (c.isSpace()) break;
        --start;
    }
    return block.mid(start, blockPos - start);
}

void SceneTextEdit::keyPressEvent(QKeyEvent* event)
{
    if (m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab: {
            // Accept the currently-highlighted completion.  QCompleter
            // does not activate on Tab by default; we handle it
            // explicitly so the promised "Tab/→ accepts ghost text"
            // UX works on Windows.
            const QModelIndex idx = m_completer->popup()->currentIndex();
            QString txt;
            if (idx.isValid()) {
                txt = idx.data().toString();
            } else if (m_completionModel->rowCount() > 0) {
                txt = m_completionModel->data(m_completionModel->index(0, 0)).toString();
            }
            m_completer->popup()->hide();
            if (!txt.isEmpty()) {
                insertCompletion(txt);
            }
            event->accept();
            return;
        }
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            m_completer->popup()->hide();
            event->accept();
            return;
        default:
            break;
        }
    }

    QPlainTextEdit::keyPressEvent(event);

    // Backspace returns empty text(); detect it separately so the
    // popup refreshes as the user edits the partial word backwards.
    if (event->key() == Qt::Key_Backspace) {
        m_completionTimer.start();
        return;
    }

    // Trigger completions only on visible character input — ignore
    // modifier keys, navigation, and deletion so the popup doesn't
    // churn on every arrow key.
    const QString txt = event->text();
    if (txt.isEmpty()) {
        return;
    }
    const QChar c = txt.at(0);
    if (!c.isLetterOrNumber() && c != QChar('_')) {
        // On whitespace / punctuation, hide any live popup.
        if (m_completer->popup()->isVisible()) {
            m_completer->popup()->hide();
        }
        return;
    }

    m_completionTimer.start();  // debounce
}

void SceneTextEdit::focusInEvent(QFocusEvent* event)
{
    if (m_completer) {
        m_completer->setWidget(this);
    }
    QPlainTextEdit::focusInEvent(event);
}

void SceneTextEdit::insertCompletion(const QString& completion)
{
    if (!m_completer || m_completer->widget() != this) return;
    QTextCursor cur = textCursor();
    const int extra = completion.length() - m_completer->completionPrefix().length();
    // Append the remainder of the suggested text at the caret.
    if (extra > 0) {
        cur.insertText(completion.right(extra));
    } else if (extra < 0) {
        // Suggested text is shorter than the prefix (shouldn't happen with
        // prefix match, but fall back to replacing the token).
        cur.movePosition(QTextCursor::StartOfWord, QTextCursor::KeepAnchor);
        cur.insertText(completion);
    }
    setTextCursor(cur);
}

void SceneTextEdit::onRequestCompletions()
{
    using namespace RISE::SceneEditorSuggestions;

    const QString partial = partialWordUnderCursor();
    if (partial.length() < 2) {
        // Keep the popup quiet until the user has typed enough to narrow
        // the list; two characters matches the design's trigger threshold.
        if (m_completer->popup()->isVisible()) m_completer->popup()->hide();
        return;
    }

    const QByteArray utf8 = toPlainText().toUtf8();
    const std::string buffer(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const std::size_t cursorByte = cursorByteOffsetUtf8();

    SuggestionEngine engine;
    const std::vector<Suggestion> sugs = engine.GetSuggestions(buffer, cursorByte, SuggestionMode::InlineCompletion);

    QStringList items;
    items.reserve(static_cast<int>(sugs.size()));
    int unambiguousIdx = -1;
    for (std::size_t i = 0; i < sugs.size(); ++i) {
        items.append(QString::fromStdString(sugs[i].insertText));
        if (sugs[i].isUnambiguousCompletion && unambiguousIdx < 0) {
            unambiguousIdx = static_cast<int>(i);
        }
    }
    if (items.isEmpty()) {
        if (m_completer->popup()->isVisible()) m_completer->popup()->hide();
        return;
    }

    m_completionModel->setStringList(items);
    m_completer->setCompletionPrefix(partial);

    // Position the popup under the current word.
    QRect rect = cursorRect();
    rect.setWidth(m_completer->popup()->sizeHintForColumn(0)
                  + m_completer->popup()->verticalScrollBar()->sizeHint().width());
    m_completer->complete(rect);

    // Pre-select the unambiguous ghost-text candidate when there is one.
    if (unambiguousIdx >= 0) {
        const QModelIndex idx = m_completionModel->index(unambiguousIdx, 0);
        m_completer->popup()->setCurrentIndex(idx);
    }
}
