//////////////////////////////////////////////////////////////////////
//
//  CstDeriveDifferentialTest.cpp - the SHARED CST-vs-legacy differential
//  harness (item 5+). One committed, fast, hermetic test that the review loop
//  RUNS instead of each reviewer rebuilding a one-off harness (the per-agent
//  full-library LTO relink was the loop's wall-clock cost, not the checks).
//
//  Two equivalence dimensions, both via the DumpJob oracle (tests/
//  CstRenderEquivalence.h):
//    * SINGLE-DERIVE: for a broad corpus of CANONICAL scenes (macro-free,
//      directive-free, own-line comments, single-space values), the CST path
//      (ParseToCst -> DeriveToJob) builds a Job whose dump equals the legacy
//      parser's. Covers every chunk mechanism the kernel touches: multi-token
//      values, references (3-level chains), transforms, numeric formats,
//      repeated params, whitespace normalisation, and the in-scene
//      translucent-energy auto-painter path.
//    * CROSS-DERIVE statelessness (the round-4 bug class): deriving scene A
//      first must NEVER change the Job that deriving scene B produces -- because
//      DeriveToJob resets the chunk parsers' file-scope parse state first
//      (ClearChunkParserState), exactly as legacy ParseAndLoadScene does. We
//      assert DumpCst(B after A) == DumpLegacy(B) for pairs/triples that
//      exercise the painter-colour cache AND the camera name-dedup / location
//      state (DumpJob now has a cameras section). Red-prove: with the reset
//      removed, the painter-leak and camera-name-dedup cases both fail.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static const std::string HDR = "RISE ASCII SCENE 6\n";

// Dump a scene derived through the CST path (fresh Job each call).
static std::string DumpCst( const std::string& scene )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	std::string s = DumpJob( *j );
	j->release();
	return s;
}
// Dump a scene parsed through the legacy path (fresh Job each call).
static std::string DumpLegacy( const std::string& scene )
{
	Job* j = new Job();
	ParseLegacy( scene, *j );
	std::string s = DumpJob( *j );
	j->release();
	return s;
}

// SINGLE-DERIVE equivalence: CST == legacy on a canonical scene.
static void Equiv( const std::string& body, const char* label )
{
	const std::string scene = HDR + body;
	std::string dc = DumpCst( scene ), dl = DumpLegacy( scene );
	Check( dc == dl, label );
	if( dc != dl ) std::printf( "    legacy=[%s]\n    cst   =[%s]\n", dl.c_str(), dc.c_str() );
}

// CROSS-DERIVE statelessness: deriving `pollute` first must not change `b`.
// DeriveToJob clears parse state at its start, so DumpCst(b) after polluting
// must equal a fresh legacy parse of b.
static void NoLeak( const std::string& polluteBody, const std::string& bBody, const char* label )
{
	const std::string pollute = HDR + polluteBody, b = HDR + bBody;
	{ Job* p = new Job(); Document d = ParseToCst( pollute ); std::vector<std::string> diags; DeriveToJob( d, *p, &diags ); p->release(); }  // dirty the parser state
	std::string after = DumpCst( b );      // DeriveToJob must reset it -> matches fresh
	std::string fresh = DumpLegacy( b );   // legacy resets it -> the reference
	Check( after == fresh, label );
	if( after != fresh ) std::printf( "    fresh=[%s]\n    after=[%s]\n", fresh.c_str(), after.c_str() );
}

int main()
{
	std::printf( "CstDeriveDifferentialTest -- shared CST-vs-legacy differential (single + cross-derive)\n" );

	//----------------------------------------------------------------------
	std::printf( "[single] CST derive == legacy derive over a canonical corpus\n" );
	//----------------------------------------------------------------------
	// geometry: radii + numeric formats
	Equiv( "sphere_geometry\n{\nname s\nradius 0.6\n}\n", "sphere radius 0.6" );
	Equiv( "sphere_geometry\n{\nname s\nradius .5\n}\n",  "sphere radius .5 (leading dot)" );
	Equiv( "sphere_geometry\n{\nname s\nradius 1e3\n}\n", "sphere radius 1e3 (exponent)" );
	Equiv( "sphere_geometry\n{\nname s\nradius 100\n}\n", "sphere radius 100 (integer)" );
	// painter (multi-token colour) + material reference
	Equiv( "uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
	       "lambertian_material\n{\nname m\nreflectance red\n}\n", "painter colour + lambertian reference" );
	// 3-level chain + transforms (multi-token DoubleVec3/Vec4/Mat4)
	Equiv( "uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
	       "lambertian_material\n{\nname m\nreflectance red\n}\n"
	       "sphere_geometry\n{\nname g\nradius 0.25\n}\n"
	       "standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 1 2 3\nscale 2 2 2\n}\n", "object->material->painter + position+scale" );
	Equiv( "sphere_geometry\n{\nname g\nradius 1\n}\n"
	       "standard_object\n{\nname o\ngeometry g\norientation 30 45 60\n}\n", "object orientation (Euler Vec3)" );
	Equiv( "sphere_geometry\n{\nname g\nradius 1\n}\n"
	       "standard_object\n{\nname o\ngeometry g\nquaternion 0 0 0 1\n}\n", "object quaternion (Vec4)" );
	Equiv( "sphere_geometry\n{\nname g\nradius 1\n}\n"
	       "standard_object\n{\nname o\ngeometry g\nmatrix 1 0 0 0 0 1 0 0 0 0 1 0 5 6 7 1\n}\n", "object matrix (Mat4)" );
	// multiple objects sharing a material
	Equiv( "uniformcolor_painter\n{\nname red\ncolor 1 0 0\n}\n"
	       "lambertian_material\n{\nname m\nreflectance red\n}\n"
	       "sphere_geometry\n{\nname g\nradius 1\n}\n"
	       "standard_object\n{\nname a\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
	       "standard_object\n{\nname b\ngeometry g\nmaterial m\nposition 3 0 0\n}\n", "two objects share one material" );
	// repeated param (last-wins, matches ParseStateBag overwrite)
	Equiv( "sphere_geometry\n{\nname s\nradius 1\nradius 2\nradius 0.3\n}\n", "repeated radius (last wins)" );
	Equiv( "piecewise_linear_function\n{\nname f\ncp 0 0\ncp 0.5 1\ncp 1 0\n}\n", "TRUE repeatable: cp accumulates as an ordered list (not last-wins) -- the RepeatGroup-view derive twin" );
	// whitespace normalisation (tabs / multi-space in string + ref values)
	Equiv( "uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
	       "lambertian_material\n{\nname m\nreflectance red\n}\n"
	       "sphere_geometry\n{\nname\tg\nradius 0.25\n}\n"
	       "standard_object\n{\nname o\ngeometry  g\nmaterial\tm\nposition 1  2   3\n}\n", "tabs/multi-space normalise like legacy" );
	// in-scene translucent energy auto-painter path (ref+tau > 1.0). `ext` needs
	// a scalar_painter (IScalarPainter slot) for the material to fully derive.
	Equiv( "uniformcolor_painter\n{\nname hot\ncolor 0.8 0.8 0.8\n}\n"
	       "scalar_painter\n{\nname ce\nvalue 0\n}\n"
	       "translucent_material\n{\nname glass\nref hot\ntau hot\next ce\n}\n", "translucent energy auto-painters (in-scene)" );
	// light + camera (exercise the cameras: dump section)
	Equiv( "omni_light\n{\nname lamp\npower 100\nposition 0 5 0\ncolor 1 1 1\n}\n", "omni light" );
	Equiv( "pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 45\n}\n", "pinhole camera (name + location dumped)" );

	//----------------------------------------------------------------------
	std::printf( "[cross] a prior derive never changes the next (parse-state reset)\n" );
	//----------------------------------------------------------------------
	// painter-colour cache leak (the round-4 repro): A defines `bright`, B's
	// translucent material references it but never defines it.
	NoLeak( "uniformcolor_painter\n{\nname bright\ncolor 0.9 0.9 0.9\n}\n",
	        "translucent_material\n{\nname t\nref bright\ntau bright\n}\n",
	        "painter-colour cache does not leak A->B" );
	// camera name-dedup leak: two UNNAMED cameras would collide on the auto-name
	// (default / default_1) if the dedup set leaked across derives.
	NoLeak( "pinhole_camera\n{\nlocation 1 1 1\nlookat 0 0 0\n}\n",
	        "pinhole_camera\n{\nlocation 2 2 2\nlookat 0 0 0\n}\n",
	        "camera name-dedup does not leak A->B (unnamed -> default both)" );
	// camera location/defaults: B is a fresh camera scene after a different one.
	NoLeak( "pinhole_camera\n{\nname camA\nlocation 9 9 9\nlookat 0 0 0\nfov 20\n}\n",
	        "pinhole_camera\n{\nname camB\nlocation 0 0 5\nlookat 0 0 0\n}\n",
	        "camera location/defaults do not leak A->B" );
	// full multi-type pollute, then a different multi-type scene
	NoLeak( "uniformcolor_painter\n{\nname p1\ncolor 0.9 0.1 0.1\n}\n"
	        "lambertian_material\n{\nname m1\nreflectance p1\n}\n"
	        "sphere_geometry\n{\nname g1\nradius 5\n}\n",
	        "uniformcolor_painter\n{\nname p2\ncolor 0.1 0.2 0.3\n}\n"
	        "lambertian_material\n{\nname m2\nreflectance p2\n}\n"
	        "sphere_geometry\n{\nname g2\nradius 0.5\n}\n"
	        "standard_object\n{\nname o2\ngeometry g2\nmaterial m2\nposition 1 1 1\n}\n",
	        "multi-type scene A does not leak into multi-type scene B" );
	// triple: two pollutes then B (state must still be clean)
	{
		const std::string p1 = HDR + "uniformcolor_painter\n{\nname bright\ncolor 0.95 0.95 0.95\n}\n";
		const std::string p2 = HDR + "pinhole_camera\n{\nlocation 7 7 7\nlookat 0 0 0\n}\n";
		const std::string b  = HDR + "translucent_material\n{\nname t\nref bright\ntau bright\n}\n"
		                             "pinhole_camera\n{\nlocation 0 0 1\nlookat 0 0 0\n}\n";
		{ Job* j = new Job(); Document d = ParseToCst( p1 ); std::vector<std::string> dg; DeriveToJob( d, *j, &dg ); j->release(); }
		{ Job* j = new Job(); Document d = ParseToCst( p2 ); std::vector<std::string> dg; DeriveToJob( d, *j, &dg ); j->release(); }
		std::string after = DumpCst( b ), fresh = DumpLegacy( b );
		Check( after == fresh, "two prior derives (painter + camera) do not leak into B" );
		if( after != fresh ) std::printf( "    fresh=[%s]\n    after=[%s]\n", fresh.c_str(), after.c_str() );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
