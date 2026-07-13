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
//        it through PainterIntrospection::Inspect.
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
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/PainterIntrospection.h"

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
				PainterIntrospection::Inspect( pJob->GetCstDocument(), String( "pnt_albedo" ) );
			bool found = false;
			for( const CameraProperty& row : rows )
			{
				if( std::string( row.name.c_str() ) == "color" )
				{
					found = true;
					Check( std::string( row.value.c_str() ) == "0.25 0.25 0.25",
						"PainterIntrospection sees the edited color value (got `" + std::string( row.value.c_str() ) + "`)" );
				}
			}
			Check( found, "PainterIntrospection::Inspect surfaces a `color` row for uniformcolor_painter" );
			Check( !rows.empty() && std::string( rows[0].name.c_str() ) == "type" && !rows[0].editable,
				"PainterIntrospection::Inspect's leading row is a read-only `type` identity row" );
		}

		// Path B: the GUI-facing SetSelection + SetProperty(Category::Painter)
		// route (Category::Painter's SetProperty case).
		Check( ctrl.SetSelection( Category::Painter, String( "pnt_albedo" ) ), "SetSelection(Painter, pnt_albedo) succeeds" );
		Check( ctrl.SetProperty( String( "color" ), String( "0.75 0.1 0.1" ) ), "SetProperty(color) on the selected painter succeeds" );

		{
			const std::vector<CameraProperty> rows =
				PainterIntrospection::Inspect( pJob->GetCstDocument(), String( "pnt_albedo" ) );
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

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
