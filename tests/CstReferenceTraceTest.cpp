//////////////////////////////////////////////////////////////////////
//
//  CstReferenceTraceTest.cpp - transfer-gate item 6: trace references through
//  the real resolver, on a three-level dependency chain.
//
//  Item 5 derives every chunk through the live registry. Item 6 builds the
//  REFERENCE GRAPH (D14/D25, §2.5): for every EXPLICIT reference it records a
//  `ReferenceUse { sourceValueNodeId, targetNodeId }`, resolving a reference
//  value to the chunk of that name in the param's reference CATEGORY (the
//  descriptor-derived namespace the named managers key on -- the SAME
//  category-name keying the engine uses, so it AGREES with the engine's
//  resolution for STATIC references). SCOPE: this is a descriptor-based resolver
//  (D14's "descriptor-provided reference resolver"); it does NOT capture DYNAMIC
//  references whose category is chosen at derive time by another param (e.g.
//  timeline.element via element_type, D14) -- those, and eliminating any
//  resolver drift, need the production D35 path (tracing recorded BY the actual
//  derivation resolver, no parallel pass), which is deferred.
//
//  This suite proves, on the canonical 3-level chain
//      object --geometry--> sphere
//      object --material--> material --reflectance--> painter
//    * [trace]    the exact edges are produced (incl. the object->material->
//                 painter chain), and a well-formed chain has no dangling refs;
//    * [resolve]  each edge's source resolves to the referring Param and its
//                 target to the referenced Chunk (durable NodeIds);
//    * [rename]   the referrers of a chunk are found from the graph (the D14
//                 rename driver) -- the only referrer of the painter is
//                 material.reflectance;
//    * [closure]  the transitive dependency closure is walkable (the D25
//                 incremental-re-derive foundation);
//    * [dangling] an unresolved reference is flagged, never a silent edge;
//    * [none]     an explicit `none` reference is not an edge.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

int main()
{
	std::printf( "CstReferenceTraceTest -- transfer-gate item 6 (reference tracing)\n" );

	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname red\ncolor 0.2 0.4 0.6\n}\n"
		"lambertian_material\n{\nname redmat\nreflectance red\n}\n"
		"sphere_geometry\n{\nname ball\nradius 0.25\n}\n"
		"standard_object\n{\nname obj\ngeometry ball\nmaterial redmat\n}\n";

	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	std::vector<ReferenceUse> uses = TraceReferences( d, &diags );

	const NodeId red    = DocFindByName( d, "uniformcolor_painter/red" );
	const NodeId redmat = DocFindByName( d, "lambertian_material/redmat" );
	const NodeId ball   = DocFindByName( d, "sphere_geometry/ball" );
	const NodeId obj    = DocFindByName( d, "standard_object/obj" );
	Check( red && redmat && ball && obj, "all 4 chunks addressable by name" );

	// The target of the edge whose source is (chunkId).role, or 0 if none.
	auto edgeTo = [&]( NodeId chunkId, const std::string& role ) -> NodeId {
		const NodeId src = DocParamId( d, chunkId, role );
		for( const auto& u : uses ) if( u.sourceValueNodeId == src ) return u.targetNodeId;
		return 0;
	};

	//----------------------------------------------------------------------
	std::printf( "[trace] 3-level chain object -> material -> painter (+ object -> geometry)\n" );
	Check( diags.empty(),     "no dangling references in a well-formed chain" );
	Check( uses.size() == 3,  "exactly 3 reference edges traced" );
	Check( edgeTo( obj, "geometry" )      == ball,   "object.geometry -> ball" );
	Check( edgeTo( obj, "material" )      == redmat, "object.material -> redmat" );
	Check( edgeTo( redmat, "reflectance" ) == red,   "material.reflectance -> red (the 3rd level)" );

	//----------------------------------------------------------------------
	std::printf( "[resolve] traced source/target NodeIds resolve to the right nodes\n" );
	NodeRef srcNode = DocResolveNodeId( d, DocParamId( d, obj, "material" ) );
	NodeRef tgtNode = DocResolveNodeId( d, redmat );
	Check( srcNode && srcNode->kind == NodeKind::Param && srcNode->role == "material",
	       "source resolves to the object's `material` Param" );
	Check( tgtNode && tgtNode->kind == NodeKind::Chunk && tgtNode->role == "lambertian_material",
	       "target resolves to the redmat Chunk" );

	//----------------------------------------------------------------------
	std::printf( "[rename] referrers of a chunk are found from the traced graph (D14)\n" );
	std::vector<NodeId> referrersOfRed;
	for( const auto& u : uses ) if( u.targetNodeId == red ) referrersOfRed.push_back( u.sourceValueNodeId );
	Check( referrersOfRed.size() == 1 && referrersOfRed[0] == DocParamId( d, redmat, "reflectance" ),
	       "the only referrer of 'red' is material.reflectance (a rename would rewrite exactly it)" );

	//----------------------------------------------------------------------
	std::printf( "[closure] transitive dependency closure is walkable (D25)\n" );
	// obj depends directly on ball + redmat; redmat depends on red; so following
	// the edges, obj's transitive closure is {ball, redmat, red}.
	const NodeId m = edgeTo( obj, "material" );        // redmat
	const NodeId g = edgeTo( obj, "geometry" );        // ball
	const NodeId p = edgeTo( m, "reflectance" );       // red, one hop deeper
	Check( m == redmat && g == ball && p == red,
	       "obj -> {ball, redmat -> red} reachable by following edges (3-level closure)" );

	//----------------------------------------------------------------------
	std::printf( "[dangling] an unresolved reference is flagged, not a silent edge\n" );
	{
		const std::string s =
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname ball\nradius 1\n}\n"
			"standard_object\n{\nname obj\ngeometry ball\nmaterial ghost\n}\n";
		Document d2 = ParseToCst( s );
		std::vector<std::string> diags2;
		std::vector<ReferenceUse> uses2 = TraceReferences( d2, &diags2 );
		Check( !diags2.empty(), "dangling material 'ghost' produces a diagnostic" );
		const NodeId obj2 = DocFindByName( d2, "standard_object/obj" );
		bool hasGeom = false, hasGhost = false;
		const NodeId gsrc = DocParamId( d2, obj2, "geometry" );
		const NodeId msrc = DocParamId( d2, obj2, "material" );
		for( const auto& u : uses2 ) { if( u.sourceValueNodeId == gsrc ) hasGeom = true; if( u.sourceValueNodeId == msrc ) hasGhost = true; }
		Check( hasGeom,  "the resolvable geometry reference is still traced" );
		Check( !hasGhost, "the dangling material reference is NOT a silent edge" );
	}

	//----------------------------------------------------------------------
	std::printf( "[none] an explicit 'none' reference is not an edge\n" );
	{
		const std::string s =
			"RISE ASCII SCENE 6\n"
			"sphere_geometry\n{\nname ball\nradius 1\n}\n"
			"standard_object\n{\nname obj\ngeometry ball\nmaterial none\n}\n";
		Document d3 = ParseToCst( s );
		std::vector<std::string> diags3;
		std::vector<ReferenceUse> uses3 = TraceReferences( d3, &diags3 );
		Check( diags3.empty(),    "explicit 'none' is not a dangling reference" );
		Check( uses3.size() == 1, "only the geometry edge; 'none' material is not an edge" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
