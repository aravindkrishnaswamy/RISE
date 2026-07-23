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
//
// RESTYLE NOTE (RISE UI redesign, left panel "AGENT TAB" — see
// /docs/gui and the approved design comp): the tab chrome, transcript
// bubbles, tool-call trace chips, the diff card (now ProposalCard.swift)
// and the composer below were restyled to the comp using Theme.swift
// tokens.  Every disabled()/help()/onDrop/gating predicate below is
// unchanged from the pre-restyle version — this pass touches
// presentation only, never the underlying gates.  ChatSettingsView
// (provider/model/API-key popover) is unchanged in this pass; only its
// trigger affordance moved into the restyled composer footer.
struct ChatPanel: View {
    @EnvironmentObject var viewModel: RenderViewModel
    @ObservedObject var chat: ChatViewModel
    @State private var showSettings = false
    /// True while a valid file drag hovers the input row — drives the
    /// highlight border below so a drop target is visually obvious
    /// (cheap `.onDrop(isTargeted:)` binding; no new pipeline).
    @State private var isDropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            ProposalsPanel(chat: chat)
            transcript
            errorAffordances
            attachmentErrorBanner
            pendingAttachmentStrip
            composer
        }
    }

    // MARK: - Transcript

    private var transcript: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 15) {
                    if chat.transcript.isEmpty {
                        Text("Describe a scene change — e.g. “make the "
                             + "orange objects red”.")
                            .font(Theme.sans(12.5))
                            .foregroundColor(Theme.textDim)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.top, 24)
                    }
                    ForEach(chat.transcript) { entry in
                        ChatTranscriptRow(entry: entry)
                            .id(entry.id)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(14)
            }
            // The transcript is the FLEXIBLE element of the agent column:
            // it absorbs all remaining vertical space (agentTabBody gives
            // the panel the full column height) and scrolls internally.
            // The old `maxHeight: 420` cap is why the panel once occupied
            // only the top half of the column.
            .frame(maxWidth: .infinity, minHeight: 220, maxHeight: .infinity)
            .background(Theme.bgPanel)
            .onChange(of: chat.transcript.count) { _, _ in
                if let last = chat.transcript.last {
                    proxy.scrollTo(last.id, anchor: .bottom)
                }
            }
        }
    }

    // MARK: - Error affordances

    /// Retry appears only for the retriable (Http-kind / network)
    /// failures; Reset after repeated HTTP 400s (honest poison
    /// scoping).  MaxTokens and IterationCap deliberately get NO retry
    /// button — their recovery is a new user message.
    @ViewBuilder
    private var errorAffordances: some View {
        if chat.retryAvailable || chat.resetOffered {
            HStack(spacing: 8) {
                if chat.retryAvailable {
                    pillButton(
                        "↻ Retry", tint: Theme.accentLight,
                        disabled: chat.isBusy || !viewModel.isSceneEditableForAgents
                    ) { chat.retry() }
                    .help("Retry the failed request — your message is kept and replays; no duplicate is recorded")
                }
                if chat.resetOffered {
                    pillButton(
                        "⌫ Reset conversation", tint: Theme.error,
                        disabled: chat.isBusy
                    ) { chat.resetConversation() }
                    .help("Drop the conversation history (keeps provider / model / keys)")
                }
                Spacer(minLength: 0)
            }
        }
    }

    // MARK: - Attachment banner + pending strip

    @ViewBuilder
    private var attachmentErrorBanner: some View {
        if let error = chat.attachmentError {
            HStack(alignment: .top, spacing: 6) {
                Text("⚠").foregroundColor(Theme.warn)
                Text(error)
                    .font(Theme.sans(11))
                    .foregroundColor(Theme.warn)
                Spacer(minLength: 4)
                Button {
                    chat.attachmentError = nil
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundColor(Theme.textDim)
                }
                .buttonStyle(.plain)
            }
        }
    }

    /// Pending-attachment thumbnail strip (Model-B F5 chat image
    /// attachments) — shows what will go out with the NEXT send.
    @ViewBuilder
    private var pendingAttachmentStrip: some View {
        if !chat.pendingAttachments.isEmpty {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(chat.pendingAttachments) { attachment in
                        ZStack(alignment: .topTrailing) {
                            Image(nsImage: attachment.thumbnail)
                                .resizable()
                                .aspectRatio(contentMode: .fill)
                                .frame(width: 40, height: 40)
                                .clipShape(RoundedRectangle(cornerRadius: 5))
                            Button {
                                chat.removePendingAttachment(attachment.id)
                            } label: {
                                Image(systemName: "xmark.circle.fill")
                                    .foregroundColor(.white)
                                    .background(Circle().fill(Color.black.opacity(0.6)))
                            }
                            .buttonStyle(.plain)
                            .offset(x: 4, y: -4)
                        }
                    }
                }
                .padding(.vertical, 2)
            }
        }
    }

    // MARK: - Composer

    /// Input row (attach / text / send-or-stop) + a footer row with the
    /// provider/model chip, capability caption, and the settings gear —
    /// restyled to the comp's composer.  Send / typing are additionally
    /// gated on the scene being editable (B2 review round 1): during a
    /// production render, tool calls would mutate Scene state the
    /// render workers read off-main.  Drag-and-drop an image file
    /// straight onto the input row attaches it — the same accept/
    /// downscale/cap pipeline as the paperclip picker.
    private var composer: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 8) {
                Button {
                    chat.attachImageFiles()
                } label: {
                    Image(systemName: "paperclip")
                        .imageScale(.medium)
                }
                .buttonStyle(.plain)
                .foregroundColor(Theme.textFaint)
                .disabled(chat.isBusy || viewModel.viewportBridge == nil
                          || !viewModel.isSceneEditableForAgents
                          || chat.pendingAttachments.count >= RISEAgentChatBridge.maxLiveUserImages)
                .help("Attach reference image(s) — up to "
                      + "\(RISEAgentChatBridge.maxLiveUserImages) per message")

                TextField(
                    "", text: $chat.inputText,
                    prompt: Text("Ask the agent — reads, edits, validates, renders…")
                        .foregroundColor(Theme.textDim)
                )
                .textFieldStyle(.plain)
                .font(Theme.sans(12.5))
                .foregroundColor(Theme.textPrimary)
                .onSubmit { chat.send() }
                .disabled(chat.isBusy || viewModel.viewportBridge == nil
                          || !viewModel.isSceneEditableForAgents)

                if chat.isBusy {
                    ProgressView()
                        .controlSize(.small)
                        .scaleEffect(0.7)
                    Button {
                        chat.requestStop()
                    } label: {
                        Image(systemName: "stop.fill")
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(Theme.error)
                    .help("Stop now — aborts an in-flight request; pending "
                          + "tool calls are reported to the model as cancelled")
                } else {
                    Button {
                        chat.send()
                    } label: {
                        HStack(spacing: 5) {
                            Text("Send")
                            Text("⌘↵").foregroundColor(Theme.accentLight.opacity(0.65))
                        }
                        .font(Theme.sans(11.5, .semibold))
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                    }
                    .buttonStyle(.plain)
                    .foregroundColor(Theme.accentLight)
                    .background(Theme.accent.opacity(0.16), in: RoundedRectangle(cornerRadius: 7))
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
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .background(Theme.bgWell, in: RoundedRectangle(cornerRadius: 10))
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .stroke(isDropTargeted ? Theme.accent : Theme.borderLight,
                            lineWidth: isDropTargeted ? 2 : 1)
            )
            .onDrop(of: [.fileURL], isTargeted: $isDropTargeted) { providers in
                Self.loadDroppedURLs(providers) { urls in
                    chat.handleDroppedFileURLs(urls)
                }
                return true
            }

            composerFooter
        }
    }

    private var composerFooter: some View {
        HStack(spacing: 7) {
            Button {
                showSettings = true
            } label: {
                HStack(spacing: 4) {
                    Text("\(chat.provider.displayName) \(chat.modelId)")
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text("▾").foregroundColor(Theme.textGhost)
                }
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textFaint)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .overlay(RoundedRectangle(cornerRadius: 5).stroke(Theme.borderLight, lineWidth: 1))

            Text("sees scene + framebuffer")
                .foregroundColor(Theme.textDim)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .overlay(RoundedRectangle(cornerRadius: 5).stroke(Theme.borderLight, lineWidth: 1))

            Spacer(minLength: 4)

            autonomySelector

            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape")
            }
            .buttonStyle(.plain)
            .foregroundColor(Theme.textDim)
            .help("Provider, model, and API-key settings")
            .popover(isPresented: $showSettings, arrowEdge: .trailing) {
                ChatSettingsView(chat: chat)
            }
        }
        .font(Theme.mono(10.5))
    }

    // MARK: - Agent autonomy selector (2026-07 GUI composer chips)

    /// The comp's L0/L1/L2 chips — "Read" / "Propose" / "Apply".  Each
    /// level maps 1:1 to `RISEAgentAutonomyLevel` (RISEViewportBridge.h);
    /// the `.help` text states EXACTLY what the level does — see that
    /// header's doc for the underlying routing this description must stay
    /// truthful to.
    private var autonomySelector: some View {
        HStack(spacing: 4) {
            autonomyChip("Read", level: .read,
                         help: "Read: agent can read the scene and render, never edit.")
            autonomyChip("Propose", level: .propose,
                         help: "Propose: edits are staged as proposals for your review.")
            autonomyChip("Apply", level: .apply,
                         help: "Apply: edits apply directly — still one undo step each.")
        }
    }

    private func autonomyChip(_ title: String, level: RISEAgentAutonomyLevel,
                               help: String) -> some View {
        let isActive = chat.autonomyLevel == level
        return Button {
            chat.setAutonomyLevel(level)
        } label: {
            Text(title)
                .font(Theme.mono(10, isActive ? .semibold : .regular))
        }
        .buttonStyle(.plain)
        .foregroundColor(isActive ? Theme.accentLight : Theme.textDim)
        .help(help)
    }

    // MARK: - Shared pill button

    private func pillButton(_ title: String, tint: Color, disabled: Bool,
                             action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(Theme.sans(11))
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
        }
        .buttonStyle(.plain)
        .foregroundColor(disabled ? Theme.textDisabled : tint)
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.borderStrong, lineWidth: 1))
        .disabled(disabled)
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
/// Restyled per the redesign comp: proposals render inline as full diff
/// cards (ProposalCard.swift) rather than a collapsible summary list —
/// the comp shows the proposal as part of the natural conversation
/// flow, not tucked behind a disclosure toggle.  Present regardless of
/// whether external hosting is CURRENTLY on — a proposal already staged
/// stays visible/resolvable even after the toggle is switched off (the
/// queue lives on the controller, not on the hosted-server connection).
private struct ProposalsPanel: View {
    @ObservedObject var chat: ChatViewModel

    /// Poll cadence — cheap (a single synchronous in-process
    /// HandleLine call against the controller's in-memory queue), so a
    /// sub-second interval is fine; matches the RenderViewModel Timer
    /// pattern used elsewhere in this app rather than a Combine/async-
    /// sleep loop.
    private static let pollInterval: TimeInterval = 1.0

    var body: some View {
        Group {
            if !chat.pendingProposals.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(chat.pendingProposals) { proposal in
                        VStack(alignment: .leading, spacing: 6) {
                            ProposalDiffCard(chat: chat, proposal: proposal)
                            if proposal.status == "pending" {
                                HStack(spacing: 6) {
                                    Text("↺").foregroundColor(Theme.accentSoft)
                                    Text("joins the shared undo history — one step")
                                        .foregroundColor(Theme.textDim)
                                }
                                .font(Theme.mono(10.5))
                            }
                        }
                    }
                }
            }
        }
        .onAppear { chat.refreshProposals() }
        .onReceive(Timer.publish(every: Self.pollInterval, on: .main, in: .common).autoconnect()) { _ in
            chat.refreshProposals()
        }
    }
}

/// One transcript row, styled by kind per the redesign comp:
///   - `.user`: a right-aligned bubble (Theme.bgBubbleUser).
///   - `.assistant`: plain left-aligned narration text.
///   - `.toolActivity`: a compact hairline-bordered "trace chip".
///   - `.error` / `.notice`: unchanged in spirit, restyled to tokens.
private struct ChatTranscriptRow: View {
    let entry: ChatViewModel.Entry

    /// GUI stage 2: whether THIS row's disclosure (a `.thinking` row,
    /// or a `.toolActivity` row that has `detailText`) is currently
    /// expanded — seeded once from `entry.isExpandedByDefault` at
    /// row-init time (see the custom `init` below), then purely local
    /// UI state the user can toggle either way afterward.  Unused for
    /// every other row kind.
    @State private var isExpanded: Bool

    init(entry: ChatViewModel.Entry) {
        self.entry = entry
        _isExpanded = State(initialValue: entry.isExpandedByDefault)
    }

    /// Matches the comp's "max-width:86%" bubble constraint against
    /// the fixed 404pt left-panel width (404 − 2×14 padding ≈ 376pt
    /// content column; 86% of that ≈ 320pt).
    private static let userBubbleMaxWidth: CGFloat = 320

    var body: some View {
        switch entry.kind {
        case .user:
            userBubble
        case .assistant:
            assistantText
        case .toolActivity:
            toolActivityRow
        case .thinking:
            thinkingRow
        case .error:
            errorRow
        case .notice:
            noticeRow
        }
    }

    private var userBubble: some View {
        VStack(alignment: .trailing, spacing: 6) {
            if !entry.attachmentThumbnails.isEmpty {
                HStack(spacing: 4) {
                    ForEach(Array(entry.attachmentThumbnails.enumerated()), id: \.offset) { _, thumb in
                        Image(nsImage: thumb)
                            .resizable()
                            .aspectRatio(contentMode: .fill)
                            .frame(width: 32, height: 32)
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                    }
                }
            }
            if !entry.text.isEmpty {
                Text(entry.text)
                    .font(Theme.sans(12.5))
                    .foregroundColor(Theme.textPrimary)
                    .lineSpacing(3)
                    .textSelection(.enabled)
                    .padding(.horizontal, 13)
                    .padding(.vertical, 10)
                    .background(Theme.bgBubbleUser)
                    .clipShape(UnevenRoundedRectangle(
                        topLeadingRadius: 12, bottomLeadingRadius: 12,
                        bottomTrailingRadius: 4, topTrailingRadius: 12))
                    .frame(maxWidth: Self.userBubbleMaxWidth, alignment: .trailing)
            }
        }
        .frame(maxWidth: .infinity, alignment: .trailing)
    }

    private var assistantText: some View {
        Text(entry.text)
            .font(Theme.sans(12.5))
            .foregroundColor(Theme.textSecondary)
            .lineSpacing(6)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// The comp's compact "trace chip" label with the leading "→ "
    /// stripped — shared by the plain chip and the disclosure variant
    /// below so both read identically at a glance.
    private var traceChipLabel: String {
        entry.text.hasPrefix("→ ") ? String(entry.text.dropFirst(2)) : entry.text
    }

    /// The comp's compact "trace chip" — one per tool call.  The comp
    /// merges several sequential verbs into a single chip with a
    /// grammar-version/error-count trailer ("grammar v2.3 · 0 errors");
    /// ChatViewModel records one `.toolActivity` entry per call and
    /// exposes no grammar/error-count data, so each tool call renders
    /// its own chip here instead of a fabricated merged trailer.
    private var traceChip: some View {
        HStack(spacing: 7) {
            Text("✓").foregroundColor(Theme.success)
            Text(traceChipLabel)
            Spacer(minLength: 0)
        }
        .font(Theme.mono(10.5))
        .foregroundColor(Theme.textMuted)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .overlay(RoundedRectangle(cornerRadius: 7).stroke(Theme.borderHairline, lineWidth: 1))
    }

    /// GUI stage 2: the trace chip PLUS an expandable disclosure once
    /// the call's result has landed and `entry.detailText` (args +
    /// result, capped) is set — before that (the brief dispatch-time
    /// window, or a call the driver abandoned mid-turn) this is
    /// byte-identical to the plain `traceChip` above: SAME one-line
    /// density, no new always-visible chrome.
    private var toolActivityRow: some View {
        VStack(alignment: .leading, spacing: 5) {
            if let detail = entry.detailText, !detail.isEmpty {
                Button {
                    isExpanded.toggle()
                } label: {
                    HStack(spacing: 7) {
                        Text("✓").foregroundColor(Theme.success)
                        Text(traceChipLabel)
                        Spacer(minLength: 0)
                        Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
                            .font(.system(size: 8))
                    }
                    .font(Theme.mono(10.5))
                    .foregroundColor(Theme.textMuted)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .overlay(RoundedRectangle(cornerRadius: 7).stroke(Theme.borderHairline, lineWidth: 1))
                }
                .buttonStyle(.plain)

                if isExpanded {
                    Text(detail)
                        .font(Theme.mono(9.5))
                        .foregroundColor(Theme.textMuted)
                        .textSelection(.enabled)
                        .padding(8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Theme.bgPanel, in: RoundedRectangle(cornerRadius: 6))
                }
            } else {
                traceChip
            }
        }
    }

    /// GUI stage 2: a dim, collapsed-by-default disclosure for the
    /// model's reasoning text — collapsed shows a word-count summary
    /// only (no chrome beyond the chevron), expanded shows the full
    /// text in a smaller secondary-styled, selectable view.  Initial
    /// state comes from `entry.isExpandedByDefault`, driven by the
    /// "Detailed transcript" setting at append time.
    private var thinkingRow: some View {
        let wordCount = entry.text.split(whereSeparator: { $0.isWhitespace }).count
        return VStack(alignment: .leading, spacing: 4) {
            Button {
                isExpanded.toggle()
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
                        .font(.system(size: 8))
                    Text(isExpanded ? "Thinking" : "Thinking (\(wordCount) word\(wordCount == 1 ? "" : "s"))")
                }
                .font(Theme.sans(11).italic())
                .foregroundColor(Theme.textDim)
            }
            .buttonStyle(.plain)

            if isExpanded {
                Text(entry.text)
                    .font(Theme.sans(11))
                    .foregroundColor(Theme.textDim)
                    .lineSpacing(4)
                    .textSelection(.enabled)
                    .padding(.leading, 13)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var errorRow: some View {
        HStack(alignment: .top, spacing: 6) {
            Text("⚠").foregroundColor(Theme.error)
            Text(entry.text)
                .font(Theme.sans(12.5))
                .foregroundColor(Theme.error)
                .textSelection(.enabled)
        }
    }

    private var noticeRow: some View {
        Text(entry.text)
            .font(Theme.sans(12.5).italic())
            .foregroundColor(Theme.textDim)
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
struct ChatSettingsView: View {
    @EnvironmentObject var viewModel: RenderViewModel
    @ObservedObject var chat: ChatViewModel

    /// GUI stage 2: the transcript verbosity toggle — SAME UserDefaults
    /// key ChatViewModel's `detailedTranscriptEnabled` reads (see its
    /// doc).  Default false (compact): `.thinking` rows append
    /// collapsed and tool rows stay one line, matching today's density.
    /// True (detailed): new `.thinking` rows append pre-expanded. Only
    /// affects rows appended AFTER the toggle changes — it does not
    /// retroactively re-collapse/re-expand anything already in the
    /// transcript.
    @AppStorage("agentChatDetailedTranscript") private var detailedTranscript = false

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

            // Menu style, not segmented: five providers' display names
            // ("Grok (xAI)", "Local (Ollama)", ...) far exceed the
            // popover's 300pt width as segments -- the segmented control
            // overflowed the frame and made the whole popover unreadable
            // (user-reported when providers 4+5 landed).  A menu scales
            // to any provider count at constant width.
            Picker("Provider", selection: $draftProvider) {
                ForEach(AgentChatProviderChoice.allCases) { p in
                    Text(p.displayName).tag(p)
                }
            }
            .pickerStyle(.menu)
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
                        // The URL is ~43 chars of monospace -- borderline at
                        // the popover's 300pt.  Wrap rather than clip/expand
                        // (an env-override URL can be arbitrarily long).
                        .fixedSize(horizontal: false, vertical: true)
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

                // GUI stage 2: display-only verbosity, independent of
                // trajectory recording above (that logs to a file for
                // eval tooling; this controls what the live transcript
                // shows).
                Toggle("Detailed transcript (auto-expand thinking)",
                       isOn: $detailedTranscript)
                .toggleStyle(.checkbox)
                .font(.caption)
                .help("Show the model's reasoning expanded by default. "
                      + "Off (default): thinking is collapsed to a word "
                      + "count and tool results stay one line — click "
                      + "either to expand. Only affects rows appended "
                      + "after this changes.")
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
