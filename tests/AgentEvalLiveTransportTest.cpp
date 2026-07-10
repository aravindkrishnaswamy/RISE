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
// T4: transport failure maps to terminalStatus transport_error.
//----------------------------------------------------------------------
static void TestTransportError()
{
	std::printf( "T4: a transport failure -> transport_error...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	MockTransport mock;
	mock.responses.push_back( { 0, "", "transport error [NSURLErrorDomain -1004]: Could not connect", 5 } );

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "t4_transport_error" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.terminalStatus == "transport_error",
	       "a status-0 transport response -> transport_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.errorMessage.find( "Could not connect" ) != std::string::npos,
	       "the transport's header-free error category propagates to errorMessage" );
	Check( h.result.llmCalls == 0, "no llm round was counted for a transport failure" );
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

int main()
{
	std::printf( "=== AgentEvalLiveTransportTest (Eval-harness slice E4: live headless runner) ===\n" );

	TestFactory();
	TestLiveRunViaMock();
	TestLiveBudget();
	TestTransportError();
	TestKeyHygieneRedProve();
	TestRunConfigLoad();
	TestRunEvalMatrix();
	TestRunEvalMatrixDuplicateId();

	std::printf( "=== AgentEvalLiveTransportTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
