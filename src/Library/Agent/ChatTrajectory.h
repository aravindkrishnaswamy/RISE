//////////////////////////////////////////////////////////////////////
//
//  ChatTrajectory.h - the trajectory schema + recorder for the LLM
//    scene-editing chat loop (Eval-harness, slice E1).
//
//    A TRAJECTORY is an append-only JSONL log of one chat session: the
//    system prompt + tool-definition fingerprint (once), every user
//    message, every LLM HTTP round (with the RAW response body -- the
//    replay payload -- plus token usage / timings / finish reasons /
//    errors), every tool call and its JSON-RPC result, the loop's
//    image-elision history rewrites, and a terminal summary line.  The
//    field names follow the OpenTelemetry GenAI vocabulary (dotted keys
//    like `gen_ai.request.model`, `gen_ai.usage.input_tokens`,
//    `gen_ai.response.finish_reasons`) with NO OTel SDK dependency --
//    they are just JSON keys serialized via the in-tree Json codec.
//
//    SANS-IO: ChatTrajectoryRecorder BUILDS records and hands each
//    serialized line to a host-supplied sink callback -- it performs no
//    file IO and reads no clock directly (the clock is injected).  The
//    IO-edge helpers at the bottom (MakeTrajectoryFileSink,
//    PruneTrajectoryDir, MakeTrajectoryTraceId, TrajectoryNowMs) DO
//    touch the filesystem / clock and live here so the Mac and Windows
//    GUI drivers share ONE implementation rather than forking it.
//
//    REDACTION is UNCONDITIONAL and happens at the serialization
//    boundary: every emitted line passes through RedactTrajectoryLine
//    (a fixed regex pass over api-key-shaped substrings) regardless of
//    any config.  The `llm` record additionally never carries auth
//    headers -- AgentChatLoop::RecordHttpRound strips them by name
//    BEFORE the record is built (belt); the regex pass is the
//    suspenders.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_CHATTRAJECTORY_
#define RISE_AGENT_CHATTRAJECTORY_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//----------------------------------------------------------------
		// Record structs.  Each is FLAT and self-contained: it carries
		// its kind-specific fields only; the recorder stamps the three
		// common fields (trace_id / dotted_order / run_type) at emit
		// time.  The Serialize* free functions below build the full JSON
		// object (common + specific), so tests can round-trip a record
		// without a recorder.
		//----------------------------------------------------------------

		//! run_type "session": the once-per-trajectory header.
		struct TrajectorySessionRecord
		{
			int64_t     startedAtMs = 0;        //!< session start (epoch-ms)
			std::string provider;               //!< "anthropic" / "gemini" / "openai"
			std::string requestModel;           //!< gen_ai.request.model
			std::string systemPrompt;           //!< the FULL system prompt text
			std::string systemPromptHash;       //!< hash of systemPrompt (hex)
			std::string toolDefsHash;           //!< hash of the tool-def fingerprint (hex)
			std::string scenePath;              //!< the scene path at session start ("" if unknown)
			long long   sceneHeadVersion = -1;  //!< scene head revision at start (-1 = unknown)
		};

		//! run_type "user": one user message.
		struct TrajectoryUserRecord
		{
			std::string text;
			int         attachments = 0;   //!< count of image attachments
		};

		//! run_type "llm": one HTTP round to the provider.  `responseBody`
		//! is the RAW body verbatim -- the replay payload.  `requestParams`
		//! and `requestHeaders` are pre-built JSON object strings (the
		//! headers already AUTH-STRIPPED by the loop).  Absent usage fields
		//! are -1.  `errorType` is empty on success.  `retryOf` is -1 for a
		//! first attempt (not a retry).
		struct TrajectoryLlmRecord
		{
			std::string requestModel;              //!< gen_ai.request.model
			std::string responseModel;             //!< gen_ai.response.model ("" if absent)
			std::string requestParamsJson  = "{}"; //!< a JSON object literal
			std::string requestHeadersJson = "{}"; //!< a JSON object literal (auth-stripped)
			long        httpStatus = 0;            //!< http.status
			int64_t     latencyMs = 0;             //!< measured round-trip latency
			std::vector<std::string> finishReasons;//!< gen_ai.response.finish_reasons[]
			long long   inputTokens = -1;          //!< gen_ai.usage.input_tokens
			long long   outputTokens = -1;         //!< gen_ai.usage.output_tokens
			long long   cacheReadInputTokens = -1; //!< gen_ai.usage.cache_read_input_tokens
			//! gen_ai.usage.reasoning_output_tokens -- the reasoning/thinking
			//! SUBSET of outputTokens (-1 = the provider reported none).
			//! outputTokens is the TOTAL billed generation on EVERY provider
			//! (ChatUsage normalizes the wire-level disagreement), so
			//! `outputTokens - reasoningOutputTokens` is the visible output
			//! and the two never double count.  Recording the split is the
			//! whole point of parsing it: for the providers whose output
			//! counter already includes reasoning (Anthropic, OpenAI), this
			//! field is the ONLY place the reasoning volume survives.
			long long   reasoningOutputTokens = -1;
			//! gen_ai.usage.reasoning_clamped -- emitted ONLY when true: the
			//! body claimed more reasoning than it billed as output, so the
			//! subset was clamped to the billed total.  A self-contradictory
			//! usage block (a mis-shaped gateway in front of a local model,
			//! say) is invisible otherwise -- the codec layer logs nothing.
			bool        reasoningClamped = false;
			//! gen_ai.usage.reasoning_output_tokens_reported -- emitted ONLY
			//! alongside the clamp flag, where it is the ONLY surviving copy of
			//! the provider's own count.  Equal to reasoningOutputTokens on
			//! every unclamped record, so emitting it there would be pure
			//! duplication; on a clamped one, without it the record cannot say
			//! whether a turn clamped to 51 had reported 52 or 362, and the
			//! size of that gap is the whole diagnostic.
			long long   reasoningOutputTokensReported = -1;
			std::string errorType;                 //!< error.type ("" when ok)
			int         attempt = 1;               //!< 1-based attempt counter
			int         retryOf = -1;              //!< index of the attempt this retries (-1 = none)
			std::string responseBody;              //!< the RAW response body (replay payload)
			//! Empty for an ordinary main-turn LLM round (the common case --
			//! nothing new here).  Non-empty marks this `llm` record as an
			//! AUXILIARY HTTP round the loop recorded on a driver's behalf
			//! that is NOT part of the main conversation -- e.g. "triage"
			//! for the Mac GUI's prompt-triage pre-flight
			//! (RecordAuxiliaryHttpRound).  Serialized as a `purpose` key
			//! ONLY when non-empty, so an ordinary record gains no noise.
			//! Doubles as the running-summary EXCLUSION key: see
			//! ChatTrajectoryRecorder::EmitLlm -- a purpose-tagged record's
			//! tokens/latency are recorded verbatim in ITS OWN line (full
			//! forensic detail) but are NOT folded into the summary's
			//! totals, because those totals are documented (and consumed by
			//! the eval harness) as the MAIN conversation's cost, and an
			//! auxiliary round was never part of it.
			std::string purpose;
		};

		//! run_type "tool": one dispatched tool call + its JSON-RPC result.
		struct TrajectoryToolRecord
		{
			std::string name;                  //!< the verb name
			std::string callId;                //!< tool.call.id
			std::string argsJson = "{}";       //!< the call arguments (JSON object literal)
			std::string jsonRpcRequest;        //!< the JSON-RPC request line
			std::string jsonRpcResponse;       //!< the JSON-RPC response envelope line
			int64_t     latencyMs = 0;         //!< dispatch latency (host clock deltas)
			long long   headVersionAfter = -1; //!< head revision the envelope reported (-1 = none)
			bool        error = false;         //!< the envelope carried a JSON-RPC error
		};

		//! run_type "history_edit": one loop-driven transcript rewrite (an
		//! image elision).  `reason` is a short machine tag.
		struct TrajectoryHistoryEditRecord
		{
			int         entryIndex = -1;   //!< which transcript entry was rewritten
			long long   beforeBytes = 0;   //!< rawJson byte size before the rewrite
			long long   afterBytes = 0;    //!< rawJson byte size after the rewrite
			std::string reason;            //!< "tool_image_elision" / "user_image_elision"
		};

		//! run_type "summary": the terminal one-line rollup.
		struct TrajectorySummaryRecord
		{
			int         nTurns = 0;
			int         nToolCalls = 0;
			long long   totalInputTokens = 0;
			long long   totalOutputTokens = 0;
			long long   totalReasoningOutputTokens = 0;   //!< the reasoning subset of totalOutputTokens (the CLAMPED values, matching totalOutputTokens)
			//! gen_ai.usage.reasoning_clamped_turns -- how many of this run's
			//! llm records had their reasoning subset clamped.  Without it the
			//! rollup cannot distinguish a healthy run from one where EVERY
			//! turn contradicted itself: totalReasoningOutputTokens sums the
			//! CLAMPED values (it has to -- it must stay a subset of
			//! totalOutputTokens), so a fully-clamped run looks like a
			//! perfectly ordinary one at this level.  Always emitted, 0 on a
			//! healthy run, so a reader never has to branch on key absence.
			int         nReasoningClampedTurns = 0;
			long long   totalCacheReadInputTokens = 0;
			int64_t     totalLatencyMs = 0;
			int64_t     wallMs = 0;
			std::string status;   //!< terminal status ("reset" / "provider_switch" / "closed" / ...)
		};

		//----------------------------------------------------------------
		// Serialization (sans-IO, testable in isolation).
		//----------------------------------------------------------------

		//! FNV-1a 64-bit hash of `s`, returned as a 16-char lowercase hex
		//! string.  Stable across runs and platforms (used for the system-
		//! prompt / tool-defs hashes).
		std::string TrajectoryHashHex( const std::string& s );

		//! Build a sortable dotted_order token: a 15-digit zero-padded
		//! epoch-ms followed by a '.' and a 9-digit zero-padded monotonic
		//! counter.  Lexicographic order == chronological+arrival order.
		std::string TrajectoryDottedOrder( int64_t epochMs, uint64_t counter );

		//! Serialize one record kind to a compact JSONL line, prefixing the
		//! three common fields (trace_id / dotted_order / run_type).  These
		//! do NOT redact -- the recorder redacts the returned line before it
		//! reaches a sink; call RedactTrajectoryLine yourself if you use
		//! these directly for output.
		std::string SerializeTrajectoryRecord( const TrajectorySessionRecord&,
			const std::string& traceId, const std::string& dottedOrder );
		std::string SerializeTrajectoryRecord( const TrajectoryUserRecord&,
			const std::string& traceId, const std::string& dottedOrder );
		std::string SerializeTrajectoryRecord( const TrajectoryLlmRecord&,
			const std::string& traceId, const std::string& dottedOrder );
		std::string SerializeTrajectoryRecord( const TrajectoryToolRecord&,
			const std::string& traceId, const std::string& dottedOrder );
		std::string SerializeTrajectoryRecord( const TrajectoryHistoryEditRecord&,
			const std::string& traceId, const std::string& dottedOrder );
		std::string SerializeTrajectoryRecord( const TrajectorySummaryRecord&,
			const std::string& traceId, const std::string& dottedOrder );

		//! UNCONDITIONAL redaction: replace every api-key-shaped substring
		//! (sk-..., AIza..., Bearer <token>) in `line` with "[REDACTED]".
		//! Applied to EVERY emitted line regardless of config.
		std::string RedactTrajectoryLine( const std::string& line );

		//----------------------------------------------------------------
		// The recorder (sans-IO: builds records, redacts, hands lines to a
		// sink; reads its clock via an injected callback).
		//----------------------------------------------------------------

		//! Construction config for a recorder.  `clock` returns epoch-ms;
		//! when empty the recorder falls back to TrajectoryNowMs (the ONE
		//! spot the sans-IO core touches a real clock -- justified as the
		//! IO edge, mirroring how the sink is host-supplied).  `traceId`
		//! empty => the recorder generates one.
		struct ChatTrajectoryConfig
		{
			std::string              traceId;
			std::function<int64_t()> clock;
			std::string              scenePath;
			long long                sceneHeadVersion = -1;
		};

		class ChatTrajectoryRecorder
		{
		public:
			ChatTrajectoryRecorder( std::function<void(const std::string&)> sink,
			                        const ChatTrajectoryConfig& config );

			const std::string& TraceId() const { return mTraceId; }

			//! Emit each record kind: stamp common fields, serialize,
			//! REDACT, hand to the sink, and update the running summary
			//! accumulators.  Each returns the exact (redacted) line
			//! emitted (so tests can assert on it without a separate sink).
			std::string EmitSession( const TrajectorySessionRecord& );
			std::string EmitUser( const TrajectoryUserRecord& );
			std::string EmitLlm( const TrajectoryLlmRecord& );
			std::string EmitTool( const TrajectoryToolRecord& );
			std::string EmitHistoryEdit( const TrajectoryHistoryEditRecord& );

			//! Emit the terminal summary from the running accumulators with
			//! the given status; wall_ms is clock()-startedAt.
			std::string EmitSummary( const std::string& status );

			//! Running totals (also surfaced for tests).
			int NTurns() const { return mNTurns; }
			int NToolCalls() const { return mNToolCalls; }

		private:
			ChatTrajectoryRecorder( const ChatTrajectoryRecorder& );
			ChatTrajectoryRecorder& operator=( const ChatTrajectoryRecorder& );

			int64_t     Now() const;
			std::string NextDottedOrder();
			std::string Emit( const std::string& serializedLine );

			std::function<void(const std::string&)> mSink;
			std::function<int64_t()>                mClock;
			std::string mTraceId;
			uint64_t    mCounter;

			int64_t   mStartedAtMs;
			int       mNTurns;
			int       mNToolCalls;
			long long mTotalInputTokens;
			long long mTotalOutputTokens;
			long long mTotalReasoningOutputTokens;
			int       mNReasoningClampedTurns;
			long long mTotalCacheReadInputTokens;
			int64_t   mTotalLatencyMs;
		};

		//----------------------------------------------------------------
		// IO-edge helpers (filesystem + clock; shared by both GUI drivers).
		//----------------------------------------------------------------

		//! Epoch milliseconds from the system clock (the real-clock default
		//! a recorder uses when its config.clock is empty).
		int64_t TrajectoryNowMs();

		//! A fresh RFC-4122-shaped trace id (random v4).  Not
		//! cryptographically strong -- just a unique session label.
		std::string MakeTrajectoryTraceId();

		//! A sink that APPENDS each line (plus a '\n') to `path`, creating
		//! parent directories as needed.  The file handle is shared across
		//! calls (opened lazily on first line); a write failure is silently
		//! dropped (trajectory recording never disrupts the chat).
		std::function<void(const std::string&)> MakeTrajectoryFileSink( const std::string& path );

		//! Rotation: in directory `dir`, keep at most `maxFiles` newest
		//! *.jsonl files AND at most `maxBytes` total, pruning oldest-first
		//! by modification time until both caps hold.  Missing dir / IO
		//! errors are no-ops.  Implemented ONCE here so Mac and Windows do
		//! not fork the policy.
		void PruneTrajectoryDir( const std::string& dir, int maxFiles, long long maxBytes );
	}
}

#endif
