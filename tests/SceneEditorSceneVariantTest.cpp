//////////////////////////////////////////////////////////////////////
//
//  SceneEditorSceneVariantTest.cpp - the interactive editor's "Variants"
//    accordion Category (SceneEditController::Category::SceneVariant): it
//    lists the scene's scene_variants plus a synthetic "(base)" entry,
//    reports which is active, and -- unlike the Animation category's
//    render-time SetActiveAnimation -- SWITCHES by RE-DERIVING the retained
//    CST Document with that variant forced active (re-baking the materials).
//
//    Verifies the GUI-only path the CLI can't reach: list / active / the
//    re-derive switch round-trip (base -> night -> base), checking the baked
//    material each step.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <fstream>
#include <cstdio>

#include "CstRenderEquivalence.h"          // Job + the material/emitter accessors
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n ) { if( c ) ++passCount; else { ++failCount; std::cout << "  FAIL: " << n << std::endl; } }

// red(=r) channel of a luminaire material's average radiant exitance; -1 if missing / non-emissive.
static double LumR( Job& j, const char* m )
{
	IMaterial* x = j.GetMaterials() ? j.GetMaterials()->GetItem( m ) : 0;
	if( !x || !x->GetEmitter() ) return -1.0;
	return (double)x->GetEmitter()->averageRadiantExitance().r;
}

int main()
{
	using Cat = SceneEditController::Category;
	std::cout << "SceneEditorSceneVariantTest" << std::endl;

	// Base scene: `lum` (scale 5) + a `night` override of `lum` (scale 0), an object bound to `lum`, and the
	// `night` scene_variant declared (no active_scene_variant => base is the default).
	const char* scene =
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
		"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n"
		"scene_variant\n{\nname night\n}\n";
	const char* tmp = "sv_editor_tmp.RISEscene";
	{ std::ofstream o( tmp ); o << scene; }

	Job* pJob = new Job();
	Check( pJob->LoadAsciiSceneViaCst( tmp ), "scene loads via the CST path (Document retained for re-derive)" );
	{
		SceneEditController c( *pJob, /*interactiveRasterizer*/ 0 );

		// List: a synthetic "(base)" at index 0, then the declared variants.
		Check( c.CategoryEntityCount( Cat::SceneVariant ) == 2, "Variants list = (base) + night" );
		Check( c.CategoryEntityName( Cat::SceneVariant, 0 ) == String( "(base)" ), "index 0 is the synthetic (base)" );
		Check( c.CategoryEntityName( Cat::SceneVariant, 1 ) == String( "night" ),  "index 1 is the night variant" );

		// Active starts at base (the scene has no active_scene_variant).
		Check( c.CategoryActiveName( Cat::SceneVariant ) == String( "(base)" ), "active starts at (base)" );
		Check( LumR( *pJob, "lum" ) > 1.0, "base material active (lum scale 5)" );

		// Switch to night: re-derives the CST with night forced active -> the override is baked + active updates.
		Check( c.SetSelection( Cat::SceneVariant, String( "night" ) ), "SetSelection(night) re-derives clean" );
		Check( LumR( *pJob, "lum" ) >= 0.0 && LumR( *pJob, "lum" ) < 0.01, "after switch: night override active (lum scale 0)" );
		Check( c.CategoryActiveName( Cat::SceneVariant ) == String( "night" ), "active is now night" );

		// Switch back to base via the synthetic "(base)" entry.
		Check( c.SetSelection( Cat::SceneVariant, String( "(base)" ) ), "SetSelection((base)) re-derives clean" );
		Check( LumR( *pJob, "lum" ) > 1.0, "after switch back: base material active (lum scale 5)" );
		Check( c.CategoryActiveName( Cat::SceneVariant ) == String( "(base)" ), "active is back to (base)" );

		// UAF regression (reviewer-found P1): a variant switch ClearAll's + rebuilds the Scene + managers, so an
		// edit routed through mEditor must NOT dereference the freed old scene -- RebindEditorToJob re-points
		// mEditor after the re-derive.  Pre-fix this segfaulted; reaching the final Check means no use-after-free.
		Check( c.SetSelection( Cat::SceneVariant, String( "night" ) ), "switch to night (for the post-switch edit)" );
		c.SetSelection( Cat::Material, String( "lum" ) );
		c.RefreshProperties();   // builds mPropertiesByCategory[Material] (via mJob, safe post-switch) so the edit has a real property
		const unsigned int np = c.PropertyCountFor( Cat::Material );
		Check( np > 0, "the material exposes editable properties (so the edit actually exercises mEditor)" );
		if( np > 0 ) {
			const String pn = c.PropertyNameFor( Cat::Material, 0 );
			const String pv = c.PropertyValueFor( Cat::Material, 0 );
			c.SetPropertyForCategory( Cat::Material, pn, pv );   // self-valued edit through mEditor -> the (rebound) new scene
		}
		Check( true, "edit through mEditor AFTER a variant switch did not crash (mEditor rebound to the new scene)" );
	}
	pJob->release();
	std::remove( tmp );

	// Legacy-load gate (reviewer-found P2): the legacy reader DECLARES the variants (record-only), but with no
	// retained CST Document the switch cannot re-derive -- so the controller reports 0 variant entries (the GUI
	// shows no pickable rows that would silently no-op), even though GetSceneVariantCount() > 0.
	{
		Job* pLegacy = new Job();
		Check( risequiv::ParseLegacy( scene, *pLegacy ), "scene parses via the legacy path" );
		Check( pLegacy->GetSceneVariantCount() > 0, "legacy DOES declare the variants (record-only)" );
		SceneEditController lc( *pLegacy, 0 );
		Check( lc.CategoryEntityCount( Cat::SceneVariant ) == 0, "legacy-loaded (no CST Document) -> 0 variant entries (switch gated off)" );
		pLegacy->release();
	}

	std::cout << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
