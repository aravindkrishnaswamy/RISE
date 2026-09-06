//////////////////////////////////////////////////////////////////////
//
//  EntityTemplatesTest.cpp - Entity-creation slice coverage:
//
//    T1  Every registered "Add Entity" template (Light / Object /
//        Material / Painter / Medium) instantiates successfully into a
//        minimal in-memory scene: InstantiateEntityTemplate reports
//        applied==true and the resulting entity name appears in
//        CategoryEntityName for that category.  This is the template-
//        validity gate -- every chunk text baked into EntityTemplates.cpp
//        must be grammar-valid v7 and Finalize-clean.
//    T2  Duplicate round-trip: duplicating a material produces a fresh
//        deduped name, the entity count for Material goes up by one, and
//        the CST Document's top-level item count goes up by exactly one
//        (a single-chunk material template).
//    T3  Remove-referenced refusal: removing a material a standard_object
//        still binds is rejected with a non-empty message and the entity
//        count is unchanged; removing an unreferenced light succeeds.
//    T4  Painter enumeration (CategoryEntityCount/CategoryEntityName see
//        a painter from the base scene) + a painter param edit via the
//        new path: both ApplyAgentParamEdit directly and the
//        SetSelection + SetProperty(Category::Painter) GUI-facing path
//        mutate the CST chunk's `color` parameter, verified by re-reading
//        it through CstIntrospection::Inspect (generic descriptor+CST rows).
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes, OIDN off,
//  no render pass is ever started (SceneEditController's cancel-and-park
//  is a no-op when mRendering is false, which it is for the lifetime of
//  every controller in this test).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/CstIntrospection.h"
#include "../src/Library/SceneEditor/ReferenceGraph.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Parsers/IAsciiChunkParser.h"

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

	// Minimal base scene: film + camera + rasterizer (so LoadAsciiSceneViaCst
	// derives cleanly) plus one named painter, material, and light so T3/T4
	// have known-good fixtures to duplicate/remove/edit without depending on
	// template instantiation having already run.
	const char* const kBaseScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 24\nheight 24\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_albedo\ncolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\nname mat_diffuse\nreflectance pnt_albedo\n}\n\n"
		"sphere_geometry\n{\nname sph\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_sph\ngeometry sph\nmaterial mat_diffuse\n}\n\n"
		"omni_light\n{\nname lgt_unused\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

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

	// Count only Chunk-kind top-level items -- DocItemCount counts chunks
	// AND inter-chunk trivia, and an agent insert appends [\n][chunk][\n]
	// (the separator is its own trivia item), so the raw item count can
	// grow by 2 per inserted chunk while the CHUNK count grows by
	// exactly 1.
	int ChunkCount( const RISE::Cst::Document& doc )
	{
		int n = 0;
		const int items = RISE::Cst::DocItemCount( doc );
		for( int i = 0; i < items; ++i )
		{
			const RISE::Cst::NodeRef it =
				RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
			if( it && it->kind == RISE::Cst::NodeKind::Chunk ) ++n;
		}
		return n;
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

	//------------------------------------------------------------------
	// T1: every template instantiates.
	//------------------------------------------------------------------
	void TestAllTemplatesInstantiate()
	{
		std::printf( "T1: every entity template instantiates into a minimal scene...\n" );
		const std::string tmp = TempPath( "entitytemplates_t1.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "T1 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, /*interactiveRasterizer*/nullptr );

		const Category cats[] = {
			Category::Light, Category::Object, Category::Material,
			Category::Painter, Category::Medium
		};
		for( Category cat : cats )
		{
			const unsigned int n = ctrl.EntityTemplateCount( cat );
			Check( n > 0, "category " + std::to_string( static_cast<int>( cat ) ) + " has at least one template" );
			for( unsigned int i = 0; i < n; ++i )
			{
				const std::string label = ctrl.EntityTemplateLabel( cat, i ).c_str();
				String outName;
				const SceneEditController::AgentCommitResult r =
					ctrl.InstantiateEntityTemplate( cat, i, &outName );
				Check( r.applied, "template `" + label + "` applied (" + std::string( r.message.c_str() ) + ")" );
				if( !r.applied ) continue;
				const std::string nm = outName.c_str();
				Check( !nm.empty(), "template `" + label + "` reported a non-empty instance name" );
				Check( NameInCategory( ctrl, cat, nm ),
					"template `" + label + "` result `" + nm + "` appears in CategoryEntityName" );
			}
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// T1b: the @MATERIAL@ bootstrap branch -- an Object template
	// instantiated into a scene with NO materials must first insert the
	// bundled default painter + lambertian pair, then bind the object
	// to it.
	//------------------------------------------------------------------
	void TestObjectBootstrapMaterial()
	{
		std::printf( "T1b: object template bootstraps a default material when none exists...\n" );
		// Same skeleton as kBaseScene but with NO painter/material/object.
		const char* const kNoMaterialScene =
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
			"film\n{\nwidth 24\nheight 24\n}\n\n"
			"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
			"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";
		const std::string tmp = TempPath( "entitytemplates_t1b.RISEscene" );
		Job* pJob = LoadScene( kNoMaterialScene, tmp );
		Check( pJob != nullptr, "T1b fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		// Job::InitializeContainers registers a `"none"` sentinel (the
		// null material), so the manager is never truly empty -- the
		// bootstrap must key off "no REAL material", which this fixture
		// provides (the only registered material is the sentinel).
		Check( !NameInCategory( ctrl, Category::Material, "default_lambertian" )
		    && ctrl.CategoryEntityCount( Category::Material ) <= 1,
			"fixture starts with no real (non-sentinel) materials" );

		// Template 0 in Object is the sphere; any object template works.
		String outName;
		const SceneEditController::AgentCommitResult r =
			ctrl.InstantiateEntityTemplate( Category::Object, 0, &outName );
		Check( r.applied, "object template applied on a material-less scene (" + std::string( r.message.c_str() ) + ")" );
		Check( NameInCategory( ctrl, Category::Object, std::string( outName.c_str() ) ),
			"object appears in CategoryEntityName(Object)" );
		Check( NameInCategory( ctrl, Category::Material, "default_lambertian" ),
			"the bootstrap material default_lambertian was created" );
		Check( NameInCategory( ctrl, Category::Painter, "default_gray" ),
			"the bootstrap painter default_gray was created" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// T2: duplicate round-trip.
	//------------------------------------------------------------------
	void TestDuplicateRoundTrip()
	{
		std::printf( "T2: duplicate round-trip (Material)...\n" );
		const std::string tmp = TempPath( "entitytemplates_t2.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "T2 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		const RISE::Cst::Document* doc = pJob->GetCstDocument();
		Check( doc != nullptr, "T2 fixture retains a CST Document" );
		const int chunksBefore = doc ? ChunkCount( *doc ) : -1;
		const unsigned int matCountBefore = ctrl.CategoryEntityCount( Category::Material );

		String outName;
		const SceneEditController::AgentCommitResult r =
			ctrl.DuplicateEntity( Category::Material, String( "mat_diffuse" ), &outName );
		Check( r.applied, "duplicate mat_diffuse applied (" + std::string( r.message.c_str() ) + ")" );
		Check( std::string( outName.c_str() ) != "mat_diffuse" && !std::string( outName.c_str() ).empty(),
			"duplicate got a fresh deduped name (`" + std::string( outName.c_str() ) + "`)" );
		Check( ctrl.CategoryEntityCount( Category::Material ) == matCountBefore + 1,
			"Material entity count went up by exactly one" );
		Check( NameInCategory( ctrl, Category::Material, std::string( outName.c_str() ) ),
			"duplicated name appears in CategoryEntityName(Material)" );

		doc = pJob->GetCstDocument();   // re-fetch: a D2 re-derive would have replaced the Document
		const int chunksAfter = doc ? ChunkCount( *doc ) : -1;
		Check( chunksBefore >= 0 && chunksAfter == chunksBefore + 1,
			"CST top-level CHUNK count went up by exactly one (single-chunk material template)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// T3: remove-referenced refusal + unreferenced-remove success.
	//------------------------------------------------------------------
	void TestRemoveReferencedRefusal()
	{
		std::printf( "T3: remove-referenced refusal / unreferenced remove succeeds...\n" );
		const std::string tmp = TempPath( "entitytemplates_t3.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "T3 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		const unsigned int matCountBefore = ctrl.CategoryEntityCount( Category::Material );

		// mat_diffuse is bound to obj_sph -- removing it must be refused.
		const SceneEditController::AgentCommitResult refused =
			ctrl.RemoveEntity( Category::Material, String( "mat_diffuse" ) );
		Check( !refused.applied, "removing a still-referenced material is refused" );
		Check( refused.message.size() > 1, "refusal carries a non-empty message" );
		Check( ctrl.CategoryEntityCount( Category::Material ) == matCountBefore,
			"Material entity count unchanged after the refused remove" );
		Check( NameInCategory( ctrl, Category::Material, "mat_diffuse" ),
			"mat_diffuse still present after the refused remove" );

		// lgt_unused has no referrer -- removing it must succeed.
		const unsigned int lightCountBefore = ctrl.CategoryEntityCount( Category::Light );
		const SceneEditController::AgentCommitResult removed =
			ctrl.RemoveEntity( Category::Light, String( "lgt_unused" ) );
		Check( removed.applied, "removing an unreferenced light succeeds (" + std::string( removed.message.c_str() ) + ")" );
		Check( ctrl.CategoryEntityCount( Category::Light ) == lightCountBefore - 1,
			"Light entity count went down by one" );
		Check( !NameInCategory( ctrl, Category::Light, "lgt_unused" ),
			"lgt_unused no longer present after the successful remove" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// T4: painter enumeration + a painter param edit via the new path.
	//------------------------------------------------------------------
	void TestPainterEnumerationAndEdit()
	{
		std::printf( "T4: painter enumeration + property edit...\n" );
		const std::string tmp = TempPath( "entitytemplates_t4.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "T4 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		Check( ctrl.CategoryEntityCount( Category::Painter ) >= 1, "at least one painter enumerated" );
		Check( NameInCategory( ctrl, Category::Painter, "pnt_albedo" ), "pnt_albedo appears in CategoryEntityName(Painter)" );

		// Path A: ApplyAgentParamEdit directly (the generic CST param-edit
		// path Category::Painter's SetProperty routes through).
		const SceneEditController::AgentCommitResult r1 = ctrl.ApplyAgentParamEdit(
			String( "pnt_albedo" ), String( "painter" ), String( "color" ), String( "0.25 0.25 0.25" ), nullptr );
		Check( r1.applied, "ApplyAgentParamEdit on pnt_albedo.color applied (" + std::string( r1.message.c_str() ) + ")" );

		{
			const std::vector<CameraProperty> rows =
				CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_albedo" ), "painter", "Painter chunk keyword" );
			bool found = false;
			for( const CameraProperty& row : rows )
			{
				if( std::string( row.name.c_str() ) == "color" )
				{
					found = true;
					Check( std::string( row.value.c_str() ) == "0.25 0.25 0.25",
						"CstIntrospection sees the edited color value (got `" + std::string( row.value.c_str() ) + "`)" );
				}
			}
			Check( found, "CstIntrospection::Inspect surfaces a `color` row for uniformcolor_painter" );
			Check( !rows.empty() && std::string( rows[0].name.c_str() ) == "chunk_type" && !rows[0].editable,
				"CstIntrospection::Inspect's leading row is a read-only `chunk_type` identity row" );
		}

		// Path B: the GUI-facing SetSelection + SetProperty(Category::Painter)
		// route (Category::Painter's SetProperty case).
		Check( ctrl.SetSelection( Category::Painter, String( "pnt_albedo" ) ), "SetSelection(Painter, pnt_albedo) succeeds" );
		Check( ctrl.SetProperty( String( "color" ), String( "0.75 0.1 0.1" ) ), "SetProperty(color) on the selected painter succeeds" );

		{
			const std::vector<CameraProperty> rows =
				CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_albedo" ), "painter", "Painter chunk keyword" );
			bool found = false;
			for( const CameraProperty& row : rows )
			{
				if( std::string( row.name.c_str() ) == "color" )
				{
					found = true;
					Check( std::string( row.value.c_str() ) == "0.75 0.1 0.1",
						"the GUI-facing SetProperty path lands the SAME edit (got `" + std::string( row.value.c_str() ) + "`)" );
				}
			}
			Check( found, "color row still present after the second edit" );
		}

		// Also exercise the panel snapshot path (RefreshProperties ->
		// PropertyCountFor/PropertyNameFor), which buildRowsFor's
		// Category::Painter case populates from the same CST source.
		ctrl.RefreshProperties();
		const unsigned int propCount = ctrl.PropertyCountFor( Category::Painter );
		Check( propCount > 0, "PropertyCountFor(Painter) is non-zero after RefreshProperties" );
		bool sawColorProp = false;
		for( unsigned int i = 0; i < propCount; ++i )
		{
			if( std::string( ctrl.PropertyNameFor( Category::Painter, i ).c_str() ) == "color" )
			{
				sawColorProp = true;
				Check( std::string( ctrl.PropertyValueFor( Category::Painter, i ).c_str() ) == "0.75 0.1 0.1",
					"PropertyValueFor(Painter, color) matches the last edit" );
			}
		}
		Check( sawColorProp, "RefreshProperties' panel snapshot includes the `color` row" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
	//------------------------------------------------------------------
	// T5: derived sub-chunk name collision -> whole-name-set dedup.
	//     An orphan `<base>_geo` geometry (left after removing a prior
	//     object) must NOT make a fresh "Add Sphere" fail mid-sequence
	//     with a name the user never chose; the selector bumps the whole
	//     instance-name set so the derived geometry name is free too.
	//------------------------------------------------------------------
	void TestDerivedNameCollisionDedup()
	{
		std::printf( "T5: derived sub-chunk name collision dedup...\n" );
		const std::string tmp = TempPath( "entitytemplates_t5.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "T5 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		// Locate the multi-chunk "Sphere" object template by label so the
		// test survives template reordering.
		unsigned int sphereIdx = 0; bool haveSphere = false;
		const unsigned int nObj = ctrl.EntityTemplateCount( Category::Object );
		for( unsigned int i = 0; i < nObj; ++i )
		{
			if( std::string( ctrl.EntityTemplateLabel( Category::Object, i ).c_str() ) == "Sphere" )
			{ sphereIdx = i; haveSphere = true; break; }
		}
		Check( haveSphere, "found the `Sphere` object template" );
		if( !haveSphere ) { pJob->release(); std::remove( tmp.c_str() ); return; }

		// First sphere: creates object `sphere` + geometry `sphere_geo`.
		String n1;
		const SceneEditController::AgentCommitResult r1 =
			ctrl.InstantiateEntityTemplate( Category::Object, sphereIdx, &n1 );
		Check( r1.applied, "first Add Sphere applied (" + std::string( r1.message.c_str() ) + ")" );
		Check( std::string( n1.c_str() ) == "sphere", "first sphere took the base name `sphere` (got `" + std::string( n1.c_str() ) + "`)" );

		// Remove ONLY the object; its geometry `sphere_geo` is left
		// orphaned (unreferenced but still deriving) -- the exact leftover
		// that used to break the next Add.
		const SceneEditController::AgentCommitResult rr =
			ctrl.RemoveEntity( Category::Object, String( "sphere" ) );
		Check( rr.applied, "removing object `sphere` succeeds, leaving `sphere_geo` orphaned (" + std::string( rr.message.c_str() ) + ")" );

		// Second sphere: top-level `sphere` is free again, but `sphere_geo`
		// is taken -> the selector must bump the WHOLE set past `sphere`.
		String n2;
		const SceneEditController::AgentCommitResult r2 =
			ctrl.InstantiateEntityTemplate( Category::Object, sphereIdx, &n2 );
		Check( r2.applied, "second Add Sphere applied despite the orphan `sphere_geo` (" + std::string( r2.message.c_str() ) + ")" );
		Check( std::string( n2.c_str() ) != "sphere", "second sphere avoided the colliding name set (got `" + std::string( n2.c_str() ) + "`)" );
		Check( NameInCategory( ctrl, Category::Object, std::string( n2.c_str() ) ), "second sphere appears in CategoryEntityName(Object)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// T6: a DUPLICATED non-repeatable param -- the panel must agree with
	//     the renderer about which occurrence is live, and must not offer
	//     an edit that would rewrite the dead one.
	//
	//     Nothing in the stack refuses `color 1 0 0` followed by
	//     `color 0 0 1`: ParseStateBag::SetSingle is an unconditional
	//     overwrite, so the scene derives from the LAST occurrence.  The
	//     panel used to read the FIRST (CstIntrospection's own
	//     ReadFirstParamValue), so it displayed a value the renderer had
	//     never used -- and because its edit route addresses occurrence 0
	//     too, editing the displayed row rewrote the invisible line and
	//     the panel appeared not to change.  Both halves are asserted
	//     here; they only agree if both were fixed.
	//------------------------------------------------------------------
	const char* const kDupParamScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 24\nheight 24\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		// The duplicated param under test: `color` is NOT repeatable.
		"uniformcolor_painter\n{\nname pnt_dup\ncolor 1 0 0\ncolor 0 0 1\n}\n\n"
		// Control: same chunk type, single occurrence -- must stay editable.
		"uniformcolor_painter\n{\nname pnt_clean\ncolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\nname mat_dup\nreflectance pnt_dup\n}\n\n"
		"sphere_geometry\n{\nname geo_first\nradius 0.25\n}\n\n"
		"sphere_geometry\n{\nname geo_last\nradius 0.9\n}\n\n"
		// The reported repro shape, on the binding side.
		"standard_object\n{\nname obj_dup\ngeometry geo_first\ngeometry geo_last\nmaterial mat_dup\n}\n";

	// Copy the row named `which` into `out`; false when the row is absent.
	bool FindRow( const std::vector<CameraProperty>& rows, const char* which, CameraProperty& out )
	{
		for( const CameraProperty& r : rows )
			if( std::string( r.name.c_str() ) == which ) { out = r; return true; }
		return false;
	}

	void TestDuplicateNonRepeatableParam()
	{
		std::printf( "T6: duplicated non-repeatable param -- panel agrees with the derive, edit refused...\n" );
		const std::string tmp = TempPath( "entitytemplates_t6.RISEscene" );
		Job* pJob = LoadScene( kDupParamScene, tmp );
		Check( pJob != nullptr, "T6 fixture loads (a duplicated non-repeatable param is ACCEPTED, not refused)" );
		if( !pJob ) return;

		// (a) GROUND TRUTH, read from the live scene rather than from any
		//     introspection code: the object bound the LAST `geometry`.
		{
			IScenePriv* scene = pJob->GetScene();
			Check( scene != nullptr, "T6 scene present" );
			const IObject* obj = scene && scene->GetObjects()
				? const_cast<IObjectManager*>( scene->GetObjects() )->GetItem( "obj_dup" ) : nullptr;
			Check( obj != nullptr, "obj_dup derived" );
			const IGeometryManager* geos = pJob->GetGeometries();
			const IGeometry* wantLast  = geos ? const_cast<IGeometryManager*>( geos )->GetItem( "geo_last" )  : nullptr;
			const IGeometry* wantFirst = geos ? const_cast<IGeometryManager*>( geos )->GetItem( "geo_first" ) : nullptr;
			Check( obj && wantLast && obj->GetGeometry() == wantLast,
				"the LIVE object binds the LAST `geometry` occurrence (geo_last)" );
			Check( obj && wantFirst && obj->GetGeometry() != wantFirst,
				"the first `geometry` occurrence is dead text, not the binding" );
		}

		// (b) THE READ HALF: the panel row shows the occurrence the scene
		//     derived from -- the LAST -- not the first.
		std::vector<CameraProperty> dupRows =
			CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_dup" ), "painter", "Painter chunk keyword" );
		CameraProperty dupColor;
		Check( FindRow( dupRows, "color", dupColor ), "a `color` row is surfaced for the duplicated painter" );
		Check( std::string( dupColor.value.c_str() ) == "0 0 1",
			"the `color` row shows the LAST occurrence, matching the derive (got `"
				+ std::string( dupColor.value.c_str() ) + "`, want `0 0 1`)" );

		// (c) THE WRITE HALF: the row is not offered as editable, because
		//     the occ=0 edit route underneath cannot address the occurrence
		//     the row displays.
		Check( !dupColor.editable, "the duplicated `color` row is READ-ONLY (an occ=0 edit could not reach the live occurrence)" );
		Check( std::string( dupColor.description.c_str() ).find( "READ-ONLY" ) != std::string::npos,
			"the read-only row explains itself (description names the duplication)" );

		// (d) ...and the refusal is enforced at the mutation boundary too,
		//     not merely hidden in the panel: the write is rejected and the
		//     live value is untouched.
		SceneEditController ctrl( *pJob, nullptr );
		Check( ctrl.SetSelection( Category::Painter, String( "pnt_dup" ) ), "SetSelection(Painter, pnt_dup) succeeds" );
		Check( !ctrl.SetProperty( String( "color" ), String( "0 1 0" ) ),
			"SetProperty on the duplicated param is REFUSED (not a silent no-op reported as success)" );
		{
			const std::vector<CameraProperty> after =
				CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_dup" ), "painter", "Painter chunk keyword" );
			CameraProperty row;
			Check( FindRow( after, "color", row ) && std::string( row.value.c_str() ) == "0 0 1",
				"the refused edit left the value unchanged" );
		}
		Check( pJob->ApplyCstParamEdit( "obj_dup", "standard_object", "geometry", 0, "geo_first" ) == 0,
			"the Job-level edit path refuses the duplicated `geometry` too (every route, not just the panel)" );

		// (d2) The insert gate deliberately does NOT refuse this shape -- see
		//      Job::ApplyCstInsertChunk's comment and SceneGraphParentTest V/6b,
		//      which requires a duplicated `geometry` to insert so the
		//      container-vs-leaf gate can be proven last-wins in the ACCEPTING
		//      direction.  Pinned here so a future "refuse it at the door"
		//      attempt fails in BOTH files rather than silently making that
		//      sibling coverage unreachable.
		{
			char kw[64] = {0}, nm[64] = {0}, diag[256] = {0};
			int at = -1;
			const int rc = pJob->ApplyCstInsertChunk(
				"uniformcolor_painter\n{\nname pnt_new_dup\ncolor 1 1 0\ncolor 0 1 1\n}\n",
				kw, sizeof( kw ), nm, sizeof( nm ), diag, sizeof( diag ), &at );
			Check( rc > 0, "a chunk with a duplicated non-repeatable param still INSERTS (the gate is on the EDIT, not the insert)" );
			const std::vector<CameraProperty> newRows =
				CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_new_dup" ), "painter", "Painter chunk keyword" );
			CameraProperty newColor;
			Check( FindRow( newRows, "color", newColor ) && std::string( newColor.value.c_str() ) == "0 1 1",
				"...and the panel reads its LAST occurrence immediately, without an edit first" );
			Check( !newColor.editable, "...and surfaces it read-only, so the defect is visible where it bites" );
		}

		// (e) CONTROL: an identical chunk WITHOUT the duplicate is
		//     unaffected -- read, editability, and the edit itself.
		std::vector<CameraProperty> cleanRows =
			CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_clean" ), "painter", "Painter chunk keyword" );
		CameraProperty cleanColor;
		Check( FindRow( cleanRows, "color", cleanColor ), "a `color` row is surfaced for the clean painter" );
		Check( std::string( cleanColor.value.c_str() ) == "0.5 0.5 0.5", "the clean painter reads its single occurrence" );
		Check( cleanColor.editable, "the clean `color` row stays EDITABLE (the refusal is scoped to the duplicate)" );
		Check( ctrl.SetSelection( Category::Painter, String( "pnt_clean" ) ), "SetSelection(Painter, pnt_clean) succeeds" );
		Check( ctrl.SetProperty( String( "color" ), String( "0.25 0.25 0.25" ) ), "SetProperty on the clean painter still applies" );
		{
			const std::vector<CameraProperty> after =
				CstIntrospection::Inspect( pJob->GetCstDocument(), *pJob, String( "pnt_clean" ), "painter", "Painter chunk keyword" );
			CameraProperty row;
			Check( FindRow( after, "color", row ) && std::string( row.value.c_str() ) == "0.25 0.25 0.25",
				"the clean painter's edit round-trips through the panel read" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}
	//==================================================================
	// S18 -- node-graph canvas chunk creation (SceneEditController::
	// CreateChunkNode).  See docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S18.
	//
	// WHY THIS FILE.  CreateChunkNode is a sibling of
	// InstantiateEntityTemplate / DuplicateEntity / RemoveEntity: the same
	// controller layer, the same UniqueEntityName-family dedup, the same
	// AgentCommitResult contract, and it composes the SAME
	// ApplyAgentInsertChunk commit.  This file's harness already is a
	// SceneEditController over a real Job with an inline v7 scene and no
	// render pass, which is exactly what these tests need.  The two files
	// the brief offered are a poorer fit: AgentChunkCrudTest exercises the
	// JOB PRIMITIVE plus the JSON-RPC dispatcher (a layer CreateChunkNode
	// deliberately does not add to -- it is not an agent verb), and
	// ReferenceGraphTest is a pure-function test over parsed Documents
	// with no controller at all.  The graph-visibility assertion below
	// reaches into SceneReferenceGraph directly, which is all that part
	// needs.
	//==================================================================

	using NodeArg = SceneEditController::ChunkNodeArg;

	std::vector<NodeArg> Args( std::initializer_list<std::pair<const char*, const char*>> kv )
	{
		std::vector<NodeArg> v;
		for( const auto& p : kv )
		{
			NodeArg a;
			a.param = String( p.first );
			a.value = String( p.second );
			v.push_back( a );
		}
		return v;
	}

	std::string DocText( Job* pJob )
	{
		const RISE::Cst::Document* d = pJob->GetCstDocument();
		return d ? RISE::Cst::SerializeCst( *d ) : std::string();
	}

	// True iff `doc` carries a top-level chunk with keyword `kw` named
	// `name` -- read through the SAME S11 scan the canvas seeds its nodes
	// from, so "the node appears in the graph snapshot" is asserted
	// against the real consumer, not a bespoke walk.
	bool GraphHasNode( const RISE::Cst::Document& doc, const char* kw, const std::string& name )
	{
		const std::vector<SceneReferenceGraph::DocumentChunk> chunks = SceneReferenceGraph::AllChunks( doc );
		for( const SceneReferenceGraph::DocumentChunk& c : chunks )
			if( std::string( c.keyword.c_str() ) == kw && std::string( c.name.c_str() ) == name ) return true;
		return false;
	}

	//------------------------------------------------------------------
	// S18a: the representative keyword classes each create, derive, and
	//       show up as a live entity AND as a graph node.
	//------------------------------------------------------------------
	void TestCreateChunkNodeClasses()
	{
		std::printf( "S18a: representative painter/material keyword classes create...\n" );
		const std::string tmp = TempPath( "s18_classes.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18a fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		// (1) A no-required-ref procedural painter.
		{
			String out;
			const auto r = ctrl.CreateChunkNode( String( "perlin3d_painter" ), String( "noise" ), {}, &out );
			Check( r.applied, std::string( "perlin3d_painter creates (" ) + r.message.c_str() + ")" );
			Check( std::string( out.c_str() ) == "noise", "...under the requested base name" );
			Check( NameInCategory( ctrl, Category::Painter, "noise" ), "...and enumerates as a live Painter" );
			Check( GraphHasNode( *pJob->GetCstDocument(), "perlin3d_painter", "noise" ),
				"...and appears in the S11 graph snapshot" );
		}

		// (2) A REQUIRED-REFERENCE painter: the contract is discoverable
		//     BEFORE the call, and the call succeeds once it is met.
		{
			const auto needs = ctrl.ChunkNodeRequirements( String( "ramp_painter" ) );
			Check( needs.size() == 1 && std::string( needs[0].param.c_str() ) == "input",
				"ramp_painter advertises exactly one required arg, `input`" );
			Check( !needs.empty() && needs[0].isReference, "...and reports it as a REFERENCE slot" );

			String out;
			const auto r = ctrl.CreateChunkNode( String( "ramp_painter" ), String( "terrain_ramp" ),
			                                     Args( { { "input", "noise" } } ), &out );
			Check( r.applied, std::string( "ramp_painter creates with `input` supplied (" ) + r.message.c_str() + ")" );
			Check( NameInCategory( ctrl, Category::Painter, "terrain_ramp" ), "...and enumerates as a live Painter" );

			// The seeded two `stop` lines are what make it derivable at
			// all -- prove the wire actually landed in the graph.
			const RISE::Cst::Document& doc = *pJob->GetCstDocument();
			const std::vector<SceneReferenceGraph::DocumentChunk> pre = SceneReferenceGraph::AllChunks( doc );
			const std::vector<ReferenceEdge> edges = SceneReferenceGraph::Edges( doc, &pre );
			bool wired = false;
			for( const ReferenceEdge& e : edges )
				if( std::string( e.referrerName.c_str() ) == "terrain_ramp"
				 && std::string( e.targetName.c_str() ) == "noise" ) wired = true;
			Check( wired, "...and its `input` reference is a real edge to `noise` in the graph" );
		}

		// (3) Materials -- the simplest and a heavily-parameterised one.
		{
			String out;
			Check( ctrl.CreateChunkNode( String( "lambertian_material" ), String( "diffuse" ), {}, &out ).applied,
				"lambertian_material creates with no args" );
			Check( NameInCategory( ctrl, Category::Material, "diffuse" ), "...and enumerates as a live Material" );
			Check( ctrl.CreateChunkNode( String( "ggx_material" ), String( "metal" ), {}, &out ).applied,
				"ggx_material creates with no args" );
			Check( NameInCategory( ctrl, Category::Material, "metal" ), "...and enumerates as a live Material" );
		}

		// (4) scalar_painter -- the FORM case: twelve mutually-exclusive
		//     forms, none of them descriptor-`required`, so only the seed
		//     table makes a bare create derivable.  Assert the form landed.
		{
			String out;
			Check( ctrl.CreateChunkNode( String( "scalar_painter" ), String( "rough" ), {}, &out ).applied,
				"scalar_painter creates with no args (seeded into the `value` form)" );
			const std::string doc = DocText( pJob );
			Check( doc.find( "name rough\nvalue 0.5\n" ) != std::string::npos,
				"...and its body carries the seeded `value 0.5` form" );
			// It must resolve in the SCALAR manager, not just the colour
			// one -- the whole point of the form.
			IScalarPainterManager* spm = pJob->GetScalarPainters();
			Check( spm && spm->GetItem( "rough" ) != nullptr, "...and registers in the IScalarPainter pipe" );
		}

		// (5) A caller ARG overrides the seed rather than duplicating it.
		{
			String out;
			Check( ctrl.CreateChunkNode( String( "scalar_painter" ), String( "rough_hi" ),
			                             Args( { { "value", "0.9" } } ), &out ).applied,
				"scalar_painter accepts an explicit `value` arg" );
			const std::string doc = DocText( pJob );
			Check( doc.find( "name rough_hi\nvalue 0.9\n}" ) != std::string::npos,
				"...and the caller's value REPLACES the seed (no duplicate `value` line)" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// S18b: name collision -> suffixed; exhaustion -> refusal.
	//------------------------------------------------------------------
	void TestCreateChunkNodeNaming()
	{
		std::printf( "S18b: name dedup, cross-kind collision, and exhaustion...\n" );
		const std::string tmp = TempPath( "s18_naming.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18b fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		String a, b, c;
		Check( ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "tint" ), {}, &a ).applied, "first `tint` creates" );
		Check( std::string( a.c_str() ) == "tint", "...named `tint`" );
		Check( ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "tint" ), {}, &b ).applied, "second `tint` creates" );
		Check( std::string( b.c_str() ) == "tint_2", "...suffixed to `tint_2`" );
		Check( ctrl.CreateChunkNode( String( "checker_painter" ), String( "tint" ), {}, &c ).applied, "a DIFFERENT keyword also asks for `tint`" );
		Check( std::string( c.c_str() ) == "tint_3",
			"...and is deduped too (the probe is doc-wide, so two same-named Painter-category chunks can never collide)" );

		// An empty base falls back to the keyword itself.
		{
			String d;
			Check( ctrl.CreateChunkNode( String( "perlin3d_painter" ), String(), {}, &d ).applied, "an empty base name still creates" );
			Check( std::string( d.c_str() ) == "perlin3d_painter", "...falling back to the keyword as the base" );
		}

		// round-1 P1: a whitespace-padded base is canonicalized through the
		// SAME choke point CloneActiveCamera's name pick uses
		// (CanonicalCameraName) BEFORE the dedup pick -- the reviewer's
		// probe scenario.  Space is not a safe CST identifier char, so each
		// one becomes `_`; the entity must exist under exactly that
		// canonical name, and a second create with the SAME padded base
		// (which canonicalizes to the SAME string) must dedup against it,
		// not silently collide or produce two differently-shaped names.
		{
			String e, f;
			const auto rE = ctrl.CreateChunkNode( String( "checker_painter" ), String( " tint pad " ), {}, &e );
			Check( rE.applied, "a whitespace-padded base creates" );
			Check( std::string( e.c_str() ) == "_tint_pad_",
				"...outName is the canonicalized base (leading/trailing/interior space -> `_`)" );
			Check( GraphHasNode( *pJob->GetCstDocument(), "checker_painter", std::string( e.c_str() ) ),
				"...and the entity exists in the graph under the canonical name" );
			String f2;
			const auto rF = ctrl.CreateChunkNode( String( "checker_painter" ), String( " tint pad " ), {}, &f2 );
			Check( rF.applied, "a second create with the SAME padded base creates too" );
			Check( std::string( f2.c_str() ) == "_tint_pad__2",
				"...and dedups against the CANONICAL form (not two distinct raw-padded collisions)" );
		}

		// round-1 P3: a 1-char base is used as-is (only a genuinely EMPTY
		// base falls back to the keyword) -- the old `< 2` check refused a
		// perfectly usable single-char base the header never documented
		// refusing.
		{
			String g;
			Check( ctrl.CreateChunkNode( String( "perlin2d_painter" ), String( "p" ), {}, &g ).applied,
				"a 1-char base creates" );
			Check( std::string( g.c_str() ) == "p", "...and is used AS-IS, not folded back to the keyword" );
		}

		// round-1 P3: long-name suffix discipline mirrors UniqueCameraName's
		// reserve-suffix-bytes truncation -- a base near the 255-byte C ABI
		// payload cap must still dedup correctly (the suffix must not be
		// silently erased by a truncation that runs AFTER formatting).
		{
			const std::string longBase( 300, 'q' );   // exceeds the 255-byte payload cap
			String h, i;
			const auto rH = ctrl.CreateChunkNode( String( "perlin2d_painter" ), String( longBase.c_str() ), {}, &h );
			Check( rH.applied, "a base past the 255-byte payload cap still creates" );
			Check( std::string( h.c_str() ).size() <= 255, "...outName is truncated to the payload cap" );
			Check( std::string( h.c_str() ) == longBase.substr( 0, 255 ), "...truncated to exactly the first 255 bytes" );
			const auto rI = ctrl.CreateChunkNode( String( "perlin2d_painter" ), String( longBase.c_str() ), {}, &i );
			Check( rI.applied, "a SECOND create with the same over-long base also creates" );
			Check( std::string( i.c_str() ) != std::string( h.c_str() ),
				"...and dedups to a DIFFERENT name (the collision, not a silently-erased suffix)" );
			Check( std::string( i.c_str() ).size() <= 255, "...the deduped name also respects the payload cap" );
			Check( std::string( i.c_str() ).substr( std::string( i.c_str() ).size() - 2 ) == "_2",
				"...and the `_2` suffix survived truncation intact (reserved BEFORE truncating the base)" );
		}

		// EXHAUSTION: occupy `full`, `full_2` .. `full_999`, then prove the
		// next create REFUSES without mutating (rather than the
		// InstantiateEntityTemplate timestamp fallback, which is unchecked).
		{
			// Build the exhausted namespace cheaply: one insert per name
			// through the same verb would be 999 full re-derives, so splice
			// the names in as one document text instead.
			std::string text = DocText( pJob );
			text += "\n";
			for( int i = 0; i < 1000; ++i )
			{
				char nm[64];
				if( i == 0 ) std::snprintf( nm, sizeof( nm ), "full" );
				else         std::snprintf( nm, sizeof( nm ), "full_%d", i + 1 );
				text += "uniformcolor_painter\n{\nname ";
				text += nm;
				text += "\ncolor 0.5 0.5 0.5\n}\n";
			}
			char diag[512] = { 0 };
			const int rc = pJob->ApplyCstReplaceDocumentText(
				text.c_str(), /*restoreActiveRasterizer*/ true, diag, sizeof( diag ), "s18-exhaustion-fixture" );
			Check( rc == 2 || rc == 3, std::string( "exhaustion fixture installs (rc=" ) + std::to_string( rc ) + ", " + diag + ")" );

			const std::string before = DocText( pJob );
			String out;
			const auto r = ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "full" ), {}, &out );
			Check( !r.applied, "a create whose whole `_2`..`_999` suffix range is taken is REFUSED" );
			Check( std::string( r.status.c_str() ) == "rejected", "...with status=rejected" );
			Check( std::string( r.message.c_str() ).find( "could not find a free name" ) != std::string::npos,
				std::string( "...naming the exhaustion honestly (" ) + r.message.c_str() + ")" );
			Check( std::string( out.c_str() ).empty(), "...and reports no name" );
			Check( DocText( pJob ) == before, "...leaving the Document BYTE-IDENTICAL" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// S18c: every refusal path leaves the Document byte-identical.
	//       This is the slice's headline contract -- red-proved below by
	//       comparing the FULL serialized document, not a chunk count.
	//------------------------------------------------------------------
	void TestCreateChunkNodeRefusalsByteIdentical()
	{
		std::printf( "S18c: every refusal is byte-identical and honest...\n" );
		const std::string tmp = TempPath( "s18_refuse.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18c fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		struct Case { const char* what; const char* kw; const char* base; std::vector<NodeArg> args; const char* expectFragment; };
		const std::vector<Case> cases = {
			{ "unknown keyword",              "not_a_painter",      "x", {},                                        "unknown chunk keyword" },
			{ "out-of-scope keyword (light)", "omni_light",         "x", {},                                        "not a painter or material" },
			{ "out-of-scope keyword (geom)",  "sphere_geometry",    "x", {},                                        "not a painter or material" },
			{ "missing required reference",   "ramp_painter",       "x", {},                                        "requires `input`" },
			{ "undeclared parameter arg",     "lambertian_material","x", Args( { { "reflectanc", "pnt_albedo" } } ), "declares no parameter named `reflectanc`" },
			// round-1 P3: an empty arg value is refused DIRECTLY, naming the
			// offending param, rather than silently composing a value-less
			// param line the parser would reject two layers down.
			{ "empty arg value",              "uniformcolor_painter","x", Args( { { "color", "" } } ),               "the value for `color` is empty" },
			{ "caller-supplied `name`",       "lambertian_material","x", Args( { { "name", "hijack" } } ),           "`name` is chosen by this verb" },
			{ "multi-line value (brace)",     "lambertian_material","x", Args( { { "reflectance", "pnt_albedo\n}\nomni_light\n{\nname evil" } } ), "single line" },
			// The SHARP form of the injection: a value carrying an extra
			// NEWLINE but no brace still parses to exactly ONE chunk, so
			// ApplyCstInsertChunk's own multi-chunk guard does NOT catch
			// it -- without this verb's single-line check the smuggled
			// `variant` line would APPLY and mutate the head.  This case
			// is the byte-identity red-prove pin for the guard.
			{ "smuggled extra param line",    "lambertian_material","x", Args( { { "reflectance", "pnt_albedo\nvariant smuggled" } } ), "single line" },
			// round-1: brace-in-value OVER-REJECTION, pinned honestly.
			// `ValueIsSingleLine` bans `{`/`}` in EVERY arg value
			// unconditionally -- it has no notion of quoting, so a value a
			// real CST author could write safely inside quotes (a literal
			// string payload that happens to contain `{`/`}`, e.g. a label
			// "a{b}") is refused here even though it could never smuggle a
			// second chunk in (there is no quoting-aware escape to exploit).
			// This is a DELIBERATE over-approximation, not a bug: the guard
			// exists to make the whole class of brace-based injection
			// (the two cases immediately above) impossible to get wrong,
			// and a value that legitimately needs a brace has a workaround
			// -- create the node with a placeholder value, then set the
			// real value through the param-edit path (ApplyAgentParamEdit),
			// which edits ONE already-declared param in place rather than
			// splicing caller text into a freshly composed chunk body.  Do
			// NOT weaken this guard to special-case quoting.
			{ "brace-in-value (over-rejection, by design)", "scalar_painter", "x", Args( { { "expression", "a{b}" } } ), "single line" },
			{ "non-deriving preset",          "ramp_painter",       "x", Args( { { "input", "no_such_painter" } } ), "would not derive" },
		};

		for( const Case& c : cases )
		{
			const std::string before = DocText( pJob );
			const auto verBefore = pJob->GetCstHeadVersion();
			String out;
			const auto r = ctrl.CreateChunkNode( String( c.kw ), String( c.base ), c.args, &out );
			Check( !r.applied, std::string( "REFUSED: " ) + c.what );
			Check( std::string( r.status.c_str() ) == "rejected", std::string( "...status=rejected: " ) + c.what );
			Check( std::string( r.message.c_str() ).find( c.expectFragment ) != std::string::npos,
				std::string( "...diagnostic names the cause (" ) + r.message.c_str() + ")" );
			Check( std::string( out.c_str() ).empty(), std::string( "...no name reported: " ) + c.what );
			Check( DocText( pJob ) == before, std::string( "...Document BYTE-IDENTICAL: " ) + c.what );
			Check( pJob->GetCstHeadVersion() == verBefore, std::string( "...head version unchanged: " ) + c.what );
		}

		// The multi-line-value case in particular must not have smuggled a
		// second chunk in even in a form the byte compare could miss.
		Check( DocText( pJob ).find( "name evil" ) == std::string::npos,
			"the brace-injection value never reached the Document" );
		Check( DocText( pJob ).find( "smuggled" ) == std::string::npos,
			"the newline-only injection never reached the Document either" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// S18d: EditHistory citizenship -- undo removes the created node and
	//       restores the Document BYTE-IDENTICALLY; redo re-creates it.
	//------------------------------------------------------------------
	void TestCreateChunkNodeUndoRedo()
	{
		std::printf( "S18d: undo/redo of a created node...\n" );
		const std::string tmp = TempPath( "s18_undo.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18d fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		const std::string before = DocText( pJob );
		String out;
		Check( ctrl.CreateChunkNode( String( "orennayar_material" ), String( "rough_diffuse" ), {}, &out ).applied,
			"a node is created" );
		const std::string after = DocText( pJob );
		Check( after != before, "...and the Document changed" );
		Check( NameInCategory( ctrl, Category::Material, "rough_diffuse" ), "...and the entity is live" );

		ctrl.Undo();
		Check( DocText( pJob ) == before, "UNDO restores the Document BYTE-IDENTICALLY" );
		Check( !NameInCategory( ctrl, Category::Material, "rough_diffuse" ), "...and the entity is gone from the live scene" );

		ctrl.Redo();
		Check( DocText( pJob ) == after, "REDO reinstates the byte-exact post-create Document" );
		Check( NameInCategory( ctrl, Category::Material, "rough_diffuse" ), "...and the entity is live again" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// S18e: mid-transaction refusal, dirty marking, epoch bump, and the
	//       agent-surface interplay (create here, then propose_patch on
	//       the result; and the head-version conflict semantics).
	//------------------------------------------------------------------
	void TestCreateChunkNodeControllerDiscipline()
	{
		std::printf( "S18e: transaction refusal / dirty / epoch / agent interplay...\n" );
		const std::string tmp = TempPath( "s18_discipline.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18e fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		// (a) DIRTY + EPOCH, on the freshly-loaded (clean) fixture.
		{
			Check( !ctrl.HasUnsavedChanges(), "the freshly-loaded fixture starts clean" );
			const unsigned int epochBefore = ctrl.SceneEpoch();
			String out;
			Check( ctrl.CreateChunkNode( String( "checker_painter" ), String( "checks" ), {}, &out ).applied, "a node is created" );
			Check( ctrl.HasUnsavedChanges(), "...which marks the scene DIRTY (save-safety)" );
			Check( ctrl.SceneEpoch() != epochBefore, "...and bumps the scene epoch the canvas re-enumerates on" );
		}

		// (b) MID-TRANSACTION: refused, retriable, non-mutating.
		{
			const std::string before = DocText( pJob );
			Check( ctrl.BeginTransaction(), "an editor transaction opens" );
			String out;
			const auto r = ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "mid_txn" ), {}, &out );
			Check( !r.applied, "a create during an open editor transaction is REFUSED" );
			Check( r.retriable, "...and is marked RETRIABLE (the gesture will end)" );
			Check( DocText( pJob ) == before, "...leaving the Document BYTE-IDENTICAL" );
			ctrl.EndTransaction();
			Check( ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "mid_txn" ), {}, &out ).applied,
				"...and the IDENTICAL create succeeds once the transaction closes (the red-prove that the gate is what rejected it)" );
		}

		// (c) AGENT INTERPLAY: a node created HERE is an ordinary chunk to
		//     the agent surface -- propose_patch retargets it in place.
		{
			String out;
			Check( ctrl.CreateChunkNode( String( "lambertian_material" ), String( "agent_target" ), {}, &out ).applied,
				"a material node is created" );
			const auto pr = ctrl.ApplyAgentParamEdit( String( "agent_target" ), String( "material" ),
			                                          String( "reflectance" ), String( "pnt_albedo" ), nullptr );
			Check( pr.applied, std::string( "...and an agent param edit retargets its `reflectance` (" ) + pr.message.c_str() + ")" );
			Check( DocText( pJob ).find( "reflectance pnt_albedo" ) != std::string::npos,
				"...with the retarget visible in the Document" );
		}

		// (d) HEAD-VERSION semantics: a create BUMPS the head, so a base
		//     version captured before it is stale for a later agent commit.
		{
			const auto stale = pJob->GetCstHeadVersion();
			String out;
			Check( ctrl.CreateChunkNode( String( "uniformcolor_painter" ), String( "bumper" ), {}, &out ).applied, "a node is created" );
			Check( !( pJob->GetCstHeadVersion() == stale ), "...bumping the head version" );
			const std::string before = DocText( pJob );
			const auto conflicted = ctrl.ApplyAgentInsertChunk(
				String( "uniformcolor_painter\n{\nname stale_insert\ncolor 1 0 0\n}\n" ), &stale );
			Check( conflicted.conflict, "...so an agent commit carrying the pre-create base CONFLICTS" );
			Check( DocText( pJob ) == before, "...without mutating" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	// round-2: a required Reference is not always Painter-typed.
	// `ChunkNodeRequirements` must advertise the descriptor's OWN
	// `referenceCategories` for a required reference slot (not just
	// `isReference`), and a Material-typed one -- `coated_material.base`,
	// `fabric_material.base` -- must actually create with a Material
	// stand-in.  Guards both halves of the fix: the requirement's
	// advertised category, and the create actually succeeding and wiring
	// the base in.
	void TestCreateChunkNodeMaterialReferenceStandIn()
	{
		std::printf( "S18g: a Material-typed required reference advertises {Material} and creates with a Material stand-in...\n" );
		const std::string tmp = TempPath( "s18_material_standin.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18g fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		struct Case { const char* keyword; const char* param; };
		const Case cases[] = {
			{ "coated_material", "base" },
			{ "fabric_material", "base" },
		};

		for( const Case& c : cases )
		{
			// (a) The requirement is advertised as a reference RESTRICTED
			//     to {Material}, not {Painter} and not unrestricted -- the
			//     caller-facing contract this fix threads through
			//     SceneEditController::ChunkNodeRequirement.
			bool found = false, isRef = false, hasMaterial = false, hasPainter = false;
			for( const auto& req : ctrl.ChunkNodeRequirements( String( c.keyword ) ) )
			{
				if( std::string( req.param.c_str() ) != c.param ) continue;
				found = true;
				isRef = req.isReference;
				for( ChunkCategory cat : req.referenceCategories )
				{
					if( cat == ChunkCategory::Material ) hasMaterial = true;
					if( cat == ChunkCategory::Painter )  hasPainter = true;
				}
			}
			Check( found, std::string( "`" ) + c.keyword + "` advertises `" + c.param + "` as a requirement" );
			Check( isRef, std::string( "...as a reference (" ) + c.keyword + ")" );
			Check( hasMaterial, std::string( "...restricted to {Material} (" ) + c.keyword + ")" );
			Check( !hasPainter, std::string( "...and NOT {Painter} (" ) + c.keyword + ")" );

			// (b) Creating it with the fixture's Material stand-in
			//     (`mat_diffuse`, from kBaseScene) succeeds and wires the
			//     base in -- the red-prove for this test is restoring the
			//     old Painter-only stand-in (`pnt_albedo`), which must
			//     make this create FAIL instead.
			String out;
			std::vector<NodeArg> args;
			NodeArg a; a.param = String( c.param ); a.value = String( "mat_diffuse" );
			args.push_back( a );
			const auto r = ctrl.CreateChunkNode( String( c.keyword ), String( c.keyword ), args, &out );
			Check( r.applied, std::string( "`" ) + c.keyword + "` creates with a Material stand-in for `" + c.param + "` (" + r.message.c_str() + ")" );
			if( r.applied )
			{
				Check( DocText( pJob ).find( std::string( c.param ) + " mat_diffuse" ) != std::string::npos,
					std::string( "...and the Document shows `" ) + c.param + " mat_diffuse` (" + c.keyword + ")" );
			}
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// S18f: THE COVERAGE GATE.  Every Painter- and Material-category
	//       keyword the registry knows must either (a) create cleanly once
	//       its advertised ChunkNodeRequirements are satisfied, or (b) be
	//       listed below as a known, reasoned exception.  A new chunk
	//       whose minimal-validity rule its descriptor cannot express
	//       fails HERE and points at EntityTemplates.cpp's seed table.
	//------------------------------------------------------------------
	void TestCreateChunkNodeKeywordSweep()
	{
		std::printf( "S18f: every painter/material keyword creates (or is a listed exception)...\n" );
		const std::string tmp = TempPath( "s18_sweep.RISEscene" );
		Job* pJob = LoadScene( kBaseScene, tmp );
		Check( pJob != nullptr, "S18f fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		// Keywords whose minimal body needs a FILE ON DISK -- the one
		// class no static default can supply (a scene-relative path would
		// depend on RISE_MEDIA_PATH, which a general creator must not
		// assume).  These must advertise the file as a NON-reference
		// requirement, and must REFUSE cleanly (never abort: OpenEXR's
		// reader THROWS on the literal `none`, so refusing before the
		// derive is load-bearing, not cosmetic).
		//
		// round-1 P2-a: spectral_painter is NOT here anymore -- its `file`
		// param was never actually required (Finalize accepts inline `cp`
		// samples too), and EntityTemplates.cpp's kNodeSeeds now seeds two
		// `cp` lines, so it creates cleanly with no caller input like
		// scalar_painter/ramp_painter above it.
		struct FileNeeded { const char* keyword; const char* param; };
		const FileNeeded kFileNeeded[] = {
			{ "png_painter",         "file" },
			{ "jpg_painter",         "file" },
			{ "hdr_painter",         "file" },
			{ "exr_painter",         "file" },
			{ "tiff_painter",        "file" },
			{ "datadriven_material", "filename" },
		};
		for( const FileNeeded& f : kFileNeeded )
		{
			const auto reqs = ctrl.ChunkNodeRequirements( String( f.keyword ) );
			bool found = false;
			for( const auto& r : reqs )
				if( std::string( r.param.c_str() ) == f.param && !r.isReference ) found = true;
			Check( found, std::string( "`" ) + f.keyword + "` advertises `" + f.param + "` as a non-reference (file) requirement" );
		}
		auto isException = [&]( const std::string& kw ) {
			for( const FileNeeded& e : kFileNeeded ) if( kw == e.keyword ) return true;
			return false;
		};

		int created = 0, refusedExpected = 0;
		const std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
		for( const ChunkParserEntry& e : entries )
		{
			if( !e.parser ) continue;
			const ChunkDescriptor& d = e.parser->Describe();
			if( d.keyword.empty() || d.keyword != e.keyword ) continue;   // skip legacy aliases
			if( d.category != ChunkCategory::Painter && d.category != ChunkCategory::Material ) continue;

			// Satisfy every advertised requirement from the fixture scene,
			// picking the stand-in PER REFERENCED CATEGORY
			// (`req.referenceCategories`) rather than assuming every
			// required reference is Painter-typed.  round-2: the old
			// version here hardcoded `pnt_albedo` (a Painter) for every
			// required reference regardless of category, which silently
			// broke on `coated_material.base` / `fabric_material.base`
			// (both `{ChunkCategory::Material}`) -- a Painter name is not
			// a registered material, so the derive failed with "base
			// material `pnt_albedo` is not a registered material".  The
			// fixture scene (kBaseScene, top of this file) seeds exactly
			// one instance of each category this sweep is known to need:
			// `pnt_albedo` (Painter) and `mat_diffuse` (Material).  A
			// category this map has no stand-in for marks the keyword
			// unsatisfiable rather than emitting a guessed value -- see
			// the enumeration note below.
			std::vector<NodeArg> args;
			bool satisfiable = true;
			for( const auto& req : ctrl.ChunkNodeRequirements( String( d.keyword.c_str() ) ) )
			{
				if( !req.isReference ) { satisfiable = false; break; }   // no static default exists

				// Empty referenceCategories is a legal "unrestricted"
				// reference (ConnectionLegality::CategoryAllowed's own
				// convention) -- a Painter is always an admissible choice
				// there.  A non-empty list must contain one of the
				// categories this sweep knows how to stand in for.
				std::string standIn;
				if( req.referenceCategories.empty() )
				{
					standIn = "pnt_albedo";
				}
				else
				{
					for( ChunkCategory c : req.referenceCategories )
					{
						if( c == ChunkCategory::Material ) { standIn = "mat_diffuse"; break; }
						if( c == ChunkCategory::Painter )  { standIn = "pnt_albedo";  break; }
						// {Function} (e.g. function2d_painter's own
						// `function2d` slot): Job.cpp's RegisterPainterDual
						// dual-indexes every successfully-added colour
						// painter into BOTH the painter manager AND the
						// Function2D manager (the two documented exceptions,
						// expression_painter and scalar_painter, are not in
						// play here), so `pnt_albedo` -- an ordinary
						// uniformcolor_painter -- resolves as a Function2D
						// too.  See kNodeExtraRequirements's own comment in
						// EntityTemplates.cpp for the same fact.
						if( c == ChunkCategory::Function ) { standIn = "pnt_albedo"; break; }
					}
				}
				// Enumeration note (this task's audit): every `required`
				// Reference parameter on a Painter/Material-category chunk
				// in ChunkParserRegistry.cpp today carries either
				// `{Painter}` or `{Material}` -- confirmed by inspection.
				// A category neither branch above recognises falls through
				// with an empty `standIn`, which correctly makes the
				// keyword unsatisfiable here (a future chunk adding e.g. a
				// required `{Geometry}` reference needs this map extended,
				// not a new per-keyword special case).
				if( standIn.empty() ) { satisfiable = false; break; }

				NodeArg a;
				a.param = req.param;
				a.value = String( standIn.c_str() );
				args.push_back( a );
			}

			const std::string before = DocText( pJob );
			String out;
			const auto r = ctrl.CreateChunkNode( String( d.keyword.c_str() ), String( d.keyword.c_str() ), args, &out );

			if( isException( d.keyword ) || !satisfiable )
			{
				Check( !r.applied, std::string( "listed exception `" ) + d.keyword + "` refuses" );
				Check( DocText( pJob ) == before,
					std::string( "...byte-identically (" ) + d.keyword + ")" );
				++refusedExpected;
			}
			else
			{
				Check( r.applied, std::string( "`" ) + d.keyword + "` creates ("
					+ r.message.c_str() + ")" );
				if( r.applied )
				{
					Check( GraphHasNode( *pJob->GetCstDocument(), d.keyword.c_str(), std::string( out.c_str() ) ),
						std::string( "...and appears in the graph snapshot (" ) + d.keyword + ")" );
					++created;
				}
			}
		}
		Check( created > 30, std::string( "the sweep created a substantial keyword set (" ) + std::to_string( created ) + ")" );
		Check( refusedExpected == (int)( sizeof( kFileNeeded ) / sizeof( kFileNeeded[0] ) ),
			std::string( "exactly the file-needing keywords refused (" ) + std::to_string( refusedExpected ) + ")" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== EntityTemplatesTest ===\n" );
	TestAllTemplatesInstantiate();
	TestObjectBootstrapMaterial();
	TestDuplicateRoundTrip();
	TestRemoveReferencedRefusal();
	TestPainterEnumerationAndEdit();
	TestDerivedNameCollisionDedup();
	TestDuplicateNonRepeatableParam();
	TestCreateChunkNodeClasses();
	TestCreateChunkNodeNaming();
	TestCreateChunkNodeRefusalsByteIdentical();
	TestCreateChunkNodeUndoRedo();
	TestCreateChunkNodeControllerDiscipline();
	TestCreateChunkNodeMaterialReferenceStandIn();
	TestCreateChunkNodeKeywordSweep();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
