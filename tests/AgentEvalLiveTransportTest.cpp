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

	// The committed local_shootout.json (5 provider rows: 1 gemini + 4
	// distinct-model local) loads cleanly end-to-end.
	{
		AgentEvalRunConfig cfg; std::string err;
		Check( LoadEvalRunConfig( "evals/runconfigs/local_shootout.json", cfg, err ),
		       "evals/runconfigs/local_shootout.json loads (" + err + ")" );
		Check( cfg.scenarios.size() == 4, "local_shootout.json: 4 scenarios" );
		Check( cfg.providers.size() == 7, "local_shootout.json: 7 providers (gemini + openai + xai + 4 distinct-model local)" );
		int localCount = 0;
		for( std::size_t i = 0; i < cfg.providers.size(); ++i )
			if( cfg.providers[i].provider == "local" ) ++localCount;
		Check( localCount == 4, "local_shootout.json: 4 'local' rows survive loading (not deduped by provider name)" );
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
	// run and never touch the transport.
	{
		MockTransport mock2;   // must NEVER be called
		mo.transport = &mock2;

		AgentEvalMatrixResult mr2 = RunEvalMatrix( cfg, scenarios, mo );
		Check( mr2.runsExecuted == 0, "second invocation: 0 runs executed" );
		Check( mr2.runsAlreadyComplete == 1, "second invocation: 1 run skipped as already-complete" );
		Check( mock2.seenRequests.empty(), "second invocation: the transport was NEVER called" );
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
	TestLiveTransportRetrySucceeds();
	TestLiveTransportRetryStillFails();
	TestLiveTransportRetryDoesNotStackWith5xx();
	TestRunConfigLoad();
	TestRunEvalMatrix();
	TestRunEvalMatrixDuplicateId();
	TestRunEvalMatrixResumeSkipsCompleted();
	TestRunEvalMatrixResumeRerunsPartial();

	std::printf( "=== AgentEvalLiveTransportTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
