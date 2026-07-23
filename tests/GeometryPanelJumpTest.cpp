//////////////////////////////////////////////////////////////////////
//
//  GeometryPanelJumpTest.cpp - GUI redesign (2026-07-22) regression
//    coverage for the three new property-surface capabilities:
//
//      T1  Category::Geometry enumeration -- geometry chunks appear in
//          the outliner surface (CategoryEntityCount/Name), mirroring
//          the Medium pattern over IJob::EnumerateGeometryNames.
//      T2  Geometry property rows -- the generic descriptor+CST
//          introspection (CstIntrospection) yields TYPED rows for a
//          geometry chunk (sphere radius = ValueKind::Double).
//      T3  Geometry edit round-trip -- SetProperty on a selected
//          geometry routes through the generic agent CST edit path:
//          the CST text changes, and Undo restores it.
//      T4  Jump-to-definition -- PropertyJumpTargetFor resolves an
//          Object's `geometry` reference row to (Geometry, name) and a
//          Material's `reflectance` row to (Painter, name); an INLINE
//          NUMERIC value on a Reference param (dielectric `tau 0.9`)
//          correctly does NOT resolve (no dangling jump).
//      T5  Material augment -- AugmentWithCstRows appends descriptor
//          params the live MaterialIntrospection never surfaced
//          (dielectric ar_film_ior), fully editable.
//      T6  Material value-param SAVE (external-review P1) -- editing a
//          non-slot material value param (dielectric `ior`) SUCCEEDS and
//          round-trips through the retained CST.  Before the Material-arm
//          slot-vs-value classification it was silently rejected by
//          SetMaterialProperty despite the row showing editable.
//      T7  Dangling-reference guard (external-review P1) -- rebinding a
//          material slot to a RUNTIME-only painter (registered, no CST
//          chunk) is REJECTED and leaves the CST binding untouched, while a
//          CST-backed rebind still succeeds (guard is not over-broad).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/CstIntrospection.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"

using namespace RISE;
using namespace RISE::Implementation;
using Category = SceneEditController::Category;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	// Same minimal fixture shape as EntityTemplatesTest, plus a dielectric
	// material (T4's inline-numeric Reference + T5's augment case).
	const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 24\nheight 24\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_albedo\ncolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_albedo2\ncolor 0.2 0.7 0.3\n}\n\n"
		"lambertian_material\n{\nname mat_diffuse\nreflectance pnt_albedo\n}\n\n"
		"dielectric_material\n{\nname mat_glass\ntau 0.9\nior 1.5\n}\n\n"
		"sphere_geometry\n{\nname sph\nradius 0.8\n}\n\n"
		"box_geometry\n{\nname bx\nwidth 1.0\nheight 1.0\ndepth 1.0\n}\n\n"
		"standard_object\n{\nname obj_sph\ngeometry sph\nmaterial mat_diffuse\n}\n\n"
		"omni_light\n{\nname lgt_a\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		return dir + name;
	}

	Job* LoadScene( const char* text, const std::string& path )
	{
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) )
		{
			pJob->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		return pJob;
	}

	bool NameInCategory( SceneEditController& ctrl, Category cat, const std::string& name )
	{
		const unsigned int n = ctrl.CategoryEntityCount( cat );
		for( unsigned int i = 0; i < n; ++i )
		{
			if( std::string( ctrl.CategoryEntityName( cat, i ).c_str() ) == name ) return true;
		}
		return false;
	}

	// Row lookup over the per-category snapshot.
	int RowIndexFor( SceneEditController& ctrl, Category cat, const std::string& rowName )
	{
		const unsigned int n = ctrl.PropertyCountFor( cat );
		for( unsigned int i = 0; i < n; ++i )
		{
			if( std::string( ctrl.PropertyNameFor( cat, i ).c_str() ) == rowName )
				return static_cast<int>( i );
		}
		return -1;
	}

	std::string CstValueOf( Job* pJob, const char* entity, const char* suffix, const char* param )
	{
		const std::vector<CameraProperty> rows = CstIntrospection::Inspect(
			pJob->GetCstDocument(), *pJob, String( entity ), suffix, "type" );
		for( const CameraProperty& r : rows )
		{
			if( std::string( r.name.c_str() ) == param ) return std::string( r.value.c_str() );
		}
		return std::string( "<missing>" );
	}
}

int main()
{
	std::printf( "=== GeometryPanelJumpTest ===\n" );
	const std::string tmp = TempPath( "geometry_panel_jump.RISEscene" );
	Job* pJob = LoadScene( kScene, tmp );
	Check( pJob != nullptr, "fixture scene loads via CST" );
	if( !pJob ) { std::printf( "=== GeometryPanelJumpTest: %d passed, %d failed ===\n", g_pass, g_fail + 1 ); return 1; }

	{
		SceneEditController ctrl( *pJob, nullptr );   // skeleton: no interactive rasterizer needed

		// ---------- T1: geometry enumeration ----------
		std::printf( "T1: Category::Geometry enumeration...\n" );
		Check( ctrl.CategoryEntityCount( Category::Geometry ) == 2,
			"CategoryEntityCount(Geometry) == 2 (sphere + box)" );
		Check( NameInCategory( ctrl, Category::Geometry, "sph" ), "geometry list contains `sph`" );
		Check( NameInCategory( ctrl, Category::Geometry, "bx" ),  "geometry list contains `bx`" );

		// ---------- T2: typed geometry rows ----------
		std::printf( "T2: typed geometry property rows...\n" );
		Check( ctrl.SetSelection( Category::Geometry, String( "sph" ) ),
			"SetSelection(Geometry, sph) succeeds" );
		ctrl.RefreshProperties();
		const int radiusIdx = RowIndexFor( ctrl, Category::Geometry, "radius" );
		Check( radiusIdx >= 0, "sphere panel surfaces a `radius` row" );
		if( radiusIdx >= 0 )
		{
			Check( ctrl.PropertyKindFor( Category::Geometry, radiusIdx ) == static_cast<int>( ValueKind::Double ),
				"MONEY (T2): `radius` row is TYPED ValueKind::Double (descriptor-driven, not a bare string)" );
			Check( ctrl.PropertyEditableFor( Category::Geometry, radiusIdx ),
				"`radius` row is editable" );
			Check( std::string( ctrl.PropertyValueFor( Category::Geometry, radiusIdx ).c_str() ) == "0.8",
				"`radius` row carries the scene's CST value 0.8" );
		}
		const int typeIdx = RowIndexFor( ctrl, Category::Geometry, "type" );
		Check( typeIdx == 0 && !ctrl.PropertyEditableFor( Category::Geometry, 0 ),
			"leading read-only `type` identity row present" );
		// The shells' single-panel path reads the PRIMARY snapshot --
		// PanelMode has no Geometry value, so RefreshProperties must route
		// the per-category rows as primary (the pre-existing Painter gap,
		// fixed in the same change).
		Check( ctrl.PropertyCount() > 0,
			"MONEY (T2): the PRIMARY property snapshot carries the geometry rows (the shells' panel path)" );

		// ---------- T3: edit round-trip + undo ----------
		std::printf( "T3: geometry edit round-trip...\n" );
		Check( ctrl.SetProperty( String( "radius" ), String( "2.5" ) ),
			"SetProperty(radius, 2.5) on the selected geometry succeeds" );
		Check( CstValueOf( pJob, "sph", "geometry", "radius" ) == "2.5",
			"MONEY (T3): the CST text now carries radius 2.5 (edit routed through the generic CST path)" );
		ctrl.Undo();   // void return -- verified by the CST value below
		Check( CstValueOf( pJob, "sph", "geometry", "radius" ) == "0.8",
			"MONEY (T3): Undo restores radius 0.8 in the CST" );

		// ---------- T4: jump-to-definition ----------
		std::printf( "T4: jump-to-definition resolution...\n" );
		Check( ctrl.SetSelection( Category::Object, String( "obj_sph" ) ),
			"SetSelection(Object, obj_sph) succeeds" );
		ctrl.RefreshProperties();
		{
			const int gIdx = RowIndexFor( ctrl, Category::Object, "geometry" );
			Check( gIdx >= 0, "object panel surfaces a `geometry` reference row" );
			if( gIdx >= 0 )
			{
				Category outCat = Category::None;
				String outName;
				Check( ctrl.PropertyJumpTargetFor( Category::Object, gIdx, outCat, outName ),
					"PropertyJumpTargetFor(Object.geometry) resolves" );
				Check( outCat == Category::Geometry && std::string( outName.c_str() ) == "sph",
					"MONEY (T4): object's `geometry` reference jumps to (Geometry, sph)" );
			}
		}
		Check( ctrl.SetSelection( Category::Material, String( "mat_diffuse" ) ),
			"SetSelection(Material, mat_diffuse) succeeds" );
		ctrl.RefreshProperties();
		{
			const int rIdx = RowIndexFor( ctrl, Category::Material, "reflectance" );
			Check( rIdx >= 0, "material panel surfaces a `reflectance` reference row" );
			if( rIdx >= 0 )
			{
				Category outCat = Category::None;
				String outName;
				Check( ctrl.PropertyJumpTargetFor( Category::Material, rIdx, outCat, outName ),
					"PropertyJumpTargetFor(Material.reflectance) resolves" );
				Check( outCat == Category::Painter && std::string( outName.c_str() ) == "pnt_albedo",
					"MONEY (T4): material's `reflectance` reference jumps to (Painter, pnt_albedo)" );
			}
		}
		// Negative case: an INLINE NUMERIC on a Reference param must not jump.
		Check( ctrl.SetSelection( Category::Material, String( "mat_glass" ) ),
			"SetSelection(Material, mat_glass) succeeds" );
		ctrl.RefreshProperties();
		{
			const int tIdx = RowIndexFor( ctrl, Category::Material, "tau" );
			Check( tIdx >= 0, "dielectric panel surfaces a `tau` row" );
			if( tIdx >= 0 )
			{
				Category outCat = Category::None;
				String outName;
				Check( !ctrl.PropertyJumpTargetFor( Category::Material, tIdx, outCat, outName ),
					"MONEY (T4): inline numeric `tau 0.9` does NOT resolve to a jump target (no dangling jump)" );
			}
		}

		// ---------- T5: material augment ----------
		std::printf( "T5: material panel augment (descriptor+CST rows)...\n" );
		{
			const int arIdx = RowIndexFor( ctrl, Category::Material, "ar_film_ior" );
			Check( arIdx >= 0,
				"MONEY (T5): dielectric panel gained the descriptor param `ar_film_ior` the live introspection never surfaced" );
			if( arIdx >= 0 )
			{
				Check( ctrl.PropertyEditableFor( Category::Material, arIdx ),
					"`ar_film_ior` is editable (routes through the CST edit path)" );
				Check( ctrl.PropertyKindFor( Category::Material, arIdx ) == static_cast<int>( ValueKind::Double ),
					"`ar_film_ior` is TYPED ValueKind::Double" );
			}
		}

		// ---------- T6: material VALUE-param edit actually SAVES ----------
		// External-review P1: an editable flag is worthless if the edit is
		// rejected.  SetMaterialProperty only accepts painter/scalar SLOTS,
		// so a non-slot descriptor param (ar_film_ior) was rejected despite
		// showing editable.  The Material arm now classifies slot-vs-value and
		// routes value params through the generic CST edit path.  Prove the
		// edit (a) returns success and (b) round-trips through the retained CST
		// -- both FALSE before the fix.  Mutation-verified: reverting the
		// Material-arm classification fails exactly these two checks.
		std::printf( "T6: material value-param edit round-trips through the CST...\n" );
		{
			Check( ctrl.SetSelection( Category::Material, String( "mat_glass" ) ),
				"SetSelection(Material, mat_glass) for the value-param edit" );
			// `ior` is a present scalar value param (`ior 1.5` in the fixture),
			// NOT a painter slot -- the exact class SetMaterialProperty rejects.
			Check( ctrl.SetProperty( String( "ior" ), String( "1.7" ) ),
				"MONEY (T6): SetProperty(ior, 1.7) on a material VALUE param SUCCEEDS (was rejected pre-fix)" );
			Check( CstValueOf( pJob, "mat_glass", "material", "ior" ) == std::string( "1.7" ),
				"MONEY (T6): the edit persisted into the retained CST (ior == 1.7)" );
		}

		// ---------- T7: dangling-reference guard ----------
		// External-review P1: rebinding a material slot to a RUNTIME-only painter
		// (registered in the manager but with no CST chunk) would render now but
		// write a DANGLING reference that fails on save/reload.  The general
		// guard (WouldPersistDanglingReference_) rejects exactly that case while
		// leaving CST-backed rebinds AND inline literals untouched.
		std::printf( "T7: dangling-reference guard on material slot rebind...\n" );
		{
			// Create a painter that exists at RUNTIME but has no CST chunk.
			Check( pJob->AddCheckerPainter( "runtime_only_ckr", 1.0, "pnt_albedo", "pnt_albedo" ),
				"fixture: registered a runtime-only painter (no CST chunk)" );
			Check( ctrl.SetSelection( Category::Material, String( "mat_diffuse" ) ),
				"SetSelection(Material, mat_diffuse) for the slot-rebind guard" );
			// Rebind reflectance to the runtime-only painter -> MUST be rejected
			// (mutation-verified: without the guard this SUCCEEDS and persists a
			// dangling `reflectance runtime_only_ckr` into the CST).
			Check( !ctrl.SetProperty( String( "reflectance" ), String( "runtime_only_ckr" ) ),
				"MONEY (T7): rebinding a slot to a runtime-only painter is REJECTED (no dangling ref)" );
			Check( CstValueOf( pJob, "mat_diffuse", "material", "reflectance" ) == std::string( "pnt_albedo" ),
				"MONEY (T7): the CST reflectance binding is UNCHANGED after the rejected edit" );
			// A CST-backed rebind to a DIFFERENT painter is NOT falsely rejected.
			Check( ctrl.SetProperty( String( "reflectance" ), String( "pnt_albedo2" ) ),
				"T7: rebinding to a CST-backed painter still SUCCEEDS (guard is not over-broad)" );
			Check( CstValueOf( pJob, "mat_diffuse", "material", "reflectance" ) == std::string( "pnt_albedo2" ),
				"T7: the CST-backed rebind persisted (reflectance == pnt_albedo2)" );
		}
	}

	pJob->release();
	std::remove( tmp.c_str() );
	std::printf( "=== GeometryPanelJumpTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
