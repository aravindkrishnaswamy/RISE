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
//        content end_turn turn -> ChatStepResult::retryDegenerateTurn fires
//        the runner's retry-once policy, the fixture's SECOND canned body
//        (also degenerate) exhausts it -> terminalStatus "provider_error",
//        2 llm calls, 1 degenerate-turn retry, 0 tool calls, errorMessage
//        names the degenerate turn (the loop's OWN documented error
//        handling -- AgentChatLoopTest T19 -- reached through the runner
//        unmodified).
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
//    T9  RunScenario stamps result.jsonl's scenarioFileFnv, and it equals
//        an INDEPENDENT FNV-1a64 recompute over the scenario file's raw
//        bytes (the C++-side half of the cross-language pin against
//        tools/eval_report.py's --selftest).
//    T10 The iteration-cap fix, through the eval path: a scenario with
//        budgets 40/40 raises the loop's per-turn cap to 40 so 25
//        consecutive single-tool-call rounds (past the GUI-posture
//        default of 20) all reach final_text (the direct regression for
//        the live-vision-baseline 24/24 provider_error("iteration cap")
//        failure); a scenario with NO budgets still trips the compiled-in
//        default of 20 at round 21 -- the GUI posture survives through
//        the eval path.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentEvalRunner.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
// Per-process scratch directory -- see ScratchRunDir below.
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid(), fork(), pipe()
	#include <sys/wait.h>		// waitpid() -- collision-hazard proof below
#endif

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
// Per-process scratch root -- same rationale and convention as
// AgentEvalCheckTest.cpp's ScratchTestRoot()/ScratchRunDir(): a FIXED root
// means two concurrent copies of this binary (stray earlier run, manual
// run alongside the suite, a repeat-run flake-chase loop) clobber each
// other's fixtures/results via the remove_all() below, surfacing as a
// bogus flaky failure.  The pid suffix makes the tree unique per process;
// ScratchTestRoot() lets main() remove the WHOLE tree on exit.
static std::string ScratchTestRoot()
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );   // Windows spelling
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	dir += "rise_agent_eval_replay_test_";
	dir += std::to_string( (long)getpid() );
	return dir;
}

static std::string ScratchRunDir( const char* leaf )
{
	std::string dir = ScratchTestRoot() + "/" + leaf;
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );   // start clean each run
	return dir;
}

//----------------------------------------------------------------------
// Collision-hazard proof: two processes calling ScratchTestRoot() must
// get DISTINCT trees.  See AgentEvalCheckTest.cpp's twin of this test for
// the full rationale; POSIX-only (no fork() on Windows).
//----------------------------------------------------------------------
static void TestScratchRootDiffersAcrossProcesses()
{
#ifndef _WIN32
	std::printf( "Collision-hazard check: ScratchTestRoot() differs across processes...\n" );
	int fds[2];
	if( pipe( fds ) != 0 ) { Check( false, "collision check: pipe() succeeded" ); return; }
	const pid_t child = fork();
	if( child < 0 ) {
		Check( false, "collision check: fork() succeeded" );
		close( fds[0] ); close( fds[1] );
		return;
	}
	if( child == 0 ) {
		close( fds[0] );
		const std::string childRoot = ScratchTestRoot();
		ssize_t w = write( fds[1], childRoot.data(), childRoot.size() );
		(void)w;
		close( fds[1] );
		_exit( 0 );
	}
	close( fds[1] );
	std::string childRoot;
	char buf[4096]; ssize_t n;
	while( ( n = read( fds[0], buf, sizeof( buf ) ) ) > 0 ) childRoot.append( buf, (size_t)n );
	close( fds[0] );
	int status = 0;
	waitpid( child, &status, 0 );

	const std::string parentRoot = ScratchTestRoot();
	Check( !childRoot.empty(), "collision check: child reported a non-empty scratch root" );
	Check( childRoot != parentRoot,
		"collision check: parent (" + parentRoot + ") and child (" + childRoot + ") scratch roots differ" );
#else
	std::printf( "Collision-hazard check: skipped on Windows (no fork()); pid-suffix logic is still exercised by every ScratchRunDir() call above.\n" );
#endif
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

//! Builds a raw replay fixture (the AgentEvalReplaySource "hand-authored
//! canned session" shape -- one JSONL line per turn, {"provider":"...",
//! "body":"..."}) of `nToolRounds` consecutive single-tool-call Anthropic
//! turns (each one tool_use block naming read_document, stop_reason
//! tool_use) followed, when `withFinalText` is true, by ONE plain
//! stop_reason end_turn text turn.  This is the direct shape of the
//! iteration-cap regression: the fixture drives past the loop's compiled-
//! in default per-turn cap (AgentChatLoop::kMaxToolRoundsPerTurn == 20) so
//! the eval-path fix (RunScenarioDriven raising the instance cap to
//! max(default, budgets.maxLlmCalls, budgets.maxToolCalls)) is exercised
//! for real, not just at the AgentChatLoop unit level.  Bodies are built
//! via JsonValue/JsonSerialize so the fixture's outer-line escaping of the
//! inner (itself-JSON) body string is correct by construction.  Writes the
//! fixture to `path` and returns it.
static std::string BuildReadDocumentFixtureFile( const std::string& path, int nToolRounds, bool withFinalText )
{
	std::string out;
	for( int i = 0; i < nToolRounds; ++i ) {
		const std::string body =
			"{\"id\":\"msg_" + std::to_string( i ) + "\",\"type\":\"message\",\"role\":\"assistant\","
			"\"model\":\"claude-sonnet-5\",\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_" +
			std::to_string( i ) + "\",\"name\":\"read_document\",\"input\":{}}],"
			"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
			"\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
		JsonValue line = JsonValue::MakeObject();
		line.set( "provider", JsonValue::MakeString( "anthropic" ) );
		line.set( "body", JsonValue::MakeString( body ) );
		out += JsonSerialize( line ) + "\n";
	}
	if( withFinalText ) {
		const std::string body =
			"{\"id\":\"msg_final\",\"type\":\"message\",\"role\":\"assistant\","
			"\"model\":\"claude-sonnet-5\",\"content\":[{\"type\":\"text\",\"text\":\"Done reading.\"}],"
			"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,"
			"\"usage\":{\"input_tokens\":10,\"output_tokens\":5}}";
		JsonValue line = JsonValue::MakeObject();
		line.set( "provider", JsonValue::MakeString( "anthropic" ) );
		line.set( "body", JsonValue::MakeString( body ) );
		out += JsonSerialize( line ) + "\n";
	}
	WriteFile( path, out );
	return path;
}

//! Test-local, INDEPENDENT reimplementation of FNV-1a 64 (NOT a call into
//! AgentEvalRunner.cpp's anonymous-namespace FnvHex, which this TU cannot
//! see) -- the C++-side half of the Finding-2 cross-language pin. The Python
//! reporter's --selftest pins fnv1a64_hex(b"hello") ==
//! "a430d84680aabd0b"; T9 below pins this implementation against a real
//! stamped result.jsonl instead (both must independently recompute the SAME
//! digest over the SAME file bytes for the staleness check to mean anything).
static std::string TestFnvHex( const std::string& bytes )
{
	std::uint64_t hash = 14695981039346656037ull;
	const std::uint64_t prime = 1099511628211ull;
	for( char ch : bytes ) {
		hash ^= static_cast<unsigned char>( ch );
		hash *= prime;
	}
	char buf[17];
	std::snprintf( buf, sizeof( buf ), "%016llx", static_cast<unsigned long long>( hash ) );
	return std::string( buf );
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
		Check( s.prompts.size() == 1 && s.prompts[0].text == "Recolor the sphere albedo to red" &&
		       s.prompts[0].imagePaths.empty(),
		       "param_edit single prompt captured verbatim (text-only, no images)" );
		Check( s.budgets.maxToolCalls == 10 && s.budgets.maxLlmCalls == 12,
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
		Check( s.budgets.maxToolCalls == 10 && s.budgets.maxLlmCalls == 12, "two_tool_observe budgets parsed" );
		// Eval-harness slice E3: a render band + a trajectory bound + a
		// finalText check (Wave B: requiredToolInOrder/finalText added to
		// discriminate the zero-tool-call dodge -- see evals/README.md).
		Check( s.checkpoints.isArray() && s.checkpoints.size() == 3,
		       "two_tool_observe checkpoints carried opaquely (3 entries, wired by E3)" );
		Check( s.checkpoints.at( 0 ).get( "kind" ).asString() == "render",
		       "two_tool_observe checkpoint[0].kind is the render band" );
		Check( s.checkpoints.at( 1 ).get( "kind" ).asString() == "trajectory",
		       "two_tool_observe checkpoint[1].kind is the trajectory bound" );
		Check( s.checkpoints.at( 2 ).get( "kind" ).asString() == "finalText",
		       "two_tool_observe checkpoint[2].kind is the finalText check" );
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
	tryLoad( "budget_fractional.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"],\"budgets\":{\"maxToolCalls\":1.5}}",
		"fractional maxToolCalls" );
	tryLoad( "budget_infinite.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"],\"budgets\":{\"maxWallMs\":1e999}}",
		"non-finite maxWallMs" );

	// Review-round P2: an id used in filesystem paths must be a bare token.
	tryLoad( "traversal_id.json",
		"{\"id\":\"../../evil\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing .. (path traversal)" );
	tryLoad( "slash_id.json",
		"{\"id\":\"a/b\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing a path separator" );

	// Finding 1a: the id-guard now ALSO rejects any character outside
	// [A-Za-z0-9._-] (so SanitizeForPath(id) == id always) and any id
	// containing "__" (the run-dir field separator).
	tryLoad( "colon_id.json",
		"{\"id\":\"scenario:v2\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing ':' (not in [A-Za-z0-9._-])" );
	tryLoad( "dunder_id.json",
		"{\"id\":\"scenario__v2\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}",
		"id containing \"__\" (the run-dir field separator)" );

	// Control: a normal (single-underscore, alnum) id still loads cleanly --
	// the hardened guard must not over-refuse the committed-suite shape.
	{
		const std::string path = dir + "/normal_id.json";
		Check( WriteFile( path,
			"{\"id\":\"a_normal_id-2\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[\"hi\"]}" ),
			"normal_id: wrote the scenario file" );
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ), "normal_id: a bare alnum/-/._ id still loads (" + err + ")" );
		Check( s.id == "a_normal_id-2", "normal_id: id parsed unchanged" );
		Check( s.sourcePath == path, "normal_id: sourcePath stamped to the loaded path" );
	}

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
	tryLoad( "iv_after_fractional.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":1.5,\"op\":\"param_edit\",\"target\":\"a\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention afterToolCalls must be whole" );
	tryLoad( "iv_after_infinite.json",
		std::string( ivHead ) + "[{\"afterToolCalls\":1e999,\"op\":\"param_edit\",\"target\":\"a\",\"param\":\"b\",\"value\":\"c\"}]}",
		"intervention afterToolCalls must be finite" );
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

	// Image-reconstruction Wave 1: prompts[] object-form loud-failure gates
	// + the two accepted object shapes (text+images, images-only).
	tryLoad( "prompt_images_empty_array.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},"
		"\"prompts\":[{\"text\":\"look\",\"images\":[]}]}",
		"prompt object: images present but empty array" );
	tryLoad( "prompt_images_empty_path.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},"
		"\"prompts\":[{\"text\":\"look\",\"images\":[\"\"]}]}",
		"prompt object: images[] contains an empty path string" );
	tryLoad( "prompt_images_traversal.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},"
		"\"prompts\":[{\"text\":\"look\",\"images\":[\"../evil.png\"]}]}",
		"prompt object: images[] path contains \"..\"" );
	tryLoad( "prompt_neither_string_nor_object.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[42]}",
		"prompt array element is neither a string nor an object" );
	tryLoad( "prompt_object_neither_text_nor_images.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[{}]}",
		"prompt object carries neither \"text\" nor \"images\"" );
	tryLoad( "prompt_object_blank_text_no_images.json",
		"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},\"prompts\":[{\"text\":\"\"}]}",
		"prompt object: empty \"text\" and no \"images\" is equivalent to neither" );

	// Positive: the object form with BOTH text and images loads and is
	// parsed into AgentEvalPrompt as expected.  The referenced paths need
	// not exist on disk -- LoadEvalScenario validates shape only; existence
	// is RunScenarioDriven's pre-flight job (see TestPromptImagesEndToEnd
	// in AgentEvalLiveTransportTest.cpp).
	{
		const std::string path = dir + "/prompt_text_and_images.json";
		Check( WriteFile( path,
			"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},"
			"\"prompts\":[{\"text\":\"Match this reference photo.\","
			"\"images\":[\"evals/refs/a.png\",\"evals/refs/b.jpg\"]}]}" ),
			"prompt_text_and_images: wrote the scenario file" );
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ),
			"prompt_text_and_images: text+images object form loads (" + err + ")" );
		Check( s.prompts.size() == 1 && s.prompts[0].text == "Match this reference photo." &&
			s.prompts[0].imagePaths.size() == 2 &&
			s.prompts[0].imagePaths[0] == "evals/refs/a.png" &&
			s.prompts[0].imagePaths[1] == "evals/refs/b.jpg",
			"prompt_text_and_images: text + both image paths parsed" );
	}

	// Positive: the object form with images ONLY (no "text" key) -- an
	// attachment-only turn.
	{
		const std::string path = dir + "/prompt_images_only.json";
		Check( WriteFile( path,
			"{\"id\":\"x\",\"title\":\"x\",\"scene\":{\"inline\":\"b\"},"
			"\"prompts\":[{\"images\":[\"evals/refs/a.png\"]}]}" ),
			"prompt_images_only: wrote the scenario file" );
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ),
			"prompt_images_only: images-only object form loads (" + err + ")" );
		Check( s.prompts.size() == 1 && s.prompts[0].text.empty() &&
			s.prompts[0].imagePaths.size() == 1 && s.prompts[0].imagePaths[0] == "evals/refs/a.png",
			"prompt_images_only: blank text + one image path parsed" );
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
		// 2 bodies (not 1): the degenerate-blank-turn retry-once policy
		// (ChatStepResult::retryDegenerateTurn) makes the runner fetch a
		// SECOND canned body to prove replay-side exhaustion -- see T6.
		Check( src.Total() == 2, "error_path fixture carries 2 canned bodies" );
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
	Check( h.result.degenerateTurnRetries == 0,
	       "param_edit is a genuinely normal successful run -- no degenerate-blank-turn retry was consumed" );
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

	// The final assembled scene is persisted beside the result, so a run can
	// be re-rendered/inspected later (renders are in-memory-only otherwise).
	{
		const std::string scenePath =
			( std::filesystem::path( opts.runDir ) / "two_tool_observe.final.RISEscene" ).string();
		std::ifstream sf( scenePath.c_str(), std::ios::binary );
		Check( sf.good(), "run wrote <scenario>.final.RISEscene beside the result" );
		std::ostringstream ss; ss << sf.rdbuf();
		const std::string scene = ss.str();
		Check( !scene.empty(), "final scene file is non-empty" );
		Check( scene.rfind( "RISE ASCII SCENE", 0 ) == 0,
		       "final scene file is a re-loadable .RISEscene (starts with the header)" );
	}
}

//----------------------------------------------------------------------
// T6: error_path end-to-end.  The fixture's canned body is a degenerate
//     blank turn (content:[], stop_reason end_turn), which the codec
//     refuses with ChatStepResult::retryDegenerateTurn set -- so the
//     runner's retry-once policy fetches a SECOND canned body from the
//     fixture (also blank) before falling through to the terminal
//     provider_error.  Proves replay-side exhaustion of the retry: the
//     fixture carries 2 bodies (not 1) specifically so this round-trips
//     through the replay source instead of hitting "replay_exhausted".
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
	Check( h.result.llmCalls == 2, "error_path drove exactly 2 llm rounds -- the degenerate turn's one retry, then exhaustion" );
	Check( h.result.degenerateTurnRetries == 1, "error_path consumed exactly 1 degenerate-turn retry before exhaustion" );
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

//----------------------------------------------------------------------
// T9: Finding 2 -- RunScenario stamps scenarioFileFnv, and it equals an
// INDEPENDENT FNV-1a64 recompute over the scenario file's raw bytes (the
// C++-side half of the cross-language pin; see TestFnvHex above and
// tools/eval_report.py's --selftest fnv1a64_hex(b"hello") pin).
//----------------------------------------------------------------------
static void TestScenarioFileFnvStamp()
{
	std::printf( "T9: RunScenario stamps scenarioFileFnv == independent FNV-1a64 recompute...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
	       "param_edit loads for the FNV pin (" + err + ")" );
	Check( !scenario.sourcePath.empty(), "LoadEvalScenario stamped sourcePath" );

	AgentEvalRunOptions opts;
	opts.runDir = ScratchRunDir( "t9_fnv_pin" );

	AgentEvalRunHandle h = RunScenario( scenario, opts );
	Check( h.result.terminalStatus == "final_text",
	       "param_edit (FNV pin run) reaches final_text (got '" + h.result.terminalStatus + "')" );
	Check( !h.resultPath.empty(), "the FNV pin run wrote a result.jsonl" );

	std::ifstream rf( h.resultPath.c_str(), std::ios::binary );
	std::string line;
	Check( static_cast<bool>( std::getline( rf, line ) ), "result.jsonl has a line to parse" );
	JsonValue r;
	std::string perr;
	Check( JsonParse( line, r, perr ), "result.jsonl line parses as JSON (" + perr + ")" );

	Check( r.has( "scenarioFileFnv" ) && r.get( "scenarioFileFnv" ).isString(),
	       "result.jsonl stamps a string scenarioFileFnv" );

	// Independent recompute over the RAW bytes of the exact file
	// LoadEvalScenario read (scenario.sourcePath) -- this is the pin: the
	// test's own FNV-1a64 (TestFnvHex, a separate implementation from
	// AgentEvalRunner.cpp's file-local FnvHex) must land on the SAME digest
	// C++ production code stamped.
	std::ifstream sf( scenario.sourcePath.c_str(), std::ios::binary );
	std::ostringstream ss;
	ss << sf.rdbuf();
	const std::string expected = TestFnvHex( ss.str() );

	Check( r.get( "scenarioFileFnv" ).asString() == expected,
	       "stamped scenarioFileFnv (" + r.get( "scenarioFileFnv" ).asString() +
	       ") matches an independent FNV-1a64 recompute (" + expected +
	       ") over evals/scenarios/param_edit.json's bytes" );

	// param_edit's scene is INLINE (no scene.path), so sceneFileFnv must be
	// OMITTED entirely -- absent means "not applicable", never a stale flag.
	Check( !r.has( "sceneFileFnv" ), "param_edit (inline scene) does not stamp sceneFileFnv" );
}

//----------------------------------------------------------------------
// T9b: result.jsonl always carries a "degenerateTurnRetries" key (the field
//      is emitted unconditionally, even when 0 -- see the "Always emitted"
//      comment beside AgentEvalRunner.cpp's r.set("degenerateTurnRetries",
//      ...)).  Covers BOTH the 0 case (a normal run, param_edit) and the
//      rescue/exhaustion case (error_path, whose T6 already pins the
//      in-memory h.result.degenerateTurnRetries == 1 -- this test proves
//      the SAME value round-trips through the on-disk result line, not just
//      the in-process AgentEvalRunResult).
//----------------------------------------------------------------------
static void TestResultJsonlDegenerateTurnRetriesField()
{
	std::printf( "T9b: result.jsonl stamps degenerateTurnRetries (0 case + rescue/exhaustion case)...\n" );

	// (A) 0 case: param_edit is a genuinely normal successful run.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T9b(A): param_edit loads" );

		AgentEvalRunOptions opts;
		opts.runDir = ScratchRunDir( "t9b_degenerate_field_zero" );

		AgentEvalRunHandle h = RunScenario( scenario, opts );
		Check( h.result.terminalStatus == "final_text",
		       "T9b(A): param_edit reaches final_text (got '" + h.result.terminalStatus + "')" );
		Check( !h.resultPath.empty(), "T9b(A): the run wrote a result.jsonl" );

		std::ifstream rf( h.resultPath.c_str(), std::ios::binary );
		std::string line;
		Check( static_cast<bool>( std::getline( rf, line ) ), "T9b(A): result.jsonl has a line to parse" );
		JsonValue r;
		std::string perr;
		Check( JsonParse( line, r, perr ), "T9b(A): result.jsonl line parses as JSON (" + perr + ")" );

		Check( r.has( "degenerateTurnRetries" ), "T9b(A): result.jsonl carries the degenerateTurnRetries key" );
		Check( r.get( "degenerateTurnRetries" ).asNumber( -1.0 ) == 0.0,
		       "T9b(A): a normal successful run stamps degenerateTurnRetries == 0" );
	}

	// (B) rescue/exhaustion case: error_path's fixture forces exactly 1
	//     degenerate-turn retry before exhaustion (see T6).
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/error_path.json", scenario, err ),
		       "T9b(B): error_path loads" );

		AgentEvalRunOptions opts;
		opts.runDir = ScratchRunDir( "t9b_degenerate_field_one" );

		AgentEvalRunHandle h = RunScenario( scenario, opts );
		Check( h.result.terminalStatus == "provider_error",
		       "T9b(B): error_path reaches provider_error (got '" + h.result.terminalStatus + "')" );
		Check( !h.resultPath.empty(), "T9b(B): the run wrote a result.jsonl" );

		std::ifstream rf( h.resultPath.c_str(), std::ios::binary );
		std::string line;
		Check( static_cast<bool>( std::getline( rf, line ) ), "T9b(B): result.jsonl has a line to parse" );
		JsonValue r;
		std::string perr;
		Check( JsonParse( line, r, perr ), "T9b(B): result.jsonl line parses as JSON (" + perr + ")" );

		Check( r.has( "degenerateTurnRetries" ), "T9b(B): result.jsonl carries the degenerateTurnRetries key" );
		Check( r.get( "degenerateTurnRetries" ).asNumber( -1.0 ) == 1.0,
		       "T9b(B): the rescue-then-exhaustion run stamps degenerateTurnRetries == 1 on disk, "
		       "matching the in-memory h.result.degenerateTurnRetries T6 already pins" );
	}
}

//----------------------------------------------------------------------
// T10: the iteration-cap fix, exercised through the eval path (the direct
// regression for the live-vision-baseline failure: RunScenarioDriven
// raising the loop's per-turn cap to max(default, budgets.maxLlmCalls,
// budgets.maxToolCalls) right after loop.SetProvider() in
// AgentEvalRunner.cpp).
//----------------------------------------------------------------------
static void TestIterationCapBudgetRaise()
{
	std::printf( "T10: the eval-path iteration-cap raise (25 tool rounds under budgets 40/40; "
	             "a no-budgets scenario still trips at the compiled-in default)...\n" );

	// (a) budgets 40/40 -> the instance cap is raised to 40, so 25
	//     consecutive single-tool-call rounds (well past the GUI-posture
	//     default of 20) all succeed and the scenario reaches final_text.
	//     Pre-fix (the loop's compiled-in default of 20 governing every
	//     host unconditionally), this run dies at round 21 with
	//     terminalStatus provider_error / errorMessage naming "iteration
	//     cap...20...".
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T10a: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "iteration_cap_raised";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Read the document repeatedly." ) );
		scenario.budgets.maxToolCalls = 40;
		scenario.budgets.maxLlmCalls  = 40;
		scenario.replayFixturePath.clear();   // fed via replaySourceOverride instead

		const std::string dir = ScratchRunDir( "t10a_raised_cap" );
		const std::string fixturePath =
			BuildReadDocumentFixtureFile( dir + "/fixture.jsonl", 25, /*withFinalText=*/true );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T10a: the 26-line (25 tool + 1 final-text) fixture loads (" + err + ")" );
		Check( src.Total() == 26, "T10a: fixture carries 26 canned bodies" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );

		Check( h.result.terminalStatus == "final_text",
		       "T10a: budgets 40/40 raise the cap enough for all 25 tool rounds to reach final_text (got '" +
		       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
		Check( h.result.toolCalls == 25, "T10a: exactly 25 tool calls dispatched" );
		Check( h.result.llmCalls == 26, "T10a: exactly 26 llm rounds (25 tool_use + 1 final text)" );
		Check( !h.result.budgetHit, "T10a: neither budget tripped (40/40 comfortably covers 25/26)" );
	}

	// (b) NO budgets object (all three fields at their -1 "unlimited"
	//     default) -> RunScenarioDriven's cap computes to
	//     max(20,-1,-1) == the compiled-in default, so the GUI posture is
	//     PRESERVED through the eval path: kMaxToolRoundsPerTurn rounds
	//     succeed, the next trips naming that compiled-in default, never a
	//     raised one.  Symbolic on the constant: the cap is a tuning knob
	//     (raised 20 -> 100 in 2026-07 when it began preempting real
	//     multi-chunk scene builds), and this test asserts the PLUMBING, not
	//     the number.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T10b: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "iteration_cap_default";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Read the document repeatedly." ) );
		scenario.budgets = AgentEvalBudgets();   // -1/-1/-1: the "no budgets" scenario shape (matches error_path.json)
		scenario.replayFixturePath.clear();

		const std::string dir = ScratchRunDir( "t10b_default_cap" );
		const std::string fixturePath =
			BuildReadDocumentFixtureFile( dir + "/fixture.jsonl",
				AgentChatLoop::kMaxToolRoundsPerTurn + 1, /*withFinalText=*/false );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T10b: the (cap+1)-tool-round fixture loads (" + err + ")" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );

		Check( h.result.terminalStatus == "provider_error",
		       "T10b: a no-budgets scenario still trips the default cap (got '" +
		       h.result.terminalStatus + "')" );
		Check( h.result.toolCalls == AgentChatLoop::kMaxToolRoundsPerTurn,
		       "T10b: exactly the cap's worth of tool calls dispatched before it trips" );
		Check( h.result.errorMessage.find( "iteration cap" ) != std::string::npos,
		       "T10b: errorMessage names the iteration cap" );
		Check( h.result.errorMessage.find( std::to_string( AgentChatLoop::kMaxToolRoundsPerTurn ) ) != std::string::npos,
		       "T10b: errorMessage names the compiled-in default, not a raised cap" );
	}
}

//----------------------------------------------------------------------
// T11: ask_user (stage 1a of clarifying-questions) mid-scenario -- the
// runner intercepts the call BEFORE dispatcher->HandleLine (ask_user has
// NO AgentRpc verb; see AgentChatCodecs.cpp / AgentRpc.cpp, untouched)
// and synthesizes a SUCCESS available:false result itself.  Proves:
//   (a) the run CONTINUES to final_text -- ask_user is never treated as
//       an error, so it does not derail the scenario;
//   (b) the trajectory records the tool call by name (ask_user) with the
//       SYNTHESIZED response embedded (available:false + the "no
//       interactive user" note) -- exactly like a real dispatched verb's
//       jsonrpc.response, just never having reached HandleLine;
//   (c) the tool-call budget counted it as a REAL round-trip: (c1) a
//       budget of exactly 1 is satisfied without tripping, and (c2) a
//       budget of 0 stops the run BEFORE ask_user is dispatched at all
//       (the RED-PROVE half -- matches T7's honest-stop pattern).
//----------------------------------------------------------------------
static void TestAskUserMidScenario()
{
	std::printf( "T11: ask_user mid-scenario -- runner-synthesized available:false, run continues...\n" );

	// The fixture: one Anthropic turn calling ask_user with a real
	// options-carrying question (the shape a model would actually send
	// per AgentChatCodecs.cpp's ask_user description), followed by a
	// plain end_turn text turn -- the model proceeding on its own
	// judgment after the synthesized available:false comes back, exactly
	// as the tool's description instructs.
	auto buildFixture = []( const std::string& path ) -> std::string {
		std::string out;
		{
			const std::string body =
				"{\"id\":\"msg_ask\",\"type\":\"message\",\"role\":\"assistant\","
				"\"model\":\"claude-sonnet-5\",\"content\":["
				"{\"type\":\"text\",\"text\":\"Let me check the intended mood first.\"},"
				"{\"type\":\"tool_use\",\"id\":\"toolu_ask1\",\"name\":\"ask_user\","
				"\"input\":{\"question\":\"Should the mood be warm sunset or cool overcast?\","
				"\"options\":[\"warm sunset\",\"cool overcast\"]}}],"
				"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
				"\"usage\":{\"input_tokens\":50,\"output_tokens\":20}}";
			JsonValue line = JsonValue::MakeObject();
			line.set( "provider", JsonValue::MakeString( "anthropic" ) );
			line.set( "body", JsonValue::MakeString( body ) );
			out += JsonSerialize( line ) + "\n";
		}
		{
			const std::string body =
				"{\"id\":\"msg_final\",\"type\":\"message\",\"role\":\"assistant\","
				"\"model\":\"claude-sonnet-5\",\"content\":[{\"type\":\"text\",\"text\":"
				"\"No one was available to answer, so I proceeded with a warm sunset mood.\"}],"
				"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,"
				"\"usage\":{\"input_tokens\":30,\"output_tokens\":15}}";
			JsonValue line = JsonValue::MakeObject();
			line.set( "provider", JsonValue::MakeString( "anthropic" ) );
			line.set( "body", JsonValue::MakeString( body ) );
			out += JsonSerialize( line ) + "\n";
		}
		WriteFile( path, out );
		return path;
	};

	// (a) + (b): the plain run, no tightened budget.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T11a: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "ask_user_mid_scenario";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Build me a sunset scene" ) );
		scenario.replayFixturePath.clear();   // fed via replaySourceOverride instead

		const std::string dir = ScratchRunDir( "t11a_ask_user" );
		const std::string fixturePath = buildFixture( dir + "/fixture.jsonl" );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T11a: the ask_user + final-text fixture loads (" + err + ")" );
		Check( src.Total() == 2, "T11a: fixture carries 2 canned bodies" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );

		// (a) the run CONTINUES -- ask_user is not treated as an error and
		// does not derail the scenario.
		Check( h.result.terminalStatus == "final_text",
		       "T11a: the run reaches final_text despite calling ask_user (got '" +
		       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
		Check( !h.result.budgetHit, "T11a: no budget tripped" );
		Check( h.result.llmCalls == 2, "T11a: exactly 2 llm rounds (ask_user turn + final text turn)" );
		Check( h.result.toolCalls == 1, "T11a: exactly 1 tool call dispatched (ask_user itself)" );
		Check( h.result.finalText ==
		       "No one was available to answer, so I proceeded with a warm sunset mood.",
		       "T11a: final text captured -- the model proceeded on its own judgment" );

		// (b) the trajectory: session,user,llm,tool,llm,summary, the tool
		// record names ask_user, and its jsonrpc.response embeds the
		// SYNTHESIZED available:false result (never having reached
		// dispatcher->HandleLine -- ask_user has no AgentRpc verb at all).
		std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
		const std::vector<std::string> expectedSeq = { "session", "user", "llm", "tool", "llm", "summary" };
		Check( RunTypeSequence( recs ) == expectedSeq,
		       "T11a: trajectory record sequence is session,user,llm,tool,llm,summary" );
		Check( ToolNameSequence( recs ) == std::vector<std::string>{ "ask_user" },
		       "T11a: the trajectory's one tool record names ask_user" );

		bool sawToolRecord = false;
		for( std::size_t i = 0; i < recs.size(); ++i ) {
			if( recs[i].get( "run_type" ).asString() != "tool" ) continue;
			sawToolRecord = true;
			Check( recs[i].get( "name" ).asString() == "ask_user", "T11a: tool record name is ask_user" );
			Check( !recs[i].get( "error" ).asBool(),
			       "T11a: the synthesized result is recorded as a SUCCESS, not a JSON-RPC error" );
			const JsonValue& resp = recs[i].get( "jsonrpc.response" );
			Check( resp.get( "result" ).get( "available" ).asBool() == false,
			       "T11a: the synthesized response carries available:false" );
			Check( resp.get( "result" ).get( "note" ).asString().find( "no interactive user is available" ) !=
			       std::string::npos,
			       "T11a: the synthesized response's note explains no interactive user is available" );
		}
		Check( sawToolRecord, "T11a: a tool record was actually found and inspected" );
	}

	// (c1) a tool-call budget of exactly 1 is satisfied -- ask_user counts
	// as the one allotted call, and the run still reaches final_text.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T11c1: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "ask_user_budget_exact";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Build me a sunset scene" ) );
		scenario.budgets.maxToolCalls = 1;
		scenario.replayFixturePath.clear();

		const std::string dir = ScratchRunDir( "t11c1_ask_user_budget_exact" );
		const std::string fixturePath = buildFixture( dir + "/fixture.jsonl" );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T11c1: fixture loads (" + err + ")" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );
		Check( h.result.terminalStatus == "final_text",
		       "T11c1: maxToolCalls=1 comfortably covers the single ask_user call (got '" +
		       h.result.terminalStatus + "')" );
		Check( h.result.toolCalls == 1, "T11c1: exactly 1 tool call counted" );
		Check( !h.result.budgetHit, "T11c1: budget not hit -- 1 call against a budget of 1 is within bounds" );
	}

	// (c2) RED-PROVE: a tool-call budget of 0 stops the run BEFORE
	// ask_user is ever dispatched -- the honest-stop contract T7 proves
	// for a dispatched verb holds identically for the intercepted one.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T11c2: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "ask_user_budget_zero";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Build me a sunset scene" ) );
		scenario.budgets.maxToolCalls = 0;
		scenario.replayFixturePath.clear();

		const std::string dir = ScratchRunDir( "t11c2_ask_user_budget_zero" );
		const std::string fixturePath = buildFixture( dir + "/fixture.jsonl" );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T11c2: fixture loads (" + err + ")" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );
		Check( h.result.terminalStatus == "budget_tool_calls",
		       "T11c2: maxToolCalls=0 trips honestly before ask_user ever dispatches (got '" +
		       h.result.terminalStatus + "')" );
		Check( h.result.budgetHit, "T11c2: budgetHit is true" );
		Check( h.result.toolCalls == 0, "T11c2: zero tool calls dispatched -- ask_user never ran" );

		std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
		Check( ToolNameSequence( recs ).empty(),
		       "T11c2: the trajectory carries NO tool record -- the honest stop happened before dispatch" );
	}
}

//----------------------------------------------------------------------
// T12: PARALLEL MIXED TURN -- ONE assistant turn carries BOTH ask_user
// AND a real dispatched tool call (read_document), as two parallel
// tool_use blocks.  Proves the runner's interception (the `if(
// call.name == "ask_user" )` branch inside RunScenarioDriven's per-call
// loop) discriminates by NAME within a batch, not by turn: ask_user is
// intercepted+synthesized, read_document is dispatched for REAL through
// the live AgentRpcDispatcher (its jsonrpc.response carries the ACTUAL
// document text -- proof it was not also intercepted), the tool-call
// budget counts BOTH (T12b RED-PROVEs this: a budget of 1 stops the run
// right after ask_user -- the FIRST call in the turn -- so
// read_document, the second call in the SAME turn, never dispatches at
// all), and the trajectory carries both tool records in the SAME order
// the model requested them.  (The companion wire-invariant proof -- that
// both results ride in ONE packed message on the actual wire -- lives in
// AgentChatLoopTest.cpp's T39d; this runner-level test cannot inspect
// the built request body directly since the replay fetch never sees it,
// only the trajectory + result the run produces.)
//----------------------------------------------------------------------
static void TestAskUserParallelWithDispatchedTool()
{
	std::printf( "T12: ask_user PARALLEL with a dispatched tool call (read_document) "
	             "in the SAME turn...\n" );

	// The fixture: ONE Anthropic turn with two parallel tool_use blocks --
	// ask_user FIRST, read_document SECOND -- followed by a plain
	// end_turn text turn.  ask_user first (rather than last) is
	// deliberate: it makes T12b's budget=1 RED-PROVE unambiguous (the
	// ONE dispatched call must be ask_user itself, not read_document
	// happening to be first).
	auto buildFixture = []( const std::string& path ) -> std::string {
		std::string out;
		{
			const std::string body =
				"{\"id\":\"msg_parallel\",\"type\":\"message\",\"role\":\"assistant\","
				"\"model\":\"claude-sonnet-5\",\"content\":["
				"{\"type\":\"text\",\"text\":\"Let me confirm the mood, then read the document.\"},"
				"{\"type\":\"tool_use\",\"id\":\"toolu_ask_par\",\"name\":\"ask_user\","
				"\"input\":{\"question\":\"Should the mood be warm sunset or cool overcast?\","
				"\"options\":[\"warm sunset\",\"cool overcast\"]}},"
				"{\"type\":\"tool_use\",\"id\":\"toolu_doc_par\",\"name\":\"read_document\",\"input\":{}}],"
				"\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
				"\"usage\":{\"input_tokens\":60,\"output_tokens\":25}}";
			JsonValue line = JsonValue::MakeObject();
			line.set( "provider", JsonValue::MakeString( "anthropic" ) );
			line.set( "body", JsonValue::MakeString( body ) );
			out += JsonSerialize( line ) + "\n";
		}
		{
			const std::string body =
				"{\"id\":\"msg_final\",\"type\":\"message\",\"role\":\"assistant\","
				"\"model\":\"claude-sonnet-5\",\"content\":[{\"type\":\"text\",\"text\":"
				"\"Proceeding with a warm sunset mood, using the document I read.\"}],"
				"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,"
				"\"usage\":{\"input_tokens\":30,\"output_tokens\":15}}";
			JsonValue line = JsonValue::MakeObject();
			line.set( "provider", JsonValue::MakeString( "anthropic" ) );
			line.set( "body", JsonValue::MakeString( body ) );
			out += JsonSerialize( line ) + "\n";
		}
		WriteFile( path, out );
		return path;
	};

	// (a) unbudgeted: BOTH calls dispatch (one intercepted, one real),
	// the run reaches final_text, and the trajectory shows both, in order.
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T12a: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "ask_user_parallel_mixed";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Check the mood with me, then read the document" ) );
		scenario.replayFixturePath.clear();   // fed via replaySourceOverride instead

		const std::string dir = ScratchRunDir( "t12a_ask_user_parallel" );
		const std::string fixturePath = buildFixture( dir + "/fixture.jsonl" );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T12a: the parallel ask_user+read_document fixture loads (" + err + ")" );
		Check( src.Total() == 2, "T12a: fixture carries 2 canned bodies" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );

		Check( h.result.terminalStatus == "final_text",
		       "T12a: the run reaches final_text with a mixed ask_user+read_document turn (got '" +
		       h.result.terminalStatus + "': " + h.result.errorMessage + ")" );
		Check( !h.result.budgetHit, "T12a: no budget tripped" );
		Check( h.result.llmCalls == 2, "T12a: exactly 2 llm rounds" );
		Check( h.result.toolCalls == 2,
		       "T12a: exactly 2 tool calls dispatched -- ask_user AND read_document BOTH count" );

		std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
		const std::vector<std::string> expectedSeq = { "session", "user", "llm", "tool", "tool", "llm", "summary" };
		Check( RunTypeSequence( recs ) == expectedSeq,
		       "T12a: trajectory record sequence is session,user,llm,tool,tool,llm,summary" );
		Check( ToolNameSequence( recs ) == ( std::vector<std::string>{ "ask_user", "read_document" } ),
		       "T12a: the trajectory's two tool records are ask_user then read_document, IN ORDER "
		       "(matching the model's parallel tool_use array order)" );

		int toolRecordsSeen = 0;
		for( std::size_t i = 0; i < recs.size(); ++i ) {
			if( recs[i].get( "run_type" ).asString() != "tool" ) continue;
			++toolRecordsSeen;
			const std::string name = recs[i].get( "name" ).asString();
			Check( !recs[i].get( "error" ).asBool(),
			       "T12a: " + name + "'s result is recorded as a SUCCESS, not a JSON-RPC error" );
			const JsonValue& resp = recs[i].get( "jsonrpc.response" );
			if( name == "ask_user" ) {
				// INTERCEPTED before dispatcher->HandleLine: the
				// synthesized available:false result.
				Check( resp.get( "result" ).get( "available" ).asBool() == false,
				       "T12a: ask_user's response is the synthesized available:false" );
			}
			else if( name == "read_document" ) {
				// DISPATCHED FOR REAL: the actual scene document text
				// rides back -- proof this call genuinely reached
				// AgentRpcDispatcher and was NOT also intercepted.
				Check( resp.get( "result" ).get( "document" ).asString().find( "RISE ASCII SCENE" ) !=
				       std::string::npos,
				       "T12a: read_document's response carries the REAL dispatched document text" );
			}
			else {
				Check( false, "T12a: unexpected tool record name '" + name + "'" );
			}
		}
		Check( toolRecordsSeen == 2, "T12a: exactly two tool records were inspected" );
	}

	// (b) RED-PROVE: a tool-call budget of 1 stops the run right AFTER
	// ask_user -- the FIRST call dispatched in the turn -- so
	// read_document, the SECOND call in the SAME turn, never dispatches.
	// Proves the budget genuinely counts an INTERCEPTED call the same as
	// a dispatched one (never "free" just because it never reaches
	// HandleLine).
	{
		AgentEvalScenario scenario;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ),
		       "T12b: param_edit reloads as the scene/session donor (" + err + ")" );
		scenario.id = "ask_user_parallel_mixed_budget1";
		scenario.prompts.clear();
		scenario.prompts.push_back( AgentEvalPrompt( "Check the mood with me, then read the document" ) );
		scenario.budgets.maxToolCalls = 1;
		scenario.replayFixturePath.clear();

		const std::string dir = ScratchRunDir( "t12b_ask_user_parallel_budget1" );
		const std::string fixturePath = buildFixture( dir + "/fixture.jsonl" );

		AgentEvalReplaySource src;
		Check( AgentEvalReplaySource::LoadFromFile( fixturePath, src, err ),
		       "T12b: fixture loads (" + err + ")" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		opts.replaySourceOverride = &src;

		AgentEvalRunHandle h = RunScenario( scenario, opts );

		Check( h.result.terminalStatus == "budget_tool_calls",
		       "T12b: maxToolCalls=1 trips honestly right after ask_user, the first call in the turn "
		       "(got '" + h.result.terminalStatus + "')" );
		Check( h.result.budgetHit, "T12b: budgetHit is true" );
		Check( h.result.toolCalls == 1,
		       "T12b: exactly 1 tool call dispatched -- the budget really counted the intercepted ask_user call" );

		std::vector<JsonValue> recs = ReadJsonl( h.trajectoryPath );
		Check( ToolNameSequence( recs ) == std::vector<std::string>{ "ask_user" },
		       "T12b: the trajectory's ONE tool record is ask_user -- read_document (2nd in the turn) never ran" );
	}
}

int main()
{
	std::printf( "=== AgentEvalReplayTest (Eval-harness slice E2: replay backend + scenario runner) ===\n" );

	TestScratchRootDiffersAcrossProcesses();
	TestLoadSeedScenarios();
	TestLoadScenarioGates();
	TestReplaySourceLoad();
	TestParamEditScenario();
	TestTwoToolObserveScenario();
	TestErrorPathScenario();
	TestBudgetEnforcementRedProve();
	TestRecordOnceReplayForeverRedProve();
	TestScenarioFileFnvStamp();
	TestResultJsonlDegenerateTurnRetriesField();
	TestIterationCapBudgetRaise();
	TestAskUserMidScenario();
	TestAskUserParallelWithDispatchedTool();

	std::printf( "=== AgentEvalReplayTest: %d passed, %d failed ===\n", g_pass, g_fail );

	// Leave TMPDIR clean: remove this process's entire scratch tree.
	{
		std::error_code ec;
		std::filesystem::remove_all( ScratchTestRoot(), ec );
	}

	return g_fail == 0 ? 0 : 1;
}
