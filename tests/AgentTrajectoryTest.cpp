//////////////////////////////////////////////////////////////////////
//
//  AgentTrajectoryTest.cpp - Eval-harness slice E1: the chat trajectory
//    schema + recorder + loop hooks + codec ParseUsage + redacting
//    writer.
//
//  Drives the WHOLE recording path with NO network and NO LLM: canned
//  provider JSON fixtures play the model's side and canned JSON-RPC
//  envelopes play the dispatcher, so the loop's trajectory hooks are
//  exercised deterministically.
//
//  Coverage:
//    E1  Schema round-trip: every record kind serializes to parseable
//        JSON carrying trace_id / dotted_order / run_type + its
//        kind-specific fields.
//    E2  dotted_order: fixed 15.9 zero-padded shape, lexicographic ==
//        chronological, and STRICTLY MONOTONIC across a recorder's
//        emissions (guaranteed by the counter even at a frozen clock).
//    E3  ParseUsage on all three codecs, from realistic usage blocks;
//        absent + malformed bodies degrade to -1 (never throw).
//    E4  Full canned session: the record SEQUENCE (session, user, llm,
//        tool, llm, summary), the tool record carrying the JSON-RPC
//        line + envelope + post-call headVersion, the llm record's
//        VERBATIM raw response body + usage + finish reasons, and the
//        summary TOTALS matching the per-record values.
//    E5  Image elision -> history_edit records (user-image and
//        tool-image elision paths, with before/after byte sizes).
//    E6  REDACTION RED-PROVE: a session whose auth header carries the
//        fake key AND whose user message embeds the same key -> the
//        trajectory NEVER contains the key (header path stripped; body
//        occurrence regex-redacted) and NO auth header NAME appears.
//    E7  Rotation: PruneTrajectoryDir keeps the newest N / <= bytes,
//        pruning oldest-first.
//    E8  RecordAuxiliaryHttpRound -- the Mac GUI prompt-triage HTTP
//        round-trip recorded into the MAIN session's trajectory as a
//        purpose-tagged `llm` record, with state isolation (touches
//        nothing else) and its own redaction red-prove.
//    E9  Durable document snapshots (AgentChatLoop::
//        SetDocumentSnapshotProvider): schema round-trip (folded into
//        E1), the write policy (one head_bump snapshot per advancing
//        headVersion, none for a non-advancing one), decimation past
//        1MB (only every 10th advance snapshots), the close-time
//        session_end snapshot's dedupe (skipped when the head has not
//        moved since the last snapshot; fires with the CURRENT head
//        when it has), and the no-provider no-op (zero snapshot
//        records, ordinary record shape unaffected).  Closes the
//        product gap where a scene built entirely inside a live GUI
//        agent session -- never Save As'd -- was unrecoverable if the
//        app quit or crashed.
//
//  RED-PROVE evidence (development-time, reverted): (a) with
//  StripAuthHeaders neutered to a pass-through, E6's "no auth header
//  name in any line" assertions FAIL (the "x-api-key" name leaks into
//  the llm record's request_headers, its value still [REDACTED] by the
//  regex) -- proving the header-name strip is load-bearing and distinct
//  from the value regex.  (b) With RedactTrajectoryLine returning its
//  input unchanged, E6's in-body "[REDACTED] present / raw key absent"
//  assertions FAIL -- proving the unconditional regex pass.  Both were
//  restored.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/ChatTrajectory.h"
#include "../src/Library/Agent/AgentChatLoop.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/Json.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static JsonValue Parse( const std::string& s )
{
	JsonValue v;
	std::string err;
	if( !JsonParse( s, v, err ) ) return JsonValue::MakeNull();
	return v;
}

static const char* const kApiKey = "sk-test-SECRETKEY-1234567890abcd";

// A monotonic injected clock (epoch-ms), advancing 5ms per call.
static std::function<int64_t()> MakeCounterClock( int64_t start )
{
	std::shared_ptr<int64_t> p = std::make_shared<int64_t>( start );
	return [p]() { int64_t v = *p; *p += 5; return v; };
}

//----------------------------------------------------------------------
// E1: schema round-trip.
//----------------------------------------------------------------------
static void TestSchemaRoundTrip()
{
	std::printf( "E1: schema round-trip (all record kinds)...\n" );
	const std::string tid = "trace-1234";
	const std::string dot = "000000000000123.000000001";

	{
		TrajectorySessionRecord r;
		r.startedAtMs = 1700000000000LL;
		r.provider = "anthropic";
		r.requestModel = "claude-sonnet-5";
		r.systemPrompt = "You are an agent.";
		r.systemPromptHash = "abc";
		r.toolDefsHash = "def";
		r.scenePath = "/s.RISEscene";
		r.sceneHeadVersion = 4;
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "session", "session run_type" );
		Check( j.get( "trace_id" ).asString() == tid, "session trace_id" );
		Check( j.get( "dotted_order" ).asString() == dot, "session dotted_order" );
		Check( j.get( "provider" ).asString() == "anthropic", "session provider" );
		Check( j.get( "gen_ai.request.model" ).asString() == "claude-sonnet-5", "session request model" );
		Check( j.get( "system_prompt" ).asString() == "You are an agent.", "session system prompt full text" );
		Check( j.get( "system_prompt_hash" ).asString() == "abc", "session prompt hash" );
		Check( j.get( "tool_defs_hash" ).asString() == "def", "session tool-defs hash" );
		Check( j.get( "scene_head_version" ).asNumber() == 4.0, "session head version" );
	}
	{
		TrajectoryUserRecord r;
		r.text = "hello \"world\"\nline2";
		r.attachments = 2;
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "user", "user run_type" );
		Check( j.get( "text" ).asString() == "hello \"world\"\nline2", "user text round-trips escapes" );
		Check( j.get( "attachments" ).asNumber() == 2.0, "user attachments" );
	}
	{
		TrajectoryLlmRecord r;
		r.requestModel = "m-req";
		r.responseModel = "m-resp";
		r.requestParamsJson = "{\"max_tokens\":16000}";
		r.requestHeadersJson = "{\"content-type\":\"application/json\"}";
		r.httpStatus = 200;
		r.latencyMs = 321;
		r.finishReasons.push_back( "tool_calls" );
		r.inputTokens = 128; r.outputTokens = 64; r.cacheReadInputTokens = 10;
		r.errorType = "";
		r.attempt = 2; r.retryOf = 0;
		r.responseBody = "{\"raw\":true}";
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "llm", "llm run_type" );
		Check( j.get( "gen_ai.request.model" ).asString() == "m-req", "llm request model" );
		Check( j.get( "gen_ai.response.model" ).asString() == "m-resp", "llm response model" );
		Check( j.get( "request_params" ).get( "max_tokens" ).asNumber() == 16000.0, "llm params embedded as object" );
		Check( j.get( "request_headers" ).get( "content-type" ).asString() == "application/json", "llm headers object" );
		Check( j.get( "http.status" ).asNumber() == 200.0, "llm http.status" );
		Check( j.get( "latency_ms" ).asNumber() == 321.0, "llm latency" );
		Check( j.get( "gen_ai.response.finish_reasons" ).at( 0 ).asString() == "tool_calls", "llm finish reasons" );
		Check( j.get( "gen_ai.usage.input_tokens" ).asNumber() == 128.0, "llm input tokens" );
		Check( j.get( "gen_ai.usage.output_tokens" ).asNumber() == 64.0, "llm output tokens" );
		Check( j.get( "gen_ai.usage.cache_read_input_tokens" ).asNumber() == 10.0, "llm cache tokens" );
		Check( !j.has( "error.type" ), "llm omits error.type on success" );
		Check( j.get( "attempt" ).asNumber() == 2.0, "llm attempt" );
		Check( j.get( "retry_of" ).asNumber() == 0.0, "llm retry_of when >= 0" );
		Check( j.get( "response_body" ).asString() == "{\"raw\":true}", "llm raw body verbatim as string" );
	}
	{
		TrajectoryLlmRecord r;   // error path
		r.errorType = "max_tokens";
		r.retryOf = -1;
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "error.type" ).asString() == "max_tokens", "llm error.type present when failed" );
		Check( !j.has( "retry_of" ), "llm omits retry_of for a first attempt" );
	}
	{
		TrajectoryToolRecord r;
		r.name = "propose_patch";
		r.callId = "toolu_1";
		r.argsJson = "{\"target\":\"x\"}";
		r.jsonRpcRequest = "{\"method\":\"propose_patch\"}";
		r.jsonRpcResponse = "{\"result\":{\"applied\":true}}";
		r.latencyMs = 12;
		r.headVersionAfter = 7;
		r.error = false;
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "tool", "tool run_type" );
		Check( j.get( "name" ).asString() == "propose_patch", "tool name" );
		Check( j.get( "tool.call.id" ).asString() == "toolu_1", "tool call id" );
		Check( j.get( "args" ).get( "target" ).asString() == "x", "tool args embedded object" );
		Check( j.get( "jsonrpc.request" ).get( "method" ).asString() == "propose_patch", "tool jsonrpc request" );
		Check( j.get( "jsonrpc.response" ).get( "result" ).get( "applied" ).asBool(), "tool jsonrpc response" );
		Check( j.get( "head_version_after" ).asNumber() == 7.0, "tool head version after" );
		Check( j.get( "error" ).asBool() == false, "tool error flag" );
	}
	{
		TrajectoryHistoryEditRecord r;
		r.entryIndex = 3; r.beforeBytes = 1000; r.afterBytes = 40;
		r.reason = "tool_image_elision";
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "history_edit", "history_edit run_type" );
		Check( j.get( "entry_index" ).asNumber() == 3.0, "history_edit entry index" );
		Check( j.get( "before_bytes" ).asNumber() == 1000.0, "history_edit before bytes" );
		Check( j.get( "after_bytes" ).asNumber() == 40.0, "history_edit after bytes" );
		Check( j.get( "reason" ).asString() == "tool_image_elision", "history_edit reason" );
	}
	{
		// E9 prep: TrajectoryDocumentSnapshotRecord round-trips every field.
		TrajectoryDocumentSnapshotRecord r;
		r.reason = "head_bump";
		r.headVersionUuid = 42;
		r.headVersionRevision = 7;
		r.documentText = "RISE ASCII SCENE 7\n{\n}\n";
		r.documentBytes = static_cast<long long>( r.documentText.size() );
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "document_snapshot", "document_snapshot run_type" );
		Check( j.get( "trace_id" ).asString() == tid, "document_snapshot trace_id" );
		Check( j.get( "dotted_order" ).asString() == dot, "document_snapshot dotted_order" );
		Check( j.get( "reason" ).asString() == "head_bump", "document_snapshot reason" );
		Check( j.get( "head_version_uuid" ).asNumber() == 42.0, "document_snapshot head_version_uuid" );
		Check( j.get( "head_version_revision" ).asNumber() == 7.0, "document_snapshot head_version_revision" );
		Check( j.get( "document_text" ).asString() == r.documentText,
		       "document_snapshot document_text round-trips verbatim" );
		Check( j.get( "document_bytes" ).asNumber() == static_cast<double>( r.documentText.size() ),
		       "document_snapshot document_bytes matches the text length" );
	}
	{
		TrajectorySummaryRecord r;
		r.nTurns = 2; r.nToolCalls = 5;
		r.totalInputTokens = 178; r.totalOutputTokens = 74; r.totalCacheReadInputTokens = 10;
		r.totalLatencyMs = 333; r.wallMs = 9000;
		r.status = "closed";
		JsonValue j = Parse( SerializeTrajectoryRecord( r, tid, dot ) );
		Check( j.get( "run_type" ).asString() == "summary", "summary run_type" );
		Check( j.get( "n_turns" ).asNumber() == 2.0, "summary n_turns" );
		Check( j.get( "n_tool_calls" ).asNumber() == 5.0, "summary n_tool_calls" );
		Check( j.get( "gen_ai.usage.input_tokens" ).asNumber() == 178.0, "summary input total" );
		Check( j.get( "total_latency_ms" ).asNumber() == 333.0, "summary latency total" );
		Check( j.get( "wall_ms" ).asNumber() == 9000.0, "summary wall ms" );
		Check( j.get( "status" ).asString() == "closed", "summary status" );
	}
}

//----------------------------------------------------------------------
// E2: dotted_order shape + sortability + monotonicity.
//----------------------------------------------------------------------
static void TestDottedOrder()
{
	std::printf( "E2: dotted_order sortability + monotonicity...\n" );
	const std::string a = TrajectoryDottedOrder( 1000, 0 );
	const std::string b = TrajectoryDottedOrder( 1000, 1 );
	const std::string c = TrajectoryDottedOrder( 2000, 0 );
	Check( a.size() == 25, "dotted_order is 15 + '.' + 9 chars" );
	Check( a == "000000000001000.000000000", "dotted_order zero-pads epoch-ms + counter" );
	Check( a < b, "same ms, later counter sorts after" );
	Check( b < c, "later ms sorts after regardless of counter" );

	// Recorder emissions are strictly monotonic even at a FROZEN clock.
	std::shared_ptr<std::vector<std::string> > lines =
		std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };
	ChatTrajectoryConfig cfg;
	cfg.traceId = "t";
	cfg.clock = []() { return static_cast<int64_t>( 5000 ); };   // frozen
	ChatTrajectoryRecorder rec( sink, cfg );
	rec.EmitSession( TrajectorySessionRecord() );
	rec.EmitUser( TrajectoryUserRecord() );
	rec.EmitUser( TrajectoryUserRecord() );
	Check( lines->size() == 3, "three lines emitted" );
	std::string prev;
	bool mono = true;
	for( std::size_t i = 0; i < lines->size(); ++i ) {
		const std::string d = Parse( ( *lines )[i] ).get( "dotted_order" ).asString();
		if( i > 0 && !( prev < d ) ) mono = false;
		prev = d;
	}
	Check( mono, "recorder dotted_orders strictly increase at a frozen clock" );
}

//----------------------------------------------------------------------
// E3: ParseUsage on all three codecs.
//----------------------------------------------------------------------
static void TestParseUsage()
{
	std::printf( "E3: ParseUsage (anthropic / gemini / openai)...\n" );
	{
		AnthropicChatCodec codec;
		ChatUsage u = codec.ParseUsage(
			"{\"usage\":{\"input_tokens\":128,\"output_tokens\":64,\"cache_read_input_tokens\":10}}" );
		Check( u.inputTokens == 128, "anthropic input_tokens" );
		Check( u.outputTokens == 64, "anthropic output_tokens" );
		Check( u.cacheReadInputTokens == 10, "anthropic cache_read_input_tokens" );
		// The realistic fixture shape (no cache field) leaves cache at -1.
		ChatUsage u2 = codec.ParseUsage( "{\"usage\":{\"input_tokens\":128,\"output_tokens\":64}}" );
		Check( u2.cacheReadInputTokens == -1, "anthropic absent cache -> -1" );
	}
	{
		GeminiChatCodec codec;
		ChatUsage u = codec.ParseUsage(
			"{\"usageMetadata\":{\"promptTokenCount\":64,\"candidatesTokenCount\":32,\"cachedContentTokenCount\":7}}" );
		Check( u.inputTokens == 64, "gemini promptTokenCount" );
		Check( u.outputTokens == 32, "gemini candidatesTokenCount" );
		Check( u.cacheReadInputTokens == 7, "gemini cachedContentTokenCount" );
	}
	{
		OpenAIChatCodec codec;
		ChatUsage u = codec.ParseUsage(
			"{\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":50,\"prompt_tokens_details\":{\"cached_tokens\":20}}}" );
		Check( u.inputTokens == 100, "openai prompt_tokens" );
		Check( u.outputTokens == 50, "openai completion_tokens" );
		Check( u.cacheReadInputTokens == 20, "openai prompt_tokens_details.cached_tokens" );
	}
	{
		AnthropicChatCodec codec;
		ChatUsage u = codec.ParseUsage( "{\"no_usage\":true}" );
		Check( u.inputTokens == -1 && u.outputTokens == -1 && u.cacheReadInputTokens == -1,
		       "absent usage block -> all -1" );
		ChatUsage bad = codec.ParseUsage( "not json at all" );
		Check( bad.inputTokens == -1 && bad.outputTokens == -1,
		       "malformed body -> all -1 (never throws)" );
	}
}

//----------------------------------------------------------------------
// E4: full canned session -> the complete record sequence.
//----------------------------------------------------------------------
static void TestFullSession()
{
	std::printf( "E4: full canned session trajectory...\n" );

	std::shared_ptr<std::vector<std::string> > lines =
		std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };

	AgentChatLoop loop;
	loop.SetProvider( ChatProvider::Anthropic );
	ChatTrajectoryConfig cfg;
	cfg.traceId = "trace-session";
	cfg.clock = MakeCounterClock( 1000 );
	cfg.scenePath = "/scenes/x.RISEscene";
	cfg.sceneHeadVersion = 3;
	loop.SetTrajectorySink( sink, cfg );

	loop.AddUserMessage( "Make it red" );

	const std::string body1 =
		"{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
		"\"content\":[{\"type\":\"text\",\"text\":\"Editing.\"},"
		"{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"propose_patch\","
		"\"input\":{\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"1 0 0\"}}],"
		"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
		"\"usage\":{\"input_tokens\":128,\"output_tokens\":64,\"cache_read_input_tokens\":10}}";
	const std::string envelope =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"applied\":true,\"status\":\"applied\","
		"\"headVersion\":{\"uuid\":\"u\",\"revision\":7}}}";
	const std::string body2 =
		"{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
		"\"content\":[{\"type\":\"text\",\"text\":\"Done.\"}],"
		"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,"
		"\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";

	// Round 1.
	loop.BuildRequest( kApiKey );
	loop.RecordHttpRound( 200, body1, 111 );
	ChatStepResult st = loop.HandleResponse( 200, body1 );
	Check( st.kind == ChatStepResult::Kind::ToolCalls && st.toolCalls.size() == 1,
	       "round1 -> one tool call" );
	if( st.toolCalls.size() == 1 ) {
		const std::string line = loop.ToolCallToJsonRpcLine( st.toolCalls[0], 1 );
		loop.AddToolResult( st.toolCalls[0], envelope );
	}
	// Round 2.
	loop.BuildRequest( kApiKey );
	loop.RecordHttpRound( 200, body2, 222 );
	ChatStepResult fin = loop.HandleResponse( 200, body2 );
	Check( fin.kind == ChatStepResult::Kind::FinalText, "round2 -> final text" );

	loop.FinishTrajectory( "closed" );

	Check( lines->size() == 6, "six records emitted (session,user,llm,tool,llm,summary)" );
	if( lines->size() != 6 ) {
		for( std::size_t i = 0; i < lines->size(); ++i )
			std::printf( "    [%zu] %s\n", i, ( *lines )[i].c_str() );
		return;
	}

	std::vector<JsonValue> j;
	for( std::size_t i = 0; i < lines->size(); ++i ) j.push_back( Parse( ( *lines )[i] ) );

	Check( j[0].get( "run_type" ).asString() == "session", "record 0 is session" );
	Check( j[1].get( "run_type" ).asString() == "user", "record 1 is user" );
	Check( j[2].get( "run_type" ).asString() == "llm", "record 2 is llm" );
	Check( j[3].get( "run_type" ).asString() == "tool", "record 3 is tool" );
	Check( j[4].get( "run_type" ).asString() == "llm", "record 4 is llm" );
	Check( j[5].get( "run_type" ).asString() == "summary", "record 5 is summary" );

	// Every record carries the same trace id.
	bool sameTrace = true;
	for( std::size_t i = 0; i < j.size(); ++i )
		if( j[i].get( "trace_id" ).asString() != "trace-session" ) sameTrace = false;
	Check( sameTrace, "all records share the session trace id" );

	// dotted_order strictly increasing across the whole trajectory.
	bool mono = true; std::string prev;
	for( std::size_t i = 0; i < j.size(); ++i ) {
		const std::string d = j[i].get( "dotted_order" ).asString();
		if( i > 0 && !( prev < d ) ) mono = false;
		prev = d;
	}
	Check( mono, "trajectory dotted_orders strictly increase" );

	// Session captures the FULL system prompt + hashes + scene meta.
	Check( j[0].get( "provider" ).asString() == "anthropic", "session provider is anthropic" );
	Check( j[0].get( "gen_ai.request.model" ).asString() == "claude-sonnet-5", "session model" );
	Check( j[0].get( "system_prompt" ).asString().find( "CO-EDIT" ) != std::string::npos ||
	       j[0].get( "system_prompt" ).asString().find( "co-edit" ) != std::string::npos,
	       "session carries the FULL system prompt text" );
	Check( j[0].get( "system_prompt_hash" ).asString().size() == 16, "session prompt hash is 16 hex" );
	Check( j[0].get( "tool_defs_hash" ).asString().size() == 16, "session tool-defs hash is 16 hex" );
	Check( j[0].get( "scene_path" ).asString() == "/scenes/x.RISEscene", "session scene path" );
	Check( j[0].get( "scene_head_version" ).asNumber() == 3.0, "session scene head version" );

	// User.
	Check( j[1].get( "text" ).asString() == "Make it red", "user text captured" );

	// llm round 1: verbatim body, usage, finish reasons, params, headers.
	Check( j[2].get( "http.status" ).asNumber() == 200.0, "llm1 status 200" );
	Check( j[2].get( "latency_ms" ).asNumber() == 111.0, "llm1 latency 111" );
	Check( j[2].get( "response_body" ).asString() == body1, "llm1 raw body is verbatim" );
	Check( j[2].get( "gen_ai.usage.input_tokens" ).asNumber() == 128.0, "llm1 usage input 128" );
	Check( j[2].get( "gen_ai.usage.output_tokens" ).asNumber() == 64.0, "llm1 usage output 64" );
	Check( j[2].get( "gen_ai.usage.cache_read_input_tokens" ).asNumber() == 10.0, "llm1 usage cache 10" );
	Check( j[2].get( "gen_ai.response.finish_reasons" ).at( 0 ).asString() == "tool_calls",
	       "llm1 finish reason tool_calls" );
	Check( j[2].get( "gen_ai.response.model" ).asString() == "claude-sonnet-5", "llm1 response model" );
	Check( j[2].get( "request_params" ).get( "model" ).asString() == "claude-sonnet-5",
	       "llm1 request params carry the model" );
	Check( j[2].get( "request_params" ).has( "max_tokens" ), "llm1 params carry max_tokens" );
	Check( !j[2].get( "request_params" ).has( "messages" ), "llm1 params exclude the messages array" );
	Check( !j[2].get( "request_params" ).has( "tools" ), "llm1 params exclude the tools array" );
	Check( !j[2].get( "request_headers" ).has( "x-api-key" ), "llm1 request headers carry NO x-api-key" );

	// tool record.
	Check( j[3].get( "name" ).asString() == "propose_patch", "tool name propose_patch" );
	Check( j[3].get( "tool.call.id" ).asString() == "toolu_1", "tool call id toolu_1" );
	Check( j[3].get( "args" ).get( "target" ).asString() == "pnt_albedo", "tool args target" );
	Check( j[3].get( "jsonrpc.request" ).get( "method" ).asString() == "propose_patch",
	       "tool jsonrpc.request carries the method line" );
	Check( j[3].get( "jsonrpc.response" ).get( "result" ).get( "applied" ).asBool(),
	       "tool jsonrpc.response carries the envelope" );
	Check( j[3].get( "head_version_after" ).asNumber() == 7.0, "tool head_version_after 7" );
	Check( j[3].get( "error" ).asBool() == false, "tool error flag false" );
	Check( j[3].get( "latency_ms" ).asNumber() >= 0.0, "tool latency non-negative" );

	// llm round 2.
	Check( j[4].get( "latency_ms" ).asNumber() == 222.0, "llm2 latency 222" );
	Check( j[4].get( "gen_ai.response.finish_reasons" ).at( 0 ).asString() == "stop",
	       "llm2 finish reason stop" );
	Check( j[4].get( "gen_ai.usage.cache_read_input_tokens" ).asNumber() == -1.0,
	       "llm2 absent cache token -> -1" );

	// Summary totals match the per-record values.
	Check( j[5].get( "n_turns" ).asNumber() == 1.0, "summary n_turns 1" );
	Check( j[5].get( "n_tool_calls" ).asNumber() == 1.0, "summary n_tool_calls 1" );
	Check( j[5].get( "gen_ai.usage.input_tokens" ).asNumber() == 178.0, "summary input total 128+50" );
	Check( j[5].get( "gen_ai.usage.output_tokens" ).asNumber() == 74.0, "summary output total 64+10" );
	Check( j[5].get( "gen_ai.usage.cache_read_input_tokens" ).asNumber() == 10.0, "summary cache total 10" );
	Check( j[5].get( "total_latency_ms" ).asNumber() == 333.0, "summary latency total 111+222" );
	Check( j[5].get( "status" ).asString() == "closed", "summary status closed" );
}

//----------------------------------------------------------------------
// E5: image elision -> history_edit records.
//----------------------------------------------------------------------
static void TestHistoryEditRecords()
{
	std::printf( "E5: image elision -> history_edit records...\n" );

	// --- user-image elision ---
	{
		std::shared_ptr<std::vector<std::string> > lines =
			std::make_shared<std::vector<std::string> >();
		std::function<void(const std::string&)> sink =
			[lines]( const std::string& l ) { lines->push_back( l ); };

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-uimg";
		cfg.clock = MakeCounterClock( 1 );
		loop.SetTrajectorySink( sink, cfg );

		std::vector<ChatAttachment> four;
		for( int i = 0; i < 4; ++i ) {
			ChatAttachment a; a.mimeType = "image/png"; a.base64Data = "QUJDRA=="; four.push_back( a );
		}
		loop.AddUserMessage( "here are four", four );   // no elision (== cap)
		std::vector<ChatAttachment> one;
		{ ChatAttachment a; a.mimeType = "image/png"; a.base64Data = "RUZHSA=="; one.push_back( a ); }
		loop.AddUserMessage( "one more", one );          // total 5 > 4 -> elide oldest

		bool sawUserElide = false;
		for( std::size_t i = 0; i < lines->size(); ++i ) {
			JsonValue r = Parse( ( *lines )[i] );
			if( r.get( "run_type" ).asString() == "history_edit" &&
			    r.get( "reason" ).asString() == "user_image_elision" ) {
				sawUserElide = true;
				Check( r.get( "before_bytes" ).asNumber() > r.get( "after_bytes" ).asNumber(),
				       "user_image_elision shrinks the entry (before > after)" );
				Check( r.get( "entry_index" ).asNumber() == 0.0, "user_image_elision targets the oldest entry" );
			}
		}
		Check( sawUserElide, "a user_image_elision history_edit was recorded when the cap was crossed" );
	}

	// --- tool-image elision (two read_image rounds; the older elides) ---
	{
		std::shared_ptr<std::vector<std::string> > lines =
			std::make_shared<std::vector<std::string> >();
		std::function<void(const std::string&)> sink =
			[lines]( const std::string& l ) { lines->push_back( l ); };

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-timg";
		cfg.clock = MakeCounterClock( 1 );
		loop.SetTrajectorySink( sink, cfg );
		loop.AddUserMessage( "render twice" );

		const char* const kReadImgA =
			"{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
			"\"content\":[{\"type\":\"tool_use\",\"id\":\"ti_a\",\"name\":\"read_image\",\"input\":{}}],"
			"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}";
		const char* const kReadImgB =
			"{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
			"\"content\":[{\"type\":\"tool_use\",\"id\":\"ti_b\",\"name\":\"read_image\",\"input\":{}}],"
			"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}";
		const char* const kEnvA =
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"png_base64\":\"QUFBQUFBQUFBQUFB\",\"byteLength\":9,\"width\":4,\"height\":4}}";
		const char* const kEnvB =
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"png_base64\":\"QkJCQkJCQkJCQkJC\",\"byteLength\":9,\"width\":4,\"height\":4}}";

		ChatStepResult a = loop.HandleResponse( 200, kReadImgA );
		if( a.kind == ChatStepResult::Kind::ToolCalls && a.toolCalls.size() == 1 ) {
			loop.ToolCallToJsonRpcLine( a.toolCalls[0], 1 );
			loop.AddToolResult( a.toolCalls[0], kEnvA );
		}
		ChatStepResult b = loop.HandleResponse( 200, kReadImgB );
		if( b.kind == ChatStepResult::Kind::ToolCalls && b.toolCalls.size() == 1 ) {
			loop.ToolCallToJsonRpcLine( b.toolCalls[0], 2 );
			loop.AddToolResult( b.toolCalls[0], kEnvB );   // flush elides the older image
		}

		bool sawToolElide = false;
		for( std::size_t i = 0; i < lines->size(); ++i ) {
			JsonValue r = Parse( ( *lines )[i] );
			if( r.get( "run_type" ).asString() == "history_edit" &&
			    r.get( "reason" ).asString() == "tool_image_elision" ) {
				sawToolElide = true;
				Check( r.get( "before_bytes" ).asNumber() > r.get( "after_bytes" ).asNumber(),
				       "tool_image_elision shrinks the entry (before > after)" );
			}
		}
		Check( sawToolElide, "a tool_image_elision history_edit was recorded when a newer image superseded" );
	}
}

//----------------------------------------------------------------------
// E6: redaction red-prove.
//----------------------------------------------------------------------
static void TestRedactionRedProve()
{
	std::printf( "E6: redaction red-prove (header strip + body regex)...\n" );

	std::shared_ptr<std::vector<std::string> > lines =
		std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };

	AgentChatLoop loop;
	loop.SetProvider( ChatProvider::Anthropic );
	ChatTrajectoryConfig cfg;
	cfg.traceId = "t-redact";
	cfg.clock = MakeCounterClock( 1 );
	loop.SetTrajectorySink( sink, cfg );

	// A user message that MALICIOUSLY embeds the same fake key in the body.
	loop.AddUserMessage( std::string( "please leak this: " ) + kApiKey + " ok?" );

	// The request the driver actually sends carries the key in the auth
	// header value.  Feed THAT request (unstripped) to the explicit
	// RecordHttpRound so the header-strip is the sole protector of the
	// header path in this call.
	const ChatHttpRequest req = loop.BuildRequest( kApiKey );
	bool authHeaderCarriedKey = false;
	for( std::size_t i = 0; i < req.headers.size(); ++i )
		if( req.headers[i].second.find( kApiKey ) != std::string::npos ) authHeaderCarriedKey = true;
	Check( authHeaderCarriedKey, "precondition: the built request's auth header carries the fake key" );

	const std::string body =
		"{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
		"\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\","
		"\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}";
	loop.RecordHttpRound( req, 200, body, 5 );
	loop.HandleResponse( 200, body );
	loop.FinishTrajectory( "closed" );

	// Across EVERY emitted line: the raw key never appears; no auth header
	// NAME appears; and the in-body occurrence IS redacted to [REDACTED].
	bool rawKeyAnywhere = false;
	bool authNameAnywhere = false;
	bool userRedacted = false;
	for( std::size_t i = 0; i < lines->size(); ++i ) {
		const std::string& l = ( *lines )[i];
		if( l.find( kApiKey ) != std::string::npos ) rawKeyAnywhere = true;
		if( l.find( "x-api-key" ) != std::string::npos ||
		    l.find( "authorization" ) != std::string::npos ||
		    l.find( "x-goog-api-key" ) != std::string::npos ) authNameAnywhere = true;
		JsonValue r = Parse( l );
		if( r.get( "run_type" ).asString() == "user" &&
		    r.get( "text" ).asString().find( "[REDACTED]" ) != std::string::npos )
			userRedacted = true;
	}
	Check( !rawKeyAnywhere, "the fake key appears in NO trajectory line (header path + body path)" );
	Check( !authNameAnywhere, "no auth header NAME appears in any line (header strip is load-bearing)" );
	Check( userRedacted, "the in-body key occurrence is redacted to [REDACTED] (regex pass)" );

	// Direct unit check of the writer's regex over the three shapes.
	Check( RedactTrajectoryLine( "x sk-abcdefghij y" ) == "x [REDACTED] y", "regex redacts sk- keys" );
	Check( RedactTrajectoryLine( "AIzaSyABCDEFGHIJKL end" ).find( "AIza" ) == std::string::npos,
	       "regex redacts AIza keys" );
	Check( RedactTrajectoryLine( "Authorization: Bearer abc123def456" ).find( "abc123def456" ) == std::string::npos,
	       "regex redacts Bearer tokens" );
	// Review-round P3: lowercase "bearer" (proxies/error bodies may
	// lowercase header names/values) is redacted too -- icase regex.
	Check( RedactTrajectoryLine( "authorization: bearer abc123def456" ).find( "abc123def456" ) == std::string::npos,
	       "regex redacts lowercase bearer tokens (icase)" );
}

//----------------------------------------------------------------------
// E7: rotation.
//----------------------------------------------------------------------
static void TestRotation()
{
	std::printf( "E7: PruneTrajectoryDir rotation...\n" );
	namespace fs = std::filesystem;
	fs::path dir = fs::temp_directory_path() /
		( std::string( "rise_traj_test_" ) + std::to_string( (long long)TrajectoryNowMs() ) );
	std::error_code ec;
	fs::create_directories( dir, ec );

	fs::file_time_type base = fs::file_time_type::clock::now();
	// Five files, staggered mtimes (a oldest .. e newest), 100 bytes each.
	const char* names[] = { "a.jsonl", "b.jsonl", "c.jsonl", "d.jsonl", "e.jsonl" };
	for( int i = 0; i < 5; ++i ) {
		std::ofstream f( ( dir / names[i] ).string(), std::ios::binary );
		f << std::string( 100, 'x' );
		f.close();
		fs::last_write_time( dir / names[i], base + std::chrono::seconds( i ), ec );
	}
	// Also a non-jsonl file that must be ignored.
	{ std::ofstream f( ( dir / "keep.txt" ).string() ); f << "leave me"; }

	// Keep the newest 3 by count.
	PruneTrajectoryDir( dir.string(), 3, 0 );
	Check( !fs::exists( dir / "a.jsonl" ) && !fs::exists( dir / "b.jsonl" ),
	       "rotation pruned the two oldest .jsonl files" );
	Check( fs::exists( dir / "c.jsonl" ) && fs::exists( dir / "d.jsonl" ) && fs::exists( dir / "e.jsonl" ),
	       "rotation kept the three newest .jsonl files" );
	Check( fs::exists( dir / "keep.txt" ), "rotation never touches non-.jsonl files" );

	// Byte cap: 3 files x 100 bytes = 300; cap at 150 -> keep only newest
	// that fit (prune oldest until total <= 150).
	PruneTrajectoryDir( dir.string(), 0, 150 );
	long long remaining = 0;
	for( const fs::directory_entry& e : fs::directory_iterator( dir ) ) {
		if( e.path().extension() == ".jsonl" ) remaining += (long long)fs::file_size( e.path() );
	}
	Check( remaining <= 150, "rotation pruned by byte cap (<= 150 bytes of .jsonl remain)" );
	Check( fs::exists( dir / "e.jsonl" ), "rotation keeps the newest under the byte cap" );

	fs::remove_all( dir, ec );
}

//----------------------------------------------------------------------
// E8: RecordAuxiliaryHttpRound -- Mac GUI prompt-triage HTTP round-trip
// recorded into the MAIN session's trajectory as a purpose-tagged `llm`
// record, closing runTriage's documented "KNOWN v1 GAP".
//----------------------------------------------------------------------
static void TestAuxiliaryHttpRound()
{
	std::printf( "E8: RecordAuxiliaryHttpRound (GUI prompt-triage aux record)...\n" );

	std::shared_ptr<std::vector<std::string> > lines =
		std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };

	AgentChatLoop loop;
	loop.SetProvider( ChatProvider::Anthropic );
	loop.SetContextBudget( 150000, 75000 );   // exercise a non-trivial budget window
	ChatTrajectoryConfig cfg;
	cfg.traceId = "trace-aux";
	cfg.clock = MakeCounterClock( 1000 );
	loop.SetTrajectorySink( sink, cfg );

	// A normal main-turn round first, so the summary has a genuine
	// main-conversation baseline to compare the aux round against.
	loop.AddUserMessage( "Make it red" );
	const std::string mainBody =
		"{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
		"\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"stop_reason\":\"end_turn\","
		"\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";
	loop.BuildRequest( kApiKey );
	loop.RecordHttpRound( 200, mainBody, 111 );
	loop.HandleResponse( 200, mainBody );

	// --- state-isolation snapshot, BEFORE the aux call ---
	const std::size_t transcriptBefore = loop.TranscriptSize();
	const std::size_t compactedBefore  = loop.CompactedEntryCount();
	const std::size_t noteCountBefore  = loop.DriverNoteCount();
	const std::size_t budgetHighBefore = loop.ContextBudgetHigh();
	const std::size_t budgetLowBefore  = loop.ContextBudgetLow();
	const std::size_t estTokensBefore  = loop.EstimateContextTokens();
	const std::size_t linesBefore      = lines->size();

	// The aux request/response bodies MALICIOUSLY embed the same fake key
	// the redaction red-prove in TestRedactionRedProve exercises via the
	// header path -- here it rides in the BODY text, so this is a second,
	// independent proof that the regex pass (not the header strip) is
	// what protects an auxiliary round.
	const std::string auxReqBody =
		std::string( "{\"model\":\"qwen3.6:27b\",\"key_leak\":\"" ) + kApiKey + "\"}";
	const std::string auxRespBody =
		std::string( "{\"error\":{\"message\":\"model not found, key was " ) + kApiKey + "\"}}";

	loop.RecordAuxiliaryHttpRound( "triage", "https://example.com/v1/models/qwen:generate",
	                               auxReqBody, 404, auxRespBody, 77 );

	// --- state-isolation snapshot, AFTER the aux call ---
	Check( loop.TranscriptSize() == transcriptBefore, "aux round does not touch mTranscript" );
	Check( loop.CompactedEntryCount() == compactedBefore, "aux round does not touch compaction count" );
	Check( loop.DriverNoteCount() == noteCountBefore, "aux round does not touch driver-note count" );
	Check( loop.ContextBudgetHigh() == budgetHighBefore, "aux round does not touch the budget high-water" );
	Check( loop.ContextBudgetLow() == budgetLowBefore, "aux round does not touch the budget low-water" );
	Check( loop.EstimateContextTokens() == estTokensBefore,
	       "aux round does not move the context-token estimate (no transcript growth)" );

	// --- exactly one new record, correctly shaped ---
	Check( lines->size() == linesBefore + 1, "RecordAuxiliaryHttpRound emits exactly one record" );
	JsonValue aux = Parse( lines->back() );
	Check( aux.get( "run_type" ).asString() == "llm", "aux record run_type is llm" );
	Check( aux.get( "purpose" ).asString() == "triage", "aux record carries purpose:triage" );
	Check( aux.get( "trace_id" ).asString() == "trace-aux",
	       "aux record shares the MAIN session's trace_id (same trajectory)" );
	Check( aux.get( "http.status" ).asNumber() == 404.0, "aux record carries the mechanical-failure status" );
	Check( aux.get( "latency_ms" ).asNumber() == 77.0, "aux record carries elapsed_ms" );
	Check( aux.get( "request_params" ).get( "url" ).asString() ==
	       "https://example.com/v1/models/qwen:generate", "aux record carries the URL" );
	Check( aux.get( "gen_ai.request.model" ).asString() == "qwen3.6:27b",
	       "aux record best-effort parses the request model" );

	// --- redaction red-prove: the fake key is in NEITHER body, ANYWHERE ---
	bool rawKeyAnywhere = false;
	for( std::size_t i = linesBefore; i < lines->size(); ++i )
		if( ( *lines )[i].find( kApiKey ) != std::string::npos ) rawKeyAnywhere = true;
	Check( !rawKeyAnywhere, "the fake key embedded in the aux request/response body never reaches disk" );
	Check( aux.get( "response_body" ).asString().find( "[REDACTED]" ) != std::string::npos,
	       "the aux response_body's key occurrence is redacted in place" );

	// --- summary-line honesty: the aux round is excluded from the rollup ---
	loop.FinishTrajectory( "closed" );
	JsonValue summary = Parse( lines->back() );
	Check( summary.get( "run_type" ).asString() == "summary", "final record is the summary" );
	Check( summary.get( "total_latency_ms" ).asNumber() == 111.0,
	       "summary total_latency_ms is the MAIN round's 111ms only -- the aux round's 77ms is excluded" );
	Check( summary.get( "gen_ai.usage.input_tokens" ).asNumber() == 50.0,
	       "summary input-token total unaffected by the aux round (which carries no usage)" );
	Check( summary.get( "gen_ai.usage.output_tokens" ).asNumber() == 10.0,
	       "summary output-token total unaffected by the aux round" );
	Check( summary.get( "n_turns" ).asNumber() == 1.0, "summary n_turns counts the one real user turn only" );

	// --- no-op when no sink is attached ---
	{
		AgentChatLoop bare;
		bare.SetProvider( ChatProvider::Anthropic );
		// No SetTrajectorySink call: TrajectoryActive() is false.
		Check( !bare.TrajectoryActive(), "precondition: a fresh loop has no active trajectory" );
		bare.RecordAuxiliaryHttpRound( "triage", "https://x", "{}", 200, "{}", 5 );
		// Nothing to assert on directly (no sink to inspect) beyond "did
		// not crash" -- covered by simply reaching this line.
	}
}

//----------------------------------------------------------------------
// E9: durable document snapshots (AgentChatLoop::SetDocumentSnapshotProvider) --
// the write policy (per-advance head_bump, decimated past 1MB, dedupe'd
// close-time session_end) and the no-provider no-op, all offline with a
// fake provider -- no GUI, no real Job/AgentSession involved.
//----------------------------------------------------------------------
namespace
{
	//! A single-tool-call assistant turn (mirrors E5's kReadImgA/B shape).
	std::string SnapshotToolUseBody( const std::string& callId )
	{
		return std::string(
			"{\"id\":\"m\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\"," ) +
			"\"content\":[{\"type\":\"tool_use\",\"id\":\"" + callId + "\",\"name\":\"insert_chunk\",\"input\":{}}]," +
			"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}";
	}

	//! The JSON-RPC success envelope reporting a numeric {uuid,revision}
	//! headVersion -- the shape AgentRpc.cpp's HeadVersionJson emits.
	std::string SnapshotEnvelope( int rpcId, uint64_t uuid, uint64_t revision )
	{
		return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string( rpcId ) +
			",\"result\":{\"applied\":true,\"status\":\"applied\",\"headVersion\":{\"uuid\":" +
			std::to_string( uuid ) + ",\"revision\":" + std::to_string( revision ) + "}}}";
	}

	//! Drive one tool-call round trip (HandleResponse -> ToolCallToJsonRpcLine
	//! -> AddToolResult) reporting the given headVersion.  `callId`/`rpcId`
	//! must be unique per call within a loop instance.
	void DriveOneToolCall( AgentChatLoop& loop, const std::string& callId, int rpcId,
	                       uint64_t uuid, uint64_t revision )
	{
		ChatStepResult st = loop.HandleResponse( 200, SnapshotToolUseBody( callId ) );
		if( st.kind != ChatStepResult::Kind::ToolCalls || st.toolCalls.size() != 1 ) {
			Check( false, "DriveOneToolCall: expected exactly one tool call" );
			return;
		}
		loop.ToolCallToJsonRpcLine( st.toolCalls[0], rpcId );
		loop.AddToolResult( st.toolCalls[0], SnapshotEnvelope( rpcId, uuid, revision ) );
	}

	//! A controllable fake document: the provider always answers with
	//! whatever this struct currently holds -- the test mutates it to
	//! simulate the real document advancing alongside each tool call's
	//! reported headVersion (they describe the SAME underlying document
	//! in the real system, so keeping them in lockstep here is the
	//! faithful fixture, not a simplification).
	struct FakeDoc
	{
		std::string text = "scene-text";
		uint64_t    uuid = 42;
		uint64_t    revision = 0;
		bool        hasDocument = true;
	};

	std::function<bool( std::string&, uint64_t&, uint64_t& )> MakeFakeProvider(
		std::shared_ptr<FakeDoc> doc )
	{
		return [doc]( std::string& outText, uint64_t& outUuid, uint64_t& outRevision ) -> bool {
			if( !doc->hasDocument ) return false;
			outText = doc->text;
			outUuid = doc->uuid;
			outRevision = doc->revision;
			return true;
		};
	}

	//! Count document_snapshot lines (optionally filtered by `reason`) in
	//! a recorded line vector.
	int CountSnapshots( const std::vector<std::string>& lines, const std::string& reason = "" )
	{
		int n = 0;
		for( std::size_t i = 0; i < lines.size(); ++i ) {
			JsonValue r = Parse( lines[i] );
			if( r.get( "run_type" ).asString() != "document_snapshot" ) continue;
			if( !reason.empty() && r.get( "reason" ).asString() != reason ) continue;
			++n;
		}
		return n;
	}
}

static void TestDocumentSnapshotPolicy()
{
	std::printf( "E9: durable document snapshots (write policy + decimation + no-provider)...\n" );

	// --- A: small document -- every advancing call snapshots 1:1;
	//     a non-advancing call snapshots nothing; a dedupe'd close (head
	//     unchanged since the last snapshot) adds no session_end. ---
	{
		std::shared_ptr<std::vector<std::string> > lines =
			std::make_shared<std::vector<std::string> >();
		std::function<void(const std::string&)> sink =
			[lines]( const std::string& l ) { lines->push_back( l ); };

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-snap-a";
		cfg.clock = MakeCounterClock( 1 );
		loop.SetTrajectorySink( sink, cfg );

		std::shared_ptr<FakeDoc> doc = std::make_shared<FakeDoc>();
		loop.SetDocumentSnapshotProvider( MakeFakeProvider( doc ) );

		loop.AddUserMessage( "build the scene" );

		// Three advancing calls -> three head_bump snapshots (small doc,
		// no decimation).
		for( int i = 1; i <= 3; ++i ) {
			doc->revision = static_cast<uint64_t>( i );
			DriveOneToolCall( loop, "toolu_a" + std::to_string( i ), i, doc->uuid, doc->revision );
		}
		Check( CountSnapshots( *lines, "head_bump" ) == 3,
		       "small doc: 3 advancing calls -> 3 head_bump snapshots" );

		// A non-advancing call (same headVersion the loop already saw --
		// e.g. a read-only verb's result echoing the current head) adds
		// no new snapshot.
		DriveOneToolCall( loop, "toolu_a4", 4, doc->uuid, doc->revision );
		Check( CountSnapshots( *lines, "head_bump" ) == 3,
		       "non-advancing headVersion -> no additional head_bump snapshot" );

		const int beforeClose = CountSnapshots( *lines );
		loop.FinishTrajectory( "closed" );
		Check( CountSnapshots( *lines, "session_end" ) == 0,
		       "close with head UNCHANGED since the last snapshot -> no session_end snapshot (dedupe)" );
		Check( CountSnapshots( *lines ) == beforeClose,
		       "dedupe'd close adds no document_snapshot record at all" );
	}

	// --- B: decimation past 1MB, and a NON-dedupe'd close. ---
	{
		std::shared_ptr<std::vector<std::string> > lines =
			std::make_shared<std::vector<std::string> >();
		std::function<void(const std::string&)> sink =
			[lines]( const std::string& l ) { lines->push_back( l ); };

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-snap-b";
		cfg.clock = MakeCounterClock( 1 );
		loop.SetTrajectorySink( sink, cfg );

		std::shared_ptr<FakeDoc> doc = std::make_shared<FakeDoc>();
		doc->text = std::string( 1200000, 'x' );   // ~1.2MB: over the 1MB decimation threshold
		loop.SetDocumentSnapshotProvider( MakeFakeProvider( doc ) );

		loop.AddUserMessage( "build a huge scene" );

		// Call #1: the FIRST advance of the trace always snapshots (no
		// prior WRITTEN snapshot to decimate by yet) -- even though this
		// document is already >1MB, so decimation only takes effect
		// STARTING from call #2.
		doc->revision = 1;
		DriveOneToolCall( loop, "toolu_b1", 101, doc->uuid, doc->revision );
		Check( CountSnapshots( *lines, "head_bump" ) == 1,
		       "large doc: call #1 always snapshots regardless of size" );

		// Calls #2..#11 (10 more advancing calls): kDocumentSnapshotDecimationStride
		// is 10, so only the LAST of these (the 10th advance since the
		// last WRITTEN snapshot) fires -- one more head_bump, at call #11.
		for( int i = 2; i <= 11; ++i ) {
			doc->revision = static_cast<uint64_t>( i );
			DriveOneToolCall( loop, "toolu_b" + std::to_string( i ), 100 + i, doc->uuid, doc->revision );
		}
		Check( CountSnapshots( *lines, "head_bump" ) == 2,
		       "large doc: only every 10th advancing call snapshots once decimating (2 total: call #1 and call #11)" );

		// Call #12: decimated again (only 1 advance since the call-#11
		// snapshot) -- head now sits at revision 12, past the last
		// WRITTEN snapshot's revision 11, so this is a NON-dedupe'd close
		// below.
		doc->revision = 12;
		DriveOneToolCall( loop, "toolu_b12", 112, doc->uuid, doc->revision );
		Check( CountSnapshots( *lines, "head_bump" ) == 2,
		       "call #12 stays decimated (still 2 head_bump snapshots)" );

		loop.FinishTrajectory( "closed" );
		Check( CountSnapshots( *lines, "session_end" ) == 1,
		       "close with head MOVED since the last snapshot (rev 11 -> 12) -> one session_end snapshot" );

		// The session_end snapshot carries the CURRENT (revision 12)
		// state, not the stale revision-11 one the decimation skipped.
		bool sawCorrectSessionEnd = false;
		for( std::size_t i = 0; i < lines->size(); ++i ) {
			JsonValue r = Parse( ( *lines )[i] );
			if( r.get( "run_type" ).asString() == "document_snapshot" &&
			    r.get( "reason" ).asString() == "session_end" ) {
				Check( r.get( "head_version_revision" ).asNumber() == 12.0,
				       "session_end snapshot carries the CURRENT revision (12), not the decimated-past one" );
				Check( r.get( "document_bytes" ).asNumber() == 1200000.0,
				       "session_end snapshot's document_bytes matches the ~1.2MB fake document" );
				sawCorrectSessionEnd = true;
			}
		}
		Check( sawCorrectSessionEnd, "found the session_end document_snapshot line to check" );
	}

	// --- C: no provider set -> zero document_snapshot records, and the
	//     rest of the trajectory is byte-for-byte the pre-feature shape
	//     (still 6 records: session,user,llm,tool,llm,summary). ---
	{
		std::shared_ptr<std::vector<std::string> > lines =
			std::make_shared<std::vector<std::string> >();
		std::function<void(const std::string&)> sink =
			[lines]( const std::string& l ) { lines->push_back( l ); };

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-snap-c";
		cfg.clock = MakeCounterClock( 1 );
		loop.SetTrajectorySink( sink, cfg );
		// Deliberately no SetDocumentSnapshotProvider call.

		loop.AddUserMessage( "no provider here" );
		DriveOneToolCall( loop, "toolu_c1", 1, 42, 1 );
		loop.FinishTrajectory( "closed" );

		Check( CountSnapshots( *lines ) == 0,
		       "no provider set -> zero document_snapshot records, even with an advancing headVersion" );
		// DriveOneToolCall never calls RecordHttpRound (mirrors E5's
		// tool-only flow, not E4's full HTTP-round flow), so the ordinary
		// shape here is session, user, tool, summary -- 4 records, none
		// of them a document_snapshot.  The point of this assertion is
		// that the pre-feature record count/shape is UNCHANGED by having
		// no provider attached, not that this exact shape is load-bearing.
		Check( lines->size() == 4,
		       "no provider set -> the ordinary (session,user,tool,summary) record count is unaffected" );
	}

	// --- Informational timing: the emit path (provider call + build +
	// serialize + redact + sink write) on a realistically sized document.
	// PRINTS, does not gate. ---
	{
		std::shared_ptr<FakeDoc> doc = std::make_shared<FakeDoc>();
		// Simulate "a few hundred chunks": a repeated chunk-shaped block,
		// large enough to also exercise the decimation-sized regime.
		std::string chunk =
			"standard_object\n{\n\tname \"obj\"\n\tgeometry \"sphere\"\n\tmaterial \"default\"\n}\n\n";
		std::string big;
		big.reserve( chunk.size() * 400 );
		for( int i = 0; i < 400; ++i ) big += chunk;
		doc->text = big;
		doc->revision = 1;

		AgentChatLoop loop;
		loop.SetProvider( ChatProvider::Anthropic );
		ChatTrajectoryConfig cfg;
		cfg.traceId = "t-snap-timing";   // real clock (no injected cfg.clock)
		std::function<void(const std::string&)> sink = []( const std::string& ) {};
		loop.SetTrajectorySink( sink, cfg );
		loop.SetDocumentSnapshotProvider( MakeFakeProvider( doc ) );
		loop.AddUserMessage( "time this" );

		const auto t0 = std::chrono::steady_clock::now();
		DriveOneToolCall( loop, "toolu_timing", 1, doc->uuid, doc->revision );
		const auto t1 = std::chrono::steady_clock::now();
		const long long micros =
			std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0 ).count();
		std::printf( "  [info] document_snapshot emit path (%zu-byte document, provider call + "
		             "build + serialize + redact + sink write): %lld us\n",
		             big.size(), micros );
	}
}

//----------------------------------------------------------------------
// GPT SLICE ITEM 3 (2026-08-24): E10, the response-body dedup.  OpenAI's
// stateful Responses API echoes the full `tools`/`instructions` request
// context back inside EVERY response body, byte-identical turn to turn --
// measured 68-78% of a recorded multi-turn trajectory file.  The recorder
// dedupes those two top-level keys against the last DIFFERENT value seen
// this session; ExpandTrajectoryResponseBody is the exact inverse, for a
// reader (AgentEvalRunner's replay path) that needs the complete body back.
//----------------------------------------------------------------------

//! A realistic-shaped response body: a big `tools` array, a long
//! `instructions` string, and a small per-turn `output` -- the SAME
//! three-field shape the real OpenAI Responses API trajectories measured
//! for this fix carry.  `outputTag` varies the ONE field that should
//! NEVER be deduped (it differs every real turn).
static std::string MakeGptStyleBody( const std::string& toolsJson, const std::string& instructionsText,
                                     const std::string& outputTag )
{
	// A JsonValue round-trip for the STRING escaping only (instructionsText
	// arrives as plain text here, same as a real caller would pass through
	// AgentChatCodecs before this ever reaches the recorder).
	JsonValue instr = JsonValue::MakeString( instructionsText );
	JsonValue outputArr = JsonValue::MakeArray();
	outputArr.push_back( JsonValue::MakeString( outputTag ) );
	JsonValue root = JsonValue::MakeObject();
	// `tools` is embedded as raw JSON text (already an array literal) --
	// ParseObjOr-style construction, matching how a real provider body
	// arrives as one already-serialized JSON document.
	std::string s = "{\"id\":\"resp_x\",\"tools\":" + toolsJson + ",\"instructions\":" +
		JsonSerialize( instr ) + ",\"output\":" + JsonSerialize( outputArr ) + "}";
	return s;
}

static void TestResponseBodyDedup()
{
	std::printf( "E10: response_body tools/instructions dedup...\n" );

	static const char* const kToolsJson =
		"[{\"type\":\"function\",\"name\":\"propose_patch\",\"description\":\"d1\"},"
		"{\"type\":\"function\",\"name\":\"render\",\"description\":\"d2\"}]";
	static const char* const kInstructions = "You are an agent. Follow the RISE scene grammar exactly.";

	std::shared_ptr<std::vector<std::string> > lines = std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };
	ChatTrajectoryConfig cfg;
	cfg.traceId = "trace-dedup";
	cfg.clock = MakeCounterClock( 1000 );
	ChatTrajectoryRecorder rec( sink, cfg );

	TrajectorySessionRecord sess;
	sess.provider = "openai";
	rec.EmitSession( sess );

	const std::vector<std::string> originalBodies = {
		MakeGptStyleBody( kToolsJson, kInstructions, "turn1" ),
		MakeGptStyleBody( kToolsJson, kInstructions, "turn2" ),
		MakeGptStyleBody( kToolsJson, kInstructions, "turn3" ),
	};
	for( const std::string& b : originalBodies ) {
		TrajectoryLlmRecord r;
		r.requestModel = "gpt-5.6";
		r.responseBody = b;
		rec.EmitLlm( r );
	}

	Check( lines->size() == 4, "session + 3 llm records emitted" );
	if( lines->size() != 4 ) return;

	std::vector<JsonValue> j;
	for( std::size_t i = 1; i < lines->size(); ++i ) j.push_back( Parse( ( *lines )[i] ) );   // skip the session record
	const std::vector<std::string> storedBodies = {
		j[0].get( "response_body" ).asString(),
		j[1].get( "response_body" ).asString(),
		j[2].get( "response_body" ).asString(),
	};

	// (1) RED-PROVE-ABLE MONEY ASSERTION: the FIRST record carries the
	// full tools array and instructions text verbatim (nothing to dedupe
	// against yet); records 2 and 3 do NOT -- the big duplicated content
	// is genuinely gone from the stored bytes, not just hidden behind a
	// flag.
	Check( storedBodies[0].find( "propose_patch" ) != std::string::npos &&
	       storedBodies[0].find( kInstructions ) != std::string::npos,
	       "E10 record 1 (nothing to dedupe against yet) carries tools+instructions in full" );
	Check( storedBodies[1].find( "propose_patch" ) == std::string::npos &&
	       storedBodies[1].find( kInstructions ) == std::string::npos,
	       "E10 MONEY ASSERTION: record 2's stored bytes do NOT contain the duplicated tools/"
	       "instructions content at all" );
	Check( storedBodies[2].find( "propose_patch" ) == std::string::npos &&
	       storedBodies[2].find( kInstructions ) == std::string::npos,
	       "E10 record 3 likewise" );
	Check( storedBodies[1].find( "__RISE_TRAJECTORY_DEDUP_REF__" ) != std::string::npos &&
	       storedBodies[2].find( "__RISE_TRAJECTORY_DEDUP_REF__" ) != std::string::npos,
	       "E10 ...replaced by the dedup marker" );

	// (2) THE PER-TURN CONTENT SURVIVES UNTOUCHED: `output` (the ONE field
	// that genuinely differs every real turn) is byte-present in every
	// record, deduped or not.
	Check( storedBodies[0].find( "turn1" ) != std::string::npos &&
	       storedBodies[1].find( "turn2" ) != std::string::npos &&
	       storedBodies[2].find( "turn3" ) != std::string::npos,
	       "E10 the per-turn `output` field is untouched in every record" );

	// (3) THE MEASURED REDUCTION on this fixture (small, but the same
	// shape as the real 68-78% measured on live trajectories -- see
	// ChatTrajectory.h's kTrajectoryDedupRefMarker doc for those numbers).
	const std::size_t origTotal = originalBodies[0].size() + originalBodies[1].size() + originalBodies[2].size();
	const std::size_t dedupTotal = storedBodies[0].size() + storedBodies[1].size() + storedBodies[2].size();
	Check( dedupTotal < origTotal, "E10 the deduped total is smaller than the original total" );
	std::printf( "  [info] E10 fixture: %zu -> %zu bytes (%.1f%% reduction)\n",
	             origTotal, dedupTotal, 100.0 * ( 1.0 - static_cast<double>( dedupTotal ) / static_cast<double>( origTotal ) ) );

	// (4) ROUND-TRIP: ExpandTrajectoryResponseBody, walked IN ORDER with
	// ONE shared state (mirroring AgentEvalRunner's replay reader),
	// reconstructs a body that PARSES to the SAME tools/instructions/
	// output content as the original -- zero information loss.
	TrajectoryDedupExpandState state;
	for( std::size_t i = 0; i < storedBodies.size(); ++i ) {
		const std::string expanded = ExpandTrajectoryResponseBody( storedBodies[i], state );
		JsonValue origParsed = Parse( originalBodies[i] );
		JsonValue expParsed  = Parse( expanded );
		Check( JsonSerialize( expParsed.get( "tools" ) ) == JsonSerialize( origParsed.get( "tools" ) ),
		       "E10 ROUND-TRIP record " + std::to_string( i ) + ": expanded tools == original tools" );
		Check( expParsed.get( "instructions" ).asString() == origParsed.get( "instructions" ).asString(),
		       "E10 ROUND-TRIP record " + std::to_string( i ) + ": expanded instructions == original" );
		Check( JsonSerialize( expParsed.get( "output" ) ) == JsonSerialize( origParsed.get( "output" ) ),
		       "E10 ROUND-TRIP record " + std::to_string( i ) + ": output preserved" );
	}

	// (5) A DIFFERENT tools value breaks the chain and becomes the new
	// baseline -- no false-positive dedup against an unrelated value.
	{
		static const char* const kDifferentTools =
			"[{\"type\":\"function\",\"name\":\"validate\",\"description\":\"d3\"}]";
		TrajectoryLlmRecord r;
		r.responseBody = MakeGptStyleBody( kDifferentTools, kInstructions, "turn4" );
		rec.EmitLlm( r );
		const JsonValue j4 = Parse( lines->back() );
		const std::string body4 = j4.get( "response_body" ).asString();
		Check( body4.find( "validate" ) != std::string::npos,
		       "E10 a genuinely DIFFERENT tools value is stored in full, not dedupe-suppressed" );
		Check( body4.find( kInstructions ) == std::string::npos &&
		       body4.find( "__RISE_TRAJECTORY_DEDUP_REF__" ) != std::string::npos,
		       "E10 ...while instructions (unchanged) still dedupes against the earlier baseline" );
	}

	// (6) SESSION BOUNDARY: a fresh EmitSession resets the dedup
	// baseline -- the same tools/instructions immediately after are
	// stored IN FULL again, not deduped against the prior session.
	{
		TrajectorySessionRecord sess2;
		sess2.provider = "openai";
		rec.EmitSession( sess2 );
		TrajectoryLlmRecord r;
		r.responseBody = MakeGptStyleBody( kToolsJson, kInstructions, "turn5" );
		rec.EmitLlm( r );
		const JsonValue j5 = Parse( lines->back() );
		const std::string body5 = j5.get( "response_body" ).asString();
		Check( body5.find( "propose_patch" ) != std::string::npos &&
		       body5.find( kInstructions ) != std::string::npos,
		       "E10 SESSION BOUNDARY: a fresh session's first llm record is stored in full, even "
		       "though it repeats the PRIOR session's exact tools/instructions" );
	}
}

//----------------------------------------------------------------------
// Review-round C, P3-c (2026-08-24): the MARKER-COLLISION case.  An
// earlier draft's marker was a bare JSON STRING -- indistinguishable, on
// the read side, from a genuine `instructions` value that happened to
// equal that exact text.  The shipped fix wraps the marker in a JSON
// OBJECT instead ({"$dedupRef":"..."}), which neither `tools` (always an
// array) nor `instructions` (always a string) can ever legitimately be --
// a structural guarantee, not a probabilistic one.  This proves it: a
// genuine `instructions` value that IS the marker's own tag text, as a
// plain string, round-trips correctly (never mistaken for the marker),
// and a genuine REPEAT of that same collision-shaped value still dedupes
// normally.
//----------------------------------------------------------------------
static void TestResponseBodyDedupMarkerCollision()
{
	std::printf( "E10b: a genuine value shaped like the dedup marker's own tag text...\n" );

	static const char* const kToolsJson =
		"[{\"type\":\"function\",\"name\":\"propose_patch\",\"description\":\"d1\"}]";
	static const char* const kInstructions = "You are an agent.";
	// The exact text an OLD bare-string marker would have spliced in --
	// authored here as a GENUINE `instructions` value, on purpose.
	static const char* const kCollisionText = "__RISE_TRAJECTORY_DEDUP_REF__";

	std::shared_ptr<std::vector<std::string> > lines = std::make_shared<std::vector<std::string> >();
	std::function<void(const std::string&)> sink =
		[lines]( const std::string& l ) { lines->push_back( l ); };
	ChatTrajectoryConfig cfg;
	cfg.traceId = "trace-collision";
	cfg.clock = MakeCounterClock( 2000 );
	ChatTrajectoryRecorder rec( sink, cfg );

	TrajectorySessionRecord sess;
	sess.provider = "openai";
	rec.EmitSession( sess );

	// Record 1: an ordinary baseline (tools/instructions cached normally).
	{
		TrajectoryLlmRecord r;
		r.responseBody = MakeGptStyleBody( kToolsJson, kInstructions, "turn1" );
		rec.EmitLlm( r );
	}
	// Record 2: instructions is now the collision text -- GENUINELY
	// DIFFERENT from record 1's cached value, so this must NOT dedupe;
	// it must be stored (and later expand back to) the collision text
	// itself, never record 1's "You are an agent." (the corruption an
	// unguarded bare-string marker would produce).
	{
		TrajectoryLlmRecord r;
		r.responseBody = MakeGptStyleBody( kToolsJson, kCollisionText, "turn2" );
		rec.EmitLlm( r );
	}
	// Record 3: a GENUINE repeat of the collision text -- this SHOULD
	// dedupe normally against record 2's now-cached collision value.
	{
		TrajectoryLlmRecord r;
		r.responseBody = MakeGptStyleBody( kToolsJson, kCollisionText, "turn3" );
		rec.EmitLlm( r );
	}

	Check( lines->size() == 4, "session + 3 llm records emitted" );
	if( lines->size() != 4 ) return;

	std::vector<JsonValue> j;
	for( std::size_t i = 1; i < lines->size(); ++i ) j.push_back( Parse( ( *lines )[i] ) );
	const std::string body1 = j[0].get( "response_body" ).asString();
	const std::string body2 = j[1].get( "response_body" ).asString();
	const std::string body3 = j[2].get( "response_body" ).asString();

	// MONEY: record 2's STORED bytes carry the collision text verbatim,
	// as a plain JSON string -- NOT converted to (or confused with) the
	// object-shaped marker.
	JsonValue p2 = Parse( body2 );
	Check( p2.get( "instructions" ).asString() == kCollisionText,
	       "E10b MONEY ASSERTION: record 2's stored `instructions` is the genuine collision-text "
	       "STRING value, not misidentified as the marker" );
	Check( !p2.get( "instructions" ).isObject(),
	       "E10b ...specifically NOT an object -- `tools` legitimately dedupes in this same record "
	       "(it repeats record 1's), so this checks the `instructions` field's own shape, not the "
	       "whole body for the marker tag" );

	// Record 3 SHOULD have genuinely deduped against record 2's cached
	// collision value (the object-shaped marker, distinguishable from
	// the plain-string collision text by its very shape).
	JsonValue p3 = Parse( body3 );
	Check( p3.get( "instructions" ).isObject() &&
	       p3.get( "instructions" ).get( "$dedupRef" ).asString() == kCollisionText,
	       "E10b record 3 (a genuine REPEAT of the collision text) dedupes normally, marked with "
	       "the object-shaped marker" );

	// ROUND-TRIP: expanding all three, in order, reconstructs the exact
	// per-record instructions value each one actually carried -- record
	// 1 "You are an agent.", records 2 AND 3 the collision text itself
	// (never record 1's value, which is what an unguarded bare-string
	// marker misread would have produced for record 2).
	TrajectoryDedupExpandState state;
	const std::string exp1 = ExpandTrajectoryResponseBody( body1, state );
	const std::string exp2 = ExpandTrajectoryResponseBody( body2, state );
	const std::string exp3 = ExpandTrajectoryResponseBody( body3, state );
	Check( Parse( exp1 ).get( "instructions" ).asString() == kInstructions,
	       "E10b round-trip record 1: expanded instructions == \"You are an agent.\"" );
	Check( Parse( exp2 ).get( "instructions" ).asString() == kCollisionText,
	       "E10b MONEY ROUND-TRIP: record 2 expands to the collision text itself, NOT record 1's "
	       "value -- the corruption an unguarded bare-string marker would have produced" );
	Check( Parse( exp3 ).get( "instructions" ).asString() == kCollisionText,
	       "E10b round-trip record 3: expands to the collision text too (the genuine dedup)" );
}

//----------------------------------------------------------------------
// Review-round P2: "recording never disrupts the chat" must hold for ANY
// sink -- a throwing sink is swallowed and recording is disabled for the
// rest of the session (the sink is dropped after the first throw).
//----------------------------------------------------------------------
static void RunThrowingSinkTest()
{
	std::printf( "=== AgentTrajectoryTest: throwing sink is swallowed + disables recording ===\n" );
	int calls = 0;
	ChatTrajectoryConfig cfg;
	cfg.clock = []() -> int64_t { return 1700000000000LL; };
	ChatTrajectoryRecorder rec(
		[&calls]( const std::string& ) {
			++calls;
			if( calls >= 2 ) throw std::runtime_error( "sink exploded" );
		}, cfg );

	TrajectoryUserRecord u;
	u.text = "first";
	bool escaped = false;
	try {
		rec.EmitUser( u );          // call 1: sink ok
		u.text = "second";
		rec.EmitUser( u );          // call 2: sink throws -- must be swallowed
		u.text = "third";
		rec.EmitUser( u );          // call 3: sink disabled -- not invoked
	}
	catch( ... ) { escaped = true; }
	Check( !escaped, "a throwing sink never propagates out of the recorder" );
	Check( calls == 2, "the sink is DISABLED after its first throw (called exactly twice, not three times)" );
}

int main()
{
	std::printf( "=== AgentTrajectoryTest (Eval-harness E1) ===\n" );
	TestSchemaRoundTrip();
	TestDottedOrder();
	TestParseUsage();
	TestFullSession();
	TestHistoryEditRecords();
	TestRedactionRedProve();
	TestRotation();
	TestAuxiliaryHttpRound();
	TestDocumentSnapshotPolicy();
	TestResponseBodyDedup();
	TestResponseBodyDedupMarkerCollision();
	RunThrowingSinkTest();
	std::printf( "=== AgentTrajectoryTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
