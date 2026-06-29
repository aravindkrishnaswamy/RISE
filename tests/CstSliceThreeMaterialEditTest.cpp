//////////////////////////////////////////////////////////////////////
//
//  CstSliceThreeMaterialEditTest.cpp - Model-B P5 Slice 3 (edit-model
//    pivot), staged to ONE edit-type.  A material painter-slot edit on a
//    CST-loaded scene routes through the canonical CST -- Job::ApplyCstParamEdit
//    (DocSetParamValue + DocEditClosure + DeriveToJobIncremental) -- instead of
//    the direct manager mutation, so the retained Document stays the source of
//    truth (Slice 4's save will serialize it).
//
//    Verifies, on a CST-loaded scene: the edit takes effect; it PERSISTS to the
//    Document (a fresh re-derive of the retained doc still shows it -- proving
//    the CST path, not just a Job mutation); undo reverts it (the inverse
//    re-point replays through the SAME CST path, so the existing mHistory undo
//    stack works unchanged).  And: a legacy-loaded scene (no retained Document)
//    still edits via the direct fallback (the CST path is gated off).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>

#include "CstRenderEquivalence.h"          // Job + manager accessors + risequiv::ParseLegacy
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n ) { if( c ) ++passCount; else { ++failCount; std::cout << "  FAIL: " << n << std::endl; } }

// The IPainter the material's slot currently binds (null if absent / non-painter).
static const IPainter* SlotPainter( Job& j, const char* mat, const char* slot )
{
	IMaterial* m = j.GetMaterials() ? j.GetMaterials()->GetItem( mat ) : 0;
	if( !m ) return 0;
	return MaterialIntrospection::GetSlot( *m, slot ).painter;
}

static const char* SCENE =
	"RISE ASCII SCENE 6\n"
	"uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
	"uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
	"lambertian_material\n{\nname m\nreflectance p1\n}\n"
	"sphere_geometry\n{\nname g\nradius 1\n}\n"
	"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

int main()
{
	using Cat = SceneEditController::Category;
	std::cout << "CstSliceThreeMaterialEditTest" << std::endl;
	const char* tmp = "cst_s3_tmp.RISEscene";
	{ std::ofstream o( tmp ); o << SCENE; }

	// ---- A: the edit takes effect AND persists to the retained CST Document ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "A: loads via CST (Document retained)" );
		const IPainter* p1 = j->GetPainters() ? j->GetPainters()->GetItem( "p1" ) : 0;
		const IPainter* p2 = j->GetPainters() ? j->GetPainters()->GetItem( "p2" ) : 0;
		Check( p1 && p2 && p1 != p2, "A: p1/p2 are distinct registered painters" );
		{
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Material, String( "m" ) );
			Check( SlotPainter( *j, "m", "reflectance" ) == p1, "A: before edit, m.reflectance == p1" );
			Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "A: material slot edit applies" );
			Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
			       "A: after edit, m.reflectance re-points to the LIVE p2" );
		}
		// PROOF the edit went through the CST (not just the Job): a fresh re-derive of the RETAINED Document.
		// If the edit had only mutated the Job, this would revert to p1; it must stay p2.
		Check( j->RederiveCstWithVariant( "none" ), "A: re-derive of the retained Document succeeds" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
		       "A: the edit PERSISTED to the CST Document (re-derive still shows p2)" );
		j->release();
	}

	// ---- B: undo reverts (the inverse re-point replays through the SAME CST path) ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "B: loads via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "B: edit applies" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ), "B: edit -> p2" );
		c.Undo();
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p1" ),
		       "B: undo reverts m.reflectance to p1" );
		j->release();
	}

	// ---- C: legacy-loaded scene (no retained Document) edits via the DIRECT fallback ----
	{
		Job* j = new Job();
		Check( risequiv::ParseLegacy( SCENE, *j ), "C: parses via the legacy path" );
		Check( !j->HasRetainedCstDocument(), "C: legacy load retains NO Document (CST path gated off)" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "C: edit applies (direct fallback)" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
		       "C: direct fallback still re-points m.reflectance to p2" );
		j->release();
	}

	// ---- D: SEQUENTIAL incremental edits on one CST-loaded Job (isolates whether a 2nd incremental works) ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "D: loads via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "D: edit #1 (p1->p2) applies" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ), "D: after edit #1, m.reflectance == p2" );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p1" ) ), "D: edit #2 (p2->p1) applies" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p1" ), "D: after edit #2, m.reflectance == p1" );
		j->release();
	}

	std::remove( tmp );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
