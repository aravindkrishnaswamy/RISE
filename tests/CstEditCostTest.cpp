//////////////////////////////////////////////////////////////////////
//
//  CstEditCostTest.cpp - transfer-gate item 8, re-measured on the SLICE-3
//  STABLE-OBJECT apply (slice 4 of docs/agentic-redesign/21-stable-apply-and-
//  resolver.md): measure a NON-SPATIAL edit AND a SPATIAL edit on the in-tree CST
//  path, in WALL-CLOCK, and report the TLAS cost SEPARATELY -- now that a non-spatial
//  edit genuinely SKIPS the TLAS rebuild.
//
//  The slice-3 stable-object apply re-points objects IN PLACE (address-stable) and
//  invalidates the top-level acceleration only when a re-pointed object's world bbox
//  actually changes.  So the item-8 result is now VALID: a non-spatial edit
//  (material/painter value) does NOT touch the TLAS; a spatial edit (geometry-extent /
//  transform) pays the engine's O(N log N) BVH build, reported separately.
//
//    ANALYTIC (robust, noise-free):
//      [closure-size] the re-derive CLOSURE (DocEditClosure, D25) has SIZE O(closure)
//                     -- invariant to scene size N for a fixed dependent count.
//      [edit-visits]  DocSetParamValue's path-copy depth is O(log N).
//
//    STABLE-OBJECT (robust, generation-counter -- the item-8 P1.2 result):
//      [tlas-skip]    after the TLAS is built, a NON-SPATIAL incremental edit leaves
//                     IObjectManager::GetSpatialStructureGeneration UNCHANGED (TLAS
//                     preserved) while a SPATIAL one ADVANCES it (TLAS invalidated).
//                     This is the direct, noise-free proof that non-spatial edits skip
//                     the TLAS -- it does not depend on wall-clock.
//
//    WALL-CLOCK (median of K trials, microseconds, scaling N):
//      [edit]         DocSetParamValue                -> O(log N)        (flat)
//      [closure]      DocEditClosure(doc,id) (from scratch) -> O(N log N) (re-traces the
//                     whole graph each call)
//      [clo:graph]    DocEditClosure(id, graph) over a PRE-BUILT graph (slice 5) ->
//                     O(closure . log N), ~flat in N: the BFS in ISOLATION (the graph
//                     build is NOT in this number)
//      [maintained]   the END-TO-END per-edit cost via a held MaintainedReferenceGraph
//                     (review P1.6): a non-reference value edit = doc edit + an O(1)
//                     reuse decision (NO graph rebuild, NO O(N) stamp recompute) + the
//                     closure -> O(log N + closure), ~flat.  THIS is the honest per-edit
//                     cost a holder pays; [clo:graph] is just its BFS component.
//      [incremental]  DeriveToJobIncremental(closure) -> O(closure . log N) (cheap,
//                     ~flat in N) AND produces a Job byte-identical (DumpJob) to a full
//                     re-derive
//      [full]         DeriveToJob (whole scene)       -> O(N log N)  (the baseline beaten)
//      [prep:spatial] PrepareForRendering AFTER a spatial edit -> rebuilds the BVH4,
//                     O(N log N): the spatial-edit premium
//      [prep:nonspat] PrepareForRendering AFTER a non-spatial edit -> SKIPS the O(N log N)
//                     BVH rebuild (the BVH is still valid); only an O(N) per-object
//                     realize sweep remains -- the SAVING the stable-object apply delivers
//                     (so prep:nonsp scales ~O(N), far below the spatial rebuild, NOT O(1))
//
//  HONEST SCOPE (R13): the bounds carry log factors -- the incremental apply is
//  O(closure . log N) (per-member identity lookup + std::map manager remove/insert + an
//  O(C log C) sort), NOT flat O(closure); a full derive is O(N . log N).  The suite
//  asserts only ratios/scaling, never constant-vs-log.  DeriveToJobIncremental accepts
//  only PER-PARSER-reversible chunks: it REFUSES painters (func2d dual-registration),
//  composed materials (helper painters), translucent (ambient parser cache), gltf_import
//  (bulk), a non-standard_object (csg), an optional-slot removal, and animated docs;
//  those fall back to a full re-derive (D51).  Slice 5 adds the maintained-graph closure
//  primitive (the (id, graph) overload measured here); the remaining D35 step is routing
//  the derive's own resolution through the recorded graph so the static graph and the
//  apply resolution cannot drift even in principle (a holder-side / parser-instrumentation
//  integration beyond this kernel slice).
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

// N painter->material->geometry->object groups, laid out TYPE-GROUPED (all painters,
// then materials, geometries, objects) so EVERY chunk's reference resolves to an
// EARLIER-defined chunk (the in-order legacy derive has no forward references).
// `shared`: one painter+material that all N objects reference.
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
	std::printf( "CstEditCostTest -- item 8 on the slice-3 stable-object apply (edit/closure/incremental/full/TLAS-skip)\n" );

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
	// WALL-CLOCK + STABLE-OBJECT generation checks, at scaling N.
	//----------------------------------------------------------------------
	std::printf( "[wall-clock] microseconds (median), scaling N:\n" );
	std::printf( "  %6s | %8s | %10s | %10s | %11s | %10s | %11s | %11s\n", "N", "edit", "closure", "clo:graph", "increment", "full", "prep:spat", "prep:nonsp" );

	const int NS[] = { 256, 1024, 4096 };
	double incrAt[3] = {0,0,0}, fullAt[3] = {0,0,0}, prepSpatAt[3] = {0,0,0}, prepNonAt[3] = {0,0,0}, cloAt[3] = {0,0,0}, cloGraphAt[3] = {0,0,0}, mtnAt[3] = {0,0,0}, editAt[3] = {0,0,0};
	for( int k = 0; k < 3; ++k ) {
		const int N = NS[k];
		Document doc = ParseToCst( SceneN( N, false ) );

		// SPATIAL edit (geometry radius): closure = {geometry0, object0}; the object's
		// world bbox changes -> the stable apply invalidates the TLAS.
		const NodeId gid  = DocFindByName( doc, "sphere_geometry/g0" );
		Document     docG = DocSetParamValue( doc, gid, "radius", 0, "2" );
		std::vector<NodeId> closG = DocEditClosure( docG, gid );

		// NON-SPATIAL edit (re-point object0's MATERIAL reference m0 -> m1): closure =
		// {object0}; o0's geometry/transform are unchanged so its world bbox is unchanged
		// -> the stable apply RE-POINTS o0 in place and SKIPS the TLAS.  DumpJob records
		// o0's resolved material name, so a wrong incremental is caught.
		const NodeId oid  = DocFindByName( doc, "standard_object/o0" );
		Document     docM = DocSetParamValue( doc, oid, "material", 0, "m1" );
		std::vector<NodeId> closM = DocEditClosure( docM, oid );

		// A PAINTER-VALUE edit's closure includes the painter; the incremental REFUSES it
		// (a painter dual-registers in the func2d manager) -> full-derive fallback.
		const NodeId pid  = DocFindByName( doc, "uniformcolor_painter/p0" );
		Document     docP = DocSetParamValue( doc, pid, "color", 0, "0.9 0.1 0.1" );
		std::vector<NodeId> closP = DocEditClosure( docP, pid );

		// [edit] + [closure] : pure-CST timings.
		editAt[k] = MedianMicros( 21, [&]{ volatile auto d = DocSetParamValue( doc, gid, "radius", 0, "2" ); (void)d; } );
		cloAt[k]  = MedianMicros( 21, [&]{ volatile auto c = DocEditClosure( docG, gid ); (void)c; } );

		// [clo:graph] : the slice-5 maintained-graph endpoint -- build the reference graph
		// ONCE (the O(N log N) trace a holder amortises), then find the closure via the
		// (id, graph) overload, a pure O(closure . log N) reverse-BFS with no re-trace.
		const ReferenceGraph rg = BuildReferenceGraph( docG );
		cloGraphAt[k] = MedianMicros( 21, [&]{ volatile auto c = DocEditClosure( gid, rg ); (void)c; } );
		Check( DocEditClosure( gid, rg ) == DocEditClosure( docG, gid ), "closure(id, reused graph) == closure(doc, id) (the overload is equivalence-preserving)" );

		// [maintained] : the END-TO-END per-edit cost via a held MaintainedReferenceGraph
		// (review P1.6 -- NOT just the isolated BFS).  A NON-reference value edit (radius)
		// updates the doc + decides reuse from the EDIT in O(1) (no graph rebuild, no O(N)
		// stamp recompute), then finds the closure over the reused graph.  Construct the
		// holder OUTSIDE the timed loop (a holder amortises the one-time O(N) build); time
		// edit+closure inside.  This is what the holder actually pays per edit -- O(log N +
		// closure), ~flat -- vs the from-scratch edit + O(N log N) closure-COMPUTE.
		{
			MaintainedReferenceGraph mg( docG );
			const NodeId mgid = DocFindByName( mg.Doc(), "sphere_geometry/g0" );
			mtnAt[k] = MedianMicros( 21, [&]{ mg.SetParamValue( mgid, "radius", 0, "2" ); volatile auto c = mg.EditClosure( mgid ); (void)c; } );
			Check( !mg.LastEditRebuilt(), "maintained: the non-reference (radius) edit did NOT rebuild the graph (reuse from the edit, not the stamp)" );
		}

		// [full] : a complete re-derive into a fresh Job (the baseline).
		fullAt[k] = MedianMicros( 3, [&]{ Job* j = new Job(); DeriveToJob( docG, *j ); j->release(); } );

		// [incremental] : re-apply ONLY the closure into an already-derived Job.  CORRECTNESS:
		// applying the spatial edit incrementally to a Job derived from the ORIGINAL scene
		// must yield the SAME Job as a full re-derive of docG.
		Job* jbase = new Job(); DeriveToJob( doc, *jbase );
		std::vector<std::string> diags;
		const int applied = DeriveToJobIncremental( docG, *jbase, closG, &diags );
		Job* jfull = new Job(); DeriveToJob( docG, *jfull );
		const bool correctG = ( applied == (int)closG.size() ) && ( DumpJob( *jbase ) == DumpJob( *jfull ) );
		jfull->release();
		char m1[80]; std::snprintf( m1, sizeof(m1), "N=%d incremental geometry edit == full re-derive (DumpJob)", N );
		Check( correctG, m1 );
		incrAt[k] = MedianMicros( 21, [&]{ std::vector<std::string> d; DeriveToJobIncremental( docG, *jbase, closG, &d ); } );

		// NON-SPATIAL incremental correctness (value-verified via DumpJob's material name).
		Job* jm = new Job(); DeriveToJob( doc, *jm );
		std::vector<std::string> dm;
		const int appliedM = DeriveToJobIncremental( docM, *jm, closM, &dm );
		Job* jmf = new Job(); DeriveToJob( docM, *jmf );
		const bool correctM = ( appliedM == (int)closM.size() ) && ( DumpJob( *jm ) == DumpJob( *jmf ) );
		jm->release(); jmf->release();
		char m2[112]; std::snprintf( m2, sizeof(m2), "N=%d incremental object material re-point (m0->m1) == full re-derive (DumpJob value-verified)", N );
		Check( correctM, m2 );

		// SAFETY: a painter-VALUE edit's closure must be REFUSED (returns 0 + a diagnostic).
		Job* jpr = new Job(); DeriveToJob( doc, *jpr );
		std::vector<std::string> dpr;
		const int appliedP = DeriveToJobIncremental( docP, *jpr, closP, &dpr );
		jpr->release();
		char m3[96]; std::snprintf( m3, sizeof(m3), "N=%d painter-value closure REFUSED (applied=0, diagnosed) -> full-derive fallback", N );
		Check( appliedP == 0 && !dpr.empty(), m3 );

		// ---- STABLE-OBJECT: the item-8 P1.2 result, generation-counter verified ----
		// SPATIAL: build the TLAS, apply the spatial edit -> the spatial generation MUST
		// advance (TLAS invalidated), and the subsequent PrepareForRendering rebuilds it.
		{
			Job* js = new Job(); DeriveToJob( doc, *js );
			js->GetObjects()->PrepareForRendering();
			const unsigned long long gs0 = js->GetObjects()->GetSpatialStructureGeneration();
			std::vector<std::string> ds; DeriveToJobIncremental( docG, *js, closG, &ds );
			char ms[104]; std::snprintf( ms, sizeof(ms), "N=%d SPATIAL edit invalidates the TLAS (spatial generation advanced)", N );
			Check( js->GetObjects()->GetSpatialStructureGeneration() > gs0, ms );
			prepSpatAt[k] = MedianMicros( 7, [&]{ js->GetObjects()->InvalidateSpatialStructure(); js->GetObjects()->PrepareForRendering(); } );
			js->release();
		}
		// NON-SPATIAL: build the TLAS, apply the non-spatial edit -> the spatial generation
		// MUST be UNCHANGED (TLAS preserved -- the slice-3 skip), and the subsequent
		// PrepareForRendering SKIPS the O(N log N) BVH rebuild (the BVH is still valid),
		// leaving only its O(N) per-object realize sweep.
		{
			Job* jns = new Job(); DeriveToJob( doc, *jns );
			jns->GetObjects()->PrepareForRendering();
			const unsigned long long g0 = jns->GetObjects()->GetSpatialStructureGeneration();
			std::vector<std::string> dns; DeriveToJobIncremental( docM, *jns, closM, &dns );
			char mn[104]; std::snprintf( mn, sizeof(mn), "N=%d NON-SPATIAL edit SKIPS the TLAS (spatial generation unchanged -- slice 3)", N );
			Check( jns->GetObjects()->GetSpatialStructureGeneration() == g0, mn );
			prepNonAt[k] = MedianMicros( 21, [&]{ jns->GetObjects()->PrepareForRendering(); } );   // skips the BVH rebuild; O(N) realize sweep only
			jns->release();
		}

		jbase->release();
		std::printf( "  %6d | %8.1f | %10.1f | %10.1f | %11.1f | %10.1f | %11.1f | %11.1f\n", N, editAt[k], cloAt[k], cloGraphAt[k], incrAt[k], fullAt[k], prepSpatAt[k], prepNonAt[k] );
	}

	//----------------------------------------------------------------------
	// What the wall-clock + generation checks prove.
	//----------------------------------------------------------------------
	std::printf( "[verdict]\n" );
	// The incremental re-apply (closure only) is dramatically cheaper than a full
	// re-derive at large N. Its cost is O(closure . log N), NOT flat O(closure)
	// (R13: we assert ratios/scaling, never const-vs-log).
	Check( incrAt[2] * 4 < fullAt[2], "incremental re-apply >=4x cheaper than a full re-derive at N=4096" );
	Check( fullAt[2] > fullAt[0] * 2.0, "full re-derive grows with N (~O(N log N))" );
	Check( incrAt[2] < incrAt[0] * 6.0 + 50.0, "incremental re-apply ~flat in N (O(closure . log N), not O(N log N))" );
	// The TLAS-skip result (slice 3): a non-spatial edit's post-edit PrepareForRendering
	// SKIPS the O(N log N) BVH rebuild (BVH still valid; only an O(N) realize sweep runs),
	// so it is DRAMATICALLY cheaper than a spatial edit's rebuild. The generation-counter
	// Checks above are the noise-free proof; this is the wall-clock.
	Check( prepSpatAt[2] > prepSpatAt[0], "spatial TLAS rebuild grows with N (~O(N log N))" );
	Check( prepNonAt[2] * 3 < prepSpatAt[2], "NON-spatial edit's post-edit prepare materially cheaper than the spatial rebuild at N=4096 (TLAS skipped; gen-counter Checks are the noise-free proof)" );
	// The maintained-graph endpoint (slice 5): finding the closure over a PRE-BUILT graph
	// is O(closure . log N) -- flat in N -- vs the from-scratch O(N log N) re-trace, which
	// removes the closure-COMPUTE term slice 4 measured as the dominant non-spatial cost.
	Check( cloGraphAt[2] * 4 < cloAt[2], "closure via a maintained graph >=4x cheaper than the from-scratch re-trace at N=4096" );
	Check( cloGraphAt[2] < cloGraphAt[0] * 6.0 + 50.0, "closure via a maintained graph ~flat in N (O(closure . log N), not O(N log N))" );
	// The END-TO-END maintained per-edit cost (review P1.6): a non-reference value edit via
	// the holder (edit + O(1) reuse decision + closure) is ~flat and far below the
	// from-scratch per-edit cost (edit + O(N log N) closure-COMPUTE).  This is the honest
	// end-to-end number -- not the isolated BFS.
	Check( mtnAt[2] * 4 < ( editAt[2] + cloAt[2] ), "maintained END-TO-END edit (edit+reuse-decision+closure) >=4x cheaper than the from-scratch edit+closure at N=4096" );
	Check( mtnAt[2] < mtnAt[0] * 6.0 + 50.0, "maintained END-TO-END edit ~flat in N (no O(N) rebuild on a non-reference edit)" );

	std::printf( "  decomposition at N=4096 (microseconds):\n" );
	std::printf( "    maintained END-TO-END per-edit (non-reference): %.1f us (edit + O(1) reuse decision + closure), ~flat -- vs from-scratch edit+closure %.1f us.\n",
		mtnAt[2], editAt[2] + cloAt[2] );
	std::printf( "    edit %.1f + closure-compute %.1f (from scratch) -> %.1f (over a maintained graph, slice 5) + incremental-apply %.1f\n",
		editAt[2], cloAt[2], cloGraphAt[2], incrAt[2] );
	std::printf( "    (the from-scratch O(N log N) closure-COMPUTE was the dominant non-spatial cost; the maintained-graph (id, graph) overload makes it O(closure . log N), ~flat in N.)\n" );
	std::printf( "    SPATIAL edit additionally pays the TLAS rebuild %.1f us (O(N log N)); a NON-SPATIAL edit SKIPS it -- post-edit prepare is %.1f us (the BVH stays valid).\n",
		prepSpatAt[2], prepNonAt[2] );
	std::printf( "    (a full non-incremental re-derive instead costs %.1f us; the incremental re-apply + the skipped TLAS are the saving.)\n", fullAt[2] );

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
