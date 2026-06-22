//////////////////////////////////////////////////////////////////////
//
//  CstRenderEquivalenceTest.cpp - transfer-gate item 1: the render-
//  equivalence harness (pre-P0 oracle), per the external review.
//
//  Establishes the regression oracle BEFORE the first in-tree CST node: it
//  proves the equivalence metric (canonical Job dump) is STABLE -- a legacy
//  parse of a scene is deterministic, so two parses produce byte-identical
//  dumps. Once the src/Library/Cst kernel exists, the CST slices assert
//  DumpJob(cstJob) == DumpJob(legacyJob) using this same primitive.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"

using namespace RISE;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool cond, const char* what ) { if( cond ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what ); } }

// Parse a scene twice via the legacy path and confirm the dump is identical
// (the oracle is deterministic) and non-trivial. Returns the dump.
static std::string OracleStable( const std::string& scene, const char* label )
{
	Job* a = new Job(); bool oka = ParseLegacy( scene, *a );
	Job* b = new Job(); bool okb = ParseLegacy( scene, *b );
	std::string da = DumpJob( *a ), db = DumpJob( *b );
	char msg[96];
	std::snprintf( msg, sizeof(msg), "%s: scene parses (ParseAndLoadScene == true)", label );          Check( oka && okb, msg );
	std::snprintf( msg, sizeof(msg), "%s: legacy parse is DETERMINISTIC (dump identical)", label );     Check( da == db, msg );
	if( da != db ) std::printf( "    A=[%s]\n    B=[%s]\n", da.c_str(), db.c_str() );
	a->release(); b->release();
	return da;
}

int main()
{
	std::printf( "CstRenderEquivalenceTest -- transfer-gate item 1 (the pre-P0 oracle)\n" );

	//------------------------------------------------------------------
	// Scene A: a single sphere.
	//------------------------------------------------------------------
	std::printf( "[oracle] sphere-only scene\n" );
	const std::string sceneA =
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius 0.6\n}\n";
	std::string dumpA = OracleStable( sceneA, "sphere" );
	Check( dumpA.find("  s bsphere=0.6") != std::string::npos, "dump captures geometry s with bounding-sphere radius 0.6" );

	//------------------------------------------------------------------
	// Scene B: painter + material (reference) + two spheres.
	//------------------------------------------------------------------
	std::printf( "[oracle] painter + material + multi-geometry scene\n" );
	const std::string sceneB =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname red\ncolor 1 0 0\n}\n"
		"lambertian_material\n{\nname redmat\nreflectance red\n}\n"
		"sphere_geometry\n{\nname ballA\nradius 0.165\n}\n"
		"sphere_geometry\n{\nname ballB\nradius 0.25\n}\n";
	std::string dumpB = OracleStable( sceneB, "multi" );
	Check( dumpB.find("ballA bsphere=0.165") != std::string::npos
	    && dumpB.find("ballB bsphere=0.25")  != std::string::npos, "dump captures both spheres with their radii" );
	Check( dumpB.find("\n  red\n")    != std::string::npos, "dump captures painter 'red'" );
	Check( dumpB.find("\n  redmat\n") != std::string::npos, "dump captures material 'redmat'" );

	//------------------------------------------------------------------
	// The metric DISCRIMINATES: a different scene must produce a different dump
	// (else the oracle would rubber-stamp a broken migration).
	//------------------------------------------------------------------
	std::printf( "[oracle] the metric discriminates (different scene -> different dump)\n" );
	const std::string sceneA2 =
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname s\nradius 0.9\n}\n";   // radius differs
	Job* c = new Job(); ParseLegacy( sceneA2, *c ); std::string dumpA2 = DumpJob( *c ); c->release();
	Check( dumpA2 != dumpA, "a changed radius yields a DIFFERENT dump (metric is not trivially equal)" );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
