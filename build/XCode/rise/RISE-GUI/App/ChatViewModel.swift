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
// arrive via environment variables) and are read at USE time only —
// never stored in UserDefaults, never logged/printed, never shown in
// the UI.  Nothing in this file logs request/response bodies.

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

    /// Environment-variable fallback consulted at USE time when the
    /// Keychain has no key for this provider.
    var apiKeyEnvVar: String {
        switch self {
        case .anthropic: return "ANTHROPIC_API_KEY"
        case .gemini:    return "GEMINI_API_KEY"
        }
    }

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
    /// Set after an Http-kind error — the one retriable case.  The
    /// Retry button re-issues the SAME conversation state (nothing was
    /// recorded for the failed step).  Never set for MaxTokens /
    /// IterationCap, which must NOT be retried verbatim.
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
    private var consecutiveHttp400s: Int = 0

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
    }

    /// Persisted model id for `p` ("" = provider default).  Lets the
    /// settings UI show each provider's stored value while editing.
    func storedModelId(for p: AgentChatProviderChoice) -> String {
        UserDefaults.standard.string(forKey: Self.modelIdKey(p)) ?? ""
    }

    /// Store (or, with an empty string, delete) the Keychain key for
    /// `p`.  Returns false on a Keychain write error.
    @discardableResult
    func saveApiKey(_ key: String, for p: AgentChatProviderChoice) -> Bool {
        let ok = AgentChatKeychain.write(
            account: p.keychainAccount,
            secret: key.trimmingCharacters(in: .whitespacesAndNewlines))
        keyStateEpoch += 1
        return ok
    }

    /// Resolution order (documented in the UI): Keychain first,
    /// environment second.  Returns the source WITHOUT the key so the
    /// UI can caption it; the key itself is fetched separately at USE
    /// time and never retained.
    func keySource(for p: AgentChatProviderChoice) -> AgentChatKeySource {
        if AgentChatKeychain.read(account: p.keychainAccount) != nil {
            return .keychain
        }
        if let env = getenv(p.apiKeyEnvVar), env.pointee != 0 {
            return .environment(p.apiKeyEnvVar)
        }
        return .none
    }

    /// The key to use RIGHT NOW (Keychain first, env second), or nil.
    /// Never logged, never stored on self.
    private func resolveApiKey() -> String? {
        if let key = AgentChatKeychain.read(account: provider.keychainAccount) {
            return key
        }
        if let env = getenv(provider.apiKeyEnvVar) {
            let key = String(cString: env)
            if !key.isEmpty { return key }
        }
        return nil
    }

    // MARK: The turn driver

    /// Send the composed input as a new user message and drive the
    /// turn to completion.
    func send() {
        let text = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !isBusy, viewportBridge != nil else { return }
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

    /// The Stop button: request a graceful stop.  The driver checks
    /// the flag between HTTP rounds and between tool calls and simply
    /// stops; the loop's next flush synthesizes cancelled results for
    /// anything left pending.
    func requestStop() {
        guard isBusy else { return }
        stopRequested = true
    }

    /// Reset the conversation (offered after repeated HTTP 400s, and
    /// available from the settings popover).  Keeps provider/model/
    /// skill-index config; drops transcript + pending state.
    func resetConversation() {
        cancelTurn()
        chatBridge.reset()
        transcript = []
        clearErrorAffordances()
        transcript.append(Entry(kind: .notice, text: "Conversation reset."))
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

    /// One conversation turn: HTTP round-trips + tool rounds until the
    /// model finishes with text, errors, or the user stops.  Runs on
    /// the main actor; only the URLSession await leaves the main
    /// thread.  Tool calls execute SYNCHRONOUSLY on main via
    /// `agentHandleLine` (the 1c-1 contract: same thread the GUI's own
    /// SetProperty edits drive) — note a `render` tool call therefore
    /// blocks the UI for its duration (acceptable for this slice; the
    /// verb's default is a small preview-sized pass).
    private func driveTurn() async {
        while true {
            if Task.isCancelled || stopRequested { return }
            guard viewportBridge != nil else { return }

            guard let apiKey = resolveApiKey() else {
                transcript.append(Entry(
                    kind: .error,
                    text: "No API key for \(provider.displayName).  Add one in "
                        + "the chat settings (stored in the Keychain) or launch "
                        + "with \(provider.apiKeyEnvVar) set."))
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
                    // Stop / scene-close between tool calls: abandon
                    // the rest; the loop synthesizes cancelled results
                    // for them at the next flush.
                    if Task.isCancelled || stopRequested { return }
                    guard let vb = viewportBridge else { return }

                    transcript.append(Entry(kind: .toolActivity,
                                            text: "→ \(call.name)"))
                    let line = chatBridge.toolCallToJsonRpcLine(call, rpcId: nextRpcId)
                    nextRpcId += 1
                    // Synchronous, on main — the 1c-1 executor contract.
                    let responseLine = vb.agentHandleLine(line)
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
