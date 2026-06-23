//////////////////////////////////////////////////////////////////////
//
//  CstResolverTest.cpp - slice 1 of docs/agentic-redesign/21-stable-apply-and-
//  resolver.md: the shared resolver (BuildReferenceGraph). Locks in the four
//  properties that make it the AUTHORITATIVE graph rename + closure consume:
//    [consistency] its edges agree with what the derivation actually binds
//                  (no drift between the resolver and the derive -- P1.8).
//    [stamp]       a content fingerprint that CHANGES iff the graph could change
//                  (a reference re-point / rename) and is STABLE otherwise (a
//                  non-reference value edit) -- so a cached graph's staleness is
//                  detectable in O(1) (P1.8 unstamped).
//    [namespace]   the runtime defaults (the `none` material/painter) are in the
//                  derivation namespace (P1.8 coarser-namespace) -- and stay in
//                  sync with the engine (drift -> this test fails).
//    [ref-or-literal] a reference-kind value that is a pure NUMBER is a literal
//                  (e.g. ggx `roughness 0.5`), not a dangling reference -- not an
//                  edge, not a false diagnostic (P1.8 ref-or-literal).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, IObject/manager interfaces

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

static bool HasEdge( const ReferenceGraph& g, NodeId src, NodeId tgt )
{
	for( const ReferenceUse& e : g.edges ) if( e.sourceValueNodeId == src && e.targetNodeId == tgt ) return true;
	return false;
}

static bool DiagsMention( const std::vector<std::string>& d, const char* needle )
{
	for( const std::string& s : d ) if( s.find( needle ) != std::string::npos ) return true;
	return false;
}

int main()
{
	std::printf( "CstResolverTest -- slice 1 (shared resolver: consistency / stamp / namespace / ref-or-literal)\n" );

	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"lambertian_material\n{\nname m2\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

	//----------------------------------------------------------------------
	// [consistency] the graph's edges agree with the derivation's bindings.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( scene );
		const NodeId pid = DocFindByName( doc, "uniformcolor_painter/p" );
		const NodeId mid = DocFindByName( doc, "lambertian_material/m" );
		const NodeId gid = DocFindByName( doc, "sphere_geometry/g" );
		const NodeId oid = DocFindByName( doc, "standard_object/o" );
		const NodeId oMat  = DocParamId( doc, oid, "material", 0 );
		const NodeId oGeom = DocParamId( doc, oid, "geometry", 0 );
		const NodeId mRefl = DocParamId( doc, mid, "reflectance", 0 );

		std::vector<std::string> diags;
		ReferenceGraph gph = BuildReferenceGraph( doc, &diags );
		Check( HasEdge( gph, oMat,  mid ), "graph: object.material -> material m" );
		Check( HasEdge( gph, oGeom, gid ), "graph: object.geometry -> geometry g" );
		Check( HasEdge( gph, mRefl, pid ), "graph: material.reflectance -> painter p" );
		Check( diags.empty(), "graph: no dangling-reference diagnostics on a clean scene" );

		// derive, and confirm the ENGINE bound the object exactly where the graph says.
		Job* j = new Job(); std::vector<std::string> dd; DeriveToJob( doc, *j, &dd );
		IObject* o = j->GetObjects() ? j->GetObjects()->GetItem( "o" ) : 0;
		const IMaterial* mPtr = j->GetMaterials()  ? j->GetMaterials()->GetItem( "m" )  : 0;
		const IGeometry* gPtr = j->GetGeometries() ? j->GetGeometries()->GetItem( "g" ) : 0;
		Check( o && mPtr && o->GetMaterial() == mPtr, "derive: object o bound to material m (matches graph edge)" );
		Check( o && gPtr && o->GetGeometry() == gPtr, "derive: object o bound to geometry g (matches graph edge)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [stamp] changes iff the graph could change.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( scene );
		const unsigned long long s0 = BuildReferenceGraph( doc ).stamp;

		// a NON-reference value edit (sphere radius) cannot change the graph -> stable.
		Document docR = DocSetParamValue( doc, DocFindByName( doc, "sphere_geometry/g" ), "radius", 0, "2" );
		Check( BuildReferenceGraph( docR ).stamp == s0, "stamp STABLE across a non-reference value edit (radius)" );

		// a reference RE-POINT (object.material m -> m2) changes the graph -> stamp moves.
		Document docP = DocSetParamValue( doc, DocFindByName( doc, "standard_object/o" ), "material", 0, "m2" );
		Check( BuildReferenceGraph( docP ).stamp != s0, "stamp CHANGES on a reference re-point (object.material m->m2)" );

		// a RENAME (material m -> mx, referrers rewritten) changes the graph -> stamp moves.
		Document docN = DocRename( doc, DocFindByName( doc, "lambertian_material/m" ), "mx" );
		Check( BuildReferenceGraph( docN ).stamp != s0, "stamp CHANGES on a rename (material m->mx)" );

		// re-stamping the SAME document is deterministic.
		Check( BuildReferenceGraph( doc ).stamp == s0, "stamp deterministic (same doc -> same stamp)" );
	}

	//----------------------------------------------------------------------
	// [namespace] the runtime defaults are in the engine's namespace + in sync.
	//----------------------------------------------------------------------
	{
		Document doc = ParseToCst( "RISE ASCII SCENE 6\n" );   // empty scene: only the engine defaults
		Job* j = new Job(); std::vector<std::string> dd; DeriveToJob( doc, *j, &dd );
		Check( j->GetMaterials() && j->GetMaterials()->GetItem( "none" ) != 0, "runtime default present: material 'none' (resolver namespace in sync)" );
		Check( j->GetPainters()  && j->GetPainters()->GetItem( "none" )  != 0, "runtime default present: painter 'none' (resolver namespace in sync)" );
		j->release();
	}

	//----------------------------------------------------------------------
	// [ref-or-literal] a numeric value in a reference-kind slot is a LITERAL.
	//----------------------------------------------------------------------
	{
		// pbr_metallic_roughness_material.roughness is ValueKind::Reference but "can be
		// a painter ref OR a scalar string"; `roughness 0.5` is a literal, not a
		// dangling reference.
		Document doc = ParseToCst( "RISE ASCII SCENE 6\npbr_metallic_roughness_material\n{\nname gx\nroughness 0.5\n}\n" );
		std::vector<std::string> diags;
		ReferenceGraph gph = BuildReferenceGraph( doc, &diags );
		Check( !DiagsMention( diags, "roughness" ) && !DiagsMention( diags, "0.5" ), "ref-or-literal: numeric `roughness 0.5` is NOT a false dangling reference" );
		const NodeId gxRough = DocParamId( doc, DocFindByName( doc, "pbr_metallic_roughness_material/gx" ), "roughness", 0 );
		bool edgeFromRough = false;
		for( const ReferenceUse& e : gph.edges ) if( e.sourceValueNodeId == gxRough ) edgeFromRough = true;
		Check( !edgeFromRough, "ref-or-literal: numeric `roughness 0.5` produces NO edge" );

		// control: a NON-numeric unresolved reference IS still a dangling diagnostic.
		Document doc2 = ParseToCst( "RISE ASCII SCENE 6\npbr_metallic_roughness_material\n{\nname gx\nroughness nosuchpainter\n}\n" );
		std::vector<std::string> diags2;
		BuildReferenceGraph( doc2, &diags2 );
		Check( DiagsMention( diags2, "nosuchpainter" ), "control: a non-numeric unresolved reference IS diagnosed dangling" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
