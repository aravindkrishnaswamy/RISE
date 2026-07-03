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
//    Two implementations:
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
//        malformed (non-object) Gemini functionCall value, a Gemini
//        candidate whose content.role is present and not "model" (a
//        spoofed role would join BuildRequest's user merge on
//        replay), and degenerate empty-content final turns.  Never
//        "salvage" part of such a body: the raw echo would replay
//        the un-answerable call blocks on every later request.
//      * All tool results for one assistant turn are packed into ONE
//        following user message (an Anthropic hard requirement for
//        parallel tool_use; mirrored for Gemini).
//      * read_image results carry a REAL image block: the base64 PNG is
//        extracted into an image part (Anthropic `tool_result` content
//        image block; Gemini `functionResponse.parts[].inlineData` --
//        the documented FunctionResponsePart mechanism for multimodal
//        function output) and STRIPPED from the textual part so it is
//        not double-sent.  RewriteElidedImages later replaces such an
//        image block/part with a short text note (the loop keeps only
//        the MOST RECENT image live -- see AgentChatLoop.h).
//
//    The six tool definitions (mapping 1:1 to the AgentRpc verbs) are
//    defined ONCE, provider-neutrally, in AgentChatCodecs.cpp; each
//    codec maps them into its native tool declaration shape.  Their
//    parameter names/shapes mirror AgentRpc.cpp exactly.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTCHATCODECS_
#define RISE_AGENT_AGENTCHATCODECS_

#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//! A fully-described HTTP request the CALLER performs.  The codec
		//! never talks to the network; it only fills this in.
		struct ChatHttpRequest
		{
			std::string url;      //!< absolute https URL (POST)
			std::vector<std::pair<std::string, std::string>> headers;   //!< header name/value pairs (includes content-type + the auth header)
			std::string body;     //!< the JSON request body
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
			MaxTokens,     //!< the reply hit the output-token cap (truncated; discarded)
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
			std::string               finalText;     //!< filled iff kind==FinalText
			std::string               errorMessage;  //!< filled iff kind==ProviderError
			ChatErrorKind             errorKind = ChatErrorKind::None;  //!< set on EVERY ProviderError; None otherwise

			//! The assistant's interim display text for THIS turn --
			//! filled for BOTH ToolCalls (the text alongside the calls,
			//! possibly empty) and FinalText (== finalText), so a GUI can
			//! show the model's narration while tools run.  Empty on
			//! ProviderError.
			std::string               assistantDisplayText;
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
		};

		//! The pure provider-codec interface.  Stateless: every method is
		//! const and the transcript lives in AgentChatLoop as a list of
		//! raw provider-native entry strings this codec produced.
		class IChatProviderCodec
		{
		public:
			virtual ~IChatProviderCodec() {}

			//! A short human name ("anthropic" / "gemini"), used to prefix
			//! HTTP-error messages (see ParseResponse's non-200 path).
			virtual const char* ProviderName() const = 0;

			//! The default model id used when the loop is given none.
			virtual const char* DefaultModelId() const = 0;

			//! A provider-native transcript entry for a plain user text
			//! message.
			virtual std::string MakeUserEntry( const std::string& text ) const = 0;

			//! Pack ALL tool results of one assistant turn into ONE
			//! provider-native user-turn entry.  Each element pairs the
			//! ChatToolCall with the raw JSON-RPC response ENVELOPE line
			//! from AgentRpcDispatcher::HandleLine.  read_image results
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
			//! contradicting the elided block; an RPC-owned "note" field
			//! is never clobbered (the attach note then lives under
			//! "image_note", which the rewrite prefers).
			//! Used by the loop to keep only the MOST RECENT image live
			//! (see AgentChatLoop.h "IMAGE RETENTION").  This regeneration
			//! is legal ONLY because ToolResults entries are loop-generated
			//! -- assistant entries carry the byte-preservation contract
			//! and must never pass through here.  Returns the entry
			//! unchanged when it carries no image (or does not parse).
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const = 0;

			//! Build the full HTTP request for the current conversation.
			//! `apiKey` is used ONLY for the auth header -- it is not
			//! retained, not placed in the body/url, and never logged.
			//! `rawEntries` are the transcript entries in order (each a
			//! provider-native message JSON produced by this codec).
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries ) const = 0;

			//! Parse one raw HTTP response.  `httpStatus` is the status
			//! code the caller observed (non-200 -> ProviderError carrying
			//! the provider's error.message when parseable).  NEVER throws.
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const = 0;
		};

		//! Anthropic Messages API codec (see file header).
		class AnthropicChatCodec : public IChatProviderCodec
		{
		public:
			virtual const char* ProviderName() const;
			virtual const char* DefaultModelId() const;
			virtual std::string MakeUserEntry( const std::string& text ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
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
			virtual std::string MakeUserEntry( const std::string& text ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
		};

		//! True iff packing (call, raw JSON-RPC envelope line) would carry
		//! a LIVE image block/part -- i.e. a read_image success result with
		//! a non-empty png_base64.  Shared by the loop (to decide when the
		//! image-elision pass must run) and the codecs (which use the same
		//! predicate to build the image block/part), so the two can never
		//! disagree about what counts as an image-bearing result.
		bool ChatToolResultCarriesImage( const ChatToolCall& call,
		                                 const std::string& rawJsonRpcResponseLine );
	}
}

#endif
