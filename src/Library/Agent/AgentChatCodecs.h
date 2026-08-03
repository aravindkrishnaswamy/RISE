//////////////////////////////////////////////////////////////////////
//
//  AgentChatCodecs.h - provider wire-format codecs for the sans-IO LLM
//    chat loop (Facet 5, the agentic surface -- slice B1).
//
//    A codec translates between ONE LLM provider's HTTP wire format and
//    the provider-neutral chat-loop types.  Codecs are PURE and sans-IO:
//    they build request descriptions (url + headers + body strings) and
//    parse raw response bodies -- the caller (the GUI, or a test)
//    performs the actual HTTP round-trip and feeds the body back.
//
//    Three implementations:
//      * AnthropicChatCodec -- the Anthropic Messages API
//        (POST https://api.anthropic.com/v1/messages, auth via the
//        `x-api-key` header + `anthropic-version: 2023-06-01`).
//      * GeminiChatCodec -- the Google Gemini v1beta REST API
//        (POST .../v1beta/models/{model}:generateContent, auth via the
//        `x-goog-api-key` header).  Wire shapes verified 2026-07-02
//        against the ai.google.dev/api Content-type reference (the
//        api/caching page): functionDeclarations, functionCall
//        {id,name,args} ("If populated, the client to execute the
//        functionCall and return the response with the matching id"),
//        functionResponse {id,name,response,parts[]} where parts is an
//        array of FunctionResponsePart {inlineData:{mimeType,data}}
//        ("Ordered Parts that constitute a function response").
//      * OpenAIChatCodec -- OpenAI Responses API by default; parameterized
//        OpenAI-compatible providers continue to use Chat Completions.
//        Both use `Authorization: Bearer ...`; OpenAI is surfaced in the
//        GUI as ChatGPT.
//
//    KEY DESIGN RULES (see AgentChatLoop.h for the loop contract):
//      * The API key appears ONLY in the auth header the codec emits --
//        never in the URL, never in the body, never in any other string
//        the codec produces.  Codecs log NOTHING (bodies may embed
//        scene content; the key must never reach a log either).  Header
//        values built from caller input (the key) are stripped of every
//        control character (< 0x20, i.e. CR/LF and friends) so a pasted
//        key cannot smuggle extra headers into the request.
//      * Assistant turns are stored as RAW provider-native JSON and
//        echoed VERBATIM (byte-preserved) on subsequent BuildRequests.
//        ParseResponse extracts the assistant content as a raw byte
//        span of the response body -- NOT a parse + re-serialize --
//        so provider-opaque fields (Anthropic thinking-block
//        `signature`s, Gemini thought signatures) round-trip intact.
//      * RECORD-OR-REFUSE: ParseResponse either returns a disposition
//        whose recorded entry is safe to echo forever, or refuses the
//        WHOLE response (ProviderError; the loop records nothing).
//        Any response whose tool calls could not become the loop's
//        pending set is refused outright: tool_use / functionCall
//        blocks under a non-tool-call stop_reason/finishReason
//        (Gemini calls additionally REQUIRE an explicit finishReason
//        STOP -- an absent finishReason is tolerated only on
//        text-only turns, where the proto makes it optional), an
//        id-less Anthropic tool_use (Gemini instead SYNTHESIZES ids
//        for id-less calls), DUPLICATE call ids on either provider
//        (ids are the result-matching key and must be unique), a
//        malformed (non-object) Gemini functionCall value, an
//        Anthropic tool_use whose "input" key is PRESENT but not a
//        JSON object (including an explicit "input":null) or a
//        Gemini functionCall whose "args" key is PRESENT but not a
//        JSON object (including an explicit "args":null) -- an
//        ABSENT input/args key is each provider's legal no-args
//        shape and still maps to "{}"; only a present-but-non-object
//        value refuses -- an OpenAI tool_call whose function.arguments
//        string does not parse as a JSON object (all three: executing
//        it would fabricate empty args), a Gemini candidate whose
//        content.role is present and not "model" (a spoofed role
//        would join BuildRequest's user merge on replay), and
//        degenerate blank-content final turns on ALL THREE codecs
//        (an OpenAI "stop", Anthropic "end_turn", or Gemini "STOP"
//        turn whose EXTRACTED text is blank -- absent/null/
//        empty-string/whitespace-only content, OR a content array
//        that is empty or carries no "text" parts, the shape a
//        non-conformant proxy can send; OpenAI additionally surfaces
//        the structured message.refusal text when present).  Never
//        "salvage" part of such a body: the raw echo would replay
//        the un-answerable call blocks on every later request.
//      * All tool results for one assistant turn are packed into ONE
//        following user message (an Anthropic hard requirement for
//        parallel tool_use; mirrored for Gemini).
//      * Image-bearing tool results (see ChatToolResultCarriesImage)
//        carry a REAL image block: the base64 PNG is
//        extracted into an image part (Anthropic `tool_result` content
//        image block; Gemini `functionResponse.parts[].inlineData` --
//        the documented FunctionResponsePart mechanism for multimodal
//        function output) and STRIPPED from the textual part so it is
//        not double-sent.  RewriteElidedImages later replaces such an
//        image block/part with a short text note (the loop keeps only
//        the MOST RECENT image live -- see AgentChatLoop.h).
//      * USER ATTACHMENTS (Model-B F5 chat image attachments): a user
//        message may carry reference images (photos the user wants the
//        agent to model against), passed as ChatAttachment{mimeType,
//        base64Data} to AddUserMessage / MakeUserEntry.  These are a
//        SEPARATE mechanism from the read_image tool-result elision
//        above -- independent policy, independent bookkeeping (see
//        AgentChatLoop.h "USER IMAGE RETENTION").  MakeUserEntry places
//        each attachment's image block/part BEFORE the text block/part
//        (Anthropic content array / Gemini parts array order) -- both
//        providers read multimodal content left-to-right, so the image
//        precedes the caption that refers to it, matching how a human
//        would present "here's a photo; do X with it".  An unsupported
//        mimeType (anything outside the provider's allow-list -- see
//        RewriteElidedUserImages) is the DRIVER's job to reject before
//        it ever reaches this codec (Swift layer); the codec still
//        degrades honestly (passes the mimeType through) rather than
//        silently dropping it, since core-layer gating is optional by
//        design (documented in the skill / CLAUDE.md deviation notes).
//
//    The tool definitions are defined ONCE, provider-neutrally, in
//    AgentChatCodecs.cpp's kToolDefs (which IS the count -- do not restate
//    one here); each codec maps them into its native tool declaration
//    shape.  Every one of them but `ask_user` maps onto an AgentRpc verb
//    of the same name, with parameter names/shapes mirroring AgentRpc.cpp
//    exactly; `ask_user` is the exception -- it maps to NO verb, because
//    the driver answers it locally from the user rather than dispatching
//    it.  The dispatcher also speaks verbs no tool exposes, so the
//    correspondence is one-way, not a bijection.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTCHATCODECS_
#define RISE_AGENT_AGENTCHATCODECS_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//! One user-supplied reference image attached to a chat message
		//! (Model-B F5 chat image attachments).  `mimeType` is a full
		//! MIME string ("image/png", "image/jpeg", "image/gif",
		//! "image/webp" -- Anthropic's exact allow-list; Gemini accepts
		//! the same four via inlineData.mimeType); `base64Data` is the
		//! already-encoded image payload, no data-URL prefix.  NEVER LOG
		//! either field -- base64Data can be hundreds of KB and together
		//! they are exactly the bytes the no-image-bytes-in-logs security
		//! rule forbids.
		struct ChatAttachment
		{
			std::string mimeType;
			std::string base64Data;
		};

		//! A fully-described HTTP request the CALLER performs.  The codec
		//! never talks to the network; it only fills this in.
		struct ChatHttpRequest
		{
			std::string url;      //!< absolute https URL (POST)
			std::vector<std::pair<std::string, std::string>> headers;   //!< header name/value pairs (includes content-type + the auth header)
			std::string body;     //!< the JSON request body

			//! The per-request wall-clock budget, in seconds, that BOTH
			//! platform transports (TlsTransportMac.mm's setTimeoutInterval
			//! + session timeoutIntervalForRequest; TlsTransportWin.cpp's
			//! WinHttpSetTimeouts send/receive phases) honor for THIS
			//! request.  Default 300s matches the GUI drivers' hosted-
			//! provider budget.  A codec that talks to a local inference
			//! server (cold model swap loading 17-43GB + long 70B
			//! generations legitimately exceeds 300s) raises this per
			//! request via its own Config rather than the transports
			//! guessing -- see OpenAIChatCodec::Config::requestTimeoutSeconds
			//! and its LOCAL-provider instantiation in AgentChatLoop.cpp's
			//! MakeCodec.
			long timeoutSeconds = 300;
		};

			//! Token-usage counts parsed from ONE provider response body
			//! (Eval-harness E1 -- fed into the trajectory `llm` record's
			//! OTel `gen_ai.usage.*` fields).  Each field is -1 when the
			//! provider omitted it (absent-tolerant, null-safe): Anthropic
			//! `usage.{input_tokens,output_tokens,cache_read_input_tokens}`
			//! + `usage.output_tokens_details.thinking_tokens`,
			//! Gemini `usageMetadata.{promptTokenCount,candidatesTokenCount,
			//! cachedContentTokenCount,thoughtsTokenCount}`, OpenAI
			//! `usage.{prompt_tokens,completion_tokens,
			//! prompt_tokens_details.cached_tokens,
			//! completion_tokens_details.reasoning_tokens}` (Chat Completions)
			//! or `usage.{input_tokens,output_tokens,
			//! input_tokens_details.cached_tokens,
			//! output_tokens_details.reasoning_tokens}` (Responses).
			//!
			//! NORMALIZED SEMANTICS (the codecs convert INTO these; providers
			//! disagree on the wire -- see each ParseUsage for the per-provider
			//! evidence):
			//!   * `outputTokens` is the TOTAL BILLED GENERATION for the turn
			//!     -- visible output PLUS any hidden reasoning/thinking the
			//!     provider charged at the output rate.
			//!   * `reasoningOutputTokens` is the reasoning/thinking SUBSET of
			//!     that total.
			//!   * INVARIANT, whenever `outputTokens >= 0`:
			//!         0 <= reasoningOutputTokens <= outputTokens
			//!     so `outputTokens - reasoningOutputTokens` is the visible
			//!     output and a downstream cost model can price the total with
			//!     ONE output rate without double counting.  This is ENFORCED,
			//!     not merely documented: every ParseUsage ends in the shared
			//!     EnforceUsageInvariant (AgentChatCodecs.cpp), which clamps a
			//!     self-contradictory body (`reasoning > output` -- reachable
			//!     from any provider, since the `local` provider's base URL is
			//!     arbitrary) down to the billed total and raises
			//!     `reasoningClamped`.  No input, however hostile, can produce
			//!     a ChatUsage that breaks the invariant.
			//!   * Every count is either -1 (absent) or >= 0.  A provider-sent
			//!     NEGATIVE is garbage, not a count: it normalizes to 0, never
			//!     to the -1 sentinel and never into arithmetic.
			//!   * A body that reports reasoning but NO output counter yields
			//!     `outputTokens == reasoningOutputTokens` (the subset is a
			//!     lower bound on the billed generation), identically on every
			//!     provider -- so `reasoningOutputTokens >= 0` implies
			//!     `outputTokens >= 0`, and those tokens are never dropped.
			//!     This holds for a REPORTED ZERO too (>= 0 is the whole
			//!     non-sentinel range, not just the positives): a body whose
			//!     only generation evidence is `reasoning_tokens: 0` publishes
			//!     `outputTokens == 0`, never -1 beside a known subset.
			//!   * The CLAMP DOES NOT DESTROY EVIDENCE.
			//!     `reasoningOutputTokensReported` carries the provider's own
			//!     count, captured before the clamp; it equals
			//!     `reasoningOutputTokens` on every healthy body and is the
			//!     only surviving copy of the claim on a clamped one.
			//! WHOSE reasoning counter is a separate summand is a PER-PROVIDER
			//! fact, decided from that provider's recorded evidence and NOT
			//! re-derived per response from body arithmetic (see the "PROVIDER
			//! DISPOSITION" table in OpenAIChatCodec::ParseUsage for why).
			//! Gemini (`thoughtsTokenCount`) and xAI
			//! (`completion_tokens_details.reasoning_tokens`) report it
			//! SEPARATELY, so it is FOLDED IN here; Anthropic and OpenAI (both
			//! wire shapes) already include it in their output counter, so they
			//! pass through untouched.
			struct ChatUsage
			{
				long long inputTokens          = -1;   //!< prompt / input tokens (-1 = absent)
				long long outputTokens         = -1;   //!< TOTAL billed generation: visible + reasoning (-1 = absent)
				long long cacheReadInputTokens = -1;   //!< cache-read input tokens (-1 = absent)
				long long reasoningOutputTokens = -1;  //!< the reasoning/thinking SUBSET of outputTokens (-1 = provider reported none)
				//! The provider's OWN reasoning count, captured before the
				//! invariant could clamp it (-1 = provider reported none).
				//! Identical to `reasoningOutputTokens` unless
				//! `reasoningClamped` is set, in which case this is the ONLY
				//! surviving copy of what the body claimed -- the clamped
				//! record alone cannot tell a 51-token turn that reported 51
				//! from one that reported 362.  The clamped value is what
				//! costs and rollups use; this one is evidence.
				long long reasoningOutputTokensReported = -1;
				//! DIAGNOSTIC: the body claimed more reasoning than it billed
				//! as output, so `reasoningOutputTokens` was clamped to
				//! `outputTokens` to keep the invariant.  The codec layer logs
				//! NOTHING (bodies carry scene content and api keys), so the
				//! anomaly rides out in-band -- the trajectory `llm` record
				//! serializes it as `gen_ai.usage.reasoning_clamped` when set.
				bool      reasoningClamped = false;
			};

		//! One tool call the model requested.  `id` is the provider's
		//! tool-call id (Anthropic `tool_use.id`; Gemini
		//! `functionCall.id`, which Gemini 3.x populates and the docs
		//! require echoed back as the matching `functionResponse.id`).
		//! Only when a Gemini functionCall carries no id does the codec
		//! synthesize "call_0", "call_1", ... per assistant turn,
		//! skipping any candidate that collides with an id already
		//! captured this turn (idSynthesized is then true, NO id field
		//! is emitted in the functionResponse, and results match by
		//! name + order).  Duplicate ids within one turn (a repeated
		//! provider id -- on EITHER provider -- or a Gemini provider id
		//! colliding with an earlier synthesized one) REFUSE the whole
		//! turn -- ids are the result-matching key and must be unique.
		struct ChatToolCall
		{
			std::string id;                  //!< provider (or synthesized) call id
			std::string name;                //!< the tool name (a JSON-RPC verb)
			std::string argsJson;            //!< the call arguments as a JSON object string
			bool        idSynthesized = false; //!< true iff `id` was fabricated by the codec (never echoed to the provider)
		};

		//! A machine-readable classification for ProviderError outcomes so
		//! a GUI driver can branch on the KIND (retry on Http, stop on
		//! Refusal, ...) without string-matching errorMessage.
		enum class ChatErrorKind
		{
			None,          //!< not an error (ToolCalls / FinalText)
			Http,          //!< non-200 HTTP status from the provider
			Parse,         //!< the response body did not parse as JSON
			Provider,      //!< the provider returned a hostile/degenerate/unexpected turn
			Refusal,       //!< the provider declined the request (safety/refusal)
			MaxTokens,     //!< the reply hit the output-token cap (truncated; discarded).  A verbatim retry will likely truncate again -- recovery needs a NEW/narrower user message, not an automatic retry.
			IterationCap,  //!< the loop's per-turn tool-round cap tripped
			Misuse         //!< caller-contract violation (e.g. HandleResponse while calls are pending)
		};

		//! The outcome of one chat step (one HTTP round-trip).
		struct ChatStepResult
		{
			enum class Kind
			{
				ToolCalls,      //!< the model wants tool calls executed
				FinalText,      //!< the model finished its turn with text
				ProviderError   //!< HTTP / parse / policy / cap failure
			};

			Kind                      kind = Kind::ProviderError;
			std::vector<ChatToolCall> toolCalls;     //!< non-empty iff kind==ToolCalls
			std::string               finalText;     //!< filled iff kind==FinalText; possibly empty (e.g. a thinking-only end_turn), like assistantDisplayText
			std::string               errorMessage;  //!< filled iff kind==ProviderError
			ChatErrorKind             errorKind = ChatErrorKind::None;  //!< set on EVERY ProviderError; None otherwise

			//! The assistant's interim display text for THIS turn --
			//! filled for BOTH ToolCalls (the text alongside the calls,
			//! possibly empty) and FinalText (== finalText), so a GUI can
			//! show the model's narration while tools run.  Empty on
			//! ProviderError.
			std::string               assistantDisplayText;

			//! DISPLAY-LAYER ENRICHMENT (regression fix): the model's
			//! reasoning/thinking text for THIS turn, extracted from
			//! whichever PROVIDER-SPECIFIC field carries it -- Anthropic
			//! `thinking` content blocks (concatenated), OpenAI-family
			//! `message.reasoning` (Ollama) or `message.reasoning_content`
			//! (xAI), never Gemini (no reasoning field is exposed).  Filled
			//! for BOTH ToolCalls and FinalText, mirroring
			//! assistantDisplayText; "" when the provider exposes none for
			//! this turn (including every gpt-family response, which
			//! carries neither field).  DISPLAY-ONLY, like
			//! assistantDisplayText: this text is NEVER re-serialized onto
			//! the wire -- the raw echo (assistantEntryJson /
			//! ChatTranscriptEntry::rawJson) carries the ORIGINAL
			//! provider-native reasoning representation (e.g. an Anthropic
			//! thinking block with its signature) byte-identically, so
			//! replay/signature-verification is completely unaffected by
			//! this field's existence.  Empty on ProviderError.
			std::string               reasoningText;

			//! TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY (set ONLY on a
			//! ProviderError whose errorKind is Http): true when the loop
			//! detected a text-only model rejecting image content
			//! (HTTP 400 "...multimodal... not support...") and has ALREADY
			//! elided every image from the transcript in response.  The
			//! DRIVER should re-issue the SAME round once (rebuild via
			//! BuildRequest -- now image-free -- fetch, and RecordHttpRound
			//! with attempt=2/retryOf=1 so the retry is an honest sibling
			//! llm record) instead of terminating.  Enforced once-per-round
			//! by the loop's sticky elide-all state: a second multimodal 400
			//! in the same session leaves this false (the normal
			//! provider_error path proceeds unchanged).  Always false for
			//! ToolCalls / FinalText and for every other error kind.
			bool                      retryWithoutImages = false;

			//! REASONING-MODEL TOOLS-VS-EFFORT 400 RECOVERY (set ONLY on a
			//! ProviderError whose errorKind is Http): true when the loop
			//! detected an OpenAI-family reasoning model rejecting a
			//! function-tools request over /v1/chat/completions (HTTP 400
			//! "...reasoning_effort... not support...") and has ALREADY set
			//! its sticky state so every later BuildRequest (including the
			//! retry) explicitly sends `"reasoning_effort":"none"`.  This is
			//! NOT an omission -- this codebase never sends a
			//! reasoning_effort field on Chat-Completions requests unless
			//! this recovery fires, so the 400 comes from the model's
			//! server-side default; the codec must actively ADD the override.
			//! OpenAI's native Responses mode does not need this recovery.
			//! The DRIVER should re-issue the SAME round once
			//! (rebuild via BuildRequest, fetch, and RecordHttpRound with
			//! attempt=2/retryOf=1 so the retry is an honest sibling llm
			//! record) instead of terminating.  Enforced once-per-round by
			//! the loop's sticky state: a second such 400 in the same
			//! session leaves this false (the normal provider_error path
			//! proceeds unchanged).  Always false for ToolCalls / FinalText
			//! and for every other error kind.
			bool                      retryReasoningEffortNone = false;

			//! DEGENERATE-BLANK-TURN RETRY (set ONLY on a ProviderError whose
			//! errorKind is Provider, at exactly the four ChatContentIsBlank-
			//! gated refusals -- Anthropic end_turn, Gemini STOP/empty
			//! finishReason, OpenAI Responses, OpenAI Chat Completions): true
			//! when the provider returned HTTP 200 with an empty/whitespace
			//! final answer and no tool calls.  Observed cause: local
			//! qwen3-thinking models served by Ollama stochastically emit a
			//! premature end-of-turn INSIDE their reasoning block (finish
			//! reason "stop", content "", reasoning cut off mid-word) --
			//! a serving artifact, not a considered refusal, so unlike
			//! MaxTokens (see its doc above) a VERBATIM retry of the identical
			//! request commonly succeeds.  Distinct from retryWithoutImages /
			//! retryReasoningEffortNone: this recovery does not modify the
			//! next request at all, it just resends the same one.  NOT set on
			//! the sibling ChatErrorKind::Refusal branches at the same sites
			//! (a structured refusal / content_filter is a considered
			//! decision, not a serving glitch, and must stay terminal), and
			//! not set on any other structural refusal (malformed tool args,
			//! id-less tool_call, ...).  Only the eval runner's attempt loop
			//! consumes this flag; AgentChatLoop's interactive driver does
			//! not retry on it.  Always false for ToolCalls / FinalText and
			//! for every other error kind.
			bool                      retryDegenerateTurn = false;
		};

		//! ParseResponse's full product: the step outcome PLUS the raw
		//! provider-native transcript entry for the assistant turn (so the
		//! loop can echo it verbatim later) and a display-text extraction
		//! for the GUI.  `assistantEntryJson` is empty on ProviderError.
		struct ChatParsedResponse
		{
			ChatStepResult step;
			std::string    assistantEntryJson;   //!< raw provider-native message entry (verbatim content span)
			std::string    assistantDisplayText; //!< concatenated text blocks/parts (for display)

			//! Mirrors ChatStepResult::reasoningText (see its doc) -- kept
			//! alongside assistantDisplayText the same way
			//! step.assistantDisplayText is kept alongside this struct's own
			//! assistantDisplayText.  DISPLAY-ONLY; never read by
			//! assistantEntryJson's construction.
			std::string    reasoningText;
		};

		//! The pure provider-codec interface.  Stateless: every method is
		//! const and the transcript lives in AgentChatLoop as a list of
		//! raw provider-native entry strings this codec produced.
		class IChatProviderCodec
		{
		public:
			virtual ~IChatProviderCodec() {}

			//! A short human name ("anthropic" / "gemini" / "openai"), used to prefix
			//! HTTP-error messages (see ParseResponse's non-200 path).
			virtual const char* ProviderName() const = 0;

			//! The default model id used when the loop is given none.
			virtual const char* DefaultModelId() const = 0;

			//! A provider-native transcript entry for a user text message,
			//! optionally carrying reference-image attachments (Model-B F5
			//! chat image attachments).  Each attachment becomes a REAL
			//! image block/part (Anthropic content-array image element;
			//! Gemini inlineData part), placed BEFORE the text block/part
			//! -- image(s) first, then the caption -- with an EMPTY
			//! `attachments` producing byte-identical output to the
			//! text-only overload below (so every existing call site is
			//! unaffected).  `attachments` defaults to empty so text-only
			//! callers need no change; the default is safe here because
			//! every call site goes through the SAME static type
			//! (IChatProviderCodec*), so the default resolves once,
			//! consistently, never depending on the concrete codec.
			virtual std::string MakeUserEntry(
				const std::string& text,
				const std::vector<ChatAttachment>& attachments = std::vector<ChatAttachment>() ) const = 0;

			//! Pack ALL tool results of one assistant turn into ONE
			//! provider-native user-turn entry.  Each element pairs the
			//! ChatToolCall with the raw JSON-RPC response ENVELOPE line
			//! from AgentRpcDispatcher::HandleLine.  Image-bearing results
			//! get a real image block/part (base64 stripped from the
			//! textual half) -- but when SEVERAL results in one pack
			//! carry images, only the LAST keeps a live image; earlier
			//! ones are packed pre-elided so at most one image is live
			//! per entry (the loop's RewriteElidedImages pass handles
			//! OLDER entries).  JSON-RPC error envelopes become error
			//! tool results (Anthropic `is_error: true`).
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const = 0;

			//! Rewrite a PackToolResults-produced entry, replacing every
			//! live image block/part with a short "[image elided ...]"
			//! text note (Anthropic: the {type:"image"} content element;
			//! Gemini: the functionResponse.parts inlineData transport).
			//! The loop-written ATTACH note ("the PNG is attached as
			//! ...") in the textual summary is rewritten to the elision
			//! text as well, so the model is never shown a note
			//! contradicting the elided block.  An RPC-owned "note" field
			//! is never clobbered, by TWO independent rules, both needed:
			//! the KEY preference (when StripPngBase64 found the result
			//! already owned a "note", it wrote the attach note under
			//! "image_note", which this rewrite prefers) covers a note on
			//! the SAME result -- and the VALUE gate (only a note reading
			//! "... is attached as ..." is rewritten at all) covers a note
			//! on a DIFFERENT result of the same entry, which the key rule
			//! alone does NOT: the OpenAI codec applies this rewrite to
			//! EVERY tool message of an image-bearing entry, so a
			//! read_skill advisory co-packed with a read_image used to be
			//! replaced by the image note.  See RewriteElidedSummaryText.
			//! Used by the loop to keep only the MOST RECENT image live
			//! (see AgentChatLoop.h "IMAGE RETENTION").  This regeneration
			//! is legal ONLY because ToolResults entries are loop-generated
			//! -- assistant entries carry the byte-preservation contract
			//! and must never pass through here.  Returns the entry
			//! unchanged when it carries no image (or does not parse).
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const = 0;

			//! USER IMAGE RETENTION (see AgentChatLoop.h): elide the OLDEST
			//! `countToElide` live image blocks/parts from a MakeUserEntry-
			//! produced entry (in the order attachments were given -- index
			//! 0 first), replacing each with the placeholder text
			//! "[reference image elided -- re-attach if needed]".  Distinct
			//! from RewriteElidedImages (which targets whole ToolResults
			//! entries and always elides ALL of an entry's images): a
			//! single user message can carry several attachments, of which
			//! only SOME may need eliding when the running cap is crossed
			//! by a small amount, so this takes a count rather than an
			//! all-or-nothing switch.  `countToElide` >= the entry's live
			//! image count elides all of them.  Legal ONLY on entries this
			//! codec's MakeUserEntry produced (loop-generated, like
			//! PackToolResults' output) -- never on assistant entries.
			//! Returns the entry unchanged when countToElide <= 0 or the
			//! entry carries no image.
			virtual std::string RewriteElidedUserImages(
				const std::string& userEntryJson, int countToElide ) const = 0;

			//! SUPERSEDED-READ RETENTION (see AgentChatLoop.h): replace the
			//! PAYLOAD of the tool results at the given positions of a
			//! PackToolResults-produced entry with `placeholderText`, leaving
			//! every other result -- and the per-result envelope that binds it
			//! to its tool call -- untouched.
			//!
			//! POSITIONAL, not id-keyed, and deliberately so: the `results`
			//! vector PackToolResults consumed maps 1:1 and IN ORDER onto this
			//! codec's own container (Anthropic's `content` tool_result array,
			//! Gemini's `parts` functionResponse array, OpenAI's message
			//! array -- whose only extra element, the trailing image user
			//! message, is APPENDED after all N results), so index i here is
			//! result i everywhere.  Id-keying would not work uniformly:
			//! Gemini deliberately WITHHOLDS a synthesized id from the wire
			//! (see PackToolResults), so an id-keyed rewrite could not
			//! address those results at all.
			//!
			//! The call binding is preserved verbatim (Anthropic's
			//! tool_use_id, Gemini's functionResponse id/name, OpenAI's
			//! tool_call_id / call_id), so a rewritten entry still answers
			//! every tool call of its assistant turn -- the wire invariant in
			//! AgentChatLoop.h's header.  Indices out of range are ignored;
			//! an entry that does not parse is returned unchanged.
			//!
			//! Legal ONLY on entries this codec's PackToolResults produced
			//! (loop-generated) -- never on assistant entries, which carry
			//! the byte-preservation contract.
			virtual std::string RewriteElidedToolResults(
				const std::string& packedEntryJson,
				const std::vector<std::size_t>& resultIndices,
				const std::string& placeholderText ) const = 0;

			//! Build the full HTTP request for the current conversation.
			//! `apiKey` is used ONLY for the auth header -- it is not
			//! retained, not placed in the body/url, and never logged.
			//! `rawEntries` are the transcript entries in order (each a
			//! provider-native message JSON produced by this codec).
			//! `forceReasoningEffortNone` (REASONING-MODEL TOOLS-VS-EFFORT
			//! 400 RECOVERY, see ChatStepResult::retryReasoningEffortNone):
			//! when true, EXPLICITLY add `"reasoning_effort":"none"` to a
			//! Chat-Completions request body.  OpenAIChatCodec ignores it in
			//! Responses mode; Anthropic and Gemini ignore it always.
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries,
				bool forceReasoningEffortNone = false ) const = 0;

			//! Parse one raw HTTP response.  `httpStatus` is the status
			//! code the caller observed (non-200 -> ProviderError carrying
			//! the provider's error.message when parseable).  NEVER throws.
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const = 0;

			//! Parse the token-usage block from ONE raw response body into a
			//! ChatUsage (Eval-harness E1).  Absent-tolerant and null-safe:
			//! every field defaults to -1 and stays -1 when the provider
			//! omits it or the body does not parse.  NEVER throws.  Pure
			//! (like every codec method) -- the raw body is the sole input.
			virtual ChatUsage ParseUsage( const std::string& rawBody ) const = 0;

			//! Byte length of this codec's fixed tool-declarations payload
			//! sent on every request -- the fixed-prefix contribution to
			//! the context-budget estimate (Facet 5 context-compaction
			//! slice S1; see AgentChatLoop::EstimateContextTokens).
			virtual std::size_t ToolsWireBytes() const = 0;
		};

		//! Anthropic Messages API codec (see file header).
		class AnthropicChatCodec : public IChatProviderCodec
		{
		public:
			virtual const char* ProviderName() const;
			virtual const char* DefaultModelId() const;
			virtual std::string MakeUserEntry(
				const std::string& text,
				const std::vector<ChatAttachment>& attachments = std::vector<ChatAttachment>() ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const;
			virtual std::string RewriteElidedUserImages(
				const std::string& userEntryJson, int countToElide ) const;
			virtual std::string RewriteElidedToolResults(
				const std::string& packedEntryJson,
				const std::vector<std::size_t>& resultIndices,
				const std::string& placeholderText ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries,
				bool forceReasoningEffortNone = false ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
			virtual ChatUsage ParseUsage( const std::string& rawBody ) const;
			virtual std::size_t ToolsWireBytes() const;
		};

		//! Google Gemini v1beta REST codec (see file header).  The
		//! functionCall.id / functionResponse.{id,parts[].inlineData}
		//! shapes were web-verified 2026-07-02 against the
		//! ai.google.dev/api Content-type reference (the api/caching
		//! page); the surrounding generateContent envelope against
		//! ai.google.dev/api/generate-content.
		class GeminiChatCodec : public IChatProviderCodec
		{
		public:
			virtual const char* ProviderName() const;
			virtual const char* DefaultModelId() const;
			virtual std::string MakeUserEntry(
				const std::string& text,
				const std::vector<ChatAttachment>& attachments = std::vector<ChatAttachment>() ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const;
			virtual std::string RewriteElidedUserImages(
				const std::string& userEntryJson, int countToElide ) const;
			virtual std::string RewriteElidedToolResults(
				const std::string& packedEntryJson,
				const std::vector<std::size_t>& resultIndices,
				const std::string& placeholderText ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries,
				bool forceReasoningEffortNone = false ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
			virtual ChatUsage ParseUsage( const std::string& rawBody ) const;
			virtual std::size_t ToolsWireBytes() const;
		};

		//! OpenAI uses the Responses API so reasoning and function tools work
		//! together.  The parameterized form remains the shared Chat
		//! Completions codec for compatible providers (xAI and local/Ollama).
		//! Do NOT opt another provider into Responses merely because its Chat
		//! Completions schema resembles OpenAI's.
		class OpenAIChatCodec : public IChatProviderCodec
		{
		public:
			//! Wire config distinguishing one OpenAI-compatible provider
			//! from another.  `requiresAuth` true -> ALWAYS emit the
			//! `Authorization: Bearer <key>` header (OpenAI, xAI).  false
			//! -> emit it ONLY when the api key is non-empty (a local
			//! server: no key -> no header at all, matching Ollama, which
			//! rejects nothing but also expects none; an explicit
			//! --api-key local server still gets a Bearer header).
			struct Config
			{
				std::string providerName;    //!< "openai" / "xai" / "local" -- the ProviderName() label
				std::string baseUrl;         //!< full endpoint URL for the selected wire mode
				std::string defaultModelId;  //!< model id when the caller leaves it empty
				bool        requiresAuth = true;  //!< see the struct doc above; defaults fail-closed (require auth) so a default-constructed Config never reads indeterminate

				//! Rides straight through to BuildRequest's ChatHttpRequest.
				//! timeoutSeconds -- the per-request budget the platform
				//! transports honor.  Default 300s matches every hosted
				//! provider (OpenAI, xAI).  MakeCodec's LOCAL provider
				//! instantiation (AgentChatLoop.cpp) raises this to 900s: a
				//! cold local-model swap (17-43GB load) plus a long 70B
				//! generation legitimately exceeds the hosted-provider
				//! budget and was observed timing out at 300s.
				long        requestTimeoutSeconds = 300;
				bool        useResponsesApi = false; //!< OpenAI-native Responses wire; false for compatible providers
			};

			//! Default: OpenAI's native Responses API.
			OpenAIChatCodec();

			//! Parameterized: any OpenAI-compatible provider.  The caller
			//! (MakeCodec) resolves any env-derived base URL BEFORE
			//! constructing, so the codec itself reads no environment.
			explicit OpenAIChatCodec( const Config& config );

			virtual const char* ProviderName() const;
			virtual const char* DefaultModelId() const;
			virtual std::string MakeUserEntry(
				const std::string& text,
				const std::vector<ChatAttachment>& attachments = std::vector<ChatAttachment>() ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const;
			virtual std::string RewriteElidedUserImages(
				const std::string& userEntryJson, int countToElide ) const;
			virtual std::string RewriteElidedToolResults(
				const std::string& packedEntryJson,
				const std::vector<std::size_t>& resultIndices,
				const std::string& placeholderText ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries,
				bool forceReasoningEffortNone = false ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
			virtual ChatUsage ParseUsage( const std::string& rawBody ) const;
			virtual std::size_t ToolsWireBytes() const;

		private:
			Config mConfig;
		};

		//! True iff packing (call, raw JSON-RPC envelope line) would carry
		//! a LIVE image block/part -- i.e. a success result from one of the
		//! image-capable verbs (read_image, compare_to_reference,
		//! read_viewport, or a render called with imageMaxEdge) with a
		//! non-empty png_base64.
		//! Shared by the loop (to decide when the
		//! image-elision pass must run) and the codecs (which use the same
		//! predicate to build the image block/part), so the two can never
		//! disagree about what counts as an image-bearing result.
		bool ChatToolResultCarriesImage( const ChatToolCall& call,
		                                 const std::string& rawJsonRpcResponseLine );

		//! SUPERSEDED-READ RETENTION (see AgentChatLoop.h): the supersession
		//! KEY of one packed tool result, or "" when the result is not
		//! supersedable.  Two results share a key iff a later one makes the
		//! earlier one obsolete, so the loop's rule is simply "for each
		//! non-empty key, keep only the LAST".
		//!
		//! A verb qualifies for the allowlist only when ALL of these hold.
		//! The list is deliberately short; every excluded verb below either
		//! FAILS one of these properties or is named with the separate reason
		//! it is off the list (render is excluded on
		//! information-preservation):
		//!   (1) WHOLE-VIEW.  The result is the entire view of one named
		//!       piece of state, so a later result fully CONTAINS what an
		//!       earlier one carried.  (Excludes read_schema and read_skill:
		//!       their results are keyed by keyword/category/name, so two
		//!       calls return DIFFERENT content and neither supersedes the
		//!       other.  Excludes query_object_at for the same reason.)
		//!   (2) MUTABLE.  The underlying state can change between calls, so
		//!       an older copy is not merely redundant but potentially STALE
		//!       -- which is what makes elision a correctness improvement and
		//!       not just a token saving.  (Excludes read_schema/read_skill
		//!       again: both are STATELESS on-disk/registry reads whose
		//!       results never go stale.)
		//!   (3) ARGUMENT-INDEPENDENT.  The result does not depend on the
		//!       call's arguments, so "same verb" IS "same view" and the key
		//!       can be the verb name alone.  A future argument-DEPENDENT
		//!       verb would need an argument-normalizing key function here;
		//!       today none is on the list, and adding one without that
		//!       function would elide results that do not supersede.
		//!   (4) NOT ALREADY COVERED.  A result IsImageResult matches is
		//!       governed by the separate, older IMAGE RETENTION rule; running
		//!       both rules over one result would elide it twice.  (An entry
		//!       may still be touched by BOTH rules when it packs an image
		//!       result ALONGSIDE a supersedable one -- that is fine and
		//!       tested; what (4) forbids is one RESULT being claimed by
		//!       both.)
		//! ON THE LIST TODAY: read_document and list_proposals.
		//!
		//! list_proposals is a parameterless whole-queue read of mutable
		//! state and is not image-bearing, so it satisfies (1)-(4).  It is
		//! NOT declared in kToolDefs, so no model is TOLD about it -- but
		//! that is not unreachability: ToolCallToJsonRpcLine forwards
		//! whatever name the model emits WITHOUT checking it against
		//! kToolDefs, and the dispatcher answers it, so an injected or
		//! hallucinated list_proposals really does dispatch and pack into
		//! the transcript.  (An earlier revision of this comment excluded it
		//! as "untestable dead code"; both halves of that were false -- the
		//! path is reachable and the test below drives it.)  The same
		//! forwarding makes read_viewport's PNGs real, which is why
		//! IsImageResult now lists it -- and which is also why read_viewport
		//! stays OFF this allowlist: property (4), it is claimed by the image
		//! rule.  render_status / render_wait / render_cancel are keyed by
		//! `renderJobId`, so they fail (1) WHOLE-VIEW and (3)
		//! ARGUMENT-INDEPENDENT -- two calls describe two different jobs and
		//! neither supersedes the other.
		//!
		//! Two further exclusions worth naming because they look eligible:
		//!   * render -- the render result's per-channel means are explicitly
		//!     meant to be COMPARED against the previous render (kToolDefs
		//!     says so to the model), so eliding the older one would destroy
		//!     information the model is instructed to use.  Its INLINE image
		//!     (imageMaxEdge) is a different matter and IS elided, by IMAGE
		//!     RETENTION -- that rule strips only the image, leaving the
		//!     statistics live, which is exactly the split wanted here.
		//!   * validate -- its result is a diagnostics list of tens of bytes,
		//!     and in the `text` form it is a pure function of an argument
		//!     (fails (3)).
		//!
		//! ERROR RESULTS NEVER SUPERSEDE, and are never supersedable: this
		//! returns "" for a JSON-RPC error envelope (or an unparseable line).
		//! A failed read_document must not evict the last GOOD one -- that
		//! would leave the model with neither.
		//!
		//! NOR DOES AN UNINFORMATIVE SUCCESS.  read_document answers the
		//! NO-HEAD case with a SUCCESS carrying hasDocument:false and an empty
		//! document; in the co-editing GUI the user can close the scene
		//! between two agent reads, and letting that supersede would evict the
		//! last real document in favour of nothing.  See the .cpp's
		//! INFORMATIVE-RESULT GATE.
		std::string ChatToolResultSupersessionKey( const ChatToolCall& call,
		                                           const std::string& rawJsonRpcResponseLine );

		//! The honest, actionable placeholder that replaces a superseded
		//! result's payload -- a pure function of the verb name, so the
		//! rewrite stays deterministic and replay-safe.  It says WHAT was
		//! removed, WHY, and HOW to get it back, exactly as the image
		//! elision note does.
		std::string ChatSupersededResultNote( const std::string& verbName );

		//! Number of LIVE (not-yet-elided) image blocks/parts in a
		//! MakeUserEntry-produced user entry -- i.e. how many of its
		//! attachments still carry a real image block/part rather than
		//! the "[reference image elided ...]" placeholder.  PROVIDER-
		//! AGNOSTIC: dispatches on the entry's own shape ("content"
		//! array of {type:"image",...} elements for Anthropic; "parts"
		//! array of {inlineData:...} elements for Gemini; "content" array
		//! of {type:"image_url",...} elements for OpenAI, gated on
		//! role=="user" so it does not double-count the Anthropic
		//! {type:"image"} count above on a shape that happens to reuse
		//! the "content" key) rather than needing a codec instance, so
		//! AgentChatLoop's cap bookkeeping (USER IMAGE RETENTION) can
		//! call it uniformly regardless of which codec produced the
		//! entry.  Returns 0 for a non-user entry, or one that does not
		//! parse.
		int ChatUserEntryLiveImageCount( const std::string& userEntryJson );

		//! A stable, provider-neutral fingerprint SOURCE string for the ten
		//! tool definitions (Eval-harness E1): the concatenation of every
		//! neutral tool's name + description + parameter-schema, in order.
		//! The trajectory `session` record hashes this so a tool-definition
		//! change is visible as a changed tool_defs hash across replays.
		//! Provider-neutral by construction -- it reads the ONE kToolDefs
		//! table every codec maps from, so the fingerprint never depends on
		//! which provider is selected.
		std::string ChatToolDefsFingerprint();
	}
}

#endif
