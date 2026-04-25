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

        let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.font = font
        textView.typingAttributes = [
            .font: font,
            .foregroundColor: NSColor.labelColor,
        ]

        // Install the syntax highlighter as the text storage delegate.
        let highlighter = RISESceneSyntaxHighlighter()
        textView.textStorage?.delegate = highlighter
        context.coordinator.highlighter = highlighter

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

        // Defer initial text population until after the view is fully
        // installed in the window's view hierarchy, avoiding the
        // "layoutSubtreeIfNeeded called during layout" recursion.
        DispatchQueue.main.async {
            textView.string = self.text
        }

        return scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        guard let textView = context.coordinator.textView,
              textView.string != text else { return }
        let selectedRanges = textView.selectedRanges
        textView.string = text
        textView.selectedRanges = selectedRanges
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
            let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
            textView.typingAttributes = [
                .font: font,
                .foregroundColor: NSColor.labelColor,
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

/// Inline scene file editor panel that slides in from the left.
struct SceneEditorPanel: View {
    @EnvironmentObject var viewModel: RenderViewModel

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Scene Editor")
                    .font(.headline)

                if viewModel.isEditorDirty {
                    Text("Modified")
                        .font(.caption)
                        .foregroundColor(.orange)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(Color.orange.opacity(0.15), in: RoundedRectangle(cornerRadius: 4))
                }

                Spacer()

                Button {
                    withAnimation(.easeInOut(duration: 0.25)) {
                        viewModel.isEditorVisible = false
                    }
                } label: {
                    Label("Close", systemImage: "sidebar.left")
                }
                .help("Close editor")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            // Toolbar
            HStack(spacing: 8) {
                Button {
                    viewModel.revertEditorFile()
                } label: {
                    Label("Revert", systemImage: "arrow.uturn.backward")
                }
                .disabled(!viewModel.isEditorDirty)
                .help("Revert all changes to the last saved version")

                Button {
                    viewModel.saveEditorFile()
                } label: {
                    Label("Save", systemImage: "square.and.arrow.down")
                }
                .disabled(!viewModel.isEditorDirty)
                .keyboardShortcut("s", modifiers: .command)
                .help("Save changes to disk")

                Button {
                    viewModel.saveAndReloadScene()
                } label: {
                    Label("Save & Reload", systemImage: "arrow.clockwise")
                }
                .disabled(viewModel.renderState == .cancelling || viewModel.renderState == .loading)
                .help("Save to disk, clear the loaded scene, and reload from disk")

                Spacer()
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            SceneTextEditor(text: $viewModel.editorText)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
