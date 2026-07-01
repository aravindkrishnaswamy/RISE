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
//  This file recovers exactly those CST-only contracts:
//
//    * [refuse-all]  DeriveToJob's PASS-1 structural rejection (Cst.cpp
//      ~1408-1416, `return 0; // refuse-all: applies NOTHING`).  A malformed
//      chunk -- unknown chunk type, unknown parameter, non-finite value
//      (radius nan), non-numeric value (radius abc), or a value-less line
//      (radius with no value) -- refuses the WHOLE document and applies
//      NOTHING, so even a VALID sibling chunk in the same document is not
//      applied.  Recovered from CstDescriptorBindTest's `[validate]` block.
//
//    * [statelessness]  DeriveToJob calls ClearChunkParserState() at its start
//      (Cst.cpp:1380), resetting the chunk parsers' file-scope state BETWEEN
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

static const std::string HDR = "RISE ASCII SCENE 6\n";

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

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
