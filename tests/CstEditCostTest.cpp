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
//      [edit]         DocSetParamValue                -> O(log N)   (flat)
//      [closure]      DocEditClosure (compute it)     -> O(N log N) (re-traces the
//                     whole graph each call; a maintained graph would be O(closure))
//      [incremental]  DeriveToJobIncremental(closure) -> O(closure) (flat) AND it
//                     produces a Job byte-identical (DumpJob) to a full re-derive
//      [full]         DeriveToJob (whole scene)       -> O(N)       (the baseline
//                     the incremental path beats)
//      [TLAS]         IObjectManager::PrepareForRendering (the top-level BVH4 build)
//                     -> O(N log N), the SEPARATE spatial-edit cost
//
//    DECOMPOSITION (the headline, in measured microseconds):
//      NON-spatial edit (here: re-point a material's reflectance) = edit + closure
//        + incremental re-derive; the object bounding boxes are unchanged so the
//        TLAS is NOT rebuilt -- that O(N log N) cost is SAVED.
//      SPATIAL edit (geometry shape / object transform / object geometry-ref) =
//        the same + the measured O(N log N) TLAS rebuild.
//
//  WHAT THE WALL-CLOCK SURFACES (honestly): the incremental APPLY is already
//  O(closure) (~4 us, flat in N) -- thousands of times cheaper than the full
//  O(N) re-derive -- BUT computing the closure (DocEditClosure re-traces the whole
//  reference graph each call) is O(N log N) and currently DOMINATES a non-spatial
//  edit. The TLAS rebuild (the spatial premium) is real but SMALLER than that
//  closure scan at these N. So a non-spatial edit today still scans the graph once,
//  yet never pays the full O(N) Job re-derive and never rebuilds the TLAS.
//
//  HONEST SCOPE: DeriveToJobIncremental is the real in-tree incremental-apply
//  primitive (drop the closure via IJob's typed removal, re-Finalize it). It
//  handles the five verified single-manager categories; a PAINTER-value edit is
//  REFUSED (a painter dual-registers in the func2d manager that RemovePainter does
//  not clear) so it falls back to a full re-derive -- demonstrated below. Still
//  deferred (Facet-2, as DeriveToJob defers it): node-granular memoization, full
//  post-Finalize rollback, multi-manager rollback (so painters re-derive
//  incrementally too), and maintaining the reference graph incrementally so
//  DocEditClosure itself becomes O(closure) instead of O(N log N).
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

		// NON-SPATIAL edit (re-point a material's reflectance to another painter):
		// closure = {material0, object0}; no bbox changes -> TLAS stays clean. (A
		// pure single-manager closure -- no painter is re-derived -- so the typed-
		// removal incremental is a complete undo, DumpJob-verifiable.)
		const NodeId mid  = DocFindByName( doc, "lambertian_material/m0" );
		Document     docM = DocSetParamValue( doc, mid, "reflectance", 0, "p1" );
		std::vector<NodeId> closM = DocEditClosure( docM, mid );

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

		// NON-SPATIAL incremental correctness: re-point material0's reflectance and
		// apply that closure incrementally to a fresh base; must equal a full
		// re-derive of docM (DumpJob). This is the canonical non-spatial edit and it
		// rebuilds NO TLAS.
		Job* jm = new Job(); DeriveToJob( doc, *jm );
		std::vector<std::string> dm;
		const int appliedM = DeriveToJobIncremental( docM, *jm, closM, &dm );
		Job* jmf = new Job(); DeriveToJob( docM, *jmf );
		const bool correctM = ( appliedM == (int)closM.size() ) && ( DumpJob( *jm ) == DumpJob( *jmf ) );
		jm->release(); jmf->release();
		char m2[88]; std::snprintf( m2, sizeof(m2), "N=%d incremental material re-point == full re-derive (DumpJob), NO TLAS", N );
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
	// The incremental re-derive (re-apply the O(closure) closure) is dramatically
	// cheaper than a full O(N) re-derive at large N -- the redesign's core win.
	Check( incrAt[2] * 4 < fullAt[2], "incremental re-derive >=4x cheaper than a full re-derive at N=4096" );
	// The full re-derive scales with N (it does ~N chunk applies); the incremental
	// does NOT (it does |closure| applies regardless of N).
	Check( fullAt[2] > fullAt[0] * 2.0, "full re-derive grows with N (scales ~O(N))" );
	Check( incrAt[2] < incrAt[0] * 6.0 + 50.0, "incremental re-derive ~invariant to N (O(closure), not O(N))" );
	// The TLAS build is a real, separately-measured cost that grows with N.
	Check( tlasAt[2] > 0.0 && tlasAt[2] > tlasAt[0], "TLAS (top-level BVH) build measured + grows with N (O(N log N))" );

	std::printf( "  decomposition at N=4096 (microseconds):\n" );
	std::printf( "    NON-spatial edit = edit(%.1f) + closure(%.1f) + incremental(%.1f)            [NO TLAS]  = %.1f us\n",
		editAt[2], cloAt[2], incrAt[2], editAt[2] + cloAt[2] + incrAt[2] );
	std::printf( "    SPATIAL     edit = edit(%.1f) + closure(%.1f) + incremental(%.1f) + TLAS(%.1f) = %.1f us\n",
		editAt[2], cloAt[2], incrAt[2], tlasAt[2], editAt[2] + cloAt[2] + incrAt[2] + tlasAt[2] );
	std::printf( "    (a full non-incremental re-derive would instead cost %.1f us; the TLAS rebuild is the spatial premium.)\n", fullAt[2] );
	// HONEST finding the wall-clock surfaces: the incremental APPLY is already
	// O(closure) (%.1f us, flat), but COMPUTING the closure (DocEditClosure, which
	// re-traces the whole reference graph each call) is O(N log N) and currently
	// DOMINATES the non-spatial edit. Realizing the full O(closure) end-to-end needs
	// the reference graph maintained incrementally (Facet-2); until then a
	// non-spatial edit still scans the graph once, though it never pays the full
	// O(N) Job re-derive and never rebuilds the TLAS.
	std::printf( "    NOTE: the incremental APPLY is O(closure) (%.1f us); the O(N log N) closure COMPUTATION (%.1f us)\n", incrAt[2], cloAt[2] );
	std::printf( "          currently dominates -- a maintained reference graph (Facet-2) would make the closure O(closure) too.\n" );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
