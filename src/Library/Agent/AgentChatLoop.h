//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.h - the provider-agnostic, sans-IO chat-loop core for
//    the LLM scene-editing chat panel (Facet 5, the agentic surface --
//    slice B1).
//
//    OWNS conversation state and translates between LLM provider wire
//    formats (via AgentChatCodecs) and the six JSON-RPC verbs the
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
//        pending call that has no buffered result, so the wire
//        invariant "every tool call is answered in the immediately-
//        following user message" holds BY CONSTRUCTION on both
//        providers (Anthropic hard-400s unanswered tool_use ids;
//        Gemini rejects mismatched functionResponses) -- a user
//        interrupt or tool crash cannot poison the transcript.
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
			//! Malformed argsJson degrades to empty params (the dispatcher
			//! then answers -32602, which flows back to the model as an
			//! error tool result -- self-correcting, never throwing).
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
			//! provider-native JSON rides along).
			std::size_t                TranscriptSize() const { return mTranscript.size(); }
			const ChatTranscriptEntry& TranscriptAt( std::size_t i ) const;

			//! Tool rounds consumed in the current conversation-turn.
			int ToolRoundCount() const { return mToolRounds; }

			//! Pending tool calls awaiting AddToolResult (empty when idle).
			const std::vector<ChatToolCall>& PendingToolCalls() const { return mPendingCalls; }

			//! The static co-editing system prompt sent on every request.
			static const char* SystemPrompt();

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
