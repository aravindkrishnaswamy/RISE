//////////////////////////////////////////////////////////////////////
//
//  RISEAgentChatBridge.mm - Obj-C++ implementation wrapping the C++
//    AgentChatLoop for the Swift chat panel (Facet 5 slice B2).
//
//    Thin type-marshalling only: NSString <-> std::string via UTF8,
//    C++ structs -> small immutable ObjC value objects.  ALL
//    correctness-bearing logic (transcript state, provider codecs,
//    flush/interrupt semantics, iteration cap, image retention) lives
//    in the tested C++ core — see AgentChatLoop.h for the contract.
//
//    SECRET HYGIENE: nothing in this file logs.  The API key passes
//    straight through to AgentChatLoop::BuildRequest, which places it
//    only in the returned auth header.
//
//////////////////////////////////////////////////////////////////////

#import "RISEAgentChatBridge.h"

#include "Agent/AgentChatLoop.h"
#include "Agent/ChatTrajectory.h"

#include <ctime>
#include <string>
#include <utility>
#include <vector>

using namespace RISE;

// ============================================================
// NSString <-> std::string helpers.  A nil NSString and a
// UTF8String that comes back null both map to the empty string
// (the C++ side treats empty as its documented no-op / omit
// convention throughout).
// ============================================================
static std::string ToStd(NSString * _Nullable s) {
    if (!s) return std::string();
    const char* utf8 = [s UTF8String];
    return std::string(utf8 ? utf8 : "");
}

static NSString *ToNS(const std::string& s) {
    NSString* out = [NSString stringWithUTF8String:s.c_str()];
    // A non-UTF8 byte sequence (shouldn't happen; the loop emits JSON)
    // degrades to the empty string rather than nil so the nonnull
    // ObjC surface holds.
    return out ?: @"";
}

@implementation RISEAgentChatAttachment

- (instancetype)initWithMimeType:(NSString *)mimeType
                       base64Data:(NSString *)base64Data {
    self = [super init];
    if (self) {
        _mimeType   = [mimeType copy];
        _base64Data = [base64Data copy];
    }
    return self;
}

@end

// Class extension: private initializer carrying the FULL C++
// ChatToolCall (args + id-synthesis flag ride along invisibly), so
// the opaque token round-trips everything result matching needs.
@interface RISEAgentChatToolCall ()
- (instancetype)initWithCall:(const Agent::ChatToolCall&)call;
- (Agent::ChatToolCall)cppCall;
@end

@implementation RISEAgentChatToolCall {
    std::string _cppId;
    std::string _cppName;
    std::string _cppArgsJson;
    BOOL        _cppIdSynthesized;
}

- (instancetype)initWithCall:(const Agent::ChatToolCall&)call {
    self = [super init];
    if (self) {
        _cppId            = call.id;
        _cppName          = call.name;
        _cppArgsJson      = call.argsJson;
        _cppIdSynthesized = call.idSynthesized ? YES : NO;
    }
    return self;
}

- (Agent::ChatToolCall)cppCall {
    Agent::ChatToolCall call;
    call.id            = _cppId;
    call.name          = _cppName;
    call.argsJson      = _cppArgsJson;
    call.idSynthesized = (_cppIdSynthesized == YES);
    return call;
}

- (NSString *)callId   { return ToNS(_cppId); }
- (NSString *)name     { return ToNS(_cppName); }
- (NSString *)argsJson { return ToNS(_cppArgsJson); }

@end

// Class extension: private initializer for RISEAgentChatRequest.
@interface RISEAgentChatRequest ()
- (instancetype)initWithRequest:(const Agent::ChatHttpRequest&)req;
@end

@implementation RISEAgentChatRequest {
    NSString *_url;
    NSDictionary<NSString *, NSString *> *_headers;
    NSString *_body;
}

- (instancetype)initWithRequest:(const Agent::ChatHttpRequest&)req {
    self = [super init];
    if (self) {
        _url  = ToNS(req.url);
        _body = ToNS(req.body);
        NSMutableDictionary<NSString *, NSString *> *headers =
            [NSMutableDictionary dictionaryWithCapacity:req.headers.size()];
        for (const std::pair<std::string, std::string>& h : req.headers) {
            headers[ToNS(h.first)] = ToNS(h.second);
        }
        _headers = [headers copy];
    }
    return self;
}

- (NSString *)url  { return _url; }
- (NSString *)body { return _body; }
- (NSDictionary<NSString *, NSString *> *)headers { return _headers; }
- (BOOL)isEmpty    { return _url.length == 0; }

@end

// Class extension: private initializer for RISEAgentChatStep.
@interface RISEAgentChatStep ()
- (instancetype)initWithStep:(const Agent::ChatStepResult&)step;
@end

@implementation RISEAgentChatStep {
    RISEAgentChatStepKind _kind;
    NSArray<RISEAgentChatToolCall *> *_toolCalls;
    NSString *_finalText;
    NSString *_errorMessage;
    RISEAgentChatErrorKind _errorKind;
    NSString *_assistantDisplayText;
    NSString *_reasoningText;
    BOOL _retryDegenerateTurn;
}

- (instancetype)initWithStep:(const Agent::ChatStepResult&)step {
    self = [super init];
    if (!self) return nil;

    switch (step.kind) {
        case Agent::ChatStepResult::Kind::ToolCalls:
            _kind = RISEAgentChatStepKindToolCalls;     break;
        case Agent::ChatStepResult::Kind::FinalText:
            _kind = RISEAgentChatStepKindFinalText;     break;
        case Agent::ChatStepResult::Kind::ProviderError:
        default:
            _kind = RISEAgentChatStepKindProviderError; break;
    }

    switch (step.errorKind) {
        case Agent::ChatErrorKind::None:
            _errorKind = RISEAgentChatErrorKindNone;         break;
        case Agent::ChatErrorKind::Http:
            _errorKind = RISEAgentChatErrorKindHttp;         break;
        case Agent::ChatErrorKind::Parse:
            _errorKind = RISEAgentChatErrorKindParse;        break;
        case Agent::ChatErrorKind::Provider:
            _errorKind = RISEAgentChatErrorKindProvider;     break;
        case Agent::ChatErrorKind::Refusal:
            _errorKind = RISEAgentChatErrorKindRefusal;      break;
        case Agent::ChatErrorKind::MaxTokens:
            _errorKind = RISEAgentChatErrorKindMaxTokens;    break;
        case Agent::ChatErrorKind::IterationCap:
            _errorKind = RISEAgentChatErrorKindIterationCap; break;
        case Agent::ChatErrorKind::Misuse:
        default:
            _errorKind = RISEAgentChatErrorKindMisuse;       break;
    }

    NSMutableArray<RISEAgentChatToolCall *> *calls =
        [NSMutableArray arrayWithCapacity:step.toolCalls.size()];
    for (const Agent::ChatToolCall& c : step.toolCalls) {
        [calls addObject:[[RISEAgentChatToolCall alloc] initWithCall:c]];
    }
    _toolCalls = [calls copy];

    _finalText            = ToNS(step.finalText);
    _errorMessage         = ToNS(step.errorMessage);
    _assistantDisplayText = ToNS(step.assistantDisplayText);
    _reasoningText        = ToNS(step.reasoningText);
    _retryDegenerateTurn  = step.retryDegenerateTurn ? YES : NO;
    return self;
}

- (RISEAgentChatStepKind)kind { return _kind; }
- (NSArray<RISEAgentChatToolCall *> *)toolCalls { return _toolCalls; }
- (NSString *)finalText    { return _finalText; }
- (NSString *)errorMessage { return _errorMessage; }
- (RISEAgentChatErrorKind)errorKind { return _errorKind; }
- (NSString *)assistantDisplayText  { return _assistantDisplayText; }
- (NSString *)reasoningText         { return _reasoningText; }
- (BOOL)retryDegenerateTurn         { return _retryDegenerateTurn; }

@end

@implementation RISEAgentChatBridge {
    // The one wrapped chat loop.  Raw new/delete, mirroring how
    // RISEViewportBridge holds its C++ objects.  Pure state — no
    // pointers into any controller / job / dispatcher, so there is no
    // cross-object teardown-order constraint (see the header note).
    Agent::AgentChatLoop* _loop;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _loop = new Agent::AgentChatLoop();
        // Context compaction is OFF until a host sets a budget; both GUI
        // drivers install the SAME shared default so they cannot drift.
        // See AgentChatLoop.h's kDefaultContextBudget* doc for the numbers
        // and their justification.  (The Windows twin is ChatPanel's
        // constructor -- keep the two in lockstep.)
        _loop->SetContextBudget(Agent::AgentChatLoop::kDefaultContextBudgetHighTokens,
                                Agent::AgentChatLoop::kDefaultContextBudgetLowTokens);
    }
    return self;
}

- (void)dealloc {
    delete _loop;
    _loop = nullptr;
}

- (void)reset {
    _loop->Reset();
}

- (void)setProvider:(RISEAgentChatProvider)provider
            modelId:(NSString * _Nullable)modelId {
    Agent::ChatProvider p = Agent::ChatProvider::Anthropic;
    if (provider == RISEAgentChatProviderGemini) {
        p = Agent::ChatProvider::Gemini;
    } else if (provider == RISEAgentChatProviderOpenAI) {
        p = Agent::ChatProvider::OpenAI;
    } else if (provider == RISEAgentChatProviderXAI) {
        p = Agent::ChatProvider::XAI;
    } else if (provider == RISEAgentChatProviderLocal) {
        p = Agent::ChatProvider::Local;
    }
    _loop->SetProvider(p, ToStd(modelId));
}

- (RISEAgentChatProvider)provider {
    if (_loop->Provider() == Agent::ChatProvider::Gemini) {
        return RISEAgentChatProviderGemini;
    }
    if (_loop->Provider() == Agent::ChatProvider::OpenAI) {
        return RISEAgentChatProviderOpenAI;
    }
    if (_loop->Provider() == Agent::ChatProvider::XAI) {
        return RISEAgentChatProviderXAI;
    }
    if (_loop->Provider() == Agent::ChatProvider::Local) {
        return RISEAgentChatProviderLocal;
    }
    return RISEAgentChatProviderAnthropic;
}

- (NSString *)modelId {
    return ToNS(_loop->ModelId());
}

+ (NSString *)defaultModelIdForProvider:(RISEAgentChatProvider)provider {
    if (provider == RISEAgentChatProviderGemini) {
        return ToNS(Agent::GeminiChatCodec().DefaultModelId());
    }
    if (provider == RISEAgentChatProviderOpenAI) {
        return ToNS(Agent::OpenAIChatCodec().DefaultModelId());
    }
    if (provider == RISEAgentChatProviderXAI) {
        // The XAI codec is a parameterized OpenAIChatCodec instance
        // (see AgentChatLoop.cpp's MakeCodec) -- there is no standalone
        // XAIChatCodec type, so mirror MakeCodec's Config here rather
        // than constructing a loop just to read its default model id.
        Agent::OpenAIChatCodec::Config cfg;
        cfg.providerName   = "xai";
        cfg.baseUrl        = "https://api.x.ai/v1/chat/completions";
        cfg.defaultModelId = "grok-4.5";
        cfg.requiresAuth   = true;
        return ToNS(Agent::OpenAIChatCodec(cfg).DefaultModelId());
    }
    if (provider == RISEAgentChatProviderLocal) {
        Agent::OpenAIChatCodec::Config cfg;
        cfg.providerName   = "local";
        cfg.baseUrl        = "http://127.0.0.1:11434/v1/chat/completions";
        cfg.defaultModelId = "opencoder";
        cfg.requiresAuth   = false;
        return ToNS(Agent::OpenAIChatCodec(cfg).DefaultModelId());
    }
    return ToNS(Agent::AnthropicChatCodec().DefaultModelId());
}

+ (NSInteger)maxToolRoundsPerTurn {
    return static_cast<NSInteger>(Agent::AgentChatLoop::kMaxToolRoundsPerTurn);
}

+ (NSInteger)maxLiveUserImages {
    return static_cast<NSInteger>(Agent::AgentChatLoop::kMaxLiveUserImages);
}

- (NSUInteger)compactedEntryCount {
    return static_cast<NSUInteger>(_loop->CompactedEntryCount());
}

- (void)setSkillIndex:(NSString *)indexText {
    _loop->SetSkillIndex(ToStd(indexText));
}

- (void)setSystemPromptOverride:(NSString *)prompt {
    _loop->SetSystemPromptOverride(ToStd(prompt));
}

- (void)addUserMessage:(NSString *)text {
    _loop->AddUserMessage(ToStd(text));
}

- (void)addUserMessage:(NSString *)text
            attachments:(NSArray<RISEAgentChatAttachment *> *)attachments {
    std::vector<Agent::ChatAttachment> cppAttachments;
    cppAttachments.reserve(attachments.count);
    for (RISEAgentChatAttachment *a in attachments) {
        Agent::ChatAttachment cppA;
        cppA.mimeType   = ToStd(a.mimeType);
        cppA.base64Data = ToStd(a.base64Data);
        cppAttachments.push_back(cppA);
    }
    _loop->AddUserMessage(ToStd(text), cppAttachments);
}

- (RISEAgentChatRequest *)buildRequestWithApiKey:(NSString *)apiKey {
    const Agent::ChatHttpRequest req = _loop->BuildRequest(ToStd(apiKey));
    return [[RISEAgentChatRequest alloc] initWithRequest:req];
}

- (RISEAgentChatStep *)handleResponseWithStatus:(NSInteger)httpStatus
                                           body:(NSString *)rawBody {
    const Agent::ChatStepResult step =
        _loop->HandleResponse(static_cast<long>(httpStatus), ToStd(rawBody));
    return [[RISEAgentChatStep alloc] initWithStep:step];
}

- (NSString *)toolCallToJsonRpcLine:(RISEAgentChatToolCall *)call
                              rpcId:(NSInteger)rpcId {
    return ToNS(_loop->ToolCallToJsonRpcLine([call cppCall],
                                             static_cast<int>(rpcId)));
}

- (NSString *)gateRefusalResponseFor:(RISEAgentChatToolCall *)call
                                rpcId:(NSInteger)rpcId {
    return ToNS(_loop->GateRefusalResponse([call cppCall],
                                           static_cast<int>(rpcId)));
}

- (void)addToolResult:(RISEAgentChatToolCall *)call
  jsonRpcResponseLine:(NSString *)line {
    _loop->AddToolResult([call cppCall], ToStd(line));
}

- (NSString *)toolOutcomeLineFor:(RISEAgentChatToolCall *)call
                       resultLine:(NSString *)line {
    // Pure/stateless (see AgentChatLoop::ToolOutcomeLineForDisplay's
    // doc) -- does not require _loop's transcript/pending state at
    // all, but is a bridge instance method for symmetry with the
    // rest of this class's tool-call surface.
    return ToNS(Agent::AgentChatLoop::ToolOutcomeLineForDisplay(
        [call cppCall], ToStd(line)));
}

- (void)startTrajectoryInDirectory:(NSString *)directory
                         scenePath:(NSString *)scenePath
                       headVersion:(long long)headVersion
                           enabled:(BOOL)enabled {
    if (!enabled) {
        // Detach: an empty sink makes every recording hook a no-op.
        _loop->SetTrajectorySink(std::function<void(const std::string&)>());
        return;
    }
    const std::string dir = ToStd(directory);
    if (dir.empty()) return;

    // Rotate BEFORE creating the new file (keep ~50 newest / ~200 MB).
    Agent::PruneTrajectoryDir(dir, 50, 200LL * 1024 * 1024);

    // Per-session file: <UTC-timestamp>-<trace8>.jsonl.
    const std::string traceId = Agent::MakeTrajectoryTraceId();
    char ts[32];
    std::time_t nowT = std::time(nullptr);
    std::tm tmv;
    gmtime_r(&nowT, &tmv);
    std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tmv);
    const std::string trace8 = traceId.substr(0, 8);
    const std::string path = dir + "/" + std::string(ts) + "-" + trace8 + ".jsonl";

    Agent::ChatTrajectoryConfig cfg;
    cfg.traceId = traceId;
    cfg.scenePath = ToStd(scenePath);
    cfg.sceneHeadVersion = headVersion;
    _loop->SetTrajectorySink(Agent::MakeTrajectoryFileSink(path), cfg);
}

- (void)recordHttpRoundWithStatus:(NSInteger)httpStatus
                             body:(NSString *)rawBody
                        elapsedMs:(int64_t)elapsedMs {
    _loop->RecordHttpRound(static_cast<long>(httpStatus), ToStd(rawBody), elapsedMs);
}

- (void)recordHttpRoundWithStatus:(NSInteger)httpStatus
                             body:(NSString *)rawBody
                        elapsedMs:(int64_t)elapsedMs
                          attempt:(NSInteger)attempt
                          retryOf:(NSInteger)retryOf {
    _loop->RecordHttpRound(static_cast<long>(httpStatus), ToStd(rawBody), elapsedMs,
                           static_cast<int>(attempt), static_cast<int>(retryOf));
}

- (void)finishTrajectoryWithStatus:(NSString *)status {
    _loop->FinishTrajectory(ToStd(status));
}

- (void)recordAuxiliaryHttpRoundWithPurpose:(NSString *)purpose
                                         url:(NSString *)url
                                 requestBody:(NSString *)requestBody
                                  httpStatus:(NSInteger)httpStatus
                                responseBody:(NSString *)responseBody
                                   elapsedMs:(int64_t)elapsedMs {
    _loop->RecordAuxiliaryHttpRound(ToStd(purpose), ToStd(url), ToStd(requestBody),
                                     static_cast<long>(httpStatus), ToStd(responseBody),
                                     elapsedMs);
}

- (NSUInteger)pendingToolCallsCount {
    return static_cast<NSUInteger>(_loop->PendingToolCalls().size());
}

- (NSUInteger)transcriptSize {
    return static_cast<NSUInteger>(_loop->TranscriptSize());
}

- (RISEAgentChatRole)transcriptRoleAtIndex:(NSUInteger)index {
    // TranscriptAt is bounds-safe (returns a static empty entry whose
    // role is User), so no extra range check is needed here.
    //
    // NO `default` CASE, deliberately.  This switch used to fold every
    // unrecognized role into RISEAgentChatRoleUser, so Role::DriverNote —
    // a message the LOOP wrote — would have been reported to the driver as
    // something the user typed, silently.  With every enumerator named and
    // no default, the next one added to Role is a -Wswitch warning, which
    // this project treats as a build failure.  The trailing return keeps
    // -Wreturn-type quiet; it is unreachable.
    switch (_loop->TranscriptAt(static_cast<std::size_t>(index)).role) {
        case Agent::ChatTranscriptEntry::Role::Assistant:
            return RISEAgentChatRoleAssistant;
        case Agent::ChatTranscriptEntry::Role::ToolResults:
            return RISEAgentChatRoleToolResults;
        case Agent::ChatTranscriptEntry::Role::DriverNote:
            return RISEAgentChatRoleDriverNote;
        case Agent::ChatTranscriptEntry::Role::User:
            return RISEAgentChatRoleUser;
    }
    return RISEAgentChatRoleUser;
}

- (NSUInteger)driverNoteCount {
    return static_cast<NSUInteger>(_loop->DriverNoteCount());
}

- (NSString *)lastDriverNoteText {
    return ToNS(_loop->LastDriverNoteText());
}

- (NSString *)transcriptDisplayTextAtIndex:(NSUInteger)index {
    return ToNS(_loop->TranscriptAt(static_cast<std::size_t>(index)).displayText);
}

@end
