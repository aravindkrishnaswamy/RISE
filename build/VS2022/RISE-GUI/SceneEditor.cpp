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
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QMessageBox>

namespace {

// Shared by makeHeaderPill() (construction) and SceneEditor::restyleTheme()
// (live theme switch) so the two can never hand-copy-drift apart.
QString headerPillStyleSheet()
{
    return QStringLiteral(
        "QPushButton { color: %1; border: 1px solid %2; border-radius: 6px; padding: 3px 10px; background: transparent; }"
        "QPushButton:disabled { color: %3; border-color: %4; }"
        "QPushButton:hover:!disabled { border-color: %5; }")
        .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderStrong),
             Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight),
             Theme::hex(Theme::borderHover));
}

// Compact bordered pill button shared by Revert/Save/Save&Reload/close —
// mirrors the design comp's header affordances.
QPushButton* makeHeaderPill(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setFont(Theme::sans(11));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);
    btn->setStyleSheet(headerPillStyleSheet());
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
    m_header = new QWidget(this);
    QWidget* header = m_header;
    header->setFixedHeight(38);
    header->setAutoFillBackground(true);
    // Palette fill + stylesheet: styled in restyleTheme() -- LIVE THEME-
    // SWITCH CONTRACT (Theme.h).

    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(QStringLiteral("Scene file"), header);
    m_titleLabel->setFont(Theme::sans(11, QFont::DemiBold));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    headerLayout->addWidget(m_titleLabel);

    m_dirtyDot = new QLabel(QString::fromUtf8("\xE2\x97\x8F"), header);   // U+25CF BLACK CIRCLE
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
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
    // library's SceneEditorSuggestions::SuggestionEngine.  Theme::mono(12);
    // the bg/selection palette is per-mode (Dark/Light), set below by
    // restyleTheme() -- mirrors the Mac editor, which also switches with
    // RISE's own theme mode rather than committing to one fixed surface.
    m_editor = new SceneTextEdit();
    m_editor->setFont(Theme::mono(12));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4 * m_editor->fontMetrics().horizontalAdvance(' '));
    m_editor->setFrameShape(QFrame::NoFrame);
    // Editor backdrop palette (Base/Text/Highlight/HighlightedText) is
    // set by restyleTheme() below (called at the end of this
    // constructor) -- see that method for why it's a per-mode literal
    // table mirroring the Mac editor's editorBackground/selectionBackground,
    // not a straight Theme::bgPanel forward.
    layout->addWidget(m_editor, 1);

    // Syntax highlighter
    m_highlighter = new RISESyntaxHighlighter(m_editor->document());

    // ---- Bottom status bar: save state + honest client-side line/char
    // counts.  NO Ctrl+S shortcut on the Save button here — the Mac
    // client removed the ⌘S-equivalent for a collision with the
    // production-render shortcut; mirrored here by simply never binding
    // one (m_saveBtn is a plain click target only).
    m_statusBar = new QWidget(this);
    QWidget* statusBar = m_statusBar;
    statusBar->setFixedHeight(28);
    statusBar->setAutoFillBackground(true);
    // Palette fill + stylesheet: styled in restyleTheme() -- LIVE THEME-
    // SWITCH CONTRACT (Theme.h).
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 0, 12, 0);
    statusLayout->setSpacing(10);

    m_saveStateLabel = new QLabel(statusBar);
    m_saveStateLabel->setFont(Theme::mono(10));
    statusLayout->addWidget(m_saveStateLabel);

    // CST <-> scene-file live sync (item 1): amber "buffer is stale"
    // warning, hidden by default -- shown BESIDE m_saveStateLabel, not
    // in place of it, when the poll sets m_behindLiveScene (see
    // setBehindLiveScene's doc).
    m_staleWarningLabel = new QLabel(
        QString::fromUtf8("\xE2\x9A\xA0 scene changed elsewhere \xE2\x80\x94 buffer is stale"), statusBar);
    m_staleWarningLabel->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_staleWarningLabel->hide();
    statusLayout->addWidget(m_staleWarningLabel);

    statusLayout->addStretch();

    m_countsLabel = new QLabel(statusBar);
    m_countsLabel->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    statusLayout->addWidget(m_countsLabel);

    layout->addWidget(statusBar);

    // Connections
    connect(m_closeBtn, &QPushButton::clicked, this, &SceneEditor::closeRequested);
    connect(m_revertBtn, &QPushButton::clicked, this, &SceneEditor::revert);
    connect(m_saveBtn, &QPushButton::clicked, this, &SceneEditor::save);
    connect(m_saveReloadBtn, &QPushButton::clicked, this, &SceneEditor::saveAndReload);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SceneEditor::onTextChanged);
    // Reverse source traceability: relay the editor's "Select in Inspector"
    // request up to MainWindow (which owns the bridge + selection).
    connect(m_editor, &SceneTextEdit::selectEntityAtByteOffsetRequested,
            this, &SceneEditor::selectEntityAtByteOffsetRequested);

    updateDirtyState();
    m_themeReady = true;
    restyleTheme();
}

void SceneEditor::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void SceneEditor::restyleTheme()
{
    // LIVE THEME-SWITCH CONTRACT (Theme.h): every one of SceneEditor's
    // own token-dependent styling sites, re-applied from the CURRENT
    // Theme:: token values. Called once at the end of the constructor
    // and again from changeEvent() on every QEvent::PaletteChange.
    // Idempotent, creates no widgets.
    m_themeEpochSeen = Theme::paletteEpoch();

    if (m_header) {
        QPalette pal = m_header->palette();
        pal.setColor(QPalette::Window, Theme::bgHeader);
        m_header->setPalette(pal);
        m_header->setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    }
    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_dirtyDot) {
        m_dirtyDot->setStyleSheet(QStringLiteral("color: %1; font-size: 7px;").arg(Theme::hex(Theme::dirty)));
    }
    for (QPushButton* btn : { m_revertBtn, m_saveBtn, m_saveReloadBtn, m_closeBtn }) {
        if (btn) btn->setStyleSheet(headerPillStyleSheet());
    }

    if (m_statusBar) {
        QPalette pal = m_statusBar->palette();
        pal.setColor(QPalette::Window, Theme::bgHeader);
        m_statusBar->setPalette(pal);
        m_statusBar->setStyleSheet(QStringLiteral("border-top: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    }
    if (m_staleWarningLabel) {
        m_staleWarningLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::warn)));
    }
    if (m_countsLabel) {
        m_countsLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }
    // m_saveStateLabel's color is dirty-vs-clean state-dependent
    // (Theme::dirty / Theme::success) and already fully recomputed by
    // updateDirtyState() below -- no separate line needed here.

    // ---- Editor backdrop (Base/Text/Highlight/HighlightedText) --------
    // Deliberately NOT a straight Theme::bgPanel/textPrimary/accent
    // forward: verified against the Mac SceneEditorWindow.swift +
    // RISESceneSyntaxHighlighter.swift RISESceneTheme.init(), whose
    // `.light` branch commits the editor to a bright white raised
    // surface (its own literal 0xffffff, matching Theme::bgWell's light
    // value, NOT Theme::bgPanel's dimmer 0xf0f0f2) while `.dark` uses
    // the same literal value Theme::bgPanel's dark value already has
    // (0x17181b). Mirrored here 1:1, per mode, rather than deriving
    // from whichever Theme:: token happens to coincide in one mode but
    // not the other.
    if (m_editor) {
        const bool dark = (Theme::effectiveMode() != Theme::ThemeMode::Light);
        QPalette pal = m_editor->palette();
        if (dark) {
            pal.setColor(QPalette::Base, QColor(0x17, 0x18, 0x1b));
            pal.setColor(QPalette::Text, QColor(0xe6, 0xe7, 0xe9));
            pal.setColor(QPalette::Highlight, QColor(0x6d, 0xb8, 0xe8));
            pal.setColor(QPalette::HighlightedText, QColor(0x0d, 0x11, 0x16));
        } else {
            pal.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
            pal.setColor(QPalette::Text, QColor(0x1a, 0x1b, 0x1e));
            pal.setColor(QPalette::Highlight, QColor(0x1a, 0x6f, 0xa8));
            pal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
        }
        m_editor->setPalette(pal);

        // Slim themed scrollbars (Task A): applied directly to the
        // QPlainTextEdit's own scrollbar widgets, not the editor itself,
        // so the QSS can't leak into any other selector. Re-applied here
        // every restyleTheme() call since the QSS bakes token colors.
        if (QScrollBar* vbar = m_editor->verticalScrollBar()) {
            vbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
        if (QScrollBar* hbar = m_editor->horizontalScrollBar()) {
            hbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
    }

    // Syntax-highlighter palette: a genuinely separate Dark/Light hex
    // table (RISESyntaxHighlighter.h's doc) -- applyTheme() itself is a
    // no-op unless Theme::effectiveMode() actually changed since the
    // last call, and only rehighlights the document when it does
    // something, per this method's own "creates no widgets, idempotent"
    // contract.
    if (m_highlighter) {
        m_highlighter->applyTheme();
    }

    // Re-derive the dirty-dependent labels (save-state color/text,
    // revert/save/save&reload enablement+tooltip) from the CURRENT
    // Theme:: tokens too -- updateDirtyState() already reads them live
    // at call time, so re-invoking it here is enough; no separate
    // restyle logic duplicated for m_saveStateLabel.
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

void SceneEditor::setCanSelectEntityPredicate(std::function<bool()> pred)
{
    m_editor->setCanSelectEntityPredicate(std::move(pred));
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

void SceneEditor::markUntitled()
{
    m_filePath.clear();
    updateDirtyState();
}

QString SceneEditor::text() const
{
    return m_editor->toPlainText();
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

void SceneEditor::refreshFromLiveScene(const QString& text)
{
    m_originalText = text;
    m_editor->setPlainText(text);
    m_behindLiveScene = false;
    updateDirtyState();
}

void SceneEditor::revealAt(quint64 byteOffset)
{
    // The whole-line (entity/whole-chunk) reveal is the length-0 case of
    // the span overload below.
    revealAt(byteOffset, 0);
}

void SceneEditor::revealAt(quint64 byteOffset, quint64 byteLength)
{
    if (!m_editor) return;

    // Convert the UTF-8 byte offset (what
    // SceneEditController::EntitySourceLocation / ResolveSourceSpan
    // resolved against) into a UTF-16 code-unit index (what
    // QTextCursor::setPosition wants) -- the inverse of
    // SceneTextEdit::cursorByteOffsetUtf8.  A byte offset at or past the
    // end of the buffer means the caller's assumption (buffer == the
    // serialization the offset was resolved against) no longer holds;
    // bail out silently rather than landing on the wrong line.
    const QString text = m_editor->toPlainText();
    const QByteArray utf8 = text.toUtf8();
    if (byteOffset >= static_cast<quint64>(utf8.size())) return;
    const QByteArray prefix = utf8.left(static_cast<int>(byteOffset));
    const int charIndex = QString::fromUtf8(prefix).length();
    if (charIndex < 0 || charIndex > text.length()) return;

    QTextCursor cursor(m_editor->document());
    cursor.setPosition(charIndex);

    // byteLength > 0: select EXACTLY [byteOffset, byteOffset+byteLength)
    // (a param's `role value` run) by anchoring at the start and dragging
    // to the end with KeepAnchor.  Convert the END byte offset the SAME
    // way (utf8.left -> QString::fromUtf8().length()).  byteLength == 0 (or
    // an end that clamps past the buffer / before the start) falls back to
    // selecting the whole line, matching the entity-level reveal.
    bool rangeSelected = false;
    if (byteLength > 0) {
        const quint64 endByteOffset = byteOffset + byteLength;
        if (endByteOffset <= static_cast<quint64>(utf8.size())) {
            const QByteArray endPrefix = utf8.left(static_cast<int>(endByteOffset));
            const int endCharIndex = QString::fromUtf8(endPrefix).length();
            if (endCharIndex > charIndex && endCharIndex <= text.length()) {
                cursor.setPosition(endCharIndex, QTextCursor::KeepAnchor);
                rangeSelected = true;
            }
        }
    }
    if (!rangeSelected) {
        cursor.select(QTextCursor::LineUnderCursor);
    }

    m_editor->setTextCursor(cursor);
    m_editor->ensureCursorVisible();
    m_editor->setFocus();

    // Brief flash: a temporary extra-selection highlight, cleared after
    // a short delay.  Purely cosmetic -- extra selections never mutate
    // the document's permanent text/formatting, so this can't desync
    // from `text` / the dirty-tracking in updateDirtyState().
    QColor flashColor = Theme::accent;
    flashColor.setAlpha(90);   // ~0.35 alpha, matches the design brief's flash intensity
    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;
    selection.format.setBackground(flashColor);
    // A whole-line reveal fills the row (FullWidthSelection); a tight span
    // must cover exactly the selected characters, so do NOT set it there.
    if (!rangeSelected) {
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    }
    m_editor->setExtraSelections({ selection });

    // Context-object overload: the lambda is dropped automatically if
    // `this` is destroyed before the timer fires (e.g. the SceneEditor
    // is torn down mid-flash), so no dangling-pointer guard is needed.
    QTimer::singleShot(600, this, [this]() {
        if (m_editor) m_editor->setExtraSelections({});
    });
}

void SceneEditor::setBehindLiveScene(bool behind)
{
    if (m_behindLiveScene == behind) return;
    m_behindLiveScene = behind;
    updateDirtyState();
}

void SceneEditor::updateDirtyState()
{
    const bool dirty = isDirty();
    m_dirtyDot->setVisible(dirty);
    m_revertBtn->setEnabled(dirty);
    // Untitled scenes (empty m_filePath) have no in-place save target --
    // save()/saveAndReload() would either no-op or, before markUntitled(),
    // have CORRUPTED the bundled starter template (review P1).  Disable
    // honestly with a hint instead.
    const bool savable = dirty && !m_filePath.isEmpty();
    m_saveBtn->setEnabled(savable);
    m_saveReloadBtn->setEnabled(savable);
    const QString hint = m_filePath.isEmpty()
        ? tr("Untitled scene -- use the top-bar Save (Save As...) first")
        : QString();
    m_saveBtn->setToolTip(hint);
    m_saveReloadBtn->setToolTip(hint);

    if (dirty) {
        m_saveStateLabel->setText(QString::fromUtf8("\xE2\x97\x8F unsaved edits"));   // ● unsaved edits
        m_saveStateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::dirty)));
    } else {
        m_saveStateLabel->setText(QString::fromUtf8("\xE2\x9C\x93 saved"));   // ✓ saved
        m_saveStateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::success)));
    }

    // CST <-> scene-file live sync (item 1): only ever true alongside
    // `dirty` (see setBehindLiveScene's doc / MainWindow's poll) --
    // shown beside "unsaved edits" rather than replacing it.
    if (m_staleWarningLabel) {
        m_staleWarningLabel->setVisible(m_behindLiveScene);
    }

    // Honest client-side counts — the editor's own document, no
    // fabricated numbers.
    const QString text = m_editor->toPlainText();
    const int lineCount = m_editor->document()->blockCount();
    const int charCount = text.length();
    m_countsLabel->setText(tr("%1 lines \xC2\xB7 %2 chars").arg(lineCount).arg(charCount));
}
