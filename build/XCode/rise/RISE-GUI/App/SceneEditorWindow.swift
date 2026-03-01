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
        let scrollView = NSTextView.scrollableTextView()
        guard let textView = scrollView.documentView as? NSTextView else {
            return scrollView
        }

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
        textView.isHorizontallyResizable = false
        textView.isVerticallyResizable = true
        textView.textContainer?.widthTracksTextView = true

        textView.delegate = context.coordinator
        context.coordinator.textView = textView

        // Defer initial text population until after the view is fully
        // installed in the window's view hierarchy, avoiding the
        // "layoutSubtreeIfNeeded called during layout" recursion.
        DispatchQueue.main.async {
            textView.string = self.text
        }

        return scrollView
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        let coordinator = context.coordinator
        // Skip updates that originate from the user typing (the coordinator
        // already wrote the binding). Only apply external binding changes.
        guard !coordinator.isUpdatingFromTextView else { return }

        let newText = text
        DispatchQueue.main.async {
            guard let textView = coordinator.textView,
                  textView.string != newText else { return }
            let selectedRanges = textView.selectedRanges
            textView.string = newText
            textView.selectedRanges = selectedRanges
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(text: $text)
    }

    class Coordinator: NSObject, NSTextViewDelegate {
        var text: Binding<String>
        weak var textView: NSTextView?
        var isUpdatingFromTextView = false
        /// Strong reference to keep the syntax highlighter alive.
        var highlighter: RISESceneSyntaxHighlighter?

        init(text: Binding<String>) {
            self.text = text
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else { return }
            isUpdatingFromTextView = true
            text.wrappedValue = textView.string
            isUpdatingFromTextView = false

            // Reset typing attributes so new keystrokes use default styling
            // rather than inheriting the color of the character at the cursor.
            let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
            textView.typingAttributes = [
                .font: font,
                .foregroundColor: NSColor.labelColor,
            ]
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
