//////////////////////////////////////////////////////////////////////
//
//  AgentEvalReplayTest.cpp - Eval-harness slice E2: the replay backend +
//    scenario runner core (see src/Library/Agent/AgentEvalRunner.h).
//
//  Drives the WHOLE committed seed-scenario suite (evals/scenarios/*.json
//  + evals/fixtures/*.fixture.jsonl) end-to-end through the REAL
//  AgentChatLoop + REAL AgentRpcDispatcher (real renders where a fixture
//  calls render) -- zero network, zero keys, deterministic.
//
//  Coverage:
//    T1  LoadEvalScenario loads the 3 committed seed scenarios with the
//        expected parsed fields (id/title/autonomy default/prompts/
//        budgets/replay.fixture/checkpoints carried opaquely).
//    T2  LoadEvalScenario loud-failure gates: missing "id", scene naming
//        BOTH path and inline, scene naming NEITHER, an unrecognized
//        autonomy string, an empty prompts array -- each returns false
//        with a non-empty, actionable `err`.
//    T3  AgentEvalReplaySource::LoadFromFile on the 3 raw fixtures: right
//        provider + right body count; loud-failure gates (a fixture line
//        missing "body", and a fixture whose second line names a
//        different provider than its first).
//    T4  param_edit scenario end-to-end: terminal status final_text,
//        exactly 1 tool call (propose_patch) + 2 llm rounds, headVersion
//        ADVANCED, the emitted trajectory parses as JSONL with the
//        expected record sequence (session,user,llm,tool,llm,summary),
//        and the E3 seam: the returned dispatcher's session is ALIVE and
//        its ReadDocument() shows the edit really landed (0.9 0.1 0.1).
//    T5  two_tool_observe scenario end-to-end: 2 tool calls (render +
//        read_image dispatched from ONE parallel tool_use turn) + 2 llm
//        rounds; trajectory record sequence session,user,llm,tool,tool,
//        llm,summary.
//    T6  error_path scenario end-to-end: the canned degenerate blank-
//        content end_turn turn -> terminalStatus "provider_error", 1 llm
//        call, 0 tool calls, errorMessage names the degenerate turn (the
//        loop's OWN documented error handling -- AgentChatLoopTest T19 --
//        reached through the runner unmodified).
//    T7  RED-PROVE budget enforcement: two_tool_observe with
//        maxToolCalls=1 stops HONESTLY after the render call -- the
//        SECOND tool call (read_image) in the same parallel-tool_use
//        turn is NEVER dispatched (the trajectory carries exactly ONE
//        `tool` record, named "render", never "read_image").
//    T8  RED-PROVE record-once-replay-forever: param_edit's own emitted
//        trajectory (from T4) is fed BACK in as a NEW replay source
//        (AgentEvalReplaySource::LoadFromFile auto-detects the recorded-
//        trajectory shape) -- the replayed run reproduces the IDENTICAL
//        tool-call sequence and the identical scene edit, with zero
//        access to the original raw fixture file.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentEvalRunner.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"

#include <cstdio>
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
// Small helpers: a fresh scratch runDir, and JSONL file readback.
//----------------------------------------------------------------------
static std::string ScratchRunDir( const char* leaf )
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );   // Windows spelling
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	dir += "rise_agent_eval_replay_test/";
	dir += leaf;
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );   // start clean each run
	return dir;
}

static std::vector<JsonValue> ReadJsonl( const std::string& path )
{
	std::vector<JsonValue> out;
	std::ifstream f( path.c_str(), std::ios::binary );
	std::string line;
	while( std::getline( f, line ) ) {
		if( line.empty() ) continue;
		JsonValue v;
		std::string err;
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

static std::vector<std::string> ToolNameSequence( const std::vector<JsonValue>& records )
{
	std::vector<std::string> out;
	for( std::size_t i = 0; i < records.size(); ++i )
		if( records[i].get( "run_type" ).asString() == "tool" ) out.push_back( records[i].get( "name" ).asString() );
	return out;
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
// T1: LoadEvalScenario on the 3 committed seed scenarios.
//----------------------------------------------------------------------
static void TestLoadSeedScenarios()
{
	std::printf( "T1: LoadEvalScenario on the committed seed scenarios...\n" );

	{
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", s, err ),
		       "param_edit.json loads (" + err + ")" );
		Check( s.id == "param_edit", "param_edit id" );
		Check( !s.title.empty(), "param_edit title" );
		Check( s.scenePath.empty() && !s.sceneInline.empty(), "param_edit carries scene.inline (not path)" );
		Check( s.autonomy == "commit", "param_edit autonomy default is commit" );
		Check( s.prompts.size() == 1 && s.prompts[0] == "Recolor the sphere albedo to red",
		       "param_edit single prompt captured verbatim" );
		Check( s.budgets.maxToolCalls == 4 && s.budgets.maxLlmCalls == 4,
		       "param_edit budgets parsed" );
		Check( s.replayFixturePath == "evals/fixtures/param_edit.fixture.jsonl",
		       "param_edit replay.fixture path captured" );
		// Eval-harness slice E3: param_edit now carries THREE real
		// checkpoints (document param_equals, an untouched guard on
		// pnt_emit, a trajectory maxToolCalls bound) -- E2's own opaque-
		// passthrough contract (LoadEvalScenario does not interpret kinds)
		// still holds; only the E3 checker (AgentEvalCheckTest.cpp) gives
		// them meaning.
		Check( s.checkpoints.isArray() && s.checkpoints.size() == 3,
		       "param_edit checkpoints carried opaquely (3 entries, wired by E3)" );
		Check( s.checkpoints.at( 0 ).get( "kind" ).asString() == "document",
		       "param_edit checkpoint[0].kind readable (opaque passthrough, not interpreted)" );
		Check( s.checkpoints.at( 1 ).get( "kind" ).asString() == "untouched",
		       "param_edit checkpoint[1].kind is the untouched PASS_TO_PASS guard" );
		Check( s.checkpoints.at( 2 ).get( "kind" ).asString() == "trajectory",
		       "param_edit checkpoint[2].kind is the trajectory bound" );
	}
	{
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/two_tool_observe.json", s, err ),
		       "two_tool_observe.json loads (" + err + ")" );
		Check( s.id == "two_tool_observe", "two_tool_observe id" );
		Check( s.budgets.maxToolCalls == 5, "two_tool_observe budgets parsed" );
		// Eval-harness slice E3: a render band + a trajectory bound.
		Check( s.checkpoints.isArray() && s.checkpoints.size() == 2,
		       "two_tool_observe checkpoints carried opaquely (2 entries, wired by E3)" );
		Check( s.checkpoints.at( 0 ).get( "kind" ).asString() == "render",
		       "two_tool_observe checkpoint[0].kind is the render band" );
		Check( s.checkpoints.at( 1 ).get( "kind" ).asString() == "trajectory",
		       "two_tool_observe checkpoint[1].kind is the trajectory bound" );
	}
	{
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/error_path.json", s, err ),
		       "error_path.json loads (" + err + ")" );
		Check( s.id == "error_path", "error_path id" );
		Check( s.budgets.maxToolCalls == -1 && s.budgets.maxLlmCalls == -1 && s.budgets.maxWallMs == -1,
		       "error_path has no budgets object -> defaults are unlimited (-1)" );
		// Eval-harness slice E3: a trajectory terminalStatus bound + a
		// diagnostics clean check (error_path never edits the scene).
		Check( s.checkpoints.isArray() && s.checkpoints.size() == 2,
		       "error_path checkpoints carried opaquely (2 entries, wired by E3)" );
		Check( s.checkpoints.at( 0 ).get( "kind" ).asString() == "trajectory",
		       "error_path checkpoint[0].kind is the trajectory bound" );
		Check( s.checkpoints.at( 1 ).get( "kind" ).asString() == "diagnostics",
		       "error_path checkpoint[1].kind is the diagnostics check" );
	}
}

//----------------------------------------------------------------------
// T2: LoadEvalScenario loud-failure gates.
//----------------------------------------------------------------------
static void TestLoadScenarioGates()
{
	std::printf( "T2: LoadEvalScenario loud-failure gates...\n" );
	const std::string dir = ScratchRunDir( "t2_scenarios" );

	auto tryLoad = [&]( const char* leaf, const std::string& json, const std::string& label ) {
		const std::string path = dir + "/" + leaf;
		Check( WriteFile( path, json ), std::string( label ) + ": wrote the malformed scenario file" );
		AgentEvalScenario s;
		std::string err;
		const bool ok = LoadEvalScenario( path, s, err );
		Check( !ok, std::string( label ) + ": LoadEvalScenario refuses" );
		Check( !err.empty(), std::string( label ) + ": refusal carries a non-empty message" );
	};

	tryLoad( "missing_id.json",
		"{\"title\":\"x\",\"scene\":{\"inline\":\"RISE ASCII SCENE 7\\n\"},\"prompts\":[\"hi\"]}",
		"missing id" );

	tryLoad( "scene_both.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"path\":\"a\",\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"scene names BOTH path and inline" );

	tryLoad( "scene_neither.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{},\"prompts\":[\"hi\"]}",
		"scene names NEITHER path nor inline" );

	tryLoad( "bad_autonomy.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"autonomy\":\"banana\",\"prompts\":[\"hi\"]}",
		"unrecognized autonomy string" );

	tryLoad( "empty_prompts.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[]}",
		"empty prompts array" );

	// Review-round P2: an id used in filesystem paths must be a bare token.
	tryLoad( "traversal_id.json",
		"{\"id\":\"../../evil\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing .. (path traversal)" );
	tryLoad( "slash_id.json",
		"{\"id\":\"a/b\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing a path separator" );

	// interventions (Part B): each malformed shape refuses LOUDLY.
	const char* const ivHead = "{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"],\"interventions\":";
	tryLoad( "iv_not_array.json",
		std::string( ivHead ) + "{\"afterToolCalls\":1}}",
		"interventions is not an array" );
	tryLoad( "iv_elem_not_object.json",
		std::string( ivHead ) + "[\"nope\"]}",
		"intervention element is not an object" );
	tryLoad( "iv_missing_after.json",
		std::string( ivHead ) + "[{\"op\":\"param_edit\",\"target\":\"a\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention missing afterToolCalls" );
	tryLoad( "iv_after_zero.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":0,\"op\":\"param_edit\",\"target\":\"a\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention afterToolCalls < 1" );
	tryLoad( "iv_bad_op.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":1,\"op\":\"remove_chunk\",\"target\":\"a\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention op other than param_edit" );
	tryLoad( "iv_empty_target.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":1,\"op\":\"param_edit\",\"target\":\"\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention empty target" );
	tryLoad( "iv_missing_value.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":1,\"op\":\"param_edit\",\"target\":\"a\",\"param\":\"b\"}]}",
		"intervention missing value" );

	// A WELL-FORMED intervention loads and populates scenario.interventions.
	{
		const std::string path = dir + "/iv_ok.json";
		Check( WriteFile( path, std::string( ivHead ) +
			"[{\"afterToolCalls\":2,\"op\":\"param_edit\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.9 0.1\"}]}" ),
			"iv_ok: wrote the scenario file" );
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ), "iv_ok: a well-formed intervention loads (" + err + ")" );
		Check( s.interventions.size() == 1, "iv_ok: one intervention parsed" );
		if( s.interventions.size() == 1 ) {
			Check( s.interventions[0].afterToolCalls == 2, "iv_ok: afterToolCalls parsed" );
			Check( s.interventions[0].op == "param_edit", "iv_ok: op parsed" );
			Check( s.interventions[0].target == "pnt_albedo" && s.interventions[0].param == "color" &&
				s.interventions[0].value == "0.9 0.9 0.1", "iv_ok: target/param/value parsed" );
		}
	}
}

//----------------------------------------------------------------------
// T3: AgentEvalReplaySource::LoadFromFile.
//----------------------------------------------------------------------
static void TestReplaySourceLoad()
{
	std::printf( "T3: AgentEvalReplaySource::LoadFromFile...\n" );

	{
		AgentEvalReplaySource src;
		std::string err;
		Check( AgentEvalReplaySource::LoadFromFile( "evals/fixtures/param_edit.fixture.jsonl", src, err ),
		       "param_edit fixture loads (" + err + ")" );
		Check( src.Provider() == "anthropic", "param_edit fixture provider is anthropic" );
		Check( src.Total() == 2, "param_edit fixture carries 2 canned bodies" );
	}
	{
		AgentEvalReplaySource src;
		std::string err;
		Check( AgentEvalReplaySource::LoadFromFile( "evals/fixtures/two_tool_observe.fixture.jsonl", src, err ),
		       "two_tool_observe fixture loads (" + err + ")" );
		Check( src.Total() == 2, "two_tool_observe fixture carries 2 canned bodies" );
	}
	{
		AgentEvalReplaySource src;
		std::string err;
		Check( AgentEvalReplaySource::LoadFromFile( "evals/fixtures/error_path.fixture.jsonl", src, err ),
		       "error_path fixture loads (" + err + ")" );
		Check( src.Total() == 1, "error_path fixture carries 1 canned body" );
	}

	// Loud-failure gates.
	const std::string dir = ScratchRunDir( "t3_fixtures" );
	{
		const std::string path = dir + "/missing_body.jsonl";
		WriteFile( path, "{\"provider\":\"anthropic\"}\n" );
		AgentEvalReplaySource src;
		std::string err;
		Check( !AgentEvalReplaySource::LoadFromFile( path, src, err ),
		       "a fixture line missing \"body\" is refused" );
		Check( !err.empty(), "the refusal carries a non-empty message" );
	}
	{
		const std::string path = dir + "/mixed_provider.jsonl";
		WriteFile( path,
			"{\"provider\":\"anthropic\",\"body\":\"{}\"}\n"
			"{\"provider\":\"gemini\",\"body\":\"{}\"}\n" );
		AgentEvalReplaySource src;
		std::string err;
		Check( !AgentEvalReplaySource::LoadFromFile( path, src, err ),
		       "a fixture naming two different providers across lines is refused" );
		Check( !err.empty(), "the mixed-provider refusal carries a non-empty message" );
	}
	{
		// Review-round P2: a RECORDED trajectory that switched provider
		// mid-chat (two session records, different providers) must ALSO
		// refuse -- the harness's own "record once, replay forever" workflow
		// would otherwise feed the second provider's wire-shaped bodies to
		// the first provider's codec.
		const std::string path = dir + "/switched_provider_trajectory.jsonl";
		WriteFile( path,
			"{\"run_type\":\"session\",\"provider\":\"anthropic\",\"gen_ai.request.model\":\"m\"}\n"
			"{\"run_type\":\"llm\",\"response_body\":\"{}\"}\n"
			"{\"run_type\":\"session\",\"provider\":\"gemini\",\"gen_ai.request.model\":\"m\"}\n"
			"{\"run_type\":\"llm\",\"response_body\":\"{}\"}\n" );
		AgentEvalReplaySource src;
		std::string err;
		Check( !AgentEvalReplaySource::LoadFromFile( path, src, err ),
		       "a recorded trajectory that switches provider mid-file is refused" );
		Check( err.find( "switches provider" ) != std::string::npos,
		       "the switched-provider refusal names the mid-file switch" );

		// Control: a single-provider trajectory with one session + two llm
		// records still loads (the switch check must not over-refuse).
		const std::string ok = dir + "/single_provider_trajectory.jsonl";
		WriteFile( ok,
			"{\"run_type\":\"session\",\"provider\":\"anthropic\",\"gen_ai.request.model\":\"m\"}\n"
			"{\"run_type\":\"llm\",\"response_body\":\"{\\\"a\\\":1}\"}\n"
			"{\"run_type\":\"llm\",\"response_body\":\"{\\\"b\\\":2}\"}\n" );
		AgentEvalReplaySource src2;
		std::string err2;
		Check( AgentEvalReplaySource::LoadFromFile( ok, src2, err2 ),
		       "a single-provider trajectory with two llm records loads (" + err2 + ")" );
		Check( src2.Provider() == "anthropic" && src2.Total() == 2,
		       "the single-provider trajectory yields anthropic + 2 bodies" );
	}
}

//----------------------------------------------------------------------
// T4: param_edit end-to-end.
//----------------------------------------------------------------------
static std::string g_paramEditTrajectoryPath;   // reused by T8's record-once-replay-forever red-prove

static void TestParamEditScenario()
{
	std::printf( "T4: param_edit scenario end-to-end...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads" );

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t4_param_edit" );

	AgentEvalRunHandle h = RunScenario( scenario, opts );

	Check( h.result.terminalStatus == "final_text",
	       "param_edit terminal status is final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "param_edit drove exactly 2 llm rounds" );
	Check( h.result.toolCalls == 1, "param_edit dispatched exactly 1 tool call" );
	Check( !h.result.budgetHit, "param_edit did not hit any budget" );
	Check( h.result.headVersionFinal > h.result.headVersionStart,
	       "param_edit headVersion ADVANCED (start=" + std::to_string( h.result.headVersionStart ) +
	       " final=" + std::to_string( h.result.headVersionFinal ) + ")" );
	Check( h.result.finalText == "Done: pnt_albedo is now red.", "param_edit final text captured" );
	Check( h.dispatcher != nullptr, "param_edit returns an ALIVE dispatcher (the E3 seam)" );

	if( h.dispatcher && h.dispatcher->Session() ) {
		const std::string doc = h.dispatcher->Session()->ReadDocument();
		Check( doc.find( "0.9 0.1 0.1" ) != std::string::npos,
		       "the post-run session's document really carries the edited color (E3 can interrogate it)" );
	}

	// The emitted trajectory: session,user,llm,tool,llm,summary.
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	std::vector<std::string> seq = RunTypeSequence( recs );
	const std::vector<std::string> expected = { "session", "user", "llm", "tool", "llm", "summary" };
	Check( seq == expected, "param_edit trajectory record sequence is session,user,llm,tool,llm,summary" );
	Check( ToolNameSequence( recs ) == std::vector<std::string>{ "propose_patch" },
	       "param_edit trajectory's one tool record names propose_patch" );

	g_paramEditTrajectoryPath = h.trajectoryPath;
}

//----------------------------------------------------------------------
// T5: two_tool_observe end-to-end.
//----------------------------------------------------------------------
static void TestTwoToolObserveScenario()
{
	std::printf( "T5: two_tool_observe scenario end-to-end...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/two_tool_observe.json", scenario, err ), "two_tool_observe loads" );

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t5_two_tool_observe" );

	AgentEvalRunHandle h = RunScenario( scenario, opts );

	Check( h.result.terminalStatus == "final_text",
	       "two_tool_observe terminal status is final_text (got '" + h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
	Check( h.result.llmCalls == 2, "two_tool_observe drove exactly 2 llm rounds" );
	Check( h.result.toolCalls == 2, "two_tool_observe dispatched exactly 2 tool calls" );
	Check( !h.result.budgetHit, "two_tool_observe did not hit any budget" );

	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	const std::vector<std::string> expected =
		{ "session", "user", "llm", "tool", "tool", "llm", "summary" };
	Check( RunTypeSequence( recs ) == expected,
	       "two_tool_observe trajectory record sequence is session,user,llm,tool,tool,llm,summary" );
	Check( ToolNameSequence( recs ) == ( std::vector<std::string>{ "render", "read_image" } ),
	       "two_tool_observe trajectory tool records are render then read_image, in order" );
}

//----------------------------------------------------------------------
// T6: error_path end-to-end.
//----------------------------------------------------------------------
static void TestErrorPathScenario()
{
	std::printf( "T6: error_path scenario end-to-end...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/error_path.json", scenario, err ), "error_path loads" );

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t6_error_path" );

	AgentEvalRunHandle h = RunScenario( scenario, opts );

	Check( h.result.terminalStatus == "provider_error",
	       "error_path terminal status is provider_error (got '" + h.result.terminalStatus + "')" );
	Check( h.result.llmCalls == 1, "error_path drove exactly 1 llm round before refusing" );
	Check( h.result.toolCalls == 0, "error_path dispatched no tool calls" );
	Check( h.result.errorMessage.find( "no readable text" ) != std::string::npos,
	       "error_path errorMessage names the degenerate-turn refusal (the loop's OWN documented handling): '" +
	       h.result.errorMessage + "'" );
	Check( h.result.headVersionFinal == h.result.headVersionStart,
	       "error_path never edited the scene -- headVersion unchanged" );
}

//----------------------------------------------------------------------
// T7: RED-PROVE budget enforcement.
//----------------------------------------------------------------------
static void TestBudgetEnforcementRedProve()
{
	std::printf( "T7: RED-PROVE budget enforcement (maxToolCalls) -- honest stop...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/two_tool_observe.json", scenario, err ),
	       "two_tool_observe loads for the budget red-prove" );
	scenario.budgets.maxToolCalls = 1;   // the fixture's turn calls 2 tools -- this must trip

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t7_budget" );

	AgentEvalRunHandle h = RunScenario( scenario, opts );

	Check( h.result.terminalStatus == "budget_tool_calls",
	       "budget-trimmed run stops with terminalStatus budget_tool_calls (got '" + h.result.terminalStatus + "')" );
	Check( h.result.budgetHit, "budget-trimmed run reports budgetHit true" );
	Check( h.result.toolCalls == 1, "budget-trimmed run dispatched exactly 1 tool call (the budget cap)" );

	// The honest-stop proof: the trajectory carries exactly ONE tool
	// record, and it is "render" -- "read_image" (the call that WOULD
	// have exceeded the budget) was NEVER dispatched, not merely
	// truncated after the fact.
	std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
	std::vector<std::string> toolNames = ToolNameSequence( recs );
	Check( toolNames.size() == 1 && toolNames[0] == "render",
	       "the runaway second tool call (read_image) never reached the dispatcher -- trajectory shows only render" );
}

//----------------------------------------------------------------------
// T8: RED-PROVE record-once-replay-forever (trajectory-extraction path).
//----------------------------------------------------------------------
static void TestRecordOnceReplayForeverRedProve()
{
	std::printf( "T8: RED-PROVE record-once-replay-forever (trajectory-extraction replay source)...\n" );
	Check( !g_paramEditTrajectoryPath.empty(), "T4's param_edit trajectory path is available" );
	if( g_paramEditTrajectoryPath.empty() ) return;

	// Build a NEW replay source straight from the PREVIOUSLY RECORDED
	// trajectory -- no access to the original raw fixture file at all.
	AgentEvalReplaySource extracted;
	std::string err;
	Check( AgentEvalReplaySource::LoadFromFile( g_paramEditTrajectoryPath, extracted, err ),
	       "the recorded trajectory auto-detects and loads as a replay source (" + err + ")" );
	Check( extracted.Provider() == "anthropic", "the extracted source recovers the provider from the session record" );
	Check( extracted.Total() == 2, "the extracted source recovers both llm response bodies" );

	AgentEvalScenario scenario;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit reloads for the replay" );

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t8_replayed" );
	opts.replaySourceOverride = &extracted;   // bypass scenario.replayFixturePath entirely

	AgentEvalRunHandle h2 = RunScenario( scenario, opts );

	Check( h2.result.terminalStatus == "final_text",
	       "the replayed-from-trajectory run reaches final_text too (got '" + h2.result.terminalStatus + "')" );
	Check( h2.result.toolCalls == 1, "the replayed-from-trajectory run reproduces exactly 1 tool call" );
	Check( h2.result.llmCalls == 2, "the replayed-from-trajectory run reproduces exactly 2 llm rounds" );
	Check( h2.result.headVersionFinal > h2.result.headVersionStart,
	       "the replayed-from-trajectory run reproduces the same committing edit" );

	std::vector<JsonValue> origRecs = ReadJsonl( g_paramEditTrajectoryPath );
	std::vector<JsonValue> replayedRecs = ReadJsonl( h2.trajectoryPath );
	Check( ToolNameSequence( origRecs ) == ToolNameSequence( replayedRecs ),
	       "the replayed run's tool-call NAME sequence is IDENTICAL to the original recording's" );

	if( h2.dispatcher && h2.dispatcher->Session() ) {
		const std::string doc = h2.dispatcher->Session()->ReadDocument();
		Check( doc.find( "0.9 0.1 0.1" ) != std::string::npos,
		       "the replayed-from-trajectory run lands the IDENTICAL scene edit" );
	}
}

int main()
{
	std::printf( "=== AgentEvalReplayTest (Eval-harness slice E2: replay backend + scenario runner) ===\n" );

	TestLoadSeedScenarios();
	TestLoadScenarioGates();
	TestReplaySourceLoad();
	TestParamEditScenario();
	TestTwoToolObserveScenario();
	TestErrorPathScenario();
	TestBudgetEnforcementRedProve();
	TestRecordOnceReplayForeverRedProve();

	std::printf( "=== AgentEvalReplayTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
