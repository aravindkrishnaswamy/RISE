//////////////////////////////////////////////////////////////////////
//
//  RISESyntaxHighlighter.h - QSyntaxHighlighter for .RISEscene files.
//
//  Ported from the Mac app's RISESceneSyntaxHighlighter.swift.
//  107 block keywords, line-by-line classification with sub-line
//  regex patterns for property keys, macros, math, numbers, braces.
//
//  Phase 2 live theme switch: colors DO switch with RISE's own Dark/
//  Light mode (Theme::effectiveMode()), same as the Mac source this was
//  ported from -- verified against RISESceneSyntaxHighlighter.swift's
//  RISESceneTheme.init(), which re-derives per `ThemeState.mode` on
//  every editor rebuild. What's fixed is that it does NOT follow the
//  OS-level light/dark appearance independently of RISE's own mode
//  (that file's header comment) -- a genuinely separate axis from the
//  Dark<->Light switch this contract cares about. See applyTheme()
//  below, called from SceneEditor::restyleTheme().
//
//////////////////////////////////////////////////////////////////////

#ifndef RISESYNTAXHIGHLIGHTER_H
#define RISESYNTAXHIGHLIGHTER_H

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QSet>

class RISESyntaxHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit RISESyntaxHighlighter(QTextDocument* parent = nullptr);

    /// LIVE THEME-SWITCH CONTRACT (Theme.h) analogue for a
    /// QSyntaxHighlighter (not a QWidget, so it gets no changeEvent of
    /// its own -- the owning SceneEditor's restyleTheme() calls this
    /// instead). Re-derives every format's foreground color from
    /// Theme::effectiveMode()'s CURRENT value and, ONLY if that mode
    /// actually differs from the one last applied (i.e. a real Dark<->
    /// Light flip happened, not a no-op re-call), reassigns every
    /// QTextCharFormat and calls rehighlight() so already-visible text
    /// repaints with the new palette. Safe to call any number of times;
    /// a no-op mode re-call does neither. See RISESyntaxHighlighter.cpp
    /// for the dark/light hex tables (mirrors the macOS
    /// RISESceneSyntaxHighlighter.swift RISESceneTheme.init() switch
    /// 1:1 -- verified against that file, including its one deliberate
    /// non-token-derived outlier, blockKeyword's light-mode hex).
    void applyTheme();

protected:
    void highlightBlock(const QString& text) override;

private:
    void highlightLineContents(const QString& text);
    /// Shared by the constructor and applyTheme(): assigns every
    /// m_*Fmt's foreground from the `dark` hex table when true, the
    /// `light` table otherwise.
    void setFormatColors(bool dark);

    // Colors -- lifted from the approved design comp; mirrors the macOS
    // RISESceneSyntaxHighlighter.swift's RISESceneTheme hex values 1:1
    // per mode (see that file's RISESceneTheme.init() for the per-
    // category, per-mode rationale -- setFormatColors() in the .cpp
    // mirrors both its .dark and .light branches).
    // Whichever mode's colors are CURRENTLY assigned to the format
    // members below -- compared against Theme::effectiveMode() at the
    // top of applyTheme() to decide whether a real switch happened.
    bool m_darkApplied = true;
    QTextCharFormat m_commentFmt;
    QTextCharFormat m_fileHeaderFmt;
    QTextCharFormat m_blockKeywordFmt;
    QTextCharFormat m_propertyKeyFmt;
    QTextCharFormat m_commandFmt;
    QTextCharFormat m_preprocessorFmt;
    QTextCharFormat m_loopDirectiveFmt;
    QTextCharFormat m_macroRefFmt;
    QTextCharFormat m_mathExprFmt;
    QTextCharFormat m_numberFmt;
    QTextCharFormat m_bracesFmt;
    // Quoted string / file-path values (e.g. "vase.obj") -- a category
    // added for the redesign; the pre-redesign highlighter had no
    // distinct color for these (they fell through to the default text
    // color), matching the comp's dedicated "string/file values" color.
    QTextCharFormat m_stringFmt;

    // Regex patterns
    static const QRegularExpression s_bracesRegex;
    static const QRegularExpression s_macroRefRegex;
    static const QRegularExpression s_mathExprRegex;
    static const QRegularExpression s_numberRegex;
    static const QRegularExpression s_propertyKeyRegex;
    static const QRegularExpression s_stringRegex;

    // Block keywords — lazily populated from SceneGrammar on first call so the
    // 126-entry list is the parser's Describe() output (single source of truth),
    // not a duplicated literal.
    static const QSet<QString>& blockKeywords();
};

#endif // RISESYNTAXHIGHLIGHTER_H
