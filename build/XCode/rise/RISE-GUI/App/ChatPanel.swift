import SwiftUI

// Facet 5 slice B2 — the LLM chat panel.
//
// The user types intent ("make the orange things red"); an LLM
// (Anthropic or Gemini) drives the seven agent verbs as tools against
// the LIVE scene through the viewport bridge's agentHandleLine — the
// same dispatcher the Agent (JSON-RPC) debug panel drives by hand.
// All state + the turn driver live in ChatViewModel; this file is
// presentation only.

/// The chat panel body, shown inline in the Controls column (mirrors
/// the Agent debug panel's placement + show/hide pattern).
struct ChatPanel: View {
    @EnvironmentObject var viewModel: RenderViewModel
    @ObservedObject var chat: ChatViewModel
    @State private var showSettings = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            // Header: title + provider/model summary + settings gear.
            HStack {
                Image(systemName: "bubble.left.and.bubble.right")
                    .imageScale(.small)
                Text("Chat")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                Spacer()
                Text("\(chat.provider.displayName) · \(chat.modelId)")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape")
                        .imageScale(.small)
                }
                .buttonStyle(.borderless)
                .help("Provider, model, and API-key settings")
                .popover(isPresented: $showSettings, arrowEdge: .trailing) {
                    ChatSettingsView(chat: chat)
                }
            }

            // Transcript.
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        if chat.transcript.isEmpty {
                            Text("Describe a scene change — e.g. “make the "
                                 + "orange objects red”.")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                        ForEach(chat.transcript) { entry in
                            ChatTranscriptRow(entry: entry)
                                .id(entry.id)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(6)
                }
                .frame(height: 200)
                .overlay(
                    RoundedRectangle(cornerRadius: 4)
                        .stroke(Color(nsColor: .separatorColor))
                )
                .onChange(of: chat.transcript.count) { _, _ in
                    if let last = chat.transcript.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }

            // Error affordances.  Retry appears only for the retriable
            // (Http-kind / network) failures; Reset after repeated
            // HTTP 400s (honest poison scoping).  MaxTokens and
            // IterationCap deliberately get NO retry button — their
            // recovery is a new user message.
            if chat.retryAvailable || chat.resetOffered {
                HStack(spacing: 8) {
                    if chat.retryAvailable {
                        Button {
                            chat.retry()
                        } label: {
                            Label("Retry", systemImage: "arrow.clockwise")
                        }
                        .disabled(chat.isBusy || !viewModel.isSceneEditableForAgents)
                        .help("Retry the failed request — your message is kept and replays; no duplicate is recorded")
                    }
                    if chat.resetOffered {
                        Button(role: .destructive) {
                            chat.resetConversation()
                        } label: {
                            Label("Reset conversation", systemImage: "trash")
                        }
                        .disabled(chat.isBusy)
                        .help("Drop the conversation history (keeps provider / model / keys)")
                    }
                    Spacer()
                }
            }

            // Input row.  Send / typing are additionally gated on the
            // scene being editable (B2 review round 1): during a
            // production render, tool calls would mutate Scene state
            // the render workers read off-main.  The driver enforces
            // the same predicate; this is the visible layer.
            HStack(spacing: 6) {
                TextField("Ask for a scene change…", text: $chat.inputText)
                    .textFieldStyle(.roundedBorder)
                    .font(.caption)
                    .onSubmit { chat.send() }
                    .disabled(chat.isBusy || viewModel.viewportBridge == nil
                              || !viewModel.isSceneEditableForAgents)
                if chat.isBusy {
                    Button {
                        chat.requestStop()
                    } label: {
                        Label("Stop", systemImage: "stop.fill")
                    }
                    .help("Stop now — aborts an in-flight request; pending "
                          + "tool calls are reported to the model as cancelled")
                    ProgressView()
                        .controlSize(.small)
                } else {
                    Button {
                        chat.send()
                    } label: {
                        Label("Send", systemImage: "paperplane.fill")
                    }
                    .disabled(viewModel.viewportBridge == nil
                              || !viewModel.isSceneEditableForAgents
                              || chat.inputText.trimmingCharacters(
                                     in: .whitespacesAndNewlines).isEmpty)
                    .help(viewModel.isSceneEditableForAgents
                          ? "Send this request to the model"
                          : "Chat is disabled while a production render is "
                            + "running — wait for it to finish")
                }
            }
        }
        .padding(.top, 4)
    }
}

/// One transcript row, styled by kind.
private struct ChatTranscriptRow: View {
    let entry: ChatViewModel.Entry

    var body: some View {
        switch entry.kind {
        case .user:
            Text("\(Text("You").bold().foregroundColor(.accentColor))  \(entry.text)")
                .font(.caption)
                .textSelection(.enabled)
        case .assistant:
            Text(entry.text)
                .font(.caption)
                .textSelection(.enabled)
        case .toolActivity:
            Text(entry.text)
                .font(.system(.caption2, design: .monospaced))
                .foregroundColor(.secondary)
        case .error:
            Text("\(Image(systemName: "exclamationmark.triangle"))  \(entry.text)")
                .font(.caption)
                .foregroundColor(.orange)
                .textSelection(.enabled)
        case .notice:
            Text(entry.text)
                .font(.caption)
                .italic()
                .foregroundColor(.secondary)
        }
    }
}

/// Provider / model / API-key settings popover.
///
/// Provider + model apply together via the Apply button; when a
/// conversation exists, applying CONFIRMS first because switching
/// resets the transcript (assistant turns are provider-native JSON
/// and cannot cross providers — the C++ loop's documented rule, which
/// also holds when re-selecting the same provider).
///
/// The API key is written to the macOS Keychain (service "RISE Agent
/// Chat", account = provider name).  The field is write-only: it
/// never displays the stored key, and nothing here logs it.  At use
/// time resolution is Keychain first, then the provider's environment
/// variable — the caption below shows WHICH source is active.
private struct ChatSettingsView: View {
    @ObservedObject var chat: ChatViewModel

    @State private var draftProvider: AgentChatProviderChoice = .anthropic
    @State private var draftModelId: String = ""
    @State private var draftKey: String = ""
    @State private var confirmSwitch = false
    @State private var keySaveNotice: String? = nil

    /// The picker shows/edits `draftProvider`, but the running chat
    /// (resolveApiKey, the turn driver) always talks to
    /// `chat.provider` — the last APPLIED selection.  Saving a key
    /// while these differ is common (pick Gemini, paste its key,
    /// Save) and entirely valid — key ops intentionally follow the
    /// picker, not Apply — but the chat itself keeps using the old
    /// provider until Apply is pressed.  Surface that explicitly so
    /// "I saved a Gemini key and chat still isn't using it" reads as
    /// "click Apply" rather than "the save didn't work".
    private var draftDiffersFromApplied: Bool { draftProvider != chat.provider }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Chat Settings")
                .font(.caption)
                .fontWeight(.semibold)
                .foregroundColor(.secondary)

            Picker("Provider", selection: $draftProvider) {
                ForEach(AgentChatProviderChoice.allCases) { p in
                    Text(p.displayName).tag(p)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .onChange(of: draftProvider) { _, newProvider in
                // Follow the picker with the provider's own stored
                // model id (each provider remembers its own).
                draftModelId = chat.storedModelId(for: newProvider)
                draftKey = ""
                keySaveNotice = nil
            }

            VStack(alignment: .leading, spacing: 2) {
                Text("Model")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                TextField(draftProvider.defaultModelId, text: $draftModelId)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.caption, design: .monospaced))
                Text("Blank uses the default (\(draftProvider.defaultModelId)).")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            HStack {
                Button("Apply") {
                    if chat.hasConversation { confirmSwitch = true }
                    else { applySelection() }
                }
                .help("Apply provider + model.  Switching resets the conversation.")
                Spacer()
            }
            .alert("Switch provider or model?", isPresented: $confirmSwitch) {
                Button("Switch & Reset", role: .destructive) { applySelection() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("Conversation history cannot cross providers or models — "
                     + "applying resets the chat.")
            }

            if draftDiffersFromApplied {
                Text("\(Image(systemName: "arrow.uturn.left")) Chat is currently "
                     + "using \(chat.provider.displayName) — click Apply above to "
                     + "switch it to \(draftProvider.displayName).")
                    .font(.caption2)
                    .foregroundColor(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider()

            VStack(alignment: .leading, spacing: 2) {
                Text("API key — \(draftProvider.displayName)")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                SecureField("Paste key (stored in Keychain)", text: $draftKey)
                    .textFieldStyle(.roundedBorder)
                    .font(.caption)
                HStack {
                    Button("Save Key") {
                        switch chat.saveApiKey(draftKey, for: draftProvider) {
                        case .saved:
                            keySaveNotice = nil
                            draftKey = ""
                        case .emptyInput:
                            // No-op, not a delete — Clear Key owns
                            // deletion.  Hint instead of silently
                            // doing nothing.
                            keySaveNotice = "Enter a key first, or use Clear Key to remove one."
                        case .keychainError:
                            keySaveNotice = "Keychain write failed."
                        }
                    }
                    .disabled(draftKey.trimmingCharacters(
                        in: .whitespacesAndNewlines).isEmpty)
                    .help("Store in the macOS Keychain (service “RISE Agent Chat”) "
                          + "for \(draftProvider.displayName)")
                    Button("Clear Key") {
                        keySaveNotice = chat.clearApiKey(for: draftProvider)
                            ? nil : "Keychain delete failed."
                        draftKey = ""
                    }
                    .disabled(chat.keySource(for: draftProvider) != .keychain)
                    .help("Delete this provider's key from the Keychain")
                    Spacer()
                }
                if let notice = keySaveNotice {
                    Text(notice)
                        .font(.caption2)
                        .foregroundColor(.orange)
                }
                // Which source would be used for THIS provider (the
                // one shown above / being edited here) RIGHT NOW —
                // never the key itself.  Resolution: Keychain first,
                // then each environment fallback in order.  Recomputed
                // on every keyStateEpoch bump (save/clear) so it never
                // lags a successful write.
                Text("Active source: \(chat.keySource(for: draftProvider).label)"
                     + " · Keychain first, then "
                     + "\(draftProvider.apiKeyEnvVars.joined(separator: " or ")).")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .id(chat.keyStateEpoch)
            }

            Divider()

            Button(role: .destructive) {
                chat.resetConversation()
            } label: {
                Label("Reset conversation", systemImage: "trash")
            }
            .disabled(!chat.hasConversation)
            .help("Drop the conversation history (keeps provider / model / keys)")
        }
        .padding(12)
        .frame(width: 300)
        .onAppear {
            draftProvider = chat.provider
            draftModelId = chat.storedModelId(for: chat.provider)
            draftKey = ""
            keySaveNotice = nil
        }
    }

    private func applySelection() {
        chat.applyProviderSelection(draftProvider, modelId: draftModelId)
    }
}
