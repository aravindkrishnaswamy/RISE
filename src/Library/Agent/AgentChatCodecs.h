//////////////////////////////////////////////////////////////////////
//
//  AgentChatCodecs.h - pure, sans-IO LLM provider wire codecs and
//    single-turn image/text completion builders.
//
//    Providers:
//      * AnthropicChatCodec -- Messages API (`x-api-key` + `anthropic-version`).
//      * GeminiChatCodec    -- v1beta `generateContent` (`x-goog-api-key`),
//        with `functionCall`/`functionResponse` (`parts[].inlineData`).
//      * OpenAIChatCodec    -- OpenAI Responses API by default; parameterized
//        Chat Completions for compatible providers (xAI, Local/Ollama).
//
//    Core invariants:
//      * Key hygiene: API keys appear only in the auth header (never URL/body/
//        logs); header values built from caller input strip control chars < 0x20.
//      * Verbatim assistant echo: ParseResponse captures assistant turns as raw
//        JSON byte spans so opaque fields (thinking/thought signatures) survive.
//      * Record-or-refuse: ParseResponse refuses the entire body (ProviderError)
//        on invalid stop/finish reasons with calls, duplicate or invalid call ids
//        (Gemini synthesizes ids for id-less calls), non-object present args,
//        spoofed roles, or degenerate blank final turns — never partial salvage.
//      * Tool results & images: one assistant turn's tool results pack into a
//        single following user entry; image-bearing results strip base64 from
//        text into native image parts (elidable via RewriteElidedImages). User
//        attachments (`ChatAttachment`) precede text and elide independently.
//      * Single tool table: `kToolDefs` in AgentChatCodecs.cpp is the sole
//        provider-neutral declaration source (`ask_user` is driver-handled and
//        maps to no AgentRpc verb).
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
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//! User-supplied reference image attachment (`image/png`, `image/jpeg`,
		//! `image/gif`, `image/webp`; raw base64 with no data-URL prefix).
		//! Never log either field.
		struct ChatAttachment
		{
			std::string mimeType;
			std::string base64Data;
		};

		//! Sans-IO HTTP POST request description executed by the caller.
		struct ChatHttpRequest
		{
			std::string url;      //!< absolute https/http POST URL
			std::vector<std::pair<std::string, std::string>> headers;   //!< includes content-type + auth header
			std::string body;     //!< JSON request body
			long timeoutSeconds = 300; //!< per-request transport budget (raised to 900s for Local)
		};

			//! Normalized token-usage counts parsed from one provider response
			//! (`-1` = absent; provider negatives normalize to 0).
			//!
			//! Invariants (enforced by EnforceUsageInvariant in .cpp):
			//!   * `outputTokens` is total billed generation (visible + reasoning).
			//!     Gemini/xAI separate reasoning counters are folded in; Anthropic/OpenAI
			//!     already include reasoning in output and pass through.
			//!   * Whenever `outputTokens >= 0`: `0 <= reasoningOutputTokens <= outputTokens`.
			//!     Self-contradictory bodies clamp `reasoningOutputTokens` to `outputTokens`,
			//!     setting `reasoningClamped` while preserving the raw claim in
			//!     `reasoningOutputTokensReported`.
			//!   * Reasoning reported without an output counter sets `outputTokens`
			//!     equal to `reasoningOutputTokens` (including reported 0).
			struct ChatUsage
			{
				long long inputTokens          = -1;          //!< prompt / input tokens (-1 = absent)
				long long outputTokens         = -1;          //!< total billed generation: visible + reasoning (-1 = absent)
				long long cacheReadInputTokens = -1;          //!< cache-read input tokens (-1 = absent)
				long long reasoningOutputTokens = -1;         //!< reasoning subset of outputTokens (-1 = absent)
				long long reasoningOutputTokensReported = -1; //!< pre-clamp provider claim (evidence when reasoningClamped)
				bool      reasoningClamped = false;           //!< true iff raw reasoning exceeded billed output
			};

		//! One requested tool call. Gemini calls without an id receive synthesized
		//! `"call_N"` ids (`idSynthesized = true`, withheld from wire responses);
		//! duplicate ids within a turn refuse the whole response.
		struct ChatToolCall
		{
			std::string id;                    //!< provider (or synthesized) call id
			std::string name;                  //!< tool name (JSON-RPC verb)
			std::string argsJson;              //!< call arguments JSON object string
			bool        idSynthesized = false; //!< true iff fabricated by codec (never echoed)
		};

		//! Machine-readable classification for ProviderError outcomes.
		enum class ChatErrorKind
		{
			None,              //!< non-error (ToolCalls / FinalText)
			Http,              //!< non-200 HTTP status
			Parse,             //!< response body did not parse as JSON
			Provider,          //!< degenerate / unexpected provider turn
			Refusal,           //!< safety / policy refusal (terminal)
			MaxTokens,         //!< output truncated at token cap (discarded; do not verbatim-retry)
			IterationCap,      //!< per-turn tool-round cap tripped
			MalformedToolCall, //!< unparseable/malformed tool call (loop applies bounded corrective retry)
			Misuse             //!< caller-contract violation (e.g. HandleResponse while calls pending)
		};

		//! Outcome of one HTTP round-trip.
		struct ChatStepResult
		{
			enum class Kind
			{
				ToolCalls,      //!< model requested tool calls
				FinalText,      //!< model completed turn with text
				ProviderError   //!< HTTP / parse / policy / cap failure
			};

			Kind                      kind = Kind::ProviderError;
			std::vector<ChatToolCall> toolCalls;               //!< non-empty iff kind == ToolCalls
			std::string               finalText;               //!< filled iff kind == FinalText (may be empty)
			std::string               errorMessage;            //!< filled iff kind == ProviderError
			ChatErrorKind             errorKind = ChatErrorKind::None;
			std::string               assistantDisplayText;    //!< display narration for ToolCalls/FinalText; empty on error

			//! Display-only extracted reasoning text (Anthropic thinking blocks;
			//! OpenAI-compatible message.reasoning / reasoning_content; empty for
			//! Gemini/gpt/errors). Never re-serialized onto the wire.
			std::string               reasoningText;

			//! Recovery hints (ProviderError only): driver should rebuild via
			//! BuildRequest and retry the same round once (or until budget expires).
			bool                      retryWithoutImages = false;       //!< Http 400: text-only model; loop already elided all images
			bool                      retryReasoningEffortNone = false; //!< Http 400: Chat-Completions reasoning_effort; sticky override set
			bool                      retryDegenerateTurn = false;      //!< Provider blank final turn (verbatim retry) or MalformedToolCall (queued corrective)
		};

		//! ParseResponse output: step disposition plus verbatim assistant wire JSON
		//! and display extractions (`assistantEntryJson` empty on ProviderError).
		struct ChatParsedResponse
		{
			ChatStepResult step;
			std::string    assistantEntryJson;   //!< raw provider-native entry (verbatim content span)
			std::string    assistantDisplayText; //!< concatenated text blocks/parts (display)
			std::string    reasoningText;        //!< display-only reasoning extraction
		};

		//! Pure, stateless provider wire codec interface.
		class IChatProviderCodec
		{
		public:
			virtual ~IChatProviderCodec() {}

			virtual const char* ProviderName() const = 0;   //!< "anthropic" / "gemini" / "openai" / "xai" / "local"
			virtual const char* DefaultModelId() const = 0; //!< fallback model id when caller supplies ""

			//! Build a user transcript entry (attachments placed before text; empty
			//! attachments matches text-only shape verbatim).
			virtual std::string MakeUserEntry(
				const std::string& text,
				const std::vector<ChatAttachment>& attachments = std::vector<ChatAttachment>() ) const = 0;

			//! Pack all tool results of one assistant turn into one user-turn entry.
			//! Strips base64 PNGs into native image parts (only the last image in a
			//! multi-image pack stays live); JSON-RPC errors map to error tool results.
			virtual std::string PackToolResults(
				const std::vector<std::pair<ChatToolCall, std::string>>& results ) const = 0;

			//! Replace live image blocks/parts in a loop-generated ToolResults entry
			//! with an elision note (preserving any RPC-owned `"note"` field via
			//! `"image_note"` preference and value gating). Never pass assistant entries.
			virtual std::string RewriteElidedImages( const std::string& packedEntryJson ) const = 0;

			//! Elide the oldest `countToElide` live reference images in a MakeUserEntry
			//! entry to a text placeholder. Never pass assistant entries.
			virtual std::string RewriteElidedUserImages(
				const std::string& userEntryJson, int countToElide ) const = 0;

			//! Replace payloads at positional `resultIndices` in a PackToolResults
			//! entry with `placeholderText` (under `noteKey` on object-payload wires),
			//! preserving tool-call id/name bindings verbatim. Never pass assistant entries.
			virtual std::string RewriteElidedToolResults(
				const std::string& packedEntryJson,
				const std::vector<std::size_t>& resultIndices,
				const std::string& placeholderText,
				const std::string& noteKey ) const = 0;

			//! Build HTTP POST request (`apiKey` for auth header only; never logged).
			//! `forceReasoningEffortNone` adds `"reasoning_effort":"none"` on OpenAI
			//! Chat Completions requests only.
			virtual ChatHttpRequest BuildRequest(
				const std::string& modelId,
				const std::string& apiKey,
				const std::string& systemPrompt,
				const std::vector<std::string>& rawEntries,
				bool forceReasoningEffortNone = false ) const = 0;

			//! Parse HTTP status + raw body into ChatParsedResponse. Never throws.
			virtual ChatParsedResponse ParseResponse(
				long httpStatus, const std::string& rawBody ) const = 0;

			//! Parse normalized ChatUsage from raw body (absent-tolerant). Never throws.
			virtual ChatUsage ParseUsage( const std::string& rawBody ) const = 0;

			//! Fixed byte length of this codec's tool declarations (for token estimates).
			virtual std::size_t ToolsWireBytes() const = 0;
		};

		//! Anthropic Messages API codec.
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
				const std::string& placeholderText,
				const std::string& noteKey ) const;
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

		//! Google Gemini v1beta REST (`generateContent`) codec.
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
				const std::string& placeholderText,
				const std::string& noteKey ) const;
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

		//! OpenAI codec: native Responses API by default; parameterized Chat
		//! Completions for compatible providers (xAI, Local/Ollama).
		class OpenAIChatCodec : public IChatProviderCodec
		{
		public:
			struct Config
			{
				std::string providerName;            //!< "openai" / "xai" / "local"
				std::string baseUrl;                 //!< full endpoint URL
				std::string defaultModelId;          //!< fallback model id
				bool        requiresAuth = true;     //!< true: always send Bearer header; false: only if key non-empty
				long        requestTimeoutSeconds = 300; //!< forwarded to ChatHttpRequest::timeoutSeconds (Local uses 900s)
				bool        useResponsesApi = false; //!< true for OpenAI native Responses; false for Chat Completions
			};

			OpenAIChatCodec();
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
				const std::string& placeholderText,
				const std::string& noteKey ) const;
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

		//! True iff (call, responseLine) is a success result carrying a non-empty
		//! `png_base64` on an image-capable verb (shared across loop, codecs, and MCP).
		bool ChatToolResultCarriesImage( const ChatToolCall& call,
		                                 const std::string& rawJsonRpcResponseLine );

		//! Supersession key for one packed tool result, or "" if not supersedable
		//! (keep only the last per non-empty key). Covers `read_document`,
		//! `list_proposals`, and named `read_skill` (keyed as `kChatReadSkillKeyPrefix + name`).
		//! Error envelopes and uninformative `hasDocument:false` results return "".
		std::string ChatToolResultSupersessionKey( const ChatToolCall& call,
		                                           const std::string& rawJsonRpcResponseLine );

		//! Placeholder JSON keys for object-payload wires (Gemini/OpenAI).
		inline constexpr char kChatSupersededNoteKey[] = "superseded_note";
		inline constexpr char kChatRetiredNoteKey[] = "retired_note";

		//! Deterministic placeholders for superseded tool results.
		std::string ChatSupersededResultNote( const std::string& verbName );
		inline constexpr char kChatReadSkillKeyPrefix[] = "read_skill\x1f";
		std::string ChatSupersededResultNoteForKey( const std::string& supersessionKey );

		//! Doc 91 L2 immutable-fact stub for build_element/insert_chunks results
		//! retired when `element` finishes (never asserts mutable edit/reopen rules).
		std::string ChatElementFinishedRetirementNote( const std::string& verbName,
		                                               const std::string& element,
		                                               const std::string& facts,
		                                               const std::string& carried );

		//! Canonical clean-insert message (pinned equal to kCstChunkInsertedMessage by tests).
		inline constexpr char kChatChunkInsertedMessage[] =
			"chunk inserted via a full re-derive (Scene + managers were replaced)";

		//! Doc 91 L3b past-tense stub for named read_skill results retired at compose phase.
		std::string ChatComposeSkillRetirementNote( const std::string& skillName );

		//! Compose phase spelling (pinned equal to AgentSession::BuildPhaseName(Compose) by tests).
		inline constexpr char kChatComposePhaseName[] = "compose";

		//! Provider-agnostic count of live (un-elided) user reference images in a
		//! MakeUserEntry JSON string (0 for non-user or unparseable entries).
		int ChatUserEntryLiveImageCount( const std::string& userEntryJson );

		//! Provider-neutral fingerprint source over `kToolDefs` for trajectory hashing.
		std::string ChatToolDefsFingerprint();

		//--------------------------------------------------------------
		// Provider image generation (`imagine_scene` wire helpers) and
		// single-turn text completion (`build_element` / clean-room passes;
		// see docs/agentic-redesign/79-clean-room-construction.md).
		//
		// Sans-IO request/response builders using the same auth-header hygiene
		// as the chat codecs; parse errors are header-free and body-free.
		//--------------------------------------------------------------

		//! True for "gemini" and "openai"; false otherwise.
		bool ChatProviderSupportsImageGeneration( const std::string& providerName );

		//! Image model id (`RISE_IMAGE_MODEL_GEMINI` default "gemini-3.1-flash-image";
		//! `RISE_IMAGE_MODEL_OPENAI` default "gpt-image-1"; "" if unsupported).
		std::string ChatImageGenerationModelId( const std::string& providerName );

		//! Host-appended flat-shaded 3D-render style directive (overridable via
		//! `RISE_IMAGE_STYLE_PROMPT`) keeping generated targets reachable.
		std::string ChatImageStyleDirective();

		//! Composes `description` verbatim + blank line + ChatImageStyleDirective().
		std::string ComposeImageGenerationPrompt( const std::string& description );

		//! Build image-generation POST (`RISE_IMAGE_ENDPOINT_{GEMINI,OPENAI}`,
		//! `RISE_IMAGE_MODALITIES_GEMINI`, `RISE_IMAGE_SIZE_OPENAI` overrides).
		bool BuildImageGenerationRequest( const std::string& providerName,
		                                  const std::string& modelId,
		                                  const std::string& apiKey,
		                                  const std::string& prompt,
		                                  ChatHttpRequest& outRequest,
		                                  std::string& outError );

		//! Extract first decoded image bytes + MIME type from a 200-OK response.
		bool ParseImageGenerationResponse( const std::string& providerName,
		                                   const std::string& body,
		                                   std::vector<unsigned char>& outBytes,
		                                   std::string& outMimeType,
		                                   std::string& outError );

		class IChatHttpTransport;

		//! Blocking image-generation outcome (mirrors AgentSession::AgentImageGenOutcome;
		//! kept separate so this header stays independent of AgentSession.h).
		struct ChatImageGenOutcome
		{
			bool                       ok = false;
			std::vector<unsigned char> bytes;
			std::string                mimeType;
			std::string                error;
		};

		//! Host-installable image generator (mirrors AgentSession::AgentImageGenerator).
		struct ChatImageGenerator
		{
			bool        supported = false;
			std::string providerName;
			std::string modelId;
			std::function<ChatImageGenOutcome( const std::string& description )> generate;
		};

		//! Build a synchronous host image generator capturing `apiKey` by value and
		//! `transport` by shared_ptr (reinstall after credential/provider changes).
		ChatImageGenerator MakeChatImageGenerator(
			const std::string& providerName,
			const std::string& apiKey,
			std::shared_ptr<IChatHttpTransport> transport );

		//! True for "anthropic", "gemini", "openai", "xai"; false for "local"/unknown.
		bool ChatProviderSupportsTextCompletion( const std::string& providerName );

		//! Text-completion model id (`RISE_BUILDER_MODEL_{ANTHROPIC,GEMINI,OPENAI,XAI}`,
		//! defaulting to each chat codec's DefaultModelId; "" if unsupported).
		std::string ChatTextCompletionModelId( const std::string& providerName );

		//! Baseline output-token budget for single-turn builder completions
		//! (Gemini requests 2x to accommodate thinking tokens).
		unsigned int ChatTextCompletionMaxTokens();

		//! Build single-turn minimal text-completion POST (no system prompt or tools;
		//! overridable via `RISE_BUILDER_ENDPOINT_{ANTHROPIC,GEMINI,OPENAI,XAI}`).
		bool BuildTextCompletionRequest( const std::string& providerName,
		                                 const std::string& modelId,
		                                 const std::string& apiKey,
		                                 const std::string& prompt,
		                                 ChatHttpRequest& outRequest,
		                                 std::string& outError );

		//! Extract assistant text from a 200-OK response (empty text fails; errors
		//! are header-free and body-free).
		bool ParseTextCompletionResponse( const std::string& providerName,
		                                  const std::string& body,
		                                  std::string& outText,
		                                  std::string& outError );

		//! Blocking text-completion outcome (mirrors AgentSession::AgentTextCompletionOutcome).
		struct ChatTextCompletionOutcome
		{
			bool        ok = false;
			std::string text;
			std::string error;
		};

		//! Host-installable text completer (mirrors AgentSession::AgentTextCompleter).
		struct ChatTextCompleter
		{
			bool        supported = false;
			std::string providerName;
			std::string modelId;
			std::function<ChatTextCompletionOutcome( const std::string& prompt )> complete;
		};

		//! Build a synchronous host text completer (same key/transport lifetime
		//! contract as MakeChatImageGenerator; never throws).
		ChatTextCompleter MakeChatTextCompleter(
			const std::string& providerName,
			const std::string& apiKey,
			std::shared_ptr<IChatHttpTransport> transport );
	}
}

#endif
