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

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
