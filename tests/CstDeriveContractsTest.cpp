//////////////////////////////////////////////////////////////////////
//
//  CstDeriveContractsTest.cpp - CST-ONLY DeriveToJob contract coverage
//  recovered from the two tests Slice 6c-3b deleted with the legacy-vs-CST
//  equivalence oracle (CstDescriptorBindTest.cpp, CstDeriveDifferentialTest.cpp).
//
//  WHY THIS EXISTS
//  ---------------
//  6c-3b retired the legacy parser and the DumpJob-equivalence oracle, and in
//  doing so DELETED CstDescriptorBindTest + CstDeriveDifferentialTest wholesale.
//  Most of those two suites was the legacy arm (ParseLegacy(...) == CST derive) --
//  correctly retired.  But each held CST-ONLY blocks that never touched the
//  legacy parser and cover LIVE DeriveToJob contracts nothing else covers.  The
//  surviving golden (CstDeriveGoldenTest) captures only scenes that derive
//  CLEANLY, in a FRESH Job per scene -- so it structurally cannot exercise
//  (a) the refuse-all rejection path, nor (b) cross-derive parse-state reset.
//  This file recovers exactly those two CST-only contracts, [refuse-all] and
//  [statelessness] below.  Two LATER blocks, [apply-continuation] and
//  [scalar-pipe-default], are NOT recovered from the deleted originals -- they
//  are NEW regression coverage added by the 2026-08-28 bug-fix wave for
//  DeriveToJob's PASS-2 continue-past-failure change and the four
//  ISCALARPAINTER-class `"none"`-default fixes, added here because this file
//  is already the home for CST-only DeriveToJob apply-loop contracts nothing
//  else covers:
//
//    * [refuse-all]  DeriveToJob's PASS-1 structural rejection (Cst.cpp,
//      `DeriveToJob`'s PASS-1 chunk-validation loop: `if( !diags.empty() )
//      return 0;   // refuse-all: a malformed scene applies NOTHING` -- a
//      symbol/comment anchor rather than a line number, which has already
//      drifted once).  A malformed
//      chunk -- unknown chunk type, unknown parameter, non-finite value
//      (radius nan), non-numeric value (radius abc), or a value-less line
//      (radius with no value) -- refuses the WHOLE document and applies
//      NOTHING, so even a VALID sibling chunk in the same document is not
//      applied.  Recovered from CstDescriptorBindTest's `[validate]` block.
//
//    * [statelessness]  DeriveToJob calls ClearChunkParserState() at its start
//      (Cst.cpp, the `ClearChunkParserState();` call at the top of `DeriveToJob`
//      -- a symbol anchor rather than a line number, which has already drifted
//      once), resetting the chunk parsers' file-scope state BETWEEN
//      consecutive derives -- the redesign runs DeriveToJob repeatedly, once per
//      edit, in the SAME process.  Two file-scope leaks are guarded: the
//      uniformcolor_painter colour cache (translucent_material's energy-
//      conservation check reads it -> a leak injects spurious energy-auto-scaled
//      painters) and the camera name-dedup set (an unnamed camera auto-names
//      `default`; a leaked dedup set renames the second to `default_1`).
//      Recovered from CstDescriptorBindTest's `[state-isolation]` block and
//      CstDeriveDifferentialTest's `[cross]` / NoLeak cases.  The golden's
//      fresh-Job-per-scene shape never exercises this.
//
//  All legacy `ParseLegacy(...)` / DumpJob-equivalence arms of the originals are
//  deliberately NOT recovered -- they are the retired oracle.  Only the CST-only
//  contract assertions survive here, at the originals' full strength.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, IObject/manager interfaces, DumpJob

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Derive a scene through the CST path (ParseToCst -> DeriveToJob) into `job`.
static int DeriveCst( const std::string& scene, Job& job, std::vector<std::string>* diags = nullptr )
{
	Document d = ParseToCst( scene );
	return DeriveToJob( d, job, diags );
}

// Dump a scene derived through the CST path (fresh Job each call).
static std::string DumpCst( const std::string& scene )
{
	Job* j = new Job();
	std::vector<std::string> diags;
	DeriveCst( scene, *j, &diags );
	std::string s = DumpJob( *j );
	j->release();
	return s;
}

static const std::string HDR = "RISE ASCII SCENE 7\n";

int main()
{
	std::printf( "CstDeriveContractsTest -- CST-only DeriveToJob contracts (refuse-all + cross-derive statelessness)\n" );

	//----------------------------------------------------------------------
	// [refuse-all] descriptor-driven PASS-1 validation refuses a malformed scene
	// and applies NOTHING (refuse-all boundary) -- even a VALID sibling chunk in
	// the same document.  Recovered from CstDescriptorBindTest `[validate]`.
	//
	// A fresh Job is NOT empty: InitializeContainers() seeds "none" defaults in
	// some managers.  Refuse-all means the counts stay at this baseline.
	//----------------------------------------------------------------------
	std::printf( "[refuse-all] a malformed scene is refused whole -- applies NOTHING\n" );
	int baseGeo, basePnt;
	{ Job* b = new Job(); baseGeo = b->GetGeometries()->getItemCount(); basePnt = b->GetPainters()->getItemCount(); b->release(); }
	auto RefusesApplyingNothing = [&]( const std::string& s, const char* what ) {
		Job* j = new Job(); std::vector<std::string> diags; int n = DeriveCst( s, *j, &diags );
		bool refused = ( n == 0 ) && !diags.empty()
			&& j->GetGeometries()->getItemCount() == baseGeo
			&& j->GetPainters()->getItemCount()  == basePnt;   // valid sibling NOT applied
		Check( refused, what );
		j->release();
	};

	// unknown parameter, WITH a valid sibling painter that must NOT be applied
	// (the refuse-all boundary: a whole document is refused, not just the bad chunk).
	RefusesApplyingNothing(
		HDR +
		"sphere_geometry\n{\nname s\nradius 1\nbogus 5\n}\n"
		"uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n",
		"unknown parameter -> refuse-all (the valid sibling painter is NOT applied)" );
	// unknown chunk type, WITH a valid sibling geometry after it.
	RefusesApplyingNothing(
		HDR +
		"not_a_real_chunk\n{\nname x\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n",
		"unknown chunk type -> refuse-all" );
	// non-finite numeric value.
	RefusesApplyingNothing(
		HDR +
		"sphere_geometry\n{\nname s\nradius nan\n}\n",
		"non-finite numeric value (nan) -> refuse-all" );
	// non-numeric value for a numeric param.
	RefusesApplyingNothing(
		HDR +
		"sphere_geometry\n{\nname s\nradius abc\n}\n",
		"non-numeric value for a numeric param (abc) -> refuse-all" );
	// value-less parameter line (a key with no value).
	RefusesApplyingNothing(
		HDR +
		"sphere_geometry\n{\nname s\nradius\n}\n",
		"value-less parameter line (radius, no value) -> refuse-all" );

	// SELF-PROVING control: the SAME sibling document, but with the malformed
	// chunk made VALID, MUST apply cleanly (both chunks).  This flips every
	// refuse-all assertion above: if the derive did NOT actually refuse malformed
	// input, this control could not distinguish it.  Here `radius 1` (valid)
	// replaces `bogus 5` / `radius nan` / etc., and both entities appear.
	{
		Job* j = new Job(); std::vector<std::string> diags;
		int n = DeriveCst(
			HDR +
			"sphere_geometry\n{\nname s\nradius 1\n}\n"
			"uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n", *j, &diags );
		Check( n == 2 && diags.empty()
			&& j->GetGeometries()->getItemCount() == baseGeo + 1
			&& j->GetPainters()->getItemCount()  == basePnt + 1,
			"self-proving control: the same document, all-valid, applies BOTH chunks (geometry + painter)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [statelessness] DeriveToJob resets the chunk parsers' cross-chunk parse
	// state at its start (ClearChunkParserState, Cst.cpp:1380), so a prior derive
	// does not leak into the next.  The golden never exercises this -- it builds
	// a fresh Job per scene.  The redesign runs DeriveToJob REPEATEDLY in one
	// process (once per edit), so consecutive derives in the SAME process MUST be
	// stateless.  Recovered from CstDescriptorBindTest `[state-isolation]` +
	// CstDeriveDifferentialTest `[cross]` / NoLeak.
	//----------------------------------------------------------------------
	std::printf( "[statelessness] a prior derive does not leak parse state into the next\n" );

	// (a) painter-colour cache leak (the round-4 repro).  Scene A defines a
	// painter `bright` whose colour, used as both ref+tau, would violate energy
	// conservation (0.9 + 0.9 > 1.0) and inject an energy-auto-scaled painter.
	// Scene B references `bright` via ref+tau but NEVER defines it.  If A's
	// colour cache leaked into B, B's translucent_material energy check would see
	// `bright` and inject a `t_auto_ref` painter that a clean derive of B never
	// makes.
	{
		const std::string a = HDR + "uniformcolor_painter\n{\nname bright\ncolor 0.9 0.9 0.9\n}\n";
		const std::string b = HDR + "translucent_material\n{\nname t\nref bright\ntau bright\n}\n";
		// Fresh derive of B (no pollution) -- the reference state.
		const std::string fresh = DumpCst( b );
		// Pollute the parser state with A, then derive B in the SAME process.
		Job* pollute = new Job(); std::vector<std::string> pd; DeriveCst( a, *pollute, &pd );   // writes the painter-colour cache
		Job* cj = new Job(); std::vector<std::string> cd; DeriveCst( b, *cj, &cd );             // must NOT see `bright` leaked from A
		const std::string after = DumpJob( *cj );
		Check( after == fresh, "deriving B after A matches a fresh derive of B (no leaked painter-colour cache)" );
		if( after != fresh ) std::printf( "    fresh=[%s]\n    after=[%s]\n", fresh.c_str(), after.c_str() );
		Check( after.find( "t_auto_ref" ) == std::string::npos, "no spurious energy-auto-scaled painter (t_auto_ref) leaked from A into B" );
		pollute->release(); cj->release();
	}

	// (b) camera name-dedup leak.  Two UNNAMED cameras auto-name `default`; if
	// the dedup set leaked across derives, the second would collide and rename to
	// `default_1`.  Scene A has one unnamed camera; scene B has one unnamed
	// camera.  A clean, stateless derive of B names its camera `default` -- NOT
	// `default_1`.  DumpJob surfaces camera names in its `cameras:` section, so a
	// dedup leak is directly observable.
	{
		const std::string a = HDR + "pinhole_camera\n{\nlocation 1 1 1\nlookat 0 0 0\n}\n";
		const std::string b = HDR + "pinhole_camera\n{\nlocation 2 2 2\nlookat 0 0 0\n}\n";
		const std::string fresh = DumpCst( b );
		Job* pollute = new Job(); std::vector<std::string> pd; DeriveCst( a, *pollute, &pd );   // dedup set sees `default`
		Job* cj = new Job(); std::vector<std::string> cd; DeriveCst( b, *cj, &cd );             // must still name its camera `default`
		const std::string after = DumpJob( *cj );
		Check( after == fresh, "deriving unnamed-camera B after unnamed-camera A matches a fresh derive of B (no leaked camera name-dedup)" );
		if( after != fresh ) std::printf( "    fresh=[%s]\n    after=[%s]\n", fresh.c_str(), after.c_str() );
		Check( after.find( "  default_1" ) == std::string::npos, "camera not renamed default -> default_1 (dedup set did not leak A->B)" );
		Check( after.find( "  default" ) != std::string::npos, "the unnamed camera IS present as `default` after a prior derive" );
		pollute->release(); cj->release();
	}

	// (c) camera name-dedup within a SINGLE derive still dedups (the reset clears
	// state BETWEEN derives, it must not disable in-scene dedup).  Two unnamed
	// cameras in ONE scene -> default + default_1.  This proves the reset targets
	// cross-derive leakage, not the legitimate same-document dedup -- so the
	// [statelessness] guard is not vacuously satisfied by disabling dedup entirely.
	{
		const std::string two = HDR +
			"pinhole_camera\n{\nlocation 1 1 1\nlookat 0 0 0\n}\n"
			"pinhole_camera\n{\nlocation 2 2 2\nlookat 0 0 0\n}\n";
		const std::string dump = DumpCst( two );
		Check( dump.find( "  default" ) != std::string::npos && dump.find( "  default_1" ) != std::string::npos,
			"in-scene dedup intact: two unnamed cameras in ONE derive -> default + default_1 (reset is cross-derive only)" );
	}

	//----------------------------------------------------------------------
	// [apply-continuation] bug-fix wave 2026-08-28: a PASS-2 (apply-time,
	// Finalize) failure used to `break` DeriveToJob's apply loop, silently
	// dropping every LATER chunk with no diagnostic of its own -- one bad
	// chunk could make an unrelated sibling three lines down look like it
	// had vanished from the scene.  DeriveToJob now keeps going: every
	// pending chunk still gets its own attempt and (on failure) its own
	// named diagnostic, and the overall derive still fails whenever
	// anything failed (every real caller gates on `diags.empty()`).
	//----------------------------------------------------------------------
	std::printf( "[apply-continuation] a PASS-2 apply failure does not hide later chunks\n" );

	// (a) TWO independent apply-time failures (unresolved painter reference --
	// PASS-1 cannot see this; the param is a syntactically-valid string, just
	// not the name of anything) bracket a perfectly valid THIRD chunk.  Before
	// the fix this scene reported exactly ONE diagnostic (`bad_a`'s) and
	// `should_apply` was never even attempted.  After the fix: both failures
	// are each individually diagnosed, `should_apply` DID apply, and the
	// derive still fails overall.
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR +
			"lambertian_material\n{\nname bad_a\nreflectance does_not_exist_a\n}\n"
			"uniformcolor_painter\n{\nname should_apply\ncolor 0.5 0.2 0.9\n}\n"
			"lambertian_material\n{\nname bad_b\nreflectance does_not_exist_b\n}\n",
			*j, &diags );
		Check( n == 1, "exactly the one valid chunk (the painter) applied" );
		Check( !diags.empty(), "the overall derive still reports failure" );
		// Each failing chunk's own diagnostic names its KEYWORD, not its
		// instance name (`ResolveOrDiagnoseScalar`'s name-carrying message
		// goes to the log, not the `diags` vector) -- so the proof that
		// BOTH failures were reached, not just the first, is that a
		// `lambertian_material` diagnostic appears TWICE.  Matches on the
		// KEYWORD prefix only, not the current "apply failed" wording, so a
		// future improvement that gives this chunk a more specific message
		// (via g_cstFinalizeDiagSink) does not make this count silently
		// drop to zero and fail claiming continuation broke.
		int perChunkFailures = 0;
		for( const std::string& d : diags )
			if( d.rfind( "lambertian_material: ", 0 ) == 0 ) ++perChunkFailures;
		Check( perChunkFailures == 2,
			"BOTH failing chunks (bad_a AND bad_b) are individually diagnosed -- "
			"bad_b was reached at all, proving the loop did not stop at bad_a" );
		Check( j->GetPainters() && j->GetPainters()->GetItem( "should_apply" ) != 0,
			"the chunk AFTER the first failure applied anyway (continuation, not silent drop)" );
		// The summary ("N of M chunk(s) failed...") is a LOG-ONLY line
		// (GlobalLog, not `diags`) precisely so a structured consumer of
		// `diags` (e.g. AgentSession::ValidateText, which maps every entry
		// to a localized AgentDiagnostic) never sees an aggregate entry
		// with no chunk to name and no offset to localize -- so `diags`
		// here must hold EXACTLY the two per-chunk failures, nothing more.
		Check( diags.size() == 2, "diags holds exactly the two per-chunk failures -- no extra aggregate entry polluting a structured consumer" );
		j->release();
	}

	// (b) SELF-PROVING control: the same three-chunk shape but with BOTH
	// references resolved -- all three chunks must apply and diags must be
	// empty, so (a) is not vacuously passing because nothing here can ever
	// derive cleanly.
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR +
			"uniformcolor_painter\n{\nname refl_a\ncolor 0.1 0.1 0.1\n}\n"
			"lambertian_material\n{\nname ok_a\nreflectance refl_a\n}\n"
			"uniformcolor_painter\n{\nname should_apply2\ncolor 0.5 0.2 0.9\n}\n"
			"uniformcolor_painter\n{\nname refl_b\ncolor 0.2 0.2 0.2\n}\n"
			"lambertian_material\n{\nname ok_b\nreflectance refl_b\n}\n",
			*j, &diags );
		Check( n == 5 && diags.empty(), "self-proving control: the same shape, all-valid, applies every chunk with no diagnostics" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [scalar-pipe-default] bug-fix wave 2026-08-28: four Scalar-pipe
	// parameters (polished_material::tau, dielectric_material::tau,
	// translucent_material::ext, generic_human_tissue_material::g) used to
	// default straight to the `none` painter name, which resolves ONLY in
	// the colour-painter manager -- so a BARE chunk (the parameter simply
	// omitted, the common case for an author who has not yet decided a
	// value) hard-failed to apply at all, with a diagnostic that reads like
	// a reference problem rather than a missing-default one.  Each now
	// defaults to the numeric literal `0.0`, which reproduces the pre-
	// ISCALARPAINTER-refactor behaviour bit-for-bit (the historical `none`
	// IPainter default WAS RISEPel(0,0,0), i.e. literal zero on every
	// channel) -- see docs/ISCALARPAINTER_REFACTOR.md and the comments at
	// each Finalize site in ChunkParserRegistry.cpp.
	//----------------------------------------------------------------------
	std::printf( "[scalar-pipe-default] a bare chunk with tau/ext/g omitted parses and applies\n" );
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR +
			"uniformcolor_painter\n{\nname pm_refl\ncolor 0.6 0.6 0.6\n}\n"
			"polished_material\n{\nname pm_bare\nreflectance pm_refl\n}\n",
			*j, &diags );
		Check( n == 2 && diags.empty(), "bare polished_material (no tau) derives cleanly" );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "pm_bare" ) != 0, "...and the material is actually registered" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst( HDR + "dielectric_material\n{\nname dm_bare\n}\n", *j, &diags );
		Check( n == 1 && diags.empty(), "bare dielectric_material (no tau) derives cleanly" );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "dm_bare" ) != 0, "...and the material is actually registered" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst( HDR + "translucent_material\n{\nname tm_bare\n}\n", *j, &diags );
		Check( n == 1 && diags.empty(), "bare translucent_material (no ext) derives cleanly" );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "tm_bare" ) != 0, "...and the material is actually registered" );
		j->release();
	}
	{
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst( HDR + "generic_human_tissue_material\n{\nname ght_bare\n}\n", *j, &diags );
		Check( n == 1 && diags.empty(), "bare generic_human_tissue_material (no g) derives cleanly" );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "ght_bare" ) != 0, "...and the material is actually registered" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [expression-diag-sink] bug-fix wave 2026-08-30: BuildExpressionProgramFromChunkFields
	// (ExpressionPainter.h) already produced a SPECIFIC compiler diagnostic
	// (e.g. "expression_painter `name`: def `wear`: unknown function
	// `saturate`") on a bad `def`/`expr` body, but only via GlobalLog() --
	// it never populated g_cstFinalizeDiagSink, so DeriveToJob's apply loop
	// (Cst.cpp) fell through to the GENERIC "<keyword>: apply failed (e.g.
	// unresolved reference); see log" diagnostic for EVERY expression
	// compile failure, indistinguishable across unknown-function /
	// unknown-variable / bad-occlusion-radius. Fixed by threading the
	// specific text through an optional `outError` out-param at all THREE
	// call sites (Job::AddExpressionPainter for expression_painter,
	// ChunkParserRegistry.cpp's scalar_painter{expression} Finalize, and
	// its expression_function2d Finalize). This block proves each of the
	// three failure classes reaches `diags` with the SPECIFIC compiler
	// text, not the generic fallback, for all three chunk kinds that share
	// the builder.
	//----------------------------------------------------------------------
	std::printf( "[expression-diag-sink] a bad expression body's SPECIFIC compiler diagnostic reaches diags, not the generic apply-failed fallback\n" );
	{
		// Asserts `diags` holds exactly one entry, that it does NOT contain
		// the generic apply-failed fallback text, and that it DOES contain
		// `needle` (the specific compiler text this fix threads through).
		auto CheckSpecificDiag = [&]( const std::string& scene, const char* needle, const char* what ) {
			Job* j = new Job(); std::vector<std::string> diags;
			const int n = DeriveCst( HDR + scene, *j, &diags );
			bool ok = ( n == 0 ) && diags.size() == 1
				&& diags[0].find( "apply failed" ) == std::string::npos
				&& diags[0].find( needle ) != std::string::npos;
			Check( ok, what );
			if( !ok ) std::printf( "    diag=[%s]\n", diags.empty() ? "<none>" : diags[0].c_str() );
			j->release();
		};

		// expression_painter: unknown function / unknown variable / bad occlusion radius.
		CheckSpecificDiag(
			"expression_painter\n{\nname bad_unknownfn_pnt\ndef wear saturate(u,0.0,1.0)\nexpr wear\n}\n",
			"unknown function `saturate`", "expression_painter: unknown function reaches diags with specific text" );
		CheckSpecificDiag(
			"expression_painter\n{\nname bad_unknownvar_pnt\ndef wear nonexistent_var_xyz\nexpr wear\n}\n",
			"unknown variable `nonexistent_var_xyz`", "expression_painter: unknown variable reaches diags with specific text" );
		CheckSpecificDiag(
			"expression_painter\n{\nname bad_occl_pnt\ndef wear occlusion(0)\nexpr wear\n}\n",
			"occlusion() radius must be > 0", "expression_painter: bad occlusion(0) radius reaches diags with specific text" );

		// scalar_painter { expression ... }: same three, same shared builder.
		CheckSpecificDiag(
			"scalar_painter\n{\nname bad_unknownfn_sp\ndef wear saturate(u,0.0,1.0)\nexpression wear\n}\n",
			"unknown function `saturate`", "scalar_painter{expression}: unknown function reaches diags with specific text" );
		CheckSpecificDiag(
			"scalar_painter\n{\nname bad_unknownvar_sp\ndef wear nonexistent_var_xyz\nexpression wear\n}\n",
			"unknown variable `nonexistent_var_xyz`", "scalar_painter{expression}: unknown variable reaches diags with specific text" );
		CheckSpecificDiag(
			"scalar_painter\n{\nname bad_occl_sp\ndef wear occlusion(0)\nexpression wear\n}\n",
			"occlusion() radius must be > 0", "scalar_painter{expression}: bad occlusion(0) radius reaches diags with specific text" );

		// expression_function2d: unknown function / unknown variable are the
		// same diagnostics as the two 3D-context surfaces above (the shared
		// builder doesn't distinguish). occlusion() is DIFFERENT here by
		// design -- expression_function2d is a frozen UV-only surface (see
		// ExpressionEval.h's EnableContextVars doc comment) and rejects
		// occlusion()/thickness() BEFORE the radius is even inspected, with
		// its own dedicated "needs the 3D surface context" diagnostic
		// rather than "radius must be > 0" -- still a SPECIFIC compiler
		// text, not the generic apply-failed fallback, which is what this
		// block is proving.
		CheckSpecificDiag(
			"expression_function2d\n{\nname bad_unknownfn_fn2d\ndef wear saturate(u,0.0,1.0)\nexpr wear\n}\n",
			"unknown function `saturate`", "expression_function2d: unknown function reaches diags with specific text" );
		CheckSpecificDiag(
			"expression_function2d\n{\nname bad_unknownvar_fn2d\ndef wear nonexistent_var_xyz\nexpr wear\n}\n",
			"unknown variable `nonexistent_var_xyz`", "expression_function2d: unknown variable reaches diags with specific text" );
		CheckSpecificDiag(
			"expression_function2d\n{\nname bad_occl_fn2d\ndef wear occlusion(0)\nexpr wear\n}\n",
			"needs the 3D surface context", "expression_function2d: occlusion() (UV-only surface) reaches diags with specific text" );

		// Self-proving control: the SAME expression_painter shape with a
		// valid def/expr derives cleanly, so the checks above are not
		// vacuously passing because every expression chunk fails to parse.
		{
			Job* j = new Job(); std::vector<std::string> diags;
			const int n = DeriveCst(
				HDR + "expression_painter\n{\nname good_pnt\ndef wear clamp(u,0.0,1.0)\nexpr wear\n}\n",
				*j, &diags );
			Check( n == 1 && diags.empty(), "self-proving control: a valid expression_painter derives cleanly" );
			j->release();
		}
	}

	//----------------------------------------------------------------------
	// [chunk-brace-formatting] task_7f42984d: a chunk written with its `{` (and/or
	// `}`) sharing a line with the keyword or a parameter -- e.g.
	// `standard_object { name x geometry g material m }` all on one line -- used
	// to be SILENTLY ACCEPTED and silently WRONG: ParseChunk's per-param
	// same-line value-collection loop (Cst.cpp) has no newline to stop at, so it
	// swallows every token after the first param's name as MORE pvalue tokens of
	// THAT param. The chunk keeps its keyword but loses every param after the
	// first -- a `standard_object` derived this way has no `geometry`/`material`
	// at all, and Finalize used to emit an object with neither, silently, with
	// zero diagnostics (the reported bug: a showcase scene's wall objects
	// vanished with no error). ChunkBraceViolations (Cst.cpp, ResolveChunkParams)
	// now hard-rejects BOTH malformed forms at PASS-1, before any param is ever
	// read off such a chunk, with a diagnostic naming the exact source line and
	// quoting the documented rule text verbatim ("chunk braces must be on their
	// own lines" -- CLAUDE.md / docs/SCENE_CONVENTIONS.md / Parsers/README.md).
	//
	// RED-PROVEN: with ChunkBraceViolations' body replaced by
	// `openSameLine = closeSameLine = false;` (i.e. the check disabled), every
	// `Check` in this block that expects a brace-formatting diagnostic FAILS --
	// the [open-brace-glued] and [close-brace-trailing] cases instead derive
	// "successfully" with `wall`'s object missing its geometry/material params
	// exactly as the original report described, proving this block is not
	// tautological.  Restoring ChunkBraceViolations makes the whole file pass
	// again.
	//----------------------------------------------------------------------
	std::printf( "[chunk-brace-formatting] a brace not on its own line is a hard PASS-1 error naming the line + rule\n" );
	{
		// (a) THE REPORTED BUG'S EXACT SHAPE: the whole chunk on one line. Both the
		// opening `{` and the closing `}` share a line with other content, so BOTH
		// sub-checks fire; either is sufficient to refuse the whole derive. HDR is
		// one line ("RISE ASCII SCENE 7\n"), so the chunk -- and every brace in it
		// -- starts on line 2.
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "standard_object { name wall geometry plane_g material wall_m }\n",
			*j, &diags );
		Check( n == 0, "single-line chunk (open+close both glued): refused (n == 0)" );
		bool sawRuleOnLine2 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 2" ) != std::string::npos )
				sawRuleOnLine2 = true;
		Check( sawRuleOnLine2, "single-line chunk: a diagnostic quotes the rule text AND names line 2" );
		Check( j->GetObjects() == nullptr || j->GetObjects()->GetItem( "wall" ) == 0,
			"single-line chunk: `wall` was NOT silently created with missing geometry/material" );
		j->release();
	}
	{
		// (b) [open-brace-glued] `keyword {` sharing the keyword's line, but the
		// body (and the closing `}`) correctly on their own lines afterward --
		// the "with or without the `}`" case from the task: this form does NOT
		// lose any param (every param still gets its own line), but it still
		// violates the documented convention and must still hard-error, not
		// silently accept a second brace syntax. Chunk starts line 2; `{` is on
		// line 2 too (glued).
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "standard_object {\nname wall2\ngeometry plane_g\nmaterial wall_m\n}\n",
			*j, &diags );
		Check( n == 0, "open-brace-glued chunk (well-formed body otherwise): refused (n == 0)" );
		bool sawRuleOnLine2 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 2" ) != std::string::npos )
				sawRuleOnLine2 = true;
		Check( sawRuleOnLine2, "open-brace-glued chunk: a diagnostic quotes the rule text AND names line 2 (the `{`'s line)" );
		j->release();
	}
	{
		// (c) [close-brace-trailing] the sibling case named in the task: `{` alone
		// on its own line (well-formed opening), but the closing `}` trails the
		// LAST parameter's line instead of getting its own. This does NOT lose
		// data (Tokenize splits `}` as its own token even glued to `wall_m`, so
		// ParseChunk never swallows it as a pvalue) but it is still the same
		// documented-convention violation and must still hard-error for
		// consistency -- accepting it would let scenes drift onto a second,
		// undocumented brace syntax. Lines: kw=2, `{`=3, name=4, geometry=5,
		// `material wall_m }`=6 (the `}` shares line 6).
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "standard_object\n{\nname wall3\ngeometry plane_g\nmaterial wall_m }\n",
			*j, &diags );
		Check( n == 0, "close-brace-trailing chunk (well-formed opening otherwise): refused (n == 0)" );
		bool sawRuleOnLine6 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 6" ) != std::string::npos )
				sawRuleOnLine6 = true;
		Check( sawRuleOnLine6, "close-brace-trailing chunk: a diagnostic quotes the rule text AND names line 6 (the `}`'s line)" );
		j->release();
	}
	{
		// (d) refuse-all boundary applies here too, exactly as for every other
		// PASS-1 violation ([refuse-all] above): a VALID sibling chunk sharing the
		// document with a single-line chunk is NOT applied either -- the existing
		// documented policy ("if ANY chunk fails validation, apply NOTHING") is
		// unchanged by this fix, just extended to cover one more violation kind.
		int baseGeo;
		{ Job* b = new Job(); baseGeo = b->GetGeometries()->getItemCount(); b->release(); }
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR +
			"sphere_geometry\n{\nname plane_g\nradius 1\n}\n"
			"standard_object { name wall geometry plane_g material wall_m }\n",
			*j, &diags );
		Check( n == 0 && !diags.empty()
			&& j->GetGeometries()->getItemCount() == baseGeo,
			"refuse-all: a single-line chunk refuses the WHOLE document -- the valid sibling geometry is NOT applied" );
		j->release();
	}
	{
		// (e) SELF-PROVING control: the identical two-chunk document, with the
		// `standard_object` reformatted to the canonical multi-line form (braces
		// each on their own line, one param per line) -- both chunks MUST apply,
		// so (a)-(d) are not vacuously passing because this shape can never
		// derive cleanly for some unrelated reason (e.g. a bad geometry/material
		// reference).
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR +
			"sphere_geometry\n{\nname plane_g\nradius 1\n}\n"
			"lambertian_material\n{\nname wall_m\nreflectance none\n}\n"
			"standard_object\n{\nname wall\ngeometry plane_g\nmaterial wall_m\n}\n",
			*j, &diags );
		Check( n == 3 && diags.empty(), "self-proving control: the same chunks, canonically formatted, derive cleanly (all 3 apply)" );
		Check( j->GetObjects() && j->GetObjects()->GetItem( "wall" ) != 0, "...and `wall` the object actually exists" );
		j->release();
	}
	{
		// (f) [open-brace-content] REVIEW_CHIP1 P1 follow-up: `{` correctly on
		// its OWN line (kw and `{` do NOT share a line -- the ORIGINAL
		// ChunkBraceViolations already accepted this much) but content follows
		// `{` on `{`'s own line instead of the body starting on the line after --
		// `kw\n{ name x geometry g material m\n}`. Before the follow-up fix this
		// shape reported ZERO violations (kw/`{` differ in line; `}` is alone on
		// its own line) while ParseChunk's same-line value loop still swallowed
		// `geometry`/`material` into `name`'s value -- the exact "vanished wall
		// objects, zero diagnostics" defect survived under this one disguise.
		// Lines: kw=2, `{ name wall geometry plane_g material wall_m`=3, `}`=4.
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "standard_object\n{ name wall geometry plane_g material wall_m\n}\n",
			*j, &diags );
		Check( n == 0, "open-brace-content chunk (content glued to `{`'s own line): refused (n == 0)" );
		bool sawRuleOnLine3 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 3" ) != std::string::npos )
				sawRuleOnLine3 = true;
		Check( sawRuleOnLine3, "open-brace-content chunk: a diagnostic quotes the rule text AND names line 3 (the `{`'s line)" );
		Check( j->GetObjects() == nullptr || j->GetObjects()->GetItem( "wall" ) == 0,
			"open-brace-content chunk: `wall` was NOT silently created with missing geometry/material" );
		j->release();
	}
	{
		// (g) [close-brace-content] the DOCUMENT-level mirror of (f): `}`
		// correctly closes its own chunk's body with nothing preceding it on its
		// line, but the NEXT top-level chunk's keyword is glued directly onto
		// that same line with no separating newline --
		// `sphere_geometry\n{\n...\n}lambertian_material\n{\n...\n}`. This isn't a
		// silent-data-loss shape the way (f) is (ParseToCst's top-level loop
		// still finds the next chunk correctly regardless of the missing
		// newline), but it's the same documented "each of `{` and `}` must be on
		// its own line" violation, so it must still hard-refuse for consistency
		// -- accepting it would let scenes drift onto an undocumented second
		// brace-adjacency convention. The second chunk is a self-contained
		// `lambertian_material` (no reference to resolve) so the ONLY diagnostic
		// in play is the brace one. Lines: sphere_geometry=2, `{`=3,
		// `name plane_g`=4, `radius 1`=5, `}lambertian_material`=6 (the `}`
		// shares line 6 with the next chunk's keyword).
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst(
			HDR + "sphere_geometry\n{\nname plane_g\nradius 1\n}lambertian_material\n{\nname wall_m\nreflectance none\n}\n",
			*j, &diags );
		Check( n == 0, "close-brace-content chunk (next keyword glued to `}`'s line): refused (n == 0)" );
		bool sawRuleOnLine6 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 6" ) != std::string::npos )
				sawRuleOnLine6 = true;
		Check( sawRuleOnLine6, "close-brace-content chunk: a diagnostic quotes the rule text AND names line 6 (the `}`'s line)" );
		j->release();
	}
	{
		// (h) [empty-one-line-chunk] POLICY DECISION: an empty chunk whose `{`
		// and `}` both sit on ONE line together -- `kw\n{ }\n` -- is a violation
		// too. Neither brace is "on its own line" when they share a line with
		// EACH OTHER, even though no param is at risk of being swallowed (there
		// is none). Accepting this shape would carve out a silent exception to
		// "each of `{` and `}` must be on its own line" for the empty case only,
		// which the documented rule (docs/SCENE_CONVENTIONS.md #0,
		// src/Library/Parsers/README.md) does not carve out -- so both braces
		// fire (openSameLine via forward-of-`{`, closeSameLine via
		// backward-of-`}`), both naming the shared line. Lines:
		// sphere_geometry=2, `{ }`=3.
		Job* j = new Job(); std::vector<std::string> diags;
		const int n = DeriveCst( HDR + "sphere_geometry\n{ }\n", *j, &diags );
		Check( n == 0, "empty one-line chunk `{ }`: refused (n == 0)" );
		bool sawRuleOnLine3 = false;
		for( const std::string& d : diags )
			if( d.find( "chunk braces must be on their own lines" ) != std::string::npos && d.find( "line 3" ) != std::string::npos )
				sawRuleOnLine3 = true;
		Check( sawRuleOnLine3, "empty one-line chunk `{ }`: a diagnostic quotes the rule text AND names line 3 (the shared line)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
