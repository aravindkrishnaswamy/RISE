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
//      T8  Guard category-correctness (round-3 P1) -- a same-named CST
//          chunk of the WRONG category (shader `global`) does not vouch
//          for a painter reference; the edit is still rejected.
//      T9  Guard declaration order (round-3 P1) -- a LATER-declared CST
//          painter (pnt_late) is a forward reference (DeriveToJob is
//          sequential); the edit is rejected.
//      T10 Rasterizer CST round-trip (round-3 P1) -- SetProperty(samples)
//          records into the retained Document (was runtime-only, lost on
//          save/reload) and flips HasUnsavedChanges.
//      T11 Unrecordable-edit refusal (round-4 P1) -- on a scene with NO
//          rasterizer chunk, the edit is refused up-front and the dirty
//          tracker stays clean (no "saved but lost" phantom).
//      T12 Listener re-entry (round-4 P1 + document-first phase 1) -- an
//          enumeration getter called INSIDE the dirty-changed listener
//          returns without deadlocking (post-unlock drain + snapshot
//          getters).  HANGS on regression.
//      T13 Rasterizer undo (document-first phase 3) -- the migrated
//          transaction-path rasterizer edit is undoable; Undo restores
//          `samples 8` in the CST.
//      T14 Kind-constrained lookup -- an agent request naming a geometry
//          while claiming `kind=material` is rejected without moving head.
//      T15 Unnamed-singleton lookup -- an empty material target does not
//          resolve to the fixture's sole NAMED material.
//      T16 Per-category callback coherence -- a dirty callback sees the
//          primary Object selection during a secondary Material edit, and
//          a re-entrant Light selection remains authoritative.
//      T17 Callback destruction refusal -- the public C dirty callback may
//          request Destroy, but deletion is deferred to an owner call after
//          copied callback-target retirement.
//      T20 POSITIVE cross-category narrowing (round-7) -- with a geometry
//          and a material BOTH named `x`, kind=material edits the material
//          and kind=geometry edits the geometry (the success side of T14).
//      T21 Camera typo refusal (round-7) -- in a document with an unnamed
//          pinhole + `name tele` camera, a typo'd `tel` camera edit is
//          REFUSED instead of silently falling back to the unnamed camera.
//      T22 Camera fallback positive directions (round-7) -- the ACTIVE
//          camera's registered runtime name (`default`) and the EMPTY
//          (Camera,"") address both still resolve the unnamed camera even
//          with a named sibling present (pins named-chunk + unnamed-
//          singleton coexistence as INTENDED resolve-the-unnamed).
//      T23 Single-camera-total fallback (round-7) -- in a ONE-camera
//          document, a camera-kind address that matches no name still
//          resolves the sole camera (the documented single-camera rule).
//      T24 Post-narrowing occurrence count (round-7 P3) -- removing `x`
//          with kind=light reports NOT-FOUND (-1), not "2 chunks named x
//          match" ambiguity (-2), now that occurrences narrows by kind.
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
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
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
		"omni_light\n{\nname lgt_a\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n\n"
		// T9 fixture: a painter declared AFTER every material chunk.  DeriveToJob is
		// sequential, so binding mat_diffuse.reflectance to it would be a FORWARD
		// reference that fails on reload -- the guard must reject it.
		"uniformcolor_painter\n{\nname pnt_late\ncolor 0.9 0.1 0.1\n}\n";

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

	struct DestroyOnDirtyContext
	{
		SceneEditController* controller = nullptr;
		int fires = 0;
		bool destroyReturned = false;
	};

	struct DirtyListenerReleaseState
	{
		std::atomic<bool> destructorRan{ false };
		std::atomic<bool> reentryReturned{ false };
	};

	struct ReenteringDirtyListenerTarget
	{
		SceneEditController* controller = nullptr;
		std::shared_ptr<DirtyListenerReleaseState> state;

		~ReenteringDirtyListenerTarget()
		{
			state->destructorRan.store( true, std::memory_order_release );
			controller->SetDirtyChangedListener(
				SceneEditController::DirtyChangedFn() );
			state->reentryReturned.store( true, std::memory_order_release );
		}
	};

	struct DestroyingDirtyListenerTarget
	{
		SceneEditController* controller = nullptr;
		std::shared_ptr<DirtyListenerReleaseState> state;

		~DestroyingDirtyListenerTarget()
		{
			state->destructorRan.store( true, std::memory_order_release );
			RISE_API_DestroySceneEditController( controller );
			state->reentryReturned.store( true, std::memory_order_release );
		}
	};

	struct StagingDirtyListenerTarget
	{
		SceneEditController* controller = nullptr;
		std::shared_ptr<DirtyListenerReleaseState> state;

		~StagingDirtyListenerTarget()
		{
			state->destructorRan.store( true, std::memory_order_release );
			const SceneEditController::AgentProposal proposal;
			const bool refused = controller->StageProposal( proposal ) == 0;
			state->reentryReturned.store( refused, std::memory_order_release );
		}
	};

	struct CopyCountingDirtyListener
	{
		std::shared_ptr<std::atomic<int>> copies;
		std::shared_ptr<std::atomic<int>> calls;

		CopyCountingDirtyListener(
			const std::shared_ptr<std::atomic<int>>& copiesIn,
			const std::shared_ptr<std::atomic<int>>& callsIn )
			: copies( copiesIn ), calls( callsIn )
		{
		}
		CopyCountingDirtyListener( const CopyCountingDirtyListener& other )
			: copies( other.copies ), calls( other.calls )
		{
			copies->fetch_add( 1, std::memory_order_acq_rel );
		}
		void operator()( bool ) const
		{
			calls->fetch_add( 1, std::memory_order_acq_rel );
		}
	};

	void DestroyControllerOnDirty( void* userData, int )
	{
		DestroyOnDirtyContext* context = static_cast<DestroyOnDirtyContext*>( userData );
		++context->fires;
		RISE_API_DestroySceneEditController( context->controller );
		context->destroyReturned = true;
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
		const int typeIdx = RowIndexFor( ctrl, Category::Geometry, "chunk_type" );
		Check( typeIdx == 0 && !ctrl.PropertyEditableFor( Category::Geometry, 0 ),
			"leading read-only `chunk_type` identity row present" );
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

		// ---------- T8: guard is CATEGORY-correct (round-3 P1) ----------
		// A CST chunk of the WRONG category sharing the value's name must not
		// vouch for it: `global` names the standard_shader chunk (declared
		// first), so the pre-fix any-role probe accepted it -- but a painter
		// reference to `global` (backed only by a runtime-registered painter)
		// still dangles on reload.  Mutation-verified against categorySatisfies.
		std::printf( "T8: dangling-reference guard is category-correct...\n" );
		{
			Check( pJob->AddCheckerPainter( "global", 1.0, "pnt_albedo", "pnt_albedo" ),
				"fixture: registered a runtime-only painter named `global` (collides with the shader chunk)" );
			Check( ctrl.SetSelection( Category::Material, String( "mat_diffuse" ) ),
				"SetSelection(Material, mat_diffuse) for the category test" );
			Check( !ctrl.SetProperty( String( "reflectance" ), String( "global" ) ),
				"MONEY (T8): a same-named WRONG-category CST chunk does not vouch -- edit REJECTED" );
			Check( CstValueOf( pJob, "mat_diffuse", "material", "reflectance" ) == std::string( "pnt_albedo2" ),
				"T8: the CST reflectance binding is unchanged" );
		}

		// ---------- T9: guard enforces declaration order (round-3 P1) ----------
		// `pnt_late` IS a CST painter -- but declared AFTER every material
		// chunk.  DeriveToJob is sequential, so the binding would be a forward
		// reference that fails on reload.  Mutation-verified against the
		// `i < refIdx` order bound.
		std::printf( "T9: dangling-reference guard enforces declaration order...\n" );
		{
			Check( !ctrl.SetProperty( String( "reflectance" ), String( "pnt_late" ) ),
				"MONEY (T9): a LATER-declared CST painter is a forward reference -- edit REJECTED" );
			Check( CstValueOf( pJob, "mat_diffuse", "material", "reflectance" ) == std::string( "pnt_albedo2" ),
				"T9: the CST reflectance binding is unchanged" );
		}

		// ---------- T10: rasterizer param edit round-trips the CST (round-3 P1) ----------
		// SetRasterizerParameter only rebuilt the runtime instance; without the
		// CST record, `samples 8 -> 16` reverted on save/reload and Save never
		// enabled.  Verify the record (SerializeNode carries the new value) and
		// the dirty flip.
		std::printf( "T10: rasterizer param edit records into the CST + marks dirty...\n" );
		{
			Check( ctrl.SetSelection( Category::Rasterizer, String( "pathtracing_pel_rasterizer" ) ),
				"SetSelection(Rasterizer, pathtracing_pel_rasterizer) succeeds" );
			Check( ctrl.SetProperty( String( "samples" ), String( "16" ) ),
				"SetProperty(samples, 16) on the rasterizer succeeds" );
			const RISE::Cst::Document* doc = pJob->GetCstDocument();
			bool recorded = false;
			if( doc ) {
				const RISE::Cst::NodeId rid = RISE::Cst::DocFindByNameAnyRole(
					*doc, "", nullptr, "pathtracing_pel_rasterizer", true );
				const RISE::Cst::NodeRef chunk = rid ? RISE::Cst::DocResolveNodeId( *doc, rid )
				                                     : RISE::Cst::NodeRef();
				if( chunk ) {
					const std::string bytes = RISE::Cst::SerializeNode( chunk );
					recorded = ( bytes.find( "samples 16" ) != std::string::npos );
				}
			}
			Check( recorded,
				"MONEY (T10): the retained CST rasterizer chunk now carries `samples 16`" );
			Check( ctrl.HasUnsavedChanges(),
				"MONEY (T10): the edit marked the scene dirty (Save enables)" );
			// Document-first phase 3: the edit rides the transaction path, whose
			// re-derive must RE-REALIZE the live rasterizer from its chunk.
			Check( pJob->GetRasterizerParameter( "pathtracing_pel_rasterizer", "samples" ) == std::string( "16" ),
				"MONEY (T10): the LIVE rasterizer re-realized with samples == 16 (derive-driven, not a bespoke rebuild)" );
		}

		// ---------- T13: rasterizer edit is UNDOABLE (document-first phase 3) ----------
		// The bespoke arm had no undo (\"rebuilding a rasterizer is heavy\");
		// riding the transaction path makes the edit a first-class history
		// citizen for free.  Undo must restore samples 8 in the CST.
		std::printf( "T13: rasterizer edit undo restores the prior value...\n" );
		{
			ctrl.Undo();
			const RISE::Cst::Document* doc = pJob->GetCstDocument();
			bool restored = false;
			if( doc ) {
				const RISE::Cst::NodeId rid = RISE::Cst::DocFindByNameAnyRole(
					*doc, "", nullptr, "pathtracing_pel_rasterizer", true );
				const RISE::Cst::NodeRef chunk = rid ? RISE::Cst::DocResolveNodeId( *doc, rid )
				                                     : RISE::Cst::NodeRef();
				if( chunk ) {
					const std::string bytes = RISE::Cst::SerializeNode( chunk );
					restored = ( bytes.find( "samples 8" ) != std::string::npos );
				}
			}
			Check( restored,
				"MONEY (T13): Undo restored `samples 8` in the CST (rasterizer edits are now undoable)" );
			Check( pJob->GetRasterizerParameter( "pathtracing_pel_rasterizer", "samples" ) == std::string( "8" ),
				"MONEY (T13): Undo also re-realized the LIVE rasterizer with samples == 8" );
			ctrl.Redo();
			Check( pJob->GetRasterizerParameter( "pathtracing_pel_rasterizer", "samples" ) == std::string( "16" ),
				"MONEY (T13): Redo re-realized the LIVE rasterizer with samples == 16" );
			ctrl.Undo();
		}

	}

	// =====================================================================
	// T11 + T12 run on a FRESH scene that declares NO rasterizer chunk --
	// exactly the "lazily-created / no unique CST chunk" configuration T11
	// exercises, and a clean dirty-tracker for T12's transition.
	// =====================================================================
	const char* const kSceneNoRaster =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"film\n{\nwidth 24\nheight 24\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_a\ncolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_b\ncolor 0.2 0.7 0.3\n}\n\n"
		"lambertian_material\n{\nname mat_a\nreflectance pnt_a\n}\n\n"
		"sphere_geometry\n{\nname g1\nradius 0.5\n}\n\n"
		"standard_object\n{\nname obj_a\ngeometry g1\nmaterial mat_a\n}\n\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";
	const std::string tmp2 = TempPath( "geometry_panel_jump_noraster.RISEscene" );
	Job* pJob2 = LoadScene( kSceneNoRaster, tmp2 );
	Check( pJob2 != nullptr, "no-rasterizer fixture scene loads via CST" );
	if( pJob2 )
	{
		SceneEditController ctrl2( *pJob2, nullptr );   // skeleton

		// ---------- T11: unrecordable rasterizer edit is REFUSED (round-4 P1) ----------
		// The scene has NO rasterizer chunk, so the (lazily-created) live
		// rasterizer's edits cannot be recorded.  Round-3 applied the live edit
		// AND marked dirty anyway -- Save then cleared the dirty state while
		// serializing NO change, and a reload reverted the edit ("saved but
		// lost").  The round-4 pre-flight refuses the edit up front and leaves
		// the dirty tracker untouched.
		std::printf( "T11: unrecordable rasterizer edit is refused (no chunk to record into)...\n" );
		{
			Check( ctrl2.SetSelection( Category::Rasterizer, String( "pathtracing_pel_rasterizer" ) ),
				"T11: SetSelection(Rasterizer, pathtracing_pel_rasterizer) succeeds (live activation)" );
			Check( !ctrl2.SetProperty( String( "samples" ), String( "4" ) ),
				"MONEY (T11): the edit is REFUSED -- no `pathtracing_pel_rasterizer` chunk to record into" );
			Check( !ctrl2.HasUnsavedChanges(),
				"MONEY (T11): the refused edit did NOT mark the scene dirty (no phantom Save)" );
		}

		// ---------- T12: dirty-listener re-entry does not deadlock ----------
		// Round-4: the listener fired on the mutating thread with mMutex HELD;
		// the snapshot getters made an in-listener re-entry serve the stale
		// (primed) snapshot instead of deadlocking.  Document-first phase 1
		// went further: the listener now fires from the POST-UNLOCK drain, so
		// the in-listener getter may even refresh.  Either way this test HANGS
		// if the no-deadlock property regresses.
		std::printf( "T12: enumeration getter inside the under-lock dirty listener...\n" );
		{
			// Prime the snapshot on this (clean, un-contended) thread first --
			// the in-listener read serves the LAST snapshot, and an unprimed
			// one is legitimately empty.
			Check( ctrl2.CategoryEntityCount( Category::Geometry ) == 1,
				"T12: primed enumeration sees the 1-geometry list" );
			unsigned int fromListener = 999;
			int fired = 0;
			ctrl2.SetDirtyChangedListener( [&]( bool ) {
				++fired;
				fromListener = ctrl2.CategoryEntityCount( Category::Geometry );
			} );
			Check( ctrl2.SetSelection( Category::Geometry, String( "g1" ) ),
				"T12: SetSelection(Geometry, g1)" );
			Check( ctrl2.SetProperty( String( "radius" ), String( "0.75" ) ),
				"T12: geometry edit succeeds with a re-entrant listener installed" );
			Check( fired > 0,
				"T12: the clean->dirty transition fired the listener" );
			Check( fromListener == 1,
				"MONEY (T12): the getter called INSIDE the dirty listener returned the 1-geometry list (no deadlock, no empty flicker)" );
			ctrl2.SetDirtyChangedListener( SceneEditController::DirtyChangedFn() );
		}

		// ---------- T14: a supplied kind is a hard constraint ----------
		std::printf( "T14: uniquely named wrong-kind agent target is rejected...\n" );
		{
			const RISE::Cst::CstHeadVersion before = pJob2->GetCstHeadVersion();
			const std::string radiusBefore = CstValueOf( pJob2, "g1", "geometry", "radius" );
			const SceneEditController::AgentCommitResult r = ctrl2.ApplyAgentParamEdit(
				String( "g1" ), String( "material" ), String( "radius" ), String( "1.25" ), nullptr );
			Check( !r.applied,
				"MONEY (T14): name=g1, kind=material does not target the uniquely named sphere_geometry" );
			Check( pJob2->GetCstHeadVersion() == before,
				"T14: wrong-kind rejection leaves the CST head unchanged" );
			Check( CstValueOf( pJob2, "g1", "geometry", "radius" ) == radiusBefore,
				"T14: sphere radius remains unchanged" );
		}

		// ---------- T15: empty target addresses unnamed singletons only ----------
		std::printf( "T15: empty material target does not resolve the sole named material...\n" );
		{
			const RISE::Cst::CstHeadVersion before = pJob2->GetCstHeadVersion();
			const std::string reflectanceBefore = CstValueOf( pJob2, "mat_a", "material", "reflectance" );
			const SceneEditController::AgentCommitResult r = ctrl2.ApplyAgentParamEdit(
				String( "" ), String( "material" ), String( "reflectance" ), String( "pnt_b" ), nullptr );
			Check( !r.applied,
				"MONEY (T15): empty target + material rejects instead of mutating sole named mat_a" );
			Check( pJob2->GetCstHeadVersion() == before,
				"T15: named-singleton rejection leaves the CST head unchanged" );
			Check( CstValueOf( pJob2, "mat_a", "material", "reflectance" ) == reflectanceBefore,
				"T15: mat_a reflectance remains unchanged" );
		}

		// ---------- T16: per-category edit never swaps primary selection ----------
		std::printf( "T16: dirty callback sees coherent primary selection during secondary edit...\n" );
		{
			ctrl2.Editor().ClearDirtyState();
			ctrl2.Editor().DrainDirtyNotification();
			Check( ctrl2.SetSelection( Category::Object, String( "obj_a" ) ),
				"T16: Object obj_a is the primary selection" );
			Category seenCategory = Category::None;
			String seenName;
			int fired = 0;
			ctrl2.SetDirtyChangedListener( [&]( bool ) {
				++fired;
				seenCategory = ctrl2.GetSelectionCategory();
				seenName = ctrl2.GetSelectionName();
				ctrl2.SetSelection( Category::Light, String( "lgt" ) );
			} );
			Check( ctrl2.SetPropertyForCategory(
					Category::Material, String( "reflectance" ), String( "pnt_b" ) ),
				"T16: secondary Material edit succeeds" );
			Check( fired == 1, "T16: clean->dirty callback fired exactly once" );
			Check( seenCategory == Category::Object && seenName == String( "obj_a" ),
				"MONEY (T16): callback observed the real primary Object selection, not temporary Material state" );
			Check( ctrl2.GetSelectionCategory() == Category::Light
					&& ctrl2.GetSelectionName() == String( "lgt" ),
				"MONEY (T16): re-entrant Light selection was not overwritten when the edit returned" );
			ctrl2.SetDirtyChangedListener( SceneEditController::DirtyChangedFn() );
		}

		pJob2->release();
		std::remove( tmp2.c_str() );
	}

	// ---------- T17: C callback-local destruction is rejected ----------
	std::printf( "T17: dirty callback-local Destroy is rejected until callback retirement...\n" );
	const std::string tmp3 = TempPath( "geometry_panel_jump_destroy_callback.RISEscene" );
	Job* pJob3 = LoadScene( kSceneNoRaster, tmp3 );
	Check( pJob3 != nullptr, "T17: destruction fixture scene loads" );
	if( pJob3 )
	{
		DestroyOnDirtyContext context;
		Check( RISE_API_CreateSceneEditController( pJob3, nullptr, &context.controller ),
			"T17: C API creates the controller" );
		Check( RISE_API_SceneEditController_SetDirtyChangedCallback(
				context.controller, &DestroyControllerOnDirty, &context ),
			"T17: destruction callback installs" );
		Check( RISE_API_SceneEditController_SetSelection(
				context.controller, static_cast<int>( Category::Geometry ), "g1" ),
			"T17: selects geometry before the dirtying edit" );
		const bool editReturned = RISE_API_SceneEditController_SetProperty(
			context.controller, "radius", "0.9" );
		Check( editReturned, "T17: the dirtying C API call returned after callback Destroy refusal" );
		Check( context.fires == 1 && context.destroyReturned
		    && context.controller != nullptr,
			"MONEY (T17): callback-local Destroy is rejected and the drain returns with its controller alive" );
		RISE_API_DestroySceneEditController( context.controller );
		context.controller = nullptr;
		pJob3->release();
		std::remove( tmp3.c_str() );
	}

	// ---------- T18: catch-all drain survives callback Destroy refusal ----------
	std::printf( "T18: RefreshProperties catch-all survives callback-local Destroy refusal...\n" );
	const std::string tmp4 = TempPath( "geometry_panel_jump_refresh_destroy.RISEscene" );
	Job* pJob4 = LoadScene( kScene, tmp4 );
	Check( pJob4 != nullptr, "T18: catch-all destruction fixture loads" );
	if( pJob4 )
	{
		SceneEditController* controller =
			new SceneEditController( *pJob4, /*interactiveRasterizer*/nullptr );
		int fires = 0;
		controller->SetDirtyChangedListener( [&]( bool ) {
			++fires;
			RISE_API_DestroySceneEditController( controller );
		} );
		controller->SetTool( SceneEditController::Tool::OrbitCamera );
		controller->OnPointerDown( Point2( 8, 8 ) );
		controller->OnPointerMove( Point2( 12, 9 ) );
		controller->OnPointerUp( Point2( 12, 9 ) );
		// Pointer gestures intentionally leave delivery to the per-frame
		// catch-all. RefreshProperties must return immediately when that
		// callback requests destruction (which is rejected until return).
		controller->RefreshProperties();
		Check( fires == 1 && controller != nullptr,
			"MONEY (T18): catch-all drain rejects callback-local destruction and returns safely" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob4->release();
		std::remove( tmp4.c_str() );
	}

	// ---------- T19: detach waits out an already-copied callback ----------
	std::printf( "T19: dirty-listener detach is a quiescence barrier...\n" );
	const std::string tmp5 = TempPath( "geometry_panel_jump_detach_barrier.RISEscene" );
	Job* pJob5 = LoadScene( kSceneNoRaster, tmp5 );
	Check( pJob5 != nullptr, "T19: detach fixture scene loads" );
	if( pJob5 )
	{
		SceneEditController controller( *pJob5, /*interactiveRasterizer*/nullptr );
		Check( controller.SetSelection( Category::Geometry, String( "g1" ) ),
			"T19: selects geometry before the dirtying edit" );
		std::atomic<bool> callbackEntered{ false };
		std::atomic<bool> releaseCallback{ false };
		std::atomic<bool> detachReturned{ false };
		std::atomic<bool> editReturned{ false };
		std::atomic<unsigned int> fires{ 0 };
		controller.SetDirtyChangedListener( [&]( bool ) {
			fires.fetch_add( 1, std::memory_order_acq_rel );
			callbackEntered.store( true, std::memory_order_release );
			while( !releaseCallback.load( std::memory_order_acquire ) ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			}
		} );
		std::thread editThread( [&] {
			editReturned.store(
				controller.SetProperty( String( "radius" ), String( "0.91" ) ),
				std::memory_order_release );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !callbackEntered.load( std::memory_order_acquire );
		     ++i ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		Check( callbackEntered.load( std::memory_order_acquire ),
			"T19: dirty callback is in flight" );
		std::thread detachThread( [&] {
			controller.SetDirtyChangedListener(
				SceneEditController::DirtyChangedFn() );
			detachReturned.store( true, std::memory_order_release );
		} );
		std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
		Check( !detachReturned.load( std::memory_order_acquire ),
			"MONEY (T19): detach has not returned while the copied callback is still in flight" );
		releaseCallback.store( true, std::memory_order_release );
		editThread.join();
		detachThread.join();
		Check( editReturned.load( std::memory_order_acquire ),
			"T19: dirtying edit returns after callback release" );
		Check( detachReturned.load( std::memory_order_acquire ),
			"MONEY (T19): detach returns after the in-flight callback quiesces" );
		Check( controller.SetProperty( String( "radius" ), String( "0.92" ) ),
			"T19: a later edit still succeeds" );
		Check( fires.load( std::memory_order_acquire ) == 1,
			"T19: detached callback never fires again" );
		pJob5->release();
		std::remove( tmp5.c_str() );
	}

	// ---------- T25: listener target final release occurs outside its mutex ----------
	std::printf( "T25: dirty-listener target destructor may replace the listener...\n" );
	const std::string tmp8 = TempPath( "geometry_panel_jump_listener_release_reentry.RISEscene" );
	Job* pJob8 = LoadScene( kSceneNoRaster, tmp8 );
	Check( pJob8 != nullptr, "T25: listener-release fixture loads" );
	if( pJob8 )
	{
		SceneEditController controller( *pJob8, nullptr );
		auto state = std::make_shared<DirtyListenerReleaseState>();
		auto target = std::make_shared<ReenteringDirtyListenerTarget>();
		target->controller = &controller;
		target->state = state;
		controller.SetDirtyChangedListener( [target]( bool ) {} );
		target.reset();
		controller.SetDirtyChangedListener( []( bool ) {} );
		Check( state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire ),
			"MONEY (T25): retired std::function target re-enters listener replacement after the notification mutex is unlocked" );
		controller.SetDirtyChangedListener( SceneEditController::DirtyChangedFn() );
		pJob8->release();
		std::remove( tmp8.c_str() );
	}

	// ---------- T26: teardown listener-target destruction cannot recurse ----------
	std::printf( "T26: dirty-listener target destructor cannot recursively destroy its controller...\n" );
	const std::string tmp9 = TempPath( "geometry_panel_jump_listener_release_destroy.RISEscene" );
	Job* pJob9 = LoadScene( kSceneNoRaster, tmp9 );
	Check( pJob9 != nullptr, "T26: listener teardown fixture loads" );
	if( pJob9 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob9, nullptr, &controller ),
			"T26: C API creates the listener teardown controller" );
		if( controller )
		{
			auto state = std::make_shared<DirtyListenerReleaseState>();
			auto target = std::make_shared<DestroyingDirtyListenerTarget>();
			target->controller = controller;
			target->state = state;
			controller->SetDirtyChangedListener( [target]( bool ) {} );
			target.reset();
			RISE_API_DestroySceneEditController( controller );
			controller = nullptr;
			Check( state->destructorRan.load( std::memory_order_acquire )
			    && state->reentryReturned.load( std::memory_order_acquire ),
				"MONEY (T26): teardown listener-target release rejects recursive Destroy and the outer destruction completes" );
		}
		pJob9->release();
		std::remove( tmp9.c_str() );
	}

	// ---------- T27: raw teardown gates listener destructors before Job access ----------
	std::printf( "T27: raw teardown rejects listener-destructor Job access after owner release...\n" );
	const std::string tmp10 = TempPath( "geometry_panel_jump_listener_release_dead_job.RISEscene" );
	Job* pJob10 = LoadScene( kSceneNoRaster, tmp10 );
	Check( pJob10 != nullptr, "T27: raw listener teardown fixture loads" );
	if( pJob10 )
	{
		auto state = std::make_shared<DirtyListenerReleaseState>();
		{
			SceneEditController controller( *pJob10, nullptr );
			auto target = std::make_shared<StagingDirtyListenerTarget>();
			target->controller = &controller;
			target->state = state;
			controller.SetDirtyChangedListener( [target]( bool ) {} );
			target.reset();
			controller.Stop();
			pJob10->release();
			pJob10 = nullptr;
		}
		Check( state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire ),
			"MONEY (T27): listener final release sees raw-destructor teardown and StageProposal refuses before dereferencing the dead borrowed Job" );
		std::remove( tmp10.c_str() );
	}

	// ---------- T28: dirty callback cannot double-delete a raw destructor ----------
	std::printf( "T28: dirty callback Destroy is rejected once raw destruction begins...\n" );
	const std::string tmp11 = TempPath( "geometry_panel_jump_raw_destroy_dirty_callback.RISEscene" );
	Job* pJob11 = LoadScene( kSceneNoRaster, tmp11 );
	Check( pJob11 != nullptr, "T28: concurrent raw-destruction fixture loads" );
	if( pJob11 )
	{
		SceneEditController* controller =
			new SceneEditController( *pJob11, nullptr );
		Check( controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T28: selects geometry before the dirtying edit" );
		std::atomic<bool> callbackEntered{ false };
		std::atomic<bool> allowCallbackDestroy{ false };
		std::atomic<bool> callbackDestroyReturned{ false };
		std::atomic<bool> editReturned{ false };
		std::atomic<bool> outerDestroyReturned{ false };
		auto lateListenerState = std::make_shared<DirtyListenerReleaseState>();
		controller->SetDirtyChangedListener( [&]( bool ) {
			callbackEntered.store( true, std::memory_order_release );
			while( !allowCallbackDestroy.load( std::memory_order_acquire ) )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			// Teardown has closed registration while waiting for this callback.
			// Attempting to replace the detached listener must release this target
			// immediately, not strand it until SceneEditor's late member teardown.
			auto lateTarget = std::make_shared<ReenteringDirtyListenerTarget>();
			lateTarget->controller = controller;
			lateTarget->state = lateListenerState;
			controller->SetDirtyChangedListener( [lateTarget]( bool ) {} );
			lateTarget.reset();
			RISE_API_DestroySceneEditController( controller );
			callbackDestroyReturned.store( true, std::memory_order_release );
		} );
		std::thread editThread( [&] {
			editReturned.store(
				controller->SetProperty( String( "radius" ), String( "0.93" ) ),
				std::memory_order_release );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !callbackEntered.load( std::memory_order_acquire ); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		Check( callbackEntered.load( std::memory_order_acquire ),
			"T28: dirty callback is held in flight" );
		std::thread destroyThread( [&] {
			delete controller;
			outerDestroyReturned.store( true, std::memory_order_release );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !controller->IsRawDestructionInProgress(); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		Check( controller->IsRawDestructionInProgress(),
			"T28: raw destructor publishes its single-owner gate before callback quiescence" );
		allowCallbackDestroy.store( true, std::memory_order_release );
		editThread.join();
		destroyThread.join();
		controller = nullptr;
		Check( editReturned.load( std::memory_order_acquire )
		    && callbackDestroyReturned.load( std::memory_order_acquire )
		    && outerDestroyReturned.load( std::memory_order_acquire )
		    && lateListenerState->destructorRan.load( std::memory_order_acquire )
		    && lateListenerState->reentryReturned.load( std::memory_order_acquire ),
			"MONEY (T28): callback-side registration/Destroy are rejected after terminal closure and the owning raw destructor completes" );
		pJob11->release();
		std::remove( tmp11.c_str() );
	}

	// ---------- T29: C Destroy owns preparation before callback quiescence ----------
	std::printf( "T29: concurrent C Destroy calls have one preparation/delete owner...\n" );
	const std::string tmp12 = TempPath( "geometry_panel_jump_c_destroy_claim.RISEscene" );
	Job* pJob12 = LoadScene( kSceneNoRaster, tmp12 );
	Check( pJob12 != nullptr, "T29: C-destruction claim fixture loads" );
	if( pJob12 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob12, nullptr, &controller )
		    && controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T29: controller and dirtying selection are ready" );
		std::atomic<bool> callbackEntered{ false };
		std::atomic<bool> allowCallbackDestroy{ false };
		std::atomic<bool> callbackDestroyReturned{ false };
		std::atomic<bool> outerDestroyReturned{ false };
		controller->SetDirtyChangedListener( [&]( bool ) {
			callbackEntered.store( true, std::memory_order_release );
			while( !allowCallbackDestroy.load( std::memory_order_acquire ) )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			RISE_API_DestroySceneEditController( controller );
			callbackDestroyReturned.store( true, std::memory_order_release );
		} );
		std::thread editThread( [&] {
			(void)controller->SetProperty( String( "radius" ), String( "0.94" ) );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !callbackEntered.load( std::memory_order_acquire ); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		std::thread destroyThread( [&] {
			RISE_API_DestroySceneEditController( controller );
			outerDestroyReturned.store( true, std::memory_order_release );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !controller->ForTest_DestructionClaimed(); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		Check( controller->ForTest_DestructionClaimed(),
			"T29: outer C Destroy claims lifecycle ownership before listener wait" );
		allowCallbackDestroy.store( true, std::memory_order_release );
		editThread.join();
		destroyThread.join();
		controller = nullptr;
		Check( callbackDestroyReturned.load( std::memory_order_acquire )
		    && outerDestroyReturned.load( std::memory_order_acquire ),
			"MONEY (T29): callback C Destroy returns without a second preparation/delete and the owner completes" );
		pJob12->release();
		std::remove( tmp12.c_str() );
	}

	// ---------- T30: explicit Prepare owns quiescence but remains retryable ----------
	std::printf( "T30: explicit Prepare excludes callback Destroy and completed Prepare stays usable...\n" );
	const std::string tmp13 = TempPath( "geometry_panel_jump_prepare_claim.RISEscene" );
	Job* pJob13 = LoadScene( kSceneNoRaster, tmp13 );
	Check( pJob13 != nullptr, "T30: explicit-prepare claim fixture loads" );
	if( pJob13 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob13, nullptr, &controller )
		    && controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T30: controller and dirtying selection are ready" );
		std::atomic<bool> callbackEntered{ false };
		std::atomic<bool> allowCallbackDestroy{ false };
		std::atomic<bool> callbackDestroyReturned{ false };
		std::atomic<bool> prepareReturned{ false };
		std::atomic<bool> prepareResult{ false };
		controller->SetDirtyChangedListener( [&]( bool ) {
			callbackEntered.store( true, std::memory_order_release );
			while( !allowCallbackDestroy.load( std::memory_order_acquire ) )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
			RISE_API_DestroySceneEditController( controller );
			callbackDestroyReturned.store( true, std::memory_order_release );
		} );
		std::thread editThread( [&] {
			(void)controller->SetProperty( String( "radius" ), String( "0.95" ) );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !callbackEntered.load( std::memory_order_acquire ); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		std::thread prepareThread( [&] {
			prepareResult.store(
				controller->PrepareForDestruction(), std::memory_order_release );
			prepareReturned.store( true, std::memory_order_release );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !controller->ForTest_DestructionClaimed(); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		Check( controller->ForTest_DestructionClaimed(),
			"T30: explicit Prepare claims lifecycle ownership before listener wait" );
		allowCallbackDestroy.store( true, std::memory_order_release );
		editThread.join();
		prepareThread.join();
		Check( callbackDestroyReturned.load( std::memory_order_acquire )
		    && prepareReturned.load( std::memory_order_acquire )
		    && prepareResult.load( std::memory_order_acquire )
		    && controller->PrepareForDestruction(),
			"MONEY (T30): callback Destroy is rejected, owner Prepare succeeds, and completed Prepare is idempotent" );
		Check( !controller->SetSelection( Category::Object, String( "obj_a" ) ),
			"MONEY (T30): completed terminal Prepare keeps ordinary mutation admission closed" );
		controller->SetTool( SceneEditController::Tool::OrbitCamera );
		controller->OnPointerDown( Point2( 8, 8 ) );
		Check( !controller->ForTest_PointerDown(),
			"MONEY (T30): completed terminal Prepare refuses a new pointer interaction before Destroy" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob13->release();
		std::remove( tmp13.c_str() );
	}

	// ---------- T31: copied dirty-listener target retires under callback guard ----------
	std::printf( "T31: self-detached dirty callback target finalizer cannot destroy its controller...\n" );
	const std::string tmp14 = TempPath( "geometry_panel_jump_dirty_copy_finalizer.RISEscene" );
	Job* pJob14 = LoadScene( kSceneNoRaster, tmp14 );
	Check( pJob14 != nullptr, "T31: copied-listener finalizer fixture loads" );
	if( pJob14 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob14, nullptr, &controller )
		    && controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T31: controller and dirtying selection are ready" );
		auto state = std::make_shared<DirtyListenerReleaseState>();
		auto target = std::make_shared<DestroyingDirtyListenerTarget>();
		target->controller = controller;
		target->state = state;
		std::atomic<bool> callbackDestroyReturned{ false };
		controller->SetDirtyChangedListener(
			[controller, target, &callbackDestroyReturned]( bool ) {
				controller->SetDirtyChangedListener(
					SceneEditController::DirtyChangedFn() );
				RISE_API_DestroySceneEditController( controller );
				callbackDestroyReturned.store( true, std::memory_order_release );
			} );
		target.reset();
		Check( controller->SetProperty( String( "radius" ), String( "0.96" ) ),
			"T31: dirtying edit and self-detaching callback return" );
		Check( callbackDestroyReturned.load( std::memory_order_acquire )
		    && state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire )
		    && controller->CategoryEntityCount( Category::Geometry ) == 1,
			"MONEY (T31): callback and copied-target finalizer Destroy attempts are rejected until copied target retirement completes" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob14->release();
		std::remove( tmp14.c_str() );
	}

	// ---------- T32: throwing dirty callback retires its copy under TLS ----------
	std::printf( "T32: throwing self-detached dirty callback retires its target before callback TLS...\n" );
	const std::string tmp15 = TempPath( "geometry_panel_jump_dirty_copy_throw.RISEscene" );
	Job* pJob15 = LoadScene( kSceneNoRaster, tmp15 );
	Check( pJob15 != nullptr, "T32: throwing copied-listener fixture loads" );
	if( pJob15 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob15, nullptr, &controller )
		    && controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T32: controller and dirtying selection are ready" );
		auto state = std::make_shared<DirtyListenerReleaseState>();
		auto target = std::make_shared<DestroyingDirtyListenerTarget>();
		target->controller = controller;
		target->state = state;
		controller->SetDirtyChangedListener(
			[controller, target]( bool ) {
				controller->SetDirtyChangedListener(
					SceneEditController::DirtyChangedFn() );
				throw std::runtime_error( "intentional dirty-listener failure" );
			} );
		target.reset();
		bool returnedWithoutThrow = true;
		try
		{
			(void)controller->SetProperty( String( "radius" ), String( "0.97" ) );
		}
		catch( ... )
		{
			returnedWithoutThrow = false;
		}
		Check( returnedWithoutThrow
		    && state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire )
		    && controller->CategoryEntityCount( Category::Geometry ) == 1,
			"MONEY (T32): listener exception is contained and its copied target retires while lifecycle rejection TLS is still active" );
		std::atomic<int> laterCalls{ 0 };
		controller->SetDirtyChangedListener( [&]( bool ) {
			laterCalls.fetch_add( 1, std::memory_order_acq_rel );
		} );
		controller->Editor().ClearDirtyState();
		controller->Editor().DrainDirtyNotification();
		Check( laterCalls.load( std::memory_order_acquire ) == 1,
			"MONEY (T32): throwing listener releases single-deliverer ownership for subsequent transitions" );

		// A deferral token drains from its noexcept destructor. Listener failures
		// must therefore be contained centrally rather than escape inconsistently
		// depending on which mutation path happened to deliver the transition.
		std::atomic<int> deferredThrowCalls{ 0 };
		controller->SetDirtyChangedListener( [&]( bool ) {
			deferredThrowCalls.fetch_add( 1, std::memory_order_acq_rel );
			throw std::runtime_error( "intentional deferred dirty-listener failure" );
		} );
		{
			auto deferral = controller->Editor().DeferDirtyNotifications();
			Check( controller->SetProperty( String( "radius" ), String( "0.98" ) ),
				"T32: deferral-backed dirtying edit succeeds" );
		}
		Check( deferredThrowCalls.load( std::memory_order_acquire ) == 1,
			"MONEY (T32): listener exception from noexcept deferral destruction is contained" );

		controller->Editor().ClearDirtyState();
		controller->Editor().DrainDirtyNotification();
		auto copies = std::make_shared<std::atomic<int>>( 0 );
		auto calls = std::make_shared<std::atomic<int>>( 0 );
		CopyCountingDirtyListener copyCounting( copies, calls );
		controller->SetDirtyChangedListener( copyCounting );
		const int copiesAfterInstall = copies->load( std::memory_order_acquire );
		Check( controller->SetProperty( String( "radius" ), String( "0.99" ) ),
			"T32: copy-counting dirtying edit succeeds" );
		Check( calls->load( std::memory_order_acquire ) == 1
		    && copies->load( std::memory_order_acquire ) == copiesAfterInstall,
			"MONEY (T32): delivery snapshots shared ownership without copying arbitrary callable state under the notification mutex" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob15->release();
		std::remove( tmp15.c_str() );
	}

	// ---------- T33: explicit Prepare closes inert proposal admission ----------
	std::printf( "T33: listener finalizer cannot stage after lifecycle preparation claims the controller...\n" );
	const std::string tmp16 = TempPath( "geometry_panel_jump_prepare_stage_gate.RISEscene" );
	Job* pJob16 = LoadScene( kSceneNoRaster, tmp16 );
	Check( pJob16 != nullptr, "T33: Prepare proposal-gate fixture loads" );
	if( pJob16 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob16, nullptr, &controller ),
			"T33: C API creates proposal-gate controller" );
		auto state = std::make_shared<DirtyListenerReleaseState>();
		auto target = std::make_shared<StagingDirtyListenerTarget>();
		target->controller = controller;
		target->state = state;
		controller->SetDirtyChangedListener( [target]( bool ) {} );
		target.reset();
		Check( controller->PrepareForDestruction(),
			"T33: explicit terminal preparation succeeds" );
		controller->Stop();
		Check( state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire ),
			"MONEY (T33): retired listener finalizer cannot StageProposal after Prepare claims lifecycle, and post-Prepare Stop preserves closure" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob16->release();
		std::remove( tmp16.c_str() );
	}

	// ---------- T34: cross-thread detach retires final target under TLS ----------
	std::printf( "T34: cross-thread detach keeps final listener-target retirement under lifecycle TLS...\n" );
	const std::string tmp17 = TempPath( "geometry_panel_jump_cross_thread_detach.RISEscene" );
	Job* pJob17 = LoadScene( kSceneNoRaster, tmp17 );
	Check( pJob17 != nullptr, "T34: cross-thread detach fixture loads" );
	if( pJob17 )
	{
		SceneEditController* controller = nullptr;
		Check( RISE_API_CreateSceneEditController( pJob17, nullptr, &controller )
		    && controller->SetSelection( Category::Geometry, String( "g1" ) ),
			"T34: controller and dirtying selection are ready" );
		auto state = std::make_shared<DirtyListenerReleaseState>();
		auto target = std::make_shared<DestroyingDirtyListenerTarget>();
		target->controller = controller;
		target->state = state;
		std::atomic<bool> callbackEntered{ false };
		std::atomic<bool> releaseCallback{ false };
		std::atomic<bool> detachReturned{ false };
		controller->SetDirtyChangedListener( [target, &callbackEntered, &releaseCallback]( bool ) {
			callbackEntered.store( true, std::memory_order_release );
			while( !releaseCallback.load( std::memory_order_acquire ) )
				std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		} );
		target.reset();
		std::thread editThread( [&] {
			(void)controller->SetProperty( String( "radius" ), String( "1.01" ) );
		} );
		for( unsigned int i = 0;
		     i < 5000 && !callbackEntered.load( std::memory_order_acquire ); ++i )
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		Check( callbackEntered.load( std::memory_order_acquire ),
			"T34: copied callback is in flight" );
		std::thread detachThread( [&] {
			controller->SetDirtyChangedListener( SceneEditController::DirtyChangedFn() );
			detachReturned.store( true, std::memory_order_release );
		} );
		std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
		Check( !detachReturned.load( std::memory_order_acquire ),
			"T34: detaching thread waits for the copied callback" );
		releaseCallback.store( true, std::memory_order_release );
		editThread.join();
		detachThread.join();
		Check( detachReturned.load( std::memory_order_acquire )
		    && state->destructorRan.load( std::memory_order_acquire )
		    && state->reentryReturned.load( std::memory_order_acquire )
		    && controller->CategoryEntityCount( Category::Geometry ) == 1,
			"MONEY (T34): detaching thread's final capture release rejects recursive Destroy and leaves owner deletion valid" );
		RISE_API_DestroySceneEditController( controller );
		controller = nullptr;
		pJob17->release();
		std::remove( tmp17.c_str() );
	}

	// =====================================================================
	// T20-T24 (round-7): cross-category narrowing success side, the gated
	// camera absent-name fallback (both directions), and the post-narrowing
	// occurrence count.  Fixture: TWO cameras (one named `tele`, one UNNAMED
	// -- the unnamed one is declared LAST so it is the ACTIVE camera,
	// registered under the runtime allocator's `default`), plus a geometry
	// and a material that BOTH carry the name `x`.
	// =====================================================================
	const char* const kSceneTwoCameras =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"film\n{\nwidth 24\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname tele\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 20.0\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_a\ncolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_b\ncolor 0.2 0.7 0.3\n}\n\n"
		"lambertian_material\n{\nname x\nreflectance pnt_a\n}\n\n"
		"sphere_geometry\n{\nname x\nradius 0.5\n}\n\n"
		"standard_object\n{\nname obj_a\ngeometry x\nmaterial x\n}\n\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";
	const std::string tmp6 = TempPath( "geometry_panel_jump_two_cameras.RISEscene" );
	Job* pJob6 = LoadScene( kSceneTwoCameras, tmp6 );
	Check( pJob6 != nullptr, "two-camera / name-collision fixture loads via CST" );
	if( pJob6 )
	{
		SceneEditController ctrl6( *pJob6, nullptr );   // skeleton

		// The UNNAMED camera chunk, resolved the same way the editor resolves the
		// (Camera, "") address; returns its serialized bytes ("" if unresolvable).
		auto unnamedCameraBytes = [&]() -> std::string {
			const RISE::Cst::Document* doc = pJob6->GetCstDocument();
			if( !doc ) return std::string();
			const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, "", nullptr, "camera", true );
			const RISE::Cst::NodeRef chunk = id ? RISE::Cst::DocResolveNodeId( *doc, id ) : RISE::Cst::NodeRef();
			return chunk ? RISE::Cst::SerializeNode( chunk ) : std::string();
		};

		// ---------- T20: cross-category narrowing SUCCESS side ----------
		std::printf( "T20: kind narrows a cross-category name collision to the right chunk...\n" );
		{
			const SceneEditController::AgentCommitResult rm = ctrl6.ApplyAgentParamEdit(
				String( "x" ), String( "material" ), String( "reflectance" ), String( "pnt_b" ), nullptr );
			Check( rm.applied,
				"MONEY (T20): name=x kind=material APPLIES despite a geometry also named x" );
			Check( CstValueOf( pJob6, "x", "material", "reflectance" ) == std::string( "pnt_b" ),
				"MONEY (T20): the MATERIAL x now carries reflectance pnt_b" );
			Check( CstValueOf( pJob6, "x", "geometry", "radius" ) == std::string( "0.5" ),
				"T20: the GEOMETRY x is untouched by the material-kind edit" );
			const SceneEditController::AgentCommitResult rg = ctrl6.ApplyAgentParamEdit(
				String( "x" ), String( "geometry" ), String( "radius" ), String( "0.6" ), nullptr );
			Check( rg.applied,
				"MONEY (T20): name=x kind=geometry APPLIES and targets the geometry" );
			Check( CstValueOf( pJob6, "x", "geometry", "radius" ) == std::string( "0.6" ),
				"T20: the GEOMETRY x now carries radius 0.6" );
			Check( CstValueOf( pJob6, "x", "material", "reflectance" ) == std::string( "pnt_b" ),
				"T20: the MATERIAL x is untouched by the geometry-kind edit" );
		}

		// ---------- T21: camera TYPO refusal (round-7 gated fallback) ----------
		std::printf( "T21: typo'd camera name refuses instead of editing the unnamed camera...\n" );
		{
			const RISE::Cst::CstHeadVersion before = pJob6->GetCstHeadVersion();
			const SceneEditController::AgentCommitResult r = ctrl6.ApplyAgentParamEdit(
				String( "tel" ), String( "camera" ), String( "fov" ), String( "25" ), nullptr );
			Check( !r.applied,
				"MONEY (T21): `tel` (typo of tele) is REFUSED -- no silent fallback to the unnamed camera" );
			Check( pJob6->GetCstHeadVersion() == before,
				"T21: the refused edit leaves the CST head unchanged" );
			Check( unnamedCameraBytes().find( "fov 40.0" ) != std::string::npos,
				"MONEY (T21): the unnamed camera still carries its original fov 40.0" );
			Check( CstValueOf( pJob6, "tele", "camera", "fov" ) == std::string( "20.0" ),
				"T21: the named camera `tele` is also untouched" );
			// The DESTRUCTIVE side of the same gate (round-7 review): a typo'd
			// camera REMOVE must refuse too -- reverting only ApplyCstRemoveChunk's
			// gate (back to unconditional kind=="camera" fallback) would delete
			// the unnamed camera here and is caught by exactly this pin.
			char kw[64]; kw[0] = '\0';
			char diag[256]; diag[0] = '\0';
			const int rmrc = pJob6->ApplyCstRemoveChunk( "tel", "camera", kw, sizeof( kw ), diag, sizeof( diag ) );
			Check( rmrc == -1,
				"MONEY (T21): remove `tel` kind=camera is REFUSED (-1) -- the destructive verb does not fall back either" );
			Check( unnamedCameraBytes().find( "fov 40.0" ) != std::string::npos,
				"T21: the unnamed camera chunk survives the refused remove" );
		}

		// ---------- T22: camera fallback POSITIVE directions ----------
		std::printf( "T22: active registered name + empty target still resolve the unnamed camera...\n" );
		{
			Check( pJob6->GetActiveCameraName() == std::string( "default" ),
				"T22: the unnamed camera (declared last) is ACTIVE under the runtime name `default`" );
			const SceneEditController::AgentCommitResult ra = ctrl6.ApplyAgentParamEdit(
				String( "default" ), String( "camera" ), String( "fov" ), String( "35" ), nullptr );
			Check( ra.applied,
				"MONEY (T22): the ACTIVE camera's registered name `default` still resolves the unnamed chunk" );
			Check( unnamedCameraBytes().find( "fov 35" ) != std::string::npos,
				"T22: the unnamed camera chunk now carries fov 35" );
			Check( CstValueOf( pJob6, "tele", "camera", "fov" ) == std::string( "20.0" ),
				"T22: the named sibling `tele` is untouched by the active-name edit" );
			const SceneEditController::AgentCommitResult re = ctrl6.ApplyAgentParamEdit(
				String( "" ), String( "camera" ), String( "fov" ), String( "38" ), nullptr );
			Check( re.applied,
				"MONEY (T22): the EMPTY (Camera, \"\") address resolves the unnamed singleton even with a named sibling (coexistence pinned as INTENDED)" );
			Check( unnamedCameraBytes().find( "fov 38" ) != std::string::npos,
				"T22: the unnamed camera chunk now carries fov 38" );
			Check( CstValueOf( pJob6, "tele", "camera", "fov" ) == std::string( "20.0" ),
				"T22: the named sibling `tele` is untouched by the empty-target edit" );
		}

		// ---------- T24: post-narrowing occurrence count (P3) ----------
		std::printf( "T24: wrong-kind removal reports not-found, not name-count ambiguity...\n" );
		{
			char kw[64]; kw[0] = '\0';
			char diag[256]; diag[0] = '\0';
			const int rc = pJob6->ApplyCstRemoveChunk( "x", "light", kw, sizeof( kw ), diag, sizeof( diag ) );
			Check( rc == -1,
				"MONEY (T24): remove x kind=light returns NOT-FOUND (-1), not ambiguous (-2) -- occurrences narrows by kind" );
			Check( std::string( diag ).find( "chunks named" ) == std::string::npos,
				"T24: no misleading `N chunks named x match` diagnostic for a wrong-kind target" );
		}

		pJob6->release();
		std::remove( tmp6.c_str() );
	}

	// ---------- T23: single-camera-total fallback survives the gate ----------
	std::printf( "T23: a one-camera document still resolves a no-match camera address by position...\n" );
	const std::string tmp7 = TempPath( "geometry_panel_jump_single_camera.RISEscene" );
	Job* pJob7 = LoadScene( kSceneNoRaster, tmp7 );
	Check( pJob7 != nullptr, "T23: single-camera fixture loads" );
	if( pJob7 )
	{
		SceneEditController ctrl7( *pJob7, nullptr );   // skeleton
		const SceneEditController::AgentCommitResult r = ctrl7.ApplyAgentParamEdit(
			String( "cam" ), String( "camera" ), String( "fov" ), String( "33" ), nullptr );
		Check( r.applied,
			"MONEY (T23): with exactly ONE camera chunk total, a non-matching camera address still resolves it (documented single-camera rule; the G5 swap recipe depends on this)" );
		const RISE::Cst::Document* doc = pJob7->GetCstDocument();
		bool has33 = false;
		if( doc ) {
			const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, "", nullptr, "camera", true );
			const RISE::Cst::NodeRef chunk = id ? RISE::Cst::DocResolveNodeId( *doc, id ) : RISE::Cst::NodeRef();
			has33 = chunk && RISE::Cst::SerializeNode( chunk ).find( "fov 33" ) != std::string::npos;
		}
		Check( has33, "T23: the sole camera chunk now carries fov 33" );
		pJob7->release();
		std::remove( tmp7.c_str() );
	}

	// ---------- T35-T38: Material Editor Slice 1 + Slice 2 ----------
	std::printf( "T35-T38: Material Editor inspector upgrades, pipe-filtered presets, inline/composed slot rebinds, and soft ranges...\n" );
	{
		static const char* kMatEditorScene =
			"RISE ASCII SCENE 7\n"
			"\n"
			"film\n"
			"{\n"
			"\twidth\t\t24\n"
			"\theight\t\t24\n"
			"}\n"
			"\n"
			"pinhole_camera\n"
			"{\n"
			"\tlocation\t0 0 5\n"
			"\tlookat\t\t0 0 0\n"
			"\tup\t\t0 1 0\n"
			"\tfov\t\t45\n"
			"}\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tpnt_white\n"
			"\tcolor\t\t0.8 0.8 0.8\n"
			"}\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tpnt_blue\n"
			"\tcolor\t\t0.2 0.4 0.9\n"
			"}\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tpnt_emit\n"
			"\tcolor\t\t2.0 2.0 2.0\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tsp_rough\n"
			"\tvalue\t\t0.25\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tsp_ior\n"
			"\tvalue\t\t1.52\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tsp_rgb\n"
			"\tvalues\t\t1.2 1.5 1.8\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tsp_expr_rgb\n"
			"\texpression\tvec3(0.2, 0.5, 0.8)\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tsp_sellmeier\n"
			"\tsellmeier\t1.03961212 0.00600069867 0.231792344 0.0200179144 1.01046945 103.560653\n"
			"}\n"
			"\n"
			"perlin3d_painter\n"
			"{\n"
			"\tname\t\tpnt_perlin\n"
			"\tcolora\t\tpnt_white\n"
			"\tcolorb\t\tpnt_blue\n"
			"\tscale\t\t2.0 2.0 2.0\n"
			"\tpersistence\t0.5\n"
			"\toctaves\t\t4\n"
			"}\n"
			"\n"
			"blend_painter\n"
			"{\n"
			"\tname\t\tbp_leaf\n"
			"\tcolora\t\tpnt_white\n"
			"\tcolorb\t\tpnt_blue\n"
			"\tmask\t\tpnt_white\n"
			"}\n"
			"\n"
			"blend_painter\n"
			"{\n"
			"\tname\t\tbp_root\n"
			"\tcolora\t\tbp_leaf\n"
			"\tcolorb\t\tpnt_blue\n"
			"\tmask\t\tpnt_white\n"
			"}\n"
			"\n"
			"lambertian_material\n"
			"{\n"
			"\tname\t\tmat_lamb\n"
			"\treflectance\tpnt_white\n"
			"}\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tpnt_after_lamb\n"
			"\tcolor\t\t0.9 0.1 0.1\n"
			"}\n"
			"\n"
			"dielectric_material\n"
			"{\n"
			"\tname\t\tmat_glass\n"
			"\ttau\t\t0.9\n"
			"\tior\t\t1.5\n"
			"\tscattering\t100.0\n"
			"}\n"
			"\n"
			"ggx_material\n"
			"{\n"
			"\tname\t\tmat_ggx_defaults\n"
			"\trs\t\tpnt_blue\n"
			"}\n"
			"\n"
			"ggx_material\n"
			"{\n"
			"\tname\t\tmat_ggx_lit\n"
			"\trd\t\tpnt_white\n"
			"\trs\t\tpnt_blue\n"
			"\talphax\t\t0.15\n"
			"\talphay\t\tsp_rough\n"
			"\tior\t\t1.8\n"
			"\textinction\t0.0\n"
			"\tfresnel_mode\tconductor\n"
			"\temissive\tpnt_emit\n"
			"\temissive_scale\t2.5\n"
			"}\n"
			"\n"
			"coated_material\n"
			"{\n"
			"\tname\t\tmat_coated\n"
			"\tbase\t\tmat_lamb\n"
			"\tcoat_roughness\t0.1\n"
			"\tcoat_ior\t1.5\n"
			"\tcoat_thickness\t2.5\n"
			"\tcoat_absorption\t0.2\n"
			"\tcoat_weight\t0.8\n"
			"}\n"
			"\n"
			"weave_material\n"
			"{\n"
			"\tname\t\tmat_weave\n"
			"\tfabric\t\tsilk\n"
			"\twarp_color\tpnt_white\n"
			"\tweft_color\tpnt_blue\n"
			"\tweave_scale\t60\n"
			"}\n";

		const std::string tmp8 = TempPath( "geometry_panel_jump_mateditor.RISEscene" );
		Job* pJob8 = LoadScene( kMatEditorScene, tmp8 );
		Check( pJob8 != nullptr, "T25: Material Editor fixture loads" );
		if( pJob8 )
		{
			SceneEditController ctrl8( *pJob8, nullptr );

			auto hasPreset = [&]( Category cat, int rowIdx, const std::string& val ) -> bool
			{
				if( rowIdx < 0 ) return false;
				const unsigned int n = ctrl8.PropertyPresetCountFor( cat, static_cast<unsigned int>( rowIdx ) );
				for( unsigned int i = 0; i < n; ++i )
				{
					if( std::string( ctrl8.PropertyPresetValueFor( cat, static_cast<unsigned int>( rowIdx ), i ).c_str() ) == val )
						return true;
				}
				return false;
			};

			// T25: ggx_material.fresnel_mode upgraded to ValueKind::Enum with presets & round-trip edit
			ctrl8.SetSelection( Category::Material, String( "mat_ggx_lit" ) );
			ctrl8.RefreshProperties();
			const int fmRow = RowIndexFor( ctrl8, Category::Material, "fresnel_mode" );
			Check( fmRow >= 0, "T25: ggx_material exposes fresnel_mode row" );
			if( fmRow >= 0 )
			{
				const unsigned int uFm = static_cast<unsigned int>( fmRow );
				Check( ctrl8.PropertyEditableFor( Category::Material, uFm ),
					"MONEY (T25): ggx_material.fresnel_mode upgraded from read-only live row to editable CST row" );
				Check( ctrl8.PropertyKindFor( Category::Material, uFm ) == static_cast<int>( ValueKind::Enum ),
					"MONEY (T25): ggx_material.fresnel_mode upgraded from ValueKind::String to ValueKind::Enum" );
				Check( hasPreset( Category::Material, fmRow, "conductor" )
					&& hasPreset( Category::Material, fmRow, "schlick_f0" )
					&& hasPreset( Category::Material, fmRow, "thinfilm" ),
					"MONEY (T25): ggx_material.fresnel_mode carries enum presets conductor / schlick_f0 / thinfilm" );

				Check( ctrl8.SetProperty( String( "fresnel_mode" ), String( "schlick_f0" ) ),
					"T25: SetProperty(fresnel_mode, schlick_f0) succeeds" );
				ctrl8.RefreshProperties();
				Check( std::string( ctrl8.PropertyValueFor( Category::Material, uFm ).c_str() ) == "schlick_f0",
					"T25: fresnel_mode value reflects schlick_f0 after edit" );
				ctrl8.Undo();
				ctrl8.RefreshProperties();
				Check( std::string( ctrl8.PropertyValueFor( Category::Material, uFm ).c_str() ) == "conductor",
					"T25: fresnel_mode value restored to conductor after Undo" );
			}

			// T26: Pipe-filtered and cycle-filtered Reference presets
			// 26a: Color-only slot (lambertian_material.reflectance) offers colour painters, excludes scalar painters
			ctrl8.SetSelection( Category::Material, String( "mat_lamb" ) );
			ctrl8.RefreshProperties();
			const int rdRow = RowIndexFor( ctrl8, Category::Material, "reflectance" );
			Check( rdRow >= 0, "T26a: lambertian_material.reflectance row exists" );
			if( rdRow >= 0 )
			{
				Check( hasPreset( Category::Material, rdRow, "pnt_white" )
					&& hasPreset( Category::Material, rdRow, "pnt_blue" ),
					"T26a: Color slot `reflectance` includes colour painters pnt_white and pnt_blue" );
				Check( !hasPreset( Category::Material, rdRow, "sp_rough" )
					&& !hasPreset( Category::Material, rdRow, "sp_rgb" ),
					"MONEY (T26a): Color slot `reflectance` excludes scalar painters sp_rough and sp_rgb" );
				Check( !hasPreset( Category::Material, rdRow, "pnt_after_lamb" ),
					"MONEY (T26a): Color slot `mat_lamb.reflectance` excludes forward-declared painter `pnt_after_lamb`" );
			}

			// 26b: Scalar slot (ggx_material.alphay allows per-channel scalar painters;
			// coated_material.coat_roughness and ggx_material.tangent_rotation_scalar have requireSingle=true)
			ctrl8.SetSelection( Category::Material, String( "mat_ggx_lit" ) );
			ctrl8.RefreshProperties();
			const int ayRow = RowIndexFor( ctrl8, Category::Material, "alphay" );
			Check( ayRow >= 0, "T26b: ggx_material.alphay row exists" );
			if( ayRow >= 0 )
			{
				Check( hasPreset( Category::Material, ayRow, "sp_rough" )
					&& hasPreset( Category::Material, ayRow, "sp_rgb" ),
					"T26b: non-requireSingle Scalar slot `alphay` includes both 1-value and 3-value scalar painters" );
				Check( !hasPreset( Category::Material, ayRow, "pnt_white" ),
					"MONEY (T26b): Scalar slot `alphay` excludes colour painter pnt_white" );
				// Also verify that attempting to set a CST colour painter on a Scalar
				// slot is rejected by the derive path (not misclassified as a runtime-only
				// dangling reference) and preserves the existing value.
				Check( !ctrl8.SetProperty( "alphay", "pnt_white" ),
					"T26b: setting colour painter `pnt_white` on Scalar slot `alphay` is rejected" );
			}
			const int trsRow = RowIndexFor( ctrl8, Category::Material, "tangent_rotation_scalar" );
			Check( trsRow >= 0, "T26b: ggx_material.tangent_rotation_scalar row exists" );
			if( trsRow >= 0 )
			{
				Check( hasPreset( Category::Material, trsRow, "sp_rough" )
					&& hasPreset( Category::Material, trsRow, "sp_ior" ),
					"T26b: requireSingle Scalar slot `tangent_rotation_scalar` includes 1-value scalar painters" );
				Check( !hasPreset( Category::Material, trsRow, "sp_rgb" ),
					"MONEY (T26b): requireSingle Scalar slot `tangent_rotation_scalar` excludes 3-value `values` scalar painter sp_rgb" );
				Check( !hasPreset( Category::Material, trsRow, "sp_expr_rgb" ),
					"MONEY (T26b): requireSingle Scalar slot `tangent_rotation_scalar` excludes per-channel vec3 expression scalar painter sp_expr_rgb" );
				Check( !hasPreset( Category::Material, trsRow, "pnt_white" ),
					"MONEY (T26b): Scalar slot `tangent_rotation_scalar` excludes colour painter pnt_white" );
			}

			// 26c: colorAlsoAcceptsScalar slot (ggx_material.tangent_rotation) offers both colour and 1-value scalar painters,
			// while perlin3d_painter.colora (Color pipe) offers colour painters and excludes scalar painters + self
			const int trRow = RowIndexFor( ctrl8, Category::Material, "tangent_rotation" );
			Check( trRow >= 0, "T26c: ggx_material.tangent_rotation row exists" );
			if( trRow >= 0 )
			{
				Check( hasPreset( Category::Material, trRow, "pnt_white" )
					&& hasPreset( Category::Material, trRow, "sp_rough" ),
					"MONEY (T26c): colorAlsoAcceptsScalar slot `ggx_material.tangent_rotation` includes both colour and 1-value scalar painters" );
				Check( !hasPreset( Category::Material, trRow, "sp_rgb" )
					&& !hasPreset( Category::Material, trRow, "sp_expr_rgb" )
					&& !hasPreset( Category::Material, trRow, "sp_sellmeier" ),
					"MONEY (T26c): `ggx_material.tangent_rotation` (ResolveRotationPainterDual) excludes per-channel and spectral-curve scalar painters" );
			}
			ctrl8.SetSelection( Category::Painter, String( "pnt_perlin" ) );
			ctrl8.RefreshProperties();
			const int caRow = RowIndexFor( ctrl8, Category::Painter, "colora" );
			Check( caRow >= 0, "T26c: perlin3d_painter.colora row exists" );
			if( caRow >= 0 )
			{
				Check( hasPreset( Category::Painter, caRow, "pnt_white" )
					&& !hasPreset( Category::Painter, caRow, "sp_rough" ),
					"MONEY (T26c): Color-pipe `perlin3d_painter.colora` includes colour painters and excludes scalar painters" );
				Check( !hasPreset( Category::Painter, caRow, "pnt_perlin" ),
					"MONEY (T26c): `perlin3d_painter.colora` excludes self-reference pnt_perlin" );
			}

			// 26d: Cycle exclusion on painter reference chain (bp_root -> bp_leaf)
			ctrl8.SetSelection( Category::Painter, String( "bp_leaf" ) );
			ctrl8.RefreshProperties();
			const int leafCaRow = RowIndexFor( ctrl8, Category::Painter, "colora" );
			Check( leafCaRow >= 0, "T26d: bp_leaf.colora row exists" );
			if( leafCaRow >= 0 )
			{
				Check( !hasPreset( Category::Painter, leafCaRow, "bp_leaf" ),
					"MONEY (T26d): bp_leaf.colora excludes self-reference bp_leaf" );
				Check( !hasPreset( Category::Painter, leafCaRow, "bp_root" ),
					"MONEY (T26d): bp_leaf.colora excludes ancestor bp_root (would form cycle bp_root -> bp_leaf -> bp_root)" );
				Check( hasPreset( Category::Painter, leafCaRow, "pnt_white" ),
					"T26d: bp_leaf.colora still includes non-cyclic pnt_white" );
			}

			// 26e: Material base slot on coated_material excludes self and "none"
			ctrl8.SetSelection( Category::Material, String( "mat_coated" ) );
			ctrl8.RefreshProperties();
			const int subRow = RowIndexFor( ctrl8, Category::Material, "base" );
			Check( subRow >= 0, "T26e: coated_material.base row exists" );
			if( subRow >= 0 )
			{
				Check( hasPreset( Category::Material, subRow, "mat_lamb" ),
					"T26e: coated_material.base includes lambertian substrate mat_lamb" );
				Check( !hasPreset( Category::Material, subRow, "mat_coated" ),
					"MONEY (T26e): coated_material.base excludes self-reference mat_coated" );
				Check( !hasPreset( Category::Material, subRow, "none" ),
					"MONEY (T26e): required Material slot coated_material.base excludes built-in `none`" );
			}
			const int coatIorRow = RowIndexFor( ctrl8, Category::Material, "coat_ior" );
			Check( coatIorRow >= 0, "T26e: coated_material.coat_ior row exists" );
			if( coatIorRow >= 0 )
			{
				Check( hasPreset( Category::Material, coatIorRow, "sp_sellmeier" ),
					"MONEY (T26e): DL-29 spectral-curve scalar_painter `sp_sellmeier` is accepted on requireSingle Scalar slot `coated_material.coat_ior`" );
				Check( !hasPreset( Category::Material, coatIorRow, "sp_rgb" )
					&& !hasPreset( Category::Material, coatIorRow, "sp_expr_rgb" ),
					"MONEY (T26e): 3-value `values` and `vec3(...)` expression scalar painters remain excluded on `coated_material.coat_ior`" );
			}

			// T27: Inline-literal-to-painter-name rebind & composed-material slot rebind
			// 27a: dielectric_material.ior authored as inline scalar `1.5`, rebound to named `sp_ior`, then undone
			ctrl8.SetSelection( Category::Material, String( "mat_glass" ) );
			ctrl8.RefreshProperties();
			int glassIorRow = RowIndexFor( ctrl8, Category::Material, "ior" );
			Check( glassIorRow >= 0, "T27a: dielectric_material.ior row exists" );
			if( glassIorRow >= 0 )
			{
				const unsigned int uIor = static_cast<unsigned int>( glassIorRow );
				Check( std::string( ctrl8.PropertyValueFor( Category::Material, uIor ).c_str() ) == "1.5",
					"T27a: dielectric_material.ior starts as inline literal 1.5" );
				Check( ctrl8.PropertyHasRangeFor( Category::Material, uIor ),
					"MONEY (T27a/T28): inline numeric dielectric_material.ior carries soft range" );
				Check( hasPreset( Category::Material, glassIorRow, "sp_ior" ),
					"MONEY (T27a): upgraded dielectric_material.ior carries scalar-painter presets including sp_ior" );

				Check( ctrl8.SetProperty( String( "ior" ), String( "sp_ior" ) ),
					"MONEY (T27a): rebinding inline literal dielectric_material.ior -> sp_ior succeeds via CST edit path" );
				ctrl8.RefreshProperties();
				glassIorRow = RowIndexFor( ctrl8, Category::Material, "ior" );
				Check( glassIorRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ).c_str() ) == "sp_ior",
					"T27a: dielectric_material.ior now reads sp_ior" );
				Check( !ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ),
					"MONEY (T28): dielectric_material.ior suppresses slider soft range when bound to named painter sp_ior" );

				ctrl8.Undo();
				ctrl8.RefreshProperties();
				glassIorRow = RowIndexFor( ctrl8, Category::Material, "ior" );
				Check( glassIorRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ).c_str() ) == "1.5",
					"MONEY (T27a): Undo restores inline literal 1.5 on dielectric_material.ior" );
				Check( ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ),
					"T27a/T28: soft range restored when Undo brings dielectric_material.ior back to 1.5" );

				// Inline numeric -> inline numeric slider edit round-trip
				Check( ctrl8.SetProperty( String( "ior" ), String( "1.75" ) ),
					"MONEY (T27a): numeric slider edit dielectric_material.ior 1.5 -> 1.75 succeeds" );
				ctrl8.RefreshProperties();
				glassIorRow = RowIndexFor( ctrl8, Category::Material, "ior" );
				Check( glassIorRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ).c_str() ) == "1.75"
					&& ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ),
					"T27a: dielectric_material.ior reads 1.75 and keeps its soft range" );
				ctrl8.Undo();
				ctrl8.RefreshProperties();
				glassIorRow = RowIndexFor( ctrl8, Category::Material, "ior" );
				Check( glassIorRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( glassIorRow ) ).c_str() ) == "1.5",
					"T27a: Undo restores dielectric_material.ior to 1.5 after numeric edit" );
			}

			// 27c: omitted parameter on ggx_material (`mat_ggx_defaults` omits `rd` and `alphax`):
			// - `alphax` gets defaultValueHint "0.15" and soft range enabled
			// - editing omitted `rd` to `pnt_blue` and pressing Undo removes `rd` from the CST chunk cleanly
			ctrl8.SetSelection( Category::Material, String( "mat_ggx_defaults" ) );
			ctrl8.RefreshProperties();
			const int defAxRow = RowIndexFor( ctrl8, Category::Material, "alphax" );
			Check( defAxRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( defAxRow ) ),
				"MONEY (T27c/T28): omitted ggx_material.alphax gets defaultValueHint 0.15 and soft range enabled" );
			Check( ctrl8.SetProperty( String( "rd" ), String( "pnt_blue" ) ),
				"T27c: setting omitted ggx_material.rd -> pnt_blue succeeds" );
			Check( CstValueOf( pJob8, "mat_ggx_defaults", "material", "rd" ) == "pnt_blue",
				"T27c: CST chunk mat_ggx_defaults now has rd pnt_blue" );
			ctrl8.Undo();
			Check( CstValueOf( pJob8, "mat_ggx_defaults", "material", "rd" ).empty(),
				"MONEY (T27c): Undo on previously-omitted ggx_material.rd removes the parameter line from CST instead of leaving `rd none`" );

			// 27b: composed material (mat_ggx_lit has emissive -> CompositeMaterial wrapper) slot rebind
			ctrl8.SetSelection( Category::Material, String( "mat_ggx_lit" ) );
			ctrl8.RefreshProperties();
			int ggxRdRow = RowIndexFor( ctrl8, Category::Material, "rd" );
			Check( ggxRdRow >= 0, "T27b: composed ggx_material.rd row exists" );
			if( ggxRdRow >= 0 )
			{
				Check( ctrl8.PropertyEditableFor( Category::Material, static_cast<unsigned int>( ggxRdRow ) ),
					"MONEY (T27b): composed ggx_material.rd upgraded to editable via AugmentWithCstRows" );
				Check( ctrl8.SetProperty( String( "rd" ), String( "pnt_blue" ) ),
					"MONEY (T27b): SetProperty on composed ggx_material.rd routes through CST edit and succeeds" );
				ctrl8.RefreshProperties();
				ggxRdRow = RowIndexFor( ctrl8, Category::Material, "rd" );
				Check( ggxRdRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( ggxRdRow ) ).c_str() ) == "pnt_blue",
					"T27b: composed ggx_material.rd now reads pnt_blue" );
				ctrl8.Undo();
				ctrl8.RefreshProperties();
				ggxRdRow = RowIndexFor( ctrl8, Category::Material, "rd" );
				Check( ggxRdRow >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( ggxRdRow ) ).c_str() ) == "pnt_white",
					"T27b: Undo restores pnt_white on composed ggx_material.rd" );
			}

			// T28: Soft ranges on material and painter rows
			ctrl8.SetSelection( Category::Material, String( "mat_coated" ) );
			ctrl8.RefreshProperties();
			const int thickRow = RowIndexFor( ctrl8, Category::Material, "coat_thickness" );
			const int weightRow = RowIndexFor( ctrl8, Category::Material, "coat_weight" );
			Check( thickRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( thickRow ) ),
				"MONEY (T28): coated_material.coat_thickness (literal 2.5) has soft range enabled" );
			if( thickRow >= 0 )
			{
				const unsigned int uThick = static_cast<unsigned int>( thickRow );
				Check( std::fabs( ctrl8.PropertyRangeMinFor( Category::Material, uThick ) - 0.0 ) < 1e-6
					&& std::fabs( ctrl8.PropertyRangeMaxFor( Category::Material, uThick ) - 10.0 ) < 1e-6,
					"T28: coated_material.coat_thickness range is [0, 10]" );
			}
			Check( weightRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( weightRow ) ),
				"MONEY (T28): coated_material.coat_weight (literal 0.8) has soft range [0, 1] enabled" );

			ctrl8.SetSelection( Category::Painter, String( "pnt_perlin" ) );
			ctrl8.RefreshProperties();
			const int persRow = RowIndexFor( ctrl8, Category::Painter, "persistence" );
			const int octRow  = RowIndexFor( ctrl8, Category::Painter, "octaves" );
			Check( persRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Painter, static_cast<unsigned int>( persRow ) ),
				"MONEY (T28): perlin3d_painter.persistence has soft range [0, 1] enabled" );
			Check( octRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Painter, static_cast<unsigned int>( octRow ) )
				&& std::fabs( ctrl8.PropertyRangeMinFor( Category::Painter, static_cast<unsigned int>( octRow ) ) - 1.0 ) < 1e-6
				&& std::fabs( ctrl8.PropertyRangeMaxFor( Category::Painter, static_cast<unsigned int>( octRow ) ) - 16.0 ) < 1e-6
				&& std::fabs( ctrl8.PropertyRangeStepFor( Category::Painter, static_cast<unsigned int>( octRow ) ) - 1.0 ) < 1e-6,
				"MONEY (T28): perlin3d_painter.octaves has integer soft range [1, 16] step 1" );

			// T28b (External-review P2 regression): weave_material `mat_weave`
			// omits `sheer` (CST-only row with defaultValueHint `"per \`fabric\` (custom: 0)"`)
			// and `weave_scale` (live row with runtime value from preset + soft range).
			// The CST-only `sheer` row must keep its honest hint and NOT activate a
			// misleading `0` slider, while live `weave_scale` has a numeric runtime
			// value and activates its soft range.
			ctrl8.SetSelection( Category::Material, String( "mat_weave" ) );
			ctrl8.RefreshProperties();
			const int sheerRow = RowIndexFor( ctrl8, Category::Material, "sheer" );
			const int weaveScaleRow = RowIndexFor( ctrl8, Category::Material, "weave_scale" );
			Check( sheerRow >= 0, "T28b: weave_material.sheer row exists" );
			if( sheerRow >= 0 )
			{
				const unsigned int uSheer = static_cast<unsigned int>( sheerRow );
				const std::string sheerVal( ctrl8.PropertyValueFor( Category::Material, uSheer ).c_str() );
				Check( sheerVal.find( "per `fabric`" ) != std::string::npos
					&& !ctrl8.PropertyHasRangeFor( Category::Material, uSheer ),
					"MONEY (T28b): omitted CST-only weave_material.sheer keeps honest `per \\`fabric\\`` hint and hasRange == false" );
			}
			Check( weaveScaleRow >= 0 && ctrl8.PropertyHasRangeFor( Category::Material, static_cast<unsigned int>( weaveScaleRow ) ),
				"MONEY (T28b): live weave_material.weave_scale row with numeric runtime value activates soft range" );

			// T29: Inline Painter Swatch + Constant Painter Quick Editing + Shared/Fork
			// 29a: TargetsPainter & InlinePainter metadata on material and painter slots
			ctrl8.SetSelection( Category::Material, String( "mat_coated" ) );
			ctrl8.RefreshProperties();
			const int coatedBaseRow = RowIndexFor( ctrl8, Category::Material, "base" );
			Check( coatedBaseRow >= 0
				&& !RISE_API_SceneEditController_PropertyTargetsPainterFor( &ctrl8, static_cast<int>( Category::Material ), static_cast<unsigned int>( coatedBaseRow ) )
				&& RISE_API_SceneEditController_PropertyInlinePainterKindFor( &ctrl8, static_cast<int>( Category::Material ), static_cast<unsigned int>( coatedBaseRow ) ) == 0,
				"MONEY (T29a): coated_material.base targets Material (targetsPainter == false, inlinePainterKind == 0)" );

			ctrl8.SetSelection( Category::Material, String( "mat_ggx_lit" ) );
			ctrl8.RefreshProperties();
			const int litRdRow = RowIndexFor( ctrl8, Category::Material, "rd" );
			const int litAyRow = RowIndexFor( ctrl8, Category::Material, "alphay" );
			Check( litRdRow >= 0 && litAyRow >= 0, "T29a: mat_ggx_lit.rd and alphay rows exist" );
			if( litRdRow >= 0 && litAyRow >= 0 )
			{
				const unsigned int uRd = static_cast<unsigned int>( litRdRow );
				const unsigned int uAy = static_cast<unsigned int>( litAyRow );
				Check( RISE_API_SceneEditController_PropertyTargetsPainterFor( &ctrl8, static_cast<int>( Category::Material ), uRd )
					&& RISE_API_SceneEditController_PropertyInlinePainterKindFor( &ctrl8, static_cast<int>( Category::Material ), uRd ) == 1,
					"MONEY (T29a): mat_ggx_lit.rd -> pnt_white reports targetsPainter == true and inlinePainterKind == 1 (UniformColor)" );
				char valBuf[128] = {};
				Check( RISE_API_SceneEditController_PropertyInlinePainterValueFor( &ctrl8, static_cast<int>( Category::Material ), uRd, valBuf, sizeof( valBuf ) )
					&& std::string( valBuf ) == "0.8 0.8 0.8",
					"T29a: mat_ggx_lit.rd inlinePainterValue reads '0.8 0.8 0.8'" );
				// pnt_white is referenced by pnt_perlin.colora, bp_leaf.colora, bp_leaf.mask,
				// bp_root.mask, mat_lamb.reflectance, mat_ggx_lit.rd, mat_weave.warp_color -> 7 slots
				Check( RISE_API_SceneEditController_PropertyInlinePainterRefCountFor( &ctrl8, static_cast<int>( Category::Material ), uRd ) == 7,
					"MONEY (T29a): shared pnt_white reports inlinePainterRefCount == 7" );

				Check( RISE_API_SceneEditController_PropertyTargetsPainterFor( &ctrl8, static_cast<int>( Category::Material ), uAy )
					&& RISE_API_SceneEditController_PropertyInlinePainterKindFor( &ctrl8, static_cast<int>( Category::Material ), uAy ) == 2,
					"MONEY (T29a): mat_ggx_lit.alphay -> sp_rough reports targetsPainter == true and inlinePainterKind == 2 (ScalarValue)" );
				Check( RISE_API_SceneEditController_PropertyInlinePainterValueFor( &ctrl8, static_cast<int>( Category::Material ), uAy, valBuf, sizeof( valBuf ) )
					&& std::string( valBuf ) == "0.25",
					"T29a: mat_ggx_lit.alphay inlinePainterValue reads '0.25'" );
				Check( RISE_API_SceneEditController_PropertyInlinePainterRefCountFor( &ctrl8, static_cast<int>( Category::Material ), uAy ) == 1,
					"MONEY (T29a): solely-owned sp_rough reports inlinePainterRefCount == 1" );
				// Verify soft-range bounds are preserved on the Reference row for the inline scalar slider
				Check( !ctrl8.PropertyHasRangeFor( Category::Material, uAy )
					&& std::fabs( ctrl8.PropertyRangeMinFor( Category::Material, uAy ) - 0.001 ) < 1e-6
					&& std::fabs( ctrl8.PropertyRangeMaxFor( Category::Material, uAy ) - 1.0 ) < 1e-6,
					"MONEY (T29a): Reference row mat_ggx_lit.alphay keeps hasRange == false while carrying soft-range bounds [0.001, 1.0] for the inline scalar slider" );
			}

			// 29b: SetReferenceSlotPainterValue on scalar_painter { value } + Undo/Redo
			Check( RISE_API_SceneEditController_SetReferenceSlotPainterValue(
				&ctrl8, static_cast<int>( Category::Material ), "alphay", "0.42" ),
				"MONEY (T29b): SetReferenceSlotPainterValue(Material, alphay, 0.42) succeeds" );
			Check( CstValueOf( pJob8, "sp_rough", "painter", "value" ) == "0.42",
				"T29b: CST chunk sp_rough.value updated to 0.42" );
			ctrl8.RefreshProperties();
			{
				const int ayIdx = RowIndexFor( ctrl8, Category::Material, "alphay" );
				Check( ayIdx >= 0
					&& std::string( ctrl8.PropertyInlinePainterValueFor( Category::Material, static_cast<unsigned int>( ayIdx ) ).c_str() ) == "0.42",
					"T29b: refreshed inlinePainterValue on alphay reads 0.42" );
			}
			ctrl8.Undo();
			Check( CstValueOf( pJob8, "sp_rough", "painter", "value" ) == "0.25",
				"MONEY (T29b): Undo restores sp_rough.value to 0.25" );
			ctrl8.Redo();
			Check( CstValueOf( pJob8, "sp_rough", "painter", "value" ) == "0.42",
				"T29b: Redo re-applies sp_rough.value 0.42" );
			ctrl8.Undo();

			// 29c: Non-constant painters (perlin3d_painter, 3-value scalar_painter { values }) report inlinePainterKind == 0
			Check( ctrl8.SetProperty( String( "rd" ), String( "pnt_perlin" ) ),
				"T29c: rebind mat_ggx_lit.rd -> pnt_perlin" );
			Check( ctrl8.SetProperty( String( "alphay" ), String( "sp_rgb" ) ),
				"T29c: rebind mat_ggx_lit.alphay -> sp_rgb" );
			ctrl8.RefreshProperties();
			{
				const int rdIdx = RowIndexFor( ctrl8, Category::Material, "rd" );
				const int ayIdx = RowIndexFor( ctrl8, Category::Material, "alphay" );
				Check( rdIdx >= 0
					&& ctrl8.PropertyTargetsPainterFor( Category::Material, static_cast<unsigned int>( rdIdx ) )
					&& ctrl8.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( rdIdx ) ) == 0,
					"MONEY (T29c): procedural pnt_perlin has targetsPainter == true but inlinePainterKind == 0" );
				Check( ayIdx >= 0
					&& ctrl8.PropertyTargetsPainterFor( Category::Material, static_cast<unsigned int>( ayIdx ) )
					&& ctrl8.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( ayIdx ) ) == 0,
					"MONEY (T29c): 3-value scalar_painter `sp_rgb` (values, not value) has inlinePainterKind == 0" );
				Check( !ctrl8.SetReferenceSlotPainterValue( Category::Material, String( "rd" ), String( "0.1 0.2 0.3" ) ),
					"T29c: SetReferenceSlotPainterValue refuses non-constant painter pnt_perlin" );
			}
			ctrl8.Undo();
			ctrl8.Undo();

			// 29d: ForkReferenceSlotPainter on shared uniformcolor_painter (`pnt_white`, refCount == 7)
			ctrl8.SetSelection( Category::Material, String( "mat_lamb" ) );
			ctrl8.RefreshProperties();
			{
				const int reflIdx = RowIndexFor( ctrl8, Category::Material, "reflectance" );
				Check( reflIdx >= 0
					&& ctrl8.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( reflIdx ) ) == 1
					&& ctrl8.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( reflIdx ) ) == 7,
					"T29d: mat_lamb.reflectance starts bound to shared pnt_white (refCount == 7)" );
			}
			char forkedNameBuf[128] = {};
			Check( RISE_API_SceneEditController_ForkReferenceSlotPainter(
				&ctrl8, static_cast<int>( Category::Material ), "reflectance", forkedNameBuf, sizeof( forkedNameBuf ) ),
				"MONEY (T29d): ForkReferenceSlotPainter on mat_lamb.reflectance succeeds" );
			Check( std::string( forkedNameBuf ) == "pnt_white_2",
				"MONEY (T29d): forked painter is named pnt_white_2" );
			Check( CstValueOf( pJob8, "mat_lamb", "material", "reflectance" ) == "pnt_white_2",
				"T29d: mat_lamb.reflectance CST parameter rebound to pnt_white_2" );
			ctrl8.RefreshProperties();
			{
				const int reflIdx = RowIndexFor( ctrl8, Category::Material, "reflectance" );
				Check( reflIdx >= 0
					&& std::string( ctrl8.PropertyValueFor( Category::Material, static_cast<unsigned int>( reflIdx ) ).c_str() ) == "pnt_white_2"
					&& ctrl8.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( reflIdx ) ) == 1
					&& ctrl8.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( reflIdx ) ) == 1,
					"MONEY (T29d): after Fork, mat_lamb.reflectance is bound to pnt_white_2 with refCount == 1" );
			}
			// Edit the forked painter via SetReferenceSlotPainterValue and verify pnt_white is untouched
			Check( ctrl8.SetReferenceSlotPainterValue( Category::Material, String( "reflectance" ), String( "0.1 0.9 0.2" ) ),
				"MONEY (T29d): SetReferenceSlotPainterValue on forked mat_lamb.reflectance succeeds" );
			Check( CstValueOf( pJob8, "pnt_white_2", "painter", "color" ) == "0.1 0.9 0.2"
				&& CstValueOf( pJob8, "pnt_white", "painter", "color" ) == "0.8 0.8 0.8",
				"MONEY (T29d): editing forked painter updates pnt_white_2 while leaving shared pnt_white untouched" );

			// Undo the colour edit, then change Film width (24 -> 48) before undoing Fork:
			// Fork undo must be operation-level (removing only pnt_white_2 and restoring
			// mat_lamb.reflectance -> pnt_white) without reverting the unrelated film edit.
			ctrl8.Undo();
			Check( CstValueOf( pJob8, "pnt_white_2", "painter", "color" ) == "0.8 0.8 0.8",
				"T29d: first Undo restores pnt_white_2 colour to 0.8 0.8 0.8" );
			Check( ctrl8.SetPropertyForCategory( Category::Film, String( "width" ), String( "48" ) )
				&& pJob8->GetScene() && pJob8->GetScene()->GetFilm()
				&& pJob8->GetScene()->GetFilm()->GetWidth() == 48,
				"T29d: changing film width from 24 to 48 after Fork succeeds" );
			ctrl8.Undo();
			Check( CstValueOf( pJob8, "mat_lamb", "material", "reflectance" ) == "pnt_white"
				&& CstValueOf( pJob8, "pnt_white_2", "painter", "color" ) == "<missing>",
				"MONEY (T29d): single Undo of ForkReferenceSlotPainter atomically restores mat_lamb.reflectance -> pnt_white and removes pnt_white_2" );
			Check( pJob8->GetScene() && pJob8->GetScene()->GetFilm()
				&& pJob8->GetScene()->GetFilm()->GetWidth() == 48
				&& pJob8->GetCstDocument()
				&& RISE::Cst::SerializeCst( *pJob8->GetCstDocument() ).find( "width\t\t48" ) != std::string::npos,
				"MONEY (T29d): Undo of ForkReferenceSlotPainter preserves unrelated film width 48 in both live scene and retained CST" );
			ctrl8.Redo();
			Check( CstValueOf( pJob8, "mat_lamb", "material", "reflectance" ) == "pnt_white_2"
				&& CstValueOf( pJob8, "pnt_white_2", "painter", "color" ) == "0.8 0.8 0.8"
				&& pJob8->GetScene() && pJob8->GetScene()->GetFilm()
				&& pJob8->GetScene()->GetFilm()->GetWidth() == 48,
				"MONEY (T29d): Redo of ForkReferenceSlotPainter re-inserts pnt_white_2 and rebinds mat_lamb.reflectance while keeping film width 48" );
			ctrl8.Undo();
			ctrl8.RefreshProperties();
			{
				const int reflIdx = RowIndexFor( ctrl8, Category::Material, "reflectance" );
				Check( reflIdx >= 0
					&& ctrl8.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( reflIdx ) ) == 7,
					"T29d: after Undo of Fork, pnt_white refCount is back to 7" );
			}

			// 29e: Intra-chunk multi-slot sharing: bind mat_ggx_lit.alphax to sp_rough
			// (while mat_ggx_lit.alphay is already bound to sp_rough) -> both slots must
			// report inlinePainterRefCount == 2 so Shared (2) + Fork appear.
			ctrl8.SetSelection( Category::Material, String( "mat_ggx_lit" ) );
			Check( ctrl8.SetProperty( String( "alphax" ), String( "sp_rough" ) ),
				"T29e: rebind mat_ggx_lit.alphax -> sp_rough succeeds" );
			ctrl8.RefreshProperties();
			{
				const int axIdx = RowIndexFor( ctrl8, Category::Material, "alphax" );
				const int ayIdx = RowIndexFor( ctrl8, Category::Material, "alphay" );
				Check( axIdx >= 0 && ayIdx >= 0
					&& ctrl8.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( axIdx ) ) == 2
					&& ctrl8.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( ayIdx ) ) == 2,
					"MONEY (T29e): intra-chunk multi-slot sharing (alphax + alphay both bound to sp_rough) reports inlinePainterRefCount == 2" );
			}
			ctrl8.Undo();

			pJob8->release();
		}
		std::remove( tmp8.c_str() );

		// 29f: Same-named colour painter + scalar_painter disambiguation,
		// colorAlsoAcceptsScalar tie-breaking, and non-linear colorspace rejection.
		const char* kPipeScene =
			"RISE ASCII SCENE 7\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tshared_name\n"
			"\tcolor\t\t0.9 0.2 0.1\n"
			"}\n"
			"\n"
			"scalar_painter\n"
			"{\n"
			"\tname\t\tshared_name\n"
			"\tvalue\t\t0.35\n"
			"}\n"
			"\n"
			"uniformcolor_painter\n"
			"{\n"
			"\tname\t\tpnt_srgb\n"
			"\tcolor\t\t0.5 0.5 0.5\n"
			"\tcolorspace\tsRGB\n"
			"}\n"
			"\n"
			"ggx_material\n"
			"{\n"
			"\tname\t\tmat_pipe\n"
			"\trd\t\tshared_name\n"
			"\trs\t\tpnt_srgb\n"
			"\talphax\t\tshared_name\n"
			"\talphay\t\t0.2\n"
			"\tior\t\t1.5\n"
			"\textinction\t0.0\n"
			"\ttangent_rotation\tshared_name\n"
			"}\n";
		const std::string tmp9 = TempPath( "geometry_panel_jump_pipedisambig.RISEscene" );
		Job* pJob9 = LoadScene( kPipeScene, tmp9 );
		Check( pJob9 != nullptr, "T29f: pipe-disambiguation fixture loads" );
		if( pJob9 )
		{
			SceneEditController ctrl9( *pJob9, nullptr );
			ctrl9.SetSelection( Category::Material, String( "mat_pipe" ) );
			ctrl9.RefreshProperties();

			const int rdIdx = RowIndexFor( ctrl9, Category::Material, "rd" );
			const int rsIdx = RowIndexFor( ctrl9, Category::Material, "rs" );
			const int axIdx = RowIndexFor( ctrl9, Category::Material, "alphax" );
			const int trIdx = RowIndexFor( ctrl9, Category::Material, "tangent_rotation" );

			Check( rdIdx >= 0
				&& ctrl9.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( rdIdx ) ) == 1
				&& std::string( ctrl9.PropertyInlinePainterValueFor( Category::Material, static_cast<unsigned int>( rdIdx ) ).c_str() ) == "0.9 0.2 0.1"
				&& ctrl9.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( rdIdx ) ) == 1,
				"MONEY (T29f): colour-pipe slot mat_pipe.rd resolves same-named uniformcolor_painter with refCount == 1" );

			// alphax and tangent_rotation (colorAlsoAcceptsScalar prefers scalar_painter) both resolve to scalar_painter shared_name (refCount == 2)
			Check( axIdx >= 0
				&& ctrl9.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( axIdx ) ) == 2
				&& std::string( ctrl9.PropertyInlinePainterValueFor( Category::Material, static_cast<unsigned int>( axIdx ) ).c_str() ) == "0.35"
				&& ctrl9.PropertyInlinePainterRefCountFor( Category::Material, static_cast<unsigned int>( axIdx ) ) == 2,
				"MONEY (T29f): scalar-pipe slot mat_pipe.alphax resolves same-named scalar_painter with refCount == 2" );

			Check( trIdx >= 0
				&& ctrl9.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( trIdx ) ) == 2
				&& std::string( ctrl9.PropertyInlinePainterValueFor( Category::Material, static_cast<unsigned int>( trIdx ) ).c_str() ) == "0.35",
				"MONEY (T29f): colorAlsoAcceptsScalar slot mat_pipe.tangent_rotation prefers scalar_painter to match Job::ResolveRotationPainterDual" );

			Check( rsIdx >= 0
				&& ctrl9.PropertyTargetsPainterFor( Category::Material, static_cast<unsigned int>( rsIdx ) )
				&& ctrl9.PropertyInlinePainterKindFor( Category::Material, static_cast<unsigned int>( rsIdx ) ) == 0,
				"MONEY (T29f): uniformcolor_painter with non-linear `colorspace sRGB` suppresses inline quick colour editing" );

			Check( !ctrl9.SetReferenceSlotPainterValue( Category::Material, String( "rs" ), String( "0.2 0.3 0.4" ) ),
				"MONEY (T29f): SetReferenceSlotPainterValue rejects uniformcolor_painter with `colorspace sRGB`" );

			pJob9->release();
		}
		std::remove( tmp9.c_str() );
	}

	pJob->release();
	std::remove( tmp.c_str() );
	std::printf( "=== GeometryPanelJumpTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
