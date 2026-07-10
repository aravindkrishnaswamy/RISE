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
	std::printf( "=== AgentTrajectoryTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
