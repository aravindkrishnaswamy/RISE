//////////////////////////////////////////////////////////////////////
//
//  SceneVariantTest.cpp - P0 of the scene_variant feature (doc 63).
//
//  scene_variant is a named, selectable overlay on the base scene; a
//  `variant <name>`-tagged material chunk OVERRIDES its base same-named
//  counterpart when that variant is active.  The override is BAKED DURING the
//  CST derive (DeriveToJob): a pre-scan finds the active variant + its
//  overridden material names, then PASS-2 skips the overridden-base + the
//  inactive-override chunks and Finalizes the ACTIVE override -- so objects bind
//  the active material BY NAME from the start (no post-derive re-pointing).
//
//  These tests drive the CST path (ParseToCst + DeriveToJob) and verify, via the
//  derived Job's material managers (the same emitter-exitance accessor DumpJob
//  uses), that the active variant's override took effect, the base is the default
//  when no variant is active, and an unnamed scene_variant is refused.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <string>
#include <vector>

#include "CstRenderEquivalence.h"          // Job + the material/emitter accessors + includes
#include "../src/Library/Cst/Cst.h"        // ParseToCst, DeriveToJob

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Cst;

namespace
{
	int s_pass = 0, s_fail = 0;
	void Check( bool ok, const std::string& what ) { if( ok ) ++s_pass; else { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); } }

	// A scene with a base luminaire `lum` (scale 5) + a `night`-variant override of `lum` (scale 0), an object
	// bound to `lum`, and the `night` scene_variant.  `active` selects the active variant ("" => no
	// active_scene_variant chunk => base default).
	std::string SceneWithActive( const char* active )
	{
		std::string s =
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
			"sphere_geometry\n{\nname s\nradius 1\n}\n"
			"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n"
			"scene_variant\n{\nname night\n}\n";
		if( active && active[0] ) {
			s += "active_scene_variant\n{\nname ";
			s += active;
			s += "\n}\n";
		}
		return s;
	}

	// Derive `text` via the CST path; returns the diagnostic count and fills *outJob (caller releases).
	int DeriveText( const std::string& text, Job*& outJob )
	{
		outJob = new Job();
		Document doc = ParseToCst( text );
		std::vector<std::string> diags;
		DeriveToJob( doc, *outJob, &diags );
		return (int)diags.size();
	}

	// The red(=r) channel of a luminaire material's average radiant exitance (the deterministic accessor DumpJob
	// uses); -1 if the material/emitter is missing.
	double LumExitanceR( Job* j, const char* matname )
	{
		IMaterial* m = ( j && j->GetMaterials() ) ? j->GetMaterials()->GetItem( matname ) : 0;
		if( !m || !m->GetEmitter() ) return -1.0;
		return (double)m->GetEmitter()->averageRadiantExitance().r;
	}
}

int main()
{
	std::printf( "=== SceneVariantTest (P0: bake-at-derive material overrides) ===\n" );

	// 1. The active variant's material override is BAKED: `lum` is the night override (scale 0 -> exitance ~0),
	//    and the object bound to `lum` resolves to it (registered under the same name; base skipped).
	{
		Job* j; const int d = DeriveText( SceneWithActive( "night" ), j );
		Check( d == 0, "night-active scene derives clean (no diagnostics)" );
		const double e = LumExitanceR( j, "lum" );
		Check( e >= 0.0 && e < 0.01, "night active: `lum` is the override (scale 0 -> exitance ~0)" );
		j->release();
	}

	// 2. Base default: no active_scene_variant chunk -> the BASE `lum` (scale 5) is used.
	{
		Job* j; DeriveText( SceneWithActive( "" ), j );
		Check( LumExitanceR( j, "lum" ) > 1.0, "no active variant: `lum` is the base (scale 5 -> exitance > 1)" );
		j->release();
	}

	// 3. `none` selects the base default explicitly.
	{
		Job* j; DeriveText( SceneWithActive( "none" ), j );
		Check( LumExitanceR( j, "lum" ) > 1.0, "active `none`: base default (exitance > 1)" );
		j->release();
	}

	// 4. An active variant that overrides NOTHING (the `night` override is inactive) -> base.
	{
		Job* j; DeriveText( SceneWithActive( "day" ), j );
		Check( LumExitanceR( j, "lum" ) > 1.0, "active `day` (no `day` overrides): the `night` override is inactive -> base" );
		j->release();
	}

	// 5. A scene_variant with no name is refused (DeclareSceneVariant returns false -> apply diagnostic).
	{
		Job* j = new Job();
		Document doc = ParseToCst( "RISE ASCII SCENE 6\nscene_variant\n{\n}\n" );
		std::vector<std::string> diags;
		DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "scene_variant without a name is refused (diagnostic)" );
		j->release();
	}

	// 6. Legacy path renders the BASE: scene_variant is CST-native, so the legacy reader skips the variant-tagged
	//    material (no dup-name collision) and does not bake active_scene_variant.
	{
		Job* j = new Job();
		const bool ok = risequiv::ParseLegacy( SceneWithActive( "night" ), *j );
		Check( ok, "legacy parses a scene_variant scene (variant material skipped -> no dup-name collision)" );
		Check( LumExitanceR( j, "lum" ) > 1.0, "legacy renders the base: `lum` scale 5 (variant override skipped, not baked)" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
