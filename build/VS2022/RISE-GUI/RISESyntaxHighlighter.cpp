//////////////////////////////////////////////////////////////////////
//
//  RISESyntaxHighlighter.cpp - Syntax highlighting implementation.
//
//  Ported from the Mac app's RISESceneSyntaxHighlighter.swift.
//  Single-pass line-by-line classification with sub-line regex.
//
//////////////////////////////////////////////////////////////////////

#include "RISESyntaxHighlighter.h"

#include <QFont>
#include <QFontDatabase>

#include "SceneEditorSuggestions/SceneGrammar.h"

// Static regex patterns (matching Mac app)
const QRegularExpression RISESyntaxHighlighter::s_bracesRegex(QStringLiteral("[{}]"));
const QRegularExpression RISESyntaxHighlighter::s_macroRefRegex(QStringLiteral("@[A-Za-z_]\\w*"));
const QRegularExpression RISESyntaxHighlighter::s_mathExprRegex(QStringLiteral("\\$\\([^)]*\\)"));
const QRegularExpression RISESyntaxHighlighter::s_numberRegex(QStringLiteral("(?<=\\s)-?(?:\\d+\\.?\\d*|\\.\\d+)(?=\\s|$)"));
const QRegularExpression RISESyntaxHighlighter::s_propertyKeyRegex(QStringLiteral("^(\\t+)(\\w+)"));

// Block keywords — populated from SceneEditorSuggestions::SceneGrammar on
// first call.  The parser's chunk registry is the single source of truth;
// adding a new chunk there automatically makes it a recognized block
// keyword here with no changes to this file.
const QSet<QString>& RISESyntaxHighlighter::blockKeywords()
{
    static const QSet<QString> s = []{
        QSet<QString> set;
        const auto& kws = RISE::SceneEditorSuggestions::SceneGrammar::Instance().AllChunkKeywords();
        for( const auto& k : kws ) {
            set.insert( QString::fromStdString(k) );
        }
        return set;
    }();
    return s;
}

RISESyntaxHighlighter::RISESyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(12);
    QFont boldFont = monoFont;
    boldFont.setBold(true);

    // Comment: green
    m_commentFmt.setForeground(QColor(0, 160, 0));
    m_commentFmt.setFont(monoFont);

    // File header: purple bold
    m_fileHeaderFmt.setForeground(QColor(128, 0, 128));
    m_fileHeaderFmt.setFont(boldFont);

    // Block keyword: blue bold
    m_blockKeywordFmt.setForeground(QColor(0, 80, 255));
    m_blockKeywordFmt.setFont(boldFont);

    // Property key: indigo
    m_propertyKeyFmt.setForeground(QColor(75, 0, 130));
    m_propertyKeyFmt.setFont(monoFont);

    // Command: teal
    m_commandFmt.setForeground(QColor(0, 128, 128));
    m_commandFmt.setFont(monoFont);

    // Preprocessor: orange
    m_preprocessorFmt.setForeground(QColor(230, 140, 0));
    m_preprocessorFmt.setFont(monoFont);

    // Loop directive: orange bold
    m_loopDirectiveFmt.setForeground(QColor(230, 140, 0));
    m_loopDirectiveFmt.setFont(boldFont);

    // Macro reference: red
    m_macroRefFmt.setForeground(QColor(220, 30, 30));
    m_macroRefFmt.setFont(monoFont);

    // Math expression: pink
    m_mathExprFmt.setForeground(QColor(255, 105, 180));
    m_mathExprFmt.setFont(monoFont);

    // Number: cyan
    m_numberFmt.setForeground(QColor(0, 170, 190));
    m_numberFmt.setFont(monoFont);

    // Braces: gray
    m_bracesFmt.setForeground(QColor(128, 128, 128));
    m_bracesFmt.setFont(monoFont);
}

void RISESyntaxHighlighter::highlightBlock(const QString& text)
{
    if (text.isEmpty()) return;

    QString trimmed = text.trimmed();

    // 1. File header (first line containing "RISE ASCII SCENE")
    if (currentBlock().blockNumber() == 0 && trimmed.startsWith("RISE ASCII SCENE")) {
        setFormat(0, text.length(), m_fileHeaderFmt);
        return;
    }

    // 2. Comment
    if (trimmed.startsWith('#')) {
        setFormat(0, text.length(), m_commentFmt);
        return;
    }

    // 3. Command directive
    if (trimmed.startsWith('>')) {
        setFormat(0, text.length(), m_commandFmt);
        return;
    }

    // 4. DEFINE / ! preprocessor
    if (trimmed.startsWith("DEFINE ") || trimmed.startsWith("define ") || trimmed.startsWith('!')) {
        setFormat(0, text.length(), m_preprocessorFmt);
        return;
    }

    // 5. UNDEF / ~ preprocessor
    if (trimmed.startsWith("UNDEF ") || trimmed.startsWith("undef ") || trimmed.startsWith('~')) {
        setFormat(0, text.length(), m_preprocessorFmt);
        return;
    }

    // 6. Loop directive (FOR / ENDFOR)
    if (trimmed.startsWith("FOR ") || trimmed == "ENDFOR" || trimmed.startsWith("ENDFOR")) {
        setFormat(0, text.length(), m_loopDirectiveFmt);
        return;
    }

    // 7. Block keyword (exact match on trimmed line)
    if (blockKeywords().contains(trimmed)) {
        setFormat(0, text.length(), m_blockKeywordFmt);
        return;
    }

    // 8. Regular line — apply sub-line patterns
    highlightLineContents(text);
}

void RISESyntaxHighlighter::highlightLineContents(const QString& text)
{
    // Property key: first word on tab-indented line
    QRegularExpressionMatch propMatch = s_propertyKeyRegex.match(text);
    if (propMatch.hasMatch() && propMatch.lastCapturedIndex() >= 2) {
        int start = propMatch.capturedStart(2);
        int length = propMatch.capturedLength(2);
        setFormat(start, length, m_propertyKeyFmt);
    }

    // Braces
    QRegularExpressionMatchIterator it = s_bracesRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_bracesFmt);
    }

    // Macro references (@NAME)
    it = s_macroRefRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_macroRefFmt);
    }

    // Math expressions $(...)
    it = s_mathExprRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_mathExprFmt);
    }

    // Numbers
    it = s_numberRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_numberFmt);
    }
}
