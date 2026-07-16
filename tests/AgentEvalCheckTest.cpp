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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
		sPass.prompts.push_back( "Now recolor the sphere to red." );
		AgentEvalRunOptions opts; opts.runDir = dir;
		AgentEvalRunHandle hPass = RunScenario( sPass, opts );
		Check( hPass.result.toolCalls == 2, "tcaut_pass: read_document + propose_patch dispatched" );
		checkOne( hPass, sPass,
			"[{\"kind\":\"trajectory\",\"toolCallAfterUserTurn\":{\"name\":\"propose_patch\",\"minUserTurns\":2}}]",
			true, "toolCallAfterUserTurn: propose_patch after the 2nd user turn PASSES minUserTurns:2" );

		AgentEvalScenario sFail = MakeScenario( "tcaut_fail", kScene, "Recolor the sphere to red.",
			"commit", kToolCallAfterUserTurnFailFixture, dir, "[]" );
		sFail.prompts.push_back( "Now just re-read the scene." );
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
// budget/refusal test).  All 16 current scenarios assert
// terminalStatus:"final_text", so a passing run already ends final_text
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
	s.prompts.push_back( "Now recolor the sphere to red." );
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
	TestTrajectoryNewAssertions();
	TestToolOutcomesArgsContainsAndConflict();
	TestToolCallAfterUserTurnArrayForm();
	TestDocumentAnyOfKind();
	TestParamSeriesOrbit();
	TestProposalCheckpoint();
	TestRenderChannelBalance();
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

	std::printf( "=== AgentEvalCheckTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
