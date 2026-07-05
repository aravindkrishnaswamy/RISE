import SwiftUI
import Combine
import Security

// Facet 5 slice B2 — the LLM chat panel's driver.
//
// ALL correctness-bearing chat state lives in the C++ AgentChatLoop
// (wrapped by RISEAgentChatBridge); this file is the thin I/O driver
// the sans-IO core was designed for: it performs the HTTP round-trips,
// executes tool calls against the live scene via the viewport bridge's
// synchronous `agentHandleLine`, and renders a display transcript.
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
///   account: the provider name ("anthropic" / "gemini")
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

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .anthropic: return "Anthropic"
        case .gemini:    return "Gemini"
        }
    }

    var bridgeValue: RISEAgentChatProvider {
        switch self {
        case .anthropic: return .anthropic
        case .gemini:    return .gemini
        }
    }

    /// Keychain account name (kSecAttrAccount).
    var keychainAccount: String { rawValue }

    /// Environment-variable fallback(s) consulted at USE time, in
    /// order, when the Keychain has no key for this provider.  Gemini
    /// gets two names because both are in conventional use by Gemini
    /// tooling (GEMINI_API_KEY is Google's own AI SDK convention;
    /// GOOGLE_API_KEY is the older/more widely-scripted one) — first
    /// match wins.
    var apiKeyEnvVars: [String] {
        switch self {
        case .anthropic: return ["ANTHROPIC_API_KEY"]
        case .gemini:    return ["GEMINI_API_KEY", "GOOGLE_API_KEY"]
        }
    }

    /// Back-compat single-name accessor — used where only "the" env
    /// var name is needed (e.g. the no-key error hint's launch
    /// suggestion).  Always the first-checked name.
    var apiKeyEnvVar: String { apiKeyEnvVars[0] }

    var defaultModelId: String {
        RISEAgentChatBridge.defaultModelId(for: bridgeValue)
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
            case error
            case notice
        }
        let id = UUID()
        let kind: Kind
        let text: String
    }

    @Published private(set) var transcript: [Entry] = []
    @Published var inputText: String = ""
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

    // Provider settings.  MODEL IDS persist in UserDefaults; keys go
    // to the Keychain only.
    @Published private(set) var provider: AgentChatProviderChoice
    @Published private(set) var modelId: String
    /// Bumped whenever a key is saved/cleared so the settings UI
    /// re-derives the key-source caption.
    @Published private(set) var keyStateEpoch: Int = 0

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

    private let chatBridge = RISEAgentChatBridge()
    /// The per-scene tool executor.  WEAK: RenderViewModel owns the
    /// viewport bridge; on clearScene it calls `sceneClosed()` (which
    /// also nils this) BEFORE shutting the bridge down, so the driver
    /// never calls `agentHandleLine` on a torn-down scene.  Even if a
    /// stale call slipped through, `agentHandleLine` is total — it
    /// answers with a JSON-RPC -32603 error rather than crashing.
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

    /// Model-B F2 slice S2b: the renderJobId of a chat-driven `render`
    /// tool call currently outstanding on the LIVE controller's async
    /// worker (0 = none).  Set right after a successful async submit, in
    /// `executeRenderToolCallAsync`; cleared there (via `defer`) once
    /// that call's render_wait loop observes completion, AND cleared by
    /// `cancelAnyOutstandingChatRender()` the instant it fires a cancel
    /// (belt-and-suspenders — see that method's doc for why it cannot
    /// rely solely on the loop's own `defer`).  Read by
    /// `cancelAnyOutstandingChatRender()` so productionRenderStarting /
    /// cancelTurn / a user Stop can trip `render_cancel` on it WITHOUT
    /// waiting for the render to actually finish — the point being that a
    /// production render must not queue up behind a chat-driven agent
    /// render on the controller's single-slot worker (see
    /// SubmitAgentRenderAsync's single-slot policy).  MainActor-only,
    /// like every other piece of driver state in this class.
    private var outstandingChatRenderJobId: UInt64 = 0

    /// Model-B F2 slice S2b: true while a chat-driven `render` tool call
    /// has an async render outstanding on the controller's single-slot
    /// worker.  This is truth `renderState` (a pure GUI/Swift notion)
    /// cannot see on its own — an agent render does not flip `renderState`
    /// out of `.sceneLoaded`/`.completed` — so `RenderViewModel`'s single
    /// `isSceneEditableForAgents` predicate (the ONE gate both the chat
    /// driver and the Agent JSON-RPC debug panel consume) folds this in
    /// alongside its existing `renderState` switch: without it, the raw
    /// JSON-RPC panel could fire a `propose_patch` (or another render)
    /// concurrently with a chat-driven render still running on the same
    /// controller.  MainActor-only, like `outstandingChatRenderJobId`
    /// itself.
    var isChatRenderOutstanding: Bool { outstandingChatRenderJobId != 0 }

    private static let providerKey = "agentChat.provider"
    private static func modelIdKey(_ p: AgentChatProviderChoice) -> String {
        "agentChat.modelId.\(p.rawValue)"
    }

    init() {
        let storedProvider = UserDefaults.standard.string(forKey: Self.providerKey)
            .flatMap(AgentChatProviderChoice.init(rawValue:)) ?? .anthropic
        let storedModelId = UserDefaults.standard.string(
            forKey: Self.modelIdKey(storedProvider)) ?? ""
        provider = storedProvider
        modelId = storedModelId.isEmpty ? storedProvider.defaultModelId : storedModelId
        chatBridge.setProvider(storedProvider.bridgeValue, modelId: storedModelId)
    }

    // MARK: Scene lifecycle (driven by RenderViewModel)

    /// A scene finished loading: bind the tool executor, reset the
    /// conversation (it was about the previous scene), and fetch the
    /// skills index ONCE by driving the read_skill verb through the
    /// live dispatcher (per the SetSkillIndex contract — the loop
    /// stays sans-IO; the driver does the fetch).
    func sceneOpened(viewportBridge vb: RISEViewportBridge) {
        cancelTurn()
        viewportBridge = vb
        chatBridge.reset()
        transcript = []
        clearErrorAffordances()
        consecutiveHttp400s = 0

        let indexLine = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"read_skill\"}"
        chatBridge.setSkillIndex(
            Self.renderSkillIndex(fromRpcResponse: vb.agentHandleLine(indexLine)))
    }

    /// The scene is closing (clearScene / reload): stop any in-flight
    /// turn and drop the tool executor so the driver cannot touch the
    /// torn-down dispatcher.  MUST be called before the viewport
    /// bridge's shutdown.
    func sceneClosed() {
        cancelTurn()
        viewportBridge = nil
        chatBridge.reset()
        transcript = []
        inputText = ""
        clearErrorAffordances()
        consecutiveHttp400s = 0
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
        clearErrorAffordances()
        consecutiveHttp400s = 0
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
    private func resolveApiKey() -> String? {
        if let cached = cachedApiKeys[provider] {
            return cached
        }
        if let key = AgentChatKeychain.read(account: provider.keychainAccount) {
            cachedApiKeys[provider] = key
            return key
        }
        for envVar in provider.apiKeyEnvVars {
            if let env = getenv(envVar) {
                let key = String(cString: env)
                if !key.isEmpty {
                    // Cache env-sourced keys too: harmless (getenv is
                    // not the thing prompting), and keeps the two
                    // sources consistent — one resolution per provider
                    // per run either way.
                    cachedApiKeys[provider] = key
                    return key
                }
            }
        }
        return nil
    }

    // MARK: The turn driver

    /// Send the composed input as a new user message and drive the
    /// turn to completion.
    func send() {
        let text = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !isBusy, viewportBridge != nil,
              sceneEditable() else { return }
        inputText = ""
        clearErrorAffordances()
        transcript.append(Entry(kind: .user, text: text))
        // AddUserMessage also flushes any tool calls abandoned by a
        // previous Stop with synthesized cancelled results — the
        // loop's designed interrupt recovery.
        chatBridge.addUserMessage(text)
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
        transcript.append(Entry(kind: .notice, text: "Stopped."))
    }

    /// Reset the conversation (offered after repeated HTTP 400s, and
    /// available from the settings popover).  Keeps provider/model/
    /// skill-index config; drops transcript + pending state.
    func resetConversation() {
        cancelTurn()
        chatBridge.reset()
        transcript = []
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
    /// Fire-and-forget: `agentHandleLine` is synchronous but render_cancel
    /// itself does not block for the render's duration (it only trips the
    /// shared cancel flag — see AgentSession::CancelAsyncRender's doc), so
    /// this call returns promptly regardless of whether a render is
    /// actually running.  Safe to call with nothing outstanding (id 0 —
    /// render_cancel's `renderJobId` param is optional) and safe to call
    /// with no viewport bridge attached (guarded below).
    ///
    /// Clears `outstandingChatRenderJobId` immediately after firing the
    /// cancel (rather than leaving that to the render_wait loop's own
    /// `defer`): this method is ALSO called from paths where that loop
    /// may no longer be running at all (e.g. `sceneOpened`/`sceneClosed`
    /// calling `cancelTurn()` when the turn was already idle, or a
    /// Stop/cancelTurn landing after the loop already returned but
    /// before its `defer` ran) — leaving the stale id set in those cases
    /// would incorrectly persist a cancelled job's id across scenes/
    /// turns.  The render_wait loop's own `defer { outstandingChatRenderJobId
    /// = 0 }` is therefore a harmless redundant clear on the common path
    /// where the loop is still alive when this fires — setting a UInt64
    /// to 0 twice from the same MainActor is not a race.
    private func cancelAnyOutstandingChatRender() {
        guard outstandingChatRenderJobId != 0, let vb = viewportBridge else { return }
        let jobId = outstandingChatRenderJobId
        outstandingChatRenderJobId = 0
        let line = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"render_cancel\"," +
                   "\"params\":{\"renderJobId\":\(jobId)}}"
        // Response intentionally discarded: its `cancelled:true` means only
        // "routed through a live controller", NOT "something was stopped"
        // (see AgentRpc.cpp's render_cancel doc — `cancelled` mirrors
        // `HasController()`, not whether a render was actually in flight);
        // this call site already knows a job WAS outstanding (the guard
        // above), and the fire-and-forget contract means we don't wait to
        // find out whether it actually stopped.
        _ = vb.agentHandleLine(line)
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
    /// (since Model-B F2 slice S2b) so does a `render` tool call's
    /// render_wait polling loop — see executeRenderToolCallAsync's doc
    /// for why that is same-actor SLICING (short synchronous
    /// `agentHandleLine` calls separated by `await Task.sleep`), not a
    /// second thread calling the dispatcher; `AgentRpcDispatcher::
    /// HandleLine` is documented single-caller, and every actual call
    /// into it — submit, each poll slice — still happens on this
    /// MainActor Task, never concurrently with anything else.  EVERY
    /// OTHER tool call still executes SYNCHRONOUSLY on main via
    /// `agentHandleLine` in one shot (the 1c-1 contract: same thread the
    /// GUI's own SetProperty edits drive) — they are fast CST reads/
    /// edits with no render-duration cost to amortize.
    ///
    /// PRODUCTION-RENDER INVARIANT (B2 review round 1; refined S2b):
    /// tool calls must never execute while the production rasterizer's
    /// workers read Scene state off-main.  `renderState` is @MainActor
    /// state and every stretch of this driver between suspension points
    /// is one synchronous MainActor slice.  Pre-S2b the tool loop had NO
    /// awaits at all, so `startRender` (also MainActor) could only ever
    /// interleave at the outer URLSession await.  S2b's render_wait
    /// polling loop adds NEW suspension points INSIDE the tool loop —
    /// the invariant is preserved by re-checking `Task.isCancelled`,
    /// `stopRequested`, `viewportBridge`, and `sceneEditable()` at the
    /// top of every render_wait slice (inside
    /// executeRenderToolCallAsync) AND again at the call site
    /// immediately after `await executeRenderToolCallAsync(...)`
    /// returns, before the result is ever handed to `addToolResult`.
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

            guard let apiKey = resolveApiKey() else {
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
            do {
                let (body, response) = try await URLSession.shared.data(for: urlRequest)
                data = body
                status = (response as? HTTPURLResponse)?.statusCode ?? 0
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

            let step = chatBridge.handleResponse(
                status: status, body: String(data: data, encoding: .utf8) ?? "")

            switch step.kind {
            case .toolCalls:
                consecutiveHttp400s = 0
                if !step.assistantDisplayText.isEmpty {
                    transcript.append(Entry(kind: .assistant,
                                            text: step.assistantDisplayText))
                }
                for call in step.toolCalls {
                    // Stop / scene-close checks (defensive): on the
                    // MainActor these flags cannot flip inside this
                    // awaitless loop (clicks queue until the slice
                    // ends), so a user Stop actually lands at the next
                    // URLSession await -- these checks matter for the
                    // cancelTurn()-from-within-a-tool-call path (a tool
                    // side effect triggering scene teardown): abandon
                    // the rest; the loop synthesizes cancelled results
                    // for them at the next flush.  The scene-editable
                    // check cannot actually flip inside this loop
                    // (no awaits — one MainActor slice, per the
                    // invariant comment above); it is kept alongside
                    // the others so every tool call is visibly gated.
                    if Task.isCancelled || stopRequested { return }
                    guard let vb = viewportBridge else { return }
                    guard sceneEditableOrReportRenderConflict() else { return }

                    transcript.append(Entry(kind: .toolActivity,
                                            text: "→ \(call.name)"))
                    let line = chatBridge.toolCallToJsonRpcLine(call, rpcId: nextRpcId)
                    nextRpcId += 1

                    // Model-B F2 slice S2b: a `render` tool call goes
                    // through the NON-BLOCKING async path (submit +
                    // short-slice render_wait polling, all still on
                    // main/MainActor — see executeRenderToolCallAsync's
                    // doc for why this is a same-actor slicing scheme, NOT
                    // a second thread calling into the dispatcher).  Every
                    // OTHER tool call keeps the 1c-1 synchronous contract
                    // unchanged (they are all fast CST reads/edits with no
                    // render-duration cost to amortize).
                    let responseLine: String
                    if call.name == "render" {
                        responseLine = await executeRenderToolCallAsync(
                            call: call, submitLine: line, vb: vb)
                    } else {
                        // Synchronous, on main — the 1c-1 executor contract.
                        responseLine = vb.agentHandleLine(line)
                    }

                    // The awaited render path above can suspend across
                    // Stop / scene-close / a production render starting;
                    // re-verify before touching shared driver state, same
                    // as every other post-await site in this file.  A
                    // failure here means the turn is already being torn
                    // down by whatever flipped the gate — abandon; the
                    // next send's flush synthesizes a cancelled result for
                    // this (and any later) pending call.
                    if Task.isCancelled || stopRequested { return }
                    guard viewportBridge != nil else { return }
                    guard sceneEditableOrReportRenderConflict() else { return }

                    chatBridge.addToolResult(call, jsonRpcResponseLine: responseLine)
                }
                continue

            case .finalText:
                consecutiveHttp400s = 0
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

    /// Model-B F2 slice S2b: execute a `render` tool call WITHOUT
    /// blocking the UI for the render's duration.
    ///
    /// THREAD-CONTRACT FINDING THAT DRIVES THIS SHAPE: `AgentRpcDispatcher`
    /// (src/Library/Agent/AgentRpc.h) is documented "NOT thread-safe (slice
    /// 0c is single-threaded, matching AgentSession)" — HandleLine (which
    /// `agentHandleLine` calls straight through to on the calling thread)
    /// is a single-caller API, not a thread-safe one.  So the fix is NOT
    /// "poll render_status/render_wait from a second thread while the
    /// MainActor does something else" — that would be a second concurrent
    /// caller into the SAME dispatcher/session the MainActor might also be
    /// driving (e.g. the Agent JSON-RPC debug panel, or another tool call
    /// in a future parallel-calls world). The correct shape, and the one
    /// used here, is SAME-ACTOR SLICING: every `agentHandleLine` call in
    /// this method still runs synchronously on the MainActor (preserving
    /// the single-caller contract exactly), but the render itself runs on
    /// the controller's dedicated agent-render worker thread (submitted
    /// via `render{"async":true}`), so each individual `agentHandleLine`
    /// call this method makes is fast — a submit, or (S2b P2-2) a
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
        // Inject {"async":true} into the params object.  A parse failure
        // here (should not happen — toolCallToJsonRpcLine always emits
        // well-formed JSON) falls back to the ORIGINAL synchronous call:
        // still correct, just blocking, which is strictly better than
        // fabricating a bogus response.
        guard let asyncLine = Self.injectAsyncTrue(intoJsonRpcLine: submitLine) else {
            return vb.agentHandleLine(submitLine)
        }

        // Submit.  Synchronous but fast — this is a queue-and-return
        // call, not the render itself (see AgentSession::RenderAsync).
        let submitResponse = vb.agentHandleLine(asyncLine)
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
        defer { outstandingChatRenderJobId = 0 }

        // Poll render_wait(timeoutMs:0) — a POLL-ONCE call, not a bounded
        // wait — separated by an `await Task.sleep`.  S2b P2-2: the prior
        // shape passed `timeoutMs:250` to render_wait, and because
        // `agentHandleLine` runs synchronously on the MainActor (see the
        // method doc), that made each slice's MainActor-blocking cost up
        // to 250ms — WaitForRenderJob's `wait_until` loop (SceneEditController.cpp)
        // really does block the calling thread for up to `timeoutMs`
        // waiting on its condvar.  At ~4Hz that read as UI jank for the
        // render's whole duration (button clicks / viewport interaction /
        // SwiftUI redraws all stalling on each slice).  `timeoutMs:0` hits
        // WaitForRenderJob's explicit "poll-once contract: no wait" early
        // return (`if( timeoutMs == 0 ) return false;` before any condvar
        // wait) — so this `agentHandleLine` call is now a single mutex-
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
            let waitResponse = vb.agentHandleLine(waitLine)
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
