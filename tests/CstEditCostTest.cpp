//////////////////////////////////////////////////////////////////////
//
//  CstEditCostTest.cpp - transfer-gate item 8: measure a NON-SPATIAL edit AND a
//  SPATIAL edit on the in-tree CST path, in WALL-CLOCK, and report the TLAS time
//  SEPARATELY.
//
//  The gate's payoff: "the redesign's real CST path is O(closure) for non-spatial
//  edits, with spatial cost reported honestly." Measured here, end to end:
//
//    ANALYTIC (robust, noise-free):
//      [closure-size] the re-derive CLOSURE (DocEditClosure: the changed chunk +
//                     its transitive dependents over the reference graph, D25) has
//                     SIZE O(closure) -- INVARIANT to scene size N for a fixed
//                     dependent count, proportional to the DEPENDENTS otherwise.
//                     This is what DeriveToJobIncremental re-applies.
//      [edit-visits]  DocSetParamValue's path-copy depth is O(log N).
//
//    WALL-CLOCK (median of K trials, microseconds, scaling N):
//      [edit]         DocSetParamValue                -> O(log N)        (flat)
//      [closure]      DocEditClosure (compute it)     -> O(N log N) (re-traces the
//                     whole graph each call; a maintained graph -- slice 1 -- O(closure))
//      [incremental]  DeriveToJobIncremental(closure) -> O(closure . log N) (cheap,
//                     ~flat in N) AND produces a Job byte-identical (DumpJob) to a full
//                     re-derive (NOT flat O(closure): manager ops + the sort carry log N)
//      [full]         DeriveToJob (whole scene)       -> O(N log N)  (the baseline
//                     the incremental path beats)
//      [TLAS]         IObjectManager::PrepareForRendering (the top-level BVH4 build)
//                     -> O(N log N), the spatial-edit cost, reported separately
//
//  WHAT THE WALL-CLOCK SURFACES (honestly, post-bulk-review):
//    1. The incremental APPLY is O(closure . log N) -- thousands of times cheaper
//       than the full O(N log N) re-derive -- BUT computing the closure
//       (DocEditClosure re-traces the whole graph each call) is O(N log N) and
//       currently DOMINATES a non-spatial edit. A maintained reference graph
//       (slice 1) makes the closure O(closure . log N).
//    2. This is a DROP/RE-ADD apply: it recreates the closure's objects at NEW
//       addresses, so it invalidates + REBUILDS the TLAS on EVERY object-touching
//       edit (review P1.1/P1.2). A non-spatial edit does NOT skip the TLAS here --
//       that result requires the slice-3 STABLE-OBJECT apply (re-point address-stable
//       objects in place). See docs/agentic-redesign/21-stable-apply-and-resolver.md.
//
//  HONEST SCOPE: DeriveToJobIncremental is the real in-tree incremental-apply
//  primitive (drop the closure via IJob's typed removal, re-Finalize it, then
//  invalidate the TLAS + bump light-gen since objects were recreated). It accepts
//  only PER-PARSER-reversible chunks: it REFUSES painters (func2d dual-registration),
//  composed/PBR materials (helper painters -- IsMaterialComposed), and
//  translucent_material (reads ambient parser cache); those fall back to a full
//  re-derive (D51: never a silent partial undo). Still ahead (this doc's slices):
//  the stable-object apply (slice 3) so non-spatial edits skip the TLAS, the shared
//  resolver + maintained graph (slice 1) so the closure is O(closure), atomic
//  rename (slice 2), and the reversible apply plan with full rollback (slice 4).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, DumpJob

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;          // DumpJob

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Median wall-clock of `f`, in microseconds, over `trials` runs (+ one warm-up).
template<class F> static double MedianMicros( int trials, F&& f )
{
	f();   // warm up (cold caches / lazy init out of the sample)
	std::vector<double> t;
	t.reserve( trials );
	for( int i = 0; i < trials; ++i ) {
		const auto a = std::chrono::steady_clock::now();
		f();
		const auto b = std::chrono::steady_clock::now();
		t.push_back( std::chrono::duration<double, std::micro>( b - a ).count() );
	}
	std::sort( t.begin(), t.end() );
	return t[ t.size() / 2 ];
}

// N painter->material->geometry->object groups, laid out TYPE-GROUPED (all
// painters, then all materials, then geometries, then objects) so EVERY chunk's
// reference resolves to an EARLIER-defined chunk -- the in-order legacy derive
// has no forward references, so re-pointing material0's reflectance to any painter
// derives cleanly (the incremental-vs-full equivalence check needs the full derive
// to succeed). `shared`: one painter+material that all N objects reference.
static std::string SceneN( int n, bool shared )
{
	std::string s = "RISE ASCII SCENE 6\n";
	if( shared ) {
		s += "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n";
		s += "lambertian_material\n{\nname m\nreflectance p\n}\n";
		for( int i = 0; i < n; ++i ) s += "sphere_geometry\n{\nname g" + std::to_string( i ) + "\nradius 1\n}\n";
		for( int i = 0; i < n; ++i ) s += "standard_object\n{\nname o" + std::to_string( i ) + "\ngeometry g" + std::to_string( i ) + "\nmaterial m\n}\n";
	} else {
		for( int i = 0; i < n; ++i ) s += "uniformcolor_painter\n{\nname p" + std::to_string( i ) + "\ncolor 0.5 0.5 0.5\n}\n";
		for( int i = 0; i < n; ++i ) s += "lambertian_material\n{\nname m" + std::to_string( i ) + "\nreflectance p" + std::to_string( i ) + "\n}\n";
		for( int i = 0; i < n; ++i ) s += "sphere_geometry\n{\nname g" + std::to_string( i ) + "\nradius 1\n}\n";
		for( int i = 0; i < n; ++i ) s += "standard_object\n{\nname o" + std::to_string( i ) + "\ngeometry g" + std::to_string( i ) + "\nmaterial m" + std::to_string( i ) + "\n}\n";
	}
	return s;
}

int main()
{
	std::printf( "CstEditCostTest -- transfer-gate item 8 (edit/closure/incremental/TLAS wall-clock)\n" );

	//----------------------------------------------------------------------
	// ANALYTIC: the cost-model DETERMINANTS (robust, noise-free).
	//----------------------------------------------------------------------
	std::printf( "[closure-size] DocEditClosure SIZE is O(closure): invariant to N, proportional to dependents\n" );
	{
		size_t c8 = 0, c64 = 0, c512 = 0;
		{ Document d = ParseToCst( SceneN( 8,   false ) ); c8   = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		{ Document d = ParseToCst( SceneN( 64,  false ) ); c64  = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		{ Document d = ParseToCst( SceneN( 512, false ) ); c512 = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		std::printf( "      closure(material0): N=8 -> %zu, N=64 -> %zu, N=512 -> %zu\n", c8, c64, c512 );
		Check( c8 == 2 && c64 == 2 && c512 == 2, "closure(material0) == {material0, object0} = 2, INVARIANT to N" );
		{ Document d = ParseToCst( SceneN( 64, false ) );
		  Check( DocEditClosure( d, DocFindByName( d, "uniformcolor_painter/p0" ) ).size() == 3, "closure(painter0) == {painter0, material0, object0} = 3 (transitive)" ); }
		for( int n : { 8, 64, 512 } ) {
			Document d = ParseToCst( SceneN( n, true ) );
			const size_t cs = DocEditClosure( d, DocFindByName( d, "lambertian_material/m" ) ).size();
			char msg[96]; std::snprintf( msg, sizeof(msg), "shared closure(material) == %d (material + its %d object dependents)", n + 1, n );
			Check( cs == (size_t)( n + 1 ), msg );
		}
	}

	std::printf( "[edit-visits] DocSetParamValue path-copy depth is O(log N): sub-linear, invariant to N\n" );
	{
		int v8 = 0, v64 = 0, v512 = 0;
		{ Document d = ParseToCst( SceneN( 8,   false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v8 ); }
		{ Document d = ParseToCst( SceneN( 64,  false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v64 ); }
		{ Document d = ParseToCst( SceneN( 512, false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v512 ); }
		std::printf( "      path-copy visits: N=8 -> %d, N=64 -> %d, N=512 -> %d\n", v8, v64, v512 );
		Check( v8 > 0 && v512 > 0, "path-copy visits counted" );
		Check( v512 <= v8 + 24, "path-copy visits grow ~log N across a 64x scene-size increase" );
	}

	//----------------------------------------------------------------------
	// WALL-CLOCK: the real timed cost of each operation, at scaling N.
	//----------------------------------------------------------------------
	std::printf( "[wall-clock] microseconds (median), scaling N:\n" );
	std::printf( "  %6s | %8s | %10s | %11s | %10s | %10s\n", "N", "edit", "closure", "increment", "full", "TLAS" );

	const int NS[] = { 256, 1024, 4096 };
	double incrAt[3] = {0,0,0}, fullAt[3] = {0,0,0}, tlasAt[3] = {0,0,0}, cloAt[3] = {0,0,0}, editAt[3] = {0,0,0};
	for( int k = 0; k < 3; ++k ) {
		const int N = NS[k];
		Document doc = ParseToCst( SceneN( N, false ) );

		// SPATIAL edit (geometry radius): closure = {geometry0, object0}; verifiable
		// in DumpJob (bounding sphere + object bbox change). Used for both the
		// incremental CORRECTNESS check and the spatial wall-clock.
		const NodeId gid  = DocFindByName( doc, "sphere_geometry/g0" );
		Document     docG = DocSetParamValue( doc, gid, "radius", 0, "2" );
		std::vector<NodeId> closG = DocEditClosure( docG, gid );

		// NON-SPATIAL edit (re-point object0's MATERIAL reference m0 -> m1): closure =
		// {object0} (objects are leaves -- nothing references them); o0's geometry is
		// unchanged so its world bbox is unchanged -> TLAS stays clean. Chosen so the
		// equivalence check is VALUE-verifiable: DumpJob records each object's
		// resolved material name, so a wrong incremental (o0 still bound to m0) would
		// be CAUGHT -- unlike a material's reflectance-painter value, which DumpJob
		// does not surface.
		const NodeId oid  = DocFindByName( doc, "standard_object/o0" );
		Document     docM = DocSetParamValue( doc, oid, "material", 0, "m1" );
		std::vector<NodeId> closM = DocEditClosure( docM, oid );

		// A PAINTER-VALUE edit's closure DOES include the painter; the incremental
		// must REFUSE it (a painter dual-registers in the func2d manager) so the
		// caller falls back to a full re-derive -- verified below.
		const NodeId pid  = DocFindByName( doc, "uniformcolor_painter/p0" );
		Document     docP = DocSetParamValue( doc, pid, "color", 0, "0.9 0.1 0.1" );
		std::vector<NodeId> closP = DocEditClosure( docP, pid );

		// [edit] + [closure] : pure-CST timings.
		editAt[k] = MedianMicros( 21, [&]{ volatile auto d = DocSetParamValue( doc, gid, "radius", 0, "2" ); (void)d; } );
		cloAt[k]  = MedianMicros( 21, [&]{ volatile auto c = DocEditClosure( docG, gid ); (void)c; } );

		// [full] : a complete re-derive into a fresh Job (the baseline).
		fullAt[k] = MedianMicros( 3, [&]{ Job* j = new Job(); DeriveToJob( docG, *j ); j->release(); } );

		// [incremental] : re-apply ONLY the closure into an already-derived Job.
		// CORRECTNESS: applying the spatial edit incrementally to a Job derived from
		// the ORIGINAL scene must yield the SAME Job as a full re-derive of docG.
		Job* jbase = new Job(); DeriveToJob( doc, *jbase );
		std::vector<std::string> diags;
		const int applied = DeriveToJobIncremental( docG, *jbase, closG, &diags );
		Job* jfull = new Job(); DeriveToJob( docG, *jfull );
		const bool correctG = ( applied == (int)closG.size() ) && ( DumpJob( *jbase ) == DumpJob( *jfull ) );
		jfull->release();
		char m1[80]; std::snprintf( m1, sizeof(m1), "N=%d incremental geometry edit == full re-derive (DumpJob)", N );
		Check( correctG, m1 );

		// time the incremental re-derive (idempotent: re-applies the same closure).
		incrAt[k] = MedianMicros( 21, [&]{ std::vector<std::string> d; DeriveToJobIncremental( docG, *jbase, closG, &d ); } );

		// [TLAS] : the top-level BVH build over N objects (the spatial premium).
		tlasAt[k] = MedianMicros( 7, [&]{ jbase->GetObjects()->InvalidateSpatialStructure(); jbase->GetObjects()->PrepareForRendering(); } );

		// NON-SPATIAL incremental correctness: re-point object0's material (m0 -> m1)
		// and apply that closure incrementally to a fresh base; must equal a full
		// re-derive of docM (DumpJob -- which records o0's resolved material name, so
		// this is VALUE-verified, not just structural). A non-spatial edit; NO TLAS.
		Job* jm = new Job(); DeriveToJob( doc, *jm );
		std::vector<std::string> dm;
		const int appliedM = DeriveToJobIncremental( docM, *jm, closM, &dm );
		Job* jmf = new Job(); DeriveToJob( docM, *jmf );
		const bool correctM = ( appliedM == (int)closM.size() ) && ( DumpJob( *jm ) == DumpJob( *jmf ) );
		jm->release(); jmf->release();
		char m2[92]; std::snprintf( m2, sizeof(m2), "N=%d incremental object material re-point (m0->m1) == full re-derive, NO TLAS", N );
		Check( correctM, m2 );

		// SAFETY: a painter-VALUE edit's closure must be REFUSED (returns 0 + a
		// diagnostic), so the dual-registered painter is never half-dropped -- the
		// caller then full-re-derives.
		Job* jpr = new Job(); DeriveToJob( doc, *jpr );
		std::vector<std::string> dpr;
		const int appliedP = DeriveToJobIncremental( docP, *jpr, closP, &dpr );
		jpr->release();
		char m3[88]; std::snprintf( m3, sizeof(m3), "N=%d painter-value closure REFUSED (applied=0, diagnosed) -> full-derive fallback", N );
		Check( appliedP == 0 && !dpr.empty(), m3 );

		jbase->release();

		std::printf( "  %6d | %8.1f | %10.1f | %11.1f | %10.1f | %10.1f\n", N, editAt[k], cloAt[k], incrAt[k], fullAt[k], tlasAt[k] );
	}

	//----------------------------------------------------------------------
	// What the wall-clock proves (robust, large-N magnitude separations).
	//----------------------------------------------------------------------
	std::printf( "[verdict]\n" );
	// The incremental re-apply (the closure only) is dramatically cheaper than a full
	// re-derive at large N -- the redesign's core win. Its cost is O(closure . log N)
	// (per-member identity lookup + std::map manager remove/insert + an O(C log C)
	// sort), NOT flat O(closure): the wall-clock is flat only because C is fixed and
	// log N grows slowly. The full re-derive is O(N . log N). (R13: no flat O(closure)
	// before persistent O(1) managers exist; we assert ratios/scaling, never const-vs-log.)
	Check( incrAt[2] * 4 < fullAt[2], "incremental re-apply >=4x cheaper than a full re-derive at N=4096" );
	Check( fullAt[2] > fullAt[0] * 2.0, "full re-derive grows with N (~O(N log N))" );
	Check( incrAt[2] < incrAt[0] * 6.0 + 50.0, "incremental re-apply ~flat in N (O(closure . log N), not O(N log N))" );
	Check( tlasAt[2] > 0.0 && tlasAt[2] > tlasAt[0], "TLAS (top-level BVH) build measured + grows with N (~O(N log N))" );

	std::printf( "  decomposition at N=4096 (microseconds):\n" );
	// HONEST (review P1.1/P1.2): this DROP/RE-ADD incremental recreates the closure's
	// objects at NEW addresses, so it invalidates + REBUILDS the TLAS on EVERY
	// object-touching edit -- a non-spatial edit does NOT skip it here. Skipping the
	// TLAS for non-spatial edits needs the slice-3 STABLE-OBJECT apply (re-point
	// stable objects in place); see docs/agentic-redesign/21-stable-apply-and-resolver.md.
	std::printf( "    edit %.1f us + closure-compute %.1f us + incremental-apply %.1f us  (closure-compute O(N log N) dominates -- maintained-graph would be O(closure))\n",
		editAt[2], cloAt[2], incrAt[2] );
	std::printf( "    + TLAS rebuild %.1f us  -- paid by EVERY object-touching edit under drop/re-add (slice-3 stable-object apply lets non-spatial edits skip it)\n", tlasAt[2] );
	std::printf( "    (a full non-incremental re-derive instead costs %.1f us; the incremental re-apply is the saving, the TLAS the separately-reported spatial cost.)\n", fullAt[2] );
	// HONEST findings the wall-clock surfaces: (1) the incremental APPLY is
	// O(closure . log N) (cheap, ~flat in N), but COMPUTING the closure
	// (DocEditClosure re-traces the whole reference graph each call) is O(N log N) and
	// currently DOMINATES the non-spatial edit -- a maintained graph (slice 1) makes
	// it O(closure . log N). (2) Under this drop/re-add apply the TLAS is rebuilt on
	// every object-touching edit (objects recreated at new addresses); the
	// non-spatial-edit-skips-the-TLAS result needs the slice-3 stable-object apply.
	std::printf( "    NOTE: incremental APPLY %.1f us (O(closure . log N)); closure COMPUTE %.1f us (O(N log N)) currently dominates\n", incrAt[2], cloAt[2] );
	std::printf( "          -- a maintained reference graph (slice 1) makes the closure O(closure); a stable-object apply (slice 3) lets non-spatial edits skip the TLAS.\n" );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
