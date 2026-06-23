//////////////////////////////////////////////////////////////////////
//
//  CstStableApplyTest.cpp - slice 3 of docs/agentic-redesign/21-stable-apply-and-
//  resolver.md: STABLE-OBJECT in-place incremental apply.  Objects are re-pointed
//  in place (their address -- which the top-level BVH stores raw -- survives the
//  edit), only non-object entities are recreated, and the TLAS is invalidated only
//  for a genuinely SPATIAL edit.  Locks in (Root A; P1.1/P1.2):
//
//    [re-point]    a value edit keeps every object's ADDRESS stable (re-pointed, not
//                  dropped/recreated) -> the TLAS pointers stay valid (no P1.1 UAF).
//    [non-spatial] a material/painter value edit leaves every object's bbox identical
//                  -> the TLAS is NOT invalidated (spatial generation unchanged): the
//                  "non-spatial edit skips the TLAS" result (P1.2 dissolved).
//    [spatial]     a geometry-extent or object-transform edit changes an object's bbox
//                  -> the TLAS IS invalidated (generation advances).
//    [equivalence] the re-pointed Job is byte-identical (DumpJob) to a fresh full
//                  derive of the edited document (the re-point is correct, not just
//                  address-stable).
//    [refusals]    an optional-slot REMOVAL (e.g. material -> none, which a re-point
//                  cannot clear) and a non-standard_object (csg_object) both fall back
//                  to a full derive (D51: never a silent partial / stale binding).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"
#include "CstRenderEquivalence.h"      // Job, DumpJob, managers

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }
static bool bbeq( const BoundingBox& a, const BoundingBox& b )
{
	return a.ll.x == b.ll.x && a.ll.y == b.ll.y && a.ll.z == b.ll.z
	    && a.ur.x == b.ur.x && a.ur.y == b.ur.y && a.ur.z == b.ur.z;
}

static const char* BASE =
	"RISE ASCII SCENE 6\n"
	"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
	"uniformcolor_painter\n{\nname p2\ncolor 0.2 0.2 0.2\n}\n"
	"lambertian_material\n{\nname m\nreflectance p\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n";

int main()
{
	std::printf( "CstStableApplyTest -- slice 3 (stable-object in-place apply)\n" );

	// ---- [non-spatial] material reflectance p -> p2: re-point, bbox stable, TLAS kept.
	{
		Document d0 = ParseToCst( BASE );
		Job* j = new Job(); std::vector<std::string> d; DeriveToJob( d0, *j, &d );
		IObjectManager* om = j->GetObjects();
		IObjectPriv* oBefore = om->GetItem( "o" );
		const BoundingBox bbBefore = oBefore->getBoundingBox();
		const unsigned long long genBefore = om->GetSpatialStructureGeneration();
		const IMaterial* matBefore = oBefore->GetMaterial();

		const NodeId mId = DocFindByName( d0, "lambertian_material/m" );
		Document d1 = DocSetParamValue( d0, mId, "reflectance", 0, "p2" );
		std::vector<NodeId> closure = DocEditClosure( d1, mId );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d1, *j, closure, &di );

		IObjectPriv* oAfter = om->GetItem( "o" );
		Check( applied == (int)closure.size() && applied > 0, "non-spatial: whole closure applied" );
		Check( oAfter == oBefore, "non-spatial: object RE-POINTED in place (address stable, no UAF)" );
		Check( bbeq( bbBefore, oAfter->getBoundingBox() ), "non-spatial: object bbox unchanged" );
		Check( om->GetSpatialStructureGeneration() == genBefore, "non-spatial: TLAS NOT invalidated (spatial generation unchanged)" );
		const IMaterial* matNow = j->GetMaterials() ? j->GetMaterials()->GetItem( "m" ) : 0;
		Check( matNow != 0 && oAfter->GetMaterial() == matNow, "non-spatial: object re-pointed to the LIVE material m (not dangling at the freed one -- P1.1)" );
		Check( matNow != matBefore, "non-spatial: the material WAS recreated (new address); the stable object re-points to it" );

		Job* jf = new Job(); std::vector<std::string> df; DeriveToJob( d1, *jf, &df );
		Check( DumpJob( *j ) == DumpJob( *jf ), "non-spatial: re-pointed Job == fresh full derive of the edited doc" );
		jf->release(); j->release();
	}

	// ---- [spatial: geometry] radius 1 -> 2: re-point, bbox CHANGES, TLAS invalidated.
	{
		Document d0 = ParseToCst( BASE );
		Job* j = new Job(); std::vector<std::string> d; DeriveToJob( d0, *j, &d );
		IObjectManager* om = j->GetObjects();
		IObjectPriv* oBefore = om->GetItem( "o" );
		const BoundingBox bbBefore = oBefore->getBoundingBox();
		const unsigned long long genBefore = om->GetSpatialStructureGeneration();

		const NodeId gId = DocFindByName( d0, "sphere_geometry/g" );
		Document d1 = DocSetParamValue( d0, gId, "radius", 0, "2" );
		std::vector<NodeId> closure = DocEditClosure( d1, gId );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d1, *j, closure, &di );

		IObjectPriv* oAfter = om->GetItem( "o" );
		Check( applied == (int)closure.size() && applied > 0, "spatial-geom: whole closure applied" );
		Check( oAfter == oBefore, "spatial-geom: object RE-POINTED in place (address stable)" );
		Check( !bbeq( bbBefore, oAfter->getBoundingBox() ), "spatial-geom: object bbox CHANGED (radius 1->2)" );
		Check( om->GetSpatialStructureGeneration() == genBefore + 1, "spatial-geom: TLAS invalidated exactly once" );

		Job* jf = new Job(); std::vector<std::string> df; DeriveToJob( d1, *jf, &df );
		Check( DumpJob( *j ) == DumpJob( *jf ), "spatial-geom: re-pointed Job == fresh full derive" );
		jf->release(); j->release();
	}

	// ---- [spatial: transform] object position 0 0 0 -> 5 0 0: re-point, bbox CHANGES.
	{
		Document d0 = ParseToCst( BASE );
		Job* j = new Job(); std::vector<std::string> d; DeriveToJob( d0, *j, &d );
		IObjectManager* om = j->GetObjects();
		IObjectPriv* oBefore = om->GetItem( "o" );
		const BoundingBox bbBefore = oBefore->getBoundingBox();
		const unsigned long long genBefore = om->GetSpatialStructureGeneration();

		const NodeId oId = DocFindByName( d0, "standard_object/o" );
		Document d1 = DocSetParamValue( d0, oId, "position", 0, "5 0 0" );
		std::vector<NodeId> closure = DocEditClosure( d1, oId );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d1, *j, closure, &di );

		IObjectPriv* oAfter = om->GetItem( "o" );
		Check( applied == (int)closure.size() && applied > 0, "spatial-xform: closure applied" );
		Check( oAfter == oBefore, "spatial-xform: object RE-POINTED in place (address stable)" );
		Check( !bbeq( bbBefore, oAfter->getBoundingBox() ), "spatial-xform: object bbox CHANGED (moved +5 X)" );
		Check( om->GetSpatialStructureGeneration() == genBefore + 1, "spatial-xform: TLAS invalidated exactly once" );

		Job* jf = new Job(); std::vector<std::string> df; DeriveToJob( d1, *jf, &df );
		Check( DumpJob( *j ) == DumpJob( *jf ), "spatial-xform: re-pointed Job == fresh full derive" );
		jf->release(); j->release();
	}

	// ---- [removal refused] material m -> none on object o: a re-point cannot clear the
	//      material slot, so the incremental REFUSES (-> full derive). Nothing mutated.
	{
		Document d0 = ParseToCst( BASE );
		Job* j = new Job(); std::vector<std::string> d; DeriveToJob( d0, *j, &d );
		const std::string before = DumpJob( *j );
		const NodeId oId = DocFindByName( d0, "standard_object/o" );
		Document d1 = DocSetParamValue( d0, oId, "material", 0, "none" );
		std::vector<NodeId> closure = DocEditClosure( d1, oId );
		std::vector<std::string> di;
		const int applied = DeriveToJobIncremental( d1, *j, closure, &di );
		Check( applied == 0 && !di.empty(), "removal: material -> none REFUSED (applied 0 + diagnosed)" );
		Check( DumpJob( *j ) == before, "removal: refusal mutated NOTHING" );
		j->release();
	}

	// ---- [csg refused] a csg_object in the closure is not re-pointed in place (its
	//      operands are themselves objects) -> REFUSED, fall back to a full derive.
	{
		const std::string csg =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
			"lambertian_material\n{\nname m\nreflectance p\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"standard_object\n{\nname o1\ngeometry g\nmaterial m\nposition -1 0 0\n}\n"
			"standard_object\n{\nname o2\ngeometry g\nmaterial m\nposition 1 0 0\n}\n"
			"csg_object\n{\nname c\nobja o1\nobjb o2\noperation union\nmaterial m\n}\n";
		Document d0 = ParseToCst( csg );
		Job* j = new Job(); std::vector<std::string> d; DeriveToJob( d0, *j, &d );
		const std::string before = DumpJob( *j );
		const NodeId cId = DocFindByName( d0, "csg_object/c" );
		if( cId != 0 ) {
			std::vector<NodeId> closure; closure.push_back( cId );
			std::vector<std::string> di;
			const int applied = DeriveToJobIncremental( d0, *j, closure, &di );
			Check( applied == 0 && !di.empty(), "csg: csg_object closure REFUSED (applied 0 + diagnosed)" );
			Check( DumpJob( *j ) == before, "csg: refusal mutated NOTHING" );
		} else {
			std::printf( "  (skip csg: chunk did not parse in this build)\n" );
		}
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
