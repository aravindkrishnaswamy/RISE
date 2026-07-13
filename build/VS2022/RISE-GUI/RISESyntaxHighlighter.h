//////////////////////////////////////////////////////////////////////
//
//  RISESyntaxHighlighter.h - QSyntaxHighlighter for .RISEscene files.
//
//  Ported from the Mac app's RISESceneSyntaxHighlighter.swift.
//  107 block keywords, line-by-line classification with sub-line
//  regex patterns for property keys, macros, math, numbers, braces.
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

protected:
    void highlightBlock(const QString& text) override;

private:
    void highlightLineContents(const QString& text);

    // Colors -- lifted from the approved design comp; mirrors the macOS
    // RISESceneSyntaxHighlighter.swift's RISESceneTheme hex values 1:1
    // (see that file's doc comment for the per-category rationale).
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
