//////////////////////////////////////////////////////////////////////
//
//  CstMaintainedGraphTest.cpp - slice 5 of docs/agentic-redesign/21-stable-apply-
//  and-resolver.md: the MAINTAINED-GRAPH closure primitive.  BuildReferenceGraph now
//  computes a reverse adjacency (`dependents`) in its single pass, so a caller holding
//  a (cached, stamp-validated) graph finds an edit's closure via
//  DocEditClosure(changedChunkId, graph) -- a pure O(closure . log N) reverse-BFS with
//  no document re-trace.  Locks in:
//    [equivalence] the (id, graph) overload yields the SAME closure as the from-scratch
//                  (doc, id) overload, across transitive / shared / multi-dependent /
//                  self-reference / leaf seeds.
//    [reuse]       one BuildReferenceGraph serves many closure queries (the holder
//                  amortises the O(N log N) trace over all edits between rebuilds).
//    [stamp-reuse] a graph-NEUTRAL edit (a non-reference value) leaves the stamp stable,
//                  so reusing the OLD graph on the edited doc still yields the correct
//                  closure; a graph-CHANGING edit (a reference re-point / rename) moves
//                  the stamp, signalling the holder to rebuild (P1.8 staleness in O(1)).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

// Closures are returned in unspecified order; compare as SETS.
static bool SameClosure( std::vector<NodeId> a, std::vector<NodeId> b )
{
	std::sort( a.begin(), a.end() );
	std::sort( b.begin(), b.end() );
	return a == b;
}

// Full graph equivalence: stamp (exact) + dependents (exact set-map) + edges (order-insensitive).
static bool SameGraph( const ReferenceGraph& a, const ReferenceGraph& b )
{
	if( a.stamp != b.stamp || a.dependents != b.dependents ) return false;
	std::vector<std::pair<NodeId,NodeId> > ea, eb;
	for( const ReferenceUse& e : a.edges ) ea.push_back( std::make_pair( e.sourceValueNodeId, e.targetNodeId ) );
	for( const ReferenceUse& e : b.edges ) eb.push_back( std::make_pair( e.sourceValueNodeId, e.targetNodeId ) );
	std::sort( ea.begin(), ea.end() ); std::sort( eb.begin(), eb.end() );
	return ea == eb;
}

int main()
{
	std::printf( "CstMaintainedGraphTest -- slice 5 (maintained-graph closure: equivalence / reuse / stamp)\n" );

	// painter p -> material m (+ m2) -> geometry g -> object o; o2 also -> m (shared).
	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"lambertian_material\n{\nname m2\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"
		"standard_object\n{\nname o2\ngeometry g\nmaterial m\n}\n";

	// [equivalence] the (id, graph) overload == the from-scratch (doc, id) overload, for
	// every seed shape (transitive painter, shared material, geometry, leaf object).
	{
		Document doc = ParseToCst( scene );
		const ReferenceGraph g = BuildReferenceGraph( doc );
		const char* seeds[] = { "uniformcolor_painter/p", "lambertian_material/m",
			"lambertian_material/m2", "sphere_geometry/g", "standard_object/o" };
		bool allEq = true;
		for( const char* s : seeds ) {
			const NodeId id = DocFindByName( doc, s );
			if( !SameClosure( DocEditClosure( id, g ), DocEditClosure( doc, id ) ) ) allEq = false;
		}
		Check( allEq, "(id, graph) closure == (doc, id) closure for transitive/shared/geometry/leaf seeds" );

		// Concrete shapes: painter p reaches p + both materials + both objects (transitive);
		// shared g reaches g + both objects; leaf o reaches just o.
		Check( DocEditClosure( DocFindByName( doc, "uniformcolor_painter/p" ), g ).size() == 5, "closure(painter p) = {p, m, m2, o, o2} = 5 (transitive over the graph)" );
		Check( DocEditClosure( DocFindByName( doc, "sphere_geometry/g" ), g ).size() == 3, "closure(geometry g) = {g, o, o2} = 3 (shared by both objects)" );
		Check( DocEditClosure( DocFindByName( doc, "lambertian_material/m" ), g ).size() == 3, "closure(material m) = {m, o, o2} = 3" );
		Check( DocEditClosure( DocFindByName( doc, "standard_object/o" ), g ).size() == 1, "closure(object o) = {o} = 1 (a leaf -- nothing references it)" );
	}

	// [self-reference] a scalar_painter whose graph references itself must not loop or
	// inflate the closure -- the from-scratch path excludes self-edges, the graph path
	// must match.
	{
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"scalar_painter\n{\nname s\nfile noise.dat\nmultiply s s\n}\n" );
		const ReferenceGraph g = BuildReferenceGraph( doc );
		const NodeId sid = DocFindByName( doc, "scalar_painter/s" );
		if( sid != 0 ) {
			Check( SameClosure( DocEditClosure( sid, g ), DocEditClosure( doc, sid ) ), "self-reference: (id, graph) == (doc, id)" );
			Check( DocEditClosure( sid, g ).size() == 1, "self-reference closure = {s} (the self-edge is excluded, no inflation/loop)" );
		} else {
			std::printf( "  (skip self-reference: scalar_painter did not parse in this build)\n" );
		}
	}

	// [reuse] ONE BuildReferenceGraph serves many closure queries (the holder amortises
	// the O(N log N) trace) -- every query matches its from-scratch closure.
	{
		Document doc = ParseToCst( scene );
		const ReferenceGraph g = BuildReferenceGraph( doc );
		std::vector<NodeRef> dummy;   // (no-op: just exercise repeated queries on ONE graph)
		bool allEq = true;
		for( int rep = 0; rep < 3; ++rep )
			for( const char* s : { "uniformcolor_painter/p", "lambertian_material/m", "sphere_geometry/g", "standard_object/o2" } ) {
				const NodeId id = DocFindByName( doc, s );
				if( !SameClosure( DocEditClosure( id, g ), DocEditClosure( doc, id ) ) ) allEq = false;
			}
		Check( allEq, "reuse: 12 closure queries over ONE cached graph all match from-scratch" );
	}

	// [stamp-reuse] the holder's reuse rule: reuse the cached graph iff the doc's stamp is
	// unchanged.  A graph-NEUTRAL edit (sphere radius -- a non-reference value) keeps the
	// stamp stable, so reusing the OLD graph on the edited doc yields the correct closure.
	{
		Document doc = ParseToCst( scene );
		const ReferenceGraph g0 = BuildReferenceGraph( doc );
		const NodeId gid = DocFindByName( doc, "sphere_geometry/g" );

		// graph-NEUTRAL edit: sphere radius.  Stamp stable -> the OLD graph is still valid,
		// and the closure over g0 on the edited doc matches a from-scratch closure.
		Document docR = DocSetParamValue( doc, gid, "radius", 0, "2" );
		Check( BuildReferenceGraph( docR ).stamp == g0.stamp, "graph-neutral edit (radius): stamp STABLE -> cached graph reusable" );
		Check( SameClosure( DocEditClosure( gid, g0 ), DocEditClosure( docR, gid ) ), "reuse-on-stable-stamp: old graph yields the correct closure on the edited doc" );

		// graph-CHANGING edit: re-point o2's material m -> m2.  Stamp moves -> the holder
		// MUST rebuild; the rebuilt graph yields the new closure (m's dependents shrink).
		const NodeId o2 = DocFindByName( doc, "standard_object/o2" );
		Document docP = DocSetParamValue( doc, o2, "material", 0, "m2" );
		const ReferenceGraph g1 = BuildReferenceGraph( docP );
		Check( g1.stamp != g0.stamp, "graph-changing edit (reference re-point): stamp MOVES -> rebuild signalled" );
		Check( DocEditClosure( DocFindByName( docP, "lambertian_material/m" ), g1 ).size() == 2, "after re-point, closure(m) = {m, o} = 2 (o2 no longer depends on m)" );
		Check( DocEditClosure( DocFindByName( docP, "lambertian_material/m2" ), g1 ).size() == 2, "after re-point, closure(m2) = {m2, o2} = 2 (o2 now depends on m2)" );
	}

	// [stamp-nodeid] (review P1.5) erase + reinsert a byte-IDENTICAL chunk -> a NEW NodeId
	// with the SAME content.  The graph's edges/dependents are NodeId-KEYED, so a holder
	// reusing the old graph on the new doc would walk a DEAD NodeId.  The stamp must MOVE.
	// Red-proven by folding each chunk's NodeId into the stamp; without it (content-only),
	// this would be a false-stable.
	{
		Document d = ParseToCst( scene );
		const NodeId mId = DocFindByName( d, "lambertian_material/m" );
		const unsigned long long s0 = BuildReferenceGraph( d ).stamp;
		NodeRef mNode;
		const int idx = DocIndexOfNodeId( d, mId, &mNode );
		Check( idx >= 0 && mNode, "stamp-nodeid: located the chunk to erase+reinsert" );
		Document d1 = DocEraseItem( d, idx );
		Document d2 = DocInsertItem( d1, idx, mNode );   // identical content, NEW NodeId
		Check( DocFindByName( d2, "lambertian_material/m" ) != mId, "stamp-nodeid: the reinserted chunk has a NEW NodeId" );
		Check( BuildReferenceGraph( d2 ).stamp != s0, "stamp-nodeid: erase+reinsert-IDENTICAL MOVES the stamp (NodeId folded -- no false-stable, review P1.5)" );
	}

	// [maintained] (review P1.6) the holder keeps the graph in sync from the EDIT (O(log N)
	// decision), not by recomputing the stamp: a non-reference value edit REUSES the graph
	// (no rebuild), a reference / cp edit updates INCREMENTALLY (#4b), a name edit REBUILDS;
	// the closure stays correct either way.
	{
		Document d = ParseToCst( scene );
		MaintainedReferenceGraph mg( d );
		// non-reference value edit (sphere radius): graph REUSED, no rebuild.
		mg.SetParamValue( DocFindByName( mg.Doc(), "sphere_geometry/g" ), "radius", 0, "2" );
		Check( !mg.LastEditRebuilt(), "maintained: non-reference value edit (radius) REUSES the graph (no rebuild)" );
		{ const NodeId gid = DocFindByName( mg.Doc(), "sphere_geometry/g" );
		  Check( SameClosure( mg.EditClosure( gid ), DocEditClosure( mg.Doc(), gid ) ), "maintained: closure correct after a REUSED edit" ); }
		// reference re-point (object material m -> m2): graph updated INCREMENTALLY (#4b), no rebuild.
		mg.SetParamValue( DocFindByName( mg.Doc(), "standard_object/o" ), "material", 0, "m2" );
		Check( !mg.LastEditRebuilt(), "maintained: reference re-point (material) updates INCREMENTALLY (no rebuild, #4b)" );
		Check( SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "maintained: incremental reference re-point == from-scratch (stamp + dependents + edges)" );
		{ const NodeId mid2 = DocFindByName( mg.Doc(), "lambertian_material/m2" );
		  Check( SameClosure( mg.EditClosure( mid2 ), DocEditClosure( mg.Doc(), mid2 ) ), "maintained: closure correct after an INCREMENTAL edit" ); }
		// name edit (rename via value): graph REBUILT.
		mg.SetParamValue( DocFindByName( mg.Doc(), "lambertian_material/m2" ), "name", 0, "mRenamed" );
		Check( mg.LastEditRebuilt(), "maintained: a `name` edit REBUILDS the graph" );
	}

	// [maintained-cp] (review #2, 2nd pass) a piecewise_linear_function2d.cp edit carries a
	// TRACED Function1D ref even though `cp` is a String param -- the maintained graph must
	// update INCREMENTALLY on it (cp is a traced Function1D ref; #4b) and the closure must reflect it.
	{
		Document d = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"piecewise_linear_function\n{\nname f1\ncp 0 0\ncp 1 1\n}\n"
			"piecewise_linear_function\n{\nname f2\ncp 0 0\ncp 1 1\n}\n"
			"piecewise_linear_function2d\n{\nname p2\ncp 0.0 f1\n}\n" );
		MaintainedReferenceGraph mg( d );
		const NodeId p2 = DocFindByName( mg.Doc(), "piecewise_linear_function2d/p2" );
		mg.SetParamValue( p2, "cp", 0, "0.0 f2" );   // re-point the cp's Function1D f1 -> f2
		Check( !mg.LastEditRebuilt(), "maintained-cp: a plf2d cp edit updates INCREMENTALLY (cp's traced Function1D ref, #4b)" );
		Check( SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "maintained-cp: incremental cp edit == from-scratch" );
		const NodeId f2 = DocFindByName( mg.Doc(), "piecewise_linear_function/f2" );
		bool f2HasP2 = false; for( NodeId n : mg.EditClosure( f2 ) ) if( n == p2 ) f2HasP2 = true;
		Check( f2HasP2, "maintained-cp: after the edit, closure(f2) includes the plf2d (new cp ref traced)" );
	}

	// [stamp-reorder] (review P1) the COMMUTATIVE stamp (#4a) must still reflect ORDER-SENSITIVE
	// resolution: namespace lookup is FIRST-WINS on duplicate definitions, so swapping two same-
	// named producers flips the resolved edge -- and the stamp MUST move even though the per-chunk
	// content SUM is identical (SAME NodeIds, reordered).  Without folding the RESOLVED TARGET
	// NodeId into the consumer's per-chunk stamp, the sum is stable -> false "graph unchanged" -> a
	// stamp-gated holder reuses a stale dependency.  DocReparse carries the duplicate producers'
	// NodeIds BY CONTENT, so the reordered reparse is the EXACT same-NodeId reorder (two independent
	// parses would differ trivially by NodeId and not isolate the bug).
	{
		auto resolvedTarget = []( const ReferenceGraph& g, NodeId src ) -> NodeId {
			for( const ReferenceUse& e : g.edges ) if( e.sourceValueNodeId == src ) return e.targetNodeId;
			return 0;
		};
		const std::string s1 =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"sphere_geometry\n{\nname g\nradius 2\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		const std::string s2 =   // the two same-named producers SWAPPED (radius 2 first)
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 2\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";
		Document d1 = ParseToCst( s1 );
		Document d2 = DocReparse( d1, s2 );   // lineage carries g{r1}/g{r2} NodeIds BY CONTENT -> reordered, SAME NodeIds
		ReferenceGraph g1 = BuildReferenceGraph( d1 );
		ReferenceGraph g2 = BuildReferenceGraph( d2 );
		const NodeId og1 = DocParamId( d1, DocFindByName( d1, "standard_object/o" ), "geometry", 0 );
		const NodeId og2 = DocParamId( d2, DocFindByName( d2, "standard_object/o" ), "geometry", 0 );
		const NodeId t1 = resolvedTarget( g1, og1 );
		const NodeId t2 = resolvedTarget( g2, og2 );
		Check( t1 != 0 && t2 != 0 && t1 != t2, "stamp-reorder: swapping duplicate producers CHANGED the resolved object->geometry edge (first-wins)" );
		Check( g1.stamp != g2.stamp, "stamp-reorder: the stamp MOVED with the resolved edge (folds the resolved target NodeId -- review P1)" );
	}

	// [stamp-alias] (review P1, 2nd) the painter same-name ALIAS (PASS A mutual dependents) must be
	// ORDER-INSENSITIVE: for >=3 same-named MIXED-kind painters the OLD first-seen alias linked a
	// different SET depending on declaration order, while the commutative stamp stayed put -> same
	// stamp, DIFFERENT graph (closure).  The fix links ALL cross-kind same-name pairs (a function of
	// the chunk SET), so a reorder leaves the closure -- and the stamp -- unchanged.  DocReparse
	// carries the 3 painters' NodeIds BY CONTENT (distinct content), giving the exact same-NodeId
	// reorder (two independent parses would differ trivially by NodeId).
	{
		const std::string sA =
			"RISE ASCII SCENE 6\n"
			"scalar_painter\n{\nname q\nfile noise.dat\n}\n"
			"uniformcolor_painter\n{\nname q\ncolor 0.5 0.5 0.5\n}\n"
			"uniformcolor_painter\n{\nname q\ncolor 0.7 0.7 0.7\n}\n";
		const std::string sB =   // the scalar moved AFTER the first colour (reorder; SAME chunk set)
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname q\ncolor 0.5 0.5 0.5\n}\n"
			"scalar_painter\n{\nname q\nfile noise.dat\n}\n"
			"uniformcolor_painter\n{\nname q\ncolor 0.7 0.7 0.7\n}\n";
		Document dA = ParseToCst( sA );
		Document dB = DocReparse( dA, sB );   // carries scalar/colour0.5/colour0.7 NodeIds BY CONTENT -> reordered, SAME NodeIds
		const ReferenceGraph gA = BuildReferenceGraph( dA );
		const ReferenceGraph gB = BuildReferenceGraph( dB );
		const NodeId sqA = DocFindByName( dA, "scalar_painter/q" );
		const NodeId sqB = DocFindByName( dB, "scalar_painter/q" );
		if( sqA != 0 && sqB != 0 ) {
			const size_t clA = DocEditClosure( sqA, gA ).size();
			const size_t clB = DocEditClosure( sqB, gB ).size();
			Check( gA.stamp == gB.stamp, "stamp-alias: reorder of >=3 same-named painters -> stamp stable (same chunk SET)" );
			Check( clA == clB && clA == 3, "stamp-alias: closure(scalar q) = {scalar, 2 colours} = 3 in BOTH orders -> alias is ORDER-INSENSITIVE (same stamp => same graph)" );
		} else {
			std::printf( "  (skip stamp-alias: scalar_painter did not parse in this build)\n" );
		}
	}

	// [maintained-incremental] (#4b) a sequence of reference + cp edits each updates the held graph
	// INCREMENTALLY (LastEditRebuilt false) with the result EQUAL to a from-scratch BuildReferenceGraph
	// (stamp + dependents + edges); a NAME edit falls back to a full REBUILD.  The cost counter proves
	// the incremental edit does EXACTLY 1 ComputeChunkRefs (O(this chunk's refs)) vs N for a rebuild.
	{
		MaintainedReferenceGraph mg( ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p1\ncolor 0.1 0.1 0.1\n}\n"
			"uniformcolor_painter\n{\nname p2\ncolor 0.2 0.2 0.2\n}\n"
			"lambertian_material\n{\nname m\nreflectance p1\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n" ) );
		const NodeId mId = DocFindByName( mg.Doc(), "lambertian_material/m" );
		// (a) reference re-point p1 -> p2: incremental, exactly 1 ComputeChunkRefs, == from-scratch.
		const unsigned long b1 = DebugChunkRefsComputed();
		mg.SetParamValue( mId, "reflectance", 0, "p2" );
		Check( !mg.LastEditRebuilt(), "incremental: reflectance re-point did NOT rebuild" );
		Check( DebugChunkRefsComputed() - b1 == 1, "incremental: reflectance re-point did EXACTLY 1 ComputeChunkRefs (O(this chunk))" );
		Check( SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "incremental: reflectance re-point graph == from-scratch" );
		// (b) reference -> none (drop the edge): incremental, the dependent is removed.
		mg.SetParamValue( mId, "reflectance", 0, "none" );
		Check( !mg.LastEditRebuilt() && SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "incremental: reflectance -> none drops the edge, == from-scratch" );
		// (c) none -> reference (add the edge back): incremental, correct.
		mg.SetParamValue( mId, "reflectance", 0, "p1" );
		Check( !mg.LastEditRebuilt() && SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "incremental: reflectance none -> p1 re-adds the edge, == from-scratch" );
		// (d) a NAME edit rebuilds (the namespace changes) -> ComputeChunkRefs runs for every chunk.
		const unsigned long b2 = DebugChunkRefsComputed();
		mg.SetParamValue( DocFindByName( mg.Doc(), "uniformcolor_painter/p1" ), "name", 0, "p1b" );
		Check( mg.LastEditRebuilt(), "rebuild: a name edit rebuilds" );
		Check( DebugChunkRefsComputed() - b2 >= 3, "rebuild: a name edit re-runs ComputeChunkRefs for EVERY chunk (>=3, not 1)" );
		Check( SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "rebuild: the rebuilt graph == from-scratch" );
	}

	// [maintained-incremental-cost] (#4b cost gate) the incremental reference edit's ComputeChunkRefs
	// count is invariant to document SIZE -- 1 at a small doc AND at a 50x-larger doc -- the committed
	// proof the maintained reference edit is O(this chunk's refs), not O(N).
	{
		auto sceneOfN = []( int extra ) {
			std::string sc = "RISE ASCII SCENE 6\n"
				"uniformcolor_painter\n{\nname p1\ncolor 0.1 0.1 0.1\n}\n"
				"uniformcolor_painter\n{\nname p2\ncolor 0.2 0.2 0.2\n}\n"
				"lambertian_material\n{\nname m\nreflectance p1\n}\n";
			for( int i = 0; i < extra; ++i ) {                 // unrelated filler producers (grow N)
				char nm[32]; std::snprintf( nm, sizeof(nm), "f%d", i );
				sc += std::string("uniformcolor_painter\n{\nname ") + nm + "\ncolor 0.5 0.5 0.5\n}\n";
			}
			return sc;
		};
		unsigned long deltaSmall = 0, deltaLarge = 0;
		for( int pass = 0; pass < 2; ++pass ) {
			MaintainedReferenceGraph mg( ParseToCst( sceneOfN( pass == 0 ? 4 : 200 ) ) );
			const NodeId mId = DocFindByName( mg.Doc(), "lambertian_material/m" );
			const unsigned long b = DebugChunkRefsComputed();
			mg.SetParamValue( mId, "reflectance", 0, "p2" );
			( pass == 0 ? deltaSmall : deltaLarge ) = DebugChunkRefsComputed() - b;
		}
		Check( deltaSmall == 1 && deltaLarge == 1, "cost: an incremental reference edit does 1 ComputeChunkRefs at BOTH N+4 and N+200 (O(this chunk), invariant to N)" );
	}

	// [maintained-alias-rebuild] (#4b) a reference edit on an ALIAS-involved painter (a same-name
	// colour+scalar pair) falls back to a full REBUILD -- its dependents entry is shared with the
	// painter same-name alias and can't be incrementally distinguished -- and the rebuild is correct.
	{
		MaintainedReferenceGraph mg( ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname base\ncolor 0.3 0.3 0.3\n}\n"
			"uniformcolor_painter\n{\nname base2\ncolor 0.7 0.7 0.7\n}\n"
			"function2d_painter\n{\nname x\nfunction2d base\n}\n"
			"scalar_painter\n{\nname x\nfile noise.dat\n}\n" ) );
		const NodeId fx = DocFindByName( mg.Doc(), "function2d_painter/x" );   // colour painter x, has a function2d ref
		const NodeId sx = DocFindByName( mg.Doc(), "scalar_painter/x" );       // scalar painter x -> alias(x,x)
		if( fx != 0 && sx != 0 ) {
			mg.SetParamValue( fx, "function2d", 0, "base2" );   // reference edit on the ALIAS-involved painter x
			Check( mg.LastEditRebuilt(), "alias: a reference edit on an ALIAS-involved painter REBUILDS (conservative guard, #4b)" );
			Check( SameGraph( mg.Graph(), BuildReferenceGraph( mg.Doc() ) ), "alias: the conservative rebuild is correct (== from-scratch)" );
		} else {
			std::printf( "  (skip maintained-alias-rebuild: function2d_painter/scalar_painter did not parse)\n" );
		}
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
