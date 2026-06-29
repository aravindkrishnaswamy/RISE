//////////////////////////////////////////////////////////////////////
//
//  SceneVariantTest.cpp - P0 of the scene_variant feature (doc 63).
//
//  scene_variant is a named, selectable overlay on the base scene; a
//  `variant <name>`-tagged material chunk OVERRIDES its base same-named
//  counterpart when that variant is active.  The override is BAKED DURING the
//  CST derive (DeriveToJob): a pre-scan finds the active variant + its
//  overridden material names, then PASS-2 applies the ACTIVE
//  override at its overridden BASE's slot (dropping the base, the override's own
//  slot, and every inactive override) -- so objects bind the active material BY
//  NAME from the start, regardless of where the override sits in the file (it may
//  follow the objects, as the watch night block does), with no post-derive re-pointing.
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
#include <fstream>
#include <cstdio>

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

	// 4. An active_scene_variant naming an UNDECLARED variant (a selector typo) is refused, not silently base.
	{
		Job* j; const int d = DeriveText( SceneWithActive( "day" ), j );   // only `night` is declared
		Check( d > 0, "active an undeclared variant (`day`) is refused (selector typo guard)" );
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

	// 7. ClearSceneVariants: re-deriving on the same Job clears prior variant state (no cross-derive leak).
	{
		Job* j = new Job();
		{ Document a = ParseToCst( SceneWithActive( "night" ) ); std::vector<std::string> d; DeriveToJob( a, *j, &d ); }
		Check( j->HasSceneVariants(), "after deriving a variant scene, the Job reports variants" );
		{ Document b = ParseToCst( "RISE ASCII SCENE 6\nsphere_geometry\n{\nname s2\nradius 1\n}\n" ); std::vector<std::string> d; DeriveToJob( b, *j, &d ); }
		Check( !j->HasSceneVariants(), "re-deriving a non-variant scene on the same Job clears prior variant state (ClearSceneVariants)" );
		j->release();
	}

	// 8. A dangling variant override (a `variant` material whose name has no base of that name -- a typo) is
	//    refused with a diagnostic, not silently registered as a phantom (doc 63 §3.2).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lumX\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
			"scene_variant\n{\nname night\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags;
		DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "dangling variant override (no base of that name) is refused (diagnostic)" );
		j->release();
	}

	// 9. A variant active_camera referencing a non-existent camera is refused (the Reference is not
	//    existence-checked in PASS-1, so the apply-time SetActiveCamera failure must diagnose).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"scene_variant\n{\nname night\nactive_camera ghostcam\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags;
		DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "variant active_camera referencing a non-existent camera is refused (diagnostic)" );
		j->release();
	}

	// 10. A DECLARED variant with no overrides is active -> base (the active-undeclared guard does not over-refuse).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"scene_variant\n{\nname quiet\n}\n"
			"active_scene_variant\n{\nname quiet\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( diags.empty(), "a declared no-override variant is active -> derives clean (not over-refused)" );
		Check( LumExitanceR( j, "lum" ) > 1.0, "declared no-override variant -> base material" );
		j->release();
	}

	// 11. A duplicate untagged base material name (masked by an active override skip) is refused (review P2-b).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 6.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
			"scene_variant\n{\nname night\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "duplicate base material name (masked by the override skip) is refused" );
		j->release();
	}

	// 12. active_camera `none` is the no-override sentinel, not a missing-camera error (review P2-a).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
			"scene_variant\n{\nname night\nactive_camera none\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( diags.empty(), "active_camera `none` derives clean (no missing-camera diagnostic)" );
		Check( LumExitanceR( j, "lum" ) < 0.01, "active_camera `none`: the material override still bakes" );
		j->release();
	}

	// 13. A variant override placed AFTER the object that binds it (the watch_dial night pattern) still bakes: it is
	//     applied at its base's slot, so the object resolves the active material by name regardless of the override's
	//     file position.  Under the prior "Finalize the override at its own slot" bake this case dangled (unresolved ref).
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"   // base
			"sphere_geometry\n{\nname s\nradius 1\n}\n"
			"standard_object\n{\nname obj\ngeometry s\nmaterial lum\n}\n"                                 // binds lum -- BEFORE the override
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"  // override AFTER the object
			"scene_variant\n{\nname night\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( diags.empty(), "override AFTER the binding object derives clean (applied at the base's slot, not the override's)" );
		const double e = LumExitanceR( j, "lum" );
		Check( e >= 0.0 && e < 0.01, "override-after-object: the night override (scale 0) baked, not the base (scale 5)" );
		j->release();
	}

	// 14. Two active overrides of one material name are ambiguous -> refused.  (The bake applies a single override per
	//     name at the base slot, so it can no longer rely on the AddItem dup-name hard-error -- only one override would
	//     reach it -- to catch this; an explicit pre-scan guard does.)
	{
		Job* j = new Job();
		Document doc = ParseToCst(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nexitance white\nscale 5.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 0.0\nmaterial none\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant night\nexitance white\nscale 1.0\nmaterial none\n}\n"
			"scene_variant\n{\nname night\n}\n"
			"active_scene_variant\n{\nname night\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "two overrides of the same name (active variant) is refused (ambiguous)" );
		j->release();
	}

	// 15. `variant none` is the no-variant sentinel -> the material is an ORDINARY BASE (registered, not dropped
	//     as a variant tag); CST and legacy agree.  (Mirrors `material none` / `active_scene_variant none`.)
	{
		Job* j; DeriveText(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant none\nexitance white\nscale 5.0\nmaterial none\n}\n", j );
		Check( LumExitanceR( j, "lum" ) > 1.0, "`variant none` material is registered as a base (CST path)" );
		j->release();
		Job* jl = new Job();
		risequiv::ParseLegacy(
			"RISE ASCII SCENE 6\n"
			"uniformcolor_painter\n{\nname white\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname lum\nvariant none\nexitance white\nscale 5.0\nmaterial none\n}\n", *jl );
		Check( LumExitanceR( jl, "lum" ) > 1.0, "`variant none` material is registered as a base (legacy path)" );
		jl->release();
	}

	// 16. DeriveToJob's activeVariantOverride FORCES a variant, winning over the active_scene_variant chunk (the
	//     GUI switch's core).  nullptr => use the chunk; "none"/"" => base; a name => that variant; + it reconciles
	//     the Job's active-variant record.
	{
		const std::string nightChunk = SceneWithActive( "night" );   // has active_scene_variant{night}
		{ Job* j = new Job(); Document d = ParseToCst( nightChunk ); std::vector<std::string> g;
		  DeriveToJob( d, *j, &g, nullptr, "none" );
		  Check( LumExitanceR( j, "lum" ) > 1.0, "override `none` forces base, overriding the active_scene_variant{night} chunk" );
		  j->release(); }
		{ Job* j = new Job(); Document d = ParseToCst( nightChunk ); std::vector<std::string> g;
		  DeriveToJob( d, *j, &g, nullptr, nullptr );
		  Check( LumExitanceR( j, "lum" ) < 0.01, "override nullptr defers to the chunk (night active)" );
		  j->release(); }
		{ Job* j = new Job(); Document d = ParseToCst( SceneWithActive( "" ) ); std::vector<std::string> g;
		  DeriveToJob( d, *j, &g, nullptr, "night" );
		  Check( LumExitanceR( j, "lum" ) < 0.01, "override `night` forces the night variant on a base scene" );
		  char buf[64] = {0}; j->GetActiveSceneVariant( buf, sizeof(buf) );
		  Check( std::string(buf) == "night", "override reconciles the Job's active-variant record" );
		  j->release(); }
	}

	// 17. RederiveCstWithVariant: load a variant scene via the CST path, then re-derive with a FORCED variant (the
	//     GUI variant switch) -- ClearAll + re-bake the retained Document, no dup-name, the active record updates.
	{
		const char* tmp = "sv_rederive_tmp.RISEscene";
		{ std::ofstream o( tmp ); o << SceneWithActive( "" ); }   // base scene; night variant declared, not active
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( tmp ), "RederiveCstWithVariant: scene loads via the CST path" );
		Check( LumExitanceR( j, "lum" ) > 1.0, "RederiveCstWithVariant: initial load is base (lum scale 5)" );
		Check( j->RederiveCstWithVariant( "night" ), "RederiveCstWithVariant(night) re-derives clean" );
		Check( LumExitanceR( j, "lum" ) < 0.01, "RederiveCstWithVariant(night): the night override (scale 0) is now active" );
		char buf[64] = {0}; j->GetActiveSceneVariant( buf, sizeof(buf) );
		Check( std::string(buf) == "night", "RederiveCstWithVariant(night): the Job's active-variant record reflects it" );
		Check( j->RederiveCstWithVariant( "none" ), "RederiveCstWithVariant(none) re-derives clean" );
		Check( LumExitanceR( j, "lum" ) > 1.0, "RederiveCstWithVariant(none): back to base (lum scale 5)" );
		j->release();
		std::remove( tmp );
	}

	// 18. Reserved variant names: `none` (the no-variant sentinel) and `(base)` (the GUI base-entry label) are
	//     REFUSED at declaration -- a variant so named would be unreachable (the switch routes both to the base).
	{
		Job* j = new Job();
		Document doc = ParseToCst( "RISE ASCII SCENE 6\nscene_variant\n{\nname none\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "scene_variant named `none` (reserved sentinel) is refused" );
		j->release();
	}
	{
		Job* j = new Job();
		Document doc = ParseToCst( "RISE ASCII SCENE 6\nscene_variant\n{\nname (base)\n}\n" );
		std::vector<std::string> diags; DeriveToJob( doc, *j, &diags );
		Check( !diags.empty(), "scene_variant named `(base)` (reserved GUI label) is refused" );
		j->release();
	}

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
