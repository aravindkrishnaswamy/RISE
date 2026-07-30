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
//    T4d "render" -- the `samples` PIN is not silently defeatable: on a
//        scene whose rasterizer does not implement
//        IRasterizer::SetSampleCountOverride (auto_rasterizer), an
//        UNPINNED band passes but the SAME band PINNED fails loudly;
//        the pinned band still passes on pathtracing_pel_rasterizer.
//    T4e "render" -- the width/height PIN is not silently defeatable: a
//        compareToImage reference wider than Job's kMaxFilmWidth makes
//        IJob::SetFilm honestly refuse, so the render falls back to the
//        scene-authored film size; the checkpoint must FAIL naming the
//        dropped pin and both dimension pairs.  Honoured dims (adopted
//        and explicit) and an unpinned checkpoint all still pass.
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
//    T7b "finalText" -- containsAll/containsAny/absent pass/fail against
//        handle.result.finalText; caseSensitive true/false/omitted; a
//        checkpoint with none of the three arrays fails loudly (never a
//        vacuous pass).
//    T8  Unknown checkpoint kind -> a FAILED checkpoint with a detail,
//        never a crash.  A non-object / kind-less checkpoint -> same.
//    T9  Partial-credit arithmetic: a scenario with a 2:1-weighted mix of
//        pass/fail checkpoints yields the exact expected fraction; a
//        scenario with zero checkpoints is a vacuous pass (fraction 1.0).
//    T10 EVERY committed evals/scenarios/*.json, discovered by a DYNAMIC
//        directory enumeration (never a hard-coded id list -- P2(i): a new
//        scenario file is covered automatically), run end-to-end through
//        its OWN evals/fixtures/*.fixture.jsonl and checked against its OWN
//        wired checkpoints[] -- every checkpoint is TRUE of the fixture
//        that produces it (allPassed, checkpointFraction 1.0).
//    T-mr material/geometry-richness "document" ops -- distinct_chunk_kinds
//        (kind-DISTINCTNESS, not raw match count -- two chunks of the SAME
//        qualifying kind still count once) / any_param_references_kind /
//        no_orphan_chunks / objects_reaching_kinds (review-round-1 P1-b
//        anti-decoy-gaming: a qualifying painter bound only to a hidden
//        far-away object does not count) -- both directions for each op,
//        plus loader validation (malformed op configs, the metricLabel
//        cross-checkpoint dedupe guard) and metricValue/metricLabel
//        population.
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
#include "../src/Library/Agent/ChatHttpTransport.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Job.h"

#include <algorithm>
#include <chrono>    // P1 fix test: wall-clock timing proxy for "no full PNG decode ran"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>   // std::memset (WriteSyntheticSolidPng's z_stream init)
#include <filesystem>
#include <fstream>
#include <iterator>
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

#ifndef NO_PNG_SUPPORT
	#include <zlib.h>   // P1 fix test: WriteSyntheticSolidPng streams a genuinely valid oversized PNG
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

// A single-round fixture whose final (only) turn carries a KNOWN, controlled
// text -- for the "finalText" checkpoint kind, which asserts on
// handle.result.finalText rather than tool calls or document state.
static const char* const kFinalTextFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"The name none is reserved as the unbind sentinel.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":10}}\"}\n";

// A single-round fixture that attempts propose_patch (a mutating verb) --
// paired with autonomy:"read" this gets refused (-32011) at the dispatcher.
static const char* const kMutatingAttemptFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_p1\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.1 0.1 0.9\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"could not edit\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A read_document then a plain end_turn (ONE tool call, no model edit) --
// the substrate for the scenario-intervention test: the runner's scripted
// co-editor is what mutates the head here, not the model.
static const char* const kReadThenDoneFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"reading\\\"},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_rd\\\",\\\"name\\\":\\\"read_document\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":50,\\\"output_tokens\\\":10}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A degenerate blank end_turn -- the loop's own documented ProviderError
// (matches evals/fixtures/error_path.fixture.jsonl).
static const char* const kErrorFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":10,\\\"output_tokens\\\":0}}\"}\n";

//----------------------------------------------------------------------
// Fixtures for TestTrajectoryNewAssertions (T7c): "toolOutcomes" and
// "toolCallAfterUserTurn" need specific per-call outcomes/turn placement
// the existing fixtures above don't exercise.
//----------------------------------------------------------------------

// propose_patch on a nonexistent target (REJECTED, status="rejected"),
// then a corrected retry on the real target (APPLIED) -- mirrors
// evals/fixtures/malformed_tool_recovery.fixture.jsonl's reject-then-apply
// shape so "occurrence":"first"/expect:"rejected" +
// "occurrence":"last"/expect:"applied" both have a real call to match.
static const char* const kToolOutcomesRejectThenApplyFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Recoloring.\\\"},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_ra1\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"bogus_target_xyz\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Retrying with the correct target.\\\"},{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_ra2\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_3\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// Two DIFFERENT, both-valid propose_patch calls (both APPLIED) -- the RED
// control for "occurrence":"first"/expect:"rejected" (the first call here
// is applied, not rejected).
static const char* const kToolOutcomesBothAppliedFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_ba1\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_ba2\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"sph\\\",\\\"param\\\":\\\"radius\\\",\\\"value\\\":\\\"0.9\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_3\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// A tool_use naming a verb that does not exist -- the dispatcher's method
// lookup returns a genuine JSON-RPC "error" envelope (kMethodNotFound,
// -32601 -- NOT the -32011 autonomy refusal), the substrate for
// expect:"error".
static const char* const kToolOutcomesErrorFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_er1\\\",\\\"name\\\":\\\"totally_bogus_tool_name\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"done\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// Two-user-turn fixture (mirrors evals/fixtures/multi_turn_edit.fixture.jsonl's
// shape): turn 1 only reads the document (no propose_patch), turn 2 calls
// propose_patch -- the substrate for toolCallAfterUserTurn's PASS case
// (minUserTurns:2 is satisfied).
static const char* const kToolCallAfterUserTurnPassFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_tc1\\\",\\\"name\\\":\\\"read_document\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Read the scene, no edit yet.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_3\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_tc2\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_4\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Recolored on the second turn.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

// The RED control: propose_patch fires on turn 1 (userCount==1 at that
// point) and turn 2 only reads -- no propose_patch call ever occurs at
// userCount>=2, so toolCallAfterUserTurn{name:"propose_patch",
// minUserTurns:2} must FAIL.
static const char* const kToolCallAfterUserTurnFailFixture =
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_1\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_tf1\\\",\\\"name\\\":\\\"propose_patch\\\",\\\"input\\\":{\\\"target\\\":\\\"pnt_albedo\\\",\\\"param\\\":\\\"color\\\",\\\"value\\\":\\\"0.9 0.1 0.1\\\"}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_2\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Recolored on the first turn.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_3\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"tool_use\\\",\\\"id\\\":\\\"toolu_tf2\\\",\\\"name\\\":\\\"read_document\\\",\\\"input\\\":{}}],\\\"stop_reason\\\":\\\"tool_use\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":100,\\\"output_tokens\\\":40}}\"}\n"
	"{\"provider\":\"anthropic\",\"body\":\"{\\\"id\\\":\\\"msg_4\\\",\\\"type\\\":\\\"message\\\",\\\"role\\\":\\\"assistant\\\",\\\"model\\\":\\\"claude-sonnet-5\\\",\\\"content\\\":[{\\\"type\\\":\\\"text\\\",\\\"text\\\":\\\"Just read again.\\\"}],\\\"stop_reason\\\":\\\"end_turn\\\",\\\"stop_sequence\\\":null,\\\"usage\\\":{\\\"input_tokens\\\":30,\\\"output_tokens\\\":8}}\"}\n";

//----------------------------------------------------------------------
// P1(b)'s MOCK transport -- the SAME minimal IChatHttpTransport seam
// tests/AgentEvalLiveTransportTest.cpp drives (canned {status,body}
// responses in sequence).  Needed here only to reach the 5xx-retry path:
// the REPLAY fetch (RunScenario) always synthesizes status 200, so a
// same-round 5xx retry can only be driven through RunScenarioLive.
//----------------------------------------------------------------------
class MockTransport : public IChatHttpTransport
{
public:
	struct Canned { long status; std::string body; std::string error; long elapsedMs; };
	std::vector<Canned>          responses;
	std::size_t                  idx = 0;
	std::vector<ChatHttpRequest> seenRequests;

	ChatHttpResponse Post( const ChatHttpRequest& req ) override
	{
		seenRequests.push_back( req );
		ChatHttpResponse r;
		if( idx >= responses.size() ) { r.status = 0; r.error = "mock: no more canned responses"; r.elapsedMs = 1; return r; }
		const Canned& c = responses[idx++];
		r.status = c.status; r.body = c.body; r.error = c.error; r.elapsedMs = c.elapsedMs;
		return r;
	}
};

// The Anthropic 500 body + the final-text body the P1(b) mock replays
// (matches evals/scenarios/param_edit.json's fixture shape).
static const char* const kBody500 = "{\"error\":\"internal server error\"}";
static const char* const kBodyFinalText =
	"{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet-5\","
	"\"content\":[{\"type\":\"text\",\"text\":\"Done: pnt_albedo is now red.\"}],"
	"\"stop_reason\":\"end_turn\",\"stop_sequence\":null,\"usage\":{\"input_tokens\":50,\"output_tokens\":10}}";

//----------------------------------------------------------------------
// Small helpers: a fresh scratch runDir, a raw-fixture writer, checkpoint
// JSON construction (built directly as JsonValue -- no scenario JSON file
// needed on disk beyond the scratch AgentEvalScenario the test builds in
// memory).
//----------------------------------------------------------------------
// Per-process scratch root.  Nothing stops two copies of this binary
// running at once (a stray earlier run, a developer running it by hand
// while the suite runs, a repeat-run loop chasing a suspected flake) --
// with a FIXED root, those processes' ScratchRunDir() calls collide: each
// invocation's remove_all() below can delete the OTHER process's
// in-flight fixtures/results out from under it, surfacing as a bogus
// "the test is flaky" failure (the exact hazard already fixed for the
// WriteTemp-style file helpers in AgentRenderAsyncTest.cpp /
// ViewportPaneSchedulerTest.cpp).  The pid suffix makes the whole tree
// unique per process; ScratchTestRoot() is split out so main() can
// recursively remove the ENTIRE per-process tree on exit, not just
// individual leaf directories.
static std::string ScratchTestRoot()
{
	const char* base = std::getenv( "TMPDIR" );
	if( !base ) base = std::getenv( "TMP" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' && dir.back() != '\\' ) dir += '/';
	dir += "rise_agent_eval_check_test_";
	dir += std::to_string( (long)getpid() );
	return dir;
}

static std::string ScratchRunDir( const char* leaf )
{
	std::string dir = ScratchTestRoot() + "/" + leaf;
	std::error_code ec;
	std::filesystem::remove_all( dir, ec );
	return dir;
}

//----------------------------------------------------------------------
// Collision-hazard proof: two processes calling ScratchTestRoot() must
// get DISTINCT trees.  This is a cheap, honest regression guard for the
// exact bug the pid suffix above fixes -- a real fork() (no exec, no
// scene load, no render) is negligible cost, and it proves the actual
// property (parent and child disagree) rather than just re-reading the
// same source line back.  POSIX-only: Windows has no fork(), and the
// pid-suffix logic itself is still exercised implicitly by every
// ScratchRunDir() call the other tests make.
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
		// ---- child: report its OWN ScratchTestRoot() back to the parent ----
		close( fds[0] );
		const std::string childRoot = ScratchTestRoot();
		ssize_t w = write( fds[1], childRoot.data(), childRoot.size() );
		(void)w;
		close( fds[1] );
		_exit( 0 );
	}
	// ---- parent ----
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

	// param_range (value-level-overfit fix): pnt_albedo.color is
	// "0.9 0.1 0.1" after the recolor.  A component-wise band that CONTAINS
	// it passes; a band that excludes any component fails; the shape
	// refusals (missing/non-array min or max, length mismatch,
	// component-count mismatch, non-numeric element) each fail LOUDLY.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0.5,0,0],\"max\":[1.0,0.5,0.5]}]",
		true, "param_range: 0.9 0.1 0.1 inside a reddish band" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0.9,0.1,0.1],\"max\":[0.9,0.1,0.1]}]",
		true, "param_range: an exact-point band (degenerate min==max) still passes" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0,0,0.5],\"max\":[0.45,0.45,1.0]}]",
		false, "param_range: a BLUE band excludes red 0.9 (component 0 out of [0,0.45]) FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"max\":[1,1,1]}]",
		false, "param_range missing \"min\" array fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0,0,0]}]",
		false, "param_range missing \"max\" array fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":\"0 0 0\",\"max\":[1,1,1]}]",
		false, "param_range non-array \"min\" fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0,0],\"max\":[1,1,1]}]",
		false, "param_range min/max length mismatch fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0,0],\"max\":[1,1]}]",
		false, "param_range component-count mismatch (2 bands vs a 3-vec value) fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"min\":[0,0,\"lo\"],\"max\":[1,1,1]}]",
		false, "param_range non-numeric \"min\" element fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"does_not_exist\",\"param\":\"color\",\"min\":[0,0,0],\"max\":[1,1,1]}]",
		false, "param_range unknown target fails loudly" );

	// param_references_kind ANY-OF array form (kind-overfit fix):
	// obj_sph.material names mat_diffuse, a lambertian_material.  An array
	// that INCLUDES lambertian_material passes; one that omits it fails; the
	// shape refusals (both/neither of the singular/array key, empty array,
	// non-string element) each fail LOUDLY.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKinds\":[\"pbr_metallic_roughness_material\",\"lambertian_material\"]}]",
		true, "param_references_kind referencedKinds: any-of matches lambertian_material" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKinds\":[\"pbr_metallic_roughness_material\",\"ggx_material\"]}]",
		false, "param_references_kind referencedKinds: none of the accepted kinds matches FAILS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKind\":\"lambertian_material\",\"referencedKinds\":[\"lambertian_material\"]}]",
		false, "param_references_kind: BOTH referencedKind and referencedKinds fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\"}]",
		false, "param_references_kind: NEITHER referencedKind nor referencedKinds fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKinds\":[]}]",
		false, "param_references_kind: empty referencedKinds array fails loudly" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_references_kind\",\"target\":\"obj_sph\",\"param\":\"material\",\"referencedKinds\":[123]}]",
		false, "param_references_kind: non-string referencedKinds element fails loudly" );
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

	// P2 fix: width/height/samples are narrowed via static_cast<unsigned
	// int>/static_cast<int> AFTER this validation -- a negative,
	// fractional, or absurdly large JSON number must be refused LOUDLY
	// (naming the offending field and value) rather than silently
	// wrapped/truncated by the cast.  checkOne already proves passed==
	// false; checkOneDetailContains additionally proves the refusal is
	// THIS validation firing (an honest diagnostic naming the field),
	// not some unrelated render crash.
	auto checkOneDetailContains = [&]( const std::string& cpJson, const std::string& mustContain, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( !r.checkpoints[0].passed, label + ": expected a FAILING checkpoint (honest refusal)" );
			Check( r.checkpoints[0].detail.find( mustContain ) != std::string::npos,
				label + ": detail names the offending field/value (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":-32,\"height\":32}]",
		"\"width\" must be a whole number",
		"a NEGATIVE width is refused, not silently wrapped by the unsigned cast" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":0,\"height\":32}]",
		"\"width\" must be a whole number",
		"a ZERO width is refused (must be >= 1)" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":32,\"height\":32.5}]",
		"\"height\" must be a whole number",
		"a FRACTIONAL height is refused, not silently truncated" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":1000000000000,\"height\":32}]",
		"\"width\" must be a whole number",
		"an ABSURDLY LARGE width is refused, not left to overflow the narrowing cast" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":32,\"height\":32,\"samples\":1.5}]",
		"\"samples\" must be a whole number",
		"a FRACTIONAL samples override is refused, not silently truncated" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":32,\"height\":32,\"samples\":-4}]",
		"\"samples\" must be a whole number",
		"a NEGATIVE samples override is refused, not silently coerced" );

	// Sanity control: a VALID whole-number override in range still passes
	// (the new validation doesn't spuriously reject legitimate values).
	checkOne( "[{\"kind\":\"render\",\"width\":16,\"height\":16,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "a valid whole-number width/height override still passes" );

	// P1 fix (2026-07-19 re-review): TOTAL-PIXEL budget.  32768x32768
	// passes BOTH per-axis caps individually ([1,32768], just proven
	// above) yet would demand ~1.07e9 pixels -- tens of GiB across the
	// rasterizer's frame + accumulation buffers if ever allocated.
	// Pre-fix, CheckRenderKind had NO product check at all -- only the
	// two INDEPENDENT per-axis IsWholeNumberInRange calls -- so this
	// checkpoint would have sailed past validation and gone straight
	// into session->Render(rp) at 32768x32768: this assertion (a clean,
	// fast REFUSAL naming the pixel budget) would have FAILED pre-fix
	// (no such refusal existed; the call would instead have attempted
	// the giant allocation).
	checkOneDetailContains(
		"[{\"kind\":\"render\",\"width\":32768,\"height\":32768,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		"exceeds the",
		"a 32768x32768 request (legal per-axis, illegal TOTAL) is refused by the pixel budget, never allocated" );

	// Sanity control: a legitimately large-but-sane render (1024x1024 =
	// ~1.05MP, comfortably under the 64,000,000-pixel budget) still
	// actually renders and passes -- the budget doesn't spuriously
	// reject sane requests.  samples:1 keeps the test fast.
	checkOne(
		"[{\"kind\":\"render\",\"width\":1024,\"height\":1024,\"samples\":1,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "a legitimate large-but-sane 1024x1024 render still passes under the pixel budget" );

	// P2 fix (2026-07-19 re-review): a present-but-WRONG-TYPED width AND
	// height (both non-numeric) must be REFUSED, not silently treated as
	// absent.  Pre-fix, `haveW`/`haveH` were `cp.has(...) &&
	// cp.get(...).isNumber()`, so when BOTH fields were non-numeric,
	// haveW==haveH==false and the haveW!=haveH mismatch guard never
	// fired either -- the checkpoint silently fell through to the
	// scene's DEFAULT render size and (with no contradicting band here)
	// would have PASSED: this assertion would have FAILED pre-fix
	// (passed==true, no "must be a number" detail -- a genuinely
	// malformed request reported success).
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":\"big\",\"height\":\"big\"}]",
		"\"width\" must be a number",
		"a wrong-typed width+height PAIR is refused, not silently defaulted" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"meanLumaMax\":\"bright\"}]",
		"\"meanLumaMax\" must be a number",
		"a wrong-typed render band is refused, not silently ignored" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"meanLumaMax\":1e999}]",
		"\"meanLumaMax\" must be a finite number",
		"a non-finite render band is refused, not silently made vacuous" );
	checkOneDetailContains( "[{\"kind\":\"render\",\"channelBalanceMax\":1.0}]",
		"\"channelBalanceMax\" must be > 1.0",
		"an unreachable channel-balance cap is refused before rendering" );

	// A wrong-typed `samples` is refused too.  Pre-fix,
	// `cp.has(\"samples\") && cp.get(\"samples\").isNumber()` was false
	// for a string value, so the override was silently SKIPPED (the
	// scene-authored sample count ran instead) rather than refused --
	// this assertion would have FAILED pre-fix (passed==true).
	checkOneDetailContains( "[{\"kind\":\"render\",\"width\":16,\"height\":16,\"samples\":\"fast\"}]",
		"\"samples\" must be a number",
		"a wrong-typed samples override is refused, not silently skipped" );

	// A camera vector with the WRONG SHAPE (2 components instead of 3)
	// is refused.  Pre-fix, CameraVec3String's `.at(2)` on a too-short
	// array returned JsonValue's static Null, whose .asNumber() default
	// is 0.0 -- so {"location":[0,0]} silently became camera location
	// "0 0 0" (zero-padded), never a refusal: this assertion would have
	// FAILED pre-fix (no "must be an array of exactly 3 numbers" detail
	// exists in the pre-fix code at all).
	checkOneDetailContains( "[{\"kind\":\"render\",\"camera\":{\"location\":[0,0],\"lookat\":[0,0,0]}}]",
		"\"camera.location\" must be an array of exactly 3 numbers",
		"a camera vector with the wrong element count is refused, not zero-padded" );

	// A NON-FINITE camera vector component (1e999 parses to +inf via
	// strtod) is refused.  Pre-fix, CameraVec3String formats "%.10g" on
	// the raw double with no finiteness check anywhere in
	// CheckRenderKind, so 1e999 became the LITERAL "inf" token inside
	// the "x y z" pose string AgentCameraOverride carries -- an invalid
	// camera state that would have PROCEEDED into session->Render(rp)
	// completely undetected: this assertion would have FAILED pre-fix
	// (no finiteness check, hence no "must be a finite number" detail,
	// existed on this path).
	checkOneDetailContains( "[{\"kind\":\"render\",\"camera\":{\"location\":[0,0,1e999],\"lookat\":[0,0,0]}}]",
		"must be a finite number",
		"a non-finite (1e999 -> +inf) camera vector component is refused" );

	// A wrong-typed `camera` (not an object) is refused too.  Pre-fix,
	// `cp.get(\"camera\").isObject()` was false, so the ENTIRE camera
	// block was silently skipped (no override applied, the scene's own
	// camera rendered) rather than refused -- this assertion would have
	// FAILED pre-fix (passed==true, no "must be an object" detail).
	checkOneDetailContains( "[{\"kind\":\"render\",\"camera\":\"oops\"}]",
		"\"camera\" must be an object",
		"a wrong-typed (non-object) camera field is refused, not silently skipped" );
}

//----------------------------------------------------------------------
// T4d: the `samples` PIN is not silently defeatable.
//
// `samples` on a render checkpoint exists so the GRADING render is
// converged and OUT of the model's control -- the scene's own sample
// count is a chunk the model may freely edit.  But
// IRasterizer::SetSampleCountOverride is OPT-IN: only the pixel-based
// family (PT / spectral PT / BDPT / VCM) implements it; MLT, the
// photon-map-only integrators, and AutoRasterizer's outer dispatcher
// wrapper honestly return false.  `auto_rasterizer` and
// `mlt_spectral_rasterizer` are first-class selectable rasterizers in
// the scene language, so on an open-ended "build a scene" task a model
// that swaps the rasterizer chunk reverts the grading render to its own
// authored sample count.
//
// This drives a scene that is BYTE-IDENTICAL to kScene except that its
// rasterizer chunk is `auto_rasterizer` -- a real, honestly
// non-supporting rasterizer, not a synthesized result -- and proves:
//   (a) the scene renders fine and an UNPINNED band passes (so the
//       failure below is the pin, not a broken scene);
//   (b) a PINNED band FAILS, with a diagnostic naming the situation;
//   (c) the SAME pinned band on kScene (pathtracing_pel_rasterizer,
//       which does support the override) still PASSES -- the new check
//       does not spuriously fire on a supporting rasterizer.  This is
//       the case that guards the pre-existing image_reconstruct_*
//       scenarios, which pin samples:64 against a PT scene.
//
// RED-PROVE: pre-fix, CheckRenderKind never inspected
// rr.samplesOverridden at all, so (b) rendered at the scene-authored 8
// spp, satisfied the generous band, and reported passed==true with a
// "render bands satisfied" detail -- assertion (b) fails without the
// fix (verified by reverting the samples-pin block).
//----------------------------------------------------------------------
static void TestRenderSamplesPinNotDefeatable()
{
	std::printf( "T4d: \"render\" checkpoint -- the `samples` pin is not silently defeatable...\n" );
	const std::string dir = ScratchRunDir( "t4b_samples_pin" );

	// kScene with ONE substitution: pathtracing_pel_rasterizer ->
	// auto_rasterizer (AutoRasterizer : public Rasterizer, which inherits
	// IRasterizer's `return false` default for SetSampleCountOverride).
	std::string autoScene = kScene;
	{
		const std::string from = "pathtracing_pel_rasterizer";
		const std::string to   = "auto_rasterizer";
		const std::size_t at = autoScene.find( from );
		Check( at != std::string::npos, "T4d fixture: kScene carries a pathtracing_pel_rasterizer chunk to substitute" );
		if( at != std::string::npos ) autoScene.replace( at, from.size(), to );
	}

	AgentEvalScenario sAuto = MakeScenario( "samples_pin_auto", autoScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle hAuto = RunScenario( sAuto, opts );
	Check( hAuto.result.terminalStatus == "final_text",
		"samples_pin_auto run reached final_text (the auto_rasterizer scene loads and renders)" );

	auto checkAgainst = [&]( AgentEvalRunHandle& h, const AgentEvalScenario& base,
	                         const std::string& cpJson, bool expectPass,
	                         const std::string& mustContain, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = base; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() != 1 ) { Check( false, label + ": expected exactly one checkpoint result" ); return; }
		Check( r.checkpoints[0].passed == expectPass,
			label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
			" (detail: " + r.checkpoints[0].detail + ")" );
		if( !mustContain.empty() ) {
			Check( r.checkpoints[0].detail.find( mustContain ) != std::string::npos,
				label + ": detail names the situation (detail: " + r.checkpoints[0].detail + ")" );
		}
	};

	// (a) CONTROL: no pin -> the auto_rasterizer scene renders and the
	// generous band passes.  Without this, a failure in (b) could just
	// mean "this scene doesn't render".
	checkAgainst( hAuto, sAuto,
		"[{\"kind\":\"render\",\"width\":16,\"height\":16,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "", "T4d(a) an UNPINNED band passes on the auto_rasterizer scene" );

	// (b) RED-PROVE: the SAME generous band, now PINNED, must FAIL --
	// the pin could not be applied, so the band was measured at the
	// scene-authored (model-controlled) count and is not trustworthy.
	checkAgainst( hAuto, sAuto,
		"[{\"kind\":\"render\",\"width\":16,\"height\":16,\"samples\":4,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		false, "\"samples\" pin was NOT APPLIED",
		"T4d(b) a PINNED band FAILS LOUDLY when the rasterizer cannot honour the pin" );

	// (c) GREEN CONTROL: the identical pinned checkpoint against the
	// stock kScene (pathtracing_pel_rasterizer -- the pixel-based family,
	// which DOES implement SetSampleCountOverride) still passes.  This is
	// the regression guard for image_reconstruct_single /
	// image_reconstruct_multi, which have always pinned samples:64.
	AgentEvalScenario sPt = MakeScenario( "samples_pin_pt", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunHandle hPt = RunScenario( sPt, opts );
	Check( hPt.result.terminalStatus == "final_text", "samples_pin_pt run reached final_text" );
	checkAgainst( hPt, sPt,
		"[{\"kind\":\"render\",\"width\":16,\"height\":16,\"samples\":4,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "", "T4d(c) the SAME pinned band still passes on a rasterizer that honours the pin" );
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

	// expectName "*" = name-AGNOSTIC hit (for open-ended build scenarios): the
	// centre pixel hits the sphere whatever it is named -> passes.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"*\"}}]", true,
		"queryAt \"*\" at the centre matches ANY object (name-agnostic hit)" );
	// A corner is background: an exact-miss expectName "" passes there (confirms
	// it is background), and "*" (expect ANY object) correctly FAILS there.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":0,\"y\":0,\"expectName\":\"\"}}]", true,
		"queryAt \"\" at a corner correctly expects a miss (confirms corner is background)" );
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":0,\"y\":0,\"expectName\":\"*\"}}]", false,
		"queryAt \"*\" at a background corner FAILS (no object to match)" );

	// A checkpoint naming none of the three assertions -- a shape error.
	checkOne( "[{\"kind\":\"objectmap\"}]", false, "objectmap with no assertions fails loudly" );
	// queryAt missing expectName -- a shape error, not a crash.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12}}]", false,
		"queryAt missing \"expectName\" fails loudly" );

	// --- Wave C: expectGeometryKind binds the HIT object's geometry to a
	// chunk kind.  In kScene obj_sph -> sph (a sphere_geometry); obj_emit ->
	// quad_emit (a clippedplane_geometry).  The centre pixel hits obj_sph.
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"*\",\"expectGeometryKind\":\"sphere_geometry\"}}]",
		true, "expectGeometryKind: \"*\" hit at centre binds obj_sph->sph, a sphere_geometry -> PASS" );
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"obj_sph\",\"expectGeometryKind\":\"sphere_geometry\"}}]",
		true, "expectGeometryKind: named obj_sph geometry is a sphere_geometry -> PASS" );
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"obj_sph\",\"expectGeometryKind\":\"clippedplane_geometry\"}}]",
		false, "expectGeometryKind: obj_sph geometry is NOT a clippedplane_geometry -> FAIL" );
	checkOne( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"*\",\"expectGeometryKind\":\"box_geometry\"}}]",
		false, "expectGeometryKind: the centred sphere is not a box_geometry -> FAIL" );
	// The mismatch detail must name the ACTUAL geometry kind (sphere_geometry).
	{
		JsonValue cps; std::string perr;
		JsonParse( "[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":12,\"y\":12,\"expectName\":\"obj_sph\",\"expectGeometryKind\":\"clippedplane_geometry\"}}]",
			cps, perr );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		const bool named = r.checkpoints.size() == 1 && !r.checkpoints[0].passed &&
			r.checkpoints[0].detail.find( "sphere_geometry" ) != std::string::npos;
		Check( named, "expectGeometryKind mismatch detail names the actual kind 'sphere_geometry' (got: " +
			( r.checkpoints.size() == 1 ? r.checkpoints[0].detail : std::string( "<no result>" ) ) + ")" );
	}
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
// T7c: "trajectory" checkpoint kind's three NEW assertion fields --
// expectAutonomyRefusal (the inverse of noAutonomyRefusal: PASS iff a
// refusal DID occur), toolOutcomes (per-call applied/staged/rejected/error
// gating, with "first"/"last"/"any" occurrence selection), and
// toolCallAfterUserTurn (a tool call must occur at/after a given running
// user-turn count).  Each gets a PASS case and a FAIL case, driven through
// real RunScenario runs (not hand-built envelopes) so the assertions are
// proven against the SAME trajectory shape the checker reads in production.
//----------------------------------------------------------------------
static void TestTrajectoryNewAssertions()
{
	std::printf( "T7c: \"trajectory\" checkpoint kind -- expectAutonomyRefusal / toolOutcomes / toolCallAfterUserTurn...\n" );
	const std::string dir = ScratchRunDir( "t7c_trajectory_new" );

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
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

	// expectAutonomyRefusal: PASS on the SAME read-autonomy/mutating-attempt
	// run TestTrajectoryCheckpoint's noAutonomyRefusal RED case drives (a
	// real -32011 refusal); FAIL on the SAME commit-autonomy/no-refusal run
	// its GREEN control drives.
	{
		AgentEvalScenario sRefused = MakeScenario( "eafr_refused", kScene, "Try to edit", "read", kMutatingAttemptFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( sRefused, opts );
		Check( h.result.toolCalls == 1, "eafr_refused: exactly 1 (refused) tool call dispatched" );
		checkOne( h, sRefused, "[{\"kind\":\"trajectory\",\"expectAutonomyRefusal\":true}]", true,
			"expectAutonomyRefusal PASSES when a -32011 refusal occurred" );

		AgentEvalScenario sClean = MakeScenario( "eafr_clean", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalRunHandle hClean = RunScenario( sClean, opts );
		checkOne( hClean, sClean, "[{\"kind\":\"trajectory\",\"expectAutonomyRefusal\":true}]", false,
			"expectAutonomyRefusal FAILS when no refusal occurred" );
	}

	// toolOutcomes: reject-then-apply run -- "first" is rejected, "last" is
	// applied; both pass.  The "first" spec re-run against the both-applied
	// run FAILS (its first call is applied, not rejected).
	{
		AgentEvalScenario s = MakeScenario( "to_reject_apply", kScene, "Recolor", "commit",
			kToolOutcomesRejectThenApplyFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 2, "to_reject_apply: 2 propose_patch calls dispatched" );

		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"first\",\"expect\":\"rejected\"}]}]",
			true, "toolOutcomes: first propose_patch is rejected" );
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"last\",\"expect\":\"applied\"}]}]",
			true, "toolOutcomes: last propose_patch is applied" );
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"first\",\"expect\":\"rejected\"},"
			"{\"name\":\"propose_patch\",\"occurrence\":\"last\",\"expect\":\"applied\"}]}]",
			true, "toolOutcomes: both specs together pass" );
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"first\",\"expect\":\"applied\"}]}]",
			false, "toolOutcomes: asserting the first call is applied FAILS (it was rejected)" );
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"never_called_tool\",\"expect\":\"applied\"}]}]",
			false, "toolOutcomes: a tool name that never occurred FAILS" );

		AgentEvalScenario sBoth = MakeScenario( "to_both_applied", kScene, "Recolor and resize", "commit",
			kToolOutcomesBothAppliedFixture, dir, "[]" );
		AgentEvalRunHandle hBoth = RunScenario( sBoth, opts );
		Check( hBoth.result.toolCalls == 2, "to_both_applied: 2 propose_patch calls dispatched" );
		checkOne( hBoth, sBoth,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"first\",\"expect\":\"rejected\"}]}]",
			false, "toolOutcomes: RED control -- first call is applied, not rejected, on the both-applied run" );
		checkOne( hBoth, sBoth,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"any\",\"expect\":\"applied\"}]}]",
			true, "toolOutcomes: occurrence \"any\" passes when either call is applied" );
	}

	// toolOutcomes expect:"staged" -- status="staged" occurs for an
	// External-authority session with a LIVE controller attached
	// (AgentSession::ProposePatch's Secure-MCP slice 5a doc).  RunScenario
	// NOW attaches a headless mock-Owner controller under autonomy:"propose"
	// (see RunScenarioDriven's propose branch), so a propose_patch there
	// GENUINELY stages -- the end-to-end proof of that lives in the dedicated
	// "propose_mode_stages: real staging through RunScenario" block below (and
	// in T10, which runs the committed propose_mode_stages scenario).  This
	// block keeps a FOCUSED unit-check of the CHECKER's "staged" branch on a
	// hand-built trajectory carrying the exact staged wire shape
	// {"applied":false,"status":"staged",...} -- isolating the checker's
	// per-outcome logic from the runner's staging plumbing.
	{
		const std::string trajPath = dir + "/staged_probe.trajectory.jsonl";
		Check( WriteFile( trajPath,
			"{\"run_type\":\"tool\",\"name\":\"propose_patch\",\"jsonrpc.response\":"
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"applied\":false,\"status\":\"staged\","
			"\"rawCode\":0,\"retriable\":false,\"message\":\"proposalId 7\"}}}\n" ),
			"wrote the hand-built staged-tool-record trajectory file" );

		AgentEvalRunHandle h;
		h.result.scenarioId = "staged_probe";
		h.result.terminalStatus = "final_text";
		h.trajectoryPath = trajPath;
		h.resultPath = dir + "/staged_probe.result.jsonl";

		AgentEvalScenario s;
		s.id = "staged_probe";

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"staged\"}]}]",
			true, "toolOutcomes: expect:\"staged\" passes on a hand-built staged tool response" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"applied\"}]}]",
			false, "toolOutcomes: expect:\"applied\" FAILS on a staged (not committed) response" );
	}

	// propose_mode_stages: real staging through RunScenario.  The committed
	// autonomy:"propose" scenario is run END-TO-END through RunScenario ->
	// RunScenarioDriven, which now attaches a headless mock-Owner controller so
	// propose_patch STAGES (instead of the old "no live controller attached"
	// rejection).  Proven three ways: (a) the run ends cleanly on final_text;
	// (b) staging did NOT commit -- the head-version is unchanged; and (c) the
	// load-bearing assertion -- the POST-RUN session's proposal queue actually
	// holds the staged param_edit on pnt_albedo, read directly off the
	// still-alive dispatcher (the E3 seam), not merely inferred from the
	// trajectory.  CheckScenario then confirms the committed checkpoints
	// (including toolOutcomes propose_patch:staged) all pass.
	{
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( "evals/scenarios/propose_mode_stages.json", s, err ),
			"propose_mode_stages: committed scenario loads (" + err + ")" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );

		Check( h.result.terminalStatus == "final_text",
			"propose_mode_stages: run ended on final_text (" + h.result.errorMessage + ")" );
		Check( h.result.headVersionFinal == h.result.headVersionStart,
			"propose_mode_stages: head-version UNCHANGED -- staging did not commit" );

		// (c) the post-run session's proposal queue carries the staged edit.
		bool staged = false;
		if( h.dispatcher && h.dispatcher->Session() ) {
			const std::vector<AgentSession::AgentProposalEntry> proposals =
				h.dispatcher->Session()->ListProposals();
			for( const AgentSession::AgentProposalEntry& p : proposals ) {
				if( p.kind == "param_edit" && p.target == "pnt_albedo" &&
				    p.param == "color" && p.status == "pending" ) { staged = true; break; }
			}
			Check( proposals.size() == 1,
				"propose_mode_stages: exactly one proposal was staged (got " +
				std::to_string( proposals.size() ) + ")" );
		}
		Check( staged,
			"propose_mode_stages: propose_patch GENUINELY staged a pending param_edit on pnt_albedo" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		for( std::size_t ci = 0; ci < r.checkpoints.size(); ++ci )
			Check( r.checkpoints[ci].passed,
				"propose_mode_stages: checkpoint[" + std::to_string( ci ) + "] (" +
				r.checkpoints[ci].kind + ") passed (detail: " + r.checkpoints[ci].detail + ")" );
		Check( r.allPassed, "propose_mode_stages: all committed checkpoints pass end-to-end" );
	}

	// toolOutcomes expect:"error" -- an unknown tool name is a genuine
	// JSON-RPC error (-32601), distinct from the -32011 autonomy refusal.
	{
		AgentEvalScenario s = MakeScenario( "to_error", kScene, "Do something odd", "commit", kToolOutcomesErrorFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 1, "to_error: 1 (errored) tool call dispatched" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"totally_bogus_tool_name\",\"expect\":\"error\"}]}]",
			true, "toolOutcomes: expect:\"error\" passes on an unknown-method JSON-RPC error" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"totally_bogus_tool_name\",\"expect\":\"applied\"}]}]",
			false, "toolOutcomes: expect:\"applied\" FAILS on an errored call" );
	}

	// toolCallAfterUserTurn: PASS when the named tool call occurs at/after
	// the running user-turn count reaches minUserTurns; FAIL when the only
	// such call happened on an earlier turn.
	{
		AgentEvalScenario sPass = MakeScenario( "tcaut_pass", kScene, "First, just read the scene.",
			"commit", kToolCallAfterUserTurnPassFixture, dir, "[]" );
		sPass.prompts.push_back( std::string( "Now recolor the sphere to red." ) );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle hPass = RunScenario( sPass, opts );
		Check( hPass.result.toolCalls == 2, "tcaut_pass: read_document + propose_patch dispatched" );
		checkOne( hPass, sPass,
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":2}}]",
			true, "toolCallAfterUserTurn: propose_patch after the 2nd user turn PASSES minUserTurns:2" );

		AgentEvalScenario sFail = MakeScenario( "tcaut_fail", kScene, "Recolor the sphere to red.",
			"commit", kToolCallAfterUserTurnFailFixture, dir, "[]" );
		sFail.prompts.push_back( std::string( "Now just re-read the scene." ) );
		AgentEvalRunHandle hFail = RunScenario( sFail, opts );
		Check( hFail.result.toolCalls == 2, "tcaut_fail: propose_patch + read_document dispatched" );
		checkOne( hFail, sFail,
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":2}}]",
			false, "toolCallAfterUserTurn: FAILS when the only propose_patch call precedes the 2nd user turn" );
	}
}

//----------------------------------------------------------------------
// T7b: "finalText" checkpoint kind -- asserts on handle.result.finalText
// (the last FinalText turn's text) rather than tool calls or document
// state, so refusal/clarification scenarios can check the ask-back
// message actually surfaces the right content.
//----------------------------------------------------------------------
static void TestFinalTextCheckpoint()
{
	std::printf( "T7b: \"finalText\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t7b_finaltext" );

	AgentEvalScenario s = MakeScenario( "finaltext_probe", kScene, "Explain the reserved name", "commit",
		kFinalTextFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "finaltext_probe run reached final_text" );
	Check( h.result.finalText == "The name none is reserved as the unbind sentinel.",
		"finaltext_probe run's finalText matches the fixture's text (got: " + h.result.finalText + ")" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
			Check( !r.checkpoints[0].detail.empty(), label + ": detail is never empty" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	// containsAll: PASSES when every needle is present, FAILS when one is missing.
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"reserved\",\"none\"]}]", true,
		"containsAll [reserved,none] passes (case-insensitive)" );
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"reserved\",\"MISSING_WORD\"]}]", false,
		"containsAll with a missing needle fails" );

	// containsAny: PASSES when at least one candidate is present, FAILS when none are.
	checkOne( "[{\"kind\":\"finalText\",\"containsAny\":[\"absent1\",\"none\"]}]", true,
		"containsAny with one matching candidate passes" );
	checkOne( "[{\"kind\":\"finalText\",\"containsAny\":[\"absent1\",\"absent2\"]}]", false,
		"containsAny with no matching candidates fails" );

	// absent: PASSES when none of the forbidden needles are present, FAILS when one is.
	checkOne( "[{\"kind\":\"finalText\",\"absent\":[\"kittens\"]}]", true,
		"absent: a genuinely-missing word passes" );
	checkOne( "[{\"kind\":\"finalText\",\"absent\":[\"reserved\"]}]", false,
		"absent: a present word fails" );

	// caseSensitive: true refuses a differently-cased needle; false (default,
	// explicit or omitted) matches case-insensitively.
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"Reserved\"],\"caseSensitive\":true}]", false,
		"caseSensitive:true fails to match the lowercase \"reserved\" in the text" );
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"Reserved\"],\"caseSensitive\":false}]", true,
		"caseSensitive:false (explicit) still matches case-insensitively" );
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"Reserved\"]}]", true,
		"caseSensitive omitted defaults to case-insensitive" );

	// None of containsAll/containsAny/absent present -> FAILS LOUDLY, never a
	// silent vacuous pass.
	checkOne( "[{\"kind\":\"finalText\"}]", false,
		"finalText checkpoint with no assertion array fails loudly (not a vacuous pass)" );

	// An EMPTY array -- or one holding only empty-string needles -- asserts
	// nothing and must also fail loudly (the vacuous-pass hole the guard closes).
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[]}]", false,
		"empty containsAll array asserts nothing -> loud fail (no silent vacuous pass)" );
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"\"]}]", false,
		"containsAll with only an empty-string needle asserts nothing -> loud fail" );

	// An empty-string needle ALONGSIDE a real one is SKIPPED, never a spurious
	// match (contains) or spurious trip (absent): the real needle still grades.
	checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"reserved\",\"\"]}]", true,
		"empty needle skipped in containsAll; the real needle still grades (pass)" );
	checkOne( "[{\"kind\":\"finalText\",\"absent\":[\"kittens\",\"\"]}]", true,
		"empty needle skipped in absent; does not spuriously trip the forbidden check (pass)" );

	// Empty finalText (a run that ended on a budget/error before any final_text
	// turn): contains checks fail (nothing to match), absent checks pass.
	{
		const std::string savedFinal = h.result.finalText;
		h.result.finalText.clear();
		checkOne( "[{\"kind\":\"finalText\",\"containsAll\":[\"reserved\"]}]", false,
			"empty finalText: containsAll fails (nothing to match)" );
		checkOne( "[{\"kind\":\"finalText\",\"absent\":[\"reserved\"]}]", true,
			"empty finalText: absent passes (nothing forbidden is present)" );
		h.result.finalText = savedFinal;
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
	bad.prompts.push_back( std::string( "hi" ) );
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

	// P2(i): enumerate EVERY committed evals/scenarios/*.json dynamically --
	// a hard-coded id list silently stops covering a scenario added later.
	// Sorted for a deterministic run order.  The non-empty assert guards
	// against a broken glob/path vacuously "passing" by finding nothing.
	std::vector<std::string> paths;
	for( const auto& entry : std::filesystem::directory_iterator( "evals/scenarios" ) ) {
		if( entry.path().extension() == ".json" ) paths.push_back( entry.path().string() );
	}
	std::sort( paths.begin(), paths.end() );
	Check( !paths.empty(), "evals/scenarios/*.json enumeration found at least one scenario file" );

	for( const std::string& path : paths ) {
		const std::string id = std::filesystem::path( path ).stem().string();
		AgentEvalScenario s;
		std::string err;
		Check( LoadEvalScenario( path, s, err ), id + ": loads (" + err + ")" );
		Check( !s.checkpoints.isArray() || s.checkpoints.size() > 0,
			id + ": carries at least one checkpoint to verify" );

		AgentEvalRunOptions opts;
		opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			id + ": run did not load_error (" + h.result.errorMessage + ")" );
		Check( h.result.wallMs >= 0,
			id + ": records a wall-clock completion duration (wallMs >= 0)" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			id + ": one check result per committed checkpoint" );
		for( std::size_t ci = 0; ci < r.checkpoints.size(); ++ci ) {
			Check( r.checkpoints[ci].passed,
				id + ": checkpoint[" + std::to_string( ci ) + "] (" + r.checkpoints[ci].kind +
				") is TRUE of the committed fixture (detail: " + r.checkpoints[ci].detail + ")" );
		}
		Check( r.allPassed, id + ": allPassed across its committed checkpoints" );
		Check( r.checkpointFraction == 1.0, id + ": checkpointFraction is 1.0" );
	}
}

//----------------------------------------------------------------------
// Scenario interventions (Part B): a scripted co-editor edit fires AFTER
// the Nth dispatched tool call, mutating the shared head WITHOUT a model
// turn -- proving the runner's intervention seam applies through the live
// session and bumps headVersion.  The fixture does read_document then a
// plain end_turn (ONE tool call, no model edit), so ANY change to the head
// is the intervention's doing.
//----------------------------------------------------------------------
static void TestScenarioIntervention()
{
	std::printf( "T-intervention: a scenario intervention fires after tool call N and bumps headVersion...\n" );
	const std::string dir = ScratchRunDir( "t_intervention" );

	// Control: the SAME fixture with NO intervention leaves the head where
	// it started (the model performs no edit).
	{
		AgentEvalScenario s = MakeScenario( "iv_control", kScene, "Read only", "commit",
			kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "iv_control: reached final_text" );
		Check( h.result.toolCalls == 1, "iv_control: exactly one tool call dispatched" );
		Check( h.result.headVersionFinal == h.result.headVersionStart,
			"iv_control: head UNCHANGED with no intervention" );
	}

	// With an intervention after tool call 1: the head moves, the document
	// carries the intervention's value, and headVersionFinal bumps by 1.
	{
		AgentEvalScenario s = MakeScenario( "iv_edit", kScene, "Read only", "commit",
			kReadThenDoneFixture, dir, "[]" );
		AgentEvalIntervention iv;
		iv.afterToolCalls = 1;
		iv.op = "param_edit";
		iv.target = "pnt_albedo";
		iv.param = "color";
		iv.value = "0.42 0.42 0.42";
		s.interventions.push_back( iv );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "iv_edit: reached final_text" );
		Check( h.result.toolCalls == 1, "iv_edit: exactly one tool call dispatched (intervention is NOT a tool call)" );
		Check( h.result.headVersionFinal == h.result.headVersionStart + 1,
			"iv_edit: head revision bumped by exactly 1 (the intervention committed)" );
		Check( h.dispatcher && h.dispatcher->Session(), "iv_edit: live dispatcher/session survives" );
		if( h.dispatcher && h.dispatcher->Session() ) {
			const std::string finalDoc = h.dispatcher->Session()->ReadDocument();
			Check( finalDoc.find( "0.42 0.42 0.42" ) != std::string::npos,
				"iv_edit: the post-run document carries the intervention's value" );
		}
		// The trajectory records the intervention honestly as a history_edit.
		std::ifstream tf( h.trajectoryPath.c_str(), std::ios::binary );
		std::string body( ( std::istreambuf_iterator<char>( tf ) ), std::istreambuf_iterator<char>() );
		Check( body.find( "scenario_intervention" ) != std::string::npos,
			"iv_edit: the trajectory carries a history_edit with reason scenario_intervention" );
	}
}

//----------------------------------------------------------------------
// P1(b): the maxLlmCalls budget must not be bypassed by a same-round
// retry.  A cap of 1 plus a fixture whose FIRST response is a transient
// HTTP 500 (which the drive loop retries once, same round) must NOT let
// the retry's request go out -- the retry would be the 2nd llm call,
// exceeding the cap.  Only reachable through RunScenarioLive (the REPLAY
// fetch always synthesizes status 200, so a same-round 5xx retry can
// never fire on the replay path -- see RunScenarioDriven's drive-loop
// comment) -- mirrors tests/AgentEvalLiveTransportTest.cpp's
// TestLiveHttp5xxRetrySucceeds/TestLiveHttp5xxRetryStillFails coverage.
//----------------------------------------------------------------------
static void TestBudgetLlmCallsNotBypassedByRetry()
{
	std::printf( "P1(b): maxLlmCalls budget is not bypassed by a same-round 5xx retry...\n" );

	AgentEvalScenario scenario;
	std::string err;
	Check( LoadEvalScenario( "evals/scenarios/param_edit.json", scenario, err ), "param_edit loads (" + err + ")" );
	scenario.budgets.maxLlmCalls = 1;

	MockTransport mock;
	mock.responses.push_back( { 500, kBody500, "", 3 } );
	mock.responses.push_back( { 200, kBodyFinalText, "", 4 } );   // must NEVER be consumed -- the cap stops the retry first

	AgentEvalLiveRunOptions opts;
	opts.runDir = ScratchRunDir( "p1b_budget_vs_5xx_retry" );
	opts.transport = &mock;
	opts.provider = ChatProvider::Anthropic;
	opts.apiKey = "unit-test-key-not-real";

	AgentEvalRunHandle h = RunScenarioLive( scenario, opts );
	Check( h.result.llmCalls <= 1,
		"llmCalls never exceeds the maxLlmCalls:1 cap (got " + std::to_string( h.result.llmCalls ) + ")" );
	Check( h.result.terminalStatus == "budget_llm_calls",
		"the run stops on budget_llm_calls, not a silently-over-cap provider_error/final_text (got '" +
		h.result.terminalStatus + "')" );
	Check( h.result.budgetHit, "budgetHit is true" );
	Check( mock.seenRequests.size() == 1,
		"the transport saw exactly ONE POST -- the retry's request was never built/sent (got " +
		std::to_string( mock.seenRequests.size() ) + ")" );
}

//----------------------------------------------------------------------
// P1(a): CheckScenario's results ledger (<runDir>/results.jsonl) RECORDS
// terminalStatus alongside checkpointFraction/allPassed -- a reviewer can
// see that a run which passed every checkpoint nonetheless ended on a
// non-clean status.  This scenario reaches "final_text", so the P2
// terminal-success gate (see TestTerminalSuccessGuard) never fires here;
// pass/fail stays checkpoint-fraction based.
//----------------------------------------------------------------------
static void TestResultsLedgerRecordsTerminalStatus()
{
	std::printf( "P1(a): results.jsonl carries a \"terminalStatus\" field...\n" );
	const std::string dir = ScratchRunDir( "p1a_results_ledger" );

	AgentEvalScenario s = MakeScenario( "ledger_probe", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "ledger_probe run reached final_text" );

	CheckScenario( h, s );

	const std::string resultsPath = dir + "/results.jsonl";
	std::ifstream rf( resultsPath.c_str(), std::ios::binary );
	Check( static_cast<bool>( rf ), "results.jsonl was written" );
	std::string line;
	bool found = false;
	while( std::getline( rf, line ) ) {
		if( line.find( "\"ledger_probe\"" ) == std::string::npos ) continue;
		JsonValue v; std::string perr;
		Check( JsonParse( line, v, perr ), "the ledger_probe results.jsonl line parses as JSON" );
		Check( v.has( "terminalStatus" ) && v.get( "terminalStatus" ).isString(),
			"the results ledger line carries a string \"terminalStatus\" field" );
		Check( v.get( "terminalStatus" ).asString() == "final_text",
			"the recorded terminalStatus matches the run's actual terminal status (got '" +
			v.get( "terminalStatus" ).asString() + "')" );
		found = true;
		break;
	}
	Check( found, "found the ledger_probe line in results.jsonl" );
}

//----------------------------------------------------------------------
// P1(c): a checkpoint field the checker RECOGNIZES for its "kind" but that
// carries the WRONG JSON type must HARD-FAIL LoadEvalScenario at load time
// (fail-fast) rather than silently degrade to a skipped assertion (the
// silent-weakening bug).  A correctly-typed sibling still loads cleanly.
//----------------------------------------------------------------------
static void TestLoadEvalScenarioStrictCheckpointFieldTypes()
{
	std::printf( "P1(c): LoadEvalScenario hard-fails a wrong-typed RECOGNIZED checkpoint field...\n" );
	const std::string dir = ScratchRunDir( "p1c_strict_types" );

	auto writeScenario = [&]( const std::string& id, const std::string& checkpointsJson ) -> std::string {
		JsonValue root = JsonValue::MakeObject();
		root.set( "id", JsonValue::MakeString( id ) );
		root.set( "title", JsonValue::MakeString( id ) );
		JsonValue scene = JsonValue::MakeObject();
		scene.set( "inline", JsonValue::MakeString( kScene ) );
		root.set( "scene", scene );
		JsonValue prompts = JsonValue::MakeArray();
		prompts.push_back( JsonValue::MakeString( "do something" ) );
		root.set( "prompts", prompts );
		JsonValue cps; std::string perr;
		Check( JsonParse( checkpointsJson, cps, perr ), id + ": checkpoints JSON itself parses" );
		root.set( "checkpoints", cps );
		const std::string path = dir + "/" + id + ".json";
		WriteFile( path, JsonSerialize( root ) );
		return path;
	};

	// "trajectory".maxToolCalls as a STRING (not a number) -- must fail
	// loudly, naming the offending field.
	{
		const std::string path = writeScenario( "bad_traj_type",
			"[{\"kind\":\"trajectory\",\"maxToolCalls\":\"7\"}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "wrong-typed maxToolCalls (string \"7\") FAILS to load" );
		Check( err.find( "maxToolCalls" ) != std::string::npos,
			"the load error names the offending field \"maxToolCalls\" (got: " + err + ")" );
	}
	// The correctly-typed sibling still loads cleanly.
	{
		const std::string path = writeScenario( "good_traj_type",
			"[{\"kind\":\"trajectory\",\"maxToolCalls\":7}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "correctly-typed maxToolCalls (number 7) loads (" + err + ")" );
	}

	// A second field/kind combination for coverage beyond one example: a
	// "document" op:param_range checkpoint's "min" as a bare number (must
	// be an array of numbers) fails; the array form loads.
	{
		const std::string path = writeScenario( "bad_range_type",
			"[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\","
			"\"min\":0.1,\"max\":[1,1,1]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "param_range \"min\" as a bare number (not an array) FAILS to load" );
		Check( err.find( "min" ) != std::string::npos, "the load error names \"min\" (got: " + err + ")" );
	}
	{
		const std::string path = writeScenario( "good_range_type",
			"[{\"kind\":\"document\",\"op\":\"param_range\",\"target\":\"pnt_albedo\",\"param\":\"color\","
			"\"min\":[0,0,0],\"max\":[1,1,1]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "correctly-typed param_range min/max arrays load (" + err + ")" );
	}

	// (g) "document".op:"param_series_orbit".timeParam as a bare NUMBER (not
	// a string) -- must fail loudly, naming the offending field.  The
	// kStringFields type check runs before the op-specific required-field
	// checks, so this fails on "timeParam" alone without needing the op's
	// other mandatory fields (chunkKind/param/minKeyframes/...) present.
	{
		const std::string path = writeScenario( "bad_orbit_timeparam_type",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"timeParam\":5}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "wrong-typed timeParam (number 5) FAILS to load" );
		Check( err.find( "timeParam" ) != std::string::npos,
			"the load error names the offending field \"timeParam\" (got: " + err + ")" );
	}

	// "finalText".containsAll as a bare NUMBER (not a string array) -- must
	// fail loudly, naming the offending field.
	{
		const std::string path = writeScenario( "bad_finaltext_type",
			"[{\"kind\":\"finalText\",\"containsAll\":42}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "wrong-typed containsAll (number 42) FAILS to load" );
		Check( err.find( "containsAll" ) != std::string::npos,
			"the load error names the offending field \"containsAll\" (got: " + err + ")" );
	}
	// The correctly-typed sibling still loads cleanly.
	{
		const std::string path = writeScenario( "good_finaltext_type",
			"[{\"kind\":\"finalText\",\"containsAll\":[\"reserved\",\"none\"]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "correctly-typed containsAll (string array) loads (" + err + ")" );
	}

	// "trajectory".toolOutcomes as a bare NUMBER (not an array) -- must fail
	// loudly, naming the offending field.
	{
		const std::string path = writeScenario( "bad_toolOutcomes_type",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":42}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "wrong-typed toolOutcomes (number 42) FAILS to load" );
		Check( err.find( "toolOutcomes" ) != std::string::npos,
			"the load error names the offending field \"toolOutcomes\" (got: " + err + ")" );
	}
	// The correctly-typed sibling still loads cleanly.
	{
		const std::string path = writeScenario( "good_toolOutcomes_type",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"first\",\"expect\":\"rejected\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "correctly-typed toolOutcomes (array of objects) loads (" + err + ")" );
	}

	// "trajectory".toolCallAfterUserTurn.minUserTurns as a STRING (not a
	// number) -- must fail loudly, naming the offending field.
	{
		const std::string path = writeScenario( "bad_toolCallAfterUserTurn_type",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":\"2\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "wrong-typed toolCallAfterUserTurn.minUserTurns (string \"2\") FAILS to load" );
		Check( err.find( "toolCallAfterUserTurn.minUserTurns" ) != std::string::npos,
			"the load error names the offending field \"toolCallAfterUserTurn.minUserTurns\" (got: " + err + ")" );
	}
	// The correctly-typed sibling still loads cleanly.
	{
		const std::string path = writeScenario( "good_toolCallAfterUserTurn_type",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":2}}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "correctly-typed toolCallAfterUserTurn loads (" + err + ")" );
	}

	// A present-but-EMPTY toolOutcomes array asserts nothing -> must be
	// rejected at load (never a silent vacuous pass at check time).
	{
		const std::string path = writeScenario( "empty_toolOutcomes",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "empty toolOutcomes array FAILS to load" );
		Check( err.find( "NON-EMPTY" ) != std::string::npos, "the load error explains the empty-array rejection (got: " + err + ")" );
	}
	// An UNRECOGNIZED occurrence (typo) must be rejected at load -- it must
	// not silently downgrade to the weakest "any" check.
	{
		const std::string path = writeScenario( "bad_occurrence_enum",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"occurrence\":\"frist\",\"expect\":\"applied\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolOutcomes.occurrence typo 'frist' FAILS to load" );
		Check( err.find( "occurrence" ) != std::string::npos, "the load error names occurrence (got: " + err + ")" );
	}
	// An UNRECOGNIZED expect must be rejected at load.
	{
		const std::string path = writeScenario( "bad_expect_enum",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"succeeded\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolOutcomes.expect 'succeeded' (not applied|staged|rejected|error) FAILS to load" );
	}
	// A toolOutcomes spec MISSING its load-bearing name/expect must be rejected.
	{
		const std::string path = writeScenario( "missing_expect",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolOutcomes spec missing \"expect\" FAILS to load" );
	}
	// toolCallAfterUserTurn missing minUserTurns must be rejected.
	{
		const std::string path = writeScenario( "missing_minUserTurns",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolCallAfterUserTurn missing minUserTurns FAILS to load" );
	}

	//--- Wave A additions -------------------------------------------------

	// toolOutcomes.expect:"conflict" is a recognized enum -> loads.
	{
		const std::string path = writeScenario( "good_toolOutcomes_conflict",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"conflict\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "toolOutcomes expect:\"conflict\" loads (" + err + ")" );
	}
	// toolOutcomes.argsContains present-but-empty must be rejected.
	{
		const std::string path = writeScenario( "empty_argsContains",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":\"\",\"expect\":\"applied\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolOutcomes empty argsContains FAILS to load" );
		Check( err.find( "argsContains" ) != std::string::npos, "the load error names argsContains (got: " + err + ")" );
	}
	// toolCallAfterUserTurn ARRAY form loads.
	{
		const std::string path = writeScenario( "good_tcaut_array",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":[{\"name\":\"read_document\",\"maxUserTurns\":1},"
			"{\"name\":\"propose_patch\",\"minUserTurns\":2}]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "toolCallAfterUserTurn ARRAY form loads (" + err + ")" );
	}
	// A toolCallAfterUserTurn spec with neither min nor max asserts nothing -> rejected.
	{
		const std::string path = writeScenario( "tcaut_neither_bound",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":[{\"name\":\"propose_patch\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolCallAfterUserTurn array spec with neither min/max FAILS to load" );
		Check( err.find( "at least one" ) != std::string::npos, "the load error explains the neither-bound rejection (got: " + err + ")" );
	}
	// An EMPTY toolCallAfterUserTurn array asserts nothing -> rejected.
	{
		const std::string path = writeScenario( "empty_tcaut_array",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":[]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "empty toolCallAfterUserTurn array FAILS to load" );
	}
	// document any-of-kind: a target-less param_range WITHOUT a chunkKind is rejected.
	{
		const std::string path = writeScenario( "anyofkind_no_chunkkind",
			"[{\"kind\":\"document\",\"op\":\"param_range\",\"param\":\"fov\",\"min\":[30],\"max\":[50]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "target-less param_range WITHOUT chunkKind FAILS to load" );
		Check( err.find( "chunkKind" ) != std::string::npos, "the load error names chunkKind (got: " + err + ")" );
	}
	// document any-of-kind: a target-less param_range WITH a chunkKind loads.
	{
		const std::string path = writeScenario( "anyofkind_with_chunkkind",
			"[{\"kind\":\"document\",\"op\":\"param_range\",\"chunkKind\":\"pinhole_camera\",\"param\":\"fov\",\"min\":[30],\"max\":[50]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "target-less param_range WITH chunkKind loads (" + err + ")" );
	}
	// render channelBalanceMax <= 1.0 is unreachable -> rejected; > 1.0 loads.
	{
		const std::string path = writeScenario( "bad_channelbalance",
			"[{\"kind\":\"render\",\"channelBalanceMax\":1.0}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "channelBalanceMax 1.0 FAILS to load" );
		Check( err.find( "channelBalanceMax" ) != std::string::npos, "the load error names channelBalanceMax (got: " + err + ")" );
	}
	{
		const std::string path = writeScenario( "good_channelbalance",
			"[{\"kind\":\"render\",\"channelBalanceMax\":3.0}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "channelBalanceMax 3.0 loads (" + err + ")" );
	}
	// proposal: a checkpoint with none of countMin/countMax/match is vacuous -> rejected.
	{
		const std::string path = writeScenario( "vacuous_proposal",
			"[{\"kind\":\"proposal\"}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "a proposal checkpoint with no assertion FAILS to load" );
	}
	// proposal: a match with a valid shape loads.
	{
		const std::string path = writeScenario( "good_proposal",
			"[{\"kind\":\"proposal\",\"countMin\":1,\"match\":{\"target\":\"pnt_albedo\",\"param\":\"color\","
			"\"status\":\"pending\",\"valueMin\":[0.5,0,0],\"valueMax\":[1,0.5,0.5]}}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "a well-formed proposal checkpoint loads (" + err + ")" );
	}
	// proposal: match.valueMin must be a NUMBER array (not a string array).
	{
		const std::string path = writeScenario( "bad_proposal_valuemin",
			"[{\"kind\":\"proposal\",\"match\":{\"valueMin\":[\"lo\"],\"valueMax\":[1]}}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "proposal match.valueMin string-array FAILS to load" );
		Check( err.find( "valueMin" ) != std::string::npos, "the load error names valueMin (got: " + err + ")" );
	}

	//--- Wave C additions -------------------------------------------------

	// document "where" is any-of-kind only -> rejected with a named target.
	{
		const std::string path = writeScenario( "where_with_target",
			"[{\"kind\":\"document\",\"op\":\"param_equals\",\"target\":\"pnt_albedo\",\"param\":\"color\",\"value\":\"1 1 1\","
			"\"where\":[{\"param\":\"name\",\"value\":\"x\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "\"where\" with a named target FAILS to load" );
		Check( err.find( "where" ) != std::string::npos, "the load error names where (got: " + err + ")" );
	}
	// document "where" range entry missing "max" -> rejected.
	{
		const std::string path = writeScenario( "where_range_no_max",
			"[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\",\"param\":\"color\",\"value\":\"1 1 1\","
			"\"where\":[{\"param\":\"color\",\"min\":[0,0,0]}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "\"where\" range entry missing \"max\" FAILS to load" );
	}
	// A well-formed any-of-kind "where" (equality + range entries) loads.
	{
		const std::string path = writeScenario( "where_ok",
			"[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\",\"param\":\"color\",\"value\":\"1 1 1\","
			"\"where\":[{\"param\":\"name\",\"value\":\"pnt_emit\"},{\"param\":\"color\",\"min\":[0,0,0],\"max\":[2,2,2]}]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "well-formed any-of-kind where loads (" + err + ")" );
	}
	// param_series_orbit: minKeyframes < 2 -> rejected.
	{
		const std::string path = writeScenario( "orbit_minkf_1",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\",\"param\":\"value\","
			"\"minKeyframes\":1,\"minAngularSpreadDeg\":180}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "param_series_orbit minKeyframes 1 FAILS to load" );
		Check( err.find( "minKeyframes" ) != std::string::npos, "the load error names minKeyframes (got: " + err + ")" );
	}
	// param_series_orbit: minAngularSpreadDeg out of (0,360] -> rejected.
	{
		const std::string path = writeScenario( "orbit_spread_400",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\",\"param\":\"value\","
			"\"minKeyframes\":4,\"minAngularSpreadDeg\":400}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "param_series_orbit minAngularSpreadDeg 400 FAILS to load" );
	}
	// param_series_orbit: a named target (not any-of-kind) -> rejected.
	{
		const std::string path = writeScenario( "orbit_with_target",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"target\":\"tl\",\"chunkKind\":\"timeline\",\"param\":\"value\","
			"\"minKeyframes\":4,\"minAngularSpreadDeg\":180}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "param_series_orbit with a named target FAILS to load" );
	}
	// param_series_orbit: maxRadiusRatio <= 1 -> rejected; a well-formed one loads.
	{
		const std::string path = writeScenario( "orbit_ratio_1",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\",\"param\":\"value\","
			"\"minKeyframes\":4,\"minAngularSpreadDeg\":180,\"maxRadiusRatio\":1.0}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "param_series_orbit maxRadiusRatio 1.0 FAILS to load" );
	}
	{
		const std::string path = writeScenario( "orbit_ok_load",
			"[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\",\"param\":\"value\","
			"\"minKeyframes\":4,\"minAngularSpreadDeg\":240,\"centerWithin\":{\"x\":0,\"z\":0,\"radius\":6},\"maxRadiusRatio\":2.0}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "a well-formed param_series_orbit loads (" + err + ")" );
	}
	// objectmap: expectGeometryKind paired with expectName "" (miss) -> rejected.
	{
		const std::string path = writeScenario( "geomkind_with_miss",
			"[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":1,\"y\":1,\"expectName\":\"\",\"expectGeometryKind\":\"sphere_geometry\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "expectGeometryKind with expectName \"\" (miss) FAILS to load" );
		Check( err.find( "expectGeometryKind" ) != std::string::npos, "the load error names expectGeometryKind (got: " + err + ")" );
	}
	// objectmap: expectGeometryKind with a real expectName (or "*") loads.
	{
		const std::string path = writeScenario( "geomkind_ok",
			"[{\"kind\":\"objectmap\",\"queryAt\":{\"x\":1,\"y\":1,\"expectName\":\"*\",\"expectGeometryKind\":\"sphere_geometry\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "expectGeometryKind with expectName \"*\" loads (" + err + ")" );
	}
	// trajectory: toolOutcomes argsContains ARRAY with an empty element -> rejected.
	{
		const std::string path = writeScenario( "argscontains_arr_empty_elem",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":[\"ok\",\"\"],\"expect\":\"applied\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolOutcomes argsContains array with empty element FAILS to load" );
		Check( err.find( "argsContains" ) != std::string::npos, "the load error names argsContains (got: " + err + ")" );
	}
	// trajectory: toolOutcomes argsContains ARRAY of non-empty strings loads.
	{
		const std::string path = writeScenario( "argscontains_arr_ok",
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":[\"pnt_albedo\",\"color\"],\"expect\":\"applied\"}]}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "toolOutcomes argsContains array loads (" + err + ")" );
	}
	// trajectory: toolCallAfterUserTurn expect with an UNKNOWN enum -> rejected.
	{
		const std::string path = writeScenario( "tcaut_expect_bad",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":1,\"expect\":\"succeeded\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( path, s, err ), "toolCallAfterUserTurn expect 'succeeded' FAILS to load" );
		Check( err.find( "expect" ) != std::string::npos, "the load error names expect (got: " + err + ")" );
	}
	// trajectory: toolCallAfterUserTurn with a valid expect + argsContains array loads.
	{
		const std::string path = writeScenario( "tcaut_expect_ok",
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":[\"pnt_albedo\",\"color\"],\"minUserTurns\":1,\"expect\":\"applied\"}}]" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( path, s, err ), "toolCallAfterUserTurn expect:applied + argsContains array loads (" + err + ")" );
	}
}

//----------------------------------------------------------------------
// P2: the terminal-success gate on CheckScenario's allPassed.  A run that
// ended on a non-"final_text" terminal status is NOT a success regardless
// of checkpoint state, UNLESS the scenario explicitly asserts that
// terminalStatus via a "trajectory" checkpoint (an intentional
// budget/refusal test).  All current scenarios except error_path.json
// assert terminalStatus:"final_text", so a passing run already ends final_text
// and this gate never fires for them (T1 below is that control); it
// guards a FUTURE scenario that omits the assertion from silently
// scoring a budget/provider-killed run as a success (T2), while still
// letting an intentional budget/refusal scenario opt in (T3).
//----------------------------------------------------------------------
static void TestTerminalSuccessGuard()
{
	std::printf( "P2: terminal-success gate on CheckScenario's allPassed...\n" );
	const std::string dir = ScratchRunDir( "p2_terminal_gate" );

	// A scenario with one checkpoint that passes on kScene (mirrors
	// TestDocumentCheckpoint's chunk_exists control) and does NOT assert
	// terminalStatus at all.
	AgentEvalScenario s = MakeScenario( "guard_probe", kScene, "Recolor", "commit", kParamEditFixture, dir,
		"[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"pnt_albedo\"}]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "guard_probe run reached final_text" );

	// T1 -- control: final_text + a passing checkpoint -> allPassed true,
	// the gate stays silent (terminalGateNote empty).
	{
		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( !r.checkpoints.empty() && r.checkpoints[0].passed, "control: precondition -- the checkpoint itself passes" );
		Check( r.allPassed, "control: final_text run with passing checkpoint -> allPassed true" );
		Check( r.terminalGateNote.empty(), "control: terminalGateNote empty when the gate doesn't fire" );
	}

	// T2 -- guard fires: overwrite the handle's terminal status to a
	// non-success status the scenario never asserts.  The checkpoint set
	// is unchanged (still passes on its own), so this isolates the gate:
	// allPassed must flip to false and terminalGateNote must explain why.
	h.result.terminalStatus = "budget_tool_calls";
	{
		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( !r.checkpoints.empty() && r.checkpoints[0].passed,
			"guard fires: precondition -- the checkpoint itself still passes" );
		Check( !r.allPassed, "guard fires: budget_tool_calls with no asserting checkpoint -> allPassed false" );
		Check( !r.terminalGateNote.empty(), "guard fires: terminalGateNote is non-empty" );
		Check( r.terminalGateNote.find( "budget_tool_calls" ) != std::string::npos,
			"guard fires: terminalGateNote names the actual terminal status (got: " + r.terminalGateNote + ")" );
	}

	// T3 -- opt-in: the SAME budget_tool_calls handle, but the scenario's
	// checkpoints now include a "trajectory" checkpoint asserting
	// terminalStatus == "budget_tool_calls" -- the scenario intentionally
	// expects that terminal, so the gate must NOT fire.  That trajectory
	// checkpoint itself must PASS (it matches the handle's actual status).
	{
		AgentEvalScenario sOptIn = MakeScenario( "guard_optin", kScene, "Recolor", "commit", kParamEditFixture, dir,
			"[{\"kind\":\"document\",\"op\":\"chunk_exists\",\"name\":\"pnt_albedo\"},"
			"{\"kind\":\"trajectory\",\"terminalStatus\":\"budget_tool_calls\"}]" );
		AgentEvalCheckResult r = CheckScenario( h, sOptIn );
		Check( r.checkpoints.size() == 2, "opt-in: two checkpoint results" );
		if( r.checkpoints.size() == 2 ) {
			Check( r.checkpoints[0].passed, "opt-in: the document checkpoint still passes" );
			Check( r.checkpoints[1].passed, "opt-in: the trajectory terminalStatus checkpoint matches and passes" );
		}
		Check( r.allPassed, "opt-in: scenario asserting terminalStatus:\"budget_tool_calls\" -> gate does not fire, allPassed true" );
		Check( r.terminalGateNote.empty(), "opt-in: terminalGateNote empty when the gate doesn't fire" );
	}
}

//----------------------------------------------------------------------
// Wave A: toolOutcomes argsContains filter + expect:"conflict".
//----------------------------------------------------------------------
static void TestToolOutcomesArgsContainsAndConflict()
{
	std::printf( "T7d: toolOutcomes argsContains filter + expect:\"conflict\"...\n" );
	const std::string dir = ScratchRunDir( "t7d_tooloutcomes_ext" );

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
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

	// argsContains FILTER over the both-applied run (two propose_patch calls
	// with DIFFERENT args: one recolors pnt_albedo, one resizes sph.radius).
	{
		AgentEvalScenario s = MakeScenario( "to_args", kScene, "Recolor and resize", "commit",
			kToolOutcomesBothAppliedFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.toolCalls == 2, "to_args: 2 propose_patch calls dispatched" );

		// "radius" appears only in the resize call's args -> selects it -> applied.
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":\"radius\",\"expect\":\"applied\"}]}]",
			true, "argsContains 'radius' selects the resize call, which is applied" );
		// A substring no propose_patch call's args carry -> name matches but the
		// filter excludes every record -> no match -> FAIL.
		checkOne( h, s,
			"[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":\"no_such_arg_xyz\",\"expect\":\"applied\"}]}]",
			false, "argsContains a substring no call carries FAILS (name matches, filter excludes)" );

		// The failure detail must NAME the filter substring.
		{
			JsonValue cps; std::string perr;
			JsonParse( "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"argsContains\":\"no_such_arg_xyz\",\"expect\":\"applied\"}]}]",
				cps, perr );
			AgentEvalScenario s2 = s; s2.checkpoints = cps;
			AgentEvalCheckResult r = CheckScenario( h, s2 );
			const bool named = r.checkpoints.size() == 1 && !r.checkpoints[0].passed &&
				r.checkpoints[0].detail.find( "no_such_arg_xyz" ) != std::string::npos;
			Check( named, "argsContains failure detail names the filter substring (got: " +
				( r.checkpoints.size() == 1 ? r.checkpoints[0].detail : std::string( "<no result>" ) ) + ")" );
		}
	}

	// expect:"conflict" -- hand-built trajectory carrying a result whose
	// status is "conflict" (a baseHeadVersion mismatch), mirroring T7c's
	// staged_probe hand-built pattern.
	{
		const std::string trajPath = dir + "/conflict_probe.trajectory.jsonl";
		Check( WriteFile( trajPath,
			"{\"run_type\":\"tool\",\"name\":\"propose_patch\",\"jsonrpc.response\":"
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"applied\":false,\"status\":\"conflict\","
			"\"rawCode\":0,\"retriable\":true,\"message\":\"baseHeadVersion mismatch\"}}}\n" ),
			"wrote the hand-built conflict-tool-record trajectory file" );

		AgentEvalRunHandle h;
		h.result.scenarioId = "conflict_probe";
		h.result.terminalStatus = "final_text";
		h.trajectoryPath = trajPath;
		h.resultPath = dir + "/conflict_probe.result.jsonl";

		AgentEvalScenario s;
		s.id = "conflict_probe";

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"conflict\"}]}]",
			true, "toolOutcomes: expect:\"conflict\" passes on a hand-built conflict tool response" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"toolOutcomes\":[{\"name\":\"propose_patch\",\"expect\":\"applied\"}]}]",
			false, "toolOutcomes: expect:\"applied\" FAILS on a conflict response" );
	}
}

//----------------------------------------------------------------------
// Wave A: toolCallAfterUserTurn array form + maxUserTurns (+ back-compat
// single object).
//----------------------------------------------------------------------
static void TestToolCallAfterUserTurnArrayForm()
{
	std::printf( "T7e: toolCallAfterUserTurn array form + maxUserTurns...\n" );
	const std::string dir = ScratchRunDir( "t7e_tcaut_array" );

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
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

	// Two user turns: turn 1 only read_document (userCount==1 at that tool),
	// turn 2 propose_patch (userCount==2 at that tool).
	AgentEvalScenario s = MakeScenario( "tcaut_arr", kScene, "First, just read the scene.",
		"commit", kToolCallAfterUserTurnPassFixture, dir, "[]" );
	s.prompts.push_back( std::string( "Now recolor the sphere to red." ) );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.toolCalls == 2, "tcaut_arr: read_document + propose_patch dispatched" );

	// Back-compat: the single-OBJECT form still works through the new code path.
	checkOne( h, s,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":2}}]",
		true, "back-compat single-object form still passes (propose_patch at turn 2)" );

	// Array form, both specs satisfied: read_document at turn 1 (<=1) AND
	// propose_patch at turn 2 (>=2).
	checkOne( h, s,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":["
		"{\"name\":\"read_document\",\"maxUserTurns\":1},"
		"{\"name\":\"propose_patch\",\"minUserTurns\":2}]}]",
		true, "array form: both specs pass" );

	// Array form where the second spec FAILS: propose_patch occurs at turn 2,
	// which VIOLATES maxUserTurns:1 -> the whole checkpoint fails.
	checkOne( h, s,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":["
		"{\"name\":\"read_document\",\"maxUserTurns\":1},"
		"{\"name\":\"propose_patch\",\"maxUserTurns\":1}]}]",
		false, "array form: a call after turn 2 FAILS a maxUserTurns:1 spec" );

	// A lone maxUserTurns:1 spec on propose_patch (which fires at turn 2) fails.
	checkOne( h, s,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"maxUserTurns\":1}}]",
		false, "maxUserTurns:1 FAILS the propose_patch that fires at turn 2" );

	// --- Wave C: toolCallAfterUserTurn `expect` + argsContains ARRAY form.
	// The reject-then-apply fixture makes TWO propose_patch calls in a single
	// user turn: the first (bogus_target_xyz) is REJECTED, the second
	// (pnt_albedo) is APPLIED.
	AgentEvalScenario sr = MakeScenario( "tcaut_expect", kScene, "Recolor the sphere.",
		"commit", kToolOutcomesRejectThenApplyFixture, dir, "[]" );
	AgentEvalRunHandle hr = RunScenario( sr, opts );
	Check( hr.result.toolCalls == 2, "tcaut_expect: two propose_patch calls dispatched" );

	// expect:"applied" filtered to the pnt_albedo call -> that call IS applied -> PASS.
	checkOne( hr, sr,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":\"pnt_albedo\",\"minUserTurns\":1,\"expect\":\"applied\"}}]",
		true, "expect:applied on the pnt_albedo call (which applied) PASSES" );
	// expect:"applied" filtered to the bogus call -> that call was REJECTED,
	// so no matching record satisfies expect -> FAIL (the `expect` discriminates).
	checkOne( hr, sr,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":\"bogus_target_xyz\",\"minUserTurns\":1,\"expect\":\"applied\"}}]",
		false, "expect:applied on the REJECTED bogus call FAILS (outcome discriminates)" );
	// expect:"rejected" on the bogus call -> that call really is rejected -> PASS.
	checkOne( hr, sr,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":\"bogus_target_xyz\",\"minUserTurns\":1,\"expect\":\"rejected\"}}]",
		true, "expect:rejected on the bogus call (which was rejected) PASSES" );
	// argsContains ARRAY: both substrings present in the pnt_albedo call -> PASS.
	checkOne( hr, sr,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":[\"pnt_albedo\",\"color\"],\"minUserTurns\":1}}]",
		true, "argsContains array [pnt_albedo,color] both present in one call PASSES" );
	// argsContains ARRAY where no single call carries BOTH substrings -> FAIL.
	checkOne( hr, sr,
		"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"argsContains\":[\"pnt_albedo\",\"radius\"],\"minUserTurns\":1}}]",
		false, "argsContains array [pnt_albedo,radius] -- no call carries both -> FAIL" );
}

//----------------------------------------------------------------------
// Wave A: document ANY-OF-KIND (target-less) matching.
//----------------------------------------------------------------------
static void TestDocumentAnyOfKind()
{
	std::printf( "T2b: document any-of-kind (target-less) matching...\n" );
	const std::string dir = ScratchRunDir( "t2b_anyofkind" );

	AgentEvalScenario s = MakeScenario( "anyofkind_doc", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );

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

	// param_range any-of-kind over the single pinhole_camera (fov 40.0).
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"chunkKind\":\"pinhole_camera\",\"param\":\"fov\",\"min\":[30],\"max\":[50]}]",
		true, "any-of-kind param_range: fov 40 inside [30,50] on the sole pinhole_camera" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"chunkKind\":\"pinhole_camera\",\"param\":\"fov\",\"min\":[100],\"max\":[200]}]",
		false, "any-of-kind param_range: an impossible fov band FAILS" );

	// param_equals any-of-kind over the TWO uniformcolor_painters (pnt_albedo
	// recolored to 0.9 0.1 0.1, pnt_emit is 1.0 1.0 1.0): the existential
	// passes because ONE of them equals the sought value, fails when none do.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\",\"param\":\"color\",\"value\":\"1.0 1.0 1.0\"}]",
		true, "any-of-kind param_equals: pnt_emit matches 1.0 1.0 1.0" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\",\"param\":\"color\",\"value\":\"0.3 0.3 0.3\"}]",
		false, "any-of-kind param_equals: no uniformcolor_painter equals 0.3 0.3 0.3 -> FAIL" );

	// An empty-of-that-kind document: any-of-kind over a kind with zero chunks fails cleanly.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_range\",\"chunkKind\":\"no_such_kind_xyz\",\"param\":\"fov\",\"min\":[0],\"max\":[1]}]",
		false, "any-of-kind over a kind with zero chunks FAILS cleanly" );

	// --- Wave C: the any-of-kind "where" co-binding filter --------------
	// Two uniformcolor_painters: pnt_albedo (recolored red 0.9 0.1 0.1) and
	// pnt_emit (1.0 1.0 1.0).  Without a filter, param_equals color
	// "1.0 1.0 1.0" passes (pnt_emit matches).  A where EQUALITY filter on
	// name restricts the candidate set to the ONE named painter, so the
	// same colour assertion now grades only THAT chunk.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\","
		"\"where\":[{\"param\":\"name\",\"value\":\"pnt_emit\"}],\"param\":\"color\",\"value\":\"1.0 1.0 1.0\"}]",
		true, "where name==pnt_emit narrows to pnt_emit, whose color is 1.0 1.0 1.0 -> PASS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\","
		"\"where\":[{\"param\":\"name\",\"value\":\"pnt_albedo\"}],\"param\":\"color\",\"value\":\"1.0 1.0 1.0\"}]",
		false, "where name==pnt_albedo narrows to the RED painter, so color 1.0 1.0 1.0 -> FAIL" );
	// A where RANGE filter (reddish colour band) selects pnt_albedo; the
	// assertion then reads a DIFFERENT param (name) on the survivor.
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\","
		"\"where\":[{\"param\":\"color\",\"min\":[0.5,0,0],\"max\":[1.0,0.45,0.45]}],\"param\":\"name\",\"value\":\"pnt_albedo\"}]",
		true, "where color-in-reddish-band selects pnt_albedo, whose name is pnt_albedo -> PASS" );
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\","
		"\"where\":[{\"param\":\"color\",\"min\":[0.5,0,0],\"max\":[1.0,0.45,0.45]}],\"param\":\"name\",\"value\":\"pnt_emit\"}]",
		false, "where color-in-reddish-band excludes the white pnt_emit -> name!=pnt_emit -> FAIL" );
	// A where filter that NO chunk satisfies -> fails cleanly (the detail
	// names the where filter, not a per-chunk assertion miss).
	checkOne( "[{\"kind\":\"document\",\"op\":\"param_equals\",\"chunkKind\":\"uniformcolor_painter\","
		"\"where\":[{\"param\":\"name\",\"value\":\"no_such_painter\"}],\"param\":\"color\",\"value\":\"1.0 1.0 1.0\"}]",
		false, "where matching zero chunks FAILS cleanly" );
}

//----------------------------------------------------------------------
// Wave C: the "param_series_orbit" document op -- a single timeline's
// keyframe track must PROVE a camera orbit (>= K keyframes, an angular
// spread threshold, a radius ratio bound).  Each sub-scene carries ONE
// timeline (element_type camera / param location) so the any-of-kind op
// grades it without a where filter; a final where-interplay case rides
// the orbit scene.
//----------------------------------------------------------------------
static void TestParamSeriesOrbit()
{
	std::printf( "T2c: document param_series_orbit (camera-orbit proof)...\n" );
	const std::string dir = ScratchRunDir( "t2c_orbit" );

	auto sceneWithTimeline = []( const std::string& timelineChunk ) -> std::string {
		return std::string(
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
			"film\n{\n\twidth 16\n\theight 16\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n" ) + timelineChunk;
	};

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
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

	// As checkOne, but also asserts the resulting detail string CONTAINS
	// `detailSubstr` -- used by the time-axis cases below to pin the
	// specific failure detail (count mismatch vs malformed vs
	// non-monotonic) rather than just the pass/fail bit.
	auto checkDetail = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                         bool expectPass, const std::string& detailSubstr, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
			Check( r.checkpoints[0].detail.find( detailSubstr ) != std::string::npos,
				label + ": detail contains '" + detailSubstr + "' (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	// --- Orbit track (mirrors the committed camera_orbit fixture): 5
	// keyframes, 4 distinct at 90-degree spacing, radius ~19.03 from the
	// origin, XZ centroid ~(0.9, 3.7); the max-gap formula gives spread 270.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n"
			"\ttime\t\t0.25\n\tvalue\t\t-18.5 4.8 4.5\n"
			"\ttime\t\t0.5\n\tvalue\t\t-4.5 4.8 -18.5\n"
			"\ttime\t\t0.75\n\tvalue\t\t18.5 4.8 -4.5\n"
			"\ttime\t\t1.0\n\tvalue\t\t4.5 4.8 18.5\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_ok", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_ok" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );

		// The committed scenario's exact checkpoint -> PASS.  (e) The pass
		// detail now also mentions the time span 0..1 (the checkpoint's
		// implicit default `timeParam` "time" paired against the 5 strictly-
		// increasing 0.0/0.25/0.5/0.75/1.0 keyframe times).
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"where\":[{\"param\":\"element_type\",\"value\":\"camera\"},{\"param\":\"param\",\"value\":\"location\"}],"
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240,"
			"\"centerWithin\":{\"x\":0.0,\"z\":0.0,\"radius\":6.0},\"maxRadiusRatio\":2.0}]",
			true, "time span", "orbit: 5kf / spread 270 / centroid within 6 / ratio 1 -> PASS, detail mentions time span" );
		// A too-strict spread threshold (300 > the measured 270) -> FAIL.
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":300,\"centerWithin\":{\"x\":0.0,\"z\":0.0,\"radius\":6.0}}]",
			false, "orbit: minAngularSpreadDeg 300 > measured 270 -> FAIL" );
		// A too-high minKeyframes (6 > the 5 distinct-after-dedupe) -> FAIL.
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":6,\"minAngularSpreadDeg\":240}]",
			false, "orbit: minKeyframes 6 > 5 keyframes -> FAIL" );
		// where interplay: a filter no timeline satisfies -> FAIL (the sole
		// timeline is element_type camera, not object).
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"where\":[{\"param\":\"element_type\",\"value\":\"object\"}],"
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			false, "orbit: where element_type==object matches no timeline -> FAIL" );
	}

	// --- Degenerate CONSTANT series: every keyframe is the same point ->
	// dedupe collapses to ONE keyframe, below any minKeyframes -> FAIL.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t0.0\n\tvalue\t\t2 3 2\n"
			"\ttime\t\t0.5\n\tvalue\t\t2 3 2\n"
			"\ttime\t\t1.0\n\tvalue\t\t2 3 2\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_constant", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_constant" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":2,\"minAngularSpreadDeg\":90}]",
			false, "constant camera: 1 keyframe after dedupe < minKeyframes 2 -> FAIL" );
	}

	// --- Radius-ratio track: 4 keyframes at 0/90/180/270 degrees (spread
	// 270) but radii 5,5,5,20 about the origin (ratio 4).  Passes without a
	// cap; a maxRadiusRatio:2 rejects the non-circular sweep.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t0.0\n\tvalue\t\t5 4 0\n"
			"\ttime\t\t0.33\n\tvalue\t\t0 4 5\n"
			"\ttime\t\t0.66\n\tvalue\t\t-5 4 0\n"
			"\ttime\t\t1.0\n\tvalue\t\t0 4 -20\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_ratio", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_ratio" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240,\"centerWithin\":{\"x\":0.0,\"z\":0.0,\"radius\":6.0}}]",
			true, "ratio track: spread 270 / centroid within 6 (no ratio cap) -> PASS" );
		checkOne( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240,\"centerWithin\":{\"x\":0.0,\"z\":0.0,\"radius\":6.0},\"maxRadiusRatio\":2.0}]",
			false, "ratio track: radius ratio 4 > maxRadiusRatio 2 -> FAIL" );
	}

	// --- Time-axis cases: an orbit-SHAPED value track (same 5 keyframes as
	// orbit_ok) that fails on the TIME axis rather than the geometry axis.
	// Time progression is intrinsic to the op -- always enforced.

	// (a) All times equal: geometry is a perfect 270-degree, 5-keyframe
	// orbit, but every `time` line reads 0.0 -- the runtime timeline range
	// is degenerate.  Must FAIL with the monotonicity detail.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n"
			"\ttime\t\t0.0\n\tvalue\t\t-18.5 4.8 4.5\n"
			"\ttime\t\t0.0\n\tvalue\t\t-4.5 4.8 -18.5\n"
			"\ttime\t\t0.0\n\tvalue\t\t18.5 4.8 -4.5\n"
			"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_time_allequal", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_time_allequal" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			false, "not strictly increasing", "orbit-shaped but all times 0.0 -> FAIL (monotonicity)" );
	}

	// (b) Times omitted entirely: the same 5 orbit-shaped `value` keyframes,
	// but no `time` lines at all (the parser defaults every missing time to
	// 0.0 -- geometry alone would still pass).  Must FAIL with the raw
	// count-mismatch detail (5 values, 0 times).
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\tvalue\t\t4.5 4.8 18.5\n"
			"\tvalue\t\t-18.5 4.8 4.5\n"
			"\tvalue\t\t-4.5 4.8 -18.5\n"
			"\tvalue\t\t18.5 4.8 -4.5\n"
			"\tvalue\t\t4.5 4.8 18.5\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_time_omitted", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_time_omitted" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			false, "5 'value' value(s) but 0 'time' entrie(s)", "orbit-shaped but times omitted entirely -> FAIL (count mismatch)" );
	}

	// (c) Times decreasing: same geometry, times run 1.0 -> 0.0.  Must FAIL
	// with the monotonicity detail (first offending pair reported).
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t1.0\n\tvalue\t\t4.5 4.8 18.5\n"
			"\ttime\t\t0.75\n\tvalue\t\t-18.5 4.8 4.5\n"
			"\ttime\t\t0.5\n\tvalue\t\t-4.5 4.8 -18.5\n"
			"\ttime\t\t0.25\n\tvalue\t\t18.5 4.8 -4.5\n"
			"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_time_decreasing", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_time_decreasing" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			false, "not strictly increasing", "orbit-shaped but times decreasing -> FAIL (monotonicity)" );
	}

	// (d) Times count != values count: one `time` line missing (4 times for
	// 5 values).  Must FAIL with the raw count-mismatch detail.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n"
			"\ttime\t\t0.25\n\tvalue\t\t-18.5 4.8 4.5\n"
			"\ttime\t\t0.5\n\tvalue\t\t-4.5 4.8 -18.5\n"
			"\ttime\t\t0.75\n\tvalue\t\t18.5 4.8 -4.5\n"
			"\tvalue\t\t4.5 4.8 18.5\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_time_undercount", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_time_undercount" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			false, "5 'value' value(s) but 4 'time' entrie(s)", "orbit-shaped but one time missing (4 vs 5) -> FAIL (count mismatch)" );
	}

	// (f) A custom `timeParam` name: the timeline uses `interpolator_params`
	// (a legal repeatable string field on the `timeline` chunk descriptor,
	// unused here for its usual spline-tangent purpose) as the stand-in
	// time series, paired 1:1 and strictly increasing; the checkpoint names
	// it via `"timeParam":"interpolator_params"`.  Must PASS.
	{
		const std::string scene = sceneWithTimeline(
			"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
			"\tvalue\t\t4.5 4.8 18.5\n\tinterpolator_params\t0.0\n"
			"\tvalue\t\t-18.5 4.8 4.5\n\tinterpolator_params\t0.25\n"
			"\tvalue\t\t-4.5 4.8 -18.5\n\tinterpolator_params\t0.5\n"
			"\tvalue\t\t18.5 4.8 -4.5\n\tinterpolator_params\t0.75\n"
			"\tvalue\t\t4.5 4.8 18.5\n\tinterpolator_params\t1.0\n}" );
		AgentEvalScenario s = MakeScenario( "orbit_customtimeparam", scene, "inspect", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error", std::string( "orbit_customtimeparam" ) + ": scene+timeline loads (" + h.result.errorMessage + ")" );
		checkDetail( h, s, "[{\"kind\":\"document\",\"op\":\"param_series_orbit\",\"chunkKind\":\"timeline\","
			"\"param\":\"value\",\"timeParam\":\"interpolator_params\",\"minKeyframes\":4,\"minAngularSpreadDeg\":240}]",
			true, "time span", "orbit-shaped, custom timeParam 'interpolator_params' strictly increasing -> PASS" );
	}
}

//----------------------------------------------------------------------
// Wave A: the "proposal" checkpoint kind.
//----------------------------------------------------------------------
static void TestProposalCheckpoint()
{
	std::printf( "T11: \"proposal\" checkpoint kind...\n" );
	const std::string dir = ScratchRunDir( "t11_proposal" );

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& s, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
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

	// Real staging: the committed autonomy:"propose" scenario attaches a
	// headless mock-Owner controller, so propose_patch STAGES a pending edit.
	{
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/propose_mode_stages.json", s, err ),
			"propose_mode_stages loads (" + err + ")" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );

		checkOne( h, s,
			"[{\"kind\":\"proposal\",\"countMin\":1,\"match\":{\"target\":\"pnt_albedo\",\"param\":\"color\",\"status\":\"pending\"}}]",
			true, "proposal countMin:1 + matching pnt_albedo/color/pending PASSES" );
		// A component-wise value band over the staged "0.1 0.1 0.9" (blue).
		checkOne( h, s,
			"[{\"kind\":\"proposal\",\"match\":{\"target\":\"pnt_albedo\",\"valueMin\":[0,0,0.5],\"valueMax\":[0.5,0.5,1.0]}}]",
			true, "proposal match valueMin/valueMax band over the staged blue PASSES" );
		// A red band excludes the staged blue -> no entry matches.
		checkOne( h, s,
			"[{\"kind\":\"proposal\",\"match\":{\"target\":\"pnt_albedo\",\"valueMin\":[0.5,0,0],\"valueMax\":[1.0,0.5,0.5]}}]",
			false, "proposal match a RED value band FAILS on the staged blue" );
		// Wrong target -> no entry matches.
		checkOne( h, s,
			"[{\"kind\":\"proposal\",\"countMin\":1,\"match\":{\"target\":\"does_not_exist\",\"param\":\"color\",\"status\":\"pending\"}}]",
			false, "proposal match on a wrong target FAILS" );
		// countMax below the one staged entry -> fails.
		checkOne( h, s, "[{\"kind\":\"proposal\",\"countMax\":0}]",
			false, "proposal countMax:0 FAILS (one entry staged)" );
	}

	// No controller: a plain commit run has an empty proposal queue, so a
	// countMin:1 checkpoint fails CLEANLY (the session is alive, just empty).
	{
		AgentEvalScenario s = MakeScenario( "prop_none", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		checkOne( h, s, "[{\"kind\":\"proposal\",\"countMin\":1}]",
			false, "proposal countMin:1 FAILS cleanly on a commit run with no staged proposals" );
	}
}

//----------------------------------------------------------------------
// Wave A: the render "channelBalanceMax" (neutral-render) assertion.
//----------------------------------------------------------------------
static void TestRenderChannelBalance()
{
	std::printf( "T4b: render channelBalanceMax...\n" );
	const std::string dir = ScratchRunDir( "t4b_channelbalance" );

	AgentEvalScenario s = MakeScenario( "cbal_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "cbal_probe run reached final_text" );

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

	// The scene is a grey sphere under a white light -> roughly NEUTRAL, so a
	// generous ratio cap passes.
	checkOne( "[{\"kind\":\"render\",\"channelBalanceMax\":5.0}]", true,
		"a generous channelBalanceMax (5.0) passes on the neutral scene" );
	// A near-unity cap the MC-noisy per-channel means always exceed -> fails
	// (exact channel equality is measure-zero under independent MC noise).
	checkOne( "[{\"kind\":\"render\",\"channelBalanceMax\":1.0000001}]", false,
		"a near-unity channelBalanceMax fails (means differ by MC noise)" );
}

//----------------------------------------------------------------------
// T4c: render "camera" override + "compareToImage"/"rmseMax" (image-
// reconstruction Wave 2).  Camera: a render taken from a FAR pose frames
// the sphere+emitter tiny -> mean luma DROPS below the near pose, and a
// luma band midway between the two distinguishes them -> proves the
// checkpoint's grading render actually honours the override.
// compareToImage: a self-render written to a scratch file compares to a
// FRESH render of the same scene (MC noise only) within a loose rmseMax
// but not a sub-floor one; a different-framing reference fails; a dims
// mismatch and a missing file each fail with a clear detail.  Plus the
// load-validator reject cases.
//----------------------------------------------------------------------

// Write raw bytes to a file (the reference PNGs the compareToImage cases
// decode) -- returns false on any I/O failure.
static bool WriteBytes( const std::string& path, const std::vector<unsigned char>& bytes )
{
	std::error_code ec;
	std::filesystem::path p( path );
	if( p.has_parent_path() ) std::filesystem::create_directories( p.parent_path(), ec );
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return false;
	if( !bytes.empty() ) f.write( reinterpret_cast<const char*>( &bytes[0] ), static_cast<std::streamsize>( bytes.size() ) );
	return true;
}

// P1 fix (2026-07-19 mutation review) test support: build a GENUINELY VALID
// PNG (correct IHDR, correctly zlib-deflated IDAT, correct per-chunk CRC32)
// whose declared dimensions exceed the eval pixel budget -- WITHOUT ever
// holding the full uncompressed image in memory.  A solid-colour image
// deflates to a few KB regardless of dimensions, so a single scanline
// buffer (filter-type byte + width*3 solid-colour bytes) is fed through
// zlib's streaming deflate() one row at a time; only the tiny compressed
// output accumulates.  This is deliberately NOT the prior attempt's
// approach (a hand-patched IHDR lying about dimensions on top of otherwise
// truncated/inconsistent image data) -- that aborts inside libpng's own
// "not enough image data" error path before DecodePngToRgb8's guard is
// ever reached, so it exercises libpng's tolerance of a lying header, not
// the guard under test.  A real 8200x8200 solid PNG here is a fully valid,
// standard-conforming file that libpng will happily decode top to bottom
// if given the chance -- which is exactly the point: it lets the test
// prove the header-probe guard stops that decode from ever starting.
static void AppendPngChunk( std::vector<unsigned char>& out, const char type[4], const unsigned char* data, unsigned int len )
{
	const unsigned char lenBuf[4] = {
		static_cast<unsigned char>( ( len >> 24 ) & 0xFF ), static_cast<unsigned char>( ( len >> 16 ) & 0xFF ),
		static_cast<unsigned char>( ( len >> 8 ) & 0xFF ),  static_cast<unsigned char>( len & 0xFF ) };
	out.insert( out.end(), lenBuf, lenBuf + 4 );
	const std::size_t typeStart = out.size();
	out.insert( out.end(), type, type + 4 );
	if( len ) out.insert( out.end(), data, data + len );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc = crc32( crc, &out[typeStart], static_cast<uInt>( 4 + len ) );
	const unsigned char crcBuf[4] = {
		static_cast<unsigned char>( ( crc >> 24 ) & 0xFF ), static_cast<unsigned char>( ( crc >> 16 ) & 0xFF ),
		static_cast<unsigned char>( ( crc >> 8 ) & 0xFF ),  static_cast<unsigned char>( crc & 0xFF ) };
	out.insert( out.end(), crcBuf, crcBuf + 4 );
}

static bool WriteSyntheticSolidPng( const std::string& path, unsigned int width, unsigned int height )
{
	std::vector<unsigned char> file;
	static const unsigned char kSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	file.insert( file.end(), kSig, kSig + 8 );

	// IHDR: width, height (big-endian uint32 each), bit depth 8, color type
	// 2 (RGB truecolor, no alpha), compression/filter/interlace all 0.
	unsigned char ihdr[13];
	ihdr[0] = static_cast<unsigned char>( ( width >> 24 ) & 0xFF );  ihdr[1] = static_cast<unsigned char>( ( width >> 16 ) & 0xFF );
	ihdr[2] = static_cast<unsigned char>( ( width >> 8 ) & 0xFF );   ihdr[3] = static_cast<unsigned char>( width & 0xFF );
	ihdr[4] = static_cast<unsigned char>( ( height >> 24 ) & 0xFF ); ihdr[5] = static_cast<unsigned char>( ( height >> 16 ) & 0xFF );
	ihdr[6] = static_cast<unsigned char>( ( height >> 8 ) & 0xFF );  ihdr[7] = static_cast<unsigned char>( height & 0xFF );
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	AppendPngChunk( file, "IHDR", ihdr, 13 );

	// IDAT: stream-deflate one scanline at a time -- the only buffer that's
	// ever `width`-sized is the single reused row; `compressed` holds only
	// the (tiny, solid-colour) deflate output.
	std::vector<unsigned char> compressed;
	z_stream strm;
	std::memset( &strm, 0, sizeof( strm ) );
	Check( deflateInit( &strm, Z_DEFAULT_COMPRESSION ) == Z_OK, "WriteSyntheticSolidPng: deflateInit succeeds" );

	std::vector<unsigned char> row( 1 + static_cast<std::size_t>( width ) * 3 );
	row[0] = 0;   // filter type: None
	for( std::size_t i = 1; i < row.size(); i += 3 ) {
		row[i] = 200; row[i + 1] = 120; row[i + 2] = 40;   // solid colour
	}

	std::vector<unsigned char> outBuf( 1 << 16 );
	for( unsigned int y = 0; y < height; ++y ) {
		strm.next_in  = row.data();
		strm.avail_in = static_cast<uInt>( row.size() );
		const int flush = ( y + 1 == height ) ? Z_FINISH : Z_NO_FLUSH;
		int ret;
		do {
			strm.next_out  = outBuf.data();
			strm.avail_out = static_cast<uInt>( outBuf.size() );
			ret = deflate( &strm, flush );
			if( ret == Z_STREAM_ERROR ) { deflateEnd( &strm ); return false; }
			const std::size_t produced = outBuf.size() - strm.avail_out;
			if( produced ) compressed.insert( compressed.end(), outBuf.data(), outBuf.data() + produced );
		} while( strm.avail_out == 0 );
	}
	deflateEnd( &strm );

	AppendPngChunk( file, "IDAT", compressed.empty() ? nullptr : compressed.data(), static_cast<unsigned int>( compressed.size() ) );
	AppendPngChunk( file, "IEND", nullptr, 0 );

	// ScratchRunDir() REMOVES its directory and does not recreate it (the
	// WriteFile/WriteBytes helpers each create parents themselves), so a raw
	// ofstream here would fail with "cannot open file" and the fixture would
	// silently never exist -- which is exactly what happened before this
	// line: the budget assertion then "failed" against a missing-file error
	// instead of the pixel-budget refusal it meant to test.
	{
		std::error_code ec;
		const std::filesystem::path fp( path );
		if( fp.has_parent_path() ) std::filesystem::create_directories( fp.parent_path(), ec );
	}
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return false;
	f.write( reinterpret_cast<const char*>( file.data() ), static_cast<std::streamsize>( file.size() ) );
	return f.good();
}

// P1 fix (2026-07-19 mutation review): the reference-PNG budget guard used
// to run AFTER PNGReader::BeginRead -- which allocates the full pixel
// buffer AND decodes every scanline via png_read_row BEFORE returning w/h
// (see PNGReader.cpp:66-172).  An oversized-but-under-libpng's-own-2GiB-cap
// reference therefore fully decoded before being refused.  DecodePngToRgb8
// now probes width/height straight out of the PNG signature+IHDR bytes
// (ProbePngDimensions, AgentEvalRunner.cpp) and refuses BEFORE constructing
// any reader at all.
//
// This test builds a REAL 8200x8200 (67,240,000 px, just over the
// 64,000,000-px budget) solid-colour PNG via WriteSyntheticSolidPng above,
// then measures wall-clock time for compareToImage to refuse it.  The
// header probe is a handful of memory compares on 24 bytes; a full decode
// of an 8200x8200 image (memset + zlib inflate + png_read_row x8200 rows)
// is not.  kSlowRefuseCeilingMs is picked well below what a full decode of
// this fixture measurably costs (calibrated during development: a full
// decode-then-refuse of this exact fixture takes tens of milliseconds; the
// header-probe refusal is sub-millisecond) but comfortably above ordinary
// scratch-disk I/O jitter for a several-KB file, so the assertion is not a
// hardware-speed guess.  Proven to FAIL pre-fix: temporarily reverting the
// ProbePngDimensions call in DecodePngToRgb8 (leaving only the pre-existing
// post-BeginRead belt-and-braces gate) makes this same refusal take an
// order of magnitude longer -- the timing assertion below trips.
static void TestCompareToImageOversizedReferenceIsRefusedCheaply()
{
	std::printf( "P1: compareToImage reference over the pixel budget is refused BEFORE decode (cheaply)...\n" );
	const std::string dir = ScratchRunDir( "p1_oversized_reference" );

	const unsigned int kWidth = 8200, kHeight = 8200;   // 67,240,000 px > 64,000,000 budget
	const std::string bigRefPath = dir + "/oversized_ref.png";
	Check( WriteSyntheticSolidPng( bigRefPath, kWidth, kHeight ),
		"WriteSyntheticSolidPng writes a genuinely valid oversized PNG fixture" );

	// Sanity: the fixture really is a valid, libpng-decodable PNG (not a
	// truncated/lying-header fixture) -- confirmed structurally by the PNG
	// signature + IHDR bytes we just wrote it with; the file-size check
	// below additionally confirms it stayed a small, real deflate output
	// not an accidentally-truncated write.  MEASURED size: ~211 KB.  (An
	// earlier guess of "well under 100 KB" was wrong -- each of the 8200
	// rows is a 24601-byte repeating pattern, and deflate's per-block
	// overhead across that many rows adds up.  The claim that matters is
	// the RATIO, not an absolute byte count: 211 KB is ~0.1% of the
	// ~192 MiB an uncompressed decode would need, which is what proves the
	// header probe refuses without materialising the image.)
	{
		std::ifstream check( bigRefPath.c_str(), std::ios::binary | std::ios::ate );
		Check( check.is_open(), "the oversized fixture file was written" );
		const auto sz = check.tellg();
		Check( sz > 100 && sz < 2 * 1024 * 1024,
			"the fixture is a small, real deflate stream (measured ~211 KB, bounded at 2 MB), not the ~192 MiB "
			"an uncompressed decode would need" );
	}

	AgentEvalScenario s = MakeScenario( "p1_oversized_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "p1_oversized_probe run reached final_text" );
	Check( h.dispatcher != nullptr && h.dispatcher->Session() != nullptr, "p1_oversized_probe has a live session" );
	if( !h.dispatcher || !h.dispatcher->Session() ) return;

	JsonValue cps; std::string jerr;
	const std::string cpJson = "[{\"kind\":\"render\",\"compareToImage\":\"" + bigRefPath + "\",\"rmseMax\":0.5}]";
	Check( JsonParse( cpJson, cps, jerr ), "oversized-reference checkpoint JSON parses (" + jerr + ")" );
	AgentEvalScenario s2 = s; s2.checkpoints = cps;

	const auto t0 = std::chrono::steady_clock::now();
	AgentEvalCheckResult r = CheckScenario( h, s2 );
	const auto t1 = std::chrono::steady_clock::now();
	const double elapsedMs = std::chrono::duration<double, std::milli>( t1 - t0 ).count();
	std::printf( "  [measured] oversized-reference refusal took %.3f ms\n", elapsedMs );

	Check( r.checkpoints.size() == 1, "exactly one checkpoint result" );
	if( r.checkpoints.size() == 1 ) {
		Check( r.checkpoints[0].passed == false, "the oversized-reference checkpoint FAILS (refused, not silently skipped)" );
		Check( r.checkpoints[0].detail.find( "exceeds the 64000000-pixel eval render budget" ) != std::string::npos,
			"the refusal detail names the pixel budget (got: " + r.checkpoints[0].detail + ")" );
		Check( r.checkpoints[0].detail.find( "67240000" ) != std::string::npos,
			"the refusal detail names the oversized reference's actual pixel count (got: " + r.checkpoints[0].detail + ")" );
	}

	// The load-bearing timing assertion: refusal via the header probe is
	// fast.  Generous ceiling (development measurement: header-probe path
	// ~0.1-1 ms; a full BeginRead decode-then-refuse of this exact fixture
	// measured tens of ms) -- this is a coarse but real proxy for "the
	// pixel-buffer allocation + scanline decode never ran", which is the
	// actual P1 claim under test.
	const double kSlowRefuseCeilingMs = 25.0;
	Check( elapsedMs < kSlowRefuseCeilingMs,
		"the refusal is fast (" + std::to_string( elapsedMs ) + " ms < " + std::to_string( kSlowRefuseCeilingMs ) +
		" ms ceiling) -- consistent with a 24-byte header probe, NOT a full BeginRead decode of a ~192 MiB image" );
}

//----------------------------------------------------------------------
// T4e: the width/height PIN is not silently defeatable (the twin of T4d).
//
// `width`/`height` on a render checkpoint are a HARD PIN for the same
// reason `samples` is: the scene's `film` chunk is under the model's
// control, and the mean-channel bands are resolution-sensitive.  But
// AgentSession's `applyFilmOverride` DISCARDS `IJob::SetFilm`'s bool
// return, and SetFilm honestly REFUSES dims beyond Job's own
// kMaxFilm{Width,Height} sanity bound (32768) -- the render then
// silently proceeds at the scene-authored film size (kScene: 24x24).
//
// REACHING A REAL DROP.  The checkpoint's own explicit-dims validator
// caps width/height at 32768, which is exactly kMaxFilmWidth, so the
// explicit-dims path cannot itself drive SetFilm past the bound.  The
// compareToImage path can and does: it ADOPTS the reference PNG's dims,
// and a reference wider than 32768 (here a genuinely valid 40000x1 solid
// PNG -- only 40,000 px, comfortably inside both the 64,000,000-px eval
// render budget and DecodePngToRgb8's own header-probe guard, so it is
// accepted and adopted) makes SetFilm(40000, 1, ...) return false for
// real.  This is a REAL render coming back at dims other than the ones
// requested, through the ordinary path -- not a synthesized result.
//
// RED-PROVE: pre-fix, CheckRenderKind contained ZERO references to
// rr.width/rr.height.  The drop was noticed only INDIRECTLY, and only on
// this one path, by the compare block's own decoded-candidate-vs-
// reference size check ("compareToImage dimension mismatch"), which
// reports the symptom at the comparison layer and says nothing about the
// pin having been dropped -- and on an explicit-dims checkpoint with no
// compareToImage there was no notice at all.  Assertion (b)'s
// detail-contains therefore FAILS without the fix (verified by deleting
// the guard: the detail reads "render band(s) violated: compareToImage
// dimension mismatch: render is 24x24 but reference ... is 40000x1").
//
// Assertions (a), (c) and (d) are the anti-spurious-fire controls, and
// (a) is specifically the regression guard for image_reconstruct_single /
// image_reconstruct_multi, whose grading render runs on exactly this
// reference-ADOPTED-dims path.
//----------------------------------------------------------------------
static void TestRenderDimsPinNotDefeatable()
{
	std::printf( "T4e: \"render\" checkpoint -- the width/height pin is not silently defeatable...\n" );
	const std::string dir = ScratchRunDir( "t4e_dims_pin" );

	AgentEvalScenario s = MakeScenario( "dims_pin_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "dims_pin_probe run reached final_text" );

	auto checkOne = [&]( const std::string& cpJson, bool expectPass,
	                     const std::string& mustContain, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() != 1 ) { Check( false, label + ": expected exactly one checkpoint result" ); return; }
		Check( r.checkpoints[0].passed == expectPass,
			label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
			" (detail: " + r.checkpoints[0].detail + ")" );
		if( !mustContain.empty() ) {
			Check( r.checkpoints[0].detail.find( mustContain ) != std::string::npos,
				label + ": detail names the situation (detail: " + r.checkpoints[0].detail + ")" );
		}
	};

	// (a) CONTROL -- the reference-ADOPTED-dims path, HONOURED.  A small
	// (17x13, deliberately neither square nor equal to kScene's authored
	// 24x24) reference is adopted, SetFilm accepts it, and the render
	// really does come back at 17x13 -> the guard stays silent and the
	// checkpoint passes.  rmseMax 1.0 is the permissive ceiling: this
	// assertion is about DIMS, not about image content (the synthetic
	// solid-orange reference looks nothing like the scene).  This is the
	// image_reconstruct_* regression guard -- those scenarios grade on
	// exactly this path.
	const std::string okRefPath = dir + "/adopted_dims_ref.png";
	Check( WriteSyntheticSolidPng( okRefPath, 17, 13 ),
		"T4e fixture: a valid 17x13 reference PNG is written" );
	checkOne( "[{\"kind\":\"render\",\"compareToImage\":\"" + okRefPath + "\",\"rmseMax\":1.0}]",
		true, "", "T4e(a) an ADOPTED-dims render that IS honoured passes (the guard does not spuriously fire)" );

	// (b) RED-PROVE -- the reference-ADOPTED-dims path, DROPPED.  40000 >
	// kMaxFilmWidth (32768), so SetFilm genuinely refuses and the render
	// falls back to kScene's authored 24x24.  The failure must name the
	// DROPPED PIN and both dimension pairs, not merely the downstream
	// comparison symptom.
	//
	// Run in THREE variants so each disjunct of the guard's
	// `rr.width != rp.width || rr.height != rp.height` is proven
	// INDEPENDENTLY (kScene's authored film is 24x24, so a reference that
	// shares one axis with it isolates the other):
	//   40000x1     -- BOTH axes differ (the headline case)
	//   40000x24    -- only the WIDTH differs  -> isolates the width disjunct
	//   24x40000    -- only the HEIGHT differs -> isolates the height disjunct
	// Without the two axis-isolating variants, either disjunct could be
	// deleted with the suite still green -- the other would satisfy the
	// test first.
	auto redProve = [&]( unsigned int refW, unsigned int refH, const std::string& tag ) {
		char dimsBuf[64];
		std::snprintf( dimsBuf, sizeof( dimsBuf ), "%ux%u", refW, refH );
		const std::string wideRefPath = dir + "/too_wide_ref_" + tag + ".png";
		Check( WriteSyntheticSolidPng( wideRefPath, refW, refH ),
			"T4e fixture: a valid " + std::string( dimsBuf ) + " reference PNG is written "
			"(inside the 64,000,000-px budget, past Job's 32768-px film bound on one axis)" );
		JsonValue cps; std::string err;
		const std::string cpJson =
			"[{\"kind\":\"render\",\"compareToImage\":\"" + wideRefPath + "\",\"rmseMax\":1.0}]";
		Check( JsonParse( cpJson, cps, err ), "T4e(b/" + tag + "): checkpoint JSON parses" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		Check( r.checkpoints.size() == 1, "T4e(b/" + tag + "): exactly one checkpoint result" );
		if( r.checkpoints.size() != 1 ) return;
		const std::string& d = r.checkpoints[0].detail;
		Check( !r.checkpoints[0].passed,
			"T4e(b/" + tag + ") a render whose dims pin was DROPPED must FAIL (detail: " + d + ")" );
		Check( d.find( "width/height pin was NOT APPLIED" ) != std::string::npos,
			"T4e(b/" + tag + ") the detail names the DROPPED PIN, not just the downstream comparison symptom "
			"(detail: " + d + ")" );
		// "requested "/"actually produced " prefixes deliberately: the
		// PRE-FIX comparison-layer message also contains the bare dimension
		// tokens ("render is 24x24 but reference ... is 40000x1"), so an
		// unprefixed find() here would pass for the wrong reason.
		Check( d.find( "requested " + std::string( dimsBuf ) ) != std::string::npos,
			"T4e(b/" + tag + ") the detail names the REQUESTED dims (detail: " + d + ")" );
		Check( d.find( "actually produced 24x24" ) != std::string::npos,
			"T4e(b/" + tag + ") the detail names the dims the render ACTUALLY produced (detail: " + d + ")" );
	};
	redProve( 40000, 1,  "both" );    // both axes differ from the 24x24 fallback
	redProve( 40000, 24, "width" );   // height matches the fallback -> only the width disjunct can fire
	redProve( 24, 40000, "height" );  // width matches the fallback  -> only the height disjunct can fire

	// (c) CONTROL -- NO dims requested at all.  rp.width/rp.height stay 0,
	// the render legitimately uses the scene's own film size, and the guard
	// must stay silent.  Without the `!= 0` predicate this passes-by-design
	// case would fail on every unpinned render checkpoint in the suite.
	checkOne( "[{\"kind\":\"render\",\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "", "T4e(c) an UNPINNED render checkpoint does not trip the dims guard" );

	// (d) CONTROL -- the EXPLICIT-dims path, honoured.  16x16 is inside
	// SetFilm's bound, so the override really applies and the guard stays
	// silent.  (The explicit path cannot itself drive SetFilm past the
	// bound -- the checkpoint validator's own [1,32768] cap is exactly
	// kMaxFilmWidth -- so this is the explicit path's positive control;
	// (b) is what proves the guard's comparison is against the dims
	// ACTUALLY REQUESTED (`rp`) rather than the checkpoint's literal
	// fields, which (b) does not even supply.)
	checkOne( "[{\"kind\":\"render\",\"width\":16,\"height\":16,\"meanLumaMin\":0.0,\"meanLumaMax\":5.0}]",
		true, "", "T4e(d) an EXPLICIT dims pin that IS honoured passes" );
}

static void TestRenderCameraCompareToImage()
{
	std::printf( "T4c: render camera override + compareToImage/rmseMax...\n" );
	const std::string dir = ScratchRunDir( "t4c_camera_compare" );

	AgentEvalScenario s = MakeScenario( "camcmp_probe", kScene, "Render it", "commit", kNoLoopFixture, dir, "[]" );
	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus == "final_text", "camcmp_probe run reached final_text" );
	Check( h.dispatcher != nullptr && h.dispatcher->Session() != nullptr, "camcmp_probe has a live session" );
	AgentSession* sess = h.dispatcher->Session();
	if( !sess ) return;

	auto checkResult = [&]( const std::string& cpJson, const std::string& label ) -> AgentEvalCheckResult {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses (" + err + ")" );
		AgentEvalScenario s2 = s; s2.checkpoints = cps;
		return CheckScenario( h, s2 );
	};
	auto checkOne = [&]( const std::string& cpJson, bool expectPass, const std::string& label ) -> std::string {
		AgentEvalCheckResult r = checkResult( cpJson, label );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
			return r.checkpoints[0].detail;
		}
		Check( false, label + ": expected exactly one checkpoint result" );
		return std::string();
	};

	// --- Camera override: measure near vs far framing directly, then pin a
	// distinguishing luma band through the checkpoint path. ---
	auto measureLuma = [&]( const char* loc, const char* look ) -> double {
		AgentRenderParams rp;
		rp.camera.hasLocation = true; rp.camera.location = loc;
		rp.camera.hasLookAt   = true; rp.camera.lookAt   = look;
		rp.camera.hasUp       = true; rp.camera.up       = "0 1 0";
		AgentRenderResult rr = sess->Render( rp );
		return 0.2126 * rr.meanR + 0.7152 * rr.meanG + 0.0722 * rr.meanB;
	};
	const double lumaNear = measureLuma( "0 0 3.5", "0 0 0" );
	const double lumaFar  = measureLuma( "0 0 30",  "0 0 0" );
	std::printf( "  [measured] near-camera meanLuma=%.5f  far-camera meanLuma=%.5f\n", lumaNear, lumaFar );
	Check( lumaFar < lumaNear,
		"far camera frames the sphere small -> mean luma drops below near (near=" +
		std::to_string( lumaNear ) + " far=" + std::to_string( lumaFar ) + ")" );
	const double midLuma = 0.5 * ( lumaNear + lumaFar );
	{
		char cp[320];
		std::snprintf( cp, sizeof( cp ),
			"[{\"kind\":\"render\",\"camera\":{\"location\":[0,0,3.5],\"lookat\":[0,0,0]},\"meanLumaMin\":%.6f}]", midLuma );
		checkOne( cp, true, "near-camera override render clears the mid-luma floor" );
	}
	{
		char cp[320];
		std::snprintf( cp, sizeof( cp ),
			"[{\"kind\":\"render\",\"camera\":{\"location\":[0,0,30],\"lookat\":[0,0,0]},\"meanLumaMin\":%.6f}]", midLuma );
		checkOne( cp, false, "far-camera override render falls BELOW the mid-luma floor (override really moved the camera)" );
	}

	// --- compareToImage self-consistency. ---
	// Write a self-reference from the session's OWN render+PNG path (the
	// exact bytes read_image would return), then compare a FRESH render.
	AgentRenderParams rpRef;                       // default scene camera + dims (24x24)
	AgentRenderResult rrRef = sess->Render( rpRef );
	Check( rrRef.ok && !rrRef.png.empty(), "session render produced PNG bytes for the self-reference" );
	const std::string refPath = dir + "/self_ref.png";
	Check( WriteBytes( refPath, rrRef.png ), "wrote the self-reference PNG to scratch" );

	// A loose (rmseMax 1.0) pass first, purely to surface the measured
	// self-RMSE noise floor in the detail.
	{
		const std::string d = checkOne(
			"[{\"kind\":\"render\",\"compareToImage\":\"" + refPath + "\",\"rmseMax\":1.0}]",
			true, "compareToImage self-consistency clears a rmseMax=1.0 ceiling" );
		std::printf( "  [measured] self-compare detail: %s\n", d.c_str() );
	}
	// The Wave-2 calibration bounds: MC noise between two fresh renders of
	// the same scene sits well under 0.06 but well above 0.0005.
	checkOne( "[{\"kind\":\"render\",\"compareToImage\":\"" + refPath + "\",\"rmseMax\":0.06}]",
		true, "compareToImage self-consistency PASSES at rmseMax=0.06 (MC noise only)" );
	checkOne( "[{\"kind\":\"render\",\"compareToImage\":\"" + refPath + "\",\"rmseMax\":0.0005}]",
		false, "compareToImage self-consistency FAILS at rmseMax=0.0005 (below the MC noise floor)" );

	// A DIFFERENT scene state: a far-camera reference vs a default-camera
	// grading render -> wildly different framing -> RMSE >> 0.06 -> FAIL at
	// the passing threshold.
	AgentRenderParams rpFar;
	rpFar.camera.hasLocation = true; rpFar.camera.location = "0 0 30";
	rpFar.camera.hasLookAt   = true; rpFar.camera.lookAt   = "0 0 0";
	rpFar.camera.hasUp       = true; rpFar.camera.up       = "0 1 0";
	AgentRenderResult rrFar = sess->Render( rpFar );
	const std::string farRefPath = dir + "/far_ref.png";
	Check( WriteBytes( farRefPath, rrFar.png ), "wrote the far-camera reference PNG to scratch" );
	checkOne( "[{\"kind\":\"render\",\"compareToImage\":\"" + farRefPath + "\",\"rmseMax\":0.06}]",
		false, "compareToImage against a different-framing reference FAILS at rmseMax=0.06" );

	// Dimension mismatch: reference is 24x24; ask for a 32x32 grading render.
	{
		const std::string d = checkOne(
			"[{\"kind\":\"render\",\"width\":32,\"height\":32,\"compareToImage\":\"" + refPath + "\",\"rmseMax\":0.06}]",
			false, "compareToImage width/height mismatch vs reference dims FAILS" );
		Check( d.find( "dims must match" ) != std::string::npos,
			"the dims-mismatch detail is clear (got: " + d + ")" );
	}
	// Missing reference file -> failed checkpoint with a clear detail.
	{
		const std::string d = checkOne(
			"[{\"kind\":\"render\",\"compareToImage\":\"" + dir + "/no_such_reference.png\",\"rmseMax\":0.06}]",
			false, "compareToImage against a missing file FAILS" );
		Check( d.find( "could not be read" ) != std::string::npos,
			"the missing-file detail is clear (got: " + d + ")" );
	}

	// The reference-image OVERSIZE case (a GENUINELY VALID >64MP PNG, not a
	// hand-patched lying header) is covered separately by
	// TestCompareToImageOversizedReferenceIsRefusedCheaply, which also
	// times the refusal to prove it happens before any pixel-buffer
	// decode -- see that function's doc for why a patched-IHDR fixture
	// doesn't exercise the guard under test.

	// --- Load-validator reject cases (image-reconstruction Wave 2). ---
	auto writeScenario = [&]( const std::string& id, const std::string& checkpointsJson ) -> std::string {
		JsonValue root = JsonValue::MakeObject();
		root.set( "id", JsonValue::MakeString( id ) );
		root.set( "title", JsonValue::MakeString( id ) );
		JsonValue scene = JsonValue::MakeObject();
		scene.set( "inline", JsonValue::MakeString( kScene ) );
		root.set( "scene", scene );
		JsonValue prompts = JsonValue::MakeArray();
		prompts.push_back( JsonValue::MakeString( "do something" ) );
		root.set( "prompts", prompts );
		JsonValue cps; std::string perr;
		Check( JsonParse( checkpointsJson, cps, perr ), id + ": checkpoints JSON itself parses (" + perr + ")" );
		root.set( "checkpoints", cps );
		const std::string path = dir + "/" + id + ".json";
		WriteFile( path, JsonSerialize( root ) );
		return path;
	};
	auto rejects = [&]( const std::string& id, const std::string& cpJson, const std::string& mustNameField ) {
		AgentEvalScenario sc; std::string err;
		Check( !LoadEvalScenario( writeScenario( id, cpJson ), sc, err ), id + ": load-rejected" );
		Check( err.find( mustNameField ) != std::string::npos,
			id + ": load error names \"" + mustNameField + "\" (got: " + err + ")" );
	};

	rejects( "cmp_no_rmse", "[{\"kind\":\"render\",\"compareToImage\":\"a.png\"}]", "rmseMax" );
	rejects( "rmse_no_cmp", "[{\"kind\":\"render\",\"rmseMax\":0.05}]", "compareToImage" );
	rejects( "rmse_hi", "[{\"kind\":\"render\",\"compareToImage\":\"a.png\",\"rmseMax\":1.5}]", "rmseMax" );
	rejects( "rmse_zero", "[{\"kind\":\"render\",\"compareToImage\":\"a.png\",\"rmseMax\":0}]", "rmseMax" );
	rejects( "cmp_dotdot", "[{\"kind\":\"render\",\"compareToImage\":\"../secret.png\",\"rmseMax\":0.05}]", "compareToImage" );
	rejects( "cam_no_location", "[{\"kind\":\"render\",\"camera\":{\"lookat\":[0,0,0]}}]", "camera.location" );
	rejects( "cam_short_vec", "[{\"kind\":\"render\",\"camera\":{\"location\":[0,0],\"lookat\":[0,0,0]}}]", "camera.location" );
	rejects( "cam_fov_hi", "[{\"kind\":\"render\",\"camera\":{\"location\":[0,0,3.5],\"lookat\":[0,0,0],\"fov\":200}}]", "camera.fov" );
	rejects( "samples_zero", "[{\"kind\":\"render\",\"samples\":0}]", "samples" );

	// A fully-formed Wave-2 render checkpoint (camera + samples +
	// compareToImage + rmseMax) LOADS cleanly (the file need not exist at
	// load time -- the validator never opens it).
	{
		AgentEvalScenario sc; std::string err;
		Check( LoadEvalScenario( writeScenario( "cmp_good",
			"[{\"kind\":\"render\",\"samples\":16,\"camera\":{\"location\":[0,0,3.5],\"lookat\":[0,0,0],\"up\":[0,1,0],\"fov\":40},"
			"\"compareToImage\":\"refs/target.png\",\"rmseMax\":0.05}]" ), sc, err ),
			"a fully-formed Wave-2 render checkpoint loads cleanly (" + err + ")" );
	}
}

//----------------------------------------------------------------------
// TestAdversarialOracleControls: T10 (above) only proves committed
// scenarios' checkpoints are TRUE of their OWN fixture -- it never
// proves a WRONG-behavior session actually fails them.  This test loads
// each of four Wave-B-strengthened committed scenarios via
// LoadEvalScenario (so it exercises the REAL checkpoints[] shipped in
// evals/scenarios/*.json, not a hand-copied approximation), swaps in a
// hand-built canned fixture that dodges the newly-added assertion while
// otherwise behaving plausibly, runs it through the real replay path,
// and asserts CheckScenario(...).allPassed == false with the SPECIFIC
// new checkpoint index the one that failed (every other checkpoint
// still passes, so the failure is attributable, not a shotgunned wrong
// fixture) and its detail naming the discriminating field.
//
// The wrong-behavior bodies are built with JsonValue/JsonSerialize (the
// SAME codec the harness itself parses fixtures with) rather than
// hand-escaped string literals, so there is no way for a typo'd escape
// to silently produce a differently-wrong fixture than intended.
//----------------------------------------------------------------------

static std::string JsonlLine( const std::string& provider, const std::string& bodyText )
{
	JsonValue line = JsonValue::MakeObject();
	line.set( "provider", JsonValue::MakeString( provider ) );
	line.set( "body", JsonValue::MakeString( bodyText ) );
	return JsonSerialize( line ) + "\n";
}

struct ToolCallSpec { std::string name; JsonValue input; };

// One Anthropic-shaped assistant message: optional text block, zero or
// more tool_use blocks (in order), stop_reason ("tool_use" when tools
// are present, "end_turn" for a plain final message).
static std::string AnthropicBody( const std::string& id, const std::string& text,
                                   const std::vector<ToolCallSpec>& tools, const std::string& stopReason )
{
	JsonValue body = JsonValue::MakeObject();
	body.set( "id", JsonValue::MakeString( id ) );
	body.set( "type", JsonValue::MakeString( "message" ) );
	body.set( "role", JsonValue::MakeString( "assistant" ) );
	body.set( "model", JsonValue::MakeString( "claude-sonnet-5" ) );

	JsonValue content = JsonValue::MakeArray();
	if( !text.empty() ) {
		JsonValue t = JsonValue::MakeObject();
		t.set( "type", JsonValue::MakeString( "text" ) );
		t.set( "text", JsonValue::MakeString( text ) );
		content.push_back( t );
	}
	int idx = 0;
	for( const ToolCallSpec& tc : tools ) {
		JsonValue u = JsonValue::MakeObject();
		u.set( "type", JsonValue::MakeString( "tool_use" ) );
		u.set( "id", JsonValue::MakeString( "toolu_adv" + std::to_string( idx++ ) ) );
		u.set( "name", JsonValue::MakeString( tc.name ) );
		u.set( "input", tc.input );
		content.push_back( u );
	}
	body.set( "content", content );
	body.set( "stop_reason", JsonValue::MakeString( stopReason ) );
	body.set( "stop_sequence", JsonValue::MakeNull() );
	JsonValue usage = JsonValue::MakeObject();
	usage.set( "input_tokens", JsonValue::MakeNumber( 100 ) );
	usage.set( "output_tokens", JsonValue::MakeNumber( 40 ) );
	body.set( "usage", usage );
	return JsonSerialize( body );
}

static JsonValue EmptyInput() { return JsonValue::MakeObject(); }

static JsonValue ProposePatchInput( const std::string& target, const std::string& param, const std::string& value,
                                     bool withBaseHeadVersion )
{
	JsonValue in = JsonValue::MakeObject();
	in.set( "target", JsonValue::MakeString( target ) );
	in.set( "param", JsonValue::MakeString( param ) );
	in.set( "value", JsonValue::MakeString( value ) );
	if( withBaseHeadVersion ) {
		JsonValue bhv = JsonValue::MakeObject();
		bhv.set( "uuid", JsonValue::MakeNumber( 0 ) );
		bhv.set( "revision", JsonValue::MakeNumber( 0 ) );
		in.set( "baseHeadVersion", bhv );
	}
	return in;
}

static JsonValue InsertChunkInput( const std::string& chunkText )
{
	JsonValue in = JsonValue::MakeObject();
	in.set( "chunkText", JsonValue::MakeString( chunkText ) );
	return in;
}

//! A single-element `propose_patches` input -- enough to prove the BATCH
//! verb is classified as document-mutating by the trajectory checker.
static JsonValue ProposePatchesInput( const std::string& target, const std::string& param, const std::string& value )
{
	JsonValue arr = JsonValue::MakeArray();
	arr.push_back( ProposePatchInput( target, param, value, false ) );
	JsonValue in = JsonValue::MakeObject();
	in.set( "patches", arr );
	return in;
}

// {reference,samples,visual:false} -- the image_reconstruct_* control
// fixtures' "measure before finishing" round (mirrors the committed
// evals/fixtures/image_reconstruct_*.fixture.jsonl compare_to_reference
// round, which the requiredToolInOrder checkpoint now requires).
static JsonValue CompareToReferenceInput( const std::string& reference, int samples )
{
	JsonValue in = JsonValue::MakeObject();
	in.set( "reference", JsonValue::MakeString( reference ) );
	in.set( "samples", JsonValue::MakeNumber( samples ) );
	in.set( "visual", JsonValue::MakeBool( false ) );
	return in;
}

// {question} -- the ask_user tool's sole REQUIRED argument (stage 2's
// scripted responder tests below only ever need to control `question`;
// `options`/`allowFreeform` are irrelevant to matching).
static JsonValue AskUserInput( const std::string& question )
{
	JsonValue in = JsonValue::MakeObject();
	in.set( "question", JsonValue::MakeString( question ) );
	return in;
}

// Load `scenarioPath` for real (so the checkpoints under test are the
// actual committed ones), swap in `fixtureText` as the replay source,
// run it, and assert the SPECIFIC checkpoint at `expectFailIndex` is
// the one (and only one) that fails, with its detail containing
// `expectedDetailSubstr`.
static void RunAdversarialControl( const std::string& scenarioPath, const std::string& fixtureText,
                                    const char* scratchLeaf, std::size_t expectFailIndex,
                                    const std::string& expectedDetailSubstr, const std::string& label )
{
	const std::string dir = ScratchRunDir( scratchLeaf );
	AgentEvalScenario s; std::string err;
	Check( LoadEvalScenario( scenarioPath, s, err ), label + ": committed scenario loads (" + err + ")" );

	s.replayFixturePath = dir + "/wrong.fixture.jsonl";
	Check( WriteFile( s.replayFixturePath, fixtureText ), label + ": wrong-behavior fixture written to scratch" );

	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus != "load_error", label + ": run did not load_error (" + h.result.errorMessage + ")" );

	AgentEvalCheckResult r = CheckScenario( h, s );
	Check( r.checkpoints.size() == s.checkpoints.size(), label + ": one check result per committed checkpoint" );
	Check( !r.allPassed, label + ": allPassed is FALSE against the wrong-behavior fixture" );

	if( expectFailIndex < r.checkpoints.size() ) {
		Check( !r.checkpoints[expectFailIndex].passed,
			label + ": checkpoints[" + std::to_string( expectFailIndex ) + "] (" + r.checkpoints[expectFailIndex].kind +
			") is the one that failed (detail: " + r.checkpoints[expectFailIndex].detail + ")" );
		Check( r.checkpoints[expectFailIndex].detail.find( expectedDetailSubstr ) != std::string::npos,
			label + ": failing detail mentions '" + expectedDetailSubstr + "' (detail: " + r.checkpoints[expectFailIndex].detail + ")" );
		// Every OTHER checkpoint should still pass -- proves the failure is
		// attributable to the ONE new assertion under test, not a
		// shotgunned wrong fixture that happens to break everything.
		for( std::size_t i = 0; i < r.checkpoints.size(); ++i ) {
			if( i == expectFailIndex ) continue;
			Check( r.checkpoints[i].passed,
				label + ": checkpoints[" + std::to_string( i ) + "] (" + r.checkpoints[i].kind +
				") unexpectedly ALSO failed (detail: " + r.checkpoints[i].detail + ")" );
		}
	} else {
		Check( false, label + ": expectFailIndex " + std::to_string( expectFailIndex ) +
			" out of range (only " + std::to_string( r.checkpoints.size() ) + " checkpoints)" );
	}
}

static void TestAdversarialOracleControls()
{
	std::printf( "T-adv: adversarial oracle controls -- wrong behavior must FAIL the strengthened checkpoints...\n" );

	// (a) two_tool_observe: a fixture that emits ONLY final text (zero tool
	// calls, straight to end_turn) -- the trajectory checkpoint's new
	// requiredToolInOrder:["render","read_image"] must fail (nothing was
	// ever dispatched).  The text still names "sphere"/"lit" so the
	// (also-new) finalText checkpoint and the render checkpoint (which
	// renders the base scene itself, independent of tool calls) both stay
	// green -- isolating the failure to requiredToolInOrder.
	{
		const std::string body = AnthropicBody( "msg_1",
			"There is a lit diffuse sphere in the scene, from memory -- no new render was actually dispatched.",
			{}, "end_turn" );
		RunAdversarialControl( "evals/scenarios/two_tool_observe.json", JsonlLine( "anthropic", body ),
			"t_adv_two_tool_observe", 1, "requiredToolInOrder",
			"two_tool_observe/zero-tool-calls control" );
	}

	// (b) conflict_retry: a fixture whose FIRST propose_patch omits
	// baseHeadVersion (so it applies immediately -- no conflict), then
	// read_document + a second unconditioned propose_patch (also applied).
	// The scenario's OWN scripted intervention still fires after tool call
	// 1 (it is unconditional, independent of the fixture), but since
	// neither propose_patch pins a baseHeadVersion, neither ever conflicts
	// -- the toolOutcomes "first ... expect:conflict" spec must fail, while
	// "last ... expect:applied" still holds, requiredToolInOrder still
	// matches the same 4-call shape, and the final colour still lands blue
	// (document/untouched/diagnostics stay green).
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading first.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Recoloring blue (no baseHeadVersion pinned).",
			{ { "propose_patch", ProposePatchInput( "pnt_albedo", "color", "0.1 0.2 0.9", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Reading again.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "Recoloring again (still unconditioned).",
			{ { "propose_patch", ProposePatchInput( "pnt_albedo", "color", "0.1 0.2 0.9", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Done: pnt_albedo is blue.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/conflict_retry.json", fixture,
			"t_adv_conflict_retry", 3, "conflict",
			"conflict_retry/never-conflicts control" );
	}

	// (c) multi_turn_edit: a fixture where turn 1 only reads the document
	// (no edit) and BOTH propose_patch calls (color, then radius) happen
	// during turn 2 -- toolCallAfterUserTurn's color spec
	// (argsContains:"color", maxUserTurns:1) must fail since the color
	// edit lands at userTurns==2, not <=1.  The radius spec
	// (minUserTurns:2) still holds, requiredToolInOrder still matches (two
	// propose_patch calls in order), and both final values stay in-band
	// (document checkpoints stay green).
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the scene first.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Just read, no edit yet.", {}, "end_turn" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Recoloring on the second turn.",
			{ { "propose_patch", ProposePatchInput( "pnt_albedo", "color", "0.9 0.1 0.1", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "Enlarging on the second turn too.",
			{ { "propose_patch", ProposePatchInput( "sph", "radius", "0.9", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Recolored and enlarged, both on turn 2.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/multi_turn_edit.json", fixture,
			"t_adv_multi_turn_edit", 3, "color",
			"multi_turn_edit/both-edits-on-turn-2 control" );
	}

	// (d) reserved_name_recovery: a fixture that NEVER attempts the
	// reserved `none` name -- it reads the document, then inserts the
	// overhead camera directly as `overhead_cam`.  The scenario's
	// toolOutcomes first spec (argsContains:"none", expect:"rejected")
	// then has no matching record at all and must fail; the "last ...
	// expect:applied" spec still holds (the one insert_chunk call
	// succeeds), and every document-side assertion (2 cameras, no chunk
	// literally named "none", the new camera's location within the
	// overhead band) stays green.
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the scene first.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Adding the overhead camera directly.",
			{ { "insert_chunk", InsertChunkInput(
				"pinhole_camera\n{\n\tname overhead_cam\n\tlocation 0 4 0\n\tlookat 0 0 0\n\tup 0 0 -1\n\tfov 40.0\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Done: added overhead_cam.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/reserved_name_recovery.json", fixture,
			"t_adv_reserved_name_recovery", 5, "none",
			"reserved_name_recovery/never-attempts-none control" );
	}

	// (e) camera_orbit_timeline: a fixture that inserts ONE camera/location
	// timeline whose keyframes are all the SAME point (a constant camera, not
	// an orbit).  chunk_count still reaches 15 (base scene has 14), the camera
	// count stays 1, the referenced chunks stay untouched, and diagnostics are
	// clean -- but param_series_orbit's `where` filter selects that single
	// constant timeline and it dedupes to ONE keyframe, so the orbit
	// checkpoint (index 2) is the only failure.
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the scene first.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Adding a (constant) camera timeline.",
			{ { "insert_chunk", InsertChunkInput(
				"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
				"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n"
				"\ttime\t\t0.5\n\tvalue\t\t4.5 4.8 18.5\n"
				"\ttime\t\t1.0\n\tvalue\t\t4.5 4.8 18.5\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Done: added a camera timeline.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/camera_orbit_timeline.json", fixture,
			"t_adv_camera_orbit_timeline", 2, "dedupe",
			"camera_orbit_timeline/constant-camera control" );
	}

	// (e2) camera_orbit_timeline, sibling control: a fixture that inserts an
	// orbit-SHAPED camera/location timeline (same 5 keyframes / 270-degree
	// spread as the committed fixture's real orbit) but whose `time` lines
	// are ALL 0.0 -- geometry alone would pass, but the timeline never
	// actually animates.  chunk_count, camera count, untouched, and
	// diagnostics all stay green exactly as in (e); param_series_orbit
	// (index 2) is again the only failure, this time on the TIME axis
	// rather than the dedupe/geometry axis.
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the scene first.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Adding an orbit-shaped camera timeline (times all 0.0).",
			{ { "insert_chunk", InsertChunkInput(
				"timeline\n{\n\telement_type\tcamera\n\tparam\t\tlocation\n"
				"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n"
				"\ttime\t\t0.0\n\tvalue\t\t-18.5 4.8 4.5\n"
				"\ttime\t\t0.0\n\tvalue\t\t-4.5 4.8 -18.5\n"
				"\ttime\t\t0.0\n\tvalue\t\t18.5 4.8 -4.5\n"
				"\ttime\t\t0.0\n\tvalue\t\t4.5 4.8 18.5\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Done: added a camera timeline.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/camera_orbit_timeline.json", fixture,
			"t_adv_camera_orbit_timeline_zerotime", 2, "strictly increasing",
			"camera_orbit_timeline/orbit-shaped-but-zero-time control" );
	}

	// (f) multi_step_build: a fixture that builds a centered BOX object at the
	// origin (a box_geometry, not a sphere) PLUS a DETACHED sphere_geometry
	// chunk that no object binds, plus the same area light.  sphere-count
	// (1, the orphan), object-count (2), the luma band, and diagnostics all
	// stay green; the objectmap checkpoint (index 3) fails because the CENTRE
	// pixel's hit object's geometry is a box_geometry, not the required
	// sphere_geometry.
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the scene.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Grey albedo painter.",
			{ { "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.6 0.6 0.6\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Lambertian material.",
			{ { "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname mat_box\n\treflectance pnt_albedo\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "A BOX geometry (not a sphere) for the visible object.",
			{ { "insert_chunk", InsertChunkInput( "box_geometry\n{\n\tname geo_box\n\twidth 0.9\n\theight 0.9\n\tdepth 0.9\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Placing the box at the origin.",
			{ { "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname obj_box\n\tgeometry geo_box\n\tmaterial mat_box\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_6", "A DETACHED sphere geometry nothing binds.",
			{ { "insert_chunk", InsertChunkInput( "sphere_geometry\n{\n\tname orphan_sph\n\tradius 0.5\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_7", "Emission painter.",
			{ { "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_8", "Luminaire material.",
			{ { "insert_chunk", InsertChunkInput( "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_9", "Emitter quad.",
			{ { "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_10", "Placing the emitter.",
			{ { "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}" ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_11", "Built a lit box + a detached sphere.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/multi_step_build.json", fixture,
			"t_adv_multi_step_build", 3, "box_geometry",
			"multi_step_build/centered-box + detached-sphere control" );
	}

	// (g) multi_turn_edit: a fixture whose turn-1 propose_patch is a REJECTED
	// color patch (a bad target, pnt_albdeo), and whose turn-2 makes the REAL
	// color + radius edits.  The turn-1 spec (argsContains ["pnt_albedo",
	// "color"], expect:"applied", userTurns 1..1) no longer matches -- the
	// rejected typo'd patch neither contains "pnt_albedo" nor applied -- so
	// the trajectory checkpoint (index 3) fails there, while the end-state
	// colour/radius bands, diagnostics, and terminal all stay green.
	{
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Recoloring (typo'd target).",
			{ { "propose_patch", ProposePatchInput( "pnt_albdeo", "color", "0.9 0.1 0.1", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "That failed; ending turn 1.", {}, "end_turn" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Recoloring correctly on turn 2.",
			{ { "propose_patch", ProposePatchInput( "pnt_albedo", "color", "0.9 0.1 0.1", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "Enlarging on turn 2.",
			{ { "propose_patch", ProposePatchInput( "sph", "radius", "0.9", false ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Recolored and enlarged.", {}, "end_turn" ) );
		RunAdversarialControl( "evals/scenarios/multi_turn_edit.json", fixture,
			"t_adv_multi_turn_edit_applied", 3, "applied",
			"multi_turn_edit/turn-1-rejected-color control" );
	}

	// (h)/(i)/(j)/(k) below exercise the Wave-5 image_reconstruct_* scenarios.
	// Their checkpoint arrays are 4 STRICT/RELAXED render compares + objectmap +
	// document + diagnostics + trajectory + 4 GRADED render compares (indices
	// 0-3 = view1..view4 structural compares, 4 = objectmap, 5 = document
	// chunk_count, 6 = diagnostics, 7 = trajectory, 8-11 = view1..view4 GRADED
	// compares -- weight 0.5 each, CALIBRATION.md Sec 12). checkpoint 0 (view1)
	// grades at the STRICT 0.015 cap; checkpoints 1-3 (view2/view3/view4) grade
	// at the RELAXED 0.04 cap (CALIBRATION.md Sec 10 -- image_reconstruct_single's
	// two-tier rubric, since only view1 is an observable reference photo).
	// checkpoints 8-11 grade at the GRADED tier's 0.9x-anchor(e) caps
	// (0.105/0.12/0.15/0.115) -- loose enough that a normal PASS also clears
	// them trivially, tight enough that an empty/near-empty stage still fails
	// them (CALIBRATION.md Sec 12). Unlike (a)-(g), an empty-stage, wrong-
	// lighting, or flat-cutout reconstruction fails MULTIPLE checkpoints at
	// once (the four compares share root causes), so RunAdversarialControl's
	// "every OTHER checkpoint must stay green" rule does not apply here --
	// these four controls are written by hand instead, asserting only the
	// SPECIFIC checkpoints the task calls out, exactly as CALIBRATION.md's
	// anchor measurements predict.

	// (h) image_reconstruct_single: a fixture that inserts NOTHING (reads the
	// skill and the document, renders and looks, then ends the turn on an
	// empty stage). CALIBRATION.md anchor (e) (gross failure -- hero object
	// deleted entirely) measures RMSE 0.118-0.170 across all four views,
	// comfortably above BOTH view1's strict 0.015 cap and views 2-4's relaxed
	// 0.04 cap -- every compare checkpoint fails, and the
	// objectmap/document checkpoints miss too (nothing was ever built). Only
	// the view1 compare (checkpoint 0) is asserted here, per the task's
	// explicit carve-out: an empty scene legitimately fails several
	// checkpoints at once, so there is no single "attributable" failure to
	// isolate the way (a)-(g) do. The GRADED tier (checkpoints 8-11) is also
	// asserted below: its caps sit at 0.9x anchor (e) BY CONSTRUCTION, so the
	// empty-stage fixture -- which literally IS anchor (e) -- fails all four
	// of them too. That is the graded tier's margin proof, re-rendered in CI:
	// the tier's whole purpose is to fail an empty/near-empty stage while
	// giving partial credit for genuine engagement short of full fidelity.
	{
		const std::string dir = ScratchRunDir( "t_adv_image_reconstruct_empty" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/image_reconstruct_single.json", s, err ),
			"image_reconstruct_single/empty-stage control: committed scenario loads (" + err + ")" );

		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the modeling skill first.",
			{ { "read_skill", ( []{ JsonValue in = JsonValue::MakeObject();
			                         in.set( "name", JsonValue::MakeString( "modeling-from-image-captures" ) );
			                         return in; } )() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Checking the scaffold.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Rendering as-is.",
			{ { "render", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "Looking at it.",
			{ { "read_image", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Done.", {}, "end_turn" ) );

		s.replayFixturePath = dir + "/wrong.fixture.jsonl";
		Check( WriteFile( s.replayFixturePath, fixture ),
			"image_reconstruct_single/empty-stage control: fixture written to scratch" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			"image_reconstruct_single/empty-stage control: run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			"image_reconstruct_single/empty-stage control: one check result per committed checkpoint" );
		Check( !r.allPassed, "image_reconstruct_single/empty-stage control: allPassed is FALSE against an empty stage" );
		if( r.checkpoints.size() > 0 ) {
			Check( !r.checkpoints[0].passed,
				"image_reconstruct_single/empty-stage control: checkpoints[0] (view1 compare) failed (detail: " +
				r.checkpoints[0].detail + ")" );
			Check( r.checkpoints[0].detail.find( "RMSE" ) != std::string::npos,
				"image_reconstruct_single/empty-stage control: checkpoints[0] detail names RMSE (detail: " +
				r.checkpoints[0].detail + ")" );
		}
		// GRADED-tier margin proof (CALIBRATION.md Sec 12): checkpoints 8-11
		// (the appended weight-0.5 graded compares) cap at 0.9x anchor (e), and
		// this fixture's empty stage IS anchor (e) -- so all four MUST fail too,
		// confirming the graded tier does not silently admit an empty/near-empty
		// stage that the structural tier already rejects.
		if( r.checkpoints.size() > 11 ) {
			for( std::size_t i = 8; i <= 11; ++i ) {
				Check( !r.checkpoints[i].passed,
					"image_reconstruct_single/empty-stage control: checkpoints[" + std::to_string( i ) +
					"] (graded compare) failed (detail: " + r.checkpoints[i].detail + ")" );
				Check( r.checkpoints[i].detail.find( "RMSE" ) != std::string::npos,
					"image_reconstruct_single/empty-stage control: checkpoints[" + std::to_string( i ) +
					"] detail names RMSE (detail: " + r.checkpoints[i].detail + ")" );
			}
		}
	}

	// (i) image_reconstruct_single: every GT chunk lifted verbatim EXCEPT the
	// key light's exitance colour, which is recoloured neutral white (1 1 1,
	// same scale/power) instead of the warm (1.0 0.82 0.58) ground truth.
	// CALIBRATION.md anchor (c) measures RMSE 0.0325-0.0479 across the four
	// views. Under the two-tier caps this does NOT fail every compare: view1
	// (0.0381) is above its strict 0.015 cap and view4 (0.0479) is above the
	// relaxed 0.04 cap, but view2 (0.0325) and view3 (0.0364) sit BELOW the
	// relaxed 0.04 cap and pass -- exactly why the checkpoint that must catch
	// this defect is view1's own strict cap (asserted below), not the relaxed
	// withheld-view caps. The object/stage/geometry are otherwise
	// exactly right, so the objectmap and sdf chunk_count checkpoints stay
	// green: the failure is attributable to lighting, not shape or a missing
	// object.
	{
		const std::string dir = ScratchRunDir( "t_adv_image_reconstruct_neutral_key" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/image_reconstruct_single.json", s, err ),
			"image_reconstruct_single/neutral-key control: committed scenario loads (" + err + ")" );

		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the document.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		int msgIdx = 2;
		auto insertMsg = [&]( const std::string& text, const std::string& chunkText ) {
			fixture += JsonlLine( "anthropic", AnthropicBody( "msg_" + std::to_string( msgIdx++ ), text,
				{ { "insert_chunk", InsertChunkInput( chunkText ) } }, "tool_use" ) );
		};
		insertMsg( "Teal albedo painter.", "uniformcolor_painter\n{\n\tname pnt_teal\n\tcolor 0.05 0.55 0.5\n}" );
		insertMsg( "Ground albedo painter.", "uniformcolor_painter\n{\n\tname pnt_ground\n\tcolor 0.62 0.62 0.64\n}" );
		insertMsg( "Backdrop albedo painter.", "uniformcolor_painter\n{\n\tname pnt_backdrop\n\tcolor 0.30 0.31 0.34\n}" );
		insertMsg( "Key light exitance painter -- NEUTRAL WHITE, not the GT warm colour.",
			"uniformcolor_painter\n{\n\tname pnt_key\n\tcolor 1.0 1.0 1.0\n}" );
		// pnt_env is NOT inserted -- the scaffold already declares it (wired into
		// the rasterizer's radiance_map; the rasterizer chunk is unnamed and
		// singleton, so it cannot be re-targeted after load). Recolour the
		// scaffold's placeholder instead, matching the committed fixture's
		// approach.
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_" + std::to_string( msgIdx++ ),
			"Recolouring the scaffold's placeholder environment-tint painter.",
			{ { "propose_patch", ProposePatchInput( "pnt_env", "color", "0.06 0.08 0.11", false ) } }, "tool_use" ) );
		insertMsg( "Hero material.", "lambertian_material\n{\n\tname teal\n\treflectance pnt_teal\n}" );
		insertMsg( "Ground material.", "lambertian_material\n{\n\tname ground\n\treflectance pnt_ground\n}" );
		insertMsg( "Backdrop material.", "lambertian_material\n{\n\tname backdrop\n\treflectance pnt_backdrop\n}" );
		insertMsg( "Key luminaire material -- same scale 42.0 as GT, only colour changed.",
			"lambertian_luminaire_material\n{\n\tname key_mat\n\texitance pnt_key\n\tscale 42.0\n\tmaterial none\n}" );
		insertMsg( "Ground geometry.",
			"clippedplane_geometry\n{\n\tname groundgeom\n\tpta 7 0 -7\n\tptb -7 0 -7\n\tptc -7 0 7\n\tptd 7 0 7\n}" );
		insertMsg( "Backdrop geometry.",
			"clippedplane_geometry\n{\n\tname backdropgeom\n\tpta -7 0 -4.5\n\tptb -7 7 -4.5\n\tptc 7 7 -4.5\n\tptd 7 0 -4.5\n}" );
		insertMsg( "Key light geometry.",
			"clippedplane_geometry\n{\n\tname keygeom\n\tpta -2.1 3.6 1.9\n\tptb -2.1 3.6 0.7\n\tptc -0.9 3.6 0.7\n\tptd -0.9 3.6 1.9\n}" );
		insertMsg( "Hero SDF geometry, verbatim from the ground truth.",
			"sdf_geometry\n{\n\tname herogeom\n"
			"\tpart capsule union 0 0 0.75 0 0 0 0 1 1 1 0.30 0.45 0 0\n"
			"\tpart sphere smin 0.15 0 1.50 0 0 0 0 1 0.62 1 0.62 0 0 0\n"
			"\tpart torus smin 0.12 0 1.20 0 0 0 0 1 1 1 0.36 0.12 0 0\n"
			"\tpart roundbox subtract 0.05 0.52 1.55 -0.08 0 0 0 1 1 1 0.22 0.17 0.17 0.04\n}" );
		insertMsg( "Ground object.", "standard_object\n{\n\tname ground\n\tgeometry groundgeom\n\tmaterial ground\n}" );
		insertMsg( "Backdrop object.", "standard_object\n{\n\tname backdrop\n\tgeometry backdropgeom\n\tmaterial backdrop\n}" );
		insertMsg( "Key light object.", "standard_object\n{\n\tname key\n\tgeometry keygeom\n\tmaterial key_mat\n}" );
		insertMsg( "Hero object.", "standard_object\n{\n\tname hero\n\tgeometry herogeom\n\tposition 0 0 0\n\tmaterial teal\n}" );
		insertMsg( "Cool omni fill light.", "omni_light\n{\n\tname fill\n\tpower 6.0\n\tposition 3.4 2.2 1.6\n\tcolor 0.55 0.62 0.78\n}" );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_" + std::to_string( msgIdx ),
			"Built the scene; key light is neutral white.", {}, "end_turn" ) );

		s.replayFixturePath = dir + "/wrong.fixture.jsonl";
		Check( WriteFile( s.replayFixturePath, fixture ),
			"image_reconstruct_single/neutral-key control: fixture written to scratch" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			"image_reconstruct_single/neutral-key control: run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			"image_reconstruct_single/neutral-key control: one check result per committed checkpoint" );
		Check( !r.allPassed, "image_reconstruct_single/neutral-key control: allPassed is FALSE against a neutral-white key light" );
		if( r.checkpoints.size() > 7 ) {
			Check( !r.checkpoints[0].passed,
				"image_reconstruct_single/neutral-key control: checkpoints[0] (view1 compare) failed on lighting alone (detail: " +
				r.checkpoints[0].detail + ")" );
			Check( r.checkpoints[4].passed,
				"image_reconstruct_single/neutral-key control: checkpoints[4] (objectmap) still PASSES -- attributable to lighting, not shape (detail: " +
				r.checkpoints[4].detail + ")" );
			Check( r.checkpoints[5].passed,
				"image_reconstruct_single/neutral-key control: checkpoints[5] (sdf chunk_count) still PASSES (detail: " +
				r.checkpoints[5].detail + ")" );
		}
	}

	// (j) image_reconstruct_single: the hero is built as a plain
	// sphere_geometry instead of an sdf_geometry (a non-SDF shape at roughly
	// the object's centre). The objectmap checkpoint's expectGeometryKind
	// binds the hit object's geometry chunk and requires it to be
	// 'sdf_geometry' -- it fails with a detail naming the ACTUAL kind
	// ('sphere_geometry'). The render compares also fail (a sphere silhouette
	// does not match the reference photo's mushroom/rocket silhouette) but
	// that is not the assertion under test here.
	{
		const std::string dir = ScratchRunDir( "t_adv_image_reconstruct_mesh_geom" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/image_reconstruct_single.json", s, err ),
			"image_reconstruct_single/non-sdf-geometry control: committed scenario loads (" + err + ")" );

		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the document.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Teal albedo painter.",
			{ { "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_teal\n\tcolor 0.05 0.55 0.5\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3", "Hero material.",
			{ { "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname teal\n\treflectance pnt_teal\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4", "Hero geometry -- a SPHERE, not an SDF.",
			{ { "insert_chunk", InsertChunkInput( "sphere_geometry\n{\n\tname herogeom\n\tradius 0.8\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5", "Placing the hero near the object's centre.",
			{ { "insert_chunk", InsertChunkInput(
				"standard_object\n{\n\tname hero\n\tgeometry herogeom\n\tposition 0 0.9 0\n\tmaterial teal\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_6", "Built the hero as a sphere.", {}, "end_turn" ) );

		s.replayFixturePath = dir + "/wrong.fixture.jsonl";
		Check( WriteFile( s.replayFixturePath, fixture ),
			"image_reconstruct_single/non-sdf-geometry control: fixture written to scratch" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			"image_reconstruct_single/non-sdf-geometry control: run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			"image_reconstruct_single/non-sdf-geometry control: one check result per committed checkpoint" );
		Check( !r.allPassed, "image_reconstruct_single/non-sdf-geometry control: allPassed is FALSE against a sphere hero" );
		if( r.checkpoints.size() > 4 ) {
			Check( !r.checkpoints[4].passed,
				"image_reconstruct_single/non-sdf-geometry control: checkpoints[4] (objectmap expectGeometryKind) failed (detail: " +
				r.checkpoints[4].detail + ")" );
			Check( r.checkpoints[4].detail.find( "sphere_geometry" ) != std::string::npos,
				"image_reconstruct_single/non-sdf-geometry control: checkpoints[4] detail names the actual kind 'sphere_geometry' (detail: " +
				r.checkpoints[4].detail + ")" );
		}
	}

	// (k) image_reconstruct_single: every GT chunk lifted verbatim EXCEPT
	// herogeom's part lines, which are replaced with a FLAT-CUTOUT ("billboard")
	// reconstruction -- the SAME primitives (capsule stem, sphere cap, torus
	// collar, roundbox notch), each rotated (ex=-20, ey=39.14 degrees) so its
	// LOCAL Z axis aligns with view1's camera direction and squashed to 15% scale
	// on that axis, so from view1 it reads as a plausible face-on match to the
	// mushroom silhouette but has near-zero real depth. This is the measured
	// anchor (f) in CALIBRATION.md Sec 3/10: at 64 spp, view1 RMSE 0.053, view2
	// 0.087, view3 0.131, view4 0.073 -- all three withheld views land well above
	// the 0.04 relaxed cap. This control demonstrates that the relaxed 0.04
	// caps on checkpoints 1-3 DO fire against a maximally-flat billboard, re-
	// rendered in CI rather than merely asserted in prose. Per CALIBRATION.md
	// Sec 11, a follow-up necessity investigation (2026-07-17) found no
	// box-intersect-cut construction that clears view1 (<=0.015) AND fails
	// all THREE withheld views (>0.04) simultaneously -- but its best attempt
	// (anchor (g): the -0.20 bas-relief) PASSES view1, view2, AND view4 while
	// failing ONLY view3, which makes anchor (g) a measured PARTIAL-NECESSITY
	// WITNESS for the withheld tier: without the withheld gates that
	// wrong-volume solution would pass the whole eval, and view3's gate alone
	// rejects it. Control (k2) below pins that witness in CI; this control
	// (k) remains the defense-in-depth complement (the caps reject the
	// maximally-flat class outright) -- see CALIBRATION.md Sec 11 for the
	// full iteration history and the two-part conclusion. view1's own strict
	// 0.015 cap is not asserted here; only the withheld-view rejection is the
	// claim under test. The fixture below follows the full required workflow
	// (read_skill -> ... -> render -> read_image -> compare_to_reference,
	// matching the committed image_reconstruct_single fixture) so the
	// trajectory checkpoint PASSES and the compares are the ONLY failures --
	// an isolated "only geometry is wrong" probe, not a probe that also
	// happens to skip required tool calls.
	{
		const std::string dir = ScratchRunDir( "t_adv_image_reconstruct_flat_cutout" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/image_reconstruct_single.json", s, err ),
			"image_reconstruct_single/flat-cutout control: committed scenario loads (" + err + ")" );

		// Tool calls are BATCHED multiple-per-message (mirroring
		// evals/fixtures/image_reconstruct_single.fixture.jsonl's grouping),
		// not one-per-message -- AgentEvalRunner enforces a 20-tool-round
		// iteration cap per turn, and one-per-message for this many chunks
		// (18 inserts/patches + render + read_image = 20 rounds) trips that
		// cap and turns the run into a `provider_error` before it ever
		// reaches the render/read_image calls, defeating the P2 fix below.
		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the modeling-from-image-captures skill before touching the scene.",
			{ { "read_skill", ( []{
				JsonValue in = JsonValue::MakeObject();
				in.set( "name", JsonValue::MakeString( "modeling-from-image-captures" ) );
				return in; } )() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Checking the starting scaffold.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3",
			"Adding the four albedo/exitance painters (hero teal, ground, backdrop, warm key -- only the "
			"hero shape is wrong here), and recolouring the scaffold's placeholder pnt_env painter to the "
			"observed dim blue-grey environment tint.",
			{	{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_teal\n\tcolor 0.05 0.55 0.5\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_ground\n\tcolor 0.62 0.62 0.64\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_backdrop\n\tcolor 0.30 0.31 0.34\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_key\n\tcolor 1.0 0.82 0.58\n}" ) },
				// pnt_env is NOT inserted -- the scaffold already declares it (wired
				// into the rasterizer's radiance_map; the rasterizer chunk is
				// unnamed and singleton, so it cannot be re-targeted after load).
				// Recolour the scaffold's placeholder instead.
				{ "propose_patch", ProposePatchInput( "pnt_env", "color", "0.06 0.08 0.11", false ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4",
			"Adding the lambertian materials for the hero/ground/backdrop, plus the key light's luminaire material.",
			{	{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname teal\n\treflectance pnt_teal\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname ground\n\treflectance pnt_ground\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname backdrop\n\treflectance pnt_backdrop\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_luminaire_material\n{\n\tname key_mat\n\texitance pnt_key\n\tscale 42.0\n\tmaterial none\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5",
			"Adding the ground plane, backdrop plane, and key-light quad geometry.",
			{	{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname groundgeom\n\tpta 7 0 -7\n\tptb -7 0 -7\n\tptc -7 0 7\n\tptd 7 0 7\n}" ) },
				{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname backdropgeom\n\tpta -7 0 -4.5\n\tptb -7 7 -4.5\n\tptc 7 7 -4.5\n\tptd 7 0 -4.5\n}" ) },
				{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname keygeom\n\tpta -2.1 3.6 1.9\n\tptb -2.1 3.6 0.7\n\tptc -0.9 3.6 0.7\n\tptd -0.9 3.6 1.9\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_6",
			"The hero SDF -- FLAT-CUTOUT variant: same 4 primitives (capsule stem, sphere cap, torus collar, "
			"roundbox notch), each rotated (ex=-20, ey=39.14 degrees) so its local Z axis aligns with view1's "
			"camera direction and squashed to scale.z=0.15 on that axis.",
			{ { "insert_chunk", InsertChunkInput(
				"sdf_geometry\n{\n\tname herogeom\n"
				"\tpart capsule union 0 0 0.75 0 -20 39.14 0 1 1 0.15 0.30 0.45 0 0\n"
				"\tpart sphere smin 0.15 0 1.50 0 -20 39.14 0 1 0.62 0.15 0.62 0 0 0\n"
				"\tpart torus smin 0.12 0 1.20 0 -20 39.14 0 1 1 0.15 0.36 0.12 0 0\n"
				"\tpart roundbox subtract 0.05 0.52 1.55 -0.08 -20 39.14 0 1 1 0.15 0.22 0.17 0.17 0.04\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_7",
			"Placing the ground, backdrop, key-light, and hero objects.",
			{	{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname ground\n\tgeometry groundgeom\n\tmaterial ground\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname backdrop\n\tgeometry backdropgeom\n\tmaterial backdrop\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname key\n\tgeometry keygeom\n\tmaterial key_mat\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname hero\n\tgeometry herogeom\n\tposition 0 0 0\n\tmaterial teal\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_8", "Adding a dim cool omni fill light on the right side.",
			{ { "insert_chunk", InsertChunkInput( "omni_light\n{\n\tname fill\n\tpower 6.0\n\tposition 3.4 2.2 1.6\n\tcolor 0.55 0.62 0.78\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_9", "Rendering to check the composition.",
			{ { "render", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_10", "Looking at the render.",
			{ { "read_image", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_11", "Measuring against the reference photo before finishing.",
			{ { "compare_to_reference", CompareToReferenceInput( "view1", 24 ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_12",
			"Built the scene; hero is a flat cutout facing view1.", {}, "end_turn" ) );

		s.replayFixturePath = dir + "/wrong.fixture.jsonl";
		Check( WriteFile( s.replayFixturePath, fixture ),
			"image_reconstruct_single/flat-cutout control: fixture written to scratch" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			"image_reconstruct_single/flat-cutout control: run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			"image_reconstruct_single/flat-cutout control: one check result per committed checkpoint" );
		Check( !r.allPassed, "image_reconstruct_single/flat-cutout control: allPassed is FALSE against a flat-cutout hero" );
		// The load-bearing assertion: the withheld-view relaxed-cap compares
		// (checkpoints 1-3 = view2/view3/view4) must each fail with an RMSE
		// detail. view1's outcome (checkpoint 0) is deliberately NOT asserted --
		// a crude billboard tuned to face view1 may or may not clear the strict
		// 0.015 cap there, and that is not the claim under test.
		if( r.checkpoints.size() > 3 ) {
			for( std::size_t i = 1; i <= 3; ++i ) {
				Check( !r.checkpoints[i].passed,
					"image_reconstruct_single/flat-cutout control: checkpoints[" + std::to_string( i ) +
					"] (withheld-view compare) failed (detail: " + r.checkpoints[i].detail + ")" );
				Check( r.checkpoints[i].detail.find( "RMSE" ) != std::string::npos,
					"image_reconstruct_single/flat-cutout control: checkpoints[" + std::to_string( i ) +
					"] detail names RMSE (detail: " + r.checkpoints[i].detail + ")" );
			}
		}
		// P2 isolation assertions: with the full required workflow now present
		// (read_skill -> ... -> render -> read_image -> end_turn), this control
		// is an isolated "only geometry is wrong" probe -- every non-render
		// checkpoint must PASS, so the render compares above are the ONLY
		// failures. checkpoints[4] (objectmap expectGeometryKind) and [5]
		// (document chunk_count) stay green because herogeom is still a single
		// sdf_geometry chunk (only its part lines changed, per the class
		// comment on expectGeometryKind: a bas-relief-class hero would pass the
		// same way, since it too is sdf_geometry -- this control just uses the
		// squash-billboard construction instead, see CALIBRATION.md Sec 11).
		if( r.checkpoints.size() > 7 ) {
			Check( r.checkpoints[4].passed,
				"image_reconstruct_single/flat-cutout control: checkpoints[4] (objectmap expectGeometryKind) still PASSES -- herogeom is still sdf_geometry (detail: " +
				r.checkpoints[4].detail + ")" );
			Check( r.checkpoints[5].passed,
				"image_reconstruct_single/flat-cutout control: checkpoints[5] (document sdf chunk_count) still PASSES (detail: " +
				r.checkpoints[5].detail + ")" );
			Check( r.checkpoints[6].passed,
				"image_reconstruct_single/flat-cutout control: checkpoints[6] (diagnostics) still PASSES (detail: " +
				r.checkpoints[6].detail + ")" );
			Check( r.checkpoints[7].passed,
				"image_reconstruct_single/flat-cutout control: checkpoints[7] (trajectory) PASSES now that the fixture follows the full required workflow (detail: " +
				r.checkpoints[7].detail + ")" );
		}
	}

	// (k2) image_reconstruct_single: the PARTIAL-NECESSITY WITNESS for the
	// withheld-view tier (CALIBRATION.md Sec 11 anchor (g), 2026-07-17). The
	// hero keeps the GT's 4 SDF parts byte-for-byte UNCHANGED and appends a
	// 5th part: a `box intersect` half-space cut whose local +Z axis exactly
	// faces view1's camera (ex=-13.63, ey=39.14 -- the closed-form solve, NOT
	// control (k)'s searched ex=-20), positioned so the cut plane sits 0.20
	// units behind the hero centroid along the camera axis -- a bas-relief
	// that preserves view1's visible surface, silhouette, and curvature
	// exactly and removes only far-side volume. Measured (64 spp, Sec 11):
	// view1 0.01132, view2 0.02520, view3 0.06937, view4 0.01479. Against the
	// two-tier caps that means view1 PASSES (24.5% headroom under its strict
	// 0.015 cap), view2 PASSES (37% under the relaxed 0.04 cap), view4 PASSES
	// (63% under it), and ONLY view3 FAILS (73% ABOVE the 0.04 cap) -- i.e.
	// WITHOUT the withheld-view gates this wrong-volume reconstruction would
	// pass the ENTIRE eval, and view3's gate alone rejects it. That is the
	// measured partial-necessity proof for the withheld tier, re-rendered in
	// CI; the PASS margins (view1 24.5%, view2 37%, view4 63%) all sit well
	// above the ~0.002 run-to-run MC noise scale implied by CALIBRATION.md's
	// Sec 3 noise floors (0.0039-0.0045 total vs a shared 256-spp reference),
	// so if this control ever flakes, suspect a pipeline change, not noise.
	// Workflow shape is identical to (k): read_skill -> read_document ->
	// batched inserts -> draft render -> read_image -> compare_to_reference ->
	// final text, so the trajectory/objectmap/document/diagnostics checkpoints
	// all PASS and the view3 compare is the ONLY failure.
	{
		const std::string dir = ScratchRunDir( "t_adv_image_reconstruct_bas_relief" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( "evals/scenarios/image_reconstruct_single.json", s, err ),
			"image_reconstruct_single/bas-relief control: committed scenario loads (" + err + ")" );

		std::string fixture;
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_1", "Reading the modeling-from-image-captures skill before touching the scene.",
			{ { "read_skill", ( []{
				JsonValue in = JsonValue::MakeObject();
				in.set( "name", JsonValue::MakeString( "modeling-from-image-captures" ) );
				return in; } )() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_2", "Checking the starting scaffold.",
			{ { "read_document", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_3",
			"Adding the four albedo/exitance painters (hero teal, ground, backdrop, warm key -- only the "
			"hero's hidden-side volume is wrong here), and recolouring the scaffold's placeholder pnt_env "
			"painter to the observed dim blue-grey environment tint.",
			{	{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_teal\n\tcolor 0.05 0.55 0.5\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_ground\n\tcolor 0.62 0.62 0.64\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_backdrop\n\tcolor 0.30 0.31 0.34\n}" ) },
				{ "insert_chunk", InsertChunkInput( "uniformcolor_painter\n{\n\tname pnt_key\n\tcolor 1.0 0.82 0.58\n}" ) },
				// pnt_env is NOT inserted -- the scaffold already declares it (wired
				// into the rasterizer's radiance_map); recolour the placeholder.
				{ "propose_patch", ProposePatchInput( "pnt_env", "color", "0.06 0.08 0.11", false ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_4",
			"Adding the lambertian materials for the hero/ground/backdrop, plus the key light's luminaire material.",
			{	{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname teal\n\treflectance pnt_teal\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname ground\n\treflectance pnt_ground\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_material\n{\n\tname backdrop\n\treflectance pnt_backdrop\n}" ) },
				{ "insert_chunk", InsertChunkInput( "lambertian_luminaire_material\n{\n\tname key_mat\n\texitance pnt_key\n\tscale 42.0\n\tmaterial none\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_5",
			"Adding the ground plane, backdrop plane, and key-light quad geometry.",
			{	{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname groundgeom\n\tpta 7 0 -7\n\tptb -7 0 -7\n\tptc -7 0 7\n\tptd 7 0 7\n}" ) },
				{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname backdropgeom\n\tpta -7 0 -4.5\n\tptb -7 7 -4.5\n\tptc 7 7 -4.5\n\tptd 7 0 -4.5\n}" ) },
				{ "insert_chunk", InsertChunkInput( "clippedplane_geometry\n{\n\tname keygeom\n\tpta -2.1 3.6 1.9\n\tptb -2.1 3.6 0.7\n\tptc -0.9 3.6 0.7\n\tptd -0.9 3.6 1.9\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_6",
			"The hero SDF -- BAS-RELIEF variant: the GT's 4 parts unchanged, plus a box-intersect half-space "
			"cut facing view1's camera (ex=-13.63, ey=39.14) that removes the far-side volume 0.20 units "
			"behind the hero centroid (CALIBRATION.md Sec 11 anchor (g), verbatim part lines).",
			{ { "insert_chunk", InsertChunkInput(
				"sdf_geometry\n{\n\tname herogeom\n"
				"\tpart capsule union 0 0 0.75 0 0 0 0 1 1 1 0.30 0.45 0 0\n"
				"\tpart sphere smin 0.15 0 1.50 0 0 0 0 1 0.62 1 0.62 0 0 0\n"
				"\tpart torus smin 0.12 0 1.20 0 0 0 0 1 1 1 0.36 0.12 0 0\n"
				"\tpart roundbox subtract 0.05 0.52 1.55 -0.08 0 0 0 1 1 1 0.22 0.17 0.17 0.04\n"
				"\tpart box intersect 0 1.7161 1.6000 2.1118 -13.63 39.14 0 1 1 1 3 3 3 0\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_7",
			"Placing the ground, backdrop, key-light, and hero objects.",
			{	{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname ground\n\tgeometry groundgeom\n\tmaterial ground\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname backdrop\n\tgeometry backdropgeom\n\tmaterial backdrop\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname key\n\tgeometry keygeom\n\tmaterial key_mat\n}" ) },
				{ "insert_chunk", InsertChunkInput( "standard_object\n{\n\tname hero\n\tgeometry herogeom\n\tposition 0 0 0\n\tmaterial teal\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_8", "Adding a dim cool omni fill light on the right side.",
			{ { "insert_chunk", InsertChunkInput( "omni_light\n{\n\tname fill\n\tpower 6.0\n\tposition 3.4 2.2 1.6\n\tcolor 0.55 0.62 0.78\n}" ) } },
			"tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_9", "Draft check of the composition.",
			{ { "render", ( []{
				JsonValue in = JsonValue::MakeObject();
				in.set( "quality", JsonValue::MakeString( "draft" ) );
				in.set( "width", JsonValue::MakeNumber( 128 ) );
				in.set( "height", JsonValue::MakeNumber( 128 ) );
				return in; } )() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_10", "Looking at the draft.",
			{ { "read_image", EmptyInput() } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_11", "Measuring against the reference photo before finishing.",
			{ { "compare_to_reference", CompareToReferenceInput( "view1", 24 ) } }, "tool_use" ) );
		fixture += JsonlLine( "anthropic", AnthropicBody( "msg_12",
			"Built the scene; the hero's visible surface matches the photo, with a plausible-looking "
			"(but wrong) hidden side.", {}, "end_turn" ) );

		s.replayFixturePath = dir + "/wrong.fixture.jsonl";
		Check( WriteFile( s.replayFixturePath, fixture ),
			"image_reconstruct_single/bas-relief control: fixture written to scratch" );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus != "load_error",
			"image_reconstruct_single/bas-relief control: run did not load_error (" + h.result.errorMessage + ")" );

		AgentEvalCheckResult r = CheckScenario( h, s );
		Check( r.checkpoints.size() == s.checkpoints.size(),
			"image_reconstruct_single/bas-relief control: one check result per committed checkpoint" );
		Check( !r.allPassed, "image_reconstruct_single/bas-relief control: allPassed is FALSE against the bas-relief hero" );
		// The partial-necessity witness assertions: every OTHER gate waves this
		// wrong-volume reconstruction through -- view1's strict cap (24.5%
		// headroom), view2's and view4's relaxed caps (37% / 63% headroom) all
		// PASS -- and view3's withheld gate ALONE rejects it (73% above its
		// 0.04 cap), with an RMSE detail.
		if( r.checkpoints.size() > 3 ) {
			Check( r.checkpoints[0].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[0] (view1 STRICT compare) PASSES -- the front surface is exact (detail: " +
				r.checkpoints[0].detail + ")" );
			Check( r.checkpoints[1].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[1] (view2 compare) PASSES under the relaxed cap (detail: " +
				r.checkpoints[1].detail + ")" );
			Check( !r.checkpoints[2].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[2] (view3 compare) FAILS -- the ONE gate that catches the missing volume (detail: " +
				r.checkpoints[2].detail + ")" );
			Check( r.checkpoints[2].detail.find( "RMSE" ) != std::string::npos,
				"image_reconstruct_single/bas-relief control: checkpoints[2] detail names RMSE (detail: " +
				r.checkpoints[2].detail + ")" );
			Check( r.checkpoints[3].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[3] (view4 compare) PASSES under the relaxed cap (detail: " +
				r.checkpoints[3].detail + ")" );
		}
		// Isolation: the non-render checkpoints all PASS (full required
		// workflow present; herogeom is still one sdf_geometry chunk; clean
		// diagnostics) -- the view3 compare is the ONLY failing checkpoint.
		if( r.checkpoints.size() > 7 ) {
			Check( r.checkpoints[4].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[4] (objectmap expectGeometryKind) PASSES -- the bas-relief hero is still sdf_geometry (detail: " +
				r.checkpoints[4].detail + ")" );
			Check( r.checkpoints[5].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[5] (document sdf chunk_count) PASSES (detail: " +
				r.checkpoints[5].detail + ")" );
			Check( r.checkpoints[6].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[6] (diagnostics) PASSES (detail: " +
				r.checkpoints[6].detail + ")" );
			Check( r.checkpoints[7].passed,
				"image_reconstruct_single/bas-relief control: checkpoints[7] (trajectory) PASSES (detail: " +
				r.checkpoints[7].detail + ")" );
		}
		// GRADED-tier admission (CALIBRATION.md Sec 12): checkpoints 8-11 (the
		// appended weight-0.5 graded compares, caps 0.105/0.12/0.15/0.115) all
		// PASS for the bas-relief -- its measured RMSEs (view1 0.01132, view2
		// 0.02520, view3 0.06937, view4 0.01479) sit well under every graded
		// cap, INCLUDING view3's 0.06937, which fails the structural relaxed
		// 0.04 cap (checkpoints[2] above) but clears the graded tier's looser
		// 0.15 cap with room to spare. This is exactly the graded tier's design
		// intent: it admits engaged-but-imperfect volume work (a bas-relief
		// that nails three of four views and only misses hidden depth on the
		// most back-facing pose) rather than scoring it identically to an empty
		// stage on the progress meter, even though the STRUCTURAL gate (which
		// stays authoritative for allPassed/pass-fail) still correctly rejects
		// it.
		if( r.checkpoints.size() > 11 ) {
			for( std::size_t i = 8; i <= 11; ++i ) {
				Check( r.checkpoints[i].passed,
					"image_reconstruct_single/bas-relief control: checkpoints[" + std::to_string( i ) +
					"] (graded compare) PASSES under the graded cap (detail: " + r.checkpoints[i].detail + ")" );
			}
		}
	}
}

//----------------------------------------------------------------------
// T-ask(a): the ask_user SCRIPTED RESPONDER (stage 2 of the clarifying-
// questions feature -- AgentEvalAskUserResponses / ResolveScriptedAskUserAnswer
// in AgentEvalRunner.cpp).  Drives real RunScenario runs (not a hand-built
// envelope) and inspects the raw trajectory file for the SYNTHESIZED
// ask_user response, proving: first-match ARRAY ORDER wins over first-
// substring-POSITION-in-the-question; the substring match is case-
// INSENSITIVE; a question matching no entry falls back to "default" when
// present; and a question matching neither any entry NOR a default falls
// back to the STAGE-1 {available:false} stub byte-for-byte (both when the
// scenario carries no askUserResponses at all, AND when it carries one
// whose matches/default simply don't cover this question -- the "unit
// path" the design calls out explicitly).
//----------------------------------------------------------------------
static void TestAskUserScriptedResponderMatching()
{
	std::printf( "T-ask(a): ask_user scripted responder -- match order / case-insensitivity / default / fallback...\n" );
	const std::string dir = ScratchRunDir( "t_ask_responder" );

	// One ask_user round, then a plain end_turn -- the minimal shape every
	// sub-case below reuses (only the question and the scripted responder
	// differ).
	auto oneAskUserThenDone = [&]( const std::string& question ) -> std::string {
		const std::string r1 = AnthropicBody( "msg_1", "Asking before building.",
			{ { "ask_user", AskUserInput( question ) } }, "tool_use" );
		const std::string r2 = AnthropicBody( "msg_2", "Proceeding.", {}, "end_turn" );
		return JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 );
	};

	auto trajectoryText = [&]( const AgentEvalRunHandle& h ) -> std::string {
		std::ifstream f( h.trajectoryPath.c_str(), std::ios::binary );
		return std::string( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
	};

	// (a) FIRST-MATCH ARRAY ORDER wins, not first-substring-position in the
	// question text: matches[0] is "b" -> "B-answer", matches[1] is "a" ->
	// "A-answer"; the question contains BOTH substrings ("a" appears
	// earlier in the text than "b"), so the array-order rule must still
	// pick "B-answer" (matches[0]) over "A-answer" (matches[1]).
	{
		AgentEvalScenario s = MakeScenario( "ask_order", kScene, "Ask something",
			"commit", oneAskUserThenDone( "does this contain a and b" ), dir, "[]" );
		AgentEvalAskUserMatch m0; m0.contains = "b"; m0.answer = "B-answer";
		AgentEvalAskUserMatch m1; m1.contains = "a"; m1.answer = "A-answer";
		s.askUserResponses.matches.push_back( m0 );
		s.askUserResponses.matches.push_back( m1 );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "ask_order: run reached final_text" );
		const std::string traj = trajectoryText( h );
		Check( traj.find( "B-answer" ) != std::string::npos,
			"ask_order: matches[0] (\"b\") wins -- the response carries \"B-answer\"" );
		Check( traj.find( "A-answer" ) == std::string::npos,
			"ask_order: matches[1] (\"a\") did NOT win despite its substring appearing earlier in the question text" );
	}

	// (b) CASE-INSENSITIVE substring match: the match entry's "contains" is
	// lowercase ("color") but the question carries it UPPERCASE ("COLOR").
	{
		AgentEvalScenario s = MakeScenario( "ask_case", kScene, "Ask something",
			"commit", oneAskUserThenDone( "What COLOR should it be?" ), dir, "[]" );
		AgentEvalAskUserMatch m; m.contains = "color"; m.answer = "Blue-scripted-answer";
		s.askUserResponses.matches.push_back( m );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "ask_case: run reached final_text" );
		const std::string traj = trajectoryText( h );
		Check( traj.find( "Blue-scripted-answer" ) != std::string::npos,
			"ask_case: lowercase \"contains\":\"color\" matches an UPPERCASE \"COLOR\" in the question (case-insensitive)" );
	}

	// (c) DEFAULT FALLBACK: the question matches no entry, but a default is
	// present -- the default answer is used, not the stage-1 stub.
	{
		AgentEvalScenario s = MakeScenario( "ask_default", kScene, "Ask something",
			"commit", oneAskUserThenDone( "Totally unrelated question with no scripted keyword" ), dir, "[]" );
		AgentEvalAskUserMatch m; m.contains = "color"; m.answer = "Blue-scripted-answer";
		s.askUserResponses.matches.push_back( m );
		s.askUserResponses.hasDefault    = true;
		s.askUserResponses.defaultAnswer = "Default-scripted-answer";

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "ask_default: run reached final_text" );
		const std::string traj = trajectoryText( h );
		Check( traj.find( "Default-scripted-answer" ) != std::string::npos,
			"ask_default: no match -> the \"default\" answer is used" );
		Check( traj.find( "Blue-scripted-answer" ) == std::string::npos,
			"ask_default: the non-matching \"color\" entry did NOT fire" );
	}

	// (d) NO-RESPONDER FALLBACK -- the stage-1 {available:false} stub is
	// preserved BYTE-FOR-BYTE, in both of its two reachable shapes: a
	// scenario that never sets askUserResponses at all (MakeScenario's
	// default-constructed AgentEvalAskUserResponses -- empty matches, no
	// default), AND a scenario whose askUserResponses is populated but
	// simply does not cover this particular question (a match list with no
	// hit and no default -- the "responder unit path" the design calls
	// out).  Both must reach the identical stage-1 note text.
	static const char* const kStage1Note =
		"ask_user: no interactive user is available in this run; use your best judgment and proceed";
	{
		AgentEvalScenario s = MakeScenario( "ask_no_responder", kScene, "Ask something",
			"commit", oneAskUserThenDone( "Any question at all" ), dir, "[]" );
		// s.askUserResponses left at its MakeScenario default (empty).

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "ask_no_responder: run reached final_text" );
		const std::string traj = trajectoryText( h );
		Check( traj.find( kStage1Note ) != std::string::npos,
			"ask_no_responder: a scenario with NO askUserResponses gets the stage-1 available:false stub, byte-for-byte" );
	}
	{
		AgentEvalScenario s = MakeScenario( "ask_no_match_no_default", kScene, "Ask something",
			"commit", oneAskUserThenDone( "A question about nothing the responder covers" ), dir, "[]" );
		AgentEvalAskUserMatch m; m.contains = "color"; m.answer = "Blue-scripted-answer";
		s.askUserResponses.matches.push_back( m );
		// no default set -- matches non-empty, but this question hits none of them.

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "ask_no_match_no_default: run reached final_text" );
		const std::string traj = trajectoryText( h );
		Check( traj.find( kStage1Note ) != std::string::npos,
			"ask_no_match_no_default: a non-matching question with no default ALSO falls back to the stage-1 stub" );
		Check( traj.find( "Blue-scripted-answer" ) == std::string::npos,
			"ask_no_match_no_default: the non-matching \"color\" entry did NOT fire" );
	}
}

//----------------------------------------------------------------------
// T-ask(b): LoadEvalScenario's LOUD validation of the "askUserResponses"
// root-level field -- mirrors TestLoadEvalScenarioStrictCheckpointFieldTypes's
// pattern (a hand-built scenario JSON, wrong-typed/malformed field FAILS
// loudly naming the field; a corrected sibling loads cleanly), but at the
// SCENARIO-ROOT level (askUserResponses is not a checkpoint field), plus
// the three new "trajectory" checkpoint fields (askUserMin/askUserMax/
// askUserBeforeMutation), which ARE checkpoint fields and so are added to
// the existing writeScenario-style helper instead.
//----------------------------------------------------------------------
static void TestAskUserLoaderValidation()
{
	std::printf( "T-ask(b): LoadEvalScenario validates askUserResponses / askUser* checkpoint fields LOUDLY...\n" );
	const std::string dir = ScratchRunDir( "t_ask_loader" );

	// A minimal, otherwise-valid scenario root as a JsonValue -- callers
	// mutate/attach an "askUserResponses" field (root level) or a
	// "checkpoints" array (askUserMin/Max/BeforeMutation) before writing.
	auto baseRoot = [&]( const std::string& id ) -> JsonValue {
		JsonValue root = JsonValue::MakeObject();
		root.set( "id", JsonValue::MakeString( id ) );
		root.set( "title", JsonValue::MakeString( id ) );
		JsonValue scene = JsonValue::MakeObject();
		scene.set( "inline", JsonValue::MakeString( kScene ) );
		root.set( "scene", scene );
		JsonValue prompts = JsonValue::MakeArray();
		prompts.push_back( JsonValue::MakeString( "do something" ) );
		root.set( "prompts", prompts );
		return root;
	};
	auto writeRoot = [&]( const std::string& id, JsonValue root ) -> std::string {
		const std::string path = dir + "/" + id + ".json";
		WriteFile( path, JsonSerialize( root ) );
		return path;
	};

	// askUserResponses must be an OBJECT.
	{
		JsonValue root = baseRoot( "aur_not_object" );
		root.set( "askUserResponses", JsonValue::MakeString( "nope" ) );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_not_object", root ), s, err ),
			"askUserResponses as a bare string FAILS to load" );
		Check( err.find( "askUserResponses" ) != std::string::npos,
			"the load error names \"askUserResponses\" (got: " + err + ")" );
	}
	// askUserResponses.matches must be an ARRAY.
	{
		JsonValue root = baseRoot( "aur_matches_not_array" );
		JsonValue aur = JsonValue::MakeObject();
		aur.set( "matches", JsonValue::MakeString( "nope" ) );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_matches_not_array", root ), s, err ),
			"askUserResponses.matches as a bare string FAILS to load" );
		Check( err.find( "matches" ) != std::string::npos,
			"the load error names \"matches\" (got: " + err + ")" );
	}
	// askUserResponses.matches[i] must be an OBJECT.
	{
		JsonValue root = baseRoot( "aur_match_not_object" );
		JsonValue aur = JsonValue::MakeObject();
		JsonValue matches = JsonValue::MakeArray();
		matches.push_back( JsonValue::MakeString( "nope" ) );
		aur.set( "matches", matches );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_match_not_object", root ), s, err ),
			"askUserResponses.matches[0] as a bare string FAILS to load" );
		Check( err.find( "matches[0]" ) != std::string::npos,
			"the load error names \"matches[0]\" (got: " + err + ")" );
	}
	// askUserResponses.matches[i].contains must be a NON-EMPTY string.
	{
		JsonValue root = baseRoot( "aur_match_empty_contains" );
		JsonValue aur = JsonValue::MakeObject();
		JsonValue matches = JsonValue::MakeArray();
		JsonValue m = JsonValue::MakeObject();
		m.set( "contains", JsonValue::MakeString( "" ) );
		m.set( "answer", JsonValue::MakeString( "some answer" ) );
		matches.push_back( m );
		aur.set( "matches", matches );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_match_empty_contains", root ), s, err ),
			"askUserResponses.matches[0].contains as an EMPTY string FAILS to load" );
		Check( err.find( "contains" ) != std::string::npos,
			"the load error names \"contains\" (got: " + err + ")" );
	}
	// askUserResponses.matches[i].answer missing entirely.
	{
		JsonValue root = baseRoot( "aur_match_missing_answer" );
		JsonValue aur = JsonValue::MakeObject();
		JsonValue matches = JsonValue::MakeArray();
		JsonValue m = JsonValue::MakeObject();
		m.set( "contains", JsonValue::MakeString( "color" ) );
		matches.push_back( m );
		aur.set( "matches", matches );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_match_missing_answer", root ), s, err ),
			"askUserResponses.matches[0] missing \"answer\" FAILS to load" );
		Check( err.find( "answer" ) != std::string::npos,
			"the load error names \"answer\" (got: " + err + ")" );
	}
	// askUserResponses.default must be a NON-EMPTY string.
	{
		JsonValue root = baseRoot( "aur_default_wrong_type" );
		JsonValue aur = JsonValue::MakeObject();
		aur.set( "default", JsonValue::MakeNumber( 7 ) );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeRoot( "aur_default_wrong_type", root ), s, err ),
			"askUserResponses.default as a NUMBER FAILS to load" );
		Check( err.find( "default" ) != std::string::npos,
			"the load error names \"default\" (got: " + err + ")" );
	}
	// The fully-correct sibling loads cleanly and round-trips into the
	// in-memory struct.
	{
		JsonValue root = baseRoot( "aur_good" );
		JsonValue aur = JsonValue::MakeObject();
		JsonValue matches = JsonValue::MakeArray();
		JsonValue m = JsonValue::MakeObject();
		m.set( "contains", JsonValue::MakeString( "color" ) );
		m.set( "answer", JsonValue::MakeString( "Blue" ) );
		matches.push_back( m );
		aur.set( "matches", matches );
		aur.set( "default", JsonValue::MakeString( "Whatever you think best." ) );
		root.set( "askUserResponses", aur );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( writeRoot( "aur_good", root ), s, err ),
			"a correctly-shaped askUserResponses loads (" + err + ")" );
		Check( s.askUserResponses.matches.size() == 1 &&
			s.askUserResponses.matches[0].contains == "color" && s.askUserResponses.matches[0].answer == "Blue",
			"the loaded scenario's askUserResponses.matches[0] round-trips correctly" );
		Check( s.askUserResponses.hasDefault && s.askUserResponses.defaultAnswer == "Whatever you think best.",
			"the loaded scenario's askUserResponses.default round-trips correctly" );
	}
	// A scenario that omits askUserResponses entirely loads with the
	// default-constructed (empty) responder.
	{
		JsonValue root = baseRoot( "aur_absent" );
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( writeRoot( "aur_absent", root ), s, err ),
			"a scenario with no askUserResponses field loads (" + err + ")" );
		Check( s.askUserResponses.matches.empty() && !s.askUserResponses.hasDefault,
			"askUserResponses defaults to empty/no-default when the field is absent" );
	}

	// --- the trajectory checkpoint's ask_user fields ---
	auto writeScenarioCps = [&]( const std::string& id, const std::string& checkpointsJson ) -> std::string {
		JsonValue root = baseRoot( id );
		JsonValue cps; std::string perr;
		Check( JsonParse( checkpointsJson, cps, perr ), id + ": checkpoints JSON itself parses" );
		root.set( "checkpoints", cps );
		return writeRoot( id, root );
	};

	// askUserMin as a STRING (not a number) fails loudly.
	{
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeScenarioCps( "bad_askusermin_type",
			"[{\"kind\":\"trajectory\",\"askUserMin\":\"1\"}]" ), s, err ),
			"wrong-typed askUserMin (string \"1\") FAILS to load" );
		Check( err.find( "askUserMin" ) != std::string::npos,
			"the load error names \"askUserMin\" (got: " + err + ")" );
	}
	// askUserMin negative fails loudly (a call COUNT can never be negative).
	{
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeScenarioCps( "bad_askusermin_negative",
			"[{\"kind\":\"trajectory\",\"askUserMin\":-1}]" ), s, err ),
			"negative askUserMin (-1) FAILS to load" );
		Check( err.find( "askUserMin" ) != std::string::npos,
			"the load error names \"askUserMin\" (got: " + err + ")" );
	}
	// askUserMax as a bool (not a number) fails loudly.
	{
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeScenarioCps( "bad_askusermax_type",
			"[{\"kind\":\"trajectory\",\"askUserMax\":true}]" ), s, err ),
			"wrong-typed askUserMax (bool true) FAILS to load" );
		Check( err.find( "askUserMax" ) != std::string::npos,
			"the load error names \"askUserMax\" (got: " + err + ")" );
	}
	// askUserBeforeMutation as a NUMBER (not a bool) fails loudly.
	{
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeScenarioCps( "bad_askuserbeforemutation_type",
			"[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":1}]" ), s, err ),
			"wrong-typed askUserBeforeMutation (number 1) FAILS to load" );
		Check( err.find( "askUserBeforeMutation" ) != std::string::npos,
			"the load error names \"askUserBeforeMutation\" (got: " + err + ")" );
	}
	// askUserQuestionContainsAny must be a non-empty string array: an empty
	// or wrong-typed vocabulary would make a relevance assertion vacuous.
	{
		AgentEvalScenario s; std::string err;
		Check( !LoadEvalScenario( writeScenarioCps( "bad_askuser_question_terms",
			"[{\"kind\":\"trajectory\",\"askUserQuestionContainsAny\":[]}]" ), s, err ),
			"empty askUserQuestionContainsAny FAILS to load" );
		Check( err.find( "askUserQuestionContainsAny" ) != std::string::npos,
			"the load error names \"askUserQuestionContainsAny\" (got: " + err + ")" );
	}
	// The correctly-typed siblings all load cleanly, together.
	{
		AgentEvalScenario s; std::string err;
		Check( LoadEvalScenario( writeScenarioCps( "good_askuser_trajectory_fields",
			"[{\"kind\":\"trajectory\",\"askUserMin\":1,\"askUserMax\":3,\"askUserBeforeMutation\":true,\"askUserQuestionContainsAny\":[\"piece\",\"object\"]}]" ), s, err ),
			"correctly-typed ask_user trajectory fields load together (" + err + ")" );
	}
}

//----------------------------------------------------------------------
// T-ask(c): the "trajectory" checkpoint kind's ask_user assertions --
// askUserMin/askUserMax (call-count bounds), askUserBeforeMutation
// (ordering), and askUserQuestionContainsAny (question relevance) -- each
// driven through a REAL RunScenario run and checked BOTH directions (a
// crafted-to-pass case actually passes; a crafted-to-fail sibling actually
// fails, with an actionable detail).
//----------------------------------------------------------------------
static void TestTrajectoryAskUserAssertions()
{
	std::printf( "T-ask(c): \"trajectory\" checkpoint askUserMin/askUserMax/askUserBeforeMutation/askUserQuestionContainsAny...\n" );
	const std::string dir = ScratchRunDir( "t_ask_trajectory" );

	auto checkOne = [&]( const AgentEvalRunHandle& h, const AgentEvalScenario& base, const std::string& cpJson,
	                      bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses" );
		AgentEvalScenario s2 = base; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
		} else Check( false, label + ": expected exactly one checkpoint result" );
	};

	// Run A: ONE ask_user call, BEFORE the (one) insert_chunk mutation --
	// the substrate for every PASS case below.
	{
		const std::string r1 = AnthropicBody( "msg_1", "Asking first.",
			{ { "ask_user", AskUserInput( "What colour should it be?" ) } }, "tool_use" );
		const std::string r2 = AnthropicBody( "msg_2", "Building now.",
			{ { "insert_chunk", InsertChunkInput(
				"uniformcolor_painter\n{\n\tname pnt_new\n\tcolor 0.2 0.3 0.4\n}" ) } }, "tool_use" );
		const std::string r3 = AnthropicBody( "msg_3", "Done.", {}, "end_turn" );
		AgentEvalScenario s = MakeScenario( "traj_ask_one_before", kScene, "Build it", "commit",
			JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ) + JsonlLine( "anthropic", r3 ), dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "traj_ask_one_before: run reached final_text" );
		Check( h.result.toolCalls == 2, "traj_ask_one_before: exactly 2 tool calls (ask_user, insert_chunk)" );

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMin\":1}]", true,
			"askUserMin:1 PASSES with exactly one ask_user call" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMin\":2}]", false,
			"askUserMin:2 FAILS with only one ask_user call" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMax\":1}]", true,
			"askUserMax:1 PASSES with exactly one ask_user call" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMax\":0}]", false,
			"askUserMax:0 FAILS with one ask_user call" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":true}]", true,
			"askUserBeforeMutation:true PASSES -- ask_user preceded the insert_chunk" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserQuestionContainsAny\":[\"colour\",\"color\"]}]", true,
			"askUserQuestionContainsAny PASSES case-insensitively when the question is relevant" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserQuestionContainsAny\":[\"piece\"]}]", false,
			"askUserQuestionContainsAny FAILS when the ask_user question lacks the required topic" );
	}

	// Run B: the mutation happens FIRST, ask_user only AFTER -- the RED
	// case for askUserBeforeMutation (still exactly one ask_user call, so
	// askUserMin/Max stay green -- isolates the failure to ordering).
	{
		const std::string r1 = AnthropicBody( "msg_1", "Building first.",
			{ { "insert_chunk", InsertChunkInput(
				"uniformcolor_painter\n{\n\tname pnt_new2\n\tcolor 0.2 0.3 0.4\n}" ) } }, "tool_use" );
		const std::string r2 = AnthropicBody( "msg_2", "Asking afterward (too late).",
			{ { "ask_user", AskUserInput( "Did I get the colour right?" ) } }, "tool_use" );
		const std::string r3 = AnthropicBody( "msg_3", "Done.", {}, "end_turn" );
		AgentEvalScenario s = MakeScenario( "traj_ask_after_mutation", kScene, "Build it", "commit",
			JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ) + JsonlLine( "anthropic", r3 ), dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "traj_ask_after_mutation: run reached final_text" );

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":true}]", false,
			"askUserBeforeMutation:true FAILS -- the insert_chunk preceded the ask_user call" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMin\":1}]", true,
			"askUserMin:1 still PASSES (the ordering failure is isolated to askUserBeforeMutation)" );
	}

	// Run B2: the SAME build-before-asking violation, but the mutation is a
	// BATCH verb.  RED-PROVE target: a mutating verb missing from
	// kMutatingToolNames is read as NON-mutating, so this run looks like
	// "ask_user with zero mutations" and VACUOUSLY PASSES the very ordering
	// assertion the checkpoint exists to enforce -- a silent hole that makes
	// the grader more permissive without any scenario file changing.  Both
	// batch verbs are driven so adding one and forgetting the other is caught.
	{
		struct BatchMutation { const char* verb; JsonValue input; };
		const BatchMutation kBatchMutations[] = {
			{ "propose_patches", ProposePatchesInput( "pnt_albedo", "color", "0.2 0.3 0.4" ) },
			{ "insert_chunks",   [] { JsonValue arr = JsonValue::MakeArray();
			                          arr.push_back( JsonValue::MakeString(
			                              "uniformcolor_painter\n{\n\tname pnt_b2\n\tcolor 0.2 0.3 0.4\n}" ) );
			                          JsonValue in = JsonValue::MakeObject();
			                          in.set( "chunks", arr ); return in; }() },
		};
		for( const BatchMutation& bm : kBatchMutations ) {
			const std::string r1 = AnthropicBody( "msg_1", "Building first.",
				{ { bm.verb, bm.input } }, "tool_use" );
			const std::string r2 = AnthropicBody( "msg_2", "Asking afterward (too late).",
				{ { "ask_user", AskUserInput( "Did I get the colour right?" ) } }, "tool_use" );
			const std::string r3 = AnthropicBody( "msg_3", "Done.", {}, "end_turn" );
			AgentEvalScenario s2 = MakeScenario(
				std::string( "traj_ask_after_batch_" ) + bm.verb, kScene, "Build it", "commit",
				JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ) + JsonlLine( "anthropic", r3 ), dir, "[]" );
			AgentEvalRunOptions opts2; opts2.runDir = dir;
			AgentEvalRunHandle h2 = RunScenario( s2, opts2 );
			Check( h2.result.terminalStatus == "final_text",
			       std::string( "traj_ask_after_batch_" ) + bm.verb + ": run reached final_text" );

			checkOne( h2, s2, "[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":true}]", false,
				std::string( "askUserBeforeMutation:true FAILS -- a " ) + bm.verb +
				" before ask_user COUNTS as a mutation (a batch verb must not vacuously pass)" );
		}
	}

	// Run C: NO ask_user call at all -- the second RED case for
	// askUserBeforeMutation (nothing to precede the mutation with), and the
	// RED case for askUserMin.
	{
		const std::string r1 = AnthropicBody( "msg_1", "Building without asking.",
			{ { "insert_chunk", InsertChunkInput(
				"uniformcolor_painter\n{\n\tname pnt_new3\n\tcolor 0.2 0.3 0.4\n}" ) } }, "tool_use" );
		const std::string r2 = AnthropicBody( "msg_2", "Done.", {}, "end_turn" );
		AgentEvalScenario s = MakeScenario( "traj_ask_never", kScene, "Build it", "commit",
			JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ), dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "traj_ask_never: run reached final_text" );

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMin\":1}]", false,
			"askUserMin:1 FAILS -- zero ask_user calls occurred" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMax\":3}]", true,
			"askUserMax:3 still PASSES (zero calls is <= any positive max)" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":true}]", false,
			"askUserBeforeMutation:true FAILS -- no ask_user call occurred to precede the mutation with" );
	}

	// Run D: an ask_user call and NO document-mutating tool call at all
	// (ask_user + render only) -- the VACUOUS-PASS case: "the first
	// ask_user precedes the first mutation" is trivially satisfied when
	// there is no mutation to precede, so askUserBeforeMutation:true must
	// PASS here even though nothing was ever built.  This is intentional,
	// not a gap -- a scenario wanting "asked AND built something" pairs
	// askUserBeforeMutation with a separate chunk_count/build checkpoint
	// (see build_ambiguous_scene.json), so a build-nothing run still fails
	// overall on THAT checkpoint even though this one vacuously passes.
	{
		const std::string r1 = AnthropicBody( "msg_1", "Asking first.",
			{ { "ask_user", AskUserInput( "What colour should it be?" ) } }, "tool_use" );
		const std::string r2 = AnthropicBody( "msg_2", "Rendering -- nothing was ever built.",
			{ { "render", EmptyInput() } }, "tool_use" );
		const std::string r3 = AnthropicBody( "msg_3", "Done.", {}, "end_turn" );
		AgentEvalScenario s = MakeScenario( "traj_ask_no_mutation", kScene, "Build it", "commit",
			JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ) + JsonlLine( "anthropic", r3 ), dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", "traj_ask_no_mutation: run reached final_text" );
		Check( h.result.toolCalls == 2, "traj_ask_no_mutation: exactly 2 tool calls (ask_user, render -- no mutation)" );

		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserBeforeMutation\":true}]", true,
			"askUserBeforeMutation:true VACUOUSLY PASSES -- an ask_user call occurred and there is no "
			"mutation for it to fail to precede" );
		checkOne( h, s, "[{\"kind\":\"trajectory\",\"askUserMin\":1}]", true,
			"askUserMin:1 still PASSES (the ask_user call itself is real, just nothing mutated afterward)" );
	}
}

//----------------------------------------------------------------------
// T-ask(d): ScenarioContentHash sensitivity to askUserResponses.
// ScenarioContentHash itself is file-local (anonymous namespace) to
// AgentEvalRunner.cpp, so this drives it BLACK-BOX through the one place
// it is externally observable: the "scenarioContentHash" field
// RunScenarioDriven stamps into <runDir>/<id>.result.jsonl (see
// AgentEvalRunner.cpp's result-summary write, ~RunScenarioDriven).  Two
// scenarios identical in every other respect but for ONE scripted
// answer's TEXT must stamp DIFFERENT hashes; the identical scenario run
// twice must stamp the SAME hash (a sanity control against a flaky/
// nondeterministic canonicalization).
//----------------------------------------------------------------------
static void TestScenarioContentHashAskUserResponsesSensitivity()
{
	std::printf( "T-ask(d): ScenarioContentHash is sensitive to askUserResponses...\n" );
	const std::string dir = ScratchRunDir( "t_ask_hash" );

	auto stampedHash = [&]( const std::string& id, const std::string& answer ) -> std::string {
		AgentEvalScenario s = MakeScenario( id, kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalAskUserMatch m; m.contains = "color"; m.answer = answer;
		s.askUserResponses.matches.push_back( m );

		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( !h.resultPath.empty(), id + ": run wrote a result.jsonl path" );

		std::ifstream f( h.resultPath.c_str(), std::ios::binary );
		std::string body( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
		JsonValue v; std::string perr;
		Check( JsonParse( body, v, perr ), id + ": result.jsonl parses as JSON (" + perr + ")" );
		Check( v.has( "scenarioContentHash" ) && v.get( "scenarioContentHash" ).isString(),
			id + ": result.jsonl carries a string \"scenarioContentHash\" field" );
		return v.get( "scenarioContentHash" ).asString();
	};

	const std::string hashA1 = stampedHash( "hash_probe_a1", "Blue-answer" );
	const std::string hashA2 = stampedHash( "hash_probe_a2", "Blue-answer" );
	const std::string hashB  = stampedHash( "hash_probe_b",  "Green-answer" );

	Check( !hashA1.empty(), "hash_probe_a1: stamped hash is non-empty" );
	Check( hashA1 == hashA2,
		"identical askUserResponses (same answer text) -> IDENTICAL scenarioContentHash (sanity control)" );
	Check( hashA1 != hashB,
		"changing ONE scripted answer's text -> DIFFERENT scenarioContentHash" );

	// Control: a scenario with NO askUserResponses at all also hashes
	// differently from one that carries a responder -- proves the field is
	// folded into the canonicalization unconditionally, not just when two
	// non-empty responders differ.
	{
		AgentEvalScenario sNone = MakeScenario( "hash_probe_none", kScene, "Recolor", "commit", kParamEditFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( sNone, opts );
		std::ifstream f( h.resultPath.c_str(), std::ios::binary );
		std::string body( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
		JsonValue v; std::string perr;
		Check( JsonParse( body, v, perr ), "hash_probe_none: result.jsonl parses as JSON (" + perr + ")" );
		const std::string hashNone = v.get( "scenarioContentHash" ).asString();
		Check( hashNone != hashA1,
			"a scenario with NO askUserResponses hashes differently from one with a responder" );
	}
}

//----------------------------------------------------------------------
// T-ask(e): ADVERSARIAL CONTROL for the committed build_ambiguous_scene
// scenario, mirroring TestAdversarialOracleControls's pattern -- loads the
// REAL committed scenario (so the checkpoints under test are the actual
// shipped ones), swaps in a wrong-behavior fixture that builds the SAME
// kind of scene but NEVER calls ask_user, and asserts the trajectory
// checkpoint (askUserMin) is the SPECIFIC one that fails while the build-
// completeness checkpoints (chunk_count/render/objectmap/diagnostics)
// still pass -- proving the oracle discriminates on ASKING, not on
// whether a reasonable scene got built.
//----------------------------------------------------------------------
static void TestAdversarialControlNeverAsksStillBuilds()
{
	std::printf( "T-ask(e): build_ambiguous_scene adversarial control -- never asking FAILS askUserMin, build checkpoints still pass...\n" );

	// The SAME proven watch+table+window recipe evals/fixtures/
	// build_watch_scene.fixture.jsonl uses (same scaffold: film 32x24,
	// pinhole at z=3.5, fov 40 -- byte-identical here) -- three
	// standard_objects (clears chunk_count's min:3) and a single white
	// window light at the SAME position/scale already proven to land the
	// render checkpoint's meanLuma/channelBalance band on this exact
	// scaffold, rather than a freshly-guessed light rig that might miss the
	// band for reasons unrelated to what THIS control is testing (asking).
	std::vector<ToolCallSpec> chunks;
	chunks.push_back( { "insert_chunks", [](){
		JsonValue chunksArr = JsonValue::MakeArray();
		static const char* const kChunks[] = {
			"uniformcolor_painter\n{\n\tname pnt_guess\n\tcolor 0.85 0.68 0.3\n}",
			"lambertian_material\n{\n\tname mat_guess\n\treflectance pnt_guess\n}",
			"cylinder_geometry\n{\n\tname geo_guess\n\taxis z\n\tradius 0.35\n\theight 0.15\n\tcapped TRUE\n}",
			"standard_object\n{\n\tname obj_guess\n\tgeometry geo_guess\n\tmaterial mat_guess\n\tposition 0 0 0\n}",
			"uniformcolor_painter\n{\n\tname pnt_guess_table\n\tcolor 0.62 0.63 0.66\n}",
			"lambertian_material\n{\n\tname mat_guess_table\n\treflectance pnt_guess_table\n}",
			"box_geometry\n{\n\tname geo_guess_table\n\twidth 3.4\n\theight 0.2\n\tdepth 1.8\n}",
			"standard_object\n{\n\tname obj_guess_table\n\tgeometry geo_guess_table\n\tmaterial mat_guess_table\n\tposition 0 -0.55 0\n}",
			"uniformcolor_painter\n{\n\tname pnt_guess_emit\n\tcolor 1.0 1.0 1.0\n}",
			"lambertian_luminaire_material\n{\n\tname mat_guess_emit\n\texitance pnt_guess_emit\n\tscale 9.0\n\tmaterial none\n}",
			"clippedplane_geometry\n{\n\tname geo_guess_emit\n\tpta -1.0 1.0 3.5\n\tptb 1.0 1.0 3.5\n\tptc 1.0 -1.0 3.5\n\tptd -1.0 -1.0 3.5\n}",
			"standard_object\n{\n\tname obj_guess_emit\n\tgeometry geo_guess_emit\n\tmaterial mat_guess_emit\n}",
		};
		for( const char* c : kChunks ) chunksArr.push_back( JsonValue::MakeString( c ) );
		JsonValue in = JsonValue::MakeObject();
		in.set( "chunks", chunksArr );
		return in;
	}() } );

	const std::string r1 = AnthropicBody( "msg_1", "Reading first.", { { "read_document", EmptyInput() } }, "tool_use" );
	const std::string r2 = AnthropicBody( "msg_2",
		"Building a guessed display piece straight away -- no clarifying question asked.",
		chunks, "tool_use" );
	const std::string r3 = AnthropicBody( "msg_3", "Rendering.", { { "render", EmptyInput() } }, "tool_use" );
	const std::string r4 = AnthropicBody( "msg_4",
		"Built a display scene for a guessed collection piece and rendered it.", {}, "end_turn" );
	const std::string wrongFixture =
		JsonlLine( "anthropic", r1 ) + JsonlLine( "anthropic", r2 ) +
		JsonlLine( "anthropic", r3 ) + JsonlLine( "anthropic", r4 );

	const std::string dir = ScratchRunDir( "t_adv_never_asks" );
	AgentEvalScenario s; std::string err;
	Check( LoadEvalScenario( "evals/scenarios/build_ambiguous_scene.json", s, err ),
		"build_ambiguous_scene: committed scenario loads (" + err + ")" );

	s.replayFixturePath = dir + "/wrong.fixture.jsonl";
	Check( WriteFile( s.replayFixturePath, wrongFixture ), "never-asks fixture written to scratch" );

	AgentEvalRunOptions opts; opts.runDir = dir;
	AgentEvalRunHandle h = RunScenario( s, opts );
	Check( h.result.terminalStatus != "load_error", "never-asks run did not load_error (" + h.result.errorMessage + ")" );
	Check( h.result.toolCalls == 3, "never-asks run dispatched exactly 3 tool calls (read_document, insert_chunks, render -- no ask_user)" );

	AgentEvalCheckResult r = CheckScenario( h, s );
	Check( r.checkpoints.size() == s.checkpoints.size(), "one check result per committed checkpoint" );
	Check( !r.allPassed, "allPassed is FALSE against the never-asks fixture" );

	// Find the trajectory checkpoint (its index is an implementation
	// detail of the scenario file, not hard-coded here) and assert it is
	// the ONE that failed, naming askUserMin specifically.
	bool foundTrajectory = false;
	for( std::size_t i = 0; i < r.checkpoints.size(); ++i ) {
		if( r.checkpoints[i].kind != "trajectory" ) continue;
		foundTrajectory = true;
		Check( !r.checkpoints[i].passed,
			"never-asks: the trajectory checkpoint FAILS (detail: " + r.checkpoints[i].detail + ")" );
		Check( r.checkpoints[i].detail.find( "askUserMin" ) != std::string::npos,
			"never-asks: the failing detail names askUserMin specifically (detail: " + r.checkpoints[i].detail + ")" );
	}
	Check( foundTrajectory, "build_ambiguous_scene carries a trajectory checkpoint to fail" );

	// Every OTHER checkpoint (chunk_count/render/objectmap/diagnostics)
	// still PASSES -- the oracle is discriminating on ASKING, not on
	// whether a plausible scene got built.  finalText is exempted: the
	// wrong-behavior fixture's guessed build never says "watch"/"pocket"
	// (it never learned the piece identity), so that checkpoint is
	// EXPECTED to also fail here -- it is testing the SAME underlying
	// defect (never asked -> can't possibly reflect the answer), not a
	// second, unrelated failure.
	for( std::size_t i = 0; i < r.checkpoints.size(); ++i ) {
		if( r.checkpoints[i].kind == "trajectory" || r.checkpoints[i].kind == "finalText" ) continue;
		Check( r.checkpoints[i].passed,
			"never-asks: checkpoints[" + std::to_string( i ) + "] (" + r.checkpoints[i].kind +
			") -- a plausible guessed build still passes the type-agnostic build checkpoints (detail: " +
			r.checkpoints[i].detail + ")" );
	}
}

//----------------------------------------------------------------------
// T-mr: material-richness P0's three new "document" ops --
// distinct_chunk_kinds / any_param_references_kind / no_orphan_chunks
// (docs/agentic-redesign/73-creative-richness-design.md).  Proves BOTH
// directions for each op against crafted documents, plus the loader's
// hard-fail-fast validation of malformed op configs.  Every scene here
// is read-only (kReadThenDoneFixture -- one read_document, no model
// edit), so the checked document is the scene TEXT verbatim.
//----------------------------------------------------------------------

// Clean baseline: ZERO orphan painter-category chunks, and THREE distinct
// non-uniformcolor/non-scalar painter kinds (perlin3d_painter,
// worley3d_painter, expression_function2d -- the last is the counterexample
// to "every Painter-category keyword ends in _painter", exercised here via
// "category":"painter" rather than a suffix match).  A scalar_painter
// (sp_rough) is bound into mat_ggx's alphax/alphay (the physical-scalar
// pipe, per materials-and-media-basics.md).
static const char* const kMaterialRichnessScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_b\n\tcolor 0.1 0.1 0.1\n}\n\n"
	"perlin3d_painter\n{\n\tname pnt_perlin\n\tcolora pnt_a\n\tcolorb pnt_b\n}\n\n"
	"worley3d_painter\n{\n\tname pnt_worley\n\tcolora pnt_a\n\tcolorb pnt_b\n\toutput f1\n}\n\n"
	"lambertian_material\n{\n\tname mat_wall\n\treflectance pnt_perlin\n}\n\n"
	"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_worley\n}\n\n"
	"box_geometry\n{\n\tname geo_wall\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"box_geometry\n{\n\tname geo_floor\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry geo_wall\n\tmaterial mat_wall\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n\tposition 2 0 0\n}\n\n"
	"expression_function2d\n{\n\tname fn_rough\n\tparam bands 4.0\n\tdef s sin( u * bands * tau )\n\texpr smoothstep( -0.3, 0.3, s )\n}\n\n"
	"scalar_painter\n{\n\tname sp_rough\n\tfunction2d fn_rough\n\tscale 0.4\n\tbias 0.02\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"ggx_material\n{\n\tname mat_ggx\n\trd pnt_a\n\trs pnt_spec\n\talphax sp_rough\n\talphay sp_rough\n\tior 1.5\n\textinction 0.0\n}\n\n"
	"box_geometry\n{\n\tname geo_ggx\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_ggx\n\tgeometry geo_ggx\n\tmaterial mat_ggx\n\tposition -2 0 0\n}\n";

// Same as kMaterialRichnessScene PLUS one deliberately UNREFERENCED
// uniformcolor_painter (pnt_orphan) -- the no_orphan_chunks FAIL case.
static const char* const kMaterialRichnessSceneWithOrphan =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_b\n\tcolor 0.1 0.1 0.1\n}\n\n"
	"perlin3d_painter\n{\n\tname pnt_perlin\n\tcolora pnt_a\n\tcolorb pnt_b\n}\n\n"
	"lambertian_material\n{\n\tname mat_wall\n\treflectance pnt_perlin\n}\n\n"
	"box_geometry\n{\n\tname geo_wall\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry geo_wall\n\tmaterial mat_wall\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_orphan\n\tcolor 0.3 0.2 0.1\n}\n";

// Same as kMaterialRichnessScene EXCEPT mat_ggx's alphax/alphay are INLINE
// numbers, not a reference to sp_rough -- sp_rough (and fn_rough) exist but
// have NO referrer.  The any_param_references_kind "exists but orphaned"
// FAIL case.
static const char* const kMaterialRichnessSceneOrphanScalar =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"expression_function2d\n{\n\tname fn_rough\n\tparam bands 4.0\n\tdef s sin( u * bands * tau )\n\texpr smoothstep( -0.3, 0.3, s )\n}\n\n"
	"scalar_painter\n{\n\tname sp_rough\n\tfunction2d fn_rough\n\tscale 0.4\n\tbias 0.02\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"ggx_material\n{\n\tname mat_ggx\n\trd pnt_a\n\trs pnt_spec\n\talphax 0.3\n\talphay 0.3\n\tior 1.5\n\textinction 0.0\n}\n\n"
	"box_geometry\n{\n\tname geo_ggx\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_ggx\n\tgeometry geo_ggx\n\tmaterial mat_ggx\n}\n";

// Uniformcolor-only: no scalar_painter, no other painter kind at all --
// distinct_chunk_kinds' "FAILS on uniformcolor-only doc" case and
// any_param_references_kind's "no such chunk exists" case.
static const char* const kFlatPainterScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_flat\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_flat\n\treflectance pnt_flat\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_flat\n\tgeometry sph\n\tmaterial mat_flat\n}\n";

// P1-a red-proof (review round 1): TWO worley3d_painter chunks (different
// names/params, both bound to their own object) -- distinct_chunk_kinds
// must count DISTINCT KEYWORDS (1: "worley3d_painter"), not raw filter-
// matching chunk COUNT (2).  Mutating distinctKinds.size() to matches.size()
// in the implementation must turn this scene's distinctMin:2 assertion from
// FAIL to PASS -- the mutation probe this scene exists to drive red.
static const char* const kDuplicateKindScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_b\n\tcolor 0.1 0.1 0.1\n}\n\n"
	"worley3d_painter\n{\n\tname pnt_worley1\n\tcolora pnt_a\n\tcolorb pnt_b\n\toutput f1\n}\n\n"
	"worley3d_painter\n{\n\tname pnt_worley2\n\tcolora pnt_b\n\tcolorb pnt_a\n\toutput f2\n}\n\n"
	"lambertian_material\n{\n\tname mat_1\n\treflectance pnt_worley1\n}\n\n"
	"lambertian_material\n{\n\tname mat_2\n\treflectance pnt_worley2\n}\n\n"
	"box_geometry\n{\n\tname geo_1\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"box_geometry\n{\n\tname geo_2\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_1\n\tgeometry geo_1\n\tmaterial mat_1\n}\n\n"
	"standard_object\n{\n\tname obj_2\n\tgeometry geo_2\n\tmaterial mat_2\n\tposition 2 0 0\n}\n";

// Geometry-diversity PASS case (geometry scope expansion): box_geometry +
// sphere_geometry + sdf_geometry -- 3 distinct geometry kinds (satisfies
// category:"geometry" distinctMin:3) with exactly one advanced-modeling
// kind present (satisfies kinds:[sdf_geometry,sweep_geometry,
// displaced_geometry] distinctMin:1).  The sdf_geometry part uses the
// documented roundcone field layout (skills/agent/object-modeling-
// recipes.md): `<prim> <op> <k>  <pos xyz>  <euler xyz deg>  <scale xyz>
// <a b c>  <round>`, a b c = r1 r2 h.
static const char* const kGeometryRichnessScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_a\n\treflectance pnt_a\n}\n\n"
	"box_geometry\n{\n\tname geo_box\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_box\n\tgeometry geo_box\n\tmaterial mat_a\n}\n\n"
	"sphere_geometry\n{\n\tname geo_sph\n\tradius 0.5\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry geo_sph\n\tmaterial mat_a\n\tposition 2 0 0\n}\n\n"
	"sdf_geometry\n{\n\tname geo_sdf\n\tpart roundcone union 0  0 0 0  0 0 0  1 1 1  0.3 0.2 0.4  0.0\n}\n\n"
	"standard_object\n{\n\tname obj_sdf\n\tgeometry geo_sdf\n\tmaterial mat_a\n\tposition -2 0 0\n}\n";

// P1-b red-proof (review round 1, the reviewer-proved single-decoy gaming
// pattern): obj_wall/obj_floor are the only VISIBLE surfaces and are
// uniformcolor-only; every diverse painter (worley3d_painter,
// perlin3d_painter) is bound ONLY to a hidden 0.001-unit decoy box at
// position 1000 1000 1000.  distinct_chunk_kinds/no_orphan_chunks both
// PASS on this document (the painters exist and are bound to SOMETHING) --
// objects_reaching_kinds is the op that catches it (only 1 standard_object,
// the decoy itself, reaches a qualifying painter; the 2 visible objects
// reach only excluded uniformcolor_painter chunks).
static const char* const kDecoyScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_vis\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_vis\n\treflectance pnt_vis\n}\n\n"
	"box_geometry\n{\n\tname geo_wall\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_wall\n\tgeometry geo_wall\n\tmaterial mat_vis\n}\n\n"
	"box_geometry\n{\n\tname geo_floor\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_floor\n\tgeometry geo_floor\n\tmaterial mat_vis\n\tposition 2 0 0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_decoy_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_decoy_b\n\tcolor 0.1 0.1 0.1\n}\n\n"
	"worley3d_painter\n{\n\tname pnt_decoy_worley\n\tcolora pnt_decoy_a\n\tcolorb pnt_decoy_b\n\toutput f1\n}\n\n"
	"perlin3d_painter\n{\n\tname pnt_decoy_perlin\n\tcolora pnt_decoy_a\n\tcolorb pnt_decoy_b\n}\n\n"
	"ggx_material\n{\n\tname mat_decoy\n\trd pnt_decoy_worley\n\trs pnt_decoy_perlin\n\talphax 0.3\n\talphay 0.3\n\tior 1.5\n\textinction 0.0\n}\n\n"
	"box_geometry\n{\n\tname geo_decoy\n\twidth 0.001\n\theight 0.001\n\tdepth 0.001\n}\n\n"
	"standard_object\n{\n\tname obj_decoy\n\tgeometry geo_decoy\n\tmaterial mat_decoy\n\tposition 1000 1000 1000\n}\n";

// review-round-2 P3 (both reviewers): objects_reaching_kinds is only exercised
// above on object->material->painter (2-hop) chains.  This scene makes the
// ONLY qualifying painter (pnt_worley_deep, a worley3d_painter) reachable
// SOLELY via a THIRD hop through an intermediate painter-referencing-painter
// chunk: obj_deep -> mat_deep -> pnt_blend (blend_painter) -> pnt_worley_deep.
// pnt_blend is itself Painter-category but is excluded from the filter below
// alongside uniformcolor_painter/scalar_painter, so a BFS that stops
// expanding after the first hop past the root (material) would find nothing
// qualifying -- it must walk past the intermediate blend_painter to reach the
// worley3d_painter one hop further out.  blend_painter's mask carries the
// qualifying reference (per ChunkParserRegistry.cpp: "out = colora * mask +
// colorb * (1 - mask)"); colora/colorb are flat uniformcolor_painter chunks
// that carry no signal here.
static const char* const kDeepChainScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_a\n\tcolor 0.6 0.6 0.6\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_b\n\tcolor 0.1 0.1 0.1\n}\n\n"
	"worley3d_painter\n{\n\tname pnt_worley_deep\n\tcolora pnt_a\n\tcolorb pnt_b\n\toutput f1\n}\n\n"
	"blend_painter\n{\n\tname pnt_blend\n\tcolora pnt_a\n\tcolorb pnt_b\n\tmask pnt_worley_deep\n}\n\n"
	"lambertian_material\n{\n\tname mat_deep\n\treflectance pnt_blend\n}\n\n"
	"box_geometry\n{\n\tname geo_deep\n\twidth 1\n\theight 1\n\tdepth 1\n}\n\n"
	"standard_object\n{\n\tname obj_deep\n\tgeometry geo_deep\n\tmaterial mat_deep\n}\n";

static void TestMaterialRichnessCheckpoints()
{
	std::printf( "T-mr: material-richness P0 -- distinct_chunk_kinds / any_param_references_kind / no_orphan_chunks...\n" );
	const std::string dir = ScratchRunDir( "t_mr_richness" );

	auto makeHandle = [&]( const char* id, const char* sceneText ) -> AgentEvalRunHandle {
		AgentEvalScenario s = MakeScenario( id, sceneText, "Read only", "commit", kReadThenDoneFixture, dir, "[]" );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle h = RunScenario( s, opts );
		Check( h.result.terminalStatus == "final_text", std::string( id ) + ": run reached final_text" );
		Check( h.dispatcher != nullptr, std::string( id ) + ": run has a live dispatcher" );
		return h;
	};

	const AgentEvalRunHandle hClean        = makeHandle( "mr_clean", kMaterialRichnessScene );
	const AgentEvalRunHandle hOrphanPainter = makeHandle( "mr_orphan_painter", kMaterialRichnessSceneWithOrphan );
	const AgentEvalRunHandle hOrphanScalar  = makeHandle( "mr_orphan_scalar", kMaterialRichnessSceneOrphanScalar );
	const AgentEvalRunHandle hFlat          = makeHandle( "mr_flat", kFlatPainterScene );
	const AgentEvalRunHandle hDupKind       = makeHandle( "mr_dup_kind", kDuplicateKindScene );
	const AgentEvalRunHandle hGeomDiverse   = makeHandle( "mr_geom_diverse", kGeometryRichnessScene );
	const AgentEvalRunHandle hDecoy         = makeHandle( "mr_decoy", kDecoyScene );
	const AgentEvalRunHandle hDeepChain     = makeHandle( "mr_deep_chain", kDeepChainScene );

	auto checkAgainst = [&]( const AgentEvalRunHandle& h, const std::string& cpJson, bool expectPass, const std::string& label ) {
		JsonValue cps; std::string err;
		Check( JsonParse( cpJson, cps, err ), label + ": checkpoint JSON parses (" + err + ")" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( h, s2 );
		Check( r.checkpoints.size() == 1, label + ": exactly one checkpoint result" );
		if( r.checkpoints.size() == 1 ) {
			Check( r.checkpoints[0].passed == expectPass,
				label + ": passed==" + std::string( expectPass ? "true" : "false" ) +
				" (detail: " + r.checkpoints[0].detail + ")" );
			Check( !r.checkpoints[0].detail.empty(), label + ": detail is never empty" );
		}
	};

	// ---- distinct_chunk_kinds ----

	// PASSES on a doc with (at least) 2 distinct qualifying painters --
	// kMaterialRichnessScene carries 3 (perlin3d_painter, worley3d_painter,
	// expression_function2d).  "category":"painter" is used (not
	// "kindSuffix") because expression_function2d is Painter-category
	// without a "_painter" suffix -- a suffix filter would silently drop it.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":2}]",
		true, "distinct_chunk_kinds: 3 distinct qualifying painters >= distinctMin:2 PASSES" );

	// FAILS on a uniformcolor-only doc (0 distinct qualifying painters).
	checkAgainst( hFlat,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":2}]",
		false, "distinct_chunk_kinds: uniformcolor-only doc (0 distinct) FAILS" );

	// FAILS when distinct kinds < distinctMin: excluding perlin3d_painter and
	// worley3d_painter too leaves ONLY expression_function2d (count 1),
	// under distinctMin:2.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\",\"perlin3d_painter\",\"worley3d_painter\"],\"distinctMin\":2}]",
		false, "distinct_chunk_kinds: count 1 < distinctMin:2 FAILS" );

	// Exclusion actually excludes: the SAME document, same distinctMin:1,
	// PASSES without the extra exclusions (count 3) and FAILS with them
	// (count 1) -- proving "exclude" changes the outcome, not just present.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":3}]",
		true, "distinct_chunk_kinds: distinctMin:3 PASSES without narrowing exclusions (count 3)" );
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\",\"perlin3d_painter\",\"worley3d_painter\"],\"distinctMin\":3}]",
		false, "distinct_chunk_kinds: the SAME distinctMin:3 FAILS once exclude narrows the count to 1" );

	// distinctMax: the count (3) exceeding an explicit ceiling FAILS.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1,\"distinctMax\":2}]",
		false, "distinct_chunk_kinds: count 3 > distinctMax:2 FAILS" );

	// distinctMax: an EXACTLY-AT-CEILING count PASSES (review round 2, P2-1)
	// -- the inclusive-upper-bound edge.  Mutating "count > distinctMax" to
	// "count >= distinctMax" in AgentEvalRunner.cpp (~line 4684) would flip
	// this to FAIL while every other distinct_chunk_kinds assertion above
	// stays green (none of them sit exactly on the ceiling).  hClean's
	// qualifying count is exactly 3 (confirmed by the metricValue==3
	// assertion below), so distinctMax:3 must still PASS.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1,\"distinctMax\":3}]",
		true, "distinct_chunk_kinds: count 3 == distinctMax:3 PASSES (inclusive upper bound)" );

	// metricValue is populated with the exact distinct count.
	{
		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
			"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1}]", cps, err ),
			"distinct_chunk_kinds metricValue: checkpoint JSON parses" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( hClean, s2 );
		Check( r.checkpoints.size() == 1 && r.checkpoints[0].hasMetricValue,
			"distinct_chunk_kinds: hasMetricValue is true" );
		if( r.checkpoints.size() == 1 )
			Check( r.checkpoints[0].metricValue == 3.0,
				"distinct_chunk_kinds: metricValue == 3 (got " + std::to_string( r.checkpoints[0].metricValue ) + ")" );
		// Default metricLabel (no explicit "metricLabel" in the checkpoint JSON
		// above) is the op name.
		if( r.checkpoints.size() == 1 )
			Check( r.checkpoints[0].metricLabel == "distinct_chunk_kinds",
				"distinct_chunk_kinds: default metricLabel == op name (got '" + r.checkpoints[0].metricLabel + "')" );
	}

	// P1-a (review round 1): distinct_chunk_kinds counts DISTINCT KEYWORDS,
	// not raw filter-matching chunk COUNT.  kDuplicateKindScene carries TWO
	// worley3d_painter chunks (pnt_worley1, pnt_worley2 -- different names,
	// different params, both bound) and nothing else qualifying -- the
	// distinct count is 1, so distinctMin:2 must FAIL even though the raw
	// matching-chunk count is 2.  A distinctMin:1 sibling on the SAME
	// document must PASS, proving this isn't a filter-collection bug.
	checkAgainst( hDupKind,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":2}]",
		false, "distinct_chunk_kinds: two worley3d_painter chunks are 1 distinct kind, FAILS distinctMin:2" );
	checkAgainst( hDupKind,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1}]",
		true, "distinct_chunk_kinds: two worley3d_painter chunks still PASS distinctMin:1 (the filter itself finds them)" );
	{
		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
			"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1}]", cps, err ),
			"distinct_chunk_kinds dup-kind metricValue: checkpoint JSON parses" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( hDupKind, s2 );
		Check( r.checkpoints.size() == 1 && r.checkpoints[0].hasMetricValue && r.checkpoints[0].metricValue == 1.0,
			"distinct_chunk_kinds: metricValue == 1 on the duplicate-kind scene (DISTINCTNESS, not match count -- got " +
			( r.checkpoints.size() == 1 ? std::to_string( r.checkpoints[0].metricValue ) : std::string( "<no result>" ) ) + ")" );
	}

	// ---- geometry-diversity (scope expansion) ----

	// FAILS: kMaterialRichnessScene's geometry is box_geometry-only (3
	// chunks, 1 distinct kind) -- a box-monoculture scene must fail
	// distinctMin:3 under category:"geometry".
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":3}]",
		false, "distinct_chunk_kinds: all-box geometry (1 distinct kind) FAILS distinctMin:3 category:geometry" );

	// PASSES: kGeometryRichnessScene mixes box_geometry + sphere_geometry +
	// sdf_geometry -- 3 distinct kinds satisfies distinctMin:3.
	checkAgainst( hGeomDiverse,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":3}]",
		true, "distinct_chunk_kinds: box+sphere+sdf (3 distinct kinds) PASSES distinctMin:3 category:geometry" );

	// The advanced-geometry "kinds" list: PASSES when the scene carries the
	// ONE sdf_geometry chunk; FAILS on the box-only scene that carries none
	// of sdf_geometry/sweep_geometry/displaced_geometry.
	checkAgainst( hGeomDiverse,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\","
		"\"kinds\":[\"sdf_geometry\",\"sweep_geometry\",\"displaced_geometry\"],\"distinctMin\":1}]",
		true, "distinct_chunk_kinds: kinds-list PASSES with one sdf_geometry present" );
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\","
		"\"kinds\":[\"sdf_geometry\",\"sweep_geometry\",\"displaced_geometry\"],\"distinctMin\":1}]",
		false, "distinct_chunk_kinds: kinds-list FAILS with none of sdf/sweep/displaced present" );

	// ---- objects_reaching_kinds (review round 1, P1-b anti-decoy-gaming) ----

	// FAILS on the decoy scene: only obj_decoy (the hidden 0.001-unit box)
	// reaches a qualifying (non-uniform, non-scalar) painter -- the two
	// VISIBLE objects (obj_wall, obj_floor) reach only excluded
	// uniformcolor_painter chunks.  reachingCount 1 < min:2.
	checkAgainst( hDecoy,
		"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"min\":2}]",
		false, "objects_reaching_kinds: decoy scene (1 qualifying root) FAILS min:2" );

	// The op's count semantics, asserted directly: metricValue == 1 on the
	// decoy scene (the decoy itself, and only the decoy).
	{
		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
			"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"min\":0}]", cps, err ),
			"objects_reaching_kinds decoy metricValue: checkpoint JSON parses" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( hDecoy, s2 );
		Check( r.checkpoints.size() == 1 && r.checkpoints[0].hasMetricValue && r.checkpoints[0].metricValue == 1.0,
			"objects_reaching_kinds: metricValue == 1 on the decoy scene (got " +
			( r.checkpoints.size() == 1 ? std::to_string( r.checkpoints[0].metricValue ) : std::string( "<no result>" ) ) + ")" );
		Check( r.checkpoints.size() == 1 && r.checkpoints[0].metricLabel == "objects_reaching_kinds",
			"objects_reaching_kinds: default metricLabel == op name" );
	}

	// PASSES on the honest scene: kMaterialRichnessScene's obj_wall reaches
	// pnt_wall (perlin3d_painter, qualifying) and obj_floor reaches
	// pnt_floor (worley3d_painter, qualifying) -- 2 qualifying roots meets
	// min:2.  (obj_ggx reaches only excluded uniformcolor_painter/
	// scalar_painter chunks, so this also proves the closure walk doesn't
	// over-count.)
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"min\":2}]",
		true, "objects_reaching_kinds: honest scene (2 qualifying roots: obj_wall, obj_floor) PASSES min:2" );

	// max: exceeding an explicit ceiling FAILS (the honest scene has exactly 2).
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"min\":0,\"max\":1}]",
		false, "objects_reaching_kinds: count 2 > max:1 FAILS" );

	// max: an EXACTLY-AT-CEILING count PASSES (review round 2, P2-1) -- the
	// inclusive-upper-bound edge.  Mutating "reachingCount > maxCount" to
	// "reachingCount >= maxCount" in AgentEvalRunner.cpp (~line 4844) would
	// flip this to FAIL while every other objects_reaching_kinds assertion
	// above stays green (none of them sit exactly on the ceiling).  The
	// qualifying-root count under THIS filter is exactly 3, not 2 -- besides
	// obj_wall/obj_floor, obj_ggx's mat_ggx binds sp_rough (scalar_painter,
	// excluded, so not itself qualifying) whose "function2d" param in turn
	// references fn_rough (expression_function2d, Painter-category and NOT
	// excluded here), so obj_ggx's closure also reaches a qualifying chunk
	// one hop further out.  Confirmed at runtime via the "count 3 > max:2"
	// FAIL detail before landing this boundary at max:3.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"min\":3,\"max\":3}]",
		true, "objects_reaching_kinds: count 3 == max:3 PASSES (inclusive upper bound)" );

	// review-round-2 P3 (both reviewers): a 3-hop closure -- the qualifying
	// painter is reachable ONLY through an intermediate painter-referencing-
	// painter chunk (obj_deep -> mat_deep -> pnt_blend -> pnt_worley_deep).
	// pnt_blend (blend_painter) is itself excluded from the filter, so this
	// PASSES only if the BFS walks past it to the worley3d_painter one hop
	// further out.  reachingCount is exactly 1 (obj_deep is the only root).
	checkAgainst( hDeepChain,
		"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
		"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\",\"blend_painter\"],\"min\":1}]",
		true, "objects_reaching_kinds: 3-hop closure (obj->mat->blend_painter->worley3d_painter) PASSES min:1" );
	{
		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\","
			"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\",\"blend_painter\"],\"min\":0}]", cps, err ),
			"objects_reaching_kinds 3-hop metricValue: checkpoint JSON parses" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( hDeepChain, s2 );
		Check( r.checkpoints.size() == 1 && r.checkpoints[0].hasMetricValue && r.checkpoints[0].metricValue == 1.0,
			"objects_reaching_kinds: metricValue == 1 on the 3-hop scene (exactly obj_deep -- got " +
			( r.checkpoints.size() == 1 ? std::to_string( r.checkpoints[0].metricValue ) : std::string( "<no result>" ) ) + ")" );
	}

	// ---- any_param_references_kind ----

	// PASSES: sp_rough (a scalar_painter) is referenced by mat_ggx's
	// alphax/alphay.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"any_param_references_kind\",\"referencedKind\":\"scalar_painter\"}]",
		true, "any_param_references_kind: a referenced scalar_painter PASSES" );

	// FAILS: sp_rough exists in the document but nothing references it
	// (mat_ggx's alphax/alphay are inline numbers instead).
	checkAgainst( hOrphanScalar,
		"[{\"kind\":\"document\",\"op\":\"any_param_references_kind\",\"referencedKind\":\"scalar_painter\"}]",
		false, "any_param_references_kind: chunk exists but is orphaned FAILS" );

	// FAILS: no scalar_painter chunk exists in the document at all.
	checkAgainst( hFlat,
		"[{\"kind\":\"document\",\"op\":\"any_param_references_kind\",\"referencedKind\":\"scalar_painter\"}]",
		false, "any_param_references_kind: no such chunk exists FAILS" );

	// ---- no_orphan_chunks ----

	// PASSES: every painter-category chunk in kMaterialRichnessScene is
	// bound to something.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\",\"category\":\"painter\"}]",
		true, "no_orphan_chunks: every painter bound PASSES" );

	// FAILS: pnt_orphan is a painter chunk with zero referrers; the detail
	// names it.
	{
		JsonValue cps; std::string err;
		Check( JsonParse( "[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\",\"category\":\"painter\"}]", cps, err ),
			"no_orphan_chunks orphan-detail: checkpoint JSON parses" );
		AgentEvalScenario s2; s2.checkpoints = cps;
		AgentEvalCheckResult r = CheckScenario( hOrphanPainter, s2 );
		Check( r.checkpoints.size() == 1 && !r.checkpoints[0].passed,
			"no_orphan_chunks: one orphaned painter FAILS" );
		if( r.checkpoints.size() == 1 )
			Check( r.checkpoints[0].detail.find( "pnt_orphan" ) != std::string::npos,
				"no_orphan_chunks: the failing detail NAMES pnt_orphan (detail: " + r.checkpoints[0].detail + ")" );
	}

	// Vacuous PASS on zero matching chunks: "kinds" names a painter kind
	// that appears nowhere in the document.
	checkAgainst( hClean,
		"[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\",\"kinds\":[\"voronoi3d_painter\"]}]",
		true, "no_orphan_chunks: zero matching chunks vacuously PASSES" );

	// ---- Loader validation: malformed op configs are hard load errors ----

	auto writeAndLoad = [&]( const std::string& id, const std::string& checkpointsJson, std::string& err ) -> bool {
		JsonValue root = JsonValue::MakeObject();
		root.set( "id", JsonValue::MakeString( id ) );
		root.set( "title", JsonValue::MakeString( id ) );
		JsonValue scene = JsonValue::MakeObject();
		scene.set( "inline", JsonValue::MakeString( kMaterialRichnessScene ) );
		root.set( "scene", scene );
		JsonValue prompts = JsonValue::MakeArray();
		prompts.push_back( JsonValue::MakeString( "do something" ) );
		root.set( "prompts", prompts );
		JsonValue cps; std::string perr;
		Check( JsonParse( checkpointsJson, cps, perr ), id + ": checkpoints JSON itself parses" );
		root.set( "checkpoints", cps );
		const std::string path = dir + "/" + id + ".json";
		WriteFile( path, JsonSerialize( root ) );
		AgentEvalScenario s;
		return LoadEvalScenario( path, s, err );
	};

	// distinct_chunk_kinds: BOTH kindSuffix and category present -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_both_filters",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"kindSuffix\":\"_painter\",\"category\":\"painter\",\"distinctMin\":1}]", err ),
			"distinct_chunk_kinds: BOTH kindSuffix and category FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// distinct_chunk_kinds: NEITHER kindSuffix/kinds/category -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_no_filter",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"distinctMin\":1}]", err ),
			"distinct_chunk_kinds: no filter field FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// distinct_chunk_kinds: missing distinctMin -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_no_distinctmin",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\"}]", err ),
			"distinct_chunk_kinds: missing distinctMin FAILS to load" );
		Check( err.find( "distinctMin" ) != std::string::npos,
			"the load error names distinctMin (got: " + err + ")" );
	}
	// The correctly-shaped sibling still loads cleanly.
	{
		std::string err;
		Check( writeAndLoad( "mr_good_distinct",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":1}]", err ),
			"distinct_chunk_kinds: correctly-shaped checkpoint loads (" + err + ")" );
	}
	// distinct_chunk_kinds: NEGATIVE distinctMin -- refused (a negative floor
	// makes the checkpoint vacuously pass on any document).
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_negative_distinctmin",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":-1}]", err ),
			"distinct_chunk_kinds: distinctMin -1 FAILS to load" );
		Check( err.find( "never be negative" ) != std::string::npos,
			"the load error explains a count can never be negative (got: " + err + ")" );
	}
	// distinct_chunk_kinds: INVERTED band (max < min) -- refused (can never pass).
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_inverted_band",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":5,\"distinctMax\":1}]", err ),
			"distinct_chunk_kinds: distinctMax 1 < distinctMin 5 FAILS to load" );
		Check( err.find( "inverted band" ) != std::string::npos,
			"the load error explains the inverted band (got: " + err + ")" );
	}
	// no_orphan_chunks: BOTH kindSuffix and kinds present -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_orphan_both_filters",
			"[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\",\"kindSuffix\":\"_painter\",\"kinds\":[\"perlin3d_painter\"]}]", err ),
			"no_orphan_chunks: BOTH kindSuffix and kinds FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// no_orphan_chunks: NEITHER filter field -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_orphan_no_filter",
			"[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\"}]", err ),
			"no_orphan_chunks: no filter field FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// any_param_references_kind: missing referencedKind -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_no_referencedkind",
			"[{\"kind\":\"document\",\"op\":\"any_param_references_kind\"}]", err ),
			"any_param_references_kind: missing referencedKind FAILS to load" );
		Check( err.find( "referencedKind" ) != std::string::npos,
			"the load error names referencedKind (got: " + err + ")" );
	}
	// The correctly-shaped siblings still load cleanly.
	{
		std::string err;
		Check( writeAndLoad( "mr_good_orphan",
			"[{\"kind\":\"document\",\"op\":\"no_orphan_chunks\",\"kindSuffix\":\"_painter\"}]", err ),
			"no_orphan_chunks: correctly-shaped checkpoint loads (" + err + ")" );
		Check( writeAndLoad( "mr_good_anyref",
			"[{\"kind\":\"document\",\"op\":\"any_param_references_kind\",\"referencedKind\":\"scalar_painter\"}]", err ),
			"any_param_references_kind: correctly-shaped checkpoint loads (" + err + ")" );
	}

	// ---- objects_reaching_kinds: loader validation (review round 1) ----

	// BOTH kindSuffix and category present -- refused (shares the filter
	// validation with distinct_chunk_kinds/no_orphan_chunks).
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_reach_both_filters",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"kindSuffix\":\"_painter\",\"category\":\"painter\",\"min\":1}]", err ),
			"objects_reaching_kinds: BOTH kindSuffix and category FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// NEITHER kindSuffix/kinds/category -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_reach_no_filter",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"min\":1}]", err ),
			"objects_reaching_kinds: no filter field FAILS to load" );
		Check( err.find( "EXACTLY ONE" ) != std::string::npos,
			"the load error explains EXACTLY ONE filter is required (got: " + err + ")" );
	}
	// missing min -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_reach_no_min",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\"}]", err ),
			"objects_reaching_kinds: missing min FAILS to load" );
		Check( err.find( "\"min\"" ) != std::string::npos,
			"the load error names min (got: " + err + ")" );
	}
	// NEGATIVE min -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_reach_negative_min",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\",\"min\":-1}]", err ),
			"objects_reaching_kinds: min -1 FAILS to load" );
		Check( err.find( "never be negative" ) != std::string::npos,
			"the load error explains a count can never be negative (got: " + err + ")" );
	}
	// INVERTED band (max < min) -- refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_reach_inverted_band",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\",\"min\":5,\"max\":1}]", err ),
			"objects_reaching_kinds: max 1 < min 5 FAILS to load" );
		Check( err.find( "inverted band" ) != std::string::npos,
			"the load error explains the inverted band (got: " + err + ")" );
	}
	// The correctly-shaped sibling still loads cleanly (with an explicit
	// rootKind override too, proving that field is accepted).
	{
		std::string err;
		Check( writeAndLoad( "mr_good_reach",
			"[{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\",\"rootKind\":\"standard_object\",\"min\":1,\"max\":10}]", err ),
			"objects_reaching_kinds: correctly-shaped checkpoint loads (" + err + ")" );
	}

	// ---- metricLabel: cross-checkpoint dedupe guard (geometry scope expansion) ----

	// Two distinct_chunk_kinds checkpoints, NEITHER carrying an explicit
	// "metricLabel" -- both default to the op name "distinct_chunk_kinds",
	// collide, and the scenario FAILS to load.  The error names metricLabel.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_label_default_collision",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":1},"
			"{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":1}]", err ),
			"metricLabel: two distinct_chunk_kinds checkpoints with NO metricLabel (both default-collide) FAILS to load" );
		Check( err.find( "metricLabel" ) != std::string::npos,
			"the load error names metricLabel (got: " + err + ")" );
	}
	// Two distinct_chunk_kinds checkpoints with the SAME EXPLICIT metricLabel
	// -- also a collision, also refused.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_label_explicit_collision",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":1,\"metricLabel\":\"richness\"},"
			"{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":1,\"metricLabel\":\"richness\"}]", err ),
			"metricLabel: two distinct_chunk_kinds checkpoints with the SAME explicit metricLabel FAILS to load" );
		Check( err.find( "metricLabel" ) != std::string::npos,
			"the load error names metricLabel (got: " + err + ")" );
	}
	// Cross-op collision: distinct_chunk_kinds (default label
	// "distinct_chunk_kinds") + objects_reaching_kinds EXPLICITLY relabeled
	// to the same string -- still a collision (the dedupe guard is op-
	// agnostic, keyed on the EFFECTIVE label, not the op name).
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_label_cross_op_collision",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":1},"
			"{\"kind\":\"document\",\"op\":\"objects_reaching_kinds\",\"category\":\"painter\",\"min\":1,\"metricLabel\":\"distinct_chunk_kinds\"}]", err ),
			"metricLabel: distinct_chunk_kinds (default label) vs objects_reaching_kinds (relabeled to the same string) FAILS to load" );
		Check( err.find( "metricLabel" ) != std::string::npos,
			"the load error names metricLabel (got: " + err + ")" );
	}
	// Two DISTINCT explicit labels load cleanly, and BOTH checkpoints carry
	// their own metricValue/metricLabel through CheckScenario -- proving the
	// dedupe guard doesn't over-fire and the label actually threads through.
	{
		std::string err;
		const bool loaded = writeAndLoad( "mr_good_two_labels",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
			"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1,\"metricLabel\":\"painter_kinds\"},"
			"{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":1,\"metricLabel\":\"geometry_kinds\"}]", err );
		Check( loaded, "metricLabel: two DISTINCT explicit labels load cleanly (" + err + ")" );
		if( loaded ) {
			JsonValue cps; std::string perr;
			Check( JsonParse(
				"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\","
				"\"exclude\":[\"uniformcolor_painter\",\"scalar_painter\"],\"distinctMin\":1,\"metricLabel\":\"painter_kinds\"},"
				"{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"geometry\",\"distinctMin\":1,\"metricLabel\":\"geometry_kinds\"}]",
				cps, perr ), "metricLabel: two-label checkpoint JSON re-parses for the runtime check" );
			AgentEvalScenario s2; s2.checkpoints = cps;
			AgentEvalCheckResult r = CheckScenario( hClean, s2 );
			Check( r.checkpoints.size() == 2, "metricLabel: two checkpoint results" );
			if( r.checkpoints.size() == 2 ) {
				Check( r.checkpoints[0].hasMetricValue && r.checkpoints[0].metricLabel == "painter_kinds",
					"metricLabel: checkpoint[0] carries label 'painter_kinds' (got '" + r.checkpoints[0].metricLabel + "')" );
				Check( r.checkpoints[1].hasMetricValue && r.checkpoints[1].metricLabel == "geometry_kinds",
					"metricLabel: checkpoint[1] carries label 'geometry_kinds' (got '" + r.checkpoints[1].metricLabel + "')" );
			}
		}
	}
	// A wrong-TYPED metricLabel (a number, not a string) is a hard load
	// error on ANY checkpoint kind, not just "document" -- it is validated
	// generically in ValidateCheckpointFieldTypes.
	{
		std::string err;
		Check( !writeAndLoad( "mr_bad_label_wrong_type",
			"[{\"kind\":\"document\",\"op\":\"distinct_chunk_kinds\",\"category\":\"painter\",\"distinctMin\":1,\"metricLabel\":5}]", err ),
			"metricLabel: a numeric metricLabel FAILS to load" );
		Check( err.find( "metricLabel" ) != std::string::npos,
			"the load error names metricLabel (got: " + err + ")" );
	}
}

int main()
{
	std::printf( "=== AgentEvalCheckTest (Eval-harness slice E3: the checker engine) ===\n" );

	TestScratchRootDiffersAcrossProcesses();
	TestDocumentCheckpoint();
	TestUntouchedCheckpoint();
	TestRenderCheckpoint();
	TestRenderSamplesPinNotDefeatable();
	TestRenderDimsPinNotDefeatable();
	TestObjectmapCheckpoint();
	TestDiagnosticsCheckpointClean();
	TestDiagnosticsLiveDocInvariant();
	TestTrajectoryCheckpoint();
	TestTrajectoryNewAssertions();
	TestToolOutcomesArgsContainsAndConflict();
	TestToolCallAfterUserTurnArrayForm();
	TestDocumentAnyOfKind();
	TestParamSeriesOrbit();
	TestProposalCheckpoint();
	TestRenderChannelBalance();
	TestRenderCameraCompareToImage();
	TestCompareToImageOversizedReferenceIsRefusedCheaply();
	TestFinalTextCheckpoint();
	TestUnknownKindAndMalformedShape();
	TestPartialCreditArithmetic();
	TestScenarioIntervention();
	TestSeedScenariosCheckpointsAreTrue();
	TestAdversarialOracleControls();
	TestBudgetLlmCallsNotBypassedByRetry();
	TestResultsLedgerRecordsTerminalStatus();
	TestLoadEvalScenarioStrictCheckpointFieldTypes();
	TestTerminalSuccessGuard();
	TestAskUserScriptedResponderMatching();
	TestAskUserLoaderValidation();
	TestTrajectoryAskUserAssertions();
	TestScenarioContentHashAskUserResponsesSensitivity();
	TestAdversarialControlNeverAsksStillBuilds();
	TestMaterialRichnessCheckpoints();

	std::printf( "=== AgentEvalCheckTest: %d passed, %d failed ===\n", g_pass, g_fail );

	// Leave the repo/TMPDIR clean: remove this process's ENTIRE scratch
	// tree (every ScratchRunDir() leaf lives under it), not just the last
	// leaf touched.
	{
		std::error_code ec;
		std::filesystem::remove_all( ScratchTestRoot(), ec );
	}

	return g_fail == 0 ? 0 : 1;
}
