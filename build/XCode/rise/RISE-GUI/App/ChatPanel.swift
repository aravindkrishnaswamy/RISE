import SwiftUI
import UniformTypeIdentifiers
import Combine

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
    /// True while a valid file drag hovers the input row — drives the
    /// highlight border below so a drop target is visually obvious
    /// (cheap `.onDrop(isTargeted:)` binding; no new pipeline).
    @State private var isDropTargeted = false

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

            // Secure-MCP slice 5c: pending proposals from an EXTERNAL
            // agent (staged via the GUI-hosted MCP endpoint above) that
            // need your approval before they touch the scene.  Present
            // regardless of whether hosting is CURRENTLY on — a
            // proposal already staged stays visible/resolvable even
            // after the toggle is switched off (the queue lives on the
            // controller, not on the hosted-server connection).
            ProposalsPanel(chat: chat)

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

            // Attachment error banner (rejected mimeType / unreadable
            // file) — a user-visible message, never a silent drop.
            if let error = chat.attachmentError {
                HStack(spacing: 4) {
                    Image(systemName: "exclamationmark.triangle")
                    Text(error)
                    Spacer()
                    Button {
                        chat.attachmentError = nil
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .buttonStyle(.borderless)
                }
                .font(.caption2)
                .foregroundColor(.orange)
            }

            // Pending-attachment thumbnail strip (Model-B F5 chat image
            // attachments) — shows what will go out with the NEXT send.
            if !chat.pendingAttachments.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(chat.pendingAttachments) { attachment in
                            ZStack(alignment: .topTrailing) {
                                Image(nsImage: attachment.thumbnail)
                                    .resizable()
                                    .aspectRatio(contentMode: .fill)
                                    .frame(width: 40, height: 40)
                                    .clipShape(RoundedRectangle(cornerRadius: 4))
                                Button {
                                    chat.removePendingAttachment(attachment.id)
                                } label: {
                                    Image(systemName: "xmark.circle.fill")
                                        .foregroundColor(.white)
                                        .background(Circle().fill(Color.black.opacity(0.6)))
                                }
                                .buttonStyle(.borderless)
                                .offset(x: 4, y: -4)
                            }
                        }
                    }
                    .padding(.vertical, 2)
                }
            }

            // Input row.  Send / typing are additionally gated on the
            // scene being editable (B2 review round 1): during a
            // production render, tool calls would mutate Scene state
            // the render workers read off-main.  The driver enforces
            // the same predicate; this is the visible layer.  Drag-and-
            // drop an image file straight onto this row attaches it —
            // the same accept/downscale/cap pipeline as the picker
            // button.
            HStack(spacing: 6) {
                Button {
                    chat.attachImageFiles()
                } label: {
                    Image(systemName: "paperclip")
                }
                .buttonStyle(.borderless)
                .disabled(chat.isBusy || viewModel.viewportBridge == nil
                          || !viewModel.isSceneEditableForAgents
                          || chat.pendingAttachments.count >= RISEAgentChatBridge.maxLiveUserImages)
                .help("Attach reference image(s) — up to "
                      + "\(RISEAgentChatBridge.maxLiveUserImages) per message")

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
                              || (chat.inputText.trimmingCharacters(
                                     in: .whitespacesAndNewlines).isEmpty
                                  && chat.pendingAttachments.isEmpty))
                    .help(viewModel.isSceneEditableForAgents
                          ? "Send this request to the model"
                          : "Chat is disabled while a production render is "
                            + "running — wait for it to finish")
                }
            }
            .padding(4)
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(isDropTargeted ? Color.accentColor : Color.clear, lineWidth: 2)
            )
            .onDrop(of: [.fileURL], isTargeted: $isDropTargeted) { providers in
                Self.loadDroppedURLs(providers) { urls in
                    chat.handleDroppedFileURLs(urls)
                }
                return true
            }
        }
        .padding(.top, 4)
    }

    /// Resolve a drop's NSItemProviders to file URLs off-main (the
    /// provider loading API is completion-handler based, not async/
    /// await), then hop back to main to hand them to the view model —
    /// ChatViewModel is @MainActor, like the rest of this file's state.
    private static func loadDroppedURLs(
        _ providers: [NSItemProvider], completion: @escaping ([URL]) -> Void
    ) {
        let group = DispatchGroup()
        var urls: [URL] = []
        let lock = NSLock()
        for provider in providers {
            guard provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) else { continue }
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier) { item, _ in
                defer { group.leave() }
                var url: URL?
                if let data = item as? Data {
                    url = URL(dataRepresentation: data, relativeTo: nil)
                } else if let u = item as? URL {
                    url = u
                }
                if let url {
                    lock.lock()
                    urls.append(url)
                    lock.unlock()
                }
            }
        }
        group.notify(queue: .main) {
            completion(urls)
        }
    }
}

/// Secure-MCP slice 5c: the pending-proposals list — an external MCP
/// client's staged edits, approved/rejected here through the OWNER
/// dispatcher (ChatViewModel.resolveProposal, NOT the external server).
/// A disclosure group so it stays out of the way when empty/collapsed;
/// auto-expands the FIRST time a proposal appears so a new arrival is
/// never missed silently.
private struct ProposalsPanel: View {
    @ObservedObject var chat: ChatViewModel
    @State private var isExpanded = false
    @State private var hasAutoExpandedOnce = false

    /// Poll cadence while the section is visible (expanded) — cheap
    /// (a single synchronous in-process HandleLine call against the
    /// controller's in-memory queue), so a sub-second interval is fine;
    /// matches the RenderViewModel Timer pattern used elsewhere in this
    /// app rather than a Combine/async-sleep loop.
    private static let pollInterval: TimeInterval = 1.0

    var body: some View {
        DisclosureGroup(isExpanded: $isExpanded) {
            VStack(alignment: .leading, spacing: 4) {
                if chat.pendingProposals.isEmpty {
                    Text("No proposals.")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                } else {
                    ForEach(chat.pendingProposals) { proposal in
                        ProposalRow(chat: chat, proposal: proposal)
                    }
                }
            }
            .padding(.top, 2)
            .onAppear { chat.refreshProposals() }
            .onReceive(Timer.publish(every: Self.pollInterval, on: .main, in: .common).autoconnect()) { _ in
                chat.refreshProposals()
            }
        } label: {
            HStack(spacing: 4) {
                Image(systemName: "tray.and.arrow.down")
                    .imageScale(.small)
                Text("Proposals")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)
                let pendingCount = chat.pendingProposals.filter { $0.status == "pending" }.count
                if pendingCount > 0 {
                    Text("\(pendingCount)")
                        .font(.caption2)
                        .fontWeight(.bold)
                        .foregroundColor(.white)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 1)
                        .background(Capsule().fill(Color.accentColor))
                }
                Spacer()
            }
        }
        .font(.caption)
        // Always refresh once when the panel first appears (even
        // collapsed) so the pending-count badge is accurate without
        // requiring the user to expand it first.
        .onAppear { chat.refreshProposals() }
        .onChange(of: chat.pendingProposals.count) { _, newCount in
            if newCount > 0 && !hasAutoExpandedOnce {
                isExpanded = true
                hasAutoExpandedOnce = true
            }
        }
    }
}

/// One proposal row: a summary line + Approve/Reject (pending) or a
/// terminal status label (applied/rejected/conflict, shown briefly —
/// see ChatViewModel.refreshProposals' linger window).
private struct ProposalRow: View {
    @ObservedObject var chat: ChatViewModel
    let proposal: ChatViewModel.ProposalEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 4) {
                Text(proposal.kind)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundColor(.secondary)
                if !proposal.sessionLabel.isEmpty {
                    Text("· \(proposal.sessionLabel)")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                }
                Spacer()
                statusLabel
            }
            Text(proposal.summary)
                .font(.caption)
                .textSelection(.enabled)
                .lineLimit(2)
            if proposal.status == "pending" {
                HStack(spacing: 8) {
                    Button {
                        chat.resolveProposal(id: proposal.id, approve: true)
                    } label: {
                        Label("Approve", systemImage: "checkmark")
                    }
                    .help("Apply this edit to the live scene")
                    Button(role: .destructive) {
                        chat.resolveProposal(id: proposal.id, approve: false)
                    } label: {
                        Label("Reject", systemImage: "xmark")
                    }
                    .help("Discard this proposal without applying it")
                    Spacer()
                }
            }
        }
        .padding(6)
        .background(
            RoundedRectangle(cornerRadius: 4)
                .fill(Color(nsColor: .controlBackgroundColor))
        )
    }

    @ViewBuilder
    private var statusLabel: some View {
        switch proposal.status {
        case "applied":
            Label("Applied", systemImage: "checkmark.circle.fill")
                .foregroundColor(.green)
                .font(.caption2)
        case "rejected":
            Label("Rejected", systemImage: "xmark.circle.fill")
                .foregroundColor(.secondary)
                .font(.caption2)
        case "conflict":
            Label("Conflict", systemImage: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
                .font(.caption2)
        default:
            EmptyView()
        }
    }
}

/// One transcript row, styled by kind.
private struct ChatTranscriptRow: View {
    let entry: ChatViewModel.Entry

    var body: some View {
        switch entry.kind {
        case .user:
            VStack(alignment: .leading, spacing: 4) {
                if !entry.attachmentThumbnails.isEmpty {
                    HStack(spacing: 4) {
                        ForEach(Array(entry.attachmentThumbnails.enumerated()), id: \.offset) { _, thumb in
                            Image(nsImage: thumb)
                                .resizable()
                                .aspectRatio(contentMode: .fill)
                                .frame(width: 32, height: 32)
                                .clipShape(RoundedRectangle(cornerRadius: 3))
                        }
                    }
                }
                if !entry.text.isEmpty {
                    Text("\(Text("You").bold().foregroundColor(.accentColor))  \(entry.text)")
                        .font(.caption)
                        .textSelection(.enabled)
                } else {
                    Text("You").bold().foregroundColor(.accentColor)
                        .font(.caption)
                }
            }
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
    @EnvironmentObject var viewModel: RenderViewModel
    @ObservedObject var chat: ChatViewModel

    @State private var draftProvider: AgentChatProviderChoice = .openai
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

            if draftProvider.requiresApiKey {
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
            } else {
                // .local is keyless by design — no key prompt, no
                // Keychain storage.  Show the resolved endpoint instead
                // so the user knows where the chat is pointing, plus a
                // hint that a local server has to actually be running.
                VStack(alignment: .leading, spacing: 2) {
                    Text("Endpoint — \(draftProvider.displayName)")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                    Text(AgentChatProviderChoice.localResolvedEndpoint)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                    Text("No API key needed. Requires a running local server, e.g. "
                         + "`ollama serve`. Override the endpoint with the "
                         + "RISE_LOCAL_LLM_BASE_URL environment variable before launch.")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            Divider()

            Button(role: .destructive) {
                chat.resetConversation()
            } label: {
                Label("Reset conversation", systemImage: "trash")
            }
            .disabled(!chat.hasConversation)
            .help("Drop the conversation history (keeps provider / model / keys)")

            Divider()

            // Eval-harness E1: chat trajectory recording (default ON).
            VStack(alignment: .leading, spacing: 4) {
                Text("Diagnostics")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                Toggle("Record chat trajectories", isOn: Binding(
                    get: { chat.recordTrajectories },
                    set: { newValue in chat.setRecordTrajectories(newValue) }
                ))
                .toggleStyle(.checkbox)
                .font(.caption)
                .help("Write a per-session JSONL log under "
                      + chat.trajectoryDirectory
                      + " (system prompt, each request/response, tool calls, "
                      + "tokens, timings).  API-key-shaped content is always "
                      + "redacted regardless of this setting.  Set "
                      + "RISE_TRAJECTORY_DIR to record elsewhere.")
            }

            Divider()

            // Secure-MCP slice 5c: the GUI-hosted external MCP endpoint.
            // Off by default (no server, no external access). When ON,
            // a SEPARATE process (a real MCP client — e.g. Claude Code
            // pointed at this URL) can connect over loopback HTTP and
            // PROPOSE edits into THIS scene; a human (you) still has to
            // Approve them in the Proposals section of the chat panel
            // before they touch the scene. Honest security framing: this
            // is loopback-only + a per-launch bearer token (the same
            // boundary `rise --agent-http` documents), not an internet-
            // facing auth system — anyone who can run a process on THIS
            // Mac as you could already edit the scene directly, so this
            // toggle's real exposure is "another program on my own
            // machine can suggest edits", not "the internet can reach my
            // scene".
            VStack(alignment: .leading, spacing: 4) {
                Text("External agent (MCP) connections")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                Toggle("Allow external agent (MCP) connections", isOn: Binding(
                    get: { chat.isHostingExternalAgent },
                    set: { newValue in
                        if newValue { chat.startExternalHosting() }
                        else { chat.stopExternalHosting() }
                    }
                ))
                .toggleStyle(.checkbox)
                .font(.caption)
                .disabled(viewModel.viewportBridge == nil)
                .help("A local MCP client (e.g. Claude Code) can connect and "
                      + "PROPOSE edits you approve below — it can never commit "
                      + "directly.")

                if let error = chat.hostedServerError {
                    Text(error)
                        .font(.caption2)
                        .foregroundColor(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }

                if chat.isHostingExternalAgent {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 4) {
                            Text("URL:")
                                .foregroundColor(.secondary)
                            Text("http://127.0.0.1:\(chat.hostedServerPort)/mcp")
                                .font(.system(.caption2, design: .monospaced))
                                .textSelection(.enabled)
                            Button {
                                Self.copyToPasteboard("http://127.0.0.1:\(chat.hostedServerPort)/mcp")
                            } label: {
                                Image(systemName: "doc.on.doc")
                            }
                            .buttonStyle(.borderless)
                            .help("Copy the URL")
                        }
                        HStack(spacing: 4) {
                            Text("Token:")
                                .foregroundColor(.secondary)
                            Text(chat.hostedServerToken)
                                .font(.system(.caption2, design: .monospaced))
                                .textSelection(.enabled)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Button {
                                Self.copyToPasteboard(chat.hostedServerToken)
                            } label: {
                                Image(systemName: "doc.on.doc")
                            }
                            .buttonStyle(.borderless)
                            .help("Copy the bearer token — paste it into your "
                                  + "MCP client's config. This token is not "
                                  + "logged anywhere; treat it like a password "
                                  + "for the duration this toggle stays on.")
                        }
                        Text("Every request must present this token via "
                             + "\u{201c}Authorization: Bearer <token>\u{201d}. "
                             + "A new token is generated each time you turn "
                             + "this on.")
                            .font(.caption2)
                            .foregroundColor(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(.leading, 2)
                }
            }

            Divider()

            // Developer: the Agent (JSON-RPC) panel is the raw-wire debug
            // surface against the LIVE controller (propose_patch etc. go
            // straight through agentHandleLine) — it predates and is now
            // superseded by this Chat panel for everyday use, but stays
            // available for wire-level debugging.  Off by default so it
            // doesn't confuse everyday users; the headless CLI agent
            // session is unaffected either way (separate process/session).
            VStack(alignment: .leading, spacing: 2) {
                Text("Developer")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                Toggle("Show raw JSON-RPC panel", isOn: $viewModel.showAgentDebugPanel)
                    .toggleStyle(.checkbox)
                    .font(.caption)
                    .help("Reveal the Agent (JSON-RPC) debug panel — the raw wire "
                          + "surface against the live controller. Superseded by "
                          + "Chat for everyday use.")
            }
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

    /// Secure-MCP slice 5c: copy the URL / bearer token to the general
    /// pasteboard. NEVER logs the value — this is the ONE place either
    /// value should ever leave `ChatViewModel`'s published state besides
    /// the read-only Text views above it.
    private static func copyToPasteboard(_ value: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(value, forType: .string)
    }
}
