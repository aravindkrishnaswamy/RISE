//////////////////////////////////////////////////////////////////////
//
//  AgentEvalCheckTest.cpp - Eval-harness slice E3: the CHECKER ENGINE
//    (see src/Library/Agent/AgentEvalRunner.h §4, CheckScenario).
//
//  Drives CheckScenario against real AgentEvalRunHandle's produced by
//  RunScenario (E2) -- real renders, real document reads, zero network,
//  zero keys.  For each checkpoint kind, proves BOTH directions: a
//  crafted-to-pass checkpoint actually passes, and a crafted-to-fail
//  sibling actually fails (with a non-empty, actionable `detail`) -- a
//  checker that only ever passes is worthless.
//
//  Coverage:
//    T1  "document" op:param_equals -- pass (value matches) / fail (value
//        mismatch) / fail (unknown target) / fail (unknown param).
//    T2  "document" op:chunk_exists / op:chunk_absent -- both pass/fail;
//        op:chunk_count (min/max band over a chunkKind, for asserting on an
//        UNNAMED non-singleton kind that param_equals/chunk_exists cannot
//        address individually) -- exact-band pass, below-min/above-max fail,
//        zero-match band both directions, missing chunkKind/min+max fail loudly.
//    T3  "untouched" -- passes on an unperturbed chunk (pnt_emit, never
//        touched by param_edit's edit), FAILS on the chunk the scenario
//        actually edited (pnt_albedo) -- the guard catches a real
//        perturbation, not just a hypothetical one.
//    T4  "render" -- passes on a generous band; fails on an impossible one
//        (meanLumaMax:0.0 -- the scene is never pure black); fails on a
//        width-without-height shape error.
//    T5  "objectmap" -- legendContains pass/fail; queryAt pass/fail
//        (expectName mismatch); a checkpoint naming none of the three
//        assertions is a shape-error fail.
//    T6  "diagnostics" -- expect:"clean" passes on an untouched, valid
//        document; expect:"code" fails when the named code is absent;
//        unknown-expect and missing-code fail loudly (T6a).  A genuinely
//        validate-dirty LIVE document is UNREACHABLE by construction (both
//        the load path and even the UNCHECKED GUI edit path refuse a
//        non-derivable head), so the "clean"-FAILS / "code"-PASSES branches
//        cannot be driven through a real session -- T6b proves that
//        invariant (the ungated edit is rejected, the document stays clean)
//        and drives the checker's "clean" branch to a PASS through a
//        hand-built WrapJob handle.  See TestDiagnosticsLiveDocInvariant.
//    T7  "trajectory" -- maxToolCalls/maxLlmCalls/terminalStatus pass/fail;
//        noAutonomyRefusal fails when a Read-autonomy scenario's mutating
//        tool call is actually refused (-32011); requiredToolInOrder
//        subsequence pass/fail; noMechanicalLoop fails on two identical
//        consecutive tool calls, passes on two DIFFERENT ones.
//    T8  Unknown checkpoint kind -> a FAILED checkpoint with a detail,
//        never a crash.  A non-object / kind-less checkpoint -> same.
//    T9  Partial-credit arithmetic: a scenario with a 2:1-weighted mix of
//        pass/fail checkpoints yields the exact expected fraction; a
//        scenario with zero checkpoints is a vacuous pass (fraction 1.0).
//    T10 Every committed evals/scenarios/*.json (the 4 seed scenarios plus
//        the "editing verbs & recovery" set: remove_object,
//        material_add_and_bind, conflict_retry, reserved_name_recovery),
//        run end-to-end through its OWN evals/fixtures/*.fixture.jsonl and
//        checked against its OWN wired checkpoints[] -- every checkpoint
//        is TRUE of the fixture that produces it (allPassed,
//        checkpointFraction 1.0).
//
//  RED-PROVE (manual, not automated in this file -- see the final report):
//    CheckDocumentKind's param_equals comparison was temporarily replaced
//    with an always-true stub; T1's fail-direction assertion (value
//    mismatch must FAIL) itself failed, as expected; the stub was reverted
//    and this file re-run clean.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentEvalRunner.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Job.h"

#include <cmath>
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
// Shared fixture text: the SAME lit-sphere + area-emitter body the other
// agent tests use (byte-for-byte identical to evals/scenarios/*.json's
// scene.inline) -- renders non-black at 24x24 / 8spp, has exactly two
// named objects (obj_sph, obj_emit) for the objectmap tests.
//----------------------------------------------------------------------
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// param_edit's OWN two-round canned fixture (propose_patch recolors
// pnt_albedo to "0.9 0.1 0.1", then a plain end_turn) -- matches
// evals/fixtures/param_edit.fixture.jsonl byte-for-byte so this file has no
// dependency on that committed path (a scratch copy is written per-test).
static const char* const kParamEditFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Recoloring the sphere.\\\"},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_01A\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":128,\\\"output_tokens\\\":64}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Done: pnt_albedo is now red.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":50,\\\"output_tokens\\\":10}}\"}\n";

// A single-round fixture that calls render then read_image (identical
// tool_use names/args, one message) -- for the noMechanicalLoop RED case.
static const char* const kMechanicalLoopFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_r1\\\",\\\"name\\\":\\\"render\\\",\\\"input\\\":{}},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_r2\\\",\\\"name\\\":\\\"render\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A single-round fixture that calls render then read_image (DIFFERENT
// tool names) -- the noMechanicalLoop GREEN control.
static const char* const kNoLoopFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_render\\\",\\\"name\\\":\\\"render\\\",\\\"input\\\":{}},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_img\\\",\\\"name\\\":\\\"read_image\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A single-round fixture that attempts propose_patch (a mutating verb) --
// paired with autonomy:"read" this gets refused (-32011) at the dispatcher.
static const char* const kMutatingAttemptFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_p1\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.1 0.1 0.9\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"could not edit\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A degenerate blank end_turn -- the loop's own documented ProviderError
// (matches evals/fixtures/error_path.fixture.jsonl).
static const char* const kErrorFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":10,\\\"output_tokens\\\":0}}\"}\n";

//----------------------------------------------------------------------
// Small helpers: a fresh scratch runDir, a raw-fixture writer, checkpoint
// JSON construction (built directly as JsonValue -- no scenario JSON file
// needed on disk beyond the scratch AgentEvalScenario the test builds in
// memory).
//----------------------------------------------------------------------
static std::string ScratchRunDir( const char* leaf )
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	dir += "rise_agent_eval_check_test/";
	dir += leaf;
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );
	return dir;
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

// Build a minimal, valid AgentEvalScenario in memory: `sceneText` inline,
// one prompt, autonomy `autonomy`, replay fixture written to `runDir`/fixture
// (auto-detected raw-fixture shape).  `checkpointsJson` (a JSON array TEXT,
// e.g. "[{\"kind\":\"document\",...}]") is parsed into scenario.checkpoints.
static AgentEvalScenario MakeScenario( const std::string& id, const std::string& sceneText,
                                       const std::string& prompt, const std::string& autonomy,
                                       const std::string& fixtureText, const std::string& runDir,
                                       const std::string& checkpointsJson )
{
	AgentEvalScenario s;
	s.id = id;
	s.title = id;
	s.sceneInline = sceneText;
	s.autonomy = autonomy;
	s.prompts.push_back( prompt );
	s.replayFixturePath = runDir + "/" + id + ".fixture.jsonl";
	WriteFile( s.replayFixturePath, fixtureText );
	if( !checkpointsJson.empty() ) {
		JsonValue cps; std::string err;
		if( JsonParse( checkpointsJson, cps, err ) && cps.isArray() ) s.checkpoints = cps;
	}
	return s;
}

//----------------------------------------------------------------------
// T1/T2: "document" checkpoint kind.
//----------------------------------------------------------------------
static void TestDocumentCheckpoint()
{
	std::printf( "T1/T2: \"document\" checkpoint kind (param_equals / chunk_exists / chunk_absent)...\n" );
	const std::string dir = ScratchRunDir( "t1_document" );

	AgentEvalScenario s = MakeScenario( "doc_edit", kScene, "Recolor", "commit", kParamEditFixture, dir,
		"[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "doc_edit run reached final_text" );
	Check( h.dispatcher != nullptr, "doc_edit run has a live dispatcher" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s;
		s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		Check( r.checkpoints.size() == 1, label + ": exactly one checkpoint result" );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
			Check( !r.checkpoints[0].detail.empty(), label + ": detail is never empty" );
		}
	};

	// param_equals: PASS (the edit really landed) / FAIL (wrong expected value).
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1 0.1\"}]",
		true, "param_equals correct value" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.1 0.1 0.9\"}]",
		false, "param_equals WRONG value" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"does_not_exist\",\"param\":\"color\",\"value\":\"1 1 1\"}]",
		false, "param_equals unknown target" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"no_such_param\",\"value\":\"1 1 1\"}]",
		false, "param_equals unknown param" );

	// chunk_exists / chunk_absent: both directions.
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"pnt_albedo\"}]", true, "chunk_exists present" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"nope_xyz\"}]", false, "chunk_exists absent" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_absent\",\"name\":\"nope_xyz\"}]", true, "chunk_absent correctly absent" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_absent\",\"name\":\"pnt_albedo\"}]", false, "chunk_absent unexpectedly present" );

	// chunkKind-narrowed lookup still resolves.  (The narrowing field is
	// "chunkKind", NOT "kind": "kind" is the top-level checkpoint discriminator,
	// and the last-wins JSON codec would otherwise read the narrowing value AS
	// the discriminator -> "unknown checkpoint kind".)
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"chunkKind\":\"uniformcolor_painter\",\"name\":\"pnt_albedo\"}]",
		true, "chunk_exists narrowed by chunkKind" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"chunkKind\":\"sphere_geometry\",\"name\":\"pnt_albedo\"}]",
		false, "chunk_exists wrong chunkKind narrowing correctly fails" );

	// chunk_count: kScene carries exactly 2 uniformcolor_painter chunks
	// (pnt_albedo, pnt_emit) and 2 standard_object chunks (obj_sph, obj_emit)
	// -- both PASS on an exact [2,2] band, FAIL when the band excludes 2.
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"uniformcolor_painter\",\"min\":2,\"max\":2}]",
		true, "chunk_count exact band [2,2] matches" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"standard_object\",\"min\":2,\"max\":2}]",
		true, "chunk_count exact band [2,2] matches (standard_object)" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"uniformcolor_painter\",\"min\":3}]",
		false, "chunk_count below min FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"uniformcolor_painter\",\"max\":1}]",
		false, "chunk_count above max FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"does_not_exist_kind\",\"min\":1}]",
		false, "chunk_count of a kind with zero matches FAILS a min:1 band" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"does_not_exist_kind\",\"max\":0}]",
		true, "chunk_count of a kind with zero matches PASSES a max:0 band" );

	// Malformed op / missing fields -- failed, not crashed.
	checkOne( "[{\"kind\":\"document\",\"op\":\"not_a_real_op\"}]", false, "unknown op fails loudly" );
	checkOne( "[{\"kind\":\"document\"}]", false, "missing \"op\" fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"min\":1}]", false, "chunk_count missing \"chunkKind\" fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"chunk_count\",\"chunkKind\":\"standard_object\"}]",
		false, "chunk_count missing both \"min\"/\"max\" fails loudly" );

	// param_references_kind (fixture-overfit fix): obj_sph.material names
	// mat_diffuse, which IS a lambertian_material -- so the op passes
	// name-agnostically for referencedKind lambertian_material, and fails
	// when the referenced chunk is of a DIFFERENT kind (no
	// pbr_metallic_roughness_material / sphere_geometry named mat_diffuse).
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKind\":\"lambertian_material\"}]",
		true, "param_references_kind: material resolves to a lambertian_material" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKind\":\"pbr_metallic_roughness_material\"}]",
		false, "param_references_kind: referencedKind mismatch (no PBR material of that name) FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKind\":\"sphere_geometry\"}]",
		false, "param_references_kind: material name is not a sphere_geometry FAILS" );
	// The TARGET chunk itself must exist; the param must be present.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"does_not_exist\",\"param\":\"material\",\"referencedKind\":\"lambertian_material\"}]",
		false, "param_references_kind: unknown target FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"no_such_param\",\"referencedKind\":\"lambertian_material\"}]",
		false, "param_references_kind: unknown param on target FAILS" );
	// Shape refusals: each required string missing.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"param\":\"material\",\"referencedKind\":\"lambertian_material\"}]",
		false, "param_references_kind missing \"target\" fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"referencedKind\":\"lambertian_material\"}]",
		false, "param_references_kind missing \"param\" fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\"}]",
		false, "param_references_kind missing \"referencedKind\" fails loudly" );

	// param_equals numeric tolerance (fixture-overfit fix): pnt_albedo.color
	// is "0.9 0.1 0.1" after the recolor.  numeric:true grades a
	// differently-formatted-but-equal value equal; a WRONG value, a
	// component-count mismatch, and a non-numeric value each fail (the last
	// LOUDLY, distinguishing "not numeric" from a plain mismatch).
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.90 0.10 0.10\",\"numeric\":true}]",
		true, "param_equals numeric: 0.90 0.10 0.10 == 0.9 0.1 0.1 within tol" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1 0.1\",\"numeric\":true}]",
		true, "param_equals numeric: exact-string value still passes" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.1 0.1 0.9\",\"numeric\":true}]",
		false, "param_equals numeric: a genuinely different colour FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1\",\"numeric\":true}]",
		false, "param_equals numeric: component-count mismatch FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"red green blue\",\"numeric\":true}]",
		false, "param_equals numeric: a non-numeric \"value\" refuses loudly" );
	// Without numeric, the differently-formatted value is a raw-string
	// mismatch -- proving the flag is what changes the behaviour.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.90 0.10 0.10\"}]",
		false, "param_equals (no numeric): 0.90 0.10 0.10 is a raw-string mismatch" );
}

//----------------------------------------------------------------------
// T3: "untouched" checkpoint kind.
//----------------------------------------------------------------------
static void TestUntouchedCheckpoint()
{
	std::printf( "T3: \"untouched\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t3_untouched" );

	AgentEvalScenario s = MakeScenario( "untouched_probe", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "untouched_probe run reached final_text" );
	Check( !h.initialDocument.empty(), "the handle captured a non-empty initialDocument (E2 additive field)" );
	Check( h.initialDocument.find( "0.5 0.5 0.5" ) != std::string::npos,
		"initialDocument carries the PRE-edit albedo colour" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	// pnt_emit was NEVER touched by the edit -> untouched PASSES.
	checkOne( "[{\"kind\":\"untouched\",\"chunks\":[{\"name\":\"pnt_emit\"}]}]", true,
		"untouched guard passes on a genuinely unperturbed chunk" );
	// pnt_albedo WAS the edit target -> untouched correctly FAILS (catches
	// a REAL perturbation, not a hypothetical one).
	checkOne( "[{\"kind\":\"untouched\",\"chunks\":[{\"name\":\"pnt_albedo\"}]}]", false,
		"untouched guard FAILS on the chunk the scenario actually edited" );
	// A mix: one untouched + one perturbed in the SAME checkpoint -> the
	// whole checkpoint fails (any violation fails the whole guard).
	checkOne( "[{\"kind\":\"untouched\",\"chunks\":[{\"name\":\"pnt_emit\"},{\"name\":\"pnt_albedo\"}]}]", false,
		"untouched guard fails overall when ANY listed chunk changed" );
	// Malformed shape.
	checkOne( "[{\"kind\":\"untouched\"}]", false, "untouched missing \"chunks\" fails loudly" );
	checkOne( "[{\"kind\":\"untouched\",\"chunks\":[]}]", false, "untouched with an EMPTY chunks array fails loudly" );
}

//----------------------------------------------------------------------
// T4: "render" checkpoint kind.
//----------------------------------------------------------------------
static void TestRenderCheckpoint()
{
	std::printf( "T4: \"render\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t4_render" );

	AgentEvalScenario s = MakeScenario( "render_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "render_probe run reached final_text" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	// A GENEROUS band absorbing ordinary MC noise -- never an exact-pixel
	// match (the scene is lit; luma sits well inside [0,5] regardless of
	// per-run noise).
	checkOne( "[{\"kind\":\"render\",\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]", true,
		"a generous render band passes" );
	// An IMPOSSIBLE band: this scene is never pure black.
	checkOne( "[{\"kind\":\"render\",\"meanLumaMax\":0.0}]", false,
		"an impossible (zero-luma-ceiling) render band fails" );
	// width without height -- a shape error, not a crash.
	checkOne( "[{\"kind\":\"render\",\"width\":32}]", false,
		"width without height fails loudly (shape error)" );
	// A per-channel band, generously wide -- also passes.
	checkOne( "[{\"kind\":\"render\",\"meanRMin\":0.0,\"meanRMax\":10.0,\"meanGMin\":0.0,\"meanGMax\":10.0,\"meanBMin\":0.0,\"meanBMax\":10.0}]",
		true, "generous per-channel bands pass" );
}

//----------------------------------------------------------------------
// T5: "objectmap" checkpoint kind.
//----------------------------------------------------------------------
static void TestObjectmapCheckpoint()
{
	std::printf( "T5: \"objectmap\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t5_objectmap" );

	AgentEvalScenario s = MakeScenario( "objectmap_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "objectmap_probe run reached final_text" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	checkOne( "[{\"kind\":\"objectmap\",\"legendContains\":\"obj_sph\"}]", true,
		"legendContains a real registered object passes" );
	checkOne( "[{\"kind\":\"objectmap\",\"legendContains\":\"not_a_real_object\"}]", false,
		"legendContains a nonexistent name fails" );

	// The sphere sits centred in frame at 24x24 -- the centre pixel hits it.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"obj_sph\"}}]", true,
		"queryAt the centre pixel correctly names obj_sph" );
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"definitely_not_it\"}}]", false,
		"queryAt with a WRONG expectName fails" );

	// A checkpoint naming none of the three assertions -- a shape error.
	checkOne( "[{\"kind\":\"objectmap\"}]", false, "objectmap with no assertions fails loudly" );
	// queryAt missing expectName -- a shape error, not a crash.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12}}]", false,
		"queryAt missing \"expectName\" fails loudly" );
}

//----------------------------------------------------------------------
// T6: "diagnostics" checkpoint kind.
//----------------------------------------------------------------------
static void TestDiagnosticsCheckpointClean()
{
	std::printf( "T6a: \"diagnostics\" checkpoint kind (clean document)...\n" );
	const std::string dir = ScratchRunDir( "t6a_diagnostics" );

	// error_path's OWN document is NEVER edited (the fixture's degenerate
	// end_turn refuses before any tool call) -- exactly the same scene
	// text every seed scenario uses, always derive-clean.
	AgentEvalScenario s = MakeScenario( "diag_probe", kScene, "Do something", "commit", kErrorFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "provider_error", "diag_probe run reached provider_error (the loop's own refusal)" );
	Check( h.dispatcher != nullptr, "diag_probe still has a live dispatcher (a ProviderError is not a load_error)" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"clean\"}]", true,
		"expect:\"clean\" passes on an untouched, valid document" );
	checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"code\",\"code\":\"UNKNOWN_PARAMETER\"}]", false,
		"expect:\"code\" fails when that code is absent (the search-not-found path)" );
	checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"banana\"}]", false,
		"an unrecognized \"expect\" value fails loudly" );
	checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"code\"}]", false,
		"expect:\"code\" without a \"code\" field fails loudly" );
}

// The load-bearing invariant that makes a validate-dirty LIVE document
// UNREACHABLE, and the diagnostics checker's "clean" branch exercised
// end-to-end through a hand-built WrapJob handle.
//
// The original intent of this test was to manufacture a genuinely
// validate-dirty live document and drive the checker's "clean"-FAILS /
// "code"-PASSES branches with it.  Empirically that document is
// UNREACHABLE by construction:
//   * Job::LoadAsciiSceneViaCst REFUSES to load (returns false) on ANY
//     derive diagnostic -- a loaded session's document is always
//     derive-clean.
//   * Even the "unchecked" GUI edit path (Job::ApplyCstParamEdit,
//     requireFullDerivability=false -- the property-panel / gizmo route)
//     still refuses to commit a non-derivable value: the incremental
//     fast path declines a non-numeric Double param, the D2 fallback
//     (Job::RederiveCstDocumentFull_) dry-runs the FULL derive into a
//     throwaway Job FIRST, sees "sphere_geometry: invalid parameter(s)",
//     and returns 0 with the live scene byte-identical.  (That
//     "...would not derive..." log line is that CORRECT rejection, not a
//     failure.)
// So there is no Job edit path -- checked OR unchecked -- that can leave a
// live session holding validate-dirty text.  A live agent session's
// document is ALWAYS validate-clean; the "clean"-FAILS / "code"-PASSES
// branches are unreachable in production and cannot be driven through a
// real session.  Their crafted-to-FAIL siblings (code-absent,
// unknown-expect, missing-code) are already covered in T6a.
//
// This test now PROVES that invariant (the ungated edit is rejected, the
// document stays clean) and, via a hand-built WrapJob handle -- exactly
// like AgentChunkCrudTest.cpp's low-level probes -- drives the checker's
// diagnostics "clean" branch to a real PASS on the guaranteed-clean live
// document (complementing T6a's RunScenario-driven passes).
static void TestDiagnosticsLiveDocInvariant()
{
	std::printf( "T6b: \"diagnostics\" checkpoint kind (the live-doc-always-clean invariant + a WrapJob \"clean\" pass)...\n" );
	const std::string dir = ScratchRunDir( "t6b_diagnostics_invariant" );
	const std::string scenePath = dir + "/clean_probe.RISEscene";
	Check( WriteFile( scenePath, kScene ), "wrote the scratch scene file" );

	// WrapJob is NON-OWNING (see its doc: "the caller keeps ownership and
	// must outlive the session") -- pJob is released exactly once, at the
	// bottom of this function, regardless of which branch runs.
	Job* pJob = new Job();
	const bool loaded = pJob->LoadAsciiSceneViaCst( scenePath.c_str() );
	Check( loaded, "the clean scene loads via LoadAsciiSceneViaCst" );

	if( loaded ) {
		// The UNCHECKED GUI edit path (requireFullDerivability=false) attempts
		// to write a non-numeric string into sph.radius (a Double-kind param).
		// It is REJECTED anyway -- the D2 validate-before-destroy dry-run
		// refuses a non-derivable head -- so rc==0 and the live document is
		// UNTOUCHED.  (The accompanying "...sph.radius would not derive..." log
		// line is that correct rejection.)
		const int rc = pJob->ApplyCstParamEdit( "sph", nullptr, "radius", 0, "not_a_number" );
		Check( rc == 0, "even the UNCHECKED GUI edit path rejects a non-derivable value (rc=" +
			std::to_string( rc ) + ", 0 == rejected, head untouched)" );

		std::unique_ptr<AgentSession> session = AgentSession::WrapJob( pJob );
		Check( session != nullptr, "WrapJob produced a live session over the (still-clean) Job" );

		if( session ) {
			// The invariant: the rejected edit left the document validate-clean.
			const std::vector<AgentDiagnostic> diags = AgentSession::ValidateText( session->ReadDocument() );
			Check( diags.empty(), "the live document is STILL validate-clean after the rejected edit (0 diagnostics) -- "
				"a validate-dirty live document is unreachable by construction" );

			// Drive the checker's diagnostics "clean" branch to a real PASS
			// through a hand-built WrapJob handle (no RunScenario, no
			// trajectory) -- the load_error-safe seam CheckScenario documents.
			AgentEvalRunHandle h;
			h.result.scenarioId = "clean_probe";
			h.result.terminalStatus = "final_text";
			h.trajectoryPath = "";   // no trajectory was ever recorded for this hand-built handle
			h.resultPath = dir + "/clean_probe.result.jsonl";   // so CheckScenario's results.jsonl write has somewhere to land
			h.dispatcher.reset( new AgentRpcDispatcher( std::move( session ) ) );

			AgentEvalScenario s2;
			s2.id = "clean_probe";

			auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
				JsonValue cps; std::string err;
				Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
				s2.checkpoints = cps;
				AgentEvalCheckResult r = CheckScenario( h, s2 );
				if( r.checkpoints.size() == 1 ) {
					Check( r.checkpoints[0].passed == expectPass,
						label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
						" (detail: " + r.checkpoints[0].detail + ")" );
				} else Check( false, label + ": expected exactly one checkpoint result" );
			};

			// PASS: the checker agrees the (guaranteed-clean) live document is clean.
			checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"clean\"}]", true,
				"expect:\"clean\" PASSES on the guaranteed-clean live document (WrapJob handle)" );
			// FAIL: a code that cannot be present on a clean document.
			checkOne( "[{\"kind\":\"diagnostics\",\"expect\":\"code\",\"code\":\"UNKNOWN_PARAMETER\"}]", false,
				"expect:\"code\" FAILS on the clean document (the code is absent)" );

			// h.dispatcher (destroyed at scope exit above) owned `session`,
			// which never owned pJob (WrapJob is non-owning) -- pJob is
			// still ours to release below either way.
		}
	}

	pJob->release();
}

//----------------------------------------------------------------------
// T7: "trajectory" checkpoint kind.
//----------------------------------------------------------------------
static void TestTrajectoryCheckpoint()
{
	std::printf( "T7: \"trajectory\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t7_trajectory" );

	// maxToolCalls / maxLlmCalls / terminalStatus, both directions, off the
	// param_edit-style run (1 tool call, 2 llm rounds, final_text).
	{
		AgentEvalScenario s = MakeScenario( "traj_counts", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 1 && h.result.llmCalls == 2, "traj_counts run drove 1 tool call / 2 llm rounds" );

		auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
			JsonValue cps; std::string err;
			Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
			AgentEvalScenario s2 = s; s2.checkpoints = cps;
			AgentEvalCheckResult r = CheckScenario( h, s2 );
			if( r.checkpoints.size() == 1 ) {
				Check( r.checkpoints[0].passed == expectPass,
					label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
					" (detail: " + r.checkpoints[0].detail + ")" );
			} else Check( false, label + ": expected exactly one checkpoint result" );
		};

		checkOne( "[{\"kind\":\"trajectory\",\"maxToolCalls\":1}]", true, "maxToolCalls==actual passes" );
		checkOne( "[{\"kind\":\"trajectory\",\"maxToolCalls\":0}]", false, "maxToolCalls below actual fails" );
		checkOne( "[{\"kind\":\"trajectory\",\"maxLlmCalls\":2}]", true, "maxLlmCalls==actual passes" );
		checkOne( "[{\"kind\":\"trajectory\",\"maxLlmCalls\":1}]", false, "maxLlmCalls below actual fails" );
		checkOne( "[{\"kind\":\"trajectory\",\"terminalStatus\":\"final_text\"}]", true, "terminalStatus match passes" );
		checkOne( "[{\"kind\":\"trajectory\",\"terminalStatus\":\"provider_error\"}]", false, "terminalStatus mismatch fails" );
	}

	// requiredToolInOrder + noMechanicalLoop, off the two-tool-call run.
	{
		AgentEvalScenario s = MakeScenario( "traj_order", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 2, "traj_order run drove 2 tool calls (render, read_image)" );

		auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
			JsonValue cps; std::string err;
			Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
			AgentEvalScenario s2 = s; s2.checkpoints = cps;
			AgentEvalCheckResult r = CheckScenario( h, s2 );
			if( r.checkpoints.size() == 1 ) {
				Check( r.checkpoints[0].passed == expectPass,
					label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
					" (detail: " + r.checkpoints[0].detail + ")" );
			} else Check( false, label + ": expected exactly one checkpoint result" );
		};

		checkOne( "[{\"kind\":\"trajectory\",\"requiredToolInOrder\":[\"render\",\"read_image\"]}]", true,
			"requiredToolInOrder as a satisfied subsequence passes" );
		checkOne( "[{\"kind\":\"trajectory\",\"requiredToolInOrder\":[\"read_image\",\"render\"]}]", false,
			"requiredToolInOrder in the WRONG order fails" );
		checkOne( "[{\"kind\":\"trajectory\",\"requiredToolInOrder\":[\"render\",\"query_object_at\"]}]", false,
			"requiredToolInOrder naming a tool never called fails" );
		checkOne( "[{\"kind\":\"trajectory\",\"noMechanicalLoop\":true}]", true,
			"noMechanicalLoop passes when consecutive tool calls DIFFER (render, read_image)" );
	}

	// noMechanicalLoop RED case: two IDENTICAL consecutive tool calls.
	{
		AgentEvalScenario s = MakeScenario( "traj_loop", kScene, "Render it", "commit", kMechanicalLoopFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 2, "traj_loop run drove 2 tool calls (render, render)" );

		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"trajectory\",\"noMechanicalLoop\":true}]", cps, err ), "noMechanicalLoop checkpoint parses" );
		s.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s );
		if( r.checkpoints.size() == 1 ) {
			Check( !r.checkpoints[0].passed,
				"noMechanicalLoop correctly FAILS on two identical consecutive tool calls (detail: " +
				r.checkpoints[0].detail + ")" );
		} else Check( false, "expected exactly one checkpoint result for traj_loop" );
	}

	// noAutonomyRefusal RED case: autonomy:"read" + a mutating tool attempt.
	{
		AgentEvalScenario s = MakeScenario( "traj_refusal", kScene, "Try to edit", "read", kMutatingAttemptFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "traj_refusal run still reaches final_text (the refusal is a tool RESULT, not a loop error)" );
		Check( h.result.toolCalls == 1, "traj_refusal dispatched exactly 1 (refused) tool call" );

		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"trajectory\",\"noAutonomyRefusal\":true}]", cps, err ), "noAutonomyRefusal checkpoint parses" );
		s.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s );
		if( r.checkpoints.size() == 1 ) {
			Check( !r.checkpoints[0].passed,
				"noAutonomyRefusal correctly FAILS when a -32011 refusal occurred (detail: " +
				r.checkpoints[0].detail + ")" );
		} else Check( false, "expected exactly one checkpoint result for traj_refusal" );

		// GREEN control: the SAME assertion on a run with no refusal at all.
		AgentEvalScenario sGreen = MakeScenario( "traj_no_refusal", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalRunOptions optsGreen; optsGreen.runDir = dir;
		AgentEvalRunHandle hGreen = RunScenario( sGreen, optsGreen );
		JsonValue cpsGreen; std::string errGreen;
		Check( JsonParse( "[{\"kind\":\"trajectory\",\"noAutonomyRefusal\":true}]", cpsGreen, errGreen ), "green noAutonomyRefusal checkpoint parses" );
		sGreen.checkpoints = cpsGreen;
		AgentEvalCheckResult rGreen = CheckScenario( hGreen, sGreen );
		if( rGreen.checkpoints.size() == 1 ) {
			Check( rGreen.checkpoints[0].passed, "noAutonomyRefusal PASSES when no refusal occurred (detail: " +
				rGreen.checkpoints[0].detail + ")" );
		} else Check( false, "expected exactly one checkpoint result for traj_no_refusal" );
	}
}

//----------------------------------------------------------------------
// T8: unknown checkpoint kind + malformed checkpoint shape -> FAILED, not
// a crash.
//----------------------------------------------------------------------
static void TestUnknownKindAndMalformedShape()
{
	std::printf( "T8: unknown checkpoint kind / malformed checkpoint shape -> failed, never a crash...\n" );
	const std::string dir = ScratchRunDir( "t8_unknown" );

	AgentEvalScenario s = MakeScenario( "unknown_kind_probe", kScene, "Do something", "commit", kErrorFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );

	auto checkOne = [&]( const std::string& cpJson, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		Check( r.checkpoints.size() == 1, label + ": exactly one checkpoint result (no crash)" );
		if( r.checkpoints.size() == 1 ) {
			Check( !r.checkpoints[0].passed, label + ": FAILED (loud), not silently skipped" );
			Check( !r.checkpoints[0].detail.empty(), label + ": carries a non-empty detail" );
		}
		Check( !r.allPassed, label + ": scenario-level allPassed is false" );
	};

	checkOne( "[{\"kind\":\"totally_made_up_kind\"}]", "unknown kind" );
	checkOne( "[{\"notAKindField\":\"whatever\"}]", "checkpoint object with no \"kind\" at all" );
	checkOne( "[\"not even an object\"]", "checkpoint that is not a JSON object" );

	// A run whose dispatcher is null (load_error) must ALSO never crash --
	// document/untouched/render/objectmap/diagnostics all guard on a null
	// session; trajectory guards on an empty trajectoryPath for its
	// file-backed sub-asserts.
	AgentEvalScenario bad;
	bad.id = "load_error_probe";
	bad.title = "t";
	bad.sceneInline = "not a valid scene at all";
	bad.prompts.push_back( "hi" );
	bad.replayFixturePath = dir + "/load_error_probe.fixture.jsonl";
	WriteFile( bad.replayFixturePath, kErrorFixture );
	AgentEvalRunOptions opts2; opts2.runDir = dir;
	AgentEvalRunHandle hBad = RunScenario( bad, opts2 );
	Check( hBad.result.terminalStatus == "load_error", "the malformed-scene run is a load_error" );
	Check( hBad.dispatcher == nullptr, "a load_error handle has a null dispatcher" );

	JsonValue cps; std::string err;
	JsonParse( "[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"x\"},"
	           "{\"kind\":\"render\",\"meanLumaMin\":0.0},"
	           "{\"kind\":\"trajectory\",\"noMechanicalLoop\":true}]", cps, err );
	bad.checkpoints = cps;
	AgentEvalCheckResult rBad = CheckScenario( hBad, bad );
	Check( rBad.checkpoints.size() == 3, "a load_error handle still yields one result per checkpoint (no crash)" );
	for( const auto& cp : rBad.checkpoints ) {
		Check( !cp.passed, "every checkpoint against a load_error handle fails cleanly" );
		Check( !cp.detail.empty(), "...with a non-empty detail" );
	}
}

//----------------------------------------------------------------------
// T9: partial-credit arithmetic.
//----------------------------------------------------------------------
static void TestPartialCreditArithmetic()
{
	std::printf( "T9: partial-credit arithmetic...\n" );
	const std::string dir = ScratchRunDir( "t9_partial" );

	AgentEvalScenario s = MakeScenario( "partial_probe", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "partial_probe run reached final_text" );

	// weight 2 PASS + weight 1 FAIL + weight 1 PASS (default weight) -> (2+1)/(2+1+1) = 0.75.
	JsonValue cps; std::string err;
	Check( JsonParse(
		"[{\"kind\":\"document\",\"weight\":2,\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"0.9 0.1 0.1\"},"
		" {\"kind\":\"document\",\"weight\":1,\"op\":\"chunk_exists\",\"name\":\"nope\"},"
		" {\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"pnt_albedo\"}]",
		cps, err ), "the mixed-weight checkpoints array parses" );
	s.checkpoints = cps;
	AgentEvalCheckResult r = CheckScenario( h, s );
	Check( r.checkpoints.size() == 3, "three checkpoint results" );
	Check( !r.allPassed, "allPassed is false (the middle checkpoint failed)" );
	if( r.checkpoints.size() == 3 ) {
		Check( r.checkpoints[0].passed && r.checkpoints[0].weight == 2.0, "checkpoint[0] passed with weight 2" );
		Check( !r.checkpoints[1].passed && r.checkpoints[1].weight == 1.0, "checkpoint[1] failed with weight 1" );
		Check( r.checkpoints[2].passed && r.checkpoints[2].weight == 1.0, "checkpoint[2] passed with default weight 1" );
	}
	Check( std::abs( r.checkpointFraction - 0.75 ) < 1e-9,
		"checkpointFraction == 0.75 (got " + std::to_string( r.checkpointFraction ) + ")" );

	// Zero checkpoints -> a vacuous pass (fraction 1.0, allPassed true).
	AgentEvalScenario sEmpty = s;
	sEmpty.checkpoints = JsonValue::MakeArray();
	AgentEvalCheckResult rEmpty = CheckScenario( h, sEmpty );
	Check( rEmpty.checkpoints.empty(), "zero checkpoints -> zero results" );
	Check( rEmpty.allPassed, "zero checkpoints -> allPassed vacuously true" );
	Check( rEmpty.checkpointFraction == 1.0, "zero checkpoints -> checkpointFraction is 1.0" );

	// Degenerate authoring: every checkpoint has weight 0 so weightSum == 0.
	// The weighted fraction is undefined, so we fall back to the UNWEIGHTED
	// pass fraction -- a failing checkpoint must still drag it below 1.0
	// rather than report a spurious perfect score.  Here: 1 pass + 1 fail
	// over 2 checkpoints -> 0.5, and allPassed is false.
	JsonValue zcps; std::string zerr;
	Check( JsonParse(
		"[{\"kind\":\"document\",\"weight\":0,\"op\":\"chunk_exists\",\"name\":\"pnt_albedo\"},"
		" {\"kind\":\"document\",\"weight\":0,\"op\":\"chunk_exists\",\"name\":\"nope\"}]",
		zcps, zerr ), "the all-zero-weight checkpoints array parses" );
	AgentEvalScenario sZero = s;
	sZero.checkpoints = zcps;
	AgentEvalCheckResult rZero = CheckScenario( h, sZero );
	Check( !rZero.allPassed, "all-zero-weight: allPassed is false (one checkpoint failed)" );
	Check( std::abs( rZero.checkpointFraction - 0.5 ) < 1e-9,
		"all-zero-weight: checkpointFraction falls back to unweighted 0.5, NOT 1.0 (got "
		+ std::to_string( rZero.checkpointFraction ) + ")" );

	// The side effect: results.jsonl was written under runDir and contains
	// at least one JSON line naming this scenario.
	const std::string resultsPath = dir + "/results.jsonl";
	std::ifstream rf( resultsPath.c_str(), std::ios::binary );
	Check( static_cast<bool>( rf ), "results.jsonl was written to the run dir" );
	bool foundLine = false;
	std::string line;
	while( std::getline( rf, line ) ) {
		if( line.find( "\"partial_probe\"" ) != std::string::npos ) { foundLine = true; break; }
	}
	Check( foundLine, "results.jsonl contains a line naming this scenario" );
}

//----------------------------------------------------------------------
// T10: every committed evals/scenarios/*.json, run end-to-end through
// its OWN committed evals/fixtures/*.fixture.jsonl and checked against
// its OWN wired checkpoints[] -- proves the committed scenarios'
// checkpoints are actually TRUE of the fixtures that produce them, not
// just plausible-looking JSON.  Covers the 4 seed scenarios plus the
// "editing verbs & recovery" set (remove_object, material_add_and_bind,
// conflict_retry, reserved_name_recovery).  A planned `diagnosed_fix`
// scenario was deliberately NOT authored: status="diagnosed" (rawCode 3)
// requires the full re-derive to diagnose AFTER the validate-before-
// destroy dry-run of the SAME document passed (Job::
// RederiveCstDocumentFull_), which no deterministic headless edit can
// produce -- the same unreachability T6b proves for a validate-dirty
// live document.
//
// Fixture-authoring rule proven here: a canned fixture must never carry
// a baseHeadVersion it expects to MATCH -- the head uuid is minted per
// LOAD from a process-global counter (Job.cpp NextCstHeadUuid), so its
// value depends on how many loads ran earlier in the process.
// conflict_retry's stale {uuid:0,revision:0} is the one safe authored
// base (uuid 0 is never minted -> a GENUINE conflict from the real
// dispatcher); its retry round omits baseHeadVersion (the documented
// unconditional mode).
//----------------------------------------------------------------------
static void TestSeedScenariosCheckpointsAreTrue()
{
	std::printf( "T10: the committed scenarios' checkpoints are TRUE of their own fixtures...\n" );
	const std::string dir = ScratchRunDir( "t10_seed_scenarios" );

	const char* const ids[] = { "param_edit", "two_tool_observe", "error_path", "camera_orbit_timeline",
	                            "remove_object", "material_add_and_bind", "conflict_retry",
	                            "reserved_name_recovery" };
	const int idCount = static_cast<int>( sizeof( ids ) / sizeof( ids[0] ) );
	for( int i = 0; i < idCount; ++i ) {
		const std::string path = std::string( "evals/scenarios/" ) + ids[i] + ".json";
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ), std::string( ids[i] ) + ": loads (" + err + ")" );
		Check( !s.checkpoints.isArray() || s.checkpoints.size() > 0,
			std::string( ids[i] ) + ": carries at least one checkpoint to verify" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			std::string( ids[i] ) + ": run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			std::string( ids[i] ) + ": one check result per committed checkpoint" );
		for( std::size_t ci = 0; ci < r.checkpoints.size(); ++ci ) {
			Check( r.checkpoints[ci].passed,
				std::string( ids[i] ) + ": checkpoint[" + std::to_string( ci ) + "] (" + r.checkpoints[ci].kind +
				") is TRUE of the committed fixture (detail: " + r.checkpoints[ci].detail + ")" );
		}
		Check( r.allPassed, std::string( ids[i] ) + ": allPassed across its committed checkpoints" );
		Check( r.checkpointFraction == 1.0, std::string( ids[i] ) + ": checkpointFraction is 1.0" );
	}
}

int main()
{
	std::printf( "=== AgentEvalCheckTest (Eval-harness slice E3: the checker engine) ===\n" );

	TestDocumentCheckpoint();
	TestUntouchedCheckpoint();
	TestRenderCheckpoint();
	TestObjectmapCheckpoint();
	TestDiagnosticsCheckpointClean();
	TestDiagnosticsLiveDocInvariant();
	TestTrajectoryCheckpoint();
	TestUnknownKindAndMalformedShape();
	TestPartialCreditArithmetic();
	TestSeedScenariosCheckpointsAreTrue();

	std::printf( "=== AgentEvalCheckTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
