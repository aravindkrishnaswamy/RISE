import SwiftUI
import AppKit

/// Width of the editor panel, computed to fit the 109-character-wide
/// header blocks found in .RISEscene files.
let sceneEditorPanelWidth: CGFloat = {
    let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    let attrs: [NSAttributedString.Key: Any] = [.font: font]
    let headerLine = String(repeating: "#", count: 109)
    let textWidth = ceil((headerLine as NSString).size(withAttributes: attrs).width)
    // textContainerInset: 8pt each side = 16
    // lineFragmentPadding: 5pt each side = 10
    // scroller: 17pt
    // safety margin: 4pt
    return textWidth + 16 + 10 + 17 + 4
}()

/// A monospaced text editor backed by NSTextView for editing RISE scene files.
struct SceneTextEditor: NSViewRepresentable {
    @Binding var text: String
    /// "Reveal in scene file" (design comp ⌗ affordance): when non-nil and
    /// its `generation` hasn't been applied yet, `updateNSView` scrolls to
    /// `byteOffset`, selects the containing line, and briefly flashes it.
    /// See `RenderViewModel.revealEntityInSceneText`.
    var revealRequest: RenderViewModel.EditorRevealRequest? = nil
    /// Reverse source traceability (text -> UI select): invoked with the
    /// UTF-8 byte offset of a right-click "Select in Inspector".  Wired to
    /// `RenderViewModel.reverseSelectEntity`.
    var onSelectEntity: ((Int) -> Void)? = nil
    /// Whether the "Select in Inspector" item is enabled right now
    /// (`RenderViewModel.canReverseSelect`); false greys it out instead of
    /// letting it silently no-op on a dirty / uneditable buffer.
    var canSelectEntity: (() -> Bool)? = nil

    func makeNSView(context: Context) -> NSScrollView {
        // Build an NSScrollView around our custom SceneSuggestionTextView
        // so menu(for:) can deliver right-click suggestions and the
        // NSTextViewDelegate completions method can feed inline completion.
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.borderType = .noBorder
        scrollView.autohidesScrollers = false

        let contentSize = scrollView.contentSize
        let layoutManager = NSLayoutManager()
        let textContainer = NSTextContainer(
            size: NSSize(width: CGFloat.greatestFiniteMagnitude,
                         height: CGFloat.greatestFiniteMagnitude))
        layoutManager.addTextContainer(textContainer)

        let textStorage = NSTextStorage()
        textStorage.addLayoutManager(layoutManager)

        let textView = SceneSuggestionTextView(
            frame: NSRect(origin: .zero, size: contentSize),
            textContainer: textContainer)

        textView.isEditable = true
        textView.isSelectable = true
        textView.allowsUndo = true
        textView.usesFindBar = true
        textView.isIncrementalSearchingEnabled = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isRichText = true

        // RISE UI redesign (left panel, "SCENE FILE TAB"): a fixed
        // editor surface + IBM Plex Mono, matching RISESceneTheme's
        // fixed syntax palette (both commit to the same fixed backdrop
        // — keyed off RISE's own `ThemeState.mode`, not the OS
        // light/dark appearance — rather than following adaptive
        // system colors; see RISESceneTheme's doc for why). Install
        // the syntax highlighter first so its `theme` (chosen from the
        // current `ThemeState.mode`) can drive the surrounding chrome
        // colors below too, keeping a single source of truth.
        let highlighter = RISESceneSyntaxHighlighter()
        textView.textStorage?.delegate = highlighter
        context.coordinator.highlighter = highlighter
        let theme = highlighter.theme

        let font = Theme.monoNSFont(12)
        textView.font = font
        let textColor = theme.defaultText
        textView.typingAttributes = [
            .font: font,
            .foregroundColor: textColor,
        ]
        textView.backgroundColor = theme.editorBackground
        textView.drawsBackground = true
        textView.insertionPointColor = theme.caret
        textView.selectedTextAttributes = [
            .backgroundColor: theme.selectionBackground,
            .foregroundColor: textColor,
        ]
        // Comfortable line spacing for the fixed-line-height scene text
        // (matches the redesign's mono-line-height ~1.75 rhythm).
        let paragraphStyle = NSMutableParagraphStyle()
        paragraphStyle.lineSpacing = 3
        textView.defaultParagraphStyle = paragraphStyle
        textView.typingAttributes[.paragraphStyle] = paragraphStyle
        scrollView.backgroundColor = theme.editorBackground
        scrollView.drawsBackground = true

        textView.textContainerInset = NSSize(width: 8, height: 8)
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.textContainer?.widthTracksTextView = false
        textView.textContainer?.containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.minSize = NSSize(width: 0, height: 0)
        textView.autoresizingMask = [.width]

        scrollView.documentView = textView

        textView.delegate = context.coordinator
        context.coordinator.textView = textView
        context.coordinator.suggestionTextView = textView
        textView.onSelectEntityAtByteOffset = onSelectEntity
        textView.canSelectEntityAtByteOffset = canSelectEntity

        // Defer initial text population until after the view is fully
        // installed in the window's view hierarchy, avoiding the
        // "layoutSubtreeIfNeeded called during layout" recursion.
        DispatchQueue.main.async {
            textView.string = self.text
        }

        return scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        guard let textView = context.coordinator.textView else { return }
        if let sv = textView as? SceneSuggestionTextView {
            sv.onSelectEntityAtByteOffset = onSelectEntity
            sv.canSelectEntityAtByteOffset = canSelectEntity
        }
        if textView.string != text {
            let selectedRanges = textView.selectedRanges
            textView.string = text
            textView.selectedRanges = selectedRanges
        }
        // "Reveal in scene file": apply a NEW reveal generation once, even
        // when `text` itself didn't change this pass (e.g. a repeat click
        // on the same entity, or a click that landed right after an
        // unrelated text edit already handled above).
        if let request = revealRequest, request.generation != context.coordinator.lastAppliedRevealGeneration {
            context.coordinator.lastAppliedRevealGeneration = request.generation
            // Review P1: on a fresh mount (switching to the Scene tab
            // recreates this view) makeNSView's DEFERRED population
            // (`textView.string = self.text` on the next run-loop turn)
            // would run AFTER a synchronous reveal and reset scroll +
            // selection — the reveal flashed for one frame, then the
            // editor snapped to the top.  Enqueue the reveal on the same
            // main queue: FIFO ordering puts it strictly after the
            // population closure on a fresh mount, and on the
            // steady-state path (view already mounted) the deferral is
            // harmless.
            let text = self.text
            let byteOffset = request.byteOffset
            let byteLength = request.byteLength
            let flash = context.coordinator.highlighter?.theme.caret
            DispatchQueue.main.async { [weak textView] in
                guard let textView else { return }
                Self.reveal(in: textView, text: text, byteOffset: byteOffset,
                            byteLength: byteLength, flashColor: flash)
            }
        }
    }

    /// Scrolls `textView` to the line containing UTF-8 byte offset
    /// `byteOffset`, selects the full line, and briefly flashes it.
    ///
    /// ASSUMPTION (documented per the design brief): `text` — the
    /// editor buffer — is byte-identical to the CST serialization
    /// `byteOffset` was resolved against, i.e. the editor is live-synced
    /// and the user has not since typed into the buffer.  If `text` is
    /// user-dirty and SHORTER than `byteOffset` (edits removed content
    /// before the target), this bails out silently rather than crashing
    /// or landing on the wrong line — a stale reveal is a no-op, not a
    /// misleading one.
    private static func reveal(in textView: NSTextView, text: String, byteOffset: UInt64, byteLength: UInt64, flashColor: NSColor?) {
        let utf8 = text.utf8
        guard byteOffset < UInt64(utf8.count) else { return }
        guard let byteIndex = utf8.index(utf8.startIndex, offsetBy: Int(byteOffset), limitedBy: utf8.endIndex),
              let stringIndex = byteIndex.samePosition(in: text) else { return }

        // byteLength > 0 highlights the EXACT span (a param row: `role value`);
        // byteLength == 0 selects the whole line (an entity / whole-chunk reveal).
        let nsRange: NSRange
        if byteLength > 0,
           let endByteIndex = utf8.index(byteIndex, offsetBy: Int(byteLength), limitedBy: utf8.endIndex),
           let endStringIndex = endByteIndex.samePosition(in: text) {
            nsRange = NSRange(stringIndex..<endStringIndex, in: text)
        } else {
            let lineRange = text.lineRange(for: stringIndex..<stringIndex)
            nsRange = NSRange(lineRange, in: text)
        }

        textView.scrollRangeToVisible(nsRange)
        textView.setSelectedRange(nsRange)
        // Harmless no-op if textView is already first responder.
        textView.window?.makeFirstResponder(textView)

        // Brief flash: a temporary background attribute over the line,
        // reverted after a short delay.  Purely cosmetic — never mutates
        // `textStorage`'s permanent attributes or the underlying string,
        // so it can't desync from `text`/`textDidChange`.
        guard let color = flashColor, let layoutManager = textView.layoutManager else { return }
        layoutManager.addTemporaryAttribute(.backgroundColor, value: color.withAlphaComponent(0.35), forCharacterRange: nsRange)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { [weak textView] in
            guard let textView, let layoutManager = textView.layoutManager else { return }
            layoutManager.removeTemporaryAttribute(.backgroundColor, forCharacterRange: nsRange)
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(text: $text)
    }

    class Coordinator: NSObject, NSTextViewDelegate {
        var text: Binding<String>
        weak var textView: NSTextView?
        weak var suggestionTextView: SceneSuggestionTextView?
        /// Strong reference to keep the syntax highlighter alive.
        var highlighter: RISESceneSyntaxHighlighter?
        /// "Reveal in scene file": the last `EditorRevealRequest.generation`
        /// already applied, so `updateNSView` re-scrolls/flashes only on a
        /// NEW request (0 never matches a real generation, which starts at 1).
        var lastAppliedRevealGeneration = 0
        /// Debounce timer for the automatic complete(_:) trigger.
        private var completionDebounceItem: DispatchWorkItem?
        /// When true, the next textDidChange is the result of our own
        /// completion insertion and should not re-trigger the popup.
        private var suppressAutoCompletion = false

        init(text: Binding<String>) {
            self.text = text
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            text.wrappedValue = textView.string

            // Reset typing attributes so new keystrokes use default styling
            // rather than inheriting the color of the character at the cursor.
            // Matches makeNSView's fixed-theme setup above.
            let font = Theme.monoNSFont(12)
            let paragraphStyle = NSMutableParagraphStyle()
            paragraphStyle.lineSpacing = 3
            textView.typingAttributes = [
                .font: font,
                .foregroundColor: highlighter?.theme.defaultText ?? NSColor(hex: 0xe6e7e9),
                .paragraphStyle: paragraphStyle,
            ]

            if suppressAutoCompletion {
                suppressAutoCompletion = false
                return
            }

            // Auto-trigger AppKit's completion popup after a short
            // debounce so the user gets inline suggestions as they
            // type without thrashing on every keystroke.  The
            // delegate method below feeds the candidate list from
            // the C++ SuggestionEngine via the bridge.
            completionDebounceItem?.cancel()
            let item = DispatchWorkItem { [weak self, weak textView] in
                guard let textView = textView, let self = self else { return }
                // Only auto-trigger when there's a partial word at the caret.
                let selected = textView.selectedRange()
                if selected.length > 0 { return }
                let ns = textView.string as NSString
                if selected.location == 0 { return }
                let prevChar = ns.substring(with: NSRange(location: selected.location - 1, length: 1))
                if prevChar == " " || prevChar == "\t" || prevChar == "\n" { return }

                // If the user pressed Escape on the popup for this word,
                // honor that dismissal until the caret moves out of it.
                if let suggestionView = textView as? SceneSuggestionTextView,
                   suggestionView.isCurrentWordSuppressed() {
                    return
                }

                // Snapshot buffer state so we can detect whether complete(_:)
                // actually inserted text.  If it did (auto-accept of a
                // pre-selected candidate), suppress the resulting
                // textDidChange; otherwise clear the flag so the next
                // keystroke can trigger fresh completions.
                let beforeText = textView.string
                let beforeLocation = selected.location
                self.suppressAutoCompletion = true
                textView.complete(nil)
                let afterText = textView.string
                let afterLocation = textView.selectedRange().location
                let changed = (afterText != beforeText) || (afterLocation != beforeLocation)
                if !changed {
                    // complete(_:) only showed a popup (or did nothing);
                    // no subsequent textDidChange will fire to reset the
                    // flag, so clear it here.
                    self.suppressAutoCompletion = false
                }
            }
            completionDebounceItem = item
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: item)
        }

        // AppKit feeds this method to build the completion popup that
        // `complete(_:)` triggers (via Escape or our textDidChange
        // auto-trigger above).  We reach through the subclass to query
        // the C++ SuggestionEngine with the UTF-8 cursor position and
        // return the matching insertion strings in rank order.
        func textView(_ textView: NSTextView,
                      completions words: [String],
                      forPartialWordRange charRange: NSRange,
                      indexOfSelectedItem index: UnsafeMutablePointer<Int>?) -> [String]
        {
            guard let suggestionView = textView as? SceneSuggestionTextView else {
                return words
            }
            let suggestions = suggestionView.suggestionsForCompletions(forPartialWordRange: charRange)
            if suggestions.isEmpty { return [] }

            var results: [String] = []
            results.reserveCapacity(suggestions.count)
            var selectedIdx = -1
            for (i, s) in suggestions.enumerated() {
                results.append(s.insertText)
                if s.isUnambiguousCompletion && selectedIdx < 0 {
                    selectedIdx = i
                }
            }
            // If there is exactly one unambiguous completion, pre-select it
            // so AppKit renders the remainder inline (highlighted ghost-text
            // style).  Otherwise let AppKit show the popup flat.
            if let indexPtr = index, selectedIdx >= 0 {
                indexPtr.pointee = selectedIdx
            }
            return results
        }
    }
}

/// Inline scene file editor panel — the left panel's "Scene file" tab.
///
/// RESTYLE NOTE (RISE UI redesign): header/toolbar/status-bar chrome
/// restyled to the comp's "SCENE FILE TAB" using Theme.swift tokens.
/// Every dirty-tracking / save / save&reload / revert / autocomplete /
/// suggestion behavior below is unchanged from the pre-restyle version.
///
/// The pre-redesign header's "Close" button (which toggled
/// `viewModel.isEditorVisible`) is dropped here: that flag predates the
/// tab-based left panel (ContentView's `leftPanel` now switches on
/// `leftTab`, gated only by `showLeftPanel`) and no longer has any
/// visible effect — keeping a button that does nothing would be a fake
/// affordance.  The design comp likewise has no close control on this
/// tab (the Agent/Scene-file tab strip is the switch).
struct SceneEditorPanel: View {
    @EnvironmentObject var viewModel: RenderViewModel

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
            toolbar
            Rectangle().fill(Theme.borderHairline).frame(height: 1)

            SceneTextEditor(text: $viewModel.editorText,
                            revealRequest: viewModel.editorRevealRequest,
                            onSelectEntity: { offset in
                                viewModel.reverseSelectEntity(atByteOffset: UInt64(max(0, offset)))
                            },
                            canSelectEntity: { viewModel.canReverseSelect })
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            statusBar
        }
        .background(Theme.bgPanel)
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("Scene file")
                .font(Theme.sans(11, .semibold))
                .foregroundColor(Theme.textPrimary)

            if viewModel.isEditorDirty {
                HStack(spacing: 5) {
                    Circle().fill(Theme.dirty).frame(width: 6, height: 6)
                    Text("edited")
                        .font(Theme.sans(10))
                        .foregroundColor(Theme.dirty)
                }
            }

            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .frame(height: 34)
        .background(Theme.bgHeader)
    }

    // MARK: - Toolbar

    private var toolbar: some View {
        HStack(spacing: 8) {
            editorPill("Revert", disabled: !viewModel.isEditorDirty,
                       help: "Discard buffer edits and reload the current live scene "
                           + "(not just the last-saved-to-disk version)") {
                viewModel.revertEditorFile()
            }

            // P1-3 fix: no keyboard shortcut here.  ⌘S was bound both
            // here AND on File > "Save Rendered Image…" (RISEApp.swift);
            // the responder chain resolves this pill's shortcut FIRST,
            // so ⌘S silently overwrote the loaded .RISEscene with the
            // raw editor buffer instead of saving the rendered image the
            // menu item's label promises.  The pill stays clickable —
            // only the keyboard binding is gone.
            editorPill("Save", disabled: !viewModel.isEditorDirty,
                       help: "Save changes to disk") {
                viewModel.saveEditorFile()
            }

            editorPill("Save & Reload",
                       disabled: viewModel.renderState == .cancelling
                                 || viewModel.renderState == .loading,
                       help: "Save to disk, clear the loaded scene, and reload from disk") {
                viewModel.saveAndReloadScene()
            }

            Spacer(minLength: 0)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
        .background(Theme.bgHeader)
    }

    /// A compact bordered pill button matching the comp's toolbar
    /// affordances (Revert / Save / Save & Reload): hairline border by
    /// default, brightening on hover.  The keyboard shortcut, when
    /// given, is applied directly to the underlying `Button` (inside
    /// `EditorPillButton`) rather than to this wrapper view — matching
    /// the pre-restyle code's pattern, since `.keyboardShortcut` needs
    /// to sit on the actual control to bind reliably.
    private func editorPill(_ title: String, disabled: Bool, help: String,
                             shortcutKey: KeyEquivalent? = nil,
                             shortcutModifiers: EventModifiers = .command,
                             action: @escaping () -> Void) -> some View {
        EditorPillButton(title: title, disabled: disabled,
                          shortcutKey: shortcutKey, shortcutModifiers: shortcutModifiers,
                          action: action)
            .help(help)
    }

    // MARK: - Status bar

    /// Honest client-side stats only — no "parsed · 0 errors" claim,
    /// since there is no live parse surface wired to this editor yet
    /// (see the slice brief).  Save state mirrors the dirty badge in
    /// the header so it reads correctly even when the header scrolls
    /// out of view on a very short window.
    ///
    /// CST <-> scene-file live sync (item 1): `editorBehindLiveScene`
    /// only ever goes true alongside `isEditorDirty` (the poll can't set
    /// it while the buffer is clean — see `pollRefinementState`), so the
    /// amber warning appears BESIDE "unsaved edits" rather than
    /// replacing it — both facts are true at once: the buffer has
    /// unsaved text AND the live scene has since moved on elsewhere
    /// (agent edit, GUI edit, ...).
    private var statusBar: some View {
        HStack(spacing: 12) {
            if viewModel.isEditorDirty {
                Text("● unsaved edits")
                    .foregroundColor(Theme.dirty)
            } else {
                Text("✓ saved")
                    .foregroundColor(Theme.success)
            }
            if viewModel.editorBehindLiveScene {
                Text("⚠ scene changed elsewhere — buffer is stale")
                    .foregroundColor(Theme.warn)
            }
            Spacer(minLength: 0)
            Text(bufferStats)
                .foregroundColor(Theme.textDim)
        }
        .font(Theme.mono(10.5))
        .padding(.horizontal, 12)
        .frame(height: 32)
        .background(Theme.bgHeader)
        .overlay(alignment: .top) {
            Rectangle().fill(Theme.borderHairline).frame(height: 1)
        }
    }

    private var bufferStats: String {
        let text = viewModel.editorText
        let chars = text.count
        // Line count = newline count + 1 for the trailing (possibly
        // empty) line, matching how editors conventionally report it;
        // an empty buffer reads as 0 lines / 0 chars rather than 1.
        let lines = text.isEmpty ? 0 : text.reduce(1) { $0 + ($1 == "\n" ? 1 : 0) }
        return "\(chars) chars · \(lines) lines"
    }
}

/// Hover-brightening bordered pill button shared by the scene-editor
/// toolbar.  SwiftUI has no built-in hover-state border, so this tracks
/// it with a plain `@State` + `.onHover`.
private struct EditorPillButton: View {
    let title: String
    let disabled: Bool
    var shortcutKey: KeyEquivalent? = nil
    var shortcutModifiers: EventModifiers = .command
    let action: () -> Void
    @State private var isHovered = false

    var body: some View {
        Group {
            if let shortcutKey {
                button.keyboardShortcut(shortcutKey, modifiers: shortcutModifiers)
            } else {
                button
            }
        }
    }

    private var button: some View {
        Button(action: action) {
            Text(title)
                .font(Theme.sans(11))
                .padding(.horizontal, 11)
                .padding(.vertical, 5)
        }
        .buttonStyle(.plain)
        .foregroundColor(disabled ? Theme.textDisabled : Theme.textTertiary)
        .overlay(
            RoundedRectangle(cornerRadius: Theme.radiusMedium)
                .stroke(isHovered && !disabled ? Theme.borderHover : Theme.borderStrong, lineWidth: 1)
        )
        .disabled(disabled)
        .onHover { isHovered = $0 }
    }
}
