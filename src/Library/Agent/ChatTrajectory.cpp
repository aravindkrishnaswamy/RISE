//////////////////////////////////////////////////////////////////////
//
//  ChatTrajectory.cpp - trajectory schema + recorder + IO-edge helpers
//    (see ChatTrajectory.h for the contract).
//
//  NO LOGGING of bodies or keys anywhere in this file.  The recorder is
//  sans-IO; the only filesystem / clock / RNG touches are the clearly-
//  labelled IO-edge helpers at the bottom.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ChatTrajectory.h"

#include "Json.h"
#include "../Interfaces/ILog.h"   // GlobalLog(): loud-once logging on a broken trajectory sink

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <regex>

namespace RISE
{
	namespace Agent
	{
		//==============================================================
		// Hash + dotted_order.
		//==============================================================
		std::string TrajectoryHashHex( const std::string& s )
		{
			// FNV-1a 64-bit.
			uint64_t h = 14695981039346656037ULL;
			for( std::size_t i = 0; i < s.size(); ++i ) {
				h ^= static_cast<unsigned char>( s[i] );
				h *= 1099511628211ULL;
			}
			static const char* const kHex = "0123456789abcdef";
			std::string out( 16, '0' );
			for( int i = 15; i >= 0; --i ) {
				out[i] = kHex[h & 0xF];
				h >>= 4;
			}
			return out;
		}

		namespace
		{
			std::string ZeroPad( uint64_t v, int width )
			{
				std::string s = std::to_string( v );
				if( static_cast<int>( s.size() ) < width )
					s = std::string( width - static_cast<int>( s.size() ), '0' ) + s;
				return s;
			}
		}

		std::string TrajectoryDottedOrder( int64_t epochMs, uint64_t counter )
		{
			// A negative clock (pathological injected clock) clamps to 0 so
			// the zero-padded token stays a fixed 15 digits and sortable.
			uint64_t ms = epochMs < 0 ? 0ULL : static_cast<uint64_t>( epochMs );
			return ZeroPad( ms, 15 ) + "." + ZeroPad( counter, 9 );
		}

		//==============================================================
		// Serialization.
		//==============================================================
		namespace
		{
			JsonValue CommonObj( const std::string& traceId,
			                     const std::string& dottedOrder,
			                     const char* runType )
			{
				JsonValue o = JsonValue::MakeObject();
				o.set( "trace_id", JsonValue::MakeString( traceId ) );
				o.set( "dotted_order", JsonValue::MakeString( dottedOrder ) );
				o.set( "run_type", JsonValue::MakeString( runType ) );
				return o;
			}

			//! Embed a JSON-object literal string as a parsed object; fall
			//! back to an empty object when it does not parse to an object.
			JsonValue ParseObjOr( const std::string& objJson )
			{
				JsonValue v;
				std::string perr;
				if( JsonParse( objJson, v, perr ) && v.isObject() ) return v;
				return JsonValue::MakeObject();
			}

			//! Embed a JSON literal string as parsed JSON; fall back to a
			//! JSON string carrying the raw text when it does not parse.
			JsonValue EmbedJsonOrString( const std::string& text )
			{
				JsonValue v;
				std::string perr;
				if( JsonParse( text, v, perr ) ) return v;
				return JsonValue::MakeString( text );
			}

			JsonValue Num( double d ) { return JsonValue::MakeNumber( d ); }
		}

		std::string SerializeTrajectoryRecord( const TrajectorySessionRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "session" );
			o.set( "started_at", Num( static_cast<double>( r.startedAtMs ) ) );
			o.set( "provider", JsonValue::MakeString( r.provider ) );
			o.set( "gen_ai.request.model", JsonValue::MakeString( r.requestModel ) );
			o.set( "system_prompt", JsonValue::MakeString( r.systemPrompt ) );
			o.set( "system_prompt_hash", JsonValue::MakeString( r.systemPromptHash ) );
			o.set( "tool_defs_hash", JsonValue::MakeString( r.toolDefsHash ) );
			o.set( "scene_path", JsonValue::MakeString( r.scenePath ) );
			o.set( "scene_head_version", Num( static_cast<double>( r.sceneHeadVersion ) ) );
			return JsonSerialize( o );
		}

		std::string SerializeTrajectoryRecord( const TrajectoryUserRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "user" );
			o.set( "text", JsonValue::MakeString( r.text ) );
			o.set( "attachments", Num( static_cast<double>( r.attachments ) ) );
			return JsonSerialize( o );
		}

		std::string SerializeTrajectoryRecord( const TrajectoryLlmRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "llm" );
			o.set( "gen_ai.request.model", JsonValue::MakeString( r.requestModel ) );
			o.set( "gen_ai.response.model", JsonValue::MakeString( r.responseModel ) );
			o.set( "request_params", ParseObjOr( r.requestParamsJson ) );
			o.set( "request_headers", ParseObjOr( r.requestHeadersJson ) );
			o.set( "http.status", Num( static_cast<double>( r.httpStatus ) ) );
			o.set( "latency_ms", Num( static_cast<double>( r.latencyMs ) ) );
			JsonValue fr = JsonValue::MakeArray();
			for( std::size_t i = 0; i < r.finishReasons.size(); ++i )
				fr.push_back( JsonValue::MakeString( r.finishReasons[i] ) );
			o.set( "gen_ai.response.finish_reasons", fr );
			o.set( "gen_ai.usage.input_tokens", Num( static_cast<double>( r.inputTokens ) ) );
			o.set( "gen_ai.usage.output_tokens", Num( static_cast<double>( r.outputTokens ) ) );
			o.set( "gen_ai.usage.cache_read_input_tokens",
			       Num( static_cast<double>( r.cacheReadInputTokens ) ) );
			o.set( "gen_ai.usage.reasoning_output_tokens",
			       Num( static_cast<double>( r.reasoningOutputTokens ) ) );
			// Present ONLY on the anomaly, so a normal record gains no noise
			// and a reader can grep the flag directly.  The provider's own
			// pre-clamp count rides ALONGSIDE the flag: the clamp normalizes
			// the number a cost model uses, and this is the evidence of what
			// was normalized away (equal to the clamped value on every healthy
			// record, hence not emitted there).
			if( r.reasoningClamped ) {
				o.set( "gen_ai.usage.reasoning_clamped", JsonValue::MakeBool( true ) );
				o.set( "gen_ai.usage.reasoning_output_tokens_reported",
				       Num( static_cast<double>( r.reasoningOutputTokensReported ) ) );
			}
			if( !r.errorType.empty() )
				o.set( "error.type", JsonValue::MakeString( r.errorType ) );
			o.set( "attempt", Num( static_cast<double>( r.attempt ) ) );
			if( r.retryOf >= 0 )
				o.set( "retry_of", Num( static_cast<double>( r.retryOf ) ) );
			// The RAW body rides as a STRING so it round-trips byte-for-byte
			// (the replay payload must not be re-serialized).
			o.set( "response_body", JsonValue::MakeString( r.responseBody ) );
			// Present ONLY on an auxiliary round (see the field's doc) -- an
			// ordinary main-turn record omits the key entirely.
			if( !r.purpose.empty() )
				o.set( "purpose", JsonValue::MakeString( r.purpose ) );
			return JsonSerialize( o );
		}

		std::string SerializeTrajectoryRecord( const TrajectoryToolRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "tool" );
			o.set( "name", JsonValue::MakeString( r.name ) );
			o.set( "tool.call.id", JsonValue::MakeString( r.callId ) );
			o.set( "args", ParseObjOr( r.argsJson ) );
			o.set( "jsonrpc.request", EmbedJsonOrString( r.jsonRpcRequest ) );
			o.set( "jsonrpc.response", EmbedJsonOrString( r.jsonRpcResponse ) );
			o.set( "latency_ms", Num( static_cast<double>( r.latencyMs ) ) );
			o.set( "head_version_after", Num( static_cast<double>( r.headVersionAfter ) ) );
			o.set( "error", JsonValue::MakeBool( r.error ) );
			return JsonSerialize( o );
		}

		std::string SerializeTrajectoryRecord( const TrajectoryHistoryEditRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "history_edit" );
			o.set( "entry_index", Num( static_cast<double>( r.entryIndex ) ) );
			o.set( "before_bytes", Num( static_cast<double>( r.beforeBytes ) ) );
			o.set( "after_bytes", Num( static_cast<double>( r.afterBytes ) ) );
			o.set( "reason", JsonValue::MakeString( r.reason ) );
			return JsonSerialize( o );
		}

		std::string SerializeTrajectoryRecord( const TrajectorySummaryRecord& r,
			const std::string& traceId, const std::string& dottedOrder )
		{
			JsonValue o = CommonObj( traceId, dottedOrder, "summary" );
			o.set( "n_turns", Num( static_cast<double>( r.nTurns ) ) );
			o.set( "n_tool_calls", Num( static_cast<double>( r.nToolCalls ) ) );
			o.set( "gen_ai.usage.input_tokens", Num( static_cast<double>( r.totalInputTokens ) ) );
			o.set( "gen_ai.usage.output_tokens", Num( static_cast<double>( r.totalOutputTokens ) ) );
			o.set( "gen_ai.usage.reasoning_output_tokens",
			       Num( static_cast<double>( r.totalReasoningOutputTokens ) ) );
			// The rollup sums the CLAMPED subsets (it must, to stay a subset of
			// the output total), so a run in which every turn contradicted
			// itself is arithmetically indistinguishable from a healthy one.
			// This count is the distinguisher -- always present, 0 when clean.
			o.set( "gen_ai.usage.reasoning_clamped_turns",
			       Num( static_cast<double>( r.nReasoningClampedTurns ) ) );
			o.set( "gen_ai.usage.cache_read_input_tokens",
			       Num( static_cast<double>( r.totalCacheReadInputTokens ) ) );
			o.set( "total_latency_ms", Num( static_cast<double>( r.totalLatencyMs ) ) );
			o.set( "wall_ms", Num( static_cast<double>( r.wallMs ) ) );
			o.set( "status", JsonValue::MakeString( r.status ) );
			return JsonSerialize( o );
		}

		//==============================================================
		// Redaction (unconditional).
		//==============================================================
		std::string RedactTrajectoryLine( const std::string& line )
		{
			// Fixed api-key-shaped patterns.  Static so the (nontrivial)
			// regex compile happens once.  ECMAScript grammar (the default).
			static const std::regex kSk( "sk-[A-Za-z0-9_-]{8,}" );
			static const std::regex kAiza( "AIza[A-Za-z0-9_-]{10,}" );
			static const std::regex kBearer( "Bearer\\s+[A-Za-z0-9._-]{8,}",
			                                 std::regex::ECMAScript | std::regex::icase );
			static const std::string kRedacted = "[REDACTED]";

			std::string out = std::regex_replace( line, kBearer, kRedacted );
			out = std::regex_replace( out, kSk, kRedacted );
			out = std::regex_replace( out, kAiza, kRedacted );
			return out;
		}

		//==============================================================
		// Recorder.
		//==============================================================
		ChatTrajectoryRecorder::ChatTrajectoryRecorder(
			std::function<void(const std::string&)> sink,
			const ChatTrajectoryConfig& config ) :
			mSink( std::move( sink ) ),
			mClock( config.clock ),
			mTraceId( config.traceId.empty() ? MakeTrajectoryTraceId() : config.traceId ),
			mCounter( 0 ),
			mStartedAtMs( 0 ),
			mNTurns( 0 ),
			mNToolCalls( 0 ),
			mTotalInputTokens( 0 ),
			mTotalOutputTokens( 0 ),
			mTotalReasoningOutputTokens( 0 ),
			mNReasoningClampedTurns( 0 ),
			mTotalCacheReadInputTokens( 0 ),
			mTotalLatencyMs( 0 )
		{
		}

		int64_t ChatTrajectoryRecorder::Now() const
		{
			if( mClock ) return mClock();
			return TrajectoryNowMs();
		}

		std::string ChatTrajectoryRecorder::NextDottedOrder()
		{
			return TrajectoryDottedOrder( Now(), mCounter++ );
		}

		std::string ChatTrajectoryRecorder::Emit( const std::string& serializedLine )
		{
			const std::string redacted = RedactTrajectoryLine( serializedLine );
			// Review-round P2: the "recording never disrupts the chat"
			// contract must hold for ANY sink, not just the shipped
			// non-throwing file sink.  A throwing sink is swallowed and
			// recording is disabled for the rest of the session (a broken
			// sink stays broken -- throwing once per record would just be
			// noise on top of a lost trajectory).
			if( mSink ) {
				try {
					mSink( redacted );
				}
				catch( ... ) {
					mSink = nullptr;
				}
			}
			return redacted;
		}

		std::string ChatTrajectoryRecorder::EmitSession( const TrajectorySessionRecord& in )
		{
			TrajectorySessionRecord r = in;
			mStartedAtMs = ( r.startedAtMs != 0 ) ? r.startedAtMs : Now();
			r.startedAtMs = mStartedAtMs;
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		std::string ChatTrajectoryRecorder::EmitUser( const TrajectoryUserRecord& r )
		{
			++mNTurns;
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		std::string ChatTrajectoryRecorder::EmitLlm( const TrajectoryLlmRecord& r )
		{
			// A purpose-tagged (auxiliary) round is recorded in FULL on its
			// own line -- the accumulators below feed ONLY the summary's
			// rollup, which is documented (and consumed by the eval
			// harness) as the MAIN conversation's cost.  An auxiliary round
			// (e.g. the Mac GUI's prompt-triage pre-flight) never was part
			// of that conversation, so it must not inflate n_turns-adjacent
			// totals or make the summary line lie about how many main-turn
			// LLM rounds this session made.
			if( !r.purpose.empty() )
				return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
			if( r.inputTokens > 0 ) mTotalInputTokens += r.inputTokens;
			if( r.outputTokens > 0 ) mTotalOutputTokens += r.outputTokens;
			// The reasoning total tracks the SAME turns as the output total:
			// each record's reasoning is a subset of that record's output, so
			// the two roll up consistently (the run-level reasoning share is
			// totalReasoning / totalOutput).  The per-record invariant
			// (ChatUsage, enforced in the codecs) bounds each ADDEND; it says
			// nothing about these CROSS-TURN accumulators, which are plain
			// `long long +=`.  Reaching overflow needs ~9.2e6 turns each
			// saturated at the 1e12 per-count ceiling -- unreachable for a
			// chat session, and deliberately left unguarded rather than
			// carrying saturation machinery no run can exercise.
			if( r.reasoningOutputTokens > 0 ) mTotalReasoningOutputTokens += r.reasoningOutputTokens;
			// Counts TURNS, not tokens: the clamped magnitude is already lost
			// from the totals by construction, so the rollup reports how often
			// the anomaly fired and the per-record
			// gen_ai.usage.reasoning_output_tokens_reported carries the detail.
			if( r.reasoningClamped ) ++mNReasoningClampedTurns;
			if( r.cacheReadInputTokens > 0 ) mTotalCacheReadInputTokens += r.cacheReadInputTokens;
			if( r.latencyMs > 0 ) mTotalLatencyMs += r.latencyMs;
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		std::string ChatTrajectoryRecorder::EmitTool( const TrajectoryToolRecord& r )
		{
			++mNToolCalls;
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		std::string ChatTrajectoryRecorder::EmitHistoryEdit( const TrajectoryHistoryEditRecord& r )
		{
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		std::string ChatTrajectoryRecorder::EmitSummary( const std::string& status )
		{
			TrajectorySummaryRecord r;
			r.nTurns = mNTurns;
			r.nToolCalls = mNToolCalls;
			r.totalInputTokens = mTotalInputTokens;
			r.totalOutputTokens = mTotalOutputTokens;
			r.totalReasoningOutputTokens = mTotalReasoningOutputTokens;
			r.nReasoningClampedTurns = mNReasoningClampedTurns;
			r.totalCacheReadInputTokens = mTotalCacheReadInputTokens;
			r.totalLatencyMs = mTotalLatencyMs;
			r.wallMs = Now() - mStartedAtMs;
			if( r.wallMs < 0 ) r.wallMs = 0;
			r.status = status;
			return Emit( SerializeTrajectoryRecord( r, mTraceId, NextDottedOrder() ) );
		}

		//==============================================================
		// IO-edge helpers.
		//==============================================================
		int64_t TrajectoryNowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(
				system_clock::now().time_since_epoch() ).count();
		}

		std::string MakeTrajectoryTraceId()
		{
			// A random v4-shaped uuid.  std::random_device seeds an mt19937_64;
			// two 64-bit draws give the 128 bits.  Not crypto-strength -- a
			// session label, not a secret.
			std::random_device rd;
			std::mt19937_64 gen(
				( static_cast<uint64_t>( rd() ) << 32 ) ^ static_cast<uint64_t>( rd() ) ^
				static_cast<uint64_t>( TrajectoryNowMs() ) );
			uint64_t hi = gen();
			uint64_t lo = gen();
			unsigned char b[16];
			for( int i = 0; i < 8; ++i ) b[i]     = static_cast<unsigned char>( ( hi >> ( 8 * i ) ) & 0xFF );
			for( int i = 0; i < 8; ++i ) b[8 + i] = static_cast<unsigned char>( ( lo >> ( 8 * i ) ) & 0xFF );
			b[6] = static_cast<unsigned char>( ( b[6] & 0x0F ) | 0x40 );   // version 4
			b[8] = static_cast<unsigned char>( ( b[8] & 0x3F ) | 0x80 );   // variant
			static const char* const kHex = "0123456789abcdef";
			std::string out;
			out.reserve( 36 );
			for( int i = 0; i < 16; ++i ) {
				if( i == 4 || i == 6 || i == 8 || i == 10 ) out += '-';
				out += kHex[( b[i] >> 4 ) & 0xF];
				out += kHex[b[i] & 0xF];
			}
			return out;
		}

		std::function<void(const std::string&)> MakeTrajectoryFileSink( const std::string& path )
		{
			// Create parent directories once; open the file for append and
			// keep the handle in a shared_ptr the lambda co-owns.  A failed
			// create/open yields a no-op sink (recording never disrupts the
			// chat) -- but the failure is reported ONCE via the library
			// logger rather than swallowed outright, so a misconfigured or
			// unwritable trajectory directory is discoverable instead of
			// masquerading as "recording is on and nothing is wrong" (the
			// GUI toggle has no other way to surface this).  `loggedFailure`
			// is shared with the returned lambda so the guard covers BOTH
			// the create-directories failure below and a later open()
			// failure without double-logging.
			bool dirCreateFailed = false;
			std::string dirCreateReason;
			try {
				std::filesystem::path p( path );
				if( p.has_parent_path() )
					std::filesystem::create_directories( p.parent_path() );
			}
			catch( const std::exception& e ) {
				dirCreateFailed = true;
				dirCreateReason = e.what();
			}
			catch( ... ) {
				dirCreateFailed = true;
				dirCreateReason = "unknown filesystem error";
			}

			// Open LAZILY on the first line so a session that records nothing
			// leaves no empty file behind (the GUI attaches a sink at every
			// scene open / provider switch, most of which never chat).
			std::shared_ptr<std::ofstream> f = std::make_shared<std::ofstream>();
			std::shared_ptr<bool> loggedFailure = std::make_shared<bool>( false );
			std::string pathCopy = path;

			if( dirCreateFailed ) {
				*loggedFailure = true;   // the lazy open below would just repeat this
				GlobalLog()->PrintEx( eLog_Warning,
					"ChatTrajectory: failed to create trajectory directory for '%s' (%s); "
					"trajectory recording is disabled for this session.",
					pathCopy.c_str(), dirCreateReason.c_str() );
			}

			return [f, pathCopy, loggedFailure]( const std::string& line ) {
				if( !f->is_open() ) {
					f->open( pathCopy, std::ios::app | std::ios::binary );
					if( !f->is_open() ) {
						if( !*loggedFailure ) {
							*loggedFailure = true;
							GlobalLog()->PrintEx( eLog_Warning,
								"ChatTrajectory: failed to open trajectory file '%s' for append; "
								"trajectory recording is disabled for this session.",
								pathCopy.c_str() );
						}
						return;
					}
				}
				( *f ) << line << '\n';
				f->flush();
			};
		}

		void PruneTrajectoryDir( const std::string& dir, int maxFiles, long long maxBytes )
		{
			try {
				std::filesystem::path d( dir );
				if( !std::filesystem::is_directory( d ) ) return;

				struct Entry { std::filesystem::path path; std::filesystem::file_time_type mtime; long long size; };
				std::vector<Entry> entries;
				long long total = 0;
				for( const std::filesystem::directory_entry& e :
				     std::filesystem::directory_iterator( d ) ) {
					if( !e.is_regular_file() ) continue;
					if( e.path().extension() != ".jsonl" ) continue;
					Entry en;
					en.path = e.path();
					en.mtime = std::filesystem::last_write_time( e.path() );
					en.size = static_cast<long long>( std::filesystem::file_size( e.path() ) );
					total += en.size;
					entries.push_back( en );
				}

				// Oldest first.
				std::sort( entries.begin(), entries.end(),
					[]( const Entry& a, const Entry& b ) { return a.mtime < b.mtime; } );

				std::size_t idx = 0;
				while( idx < entries.size() &&
				       ( ( maxFiles > 0 && static_cast<int>( entries.size() - idx ) > maxFiles ) ||
				         ( maxBytes > 0 && total > maxBytes ) ) ) {
					std::error_code ec;
					std::filesystem::remove( entries[idx].path, ec );
					if( !ec ) total -= entries[idx].size;
					++idx;
				}
			}
			catch( ... ) {}
		}
	}
}
