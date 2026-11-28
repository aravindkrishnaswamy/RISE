//////////////////////////////////////////////////////////////////////
//
//  AgentChatLoop.h - provider-agnostic, sans-IO chat-loop core for LLM
//    scene editing.
//
//    Owns conversation state and translates between provider wire formats
//    (AgentChatCodecs) and JSON-RPC verbs (AgentRpcDispatcher) without I/O.
//    Caller drives HTTP + tool dispatch:
//      AddUserMessage -> BuildRequest -> (HTTP) -> RecordHttpRound ->
//      HandleResponse -> [GateRefusalResponse / HandleLine -> AddToolResult]*.
//
//    Key contracts:
//      * Provider switch resets transcript: assistant turns are stored and
//        echoed as raw provider-native JSON (preserving opaque fields such
//        as thinking signatures) and cannot cross providers.
//      * Key hygiene: API key is passed per BuildRequest for the auth header
//        only, never stored or logged.
//      * Tool-call pairing invariant: all tool results for one assistant turn
//        flush into a single user message; unanswered pending calls get
//        synthesized error results. Combined with codec record-or-refuse,
//        every recorded tool call is answered immediately on the wire.
//        Reset()/SetProvider() recovers from unreplayable exotic provider echoes.
//      * Fixed system prompt: ComposeSystemPrompt() is a pure function of
//        (base prompt, skill index, override) so provider prompt-cache prefixes
//        remain byte-identical across turns.
//      * Sequencing gate: AddToolResult tracks consecutive document-mutating
//        calls without a visual observation (`mBlindEditStreak`). Once it
//        reaches `BlindEditGateThreshold()`, `GateRefusalResponse` returns a
//        normal JSON-RPC refusal envelope; hosts (eval runner, macOS/Windows
//        chat drivers) MUST check it pre-dispatch and feed refusals directly
//        to AddToolResult. (MCP/loopback callers have no chat loop.)
//      * Retention & context lifetimes (unconditional, deterministic,
//        ToolResults/User loop-generated entries only; assistant rawJson is
//        never rewritten):
//          - Tool-result images: only the newest PNG stays live; older ones
//            are elided via RewriteElidedImages (leaving render stats intact).
//          - User reference images: up to kMaxLiveUserImages (4) stay live;
//            oldest attachments elide first via RewriteElidedUserImages.
//          - Superseded reads: ChatToolResultSupersessionKey keeps only the
//            latest copy per key (e.g. read_document, list_proposals, same-name
//            read_skill) via RewriteElidedToolResults.
//          - Context lifetimes (docs/agentic-redesign/91-context-lifetimes-and-visible-detail.md):
//            successful finish_element retires that element's build/insert
//            results to immutable-fact stubs (cancelled by reopen_element before
//            application); entering compose phase retires prior named read_skill
//            results once per plan (re-armed by file_build_plan).
//      * Caller guards & caps: HandleResponse refuses while calls are pending;
//        unknown/duplicate AddToolResult ids are ignored; empty transcript
//        yields empty BuildRequest (url == ""); per-turn round cap prevents spin.
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
		//! Selectable LLM providers (XAI and Local reuse OpenAIChatCodec with
		//! provider-specific Config).
		enum class ChatProvider
		{
			Anthropic,
			Gemini,
			OpenAI,
			XAI,
			Local
		};

		//! Display-only summary of one tool call on a ToolResults entry
		//! (never read by BuildRequest or EstimateContextTokens).
		struct ChatToolDisplaySummary
		{
			std::string name;         //!< JSON-RPC verb name
			std::string outcomeLine;  //!< deterministic one-line outcome summary
			std::string argsJson;     //!< call arguments JSON

			//! Raw JSON-RPC response line capped at 8 KB for GUI inspection.
			//! Replaced with an elision placeholder when `carriesImage` is
			//! superseded or cleared by ElideAllLiveImages.
			std::string resultJson;

			//! True while `resultJson` holds a live base64 image payload.
			bool        carriesImage = false;
		};

		//! Wire-bearing per-result retention/lifetime state for a ToolResults
		//! entry. Parallel in index order to PackToolResults' output (used by
		//! RewriteElidedToolResults); kept separate from display-only toolSummaries.
		struct ChatToolResultSlot
		{
			std::string supersessionKey;     //!< from ChatToolResultSupersessionKey ("" if not supersedable)
			std::string verb;                //!< answered verb name (for lifetime stubs)
			std::string lifetimeElement;     //!< build-plan element described by build_element / insert_chunks, or ""
			std::string lifetimeCarry;       //!< compact non-document outcomes (rejections, notices) preserved in L2 stub
			std::string lifetimeFacts;       //!< compact factual metadata (bbox, head version) preserved in L2 stub
			std::string lifetimeSkill;       //!< skill name for successful named read_skill (L3b compose retirement), or ""
			std::string pendingRetireNote;   //!< decided lifetime stub awaiting next flush rewrite
			std::string pendingRetireReason; //!< trajectory history_edit reason for pendingRetireNote
			bool        live = true;         //!< false once rewritten to a placeholder/stub (idempotent)
		};

		//! One transcript entry (`rawJson` rides the wire; `displayText` is for UI).
		struct ChatTranscriptEntry
		{
			enum class Role
			{
				User,         //!< user message
				Assistant,    //!< model turn (text and/or tool calls)
				ToolResults,  //!< packed tool results for one assistant turn
				//! Loop-injected content (currently malformed-tool-call correctives).
				//! Wire-identical to User (built via MakeUserEntry), rendered by GUIs
				//! as a dim system-notice row, and NOT a compaction span boundary.
				DriverNote
			};

			Role        role = Role::User;
			std::string displayText;
			std::string rawJson;             //!< provider-native message JSON (verbatim for Assistant)
			bool        carriesLiveImage = false;  //!< ToolResults only: true while image block/part is live
			int         liveUserImageCount = 0;    //!< User only: count of still-live reference attachments
			std::size_t imageContentBytes = 0;     //!< live base64 bytes excluded from text-proxy token estimate

			//! DISPLAY-ONLY enrichment (`reasoningText` and `toolSummaries`):
			//! never read by BuildRequest or EstimateContextTokens, and never
			//! re-serialized to any provider (provider reasoning stays verbatim
			//! in `rawJson`). `toolResultSlots` below is WIRE-BEARING and must
			//! remain separate from `toolSummaries`.
			std::string reasoningText;                         //!< Assistant only: extracted reasoning text, or ""
			std::vector<ChatToolDisplaySummary> toolSummaries; //!< ToolResults only: per-call UI summaries
			std::vector<ChatToolResultSlot> toolResultSlots;   //!< ToolResults only: wire-bearing elision/lifetime slots
		};

		//! Single-threaded, sans-IO chat loop (see file header).
		class AgentChatLoop
		{
		public:
			//! Anti-spin per-turn tool-round defaults (hosted vs. local).
			static const int kMaxToolRoundsPerTurn = 100;
			static const int kMaxToolRoundsPerTurnLocal = 1000;

			//! Pin an explicit per-turn tool-round cap (>= 1) that survives
			//! SetProvider() (for hosts enforcing their own run budgets).
			void SetMaxToolRoundsPerTurn( int cap )
			{
				if( cap >= 1 ) {
					mMaxToolRoundsPerTurn = cap;
					mToolRoundsCapExplicit = true;
				}
			}

			int MaxToolRoundsPerTurn() const { return mMaxToolRoundsPerTurn; }

			//! Default consecutive blind-mutation threshold before
			//! GateRefusalResponse refuses further edits until a visual observe.
			static const int kDefaultBlindEditGateThreshold = 10;

			//! Configure sequencing gate threshold (<= 0 disables).
			void SetBlindEditGateThreshold( int threshold ) { mBlindEditGateThreshold = threshold; }
			int BlindEditGateThreshold() const { return mBlindEditGateThreshold; }

			//! Max live user-attached reference images across the conversation.
			static const int kMaxLiveUserImages = 4;

			//! Minimum trailing User-rooted spans retained by CompactTranscript.
			static const int kMinRetainedSpans = 2;

			//! Durable document snapshot decimation thresholds for large scenes.
			static const long long kDocumentSnapshotDecimationBytes = 1024 * 1024;   // 1 MB
			static const int kDocumentSnapshotDecimationStride = 10;

			//! Shared GUI default compaction budget (chars/4 text proxy + flat
			//! image charge; off by default until SetContextBudget is called).
			//! Span compaction drops whole User-rooted spans and retains >=
			//! kMinRetainedSpans, so it only fires on >= 3 user turns (single-turn
			//! growth is handled by superseded-read/lifetime elision). Note:
			//! acts as a hosted backstop; small local `num_ctx` servers truncate
			//! earlier.
			static const std::size_t kDefaultContextBudgetHighTokens = 150000;
			static const std::size_t kDefaultContextBudgetLowTokens  = 75000;

			//! Constructs with ChatProvider::OpenAI and its default model.
			AgentChatLoop();
			~AgentChatLoop();

			//! Clear transcript, pending tool state, and round counter (keeps config).
			void Reset();

			//! Select provider (+ optional model id; empty = default). Always resets
			//! transcript (raw assistant entries are provider-specific).
			void SetProvider( ChatProvider provider, const std::string& modelId = std::string() );

			ChatProvider       Provider() const { return mProvider; }
			const std::string& ModelId() const  { return mModelId; }

			//! Append user text, flushing pending tool calls first and resetting the
			//! turn round counter / blind-edit streak. Blank text is a no-op.
			void AddUserMessage( const std::string& text );

			//! Append user text + reference-image attachments (no-op iff both empty).
			//! Elides oldest live user images across the transcript past kMaxLiveUserImages.
			void AddUserMessage( const std::string& text,
			                     const std::vector<ChatAttachment>& attachments );

			//! Flush pending tool calls, run compaction/elision if active, and build
			//! the next HTTP request (`apiKey` used for auth header only). Returns
			//! an empty request (`url == ""`) if transcript is empty.
			ChatHttpRequest BuildRequest( const std::string& apiKey );

			//! Parse raw HTTP response, enforce iteration cap, and record valid turns.
			//! Refuses (ProviderError, nothing recorded) while tool calls are pending.
			ChatStepResult HandleResponse( long httpStatus, const std::string& rawBody );

			//! Translate a tool call into a JSON-RPC request line (malformed argsJson
			//! degrades to `{}`). Non-const: stamps dispatch start time when a
			//! trajectory sink is attached.
			std::string ToolCallToJsonRpcLine( const ChatToolCall& call, int rpcId );

			//! Pre-dispatch sequencing gate check. Returns a ready-to-feed JSON-RPC
			//! refusal envelope when `call` is a document mutation at/past
			//! BlindEditGateThreshold() (caller MUST skip dispatch and pass it straight
			//! to AddToolResult), or "" if allowed. Marks `call.id` so AddToolResult
			//! does not advance the blind-edit streak on the refusal.
			std::string GateRefusalResponse( const ChatToolCall& call, int rpcId );

			//! Record one pending tool call's JSON-RPC response line (unknown or
			//! duplicate ids ignored). Flushes all results into one entry when complete.
			void AddToolResult( const ChatToolCall& call, const std::string& rawJsonRpcResponseLine );

			//! Bounds-safe transcript inspection (out-of-range returns static empty entry).
			std::size_t                TranscriptSize() const { return mTranscript.size(); }
			const ChatTranscriptEntry& TranscriptAt( std::size_t i ) const;

			int ToolRoundCount() const { return mToolRounds; }
			const std::vector<ChatToolCall>& PendingToolCalls() const { return mPendingCalls; }

			//! Base co-editing system prompt (excluding any appended skill index).
			static const char* SystemPrompt();

			//! Stateless display-only formatter matching ChatToolDisplaySummary::outcomeLine
			//! (used by GUI async render paths before transcript flush).
			static std::string ToolOutcomeLineForDisplay( const ChatToolCall& call,
			                                               const std::string& rawJsonRpcResponseLine );

			//! Set rendered read_skill index appended to system prompt (empty omits).
			//! Provider-neutral config: survives Reset() and SetProvider().
			void SetSkillIndex( const std::string& indexText );

			//! Override the entire composed system prompt verbatim (for auxiliary
			//! single-purpose loops such as prompt triage; "" clears override).
			//! Survives Reset() and SetProvider().
			void SetSystemPromptOverride( const std::string& prompt );

			//! Configure context-compaction high/low token watermarks (high == 0
			//! disables). Survives Reset() and SetProvider().
			void SetContextBudget( std::size_t highWaterTokens, std::size_t lowWaterTokens );

			//! Pure deterministic token estimate (system + tools + image-discounted transcript).
			std::size_t EstimateContextTokens() const;

			//! True iff budget window is active and EstimateContextTokens() >= high water.
			bool WouldCompactNow() const
			{
				return ContextBudgetActive() &&
				       EstimateContextTokens() >= mContextBudgetHighTokens;
			}

			std::size_t ContextBudgetHigh() const { return mContextBudgetHighTokens; }
			std::size_t ContextBudgetLow() const { return mContextBudgetLowTokens; }

			//! Number of transcript rows (entries, not turns) dropped by compaction
			//! in the current conversation; cleared on Reset/SetProvider.
			std::size_t CompactedEntryCount() const { return mCompactedEntryCount; }

			//! Monotone count and latest text of Role::DriverNote entries injected
			//! this conversation (unperturbed by compaction; cleared on Reset/SetProvider).
			std::size_t DriverNoteCount() const { return mDriverNoteCount; }
			const std::string& LastDriverNoteText() const { return mLastDriverNoteText; }

			//==========================================================
			// Trajectory recording & durable document snapshots.
			//==========================================================

			//! Attach/replace a JSONL trajectory sink (empty detaches). Emits the
			//! session record lazily on first action; closes any active session first.
			void SetTrajectorySink( std::function<void(const std::string&)> sink,
			                        const ChatTrajectoryConfig& config = ChatTrajectoryConfig() );

			//! Install a callback returning (documentText, uuid, revision) for durable
			//! `document_snapshot` trajectory records (`head_bump` on advancing tool
			//! results with large-scene decimation, plus deduped `session_end` on close).
			//! Intended wrapper: AgentSession::ReadDocumentSnapshot on the agent-RPC thread.
			void SetDocumentSnapshotProvider(
				std::function<bool( std::string& outText, uint64_t& outUuid, uint64_t& outRevision )> provider );

			//! Record one LLM HTTP round just before HandleResponse (auth headers
			//! stripped; verbatim response stored for replay). No-op without a sink.
			void RecordHttpRound( const ChatHttpRequest& request, long httpStatus,
			                      const std::string& rawBody, int64_t elapsedMs,
			                      int attempt = 1, int retryOf = -1 );

			//! Overload reusing the last BuildRequest's ChatHttpRequest.
			void RecordHttpRound( long httpStatus, const std::string& rawBody,
			                      int64_t elapsedMs, int attempt = 1, int retryOf = -1 );

			//! Emit terminal `summary` record and roll to a fresh trace id.
			void FinishTrajectory( const std::string& status );

			bool TrajectoryActive() const { return mRecorder != nullptr; }

			//! Record an out-of-band auxiliary HTTP round (e.g. prompt triage) into
			//! this session's trajectory tagged with `purpose`. Pure trajectory write
			//! (untouches transcript/budgets and excluded from summary totals);
			//! `requestBodySansAuth` must already exclude credentials.
			void RecordAuxiliaryHttpRound( const std::string& purpose, const std::string& url,
			                               const std::string& requestBodySansAuth,
			                               long httpStatus, const std::string& responseBody,
			                               int64_t elapsedMs );

			//! Record a non-LLM/non-tool history event (`history_edit`) in the trajectory.
			void RecordHistoryEdit( const std::string& reason, long long beforeBytes = 0,
			                        long long afterBytes = 0, int entryIndex = -1,
			                        const std::string& text = std::string() );

		private:
			AgentChatLoop( const AgentChatLoop& );             // deleted
			AgentChatLoop& operator=( const AgentChatLoop& );  // deleted

			void FlushPendingToolResults();
			std::string ComposeSystemPrompt() const;

			//! Shared budget-validity guard for WouldCompactNow() and CompactTranscript().
			bool ContextBudgetActive() const
			{
				return mContextBudgetHighTokens > 0 && mContextBudgetLowTokens > 0 &&
				       mContextBudgetLowTokens < mContextBudgetHighTokens;
			}

			//! Strip all live tool and user images across the transcript (idempotent).
			void ElideAllLiveImages();

			//! Rewrite older live slots sharing a supersession key to placeholders.
			void ElideSupersededToolResults();

			struct ToolResultSlotRewrite
			{
				std::size_t entry = 0;   //!< mTranscript index (ToolResults entry)
				std::size_t slot = 0;    //!< index into entry's toolResultSlots
				std::string note;        //!< placeholder text
			};

			//! Shared slot-rewrite pass for supersession and lifetime retirements.
			void RewriteToolResultSlots( const std::vector<ToolResultSlotRewrite>& targets,
			                             const char* reason, const char* noteKey );

			//! Doc 91 L2/L3b lifetime retirement pass (never rewrites `newEntry` itself).
			void RetireToolResultsByLifetime( std::size_t newEntry,
			                                  const std::vector<std::pair<ChatToolCall, std::string>>& ordered );

			//! Drop oldest whole User-rooted spans down to low-water or kMinRetainedSpans.
			void CompactTranscript();

			void EnsureSessionRecordEmitted();
			void CloseTrajectorySession( const std::string& status );
			void MaybeEmitHeadBumpSnapshot( uint64_t uuid, uint64_t revision );
			void WriteDocumentSnapshotRecord( const std::string& reason,
				const std::string& text, uint64_t uuid, uint64_t revision );

			struct ToolLinePending
			{
				std::string id;
				std::string line;
				int64_t     startMs;
			};

			std::unique_ptr<IChatProviderCodec> mCodec;
			ChatProvider                        mProvider;
			std::string                         mModelId;
			std::string                         mSkillIndexText;         //!< survives Reset/SetProvider
			std::string                         mSystemPromptOverride;   //!< survives Reset/SetProvider
			std::size_t                         mContextBudgetHighTokens;
			std::size_t                         mContextBudgetLowTokens;
			std::size_t                         mCompactedEntryCount = 0;
			std::vector<ChatTranscriptEntry>    mTranscript;

			std::vector<ChatToolCall>                          mPendingCalls;
			std::vector<std::pair<ChatToolCall, std::string>>  mPendingResults;

			int mToolRounds;
			int mMaxToolRoundsPerTurn = kMaxToolRoundsPerTurn;
			bool mToolRoundsCapExplicit = false;        //!< true if pinned via SetMaxToolRoundsPerTurn
			int mBlindEditStreak = 0;                  //!< consecutive mutations since last visual observe
			int mBlindEditGateThreshold = kDefaultBlindEditGateThreshold;
			std::vector<std::string> mGateRefusedCallIds; //!< consumed by AddToolResult to skip streak advance

			std::size_t mDriverNoteCount = 0;
			std::string mLastDriverNoteText;

			// Trajectory & durable snapshot state (inert when mRecorder is null).
			std::unique_ptr<ChatTrajectoryRecorder> mRecorder;
			bool                                     mSessionEmitted;
			std::function<void(const std::string&)>  mTrajectorySink;
			ChatTrajectoryConfig                     mTrajectoryConfig;
			std::function<int64_t()>                 mTrajectoryClock;
			ChatHttpRequest                          mLastRequest;
			std::vector<ToolLinePending>             mToolLineStash;

			std::function<bool( std::string&, uint64_t&, uint64_t& )> mDocumentSnapshotProvider;
			bool     mHasLastSnapshotVersion = false;
			uint64_t mLastSnapshotUuid = 0;
			uint64_t mLastSnapshotRevision = 0;
			long long mLastSnapshotDocumentBytes = 0;
			int mSnapshotAdvanceCount = 0;

			bool                                     mElideAllImages;           //!< sticky after text-only model rejects multimodal 400
			bool mComposeSkillRetirementDone = false;                           //!< once per plan; re-armed by file_build_plan
			bool                                     mForceReasoningEffortNone; //!< sticky after Chat-Completions reasoning_effort 400
			int                                      mMalformedRetries = 0;     //!< corrective retries used in current user turn
			std::string                              mPendingCorrective;        //!< queued corrective user text for next BuildRequest
			bool                                     mAppendingCorrective = false; //!< suppresses turn/retry counter reset during injection

		public:
			//! Corrective retries allowed per user turn after ChatErrorKind::MalformedToolCall.
			static const int kMaxMalformedToolCallRetries = 2;
		};
	}
}

#endif
