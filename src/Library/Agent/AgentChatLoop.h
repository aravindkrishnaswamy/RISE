//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.h - the provider-agnostic, sans-IO chat-loop core for
//    the LLM scene-editing chat panel (Facet 5, the agentic surface --
//    slice B1).
//
//    OWNS conversation state and translates between LLM provider wire
//    formats (via AgentChatCodecs) and the seven JSON-RPC verbs the
//    AgentRpcDispatcher speaks -- but performs NO I/O.  The caller (the
//    Swift GUI in slice B2; C++ tests today) drives the loop:
//
//      loop.AddUserMessage(text);
//      for (;;) {
//          ChatHttpRequest req = loop.BuildRequest(apiKey);   // caller does HTTP
//          ChatStepResult  st  = loop.HandleResponse(status, body);
//          if (st.kind != ToolCalls) break;                   // FinalText / error
//          for (call : st.toolCalls) {
//              line = dispatcher.HandleLine(loop.ToolCallToJsonRpcLine(call, id++));
//              loop.AddToolResult(call, line);                // packed per-provider
//          }
//      }
//
//    CONTRACT / KEY RULES:
//      * SWITCHING PROVIDER RESETS THE TRANSCRIPT.  Assistant turns are
//        stored as RAW provider-native JSON (echoed verbatim on later
//        requests -- thinking blocks with signatures must round-trip
//        unmodified), so they CANNOT cross providers.
//      * The API key is passed INTO BuildRequest per call, held nowhere,
//        and appears ONLY in the request's auth header.  Nothing in this
//        module logs request/response bodies or the key.
//      * All tool results for one assistant turn are flushed into ONE
//        user message (AddToolResult buffers; the flush happens when
//        every call of the turn has a result, or at the next
//        BuildRequest / AddUserMessage).  A flush SYNTHESIZES an error
//        tool result ("tool call was not executed...") for every
//        pending call that has no buffered result.  Together with the
//        codecs' record-or-refuse rule (ParseResponse REFUSES -- and
//        the loop records nothing of -- any response whose tool calls
//        could not become the pending set: calls under a non-tool-call
//        stop_reason/finishReason, malformed call blocks, DUPLICATE
//        call ids on either provider, and id-less Anthropic tool_use
//        blocks -- Gemini instead SYNTHESIZES ids for id-less calls;
//        see AgentChatCodecs.h), the wire invariant "every RECORDED
//        tool call is answered in the immediately-following user
//        message" holds for every entry this loop records, on both
//        providers (Anthropic hard-400s unanswered tool_use ids;
//        Gemini rejects mismatched functionResponses).
//      * HONEST POISON SCOPING: a user interrupt, tool crash, or
//        hostile response body cannot create RECORDED-BUT-
//        UNANSWERABLE tool calls -- the parse gates plus the flush
//        synthesis guarantee that much.  What byte-preservation CANNOT
//        guarantee is that every recorded echo replays cleanly: an
//        exotic-but-well-formed content the codec does not model
//        (e.g. a server_tool_use block under end_turn) is echoed
//        verbatim and may be invalid on replay.  Reset() /
//        SetProvider() recovers; the GUI driver should offer that on
//        repeated HTTP 400s.
//      * IMAGE RETENTION: only the MOST RECENT read_image PNG stays
//        live in the transcript.  When a new tool-results entry packs
//        an image, every OLDER ToolResults entry's image block/part is
//        rewritten to a short "[image elided -- superseded by a newer
//        render]" text note (Anthropic: the {type:"image"} element;
//        Gemini: the functionResponse.parts inlineData) via the
//        codec's RewriteElidedImages -- which also rewrites the loop-
//        written "the PNG is attached ..." note so the model never
//        sees contradictory context.  The rule is entry-internal too:
//        when ONE flush packs several image results, PackToolResults
//        keeps only the LAST live (earlier ones are packed pre-
//        elided), so exactly one image is live globally.  Without all
//        this, every request re-sends (and re-bills) every historical
//        ~1.3MB base64 PNG and long sessions eventually exceed
//        provider request-size limits.  Rewriting is legal ONLY for
//        ToolResults entries -- they are loop-generated; assistant
//        entries keep the verbatim byte-preservation contract and are
//        never touched.
//      * CALLER-CONTRACT GUARDS (each refuses or ignores; none throw):
//          - HandleResponse while the previous turn's tool calls are
//            still pending returns ProviderError and records NOTHING
//            (a legitimate response can only follow a BuildRequest,
//            which flushes the pending set first).
//          - AddToolResult for a call id that is NOT pending, or a
//            SECOND result for an already-answered id, is IGNORED
//            (first result wins; results are accepted only for the
//            current assistant turn's pending calls).
//          - BuildRequest on an empty transcript returns an EMPTY
//            request (url == "") that the caller must not send.
//      * ITERATION CAP: at most kMaxToolRoundsPerTurn (20) tool rounds
//        per conversation-turn.  The 21st tool-call response within one
//        user turn returns ProviderError("iteration cap...") WITHOUT
//        recording the turn, so a GUI loop cannot spin.  The counter
//        resets on AddUserMessage.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTCHATLOOP_
#define RISE_AGENT_AGENTCHATLOOP_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AgentChatCodecs.h"

namespace RISE
{
	namespace Agent
	{
		//! The selectable providers (each backed by a codec).
		enum class ChatProvider
		{
			Anthropic,
			Gemini
		};

		//! One transcript entry as the GUI sees it.  `rawJson` is the
		//! provider-native message this entry contributes to BuildRequest;
		//! `displayText` is the human-readable extraction.
		struct ChatTranscriptEntry
		{
			enum class Role
			{
				User,         //!< a user text message
				Assistant,    //!< a model turn (text and/or tool calls)
				ToolResults   //!< the packed tool results of one model turn
			};

			Role        role = Role::User;
			std::string displayText;
			std::string rawJson;   //!< provider-native message JSON (verbatim for assistant turns)

			//! True while this ToolResults entry still carries a live
			//! image block/part.  Cleared when a NEWER image supersedes
			//! it and rawJson is rewritten with the image elided (see the
			//! IMAGE RETENTION rule in the file header).  Always false
			//! for User/Assistant entries.
			bool        carriesLiveImage = false;
		};

		//! The sans-IO chat loop (see the file header for the contract).
		//! Single-threaded, like the rest of the Agent module.
		class AgentChatLoop
		{
		public:
			//! Tool rounds allowed per conversation-turn before the loop
			//! refuses with ProviderError("iteration cap...").  Round N
			//! (N <= cap) succeeds; round cap+1 trips.
			static const int kMaxToolRoundsPerTurn = 20;

			//! Constructs with the Anthropic provider + its default model.
			AgentChatLoop();
			~AgentChatLoop();

			//! Clear the transcript, pending tool state, and the round
			//! counter.  The provider/model selection is kept.
			void Reset();

			//! Select the provider (and optionally a model id; empty =
			//! the provider's default).  RESETS THE TRANSCRIPT (assistant
			//! turns are provider-native and cannot cross providers) --
			//! even when re-selecting the current provider, for a uniform
			//! rule the GUI can state plainly.
			void SetProvider( ChatProvider provider, const std::string& modelId = std::string() );

			ChatProvider       Provider() const { return mProvider; }
			const std::string& ModelId() const  { return mModelId; }

			//! Append a user text message and reset the per-turn tool-round
			//! counter.  Any pending tool calls are flushed first (they
			//! belong to the previous assistant turn; unanswered ones get
			//! synthesized error results so the wire stays valid).
			//! EMPTY or whitespace-only text is a documented NO-OP
			//! (Anthropic hard-400s an empty text block): nothing is
			//! appended, flushed, or reset.
			void AddUserMessage( const std::string& text );

			//! Build the next HTTP request for the caller to perform.
			//! `apiKey` is forwarded to the codec for the auth header only
			//! -- it is not stored.  Pending tool calls are flushed into
			//! the transcript first (unanswered ones get synthesized error
			//! results).  If the transcript is empty there is nothing to
			//! send: the returned request is EMPTY (url == "") and must
			//! not be performed.
			ChatHttpRequest BuildRequest( const std::string& apiKey );

			//! Feed back the raw HTTP response (status + body).  On
			//! ToolCalls the assistant turn is recorded and the calls
			//! become the pending set AddToolResult answers; on FinalText
			//! the turn is recorded and the loop is idle; on ProviderError
			//! nothing is recorded.  Enforces the iteration cap.  REFUSED
			//! (ProviderError, nothing recorded) while the previous turn's
			//! tool calls are still pending -- resolve them via
			//! AddToolResult, or let BuildRequest / AddUserMessage flush
			//! them with synthesized error results.
			ChatStepResult HandleResponse( long httpStatus, const std::string& rawBody );

			//! Translate one tool call into the JSON-RPC request line the
			//! AgentRpcDispatcher consumes:
			//!   {"jsonrpc":"2.0","id":<rpcId>,"method":<name>,"params":<args>}
			//! Malformed argsJson degrades to empty params (for verbs with
			//! REQUIRED params the dispatcher then answers -32602, which
			//! flows back to the model as an error tool result; verbs whose
			//! params are all optional simply execute with their defaults --
			//! self-correcting either way, never throwing).
			std::string ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId ) const;

			//! Record the raw JSON-RPC response ENVELOPE line for one of
			//! the pending calls.  Results are buffered and packed into
			//! ONE provider-native user message when every pending call
			//! has been answered (or at the next BuildRequest /
			//! AddUserMessage, which synthesize error results for any
			//! still-unanswered calls).  A result whose call id is not in
			//! the pending set, or a second result for an already-answered
			//! id, is IGNORED (first result wins).
			void AddToolResult( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine );

			//! Transcript access for the GUI (role + display text; the raw
			//! provider-native JSON rides along).  An out-of-range index
			//! returns a reference to a static EMPTY entry (never throws)
			//! -- the same bounds-safe convention as JsonValue::at.
			std::size_t                TranscriptSize() const { return mTranscript.size(); }
			const ChatTranscriptEntry& TranscriptAt( std::size_t i ) const;

			//! Tool rounds consumed in the current conversation-turn.
			int ToolRoundCount() const { return mToolRounds; }

			//! Pending tool calls awaiting AddToolResult (empty when idle).
			const std::vector<ChatToolCall>& PendingToolCalls() const { return mPendingCalls; }

			//! The static co-editing system prompt sent on every request.
			//! (The BASE prompt only -- when a skill index is set via
			//! SetSkillIndex, BuildRequest appends the skills section to
			//! this base; SystemPrompt() itself never changes.)
			static const char* SystemPrompt();

			//! Facet 5 slice S1: set the "Available skills" section appended
			//! to the system prompt of every subsequent BuildRequest.
			//! `indexText` is a stable, human-readable rendering of the
			//! read_skill INDEX (one "name -- hook" line per skill); the
			//! DRIVER fetches it ONCE via the read_skill verb at panel init
			//! and passes the rendered text here -- this loop stays sans-IO
			//! (it never reads the skills itself).  An EMPTY string OMITS
			//! the section entirely (the base prompt is sent unchanged).
			//! The setting is provider-neutral config, like the provider /
			//! model selection: it survives Reset() and SetProvider().
			void SetSkillIndex( const std::string& indexText );

		private:
			AgentChatLoop( const AgentChatLoop& );             // deleted
			AgentChatLoop& operator=( const AgentChatLoop& );  // deleted

			//! Pack the pending turn's tool results into one transcript
			//! entry, in pending-call order, SYNTHESIZING an error result
			//! for every pending call that has no buffered result (so the
			//! wire never carries an unanswered tool call).  No-op when
			//! there is no pending turn.
			void FlushPendingToolResults();

			std::unique_ptr<IChatProviderCodec> mCodec;
			ChatProvider                        mProvider;
			std::string                         mModelId;

			//! Facet 5 slice S1: the rendered skills-index text ("" = no
			//! skills section).  Provider-neutral config -- survives Reset()
			//! and SetProvider(), like mProvider/mModelId.
			std::string                         mSkillIndexText;

			std::vector<ChatTranscriptEntry>    mTranscript;

			//! The tool calls of the last assistant turn + the results
			//! received so far (flushed into ONE user message).
			std::vector<ChatToolCall>                          mPendingCalls;
			std::vector<std::pair<ChatToolCall, std::string>>  mPendingResults;

			int mToolRounds;   //!< tool rounds in the current conversation-turn
		};
	}
}

#endif
