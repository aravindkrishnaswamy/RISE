//////////////////////////////////////////////////////////////////////
//
//  CstEditCostTest.cpp - transfer-gate item 8: measure a NON-SPATIAL edit AND a
//  SPATIAL edit; report the TLAS cost SEPARATELY.
//
//  The gate's payoff: "the redesign's real CST path is O(closure) for non-spatial
//  edits, with spatial cost reported honestly." This suite measures the cost
//  MODEL's determinants on the in-tree CST path:
//    * [edit-cost]   the structured edit (DocSetParamValue) is O(log N): its
//                    path-copy depth is sub-linear, invariant to scene size N.
//    * [closure]     the re-derive CLOSURE (what an incremental re-derive must
//                    re-apply -- the changed chunk + its transitive dependents,
//                    DocEditClosure over the reference graph, D25) has SIZE
//                    O(closure): INVARIANT to N for a fixed dependent count, and
//                    proportional to the DEPENDENTS (a shared chunk -> O(deps)),
//                    never O(N). This is the cost-model determinant -- the
//                    re-derive work scales with the dependents, not the scene.
//    * [spatial]     a NON-SPATIAL edit (a material/painter VALUE) changes no
//                    object's world bounding box -> the top-level acceleration
//                    structure (TLAS) stays clean, so its cost is O(log N) edit +
//                    O(closure) re-derive, NO TLAS rebuild. A SPATIAL edit (a
//                    geometry's shape or an object's transform) dirties the TLAS,
//                    adding the engine's O(N log N) BVH rebuild as a SEPARATE
//                    component (reported, not charged to the CST re-derive).
//
//  HONEST SCOPE: DocEditClosure COMPUTES the closure via an O(N) graph walk
//  (it rebuilds the param->chunk map + traces references each call); a production
//  system maintains the reference graph incrementally (O(closure) to find the
//  closure). The incremental-apply ENGINE that re-applies ONLY the closure (drop
//  + re-Finalize, skipping the rest) is prototype-validated (slices 1.5/3) and the
//  in-tree pieces (RemoveItem + Finalize + DocEditClosure) support it; this suite
//  measures the cost-model DETERMINANTS (edit O(log N); closure SIZE O(closure);
//  TLAS O(N log N) separate), not the full incremental engine. The TLAS O(N log N)
//  is the engine's documented top-level BVH4 build (SAH-binned; see the TLAS entry
//  in CLAUDE.md / docs/ARCHITECTURE.md), referenced here as the separate spatial
//  component rather than re-measured (it builds at render-prep, off the CST path).
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

// N independent painter->material->object(+geometry) groups, or (shared) one
// painter+material that all N objects reference.
static std::string SceneN( int n, bool shared )
{
	std::string s = "RISE ASCII SCENE 6\n";
	if( shared ) {
		s += "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n";
		s += "lambertian_material\n{\nname m\nreflectance p\n}\n";
		for( int i = 0; i < n; ++i ) {
			const std::string si = std::to_string( i );
			s += "sphere_geometry\n{\nname g" + si + "\nradius 1\n}\n";
			s += "standard_object\n{\nname o" + si + "\ngeometry g" + si + "\nmaterial m\n}\n";
		}
	} else {
		for( int i = 0; i < n; ++i ) {
			const std::string si = std::to_string( i );
			s += "uniformcolor_painter\n{\nname p" + si + "\ncolor 0.5 0.5 0.5\n}\n";
			s += "lambertian_material\n{\nname m" + si + "\nreflectance p" + si + "\n}\n";
			s += "sphere_geometry\n{\nname g" + si + "\nradius 1\n}\n";
			s += "standard_object\n{\nname o" + si + "\ngeometry g" + si + "\nmaterial m" + si + "\n}\n";
		}
	}
	return s;
}

// Does the chunk's closure include a chunk of geometry category (keyword ends
// "_geometry")? (Used to show a non-spatial edit's closure carries no geometry.)
static bool ClosureHasGeometry( const Document& d, const std::vector<NodeId>& closure )
{
	for( NodeId id : closure ) {
		NodeRef n = DocResolveNodeId( d, id );
		if( n && n->kind == NodeKind::Chunk && n->role.size() >= 9
		    && n->role.compare( n->role.size() - 9, 9, "_geometry" ) == 0 ) return true;
	}
	return false;
}

int main()
{
	std::printf( "CstEditCostTest -- transfer-gate item 8 (non-spatial vs spatial edit cost)\n" );

	//----------------------------------------------------------------------
	std::printf( "[edit-cost] DocSetParamValue is O(log N): path-copy depth sub-linear, invariant to N\n" );
	{
		int v8 = 0, v64 = 0, v512 = 0;
		{ Document d = ParseToCst( SceneN( 8,   false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v8 ); }
		{ Document d = ParseToCst( SceneN( 64,  false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v64 ); }
		{ Document d = ParseToCst( SceneN( 512, false ) ); DocSetParamValue( d, DocFindByName( d, "sphere_geometry/g0" ), "radius", 0, "2", &v512 ); }
		std::printf( "      edit visits: N=8 -> %d, N=64 -> %d, N=512 -> %d  (item count ~ 4N)\n", v8, v64, v512 );
		Check( v8 > 0 && v512 > 0, "edit visits counted" );
		Check( v512 < 8 * 512, "edit visits << N (not a linear scan)" );
		// 64x more items (8 -> 512) must NOT multiply the edit cost ~64x: log-like.
		Check( v512 <= v8 + 24, "edit visits grow ~log N across a 64x scene-size increase" );
	}

	//----------------------------------------------------------------------
	std::printf( "[closure] DocEditClosure SIZE is O(closure): invariant to N, proportional to dependents\n" );
	{
		// Independent groups: editing material[0] only re-derives {material0, object0}.
		size_t c8 = 0, c64 = 0, c512 = 0;
		{ Document d = ParseToCst( SceneN( 8,   false ) ); c8   = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		{ Document d = ParseToCst( SceneN( 64,  false ) ); c64  = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		{ Document d = ParseToCst( SceneN( 512, false ) ); c512 = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) ).size(); }
		std::printf( "      closure(material0) size: N=8 -> %zu, N=64 -> %zu, N=512 -> %zu\n", c8, c64, c512 );
		Check( c8 == 2 && c64 == 2 && c512 == 2, "closure(material0) == {material0, object0} = 2, INVARIANT to N" );
		// Transitive: editing painter[0] re-derives {painter0, material0, object0}.
		{ Document d = ParseToCst( SceneN( 64, false ) );
		  Check( DocEditClosure( d, DocFindByName( d, "uniformcolor_painter/p0" ) ).size() == 3, "closure(painter0) == {painter0, material0, object0} = 3 (transitive)" ); }
		// Shared chunk: a material used by ALL N objects -> closure is O(deps) = N+1.
		for( int n : { 8, 64, 512 } ) {
			Document d = ParseToCst( SceneN( n, true ) );
			const size_t cs = DocEditClosure( d, DocFindByName( d, "lambertian_material/m" ) ).size();
			char msg[96]; std::snprintf( msg, sizeof(msg), "shared closure(material) == %d (the material + its %d object dependents)", n + 1, n );
			Check( cs == (size_t)( n + 1 ), msg );
		}
	}

	//----------------------------------------------------------------------
	std::printf( "[spatial] non-spatial edit leaves the TLAS clean; spatial edit dirties it (O(N log N) separate)\n" );
	{
		Document d = ParseToCst( SceneN( 32, false ) );
		// NON-SPATIAL: edit a material's reflectance value. Closure = {material, object};
		// no geometry shape / object transform changed -> object bboxes unchanged ->
		// TLAS stays clean -> NO O(N log N) rebuild.
		std::vector<NodeId> nonSpatial = DocEditClosure( d, DocFindByName( d, "lambertian_material/m0" ) );
		Check( !ClosureHasGeometry( d, nonSpatial ), "non-spatial (material-value) closure carries NO geometry -> object bboxes unchanged -> TLAS CLEAN" );
		Check( nonSpatial.size() == 2, "non-spatial closure is O(closure) = 2 (no scene-wide work)" );

		// SPATIAL: edit a geometry's radius (shape) -- the object using it changes
		// its world bbox -> the TLAS is dirty -> the engine's O(N log N) top-level
		// BVH rebuild is the SEPARATE spatial cost (reported, not part of the CST
		// re-derive). The closure carries the geometry + its dependent object.
		std::vector<NodeId> spatial = DocEditClosure( d, DocFindByName( d, "sphere_geometry/g0" ) );
		Check( ClosureHasGeometry( d, spatial ), "spatial (geometry-shape) closure carries the geometry -> a bbox changed -> TLAS DIRTY" );
		Check( spatial.size() == 2, "spatial CST closure is still O(closure) = 2 (the geometry + its object); the TLAS rebuild is a SEPARATE O(N log N) cost" );

		std::printf( "      cost model: NON-spatial edit = O(log N) edit + O(closure) re-derive, NO TLAS;\n" );
		std::printf( "                  SPATIAL edit     = O(log N) edit + O(closure) re-derive + O(N log N) TLAS (reported separately)\n" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
