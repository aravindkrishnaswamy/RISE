import AppKit

/// Color and font theme for RISE scene file syntax highlighting.
///
/// RISE UI redesign (left panel, "SCENE FILE TAB"): fixed dark colors
/// lifted from the approved design comp, replacing the previous
/// NSColor.system* variants — the redesigned editor commits to the
/// same dark surface in light and dark OS appearance (see
/// SceneEditorWindow's SceneTextEditor, which now also sets a fixed
/// dark background instead of the system control background), so
/// adaptive system colors would fight the fixed backdrop rather than
/// complement it.
struct RISESceneTheme {
    /// Entity / plain text.
    let defaultText: NSColor = NSColor(hex: 0xe6e7e9)
    let comment: NSColor = NSColor(hex: 0x5c5f66)
    let fileHeader: NSColor = NSColor(hex: 0xc8a0e8)
    let blockKeyword: NSColor = NSColor(hex: 0x8fb8e8)
    let propertyKey: NSColor = NSColor(hex: 0x9a9da4)
    let preprocessor: NSColor = NSColor(hex: 0xe0b25a)
    let loopDirective: NSColor = NSColor(hex: 0xe0b25a)
    let command: NSColor = NSColor(hex: 0x8fd4c4)
    /// References / macros (`@NAME`).
    let macroRef: NSColor = NSColor(hex: 0xc8a0e8)
    let mathExpr: NSColor = NSColor(hex: 0xc9a0d4)
    /// Numbers / vectors.
    let number: NSColor = NSColor(hex: 0xa9d4b1)
    let braces: NSColor = NSColor(hex: 0x6f7278)
    /// Quoted string / file-path values (e.g. `"vase.obj"`) — a new
    /// category added for the redesign; the pre-redesign highlighter
    /// had no distinct color for quoted strings (they fell through to
    /// `defaultText`), but the comp calls out "string/file values" as
    /// its own color, so `Self.stringRegex` below now classifies them.
    let string: NSColor = NSColor(hex: 0xd4b98a)

    let font: NSFont = Theme.monoNSFont(12)
    let boldFont: NSFont = Theme.monoNSFont(12, .semibold)
}

/// Applies syntax highlighting to RISE .RISEscene file content in an NSTextStorage.
///
/// Installed as the `NSTextStorageDelegate` on the editor's text storage. Whenever
/// characters change, the full document is re-highlighted using a single-pass
/// line-by-line classification. This is fast enough for typical scene files
/// (a few thousand lines).
final class RISESceneSyntaxHighlighter: NSObject, NSTextStorageDelegate {

    let theme = RISESceneTheme()

    // MARK: - Block Type Keywords
    //
    // Populated on first access from the library's SceneGrammar via
    // RISESceneEditorBridge.  The chunk-parser registry is the single
    // source of truth — adding a new chunk type in ChunkParserRegistry.cpp
    // automatically makes it a recognized block keyword here with no
    // changes to this file.

    static let blockKeywords: Set<String> = {
        let bridge = RISESceneEditorBridge()
        return Set(bridge.allChunkKeywords())
    }()

    // MARK: - Pre-compiled Regex Patterns

    private static let bracesRegex = try! NSRegularExpression(pattern: #"[{}]"#)
    private static let macroRefRegex = try! NSRegularExpression(pattern: #"@[A-Za-z_]\w*"#)
    private static let mathExprRegex = try! NSRegularExpression(pattern: #"\$\([^)]*\)"#)
    private static let numberRegex = try! NSRegularExpression(
        pattern: #"(?<=\s)-?(?:\d+\.?\d*|\.\d+)(?=\s|$)"#
    )
    /// Double-quoted string / file-path literals (`"vase.obj"`) — a
    /// single line's worth (RISE scene text has no multi-line quoted
    /// strings), non-greedy so back-to-back `"a" "b"` colors as two
    /// tokens rather than one.
    private static let stringRegex = try! NSRegularExpression(pattern: #""[^"\n]*""#)
    private static let propertyKeyRegex = try! NSRegularExpression(
        pattern: #"^(\t+)(\w+)"#, options: .anchorsMatchLines
    )

    // MARK: - NSTextStorageDelegate

    func textStorage(
        _ textStorage: NSTextStorage,
        didProcessEditing editedMask: NSTextStorageEditActions,
        range editedRange: NSRange,
        changeInLength delta: Int
    ) {
        // Only re-highlight when actual characters changed, not attribute-only edits.
        // This prevents an infinite loop: we change attributes → delegate fires again
        // → but the second call has only .editedAttributes → we skip it.
        guard editedMask.contains(.editedCharacters) else { return }
        highlightAll(in: textStorage)
    }

    // MARK: - Full-Document Highlighting

    /// Re-highlights the entire text storage using a single-pass line-by-line scan.
    func highlightAll(in textStorage: NSTextStorage) {
        let text = textStorage.string
        let fullRange = NSRange(location: 0, length: textStorage.length)
        guard fullRange.length > 0 else { return }

        // Reset everything to default font and color
        textStorage.addAttributes([
            .font: theme.font,
            .foregroundColor: theme.defaultText,
        ], range: fullRange)

        let nsString = text as NSString
        var lineStart = 0

        while lineStart < nsString.length {
            var lineEnd = 0
            var contentsEnd = 0
            nsString.getLineStart(
                nil, end: &lineEnd, contentsEnd: &contentsEnd,
                for: NSRange(location: lineStart, length: 0)
            )

            let lineRange = NSRange(location: lineStart, length: contentsEnd - lineStart)
            let lineStr = nsString.substring(with: lineRange)
            let trimmed = lineStr.trimmingCharacters(in: .whitespaces)

            if lineStart == 0 && trimmed.hasPrefix("RISE ASCII SCENE") {
                // File header line
                textStorage.addAttributes([
                    .foregroundColor: theme.fileHeader,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else if trimmed.hasPrefix("#") {
                // Comment — color the entire line and skip sub-line patterns
                textStorage.addAttribute(
                    .foregroundColor, value: theme.comment, range: lineRange
                )

            } else if trimmed.hasPrefix(">") {
                // Command directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.command, range: lineRange
                )

            } else if trimmed.hasPrefix("DEFINE ") || trimmed.hasPrefix("define ")
                        || trimmed.hasPrefix("!") {
                // DEFINE / ! directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.preprocessor, range: lineRange
                )

            } else if trimmed.hasPrefix("UNDEF ") || trimmed.hasPrefix("undef ")
                        || trimmed.hasPrefix("~") {
                // UNDEF / ~ directive
                textStorage.addAttribute(
                    .foregroundColor, value: theme.preprocessor, range: lineRange
                )

            } else if trimmed.hasPrefix("FOR ") || trimmed == "ENDFOR"
                        || trimmed.hasPrefix("ENDFOR") {
                // Loop directive
                textStorage.addAttributes([
                    .foregroundColor: theme.loopDirective,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else if Self.blockKeywords.contains(trimmed) {
                // Block type keyword (appears alone on a line)
                textStorage.addAttributes([
                    .foregroundColor: theme.blockKeyword,
                    .font: theme.boldFont,
                ], range: lineRange)

            } else {
                // Regular line — apply sub-line highlighting for property keys,
                // braces, macros, math expressions, and numbers.
                highlightLineContents(
                    nsString: nsString, lineRange: lineRange,
                    lineStr: lineStr, storage: textStorage
                )
            }

            lineStart = lineEnd
        }
    }

    // MARK: - Sub-line Highlighting

    /// Applies fine-grained highlighting to a non-classified line (typically
    /// a property line inside a block, a bare brace, or a blank line).
    private func highlightLineContents(
        nsString: NSString, lineRange: NSRange,
        lineStr: String, storage: NSTextStorage
    ) {
        let lineNSRange = NSRange(location: 0, length: lineStr.count)

        // Property key: first word on a tab-indented line
        let propertyMatches = Self.propertyKeyRegex.matches(
            in: lineStr, range: lineNSRange
        )
        if let match = propertyMatches.first, match.numberOfRanges >= 3 {
            let keyLocalRange = match.range(at: 2)
            let keyRange = NSRange(
                location: lineRange.location + keyLocalRange.location,
                length: keyLocalRange.length
            )
            storage.addAttribute(.foregroundColor, value: theme.propertyKey, range: keyRange)
        }

        // Braces
        applyRegex(
            Self.bracesRegex, color: theme.braces,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Quoted string / file-path literals.
        applyRegex(
            Self.stringRegex, color: theme.string,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Macro references (@NAME) — applied before numbers so @I doesn't get
        // partially colored if adjacent to digits.
        applyRegex(
            Self.macroRefRegex, color: theme.macroRef,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Math expressions $(...)
        applyRegex(
            Self.mathExprRegex, color: theme.mathExpr,
            in: nsString, searchRange: lineRange, storage: storage
        )

        // Numeric literals
        applyRegex(
            Self.numberRegex, color: theme.number,
            in: nsString, searchRange: lineRange, storage: storage
        )
    }

    // MARK: - Helpers

    private func applyRegex(
        _ regex: NSRegularExpression, color: NSColor,
        in nsString: NSString, searchRange: NSRange, storage: NSTextStorage
    ) {
        let matches = regex.matches(
            in: nsString as String, range: searchRange
        )
        for match in matches {
            storage.addAttribute(.foregroundColor, value: color, range: match.range)
        }
    }
}
