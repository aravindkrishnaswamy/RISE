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
//        `x-goog-api-key` header).  Wire shape verified against the
//        live v1beta docs on 2026-07-02 (functionDeclarations /
//        functionCall / functionResponse / inlineData).
//
//    KEY DESIGN RULES (see AgentChatLoop.h for the loop contract):
//      * The API key appears ONLY in the auth header the codec emits --
//        never in the URL, never in the body, never in any other string
//        the codec produces.  Codecs log NOTHING (bodies may embed
//        scene content; the key must never reach a log either).
//      * Assistant turns are stored as RAW provider-native JSON and
//        echoed VERBATIM (byte-preserved) on subsequent BuildRequests.
//        ParseResponse extracts the assistant content as a raw byte
//        span of the response body -- NOT a parse + re-serialize --
//        so provider-opaque fields (Anthropic thinking-block
//        `signature`s, Gemini thought signatures) round-trip intact.
//      * All tool results for one assistant turn are packed into ONE
//        following user message (an Anthropic hard requirement for
//        parallel tool_use; mirrored for Gemini).
//      * read_image results carry a REAL image block: the base64 PNG is
//        extracted into an image part (Anthropic `tool_result` content
//        image block / Gemini `inlineData` part) and STRIPPED from the
//        textual part so it is not double-sent.
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
		//! tool-call id (Anthropic `tool_use.id`); Gemini function calls
		//! carry NO id, so the codec synthesizes "call_0", "call_1", ...
		//! per assistant turn (results are matched by name + order).
		struct ChatToolCall
		{
			std::string id;        //!< provider (or synthesized) call id
			std::string name;      //!< the tool name (a JSON-RPC verb)
			std::string argsJson;  //!< the call arguments as a JSON object string
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

			//! A short human name ("anthropic" / "gemini") for messages.
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
			//! textual half); JSON-RPC error envelopes become error tool
			//! results (Anthropic `is_error: true`).
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const = 0;

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
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
		};

		//! Google Gemini v1beta REST codec (see file header).  Wire shape
		//! web-verified 2026-07-02 against ai.google.dev/api/generate-content.
		class GeminiChatCodec : public IChatProviderCodec
		{
		public:
			virtual const char* ProviderName() const;
			virtual const char* DefaultModelId() const;
			virtual std::string MakeUserEntry( const std::string& text ) const;
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const;
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries ) const;
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const;
		};
	}
}

#endif
