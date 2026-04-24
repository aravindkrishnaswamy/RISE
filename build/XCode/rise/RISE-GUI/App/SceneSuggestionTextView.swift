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

        if suggestions.isEmpty {
            // Fall back to AppKit's default text menu so the user can
            // still cut / copy / paste / look up.
            return super.menu(for: event)
        }

        let root = NSMenu(title: "Suggestions")

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
                root.addItem(header)
            }
        } else {
            for s in suggestions {
                root.addItem(makeItem(for: s))
            }
        }

        // Append the default editing actions (Cut / Copy / Paste / …)
        // so the right-click menu is still fully functional.
        if let stdMenu = super.menu(for: event) {
            root.addItem(.separator())
            for item in stdMenu.items {
                // items can only belong to one menu; copy them
                let copy = NSMenuItem(title: item.title, action: item.action, keyEquivalent: item.keyEquivalent)
                copy.target = item.target
                copy.keyEquivalentModifierMask = item.keyEquivalentModifierMask
                copy.representedObject = item.representedObject
                copy.tag = item.tag
                root.addItem(copy)
            }
        }

        return root
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
}
