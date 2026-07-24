//////////////////////////////////////////////////////////////////////
//
//  RISEAgentChatBridge.h - Pure-Objective-C interface to the C++
//    AgentChatLoop (Facet 5 slice B2: the LLM chat panel).
//
//    Wraps the sans-IO chat-loop core (src/Library/Agent/
//    AgentChatLoop.h) for the Swift driver.  The bridge OWNS one
//    AgentChatLoop and is PURE STATE — it performs no I/O, holds no
//    pointers into the viewport bridge / controller / job, and never
//    touches the network.  The Swift ChatViewModel drives the loop:
//
//        bridge.addUserMessage(text)
//        repeat {
//            let req  = bridge.buildRequest(apiKey: key)   // Swift does HTTP
//            let step = bridge.handleResponse(status:body:)
//            step.toolCalls are opaque tokens: for each, the driver
//            asks toolCallToJsonRpcLine, executes it via the viewport
//            bridge's agentHandleLine (main thread), and hands the
//            response line back through addToolResult.
//        } while step.kind == toolCalls
//
//    THREADING: single-threaded, like the loop it wraps.  Every method
//    must be called from the same thread — in practice Swift's
//    @MainActor (the main thread).  The HTTP round-trip itself happens
//    off-main inside URLSession; only the build/handle bookkeeping
//    runs here.
//
//    LIFETIME: owned by the Swift ChatViewModel (app-lifetime, since
//    provider/model selection outlives any one scene).  The loop holds
//    NO pointers into the per-scene controller/dispatcher, so there is
//    no teardown ORDER constraint against the viewport bridge — the
//    constraint is on the DRIVER: it must stop calling the viewport
//    bridge's agentHandleLine once the scene closes (ChatViewModel
//    cancels its in-flight turn Task on clearScene) and reset this
//    bridge so the stale-scene conversation doesn't leak into the next
//    scene.
//
//    SECRET HYGIENE: the API key is passed INTO buildRequest per call
//    and appears only in the returned request's auth header (the C++
//    contract).  Nothing here logs or retains it.
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Mirrors RISE::Agent::ChatProvider.
typedef NS_ENUM(NSInteger, RISEAgentChatProvider) {
    RISEAgentChatProviderAnthropic = 0,
    RISEAgentChatProviderGemini    = 1,
    RISEAgentChatProviderOpenAI    = 2,
    RISEAgentChatProviderXAI       = 3,
    RISEAgentChatProviderLocal     = 4,
};

/// Mirrors RISE::Agent::ChatStepResult::Kind.
typedef NS_ENUM(NSInteger, RISEAgentChatStepKind) {
    RISEAgentChatStepKindToolCalls     = 0,  ///< execute step.toolCalls, then loop
    RISEAgentChatStepKindFinalText     = 1,  ///< turn complete; show step.finalText
    RISEAgentChatStepKindProviderError = 2,  ///< branch on step.errorKind
};

/// Mirrors RISE::Agent::ChatErrorKind.  The Swift driver's per-kind
/// UX contract (from AgentChatCodecs.h / AgentChatLoop.h):
///   Http         — retriable (offer Retry; repeated HTTP 400s should
///                  offer "Reset conversation" — honest poison scoping)
///   Parse        — hostile/garbled body; nothing recorded; show error
///   Provider     — degenerate/unexpected turn; nothing recorded
///   Refusal      — surface the refusal text; do not auto-retry
///   MaxTokens    — truncated reply DISCARDED; a verbatim retry will
///                  likely truncate again — user must rephrase/narrow
///                  (NEVER auto-retry)
///   IterationCap — per-turn tool-round cap; continuing requires a NEW
///                  user message (which resets the counter)
///   Misuse       — driver-contract violation; should not happen (log
///                  to stderr, show a generic error)
typedef NS_ENUM(NSInteger, RISEAgentChatErrorKind) {
    RISEAgentChatErrorKindNone         = 0,
    RISEAgentChatErrorKindHttp         = 1,
    RISEAgentChatErrorKindParse        = 2,
    RISEAgentChatErrorKindProvider     = 3,
    RISEAgentChatErrorKindRefusal      = 4,
    RISEAgentChatErrorKindMaxTokens    = 5,
    RISEAgentChatErrorKindIterationCap = 6,
    RISEAgentChatErrorKindMisuse       = 7,
};

/// Mirrors RISE::Agent::ChatTranscriptEntry::Role.
typedef NS_ENUM(NSInteger, RISEAgentChatRole) {
    RISEAgentChatRoleUser        = 0,
    RISEAgentChatRoleAssistant   = 1,
    RISEAgentChatRoleToolResults = 2,
};

/// One user-supplied reference image attachment (Model-B F5 chat image
/// attachments) — mirrors RISE::Agent::ChatAttachment.  `mimeType` MUST
/// be one of the four the provider APIs accept (image/png, image/jpeg,
/// image/gif, image/webp) — the driver rejects anything else BEFORE
/// constructing this (see ChatViewModel's attach flow); the C++ core
/// passes an unrecognized mimeType through rather than gating on it
/// (documented in AgentChatCodecs.h).  `base64Data` carries NO data-URL
/// prefix.  NEVER LOG either field.
@interface RISEAgentChatAttachment : NSObject
- (instancetype)initWithMimeType:(NSString *)mimeType
                       base64Data:(NSString *)base64Data;
@property (nonatomic, readonly, copy) NSString *mimeType;
@property (nonatomic, readonly, copy) NSString *base64Data;
@end

/// One tool call the model requested — an OPAQUE token.  The Swift
/// driver reads `name` (for the "→ propose_patch …" activity line)
/// and passes the token back verbatim to toolCallToJsonRpcLine: /
/// addToolResult:.  The token privately carries the full C++
/// ChatToolCall (args + id-synthesis flag), so round-tripping through
/// it preserves everything the loop needs for result matching.
@interface RISEAgentChatToolCall : NSObject
@property (nonatomic, readonly, copy) NSString *callId;
@property (nonatomic, readonly, copy) NSString *name;
/// The call's arguments, as sent (mirrors RISE::Agent::ChatToolCall::argsJson) --
/// GUI stage 2: exposed for the transcript row's expandable detail view
/// (argsJson + the eventual result line), alongside `outcomeLine`.
@property (nonatomic, readonly, copy) NSString *argsJson;
@end

/// A fully-described HTTP request the Swift driver performs.
/// `isEmpty` mirrors the loop's empty-request convention (url == ""
/// when the transcript is empty) — an empty request MUST NOT be sent.
@interface RISEAgentChatRequest : NSObject
@property (nonatomic, readonly, copy) NSString *url;
/// Header name → value.  Includes content-type and the auth header.
/// (The C++ side emits unique header names, so a dictionary is a
/// faithful carrier.)
@property (nonatomic, readonly, copy) NSDictionary<NSString *, NSString *> *headers;
@property (nonatomic, readonly, copy) NSString *body;
@property (nonatomic, readonly) BOOL isEmpty;
@end

/// The outcome of one chat step (one HTTP round-trip).
@interface RISEAgentChatStep : NSObject
@property (nonatomic, readonly) RISEAgentChatStepKind kind;
/// Non-empty iff kind == ToolCalls; the pending set, in order.
@property (nonatomic, readonly, copy) NSArray<RISEAgentChatToolCall *> *toolCalls;
/// Filled iff kind == FinalText (possibly empty, e.g. a thinking-only turn).
@property (nonatomic, readonly, copy) NSString *finalText;
/// Filled iff kind == ProviderError.
@property (nonatomic, readonly, copy) NSString *errorMessage;
/// Set on every ProviderError; None otherwise.
@property (nonatomic, readonly) RISEAgentChatErrorKind errorKind;
/// The model's interim narration for THIS turn — filled for BOTH
/// ToolCalls (text alongside the calls, possibly empty) and FinalText
/// (== finalText).  Empty on ProviderError.
@property (nonatomic, readonly, copy) NSString *assistantDisplayText;
/// GUI stage 2: the model's reasoning/thinking text for THIS turn —
/// mirrors RISE::Agent::ChatStepResult::reasoningText (see its doc).
/// Filled for BOTH ToolCalls and FinalText; "" when the provider
/// exposes no reasoning for this turn (every Gemini turn, a plain
/// gpt-family turn, or any ProviderError).  DISPLAY-ONLY.
@property (nonatomic, readonly, copy) NSString *reasoningText;
@end

@interface RISEAgentChatBridge : NSObject

/// Constructs with the ChatGPT/OpenAI provider + its default model
/// (mirrors AgentChatLoop's default construction).
- (instancetype)init;

/// Clear the transcript, pending tool state, and the per-turn round
/// counter.  Provider/model selection AND the skill index are kept.
- (void)reset;

/// Select the provider (and optionally a model id; empty/nil = the
/// provider's default).  RESETS THE TRANSCRIPT — assistant turns are
/// provider-native JSON and cannot cross providers; the rule holds
/// even when re-selecting the current provider (uniform, so the GUI
/// can state it plainly and confirm before switching mid-conversation).
- (void)setProvider:(RISEAgentChatProvider)provider
            modelId:(NSString * _Nullable)modelId;

@property (nonatomic, readonly) RISEAgentChatProvider provider;
@property (nonatomic, readonly, copy) NSString *modelId;

/// The provider's built-in default model id (for placeholder text in
/// the settings UI).  Pure function.
+ (NSString *)defaultModelIdForProvider:(RISEAgentChatProvider)provider;

/// Mirrors AgentChatLoop::kMaxToolRoundsPerTurn (tool rounds allowed
/// per conversation-turn before IterationCap trips).  For UX copy.
@property (class, nonatomic, readonly) NSInteger maxToolRoundsPerTurn;

/// Mirrors AgentChatLoop::kMaxLiveUserImages (user-attached reference
/// images kept live across the whole conversation before the oldest is
/// elided — see AgentChatLoop.h "USER IMAGE RETENTION").  For UX copy
/// and for the panel's own per-message attach-count cap.
@property (class, nonatomic, readonly) NSInteger maxLiveUserImages;

/// Facet 5 slice S1: the "Available skills" section appended to every
/// subsequent request's system prompt.  `indexText` is the rendered
/// index (one "name -- hook" line per skill) the driver fetched ONCE
/// by driving the read_skill verb (no name) through the viewport
/// bridge's agentHandleLine at scene open.  Empty string omits the
/// section.  Survives reset / setProvider (provider-neutral config).
- (void)setSkillIndex:(NSString *)indexText;

/// GUI stage 3 (prompt triage): override the ENTIRE system prompt for
/// this bridge's loop — mirrors AgentChatLoop::SetSystemPromptOverride
/// (see its doc for the full contract).  A non-empty `prompt` REPLACES
/// the base co-editing prompt and the skills section wholesale; an
/// empty string clears the override.  Intended for a SEPARATE,
/// short-lived bridge instance (a triage pass), never the main
/// scene-editing bridge — deliberately NOT cleared by -reset or
/// -setProvider:modelId:.
- (void)setSystemPromptOverride:(NSString *)prompt;

/// Append a user text message (resets the per-turn tool-round
/// counter; flushes any pending tool calls with synthesized cancelled
/// results — the designed Stop/interrupt path).  Empty or whitespace-
/// only text is a documented no-op.
- (void)addUserMessage:(NSString *)text;

/// Append a user message carrying reference-image attachments
/// alongside the text (Model-B F5 chat image attachments) — see
/// AgentChatLoop.h "USER IMAGE RETENTION" for the persistence + cap
/// policy.  `text` may be empty when `attachments` is non-empty (an
/// attachment-only message); the no-op above still applies when BOTH
/// are empty.  Same flush/reset behavior as the text-only overload.
- (void)addUserMessage:(NSString *)text
            attachments:(NSArray<RISEAgentChatAttachment *> *)attachments
    NS_SWIFT_NAME(addUserMessage(_:attachments:));

/// Build the next HTTP request.  `apiKey` goes into the auth header
/// only — not retained, never logged.  Check `isEmpty` before sending.
- (RISEAgentChatRequest *)buildRequestWithApiKey:(NSString *)apiKey
    NS_SWIFT_NAME(buildRequest(apiKey:));

/// Feed back the raw HTTP response.  On ToolCalls the returned step's
/// `toolCalls` become the pending set the driver must answer (or
/// abandon — the next addUserMessage/buildRequest synthesizes
/// cancelled results).  Never throws; hostile bodies come back as
/// ProviderError with nothing recorded.
- (RISEAgentChatStep *)handleResponseWithStatus:(NSInteger)httpStatus
                                           body:(NSString *)rawBody
    NS_SWIFT_NAME(handleResponse(status:body:));

/// Translate one pending tool call into the JSON-RPC request line the
/// viewport bridge's agentHandleLine consumes.  `rpcId` is a driver-
/// chosen monotonic id (echoed in the response envelope).
- (NSString *)toolCallToJsonRpcLine:(RISEAgentChatToolCall *)call
                              rpcId:(NSInteger)rpcId
    NS_SWIFT_NAME(toolCallToJsonRpcLine(_:rpcId:));

/// Record the raw JSON-RPC response ENVELOPE line for one pending
/// call.  First result per call id wins; unknown ids are ignored.
- (void)addToolResult:(RISEAgentChatToolCall *)call
  jsonRpcResponseLine:(NSString *)line
    NS_SWIFT_NAME(addToolResult(_:jsonRpcResponseLine:));

/// GUI stage 2: the same deterministic one-line outcome summary the
/// C++ core computes for a tool's ChatToolDisplaySummary (e.g.
/// "160x120, luma 0.19", "3/4 applied", "error: <msg>") — wraps
/// RISE::Agent::AgentChatLoop::ToolOutcomeLineForDisplay.  Pure
/// function of `call` + `line`; does not touch the loop's transcript
/// or pending state, so it is safe to call for a result the driver is
/// handling OUT OF BAND (the async render-tool path) before/without
/// ever calling addToolResult:jsonRpcResponseLine: for it.
- (NSString *)toolOutcomeLineFor:(RISEAgentChatToolCall *)call
                       resultLine:(NSString *)line
    NS_SWIFT_NAME(toolOutcomeLine(for:resultLine:));

/// Eval-harness E1: begin recording this chat session's trajectory to a
/// per-session JSONL file under `directory` (created as needed), rotating
/// old files first (keeps ~50 newest / ~200 MB).  `scenePath`/`headVersion`
/// are captured in the session record ("" / -1 when unknown).  Pass
/// enabled:NO to DETACH recording entirely.  Redaction of api-key-shaped
/// content is UNCONDITIONAL in the core regardless of this toggle.
- (void)startTrajectoryInDirectory:(NSString *)directory
                         scenePath:(NSString *)scenePath
                       headVersion:(long long)headVersion
                           enabled:(BOOL)enabled
    NS_SWIFT_NAME(startTrajectory(directory:scenePath:headVersion:enabled:));

/// Record one LLM HTTP round (driver-measured status/body/latency) into
/// the `llm` trajectory record -- call just BEFORE handleResponse.  No-op
/// when recording is not active.  Uses the request most recently built by
/// buildRequest (auth headers already stripped from the cached copy).
- (void)recordHttpRoundWithStatus:(NSInteger)httpStatus
                             body:(NSString *)rawBody
                        elapsedMs:(int64_t)elapsedMs
    NS_SWIFT_NAME(recordHttpRound(status:body:elapsedMs:));

/// Emit the terminal `summary` record with `status` and detach the
/// session (a fresh session begins on the next recorded action).
- (void)finishTrajectoryWithStatus:(NSString *)status
    NS_SWIFT_NAME(finishTrajectory(status:));

/// Pending tool calls awaiting addToolResult (0 when idle).
@property (nonatomic, readonly) NSUInteger pendingToolCallsCount;

/// Transcript accessors for display.  Out-of-range index returns
/// RISEAgentChatRoleUser / the empty string (the loop's bounds-safe
/// convention) — never throws.
@property (nonatomic, readonly) NSUInteger transcriptSize;
- (RISEAgentChatRole)transcriptRoleAtIndex:(NSUInteger)index;
- (NSString *)transcriptDisplayTextAtIndex:(NSUInteger)index;

@end

NS_ASSUME_NONNULL_END
