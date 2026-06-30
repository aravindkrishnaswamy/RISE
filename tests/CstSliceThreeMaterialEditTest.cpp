//////////////////////////////////////////////////////////////////////
//
//  CstSliceThreeMaterialEditTest.cpp - Model-B P5 Slice 3 (edit-model
//    pivot), staged to ONE edit-type.  A material painter-slot edit on a
//    CST-loaded scene routes through the canonical CST -- Job::ApplyCstParamEdit
//    (DocSetOrAddParamValue + DocEditClosure + DeriveToJobIncremental, or a full
//    re-derive fallback for variant/animated/instance_array scenes) -- instead of
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

	// ---- E: editing a DEFAULTED slot (omitted from the scene text) INSERTS it + takes effect (P1 #1) ----
	//      Before the set-or-add fix this silently no-op'd while reporting success.
	{
		const char* te = "cst_s3_defaulted.RISEscene";
		{ std::ofstream o( te );
		  o << "RISE ASCII SCENE 6\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname md\n}\n"            // reflectance OMITTED -> defaulted
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial md\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( te ), "E: loads via CST (defaulted reflectance)" );
		const IPainter* p2 = j->GetPainters() ? j->GetPainters()->GetItem( "p2" ) : 0;
		Check( p2 && SlotPainter( *j, "md", "reflectance" ) != p2, "E: before edit, md.reflectance is the default (not p2)" );
		{
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Material, String( "md" ) );
			Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ),
			       "E: editing the defaulted slot applies (no longer a silent no-op)" );
			Check( SlotPainter( *j, "md", "reflectance" ) == p2, "E: after edit, the defaulted md.reflectance now binds p2" );
		}
		Check( j->RederiveCstWithVariant( "none" ), "E: re-derive of the retained Document succeeds" );
		Check( SlotPainter( *j, "md", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
		       "E: the INSERTED reflectance PERSISTED to the CST Document (re-derive still shows p2)" );
		j->release();
		std::remove( te );
	}

	// ---- F: a cross-category bare-name collision is disambiguated by the "material" kind hint (P2 #4) ----
	{
		const char* tf = "cst_s3_collision.RISEscene";
		{ std::ofstream o( tf );
		  o << "RISE ASCII SCENE 6\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname shared\nreflectance p1\n}\n"   // material named "shared"
		       "sphere_geometry\n{\nname shared\nradius 1\n}\n"             // geometry ALSO named "shared"
		       "standard_object\n{\nname o\ngeometry shared\nmaterial shared\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tf ), "F: loads via CST (material + geometry share the name 'shared')" );
		{
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Material, String( "shared" ) );
			Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ),
			       "F: the material edit resolves despite the cross-category name clash" );
			Check( SlotPainter( *j, "shared", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
			       "F: the right entity (the material, not the geometry) was edited -> p2" );
		}
		j->release();
		std::remove( tf );
	}

	// ---- G: a scene that DECLARES a scene_variant forces the D2 full-re-derive fallback (P1 #2) ----
	//      DeriveToJobIncremental refuses on HasSceneVariants(); ApplyCstParamEdit re-derives the whole
	//      document (result 2) and the SceneEditor self-rebinds -- a stale-pointer read here would crash.
	{
		const char* tg = "cst_s3_variant.RISEscene";
		{ std::ofstream o( tg );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"               // declares a variant -> HasSceneVariants()
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tg ), "G: loads via CST (declares scene_variant night)" );
		Check( j->GetSceneVariantCount() > 0, "G: the variant is declared (incremental will refuse -> D2)" );
		{
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Material, String( "m" ) );
			Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ),
			       "G: material edit on a variant scene applies via the D2 full-re-derive fallback" );
			Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
			       "G: after the D2 edit, m.reflectance binds the LIVE p2 (self-rebind worked -- no UAF)" );
		}
		Check( j->GetSceneVariantCount() > 0, "G: the scene_variant survived the edit (still in the CST)" );
		Check( j->RederiveCstWithVariant( "none" ), "G: re-derive succeeds" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
		       "G: the D2 edit PERSISTED to the CST Document" );
		j->release();
		std::remove( tg );
	}

	// ---- H: redo replays the forward edit through the same CST path ----
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "H: loads via CST" );
		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p2" ) ), "H: edit -> p2" );
		c.Undo();
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p1" ), "H: undo -> p1" );
		c.Redo();
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ), "H: redo -> p2" );
		j->release();
	}

	// ---- I: the Job::ApplyCstParamEdit return-code contract (0 reject / 1 incremental / 2 D2 full re-derive; 3 = replaced-but-diagnosed, not separately reachable) ----
	{
		// non-variant scene -> incremental fast path (1)
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "I: loads non-variant via CST" );
		Check( j->ApplyCstParamEdit( "m", "material", "reflectance", 0, "p2" ) == 1,
		       "I: non-variant material edit returns 1 (incremental)" );
		Check( j->ApplyCstParamEdit( "nope", "material", "reflectance", 0, "p2" ) == 0,
		       "I: unknown entity returns 0 (rejected, no change)" );
		j->release();
	}
	{
		// variant scene -> D2 full re-derive (2)
		const char* ti = "cst_s3_rc.RISEscene";
		{ std::ofstream o( ti );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( ti ), "I: loads variant via CST" );
		Check( j->ApplyCstParamEdit( "m", "material", "reflectance", 0, "p2" ) == 2,
		       "I: variant material edit returns 2 (D2 full re-derive)" );
		Check( SlotPainter( *j, "m", "reflectance" ) == j->GetPainters()->GetItem( "p2" ),
		       "I: after the D2 edit the live scene shows p2" );
		j->release();
		std::remove( ti );
	}

	// ---- K: a D2 material edit on a variant scene PRESERVES the user's active camera (a full re-derive
	//      would otherwise reset it to the document default -- a surprising side-effect on the hero scene) ----
	{
		const char* tk = "cst_s3_camkeep.RISEscene";
		{ std::ofstream o( tk );
		  o << "RISE ASCII SCENE 6\n"
		       "scene_variant\n{\nname night\n}\n"
		       "film\n{\nwidth 64\nheight 64\n}\n"
		       "pinhole_camera\n{\nname cam1\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		       "pinhole_camera\n{\nname cam2\nlocation 3 0 0\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		       "uniformcolor_painter\n{\nname p1\ncolor 1 0 0\n}\n"
		       "uniformcolor_painter\n{\nname p2\ncolor 0 1 0\n}\n"
		       "lambertian_material\n{\nname m\nreflectance p1\n}\n"
		       "sphere_geometry\n{\nname g\nradius 1\n}\n"
		       "standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n"; }
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tk ), "K: loads variant + 2-camera scene via CST" );
		Check( j->SetActiveCamera( "cam1" ), "K: switch the active camera to cam1 (cam2 is the load default)" );
		Check( j->GetActiveCameraName() == std::string( "cam1" ), "K: active camera is cam1 before the edit" );
		Check( j->ApplyCstParamEdit( "m", "material", "reflectance", 0, "p2" ) == 2, "K: the material edit takes the D2 path" );
		Check( j->GetActiveCameraName() == std::string( "cam1" ),
		       "K: the D2 material edit PRESERVED the active camera (not reset to the cam2 doc default)" );
		j->release();
		std::remove( tk );
	}

	std::remove( tmp );
	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
