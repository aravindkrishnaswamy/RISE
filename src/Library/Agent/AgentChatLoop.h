//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.h - the provider-agnostic, sans-IO chat-loop core for
//    the LLM scene-editing chat panel (Facet 5, the agentic surface --
//    slice B1).
//
//    OWNS conversation state and translates between LLM provider wire
//    formats (via AgentChatCodecs) and the ten JSON-RPC verbs the
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
//      * USER IMAGE RETENTION (Model-B F5 chat image attachments): a
//        SEPARATE, INDEPENDENT policy from the tool-result IMAGE
//        RETENTION rule above -- user reference images are the
//        modeling TARGET (the agent keeps comparing against them
//        across turns), not a disposable render preview, so "keep only
//        the newest" is the wrong rule here.  Instead: up to
//        kMaxLiveUserImages (4) user-attached images stay live
//        UN-ELIDED across the WHOLE conversation.  AddUserMessage's
//        attachments overload elides the OLDEST already-live user
//        image(s) -- across every earlier User entry, oldest entry
//        first, oldest attachment within an entry first -- just enough
//        to make room so the running live-image count never exceeds
//        the cap once the new message's own images are added (an
//        attachment that itself would still exceed the cap alone is
//        simply added anyway -- the cap bounds STEADY-STATE growth, it
//        never refuses an attach).  An elided attachment is replaced
//        with the text placeholder "[reference image elided --
//        re-attach if needed]" via the codec's RewriteElidedUserImages.
//        Rewriting is legal ONLY for User entries this loop generated
//        via MakeUserEntry; assistant entries are never touched.  The
//        cap bounds per-turn token cost (a 1024px-edge JPEG runs
//        ~100-400KB base64, re-sent on EVERY later request since the
//        whole history replays) while keeping enough recent references
//        live for a multi-turn modeling session to compare against.
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
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AgentChatCodecs.h"
#include "ChatTrajectory.h"

namespace RISE
{
	namespace Agent
	{
		//! The selectable providers (each backed by a codec).  XAI and
		//! Local both reuse the OpenAIChatCodec (OpenAI-Chat-Completions-
		//! compatible wire shape) with a different Config -- see MakeCodec.
		enum class ChatProvider
		{
			Anthropic,
			Gemini,
			OpenAI,
			XAI,
			Local
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

			//! Number of still-LIVE user-attached reference images this
			//! User entry carries (see USER IMAGE RETENTION in the file
			//! header) -- a SEPARATE counter from carriesLiveImage, which
			//! only ever applies to ToolResults entries.  Decremented (via
			//! the codec's RewriteElidedUserImages) as the running cap
			//! elides the oldest live attachments first.  Always 0 for
			//! Assistant/ToolResults entries and for a User entry with no
			//! attachments.
			int         liveUserImageCount = 0;
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

			//! Maximum user-attached reference images kept LIVE (un-
			//! elided) across the WHOLE conversation -- see USER IMAGE
			//! RETENTION in the file header.  Attaching beyond this cap
			//! elides the oldest live one(s) first.
			static const int kMaxLiveUserImages = 4;

			//! Constructs with the ChatGPT/OpenAI provider + its default model.
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

			//! Append a user message carrying reference-image attachments
			//! (Model-B F5 chat image attachments) alongside the text --
			//! see USER IMAGE RETENTION in the file header for the
			//! persistence + cap policy.  `text` may be empty when
			//! `attachments` is non-empty (an attachment-only message);
			//! the documented NO-OP rule above still holds when BOTH are
			//! empty.  After appending, elides the oldest live user
			//! image(s) across the transcript so the running live count
			//! never exceeds kMaxLiveUserImages.
			void AddUserMessage( const std::string& text,
			                     const std::vector<ChatAttachment>& attachments );

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
			//! NOTE (Eval-harness E1): NON-const.  When a trajectory sink is
			//! attached this STAMPS the tool call's dispatch start time and
			//! the JSON-RPC line, keyed by call id, so AddToolResult can
			//! complete the `tool` trajectory record with a measured latency.
			//! With no sink attached it is a pure translation (the stamp is
			//! skipped) -- behaviour is byte-identical to the pre-E1 const
			//! version.
			std::string ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId );

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

			//==========================================================
			// Eval-harness E1: trajectory recording.
			//==========================================================

			//! Attach (or replace) a trajectory sink: every subsequent user
			//! message, LLM round, tool call, and image-elision rewrite is
			//! recorded as a JSONL line handed to `sink`.  `config` supplies
			//! the trace id (empty => generated), an optional epoch-ms clock
			//! (empty => a real clock at the IO edge), and the scene
			//! path/head version captured in the session record.  The
			//! session record is emitted lazily on the first recorded action
			//! (so it always leads, and captures the skill index set by
			//! then).  Passing an EMPTY sink detaches recording.  With no
			//! sink attached EVERY hook below is a cheap no-op -- the loop's
			//! non-recording behaviour is byte-identical to the pre-E1 core.
			//! Replacing an active session emits its summary ("replaced")
			//! first.
			void SetTrajectorySink( std::function<void(const std::string&)> sink,
			                        const ChatTrajectoryConfig& config = ChatTrajectoryConfig() );

			//! Record one LLM HTTP round into the `llm` trajectory record.
			//! Called by the DRIVER just before HandleResponse (the driver
			//! owns the HTTP round-trip, so it alone knows the status/body/
			//! latency).  The response body is stored VERBATIM (the replay
			//! payload); usage/finish-reasons/error are derived from it via
			//! the codec.  AUTH HEADERS ARE STRIPPED from `request` by name
			//! before the record is built (belt); the writer's regex pass is
			//! the suspenders.  `attempt`/`retryOf` capture driver-initiated
			//! retries (retryOf < 0 for a first attempt).  No-op when no sink
			//! is attached.
			void RecordHttpRound( const ChatHttpRequest& request, long httpStatus,
			                      const std::string& rawBody, int64_t elapsedMs,
			                      int attempt = 1, int retryOf = -1 );

			//! Convenience overload using the request most recently returned
			//! by BuildRequest (what the driver actually sent) -- so a GUI
			//! driver need not marshal the request back into C++.  No-op when
			//! no sink is attached or no request has been built yet.
			void RecordHttpRound( long httpStatus, const std::string& rawBody,
			                      int64_t elapsedMs, int attempt = 1, int retryOf = -1 );

			//! Emit the terminal `summary` record with `status` and detach the
			//! current session (a fresh trace id / new session begins on the
			//! next recorded action).  No-op when no session is active.  Use
			//! this at chat end / app close for a clean summary line; a Reset
			//! or SetProvider also closes the session automatically.
			void FinishTrajectory( const std::string& status );

			//! True while a trajectory sink is attached.
			bool TrajectoryActive() const { return mRecorder != nullptr; }

		private:
			AgentChatLoop( const AgentChatLoop& );             // deleted
			AgentChatLoop& operator=( const AgentChatLoop& );  // deleted

			//! Pack the pending turn's tool results into one transcript
			//! entry, in pending-call order, SYNTHESIZING an error result
			//! for every pending call that has no buffered result (so the
			//! wire never carries an unanswered tool call).  No-op when
			//! there is no pending turn.
			void FlushPendingToolResults();

			//! Compose the full system prompt (base + skills section when a
			//! skill index is set) -- shared by BuildRequest and the session
			//! record so the recorded prompt matches the sent prompt.
			std::string ComposeSystemPrompt() const;

			//! TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: strip EVERY live
			//! image from the WHOLE transcript in one sweep -- both packed
			//! read_image tool-result images (via the codec's
			//! RewriteElidedImages) and live user reference-image
			//! attachments (via RewriteElidedUserImages), recording a
			//! history_edit for each rewritten entry.  Idempotent (a second
			//! sweep finds nothing live and is a no-op).  Invoked when a
			//! text-only model 400-rejects multimodal content, and again at
			//! the top of every later BuildRequest while mElideAllImages is
			//! set, so once a model proves text-only the conversation never
			//! re-sends an image.
			void ElideAllLiveImages();

			//! Emit the `session` record exactly once, on the first recorded
			//! action, so it always leads the trajectory.  No-op with no sink.
			void EnsureSessionRecordEmitted();

			//! Emit the terminal `summary` (when a session is active) and roll
			//! the recorder to a fresh trace id on the same sink, so the next
			//! action starts a new session.  Shared by Reset / SetProvider /
			//! FinishTrajectory.
			void CloseTrajectorySession( const std::string& status );

			//! One in-flight tool dispatch: the JSON-RPC line + start time
			//! stamped by ToolCallToJsonRpcLine, completed at AddToolResult.
			struct ToolLinePending
			{
				std::string id;
				std::string line;
				int64_t     startMs;
			};

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

			//! TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY: sticky once a
			//! text-only model 400-rejects multimodal content.  While set,
			//! every BuildRequest strips images from the transcript, so the
			//! session never re-sends an image to a model that proved it has
			//! no vision.  Doubles as the once-per-round retry guard (a
			//! second multimodal 400 finds it already true and does NOT
			//! retry again).  Cleared by Reset() (a new session may target a
			//! different, image-capable model).

			//! Eval-harness E1 trajectory state (all inert when mRecorder is
			//! null -- the non-recording path is byte-identical to pre-E1).
			std::unique_ptr<ChatTrajectoryRecorder> mRecorder;
			bool                                     mSessionEmitted;
			std::function<void(const std::string&)>  mTrajectorySink;
			ChatTrajectoryConfig                     mTrajectoryConfig;
			std::function<int64_t()>                 mTrajectoryClock;
			ChatHttpRequest                          mLastRequest;
			std::vector<ToolLinePending>             mToolLineStash;

			//! See the "TEXT-ONLY-MODEL IMAGE-REJECTION RECOVERY" note above
			//! mToolRounds.
			bool                                     mElideAllImages;
		};
	}
}

#endif
