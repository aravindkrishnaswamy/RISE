# RISE LLM / Agent Runtime — Provider Adapters, the Agent Loop, Chat UX, and Staged Autonomy

**Status:** DESIGN (no code). One of two AI deep-dives spun off [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §9. The sibling [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) owns the **tool/resource catalog** (what an LLM can read/do to RISE, schema auto-generation from `SceneGrammar::Describe()`, `validate`, framebuffer resource, transport, permissions). This doc owns the **agent runtime**: the provider-adapter abstraction, the agent loop, auth & secrets, the chat panel, the "show me the code" duality, staged autonomy (L0–L3), and guardrails/cost. Where the two meet (the tool catalog the adapter translates; the permission scopes the runtime enforces) this doc references the MCP doc rather than re-specifying it.
**Owner:** Aravind Krishnaswamy
**Scope:** Make the in-app AI a first-class authoring surface over the canonical `.RISEscene` text, on macOS, Windows, and (Tier A) Android, with **the entire runtime in the shared C++ library** and only credential storage + the chat bubble UI per-platform. Answers GUI_ROADMAP §13 open-question #4 (the minimal provider-adapter interface) and maps to the A0–A3 AI spine of GUI_ROADMAP §11.
**Honors:** GUI_ROADMAP §1 principles — text is canonical (#1), maximize shared C++ (#2), Android keeps the core incl. LLM (#3), one mutation path through `SceneEditController` (#6) — and §16 confirmed decisions: the credential interface is a single reference-counted **`ICredentialStore : IReference`** in **`src/Library/Agent/`** (drop `ISecretStore`); the agent subsystem **avoids the bare `MCP` token** in type/dir names; cloud auth ships **API-key paste first**.
**Ground truth:** every "today" claim is reconciled to [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (code-verified 2026-06-19; plan-doc `Status:` headers treated as suspect) — in particular **round-trip save is largely IMPLEMENTED** (structured property/transform/created-camera save ships; only non-camera creation persistence is the gap), which corrects this doc's earlier "MVP requires no round-trip save" framing throughout.
**Related (siblings this doc now defers to):** [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) (tool catalog / transports / scopes), [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) (threat model: ProjectRootJail, secret redaction, autonomy×scope threat mapping), [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) (epochs, L1 staging = proposed transaction, undo attribution), [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) (agent renders are coordinator jobs), [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) (the `validate` dry-run), [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md), [../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md), [../INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md).

---

## 1. Executive summary

RISE is, by architecture, the best-positioned renderer in existence for genuine AI operation: the scene is **documented, descriptor-validated, diffable text**, and the grammar is machine-introspectable (`SceneGrammar::Describe()` already self-describes every chunk/parameter — it powers the syntax highlighter and the suggestion engine today, see `src/Library/SceneEditorSuggestions/SceneGrammar.h`). An LLM can be handed the exact schema of what is valid and can check its output against the *same parser the engine uses* — **validity-by-construction**, the hard part of agentic tooling, is already half-built.

The runtime is a single shared-C++ subsystem with three seams:

1. **`ILLMProvider`** — a minimal C++ abstraction with exactly three concrete adapters (Claude Messages API tool use; Gemini function calling; OpenAI-compatible-local for Ollama / LM Studio / llama.cpp). It normalizes streaming tokens, tool-call request/return, and capability differences (vision, parallel tool calls, context window).
2. **`AgentSession`** — the agent loop: message → model → tool calls (dispatched to the MCP layer from the sibling doc) → tool results → repeat, with cancellation, cost caps, and the **vision feedback loop** (render preview → return framebuffer → model sees → iterates).
3. **`AgentTranscript`** — the shared conversation/diff model behind the per-platform chat bubble UI and the **"show me the code"** mirror (every AI action shown as the `.RISEscene` diff).

Only two things are per-platform: **secure credential storage** (Keychain / Windows Credential Manager / Android Keystore) and the **chat bubble widget**. Everything else — adapters, loop, transcript, diff rendering, cost accounting, scope enforcement — is written once in `src/Library/` and consumed via the existing bridge pattern (`RISEBridge` on macOS, `ViewportBridge`/`RenderEngine` on Qt, JNI on Android).

The **MVP requires no *new* engine work** and depends on no *unbuilt* save path. Structured round-trip save (transform/property/created-camera) **already ships** ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1; `SaveEngine.cpp`), so AI edits in those classes persist structurally today; the only persistence gap is **non-camera creation**. And like a human hand-editor, the agent can always fall back to rewriting scene text wholesale and reloading — `read scene + read grammar + propose diff + validate + reload + render + look` is a complete, powerful loop on infrastructure that exists today (`loadAsciiScene` + `rasterize` + the progressive framebuffer the GUI already polls via `pollProductionVFS`).

---

## 2. Where it runs — shared C++, not Swift/Qt/Kotlin

The decisive call (GUI_ROADMAP principle #2): **the agent loop lives in `src/Library/`**, not in the SwiftUI `RenderViewModel` or the Qt `MainWindow`. Reasons:

- **Write-once across three platforms.** The HTTP/SSE client, JSON (de)serialization, the three provider dialects, the loop state machine, cost accounting, and scope enforcement are identical on macOS, Windows, and Android. Re-implementing them in Swift + C++ + Kotlin would triple the surface area and guarantee drift — the exact anti-pattern §10 of the roadmap warns against. The bridges (`RISEBridge.h`, `RISESceneEditorBridge.h`) are already structurally identical thin marshaling layers; the agent is a new subsystem behind the same kind of C-ABI surface.
- **It already lives next to the engine and the controller.** Tool dispatch must call into `SceneEditController` (the single mutation path, principle #6 — `RequestSave`, `Undo`/`Redo`, `SetSelection`, property scrubs; see `src/Library/.../SceneEditController.h`) and `IJob`/`rasterize`. Putting the loop in the library keeps it on the right side of every bridge.
- **The framebuffer is already in the library.** The vision loop needs the rendered image as bytes; the production `ViewportFrameStore` and `RenderToBuffer` path the GUI polls today (`RISEBridge` `pollProductionVFS`, `saveAs`) is the source. No new platform plumbing.

What stays per-platform (and *only* this):

| Concern | Shared C++ (library) | Per-platform shell |
|---|---|---|
| `ILLMProvider` + 3 adapters (HTTP/SSE, dialect translation) | all of it | — |
| `AgentSession` loop, cancellation, cost caps, scope checks | all of it | — |
| `AgentTranscript` model, diff computation, "show me the code" | all of it | — |
| MCP tool catalog + dispatch (sibling doc) | all of it | — |
| **HTTP networking** | a `IHttpClient` interface (one impl per platform, or one shared libcurl); **all outbound URLs pass `IOutboundUrlPolicy`** (SSRF chokepoint, AI_SECURITY_MODEL §3) | the concrete socket/TLS stack |
| **Secure credential storage** | `ICredentialStore : IReference` interface (`src/Library/Agent/`, per §16; **not** `ISecretStore`) | Keychain / Cred-Mgr / Keystore impl (§4) |
| **Chat bubble UI + token streaming display** | transcript model + delta callbacks | SwiftUI `List`, Qt `QListView`, Compose `LazyColumn` |
| **OAuth browser hand-off** | URL + PKCE state machine | `ASWebAuthenticationSession` / system browser / Custom Tab |

> **Uncertainty (HTTP client).** RISE already vendors libraries under `extlib/`. The cleanest path is one shared `IHttpClient` backed by libcurl (with TLS) compiled into the library so streaming SSE parsing is also shared. The fallback is a per-platform `IHttpClient` impl (`URLSession` / `QNetworkAccessManager` / `HttpURLConnection`/OkHttp) injected through the interface — more platform code but zero new third-party deps. Decide during A1; the `ILLMProvider`/`AgentSession` design is identical either way because both sit behind `IHttpClient`.

---

## 3. The provider-adapter interface (`ILLMProvider`)

### 3.1 Design goal

One narrow C++ interface that the agent loop talks to, with three implementations. The loop must never branch on "which provider"; all dialect differences are absorbed by the adapter. The interface is **streaming-first** (so the chat panel shows tokens as they arrive) and **tool-call-native** (the unit of progress is a tool call, not a regex over prose).

### 3.2 The three dialects, normalized

The three providers express the *same* agentic loop with different field names. The adapter's whole job is this table:

| Concept | Claude Messages API | Gemini `generateContent` | OpenAI-compatible local | RISE-normalized type |
|---|---|---|---|---|
| Conversation turn | `messages[]` (`role: user`/`assistant`) | `contents[]` (`role: user`/`model`) | `messages[]` (`role`) | `AgentMessage` |
| Tool catalog | `tools[]` = `{name, description, input_schema}` (JSON Schema 2020-12) | `tools[].function_declarations[]` = `{name, description, parameters}` (OpenAPI subset) | `tools[]` = `{type:"function", function:{name, description, parameters}}` | `ToolSpec` (from MCP `inputSchema`) |
| Model wants a tool | content block `{type:"tool_use", id, name, input}`, `stop_reason:"tool_use"` | part `{functionCall:{id, name, args}}` | `message.tool_calls[]` = `{id, function:{name, arguments(JSON string)}}`, `finish_reason:"tool_calls"` | `ToolCallRequest{id, name, argsJson}` |
| You return result | user message with `{type:"tool_result", tool_use_id, content, is_error}` | `function` role part `{functionResponse:{id, name, response}}` | `role:"tool"` message `{tool_call_id, content}` | `ToolCallResult{id, contentJson, isError}` |
| Choose/force a tool | `tool_choice` `{auto|any|tool}` | `function_calling_config.mode` `AUTO`/`ANY`/`NONE`/`VALIDATED` | `tool_choice` `auto`/`required`/`{name}` | `ToolChoice` enum |
| Streaming text | SSE `content_block_delta` / `text_delta`; tool args as `input_json_delta` | SSE chunks; partial `functionCall` | SSE `delta.content` / `delta.tool_calls[].function.arguments` | `onTextDelta` / `onToolArgsDelta` |
| Image input (vision) | image content block (base64 / source) | `inlineData {mimeType, data}` part | `image_url` content part (model-dependent) | `AgentImage{mime, bytes}` |

Sources for the loop mechanics: Claude tool-use contract and the `while stop_reason == "tool_use"` loop ([how-tool-use-works](https://platform.claude.com/docs/en/agents-and-tools/tool-use/how-tool-use-works), [tool-use overview](https://platform.claude.com/docs/en/agents-and-tools/tool-use/overview)) and streaming events ([streaming](https://platform.claude.com/docs/en/build-with-claude/streaming)); Gemini `functionCall`/`functionResponse` parts, `function_calling_config` modes, and per-call `id` ([function calling](https://ai.google.dev/gemini-api/docs/function-calling)); OpenAI-compatible `tool_calls` with `finish_reason:"tool_calls"` and the local-server caveats ([Ollama tool calling](https://docs.ollama.com/capabilities/tool-calling), [Ollama streaming+tools blog](https://ollama.com/blog/streaming-tool)).

### 3.3 The interface (sketch)

```cpp
// src/Library/Agent/ILLMProvider.h  (illustrative; tabs in real file)
namespace RISE {

enum class ToolChoice { Auto, Any, None };

struct ToolSpec {                  // built once from the MCP catalog (sibling doc)
    std::string name;
    std::string description;
    std::string inputSchemaJson;   // JSON Schema 2020-12, RISE-canonical form
};

struct ToolCallRequest { std::string id, name, argsJson; };
struct ToolCallResult  { std::string id, contentJson; bool isError = false; };

struct AgentImage { std::string mimeType; std::vector<uint8_t> bytes; };

struct AgentMessage {              // provider-neutral; adapter serializes it
    enum class Role { System, User, Assistant, Tool } role;
    std::string text;                          // may be empty
    std::vector<ToolCallRequest> toolCalls;    // assistant turns
    std::vector<ToolCallResult>  toolResults;  // tool turns
    std::vector<AgentImage>      images;        // user/tool turns (vision)
};

// What the loop learns about a model without branching on its brand.
struct ProviderCaps {
    bool supportsVision         = false;
    bool supportsParallelTools  = false;  // >1 tool_use block per turn
    bool supportsToolStreaming  = true;   // reliable tool calls WHILE streaming
    uint32_t contextWindowTokens = 0;     // 0 = unknown (local)
    std::string modelId;
};

// Streaming sink the adapter pushes into; the chat UI subscribes.
struct IStreamSink {
    virtual void OnTextDelta(std::string_view) = 0;
    virtual void OnToolArgsDelta(std::string_view callId, std::string_view jsonFrag) = 0;
    virtual void OnToolCallStarted(const ToolCallRequest&) = 0;
    virtual void OnThinkingDelta(std::string_view) {}   // Claude extended thinking; no-op elsewhere
    virtual ~IStreamSink() = default;
};

enum class StopReason { EndTurn, ToolUse, MaxTokens, StopSequence, Refusal, Error, Cancelled };

struct TurnResult {
    AgentMessage assistant;        // accumulated text + tool calls
    StopReason   stop;
    uint32_t inputTokens=0, outputTokens=0, cachedReadTokens=0; // for the cost cap
    std::string errorDetail;       // populated on Error
};

class ILLMProvider {
public:
    virtual ProviderCaps Capabilities() const = 0;

    // ONE turn. Adapter: serialize messages+tools to the dialect, open the
    // SSE stream, push deltas into `sink`, assemble the AgentMessage, map the
    // brand's stop field to StopReason. The agent LOOP lives in AgentSession,
    // not here — this is a single request/response.
    virtual TurnResult RunTurn(const std::vector<AgentMessage>& messages,
                               const std::vector<ToolSpec>&     tools,
                               ToolChoice                       choice,
                               IStreamSink&                     sink,
                               const std::atomic<bool>&         cancel) = 0;
    virtual ~ILLMProvider() = default;
};

} // namespace RISE
```

### 3.4 Capability differences the loop must respect

- **Vision.** Claude and Gemini accept images; the vision feedback loop (§5.4) is gated on `Capabilities().supportsVision`. Most local models are text-only — on those the loop falls back to returning *render statistics + variance + auto-router decision as text* (which the MCP layer already exposes) instead of pixels, and tells the user the model can't see the image. Gemini additionally allows images *inside* a `functionResponse` (`inlineData`) ([Gemini function calling](https://ai.google.dev/gemini-api/docs/function-calling)); Claude takes the framebuffer as an image block in the next user turn. The adapter hides this: the agent returns an `AgentImage` on the tool result and the adapter routes it correctly.
- **Parallel tool calls.** Claude can emit several `tool_use` blocks in one turn; the loop must execute all of them and return all `tool_result`s before the next turn. Gemini supports parallel `functionCall` parts and explicitly does **not** require returning results in call order. Local models vary — when `supportsParallelTools` is false the adapter coalesces to one call per turn. `AgentSession` always handles a *vector* of calls so the single-call case is just N=1.
- **Context window.** `contextWindowTokens` drives the `ContextAssembler`'s transcript-trimming + resource-selection policy (§5.6). For local models it is often unknown (`0`) → the runtime uses a conservative default and surfaces token counts so the user can judge.
- **Tool-calls-while-streaming.** The single sharpest local-model caveat: Ollama's **OpenAI-compatible `/v1/chat/completions` silently drops tool calls when streaming is on** — the model decides to call a tool but the stream returns empty content with `finish_reason:"stop"`, losing the call; its native `/api/chat` endpoint handles streaming+tools correctly ([Ollama #12557](https://github.com/ollama/ollama/issues/12557), [Ollama tool calling](https://docs.ollama.com/capabilities/tool-calling)). `ProviderCaps::supportsToolStreaming=false` makes the local adapter **disable streaming on tool-enabled turns** (fall back to a single non-streamed request, then stream only the final prose turn). This is the one place the local adapter is materially different, and it is contained entirely inside the adapter.

### 3.5 Translating the MCP catalog into three dialects

The sibling [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) owns the catalog and its `inputSchema` (auto-generated from `SceneGrammar::Describe()` so the AI's view can never drift from the parser). The adapter consumes that catalog as `ToolSpec[]` and emits the provider's tool block. The translation is mostly field-renaming, but **schema dialects differ and this is a real bug source**:

- **Claude** accepts full **JSON Schema 2020-12** — the RISE-canonical form passes through nearly verbatim into `input_schema`.
- **OpenAI-compatible** drops some constraint keywords (e.g. `format: uuid`, certain `minimum`/`maximum` combos) — usually tolerated/ignored, occasionally rejected.
- **Gemini** is the strictest: it rejects `$schema`, and historically **rejects `$ref`/`$defs` and some `anyOf` patterns** that MCP servers emit ([gemini-cli #13326](https://github.com/google-gemini/gemini-cli/issues/13326), [Mastra compatibility layer](https://mastra.ai/blog/mcp-tool-compatibility-layer)).

Mitigation, owned by the adapter (a `SchemaTranscoder` helper): **inline `$ref`/`$defs`, strip `$schema`, drop unsupported keywords, flatten `anyOf` where possible**, per-provider. The MCP doc keeps the canonical schema rich; the adapter degrades it for the lossy dialects. RISE has a structural advantage here: because schemas are generated from `Describe()`, they are simple and flat by construction (enums, scalars, references-by-name), so the `$ref`/`anyOf` minefield is largely avoided at the source. A round-trip self-test (`for each tool: transcode → re-validate against provider rules`) belongs in the test suite.

---

## 4. Auth & secrets (the only platform-specific runtime piece)

The security design for everything in this section — secrets never entering model context, redaction at every egress boundary, the no-egress local path, SSRF-confined endpoints — is owned by [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §7 (cloud-data consent) and §9 (secret redaction). This section specifies the *runtime mechanics*; that doc specifies *what must never leak*.

### 4.1 The three sign-in modes

| Provider | Auth | What the user does | Where the secret lives |
|---|---|---|---|
| **Claude** | API key (paste first, per §16), or OAuth/login on the user's account | "Paste API key" or "Sign in with Claude" (OAuth, phase 2) | `ICredentialStore` (platform secure store backend) |
| **Gemini** | API key (Google AI Studio), or OAuth | paste key or "Sign in with Google" | `ICredentialStore` (platform secure store backend) |
| **Local** | none / endpoint URL (+ optional key) | type `http://localhost:11434` (Ollama) or LM Studio's port | endpoint in prefs; optional key in `ICredentialStore` |

Tokens are **on the user's own account** — RISE never proxies a hosted key. **Keys never enter the model context** — the provider key authenticates the HTTP request in the adapter and is never placed in a prompt, a tool arg, or a `ToolSpec` (AI_SECURITY_MODEL §9.1.2).

> **The "Local = no egress" guarantee holds ONLY for a loopback endpoint (resolves second-review §3 / the AI_SECURITY_MODEL §7 contradiction).** A user-typed *remote* endpoint (a LAN box `http://192.168.1.50:11434`, a tunnelled host) sends the scene and framebuffer **off this device** — so calling that "Local — no data leaves this machine" would be a false promise. The rule the runtime enforces (and the egress indicator obeys, §4.4 / §5.4):
> - A "Local" provider whose endpoint resolves to **loopback** (`127.0.0.1` / `::1`, the only loopback exception `IOutboundUrlPolicy` allows — AI_SECURITY_MODEL §3.2.1) is the **no-egress** path: scenes/framebuffers never leave the machine, no cloud-egress consent needed, indicator shows **"Local — no data leaves this machine."**
> - A "Local" provider pointed at **any non-loopback host** is treated, for consent and disclosure, **exactly like a cloud provider**: it requires the first-egress consent (AI_SECURITY_MODEL §5 always-confirm / §7.2.1), the persistent indicator shows **"Custom remote (<host>) — content leaves this machine,"** and vision egress is separately consented (§5.4). It still passes `IOutboundUrlPolicy` (HTTPS-or-explicit-loopback, blocked private/metadata ranges — so a non-loopback custom endpoint must satisfy the same SSRF rules; a plain-`http://` LAN box is refused unless the user explicitly accepts the downgrade, AI_SECURITY_MODEL §3).
>
> This closes the loophole where "Local" implied privacy regardless of where the endpoint actually pointed. The endpoint's *resolved address class*, not the provider label, decides the egress regime.

### 4.2 `ICredentialStore : IReference` — one interface, three backends

Per [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §16, the credential interface is a single **reference-counted `ICredentialStore : IReference`** (matching RISE's `IReference` convention) in `src/Library/Agent/`. **`ISecretStore` is dropped** — there is no separate secret-store type. It is the *sole* home for provider keys/tokens (AI_SECURITY_MODEL §9.1.1); external-MCP-server credentials, if any, live under a distinct account namespace in the same store, never the provider key (§9.4 / AI_SECURITY_MODEL §6.2.4).

```cpp
// src/Library/Agent/ICredentialStore.h
class ICredentialStore : public IReference {           // reference-counted, §16
public:
    virtual bool  Store (std::string_view account, std::string_view secret) = 0;
    virtual std::optional<std::string> Load(std::string_view account) = 0;
    virtual bool  Erase (std::string_view account) = 0;
protected:
    virtual ~ICredentialStore() = default;             // lifetime via IReference, not delete
};
```

Per-platform implementations (the *only* new platform code in the runtime):

- **macOS — Keychain Services.** `SecItemAdd`/`SecItemCopyMatching`/`SecItemDelete` with `kSecClassGenericPassword`, service `"ca.aravind.RISE.llm"`, account = provider id. On Apple silicon the item can be protected by the Secure Enclave / device passcode ([Apple Keychain](https://deepwiki.com/open-source-cooperative/keyring-rs/5.1-apple-platforms-(macos-and-ios))).
- **Windows — Credential Manager.** `CredWriteW`/`CredReadW`/`CredDeleteW` with `CRED_TYPE_GENERIC` and a `TargetName` of `"RISE/llm/<provider>"` ([Windows Credential Store](https://github.com/hrantzsch/keychain)).
- **Android — Keystore + EncryptedSharedPreferences.** The API key is an app secret, not a `KeyStore`-native asymmetric key, so the idiomatic pattern is **EncryptedSharedPreferences keyed by a `MasterKey` in the AndroidKeyStore** (hardware-backed / TEE on most devices) ([Android Keystore](https://developer.android.com/privacy-and-security/keystore)). The JNI layer exposes `Store/Load/Erase` to the shared `ICredentialStore` contract.

> **Optional simplification.** A thin cross-platform C++ wrapper such as `hrantzsch/keychain` already abstracts macOS Keychain / Windows Credential Vault / Linux libsecret behind one API ([keychain](https://github.com/hrantzsch/keychain)) — adopting it would collapse the desktop two of the three into shared code, leaving only the Android JNI impl bespoke. Evaluate vs. the ~40-line-each native calls during A1; either way the *interface* above is unchanged.

### 4.3 OAuth hand-off (shared state machine, platform browser) — phase 2

Per §16, **API-key paste ships first** (no redirect infra); OAuth/PKCE is phase 2. The OAuth dance (authorization-code + PKCE) is a small state machine that belongs in the library: build the authorize URL with a generated `code_verifier`/`code_challenge`, hand the URL to the shell to open in the platform browser/auth session, receive the redirect (loopback `http://127.0.0.1:<port>/callback` or a custom URL scheme), exchange the code for tokens, persist via `ICredentialStore`, schedule refresh. The loopback redirect listener binds loopback only and exact-matches the registered `redirect_uri` with a single-use `state` (the MCP confused-deputy mitigations, AI_SECURITY_MODEL §3.2.4), and discovery URLs pass `IOutboundUrlPolicy`. Only the **open-URL-and-capture-redirect** step is per-platform: `ASWebAuthenticationSession` (macOS), default browser + a transient loopback listener (Windows), Chrome Custom Tab + `intent-filter` (Android). API-key entry is even simpler — a text field → `ICredentialStore::Store`.

### 4.4 Sign-in UX

A **Settings → AI** pane (shared model, platform widgets): provider radio (Claude / Gemini / Local), a "Paste API key" / "Sign in" affordance, model picker (populated from the provider where listable), local endpoint URL field with a "Test connection" button (round-trips a 1-token request and reports the resolved `ProviderCaps`), and the autonomy-level selector (§5) + cost-cap fields (§6). **The local-endpoint field is validated, not free-form** — it passes `IOutboundUrlPolicy` (AI_SECURITY_MODEL §3) and a scene file or external MCP can never set or change it (AI_SECURITY_MODEL §3.2.2). **The endpoint's resolved address class sets the egress regime (§4.1):** a loopback endpoint is the no-egress path; a non-loopback host is classified as remote egress and triggers the cloud-style first-egress consent before any content is sent. The **persistent egress indicator** therefore has three states, driven by the *resolved* endpoint, not the provider label: **"Cloud: <provider> — content leaves this machine,"** **"Custom remote (<host>) — content leaves this machine,"** or **"Local — no data leaves this machine"** (loopback only) (AI_SECURITY_MODEL §7.2.2). Status line shows the active model and remaining budget. The chat panel's composer shows a small "not signed in — configure AI" inline prompt when no provider is ready.

---

## 5. The agent loop (`AgentSession`)

### 5.1 Canonical loop

The loop is the provider-agnostic version of Claude's `while stop_reason == "tool_use"` contract ([how-tool-use-works](https://platform.claude.com/docs/en/agents-and-tools/tool-use/how-tool-use-works)), and is identical for Gemini (`finish` with `functionCall` parts) and local (`finish_reason:"tool_calls"`):

```
AgentSession::Send(userText, images?):
  append user AgentMessage to transcript
  loop:
    enforce budget (token + render caps); if exceeded -> stop, surface to user
    turn = provider.RunTurn(transcript, toolSpecs, choice, streamSink, cancelFlag)
    append turn.assistant to transcript          // text shown live via streamSink
    add turn.{input,output,cached} tokens to budget
    switch turn.stop:
      EndTurn | StopSequence | Refusal -> break          // final answer
      MaxTokens                        -> offer "continue"; break
      Error | Cancelled                -> surface; break
      ToolUse:
        for each call in turn.assistant.toolCalls:        // vector handles parallel
          if !PermitsToolAtLevel(call) -> gate (see §5.5/§7): confirm or refuse
          result = mcp.Dispatch(call)                      // sibling doc
          append Tool AgentMessage(result) to transcript
        continue                                           // feed results back
```

`mcp.Dispatch` is the bridge into the tool surface from [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md). Read tools return resources (scene text, grammar, scene-graph, framebuffer, stats); act tools route through `SceneEditController` (§7). The loop never parses prose to recover intent — the structure is in the tool schema.

### 5.2 Threading & integration with the existing GUI

`AgentSession::Send` is **blocking and runs on a background thread** — exactly like `rasterize()` does today (`RenderViewModel.startRender` dispatches `bridge.rasterize()` on `Task.detached`; Qt uses its `RenderEngine` worker). Streaming deltas and tool-progress events marshal back to the UI thread the same way the render progress block and image block already do (`@MainActor` hops on macOS, queued signals on Qt). Critically: **a tool that renders must serialize against the interactive viewport and any production render**, because RISE's render threads and the scene state are not concurrently mutable. **The agent does NOT hand-roll its own viewport stop/restart** — that separate path is folded into the single `RenderCoordinator` ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §6): the agent submits a `RenderClass::Agent` (or `Thumbnail`/`IsolatedAdhoc`) job and the coordinator suspends the viewport, runs the job on the one render slot, and resumes — the same operation a Render-button or external-MCP render uses. `Submit()` is thread-safe, so an agent render arriving on this background thread is admitted under the coordinator's lock exactly like a UI-thread render (RENDER_COORDINATOR §6). This eliminates the divergent "agent stops/restarts the viewport separately" path.

### 5.3 Cancellation

The loop takes a `std::atomic<bool> cancel` threaded into `RunTurn` (so an in-flight SSE stream aborts) and checked between turns and between tool calls. The chat panel's Stop button flips it — mirroring the existing `cancelFlag` / `cancelRender()` pattern (`RenderViewModel.cancelRender` sets an `AtomicBool`; the render loop returns `!cancel` from the progress block). A cancelled render-tool leaves the partial production image on screen (the production `ViewportFrameStore` already preserves partial results on cancel). Cancellation is cooperative and prompt: abort the network read, skip remaining tool calls, append a "cancelled" transcript entry.

### 5.4 The vision feedback loop (the part that's impossible to fake in RGB)

This is the differentiator (GUI_ROADMAP §9.3): a spectral renderer that converges on "believable sunset through the window" by *looking at its own output*.

```
see -> adjust -> render -> see:
  1. agent calls render(quality=preview, region?)        [act, scoped] → a RenderCoordinator job
       (a preview maps to an isolated job against a private film, RENDER_COORDINATOR §5,
        so it never tears the live framebuffer)
  2. loop waits for the coordinator's completion callback
  3. agent calls get_framebuffer()                       [read]
     -> the agent server returns the rendered image bytes (from the captured isolated film)
  4. adapter attaches it as AgentImage on the tool_result (Claude image
     block / Gemini inlineData) IF Capabilities().supportsVision AND the user
     consented to vision egress (AI_SECURITY_MODEL §7.2.4)
  5. model "sees" the image, reasons, issues the next edit (e.g. nudge
     sun elevation, warm the HDRI, drop exposure), back to step 1
```

Cost discipline is essential here (§6): each iteration is a render. The loop **caps render iterations**, prefers **preview quality + region render** (the highest sample-efficiency-per-effort, GUI_ROADMAP §3), and **caches the last framebuffer** so a "look again" without an intervening edit doesn't re-render (a cache the coordinator also supports). Stale in-flight previews are dropped by epoch if the user edits mid-task (RENDER_COORDINATOR §4.3). On text-only (most local) models, step 4 degrades to returning render stats/auto-router rationale as text (and the **net-new** variance read-back once it exists — MCP_TOOL_SURFACE §3 / SPECTRAL_DIFFERENTIATORS D5; it is not in master today), and the agent is told it is "flying blind" so it relies on numeric convergence + user confirmation.

### 5.5 Per-call permission check

Before any *act* tool runs, the loop calls `PermitsToolAtLevel(call, level, scopes)` (§5/§7). Read tools are always permitted (subject to scope). Mutating tools at L1 raise the **diff-review gate**; at L2 they run within scopes; render/spend tools at L2 also count against the cost cap. The check is in the loop, not the adapter, so it is provider-independent and uniformly enforced. The **always-confirm set** (`remove_entity`, `load_scene`, file overwrite, enabling an external MCP, first cloud egress) is gated here by a human confirmation the model cannot satisfy at *every* level, incl. L3 (AI_SECURITY_MODEL §5) — an injection can never escalate its own autonomy (AI_SECURITY_MODEL §2.2.3).

**Every mutating tool call carries a `baseEpoch` precondition (mirrors MCP_TOOL_SURFACE §4 head).** The runtime stamps each mutating `tools/call` with the `baseEpoch = (uuidHi, uuidLo, revision)` the model last read from `rise://scene/text|graph` (MCP_TOOL_SURFACE §3.1 / §4) — the whole `DocumentId` pair, so a write computed against a *reloaded* document is rejected by the UUID mismatch, not just an advanced revision. The `ContextAssembler` (§5.6) tracks the freshest `DocumentId` it has surfaced to the model so the loop can fill `baseEpoch` even when the model omits it, and the model is instructed to thread the `newEpoch` returned by a successful mutation (MCP_TOOL_SURFACE §8) into its next edit. **Parallel mutating calls in one turn** are handled per MCP_TOOL_SURFACE §4 head: the dispatcher serializes them (reject-stale-then-reread) or, when the model signals atomic intent, composes them into one `Propose`/transaction — never silent last-write-wins. A returned `conflict` is fed back as a tool result (§5.1) so the model re-reads `rise://scene/*` and rebases (§7.4); a UUID-mismatch conflict tells it the document was reloaded and it must re-bootstrap, not rebase a delta.

**L1 staging is a *proposed transaction*, not a live mutation** ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §7, which resolves this doc's own §7.1-vs-§1 contradiction): at L1 an act tool does **not** call `SceneEditController::Commit`. It builds a `SceneTransaction` carrying the `baseEpoch` it read the scene at, hands it to the staging area, and the diff (§9.3) is computed from that *staged* transaction with the live scene untouched. Only on user **Approve** does staging call `Commit` — with the epoch **re-checked at apply time** (TRANSACTION_MODEL §7.2): if the scene advanced between proposal and approval, the commit conflicts and the proposal is re-diffed and re-presented (TRANSACTION_MODEL §7.4). So "act tools route through the controller" (§7.1) and "L1 diffs before mutating" (§1) are both true — the dispatch is *deferred to approval time*, not skipped. L2/L3 commit immediately within scope but still as attributed transactions; a conflict auto-retries once then surfaces (TRANSACTION_MODEL §7.3).

### 5.6 Resources → model context (the read bridge + context assembler)

MCP resources are read-only addresses; they do not become model context by themselves. Two shared-C++ pieces bridge them into the prompt:

1. **A generic resource-read tool** (`read_resource{ uri }`) plus the typed convenience reads (`list_*`, the scene-graph/framebuffer/stats reads). The model pulls a resource (e.g. `rise://scene/graph`, `rise://grammar/chunk/{kw}`, `rise://framebuffer`) **on demand** — resources are pull, not push (AI_SECURITY_MODEL §7.2.5), so the agent never bulk-ships the whole scene. The result is wrapped as a `Tool` `AgentMessage` and, for `rise://framebuffer`, attached as an `AgentImage` (vision-gated, §5.4).
2. **A `ContextAssembler`** that decides *which* resources ride in the next `RunTurn` and trims the transcript to fit `ProviderCaps::contextWindowTokens`. Its selection policy: (a) **always-resident, cached blocks** — the system prompt + the grammar resource + tool definitions (stable across a session → an ideal prompt-cache breakpoint, §7.3); (b) **task-relevant pulls** — the most recent scene-graph snapshot and any resource the model explicitly read this task, kept until superseded by a newer epoch; (c) **a token budget** — when the assembled context would exceed the window, drop oldest tool-result payloads first (keeping their one-line summaries), then oldest assistant prose, never the system/grammar blocks. Untrusted resource payloads are wrapped in `<rise:untrusted source=…>` markers before they enter context (AI_SECURITY_MODEL §2.2.1), and every text payload has already passed the `SecretRedactor` at the resource boundary (AI_SECURITY_MODEL §9). For local models with unknown context window (`0`), the assembler uses a conservative default and surfaces live token counts so the user can judge (§3.4).

### 5.7 Consuming external MCP servers — the A3 client runtime (resolves second-review §5 / "A3 has no runtime design")

Roadmap A3 (and §10 below) lets the in-app agent **consume** curated external MCP servers (HDRI/asset, spectral n,k, web). The *trust/governance* side — the maintainer-curated allowlist, per-server trust tiers, untrusted-result labeling, no-token-passthrough, enablement-is-always-confirm — is owned by [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §6. **This subsection owns the missing runtime architecture: how RISE-as-MCP-*client* actually talks to those servers.** It is the gating design for A3 — A3 does not ship until this lands. (Naming, per §16: RISE is the *client* here; the in-tree types are `Agent`/`ExternalServer`-prefixed and avoid the bare `MCP` token, exactly as the first-party server surface does — MCP_TOOL_SURFACE §0.)

A new shared-C++ component, **`ExternalServerHub`** (`src/Library/Agent/ExternalServerHub.{h,cpp}`), is the single client-side manager. It is distinct from the first-party `AgentServer` (which exposes RISE's *own* tools); the hub *calls out* to other servers and merges their tools into the loop's `ToolSpec[]`.

1. **Client transport + process lifecycle.** RISE-as-client speaks the two standard MCP client transports through one `IExternalServerTransport` interface (mirroring the server-side `AgentTransport`, MCP_TOOL_SURFACE §2.3): **stdio** (RISE spawns the server as a child process — `command` + `args` + a *scrubbed* env from the manifest; the child's `stdout` is MCP, `stderr` is captured to a quarantined log that itself passes the `SecretRedactor`) and **Streamable HTTP** (for an already-running remote server — its URL passes `IOutboundUrlPolicy`, AI_SECURITY_MODEL §3, so a server URL can never target a blocked/internal range). Lifecycle: lazy spawn on first enable, health-checked via the MCP `initialize` handshake, supervised restart with backoff on crash, and a hard kill + dropped-from-loop on revoke (AI_SECURITY_MODEL §6.2.5). Each child runs under the host OS user with no extra privilege; RISE does not sandbox the child's own machine access (that is the user's server, AI_SECURITY_MODEL §11) but **does** confine everything the child can make *RISE* do.
2. **Discovery.** Servers come from two sources: the **checked-in allowlist manifest** (maintainer-curated, AI_SECURITY_MODEL §6.1 / §13 open-q 2) and **user-added entries** (lowest trust tier, flagged "unverified"). On enable, the hub runs `initialize` → `tools/list` (+ `resources/list` if used) and caches the advertised catalog with its declared schemas. No auto-discovery from the network and no auto-enable — a server is inert until the user enables it (always-confirm, AI_SECURITY_MODEL §5/§6.1).
3. **Tool-name collision handling.** External tool names share the loop's flat namespace with RISE's first-party tools and with each other. The hub **namespaces every external tool** as `ext__<serverId>__<toolName>` before it enters `ToolSpec[]`, so (a) an external server can **never** shadow or impersonate a first-party tool (e.g. a malicious server advertising `save_scene` or `apply_scene_text` is exposed as `ext__evil__save_scene`, not the real one — and external tools get none of RISE's scopes anyway, AI_SECURITY_MODEL §6.1), and (b) two external servers offering `search` don't clash. The dispatcher routes a namespaced call back to its owning server; first-party (un-prefixed) names are reserved and an external server claiming one is rejected at merge time with a logged warning.
4. **Schema merging.** Each external tool's advertised `inputSchema` is run through the **same `SchemaTranscoder` (§3.5)** that adapts RISE's own schemas to the active provider dialect (inline `$ref`/`$defs`, strip `$schema`, drop unsupported keywords, flatten `anyOf` for Gemini) — external servers emit arbitrary JSON Schema, so this is *more* necessary here than for RISE's flat descriptor-generated schemas. A schema that fails transcoding (or exceeds a size/complexity cap) drops that one tool with a surfaced warning rather than poisoning the whole turn. External tool results re-enter the loop as `Tool` `AgentMessage`s wrapped in `<rise:untrusted source="external-mcp:<serverId>">` (AI_SECURITY_MODEL §2.2.1 / §6.2.1) — they are data, never instructions or authorization.
5. **Timeouts & cancellation.** Every external call has a per-call wall-clock timeout (manifest-configurable, conservative default) after which the hub cancels it and returns an `isError` tool result the model can react to — a slow/hung external server can never stall the agent loop indefinitely. The loop's `std::atomic<bool> cancel` (§5.3) propagates: a user Stop aborts in-flight external calls (close the HTTP read / signal the stdio child) exactly as it aborts a provider stream and skips remaining tool calls. A per-server concurrency cap and a per-task external-call budget bound runaway fan-out.
6. **Credential routing (per-server, never the provider key).** If an external server needs auth, its credential lives in `ICredentialStore` under a **distinct per-server account namespace** (`ext/<serverId>`), set by the user when enabling that server — and is sent **only** to that server, only on its own transport. RISE **never** forwards the user's Claude/Gemini provider key (or any other server's credential) to an external server — the MCP spec's absolute "MUST NOT forward tokens not issued for this server" (AI_SECURITY_MODEL §6.2.4 / §9.4). The provider key authenticates only the LLM request, in the provider adapter (§4.1). Credentials never enter a `ToolSpec`, a tool arg, or model context.
7. **Server version / update handling.** The manifest pins each allowlisted server to a vetted version/identity (stdio: a pinned package/command + expected version reported at `initialize`; HTTP: an expected server `name`/`version` from the handshake). On a **version change** the hub treats the server as *re-enable-required*: it does **not** silently adopt a new toolset (a server that grew a `delete_everything` tool overnight must not appear un-reviewed). A changed advertised catalog vs the cached one surfaces a "this server changed — re-review" prompt (always-confirm), and the allowlist manifest is reviewed like any code change (AI_SECURITY_MODEL §13 open-q 2). User-added servers re-prompt on any catalog change.

The hub touches no render or scene-mutation path — external servers feed *data* into the loop, which still flows through the diff-gate and scopes like any other model input. New `.cpp`/`.h` in `src/Library/Agent/` here (`ExternalServerHub`, `IExternalServerTransport` + concretes) follow the CLAUDE.md five-build-project rule (§15 Packaging).

---

## 6. Autonomy levels (L0 → L3)

A single `AutonomyLevel` enum, set in Settings → AI, gates what the loop may do. It composes with **scopes** (`read`, `edit`, `render`/`spend`) so a level is "what kinds of actions are allowed and whether each needs confirmation," and scopes are "which capability families are unlocked at all."

| Level | Name | What it can do | Gate | Concrete RISE example |
|---|---|---|---|---|
| **L0** | Advisor | Chat + **read-only** resources (scene text, grammar, scene-graph, framebuffer, stats). Suggests text the user copies. No tool that mutates or renders. | n/a (cannot act) | "Why is my glass sphere black?" → agent reads the scene, spots a `spectral_painter` bound to a scalar slot, **explains** the IScalarPainter fix and prints the corrected chunk for the user to paste. |
| **L1** | Propose-and-confirm | Composes a full scene **diff**; user approves/rejects per change (the **code-review gate**). On approve, applies via `SceneEditController` (or wholesale rewrite + reload in MVP). May `validate` freely (read-only). | **diff-review gate per apply** | "Make the key light warmer and 30% brighter." → agent edits the light chunk, runs `validate`, presents a red/green diff; user clicks Apply; change lands undoably. |
| **L2** | Operate-with-guardrails | Applies edits **and renders** within granted **scopes** and **cost caps**, without per-change confirmation — but every action still appears in "show me the code" and is undoable. | scopes + token/render caps (not per-action confirm) | "Frame the watch dial, switch to Auto integrator, render a preview." → agent sets the camera, selects `auto_rasterizer`, starts a preview render, reports "Auto → BDPT (glossy/indirect variance high)" — all without stopping to ask, because `edit`+`render` scopes are granted and it's under budget. |
| **L3** | Autonomous multi-step | Plans and executes a multi-step task with many edit/render iterations, including the **vision loop**, until a goal or a cap is hit. | hard caps (token, render-count, wall-clock); optional checkpoint approvals | "Make 5 lighting variations of this scene and render thumbnails." → agent clones the scene state 5×, perturbs HDRI/sun/exposure each, renders 5 preview thumbnails, returns a contact sheet; stops at 5 renders or the cap, whichever first. Also: "Get the sunset-through-the-window believable" → see→adjust→render→see until it looks right or the render cap trips. |

Defaults: ship at **L1** (safe, demonstrably useful, no surprise spend). L0 is the read-only fallback when no `edit` scope is granted. L2/L3 are opt-in toggles with explicit cap fields. Android defaults to **L1** as well (chat is Tier A; mutating tools are Tier A read + L1 edit — see §8).

---

## 7. Guardrails & cost

Everything below is enforced in the shared loop, so it holds on all three platforms by construction.

### 7.1 One mutation path (principle #6)

**Every** act tool routes through `SceneEditController::Commit` (a transaction wrapping the shipped `Apply`; TRANSACTION_MODEL §4) + undo/redo — there is **no parallel write path** for the AI. Concretely the tool layer drives the same controller machinery the GUI uses (selection, property scrubs, `RequestSave`, `Undo`/`Redo`; `src/Library/.../SceneEditController.h`), now expressed as attributed transactions. This is what makes AI actions **undoable, diff-reviewable, attributed, and persistent** — a botched AI edit is one Cmd-Z, identical to a botched gizmo drag. **Persistence is not blocked on an unbuilt Phase-0 save:** structured round-trip save (transform/property/created-camera) **ships today** ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1), so edits in those classes persist structurally now; the remaining dependency is **non-camera creation persistence only** (CURRENT_STATE_AUDIT §1/§2; owner [ENTITY_CREATION.md](ENTITY_CREATION.md)). For that gap (and as a universal fallback) the "apply" is a **wholesale text rewrite + reload** via the `apply_scene_text` tool (MCP_TOOL_SURFACE §4.5). **This wholesale path is a *document swap*, not an undoable transaction (corrects the earlier "`Reload`-origin transaction with prior text as undo state" framing; TRANSACTION_MODEL §7.5, finding #6).** Re-parsing the whole document destroys the current controller and its undo history and re-roots the session on a **fresh document UUID** — it is not a delta you can Cmd-Z back across, because the post-swap controller has no edges to the pre-swap state. (The `baseEpoch` precondition is still re-checked at apply so a wholesale replace computed from a stale read is rejected, not silently swapped, MCP_TOOL_SURFACE §4.5.) So a wholesale `apply_scene_text` is *not* "one Cmd-Z" the way a structured edit is — that undo guarantee holds for the structured tools (`set_property`/`set_transform`/created-camera) that route through `Commit`; the wholesale fallback trades undoability for the ability to express edits the structured save engine can't yet represent (the model is told this in the tool description, and the "undo" of a swap is re-applying the prior text as another swap, not a history pop).

### 7.2 Diff-review before apply

At L1 (and as an always-available "review" affordance at L2), the agent's proposed change is shown as a **`.RISEscene` diff** (the "show me the code" panel, §9) before anything mutates. The diff is the **same artifact** as the staged proposed transaction (TRANSACTION_MODEL §10 `RenderTransactionDiff`), computed once in shared C++ via the `SaveEngine` re-emit machinery in dry-run mode, and rendered with platform-native styling. Approve applies (re-checking the epoch, §5.5); reject discards and the rejection is fed back to the model as a tool result so it can revise. This is the single most important guardrail — it makes L1+ safe, and it is the human firewall against prompt injection (an injected "delete all lights" is a red diff the user rejects; GUI_ROADMAP §9.4, AI_SECURITY_MODEL §2.2.2).

### 7.3 Cost caps (tokens + render loops)

Two budgets, both surfaced live and both hard-stoppable:

- **Token budget.** `AgentSession` accumulates `input/output/cached` tokens per turn (from `TurnResult`) against a per-session and per-task cap. **Prompt caching** is used aggressively to cut cost: the system prompt + grammar resource + tool definitions are stable across a session, so Claude's `cache_control: {type:"ephemeral"}` on those blocks yields ~90% cheaper cached reads ([prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)) (Gemini and some local stacks have analogous context caching; the adapter sets it where available, no-ops elsewhere). The grammar resource is large and constant → an ideal cache breakpoint.
- **Render budget.** Each agent-initiated render counts against a render-count cap and (optionally) a wall-clock cap. The vision loop (§5.4) is the main consumer; it prefers **preview quality + region render** and **caches the last framebuffer** so "look again" is free. When a cap trips the loop stops and asks the user to raise it or confirm continuation.

Defaults are conservative (e.g. a handful of renders, a modest token ceiling) and editable in Settings → AI. Caps are checked *before* each turn and before each render tool, so the user is never surprised by a runaway L3 task.

### 7.4 Scoped permissions

Three capability scopes, independent of level: **`read`** (resources), **`edit`** (mutate scene via the controller), **`render`/`spend`** (run renders, which cost money on cloud + time). A level only unlocks the *behavior* (confirm vs. auto); a scope unlocks the *family*. e.g. an L2 session with `read`+`edit` but **not** `render` can rewrite the scene autonomously but must hand back to the user to render. Transport- and capability-level guardrails — loopback-only server, no shell, the **`ProjectRootJail`** (no model-supplied arbitrary/`..` paths; Save-As via the human OS dialog only), the `IOutboundUrlPolicy` SSRF chokepoint, and **secret redaction** (keys never in transcripts, logs, or the "show me the code" panel) — are owned by [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4/§3/§9; this doc enforces the *per-action* scope check inside the loop. The full autonomy×scope threat mapping is AI_SECURITY_MODEL §10.

### 7.5 Honest caveats (especially local models)

- **Local tool-use is weaker.** Smaller local models call tools less reliably, emit malformed JSON args more often, and (on Ollama's OpenAI-compatible endpoint) can **drop tool calls under streaming** (§3.4). Mitigations baked in: lean on the **`validate` tool** + the descriptor-generated schema (validity-by-construction), the `supportsToolStreaming=false` non-streamed-tool-turn fallback, JSON-arg repair-and-retry (one bounded retry on parse failure, then surface to user), and a **clear UI note** that local models trade capability for privacy — **but that privacy holds only for a loopback endpoint**; a "Local" provider pointed at a remote host is a cloud-class egress and is labeled as such (§4.1). Prefer Ollama's native `/api/chat` over `/v1/chat/completions` for tool turns where the local adapter can target it.
- **Vision is not universal.** Text-only models can't see renders; the loop degrades to numeric convergence (§5.4) and says so.
- **Cloud costs real money and time.** Repeated renders + tokens add up; the caps (§7.3) and preview/region/cache discipline are the answer, and the diff-gate means the user is never paying for an applied change they didn't see.
- **The model can be wrong.** Undo + diff-review + the engine's own descriptor validation are the safety net; the AI is an authoring surface, not an oracle.

---

## 8. Android — chat is the mobile-native headline

Per GUI_ROADMAP §3 and §10.4, **the agent runtime is shared C++ consumed via JNI**, so Android gets full AI operation essentially for free — and **chat is more natural on mobile than on desktop**: "talk to your renderer from your phone" is a legitimate Android headline.

- **Tiering (GUI_ROADMAP §10.4):** the **full LLM chat + agent operation is Tier A (must-have)** on Android, alongside scene browse/open, render view, basic param edit, and timeline scrub. It is *not* deferred like the node graph (Tier C).
- **What's reused unchanged:** `ILLMProvider` + the three adapters, `AgentSession`, `AgentTranscript`, the diff/"show me the code" model, cost accounting, scope/level enforcement — all the C++ above, behind JNI.
- **What's Android-specific:** the **chat bubble UI** (Jetpack Compose `LazyColumn`), **secret storage** (Keystore-backed EncryptedSharedPreferences, §4.2), the **OAuth Custom Tab** hand-off (§4.3), and an `IHttpClient` impl if RISE doesn't ship shared libcurl (OkHttp via JNI is the idiomatic fallback). The vision loop works wherever the chosen model supports vision and the device can render a preview.
- **Mobile UX shape:** a full-screen chat tab (not a side panel), the framebuffer thumbnail inline in the transcript when the agent renders, and the "show me the code" diff as a swipe-to-reveal sheet. Default autonomy **L1** with the diff-gate, identical to desktop.

Per the roadmap rule, this is the doc's required Android-tier note: **Tier A, chat-first, shared C++ via JNI, only credential storage + bubble UI + browser hand-off are platform code.**

---

## 9. Chat UX & the "show me the code" duality

### 9.1 Where the panel lives

- **macOS (SwiftUI).** A new collapsible **AI panel**, sibling to the existing left-edge `SceneEditorPanel` (`ContentView.swift` already does a slide-in left sidebar with a draggable resize handle — the AI panel mirrors that pattern, or docks on the right next to `PropertiesPanel`). The transcript is a `List` of bubbles bound to the shared `AgentTranscript` via a new `AIChatViewModel`/bridge analogous to `RenderViewModel`'s wiring of `RISEBridge` blocks (streaming deltas arrive on `@MainActor` like the progress/image blocks do today).
- **Windows (Qt).** A new `QDockWidget` (or a pane in `m_mainSplitter` beside `SceneEditor`), with a `QListView`/custom delegate for bubbles, fed by signals from the shared runtime exactly as `MainWindow` wires `RenderEngine`/`ViewportBridge` signals today (`MainWindow.cpp`).
- **Android (Compose).** Full-screen chat tab (§8).

In all three, the chat panel is **just a view of the shared transcript model**; no chat logic lives in the shell.

### 9.2 Transcript model (shared) vs. bubbles (platform)

`AgentTranscript` (library) holds the ordered turns (user / assistant text / tool-call + result / rendered-image / diff-proposal / system notes) plus token + render running totals. It exposes deltas (append, stream-into-last) the UI subscribes to. Bubble rendering, markdown/code styling, image thumbnails, and the diff viewer are platform widgets over that model. Because the model is shared, conversation state, cost display, and the diff are identical across platforms by construction.

### 9.3 The "show me the code" duality (load-bearing)

This is the chat panel's signature and the embodiment of "text is canonical" (GUI_ROADMAP §4, §9.5): **every AI action is mirrored as the `.RISEscene` diff it produces.** When the agent edits a light, the chat shows a friendly sentence ("Warmed the key light to 3200 K and raised power 30%") *and* the panel shows the exact unified diff against the scene text. Properties:

- The diff is the **same artifact** as the L1 review gate (§7.2) — pre-apply it's a proposal to approve/reject; post-apply it's the receipt of what changed.
- It teaches the scene language: users learn the text by watching the AI write it, then hand-edit with confidence. The AI and the hand-editor are two surfaces over the *same* text (principle #1).
- It is the audit trail: every mutation is visible, reviewable, and (via the controller) undoable. "The 'show me the code' panel and the AI spine are the same surface" (GUI_ROADMAP §9.5).
- Clicking a diff jumps the scene editor to that span (reuse the editor's existing span/selection plumbing).

---

## 10. Phasing — mapped to the GUI_ROADMAP A0–A3 spine

This doc's runtime work attaches to GUI_ROADMAP §11's AI spine. A0 (the agent server + read resources + `validate` + render-control) is owned by the sibling doc; the runtime begins at A1.

| Spine | Roadmap theme | This doc delivers | Depends on |
|---|---|---|---|
| **A0** | Agent server: read scene + grammar + framebuffer + `validate` + render-control | (sibling doc) — runtime consumes its catalog as `ToolSpec[]` | mostly exists; text-loop needs no *new* round-trip save (structured save already ships, CURRENT_STATE_AUDIT §1) |
| **A1** | In-app chat panel + provider adapters; **L0→L1** diff-review | `ILLMProvider` + 3 adapters; `IHttpClient` (+ `IOutboundUrlPolicy`); **`ICredentialStore : IReference`** (3 backends, §16; **not** `ISecretStore`) + sign-in UX; `AgentSession` loop + `ContextAssembler` (§5.6); `AgentTranscript` + chat panel + "show me the code"; `SecretRedactor`; prompt-cache wiring; token cost cap | A0 |
| **A2** | Edit/create tools via `SceneEditController`; **L2** operate-with-guardrails + scopes | scope enforcement in the loop; structured edits through `SceneEditController::Commit` (already persistent for transform/property/created-camera; **wholesale `apply_scene_text` only for non-camera creation**); render cost cap | **non-camera creation persistence only** (structured save for the other classes already ships — CURRENT_STATE_AUDIT §1; owner [ENTITY_CREATION.md](ENTITY_CREATION.md)) |
| **A3** | Vision feedback loop + curated external MCPs; **L3** autonomous tasks | the see→adjust→render→see loop (§5.4) via `RenderCoordinator` jobs; render-iteration caps + framebuffer cache; L3 multi-step planner + checkpoint approvals; vision-capability gating; **the external-MCP client runtime `ExternalServerHub` (§5.7) — transport/lifecycle, namespaced tool merge, timeouts/cancel, per-server credential routing, version-change re-review — gating the curated-external-MCP capability**; per-server external-MCP trust tiers (AI_SECURITY_MODEL §6) | A1, A2; **external-MCP consumption blocked on §5.7 landing** |

A1 is the high-leverage MVP and is **buildable on today's engine** (load/rasterize/poll-framebuffer + the existing controller) with no dependency on an unbuilt save path — structured save already ships, and text-as-truth lets the agent rewrite-and-reload for the rest.

---

## 11. Open questions / uncertainties (flagged)

1. **HTTP/SSE client: shared libcurl vs. per-platform `IHttpClient`.** Shared maximizes code reuse (SSE parsing once) but adds a dep; per-platform adds shells but no dep. Both sit behind `IHttpClient`; pick in A1. (§2)
2. **Cross-platform secret wrapper vs. native calls.** `hrantzsch/keychain` collapses the desktop two into shared code; native `SecItem*`/`Cred*` calls are ~40 lines each and dep-free. Android is bespoke either way. (§4.2)
3. **Schema transcoding fidelity for Gemini.** Confirm the `Describe()`-generated schemas never emit `$ref`/`$defs`/`anyOf` that Gemini rejects; if they do, the `SchemaTranscoder` must inline/flatten. Needs a round-trip test against live Gemini validation. (§3.5)
4. **Local-model tool reliability bar.** Which local models clear a usable tool-call bar with the RISE schema + `validate` retry loop? Empirical; document a "known-good local models" list. The OpenAI-compatible-streaming-drops-tool-calls caveat is real and adapter-contained, but per-model behavior varies. (§3.4, §7.5)
5. **OAuth availability & exact flows.** Confirm the current first-party OAuth/login flows and scopes for Claude and Gemini sign-in (vs. API-key-only); API-key entry is the guaranteed fallback if a given OAuth flow isn't available to a desktop app. (§4.1, §4.3)
6. **`validate` dry-run support in the parser** (GUI_ROADMAP §13 #5). Design now owned by [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) (isolated throwaway `Job`, not a capturing mock). The self-correction loop's reliability depends on that no-side-effect pass; the runtime assumes it and degrades to "rewrite + reload + read log" if absent.
7. **Render serialization with the agent — RESOLVED in design.** The clean library-level entry is the `RenderCoordinator` ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §6): the agent submits a `RenderClass::Agent` (or isolated) job and the coordinator does the viewport suspend/resume on its own thread, so no per-platform view-model path is needed. (§5.2)

---

## 12. Non-goals (deliberately NOT doing)

- **A second mutation path for AI edits.** All AI writes go through `SceneEditController` + undo (principle #6); no bespoke AI writer. (§7.1)
- **A hosted/proxied API key.** Tokens are on the user's account; RISE is not an LLM reseller. Local is the zero-cost/privacy path. (§4.1)
- **Re-implementing the loop per platform.** The runtime is shared C++; Swift/Qt/Compose only render the transcript and store the secret. (§2)
- **Pretending local models match cloud.** Honest UI about weaker tool-use + no-vision; lean on `validate`. (§7.5)
- **Owning RISE's first-party tool catalog or server transport.** That's [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md); this doc consumes it. (The *client* runtime for consuming **external** MCP servers — `ExternalServerHub`, §5.7 — IS this doc's, because it is agent-loop infrastructure, not part of RISE's exposed server surface; its trust/allowlist side is AI_SECURITY_MODEL §6.)
- **Unbounded autonomy.** Even L3 is hard-capped (tokens, renders, wall-clock) and diff-visible. (§6, §7.3)
- **Driving the GUI by simulated clicks / screen-scraping.** The AI speaks the native scene language and calls typed tools — the whole point of text-as-truth (GUI_ROADMAP §4). No UI automation.
- **A chat-only product.** Chat is one of *two parallel* authoring surfaces over the text; it never replaces the GUI or hand-editing (principle #1).

---

## 13. How this doc consumes the sibling specs (deference map)

| Sibling | What this runtime defers there |
|---|---|
| [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) | the tool/resource catalog the adapter translates to `ToolSpec[]`; transports; scope tiers; the `validate`/`apply_scene_text`/`render` tools the loop calls. |
| [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) | `ProjectRootJail`, `IOutboundUrlPolicy`, `SecretRedactor` (egress-only), the always-confirm set, prompt-injection quarantine, cloud-egress consent, and the autonomy×scope threat map (§10). **External-MCP split:** AI_SECURITY §6 owns the *trust/allowlist/credential-isolation* side; this doc §5.7 owns the *client runtime* (transport/lifecycle/namespacing/merge/timeouts/version) — they cross-reference. |
| [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) | L1 staging = a proposed transaction (§7); `Commit`/`baseEpoch`/conflict-re-check on approve (§5.5); undo attribution for AI edits; the shared "show me the code" diff (`RenderTransactionDiff`). |
| [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) | agent renders are `RenderClass::Agent`/isolated coordinator jobs; the folded-in viewport suspend/restart (§5.2, §5.4); stale-epoch preview drop. |
| [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md) | the side-effect-free `validate` the self-correction loop depends on (§5.5, §11.6). |
| [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) | ground truth for "round-trip save ships; only non-camera creation is the gap" (§1, §7.1, §10). |

---

## 14. File-link index

- Umbrella + principles + §15 template + §16 decisions (`ICredentialStore`, `src/Library/Agent/`, no bare `MCP`, API-key-first): [../GUI_ROADMAP.md](../GUI_ROADMAP.md)
- Code-verified ground truth: [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md)
- Tool/resource catalog + transports + scopes: [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md)
- Threat model: [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md)
- State / epochs / staging / undo attribution: [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)
- Render arbiter: [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md)
- `validate` dry-run: [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md)
- The one mutation path: [../../src/Library/SceneEditor/SceneEditController.h](../../src/Library/SceneEditor/SceneEditController.h)
- Credential interface convention: [../../src/Library/Interfaces/IReference.h](../../src/Library/Interfaces/IReference.h)
- Provider docs: Claude tool use ([how-tool-use-works](https://platform.claude.com/docs/en/agents-and-tools/tool-use/how-tool-use-works), [streaming](https://platform.claude.com/docs/en/build-with-claude/streaming), [prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)); Gemini ([function calling](https://ai.google.dev/gemini-api/docs/function-calling)); Ollama ([tool calling](https://docs.ollama.com/capabilities/tool-calling))
- ABI discipline for any additive C-ABI export (refer by name; the skill lives at `.claude/skills/abi-preserving-api-evolution/`): **abi-preserving-api-evolution**

---

## 15. Acceptance criteria (per [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §15)

- **Tests.** (1) `ILLMProvider` round-trip per adapter: a fixed `[messages, tools, ToolChoice]` serializes to each dialect and the streamed reply parses back to the same `TurnResult` (Claude/Gemini/OpenAI-local), incl. the `supportsToolStreaming=false` non-streamed-tool-turn fallback. (2) `SchemaTranscoder` self-test: every tool schema transcodes to each provider's accepted form and re-validates (the Gemini `$ref`/`anyOf` minefield, §3.5). (3) `AgentSession` loop: a scripted tool-use turn drives `mcp.Dispatch`, feeds the result back, and terminates on `EndTurn`; cancellation flips mid-stream and skips remaining tool calls (§5.3). (4) `ContextAssembler`: with a small `contextWindowTokens`, oldest tool-result payloads are dropped before the system/grammar blocks, and untrusted payloads are `<rise:untrusted>`-wrapped (§5.6). (5) L1 staging: an act tool at L1 produces a *proposed* transaction and a diff with the live scene **byte-identical** (no mutation) until Approve, which commits with an epoch re-check (TRANSACTION_MODEL §7; cross-checked by that doc's `ConflictRebaseTest`). (6) Secret non-leak: a key in a scene comment / log never appears in a transcript, the "show me the code" diff, or a tool arg (delegated assertion to AI_SECURITY_MODEL's `SecretRedactor` suite). (7) **`baseEpoch` precondition plumbing:** a mutating tool call is stamped with the last-read `DocumentId`; a call whose `baseEpoch` lost the race returns `conflict` and is fed back (not applied); a UUID-mismatch conflict triggers re-bootstrap, not delta-rebase (§5.5); two parallel mutating calls on one base do **not** both apply silently (reject-stale-then-reread or one-`Propose`, §5.5 / MCP_TOOL_SURFACE §4 head). (8) **Local-endpoint egress classification (§4.1):** a "Local" provider with a loopback endpoint reports the no-egress indicator and sends no first-egress consent; the *same* provider repointed at a non-loopback host triggers the cloud-style first-egress consent and the "Custom remote" indicator, and its URL is subject to `IOutboundUrlPolicy` (a blocked/private range is refused) — asserted against the indicator state + a recording `IOutboundUrlPolicy`/consent stub. (9) **`ExternalServerHub` runtime (§5.7):** an external tool is merged as `ext__<id>__<name>` and cannot shadow a first-party tool name; a hung external call hits its timeout and returns `isError` without stalling the loop; user Stop cancels an in-flight external call; the provider key is **never** sent to an external server (only its `ext/<id>` credential is) — delegated where it overlaps AI_SECURITY_MODEL §6/§9 suites. **Correctness invariant:** the loop never mutates the scene except via `SceneEditController::Commit`, and an applied-then-undone AI edit leaves the scene byte-identical on save (reuses the TRANSACTION_MODEL NoOp byte-identity discipline) — the runtime is orchestration; integrators are byte-identical.
- **Platform parity.** The entire runtime — `ILLMProvider` + 3 adapters, `AgentSession`, `ContextAssembler`, `AgentTranscript`, diff/"show me the code", cost accounting, scope/level enforcement — is **shared C++**, identical on macOS / Windows / Android. Per-platform: only the **chat bubble UI**, the **`ICredentialStore` backend**, the **OAuth browser hand-off** (phase 2), and an `IHttpClient` impl if libcurl isn't shared. **Android is Tier A** (chat-first, default L1; §8) — the same shared runtime via JNI; graceful degradation is cosmetic only (mobile chat tab vs. side panel). The vision loop works wherever the model supports vision and the device can render a preview.
- **Performance budget.** The agent runtime is off the render hot path — **zero production-render regression** (it never runs inside the integrator; cite the L8 ~0.4% bar). Renders it initiates obey the `RenderCoordinator`'s budgets (RENDER_COORDINATOR §9). Streaming-token UI latency is bounded by the SSE read + a `@MainActor`/queued-signal hop, the same as today's render-progress marshaling. Prompt caching keeps the stable grammar/system/tool blocks cached (~90% cheaper cached reads) so repeated turns don't re-send the large grammar.
- **Memory budget.** Per session: the `AgentTranscript` (bounded; oldest turns trimmable), the `ContextAssembler`'s working set (one assembled context + cached stable blocks), and the staging area's proposed-transaction `SceneEdit` vectors (a handful). No per-pixel/per-sample allocation; the framebuffer cache reuses the coordinator's isolated-film buffer. Peak RSS delta: negligible against a render.
- **Accessibility.** The chat composer, transcript, Stop button, the diff approve/reject controls, and the egress indicator are keyboard-reachable and screen-reader-labeled; the egress indicator pairs colour with text across its three states ("Cloud: <provider> — content leaves this machine" / "Custom remote (<host>) — content leaves this machine" / "Local — no data leaves this machine"), never colour-only; the "show me the code" diff exposes a keyboard path to next/previous change. No numpad dependence.
- **Packaging.** No new mandated third-party dependency beyond the `IHttpClient`/libcurl + secure-store choices weighed in §2/§4. Per the [../../CLAUDE.md](../../CLAUDE.md) file-add checklist, every new `.cpp`/`.h` in `src/Library/Agent/` (`ILLMProvider`, the adapters, `AgentSession`, `ContextAssembler`, `AgentTranscript`, `ICredentialStore`, and the A3 `ExternalServerHub` + `IExternalServerTransport` concretes from §5.7) must be added to **all five** build projects.
- **Migration.** No scene-format change. ABI: the runtime is a net-new subsystem behind a C-ABI surface added to the **end** of `RISE_API.h` (additive, out-of-tree-safe — refer to the **abi-preserving-api-evolution** skill at `.claude/skills/abi-preserving-api-evolution/`). No edits to existing signatures. Existing `.RISEscene` files and headless scripts are unaffected.
- **Rollback.** The whole agent runtime is behind the "Enable AI access" toggle (default off; shared with [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §12 rollback). Each tier — cloud provider, external MCP, L2/L3 autonomy — is independently default-off. Disabling AI removes the chat panel and all egress and never affects saved scenes or non-AI GUI behavior.

### Android tier note
**Tier A** (must-have, GUI_ROADMAP §10.4 / §16). The runtime is 100% shared C++ via JNI, so Android inherits the adapters, loop, transcript, scopes, and cost caps with **zero** Android-specific logic; only the chat bubble UI (Compose `LazyColumn`), the `ICredentialStore` Keystore backend, the OAuth Custom Tab (phase 2), and an optional OkHttp `IHttpClient` are platform code. Default autonomy **L1** with the diff-gate, identical to desktop. Chat-first is the mobile-native headline ("talk to your renderer from your phone", §8).
