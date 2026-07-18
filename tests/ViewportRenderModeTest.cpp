//////////////////////////////////////////////////////////////////////
//
//  ViewportRenderModeTest.cpp - GUI render modes P1+P2a (docs/gui/RENDER_MODES.md
//    §5, §6): the shared ViewportRenderMode registry, the ephemeral view-mode
//    caster factory, InteractivePelRasterizer::SetViewModeCaster's caster-
//    swap mechanics, SceneEditController::{Set,Get}ViewportRenderMode, and
//    the C-ABI surface (registry accessors + the two controller wrappers),
//    PLUS the P2a BeautyVariant pipeline (CreateBeautyVariantPipeline,
//    IsBeautyVariantMode, the registry's variant* fields + wantsDenoise
//    C-ABI, and the controller's mVariantRasterizer install/leave +
//    preview-scale pin lifecycle).
//
//    R1  Registry enumeration: count >= 6, names unique, index 0 is
//        "preview" and viewportSelectable, "objectmap" present but NOT
//        viewportSelectable.
//    R2  FindViewportRenderModeByName round-trips every registry name
//        (name -> mode -> FindViewportRenderModeInfo -> same name) and
//        rejects an unknown name / null.
//    R3  CreateInteractiveViewModeCaster succeeds for the four data modes
//        (Normals/Depth/Facets/Wireframe) and fails for Preview/ObjectMap
//        (own factories -- see the header doc).
//    R4  C-ABI registry enumeration (RISE_API_GetViewportRenderModeCount /
//        RISE_API_GetViewportRenderModeInfo) matches the core registry 1:1,
//        plus null-safety on the controller wrappers.
//    R5  InteractivePelRasterizer::SetViewModeCaster install / switch /
//        restore does not leak or over-release -- proven via refcount()
//        on every caster involved (the material-preview pipeline's own
//        preview caster, and two distinct view-mode casters).
//    R6  SceneEditController::SetViewportRenderMode in SKELETON mode (null
//        interactive rasterizer) refuses rather than crashing; the getter
//        still reports the "preview" default.
//    R7  SceneEditController end-to-end with a REAL interactive rasterizer:
//        unknown / non-selectable ("objectmap") refusals, a successful
//        switch, same-mode no-op, direct mode-to-mode switching, and
//        restoring "preview".
//    R8  A whole-scene re-derive (scene_variant switch, which calls
//        RebindEditorToJob) resets an active view mode back to "preview" --
//        the ratified "every scene load/reload opens in preview" rule.
//    R9  X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"), DEFAULT ON
//        (2026-07-17): SceneEditController::{Set,Get}ViewportXray in
//        skeleton mode (refuses; getter still reports the TRUE default),
//        live round-trip (applies immediately with NO caster rebuild --
//        including while "preview" is active, stamping the studio preview
//        caster itself -- survives a mode switch), the C-ABI wrappers, and
//        reset-to-TRUE (the default) alongside the mode reset on a
//        whole-scene re-derive.
//    R10 External review P2: SetViewportRenderMode / SetViewportXray
//        reset a QUEUED polish pass (PolishState::FinalRegularRunning,
//        reached synchronously via a camera-orbit gesture's OnPointerUp
//        -- no render thread ever started) rather than leaving it stale
//        for the next pass to run as a 4-SPP polish of the NEW mode/
//        x-ray state.  Observed via the public GetRefinementStatus()
//        (Polishing -> Idle across the switch).
//    R11 P2a registry rows: deep_reflect/direct carry consistent
//        variant* fields (nonzero, casterFactory=false, viewportSelectable=
//        true, wantsDenoise=true), IsBeautyVariantMode agrees, and every
//        P1 (non-variant) row's variant* fields are all zero.
//    R12 P2a CreateBeautyVariantPipeline succeeds for deep_reflect/direct
//        (rasterizer + caster both non-null, caster is a plain RayCaster
//        not InteractiveMaterialPreviewRayCaster/InteractiveViewModeRayCaster)
//        and refuses for every non-variant mode (Preview/ObjectMap/the four
//        P1 data modes), out-params zeroed on refusal.
//    R13 P2a RISE_API_GetViewportRenderModeWantsDenoise matches the core
//        registry 1:1, plus out-of-range/null-out refusal.
//    R14 P2a controller lifecycle: skeleton mode refuses a variant-mode
//        switch; live mode reaches deep_reflect/direct (GetViewportRenderMode
//        echoes the name) and the preview-scale divisor PINS to the mode's
//        variantScaleDivisor -- surviving a gesture (OnPointerDown/Up) that
//        would otherwise reset it -- then UNPINS (a gesture's reset takes
//        effect again) after switching to a non-variant mode.
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
#include <set>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		return dir + name;
	}

	Job* LoadScene( const std::string& text, const std::string& path )
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

	const char* const kPlainScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n\n"
		"lambertian_material\n{\nname m\nreflectance white\n}\n\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial m\n}\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	// Same as above, plus a `night` scene_variant that overrides the
	// material's reflectance -- material chunks are the ones that carry
	// the `variant` tag (ChunkParserRegistry.cpp AddVariantTagParam), so
	// this mirrors SceneEditorSceneVariantTest.cpp's fixture shape (a
	// `night`-tagged override of `m`) rather than tagging the light.
	const char* const kVariantScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname white\ncolor 0.8 0.8 0.8\n}\n\n"
		"uniformcolor_painter\n{\nname black\ncolor 0.02 0.02 0.02\n}\n\n"
		"lambertian_material\n{\nname m\nreflectance white\n}\n"
		"lambertian_material\n{\nname m\nvariant night\nreflectance black\n}\n\n"
		"sphere_geometry\n{\nname s\nradius 1\n}\n"
		"standard_object\n{\nname obj\ngeometry s\nmaterial m\n}\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n"
		"scene_variant\n{\nname night\n}\n";

	//------------------------------------------------------------------
	// R1: registry enumeration.
	//------------------------------------------------------------------
	void TestRegistryEnumeration()
	{
		std::printf( "R1: registry enumeration...\n" );
		unsigned int count = 0;
		const ViewportRenderModeInfo* modes = GetViewportRenderModes( count );
		Check( modes != nullptr, "registry pointer non-null" );
		Check( count >= 6, "at least the 6 P1 modes are registered" );

		Check( count > 0 && std::string( modes[0].name ) == "preview", "index 0 is \"preview\"" );
		Check( count > 0 && modes[0].viewportSelectable, "\"preview\" is viewport-selectable" );

		std::set<std::string> names;
		bool sawObjectMap = false, objectMapSelectable = true;
		for( unsigned int i = 0; i < count; ++i )
		{
			Check( modes[i].name != nullptr && modes[i].name[0] != '\0', "every entry has a non-empty name" );
			names.insert( modes[i].name );
			if( std::string( modes[i].name ) == "objectmap" )
			{
				sawObjectMap = true;
				objectMapSelectable = modes[i].viewportSelectable;
			}
		}
		Check( names.size() == count, "every registry name is unique" );
		Check( sawObjectMap, "\"objectmap\" is registered" );
		Check( !objectMapSelectable, "\"objectmap\" is NOT viewport-selectable (own palette-lifecycle pipeline)" );
	}

	//------------------------------------------------------------------
	// R2: FindViewportRenderModeByName round-trips every name; rejects junk.
	//------------------------------------------------------------------
	void TestFindByName()
	{
		std::printf( "R2: FindViewportRenderModeByName round-trip...\n" );
		unsigned int count = 0;
		const ViewportRenderModeInfo* modes = GetViewportRenderModes( count );
		for( unsigned int i = 0; i < count; ++i )
		{
			const ViewportRenderModeInfo* byName = FindViewportRenderModeByName( modes[i].name );
			Check( byName == &modes[i], "FindViewportRenderModeByName finds the SAME registry entry" );
			const ViewportRenderModeInfo* byMode = FindViewportRenderModeInfo( modes[i].mode );
			Check( byMode == &modes[i], "FindViewportRenderModeInfo round-trips the same entry via the enum" );
		}
		Check( FindViewportRenderModeByName( "nonsense" ) == nullptr, "unknown name rejected" );
		Check( FindViewportRenderModeByName( nullptr ) == nullptr, "null name rejected" );
		Check( FindViewportRenderModeByName( "" ) == nullptr, "empty name rejected" );
	}

	//------------------------------------------------------------------
	// R3: CreateInteractiveViewModeCaster succeeds for the 4 data modes,
	// fails for Preview / ObjectMap.
	//------------------------------------------------------------------
	void TestCasterFactory()
	{
		std::printf( "R3: CreateInteractiveViewModeCaster success/failure per mode...\n" );
		const ViewportRenderMode dataModes[4] = {
			ViewportRenderMode::Normals, ViewportRenderMode::Depth,
			ViewportRenderMode::Facets,  ViewportRenderMode::Wireframe
		};
		for( unsigned int i = 0; i < 4; ++i )
		{
			IRayCaster* caster = nullptr;
			Check( CreateInteractiveViewModeCaster( dataModes[i], &caster ), "data mode caster factory succeeds" );
			Check( caster != nullptr, "data mode caster is non-null" );
			if( caster ) caster->release();
		}

		IRayCaster* previewCaster = reinterpret_cast<IRayCaster*>( 1 );   // sentinel to prove it's zeroed
		Check( !CreateInteractiveViewModeCaster( ViewportRenderMode::Preview, &previewCaster ), "Preview refused (restore the platform casters instead)" );
		Check( previewCaster == nullptr, "out-param zeroed on refusal (Preview)" );

		IRayCaster* objectMapCaster = reinterpret_cast<IRayCaster*>( 1 );
		Check( !CreateInteractiveViewModeCaster( ViewportRenderMode::ObjectMap, &objectMapCaster ), "ObjectMap refused (own palette-lifecycle factory)" );
		Check( objectMapCaster == nullptr, "out-param zeroed on refusal (ObjectMap)" );
	}

	//------------------------------------------------------------------
	// R4: C-ABI registry enumeration matches the core registry 1:1, plus
	// null-safety on the controller wrappers.
	//------------------------------------------------------------------
	void TestCAbi()
	{
		std::printf( "R4: C-ABI registry + controller-wrapper null-safety...\n" );
		unsigned int coreCount = 0;
		const ViewportRenderModeInfo* modes = GetViewportRenderModes( coreCount );

		const unsigned int abiCount = RISE_API_GetViewportRenderModeCount();
		Check( abiCount == coreCount, "RISE_API_GetViewportRenderModeCount matches the core registry" );

		for( unsigned int i = 0; i < coreCount; ++i )
		{
			const char* name = nullptr; const char* title = nullptr;
			const char* question = nullptr; bool selectable = !modes[i].viewportSelectable;
			Check( RISE_API_GetViewportRenderModeInfo( i, &name, &title, &question, &selectable ),
				"RISE_API_GetViewportRenderModeInfo succeeds in range" );
			Check( name == modes[i].name, "name pointer matches the static registry string" );
			Check( title == modes[i].title, "title pointer matches the static registry string" );
			Check( question == modes[i].question, "question pointer matches the static registry string" );
			Check( selectable == modes[i].viewportSelectable, "viewportSelectable matches" );
		}

		// Bad index: refused, outputs untouched.
		const char* sentinelName = reinterpret_cast<const char*>( 1 );
		bool sentinelSel = true;
		Check( !RISE_API_GetViewportRenderModeInfo( coreCount, &sentinelName, nullptr, nullptr, &sentinelSel ),
			"out-of-range index refused" );
		Check( sentinelName == reinterpret_cast<const char*>( 1 ) && sentinelSel == true,
			"out-parameters untouched on refusal" );
		// Nullable out-parameters: passing all-null must not crash.
		Check( RISE_API_GetViewportRenderModeInfo( 0, nullptr, nullptr, nullptr, nullptr ),
			"all-null out-parameters accepted (nullable-tolerant)" );

		// Controller wrappers on a null controller.
		Check( !RISE_API_SceneEditController_SetViewportRenderMode( nullptr, "depth" ), "Set wrapper refuses null controller" );
		Check( std::string( RISE_API_SceneEditController_GetViewportRenderMode( nullptr ) ) == "preview",
			"Get wrapper reports \"preview\" on a null controller" );

		// X-ray axis C-ABI wrappers on a null controller.
		Check( !RISE_API_SceneEditController_SetViewportXray( nullptr, true ), "xray Set wrapper refuses null controller" );
		bool xraySentinel = true;
		Check( !RISE_API_SceneEditController_GetViewportXray( nullptr, &xraySentinel ), "xray Get wrapper refuses null controller" );
		Check( xraySentinel == true, "xray Get wrapper leaves *out untouched on refusal" );
		Check( !RISE_API_SceneEditController_GetViewportXray( nullptr, nullptr ), "xray Get wrapper refuses null out-param too" );
	}

	//------------------------------------------------------------------
	// R5: InteractivePelRasterizer::SetViewModeCaster install / switch /
	// restore leaks nothing and over-releases nothing (refcount-proven).
	//------------------------------------------------------------------
	void TestSetViewModeCasterRefcounting()
	{
		std::printf( "R5: InteractivePelRasterizer::SetViewModeCaster refcount discipline...\n" );

		IRasterizer* pRasterizer = nullptr;
		IRayCaster*  pPreview    = nullptr;
		IRayCaster*  pPolish     = nullptr;
		Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ),
			"material preview pipeline builds" );
		if( !pRasterizer || !pPreview || !pPolish )
		{
			if( pRasterizer ) pRasterizer->release();
			if( pPreview )    pPreview->release();
			if( pPolish )     pPolish->release();
			return;
		}

		InteractivePelRasterizer* impl = dynamic_cast<InteractivePelRasterizer*>( pRasterizer );
		Check( impl != nullptr, "rasterizer downcasts to InteractivePelRasterizer" );
		Check( pPreview->refcount() == 2, "preview caster refcount == 2 right after the factory (factory ref + rasterizer ref)" );
		Check( !( impl && impl->HasViewModeCaster() ), "no view mode installed initially" );

		IRayCaster* viewA = nullptr;
		Check( CreateInteractiveViewModeCaster( ViewportRenderMode::Depth, &viewA ), "depth caster builds" );
		IRayCaster* viewB = nullptr;
		Check( CreateInteractiveViewModeCaster( ViewportRenderMode::Normals, &viewB ), "normals caster builds" );
		Check( viewA && viewA->refcount() == 1, "fresh viewA refcount == 1 (factory ref only)" );
		Check( viewB && viewB->refcount() == 1, "fresh viewB refcount == 1 (factory ref only)" );

		if( impl && viewA && viewB )
		{
			impl->SetViewModeCaster( viewA );
			Check( impl->HasViewModeCaster(), "HasViewModeCaster true after install" );
			Check( viewA->refcount() == 2, "viewA addref'd by install (factory ref + rasterizer ref)" );
			Check( pPreview->refcount() == 2, "preview caster's refcount untouched by install (transferred, not released)" );

			// Direct mode-to-mode switch (no explicit clear in between).
			impl->SetViewModeCaster( viewB );
			Check( impl->HasViewModeCaster(), "still installed after a direct switch" );
			Check( viewA->refcount() == 1, "viewA released back to factory-ref-only after the switch" );
			Check( viewB->refcount() == 2, "viewB addref'd by the switch" );

			// Restore.
			impl->SetViewModeCaster( nullptr );
			Check( !impl->HasViewModeCaster(), "HasViewModeCaster false after restore" );
			Check( viewB->refcount() == 1, "viewB released back to factory-ref-only after restore" );
			Check( pPreview->refcount() == 2, "preview caster refcount restored to 2 (factory ref + rasterizer ref)" );

			// Clearing again is a documented no-op.
			impl->SetViewModeCaster( nullptr );
			Check( !impl->HasViewModeCaster(), "double-clear stays a no-op" );
			Check( pPreview->refcount() == 2, "double-clear doesn't touch the preview caster's refcount" );
		}

		safe_release( viewA );
		safe_release( viewB );
		pRasterizer->release();
		Check( pPreview->refcount() == 1, "rasterizer teardown released its preview-caster ref (only our local ref remains)" );
		Check( pPolish->refcount() == 1, "rasterizer teardown released its polish-caster ref (only our local ref remains)" );
		pPreview->release();
		pPolish->release();
	}

	//------------------------------------------------------------------
	// R6: SceneEditController in SKELETON mode (no interactive rasterizer).
	//------------------------------------------------------------------
	void TestSkeletonMode()
	{
		std::printf( "R6: SceneEditController::SetViewportRenderMode in skeleton mode...\n" );
		const std::string tmp = TempPath( "vrm_r6.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );   // skeleton: no interactive rasterizer
		Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "GetViewportRenderMode reports \"preview\" by default" );
		Check( !ctrl.SetViewportRenderMode( "depth" ), "SetViewportRenderMode refused in skeleton mode (a valid name)" );
		Check( !ctrl.SetViewportRenderMode( "nonsense" ), "SetViewportRenderMode also refused for an unknown name" );
		Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "mode stays \"preview\" after refused calls" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// R7: end-to-end with a REAL interactive rasterizer.
	//------------------------------------------------------------------
	void TestLiveModeSwitching()
	{
		std::printf( "R7: SceneEditController::SetViewportRenderMode end-to-end...\n" );
		const std::string tmp = TempPath( "vrm_r7.RISEscene" );
		Job* pJob = LoadScene( kPlainScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) { return; }

		IRasterizer* pRasterizer = nullptr;
		IRayCaster*  pPreview    = nullptr;
		IRayCaster*  pPolish     = nullptr;
		Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ), "pipeline builds" );
		if( !pRasterizer ) { pJob->release(); std::remove( tmp.c_str() ); return; }

		{
			SceneEditController ctrl( *pJob, pRasterizer );

			Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "starts at \"preview\"" );
			Check( !ctrl.SetViewportRenderMode( "nonsense" ), "unknown name refused" );
			Check( !ctrl.SetViewportRenderMode( "objectmap" ), "non-viewport-selectable name refused" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "still \"preview\" after refused calls" );

			Check( ctrl.SetViewportRenderMode( "depth" ), "switch to \"depth\" succeeds" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "GetViewportRenderMode reports \"depth\"" );

			// Same-mode call is a no-op that still reports success.
			Check( ctrl.SetViewportRenderMode( "depth" ), "re-setting the SAME mode succeeds (no-op)" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "still \"depth\" after the no-op call" );

			// Direct mode-to-mode switch.
			Check( ctrl.SetViewportRenderMode( "normals" ), "direct switch to \"normals\" succeeds" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "normals", "GetViewportRenderMode reports \"normals\"" );

			Check( ctrl.SetViewportRenderMode( "wireframe" ), "direct switch to \"wireframe\" succeeds" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "wireframe", "GetViewportRenderMode reports \"wireframe\"" );

			Check( ctrl.SetViewportRenderMode( "facets" ), "direct switch to \"facets\" succeeds" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "facets", "GetViewportRenderMode reports \"facets\"" );

			// Restore preview.
			Check( ctrl.SetViewportRenderMode( "preview" ), "restore \"preview\" succeeds" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "GetViewportRenderMode reports \"preview\" again" );

			// Idempotent: restoring preview again is a no-op that still succeeds.
			Check( ctrl.SetViewportRenderMode( "preview" ), "re-restoring \"preview\" is a no-op success" );
		}

		pRasterizer->release();
		pPreview->release();
		pPolish->release();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// R8: a whole-scene re-derive (scene_variant switch) resets an active
	// view mode back to "preview" (RebindEditorToJob's every-load reset).
	//------------------------------------------------------------------
	void TestSceneReloadResetsToPreview()
	{
		std::printf( "R8: scene_variant switch resets the viewport render mode to \"preview\"...\n" );
		using Cat = SceneEditController::Category;

		const std::string tmp = TempPath( "vrm_r8.RISEscene" );
		Job* pJob = LoadScene( kVariantScene, tmp );
		Check( pJob != nullptr, "variant fixture loads" );
		if( !pJob ) return;

		IRasterizer* pRasterizer = nullptr;
		IRayCaster*  pPreview    = nullptr;
		IRayCaster*  pPolish     = nullptr;
		Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ), "pipeline builds" );
		if( !pRasterizer ) { pJob->release(); std::remove( tmp.c_str() ); return; }

		{
			SceneEditController ctrl( *pJob, pRasterizer );

			Check( ctrl.SetViewportRenderMode( "depth" ), "switch to \"depth\" before the variant switch" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "mode is \"depth\" pre-switch" );

			// A scene_variant switch ClearAll's + rebuilds the Scene + managers
			// (RederiveCstWithVariant) and calls RebindEditorToJob -- exactly
			// the "whole-scene (re)bind" RebindEditorToJob's own doc says
			// resets the viewport to Preview.
			Check( ctrl.SetSelection( Cat::SceneVariant, String( "night" ) ), "SetSelection(night) re-derives clean" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "preview",
				"viewport render mode reset to \"preview\" by the whole-scene re-derive" );

			// A second switch (back to base) also re-derives; mode stays preview
			// (nothing to reset FROM this time) and the controller is still sane.
			Check( ctrl.SetSelection( Cat::SceneVariant, String( "(base)" ) ), "switch back to (base) re-derives clean" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "still \"preview\" after switching back" );

			// The reset didn't wedge the controller -- a fresh mode switch still works.
			Check( ctrl.SetViewportRenderMode( "facets" ), "mode switch still works after the re-derives" );
			Check( std::string( ctrl.GetViewportRenderMode() ) == "facets", "GetViewportRenderMode reports \"facets\"" );
			Check( ctrl.SetViewportRenderMode( "preview" ), "restore \"preview\" before teardown" );
		}

		pRasterizer->release();
		pPreview->release();
		pPolish->release();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// R9: X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"), DEFAULT ON
	// (2026-07-17) -- skeleton refusal (getter still reports the TRUE
	// default), live round-trip with NO caster rebuild (applies to
	// "preview" too -- verified by reading the actual preview caster's
	// RayCaster::GetXrayViewResolve()), and reset-to-TRUE on rebind.
	//------------------------------------------------------------------
	void TestXrayAxis()
	{
		std::printf( "R9: X-ray axis set/get round-trip + reset-on-rebind...\n" );

		// Skeleton mode: refused, not crashing; getter still reports the
		// DEFAULT (true -- x-ray is default-ON).
		{
			const std::string tmp = TempPath( "vrm_r9_skeleton.RISEscene" );
			Job* pJob = LoadScene( kPlainScene, tmp );
			Check( pJob != nullptr, "R9 skeleton fixture loads" );
			if( pJob )
			{
				SceneEditController ctrl( *pJob, nullptr );   // skeleton: no interactive rasterizer
				Check( ctrl.GetViewportXray(), "GetViewportXray reports TRUE by default (skeleton) -- x-ray is default-ON" );
				Check( !ctrl.SetViewportXray( false ), "SetViewportXray refused in skeleton mode" );
				Check( ctrl.GetViewportXray(), "xray flag stays true after a refused skeleton-mode call" );
				pJob->release();
				std::remove( tmp.c_str() );
			}
		}

		// Live controller: round-trip + compose with the render-mode switch.
		{
			const std::string tmp = TempPath( "vrm_r9_live.RISEscene" );
			Job* pJob = LoadScene( kVariantScene, tmp );
			Check( pJob != nullptr, "R9 live fixture loads" );
			if( !pJob ) return;

			IRasterizer* pRasterizer = nullptr;
			IRayCaster*  pPreview    = nullptr;
			IRayCaster*  pPolish     = nullptr;
			Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ), "R9 pipeline builds" );
			if( !pRasterizer ) { pJob->release(); std::remove( tmp.c_str() ); return; }

			RayCaster* pPreviewRC = dynamic_cast<RayCaster*>( pPreview );
			Check( pPreviewRC != nullptr, "R9 preview caster downcasts to RayCaster" );

			{
				SceneEditController ctrl( *pJob, pRasterizer );
				using Cat = SceneEditController::Category;

				Check( ctrl.GetViewportXray(), "starts at xray=true (default-ON)" );
				if( pPreviewRC ) {
					Check( pPreviewRC->GetXrayViewResolve(), "constructing the controller stamps the DEFAULT true onto the preview caster (first-attach stamp)" );
				}

				// Set OFF while "preview" is active: x-ray now applies to
				// EVERY mode including preview (caster-layer resolution,
				// no per-mode caster rebuild) -- so this is a plain flag
				// stamp that still succeeds and is immediately visible on
				// the actual preview caster.
				Check( ctrl.SetViewportXray( false ), "SetViewportXray(false) succeeds while \"preview\" is active" );
				Check( !ctrl.GetViewportXray(), "GetViewportXray reports false after the set" );
				if( pPreviewRC ) {
					Check( !pPreviewRC->GetXrayViewResolve(), "the studio preview caster itself reflects the flip -- x-ray composes with \"preview\" now" );
				}

				// Same-value call is a documented no-op that still succeeds.
				Check( ctrl.SetViewportXray( false ), "re-setting the SAME xray value succeeds (no-op)" );
				Check( !ctrl.GetViewportXray(), "still false after the no-op call" );

				// Switching to a data mode: the newly-built caster is
				// immediately re-stamped with the CURRENT flag by
				// InteractivePelRasterizer::SetViewModeCaster -- no
				// separate re-apply needed on the controller side.
				Check( ctrl.SetViewportRenderMode( "depth" ), "switch to \"depth\" with xray already false" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "mode is \"depth\"" );
				Check( !ctrl.GetViewportXray(), "xray flag survives the mode switch" );

				// Flip xray ON while a data mode is active: no caster
				// rebuild, just InteractivePelRasterizer::SetXrayView
				// stamping every caster the rasterizer holds.
				Check( ctrl.SetViewportXray( true ), "SetViewportXray(true) succeeds while \"depth\" is active" );
				Check( ctrl.GetViewportXray(), "GetViewportXray reports true after the flip" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "mode stays \"depth\" across the xray flip" );

				// A whole-scene re-derive resets BOTH the mode AND the xray
				// flag to their DEFAULTS ("preview" / true).
				Check( ctrl.SetSelection( Cat::SceneVariant, String( "night" ) ), "R9 SetSelection(night) re-derives clean" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "mode reset to \"preview\" by the re-derive" );
				Check( ctrl.GetViewportXray(),
					"MONEY ASSERTION: xray flag reset to TRUE (the default) by the SAME whole-scene re-derive" );
				if( pPreviewRC ) {
					Check( pPreviewRC->GetXrayViewResolve(), "the re-derive's reset is stamped onto the preview caster too" );
				}

				// C-ABI round-trip through the live controller.
				Check( RISE_API_SceneEditController_SetViewportXray( &ctrl, false ), "C-ABI SetViewportXray succeeds" );
				bool abiOut = true;
				Check( RISE_API_SceneEditController_GetViewportXray( &ctrl, &abiOut ), "C-ABI GetViewportXray succeeds" );
				Check( abiOut == false, "C-ABI GetViewportXray reflects the C-ABI set" );
				Check( RISE_API_SceneEditController_SetViewportXray( &ctrl, true ), "C-ABI SetViewportXray(true) restores" );

				Check( ctrl.SetViewportRenderMode( "preview" ), "restore \"preview\" before teardown" );
			}

			pRasterizer->release();
			pPreview->release();
			pPolish->release();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}

	//------------------------------------------------------------------
	// R10: external-review P2 -- a mode/x-ray switch mid-polish must NOT
	// leave `mPolishState` stale.  Reachable ENTIRELY SYNCHRONOUSLY, with
	// no render thread ever started (ctrl.Start() is never called): a
	// camera-motion gesture's OnPointerUp stores PolishState::
	// FinalRegularRunning directly (SceneEditController.cpp, "wasMotion"
	// branch) -- reaching the LATER PolishState::PolishQueued value needs
	// the render loop thread to actually run a pass (RenderLoop's post-
	// roll transition), which is NOT reachable without starting that
	// thread and racing it, so THIS test is honestly scoped to the
	// FinalRegularRunning entry point. That is still full coverage of the
	// actual fix: SetViewportRenderMode / SetViewportXray's new reset
	// (`mPolishState.store( PolishState::None, ... )`) is UNCONDITIONAL --
	// the exact same store statement retires FinalRegularRunning and
	// PolishQueued alike, so there is no separate code path for the
	// PolishQueued case this test would miss.
	//
	// GetRefinementStatus() is the observation point: with `mRendering`
	// false (no thread) and the preview scale already at its minimum
	// (which OnPointerUp sets right before queuing the polish state),
	// `polish != PolishState::None` alone is enough for it to report
	// RefinementPhase::Polishing (SceneEditController.cpp ~line 858) --
	// so this is a real, documented, public-API-observable symptom of
	// the bug, not a peek at the private state machine.
	//------------------------------------------------------------------
	void TestPolishStateResetOnSwitch()
	{
		std::printf( "R10: mode/x-ray switch resets a queued polish pass (external review P2)...\n" );

		auto driveOrbitGesture = []( SceneEditController& ctrl )
		{
			// A plain camera orbit: no object selection needed, and
			// camera edits apply immediately (no cancel-and-park wait --
			// see OnPointerMove's own doc), so this is safe to drive with
			// no render thread running.
			ctrl.SetTool( SceneEditController::Tool::OrbitCamera );
			ctrl.OnPointerDown( Point2( 10, 10 ) );
			ctrl.OnPointerMove( Point2( 18, 14 ) );   // a real (dx,dy) != (0,0)
			ctrl.OnPointerUp( Point2( 18, 14 ) );
		};

		// Sub-test (a): SetViewportRenderMode.
		{
			const std::string tmp = TempPath( "vrm_r10_mode.RISEscene" );
			Job* pJob = LoadScene( kPlainScene, tmp );
			Check( pJob != nullptr, "R10a fixture loads" );
			if( pJob )
			{
				IRasterizer* pRasterizer = nullptr;
				IRayCaster*  pPreview    = nullptr;
				IRayCaster*  pPolish     = nullptr;
				Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ),
				       "R10a pipeline builds" );
				if( pRasterizer )
				{
					{
						SceneEditController ctrl( *pJob, pRasterizer );
						driveOrbitGesture( ctrl );

						unsigned int divisor = 0;
						Check( ctrl.GetRefinementStatus( divisor ) == SceneEditController::RefinementPhase::Polishing,
						       "orbit gesture's OnPointerUp queues a polish pass (Polishing, no thread running)" );

						Check( ctrl.SetViewportRenderMode( "depth" ), "switch to \"depth\" while polish is queued" );

						unsigned int divisorAfter = 0;
						Check( ctrl.GetRefinementStatus( divisorAfter ) == SceneEditController::RefinementPhase::Idle,
						       "MONEY ASSERTION: the mode switch retires the queued polish -- Idle, not still Polishing "
						       "(external review P2: the FIRST post-switch pass must not run as a 4-SPP polish "
						       "pass of the NEW mode)" );
						Check( std::string( ctrl.GetViewportRenderMode() ) == "depth", "mode is \"depth\" after the switch" );
					}
					pRasterizer->release();
					pPreview->release();
					pPolish->release();
				}
				pJob->release();
				std::remove( tmp.c_str() );
			}
		}

		// Sub-test (b): SetViewportXray -- same fix, same assertion shape.
		{
			const std::string tmp = TempPath( "vrm_r10_xray.RISEscene" );
			Job* pJob = LoadScene( kPlainScene, tmp );
			Check( pJob != nullptr, "R10b fixture loads" );
			if( pJob )
			{
				IRasterizer* pRasterizer = nullptr;
				IRayCaster*  pPreview    = nullptr;
				IRayCaster*  pPolish     = nullptr;
				Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ),
				       "R10b pipeline builds" );
				if( pRasterizer )
				{
					{
						SceneEditController ctrl( *pJob, pRasterizer );
						driveOrbitGesture( ctrl );

						unsigned int divisor = 0;
						Check( ctrl.GetRefinementStatus( divisor ) == SceneEditController::RefinementPhase::Polishing,
						       "orbit gesture's OnPointerUp queues a polish pass (Polishing, no thread running)" );

						// Toggle the CURRENT flag off (default is ON) so this is a
						// real flip, not the same-value no-op short-circuit.
						Check( ctrl.SetViewportXray( !ctrl.GetViewportXray() ), "flip x-ray while polish is queued" );

						unsigned int divisorAfter = 0;
						Check( ctrl.GetRefinementStatus( divisorAfter ) == SceneEditController::RefinementPhase::Idle,
						       "MONEY ASSERTION: the x-ray flip retires the queued polish -- Idle, not still Polishing" );
					}
					pRasterizer->release();
					pPreview->release();
					pPolish->release();
				}
				pJob->release();
				std::remove( tmp.c_str() );
			}
		}
	}
	//------------------------------------------------------------------
	// R11: P2a registry rows -- variant* fields consistent, denoise flags.
	//------------------------------------------------------------------
	void TestVariantRegistryRows()
	{
		std::printf( "R11: P2a registry variant* fields + wantsDenoise consistency...\n" );
		unsigned int count = 0;
		const ViewportRenderModeInfo* modes = GetViewportRenderModes( count );

		bool sawDeepReflect = false, sawDirect = false;
		for( unsigned int i = 0; i < count; ++i )
		{
			const ViewportRenderModeInfo& info = modes[i];
			const bool isVariant = IsBeautyVariantMode( info.mode );
			// variant* fields are all-zero for non-variant, all-nonzero for variant.
			const bool allZero = ( info.variantScaleDivisor == 0
			                     && info.variantMaxBounces == 0
			                     && info.variantSamplesPerPass == 0 );
			const bool allNonZero = ( info.variantScaleDivisor > 0
			                        && info.variantMaxBounces > 0
			                        && info.variantSamplesPerPass > 0 );
			Check( isVariant ? allNonZero : allZero,
			       std::string( info.name ) + ": variant* fields are all-zero (non-variant) or all-nonzero (variant), never mixed" );
			// IsBeautyVariantMode is keyed on variantSamplesPerPass > 0 --
			// cross-check it agrees with the direct field read.
			Check( isVariant == ( info.variantSamplesPerPass > 0 ),
			       std::string( info.name ) + ": IsBeautyVariantMode agrees with variantSamplesPerPass > 0" );
			if( isVariant )
			{
				Check( info.wantsDenoise, std::string( info.name ) + ": a variant mode wants denoise" );
				Check( !info.casterFactory, std::string( info.name ) + ": a variant mode is NOT a casterFactory row (routes through CreateBeautyVariantPipeline)" );
				Check( info.viewportSelectable, std::string( info.name ) + ": a variant mode is viewport-selectable" );
			}
			if( std::string( info.name ) == "deep_reflect" )
			{
				sawDeepReflect = true;
				Check( info.variantScaleDivisor == 4, "deep_reflect: variantScaleDivisor == 4 (quarter-res)" );
				Check( info.variantMaxBounces == 24, "deep_reflect: variantMaxBounces == 24" );
				Check( info.variantSamplesPerPass == 16, "deep_reflect: variantSamplesPerPass == 16" );
			}
			else if( std::string( info.name ) == "direct" )
			{
				sawDirect = true;
				Check( info.variantScaleDivisor == 2, "direct: variantScaleDivisor == 2 (half-res)" );
				Check( info.variantMaxBounces == 1, "direct: variantMaxBounces == 1 (direct lighting only)" );
				Check( info.variantSamplesPerPass == 8, "direct: variantSamplesPerPass == 8" );
			}
		}
		Check( sawDeepReflect, "\"deep_reflect\" is registered" );
		Check( sawDirect, "\"direct\" is registered" );
		Check( count >= 8, "at least the 6 P1 + 2 P2a modes are registered" );
	}

	//------------------------------------------------------------------
	// R12: CreateBeautyVariantPipeline succeeds for deep_reflect/direct,
	// refuses for every non-variant mode.
	//------------------------------------------------------------------
	void TestBeautyVariantPipelineFactory()
	{
		std::printf( "R12: CreateBeautyVariantPipeline success/failure per mode...\n" );
		const ViewportRenderMode variantModes[2] = {
			ViewportRenderMode::DeepReflect, ViewportRenderMode::Direct
		};
		for( unsigned int i = 0; i < 2; ++i )
		{
			IRasterizer* rast   = nullptr;
			IRayCaster*  caster = nullptr;
			Check( CreateBeautyVariantPipeline( variantModes[i], &rast, &caster ),
			       "variant mode pipeline factory succeeds" );
			Check( rast != nullptr, "variant mode rasterizer is non-null" );
			Check( caster != nullptr, "variant mode caster is non-null" );
			// The caster must be a PLAIN RayCaster (production-real shading),
			// not the studio-preview/view-mode subclasses.
			RayCaster* concreteRC = dynamic_cast<RayCaster*>( caster );
			Check( concreteRC != nullptr, "variant mode caster downcasts to the concrete RayCaster" );
			if( rast )   rast->release();
			if( caster ) caster->release();
		}

		const ViewportRenderMode nonVariantModes[6] = {
			ViewportRenderMode::Preview, ViewportRenderMode::ObjectMap,
			ViewportRenderMode::Normals, ViewportRenderMode::Depth,
			ViewportRenderMode::Facets,  ViewportRenderMode::Wireframe
		};
		for( unsigned int i = 0; i < 6; ++i )
		{
			IRasterizer* rast   = reinterpret_cast<IRasterizer*>( 1 );   // sentinel
			IRayCaster*  caster = reinterpret_cast<IRayCaster*>( 1 );
			Check( !CreateBeautyVariantPipeline( nonVariantModes[i], &rast, &caster ),
			       "non-variant mode refused by CreateBeautyVariantPipeline" );
			Check( rast == nullptr && caster == nullptr, "out-params zeroed on refusal" );
		}
	}

	//------------------------------------------------------------------
	// R13: RISE_API_GetViewportRenderModeWantsDenoise matches the core
	// registry 1:1, plus out-of-range/null-out refusal.
	//------------------------------------------------------------------
	void TestWantsDenoiseCAbi()
	{
		std::printf( "R13: RISE_API_GetViewportRenderModeWantsDenoise parity...\n" );
		unsigned int count = 0;
		const ViewportRenderModeInfo* modes = GetViewportRenderModes( count );

		for( unsigned int i = 0; i < count; ++i )
		{
			bool wantsDenoise = !modes[i].wantsDenoise;   // start inverted so a no-op is detectable
			Check( RISE_API_GetViewportRenderModeWantsDenoise( i, &wantsDenoise ),
			       "RISE_API_GetViewportRenderModeWantsDenoise succeeds in range" );
			Check( wantsDenoise == modes[i].wantsDenoise, "wantsDenoise matches the core registry" );
		}

		bool sentinel = true;
		Check( !RISE_API_GetViewportRenderModeWantsDenoise( count, &sentinel ), "out-of-range index refused" );
		Check( sentinel == true, "out-param untouched on refusal" );
		Check( !RISE_API_GetViewportRenderModeWantsDenoise( 0, nullptr ), "null out-param refused" );
	}

	//------------------------------------------------------------------
	// R14: controller variant-mode lifecycle -- skeleton refusal, live
	// reachability, and the preview-scale pin surviving a gesture.
	//------------------------------------------------------------------
	void TestControllerVariantModeLifecycle()
	{
		std::printf( "R14: SceneEditController BeautyVariant mode lifecycle...\n" );

		// Skeleton mode: refused like every other mode switch.
		{
			const std::string tmp = TempPath( "vrm_r14_skeleton.RISEscene" );
			Job* pJob = LoadScene( kPlainScene, tmp );
			Check( pJob != nullptr, "R14 skeleton fixture loads" );
			if( pJob )
			{
				SceneEditController ctrl( *pJob, nullptr );
				Check( !ctrl.SetViewportRenderMode( "deep_reflect" ), "SetViewportRenderMode(\"deep_reflect\") refused in skeleton mode" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "preview", "mode stays \"preview\" after the refused call" );
				pJob->release();
				std::remove( tmp.c_str() );
			}
		}

		// Live controller: reach both variant modes; the preview-scale
		// divisor pins to each mode's variantScaleDivisor and SURVIVES a
		// gesture that would otherwise reset it (OnPointerDown bumps to
		// kPreviewScaleMotionStart, OnPointerUp resets to kPreviewScaleMin
		// -- see SceneEditController.h's mPreviewScalePinned doc); leaving
		// the variant mode unpins, and the SAME gesture then DOES reset
		// the divisor, proving the pin is lifted, not just coincidentally
		// unaffected.
		{
			const std::string tmp = TempPath( "vrm_r14_live.RISEscene" );
			Job* pJob = LoadScene( kPlainScene, tmp );
			Check( pJob != nullptr, "R14 live fixture loads" );
			if( !pJob ) return;

			IRasterizer* pRasterizer = nullptr;
			IRayCaster*  pPreview    = nullptr;
			IRayCaster*  pPolish     = nullptr;
			Check( CreateInteractiveMaterialPreviewPipeline( &pRasterizer, &pPreview, &pPolish ), "R14 pipeline builds" );
			if( !pRasterizer ) { pJob->release(); std::remove( tmp.c_str() ); return; }

			auto driveOrbitGesture = []( SceneEditController& ctrl )
			{
				ctrl.SetTool( SceneEditController::Tool::OrbitCamera );
				ctrl.OnPointerDown( Point2( 10, 10 ) );
				ctrl.OnPointerMove( Point2( 18, 14 ) );
				ctrl.OnPointerUp( Point2( 18, 14 ) );
			};

			{
				SceneEditController ctrl( *pJob, pRasterizer );

				Check( ctrl.SetViewportRenderMode( "deep_reflect" ), "switch to \"deep_reflect\" succeeds" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "deep_reflect", "GetViewportRenderMode reports \"deep_reflect\"" );
				unsigned int divisor = 0;
				ctrl.GetRefinementStatus( divisor );
				Check( divisor == 4, "deep_reflect pins the preview-scale divisor to 4" );

				driveOrbitGesture( ctrl );
				unsigned int divisorAfterGesture = 0;
				ctrl.GetRefinementStatus( divisorAfterGesture );
				Check( divisorAfterGesture == 4,
				       "MONEY ASSERTION: the divisor SURVIVES a gesture (OnPointerUp would normally reset it to 1) -- the pin held" );

				Check( ctrl.SetViewportRenderMode( "direct" ), "direct switch deep_reflect -> \"direct\" succeeds" );
				Check( std::string( ctrl.GetViewportRenderMode() ) == "direct", "GetViewportRenderMode reports \"direct\"" );
				unsigned int divisorDirect = 0;
				ctrl.GetRefinementStatus( divisorDirect );
				Check( divisorDirect == 2, "direct pins the preview-scale divisor to 2" );

				driveOrbitGesture( ctrl );
				unsigned int divisorDirectAfterGesture = 0;
				ctrl.GetRefinementStatus( divisorDirectAfterGesture );
				Check( divisorDirectAfterGesture == 2,
				       "the divisor SURVIVES a gesture under \"direct\" too" );

				// Leaving the variant mode unpins -- the SAME gesture now
				// resets the divisor (proving the pin was actually lifted,
				// not that OnPointerUp coincidentally leaves it alone).
				Check( ctrl.SetViewportRenderMode( "normals" ), "switch \"direct\" -> \"normals\" (non-variant) succeeds" );
				driveOrbitGesture( ctrl );
				unsigned int divisorAfterUnpin = 0;
				ctrl.GetRefinementStatus( divisorAfterUnpin );
				// kPreviewScaleMin (1) is private -- OnPointerUp's own doc
				// says it resets the divisor to that literal value.
				Check( divisorAfterUnpin == 1,
				       "MONEY ASSERTION: after leaving the variant mode, a gesture's OnPointerUp reset TAKES EFFECT again -- the pin was lifted" );

				Check( ctrl.SetViewportRenderMode( "preview" ), "restore \"preview\" before teardown" );
			}

			pRasterizer->release();
			pPreview->release();
			pPolish->release();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== ViewportRenderModeTest ===\n" );
	TestRegistryEnumeration();
	TestFindByName();
	TestCasterFactory();
	TestCAbi();
	TestSetViewModeCasterRefcounting();
	TestSkeletonMode();
	TestLiveModeSwitching();
	TestSceneReloadResetsToPreview();
	TestXrayAxis();
	TestPolishStateResetOnSwitch();
	TestVariantRegistryRows();
	TestBeautyVariantPipelineFactory();
	TestWantsDenoiseCAbi();
	TestControllerVariantModeLifecycle();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
