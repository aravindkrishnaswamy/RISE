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
// Double-quoted string / file-path literals ("vase.obj") -- a single
// line's worth (RISE scene text has no multi-line quoted strings),
// non-greedy so back-to-back "a" "b" colors as two tokens.
const QRegularExpression RISESyntaxHighlighter::s_stringRegex(QStringLiteral("\"[^\"\\n]*\""));

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

    // Colors mirror the macOS RISESceneSyntaxHighlighter.swift's
    // RISESceneTheme hex values exactly, per the design comp.

    // Comment: dim gray (Theme::textDisabled)
    m_commentFmt.setForeground(QColor(0x5c, 0x5f, 0x66));
    m_commentFmt.setFont(monoFont);

    // File header: purple bold
    m_fileHeaderFmt.setForeground(QColor(0xc8, 0xa0, 0xe8));
    m_fileHeaderFmt.setFont(boldFont);

    // Block keyword: soft blue bold (Theme::accentSoft)
    m_blockKeywordFmt.setForeground(QColor(0x8f, 0xb8, 0xe8));
    m_blockKeywordFmt.setFont(boldFont);

    // Property key: muted gray (Theme::textMuted)
    m_propertyKeyFmt.setForeground(QColor(0x9a, 0x9d, 0xa4));
    m_propertyKeyFmt.setFont(monoFont);

    // Command (> directive): teal
    m_commandFmt.setForeground(QColor(0x8f, 0xd4, 0xc4));
    m_commandFmt.setFont(monoFont);

    // Preprocessor: amber (Theme::warn)
    m_preprocessorFmt.setForeground(QColor(0xe0, 0xb2, 0x5a));
    m_preprocessorFmt.setFont(monoFont);

    // Loop directive: amber bold
    m_loopDirectiveFmt.setForeground(QColor(0xe0, 0xb2, 0x5a));
    m_loopDirectiveFmt.setFont(boldFont);

    // Macro reference (@NAME): purple
    m_macroRefFmt.setForeground(QColor(0xc8, 0xa0, 0xe8));
    m_macroRefFmt.setFont(monoFont);

    // Math expression $(...): muted purple/pink
    m_mathExprFmt.setForeground(QColor(0xc9, 0xa0, 0xd4));
    m_mathExprFmt.setFont(monoFont);

    // Number / vector literal: soft green (Theme::successLight)
    m_numberFmt.setForeground(QColor(0xa9, 0xd4, 0xb1));
    m_numberFmt.setFont(monoFont);

    // Braces: dim gray (Theme::textDim)
    m_bracesFmt.setForeground(QColor(0x6f, 0x72, 0x78));
    m_bracesFmt.setFont(monoFont);

    // Quoted string / file-path values: gold (Theme::gold)
    m_stringFmt.setForeground(QColor(0xd4, 0xb9, 0x8a));
    m_stringFmt.setFont(monoFont);
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

    // Quoted string / file-path literals
    it = s_stringRegex.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_stringFmt);
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
