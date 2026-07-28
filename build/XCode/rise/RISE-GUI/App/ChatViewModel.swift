import SwiftUI
import Combine
import Security
import AppKit
import CoreGraphics
import UniformTypeIdentifiers

// Facet 5 slice B2 — the LLM chat panel's driver.
//
// ALL correctness-bearing chat state lives in the C++ AgentChatLoop
// (wrapped by RISEAgentChatBridge); this file is the thin I/O driver
// the sans-IO core was designed for: it performs the HTTP round-trips,
// executes tool calls against the live scene via the viewport bridge's
// synchronous `agentHandleToolCall` (the AUTONOMY-ROUTED dispatcher —
// every model-requested verb, `render` included since the 2026-07
// per-session image-cache fix), and renders a display transcript.  The
// bridge's `agentHandleLine` is a SEPARATE administrative session; this
// driver calls it only for its own housekeeping (proposals polling /
// resolution, the one-time skill-index fetch), never for a tool call.
//
// SECRET HYGIENE (hard rule): API keys live in the macOS Keychain (or
// arrive via environment variables), never in UserDefaults, never
// logged/printed, never shown in the UI.  Nothing in this file logs
// request/response bodies.  Resolution (Keychain, else env-var
// fallback) happens at most ONCE per provider per process lifetime:
// the result is cached in `ChatViewModel.cachedApiKeys`, an in-memory
// (never persisted, never @Published) dictionary that lives exactly
// as long as the app process does — see that property's doc comment
// for the full security posture.

// MARK: - Keychain-backed API-key store

/// Minimal generic-password Keychain wrapper for the chat panel's
/// per-provider API keys.
///   service: "RISE Agent Chat"
///   account: the provider name ("anthropic" / "gemini" / "openai")
/// Update-or-add semantics on write; delete when the field is cleared.
enum AgentChatKeychain {
    static let service = "RISE Agent Chat"

    private static func baseQuery(account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }

    /// The stored key, or nil when absent (or unreadable).
    static func read(account: String) -> String? {
        var query = baseQuery(account: account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne

        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess,
              let data = item as? Data,
              let secret = String(data: data, encoding: .utf8),
              !secret.isEmpty else { return nil }
        return secret
    }

    /// Update-or-add.  An empty secret deletes the item instead (the
    /// "field cleared" affordance).  Returns false on a Keychain
    /// error; the caller surfaces that without echoing the secret.
    @discardableResult
    static func write(account: String, secret: String) -> Bool {
        guard !secret.isEmpty else { return delete(account: account) }
        guard let data = secret.data(using: .utf8) else { return false }

        // Try update first (the common re-key path)...
        let update: [String: Any] = [kSecValueData as String: data]
        let updateStatus = SecItemUpdate(baseQuery(account: account) as CFDictionary,
                                         update as CFDictionary)
        if updateStatus == errSecSuccess { return true }
        guard updateStatus == errSecItemNotFound else { return false }

        // ...then add.
        var add = baseQuery(account: account)
        add[kSecValueData as String] = data
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }

    /// Remove the stored key.  Absent-item deletes count as success.
    @discardableResult
    static func delete(account: String) -> Bool {
        let status = SecItemDelete(baseQuery(account: account) as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }

    /// Presence check WITHOUT kSecReturnData.  The settings UI calls
    /// this from view-body re-renders (via `keySource(for:)`); a
    /// data-returning query would materialize the secret on every
    /// render.  Matching on attributes alone answers "is there a
    /// key?" without the secret leaving the Keychain.  (`write`
    /// never stores an empty secret, so attribute-presence and
    /// `read() != nil` agree.)
    static func exists(account: String) -> Bool {
        var query = baseQuery(account: account)
        query[kSecReturnAttributes as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        return SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess
    }
}

// MARK: - Provider selection

/// Swift-side mirror of RISEAgentChatProvider, with the names the
/// Keychain accounts and env-var fallbacks hang off.
enum AgentChatProviderChoice: String, CaseIterable, Identifiable {
    case anthropic
    case gemini
    case openai
    case xai
    case local

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .anthropic: return "Anthropic"
        case .gemini:    return "Gemini"
        case .openai:    return "ChatGPT"
        case .xai:       return "Grok (xAI)"
        case .local:     return "Local (Ollama)"
        }
    }

    var bridgeValue: RISEAgentChatProvider {
        switch self {
        case .anthropic: return .anthropic
        case .gemini:    return .gemini
        case .openai:    return .openAI
        case .xai:       return .XAI
        case .local:     return .local
        }
    }

    /// Keychain account name (kSecAttrAccount).
    var keychainAccount: String { rawValue }

    /// Environment-variable fallback(s) consulted at USE time, in
    /// order, when the Keychain has no key for this provider.  Gemini
    /// gets two names because both are in conventional use by Gemini
    /// tooling (GEMINI_API_KEY is Google's own AI SDK convention;
    /// GOOGLE_API_KEY is the older/more widely-scripted one) — first
    /// match wins.  `local` is KEYLESS BY DESIGN (see MakeCodec in
    /// AgentChatLoop.cpp — requiresAuth=false): empty list, never
    /// prompted for, never resolved from the environment.
    var apiKeyEnvVars: [String] {
        switch self {
        case .anthropic: return ["ANTHROPIC_API_KEY"]
        case .gemini:    return ["GEMINI_API_KEY", "GOOGLE_API_KEY"]
        case .openai:    return ["OPENAI_API_KEY"]
        case .xai:       return ["XAI_API_KEY"]
        case .local:     return []
        }
    }

    /// Back-compat single-name accessor — used where only "the" env
    /// var name is needed (e.g. the no-key error hint's launch
    /// suggestion).  Always the first-checked name.  Never called for
    /// `.local` (guarded by `requiresApiKey` at every call site).
    var apiKeyEnvVar: String { apiKeyEnvVars[0] }

    /// False only for `.local` — a local/Ollama-style server is
    /// keyless by design (an empty key omits the Authorization header
    /// entirely; see OpenAIChatCodec::Config::requiresAuth).  The
    /// settings UI shows the resolved endpoint instead of a key
    /// prompt, and `driveTurn()` skips the no-key error for this case.
    var requiresApiKey: Bool {
        self != .local
    }

    var defaultModelId: String {
        RISEAgentChatBridge.defaultModelId(for: bridgeValue)
    }

    /// `.local` only: the endpoint the settings UI shows in place of a
    /// key prompt.  Mirrors MakeCodec's RISE_LOCAL_LLM_BASE_URL
    /// override / Ollama-default fallback (src/Library/Agent/
    /// AgentChatLoop.cpp) for DISPLAY purposes only — the GUI reads no
    /// library API for this (a base URL is config, not a credential,
    /// but there's no bridge accessor exposing the codec's resolved
    /// config, so the literal default is mirrored here).  Changing the
    /// env var takes effect on the next provider (re-)selection, same
    /// as the library side.
    static var localResolvedEndpoint: String {
        if let env = ProcessInfo.processInfo.environment["RISE_LOCAL_LLM_BASE_URL"],
           !env.isEmpty {
            return env
        }
        return "http://127.0.0.1:11434/v1/chat/completions"
    }
}

/// Where the API key in use came from — shown in the UI (the source
/// only, NEVER the key).
enum AgentChatKeySource: Equatable {
    case keychain
    case environment(String)   // the env-var name
    case none

    var label: String {
        switch self {
        case .keychain:             return "Keychain"
        case .environment(let v):   return "environment (\(v))"
        case .none:                 return "no key"
        }
    }
}

// MARK: - Chat view model

@MainActor
final class ChatViewModel: ObservableObject {

    /// One display-transcript row.  This is the GUI-facing rendering
    /// (user text, assistant narration, "→ tool" activity lines,
    /// errors); the authoritative wire transcript lives in the C++
    /// loop.
    struct Entry: Identifiable {
        enum Kind {
            case user
            case assistant
            case toolActivity
            /// GUI stage 2: the model's reasoning/thinking text for one
            /// turn (mirrors RISEAgentChatStep.reasoningText — see its
            /// doc).  Rendered as a collapsed-by-default disclosure; see
            /// `isExpandedByDefault`.
            case thinking
            case error
            case notice
            /// Stage 1b (clarifying-questions): the model's `ask_user`
            /// question text, appended when the tool call is intercepted
            /// (see `handleAskUserToolCall`) — this row IS the call's
            /// transcript representation, so `ask_user` gets no separate
            /// `.toolActivity` "→ ask_user" row.  Styled like `.assistant`
            /// narration but with a small accent marker so questions are
            /// scannable in history (ChatPanel's `questionRow`).
            case question
        }
        let id = UUID()
        let kind: Kind
        /// GUI stage 2: `var`, not `let` — a `.toolActivity` row is
        /// appended at DISPATCH time ("→ name") and updated IN PLACE
        /// once the tool's result line arrives ("→ name  outcome"), so
        /// the panel shows immediate activity feedback without a
        /// second, duplicate row per call.
        var text: String
        /// GUI stage 2: the row's expandable detail payload — a
        /// `.thinking` row's full reasoning text, or a `.toolActivity`
        /// row's args+result detail (set once the result arrives,
        /// alongside the in-place `text` update above).  nil/absent
        /// means "nothing to expand" (no chevron is shown).
        var detailText: String? = nil
        /// GUI stage 2: whether this row's disclosure starts expanded.
        /// Only ever set true for `.thinking` rows, and only when the
        /// "Detailed transcript" setting is on (see
        /// `ChatViewModel.detailedTranscriptEnabled`) — every other row
        /// kind, and every `.thinking` row in the default (compact)
        /// posture, starts collapsed.
        var isExpandedByDefault: Bool = false
        /// Small LOCAL thumbnails for images attached to a `.user` entry
        /// (Model-B F5 chat image attachments) — shows what was actually
        /// sent, without re-decoding the base64 that rode on the wire.
        /// Always empty for every other kind.
        var attachmentThumbnails: [NSImage] = []
    }

    /// One image queued to send with the NEXT message (Model-B F5 chat
    /// image attachments) — composed by `attachImageFile(at:)` /
    /// `attachImage(data:utType:)`, cleared on send/scene-close.
    struct PendingAttachment: Identifiable {
        let id = UUID()
        /// The exact bytes that will be base64-encoded onto the wire —
        /// already downscaled + re-encoded (see attachImage's doc for
        /// the JPEG-vs-PNG choice).  NEVER LOG.
        let encodedData: Data
        let mimeType: String
        /// A SMALL thumbnail for the composer row / transcript, built
        /// once at attach time and bounded to `thumbnailEdge` on its long
        /// edge (NOT the full-res source image) — the transcript is
        /// append-only for the life of the conversation, so an unbounded
        /// full-res `NSImage` per attachment would grow memory without
        /// limit over a long session.
        let thumbnail: NSImage
    }

    @Published private(set) var transcript: [Entry] = []

    /// Highest `compactedEntryCount` already reported to the user as a
    /// `.notice` row -- so one compaction event produces one notice, not
    /// one per subsequent request.  Reset wherever `transcript` is, since
    /// the loop's own counter is cleared by Reset()/SetProvider() too.
    private var lastReportedCompactedEntryCount: UInt = 0
    @Published var inputText: String = ""
    /// Images queued to go out with the NEXT send() (Model-B F5 chat
    /// image attachments) — populated by attachImageFile(at:), shown as
    /// thumbnails in the composer, cleared on send / Stop-does-NOT-
    /// clear-these (only send/sceneClosed/removePendingAttachment do —
    /// a Stop is a mid-turn abort, not "throw away what I was about to
    /// attach").  Capped at kMaxLiveUserImages per message — the SAME
    /// cap the loop enforces across the whole conversation, so one
    /// message can never itself force an immediate elision of ITS OWN
    /// images.
    @Published private(set) var pendingAttachments: [PendingAttachment] = []
    /// Set by attachImageFile(at:) on a rejected mimeType or a decode
    /// failure — a user-visible message, per the "reject unsupported
    /// types with a message, not a silent drop" contract.  Cleared on
    /// the next successful attach or explicitly by the panel.
    @Published var attachmentError: String? = nil
    /// True while a turn's request/tool loop is in flight (Send
    /// disabled, Stop shown).
    @Published private(set) var isBusy: Bool = false
    /// Set after an Http-kind error or the no-API-key failure — the
    /// retriable cases.  The Retry button re-issues the SAME
    /// conversation state (nothing was recorded for the failed step;
    /// for no-key, the user message IS recorded and BuildRequest
    /// replays it safely once a key is set).  Never set for
    /// MaxTokens / IterationCap, which must NOT be retried verbatim.
    @Published private(set) var retryAvailable: Bool = false
    /// Honest poison scoping: offered after repeated HTTP 400s (an
    /// exotic-but-well-formed recorded turn may be invalid on replay;
    /// Reset is the documented recovery).
    @Published private(set) var resetOffered: Bool = false

    // MARK: - Clarifying questions (`ask_user`, stage 1b)

    /// One `ask_user` call awaiting the user's answer — the Swift-facing
    /// mirror of the tool's parsed arguments (see
    /// `ChatViewModel.parseAskUserArgs`).  ChatPanel renders this as an
    /// answer card above the composer while non-nil.
    struct PendingQuestion: Equatable {
        let question: String
        let options: [String]
        let allowFreeform: Bool
    }
    /// Non-nil exactly while `handleAskUserToolCall` is suspended awaiting
    /// an answer (i.e. exactly while `pendingAnswerContinuation` is
    /// non-nil — the two are always set/cleared together).
    @Published private(set) var pendingQuestion: PendingQuestion? = nil
    /// The suspended `ask_user` continuation, or nil when no question is
    /// pending.  `resumePendingQuestion(with:)` is the ONLY place that
    /// resumes it — it nils this property BEFORE calling `.resume`, so a
    /// second resume attempt (a stray double-tap on an answer-card button
    /// racing a Stop, say) finds nil and safely no-ops.  A
    /// `CheckedContinuation` traps on a second `.resume`, so that guard is
    /// load-bearing, not defensive dressing.
    private var pendingAnswerContinuation: CheckedContinuation<String?, Never>?

    // Provider settings.  MODEL IDS persist in UserDefaults; keys go
    // to the Keychain only.
    @Published private(set) var provider: AgentChatProviderChoice
    @Published private(set) var modelId: String

    // MARK: - Agent autonomy selector (2026-07 GUI composer chips)

    /// The composer's current autonomy level for the CHAT AGENT's OWN tool
    /// calls (RISEViewportBridge's `-agentHandleToolCall:` routing — see
    /// that method's doc for the exact per-level verb behaviour). Persisted
    /// in UserDefaults so the choice survives relaunch; applied to a fresh
    /// `RISEViewportBridge` on every `sceneOpened(viewportBridge:)` (a new
    /// scene builds a NEW bridge, whose own dispatchers default to
    /// `.apply` until told otherwise — see the bridge header's doc).
    ///
    /// DEFAULT IS `.apply`, matching today's actual behaviour byte-for-
    /// byte: investigation for this feature (2026-07) found that the
    /// in-app chat's own tool calls have ALWAYS committed directly (Owner
    /// authority + Commit autonomy) — the existing "staged proposal" path
    /// (ProposalCard / `resolveProposal` below) is fed ONLY by the
    /// SEPARATE external-hosted-MCP-server session (`startExternalHosting`),
    /// never by this in-app chat driver. So `.propose` is a genuinely NEW
    /// behaviour for the in-app chat, not a re-labeling of something
    /// already true by default — defaulting to it would silently change
    /// what happens when a user who has never touched this control sends a
    /// message, which the feature brief explicitly rules out.
    @Published private(set) var autonomyLevel: RISEAgentAutonomyLevel
    private static let autonomyLevelKey = "agentAutonomyLevel"

    /// Change the composer's autonomy level: persist it and push it onto
    /// the live bridge (if any) immediately, so an in-flight session
    /// reflects the new choice on its very next tool call.
    func setAutonomyLevel(_ level: RISEAgentAutonomyLevel) {
        autonomyLevel = level
        UserDefaults.standard.set(level.rawValue, forKey: Self.autonomyLevelKey)
        viewportBridge?.agentAutonomyLevel = level
    }
    /// Bumped whenever a key is saved/cleared so the settings UI
    /// re-derives the key-source caption.
    @Published private(set) var keyStateEpoch: Int = 0

    /// Eval-harness E1: when ON (default), every chat session writes a
    /// trajectory JSONL under evals/runs/gui/.  Redaction of api-key-shaped
    /// content is unconditional in the core regardless of this toggle;
    /// this switch only controls whether a file is written at all.
    @Published private(set) var recordTrajectories: Bool

    // MARK: - Secure-MCP slice 5c: GUI-hosted external MCP endpoint +
    // proposals panel.  See `startExternalHosting()` / `stopExternalHosting()`
    // and the "Proposals panel" section further below.

    /// True while the GUI is hosting a loopback MCP HTTP server an
    /// EXTERNAL client (a separate process — e.g. Claude Code configured
    /// with this endpoint) can connect to and PROPOSE edits into this
    /// scene.  Off by default: no server, no external access, matching
    /// the "opt-in, honest security UX" contract the settings toggle
    /// documents.
    @Published private(set) var isHostingExternalAgent: Bool = false
    /// The bound loopback port, meaningful only while `isHostingExternalAgent`.
    @Published private(set) var hostedServerPort: Int = 0
    /// The per-launch bearer token an external client must present.
    /// Shown in-UI (copyable) so the user can paste it into their
    /// client's MCP config — NEVER logged (see ChatSettingsView / the
    /// bridge's own doc for the same rule stated at every layer this
    /// value passes through).
    @Published private(set) var hostedServerToken: String = ""
    /// Set when -startAgentHostedServerWithLabel: refuses (bind
    /// failure, or an internal-error guard) — surfaced as a one-line
    /// settings-panel error rather than silently doing nothing.
    @Published private(set) var hostedServerError: String? = nil

    /// Start hosting.  No-op (leaves `hostedServerError` set) if there is
    /// no live viewport bridge (no scene open) or the bridge refuses
    /// (already hosting, or a bind failure).
    func startExternalHosting() {
        guard let vb = viewportBridge else {
            hostedServerError = "No scene is open."
            return
        }
        let info = vb.startAgentHostedServer(sessionLabel: "external-mcp")
        if info.ok {
            isHostingExternalAgent = true
            hostedServerPort = Int(info.port)
            hostedServerToken = info.bearerToken
            hostedServerError = nil
        } else {
            hostedServerError = info.message
        }
    }

    /// Stop hosting (a no-op if not currently hosting).  Also called by
    /// `sceneClosed()` — the server borrows the live controller/Job, so
    /// it must not outlive the scene it was hosting against (the
    /// bridge's own `-shutdown` ALSO stops it defensively, but stopping
    /// here too keeps this view model's published state from lying
    /// about a server that the bridge is about to tear down anyway).
    func stopExternalHosting() {
        viewportBridge?.stopAgentHostedServer()
        isHostingExternalAgent = false
        hostedServerPort = 0
        hostedServerToken = ""
        hostedServerError = nil
    }

    // MARK: - Secure-MCP slice 5c: proposals panel

    /// One entry of `pendingProposals` — the Swift-facing rendering of
    /// list_proposals' wire shape (AgentRpc.h).  `summary` is a single
    /// human-readable line built from whichever fields are populated
    /// for this proposal's `kind` (param_edit vs insert_chunk /
    /// remove_chunk) — kept for any plain-text use; the diff-card
    /// presentation (ProposalDiffCard) instead renders the structured
    /// fields below directly, matching the RISE UI redesign comp.
    struct ProposalEntry: Identifiable, Equatable {
        let id: UInt64
        let kind: String
        let sessionLabel: String
        let status: String
        let summary: String
        /// Structured wire fields (AgentRpc.h's list_proposals shape),
        /// echoed straight through for the diff card — empty string
        /// when not meaningful for this `kind` (e.g. `param`/`value`
        /// are empty for `insert_chunk`/`remove_chunk`).
        let target: String
        let entityKind: String
        let param: String
        let value: String
        let chunkText: String
        /// True when the wire clipped `value`/`chunkText` for this
        /// listing (see AgentRpc.cpp's ClipProposalFieldEcho) — the
        /// diff card must never present a clipped preview as the whole
        /// (never-clipped) stored text without saying so.
        let truncated: Bool
    }

    /// The proposals panel's current listing — pending AND recently
    /// resolved (resolved entries are dropped from THIS array a few
    /// seconds after they leave "pending"; see `refreshProposals()`).
    @Published private(set) var pendingProposals: [ProposalEntry] = []
    /// proposalId -> the Date it was first observed leaving "pending"
    /// (applied/rejected/conflict) — drives the "show briefly, then
    /// drop" behaviour for resolved entries without a separate timer
    /// per row.
    private var resolvedProposalObservedAt: [UInt64: Date] = [:]
    /// How long a resolved proposal stays visible in the panel before
    /// being dropped on the next refresh.
    private static let resolvedProposalLingerSeconds: TimeInterval = 4.0

    /// Poll list_proposals through the ADMINISTRATIVE dispatcher
    /// (`agentHandleLine` → the bridge's `_agentDispatcher` — the same
    /// in-process path the raw JSON-RPC debug panel drives, and a
    /// GENUINELY DIFFERENT AgentSession from `agentHandleToolCall`'s
    /// `_agentToolDispatcherOwner`, which is where every model-requested
    /// tool call goes). This panel keeps its own path because
    /// resolve_proposal is refused outside Owner/Commit and
    /// `agentHandleLine`'s session is permanently Commit-capable. NOT the
    /// external hosted server. Cheap: a single
    /// synchronous HandleLine call returning the controller's in-memory
    /// queue — safe to call from a timer at a modest interval (see
    /// ChatPanel's ProposalsPanel, which drives this on a few-times-a-
    /// second Timer while the section is expanded, and once on appear).
    /// A no-op (clears the list) when there is no live viewport bridge.
    func refreshProposals() {
        guard let vb = viewportBridge else {
            if !pendingProposals.isEmpty { pendingProposals = [] }
            if !resolvedProposalObservedAt.isEmpty { resolvedProposalObservedAt = [:] }
            return
        }
        // Field fix (2026-07-11 "production render hangs the app"):
        // a render — production, or this chat's own outstanding render
        // job — holds the controller's mMutex for its WHOLE duration,
        // and list_proposals serializes on that same mutex.  This poll
        // runs synchronously on the main thread, so one tick during a
        // render wedged the entire UI until the render finished (and
        // with it, the production render's own progressive updates).
        // Skipping is lossless: StageProposal takes the same mutex, so
        // the queue cannot change while we're gated — the first
        // allowed tick re-syncs.  Keep the current list on screen
        // (don't clear) so the badge doesn't flicker during renders.
        guard sceneEditable() else { return }
        let line = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"list_proposals\"}"
        let response = vb.agentHandleLine(line)
        guard let data = response.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let result = root["result"] as? [String: Any],
              let proposals = result["proposals"] as? [[String: Any]] else {
            pendingProposals = []
            return
        }

        let now = Date()
        var next: [ProposalEntry] = []
        var nextObservedAt: [UInt64: Date] = [:]
        for p in proposals {
            guard let idNumber = p["id"] as? NSNumber else { continue }
            let id = idNumber.uint64Value
            let kind = p["kind"] as? String ?? ""
            let status = p["status"] as? String ?? ""
            let sessionLabel = p["sessionLabel"] as? String ?? ""

            if status != "pending" {
                // Carry forward (or start) the observed-resolved timestamp;
                // drop it from the panel once it has lingered long enough.
                let observedAt = resolvedProposalObservedAt[id] ?? now
                if now.timeIntervalSince(observedAt) > Self.resolvedProposalLingerSeconds {
                    continue
                }
                nextObservedAt[id] = observedAt
            }

            let target = p["target"] as? String ?? ""
            let entityKind = p["entityKind"] as? String ?? ""
            let param = p["param"] as? String ?? ""
            let value = p["value"] as? String ?? ""
            let chunkText = p["chunkText"] as? String ?? ""
            // Secure-MCP slice 6 fix round: the wire's per-proposal echo
            // cap (see AgentRpc.cpp's ClipProposalFieldEcho) marks
            // `truncated:true` whenever `value`/`chunkText` was clipped for
            // the listing -- surface that here (both in `summary` and as
            // its own field, for ProposalDiffCard) so the owner never
            // mistakes a clipped preview for the whole (never-clipped)
            // stored text.
            let truncated = (p["truncated"] as? Bool) == true

            var summary: String
            switch kind {
            case "insert_chunk":
                let firstLine = chunkText.split(separator: "\n").first.map(String.init) ?? chunkText
                summary = "insert: \(firstLine)"
            case "remove_chunk":
                summary = entityKind.isEmpty ? "remove: \(target)" : "remove: \(target) (\(entityKind))"
            default:
                summary = "\(target).\(param) = \(value)"
            }
            if truncated {
                summary += " (truncated)"
            }

            next.append(ProposalEntry(
                id: id, kind: kind, sessionLabel: sessionLabel, status: status, summary: summary,
                target: target, entityKind: entityKind, param: param, value: value,
                chunkText: chunkText, truncated: truncated))
        }
        // Publish-on-change: this poll runs at 1 Hz and the list is
        // usually IDENTICAL tick to tick — unconditional assignment
        // invalidated every observer (including, pre-decoupling, the
        // whole menu bar) once a second.
        if pendingProposals != next { pendingProposals = next }
        if resolvedProposalObservedAt != nextObservedAt {
            resolvedProposalObservedAt = nextObservedAt
        }
    }

    /// Approve or reject proposal `id` through the ADMINISTRATIVE
    /// dispatcher (the Owner-only + Commit-only gates on resolve_proposal
    /// — see AgentRpc.h — mean this MUST go through `agentHandleLine`'s
    /// `_agentDispatcher`, never the tool-call sessions
    /// `agentHandleToolCall` selects and never the external hosted
    /// server's own dispatcher, which would be
    /// refused outright). Refreshes the listing immediately afterward so
    /// the panel reflects the new status without waiting for the next
    /// timer tick.
    ///
    /// P1-2 fix: gated on `sceneEditable()` — the SAME closure `send()`
    /// checks (see its guard above) and every other agent-mutation
    /// surface (the raw JSON-RPC debug panel's `sendAgentRequest`)
    /// consumes.  Applying a proposal drives a CST re-derive through the
    /// live controller exactly like a `propose_patch` tool call does;
    /// without this guard a user could hit Apply/Reject while a
    /// production render's workers are reading Scene state off-main, or
    /// while a chat-driven agent render is outstanding on the
    /// controller's single-slot worker — the same race `sceneEditable`
    /// exists to prevent everywhere else.  No user-visible error row
    /// here (unlike `send()`'s turn-based error entry): the button
    /// itself is disabled on the same predicate in ProposalCard's
    /// `pendingFooter`, so reaching this guard's false branch means a
    /// state change raced the click between render and dispatch — a
    /// silent no-op is the right degrade, matching `sendAgentRequest`'s
    /// nil-bridge branch.
    func resolveProposal(id: UInt64, approve: Bool) {
        guard let vb = viewportBridge, sceneEditable() else { return }
        let line = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"resolve_proposal\"," +
                   "\"params\":{\"proposalId\":\(id),\"approve\":\(approve ? "true" : "false")}}"
        _ = vb.agentHandleLine(line)
        refreshProposals()
    }

    /// Process-memory-only cache of the resolved API key, per provider.
    ///
    /// WHY: `resolveApiKey()` used to hit the Keychain
    /// (`SecItemCopyMatching` with `kSecReturnData`) on every single
    /// HTTP round of every turn.  On a dev-signed (ad-hoc / no stable
    /// Team ID) build, macOS cannot verify the app's code-signing
    /// identity against the ACL recorded on the Keychain item, so
    /// *every* data-returning query re-prompts the user for their
    /// login password — one popup per HTTP round-trip, which made
    /// multi-tool-call turns nearly unusable.
    ///
    /// This cache resolves the key ONCE (Keychain, else env-var
    /// fallback) and serves every subsequent call from memory for the
    /// rest of the process's life, until the user changes what's
    /// stored (`saveApiKey`) or clears it (`clearApiKey`).
    ///
    /// Security posture (stated plainly rather than overstated): this
    /// is a plaintext `String` held in process memory.  Swift/Combine
    /// give us no reliable way to zero a `String`'s backing storage on
    /// dealloc (unlike a manually-managed byte buffer), so "wiped on
    /// exit" really means "the value dies when the process's address
    /// space is torn down by the OS" — not an explicit scrub.  That is
    /// an acceptable bar here: the secret is a user-supplied API key
    /// the user already explicitly granted THIS app access to via the
    /// Keychain (or an env var they set for this process), this is a
    /// single-user trusted-operator desktop tool with no other
    /// tenants/processes/users to isolate from, and the value never
    /// leaves this property (never logged, never published, never
    /// serialized to disk, never shown in any view).  Not @Published —
    /// no view ever binds to it, directly or indirectly.
    private var cachedApiKeys: [AgentChatProviderChoice: String] = [:]

    /// B2 review round 1: "may agent tool calls mutate the scene
    /// RIGHT NOW?"  Injected by RenderViewModel at construction (it
    /// answers false during a production render, whose worker threads
    /// read Scene state off-main — a tool call's CST re-derive would
    /// destroy scene objects under them).  Fails CLOSED when unset.
    /// Checked in send(), at the top of every driver round, after
    /// every await, and before each tool call; all of those sites and
    /// the predicate itself run on the main actor, so check-then-
    /// execute cannot interleave with a render kick (see driveTurn's
    /// invariant comment).
    var sceneEditable: () -> Bool = { false }

    /// Model-B F2 slice S2b: a NARROWER sibling of `sceneEditable`, used
    /// ONLY inside a chat-driven `render` tool call's own render_wait
    /// poll loop (`executeRenderToolCallAsync`).  Answers "did a
    /// PRODUCTION render start?" — i.e. the exact same `renderState`
    /// switch `isSceneEditableForAgents` uses — WITHOUT folding in
    /// `chat.isChatRenderOutstanding`.  This distinction is load-bearing,
    /// not a duplicated case list: `isSceneEditableForAgents` (which
    /// backs `sceneEditable` below) now ALSO returns false while THIS
    /// call's own render is outstanding — correct for every OTHER caller
    /// (the JSON-RPC debug panel, a later tool call, a fresh send()), but
    /// self-defeating for the render call's OWN poll loop, which would
    /// otherwise observe its own in-flight status as a conflict and
    /// abandon on its very first slice.  Injected by RenderViewModel
    /// alongside `sceneEditable`, reading `renderState` directly (the
    /// same cases, deliberately re-derived rather than calling the
    /// combined predicate) so this loop still correctly aborts when a
    /// PRODUCTION render starts mid-poll (productionRenderStarting's
    /// cancelTurn path is what actually tears the turn down in that
    /// case; this predicate is the belt-and-suspenders defensive check
    /// alongside Task.isCancelled/stopRequested, matching the rest of
    /// this file's post-await pattern).  Fails CLOSED when unset — unlike
    /// `sceneEditable` (whose false-by-default means "not editable"),
    /// this property's safe default is `true` ("assume a production
    /// render IS active"), since a poller that can't determine the real
    /// state should abandon rather than proceed.
    var productionRenderActive: () -> Bool = { true }

    /// Invoked synchronously on the MainActor immediately before any chat
    /// render tool call reaches the controller.  RenderViewModel uses this
    /// last safe ownership window to stop timeline playback and close open
    /// viewport interactions before the render worker can acquire the scene.
    var chatRenderWillSubmit: () -> Void = {}

    private let chatBridge = RISEAgentChatBridge()
    /// The per-scene tool executor.  WEAK: RenderViewModel owns the
    /// viewport bridge; on clearScene it calls `sceneClosed()` (which
    /// also nils this) BEFORE shutting the bridge down, so the driver
    /// never calls into the bridge's agent dispatchers
    /// (`agentHandleToolCall` for tool calls, `agentHandleLine` for its
    /// own housekeeping) on a torn-down scene.  Even if a stale call
    /// slipped through, both entry points are total — they answer with a
    /// JSON-RPC -32603 error rather than crashing.
    private weak var viewportBridge: RISEViewportBridge?
    private var driverTask: Task<Void, Never>? = nil
    /// The Stop button's flag.  Checked between HTTP rounds and
    /// between tool calls; the driver simply STOPS — the next
    /// addUserMessage flushes the abandoned pending calls with
    /// synthesized cancelled results (the loop's designed interrupt
    /// path), so no special cleanup happens here.
    private var stopRequested: Bool = false
    /// Monotonic JSON-RPC id for tool-call request lines.
    private var nextRpcId: Int = 1
    /// Turn generation — bumped by every startTurn/cancelTurn so a
    /// cancelled turn's completion tail can't clobber the busy state
    /// (or the task handle) of a NEWER turn started in between.
    private var turnGeneration: Int = 0
    /// Consecutive HTTP-400 count for the reset-offer heuristic.
    /// Scoped to the CURRENT conversation: reset wherever the
    /// conversation resets (sceneOpened / sceneClosed /
    /// applyProviderSelection / resetConversation) so a stale count
    /// can't trigger the reset offer on a fresh conversation.
    private var consecutiveHttp400s: Int = 0
    /// Scene generation — bumped by every sceneOpened/sceneClosed.  A
    /// detached image-attach encode (see `attachImageFile(at:)`) captures
    /// this at attach-start and re-checks it on the main-actor hop before
    /// appending to `pendingAttachments`: if the value moved, the scene
    /// this attachment was composed against is already gone (a fresh
    /// scene may have loaded, or the app may have gone scene-less), and
    /// the result is dropped rather than leaking into whatever composer
    /// is showing now.  Distinct from `turnGeneration` (which scopes a
    /// CONVERSATION turn, not a scene's lifetime).  Deliberately NOT
    /// bumped by `send()` (which also clears `pendingAttachments`) or by
    /// `resetConversation()`/`applyProviderSelection()`: none of those
    /// tear down the SCENE, so an attach still in flight when one of them
    /// fires is still valid to land in the composer for the NEXT message
    /// — only an actual scene teardown invalidates an in-flight attach.
    private var sceneGeneration: Int = 0

    /// Model-B F2 slice S2b: the renderJobId used to address a chat-driven
    /// async render for cancellation (0 = no cancel request is needed).
    /// This is deliberately NOT the UI safety gate: render_cancel is
    /// fire-and-forget, so its id must be cleared promptly while the worker
    /// can still own the controller. Read by
    /// `cancelAnyOutstandingChatRender()` so productionRenderStarting /
    /// cancelTurn / a user Stop can trip `render_cancel` on it WITHOUT
    /// waiting for the render to actually finish — the point being that a
    /// production render must not queue up behind a chat-driven agent
    /// render on the controller's single-slot worker (see
    /// SubmitAgentRenderAsync's single-slot policy).  MainActor-only,
    /// like every other piece of driver state in this class.
    private var outstandingChatRenderJobId: UInt64 = 0

    /// The level pinned for the currently-outstanding chat render job
    /// (captured once at submit time in `executeRenderToolCallAsync`).
    ///
    /// What it pins is the SESSION SELECTION, not the autonomy posture: the
    /// level chooses which dispatcher/session handles a call, and the live
    /// posture on the selected session still applies (the bridge's
    /// `setAgentAutonomyLevel` mutates the TOOL-CALL Owner session's
    /// autonomy in place — `_agentToolDispatcherOwner`, never the
    /// administrative dispatcher), so a mid-render drop to Read is NOT
    /// defeated by this pin.
    ///
    /// Why pin at all: renderJobIds themselves are addressable from any
    /// session on the same controller (they are minted BY the controller),
    /// but two things about a render job are session-scoped — `render_wait`'s
    /// optional `result` payload, and the last-render PNG cache `read_image`
    /// serves.  Poll from a sibling session and the job completes with no
    /// result to report.  Full derivation in
    /// `-agentHandleToolCall:autonomy:`'s doc in RISEViewportBridge.h.
    ///
    /// Scoped to ONE JOB on purpose, never to a whole turn: autonomy is a
    /// safety control, and a user who drops to Read mid-turn must have that
    /// bind on the agent's very next tool call.
    private var outstandingChatRenderAutonomy: RISEAgentAutonomyLevel = .apply

    /// The controller worker is still occupied by this chat render, including
    /// after a fire-and-forget cancellation request. RenderViewModel forwards
    /// this published state to every control that relies on its single scene
    /// edit gate, preventing a synchronous bridge call from blocking on the
    /// worker's mutex during cancellation drain.
    @Published private(set) var chatRenderWorkerOccupied: Bool = false
    private var chatRenderOccupancyJobId: UInt64 = 0
    private var chatRenderDrainTask: Task<Void, Never>? = nil

    /// Model-B F2 slice S2b: true while the controller's single-slot worker
    /// is occupied by a chat-driven render. This remains true after a cancel
    /// request until render_wait observes actual worker completion. This is
    /// truth `renderState` (a pure GUI/Swift notion)
    /// cannot see on its own — an agent render does not flip `renderState`
    /// out of `.sceneLoaded`/`.completed` — so `RenderViewModel`'s single
    /// `isSceneEditableForAgents` predicate (the ONE gate both the chat
    /// driver and the Agent JSON-RPC debug panel consume) folds this in
    /// alongside its existing `renderState` switch: without it, the raw
    /// JSON-RPC panel could fire a `propose_patch` (or another render)
    /// concurrently with a chat-driven render still running on the same
    /// controller.  MainActor-only, like `outstandingChatRenderJobId`
    /// itself.
    var isChatRenderOutstanding: Bool { chatRenderWorkerOccupied }

    /// Start-screen create path: a prompt stashed by
    /// `RenderViewModel.loadStarterScene(withPrompt:)` BEFORE the starter
    /// scene load begins, consumed EXACTLY ONCE at the end of
    /// `sceneOpened` (the earliest point a message can validly be sent —
    /// fresh bridge attached, chat state reset).  MainActor-only.
    var pendingFirstPrompt: String? = nil

    /// True when the selected provider can actually take a message: keyless
    /// providers (local/Ollama) always can; the rest need a resolvable key
    /// (Keychain or env var).  The start screen's Create column mirrors this
    /// so the agent path never dead-ends (spec §6).  Reads `keyStateEpoch`
    /// so SwiftUI re-evaluates when a key is saved or cleared in settings.
    var agentConfigured: Bool {
        _ = keyStateEpoch
        return !provider.requiresApiKey || keySource(for: provider) != .none
    }

    private static let providerKey = "agentChat.provider"
    private static let recordTrajectoriesKey = "agentChat.recordTrajectories"
    private static func modelIdKey(_ p: AgentChatProviderChoice) -> String {
        "agentChat.modelId.\(p.rawValue)"
    }

    /// GUI stage 2: the verbosity toggle's UserDefaults key — SAME key
    /// ChatSettingsView's `@AppStorage("agentChatDetailedTranscript")`
    /// reads/writes (both sides of one UserDefaults entry; there is no
    /// dedicated `@Published` mirror here because this is read once per
    /// append, not observed continuously).  Default false (compact):
    /// `.thinking` rows append collapsed.  True (detailed): `.thinking`
    /// rows append pre-expanded, so the reasoning is visible without an
    /// extra click.
    private static let detailedTranscriptKey = "agentChatDetailedTranscript"

    /// The CURRENT value of the verbosity toggle, read fresh each time
    /// (see the key doc above) — used only to seed a NEW `.thinking`
    /// entry's `isExpandedByDefault`; never retroactively re-collapses/
    /// re-expands rows already in the transcript.
    private var detailedTranscriptEnabled: Bool {
        UserDefaults.standard.bool(forKey: Self.detailedTranscriptKey)
    }

    // MARK: - Prompt triage (GUI stage 3 of clarifying-questions)

    /// SAME UserDefaults keys `ChatSettingsView`'s `@AppStorage` fields
    /// read/write — see `detailedTranscriptKey`'s doc for why this file
    /// re-reads rather than mirroring with a `@Published` property (read
    /// once per `send()`, not observed continuously).
    private static let triageEnabledKey  = "agentChatTriageEnabled"
    private static let triageProviderKey = "agentChatTriageProvider"
    private static let triageModelKey    = "agentChatTriageModel"

    /// Default OFF — triage is an opt-in pre-pass (see `send()`'s
    /// eligibility gate).
    private var triageEnabled: Bool {
        UserDefaults.standard.bool(forKey: Self.triageEnabledKey)
    }

    /// "local" ($0, matches the main provider's own keyless-by-design
    /// default posture) when unset or when the stored raw value no
    /// longer names a real provider (e.g. a future removed case).
    private var triageProvider: AgentChatProviderChoice {
        let raw = UserDefaults.standard.string(forKey: Self.triageProviderKey) ?? "local"
        return AgentChatProviderChoice(rawValue: raw) ?? .local
    }

    /// "" (provider default) is legal — `runTriage` passes it straight
    /// to `setProvider:modelId:`, same as the main provider picker's
    /// blank-model convention.  The DEFAULT value here (before the user
    /// ever opens settings) is "qwen3.6:27b" — a small, fast local model
    /// appropriate for a one-shot triage question, not the main chat's
    /// default per-provider model.
    private var triageModelId: String {
        let raw = UserDefaults.standard.string(forKey: Self.triageModelKey) ?? ""
        return raw.isEmpty ? "qwen3.6:27b" : raw
    }

    /// GUI stage 2: cap a tool-call detail payload (args JSON or a raw
    /// result line) to ~4KB — Swift `String.prefix` counts Characters
    /// (grapheme clusters), so this can never split a multi-byte
    /// codepoint the way a raw byte-offset cut could; matches the
    /// UTF-8-safety the C++ side's own ~8KB resultJson cap provides
    /// (AgentChatLoop.cpp's Utf8SafeCutPoint), just via Swift's native
    /// Character-counted `String` rather than a manual byte scan.
    private static func cappedForDetail(_ s: String, limit: Int = 4096) -> String {
        if s.count <= limit { return s }
        return String(s.prefix(limit)) + "…[truncated]"
    }

    /// Review-round P2 (E1): the loaded scene's path, handed in by
    /// RenderViewModel at sceneOpened -- the trajectory<->document
    /// correlator for the session record.  Cleared at scene close.
    private var currentScenePath: String = ""

    /// The per-session trajectory directory.
    ///
    /// A GUI app's process CWD is meaningless -- Finder-launched, it's "/"
    /// (creating "/evals" under it fails with a permission error); Xcode-
    /// launched, it's some DerivedData build directory nobody would think
    /// to look in.  Either way `evals/runs/gui` relative to it silently
    /// never got created and every trajectory line vanished into a sink
    /// that opened nothing (recording toggle said ON; nothing was
    /// written -- see MakeTrajectoryFileSink's loud-once-failure log,
    /// which is the ONLY place that ever surfaced this before the fix).
    ///
    /// Default is a deterministic, always-writable location under the
    /// app's Application Support directory.  `RISE_TRAJECTORY_DIR`
    /// overrides it verbatim when set + non-empty -- the dev workflow of
    /// running the GUI straight out of the repo and wanting files under
    /// `evals/runs/gui` there.
    ///
    /// Not private: ChatPanel reads it to show the resolved path in the
    /// "Record chat trajectories" toggle's help text (closing-verify D) --
    /// a toggle that's silently ON with no visible destination is exactly
    /// the discoverability gap this fix closes.
    var trajectoryDirectory: String {
        if let override = ProcessInfo.processInfo.environment["RISE_TRAJECTORY_DIR"],
           !override.isEmpty {
            return override
        }
        if let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first {
            return appSupport.appendingPathComponent("RISE/trajectories/gui").path
        }
        // Application Support should always resolve on macOS; this is a
        // last-ditch fallback that keeps the property total rather than
        // introducing an optional the many call sites would need to unwrap.
        return NSTemporaryDirectory() + "RISE/trajectories/gui"
    }

    /// (Re)start the per-session trajectory file for the current scene, or
    /// DETACH when recording is disabled.  Invoked at scene open, on a
    /// provider switch, and when the toggle changes.
    private func startTrajectory() {
        // Review-round P2: the session record's scene-identity fields were
        // shipped dead (""/-1) -- the scene PATH is the trajectory<->document
        // correlator the eval harness needs, and it is available right here.
        // headVersion stays best-effort -1 on the GUI (no cheap accessor at
        // attach time; the headless runner populates it precisely).
        chatBridge.startTrajectory(directory: trajectoryDirectory,
                                   scenePath: currentScenePath,
                                   headVersion: -1,
                                   enabled: recordTrajectories)
    }

    /// Toggle trajectory recording (persisted).  Turning it OFF finishes
    /// the current session's summary line, then detaches; turning it ON
    /// starts a fresh session file.
    func setRecordTrajectories(_ on: Bool) {
        guard on != recordTrajectories else { return }
        recordTrajectories = on
        UserDefaults.standard.set(on, forKey: Self.recordTrajectoriesKey)
        if on {
            startTrajectory()
        } else {
            chatBridge.finishTrajectory(status: "toggle_off")
            chatBridge.startTrajectory(directory: trajectoryDirectory,
                                       scenePath: "", headVersion: -1, enabled: false)
        }
    }

    // MARK: - Image attachments (Model-B F5)

    /// The provider-accepted MIME allow-list, exactly as documented in
    /// AgentChatCodecs.h (Anthropic's `media_type` list; Gemini accepts
    /// the same four via `inlineData.mimeType`).  Anything outside this
    /// set is rejected HERE, at the Swift layer, with a user-visible
    /// message — the C++ core deliberately does not gate on mimeType
    /// (see ChatAttachment's doc), so this is the one enforcement point.
    private static let acceptedMimeTypes: Set<String> = [
        "image/png", "image/jpeg", "image/gif", "image/webp",
    ]

    /// Long-edge bound for a downscaled attachment, in pixels.  1024 is
    /// generous enough for the agent to read fine reference detail
    /// (proportions, distinguishing features, color) while keeping
    /// re-encoded JPEG payloads in the low hundreds of KB rather than
    /// the multi-MB a modern phone photo starts at — the whole
    /// conversation history re-sends on every turn, so attachment size
    /// is a per-turn, not per-attach, token cost (see AgentChatLoop.h
    /// "USER IMAGE RETENTION" for the cap that bounds the OTHER half of
    /// that cost: how many stay live across turns).  `nonisolated`: read
    /// only from `downscaleAndEncode`, which itself is `nonisolated` so
    /// it can run on the `Task.detached` background context in
    /// `attachImageFile(at:)` — a MainActor-isolated constant could not
    /// be referenced from there.
    private nonisolated static let maxAttachmentEdge: CGFloat = 1024

    /// JPEG re-encode quality for downscaled photographic attachments.
    /// PNG-vs-JPEG reasoning: reference photos (the whole point of this
    /// feature) are photographic content, where JPEG's DCT compression
    /// beats PNG's lossless filter+deflate by a wide margin at a
    /// perceptually-lossless quality — a 1024px-edge JPEG at 0.8 quality
    /// typically lands in the 100-300KB range where the equivalent PNG
    /// would run several times larger, and that size rides on EVERY
    /// later request for as long as the image stays live (see the cap
    /// above).  The exception: an attachment whose SOURCE carries an
    /// alpha channel (a screenshot or a graphic with transparency, not a
    /// camera photo) keeps PNG — JPEG has no alpha channel, so
    /// re-encoding would either flatten transparency onto an arbitrary
    /// background or lose it outright, and a UI screenshot is exactly
    /// the case where flat-color regions make PNG's compression
    /// competitive anyway.  `nonisolated` for the same reason as
    /// `maxAttachmentEdge` above.
    private nonisolated static let jpegQuality: CGFloat = 0.8

    /// Long-edge bound, in pixels, for the composer/transcript thumbnail
    /// stored on `PendingAttachment` (and copied onto the transcript
    /// `Entry` on send).  96 is comfortably above the 40×40 (composer
    /// strip) / 32×32 (transcript row) display sizes ChatPanel draws at
    /// @2x, so it stays crisp on Retina, while being tiny next to a
    /// full-res source image — this bounds the append-only transcript's
    /// memory instead of retaining `sourceImage` (which can be many
    /// megapixels for a phone photo) for the life of the conversation.
    /// `nonisolated`: read only from `makeThumbnail`, which itself is
    /// `nonisolated` for the same background-context reason as
    /// `maxAttachmentEdge` above.
    private nonisolated static let thumbnailEdge: CGFloat = 96

    init() {
        let storedProvider = UserDefaults.standard.string(forKey: Self.providerKey)
            .flatMap(AgentChatProviderChoice.init(rawValue:)) ?? .local   // default: keyless local (opencoder), user decision 2026-07-16
        let storedModelId = UserDefaults.standard.string(
            forKey: Self.modelIdKey(storedProvider)) ?? ""
        provider = storedProvider
        modelId = storedModelId.isEmpty ? storedProvider.defaultModelId : storedModelId
        // Eval-harness E1: default ON (records every session under
        // evals/runs/gui/).  The actual file is attached at scene open.
        recordTrajectories =
            (UserDefaults.standard.object(forKey: Self.recordTrajectoriesKey) as? Bool) ?? true
        chatBridge.setProvider(storedProvider.bridgeValue, modelId: storedModelId)

        // Agent autonomy selector: an absent/out-of-range stored value
        // resolves to `.apply` — see `autonomyLevel`'s doc for why that
        // (not `.propose`) is the honest default.
        let storedAutonomyRaw = UserDefaults.standard.object(forKey: Self.autonomyLevelKey) as? Int
        autonomyLevel = storedAutonomyRaw.flatMap(RISEAgentAutonomyLevel.init(rawValue:)) ?? .apply
    }

    // MARK: Scene lifecycle (driven by RenderViewModel)

    /// A scene finished loading: bind the tool executor, reset the
    /// conversation (it was about the previous scene), and fetch the
    /// skills index ONCE by driving the read_skill verb through the
    /// live dispatcher (per the SetSkillIndex contract — the loop
    /// stays sans-IO; the driver does the fetch).
    /// NOTE: `scenePath` has a compatibility default, but every real call
    /// site must pass the loaded path (RenderViewModel does) -- a future
    /// caller that omits it silently ships a session record with no
    /// scene identity (closing-verify P3).
    func sceneOpened(viewportBridge vb: RISEViewportBridge, scenePath: String = "") {
        cancelTurn()
        // RenderViewModel calls sceneClosed(), then synchronously shuts down
        // the previous bridge before constructing this fresh one. That join
        // proves an old worker is drained, so discard any cancellation poll
        // that was waiting on the now-gone bridge and start this scene clean.
        chatRenderDrainTask?.cancel()
        chatRenderDrainTask = nil
        chatRenderOccupancyJobId = 0
        chatRenderWorkerOccupied = false
        sceneGeneration += 1
        viewportBridge = vb
        // Agent autonomy selector: a fresh bridge's dispatchers default to
        // `.apply` (see the bridge header's doc) until told otherwise —
        // apply THIS view model's persisted choice immediately so a scene
        // switch never silently resets a user's Read/Propose selection
        // back to Apply.
        vb.agentAutonomyLevel = autonomyLevel
        currentScenePath = scenePath
        chatBridge.reset()
        transcript = []
        lastReportedCompactedEntryCount = 0
        clearErrorAffordances()
        consecutiveHttp400s = 0
        pendingAttachments = []
        attachmentError = nil
        // Secure-MCP slice 5c: a freshly-opened scene never inherits a
        // "hosting"/proposals state from whatever was showing for the
        // PREVIOUS scene — the bridge itself is a fresh instance (no
        // server running on it yet either), so this is a state-only
        // reset, not an actual stop.
        isHostingExternalAgent = false
        hostedServerPort = 0
        hostedServerToken = ""
        hostedServerError = nil
        pendingProposals = []
        resolvedProposalObservedAt = [:]

        let indexLine = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"read_skill\"}"
        chatBridge.setSkillIndex(
            Self.renderSkillIndex(fromRpcResponse: vb.agentHandleLine(indexLine)))

        // Eval-harness E1: a freshly-opened scene starts a NEW trajectory
        // file (the skill index is set above, so the session record captures
        // the full system prompt including skills on first user message).
        startTrajectory()

        // Start-screen create path (spec §7): consume the pending first
        // prompt EXACTLY ONCE, and only here — after the fresh bridge is
        // attached and the chat state reset above, i.e. the same point a
        // typed message could first be sent.  Stashing-then-consuming (vs
        // sending from the start screen directly) is what makes the handoff
        // immune to the slow-load race: however long the scene load takes,
        // the prompt can neither fire early (no bridge yet) nor double-fire
        // (cleared before send).
        if let prompt = pendingFirstPrompt {
            pendingFirstPrompt = nil
            inputText = prompt
            send()
        }
    }

    /// The scene is closing (clearScene / reload): stop any in-flight
    /// turn and drop the tool executor so the driver cannot touch the
    /// torn-down dispatcher.  MUST be called before the viewport
    /// bridge's shutdown.  Also clears any queued-but-unsent image
    /// attachments (Model-B F5) — they were composed against the scene
    /// that's going away, same as the text input clear right below.
    func sceneClosed() {
        cancelTurn()
        // Secure-MCP slice 5c: stop the hosted server (if any) BEFORE
        // dropping `viewportBridge` below — stopExternalHosting() reads
        // it to call -stopAgentHostedServer, which the bridge's own
        // -shutdown will ALSO call defensively, but stopping here first
        // means this view model's published state is never stale by the
        // time the panel re-renders for the (about to be gone) scene.
        stopExternalHosting()
        sceneGeneration += 1
        viewportBridge = nil
        currentScenePath = ""
        chatBridge.reset()
        // Eval-harness E1: reset() closed the session ("reset" summary to
        // the current file); detach so no file lingers between scenes (the
        // next sceneOpened starts a fresh one).
        chatBridge.startTrajectory(directory: trajectoryDirectory,
                                   scenePath: "", headVersion: -1, enabled: false)
        transcript = []
        lastReportedCompactedEntryCount = 0
        inputText = ""
        clearErrorAffordances()
        consecutiveHttp400s = 0
        pendingAttachments = []
        attachmentError = nil
        pendingProposals = []
        resolvedProposalObservedAt = [:]
    }

    /// Render the read_skill (no name) JSON-RPC response into the
    /// stable "name -- hook" lines SetSkillIndex documents.  Any
    /// parse/shape failure yields "" (the loop then omits the skills
    /// section — degraded but valid).
    private static func renderSkillIndex(fromRpcResponse line: String) -> String {
        guard let data = line.data(using: .utf8),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let result = root["result"] as? [String: Any],
              let skills = result["skills"] as? [[String: Any]] else { return "" }
        let lines = skills.compactMap { skill -> String? in
            guard let name = skill["name"] as? String, !name.isEmpty else { return nil }
            let hook = skill["hook"] as? String ?? ""
            return hook.isEmpty ? name : "\(name) -- \(hook)"
        }
        return lines.joined(separator: "\n")
    }

    // MARK: Provider / model / key settings

    var hasConversation: Bool { !transcript.isEmpty }

    /// Apply a provider (and/or model-id) change.  The C++ contract:
    /// SetProvider RESETS the transcript (assistant turns are
    /// provider-native JSON) — the settings UI confirms with the user
    /// first when `hasConversation`; this method just applies.
    /// An empty modelId selects the provider's default.
    func applyProviderSelection(_ newProvider: AgentChatProviderChoice,
                                modelId newModelId: String) {
        cancelTurn()

        let trimmed = newModelId.trimmingCharacters(in: .whitespacesAndNewlines)
        UserDefaults.standard.set(newProvider.rawValue, forKey: Self.providerKey)
        UserDefaults.standard.set(trimmed, forKey: Self.modelIdKey(newProvider))

        chatBridge.setProvider(newProvider.bridgeValue, modelId: trimmed)
        provider = newProvider
        modelId = chatBridge.modelId
        transcript = []
        lastReportedCompactedEntryCount = 0
        clearErrorAffordances()
        consecutiveHttp400s = 0
        // Eval-harness E1: setProvider closed the old session (a
        // "provider_switch" summary to the old file); begin a new file so
        // the new provider's session records land separately.
        startTrajectory()
    }

    /// Persisted model id for `p` ("" = provider default).  Lets the
    /// settings UI show each provider's stored value while editing.
    func storedModelId(for p: AgentChatProviderChoice) -> String {
        UserDefaults.standard.string(forKey: Self.modelIdKey(p)) ?? ""
    }

    /// Outcome of a Save-Key action — distinct from a plain Bool so
    /// the UI can tell "you didn't type anything" (a hint, not a
    /// Keychain failure) apart from an actual write error.  Deletion
    /// is `clearApiKey`'s job, not an empty-string Save (see the
    /// hardening note on `saveApiKey`).
    enum SaveKeyResult {
        case saved
        case emptyInput
        case keychainError
    }

    /// Store the Keychain key for `p`.  An empty/whitespace-only
    /// `key` is a NO-OP that reports `.emptyInput` — it does NOT
    /// delete anything (that's `clearApiKey`'s job; Save meaning
    /// "delete" on empty text was a foot-gun: a stray extra click
    /// after the field is cleared, or the Clear/Save state simply
    /// looking similar, must never silently wipe a stored key).
    @discardableResult
    func saveApiKey(_ key: String, for p: AgentChatProviderChoice) -> SaveKeyResult {
        let trimmed = key.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return .emptyInput }
        let ok = AgentChatKeychain.write(account: p.keychainAccount, secret: trimmed)
        if ok {
            // Seed the cache directly with the just-saved value so the
            // very next resolveApiKey() call (the next HTTP round, or
            // an immediate Retry) is served from memory — zero
            // Keychain reads, hence zero prompts, after a successful
            // save.  On failure the cache is left untouched: whatever
            // was resolving before (Keychain or env) keeps resolving.
            cachedApiKeys[p] = trimmed
        }
        keyStateEpoch += 1
        return ok ? .saved : .keychainError
    }

    /// Delete the stored Keychain key for `p` (the Clear Key action).
    /// Returns false only on an actual Keychain error; an
    /// already-absent item counts as success (matches
    /// `AgentChatKeychain.delete`).
    @discardableResult
    func clearApiKey(for p: AgentChatProviderChoice) -> Bool {
        let ok = AgentChatKeychain.delete(account: p.keychainAccount)
        // Drop the cached value regardless of outcome: even on a
        // (rare) Keychain delete failure, the user's intent was to
        // stop using this key, so the next resolve should re-probe
        // rather than keep serving the stale cached secret.  A
        // successful delete means the Keychain no longer has it
        // either way, so re-probing next time correctly falls through
        // to the env-var fallback (or "no key").
        cachedApiKeys.removeValue(forKey: p)
        keyStateEpoch += 1
        return ok
    }

    /// Resolution order (documented in the UI): Keychain first, then
    /// each of the provider's environment-variable fallbacks in
    /// order.  Returns the source WITHOUT the key so the UI can
    /// caption it; the key itself is fetched separately at USE time
    /// and never retained.
    func keySource(for p: AgentChatProviderChoice) -> AgentChatKeySource {
        // Presence check only (no kSecReturnData) — view-body
        // re-renders call this; the secret is materialized solely at
        // USE time in resolveApiKey().
        if AgentChatKeychain.exists(account: p.keychainAccount) {
            return .keychain
        }
        for envVar in p.apiKeyEnvVars {
            if let env = getenv(envVar), env.pointee != 0 {
                return .environment(envVar)
            }
        }
        return .none
    }

    /// The key to use RIGHT NOW (Keychain first, then each env-var
    /// fallback in order), or nil.  Never logged.
    ///
    /// Cached in `cachedApiKeys` for the rest of the process's life
    /// once resolved (see that property's doc for the security
    /// posture) — this is what turns "one Keychain prompt per HTTP
    /// round" into "at most one Keychain prompt per provider per app
    /// run".  A miss (nil) is deliberately NOT cached: if the user has
    /// no key yet, the next call must re-probe so that setting a key
    /// mid-conversation (Save Key, which seeds the cache directly) or
    /// exporting an env var and relaunching recovers without a restart
    /// — see `saveApiKey`'s cache-seed for the in-app case.
    /// GUI stage 3: `for` defaults to the main chat provider (`self.provider`)
    /// so every pre-stage-3 call site is unchanged; the triage flow passes
    /// its OWN (possibly different) provider explicitly.  Same cache
    /// (`cachedApiKeys` is keyed by provider already), same Keychain-then-
    /// env-var resolution order.
    private func resolveApiKey(for lookupProvider: AgentChatProviderChoice? = nil) -> String? {
        let p = lookupProvider ?? provider
        if let cached = cachedApiKeys[p] {
            return cached
        }
        if let key = AgentChatKeychain.read(account: p.keychainAccount) {
            cachedApiKeys[p] = key
            return key
        }
        for envVar in p.apiKeyEnvVars {
            if let env = getenv(envVar) {
                let key = String(cString: env)
                if !key.isEmpty {
                    // Cache env-sourced keys too: harmless (getenv is
                    // not the thing prompting), and keeps the two
                    // sources consistent — one resolution per provider
                    // per run either way.
                    cachedApiKeys[p] = key
                    return key
                }
            }
        }
        return nil
    }

    // MARK: - Image attachments (Model-B F5)

    /// The picker affordance: choose one or more image files from disk
    /// and queue them as pending attachments.  The must-have per the
    /// slice brief; drag-drop (`handleDroppedFileURLs`) is the second,
    /// cheaper-to-add affordance sharing the same pipeline below.
    func attachImageFiles() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.png, .jpeg, .gif, .webP]
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.message = "Attach reference image(s)"
        guard panel.runModal() == .OK else { return }
        handleDroppedFileURLs(panel.urls)
    }

    /// Drag-and-drop entry point: the same accept/downscale/cap pipeline
    /// as the picker, so both affordances behave identically.  Called by
    /// ChatPanel's `.onDrop` handler with the file URLs SwiftUI already
    /// resolved from the drop's item providers.
    func handleDroppedFileURLs(_ urls: [URL]) {
        for url in urls {
            attachImageFile(at: url)
        }
    }

    /// Load, validate, downscale, and queue one image file.  On any
    /// rejection (unsupported type, unreadable/undecodable file) sets
    /// `attachmentError` to a user-visible message and queues nothing —
    /// never a silent drop.  Enforces the per-message cap
    /// (kMaxLiveUserImages, the SAME constant the loop caps the WHOLE
    /// conversation at) by refusing once the composer already holds that
    /// many.
    ///
    /// THREADING MODEL (P2-1 fix): the disk read (`Data(contentsOf:)`),
    /// CoreGraphics decode, box-filter downscale, and JPEG/PNG re-encode
    /// below can add up to real wall-clock time on a large source (a
    /// modern phone photo is routinely 10-40MB) — doing that
    /// SYNCHRONOUSLY on the MainActor, as this method used to, stalls
    /// the whole UI for the duration, ×N for a multi-file drag-drop
    /// batch.  This mirrors the render path's S2b fix (see
    /// `executeRenderToolCallAsync`'s doc): the actual work moves to a
    /// detached background context, and this method hops back to the
    /// MainActor ONLY to append the finished attachment (or report an
    /// error) — never to touch NSImage/CGContext/file I/O off-main
    /// itself, which stays fully on the detached task.
    ///
    /// The extension/UTType accept-reject gate and the per-message cap
    /// pre-check both run HERE, synchronously, before any dispatch —
    /// they're cheap (no bytes touched yet) and let a rejection surface
    /// immediately rather than after a round-trip through the background
    /// queue.  The cap is CHECKED AGAIN on the main-actor hop after the
    /// background work finishes, because a batch of attaches racing each
    /// other (e.g. a 6-file drag-drop) can each pass the pre-check before
    /// any of them has appended — the post-hop re-check is what actually
    /// enforces the cap under concurrency; the pre-check is purely a
    /// fast-path UX improvement (reject obviously-over-cap attempts
    /// without paying for a decode first).
    ///
    /// STALE-COMPLETION GUARD: `sceneGeneration` is captured before the
    /// detached work starts and re-checked on the main-actor hop.  If a
    /// scene closed/switched while this attach's background work was in
    /// flight, `sceneGeneration` will have moved (see `sceneOpened`/
    /// `sceneClosed`) — the finished attachment is DROPPED rather than
    /// appended to whatever composer is showing now, since it was
    /// composed against a scene that no longer exists.  `attachmentError`
    /// is likewise only set if the generation still matches (an error
    /// about a stale attach isn't useful to show against a new scene).
    func attachImageFile(at url: URL) {
        guard pendingAttachments.count < RISEAgentChatBridge.maxLiveUserImages else {
            attachmentError = "You can attach up to \(RISEAgentChatBridge.maxLiveUserImages) "
                + "images per message."
            return
        }

        // Gate on the file's EXTENSION-derived UTType — this is an
        // accept/reject check only ("is this a supported image type at
        // all?"), not a content sniff: UTType(filenameExtension:) reads
        // the extension string, so a file renamed to a different
        // extension is classified by that renamed extension, not by its
        // actual bytes.  That is fine here because this gate never
        // decides what MIME type rides the wire — `downscaleAndEncode`
        // below always re-encodes the decoded pixels itself and reports
        // the MIME type that matches ITS OWN output format (JPEG for
        // opaque content, PNG when alpha is present), so the sent bytes
        // and the declared `media_type` are always mutually consistent
        // regardless of what the source file was named.
        let sourceType = UTType(filenameExtension: url.pathExtension.lowercased())
        guard let sourceMimeType = Self.mimeType(for: sourceType),
              Self.acceptedMimeTypes.contains(sourceMimeType)
        else {
            attachmentError = "Unsupported image type"
                + (url.pathExtension.isEmpty ? "" : " “.\(url.pathExtension)”")
                + " — supported: PNG, JPEG, GIF, WEBP."
            return
        }

        let generation = sceneGeneration
        let fileName = url.lastPathComponent

        Task.detached { [weak self] in
            // Everything in this block runs OFF the MainActor: disk I/O,
            // CoreGraphics decode, downscale, re-encode, and thumbnail
            // generation are all plain value-in/value-out work with no
            // actor affinity of their own.
            guard let data = try? Data(contentsOf: url), let sourceImage = NSImage(data: data) else {
                await self?.finishAttach(
                    generation: generation,
                    result: .failure("Couldn’t read “\(fileName)” as an image."))
                return
            }
            guard let encoded = Self.downscaleAndEncode(sourceImage) else {
                await self?.finishAttach(
                    generation: generation,
                    result: .failure("Couldn’t process “\(fileName)” for attaching."))
                return
            }
            let thumbnail = Self.makeThumbnail(fromScaled: encoded.scaledImage)
            await self?.finishAttach(
                generation: generation,
                result: .success(PendingAttachment(
                    encodedData: encoded.data, mimeType: encoded.mimeType, thumbnail: thumbnail)))
        }
    }

    /// Outcome of the detached attach pipeline, handed back to the
    /// MainActor by `finishAttach`.
    private enum AttachOutcome {
        case success(PendingAttachment)
        case failure(String)
    }

    /// The MainActor-hop tail of `attachImageFile(at:)`.  Re-checks the
    /// scene-generation stale-completion guard and the per-message cap
    /// (both documented on `attachImageFile`'s doc) before mutating
    /// `pendingAttachments`/`attachmentError`.
    @MainActor
    private func finishAttach(generation: Int, result: AttachOutcome) {
        guard generation == sceneGeneration else {
            // The scene this attachment was composed against is gone —
            // drop it silently rather than leak it into a new scene's
            // composer or report a stale error against it.
            return
        }
        switch result {
        case .failure(let message):
            attachmentError = message
        case .success(let attachment):
            guard pendingAttachments.count < RISEAgentChatBridge.maxLiveUserImages else {
                attachmentError = "You can attach up to \(RISEAgentChatBridge.maxLiveUserImages) "
                    + "images per message."
                return
            }
            attachmentError = nil
            pendingAttachments.append(attachment)
        }
    }

    /// Drop one queued attachment (the composer row's ✕ button).
    func removePendingAttachment(_ id: UUID) {
        pendingAttachments.removeAll { $0.id == id }
    }

    /// UTType → provider MIME string.  Returns nil for anything not in
    /// the recognized set (the caller then rejects it) — this is
    /// intentionally narrower than UTType's own broad image hierarchy,
    /// since only these four have provider support (see
    /// AgentChatCodecs.h's allow-list comment).
    private static func mimeType(for type: UTType?) -> String? {
        guard let type else { return nil }
        if type.conforms(to: .png) { return "image/png" }
        if type.conforms(to: .jpeg) { return "image/jpeg" }
        if type.conforms(to: .gif) { return "image/gif" }
        if type.conforms(to: .webP) { return "image/webp" }
        return nil
    }

    /// Downscale `image` so its long edge is at most `maxAttachmentEdge`
    /// (never upscales a smaller source) and re-encode it — JPEG at
    /// `jpegQuality` for opaque content, PNG when the source carries an
    /// alpha channel (see `jpegQuality`'s doc for the reasoning).
    /// Returns nil only on a CoreGraphics failure (corrupt/unsupported
    /// source data NSImage nonetheless decoded a representation for).
    /// `nonisolated`: this touches no actor state (pure value in/out over
    /// CoreGraphics/AppKit APIs that are safe off-main), which is what
    /// lets `attachImageFile(at:)` call it from inside a `Task.detached`
    /// background context instead of blocking the MainActor.
    /// Also returns the downscaled `CGImage` it built (bounded to
    /// `maxAttachmentEdge`, i.e. at most 1024px on its long edge) so
    /// `attachImageFile(at:)` can build the composer/transcript thumbnail
    /// FROM THIS already-small image rather than from the full-res
    /// source — see `makeThumbnail`'s doc for why that bound matters even
    /// on its own failure path.
    private nonisolated static func downscaleAndEncode(
        _ image: NSImage
    ) -> (data: Data, mimeType: String, scaledImage: CGImage)? {
        guard let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            return nil
        }

        let srcWidth = CGFloat(cgImage.width)
        let srcHeight = CGFloat(cgImage.height)
        let longEdge = max(srcWidth, srcHeight)
        let scale = longEdge > maxAttachmentEdge ? (maxAttachmentEdge / longEdge) : 1.0
        let dstWidth = max(1, Int((srcWidth * scale).rounded()))
        let dstHeight = max(1, Int((srcHeight * scale).rounded()))

        let hasAlpha = cgImage.alphaInfo != .none
            && cgImage.alphaInfo != .noneSkipFirst
            && cgImage.alphaInfo != .noneSkipLast

        // Draw into a freshly-sized bitmap context (box-filter-quality
        // downscale via CoreGraphics' interpolation) even when scale is
        // 1.0 — a single code path re-encodes every attachment at the
        // chosen quality/format rather than special-casing "already
        // small enough" (which would pass the ORIGINAL, un-recompressed
        // bytes through — still correct, just a needless second code
        // path for a marginal size win).
        let colorSpace = hasAlpha ? CGColorSpaceCreateDeviceRGB() : CGColorSpaceCreateDeviceRGB()
        let bitmapInfo: CGBitmapInfo = hasAlpha
            ? CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
            : CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue)
        guard let context = CGContext(
            data: nil, width: dstWidth, height: dstHeight,
            bitsPerComponent: 8, bytesPerRow: 0,
            space: colorSpace, bitmapInfo: bitmapInfo.rawValue
        ) else { return nil }
        context.interpolationQuality = .high
        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: dstWidth, height: dstHeight))
        guard let scaledImage = context.makeImage() else { return nil }

        let rep = NSBitmapImageRep(cgImage: scaledImage)
        if hasAlpha {
            guard let png = rep.representation(using: .png, properties: [:]) else { return nil }
            return (png, "image/png", scaledImage)
        }
        guard let jpeg = rep.representation(
            using: .jpeg, properties: [.compressionFactor: jpegQuality]
        ) else { return nil }
        return (jpeg, "image/jpeg", scaledImage)
    }

    /// Build a SMALL thumbnail (long edge at most `thumbnailEdge`, never
    /// upscaled) for the composer strip / transcript row — this is what
    /// `PendingAttachment.thumbnail` actually stores, NOT the full-res
    /// source (P3-a: an append-only transcript holding full-res
    /// `NSImage`s per attachment is unbounded memory over a long
    /// conversation).  Same CGContext draw-down approach as
    /// `downscaleAndEncode` (safe to call off-main, unlike
    /// `NSImage.lockFocus`-based drawing, which requires the main
    /// thread).
    ///
    /// Takes the ALREADY-DOWNSCALED `scaledImage` `downscaleAndEncode`
    /// returned (bounded to `maxAttachmentEdge`, at most 1024px) rather
    /// than the raw source — this makes the memory bound hold even on
    /// this function's own failure path: if the second (thumbnail-sized)
    /// CGContext allocation fails, the fallback below returns the
    /// 1024px-bounded image, never the full-res original, so the P3
    /// memory bound cannot be silently defeated by a rare CoreGraphics
    /// allocation failure under memory pressure — exactly the situation
    /// where retaining a full-res fallback would be worst.
    /// `nonisolated` for the same reason as `downscaleAndEncode`: called
    /// from the `Task.detached` background context in
    /// `attachImageFile(at:)`.
    private nonisolated static func makeThumbnail(fromScaled cgImage: CGImage) -> NSImage {
        let boundedFallback = NSImage(
            cgImage: cgImage, size: CGSize(width: cgImage.width, height: cgImage.height))

        let srcWidth = CGFloat(cgImage.width)
        let srcHeight = CGFloat(cgImage.height)
        let longEdge = max(srcWidth, srcHeight)
        guard longEdge > thumbnailEdge else { return boundedFallback }
        let scale = thumbnailEdge / longEdge
        let dstWidth = max(1, Int((srcWidth * scale).rounded()))
        let dstHeight = max(1, Int((srcHeight * scale).rounded()))

        guard let context = CGContext(
            data: nil, width: dstWidth, height: dstHeight,
            bitsPerComponent: 8, bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue).rawValue
        ) else {
            return boundedFallback
        }
        context.interpolationQuality = .high
        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: dstWidth, height: dstHeight))
        guard let scaledThumbnail = context.makeImage() else { return boundedFallback }
        return NSImage(cgImage: scaledThumbnail, size: CGSize(width: dstWidth, height: dstHeight))
    }

    // MARK: The turn driver

    /// Send the composed input (and any queued image attachments) as a
    /// new user message and drive the turn to completion.  Either the
    /// text or at least one attachment must be present — an
    /// attachment-only message (no caption) is legitimate (Model-B F5).
    func send() {
        let text = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        let attachments = pendingAttachments
        guard !text.isEmpty || !attachments.isEmpty, !isBusy, viewportBridge != nil,
              sceneEditable() else { return }
        inputText = ""
        pendingAttachments = []
        attachmentError = nil
        clearErrorAffordances()

        // GUI stage 3 (prompt triage): eligibility, computed BEFORE this
        // message's own `.user` row is appended below -- a transcript
        // that ALREADY has a `.user` row means this is a later turn, not
        // the conversation's first message.  Images skip triage entirely
        // (v1 simplicity, per the feature brief).
        let isFirstUserMessage = !transcript.contains { $0.kind == .user }
        let shouldTriage = triageEnabled && isFirstUserMessage
            && attachments.isEmpty && !text.isEmpty

        transcript.append(Entry(
            kind: .user, text: text,
            attachmentThumbnails: attachments.map(\.thumbnail)))

        if shouldTriage {
            // runTriage decides what (if anything) actually reaches
            // chatBridge -- the original brief, optionally augmented
            // with clarifications/context, or nothing at all if the user
            // abandons a clarifying question.  See its doc.
            startTurnWithTriage(originalText: text)
            return
        }

        // AddUserMessage also flushes any tool calls abandoned by a
        // previous Stop with synthesized cancelled results — the
        // loop's designed interrupt recovery.
        if attachments.isEmpty {
            chatBridge.addUserMessage(text)
        }
        else {
            chatBridge.addUserMessage(text, attachments: attachments.map {
                RISEAgentChatAttachment(mimeType: $0.mimeType,
                                        base64Data: $0.encodedData.base64EncodedString())
            })
        }
        startTurn()
    }

    /// Retry after an Http-kind failure.  Nothing was recorded for the
    /// failed step, so BuildRequest re-sends the same conversation.
    /// (Deliberately NOT offered for MaxTokens / IterationCap — those
    /// need a new/narrower user message, per the ChatErrorKind
    /// contract.)
    func retry() {
        guard retryAvailable, !isBusy else { return }
        clearErrorAffordances()
        startTurn()
    }

    /// The Stop button: stop NOW.  The flag covers the synchronous
    /// stretches (checked between HTTP rounds and between tool
    /// calls); cancelling the Task additionally aborts an in-flight
    /// URLSession await immediately, so a Stop during the HTTP phase
    /// doesn't wait out the round-trip.  The catch path treats the
    /// resulting CancellationError as a silent stop, and nothing is
    /// recorded until handleResponse — contract-safe.  The loop's
    /// next flush synthesizes cancelled results for anything left
    /// pending.
    func requestStop() {
        guard isBusy else { return }
        stopRequested = true
        driverTask?.cancel()
        cancelAnyOutstandingChatRender()
        // Stage 1b: a pending ask_user question is exactly the kind of
        // synchronous-stretch-vs-await distinction this method's doc talks
        // about — the driver is suspended AWAITING the continuation, not
        // between HTTP rounds, so cancelling driverTask alone would leave
        // it parked forever.  Resuming with nil is what lets it observe
        // Task.isCancelled/stopRequested and unwind (see
        // `handleAskUserToolCall`'s doc); a no-op when no question is
        // pending.
        resumePendingQuestion(with: nil)
        transcript.append(Entry(kind: .notice, text: "Stopped."))
    }

    /// Stage 1b: answer a pending `ask_user` question by tapping one of
    /// its offered options — used VERBATIM (no trimming), matching the
    /// tool's documented contract ("the text of the option the user
    /// picked, verbatim"; see AgentChatCodecs.cpp's `ask_user`
    /// description).  A no-op if no question is pending (button already
    /// gone) or a race already resumed it (see `resumePendingQuestion`'s
    /// double-resume guard).
    func answerQuestion(option text: String) {
        resumePendingQuestion(with: text)
    }

    /// Stage 1b: answer a pending `ask_user` question with typed free
    /// text.  Empty/whitespace-only input is a no-op — there is nothing
    /// useful to send, and the Send button in the answer card is disabled
    /// on the same predicate.
    func answerQuestion(freeform text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        resumePendingQuestion(with: trimmed)
    }

    /// Stage 1b: resume the suspended `ask_user` continuation exactly
    /// once.  `answer == nil` means "no answer is coming" (Stop / scene
    /// close / a production render starting — see every `cancelTurn()`
    /// call site and `requestStop()` above); `handleAskUserToolCall`
    /// reads that as the same interrupted-tool-call signal every other
    /// mid-turn cancellation produces, and abandons without recording a
    /// result (the next `addUserMessage`'s flush synthesizes the
    /// cancelled result, same recovery as every other pending tool call).
    ///
    /// DOUBLE-RESUME GUARD: nils `pendingAnswerContinuation` BEFORE
    /// calling `.resume` — a `CheckedContinuation` traps if resumed
    /// twice, and two independent callers can legitimately race here (a
    /// button tap racing a Stop click, or Stop firing from both
    /// `requestStop()` and a `cancelTurn()` triggered by the same user
    /// gesture). Taking the continuation out of the property first means
    /// the SECOND caller finds nil and returns without touching it.
    private func resumePendingQuestion(with answer: String?) {
        guard let continuation = pendingAnswerContinuation else { return }
        pendingAnswerContinuation = nil
        pendingQuestion = nil
        continuation.resume(returning: answer)
    }

    /// Reset the conversation (offered after repeated HTTP 400s, and
    /// available from the settings popover).  Keeps provider/model/
    /// skill-index config; drops transcript + pending state.
    func resetConversation() {
        cancelTurn()
        chatBridge.reset()
        transcript = []
        lastReportedCompactedEntryCount = 0
        clearErrorAffordances()
        consecutiveHttp400s = 0
        transcript.append(Entry(kind: .notice, text: "Conversation reset."))
    }

    /// B2 review round 1: a production render is about to start
    /// (startRender / startAnimationRender call this BEFORE kicking
    /// the production rasterizer, mirroring the sceneClosed teardown
    /// discipline).  A turn suspended in its HTTP await would
    /// otherwise resume mid-render — its cancelled/stop/executor
    /// checks all passing — and execute tool calls against Scene
    /// state the production workers read off-main.  Cancelling here
    /// plus the driver's check-after-every-await of `sceneEditable`
    /// closes both interleaving orders (see driveTurn's invariant
    /// comment).  Nothing recorded mid-round needs cleanup: the next
    /// send's flush synthesizes cancelled results for pending tool
    /// calls, same as Stop.
    func productionRenderStarting() {
        guard isBusy else { return }
        cancelTurn()
        transcript.append(Entry(
            kind: .notice,
            text: "Turn cancelled — a production render started."))
    }

    private func clearErrorAffordances() {
        retryAvailable = false
        resetOffered = false
    }

    /// Cancel any in-flight turn Task (scene close / provider switch /
    /// manual reset).  The driver re-checks state after every await,
    /// so cancellation takes effect at the next suspension point.
    private func cancelTurn() {
        stopRequested = true
        turnGeneration += 1
        driverTask?.cancel()
        driverTask = nil
        isBusy = false
        cancelAnyOutstandingChatRender()
        // Stage 1b: every caller of cancelTurn() (sceneOpened, sceneClosed,
        // applyProviderSelection, resetConversation, productionRenderStarting)
        // is one of the transcript-clear / turn-teardown sites that must not
        // leave a driveTurn() Task parked forever awaiting an answer nobody
        // will give — see `requestStop()`'s matching call for the full
        // rationale. No-op when no question is pending.
        resumePendingQuestion(with: nil)
    }

    /// Model-B F2 slice S2b: trip `render_cancel` on the controller if a
    /// chat-driven `render` tool call is currently outstanding on its
    /// single-slot async worker.  Called from every path that ends a turn
    /// early (Stop, productionRenderStarting → cancelTurn, sceneClosed →
    /// cancelTurn, provider switch → cancelTurn, manual reset →
    /// cancelTurn) so a NEWER intent (most importantly: the user clicking
    /// Render) never has to wait behind an agent render queued on the
    /// SAME single-slot worker (SceneEditController::SubmitAgentRenderAsync's
    /// single-slot policy would otherwise serialize them).
    ///
    /// Fire-and-forget: `agentHandleToolCall` is synchronous but render_cancel
    /// itself does not block for the render's duration (it only trips the
    /// shared cancel flag — see AgentSession::CancelAsyncRender's doc), so
    /// this call returns promptly regardless of whether a render is
    /// actually running.  Safe to call with nothing outstanding (id 0 —
    /// render_cancel's `renderJobId` param is optional) and safe to call
    /// with no viewport bridge attached (guarded below).
    ///
    /// Clears only the cancellation id immediately. The distinct published
    /// occupancy state stays true until a cancellation-safe poll observes the
    /// worker has actually drained; otherwise controls could re-enable and
    /// synchronously block on the controller mutex in the cancellation gap.
    private func cancelAnyOutstandingChatRender() {
        guard outstandingChatRenderJobId != 0 else { return }
        let jobId = outstandingChatRenderJobId
        // The job's pinned SESSION SELECTION — render_cancel and the drain
        // poll that follows must reach the session that RAN this job, not
        // whichever session the autonomy chip happens to select right now:
        // the drain's render_wait only sees this job's cached `result` on
        // that session (see `outstandingChatRenderAutonomy`).  The live
        // autonomy posture still applies to the pinned session.
        let autonomy = outstandingChatRenderAutonomy
        outstandingChatRenderJobId = 0
        guard let vb = viewportBridge else {
            // No live bridge remains that UI can call. A subsequent
            // sceneOpened happens only after its old bridge was shut down.
            return
        }
        let line = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"render_cancel\"," +
                   "\"params\":{\"renderJobId\":\(jobId)}}"
        // Response intentionally discarded: its `cancelled:true` means only
        // "routed through a live controller", NOT "something was stopped"
        // (see AgentRpc.cpp's render_cancel doc — `cancelled` mirrors
        // `HasController()`, not whether a render was actually in flight);
        // this call site already knows a job WAS outstanding (the guard
        // above), and the fire-and-forget contract means we don't wait to
        // find out whether it actually stopped.
        _ = vb.agentHandleToolCall(line, autonomy: autonomy)
        beginChatRenderDrain(jobId: jobId, bridge: vb, autonomy: autonomy)
    }

    /// Poll the already-cancelled worker without coupling it to the turn task:
    /// cancelling that task is precisely what leaves its normal render_wait
    /// loop early. Every dispatcher call remains on MainActor, preserving the
    /// bridge's single-caller contract.
    private func beginChatRenderDrain(jobId: UInt64, bridge vb: RISEViewportBridge,
                                      autonomy: RISEAgentAutonomyLevel) {
        guard chatRenderWorkerOccupied, chatRenderOccupancyJobId == jobId else { return }
        chatRenderDrainTask?.cancel()
        chatRenderDrainTask = Task { @MainActor [weak self, weak vb] in
            guard let self, let vb else { return }
            await self.waitForChatRenderDrain(jobId: jobId, bridge: vb, autonomy: autonomy)
        }
    }

    /// `autonomy` is the job's pinned SESSION SELECTION, threaded down from
    /// `cancelAnyOutstandingChatRender` — this loop must poll the session
    /// that RAN `jobId`, because only that session holds the job's cached
    /// completion result.  (The id resolves on any session attached to the
    /// controller; the cached result does not.)  The live autonomy posture
    /// still applies to the pinned session.
    private func waitForChatRenderDrain(jobId: UInt64, bridge vb: RISEViewportBridge,
                                        autonomy: RISEAgentAutonomyLevel) async {
        let sliceMs: UInt64 = 250
        while !Task.isCancelled && chatRenderOccupancyJobId == jobId {
            // sceneClosed() detaches this bridge before shutdown. Its
            // synchronous shutdown joins the worker; sceneOpened() then
            // resets the stale occupancy state before binding a new bridge.
            guard viewportBridge === vb else { return }
            let waitLine = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"render_wait\"," +
                "\"params\":{\"renderJobId\":\(jobId),\"timeoutMs\":0}}"
            let waitResponse = vb.agentHandleToolCall(waitLine, autonomy: autonomy)
            if Self.renderWaitCompleted(waitResponse) {
                finishChatRenderOccupancy(jobId: jobId)
                return
            }
            try? await Task.sleep(nanoseconds: sliceMs * 1_000_000)
        }
    }

    private func finishChatRenderOccupancy(jobId: UInt64) {
        guard chatRenderOccupancyJobId == jobId else { return }
        chatRenderOccupancyJobId = 0
        chatRenderWorkerOccupied = false
        chatRenderDrainTask = nil
    }

    private static func renderWaitCompleted(_ response: String) -> Bool {
        guard
            let data = response.data(using: .utf8),
            let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
            let result = object["result"] as? [String: Any],
            let completed = result["completed"] as? Bool
        else { return false }
        return completed
    }

    private func startTurn() {
        stopRequested = false
        isBusy = true
        turnGeneration += 1
        let generation = turnGeneration
        driverTask = Task { [weak self] in
            await self?.driveTurn()
            // Only the CURRENT turn's tail may clear the busy state —
            // a cancelled turn resuming late must not stomp a newer one.
            guard let self, self.turnGeneration == generation else { return }
            self.isBusy = false
            self.driverTask = nil
        }
    }

    /// GUI stage 3 (prompt triage): like `startTurn()`, but runs
    /// `runTriage` FIRST — same `driverTask`/`turnGeneration`/`isBusy`
    /// bookkeeping, so the composer stays disabled and Stop works
    /// identically whether the user is mid-triage-question or
    /// mid-main-turn.  `runTriage` returning false means it was
    /// abandoned (a nil answer to a clarifying question) — the main
    /// turn never starts and nothing was sent.
    private func startTurnWithTriage(originalText: String) {
        stopRequested = false
        isBusy = true
        turnGeneration += 1
        let generation = turnGeneration
        driverTask = Task { [weak self] in
            guard let self else { return }
            let proceed = await self.runTriage(originalText: originalText)
            if proceed, !Task.isCancelled, !self.stopRequested,
               self.turnGeneration == generation {
                await self.driveTurn()
            }
            // Only the CURRENT turn's tail may clear the busy state —
            // a cancelled turn resuming late must not stomp a newer one.
            guard self.turnGeneration == generation else { return }
            self.isBusy = false
            self.driverTask = nil
        }
    }

    /// The B2-review scene-editable gate, applied at the same points
    /// as the cancellation checks.  On failure it ends the turn with
    /// a user-visible error row; nothing recorded mid-round needs
    /// cleanup (the next send's flush synthesizes cancelled results —
    /// same recovery as Stop).
    private func sceneEditableOrReportRenderConflict() -> Bool {
        if sceneEditable() { return true }
        transcript.append(Entry(
            kind: .error,
            text: "A production render is running — wait for it to "
                + "finish, then send again."))
        return false
    }

    /// One conversation turn: HTTP round-trips + tool rounds until the
    /// model finishes with text, errors, or the user stops.  Runs on
    /// the main actor; the URLSession await leaves the main thread, and
    /// so do BOTH of the in-loop suspension families — a `render` tool
    /// call's render_wait polling loop (Model-B F2 slice S2b) and a
    /// refused EDIT call's retry backoff (FIX 2) — see
    /// executeRenderToolCallAsync's doc
    /// for why that is same-actor SLICING (short synchronous
    /// `agentHandleToolCall` calls separated by `await Task.sleep`), not a
    /// second thread calling the dispatcher; `AgentRpcDispatcher::
    /// HandleLine` is documented single-caller, and every actual call
    /// into it — submit, each poll slice — still happens on this
    /// MainActor Task, never concurrently with anything else.  EVERY
    /// OTHER tool call still DISPATCHES SYNCHRONOUSLY on main via
    /// `agentHandleToolCall` (the 1c-1 contract: same thread the GUI's
    /// own SetProperty edits drive) — they are fast CST reads/edits
    /// with no render-duration cost to amortize.  ONE REFINEMENT (FIX
    /// 2): an EDIT verb whose first dispatch comes back a
    /// wholly-unapplied retriable refusal is re-dispatched up to
    /// `editRetryMaxAttempts` times by `retryWhollyRefusedEditToolCall`
    /// — the SAME same-actor slicing shape as the render path (each
    /// individual dispatch is still one synchronous MainActor call into
    /// the single-caller dispatcher; only the gaps between them are
    /// `await Task.sleep`).  Every other verb, and every edit call that
    /// is not refused, is still exactly one shot.
    ///
    /// PRODUCTION-RENDER INVARIANT (B2 review round 1; refined S2b):
    /// tool calls must never execute while the production rasterizer's
    /// workers read Scene state off-main.  `renderState` is @MainActor
    /// state and every stretch of this driver between suspension points
    /// is one synchronous MainActor slice.  Pre-S2b the tool loop had NO
    /// awaits at all, so `startRender` (also MainActor) could only ever
    /// interleave at the outer URLSession await.  S2b's render_wait
    /// polling loop added the FIRST suspension points INSIDE the tool
    /// loop, and FIX 2's edit-refusal backoff added a second family of
    /// them — the invariant is preserved by re-checking the driver's
    /// standing gates after EVERY such suspension.  The gate SET is NOT
    /// identical across the three sites, and the difference is
    /// deliberate:
    ///   * render_wait slice (inside executeRenderToolCallAsync):
    ///     `Task.isCancelled`, `stopRequested`, `viewportBridge`, and
    ///     `productionRenderActive()` — the NARROWER predicate, because
    ///     `sceneEditable()` folds in `isChatRenderOutstanding` and this
    ///     loop would otherwise read its OWN in-flight render as a
    ///     conflict on its first slice (see `productionRenderActive`'s
    ///     doc);
    ///   * edit-retry backoff (inside retryWhollyRefusedEditToolCall):
    ///     the same three plus the FULL `sceneEditable()` — an edit call
    ///     has no render of its own to be confused by, and it must not
    ///     re-dispatch a MUTATION while ANY render (production or
    ///     chat-driven) owns the scene, so the stricter predicate is the
    ///     correct one here;
    ///   * the call site below, immediately after either awaited call
    ///     returns and before the result is handed to `addToolResult`:
    ///     the same three plus `sceneEditableOrReportRenderConflict()`
    ///     (`sceneEditable()` plus a user-visible error row).
    /// `startRender`'s `productionRenderStarting()` additionally calls
    /// `cancelAnyOutstandingChatRender()` (via `cancelTurn()`), which
    /// trips `render_cancel` on the controller so a production render
    /// does not have to wait behind an agent render queued on the SAME
    /// single-slot worker — this closes the "resume-after-kick" order
    /// promptly rather than merely safely-but-slowly (the pre-S2b
    /// pattern already handled "safely"; S2b adds "promptly").
    private func driveTurn() async {
        while true {
            if Task.isCancelled || stopRequested { return }
            guard viewportBridge != nil else { return }
            guard sceneEditableOrReportRenderConflict() else { return }

            let apiKey: String
            if let resolved = resolveApiKey() {
                apiKey = resolved
            } else if !provider.requiresApiKey {
                // Keyless-by-design (.local): an empty key omits the
                // Authorization header entirely (OpenAIChatCodec::
                // Config::requiresAuth=false) — this is the expected
                // steady state, not an error.
                apiKey = ""
            } else {
                transcript.append(Entry(
                    kind: .error,
                    text: "No API key for \(provider.displayName).  Add one in "
                        + "the chat settings (stored in the Keychain) or launch "
                        + "with \(provider.apiKeyEnvVars.joined(separator: " or ")) set."))
                // The user message IS recorded; once a key is set,
                // Retry replays the same conversation safely via
                // BuildRequest.
                retryAvailable = true
                return
            }

            let request = chatBridge.buildRequest(apiKey: apiKey)
            // buildRequest is where span compaction runs, so this is the
            // moment to notice it did.  THIS DRIVER'S display transcript is
            // append-only and independent of the wire transcript, so a
            // dropped span costs it nothing visually -- which is precisely
            // the hazard: the panel keeps showing turns the model can no
            // longer see, and the resulting amnesia is indistinguishable
            // from a model defect.  Say so once per compaction event.
            // (The Windows panel renders the wire transcript directly and
            // therefore has the OPPOSITE symptom -- the turns visibly
            // disappear -- so its notice is worded differently.  Both are
            // guarded by SourceHygieneTest.)
            let droppedNow = chatBridge.compactedEntryCount
            if droppedNow > lastReportedCompactedEntryCount {
                // Report the DELTA, and name the running total separately --
                // the bridge counter is cumulative, so printing it bare would
                // read as additive on a second compaction event.
                let droppedJustNow = droppedNow - lastReportedCompactedEntryCount
                lastReportedCompactedEntryCount = droppedNow
                transcript.append(Entry(
                    kind: .notice,
                    text: "\(droppedJustNow) earlier turn(s) of history — messages and "
                        + "their tool results — were dropped from the model's memory to "
                        + "stay within the context budget (\(droppedNow) in total this "
                        + "conversation). They are still shown above, but the agent can "
                        + "no longer see them."))
            }
            guard !request.isEmpty, let url = URL(string: request.url) else { return }

            var urlRequest = URLRequest(url: url)
            urlRequest.httpMethod = "POST"
            urlRequest.timeoutInterval = 300
            for (name, value) in request.headers {
                urlRequest.setValue(value, forHTTPHeaderField: name)
            }
            urlRequest.httpBody = request.body.data(using: .utf8)

            let data: Data
            let status: Int
            let httpStarted = Date()
            var elapsedMs: Int64 = 0
            do {
                let (body, response) = try await URLSession.shared.data(for: urlRequest)
                data = body
                status = (response as? HTTPURLResponse)?.statusCode ?? 0
                elapsedMs = Int64(Date().timeIntervalSince(httpStarted) * 1000.0)
            } catch {
                if Task.isCancelled || stopRequested { return }
                // Transport-level failure: nothing reached the loop,
                // so nothing was recorded — retriable like an Http-
                // kind error.  (Message text only; never the body.)
                transcript.append(Entry(
                    kind: .error,
                    text: "Network error: \(error.localizedDescription)"))
                retryAvailable = true
                return
            }
            if Task.isCancelled || stopRequested { return }
            // The scene may have closed while the request was in
            // flight; the pending state flushes on the next turn.
            guard viewportBridge != nil else { return }
            // ...or a production render may have started while we
            // were suspended (see the invariant comment above; the
            // common order is already caught by the cancellation
            // check, this closes the rest).
            guard sceneEditableOrReportRenderConflict() else { return }

            let bodyString = String(data: data, encoding: .utf8) ?? ""
            // Eval-harness E1: record the LLM round (status/body/latency)
            // into the trajectory BEFORE handling it (no-op when recording
            // is off).  The bridge strips auth headers from the cached
            // request; the writer redacts every line unconditionally.
            chatBridge.recordHttpRound(status: status, body: bodyString, elapsedMs: elapsedMs)
            let step = chatBridge.handleResponse(status: status, body: bodyString)

            switch step.kind {
            case .toolCalls:
                consecutiveHttp400s = 0
                // GUI stage 2: the model's reasoning for THIS turn, ahead
                // of the assistant/tool rows it precedes on the wire —
                // see ChatStepResult::reasoningText's doc (Anthropic
                // `thinking` blocks, OpenAI-family `reasoning`/
                // `reasoning_content`; "" on Gemini and plain gpt turns).
                if !step.reasoningText.isEmpty {
                    transcript.append(Entry(
                        kind: .thinking, text: step.reasoningText,
                        isExpandedByDefault: detailedTranscriptEnabled))
                }
                if !step.assistantDisplayText.isEmpty {
                    transcript.append(Entry(kind: .assistant,
                                            text: step.assistantDisplayText))
                }
                for call in step.toolCalls {
                    // Stop / scene-close / scene-editable checks: these
                    // matter for the cancelTurn()-from-within-a-tool-call
                    // path (a tool side effect triggering scene teardown)
                    // AND for a real user Stop, which can now land inside
                    // this loop — the loop is NO LONGER awaitless (an
                    // async `render` slices at every render_wait poll, and
                    // FIX 2's edit-refusal retry slices at every backoff),
                    // so a click queued while we were suspended is
                    // delivered before the next iteration starts.  On a
                    // failure: abandon the rest; the loop synthesizes
                    // cancelled results for them at the next flush.
                    if Task.isCancelled || stopRequested { return }
                    guard let vb = viewportBridge else { return }
                    guard sceneEditableOrReportRenderConflict() else { return }

                    // Stage 1b (clarifying-questions): ask_user is a
                    // CHAT-LOOP-ONLY tool with no AgentRpc verb (see
                    // AgentChatCodecs.cpp's layout doc) -- the HOST
                    // intercepts it HERE, before toolCallToJsonRpcLine /
                    // agentHandleToolCall, exactly as AgentEvalRunner.cpp's
                    // headless interception does for stage 1a (that one
                    // always answers available:false; this one has a real
                    // interactive user to ask).  It gets NO "→ ask_user"
                    // activity row -- the question row
                    // `handleAskUserToolCall` appends IS its transcript
                    // representation -- so this branch never falls into the
                    // toolRowIndex/agentHandleToolCall machinery below.
                    if call.name == "ask_user" {
                        guard await handleAskUserToolCall(call) else { return }
                        continue
                    }

                    transcript.append(Entry(kind: .toolActivity,
                                            text: "→ \(call.name)"))
                    // GUI stage 2: this call's row — found BY INDEX (it
                    // was just appended last), not by re-scanning text,
                    // so the later in-place update can't mismatch a
                    // same-named earlier call still visible above it.
                    let toolRowIndex = transcript.count - 1
                    let line = chatBridge.toolCallToJsonRpcLine(call, rpcId: nextRpcId)
                    nextRpcId += 1

                    // Model-B F2 slice S2b: a `render` tool call goes
                    // through the NON-BLOCKING async path (submit +
                    // short-slice render_wait polling, all still on
                    // main/MainActor — see executeRenderToolCallAsync's
                    // doc for why this is a same-actor slicing scheme, NOT
                    // a second thread calling into the dispatcher).  Every
                    // OTHER tool call keeps the 1c-1 synchronous DISPATCH
                    // contract (they are all fast CST reads/edits with no
                    // render-duration cost to amortize) — with the one
                    // FIX-2 refinement noted in the `else` branch below: a
                    // refused EDIT verb is re-dispatched a bounded number
                    // of times, each dispatch still one synchronous
                    // MainActor call, with an `await` only in the gaps.
                    //
                    // SESSION-ROUTING INVARIANT (2026-07 fix): BOTH branches
                    // below now reach the SAME autonomy-selected session.
                    // `render` used to go to `agentHandleLine`'s separate
                    // administrative session while every other verb went
                    // through `agentHandleToolCall` — and because
                    // AgentSession's last-render PNG cache (`mLastPng` /
                    // `mLastSink`, AgentSession.cpp's RenderCore_) is
                    // PER-SESSION, the agent's `read_image` then read a cache
                    // its own `render` had never written: zero bytes, or the
                    // stale objectmap PNG left by `query_object_at`'s internal
                    // render.  Keep `render` and `read_image` on one session.
                    // (That second symptom has since been fixed at its own
                    // source too — `query_object_at`'s internal render is now
                    // stash/restore-guarded and no longer clobbers the cache;
                    // see AgentSession.cpp's EphemeralRenderCacheGuard.  It is
                    // named here because it is what the split ACTUALLY
                    // surfaced in production trajectories.)
                    //
                    // KNOWN RESIDUAL, deliberately not "fixed", and narrower
                    // than it first looks: only a chip flip that CROSSES
                    // Propose changes session.  Read and Apply both select
                    // the SAME tool-call Owner session
                    // (`_agentToolDispatcherOwner`; they differ only in the
                    // autonomy set on it), so a Read↔Apply flip between a
                    // render and the `read_image` that follows changes
                    // nothing.  A flip TO or FROM Propose does: that
                    // `read_image` lands on the other session and sees an
                    // empty/stale cache.  Pinning the whole TURN would close
                    // it, and we do not do that — autonomy is a safety
                    // control that must bind on the very next tool call, so a
                    // mid-turn drop to Read cannot be deferred to a turn
                    // boundary.  Only a render JOB pins its session (see
                    // `outstandingChatRenderAutonomy`), and even there the
                    // live posture still governs what the pinned session may
                    // do.  The model self-corrects here by re-rendering.
                    // Windows/Qt sibling of this routing site:
                    // build/VS2022/RISE-GUI/ChatPanel.cpp's
                    // processNextToolCall / startAsyncRenderToolCall.
                    let responseLine: String
                    if call.name == "render" {
                        responseLine = await executeRenderToolCallAsync(
                            call: call, submitLine: line, vb: vb)
                    } else {
                        // Each dispatch below is synchronous, on main —
                        // the 1c-1 executor contract.
                        // Agent autonomy selector: routes to whichever
                        // dispatcher matches the composer's CURRENT level
                        // (see RISEViewportBridge.h's `-agentHandleToolCall:`
                        // doc) — Read refuses edit verbs, Propose stages
                        // them, Apply commits them directly (today's
                        // behaviour).  `render` goes through the SAME
                        // selector, just via the pinned overload — see the
                        // SESSION-ROUTING INVARIANT comment in the `if`
                        // branch above.  (Round-15 P2: this pointer existed
                        // only on the Qt side; the two halves now match.)
                        let firstResponse = vb.agentHandleToolCall(line)
                        // FIX 2 (edit-refusal retry): if that first attempt
                        // came back a WHOLLY-UNAPPLIED retriable refusal
                        // ("editor transaction or gesture in progress"), do
                        // NOT burn an LLM round-trip on it — re-issue it
                        // here, yielding the MainActor between attempts so
                        // the blocking gesture can actually finish.  Returns
                        // `firstResponse` unchanged for every other verb and
                        // every other outcome.  See the method's doc for the
                        // correctness constraint that makes this safe.
                        responseLine = await retryWhollyRefusedEditToolCall(
                            call: call, line: line, vb: vb,
                            firstResponse: firstResponse)
                    }

                    // BOTH awaited paths above can suspend across Stop /
                    // scene-close / a production render starting — the
                    // render path's render_wait poll loop AND (FIX 2) the
                    // edit-refusal retry backoff;
                    // re-verify before touching shared driver state, same
                    // as every other post-await site in this file.  A
                    // failure here means the turn is already being torn
                    // down by whatever flipped the gate — abandon; the
                    // next send's flush synthesizes a cancelled result for
                    // this (and any later) pending call.
                    if Task.isCancelled || stopRequested { return }
                    guard viewportBridge != nil else { return }
                    guard sceneEditableOrReportRenderConflict() else { return }

                    // GUI stage 2: update the dispatch-time row IN
                    // PLACE now that a result line exists — covers ALL
                    // THREE paths (one-shot synchronous, async render,
                    // and FIX 2's retried edit), since they all funnel
                    // into the same `responseLine` above.
                    // The one-liner is the SAME deterministic summary
                    // (ToolOutcomeLineForDisplay) FlushPendingToolResults
                    // uses for the wire-side ChatToolDisplaySummary, so
                    // this row and the C++ transcript never disagree.
                    if toolRowIndex < transcript.count {
                        let outcome = chatBridge.toolOutcomeLine(for: call, resultLine: responseLine)
                        transcript[toolRowIndex].text = "→ \(call.name)  \(outcome)"
                        transcript[toolRowIndex].detailText =
                            "Args:\n" + Self.cappedForDetail(call.argsJson)
                            + "\n\nResult:\n" + Self.cappedForDetail(responseLine)
                    }

                    chatBridge.addToolResult(call, jsonRpcResponseLine: responseLine)
                }
                continue

            case .finalText:
                consecutiveHttp400s = 0
                // GUI stage 2: same reasoning-first ordering as the
                // .toolCalls branch above.
                if !step.reasoningText.isEmpty {
                    transcript.append(Entry(
                        kind: .thinking, text: step.reasoningText,
                        isExpandedByDefault: detailedTranscriptEnabled))
                }
                if !step.finalText.isEmpty {
                    transcript.append(Entry(kind: .assistant, text: step.finalText))
                }
                return

            case .providerError:
                handleProviderError(step, httpStatus: status)
                return

            @unknown default:
                return
            }
        }
    }

    /// Stage 1b (clarifying-questions): parsed `ask_user` arguments
    /// (mirrors the JSON-Schema in AgentChatCodecs.cpp's `ask_user`
    /// tool definition — `question` required, `options`/`allowFreeform`
    /// optional).
    private struct AskUserArgs {
        let question: String
        let options: [String]
        let allowFreeform: Bool
    }

    /// Parse one `ask_user` call's `argsJson`.  Returns nil on ANYTHING
    /// short of a well-formed object with a non-blank `question` string,
    /// OR an unanswerable card: `allowFreeform:false` requires a valid
    /// 2...5-item options list.  This latter invariant is load-bearing:
    /// without it, an optional/malformed options array plus
    /// `allowFreeform:false` would render neither choice buttons nor a text
    /// field while the ordinary composer is disabled for `pendingQuestion`.
    /// When freeform remains allowed, invalid options safely degrade to a
    /// freeform-only question instead of trapping the user on a malformed
    /// model call.
    private static func parseAskUserArgs(_ argsJson: String) -> AskUserArgs? {
        guard let data = argsJson.data(using: .utf8),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let rawQuestion = obj["question"] as? String
        else { return nil }
        let question = rawQuestion.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !question.isEmpty else { return nil }
        let rawOptions = (obj["options"] as? [Any])?.compactMap { value -> String? in
            guard let text = value as? String else { return nil }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        } ?? []
        let options = (2...5).contains(rawOptions.count) ? rawOptions : []
        let allowFreeform = (obj["allowFreeform"] as? Bool) ?? true
        guard allowFreeform || !options.isEmpty else { return nil }
        return AskUserArgs(question: question, options: options, allowFreeform: allowFreeform)
    }

    /// Build the JSON-RPC success envelope `chatBridge.addToolResult`
    /// expects for an answered `ask_user` call: `{"jsonrpc":"2.0",
    /// "id":<rpcId>,"result":{"answer":"<text>"}}` — the exact shape
    /// AgentChatLoopTest.cpp's T39b/T39c hand-feed to `AddToolResult`.
    /// Goes through `JSONSerialization` (rather than hand-splicing
    /// `answer` into a string literal) so an answer containing quotes,
    /// backslashes, or newlines is escaped correctly.
    private static func askUserResultLine(rpcId: Int, answer: String) -> String {
        let obj: [String: Any] = ["jsonrpc": "2.0", "id": rpcId, "result": ["answer": answer]]
        guard let data = try? JSONSerialization.data(withJSONObject: obj),
              let line = String(data: data, encoding: .utf8)
        else {
            // A plain-string `answer` value is always JSON-representable
            // as UTF-8 -- this branch is unreachable in practice, but the
            // function stays total (an empty answer beats a crash).
            return "{\"jsonrpc\":\"2.0\",\"id\":\(rpcId),\"result\":{\"answer\":\"\"}}"
        }
        return line
    }

    /// Stage 1b (clarifying-questions): handle one `ask_user` call —
    /// parse its arguments, publish `pendingQuestion` so ChatPanel shows
    /// the answer card, suspend until the user answers (or the turn is
    /// torn down), and feed the result back to `chatBridge`.
    ///
    /// MALFORMED ARGS: synthesized as a JSON-RPC `error` result (code
    /// -32602, matching JSON-RPC's own "Invalid params") fed straight to
    /// `addToolResult` — never a crash, never a transcript row (there is
    /// no question worth showing).  The model sees the error and can
    /// retry with valid arguments.
    ///
    /// CONTINUATION LIFECYCLE: `pendingAnswerContinuation` is created by
    /// the `withCheckedContinuation` call below and consumed EXACTLY ONCE
    /// by `resumePendingQuestion(with:)` — either with the user's answer
    /// (`answerQuestion(option:)` / `answerQuestion(freeform:)`, driven by
    /// ChatPanel's answer card) or with `nil` (`requestStop()` / every
    /// `cancelTurn()` call site: scene close, provider switch, manual
    /// reset, a production render starting).  `resumePendingQuestion`'s
    /// own doc has the double-resume guard.
    ///
    /// Returns `false` when the turn is being torn down (a nil answer, OR
    /// Stop/scene-close/a-render-starting fired in the gap between resume
    /// and this Task actually running again — the SAME post-await
    /// re-verification every other suspension point in `driveTurn` does)
    /// — the caller's `guard ... else { return }` mirrors every other
    /// post-await site in this file. On `false`, nothing is recorded for
    /// this call: the next `addUserMessage`'s flush synthesizes the
    /// cancelled result, the SAME recovery path every other interrupted
    /// tool call uses.
    private func handleAskUserToolCall(_ call: RISEAgentChatToolCall) async -> Bool {
        let rpcId = nextRpcId
        nextRpcId += 1

        // Stamp the trajectory's request record + latency stash EXACTLY like
        // the normal dispatch path does (line ~1999). We discard the returned
        // JSON-RPC line — ask_user is never dispatched to the scene — but the
        // call's side effect is load-bearing: without it, every recorded
        // ask_user `tool` record carries an EMPTY jsonRpcRequest and a latency
        // of 0ms, which reads as "instant" when the round-trip actually took
        // however long the user thought about the answer. (Review P2; the
        // eval runner's interception already stamps it — this mirrors that.)
        _ = chatBridge.toolCallToJsonRpcLine(call, rpcId: rpcId)

        guard let args = Self.parseAskUserArgs(call.argsJson) else {
            let errorLine = "{\"jsonrpc\":\"2.0\",\"id\":\(rpcId),\"error\":" +
                "{\"code\":-32602,\"message\":\"ask_user: malformed arguments\"}}"
            chatBridge.addToolResult(call, jsonRpcResponseLine: errorLine)
            return true
        }

        guard let answerText = await askQuestionAndAwaitAnswer(
            question: args.question, options: args.options, allowFreeform: args.allowFreeform)
        else { return false }

        chatBridge.addToolResult(
            call, jsonRpcResponseLine: Self.askUserResultLine(rpcId: rpcId, answer: answerText))
        return true
    }

    /// GUI stage 3 (prompt triage): the shared "ask one question, await
    /// the answer via the pendingQuestion/answer-card mechanism"
    /// primitive — factored out of `handleAskUserToolCall` so
    /// `runTriage`'s clarifying-question loop drives the SAME card
    /// without a second parallel implementation.  Appends the
    /// `.question` transcript row, suspends until the user answers (or
    /// the turn is torn down), and — only on a real answer — appends
    /// the matching `.user` row.  Mirrors `handleAskUserToolCall`'s
    /// pre-refactor shape exactly (see that method's CONTINUATION
    /// LIFECYCLE doc, still accurate): `pendingAnswerContinuation` is
    /// consumed exactly once by `resumePendingQuestion(with:)`, either
    /// with the user's answer or with `nil` (Stop / scene close / a
    /// production render starting).
    ///
    /// Returns nil when the turn is being torn down (a nil answer, or
    /// Stop/scene-close/a-render-starting raced the resume) — the SAME
    /// post-await re-verification every other suspension point in this
    /// file performs.  Callers treat nil exactly like
    /// `handleAskUserToolCall` treats `false`: abandon, record nothing
    /// for this step, and let the next flush/send synthesize recovery.
    private func askQuestionAndAwaitAnswer(
        question: String, options: [String], allowFreeform: Bool
    ) async -> String? {
        transcript.append(Entry(kind: .question, text: question))

        let answer: String? = await withCheckedContinuation { continuation in
            pendingAnswerContinuation = continuation
            pendingQuestion = PendingQuestion(
                question: question, options: options, allowFreeform: allowFreeform)
        }

        guard let answerText = answer,
              !Task.isCancelled, !stopRequested,
              viewportBridge != nil,
              sceneEditableOrReportRenderConflict()
        else { return nil }

        transcript.append(Entry(kind: .user, text: answerText))
        return answerText
    }

    // MARK: - Prompt triage (GUI stage 3 of clarifying-questions)

    /// The triage assistant's system prompt — shipped VERBATIM via
    /// `RISEAgentChatBridge.setSystemPromptOverride` (see that method's
    /// doc: no base co-editing prompt, no skills section rides
    /// alongside it).  Deliberately terse and JSON-only: this is a
    /// single-turn classifier, not a conversational agent, so it gets
    /// none of the main prompt's scene-tool guidance.
    private static let triagePrompt =
        "You are a scene-brief triage assistant for a 3D renderer. Given a " +
        "user's scene request, decide if it is ambiguous in ways that " +
        "MATERIALLY change the scene (subject identity, setting/placement, " +
        "mood/lighting). Most briefs are fine as-is: when in doubt, do NOT " +
        "ask. Reply with ONLY a JSON object, no prose: {\"needsClarification\": " +
        "bool, \"questions\": [{\"question\": str, \"options\": [str,...]} up " +
        "to 2], \"augmentedPrompt\": str}. augmentedPrompt: the user's request " +
        "restated with any obvious defaults made explicit; empty string if no " +
        "augmentation helps. If needsClarification is false, questions must be []."

    /// One parsed `questions[]` entry of the triage verdict.
    private struct TriageQuestion {
        let question: String
        let options: [String]
    }

    /// The parsed triage verdict — mirrors the JSON shape `triagePrompt`
    /// mandates.
    private struct TriageVerdict {
        let needsClarification: Bool
        /// Capped at 2 entries regardless of how many the model returned
        /// (the prompt asks for "up to 2"; a hostile/confused model that
        /// sends more is silently truncated rather than treated as
        /// malformed — same "degrade the optional bits, don't fail the
        /// whole parse" spirit as `parseAskUserArgs`).
        let questions: [TriageQuestion]
        let augmentedPrompt: String
    }

    /// Parse the triage model's FinalText into `TriageVerdict`.  Strips
    /// ONE leading/trailing markdown code fence (```json ... ``` or
    /// plain ``` ... ```) before parsing — local/open models routinely
    /// wrap a "JSON only" reply in one despite the instruction.  Returns
    /// nil on ANY shape mismatch (not an object, `needsClarification`
    /// missing/non-bool) — `runTriage`'s fall-through-silently contract
    /// handles that; a per-question shape problem instead drops just
    /// that question (mirrors `parseAskUserArgs`'s per-field leniency).
    private static func parseTriageVerdict(_ rawText: String) -> TriageVerdict? {
        var text = rawText.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasPrefix("```") {
            if let firstNewline = text.firstIndex(of: "\n") {
                text = String(text[text.index(after: firstNewline)...])
            }
            if text.hasSuffix("```") {
                text = String(text.dropLast(3))
            }
            text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard let data = text.data(using: .utf8),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let needsClarification = obj["needsClarification"] as? Bool
        else { return nil }

        let rawQuestions = (obj["questions"] as? [[String: Any]]) ?? []
        let questions: [TriageQuestion] = rawQuestions.compactMap { q in
            guard let questionText = (q["question"] as? String)?
                    .trimmingCharacters(in: .whitespacesAndNewlines),
                  !questionText.isEmpty
            else { return nil }
            let options = (q["options"] as? [Any])?.compactMap { $0 as? String } ?? []
            return TriageQuestion(question: questionText, options: options)
        }
        let augmentedPrompt = (obj["augmentedPrompt"] as? String) ?? ""
        return TriageVerdict(needsClarification: needsClarification,
                              questions: Array(questions.prefix(2)),
                              augmentedPrompt: augmentedPrompt)
    }

    /// GUI stage 3: run the triage pre-pass on `originalText` (the
    /// user's just-sent, not-yet-forwarded brief) BEFORE the main turn.
    /// The ONE caller is `startTurnWithTriage`, itself only reached from
    /// `send()`'s eligibility gate (triage enabled, first user message,
    /// no attachments, non-empty text).
    ///
    /// FRESH BRIDGE, ONE TURN: constructs its own `RISEAgentChatBridge`
    /// (separate from `chatBridge`), configured from the triage settings
    /// + the SAME Keychain/env-var lookup the main loop uses (via
    /// `resolveApiKey(for:)`), with `setSystemPromptOverride(Self
    /// .triagePrompt)` so it never sees the co-editing prompt or tool
    /// guidance.  Drives exactly one AddUserMessage → BuildRequest →
    /// URLSession → HandleResponse round; never retries.
    ///
    /// FALL-THROUGH CONTRACT (triage must NEVER block or degrade the
    /// normal path): on every one of the following, `chatBridge
    /// .addUserMessage(originalText)` is called with the UNMODIFIED
    /// original text and this returns true, exactly as if triage were
    /// disabled —
    ///   * the triage provider's raw UserDefaults value no longer names
    ///     a real provider (defensive; `triageProvider` already falls
    ///     back to `.local`, so this is effectively unreachable),
    ///   * no resolvable API key for the triage provider,
    ///   * BuildRequest returns the documented empty request,
    ///   * a network/transport error,
    ///   * the HTTP round exceeds the 20s timeout (`urlRequest
    ///     .timeoutInterval`),
    ///   * the step is anything OTHER than FinalText (a tool-call step,
    ///     a ProviderError) — a triage model that tries to call a tool
    ///     is treated as a malformed reply, not driven further,
    ///   * FinalText whose text does not parse as the documented JSON
    ///     verdict shape (`parseTriageVerdict` returns nil).
    ///
    /// CLARIFICATION PATH: `needsClarification` with 1–2 questions
    /// drives them SEQUENTIALLY through `askQuestionAndAwaitAnswer` (the
    /// SAME pendingQuestion/answer-card mechanism `ask_user` uses — each
    /// question becomes a `.question` transcript row, each answer a
    /// `.user` row).  A nil answer (Stop / scene close / a production
    /// render starting) ABANDONS TRIAGE ENTIRELY: nothing is sent to
    /// `chatBridge` and this returns false — the caller does not start
    /// the main turn.  On a full set of answers, the composed wire text
    /// is `originalText + "\n\n[Clarifications the user provided: ...]"`
    /// (augmentedPrompt is IGNORED here — real answers beat guesses),
    /// followed by a `.notice` row ("Triage asked N questions") so the
    /// behaviour is visible without cluttering the visible `.user` row
    /// (which stays the user's own words, appended by `send()` before
    /// this function ever runs).
    ///
    /// CONTEXT-ONLY PATH: `needsClarification == false` with a non-empty
    /// `augmentedPrompt` composes `originalText` + a bracketed background note
    /// explicitly framed as informational-not-instructions (the triage model's
    /// output is MODEL-GENERATED, not user text -- review P2 injection framing) `+
    /// augmentedPrompt + "]"` — the ORIGINAL text always rides FIRST,
    /// never replaced (the user must recognize their own words in the
    /// transcript and on the wire).  A `.notice` row ("Triage added
    /// context") follows.
    ///
    /// KNOWN v1 GAP: the triage bridge has NO trajectory sink configured
    /// (it is a throwaway instance, never `startTrajectory`'d), so this
    /// one HTTP round-trip is NOT recorded to the eval-harness JSONL log
    /// — only the MAIN turn is (automatic: whatever composed text this
    /// function hands to `chatBridge.addUserMessage` is what the main
    /// bridge's own `user` trajectory record captures, and
    /// ComposeSystemPrompt on that side is unaffected by the triage
    /// bridge's override — the two bridges are fully independent
    /// objects).
    private func runTriage(originalText: String) async -> Bool {
        // Local, non-escaping, called only synchronously within this
        // method -- an ordinary (strong) capture is fine, it never
        // outlives this call.
        func sendUnmodified() -> Bool {
            chatBridge.addUserMessage(originalText)
            return true
        }

        let triageBridge = RISEAgentChatBridge()
        let triageProviderChoice = triageProvider
        let triageModel = triageModelId
        triageBridge.setProvider(triageProviderChoice.bridgeValue,
                                 modelId: triageModel.isEmpty ? nil : triageModel)
        triageBridge.setSystemPromptOverride(Self.triagePrompt)

        let apiKey: String
        if triageProviderChoice.requiresApiKey {
            guard let resolved = resolveApiKey(for: triageProviderChoice) else {
                return sendUnmodified()
            }
            apiKey = resolved
        } else {
            apiKey = ""
        }

        triageBridge.addUserMessage(originalText)
        let request = triageBridge.buildRequest(apiKey: apiKey)
        guard !request.isEmpty, let url = URL(string: request.url) else {
            return sendUnmodified()
        }

        var urlRequest = URLRequest(url: url)
        urlRequest.httpMethod = "POST"
        // Hard cap: a slow/unavailable triage model must never stall the
        // main turn — a timeout here falls through exactly like any
        // other triage failure.
        urlRequest.timeoutInterval = 20
        for (name, value) in request.headers {
            urlRequest.setValue(value, forHTTPHeaderField: name)
        }
        urlRequest.httpBody = request.body.data(using: .utf8)

        let data: Data
        let status: Int
        do {
            let (body, response) = try await URLSession.shared.data(for: urlRequest)
            data = body
            status = (response as? HTTPURLResponse)?.statusCode ?? 0
        } catch {
            // Covers both a real transport error AND the 20s timeout
            // above (URLSession surfaces a timeout as NSURLErrorTimedOut
            // through this same catch). A Stop mid-request must abandon
            // triage like any other cancellation, not fall through to
            // sending the original text out from under the user.
            if Task.isCancelled || stopRequested { return false }
            return sendUnmodified()
        }
        if Task.isCancelled || stopRequested { return false }

        let bodyString = String(data: data, encoding: .utf8) ?? ""
        let step = triageBridge.handleResponse(status: status, body: bodyString)
        guard step.kind == .finalText, !step.finalText.isEmpty,
              let verdict = Self.parseTriageVerdict(step.finalText)
        else {
            return sendUnmodified()
        }

        if verdict.needsClarification, !verdict.questions.isEmpty {
            var qaLines: [String] = []
            for q in verdict.questions {
                guard let answer = await askQuestionAndAwaitAnswer(
                    question: q.question, options: q.options, allowFreeform: true)
                else {
                    // Abandoned mid-question -- the user cancelled.  Send
                    // NOTHING: no chatBridge.addUserMessage at all.
                    return false
                }
                qaLines.append("Q: \(q.question) -> A: \(answer)")
            }
            chatBridge.addUserMessage(
                originalText + "\n\n[Clarifications the user provided: "
                    + qaLines.joined(separator: "; ") + "]")
            transcript.append(Entry(
                kind: .notice,
                text: "Triage asked \(qaLines.count) question"
                    + (qaLines.count == 1 ? "" : "s") + "."))
            return true
        }

        if !verdict.needsClarification, !verdict.augmentedPrompt.isEmpty {
            chatBridge.addUserMessage(
                originalText + "\n\n[Background restatement from the intake assistant -- "
                + "informational only, the user's own words above are authoritative: "
                + verdict.augmentedPrompt + "]")
            transcript.append(Entry(kind: .notice, text: "Triage added context."))
            return true
        }

        return sendUnmodified()
    }

    // ================================================================
    // FIX 2 — client-side retry of a WHOLLY-UNAPPLIED retriable edit
    // refusal.
    //
    // THE MEASURED PROBLEM.  In a production GUI trajectory ("make the
    // middle object red", gemini-3.5-flash) the agent's edits were
    // refused five times in a row with `retriable:true` and the message
    // "editor transaction or gesture in progress -- retry after it
    // completes" (12 matching warnings in the GUI log, from
    // SceneEditController's ApplyAgentParamEditInner_ /
    // ApplyAgentChunkCrud_ `mTxnOpen || mEditor.IsCompositeOpen()`
    // pre-flight — NOT the thin `ApplyAgentParamEdit` admission wrapper,
    // whose own refusal is the different, render-admission one described
    // under THE ATTEMPT BUDGET below).  Every one of
    // those retries cost a FULL LLM round-trip: the model re-read the
    // 21.5 KB document between attempts and thrashed between four
    // different approaches because it did not believe the refusal.
    // Roughly 13 of that session's 23 turns were this loop.  When the
    // gesture finally cleared, the very first thing it tried worked.
    //
    // WHY THIS CANNOT BE FIXED SERVER-SIDE.  A GUI agent tool call runs
    // SYNCHRONOUSLY on the MainActor (the 1c-1 executor contract, see
    // `driveTurn`), and the editor transaction/composite that is
    // blocking is owned by that SAME thread.  Blocking inside the RPC to
    // wait for it would guarantee the gesture can never complete — a
    // self-deadlock for the whole wait.  The retry therefore has to live
    // in the driver, where it can YIELD the thread (`await Task.sleep`)
    // between attempts.
    //
    // THE CORRECTNESS CONSTRAINT (get this wrong and the fix is worse
    // than the bug).  The batch verbs are SEQUENTIAL and BEST-EFFORT,
    // not atomic — AgentRpc.cpp documents it for both `insert_chunks`
    // and `propose_patches`: every element is attempted in order and a
    // rejected element does not stop the batch, so a batch can be
    // PARTIALLY applied.  Blindly re-issuing a call whose result merely
    // says `retriable` would DOUBLE-APPLY whatever already succeeded.
    // So `editRefusalIsWhollyUnapplied` retries ONLY when NOTHING was
    // applied — see its doc for the exact per-shape gate — and only
    // while the head has not moved under it, see
    // `refusalHeadVersionKey` and the HEAD-VERSION GUARD note on
    // `retryWhollyRefusedEditToolCall`.
    //
    // Windows/Qt sibling: build/VS2022/RISE-GUI/ChatPanel.cpp's
    // `scheduleEditToolCallRetry` / `editRefusalIsWhollyUnapplied` /
    // `refusalHeadVersionKey` (a
    // QTimer::singleShot chain rather than `await`, because Qt returns
    // to the event loop instead of suspending a Task — the retry gate,
    // the head guard, the attempt bound, the backoff table and the
    // driver stamp are deliberately identical, and
    // tests/SourceHygieneTest.cpp pins each of them on both platforms).
    // ================================================================

    /// The FIVE mutating verbs whose results carry the `applied` +
    /// `retriable` pair this retry gate reads — exactly the set
    /// `AgentRpc.cpp`'s `IsProposeSafeVerb` enumerates, and exactly the
    /// set whose result JSON is built by `ChunkResultJson` /
    /// `propose_patch`'s inline result / `propose_patches`' per-element
    /// result (the only three sites in AgentRpc.cpp that emit
    /// `retriable` at all).  Checked BEFORE parsing the response so a
    /// multi-megabyte `read_image` result is never JSON-parsed twice.
    /// tests/SourceHygieneTest.cpp derives this set from
    /// `IsProposeSafeVerb` and fails if the two drift.
    private static let retriableEditVerbs: Set<String> = [
        "propose_patch", "propose_patches", "insert_chunk", "insert_chunks", "remove_chunk"
    ]

    /// THE ATTEMPT BUDGET.  Total dispatch attempts for one refused edit
    /// call, the first included.  Five attempts spread over the backoff
    /// table below is ~2.25 s of added wall-clock in the WORST case,
    /// against a measured alternative of ~5 LLM round-trips (seconds
    /// each, plus a 21.5 KB document re-read per round).
    ///
    /// There are TWO retriable blocking conditions, not one, and the
    /// budget is sized for the first:
    ///   * an open editor TRANSACTION / composite
    ///     (`ApplyAgentParamEditInner_` / `ApplyAgentChunkCrud_`'s
    ///     `mTxnOpen || mEditor.IsCompositeOpen()` pre-flight) — a
    ///     human-scale gesture (a viewport drag, a property-slider
    ///     scrub) that ends when the user releases the mouse, well under
    ///     a second in the common case.  This is the measured failure
    ///     this fix is for, and 2.25 s comfortably covers it;
    ///   * the RENDER-ADMISSION gate (`mAgentRenderBlocksInteractive`,
    ///     checked by the thin `ApplyAgentParamEdit` /
    ///     `ApplyAgentInsertChunk` / `ApplyAgentRemoveChunk` wrappers) —
    ///     "render queued or in progress".  That clears on a RENDER
    ///     duration, which 2.25 s will usually NOT outlast, so a retry
    ///     of this kind normally exhausts the budget and hands the
    ///     refusal to the model anyway.  It costs at most the 2.25 s;
    ///     it is not the case the budget is tuned for.  (The permanently
    ///     LATCHED form of that same gate — set by
    ///     `~SceneEditController` / `PrepareForDestruction` and never
    ///     cleared — is reported `retriable:false` by the controller
    ///     precisely so it is never retried here.)
    ///
    /// Past ~2 s the block is no longer a transient gesture (a user is
    /// holding a drag, or a render is running), and the honest move is
    /// to hand the refusal to the model — which now also learns, from
    /// the stamp below, that the driver already tried.
    private static let editRetryMaxAttempts = 5

    /// Backoff BEFORE attempt i+1 (index i-1), in milliseconds.  Starts
    /// short so the overwhelmingly common "we caught the tail of a
    /// gesture" case costs a sixth of a second, then doubles so the
    /// total stays bounded and the controller's `mMutex` (which every
    /// attempt takes) is not hammered.  Sum = 2250 ms across the four
    /// gaps between five attempts.
    private static let editRetryBackoffMs: [UInt64] = [150, 300, 600, 1200]

    /// Is this tool result a refusal with **nothing applied** — the only
    /// shape a client-side retry may re-issue?
    ///
    /// SINGULAR verbs (`propose_patch`, `insert_chunk`, `remove_chunk`):
    /// `applied == false` AND `retriable == true` AND
    /// `status == "rejected"`.  The rejection contract guarantees the
    /// head is byte-identical on a reject (SceneEditController refuses
    /// BEFORE any mutation or park), so re-issuing is a true
    /// no-op-then-retry.
    ///
    /// BATCH verbs (`insert_chunks`, `propose_patches`): `applied` is a
    /// COUNT, and it must be 0, AND EVERY element must itself be
    /// `applied == false` + `retriable == true` + `status == "rejected"`.
    /// A PARTIALLY applied batch is handed to the model exactly as
    /// today — re-issuing it would double-apply the elements that
    /// already landed.
    ///
    /// WHY `status == "rejected"` IS TESTED EXPLICITLY (and is not
    /// redundant with `retriable`).  `applied` means CLEAN APPLY ONLY.
    /// A `status:"diagnosed"` outcome reports `applied:false` even
    /// though the Document WAS mutated and the live managers WERE
    /// replaced (SceneEditController's code-3 arm says so in as many
    /// words), so `{"applied":0,...,"results":[{"applied":false,
    /// "status":"diagnosed",...}]}` is a real shape in which the count
    /// is zero and the head HAS moved.  What excludes it today is only
    /// that all five `retriable = true` origins in SceneEditController
    /// happen to be pre-mutation, pre-park refusals — an observation
    /// about today's controller, not a contract, and nothing pins it.
    /// Requiring the status makes the exclusion STRUCTURAL: `diagnosed`,
    /// `conflict` and (should it ever gain `retriable`) `staged` all
    /// fall out on the status alone.  `staged` is the one with teeth:
    /// `SceneEditController::StageProposal` has NO dedupe, so a future
    /// `staged` + `retriable:true` would enqueue up to
    /// `editRetryMaxAttempts` identical proposals for the Owner to
    /// resolve.  Today the check is a no-op; that is the point.
    ///
    /// BATCH-FIRST ordering is DEFENCE IN DEPTH, not load-bearing.  A
    /// batch envelope carries no TOP-LEVEL `retriable` (AgentRpc.cpp
    /// emits only `{applied,total,results}`), so the singular arm would
    /// reject it on the missing key even if it ran first; and a
    /// PARTIALLY applied batch has `applied >= 1`, which bridges to
    /// `true`, not `false`.  What is real is the bridging hazard the
    /// ordering removes the need to reason about: `JSONSerialization`
    /// decodes JSON booleans and JSON numbers alike into `NSNumber`, and
    /// `NSNumber(0) as? Bool` succeeds as `false`.  Testing the batch
    /// shape first — and rejecting a CFBoolean in the count position
    /// below — means neither arm ever has to answer the other's
    /// question.
    private static func editRefusalIsWhollyUnapplied(_ responseLine: String) -> Bool {
        guard
            let data = responseLine.data(using: .utf8),
            let envelope = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
            envelope["error"] == nil,
            let result = envelope["result"] as? [String: Any]
        else { return false }

        // BATCH FIRST — see the BATCH-FIRST note above.
        if let results = result["results"] as? [[String: Any]] {
            guard
                !results.isEmpty,
                let appliedCount = result["applied"] as? NSNumber,
                // A JSON boolean ALSO bridges to NSNumber, so type-check
                // it out — the Qt sibling's `isDouble()` rejects a bool
                // outright and the two halves must not diverge on shape
                // validation.
                CFGetTypeID(appliedCount) != CFBooleanGetTypeID(),
                appliedCount.intValue == 0
            else { return false }
            for element in results {
                guard
                    let elementApplied = element["applied"] as? Bool, elementApplied == false,
                    let elementRetriable = element["retriable"] as? Bool, elementRetriable,
                    let elementStatus = element["status"] as? String, elementStatus == "rejected"
                else { return false }
            }
            return true
        }

        guard
            let applied = result["applied"] as? Bool, applied == false,
            let retriable = result["retriable"] as? Bool, retriable,
            let status = result["status"] as? String, status == "rejected"
        else { return false }
        return true
    }

    /// The refusal's reported head, canonicalized to `"<uuid>:<revision>"`
    /// — or `nil` when the response does not report one at all.
    ///
    /// This is the C2 guard's observable.  The retry is timed to land the
    /// instant the blocking gesture commits, which is EXACTLY when the
    /// head is most likely to have just moved, and — unlike the
    /// model-mediated retry this replaces — the driver does NOT re-read
    /// the document in between.  That matters because `insert_chunks` has
    /// no batch-fatal rule (`AgentSession::InsertChunks` gives only
    /// element 0 the caller's `baseHeadVersion` and attempts elements
    /// 1..N-1 unconditionally, unlike `ProposePatches`, which stops):
    /// re-issuing a batch whose head moved during the backoff returns
    /// `results[0].status:"conflict"` while the other N-1 chunks insert
    /// against a head the model never read.
    ///
    /// Singular results carry `headVersion` at the top of `result`;
    /// batch envelopes carry it per element, and a WHOLLY-refused batch
    /// refused every element against the same head, so element 0's is
    /// the batch's.  Returns `nil` on a missing/malformed field, which
    /// the caller treats as "cannot verify" and therefore "do not
    /// retry" — fail-safe, not fail-open.
    private static func refusalHeadVersionKey(_ responseLine: String) -> String? {
        guard
            let data = responseLine.data(using: .utf8),
            let envelope = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
            let result = envelope["result"] as? [String: Any]
        else { return nil }
        var holder = result
        if let results = result["results"] as? [[String: Any]], let first = results.first {
            holder = first
        }
        guard
            let head = holder["headVersion"] as? [String: Any],
            let uuid = head["uuid"] as? NSNumber,
            let revision = head["revision"] as? NSNumber
        else { return nil }
        // `doubleValue` (not `stringValue`) so the key does not depend on
        // which numeric NSNumber flavour JSONSerialization happened to
        // pick; AgentRpc.cpp's HeadVersionJson emits both fields as JSON
        // numbers.  Swift's Double description is deterministic, so equal
        // heads always produce equal keys.
        return "\(uuid.doubleValue):\(revision.doubleValue)"
    }

    /// OBSERVABILITY.  Stamp a DRIVER-ATTRIBUTED retry record onto the
    /// tool result the driver hands to `addToolResult`.
    ///
    /// The trajectory's `tool` record (AgentChatLoop.cpp's
    /// `AddToolResult`) stores the raw JSON-RPC response line verbatim
    /// as `jsonRpcResponse`, so stamping the response is what makes the
    /// retries visible in the trajectory JSONL.  `latencyMs` needs
    /// nothing: it is stamped at `toolCallToJsonRpcLine` (before attempt
    /// 1) and completed at `addToolResult` (after the last attempt), so
    /// it already spans the retries honestly.
    ///
    /// The model sees this too — the codecs pack `result` (not the
    /// envelope) into the tool_result block.  That is DELIBERATE and is
    /// why the key is namespaced `guiDriverRetry` and carries a note
    /// saying who added it: a model that learns the driver already spent
    /// five attempts should stop re-issuing the same call itself, which
    /// is the whole point of this fix.  Nothing is fabricated — every
    /// other field is the engine's own last-attempt result, carried
    /// through FIELD-FOR-FIELD.  Not byte-for-byte: this decodes and
    /// RE-SERIALIZES the envelope (Swift emits `Dictionary` hash order;
    /// the Qt sibling emits QJsonDocument's sorted-key order), so the
    /// line AgentChatLoop.cpp:~993 documents as "the raw JSON-RPC
    /// response line verbatim" is, on a stamped result only, a
    /// re-serialization of it — same keys, same values, possibly
    /// different key order and whitespace.  Stamping happens ONLY when
    /// at least one retry happened (`attempts > 1`), so the response
    /// really is byte-for-byte unchanged on the overwhelmingly common
    /// single-attempt path.
    private static func stampDriverRetry(onResponseLine line: String, attempts: Int) -> String {
        guard
            let data = line.data(using: .utf8),
            var envelope = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
            var result = envelope["result"] as? [String: Any]
        else { return line }
        result["guiDriverRetry"] = [
            "attempts": attempts,
            "note": "Added by the RISE GUI chat driver, not by the scene engine: this "
                + "call was refused as retriable with NOTHING applied, so the driver "
                + "re-issued it verbatim, yielding the GUI thread between attempts. "
                + "Only the last attempt's outcome is reported above; no earlier "
                + "attempt applied anything."
        ]
        envelope["result"] = result
        guard
            let outData = try? JSONSerialization.data(withJSONObject: envelope),
            let outLine = String(data: outData, encoding: .utf8)
        else { return line }
        return outLine
    }

    /// Re-issue an edit tool call that came back a wholly-unapplied
    /// retriable refusal, up to `editRetryMaxAttempts` attempts total,
    /// yielding the MainActor between them.  `firstResponse` is the
    /// already-executed attempt 1 from `driveTurn`'s dispatch site (kept
    /// there so the ordinary dispatch path — and the source-hygiene
    /// marker that pins it to `agentHandleToolCall` — stays visible at
    /// the call site, and so the Qt sibling can have the same shape).
    ///
    /// Returns `firstResponse` untouched for every non-edit verb and for
    /// every outcome that is not a wholly-unapplied retriable refusal.
    ///
    /// POST-AWAIT RE-VERIFICATION: `Task.sleep` is a real suspension
    /// point, so the driver's standing gates are re-checked before the
    /// next attempt: `Task.isCancelled`, `stopRequested`,
    /// `viewportBridge`, and the FULL `sceneEditable()`.
    ///
    /// That is NOT the same predicate `executeRenderToolCallAsync` uses
    /// at the top of each render_wait poll slice, and the difference is
    /// deliberate.  The render slice checks the NARROWER
    /// `productionRenderActive()` because `sceneEditable()` folds in
    /// `isChatRenderOutstanding`, which would make that loop read its OWN
    /// in-flight render as a conflict (see `productionRenderActive`'s
    /// doc).  A retried EDIT has no render of its own to be confused by,
    /// and it must not re-dispatch a MUTATION while ANY render — the
    /// production rasterizer or a chat-driven one — owns the scene, so
    /// the stricter `sceneEditable()` is the correct gate here.  It IS
    /// the same predicate `driveTurn` re-checks immediately after this
    /// call returns (there via `sceneEditableOrReportRenderConflict()`,
    /// which adds a user-visible error row).
    ///
    /// A Stop, a scene close, a production render starting, or a
    /// cancelled Task abandons the retry rather than resuming into a
    /// torn-down world; the caller's own guards then return without
    /// recording anything for this call, and the next send's flush
    /// synthesizes the cancelled result (the same recovery every other
    /// interrupted tool call uses).
    ///
    /// HEAD-VERSION GUARD (C2): the retry is timed to land the instant
    /// the blocking gesture commits — precisely when the head is most
    /// likely to have JUST moved — and the driver never re-reads the
    /// document across the backoff.  So a re-issue is admitted only
    /// while the refusal keeps reporting the SAME head it reported on
    /// attempt 1 (`refusalHeadVersionKey`); the first refusal that
    /// reports a different head ends the retry and is handed to the
    /// model, which re-reads before deciding.  A response that reports
    /// no head at all is treated as unverifiable and also ends the
    /// retry.  RESIDUAL, stated honestly: this cannot make the count
    /// zero.  The head can move DURING a backoff, and the driver has no
    /// way to observe that without issuing a read of its own, so ONE
    /// re-issue can still land against a moved head — the guard bounds
    /// it to one instead of `editRetryMaxAttempts - 1`.  Element 0 of a
    /// batch is still protected by its own `baseHeadVersion`
    /// precondition; elements 1..N-1 of an `insert_chunks` are not (see
    /// `refusalHeadVersionKey`'s doc for why `InsertChunks` differs from
    /// `ProposePatches` here).
    private func retryWhollyRefusedEditToolCall(
        call: RISEAgentChatToolCall, line: String, vb: RISEViewportBridge,
        firstResponse: String
    ) async -> String {
        guard Self.retriableEditVerbs.contains(call.name) else { return firstResponse }
        // Evaluated ONCE per response, here and at the bottom of the loop,
        // so the common (applied) path parses the envelope exactly once.
        guard
            Self.editRefusalIsWhollyUnapplied(firstResponse),
            let baselineHead = Self.refusalHeadVersionKey(firstResponse)
        else { return firstResponse }

        var responseLine = firstResponse
        var attempts = 1
        while attempts < Self.editRetryMaxAttempts {
            let backoffMs = Self.editRetryBackoffMs[
                min(attempts - 1, Self.editRetryBackoffMs.count - 1)]
            try? await Task.sleep(nanoseconds: backoffMs * 1_000_000)
            if Task.isCancelled || stopRequested { break }
            guard viewportBridge != nil else { break }
            guard sceneEditable() else { break }
            attempts += 1
            responseLine = vb.agentHandleToolCall(line)
            guard
                Self.editRefusalIsWhollyUnapplied(responseLine),
                Self.refusalHeadVersionKey(responseLine) == baselineHead
            else { break }
        }
        guard attempts > 1 else { return responseLine }
        return Self.stampDriverRetry(onResponseLine: responseLine, attempts: attempts)
    }

    /// Model-B F2 slice S2b: execute a `render` tool call WITHOUT
    /// blocking the UI for the render's duration.
    ///
    /// THREAD-CONTRACT FINDING THAT DRIVES THIS SHAPE: `AgentRpcDispatcher`
    /// (src/Library/Agent/AgentRpc.h) is documented "NOT thread-safe (slice
    /// 0c is single-threaded, matching AgentSession)" — HandleLine (which
    /// `agentHandleToolCall` calls straight through to on the calling
    /// thread) is a single-caller API, not a thread-safe one.  So the fix
    /// is NOT "poll render_status/render_wait from a second thread while
    /// the MainActor does something else" — that would be a second
    /// concurrent caller into the SAME dispatcher/session the MainActor
    /// might also be driving (e.g. the Agent JSON-RPC debug panel, or
    /// another tool call in a future parallel-calls world). The correct
    /// shape, and the one used here, is SAME-ACTOR SLICING: every
    /// `agentHandleToolCall` call in this method still runs synchronously
    /// on the MainActor (preserving the single-caller contract exactly),
    /// but the render itself runs on the controller's dedicated
    /// agent-render worker thread (submitted via `render{"async":true}`),
    /// so each individual `agentHandleToolCall` call this method makes is
    /// fast — a submit, or (S2b P2-2) a
    /// render_wait(timeoutMs:0) POLL-ONCE call that hits
    /// `WaitForRenderJob`'s "no wait" early return and so costs a single
    /// mutex-guarded status read, near-instant rather than up to
    /// timeoutMs — never a multi-second blocking call. Between slices we
    /// `await Task.sleep` for a small pause, which SUSPENDS this Task and
    /// returns control to the run loop — that's what keeps the UI
    /// responsive (button clicks, viewport interaction, SwiftUI redraws
    /// all run in that gap), not any actual parallelism against the
    /// dispatcher.  See `executeRenderToolCallAsync`'s poll-loop comment
    /// below for the full before/after of the per-cycle MainActor cost.
    ///
    /// SESSION ROUTING: every call this method makes goes through
    /// `agentHandleToolCall(_:autonomy:)` with the session PINNED at submit
    /// time — the same autonomy-selected session every OTHER tool call in
    /// the turn uses.  That is what keeps `read_image` reading the PNG
    /// cache THIS render populated (`AgentSession::mLastPng` is per-session).
    /// The pin keeps every poll on the session that RAN the job, which is
    /// where `render_wait`'s optional `result` payload lives — not because
    /// the `renderJobId` would otherwise be unresolvable (it resolves on any
    /// session attached to the same controller; the ids are minted BY the
    /// controller).  The pin fixes the SESSION, not the autonomy posture:
    /// the live posture still applies to the pinned session.  See the
    /// routing site in `driveTurn` for the residual, and
    /// `-agentHandleToolCall:autonomy:` in RISEViewportBridge.h for the full
    /// derivation.
    ///
    /// CONTRACT WITH THE LLM: the taught `render` tool schema
    /// (AgentChatCodecs.cpp) has no `async` parameter — the LLM never
    /// asks for async execution, and this method does not change what the
    /// LLM is taught. The DRIVER transparently upgrades every `render`
    /// call to the async wire shape, then downgrades the result back to
    /// the exact synchronous shape before handing it to
    /// `chatBridge.addToolResult` — RenderResultJson's shared field-by-
    /// field construction (AgentRpc.cpp) is what makes the synchronous
    /// `render` verb and `render_wait`'s post-completion `result` echo
    /// byte-for-byte the same shape, so the LLM cannot tell the
    /// difference.
    ///
    /// `submitLine` is the ALREADY-BUILT synchronous-shaped JSON-RPC line
    /// from `toolCallToJsonRpcLine` (id + method "render" + the LLM's
    /// params); this method parses it, injects `"async":true`, and
    /// re-serializes before submitting — never touching the LLM-authored
    /// param fields (width/height/camera/samples all pass through
    /// unchanged).
    ///
    /// Returns the JSON-RPC response line to feed into `addToolResult`.
    /// On any refusal/malformed-response/timeout-exhaustion path this
    /// returns an HONEST result (a JSON-RPC success envelope whose
    /// `result.ok` is false with an explanatory `message`, or the raw
    /// refusal line from the submit call) rather than silently
    /// fabricating success — the codec only inspects `error` vs `result`
    /// (AgentChatCodecs.cpp), so an `ok:false` result reads to the LLM as
    /// "the render did not succeed", which is the truth.
    private func executeRenderToolCallAsync(
        call: RISEAgentChatToolCall, submitLine: String, vb: RISEViewportBridge
    ) async -> String {
        chatRenderWillSubmit()

        // PIN the SESSION SELECTION for this render job's whole lifecycle,
        // captured ONCE, here, before the submit, so every later call about
        // this job (the render_wait polls below, and render_cancel / the
        // drain poll in cancelAnyOutstandingChatRender) reaches the session
        // that actually RAN it.  Not because the job id would otherwise be
        // unresolvable — it resolves on any session attached to the same
        // controller — but because render_wait's `result` payload and the
        // read_image PNG cache both live on the running session.
        //
        // This pins WHICH SESSION, not what that session may do: the live
        // autonomy posture still applies to it, so a mid-render drop to
        // Read is not defeated here.  And it is scoped to the JOB, never to
        // the turn.  See `outstandingChatRenderAutonomy` and
        // RISEViewportBridge.h's `-agentHandleToolCall:autonomy:` doc.
        let pinnedAutonomy = vb.agentAutonomyLevel
        outstandingChatRenderAutonomy = pinnedAutonomy

        // Inject {"async":true} into the params object.  A parse failure
        // here (should not happen — toolCallToJsonRpcLine always emits
        // well-formed JSON) falls back to the ORIGINAL synchronous call:
        // still correct, just blocking, which is strictly better than
        // fabricating a bogus response.  Still routed to the pinned
        // session, so the ReadImage cache it populates is the one a
        // following `read_image` tool call will read.
        guard let asyncLine = Self.injectAsyncTrue(intoJsonRpcLine: submitLine) else {
            return vb.agentHandleToolCall(submitLine, autonomy: pinnedAutonomy)
        }

        // Submit.  Synchronous but fast — this is a queue-and-return
        // call, not the render itself (see AgentSession::RenderAsync).
        let submitResponse = vb.agentHandleToolCall(asyncLine, autonomy: pinnedAutonomy)
        guard
            let submitData = submitResponse.data(using: .utf8),
            let submitObj = (try? JSONSerialization.jsonObject(with: submitData)) as? [String: Any]
        else {
            // Malformed response line — shouldn't happen (HandleLine
            // never throws / always emits valid JSON-RPC), but fail
            // honestly rather than crash on a force-unwrap.
            return submitResponse
        }
        if submitObj["error"] != nil {
            // Refused outright (no controller attached — e.g. a headless
            // context, though the Mac GUI always has one once a scene is
            // open) — surface the refusal verbatim; it is already a
            // well-formed JSON-RPC error envelope.
            return submitResponse
        }
        guard
            let result = submitObj["result"] as? [String: Any],
            let status = result["status"] as? String
        else {
            return submitResponse
        }
        if status != "submitted" {
            // "refused" — the single-slot worker is busy or an editor
            // transaction is open.  Deliver an HONEST ok:false result in
            // the SYNCHRONOUS shape (not the raw submit envelope, which
            // has a different shape than what the LLM is taught to
            // expect from `render`) so the model sees a clean failure
            // and can retry or adjust, exactly as a synchronous render
            // failure would read.
            let message = (result["message"] as? String) ?? "render refused"
            return Self.makeSyntheticRenderResultLine(ok: false, message: message)
        }
        guard let jobIdNumber = result["renderJobId"] as? NSNumber else {
            return Self.makeSyntheticRenderResultLine(
                ok: false, message: "render accepted but no renderJobId was returned")
        }
        let jobId = jobIdNumber.uint64Value

        // Track the outstanding job so cancelTurn / Stop /
        // productionRenderStarting can trip render_cancel on it without
        // waiting for THIS wait loop to notice.
        outstandingChatRenderJobId = jobId
        chatRenderOccupancyJobId = jobId
        chatRenderWorkerOccupied = true
        defer {
            // A cancellation path clears this id first so it cannot cancel a
            // later job. It must NOT clear worker occupancy: the dedicated
            // drain poll owns that transition after cancellation.
            if outstandingChatRenderJobId == jobId {
                outstandingChatRenderJobId = 0
            }
        }

        // Poll render_wait(timeoutMs:0) — a POLL-ONCE call, not a bounded
        // wait — separated by an `await Task.sleep`.  S2b P2-2: the prior
        // shape passed `timeoutMs:250` to render_wait, and because
        // `agentHandleToolCall` runs synchronously on the MainActor (see the
        // method doc), that made each slice's MainActor-blocking cost up
        // to 250ms — WaitForRenderJob's `wait_until` loop (SceneEditController.cpp)
        // really does block the calling thread for up to `timeoutMs`
        // waiting on its condvar.  At ~4Hz that read as UI jank for the
        // render's whole duration (button clicks / viewport interaction /
        // SwiftUI redraws all stalling on each slice).  `timeoutMs:0` hits
        // WaitForRenderJob's explicit "poll-once contract: no wait" early
        // return (`if( timeoutMs == 0 ) return false;` before any condvar
        // wait) — so this `agentHandleToolCall` call is now a single mutex-
        // guarded status read, not a wait, and the MainActor-blocking
        // portion of each cycle is near-instant. The actual pacing between
        // polls moves entirely to the `await Task.sleep` below, which
        // SUSPENDS this Task and returns control to the run loop — same
        // mechanism as before, just no longer sharing the interval with a
        // blocking call. 250ms between polls keeps the dispatcher from
        // being spammed while still reading as prompt; the outer loop has
        // no fixed cap (a render legitimately can take a long time) —
        // cancellation is what bounds it in practice: a Stop/scene-close/
        // production-render-start sets stopRequested / cancels the Task /
        // flips sceneEditable, and the check at the top of every slice
        // below returns promptly (the render itself keeps running on the
        // worker thread — cancelAnyOutstandingChatRender, called by all
        // three of those paths, is what actually tells the controller to
        // abort it).  Confirmed empirically (and by reading
        // WaitForRenderJob/RenderWait/the render_wait RPC handler in
        // AgentRpc.cpp) that timeoutMs:0 on an ALREADY-completed job still
        // reports `completed:true` plus the cached `result` sub-object —
        // the poll-once early return happens AFTER the "is it already
        // done" check, not instead of it — so the final-result fetch
        // below needs no separate call shape.
        let sliceMs: UInt64 = 250
        while true {
            if Task.isCancelled || stopRequested { return "" }
            guard viewportBridge != nil else { return "" }
            // NOT `sceneEditable()` — see `productionRenderActive`'s doc:
            // that combined predicate now correctly reports false for
            // THIS call's own outstanding render, which would make this
            // loop mistake itself for a conflict.  This narrower check
            // answers only "did a PRODUCTION render start?".
            guard !productionRenderActive() else { return "" }

            let waitLine = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"render_wait\"," +
                "\"params\":{\"renderJobId\":\(jobId),\"timeoutMs\":0}}"
            let waitResponse = vb.agentHandleToolCall(waitLine, autonomy: pinnedAutonomy)
            if let waitData = waitResponse.data(using: .utf8),
               let waitObj = (try? JSONSerialization.jsonObject(with: waitData)) as? [String: Any],
               let waitResult = waitObj["result"] as? [String: Any],
               let completed = waitResult["completed"] as? Bool, completed
            {
                // Done.  render_wait's `result` sub-object (present iff
                // completed AND this session cached that job's stats —
                // see AgentRpc.cpp's render_wait handler) carries the
                // EXACT synchronous-render shape.  Re-wrap it as a plain
                // JSON-RPC success envelope so it is indistinguishable
                // from what a synchronous `render` call would have
                // returned for this same tool call.
                finishChatRenderOccupancy(jobId: jobId)
                if let inner = waitResult["result"] as? [String: Any] {
                    return Self.makeSyntheticResponseLine(result: inner)
                }
                // completed==true but no cached result (shouldn't happen
                // for a job this method itself submitted — the session
                // that submitted it is the same one being polled here —
                // but degrade honestly rather than hang or crash).
                return Self.makeSyntheticRenderResultLine(
                    ok: false,
                    message: "render completed but no cached result was found")
            }
            // Not yet complete (or a transient parse hiccup) — sleep
            // briefly, then loop.  The `await` here is what actually
            // yields to the run loop between slices, and is now the ONLY
            // source of per-cycle delay (the poll itself above is near-
            // instant — see this loop's leading comment).
            try? await Task.sleep(nanoseconds: sliceMs * 1_000_000)
        }
    }

    /// Parse a JSON-RPC request line and set params.async = true,
    /// re-serializing.  Returns nil on any parse failure (caller falls
    /// back to the original line).
    private static func injectAsyncTrue(intoJsonRpcLine line: String) -> String? {
        guard
            let data = line.data(using: .utf8),
            var obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        else { return nil }
        var params = (obj["params"] as? [String: Any]) ?? [:]
        params["async"] = true
        obj["params"] = params
        guard
            let outData = try? JSONSerialization.data(withJSONObject: obj),
            let outLine = String(data: outData, encoding: .utf8)
        else { return nil }
        return outLine
    }

    /// Build a plain JSON-RPC success envelope {"jsonrpc":"2.0","id":0,
    /// "result":result} — the `id` value is never inspected by the
    /// chat codec (AgentChatCodecs.cpp only branches on `error` vs
    /// `result`), so a fixed placeholder id is honest here.
    private static func makeSyntheticResponseLine(result: [String: Any]) -> String {
        let obj: [String: Any] = ["jsonrpc": "2.0", "id": 0, "result": result]
        guard
            let data = try? JSONSerialization.data(withJSONObject: obj),
            let line = String(data: data, encoding: .utf8)
        else {
            // JSONSerialization on a dictionary of JSON-safe values
            // should not fail; degrade to a minimal honest error rather
            // than force-unwrap.
            return "{\"jsonrpc\":\"2.0\",\"id\":0,\"error\":" +
                "{\"code\":-32603,\"message\":\"internal error: failed to serialize render result\"}}"
        }
        return line
    }

    /// Build a synthetic render-shaped result (the SAME field set
    /// RenderResultJson emits in AgentRpc.cpp) for the honest-failure
    /// paths above, where only `ok` and `message` are known.  The other
    /// numeric/string fields default to the same zero/empty values
    /// AgentRenderResult's own default member initializers use, so a
    /// consumer that reads any of them sees a value consistent with "no
    /// render ran" rather than an absent key.
    private static func makeSyntheticRenderResultLine(ok: Bool, message: String) -> String {
        let result: [String: Any] = [
            "ok": ok,
            "width": 0, "height": 0,
            "meanR": 0.0, "meanG": 0.0, "meanB": 0.0,
            "integrator": "",
            "previewWidth": 0, "previewHeight": 0,
            "cameraOverridden": false,
            "message": message,
            "renderJobId": 0,
        ]
        return makeSyntheticResponseLine(result: result)
    }

    /// Per-kind error UX, honoring each ChatErrorKind's documented
    /// driver reaction (see RISEAgentChatBridge.h / AgentChatCodecs.h).
    private func handleProviderError(_ step: RISEAgentChatStep, httpStatus: Int) {
        switch step.errorKind {
        case .http:
            // The one retriable kind.  Track consecutive 400s — the
            // honest-poison-scoping recovery is a conversation Reset.
            if httpStatus == 400 {
                consecutiveHttp400s += 1
            } else {
                consecutiveHttp400s = 0
            }
            transcript.append(Entry(kind: .error, text: step.errorMessage))
            retryAvailable = true
            if consecutiveHttp400s >= 2 {
                resetOffered = true
                transcript.append(Entry(
                    kind: .notice,
                    text: "Repeated request rejections — the conversation may "
                        + "carry content the provider no longer accepts.  "
                        + "Reset the conversation to recover."))
            }

        case .refusal:
            consecutiveHttp400s = 0
            transcript.append(Entry(
                kind: .error,
                text: "The model declined this request: \(step.errorMessage)"))

        case .maxTokens:
            // Truncated reply was DISCARDED; a verbatim retry would
            // likely truncate again.  No Retry button — recovery is a
            // new, narrower user message.
            consecutiveHttp400s = 0
            transcript.append(Entry(
                kind: .error,
                text: "The reply hit the output-token limit and was discarded.  "
                    + "Try a narrower request in a new message."))

        case .iterationCap:
            // The per-turn tool-round cap tripped; the counter resets
            // on the next user message.
            consecutiveHttp400s = 0
            transcript.append(Entry(
                kind: .error,
                text: "Stopped after \(RISEAgentChatBridge.maxToolRoundsPerTurn) tool "
                    + "rounds in one turn.  Send a new message to continue."))

        case .parse, .provider:
            consecutiveHttp400s = 0
            transcript.append(Entry(kind: .error, text: step.errorMessage))

        case .misuse, .none:
            // Driver-contract violation — should not happen.  Log the
            // loop's own diagnostic to stderr (no bodies, no keys) and
            // show a generic error.
            consecutiveHttp400s = 0
            FileHandle.standardError.write(Data(
                "RISE agent chat: driver misuse: \(step.errorMessage)\n".utf8))
            transcript.append(Entry(
                kind: .error,
                text: "Internal chat error.  Reset the conversation if it persists."))
            resetOffered = true

        @unknown default:
            transcript.append(Entry(kind: .error, text: step.errorMessage))
        }
    }
}
