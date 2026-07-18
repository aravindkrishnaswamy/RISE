//////////////////////////////////////////////////////////////////////
//
//  AgentEvalLiveTransportTest.cpp - Eval-harness slice E4 (the LIVE
//    headless runner) unit tests.
//
//    NO REAL NETWORK.  CI has no api keys and a real POST is
//    non-deterministic, so this suite NEVER performs a real HTTP round-
//    trip.  Instead it:
//      (a) asserts the platform transport FACTORY returns a non-null impl
//          and (via a loopback-only bogus URL) that this platform's impl
//          is a REAL transport, not the Linux "unsupported" stub;
//      (b) drives RunScenarioLive over a MOCK IChatHttpTransport (canned
//          responses) -- the SAME runner code path `rise --agent-eval`
//          uses, minus the real socket -- proving the transport plumbing +
//          RecordHttpRound (status/elapsed) + tool dispatch + budgets +
//          CheckScenario wiring end-to-end;
//      (c) the KEY-HYGIENE red-prove: a distinctive FAKE key rides the
//          mock's auth header (proving the key reaches the wire) yet
//          appears in NONE of the runDir output files;
//      (d) LoadEvalRunConfig parsing + malformed refusals, and
//          RunEvalMatrix's missing-key SKIP (via an injected env lookup).
//
//    The real CFNetwork/NSURLSession Post is exercised ONLY by a MANUAL
//    smoke (documented in the E4 report), never here.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentEvalRunner.h"
#include "../src/Library/Agent/ChatHttpTransport.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Agent/Base64.h"   // image-reconstruction Wave 1: Base64Encode/Decode for the reference-image tests

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

//----------------------------------------------------------------------
// Scratch dir + small helpers.
//----------------------------------------------------------------------
static std::string ScratchRunDir( const char* leaf )
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	dir += "rise_agent_eval_live_test/";
	dir += leaf;
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );
	return dir;
}

static std::vector<JsonValue> ReadJsonl( const std::string& path )
{
	std::vector<JsonValue> out;
	std::ifstream f( path.c_str(), std::ios::binary );
	std::string line;
	while( std::getline( f, line ) ) {
		if( line.empty() ) continue;
		JsonValue v; std::string err;
		if( JsonParse( line, v, err ) ) out.push_back( v );
	}
	return out;
}

static std::vector<std::string> RunTypeSequence( const std::vector<JsonValue>& records )
{
	std::vector<std::string> out;
	for( std::size_t i = 0; i < records.size(); ++i ) out.push_back( records[i].get( "run_type" ).asString() );
	return out;
}

// True iff `needle` appears anywhere under `dir` (any file's bytes) -- the
// key-hygiene red-prove's scanner.
static bool AnyFileUnderContains( const std::string& dir, const std::string& needle )
{
	std::error_code ec;
	if( !std::filesystem::exists( dir, ec ) ) return false;
	for( std::filesystem::recursive_directory_iterator it( dir, ec ), end; it != end; it.increment( ec ) ) {
		if( ec ) break;
		if( !it->is_regular_file( ec ) ) continue;
		std::ifstream f( it->path().string().c_str(), std::ios::binary );
		std::ostringstream ss; ss << f.rdbuf();
		if( ss.str().find( needle ) != std::string::npos ) return true;
	}
	return false;
}

static bool WriteFile( const std::string& path, const std::string& text )
{
	std::error_code ec;
	std::filesystem::path p( path );
	if( p.has_parent_path() ) std::filesystem::create_directories( p.parent_path(), ec );
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return false;
	f.write( text.data(), static_cast<std::streamsize>( text.size() ) );
	return true;
}

//----------------------------------------------------------------------
// The MOCK transport: canned responses + captured requests.  This is the
// SAME IChatHttpTransport seam the real socket plugs into -- so it drives
// byte-identical RunScenarioLive code.
//----------------------------------------------------------------------
class MockTransport : public IChatHttpTransport
{
public:
	struct Canned { long status; std::string body; std::string error; long elapsedMs; };
	std::vector<Canned>          responses;
	bool                         repeatLast = false;   // after exhaustion, keep returning the last canned response
	std::size_t                  idx = 0;
	std::vector<ChatHttpRequest> seenRequests;

	ChatHttpResponse Post( const ChatHttpRequest& req ) override
	{
		seenRequests.push_back( req );
		ChatHttpResponse r;
		Canned c;
		if( idx < responses.size() ) c = responses[idx++];
		else if( repeatLast && !responses.empty() ) c = responses.back();
		else { r.status = 0; r.error = "mock: no more canned responses"; r.elapsedMs = 1; return r; }
		r.status = c.status; r.body = c.body; r.error = c.error; r.elapsedMs = c.elapsedMs;
		return r;
	}

	bool AnyRequestHeaderContains( const std::string& needle ) const
	{
		for( std::size_t i = 0; i < seenRequests.size(); ++i )
			for( std::size_t h = 0; h < seenRequests[i].headers.size(); ++h )
				if( seenRequests[i].headers[h].second.find( needle ) != std::string::npos ) return true;
		return false;
	}
};

// Canned Anthropic response bodies (raw wire JSON) -- a tool_use turn (recolor
// pnt_albedo red) then a final-text turn.  These target the pnt_albedo chunk
// that the committed param_edit scenario's inline scene carries.
static const char* kBodyToolUse =
	"{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
	"\"content\":[{\"type\":\"text\",\"text\":\"Recoloring the sphere.\"},"
	"{\"type\":\"tool_use\",\"id\":\"toolu_01A\",\"name\":\"propose_patch\","
	"\"input\":{\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1 0.1\"}}],"
	"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":128,\"output_tokens\":64}}";

static const char* kBodyFinal =
	"{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
	"\"content\":[{\"type\":\"text\",\"text\":\"Done: pnt_albedo is now red.\"}],"
	"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";

//----------------------------------------------------------------------
// T1: the platform transport factory.
//----------------------------------------------------------------------
static void TestFactory()
{
	std::printf( "T1: CreateSystemChatHttpTransport factory...\n" );
	std::unique_ptr<IChatHttpTransport> t = CreateSystemChatHttpTransport();
	Check( t != nullptr, "factory returns a non-null transport on this platform" );

#if defined(__APPLE__) || defined(_WIN32)
	// A REAL platform transport: a loopback POST to an invalid port fails fast
	// with a transport error (status 0) whose message is NOT the "unsupported"
	// stub's.  Loopback only -- no external DNS / network.
	if( t ) {
		ChatHttpRequest req;
		req.url = "https://127.0.0.1:0/__rise_probe__";
		req.headers.push_back( std::make_pair( "content-type", "application/json" ) );
		req.body = "{}";
		ChatHttpResponse r = t->Post( req );
		Check( r.status <= 0, "real transport: loopback:0 POST does not reach an HTTP status" );
		Check( r.error.find( kUnsupportedTransportSentinel ) == std::string::npos,
		       "real transport: the error is a genuine transport failure, not the unsupported stub" );
		// The intended property (not the tautology it replaced): a REAL platform
		// transport, handed an unreachable endpoint, returns a status<=0 transport
		// failure carrying a NON-EMPTY header-free error category.  (Header-freeness
		// itself -- that the api key never appears -- is proven by T5's red-prove.)
		Check( !r.error.empty(),
		       "real transport: an unreachable endpoint yields a non-empty transport error" );
	}
#else
	// Linux/other: the factory returns the honest "unsupported" stub.
	if( t ) {
		ChatHttpRequest req; req.url = "https://example.com"; req.body = "{}";
		ChatHttpResponse r = t->Post( req );
		Check( r.status == 0 && r.error.find( kUnsupportedTransportSentinel ) != std::string::npos,
		       "Linux stub: Post honestly reports unsupported" );
	}
#endif
}

//----------------------------------------------------------------------
// T2: MOCK-driven RunScenarioLive end-to-end (the --agent-eval code path).
//----------------------------------------------------------------------
static void TestLiveRunViaMock()
{
	std::printf( "T2: RunScenarioLive over a mock transport (param_edit)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
	       "param_edit scenario loads (" + err + ")" );

	MockTransport mock;
	mock.responses.push_back( { 200, kBodyToolUse, "", 12 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 7 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t2_live_param_edit" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );

	Check( h.result.terminalStatus == "final_text",
	       "live run terminal status is final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "live run drove exactly 2 llm rounds through the transport" );
	Check( h.result.toolCalls == 1, "live run dispatched exactly 1 tool call" );
	Check( !h.result.budgetHit, "live run hit no budget" );
	Check( h.result.headVersionFinal > h.result.headVersionStart,
	       "live run headVersion ADVANCED (a committing edit really landed)" );
	Check( h.result.finalText == "Done: pnt_albedo is now red.", "live run captured the final text" );
	Check( h.dispatcher != nullptr, "live run returns an ALIVE dispatcher (the E3 seam)" );
	Check( mock.seenRequests.size() == 2, "the mock transport saw exactly 2 POSTs" );

	// FIX 1 (per-request transport timeout): the hosted Anthropic provider's
	// BuildRequest carries the unchanged 300s budget all the way out to what
	// the transport actually receives -- proving the plumbing (codec Config
	// -> ChatHttpRequest.timeoutSeconds -> the seam a real transport reads)
	// rather than just the codec-level unit test in AgentChatLoopTest.cpp.
	Check( !mock.seenRequests.empty() && mock.seenRequests[0].timeoutSeconds == 300,
	       "the request the (mock) transport received carries timeoutSeconds==300 for a hosted provider" );

	// The trajectory records the LIVE round through RecordHttpRound.
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	const std::vector<std::string> expected = { "session", "user", "llm", "tool", "llm", "summary" };
	Check( RunTypeSequence( recs ) == expected,
	       "live trajectory sequence is session,user,llm,tool,llm,summary" );

	// The recorded llm round carried the transport's measured elapsedMs (12).
	bool sawElapsed = false;
	for( std::size_t i = 0; i < recs.size(); ++i )
		if( recs[i].get( "run_type" ).asString() == "llm" &&
		    recs[i].get( "latency_ms" ).asNumber( -1.0 ) == 12.0 ) sawElapsed = true;
	Check( sawElapsed, "the first llm round recorded the transport's measured elapsedMs (12)" );

	// The one-line result file was written.
	std::vector<JsonValue> resRecs = ReadJsonl( h.resultPath );
	Check( resRecs.size() == 1 && resRecs[0].get( "terminalStatus" ).asString() == "final_text",
	       "the <id>.result.jsonl one-liner was written with terminalStatus final_text" );
}

//----------------------------------------------------------------------
// T3: budget enforcement on the live path.
//----------------------------------------------------------------------
static void TestLiveBudget()
{
	std::printf( "T3: budget enforcement on the live path...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	scenario.budgets.maxToolCalls = 0;   // the FIRST tool call must be refused

	MockTransport mock;
	mock.responses.push_back( { 200, kBodyToolUse, "", 3 } );
	mock.repeatLast = true;   // keep offering the tool_use turn

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t3_live_budget" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "budget_tool_calls",
	       "maxToolCalls=0 trips budget_tool_calls (got '" + h.result.terminalStatus + "')" );
	Check( h.result.budgetHit, "budgetHit is set" );
	Check( h.result.toolCalls == 0, "no tool call was dispatched under the 0 budget" );
}

//----------------------------------------------------------------------
// T4: a transport failure that fails on BOTH attempts still maps to
//     terminalStatus transport_error -- and is BOUNDED to exactly 2 POSTs
//     (the original attempt + the one same-round retry from FIX 3), not an
//     unbounded retry loop.
//----------------------------------------------------------------------
static void TestTransportError()
{
	std::printf( "T4: a transport failure on both attempts -> transport_error, bounded to 2 POSTs (FIX 3)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 5 } );
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 5 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t4_transport_error" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "transport_error",
	       "a status-0 transport response on both attempts -> transport_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.errorMessage.find( "connection lost" ) != std::string::npos,
	       "the transport's header-free error category propagates to errorMessage" );
	Check( h.result.llmCalls == 2, "both transport-failed POSTs count against the budget -- a request sent may be billed even when its response is lost" );
	Check( mock.seenRequests.size() == 2,
	       "the transport saw exactly 2 POSTs -- the retry is bounded to ONE attempt, not a loop" );
}

//----------------------------------------------------------------------
// P1-A: the transport-error retry is BUDGET-GATED.  A transport-failed POST
//      returns proceed=false and (pre-fix) never incremented llmCalls, so a
//      same-round transport retry could send a 2nd POST past a maxLlmCalls:1
//      cap -- a request that may still have reached (and been billed by) the
//      provider.  The fix counts every POST attempted, so the inner budget
//      check bounds the retry: cap 1 -> exactly 1 POST, stop on the llm-call
//      budget (NOT transport_error).
//----------------------------------------------------------------------
static void TestTransportRetryRespectsLlmBudget()
{
	std::printf( "P1-A: transport retry is budget-gated -- maxLlmCalls:1 sends exactly 1 POST...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	scenario.budgets.maxLlmCalls = 1;   // tighten the cap so the retry would exceed it

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 5 } );
	// A 2nd transport response is queued but MUST NEVER be consumed -- the
	// budget has to block the retry before its POST leaves the client.
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 5 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "p1a_transport_retry_budget" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( mock.seenRequests.size() == 1,
	       "P1-A: maxLlmCalls:1 -> the transport retry is budget-blocked; exactly 1 POST (not 2)" );
	Check( h.result.llmCalls == 1, "P1-A: the single attempted POST is counted" );
	Check( h.result.terminalStatus == "budget_llm_calls",
	       "P1-A: the run stops on the llm-call budget, not transport_error (got '" + h.result.terminalStatus + "')" );
}

//----------------------------------------------------------------------
// T16: FIX 3 -- one automatic same-round retry on a transport-class failure
//      (status<=0, e.g. NSURLError -1005 connection-lost / -1009 offline).
//      Chat-completions POSTs are stateless, so re-issuing the SAME round
//      once is safe.  The first attempt never reaches HTTP (no body), so no
//      llm round is recorded for it; the retry succeeds and the scenario
//      reaches final_text having driven exactly 1 recorded llm round from
//      exactly 2 POSTs.
//----------------------------------------------------------------------
static void TestLiveTransportRetrySucceeds()
{
	std::printf( "T16: transport failure then success -> one retry, round succeeds (FIX 3)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 5 } );
	mock.responses.push_back( { 200, kBodyToolUse, "", 4 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 4 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t16_live_transport_retry_succeeds" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "a transport-fail-then-200 round still reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 3, "all 3 POSTs count -- the 2 real HTTP rounds (tool_use, final) PLUS the transport-failed attempt (requests sent)" );
	Check( mock.seenRequests.size() == 3,
	       "the transport saw exactly 3 POSTs: the failed attempt + its retry, then the second turn's round" );
	Check( h.result.finalText == "Done: pnt_albedo is now red.", "the retried round's eventual final text is captured" );

	// The trajectory carries NO llm record for the failed transport attempt
	// (RecordHttpRound is never reached when fo.proceed is false) -- only the
	// two REAL rounds.  The first round's inner attempt counter had already
	// advanced past the failed transport try before it got its first HTTP
	// response, so that round is recorded as attempt 2 / retry_of 1 (an
	// honest sibling record, same convention as the 5xx/400 retries in
	// T12/T15); the second round's assistant turn succeeds on its own first
	// try and is recorded as attempt 1 / retry_of -1.
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	int llmRecords = 0, attempt2RetryOf1 = 0, attempt1NoRetry = 0;
	for( std::size_t i = 0; i < recs.size(); ++i ) {
		if( recs[i].get( "run_type" ).asString() != "llm" ) continue;
		++llmRecords;
		const int at = static_cast<int>( recs[i].get( "attempt" ).asNumber( -1.0 ) );
		const int ro = static_cast<int>( recs[i].get( "retry_of" ).asNumber( -1.0 ) );
		if( at == 2 && ro == 1 ) ++attempt2RetryOf1;
		if( at == 1 && ro == -1 ) ++attempt1NoRetry;
	}
	Check( llmRecords == 2, "exactly 2 llm trajectory records -- the transport-failed attempt left none" );
	Check( attempt2RetryOf1 == 1, "the round whose first try was the transport failure is recorded as attempt 2 / retry_of 1" );
	Check( attempt1NoRetry == 1, "the second round's assistant turn succeeded first-try and is recorded as attempt 1 / retry_of -1" );
}

//----------------------------------------------------------------------
// T17: FIX 3 -- a SECOND transport failure (fail-then-fail) is NOT retried
//      again; the round falls through to the unchanged transport_error
//      termination.  Proves the retry is bounded to exactly one attempt per
//      round, not a loop.  (Same property as T4; kept as its own case to
//      match the fail-fail / fail-succeed pairing used for FIX 2's 5xx
//      retry in T12/T13.)
//----------------------------------------------------------------------
static void TestLiveTransportRetryStillFails()
{
	std::printf( "T17: transport failure then transport failure -> transport_error, bounded (FIX 3)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1009]: offline", 2 } );
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1009]: offline", 2 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t17_live_transport_retry_fails" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "transport_error",
	       "fail-then-fail falls through to transport_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.llmCalls == 2, "both transport-failed POSTs count against the budget (requests sent, possibly billed)" );
	Check( mock.seenRequests.size() == 2,
	       "the transport saw exactly 2 POSTs -- the retry is bounded to ONE attempt, not a loop" );
}

//----------------------------------------------------------------------
// T18: FIX 3 independence -- a transport failure on attempt 1 followed by an
//      HTTP 500 on attempt 2 falls through to provider_error after exactly
//      2 POSTs.  Proves the transport-retry and the 5xx-retry (FIX 2) do
//      NOT stack into two retries of the same round: the attempt counter is
//      shared across causes, so whichever cause fires first consumes the
//      round's single allowed retry.
//----------------------------------------------------------------------
static void TestLiveTransportRetryDoesNotStackWith5xx()
{
	std::printf( "T18: transport failure then 500 -> provider_error after 2 POSTs, retries do not stack (FIX 2+3 independence)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1005]: connection lost", 2 } );
	mock.responses.push_back( { 500, "{\"error\":\"internal server error\"}", "", 3 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t18_live_transport_then_5xx" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "provider_error",
	       "transport-fail-then-500 falls through to provider_error, not a second retry (got '" + h.result.terminalStatus + "')" );
	Check( h.result.llmCalls == 2, "both POSTs count -- the 500 attempt AND the transport-failed attempt (requests sent)" );
	Check( mock.seenRequests.size() == 2,
	       "the transport saw exactly 2 POSTs total -- the two independent retry causes did not stack" );
}

//----------------------------------------------------------------------
// T19: HTTP 429 (rate-limit) backoff -- a hint-less 429 followed by a
//      success reaches final_text after exactly 2 POSTs, asking the
//      injected sleeper for the exponential fallback table's first-attempt
//      wait (10000ms), and does NOT count the 429 attempt against
//      llmCalls.
//----------------------------------------------------------------------
static void TestLive429RetrySucceeds()
{
	std::printf( "T19: HTTP 429 then success -> one retry after a backoff wait, round succeeds...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 429,
		"{\"error\":{\"message\":\"Rate limit reached for requests. Limit 500000, Used 488104.\"}}", "", 3 } );
	mock.responses.push_back( { 200, kBodyFinal, "", 4 } );

	std::vector<int64_t> waits;
	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t19_live_429_retry_succeeds" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";
	opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "a 429-then-200 round still reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( mock.seenRequests.size() == 2, "the transport saw exactly 2 POSTs (the retry rebuilt the SAME round)" );
	Check( h.result.finalText == "Done: pnt_albedo is now red.", "the retried round's final text is captured" );
	Check( h.result.llmCalls == 1, "the 429 attempt does not count toward llmCalls -- only the successful POST does" );
	Check( waits.size() == 1, "exactly one backoff wait was requested" );
	Check( !waits.empty() && waits[0] == 10000,
	       "a hint-less 429's first-attempt wait is the exponential fallback's 10000ms (got " +
	       ( waits.empty() ? std::string( "<none>" ) : std::to_string( waits[0] ) ) + ")" );
}

//----------------------------------------------------------------------
// T20: HTTP 429 body-hint parsing -- "try again in <N>s" and
//      "retry after <N> seconds" (case-insensitive, float N) are parsed
//      out of the response body and used INSTEAD of the exponential
//      fallback.
//----------------------------------------------------------------------
static void TestLive429BodyHintParsing()
{
	std::printf( "T20: HTTP 429 body-hint parsing...\n" );

	// (A) "Please try again in 2.5s." -> ~2500ms.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T20(A): param_edit loads" );

		MockTransport mock;
		mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limited. Please try again in 2.5s.\"}}", "", 2 } );
		mock.responses.push_back( { 200, kBodyFinal, "", 4 } );

		std::vector<int64_t> waits;
		AgentEvalLiveRunOptions opts;
		opts.runDir = ScratchRunDir( "t20a_live_429_hint_try_again_in" );
		opts.transport = &mock;
		opts.provider = ChatProvider::Anthropic;
		opts.apiKey = "unit-test-key-not-real";
		opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

		AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
		Check( h.result.terminalStatus == "final_text", "T20(A): the hinted-429-then-200 round reaches final_text" );
		Check( waits.size() == 1 && waits[0] == 2500,
		       "T20(A): 'try again in 2.5s' parses to a 2500ms wait (got " +
		       ( waits.empty() ? std::string( "<none>" ) : std::to_string( waits[0] ) ) + ")" );
	}

	// (B) "Please retry after 5 seconds." -> 5000ms, case-INSENSITIVE
	//     ("RETRY AFTER") and an integer N.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T20(B): param_edit loads" );

		MockTransport mock;
		mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limited. Please RETRY AFTER 5 seconds.\"}}", "", 2 } );
		mock.responses.push_back( { 200, kBodyFinal, "", 4 } );

		std::vector<int64_t> waits;
		AgentEvalLiveRunOptions opts;
		opts.runDir = ScratchRunDir( "t20b_live_429_hint_retry_after" );
		opts.transport = &mock;
		opts.provider = ChatProvider::Anthropic;
		opts.apiKey = "unit-test-key-not-real";
		opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

		AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
		Check( h.result.terminalStatus == "final_text", "T20(B): the hinted-429-then-200 round reaches final_text" );
		Check( waits.size() == 1 && waits[0] == 5000,
		       "T20(B): 'RETRY AFTER 5 seconds' parses case-insensitively to a 5000ms wait (got " +
		       ( waits.empty() ? std::string( "<none>" ) : std::to_string( waits[0] ) ) + ")" );
	}
}

//----------------------------------------------------------------------
// T21: HTTP 429 exponential fallback -- two hint-less 429s in a row, then
//      success, wait 10000ms then 20000ms (the fallback table's first two
//      entries), not a repeat of the same wait.
//----------------------------------------------------------------------
static void TestLive429ExponentialFallback()
{
	std::printf( "T21: HTTP 429 exponential fallback (10000ms, then 20000ms)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limit reached. Limit 500000, Used 488104.\"}}", "", 1 } );
	mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limit reached. Limit 500000, Used 499999.\"}}", "", 1 } );
	mock.responses.push_back( { 200, kBodyFinal, "", 4 } );

	std::vector<int64_t> waits;
	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t21_live_429_exponential_fallback" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";
	opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text", "two hint-less 429s then 200 still reaches final_text" );
	Check( mock.seenRequests.size() == 3, "the transport saw exactly 3 POSTs (2 rate-limited + 1 success)" );
	Check( waits.size() == 2, "exactly 2 backoff waits were requested" );
	Check( waits.size() > 0 && waits[0] == 10000, "attempt 1's fallback wait is 10000ms" );
	Check( waits.size() > 1 && waits[1] == 20000, "attempt 2's fallback wait is 20000ms" );
}

//----------------------------------------------------------------------
// T22: HTTP 429 attempts are capped at 5 per round -- 5 consecutive 429s
//      exhaust the retry budget and fail with provider_error naming
//      "rate limit", having asked the sleeper for a wait after EACH of
//      the 5 attempts (the trajectory records an honest attempt/retry_of
//      sibling llm record for every one of them, mirroring the 5xx-retry
//      convention).
//----------------------------------------------------------------------
static void TestLive429ExhaustedAttempts()
{
	std::printf( "T22: 5x HTTP 429 -> provider_error naming rate limit, 5 waits recorded...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	for( int i = 0; i < 5; ++i )
		mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limit reached. Limit 500000, Used 488104.\"}}", "", 1 } );

	std::vector<int64_t> waits;
	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t22_live_429_exhausted" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";
	opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "provider_error",
	       "5 consecutive 429s exhaust the retry budget -> provider_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.errorMessage.find( "rate limit" ) != std::string::npos,
	       "the error message names \"rate limit\" (got '" + h.result.errorMessage + "')" );
	Check( mock.seenRequests.size() == 5, "the transport saw exactly 5 POSTs (the max attempts)" );
	Check( waits.size() == 5, "exactly 5 backoff waits were requested (one per 429, including the last before giving up)" );
	Check( h.result.llmCalls == 0, "none of the 5 429 attempts consumed llmCalls" );

	// ChatTrajectory.cpp OMITS "retry_of" entirely when retryOf < 0 (see
	// ChatTrajectory.h: "-1 = none") -- so attempt 1's record carries NO
	// retry_of key at all (matching every other retry family's attempt-1
	// record, e.g. T12's).  Only attempts 2-5 (retryOf >= 0) carry it.
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	std::vector<int> attempts, retryOfs;
	for( std::size_t i = 0; i < recs.size(); ++i ) {
		if( recs[i].get( "run_type" ).asString() != "llm" ) continue;
		attempts.push_back( static_cast<int>( recs[i].get( "attempt" ).asNumber( -1.0 ) ) );
		retryOfs.push_back( static_cast<int>( recs[i].get( "retry_of" ).asNumber( -1.0 ) ) );
	}
	Check( attempts.size() == 5, "the trajectory carries an honest llm record for each of the 5 429 attempts" );
	if( attempts.size() == 5 && retryOfs.size() == 5 ) {
		Check( attempts[0] == 1, "attempt 1 (no retry_of key -- first try)" );
		Check( attempts[1] == 2 && retryOfs[1] == 1, "attempt 2 / retry_of 1" );
		Check( attempts[2] == 3 && retryOfs[2] == 2, "attempt 3 / retry_of 2" );
		Check( attempts[3] == 4 && retryOfs[3] == 3, "attempt 4 / retry_of 3" );
		Check( attempts[4] == 5 && retryOfs[4] == 4, "attempt 5 / retry_of 4" );
	}
}

//----------------------------------------------------------------------
// T23: an HTTP 429 backoff wait that would exceed the scenario's REMAINING
//      maxWallMs budget stops the run honestly with terminalStatus
//      "budget_wall_ms" -- never a sleep call longer than what remains
//      (here: no sleep call at all, since the very first computed wait
//      already exceeds the budget).
//----------------------------------------------------------------------
static void TestLive429WallBudgetStop()
{
	std::printf( "T23: HTTP 429 backoff respects the remaining maxWallMs budget...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	scenario.budgets.maxWallMs = 3000;   // well under the 10000ms fallback wait attempt 1 would need

	MockTransport mock;
	mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limit reached. Limit 500000, Used 488104.\"}}", "", 1 } );
	mock.responses.push_back( { 200, kBodyFinal, "", 4 } );   // never reached

	std::vector<int64_t> waits;
	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t23_live_429_wall_budget" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";
	opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "budget_wall_ms",
	       "a 429 whose backoff wait would exceed the remaining wall budget stops honestly (got '" + h.result.terminalStatus + "')" );
	Check( h.result.budgetHit, "budgetHit is set" );
	Check( mock.seenRequests.size() == 1, "only the original 429 POST was sent -- the retry never happened" );
	Check( waits.empty(), "no sleep was dispatched -- the wait would have exceeded the remaining budget" );
}

//----------------------------------------------------------------------
// T24: HTTP 429 retries do NOT consume maxLlmCalls -- a scenario capped at
//      exactly the 2 REAL (successful) rounds it needs still reaches
//      final_text despite an extra 429 attempt ahead of the first one.
//----------------------------------------------------------------------
static void TestLive429DoesNotConsumeLlmCallsBudget()
{
	std::printf( "T24: HTTP 429 retries do not consume maxLlmCalls...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	scenario.budgets.maxLlmCalls = 2;   // exactly the 2 REAL rounds the scenario needs

	MockTransport mock;
	mock.responses.push_back( { 429, "{\"error\":{\"message\":\"Rate limit reached. Limit 500000, Used 488104.\"}}", "", 1 } );
	mock.responses.push_back( { 200, kBodyToolUse, "", 3 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 4 } );

	std::vector<int64_t> waits;
	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t24_live_429_llm_calls_budget" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";
	opts.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "a maxLlmCalls:2 scenario survives a 429-then-200 first round + a normal second round (got '" +
	       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "llmCalls counts only the 2 SUCCESSFUL rounds, not the 429" );
	Check( mock.seenRequests.size() == 3, "3 POSTs reached the transport (1 rate-limited + 2 successful)" );
	Check( waits.size() == 1, "exactly one backoff wait for the single 429" );
}

//----------------------------------------------------------------------
// T25: RunEvalMatrix pre-flight auth probe: an HTTP 429 probe response is
//      NOT classified as a dead credential (429 is neither 401/403 nor a
//      body matching the auth-failure phrases) and the probe itself is
//      SINGLE-SHOT -- it never backs off, so the column proceeds straight
//      into its scenario x repeat loop.
//----------------------------------------------------------------------
static void TestRunEvalMatrixAuthProbe429NotFatal()
{
	std::printf( "T25: RunEvalMatrix auth probe: HTTP 429 is not auth-fatal and is not backed off...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T25: param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	const std::string kFakeKey = "sk-ant-AUTHPROBE-429-FAKEKEY-DO-NOT-LEAK";
	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t25_authprobe_429" );

	MockTransport mock;
	mock.responses.push_back( { 429,
		"{\"error\":{\"message\":\"Rate limit reached for requests. Limit 500000, Used 488104.\"}}", "", 1 } );   // the auth probe
	mock.responses.push_back( { 200, kBodyToolUse, "", 2 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 2 } );

	std::vector<int64_t> waits;
	AgentEvalMatrixOptions mo;
	mo.transport = &mock;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};
	mo.sleeper = [&]( int64_t ms ) { waits.push_back( ms ); };

	AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
	Check( mr.providersUsed == 1 && mr.providersSkipped == 0,
	       "T25: a 429 probe response leaves the column USED (not auth-fatal)" );
	Check( mr.providersAuthFailed == 0, "T25: providersAuthFailed == 0" );
	Check( mr.runsExecuted == 1, "T25: the column executed" );
	Check( !mock.seenRequests.empty() && mock.seenRequests[0].body.find( "ping" ) != std::string::npos,
	       "T25: the FIRST captured request is the auth probe (body contains \"ping\")" );
	Check( mock.seenRequests.size() == 3,
	       "T25: exactly 1 probe POST + 2 real-run POSTs reached the transport -- no probe backoff" );
	Check( waits.empty(), "T25: the sleeper was never invoked -- the probe's 429 is not retried" );

	std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
	Check( !manifest.empty(), "T25: run.manifest.jsonl has a record" );
	if( !manifest.empty() ) {
		Check( manifest.back().get( "authProbes" ).get( "anthropic" ).asString() == "ok",
		       "T25: manifest records authProbes.anthropic == \"ok\" (a 429 probe is not auth-fatal)" );
	}
}

//----------------------------------------------------------------------
// T5: KEY-HYGIENE red-prove -- a fake key reaches the wire but no output.
//----------------------------------------------------------------------
static void TestKeyHygieneRedProve()
{
	std::printf( "T5: key-hygiene red-prove (fake key never in any output)...\n" );

	const std::string kFakeKey = "sk-ant-FAKEKEY-DEADBEEF-DO-NOT-LEAK-0123456789";

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 200, kBodyToolUse, "", 4 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 4 } );

	const std::string runDir = ScratchRunDir( "t5_key_hygiene" );
	AgentEvalLiveRunOptions opts;
	opts.runDir = runDir;
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = kFakeKey;

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	CheckScenario( h, scenario );   // also writes results.jsonl under runDir

	// (1) The key DID reach the wire (the auth header the transport received) --
	//     otherwise the red-prove would be vacuous.
	Check( mock.AnyRequestHeaderContains( kFakeKey ),
	       "the fake key rode the request auth header to the transport (red-prove is meaningful)" );

	// (2) The key appears in NONE of the runDir output files (trajectory,
	//     result, check-result) -- E1's auth-strip + regex scrub + the
	//     transport never echoing headers into its response.
	Check( !AnyFileUnderContains( runDir, kFakeKey ),
	       "the fake key appears in NO runDir output file (trajectory/result/results.jsonl)" );
}

//----------------------------------------------------------------------
// T11: FIX 1 -- the LOCAL provider's raised 900s transport timeout budget
//      reaches the (mock) transport, end-to-end through RunScenarioLive
//      (not just the codec-level unit test in AgentChatLoopTest.cpp).
//----------------------------------------------------------------------
static void TestLiveLocalProviderTimeoutBudget()
{
	std::printf( "T11: local provider's 900s timeout budget reaches the transport (FIX 1)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	// A single OpenAI-shaped final-text turn (the local provider reuses the
	// OpenAI-compatible codec) ends the loop in one round.
	mock.responses.push_back( { 200,
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
		"\"finish_reason\":\"stop\"}],"
		"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 2 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t11_live_local_timeout" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Local;
	opts.apiKey = "";   // keyless local

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "local live run reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( !mock.seenRequests.empty() && mock.seenRequests[0].timeoutSeconds == 900,
	       "the request the (mock) transport received carries timeoutSeconds==900 for the local provider" );
}

//----------------------------------------------------------------------
// T12: FIX 2 -- one automatic retry on a transient HTTP 5xx.  A 500 on the
//      first attempt of a round is retried ONCE (same round, rebuilt
//      request); the second attempt succeeds -> the scenario reaches
//      final_text, having driven 2 llm rounds (an honest attempt-2/
//      retry-of-1 sibling trajectory record), matching the observed
//      Ollama-500-then-retry-succeeds shootout behaviour.
//----------------------------------------------------------------------
static void TestLiveHttp5xxRetrySucceeds()
{
	std::printf( "T12: HTTP 500 then success -> one retry, round succeeds (FIX 2)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 500, "{\"error\":\"internal server error\"}", "", 3 } );
	mock.responses.push_back( { 200, kBodyFinal, "", 4 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t12_live_5xx_retry_succeeds" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "a 500-then-200 round still reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "both the 500 attempt and the retry are counted as llm rounds" );
	Check( mock.seenRequests.size() == 2, "the transport saw exactly 2 POSTs (the retry rebuilt the SAME round)" );
	Check( h.result.finalText == "Done: pnt_albedo is now red.", "the retried round's final text is captured" );

	// The trajectory carries an honest sibling llm record: attempt 1 (the
	// 500) and attempt 2 / retry_of 1 (the successful retry).
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	int attempt1 = 0, attempt2RetryOf1 = 0;
	for( std::size_t i = 0; i < recs.size(); ++i ) {
		if( recs[i].get( "run_type" ).asString() != "llm" ) continue;
		const int at = static_cast<int>( recs[i].get( "attempt" ).asNumber( -1.0 ) );
		if( at == 1 ) ++attempt1;
		if( at == 2 && recs[i].get( "retry_of" ).asNumber( -1.0 ) == 1.0 ) ++attempt2RetryOf1;
	}
	Check( attempt1 == 1, "exactly one attempt-1 llm record (the 500)" );
	Check( attempt2RetryOf1 == 1, "exactly one attempt-2/retry_of-1 llm record (the successful retry)" );
}

//----------------------------------------------------------------------
// T13: FIX 2 -- a SECOND 5xx (500-then-500) is NOT retried again; the round
//      falls through to the unchanged provider_error path.  Proves the
//      retry is bounded to exactly one attempt per round, not a loop.
//----------------------------------------------------------------------
static void TestLiveHttp5xxRetryStillFails()
{
	std::printf( "T13: HTTP 500 then 500 -> one retry, still provider_error (FIX 2)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 500, "{\"error\":\"internal server error\"}", "", 3 } );
	mock.responses.push_back( { 500, "{\"error\":\"internal server error again\"}", "", 3 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t13_live_5xx_retry_fails" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "provider_error",
	       "500-then-500 falls through to provider_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.llmCalls == 2, "exactly 2 llm rounds counted (the original attempt + the one retry, no more)" );
	Check( mock.seenRequests.size() == 2,
	       "the transport saw exactly 2 POSTs -- the retry is bounded to ONE attempt, not a loop" );
}

//----------------------------------------------------------------------
// T14: FIX 2 -- a plain (non-multimodal) HTTP 400 is NOT retried.  Proves
//      the 5xx retry is scoped to [500,599] and does not widen to 4xx.
//----------------------------------------------------------------------
static void TestLiveHttp400NotRetried()
{
	std::printf( "T14: a non-multimodal HTTP 400 is NOT retried (FIX 2 scope)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 400, "{\"error\":{\"message\":\"invalid request: unknown field foo\"}}", "", 2 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t14_live_400_no_retry" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "provider_error",
	       "a plain 400 -> provider_error immediately (got '" + h.result.terminalStatus + "')" );
	Check( h.result.llmCalls == 1, "exactly 1 llm round -- a 4xx (non-multimodal) is never retried" );
	Check( mock.seenRequests.size() == 1, "the transport saw exactly 1 POST -- no retry attempt" );
}

//----------------------------------------------------------------------
// T15: provider-compat fix -- an OpenAI-family reasoning model's 400
//      "Function tools with reasoning_effort are not supported ... To use
//      function tools, use /v1/responses or set reasoning_effort to
//      'none'." (observed on every gpt-5.6-terra live eval round) is
//      retried ONCE with an explicit "reasoning_effort":"none" override;
//      the second attempt succeeds -> the scenario reaches final_text,
//      having driven 2 llm rounds (an honest attempt-2/retry-of-1 sibling
//      trajectory record), proving the RunScenarioLive attempt-loop wiring
//      (not just the AgentChatLoopTest.cpp T34 codec-level unit test).
//----------------------------------------------------------------------
static void TestLiveReasoningEffort400RetrySucceeds()
{
	std::printf( "T15: reasoning_effort-400 then success -> one retry, round succeeds (provider-compat fix)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 400,
		"{\"error\":{\"message\":\"Function tools with reasoning_effort are "
		"not supported for gpt-5.6-terra in /v1/chat/completions. To use "
		"function tools, use /v1/responses or set reasoning_effort to "
		"'none'.\",\"type\":\"invalid_request_error\","
		"\"param\":\"reasoning_effort\",\"code\":null}}", "", 3 } );
	mock.responses.push_back( { 200,
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
		"\"finish_reason\":\"stop\"}],"
		"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 4 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t15_live_reasoning_effort_retry_succeeds" );
	opts.transport = &mock;
	opts.provider = ChatProvider::OpenAI;
	// A model id UNIQUE to this 400-then-retry sequence (T15b uses its own;
	// T15c REUSES this one on purpose): AgentChatLoop now memoizes "this
	// (provider,model) needs reasoning_effort:none" in a PROCESS-WIDE
	// cache (see ReasoningEffortNoneAlreadyKnown in AgentChatLoop.cpp) so
	// later sessions against the SAME pair skip the 400 entirely --
	// exactly what T15c below proves against THIS model id.  Reusing the
	// codec's default model id here would make this test's own
	// 400-then-retry sequence order-dependent on whatever OTHER test in
	// this binary touched that model id first.
	opts.modelId = "gpt-5.6-terra-t15-unit-test";
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "a reasoning_effort-400-then-200 round still reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "both the 400 attempt and the retry are counted as llm rounds" );
	Check( mock.seenRequests.size() == 2, "the transport saw exactly 2 POSTs (the retry rebuilt the SAME round)" );

	// The FIRST request carries no reasoning_effort key (this codec never
	// sends one on its own); the RETRY explicitly overrides it to "none".
	Check( mock.seenRequests[0].body.find( "reasoning_effort" ) == std::string::npos,
	       "the first request carries no reasoning_effort key" );
	Check( mock.seenRequests[1].body.find( "\"reasoning_effort\":\"none\"" ) != std::string::npos,
	       "the retry request explicitly sets reasoning_effort:\"none\"" );

	// The trajectory carries an honest sibling llm record: attempt 1 (the
	// 400) and attempt 2 / retry_of 1 (the successful retry).
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	int attempt1 = 0, attempt2RetryOf1 = 0;
	for( std::size_t i = 0; i < recs.size(); ++i ) {
		if( recs[i].get( "run_type" ).asString() != "llm" ) continue;
		const int at = static_cast<int>( recs[i].get( "attempt" ).asNumber( -1.0 ) );
		if( at == 1 ) ++attempt1;
		if( at == 2 && recs[i].get( "retry_of" ).asNumber( -1.0 ) == 1.0 ) ++attempt2RetryOf1;
	}
	Check( attempt1 == 1, "exactly one attempt-1 llm record (the 400)" );
	Check( attempt2RetryOf1 == 1, "exactly one attempt-2/retry_of-1 llm record (the successful retry)" );
}

//----------------------------------------------------------------------
// T15b: the reasoning_effort-400 retry (T15) must preserve a user-message
// image attachment across the rebuilt request -- a vision-shaped mirror of
// T15, combined with T13's attachment setup.  Guards against a retry path
// that rebuilds the request body WITHOUT the attachments (a live-baseline
// hypothesis: gpt-5.6-terra vision runs die inconsistently on the reasoning-
// effort 400, which was traced to confirm the retry genuinely re-sends the
// SAME transcript, images included -- not a rebuild-from-scratch that could
// silently drop them).
//----------------------------------------------------------------------
static void TestLiveReasoningEffort400RetryPreservesImages()
{
	std::printf( "T15b: reasoning_effort-400 retry preserves a user image attachment...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T15b: param_edit loads" );

	// Same committed PNG T13 uses -- read-only, no scratch-file synthesis.
	const std::string pngPath = "textures/cel.png";
	std::string pngBytes;
	{
		std::ifstream f( pngPath.c_str(), std::ios::binary );
		Check( static_cast<bool>( f ), "T15b: textures/cel.png is readable from the repo-root cwd" );
		std::ostringstream ss; ss << f.rdbuf();
		pngBytes = ss.str();
	}
	Check( !pngBytes.empty(), "T15b: textures/cel.png is non-empty" );
	Check( scenario.prompts.size() == 1, "T15b: param_edit carries exactly 1 prompt to attach to" );
	scenario.prompts[0].imagePaths.push_back( pngPath );

	MockTransport mock;
	mock.responses.push_back( { 400,
		"{\"error\":{\"message\":\"Function tools with reasoning_effort are "
		"not supported for gpt-5.6-terra in /v1/chat/completions. To use "
		"function tools, use /v1/responses or set reasoning_effort to "
		"'none'.\",\"type\":\"invalid_request_error\","
		"\"param\":\"reasoning_effort\",\"code\":null}}", "", 3 } );
	mock.responses.push_back( { 200,
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
		"\"finish_reason\":\"stop\"}],"
		"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 4 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t15b_live_reasoning_effort_retry_preserves_images" );
	opts.transport = &mock;
	opts.provider = ChatProvider::OpenAI;
	// UNIQUE model id (see T15's comment): this test's precondition is an
	// UN-armed session that genuinely receives the 400 -- a model id some
	// earlier test already 400ed would be pre-armed by the process-wide
	// capability cache and never see the 400 at all.
	opts.modelId = "gpt-5.6-terra-t15b-unit-test";
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "T15b: a reasoning_effort-400-then-200 vision round still reaches final_text (got '" +
	       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "T15b: both the 400 attempt and the retry are counted as llm rounds" );
	Check( mock.seenRequests.size() == 2, "T15b: the transport saw exactly 2 POSTs (the retry rebuilt the SAME round)" );

	const std::vector<unsigned char> rawBytes( pngBytes.begin(), pngBytes.end() );
	const std::string expectedB64 = Base64Encode( rawBytes );

	Check( mock.seenRequests.size() == 2 &&
	       mock.seenRequests[0].body.find( expectedB64 ) != std::string::npos,
	       "T15b: the FIRST (400) request body already carries the reference image's base64 payload" );
	Check( mock.seenRequests[0].body.find( "reasoning_effort" ) == std::string::npos,
	       "T15b: the first request carries no reasoning_effort key" );

	Check( mock.seenRequests.size() == 2 &&
	       mock.seenRequests[1].body.find( "\"reasoning_effort\":\"none\"" ) != std::string::npos,
	       "T15b: the retry request explicitly sets reasoning_effort:\"none\"" );
	Check( mock.seenRequests.size() == 2 &&
	       mock.seenRequests[1].body.find( expectedB64 ) != std::string::npos,
	       "T15b: the RETRY request body STILL carries the reference image's base64 payload -- the "
	       "rebuild did not drop the attachment" );
}

//----------------------------------------------------------------------
// T15c: PROCESS-WIDE reasoning-effort memoization at the RUNNER level --
// the root-cause fix for the vision-baseline waste.  T15 above already
// taught the process-wide capability cache (see
// ReasoningEffortNoneAlreadyKnown in AgentChatLoop.cpp) that its model id
// needs reasoning_effort:"none"; a SECOND RunScenarioLive against the
// SAME (provider, model) -- a brand-new AgentChatLoop, exactly like every
// new eval cell or GUI session -- must now start pre-armed: ONE POST,
// no 400, the first request already carrying the override.  Before the
// memoization, EVERY session against a gpt-5.6-terra-class model re-paid
// the wasted 400 round-trip (re-sending the full image payload on vision
// scenarios -- the token cost that rate-limited the live baseline).
// ORDER DEPENDENCY (deliberate): this test MUST run after T15 in main();
// it shares T15's model id because the shared cache entry IS the thing
// under test.
//----------------------------------------------------------------------
static void TestLiveReasoningEffortMemoizedAcrossSessions()
{
	std::printf( "T15c: reasoning_effort lesson is memoized process-wide -- a later session never pays the 400...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T15c: param_edit loads" );

	// ONE canned response only: the run must never receive a 400, so a
	// single 200 final-text is the whole conversation.
	MockTransport mock;
	mock.responses.push_back( { 200,
		"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
		"\"finish_reason\":\"stop\"}],"
		"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 4 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t15c_live_reasoning_effort_memoized" );
	opts.transport = &mock;
	opts.provider = ChatProvider::OpenAI;
	opts.modelId = "gpt-5.6-terra-t15-unit-test";   // T15's model id -- the shared cache entry under test
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "T15c: the pre-armed session reaches final_text (got '" +
	       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 1, "T15c: exactly ONE llm round -- the wasted 400 round-trip is gone" );
	Check( mock.seenRequests.size() == 1, "T15c: the transport saw exactly 1 POST" );
	Check( !mock.seenRequests.empty() &&
	       mock.seenRequests[0].body.find( "\"reasoning_effort\":\"none\"" ) != std::string::npos,
	       "T15c: the FIRST request already carries reasoning_effort:\"none\" (pre-armed from the cache)" );
}

//----------------------------------------------------------------------
// T6: LoadEvalRunConfig -- happy path + malformed refusals.
//----------------------------------------------------------------------
static void TestRunConfigLoad()
{
	std::printf( "T6: LoadEvalRunConfig parse + malformed refusals...\n" );
	const std::string dir = ScratchRunDir( "t6_runconfig" );

	// Happy path.
	{
		const std::string path = dir + "/ok.json";
		WriteFile( path,
			"{\"scenarios\":[\"evals/scenarios/param_edit.json\",\"evals/scenarios/*.json\"],"
			"\"providers\":[{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-5\",\"keyEnvVar\":\"ANTHROPIC_API_KEY\"},"
			"{\"provider\":\"openai\",\"keyEnvVar\":\"OPENAI_API_KEY\"}],"
			"\"repeats\":5,\"runDir\":\"evals/runs/x\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ), "well-formed run config loads (" + err + ")" );
		Check( cfg.scenarios.size() == 2, "two scenario entries captured (path + glob, verbatim)" );
		Check( cfg.providers.size() == 2, "two provider entries captured" );
		Check( cfg.providers[0].provider == "anthropic" && cfg.providers[0].model == "claude-sonnet-5" &&
		       cfg.providers[0].keyEnvVar == "ANTHROPIC_API_KEY", "provider[0] fields captured" );
		Check( cfg.providers[1].model.empty(), "provider[1] omitted model -> empty (codec default)" );
		Check( cfg.repeats == 5, "repeats captured" );
		Check( cfg.runDir == "evals/runs/x", "runDir captured" );
	}

	// Default repeats.
	{
		const std::string path = dir + "/default_repeats.json";
		WriteFile( path,
			"{\"scenarios\":[\"a.json\"],\"providers\":[{\"provider\":\"gemini\",\"keyEnvVar\":\"K\"}],"
			"\"runDir\":\"d\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ), "config without repeats loads" );
		Check( cfg.repeats == 3, "repeats defaults to 3" );
	}

	// xAI provider (keyEnvVar required, same as the cloud providers).
	{
		const std::string path = dir + "/xai.json";
		WriteFile( path,
			"{\"scenarios\":[\"a.json\"],\"providers\":[{\"provider\":\"xai\",\"model\":\"grok-4.5\",\"keyEnvVar\":\"XAI_API_KEY\"}],"
			"\"runDir\":\"d\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ), "xai run config loads (" + err + ")" );
		Check( cfg.providers.size() == 1 && cfg.providers[0].provider == "xai" &&
		       cfg.providers[0].keyEnvVar == "XAI_API_KEY", "xai provider fields captured" );
	}

	// local provider WITHOUT keyEnvVar -> loads (keyless local is allowed).
	{
		const std::string path = dir + "/local_keyless.json";
		WriteFile( path,
			"{\"scenarios\":[\"a.json\"],\"providers\":[{\"provider\":\"local\",\"model\":\"qwen3:32b\"}],"
			"\"runDir\":\"d\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ), "keyless local run config loads (" + err + ")" );
		Check( cfg.providers.size() == 1 && cfg.providers[0].provider == "local" &&
		       cfg.providers[0].keyEnvVar.empty(), "local provider loads with an EMPTY keyEnvVar" );
	}

	// local provider WITH keyEnvVar -> also allowed (a --api-key local server).
	{
		const std::string path = dir + "/local_keyed.json";
		WriteFile( path,
			"{\"scenarios\":[\"a.json\"],\"providers\":[{\"provider\":\"local\",\"keyEnvVar\":\"LOCAL_LLM_KEY\"}],"
			"\"runDir\":\"d\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ), "keyed local run config loads (" + err + ")" );
		Check( cfg.providers[0].keyEnvVar == "LOCAL_LLM_KEY", "local provider may still name a keyEnvVar" );
	}

	// Multiple provider entries sharing the SAME provider name but DISTINCT
	// models -> ALL load (LoadEvalRunConfig has no provider-NAME dedup; a
	// local-model shootout config legitimately wants several "local" rows,
	// keyed by (provider, model), not by provider name alone -- see the
	// committed evals/runconfigs/local_shootout.json for the real case).
	{
		const std::string path = dir + "/local_multi_model.json";
		WriteFile( path,
			"{\"scenarios\":[\"a.json\"],\"providers\":["
			"{\"provider\":\"local\",\"model\":\"qwen3:32b\"},"
			"{\"provider\":\"local\",\"model\":\"qwen3.6:27b\"},"
			"{\"provider\":\"local\",\"model\":\"qwen3-coder:30b\"},"
			"{\"provider\":\"local\",\"model\":\"llama3.3:70b-instruct-q4_K_M\"}],"
			"\"runDir\":\"d\"}" );
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( path, cfg, err ),
		       "4 same-provider-name/distinct-model entries ALL load (" + err + ")" );
		Check( cfg.providers.size() == 4, "all 4 distinct-model 'local' provider rows are captured, none dropped/merged" );
	}

	// The committed local_shootout.json (8 provider rows: gemini + openai +
	// xai + 5 distinct-model local) loads cleanly end-to-end.
	{
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( "evals/runconfigs/local_shootout.json", cfg, err ),
		       "evals/runconfigs/local_shootout.json loads (" + err + ")" );
		Check( cfg.scenarios.size() == 5, "local_shootout.json: 5 scenarios (reserved_name_recovery + its reserved_name_clarify sibling)" );
		Check( cfg.providers.size() == 8, "local_shootout.json: 8 providers (gemini + openai + xai + 5 distinct-model local)" );
		int localCount = 0;
		for( std::size_t i = 0; i < cfg.providers.size(); ++i )
			if( cfg.providers[i].provider == "local" ) ++localCount;
		Check( localCount == 5, "local_shootout.json: 5 'local' rows survive loading (not deduped by provider name)" );
		Check( cfg.repeats == 3, "local_shootout.json: repeats == 3" );
		Check( cfg.runDir == "evals/runs/local_shootout", "local_shootout.json: runDir captured" );
	}

	// Malformed refusals.
	auto refuse = [&]( const char* leaf, const std::string& json, const std::string& label ) {
		const std::string path = dir + "/" + leaf;
		WriteFile( path, json );
		AgentEvalRunConfig cfg; std::string err;
		const bool ok = LoadEvalRunConfig( path, cfg, err );
		Check( !ok, std::string( label ) + ": refused" );
		Check( !err.empty(), std::string( label ) + ": carries a message" );
	};

	refuse( "no_scenarios.json",
		"{\"providers\":[{\"provider\":\"anthropic\",\"keyEnvVar\":\"K\"}],\"runDir\":\"d\"}",
		"missing scenarios" );
	refuse( "empty_scenarios.json",
		"{\"scenarios\":[],\"providers\":[{\"provider\":\"anthropic\",\"keyEnvVar\":\"K\"}],\"runDir\":\"d\"}",
		"empty scenarios" );
	refuse( "no_providers.json",
		"{\"scenarios\":[\"a\"],\"runDir\":\"d\"}",
		"missing providers" );
	refuse( "bad_provider_name.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"banana\",\"keyEnvVar\":\"K\"}],\"runDir\":\"d\"}",
		"unknown provider name" );
	refuse( "no_keyenv.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"anthropic\"}],\"runDir\":\"d\"}",
		"provider missing keyEnvVar" );
	refuse( "xai_no_keyenv.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"xai\"}],\"runDir\":\"d\"}",
		"non-local (xai) missing keyEnvVar is still refused" );
	refuse( "local_empty_keyenv.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"local\",\"keyEnvVar\":\"\"}],\"runDir\":\"d\"}",
		"present-but-empty keyEnvVar refused even for local" );
	refuse( "no_rundir.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"anthropic\",\"keyEnvVar\":\"K\"}]}",
		"missing runDir" );
	refuse( "bad_repeats.json",
		"{\"scenarios\":[\"a\"],\"providers\":[{\"provider\":\"anthropic\",\"keyEnvVar\":\"K\"}],"
		"\"repeats\":0,\"runDir\":\"d\"}",
		"repeats < 1" );
}

//----------------------------------------------------------------------
// T7: RunEvalMatrix -- execute-with-key + missing-key SKIP + layout.
//----------------------------------------------------------------------
static void TestRunEvalMatrix()
{
	std::printf( "T7: RunEvalMatrix execute + missing-key skip + per-run layout...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	const std::string kFakeKey = "sk-ant-MATRIX-FAKEKEY-DO-NOT-LEAK-abcdef";

	// (A) Provider WITH a key -> the run executes; per-run subdir + files exist.
	{
		AgentEvalRunConfig cfg;
		cfg.scenarios.push_back( "evals/scenarios/param_edit.json" );   // (unused by RunEvalMatrix -- scenarios are pre-loaded)
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t7_matrix_exec" );

		MockTransport mock;
		mock.responses.push_back( { 200, "{}", "", 1 } );   // the pre-flight auth probe (credential looks live)
		mock.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock.repeatLast = true;

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = [&]( const std::string& name ) -> const char* {
			return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
		};

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 1 && mr.providersSkipped == 0, "provider with a key is USED" );
		Check( mr.providersAuthFailed == 0, "a passing auth probe leaves providersAuthFailed == 0" );
		Check( mr.runsExecuted == 1 && mr.runsSkipped == 0, "1 scenario x 1 provider x 1 repeat = 1 run executed" );

		// The documented per-run layout: <runDir>/<id>__anthropic__<model>__r1/.
		const std::string runSub =
			( std::filesystem::path( cfg.runDir ) / "param_edit__anthropic__claude-sonnet-5__r1" ).string();
		Check( std::filesystem::exists( std::filesystem::path( runSub ) / "param_edit.trajectory.jsonl" ),
		       "per-run trajectory.jsonl exists at the documented subdir path" );
		Check( std::filesystem::exists( std::filesystem::path( runSub ) / "param_edit.result.jsonl" ),
		       "per-run result.jsonl exists" );
		Check( std::filesystem::exists( std::filesystem::path( runSub ) / "results.jsonl" ),
		       "per-run results.jsonl (E3 check output) exists" );

		// Matrix-level key hygiene: the fake key is in NO output file.
		Check( !AnyFileUnderContains( cfg.runDir, kFakeKey ),
		       "matrix run: the fake key appears in NO output file under runDir" );
	}

	// (B) Provider WITHOUT a key -> the whole column is SKIPPED, nothing runs.
	{
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.keyEnvVar = "RISE_TEST_MISSING_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 3;
		cfg.runDir = ScratchRunDir( "t7_matrix_skip" );

		MockTransport mock;   // must never be called

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = []( const std::string& ) -> const char* { return nullptr; };   // key unset

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 0 && mr.providersSkipped == 1, "keyless provider is SKIPPED (not used)" );
		Check( mr.runsExecuted == 0, "no runs executed for a keyless provider" );
		Check( mr.runsSkipped == static_cast<int>( scenarios.size() ) * 3, "skip count = scenarios x repeats" );
		Check( mock.seenRequests.empty(), "the transport was NEVER called for a keyless provider" );
	}

	// (C) The "local" provider with NO keyEnvVar is KEYLESS BY DESIGN, not
	//     missing -> it is NOT skipped: the run executes against the mock, and
	//     the request carries NO Authorization header (the key-hygiene
	//     inverse, proven end-to-end through the matrix).  local reuses the
	//     OpenAI codec, so the canned response is OpenAI-shaped.
	{
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "local"; p.model = "qwen3:32b"; /* keyEnvVar left EMPTY */
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t7_matrix_local_keyless" );

		MockTransport mock;
		// One OpenAI-shaped final-text turn ends the loop in a single round.
		mock.responses.push_back( { 200,
			"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
			"\"finish_reason\":\"stop\"}],"
			"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 2 } );
		mock.repeatLast = true;

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		// envLookup must NEVER be consulted for a keyless local provider, but
		// provide a benign one anyway (returning nullptr).
		mo.envLookup = []( const std::string& ) -> const char* { return nullptr; };

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 1 && mr.providersSkipped == 0,
		       "keyless LOCAL provider is USED (not skipped -- keyless by design)" );
		Check( mr.runsExecuted == 1, "the keyless local run executes against the mock transport" );
		Check( !mock.seenRequests.empty(), "the transport WAS called for the keyless local provider" );
		bool anyAuth = false;
		for( std::size_t i = 0; i < mock.seenRequests.size(); ++i )
			for( std::size_t h = 0; h < mock.seenRequests[i].headers.size(); ++h )
				if( mock.seenRequests[i].headers[h].first == "authorization" ) anyAuth = true;
		Check( !anyAuth, "keyless local matrix request carries NO Authorization header" );
	}

	// (D) Defense-in-depth: a non-"local" provider with an EMPTY keyEnvVar,
	//     built PROGRAMMATICALLY (bypassing LoadEvalRunConfig, which is what
	//     enforces "only local may omit keyEnvVar" on the config-file path)
	//     -> RunEvalMatrix must still SKIP the column loudly rather than run
	//     keyless against a live cloud endpoint.  Proves (a) the skip fires
	//     with the same counters as the missing-key case and (b) the mock
	//     transport is NEVER called.
	{
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; /* keyEnvVar left EMPTY, no LoadEvalRunConfig involved */
		cfg.providers.push_back( p );
		cfg.repeats = 2;
		cfg.runDir = ScratchRunDir( "t7_matrix_empty_keyenvvar" );

		MockTransport mock;   // must never be called

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = []( const std::string& ) -> const char* { return nullptr; };   // must never be consulted

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 0 && mr.providersSkipped == 1,
		       "non-local provider with empty keyEnvVar is SKIPPED (defense-in-depth)" );
		Check( mr.runsExecuted == 0, "no runs executed for an empty-keyEnvVar non-local provider" );
		Check( mr.runsSkipped == static_cast<int>( scenarios.size() ) * 2, "skip count = scenarios x repeats" );
		Check( mock.seenRequests.empty(), "the transport was NEVER called for an empty-keyEnvVar non-local provider" );
	}
}

//----------------------------------------------------------------------
// T7b: RunEvalMatrix emits <runDir>/run.manifest.jsonl -- the per-run
//      reproducibility manifest, an APPEND-ONLY provenance log (one JSON
//      record per line per RunEvalMatrix invocation into a runDir).
//      Proves the injected clock feeds createdUtcMs, the
//      git/build/config/scenario/provider/result fields are all present
//      and correctly shaped in the LAST record, the key-hygiene red-prove
//      (the fake key never lands in the manifest text), and -- the
//      provenance regression proof -- that a SECOND invocation into the
//      SAME runDir APPENDS a second record rather than overwriting the
//      first.
//----------------------------------------------------------------------
static void TestRunEvalMatrixManifest()
{
	std::printf( "T7b: RunEvalMatrix emits run.manifest.jsonl...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads (manifest test)" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	const std::string kFakeKey = "sk-ant-MANIFEST-FAKEKEY-DO-NOT-LEAK-012345";

	AgentEvalRunConfig cfg;
	cfg.scenarios.push_back( "evals/scenarios/param_edit.json" );
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t7b_matrix_manifest" );

	MockTransport mock;
	mock.responses.push_back( { 200, "{}", "", 1 } );   // the pre-flight auth probe (credential looks live)
	mock.responses.push_back( { 200, kBodyToolUse, "", 2 } );
	mock.responses.push_back( { 200, kBodyFinal,   "", 2 } );
	mock.repeatLast = true;

	AgentEvalMatrixOptions mo;
	mo.transport = &mock;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};
	mo.clock = []() -> int64_t { return int64_t( 1234567890000 ); };   // injected -- proves createdUtcMs uses it, not wall time

	AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
	Check( mr.runsExecuted == 1, "manifest test: the one configured run executed" );

	const std::string manifestPath = ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string();
	Check( std::filesystem::exists( manifestPath ), "run.manifest.jsonl exists under runDir" );

	// Split into non-empty lines; each line is one appended provenance record.
	auto readNonEmptyLines = [&]( const std::string& path ) -> std::vector<std::string> {
		std::vector<std::string> lines;
		std::ifstream f( path.c_str(), std::ios::binary );
		std::string line;
		while( std::getline( f, line ) ) {
			if( !line.empty() ) lines.push_back( line );
		}
		return lines;
	};

	std::vector<std::string> lines1 = readNonEmptyLines( manifestPath );
	Check( lines1.size() == 1, "run.manifest.jsonl has exactly 1 record after the first invocation" );

	std::string manifestText;
	{
		std::ifstream f( manifestPath.c_str(), std::ios::binary );
		std::ostringstream ss; ss << f.rdbuf();
		manifestText = ss.str();
	}

	JsonValue root; std::string perr;
	Check( JsonParse( lines1.back(), root, perr ), "run.manifest.jsonl last record parses as JSON: " + perr );

	Check( root.get( "schemaVersion" ).asNumber( -1 ) == 1, "manifest schemaVersion == 1" );
	Check( static_cast<int64_t>( root.get( "createdUtcMs" ).asNumber( -1 ) ) == 1234567890000,
	       "manifest createdUtcMs reflects the INJECTED clock, not wall time" );

	Check( !root.get( "riseBuild" ).asString().empty(), "manifest riseBuild is a non-empty version string" );

	const JsonValue& gitObj = root.get( "git" );
	Check( gitObj.isObject(), "manifest git field is an object" );
	Check( gitObj.has( "sha" ) && gitObj.get( "sha" ).isString(), "manifest git.sha is a string" );
	Check( gitObj.has( "dirty" ) && gitObj.get( "dirty" ).isBool(), "manifest git.dirty is a bool" );
	Check( gitObj.has( "available" ) && gitObj.get( "available" ).isBool(), "manifest git.available is a bool" );
	// git-integrity invariant (portable: does NOT force available==true, so a
	// tarball build with no .git still passes): IF the capture succeeded, the
	// sha MUST be a well-formed 40-char lowercase-hex commit, never a partial
	// or error-text value.  This is the assertion that would fail on a broken
	// git-capture (e.g. the Windows null-device redirect bug) in a real checkout.
	if( gitObj.get( "available" ).asBool( false ) ) {
		const std::string sha = gitObj.get( "sha" ).asString();
		bool wellFormed = sha.size() == 40;
		for( std::size_t i = 0; wellFormed && i < sha.size(); ++i ) {
			const char c = sha[i];
			if( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) ) ) wellFormed = false;
		}
		Check( wellFormed, "manifest git.sha is 40-char lowercase hex when git.available is true" );
	}

	Check( root.get( "runDir" ).asString() == cfg.runDir, "manifest runDir matches the config" );
	Check( root.get( "repeats" ).asNumber( -1 ) == 1, "manifest repeats == 1" );

	const JsonValue& sourcesArr = root.get( "scenarioSources" );
	Check( sourcesArr.isArray() && sourcesArr.size() == 1, "manifest scenarioSources array has 1 entry" );
	Check( sourcesArr.at( 0 ).asString() == "evals/scenarios/param_edit.json",
	       "manifest scenarioSources[0] is the verbatim config path (NOT the loaded id -- guards against array swap)" );

	const JsonValue& scenariosArr = root.get( "scenarios" );
	Check( scenariosArr.isArray() && scenariosArr.size() == 1, "manifest scenarios array has 1 entry" );
	Check( scenariosArr.at( 0 ).get( "id" ).asString() == "param_edit", "manifest scenarios[0].id == param_edit" );

	const JsonValue& providersArr = root.get( "providers" );
	Check( providersArr.isArray() && providersArr.size() == 1, "manifest providers array has 1 entry" );
	Check( providersArr.at( 0 ).get( "provider" ).asString() == "anthropic", "manifest providers[0].provider == anthropic" );
	Check( providersArr.at( 0 ).get( "model" ).asString() == "claude-sonnet-5", "manifest providers[0].model == claude-sonnet-5" );
	Check( providersArr.at( 0 ).get( "keyEnvVar" ).asString() == "RISE_TEST_FAKE_KEY",
	       "manifest providers[0].keyEnvVar == RISE_TEST_FAKE_KEY (the NAME, never the key value)" );

	Check( root.get( "result" ).get( "runsExecuted" ).asNumber( -1 ) == 1, "manifest result.runsExecuted == 1" );

	// Key-hygiene red-prove: the fake key rode the mock's auth header (the
	// run actually executed live) yet must appear in NO output file --
	// including this new manifest -- and not in the manifest text directly.
	Check( !AnyFileUnderContains( cfg.runDir, kFakeKey ),
	       "manifest test: the fake key appears in NO output file under runDir" );
	Check( manifestText.find( kFakeKey ) == std::string::npos,
	       "run.manifest.jsonl text does NOT contain the fake key" );

	// Provenance regression proof: a SECOND invocation into the SAME runDir
	// with the SAME config must APPEND a second record, not overwrite the
	// first.  The resume guard will skip the already-complete run (its
	// result.jsonl is non-empty from the first invocation), so this second
	// call executes zero NEW runs but still appends its own manifest record.
	AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );

	std::vector<std::string> lines2 = readNonEmptyLines( manifestPath );
	Check( lines2.size() == 2,
	       "run.manifest.jsonl has exactly 2 records after a second invocation into the same runDir "
	       "(the first record was preserved, not overwritten)" );

	JsonValue root2; std::string perr2;
	Check( JsonParse( lines2.back(), root2, perr2 ), "run.manifest.jsonl second record parses as JSON: " + perr2 );
	Check( root2.get( "result" ).get( "runsAlreadyComplete" ).asNumber( -1 ) >= 1,
	       "run.manifest.jsonl second record's result.runsAlreadyComplete >= 1 (the resume happened)" );
	Check( mr2.runsAlreadyComplete >= 1, "second RunEvalMatrix invocation actually resumed (runsAlreadyComplete >= 1)" );
}

//----------------------------------------------------------------------
// T8: RunEvalMatrix REFUSES a duplicate scenario id (the id-collision guard
//     -- the belt to the CLI's path-canonical dedup suspenders).  Two loaded
//     scenarios sharing an id would collide into ONE per-run subdir, silently
//     concatenating trajectory.jsonl (append) and overwriting result.jsonl
//     (truncate).  This must fail loudly BEFORE any run executes.
//----------------------------------------------------------------------
static void TestRunEvalMatrixDuplicateId()
{
	std::printf( "T8: RunEvalMatrix refuses a duplicate scenario id...\n" );

	AgentEvalScenario s;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", s, err ), "param_edit loads" );

	// Two DISTINCT scenario objects that declare the SAME id (the case the
	// CLI's path-dedup can't catch -- two different files, one id).
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( s );
	scenarios.push_back( s );
	Check( !scenarios[0].id.empty() && scenarios[0].id == scenarios[1].id,
	       "precondition: both scenarios carry the same non-empty id" );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t8_dup_id" );

	const std::string kFakeKey = "sk-ant-DUPID-FAKEKEY-DO-NOT-LEAK";
	MockTransport mock;   // must NEVER be called -- the refusal precedes the run loop

	AgentEvalMatrixOptions mo;
	mo.transport = &mock;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
	Check( !mr.errorMessage.empty(), "the matrix REFUSED loudly (non-empty errorMessage)" );
	Check( mr.errorMessage.find( scenarios[0].id ) != std::string::npos,
	       "the refusal names the duplicated scenario id" );
	Check( mr.runsExecuted == 0 && mr.runsSkipped == 0 &&
	       mr.providersUsed == 0 && mr.providersSkipped == 0,
	       "the refusal fired BEFORE the provider/run loop (all counts 0)" );
	Check( mock.seenRequests.empty(), "the transport was NEVER called on a refused matrix" );
}

//----------------------------------------------------------------------
// T9: RunEvalMatrix cross-invocation idempotent-resume -- re-running the
//     SAME 1-scenario matrix into the SAME runDir a second time executes
//     ZERO runs (all skipped-as-complete), and the second invocation's
//     transport is NEVER called.  This is the fix for the observed bug:
//     without the guard, the second invocation would reopen
//     trajectory.jsonl in APPEND mode (two concatenated sessions) and
//     truncate-overwrite result.jsonl.
//----------------------------------------------------------------------
static void TestRunEvalMatrixResumeSkipsCompleted()
{
	std::printf( "T9: RunEvalMatrix cross-invocation resume skips completed runs...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t9_matrix_resume" );

	const std::string kFakeKey = "sk-ant-RESUME-FAKEKEY-DO-NOT-LEAK";
	AgentEvalMatrixOptions mo;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	const std::string runSub =
		( std::filesystem::path( cfg.runDir ) / "param_edit__anthropic__claude-sonnet-5__r1" ).string();
	const std::string trajPath = ( std::filesystem::path( runSub ) / "param_edit.trajectory.jsonl" ).string();

	// First invocation: completes normally.
	{
		MockTransport mock1;
		mock1.responses.push_back( { 200, "{}", "", 1 } );   // the pre-flight auth probe
		mock1.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock1.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock1.repeatLast = true;
		mo.transport = &mock1;

		AgentEvalMatrixResult mr1 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr1.runsExecuted == 1 && mr1.runsAlreadyComplete == 0,
		       "first invocation: the run executes (nothing to resume yet)" );
		Check( !mock1.seenRequests.empty(), "first invocation: the transport WAS called" );
		Check( std::filesystem::exists( ( std::filesystem::path( runSub ) / "param_edit.result.jsonl" ) ),
		       "first invocation: result.jsonl exists after completion" );
	}

	const std::vector<JsonValue> trajAfterFirst = ReadJsonl( trajPath );
	Check( !trajAfterFirst.empty(), "first invocation: trajectory.jsonl has content" );

	// Second invocation into the SAME runDir: must SKIP the already-complete
	// run.  The pre-flight auth probe still fires ONCE (unconditionally, per
	// resolved provider column, before any per-scenario resume check), so
	// the mock needs exactly one canned probe response.
	{
		MockTransport mock2;
		mock2.responses.push_back( { 200, "{}", "", 1 } );   // auth probe only -- the resumed cell never re-executes
		mo.transport = &mock2;

		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 0, "second invocation: 0 runs executed" );
		Check( mr2.runsAlreadyComplete == 1, "second invocation: 1 run skipped as already-complete" );
		Check( mock2.seenRequests.size() == 1,
		       "second invocation: the transport is called exactly ONCE (the auth probe, no cell re-executes)" );
	}

	// The trajectory file is untouched by the second (skipped) invocation --
	// no re-open, no re-append.
	const std::vector<JsonValue> trajAfterSecond = ReadJsonl( trajPath );
	Check( trajAfterSecond.size() == trajAfterFirst.size(),
	       "trajectory.jsonl record count is UNCHANGED by the skipped second invocation" );
}

//----------------------------------------------------------------------
// T10: RunEvalMatrix cross-invocation resume -- a subdir left behind by a
//     CRASHED/interrupted run (a trajectory.jsonl with a partial session
//     but NO result.jsonl) is wiped and RE-RUN, rather than skipped or
//     appended-to.  Proves the trajectory file holds exactly ONE session
//     record afterward (not two concatenated sessions).
//----------------------------------------------------------------------
static void TestRunEvalMatrixResumeRerunsPartial()
{
	std::printf( "T10: RunEvalMatrix cross-invocation resume re-runs a crashed/partial run...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t10_matrix_resume_partial" );

	const std::string kFakeKey = "sk-ant-PARTIAL-FAKEKEY-DO-NOT-LEAK";
	AgentEvalMatrixOptions mo;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	const std::string runSub =
		( std::filesystem::path( cfg.runDir ) / "param_edit__anthropic__claude-sonnet-5__r1" ).string();
	const std::string trajPath = ( std::filesystem::path( runSub ) / "param_edit.trajectory.jsonl" ).string();
	const std::string resultPath = ( std::filesystem::path( runSub ) / "param_edit.result.jsonl" ).string();

	// First invocation: completes normally (this leaves a full trajectory +
	// result behind, which we then simulate a crash against below).
	{
		MockTransport mock1;
		mock1.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock1.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock1.repeatLast = true;
		mo.transport = &mock1;

		AgentEvalMatrixResult mr1 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr1.runsExecuted == 1, "first invocation: the run executes" );
	}

	const std::vector<JsonValue> trajAfterFirst = ReadJsonl( trajPath );
	const std::size_t sessionsAfterFirst =
		static_cast<std::size_t>( std::count_if( trajAfterFirst.begin(), trajAfterFirst.end(),
			[]( const JsonValue& v ) { return v.get( "run_type" ).asString() == "session"; } ) );
	Check( sessionsAfterFirst == 1, "first invocation: trajectory holds exactly one session record" );

	// Simulate a CRASH mid-run: delete only the result file, leaving the
	// (now "partial" from the guard's point of view) trajectory behind.
	Check( std::filesystem::remove( resultPath ), "simulated crash: result.jsonl removed, trajectory left behind" );
	Check( std::filesystem::exists( trajPath ), "simulated crash: trajectory.jsonl still present with no result" );

	// Second invocation: the subdir exists but carries NO result.jsonl -> it
	// must be treated as crashed/interrupted, WIPED, and RE-RUN (not
	// skipped, not appended-to).
	{
		MockTransport mock2;
		mock2.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock2.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock2.repeatLast = true;
		mo.transport = &mock2;

		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 1, "second invocation: the crashed/partial run IS re-executed" );
		Check( mr2.runsAlreadyComplete == 0, "second invocation: not counted as already-complete" );
		Check( !mock2.seenRequests.empty(), "second invocation: the transport WAS called" );
		Check( std::filesystem::exists( resultPath ), "second invocation: result.jsonl exists again after re-run" );
	}

	const std::vector<JsonValue> trajAfterSecond = ReadJsonl( trajPath );
	const std::size_t sessionsAfterSecond =
		static_cast<std::size_t>( std::count_if( trajAfterSecond.begin(), trajAfterSecond.end(),
			[]( const JsonValue& v ) { return v.get( "run_type" ).asString() == "session"; } ) );
	Check( sessionsAfterSecond == 1,
	       "trajectory.jsonl holds exactly ONE session record after the re-run (old partial content was WIPED, not appended-to)" );
}

//----------------------------------------------------------------------
// T11: RunEvalMatrix's resume guard is CONTENT-AWARE.  A cell completed under
//     one oracle is only reusable while the scenario that produced it is
//     UNCHANGED.  Three invocations into the SAME runDir:
//       (1) fresh -> executes.
//       (2) same scenario again -> skipped-as-complete, transport untouched
//           (the hash matched).
//       (3) the scenario's checkpoints[] MUTATED (a different content hash) ->
//           the stale cell is wiped and RE-EXECUTED, transport called again.
//     Without the hash comparison, (3) would keep publishing the score graded
//     under the OLD oracle.
//----------------------------------------------------------------------
static void TestRunEvalMatrixResumeContentAware()
{
	std::printf( "T11: RunEvalMatrix resume is content-aware (re-runs a changed oracle)...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t11_matrix_resume_content_aware" );

	const std::string kFakeKey = "sk-ant-CONTENTAWARE-FAKEKEY-DO-NOT-LEAK";
	AgentEvalMatrixOptions mo;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	// (1) First invocation: completes normally.
	{
		MockTransport mock1;
		mock1.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock1.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock1.repeatLast = true;
		mo.transport = &mock1;

		AgentEvalMatrixResult mr1 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr1.runsExecuted == 1 && mr1.runsAlreadyComplete == 0,
		       "first invocation: the run executes (nothing to resume yet)" );
		Check( !mock1.seenRequests.empty(), "first invocation: the transport WAS called" );
	}

	// (2) Second invocation, SAME (unmodified) scenario: the stamped hash
	// matches -> skipped-as-complete.  The pre-flight auth probe still fires
	// ONCE (unconditionally, per resolved provider column, before any
	// per-scenario resume check), so the mock needs exactly one canned
	// probe response; the resumed cell itself never re-executes.
	{
		MockTransport mock2;
		mock2.responses.push_back( { 200, "{}", "", 1 } );   // auth probe only
		mo.transport = &mock2;

		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 0, "second invocation (unchanged scenario): 0 runs executed" );
		Check( mr2.runsAlreadyComplete == 1, "second invocation: 1 run skipped as already-complete (hash matched)" );
		Check( mock2.seenRequests.size() == 1,
		       "second invocation: the transport is called exactly ONCE (the auth probe, no cell re-executes)" );
	}

	// (3) MUTATE the scenario's checkpoints[] so its content hash differs, then
	// re-run into the SAME runDir.  The cell must be treated as STALE, wiped,
	// and RE-EXECUTED.  (We only exercise the resume guard's hash comparison
	// here, not full grading -- a plausible array-of-objects shape suffices.)
	{
		JsonValue newCheckpoints = JsonValue::MakeArray();
		JsonValue extra = JsonValue::MakeObject();
		extra.set( "kind", JsonValue::MakeString( "trajectory" ) );
		extra.set( "terminalStatus", JsonValue::MakeString( "final_text" ) );
		newCheckpoints.push_back( extra );
		scenarios[0].checkpoints = newCheckpoints;

		MockTransport mock3;
		mock3.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock3.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock3.repeatLast = true;
		mo.transport = &mock3;

		AgentEvalMatrixResult mr3 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr3.runsExecuted == 1,
		       "third invocation (mutated oracle): the stale cell IS re-executed" );
		Check( mr3.runsAlreadyComplete == 0,
		       "third invocation: NOT counted as already-complete (hash mismatch => stale)" );
		Check( !mock3.seenRequests.empty(),
		       "third invocation: the transport WAS called (genuine re-run, not a stale-count accident)" );
	}

	// (4) Mutate a NON-checkpoint field (a budget) -- the hash must ALSO change,
	// proving ScenarioContentHash covers autonomy/prompts/budgets/scene, not
	// just checkpoints[].  Re-run into the same runDir -> stale -> re-executed.
	{
		scenarios[0].budgets.maxToolCalls = 7;   // was param_edit's 10 -> hash differs
		MockTransport mock4;
		mock4.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock4.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock4.repeatLast = true;
		mo.transport = &mock4;

		AgentEvalMatrixResult mr4 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr4.runsExecuted == 1 && mr4.runsAlreadyComplete == 0,
		       "fourth invocation (mutated BUDGET, checkpoints unchanged): re-executed (hash covers non-checkpoint fields)" );
	}

	// (5) Determinism across a fresh RELOAD of the SAME file: re-loading the
	// unchanged scenario from disk must reproduce a byte-identical canonical
	// string -> identical hash -> skip.  Guards against a future JsonSerialize
	// non-determinism (e.g. a switch to sorted keys) silently forcing spurious
	// full re-runs.
	{
		const std::string dir2 = ScratchRunDir( "t11_reload_determinism" );
		AgentEvalRunConfig cfg2 = cfg; cfg2.runDir = dir2;

		AgentEvalScenario s2a; std::string e2a;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", s2a, e2a ), "param_edit reloads (a)" );
		std::vector<AgentEvalScenario> v2a; v2a.push_back( s2a );
		MockTransport m5a;
		m5a.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		m5a.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		m5a.repeatLast = true;
		mo.transport = &m5a;
		RunEvalMatrix( cfg2, v2a, mo );

		AgentEvalScenario s2b; std::string e2b;   // FRESH reload -> independent in-memory object
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", s2b, e2b ), "param_edit reloads (b)" );
		std::vector<AgentEvalScenario> v2b; v2b.push_back( s2b );
		MockTransport m5b;
		m5b.responses.push_back( { 200, "{}", "", 1 } );   // auth probe only -- the resumed cell never re-executes
		mo.transport = &m5b;
		AgentEvalMatrixResult mr5 = RunEvalMatrix( cfg2, v2b, mo );
		Check( mr5.runsAlreadyComplete == 1 && mr5.runsExecuted == 0,
		       "a reloaded-from-disk unchanged scenario hashes IDENTICALLY -> skipped (determinism)" );
		Check( m5b.seenRequests.size() == 1,
		       "reload-determinism: the transport is called exactly ONCE (the auth probe) on the skip" );
	}

	// (6) Mutate the scenario's interventions[] -- must ALSO change the hash
	// and force re-execution.  Before the v2 ScenarioContentHash, this field
	// was omitted from the canonicalization entirely, so this mutation left
	// a cached cell wrongly marked "current".
	{
		AgentEvalIntervention iv;
		iv.afterToolCalls = 1;
		iv.op = "param_edit";
		iv.target = "pnt_albedo";
		iv.param = "color";
		iv.value = "0.7 0.1 0.1";
		scenarios[0].interventions.push_back( iv );

		MockTransport mock6;
		mock6.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock6.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock6.repeatLast = true;
		mo.transport = &mock6;

		AgentEvalMatrixResult mr6 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr6.runsExecuted == 1 && mr6.runsAlreadyComplete == 0,
		       "sixth invocation (mutated interventions[], everything else unchanged): "
		       "re-executed (hash covers interventions[])" );
		Check( !mock6.seenRequests.empty(), "sixth invocation: the transport WAS called" );
	}

	// (7) A PATH-backed scene hashes the file's BYTES, not just the pathname.
	// Build a fresh scenario whose scene is scenePath (not sceneInline),
	// pointing at a throwaway temp file written with param_edit's own inline
	// scene text (a known-loadable v7 scene, taken from the ORIGINAL `scenario`
	// local -- unmutated by cases (1)-(6) above, which only touched the
	// `scenarios[0]` copy). Run once (executes), then edit the temp file's
	// BYTES in place (append a harmless '#'-comment line -- '#' starts a
	// to-end-of-line comment in the CST tokenizer, so the scene stays
	// loadable) and re-run into the SAME runDir -> must be re-executed, not
	// skipped-as-complete.  Before this fix, ScenarioContentHash hashed only
	// `s.scenePath` (the path STRING), so an in-place edit like this left the
	// cached cell wrongly marked "current".
	{
		const std::string dir7 = ScratchRunDir( "t11_path_scene_bytes" );
		const std::string scenePath = dir7 + "_scene/param_edit_copy.RISEscene";
		Check( WriteFile( scenePath, scenario.sceneInline ), "path-scene: wrote the temp scene file" );

		AgentEvalScenario pathScenario = scenario;   // fresh copy of the ORIGINAL (unmutated) param_edit load
		pathScenario.scenePath = scenePath;
		pathScenario.sceneInline.clear();

		AgentEvalRunConfig cfg7;
		cfg7.providers.push_back( p );
		cfg7.repeats = 1;
		cfg7.runDir = dir7;

		std::vector<AgentEvalScenario> scenarios7;
		scenarios7.push_back( pathScenario );

		MockTransport mock7a;
		mock7a.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock7a.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock7a.repeatLast = true;
		mo.transport = &mock7a;
		AgentEvalMatrixResult mr7a = RunEvalMatrix( cfg7, scenarios7, mo );
		Check( mr7a.runsExecuted == 1 && mr7a.runsAlreadyComplete == 0,
		       "path-scene first invocation: executes" );
		Check( !mock7a.seenRequests.empty(), "path-scene first invocation: the transport WAS called" );

		// MUTATE the temp scene file's bytes IN PLACE -- the path string is
		// unchanged, only the file's contents.
		Check( WriteFile( scenePath, scenario.sceneInline + "\n# mutated for T11(7)\n" ),
		       "path-scene: mutated the temp scene file's bytes" );

		MockTransport mock7b;
		mock7b.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock7b.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock7b.repeatLast = true;
		mo.transport = &mock7b;
		AgentEvalMatrixResult mr7b = RunEvalMatrix( cfg7, scenarios7, mo );
		Check( mr7b.runsExecuted == 1 && mr7b.runsAlreadyComplete == 0,
		       "path-scene second invocation (mutated FILE BYTES, same path): re-executed "
		       "(hash covers a path-backed scene's bytes, not just its path)" );
		Check( !mock7b.seenRequests.empty(), "path-scene second invocation: the transport WAS called" );
	}
}

//----------------------------------------------------------------------
// T12: RunEvalMatrix REFUSES a provider/model leaf collision (P2).  Two
//     DISTINCT (provider, model) pairs whose SanitizeForPath fragments are
//     identical would collide into ONE cell + ONE report row.  The two models
//     below -- "a:b" and "a b" -- both sanitize to "a_b" per the documented
//     rule (any char outside [A-Za-z0-9._-] -> '_'; verified by inspection,
//     since SanitizeForPath is file-local to AgentEvalRunner.cpp and cannot be
//     called from here).  The refusal must fire BEFORE any run executes.
//----------------------------------------------------------------------
static void TestRunEvalMatrixProviderModelCollision()
{
	std::printf( "T12: RunEvalMatrix refuses a provider/model leaf collision...\n" );

	AgentEvalScenario s;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", s, err ), "param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( s );

	AgentEvalRunConfig cfg;
	// Same provider, DIFFERENT models that sanitize identically: "a:b" and
	// "a b" both -> "a_b".
	AgentEvalProviderConfig p1; p1.provider = "anthropic"; p1.model = "a:b"; p1.keyEnvVar = "RISE_TEST_FAKE_KEY";
	AgentEvalProviderConfig p2; p2.provider = "anthropic"; p2.model = "a b"; p2.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p1 );
	cfg.providers.push_back( p2 );
	cfg.repeats = 1;
	cfg.runDir = ScratchRunDir( "t12_provider_model_collision" );

	const std::string kFakeKey = "sk-ant-COLLIDE-FAKEKEY-DO-NOT-LEAK";
	MockTransport mock;   // must NEVER be called -- the refusal precedes the run loop

	AgentEvalMatrixOptions mo;
	mo.transport = &mock;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
	Check( !mr.errorMessage.empty(), "the matrix REFUSED loudly (non-empty errorMessage)" );
	Check( mr.errorMessage.find( "a:b" ) != std::string::npos &&
	       mr.errorMessage.find( "a b" ) != std::string::npos,
	       "the refusal names BOTH colliding original models" );
	Check( mr.errorMessage.find( "a_b" ) != std::string::npos,
	       "the refusal names the colliding sanitized fragment" );
	Check( mr.runsExecuted == 0 && mr.runsSkipped == 0 &&
	       mr.providersUsed == 0 && mr.providersSkipped == 0,
	       "the refusal fired BEFORE the provider/run loop (all counts 0)" );
	Check( mock.seenRequests.empty(), "the transport was NEVER called on a refused matrix" );
}

//----------------------------------------------------------------------
// T12b: RunEvalMatrix's PRE-FLIGHT AUTH PROBE.  Once a column's key
// resolves, RunEvalMatrix sends ONE minimal "ping" request through the SAME
// codec/transport path a real run uses, BEFORE that column's scenario x
// repeat loop runs, and classifies the response -- see RunEvalMatrix's doc
// comment in AgentEvalRunner.h.  Five sub-cases:
//   (A) the probe passes (a plain 200) -> the column executes normally, the
//       FIRST captured request is the probe (body contains "ping"),
//       providersAuthFailed == 0, and the manifest records
//       authProbes.anthropic == "ok".
//   (B) the probe 401s -> the column is SKIPPED, providersAuthFailed == 1,
//       runsSkipped == scenarios x reps, the transport is called EXACTLY
//       once (the probe itself -- no run ever starts), the log line
//       mentions "auth probe", the manifest records "auth-failed", and the
//       fake key appears in NO output file.
//   (C) the probe 400s with an Anthropic-style "credit balance" body -> same
//       SKIP outcome as (B) (the exhausted-credits case).
//   (D) the probe 500s -> NOT auth-fatal: the column proceeds and the run
//       executes against the mock (a transient server hiccup must never
//       cost a provider its whole column).
//   (E) a keyless "local" provider is NEVER probed -- the FIRST captured
//       request is the run itself, and the manifest records
//       authProbes.local == "skipped-keyless".
//----------------------------------------------------------------------
static void TestRunEvalMatrixAuthProbe()
{
	std::printf( "T12b: RunEvalMatrix pre-flight auth probe...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "T12b: param_edit loads" );
	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	// (A) The probe passes -> the column executes.
	{
		const std::string kFakeKey = "sk-ant-AUTHPROBE-A-FAKEKEY-DO-NOT-LEAK";
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t12b_authprobe_ok" );

		MockTransport mock;
		mock.responses.push_back( { 200, kBodyFinal, "", 1 } );   // the auth probe -- a plain 200
		mock.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock.repeatLast = true;

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = [&]( const std::string& name ) -> const char* {
			return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
		};

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 1 && mr.providersSkipped == 0,
		       "T12b(A): a passing probe leaves the column USED" );
		Check( mr.providersAuthFailed == 0, "T12b(A): providersAuthFailed == 0" );
		Check( mr.runsExecuted == 1, "T12b(A): the column executed" );
		Check( !mock.seenRequests.empty() && mock.seenRequests[0].body.find( "ping" ) != std::string::npos,
		       "T12b(A): the FIRST captured request is the auth probe (body contains \"ping\")" );
		Check( mock.seenRequests.size() >= 2,
		       "T12b(A): the probe AND at least one real run request reached the transport" );

		std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
		Check( !manifest.empty(), "T12b(A): run.manifest.jsonl has a record" );
		if( !manifest.empty() ) {
			Check( manifest.back().get( "authProbes" ).get( "anthropic" ).asString() == "ok",
			       "T12b(A): manifest records authProbes.anthropic == \"ok\"" );
		}
	}

	// (B) The probe 401s -> the column is SKIPPED, the transport is called
	// EXACTLY once (no run ever starts), and the fake key never leaks.
	{
		const std::string kFakeKey = "sk-ant-AUTHPROBE-B-FAKEKEY-DO-NOT-LEAK";
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 2;
		cfg.runDir = ScratchRunDir( "t12b_authprobe_401" );

		MockTransport mock;
		mock.responses.push_back( { 401, "{\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", "", 1 } );

		std::vector<std::string> logLines;
		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = [&]( const std::string& name ) -> const char* {
			return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
		};
		mo.log = [&]( const std::string& m ) { logLines.push_back( m ); };

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 0, "T12b(B): a 401 probe leaves the column NOT used" );
		Check( mr.providersSkipped == 1, "T12b(B): providersSkipped == 1" );
		Check( mr.providersAuthFailed == 1, "T12b(B): providersAuthFailed == 1" );
		Check( mr.runsSkipped == static_cast<int>( scenarios.size() ) * cfg.repeats,
		       "T12b(B): runsSkipped == scenarios x repeats" );
		Check( mr.runsExecuted == 0, "T12b(B): no run ever started" );
		Check( mock.seenRequests.size() == 1,
		       "T12b(B): the transport was called EXACTLY once (the probe -- no run ever starts)" );

		bool sawAuthProbeSkip = false;
		for( std::size_t i = 0; i < logLines.size(); ++i )
			if( logLines[i].find( "auth probe" ) != std::string::npos ) sawAuthProbeSkip = true;
		Check( sawAuthProbeSkip, "T12b(B): a log line mentions \"auth probe\"" );

		Check( !AnyFileUnderContains( cfg.runDir, kFakeKey ),
		       "T12b(B): the fake key appears in NO output file" );
		for( std::size_t i = 0; i < logLines.size(); ++i )
			Check( logLines[i].find( kFakeKey ) == std::string::npos,
			       "T12b(B): no log line contains the fake key" );

		std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
		Check( !manifest.empty(), "T12b(B): run.manifest.jsonl has a record" );
		if( !manifest.empty() ) {
			Check( manifest.back().get( "authProbes" ).get( "anthropic" ).asString() == "auth-failed",
			       "T12b(B): manifest records authProbes.anthropic == \"auth-failed\"" );
			Check( manifest.back().get( "result" ).get( "providersAuthFailed" ).asNumber( -1 ) == 1,
			       "T12b(B): manifest result.providersAuthFailed == 1" );
		}
	}

	// (C) The probe 400s with an Anthropic-style exhausted-credits body ->
	// the same SKIP outcome as (B).
	{
		const std::string kFakeKey = "sk-ant-AUTHPROBE-C-FAKEKEY-DO-NOT-LEAK";
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t12b_authprobe_credit_balance" );

		MockTransport mock;
		mock.responses.push_back( { 400,
			"{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
			"\"message\":\"Your credit balance is too low to access the Anthropic API. "
			"Please go to Plans & Billing to upgrade or purchase credits.\"}}", "", 1 } );

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = [&]( const std::string& name ) -> const char* {
			return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
		};

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersAuthFailed == 1, "T12b(C): an exhausted-credits 400 body IS classified as a dead credential" );
		Check( mr.runsExecuted == 0, "T12b(C): no run ever started" );
		Check( mock.seenRequests.size() == 1, "T12b(C): the transport was called EXACTLY once (the probe)" );

		std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
		Check( !manifest.empty(), "T12b(C): run.manifest.jsonl has a record" );
		if( !manifest.empty() ) {
			Check( manifest.back().get( "authProbes" ).get( "anthropic" ).asString() == "auth-failed",
			       "T12b(C): manifest records authProbes.anthropic == \"auth-failed\"" );
		}
	}

	// (D) The probe 500s -> NOT auth-fatal: the column proceeds and the run
	// executes against the mock.
	{
		const std::string kFakeKey = "sk-ant-AUTHPROBE-D-FAKEKEY-DO-NOT-LEAK";
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t12b_authprobe_500" );

		MockTransport mock;
		mock.responses.push_back( { 500, "{\"error\":\"internal server error\"}", "", 1 } );   // the probe -- a transient 5xx
		mock.responses.push_back( { 200, kBodyToolUse, "", 2 } );
		mock.responses.push_back( { 200, kBodyFinal,   "", 2 } );
		mock.repeatLast = true;

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = [&]( const std::string& name ) -> const char* {
			return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
		};

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersAuthFailed == 0, "T12b(D): a transient 5xx probe response is NOT auth-fatal" );
		Check( mr.providersUsed == 1 && mr.providersSkipped == 0,
		       "T12b(D): the column proceeds (not skipped) after a 500 probe" );
		Check( mr.runsExecuted == 1, "T12b(D): the run executes against the mock" );
		Check( mock.seenRequests.size() >= 2,
		       "T12b(D): the probe AND at least one real run request reached the transport" );

		std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
		Check( !manifest.empty(), "T12b(D): run.manifest.jsonl has a record" );
		if( !manifest.empty() ) {
			Check( manifest.back().get( "authProbes" ).get( "anthropic" ).asString() == "ok",
			       "T12b(D): manifest records authProbes.anthropic == \"ok\" (a 5xx does not count as a dead credential)" );
		}
	}

	// (E) A keyless "local" provider is NEVER probed.
	{
		AgentEvalRunConfig cfg;
		AgentEvalProviderConfig p; p.provider = "local"; p.model = "qwen3:32b";   // keyEnvVar left EMPTY -- keyless by design
		cfg.providers.push_back( p );
		cfg.repeats = 1;
		cfg.runDir = ScratchRunDir( "t12b_authprobe_local_skipped" );

		MockTransport mock;
		mock.responses.push_back( { 200,
			"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Done.\"},"
			"\"finish_reason\":\"stop\"}],"
			"\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", "", 2 } );

		AgentEvalMatrixOptions mo;
		mo.transport = &mock;
		mo.envLookup = []( const std::string& ) -> const char* { return nullptr; };   // never consulted for "local"

		AgentEvalMatrixResult mr = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr.providersUsed == 1 && mr.providersAuthFailed == 0,
		       "T12b(E): the keyless local column is used, never auth-probed" );
		Check( mr.runsExecuted == 1, "T12b(E): the local run executes" );
		Check( mock.seenRequests.size() == 1,
		       "T12b(E): the transport was called EXACTLY once (the run itself -- no probe)" );
		Check( mock.seenRequests[0].body.find( "ping" ) == std::string::npos,
		       "T12b(E): the FIRST (and only) captured request is the run, not a probe" );

		std::vector<JsonValue> manifest = ReadJsonl( ( std::filesystem::path( cfg.runDir ) / "run.manifest.jsonl" ).string() );
		Check( !manifest.empty(), "T12b(E): run.manifest.jsonl has a record" );
		if( !manifest.empty() ) {
			Check( manifest.back().get( "authProbes" ).get( "local" ).asString() == "skipped-keyless",
			       "T12b(E): manifest records authProbes.local == \"skipped-keyless\"" );
		}
	}
}

//----------------------------------------------------------------------
// Image-reconstruction Wave 1, T13: a scenario prompt carrying a
// reference image reaches the model as a REAL ChatAttachment --
// end-to-end through RunScenarioLive over the mock transport (the same
// seam T2/TestLiveRunViaMock drives), proving the pre-flight load +
// base64 encode + AddUserMessage attachments overload + trajectory
// recording all wire together.
//----------------------------------------------------------------------
static void TestPromptImagesEndToEnd()
{
	std::printf( "T13: a prompt carrying a reference image reaches the wire as a real attachment...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
	       "T13: param_edit loads (" + err + ")" );

	// Attach a real, small, COMMITTED PNG (textures/cel.png, 319 bytes) to
	// the scenario's single prompt -- read-only, no scratch-file synthesis
	// needed for this end-to-end check (TestPromptImagesMatrixResume below
	// needs a MUTABLE file, so it decodes its own scratch PNG instead).
	const std::string pngPath = "textures/cel.png";
	std::string pngBytes;
	{
		std::ifstream f( pngPath.c_str(), std::ios::binary );
		Check( static_cast<bool>( f ), "T13: textures/cel.png is readable from the repo-root cwd" );
		std::ostringstream ss; ss << f.rdbuf();
		pngBytes = ss.str();
	}
	Check( !pngBytes.empty(), "T13: textures/cel.png is non-empty" );
	Check( scenario.prompts.size() == 1, "T13: param_edit carries exactly 1 prompt to attach to" );
	scenario.prompts[0].imagePaths.push_back( pngPath );

	// A single final-text turn is enough to prove the attachment reached
	// the wire -- no tool call is needed for this check.
	MockTransport mock;
	mock.responses.push_back( { 200, kBodyFinal, "", 5 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t13_prompt_images_e2e" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "T13: run reaches final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 1, "T13: exactly 1 llm round (no tool calls)" );
	Check( mock.seenRequests.size() == 1, "T13: exactly 1 POST reached the mock transport" );

	// The BUILT request body carries the base64 payload -- proving the
	// pre-flight-loaded bytes actually reached the wire, not just the
	// attachment COUNT.
	const std::vector<unsigned char> rawBytes( pngBytes.begin(), pngBytes.end() );
	const std::string expectedB64 = Base64Encode( rawBytes );
	Check( !mock.seenRequests.empty() &&
	       mock.seenRequests[0].body.find( expectedB64 ) != std::string::npos,
	       "T13: the request body sent to the transport contains the reference image's base64 payload" );

	// The trajectory's `user` record reflects the attachment.
	// ChatTrajectory's TrajectoryUserRecord.attachments is a COUNT, never
	// the bytes -- ChatAttachment's doc comment forbids ever logging
	// base64Data -- so this is the honest field to assert on.  A
	// sub-assert on the recorded llm round's requestParamsJson containing
	// the base64 was considered and SKIPPED: AgentChatLoop::
	// ExtractRequestParams (AgentChatLoop.cpp) deliberately strips the
	// "messages"/"contents" array -- where an image block/part would live
	// -- before a request is ever handed to the trajectory recorder, so no
	// image byte reaches ANY recorded/logged field by design (verified by
	// reading ExtractRequestParams; this assert would always vacuously
	// fail-to-find, proving nothing).
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	bool sawUserAttachment = false;
	for( std::size_t i = 0; i < recs.size(); ++i ) {
		if( recs[i].get( "run_type" ).asString() != "user" ) continue;
		if( recs[i].get( "attachments" ).asNumber( -1.0 ) == 1.0 ) sawUserAttachment = true;
	}
	Check( sawUserAttachment, "T13: the trajectory's user record carries attachments==1" );
}

// A minimal, well-formed 1x1 transparent PNG (67 bytes decoded) -- the
// widely-used smallest-valid-PNG literal.  Used ONLY by
// TestPromptImagesMatrixResume, which needs a file it can MUTATE between
// matrix invocations (textures/cel.png above is a real committed asset
// and must never be touched by a test).
static const char* kMinimalPngBase64 =
	"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";

//----------------------------------------------------------------------
// Image-reconstruction Wave 1, T14: RunEvalMatrix resume is content-aware
// for a referenced image's BYTES, mirroring T11(7)'s path-backed-scene-
// bytes case above -- editing the image file in place (same path,
// different bytes) must invalidate the cached cell and force a re-run.
//----------------------------------------------------------------------
static void TestPromptImagesMatrixResume()
{
	std::printf( "T14: RunEvalMatrix resume is content-aware for a referenced image's BYTES...\n" );

	AgentEvalScenario base;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", base, err ),
	       "T14: param_edit loads (for its scene text)" );

	const std::string dir = ScratchRunDir( "t14_prompt_images_matrix_resume" );
	const std::string pngPath = dir + "_img/ref.png";

	std::vector<unsigned char> pngBytes;
	Check( Base64Decode( kMinimalPngBase64, pngBytes ), "T14: the embedded minimal PNG base64 decodes" );
	Check( WriteFile( pngPath, std::string( pngBytes.begin(), pngBytes.end() ) ),
	       "T14: wrote the initial reference image" );

	AgentEvalScenario scenario;
	scenario.id = "t14_img_resume";
	scenario.title = "t14_img_resume";
	scenario.sceneInline = base.sceneInline;
	scenario.autonomy = "commit";
	AgentEvalPrompt prompt;
	prompt.text = "Match this reference photo.";
	prompt.imagePaths.push_back( pngPath );
	scenario.prompts.push_back( prompt );

	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = dir;

	const std::string kFakeKey = "sk-ant-T14-FAKEKEY-DO-NOT-LEAK";
	AgentEvalMatrixOptions mo;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	// (1) First invocation: executes.  The FIRST canned response is the
	// pre-flight auth probe's (a plain 200 -- credential looks live), THEN
	// the run's own final-text body.
	{
		MockTransport mock1;
		mock1.responses.push_back( { 200, "{}", "", 1 } );   // auth probe
		mock1.responses.push_back( { 200, kBodyFinal, "", 2 } );
		mo.transport = &mock1;
		AgentEvalMatrixResult mr1 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr1.runsExecuted == 1 && mr1.runsAlreadyComplete == 0, "T14: first invocation executes" );
		Check( !mock1.seenRequests.empty(), "T14: first invocation calls the transport" );
	}

	// (2) Second invocation, unchanged image bytes: skipped as complete.
	// The pre-flight auth probe fires UNCONDITIONALLY per resolved provider
	// column, BEFORE the per-scenario resume check runs (it has no way to
	// know yet that every cell will resume) -- so the mock still sees
	// exactly the ONE probe request, even though the resumed cell itself
	// never re-executes.
	{
		MockTransport mock2;
		mock2.responses.push_back( { 200, "{}", "", 1 } );   // auth probe only -- no cell re-executes
		mo.transport = &mock2;
		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 0 && mr2.runsAlreadyComplete == 1,
		       "T14: second invocation (unchanged image) is skipped as already-complete" );
		Check( mock2.seenRequests.size() == 1,
		       "T14: second invocation calls the transport exactly ONCE (the auth probe, no cell re-executes)" );
	}

	// (3) MUTATE the referenced image's BYTES in place (same path) -- the
	// cell must be treated as stale and re-executed.
	{
		std::vector<unsigned char> mutated = pngBytes;
		mutated.push_back( 0x00 );   // appended byte -- content differs, path unchanged
		Check( WriteFile( pngPath, std::string( mutated.begin(), mutated.end() ) ),
		       "T14: mutated the reference image's bytes in place" );

		MockTransport mock3;
		mock3.responses.push_back( { 200, "{}", "", 1 } );   // auth probe
		mock3.responses.push_back( { 200, kBodyFinal, "", 2 } );
		mo.transport = &mock3;
		AgentEvalMatrixResult mr3 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr3.runsExecuted == 1 && mr3.runsAlreadyComplete == 0,
		       "T14: third invocation (mutated image BYTES, same path) is RE-EXECUTED" );
		Check( !mock3.seenRequests.empty(), "T14: third invocation calls the transport (genuine re-run)" );
	}
}

//----------------------------------------------------------------------
// P1-1, T15: RunEvalMatrix resume is content-aware for a render
// checkpoint's compareToImage reference image's BYTES, mirroring T14's
// prompt-image resume case above -- editing the referenced PNG in place
// (same path, different bytes) must invalidate the cached cell and force
// a re-run, even though the checkpoints[] JSON itself (the "compareToImage"
// PATH STRING) never changes.  Before P1-1, ScenarioContentHash's
// checkpoints= line only hashed that path string, so this exact mutation
// left the cell wrongly "current" -- the bug this test proves fixed.
//----------------------------------------------------------------------
static void TestCompareToImageMatrixResume()
{
	std::printf( "T15: RunEvalMatrix resume is content-aware for a compareToImage reference's BYTES...\n" );

	AgentEvalScenario base;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", base, err ),
	       "T15: param_edit loads (for its scene text)" );

	const std::string dir = ScratchRunDir( "t15_compare_image_matrix_resume" );
	const std::string pngPath = dir + "_img/cmp_ref.png";

	// Render a genuine reference PNG from the SAME scene (via a throwaway
	// live probe run, reusing THIS file's own MockTransport/RunScenarioLive
	// plumbing rather than a replay fixture) so the compareToImage
	// checkpoint decodes a well-formed PNG on every invocation -- a clean
	// decode keeps this test from masking a genuine crash behind a
	// "PNG decode failed" no-op.  No explicit width/height is requested
	// below, so the grading render adopts the reference's dims (the
	// scene's authored 24x24 film) -- self-consistent, like T4c's
	// compareToImage self-consistency case in AgentEvalCheckTest.cpp.
	{
		AgentEvalScenario probe = base;
		probe.id = "t15_probe";
		probe.title = "t15_probe";
		probe.checkpoints = JsonValue::MakeArray();

		AgentEvalLiveRunOptions plo;
		plo.runDir = dir + "_probe";
		MockTransport probeMock;
		probeMock.responses.push_back( { 200, kBodyFinal, "", 2 } );
		plo.transport = &probeMock;
		plo.provider  = ChatProvider::Anthropic;
		plo.modelId   = "claude-sonnet-5";
		plo.apiKey    = "sk-ant-T15-PROBE-FAKEKEY-DO-NOT-LEAK";

		AgentEvalRunHandle ph = RunScenarioLive( probe, plo );
		Check( ph.dispatcher != nullptr && ph.dispatcher->Session() != nullptr,
		       "T15: probe run has a live session to render the reference" );
		if( ph.dispatcher && ph.dispatcher->Session() ) {
			AgentRenderParams rp;   // no overrides -- the scene's authored 24x24 film
			AgentRenderResult rr = ph.dispatcher->Session()->Render( rp );
			Check( rr.ok && !rr.png.empty(), "T15: probe render produced PNG bytes for the reference image" );
			Check( WriteFile( pngPath, std::string( rr.png.begin(), rr.png.end() ) ),
			       "T15: wrote the initial compareToImage reference" );
		}
	}

	AgentEvalScenario scenario = base;
	scenario.id = "t15_cmp_resume";
	scenario.title = "t15_cmp_resume";
	{
		JsonValue cp = JsonValue::MakeObject();
		cp.set( "kind", JsonValue::MakeString( "render" ) );
		cp.set( "compareToImage", JsonValue::MakeString( pngPath ) );
		cp.set( "rmseMax", JsonValue::MakeNumber( 1.0 ) );   // generous -- pass/fail is irrelevant to resume mechanics
		JsonValue cps = JsonValue::MakeArray();
		cps.push_back( cp );
		scenario.checkpoints = cps;
	}

	std::vector<AgentEvalScenario> scenarios;
	scenarios.push_back( scenario );

	AgentEvalRunConfig cfg;
	AgentEvalProviderConfig p; p.provider = "anthropic"; p.model = "claude-sonnet-5"; p.keyEnvVar = "RISE_TEST_FAKE_KEY";
	cfg.providers.push_back( p );
	cfg.repeats = 1;
	cfg.runDir = dir;

	const std::string kFakeKey = "sk-ant-T15-FAKEKEY-DO-NOT-LEAK";
	AgentEvalMatrixOptions mo;
	mo.envLookup = [&]( const std::string& name ) -> const char* {
		return name == "RISE_TEST_FAKE_KEY" ? kFakeKey.c_str() : nullptr;
	};

	// (1) First invocation: executes.  The FIRST canned response is the
	// pre-flight auth probe's (a plain 200), THEN the run's own final-text
	// body.
	{
		MockTransport mock1;
		mock1.responses.push_back( { 200, "{}", "", 1 } );   // auth probe
		mock1.responses.push_back( { 200, kBodyFinal, "", 2 } );
		mo.transport = &mock1;
		AgentEvalMatrixResult mr1 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr1.runsExecuted == 1 && mr1.runsAlreadyComplete == 0, "T15: first invocation executes" );
		Check( !mock1.seenRequests.empty(), "T15: first invocation calls the transport" );
	}

	// (2) Second invocation, unchanged reference bytes: skipped as complete.
	// The pre-flight auth probe still fires ONCE (unconditionally, per
	// resolved provider column, before any per-scenario resume check), so
	// the mock needs exactly one canned probe response.
	{
		MockTransport mock2;
		mock2.responses.push_back( { 200, "{}", "", 1 } );   // auth probe only -- the resumed cell never re-executes
		mo.transport = &mock2;
		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 0 && mr2.runsAlreadyComplete == 1,
		       "T15: second invocation (unchanged reference) is skipped as already-complete" );
		Check( mock2.seenRequests.size() == 1,
		       "T15: second invocation calls the transport exactly ONCE (the auth probe, no cell re-executes)" );
	}

	// (3) MUTATE the compareToImage reference's BYTES in place (same path,
	// checkpoints[] JSON byte-identical) -- P1-1's fix means the cell must
	// be treated as stale and re-executed.
	{
		std::ifstream rf( pngPath.c_str(), std::ios::binary );
		std::string bytes( ( std::istreambuf_iterator<char>( rf ) ), std::istreambuf_iterator<char>() );
		rf.close();
		bytes.push_back( '\0' );   // appended byte -- content differs, path unchanged
		Check( WriteFile( pngPath, bytes ), "T15: mutated the reference image's bytes in place" );

		MockTransport mock3;
		mock3.responses.push_back( { 200, "{}", "", 1 } );   // auth probe
		mock3.responses.push_back( { 200, kBodyFinal, "", 2 } );
		mo.transport = &mock3;
		AgentEvalMatrixResult mr3 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr3.runsExecuted == 1 && mr3.runsAlreadyComplete == 0,
		       "T15: third invocation (mutated compareToImage BYTES, same path) is RE-EXECUTED" );
		Check( !mock3.seenRequests.empty(), "T15: third invocation calls the transport (genuine re-run)" );
	}
}

int main()
{
	std::printf( "=== AgentEvalLiveTransportTest (Eval-harness slice E4: live headless runner) ===\n" );

	TestFactory();
	TestLiveRunViaMock();
	TestLiveBudget();
	TestTransportError();
	TestTransportRetryRespectsLlmBudget();
	TestKeyHygieneRedProve();
	TestLiveLocalProviderTimeoutBudget();
	TestLiveHttp5xxRetrySucceeds();
	TestLiveHttp5xxRetryStillFails();
	TestLiveHttp400NotRetried();
	TestLiveReasoningEffort400RetrySucceeds();
	TestLiveReasoningEffort400RetryPreservesImages();
	TestLiveReasoningEffortMemoizedAcrossSessions();   // MUST follow T15 (shares its model id -- see its header)
	TestLiveTransportRetrySucceeds();
	TestLiveTransportRetryStillFails();
	TestLiveTransportRetryDoesNotStackWith5xx();
	TestLive429RetrySucceeds();
	TestLive429BodyHintParsing();
	TestLive429ExponentialFallback();
	TestLive429ExhaustedAttempts();
	TestLive429WallBudgetStop();
	TestLive429DoesNotConsumeLlmCallsBudget();
	TestRunConfigLoad();
	TestRunEvalMatrix();
	TestRunEvalMatrixManifest();
	TestRunEvalMatrixDuplicateId();
	TestRunEvalMatrixResumeSkipsCompleted();
	TestRunEvalMatrixResumeRerunsPartial();
	TestRunEvalMatrixResumeContentAware();
	TestRunEvalMatrixProviderModelCollision();
	TestRunEvalMatrixAuthProbe();
	TestRunEvalMatrixAuthProbe429NotFatal();
	TestPromptImagesEndToEnd();
	TestPromptImagesMatrixResume();
	TestCompareToImageMatrixResume();

	std::printf( "=== AgentEvalLiveTransportTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
