//
//  SceneSuggestionTextView.swift
//
//  NSTextView subclass that wires the RISESceneEditorBridge into the
//  scene editor.  Two integration points:
//
//    1) menu(for:) — builds a right-click NSMenu from the library's
//       SuggestionEngine.  Chunk-keyword suggestions at scene root are
//       grouped by category into submenus; parameter lists inside a
//       block are flat.
//
//    2) Completions delegate (installed on the view's delegate) —
//       feeds AppKit's built-in complete(_:) popup with suggestions
//       from the bridge.  Triggered automatically on text change so
//       the user gets inline completion without pressing Escape.
//

import AppKit

/// Converts a UTF-16 character index into a UTF-8 byte offset so
/// the C++ suggestion engine can consume cursor positions produced
/// by NSTextView (which are NSString character indices).
@inline(__always)
func utf8ByteOffset(text: String, utf16Index: Int) -> Int {
    let ns = text as NSString
    let clamped = max(0, min(utf16Index, ns.length))
    let prefix = ns.substring(to: clamped)
    return prefix.utf8.count
}

final class SceneSuggestionTextView: NSTextView {
    let suggestionBridge = RISESceneEditorBridge()

    /// Reverse source traceability (text -> UI select): called with the
    /// UTF-8 byte offset of the click when the user picks "Select in
    /// Inspector".  Wired by the SceneTextEditor representable to
    /// `RenderViewModel.reverseSelectEntity`; nil when unwired.
    var onSelectEntityAtByteOffset: ((Int) -> Void)?

    /// Reverse source traceability: whether "Select in Inspector" can act
    /// right now (a clean, editable scene buffer -- `canReverseSelect`).
    /// When it returns false the item is shown DISABLED rather than silently
    /// no-opping, so the affordance stays "right or absent, never misleading"
    /// -- matching the forward reveal affordances.  Consulted from
    /// `validateMenuItem`.  nil == always enabled.
    var canSelectEntityAtByteOffset: (() -> Bool)?

    /// Cached chunk categories for submenu titles — mirrors the
    /// grouping RISESceneEditorBridge provides via categoryDisplayName
    /// on each suggestion.
    override func menu(for event: NSEvent) -> NSMenu? {
        // Move the insertion point to wherever the right-click landed
        // so suggestions reflect the click target rather than the
        // prior selection.
        let point = convert(event.locationInWindow, from: nil)
        let charIndex = characterIndexForInsertion(at: point)
        setSelectedRange(NSRange(location: charIndex, length: 0))

        let byteOffset = utf8ByteOffset(text: string, utf16Index: charIndex)
        let suggestions = suggestionBridge.suggestions(
            forText: string,
            cursorByte: UInt(byteOffset),
            mode: .contextMenu) as [RISESuggestion]

        // Start from AppKit's native text menu so its submenus (Font,
        // Spelling, Substitutions, …), separators, and per-item enablement
        // stay intact; PREPEND our items rather than cloning (a manual clone
        // drops submenus / state / separators — a real regression).
        let menu = super.menu(for: event) ?? NSMenu(title: "Suggestions")
        // Our "Select in Inspector" greying relies on validateMenuItem being
        // consulted, which requires autoenablesItems (true on AppKit's text
        // menu, but make the invariant explicit so the disabled state is
        // robust against a future menu source).
        menu.autoenablesItems = true

        // Build the leading block in display order, then insert it at the top.
        var leading: [NSMenuItem] = []

        // Reverse source traceability: always the first item, independent of
        // whether there are suggestions here.  Enablement is decided by
        // validateMenuItem (canSelectEntityAtByteOffset), so a dirty or
        // uneditable buffer shows it greyed rather than silently dead.
        let selectItem = NSMenuItem(title: "Select in Inspector",
                                    action: #selector(selectEntityAction(_:)),
                                    keyEquivalent: "")
        selectItem.target = self
        selectItem.representedObject = byteOffset
        selectItem.toolTip = "Select the scene entity this text belongs to"
        leading.append(selectItem)
        leading.append(.separator())

        if !suggestions.isEmpty {
            // Group by category when at scene root (lots of chunk types);
            // flat otherwise.
            let hasChunkKeywords = suggestions.contains(where: { $0.kind == .chunkKeyword })
            if hasChunkKeywords {
                var byCategory: [String: [RISESuggestion]] = [:]
                var order: [String] = []
                for s in suggestions {
                    let cat = s.categoryDisplayName
                    if byCategory[cat] == nil {
                        byCategory[cat] = []
                        order.append(cat)
                    }
                    byCategory[cat]!.append(s)
                }
                for cat in order {
                    let sub = NSMenu(title: cat)
                    for s in byCategory[cat]! {
                        sub.addItem(makeItem(for: s))
                    }
                    let header = NSMenuItem(title: cat, action: nil, keyEquivalent: "")
                    header.submenu = sub
                    leading.append(header)
                }
            } else {
                for s in suggestions {
                    leading.append(makeItem(for: s))
                }
            }
            leading.append(.separator())
        }

        for (i, item) in leading.enumerated() {
            menu.insertItem(item, at: i)
        }
        return menu
    }

    /// Drives the enablement of "Select in Inspector" (reverse source
    /// traceability).  All other items fall through to NSTextView's own
    /// validation so the native editing actions grey out normally.
    override func validateMenuItem(_ item: NSMenuItem) -> Bool {
        if item.action == #selector(selectEntityAction(_:)) {
            return canSelectEntityAtByteOffset?() ?? true
        }
        return super.validateMenuItem(item)
    }

    private func makeItem(for suggestion: RISESuggestion) -> NSMenuItem {
        let item = NSMenuItem(title: suggestion.displayText, action: #selector(insertSuggestionAction(_:)), keyEquivalent: "")
        item.target = self
        item.representedObject = suggestion.insertText
        if !suggestion.descriptionText.isEmpty {
            item.toolTip = suggestion.descriptionText
        }
        return item
    }

    @objc private func insertSuggestionAction(_ sender: NSMenuItem) {
        guard let text = sender.representedObject as? String else { return }
        insertText(text, replacementRange: selectedRange())
    }

    @objc private func selectEntityAction(_ sender: NSMenuItem) {
        guard let offset = sender.representedObject as? Int else { return }
        onSelectEntityAtByteOffset?(offset)
    }

    /// Expose the bridge so the SceneTextEditor's coordinator can reach
    /// it from the NSTextViewDelegate completion methods.
    func suggestionsForCompletions(
        forPartialWordRange charRange: NSRange) -> [RISESuggestion]
    {
        let byteOffset = utf8ByteOffset(text: string, utf16Index: NSMaxRange(charRange))
        let array = suggestionBridge.suggestions(
            forText: string,
            cursorByte: UInt(byteOffset),
            mode: .inlineCompletion) as [RISESuggestion]
        return array
    }

    /// UTF-16 character index where the partial word started when the
    /// user dismissed the completion popup with Escape.  While set,
    /// `isCurrentWordSuppressed()` returns true and the auto-trigger
    /// in the coordinator should leave the popup hidden.  Cleared
    /// automatically once the caret moves to a different word.
    private var escapeSuppressedWordStart: Int?

    /// AppKit invokes this when the completion popup is being
    /// dismissed.  Escape produces `flag == true` with
    /// `movement == NSTextMovement.cancel.rawValue`; record the
    /// partial-word start so the auto-trigger does not immediately
    /// re-show the popup for the same word.  Always forward to super
    /// — the default implementation handles the dismissal (no
    /// insertion on cancel, normal insertion on accept).
    override func insertCompletion(_ word: String,
                                   forPartialWordRange charRange: NSRange,
                                   movement: Int,
                                   isFinal flag: Bool)
    {
        if flag && movement == NSTextMovement.cancel.rawValue {
            escapeSuppressedWordStart = charRange.location
        }
        super.insertCompletion(word,
                               forPartialWordRange: charRange,
                               movement: movement,
                               isFinal: flag)
    }

    /// True while the caret is still inside the word for which the
    /// user just pressed Escape.  Walks backward from the caret to the
    /// previous whitespace to find the current word's start; if that
    /// matches the recorded suppression offset we are still in the
    /// same word.  Any mismatch (caret moved to a different word, or
    /// the word boundary shifted past the recorded offset) clears the
    /// flag so a fresh keystroke can re-arm the popup.
    func isCurrentWordSuppressed() -> Bool {
        guard let suppressedStart = escapeSuppressedWordStart else { return false }
        let ns = string as NSString
        let cursor = selectedRange().location
        if cursor > ns.length {
            escapeSuppressedWordStart = nil
            return false
        }
        var wordStart = cursor
        while wordStart > 0 {
            let prev = ns.substring(with: NSRange(location: wordStart - 1, length: 1))
            if prev == " " || prev == "\t" || prev == "\n" || prev == "\r" {
                break
            }
            wordStart -= 1
        }
        if wordStart == suppressedStart {
            return true
        }
        escapeSuppressedWordStart = nil
        return false
    }
}
