//////////////////////////////////////////////////////////////////////
//
//  RISESyntaxHighlighter.cpp - Syntax highlighting implementation.
//
//  Ported from the Mac app's RISESceneSyntaxHighlighter.swift.
//  Single-pass line-by-line classification with sub-line regex.
//
//////////////////////////////////////////////////////////////////////

#include "RISESyntaxHighlighter.h"
#include "Theme.h"

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

    m_commentFmt.setFont(monoFont);
    m_fileHeaderFmt.setFont(boldFont);
    m_blockKeywordFmt.setFont(boldFont);
    m_propertyKeyFmt.setFont(monoFont);
    m_commandFmt.setFont(monoFont);
    m_preprocessorFmt.setFont(monoFont);
    m_loopDirectiveFmt.setFont(boldFont);
    m_macroRefFmt.setFont(monoFont);
    m_mathExprFmt.setFont(monoFont);
    m_numberFmt.setFont(monoFont);
    m_bracesFmt.setFont(monoFont);
    m_stringFmt.setFont(monoFont);

    // Dark is RISE's primary/first-class theme (Theme.h) and the mode
    // Theme:: tokens hold before Theme::loadPersistedMode() ever runs at
    // startup -- matches this constructor's pre-Phase-2 hardcoded dark
    // values 1:1, so a scene loaded before the persisted mode resolves
    // still starts correctly themed.
    setFormatColors(/*dark=*/true);
}

void RISESyntaxHighlighter::setFormatColors(bool dark)
{
    // Colors mirror the macOS RISESceneSyntaxHighlighter.swift's
    // RISESceneTheme.init() hex values exactly, per mode (verified
    // against that file's `.dark`/`.light` switch branches). These are
    // a deliberately INDEPENDENT literal palette, not derived from
    // Theme:: tokens at call time -- most categories happen to equal a
    // Theme:: token's value in a given mode (e.g. m_commentFmt mirrors
    // textDisabled/textDim), but blockKeyword's LIGHT value (0x2e6da8)
    // is a bespoke darker/more-saturated blue chosen for legibility on
    // a light editor background that does NOT equal Theme::accentSoft's
    // light value (0x3a77ad) -- mirroring Mac exactly means keeping our
    // own hex table here rather than reading Theme:: tokens directly.
    if (dark) {
        m_commentFmt.setForeground(QColor(0x5c, 0x5f, 0x66));
        m_fileHeaderFmt.setForeground(QColor(0xc8, 0xa0, 0xe8));
        m_blockKeywordFmt.setForeground(QColor(0x8f, 0xb8, 0xe8));
        m_propertyKeyFmt.setForeground(QColor(0x9a, 0x9d, 0xa4));
        m_commandFmt.setForeground(QColor(0x8f, 0xd4, 0xc4));
        m_preprocessorFmt.setForeground(QColor(0xe0, 0xb2, 0x5a));
        m_loopDirectiveFmt.setForeground(QColor(0xe0, 0xb2, 0x5a));
        m_macroRefFmt.setForeground(QColor(0xc8, 0xa0, 0xe8));
        m_mathExprFmt.setForeground(QColor(0xc9, 0xa0, 0xd4));
        m_numberFmt.setForeground(QColor(0xa9, 0xd4, 0xb1));
        m_bracesFmt.setForeground(QColor(0x6f, 0x72, 0x78));
        m_stringFmt.setForeground(QColor(0xd4, 0xb9, 0x8a));
    } else {
        m_commentFmt.setForeground(QColor(0x8f, 0x93, 0x9c));
        m_fileHeaderFmt.setForeground(QColor(0x7b, 0x4f, 0xa6));
        m_blockKeywordFmt.setForeground(QColor(0x2e, 0x6d, 0xa8));
        m_propertyKeyFmt.setForeground(QColor(0x5f, 0x63, 0x6b));
        m_commandFmt.setForeground(QColor(0x2b, 0x7d, 0x6e));
        m_preprocessorFmt.setForeground(QColor(0x9a, 0x6b, 0x10));
        m_loopDirectiveFmt.setForeground(QColor(0x9a, 0x6b, 0x10));
        m_macroRefFmt.setForeground(QColor(0x7b, 0x4f, 0xa6));
        m_mathExprFmt.setForeground(QColor(0x8a, 0x5a, 0x9e));
        m_numberFmt.setForeground(QColor(0x2e, 0x7d, 0x43));
        m_bracesFmt.setForeground(QColor(0xa6, 0xaa, 0xb2));
        m_stringFmt.setForeground(QColor(0x8a, 0x6c, 0x2f));
    }
}

void RISESyntaxHighlighter::applyTheme()
{
    const bool wantDark = (Theme::effectiveMode() != Theme::ThemeMode::Light);
    if (wantDark == m_darkApplied) return;   // no real mode change -- no-op
    m_darkApplied = wantDark;
    setFormatColors(wantDark);
    rehighlight();
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
