//////////////////////////////////////////////////////////////////////
//
//  CstKernelTest.cpp - transfer-gate item 2: the in-tree src/Library/Cst
//  kernel, gated by the render-equivalence harness.
//
//  Exercises the REAL library module (RISE::Cst), not a tests/-only prototype:
//   * G1 lossless round-trip (ParseToCst -> SerializeCst == input) on real
//     scenes (with the `RISE ASCII SCENE 6` header, multi-chunk, tar-pit cases).
//   * Equivalence: for a sphere scene, deriving via the CST kernel
//     (Cst::DeriveToJob) produces the SAME Job as the legacy AsciiSceneParser,
//     proven with the harness metric DumpJob(cstJob) == DumpJob(legacyJob).
//
//  Item-2 scope note: the CST models the `RISE ASCII SCENE 6` header as a
//  generic (losslessly-preserved) leading construct that DeriveToJob ignores;
//  a dedicated version-header node and broader chunk-type derivation are
//  subsequent transfer-gate items.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "CstRenderEquivalence.h"   // risequiv::ParseLegacy + risequiv::DumpJob

using namespace RISE;

static int g_pass = 0, g_fail = 0;
static void Check( bool cond, const char* what ) { if( cond ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", what ); } }

int main()
{
	std::printf( "CstKernelTest -- transfer-gate item 2 (in-tree src/Library/Cst, harness-gated)\n" );

	//------------------------------------------------------------------
	// G1: lossless round-trip through the REAL library module.
	//------------------------------------------------------------------
	std::printf( "[G1] ParseToCst -> SerializeCst is byte-identical\n" );
	const std::vector<std::string> g1 = {
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n",
		"RISE ASCII SCENE 6\n# a comment\nsphere_geometry {\n\tname s\n\tradius 0.6  # ball\n\n}\n",
		"RISE ASCII SCENE 6\r\nsphere_geometry\r\n{\r\nname s\r\nradius 0.6\r\n}\r\n",
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname red\ncolor 1 0 0\n}\n"
		"lambertian_material\n{\nname m\nreflectance red\n}\n"
		"sphere_geometry\n{\nname a\nradius 0.165\n}\n"
		"sphere_geometry\n{\nname b\nradius 0.25\n}\n",
	};
	for( size_t i = 0; i < g1.size(); ++i ) {
		Cst::Document d = Cst::ParseToCst( g1[i] );
		bool ok = ( Cst::SerializeCst(d) == g1[i] );
		char msg[64]; std::snprintf( msg, sizeof(msg), "fixture %zu round-trips byte-identical", i );
		Check( ok, msg );
		if( !ok ) std::printf( "    out=[%s]\n", Cst::SerializeCst(d).c_str() );
	}

	//------------------------------------------------------------------
	// Equivalence: CST derive == legacy parse (sphere scenes), via the harness.
	//------------------------------------------------------------------
	std::printf( "[equiv] DumpJob(cstJob) == DumpJob(legacyJob) for sphere scenes\n" );
	const std::vector<std::string> scenes = {
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 0.6\n}\n",
		"RISE ASCII SCENE 6\n"
		"sphere_geometry\n{\nname a\nradius 0.165\n}\n"
		"sphere_geometry\n{\nname b\nradius 0.25\n}\n",
	};
	for( size_t i = 0; i < scenes.size(); ++i ) {
		Job* legacy = new Job();
		bool okL = risequiv::ParseLegacy( scenes[i], *legacy );

		Job* cstJob = new Job();
		Cst::Document d = Cst::ParseToCst( scenes[i] );
		int n = Cst::DeriveToJob( d, *cstJob );

		std::string dl = risequiv::DumpJob( *legacy );
		std::string dc = risequiv::DumpJob( *cstJob );
		char msg[96];
		std::snprintf( msg, sizeof(msg), "scene %zu: legacy parse succeeded", i );           Check( okL, msg );
		std::snprintf( msg, sizeof(msg), "scene %zu: CST derived the expected geometry count", i ); Check( n == (int)(i==0?1:2), msg );
		std::snprintf( msg, sizeof(msg), "scene %zu: CST-derived Job == legacy-parsed Job (DumpJob)", i ); Check( dl == dc, msg );
		if( dl != dc ) std::printf( "    legacy=[%s]\n    cst   =[%s]\n", dl.c_str(), dc.c_str() );

		legacy->release();
		cstJob->release();
	}

	//------------------------------------------------------------------
	// The kernel actually drove the real apply layer: read a radius back.
	//------------------------------------------------------------------
	std::printf( "[derive] the kernel drove Job::AddSphereGeometry (engine read-back)\n" );
	{
		Job* job = new Job();
		Cst::DeriveToJob( Cst::ParseToCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nname only\nradius 0.42\n}\n" ), *job );
		IGeometry* g = job->GetGeometries()->GetItem( "only" );
		double r = -1; if( g ) { Point3 c; Scalar rr = 0; g->GenerateBoundingSphere( c, rr ); r = rr; }
		Check( r == 0.42, "derived sphere 'only' has radius 0.42 in the engine" );
		job->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
