//////////////////////////////////////////////////////////////////////
//
//  EnvironmentEditTest.cpp - GUI Environment / IBL section coverage.
//
//    E1  Read: a scene whose active rasterizer binds an hdr_painter via
//        radiance_map/scale/background/orient is reported by
//        GetEnvironment with hasEnvironment==true, the bound painter
//        name, the resolved HDRI file, the scale/background/orient
//        (orient round-trips in DEGREES), and editable==true.
//    E2  Write + persist: SetEnvironmentScale / Background / Orient
//        mutate the live snapshot (re-read via GetEnvironment) AND write
//        the param into the retained CST (verified by SerializeCst of the
//        rasterizer chunk) so a save survives.
//    E3  Add / Remove: on a scene with NO environment, GetEnvironment
//        reports hasEnvironment==false; AddEnvironment inserts a painter +
//        binds radiance_map (hasEnvironment==true, file resolved, painter
//        chunk present in the CST); RemoveEnvironment unbinds (radiance_map
//        gone from the CST, hasEnvironment==false) leaving the painter.
//    E4  File swap: SetEnvironmentFile changes the bound painter's `file`
//        (re-read via GetEnvironment + present in the serialized CST).
//    E5  MLT refusal: on an mlt_rasterizer scene GetEnvironment reports
//        editable==false and every setter returns false (no radiance map
//        on MLT -> a rebuild would silently drop the edit).
//
//  Self-contained: inline native-v7 scenes, OIDN off, no render pass is
//  started (cancel-and-park is a no-op while mRendering is false).  The
//  HDRI file paths need not exist -- a missing image registers a black
//  texture rather than failing the derive, which is all these read/write
//  path tests exercise.
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

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;
using Env = SceneEditController::EnvironmentInfo;

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

	// Serialize the Job's retained CST Document to text (proves what a SAVE
	// would write) -- the persistence oracle for E2 / E3 / E4.
	std::string SerializedScene( Job& job )
	{
		const RISE::Cst::Document* doc = job.GetCstDocument();
		return doc ? RISE::Cst::SerializeCst( *doc ) : std::string();
	}

	bool Contains( const std::string& hay, const std::string& needle )
	{
		return hay.find( needle ) != std::string::npos;
	}

	const std::string kHdriFile = "/tmp/rise_env_test_probe.hdr";   // need not exist (black-texture fallback)

	// Scene whose active rasterizer BINDS an environment (hdr_painter + radiance_*).
	std::string EnvBoundScene()
	{
		return std::string(
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			"hdr_painter\n{\nname pnt_env\nfile " ) + kHdriFile + "\n}\n\n"
			"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n"
			"radiance_map pnt_env\nradiance_scale 2\nradiance_background TRUE\nradiance_orient 0 90 0\n}\n\n"
			"film\n{\nwidth 16\nheight 16\n}\n\n"
			"pinhole_camera\n{\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";
	}

	// Scene with NO environment (no radiance_map on the rasterizer).
	const char* const kNoEnvScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	// MLT rasterizer scene (RebuildRasterizer handles it, but the MLT factory
	// takes NO radiance map -> not env-editable).
	const char* const kMltScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"mlt_rasterizer\n{\nbootstrap_samples 100\nchains 4\nmutations_per_pixel 2\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	// pixelpel_rasterizer ACCEPTS radiance_* in scene files, but RebuildRasterizer
	// has no case for it -> a live env edit would silently no-op, so it must
	// report editable==false (the allow-list, not an MLT-only deny-list).
	std::string PixelPelEnvScene()
	{
		return std::string(
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			"hdr_painter\n{\nname pnt_env\nfile " ) + kHdriFile + "\n}\n\n"
			"pixelpel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n"
			"radiance_map pnt_env\nradiance_scale 1.5\n}\n\n"
			"film\n{\nwidth 16\nheight 16\n}\n\n"
			"pinhole_camera\n{\nlocation 0 0 3\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";
	}

	//------------------------------------------------------------------
	// E1: read a bound environment.
	//------------------------------------------------------------------
	void TestReadBoundEnvironment()
	{
		std::printf( "E1: read a bound environment...\n" );
		const std::string tmp = TempPath( "env_e1.RISEscene" );
		Job* pJob = LoadScene( EnvBoundScene(), tmp );
		Check( pJob != nullptr, "E1 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		Env e;
		Check( ctrl.GetEnvironment( e ), "GetEnvironment succeeds" );
		Check( e.hasEnvironment, "hasEnvironment == true" );
		Check( e.editable, "editable == true (PT rasterizer)" );
		Check( std::string( e.painterName.c_str() ) == "pnt_env", "painterName == pnt_env" );
		Check( std::string( e.file.c_str() ) == kHdriFile, "resolved file == authored HDRI path" );
		Check( e.scale > 1.99 && e.scale < 2.01, "scale == 2" );
		Check( e.background, "background == true" );
		Check( e.orientDeg[1] > 89.9 && e.orientDeg[1] < 90.1, "orient Y == 90 degrees (radian round-trip)" );
		Check( !e.proceduralSky, "proceduralSky == false" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E2: write + persist scale / background / orient.
	//------------------------------------------------------------------
	void TestWriteAndPersist()
	{
		std::printf( "E2: write + persist scale / background / orient...\n" );
		const std::string tmp = TempPath( "env_e2.RISEscene" );
		Job* pJob = LoadScene( EnvBoundScene(), tmp );
		Check( pJob != nullptr, "E2 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		Check( ctrl.SetEnvironmentScale( 4.5 ), "SetEnvironmentScale(4.5) applied" );
		Check( ctrl.SetEnvironmentBackground( false ), "SetEnvironmentBackground(false) applied" );
		Check( ctrl.SetEnvironmentOrient( 10, 20, 30 ), "SetEnvironmentOrient(10,20,30) applied" );

		// Live snapshot reflects every edit.
		Env e;
		Check( ctrl.GetEnvironment( e ), "GetEnvironment after edits" );
		Check( e.scale > 4.49 && e.scale < 4.51, "live scale == 4.5" );
		Check( !e.background, "live background == false" );
		Check( e.orientDeg[0] > 9.9 && e.orientDeg[0] < 10.1, "live orient X == 10" );
		Check( e.orientDeg[2] > 29.9 && e.orientDeg[2] < 30.1, "live orient Z == 30" );

		// CST persistence: a SAVE would carry the new values.
		const std::string ser = SerializedScene( *pJob );
		Check( Contains( ser, "radiance_scale 4.5" ), "CST has radiance_scale 4.5 (persists)" );
		Check( Contains( ser, "radiance_background false" ), "CST has radiance_background false (persists)" );
		Check( Contains( ser, "radiance_orient 10 20 30" ), "CST has radiance_orient 10 20 30 (persists)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E3: add on an env-less scene, then remove.
	//------------------------------------------------------------------
	void TestAddAndRemove()
	{
		std::printf( "E3: add environment on an env-less scene, then remove...\n" );
		const std::string tmp = TempPath( "env_e3.RISEscene" );
		Job* pJob = LoadScene( kNoEnvScene, tmp );
		Check( pJob != nullptr, "E3 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		Env e0;
		Check( ctrl.GetEnvironment( e0 ), "GetEnvironment on env-less scene" );
		Check( !e0.hasEnvironment, "hasEnvironment == false initially" );
		Check( e0.editable, "editable == true (PT rasterizer)" );

		String outName;
		const SceneEditController::AgentCommitResult r =
			ctrl.AddEnvironment( String( kHdriFile.c_str() ), &outName );
		Check( r.applied, "AddEnvironment applied (" + std::string( r.message.c_str() ) + ")" );
		Check( !std::string( outName.c_str() ).empty(), "AddEnvironment reported a painter name" );

		Env e1;
		Check( ctrl.GetEnvironment( e1 ), "GetEnvironment after add" );
		Check( e1.hasEnvironment, "hasEnvironment == true after add" );
		Check( std::string( e1.file.c_str() ) == kHdriFile, "resolved file == added HDRI path" );
		Check( std::string( e1.painterName.c_str() ) == std::string( outName.c_str() ),
			"bound painter == the added painter" );

		const std::string serAdd = SerializedScene( *pJob );
		Check( Contains( serAdd, "hdr_painter" ), "CST has an hdr_painter chunk after add" );
		Check( Contains( serAdd, "radiance_map " + std::string( outName.c_str() ) ),
			"CST binds radiance_map to the added painter (persists)" );

		// Remove: unbinds radiance_map (live + CST); the painter chunk remains.
		Check( ctrl.RemoveEnvironment(), "RemoveEnvironment applied" );
		Env e2;
		Check( ctrl.GetEnvironment( e2 ), "GetEnvironment after remove" );
		Check( !e2.hasEnvironment, "hasEnvironment == false after remove" );
		// The stale-map regression: the live global map must actually be gone,
		// else GetEnvironment's else-branch would misreport proceduralSky==true.
		Check( !e2.proceduralSky, "proceduralSky == false after remove (live map cleared, not stale)" );

		const std::string serRem = SerializedScene( *pJob );
		Check( !Contains( serRem, "radiance_map" ), "CST radiance_map erased after remove (persists)" );
		Check( Contains( serRem, "hdr_painter" ), "CST still has the (now-orphan) hdr_painter chunk" );

		// Removing again when nothing is bound is an honest no-op (returns false),
		// and must NOT touch any (hypothetical procedural-sky) global map.
		Check( !ctrl.RemoveEnvironment(), "RemoveEnvironment returns false when nothing is bound" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E3b: AddEnvironment refuses when an environment is already bound.
	//------------------------------------------------------------------
	void TestAddWhenAlreadyBound()
	{
		std::printf( "E3b: AddEnvironment refuses when already bound...\n" );
		const std::string tmp = TempPath( "env_e3b.RISEscene" );
		Job* pJob = LoadScene( EnvBoundScene(), tmp );
		Check( pJob != nullptr, "E3b fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		String nm;
		const SceneEditController::AgentCommitResult r =
			ctrl.AddEnvironment( String( "/tmp/rise_env_test_other.hdr" ), &nm );
		Check( !r.applied, "AddEnvironment refused (env already bound)" );
		Check( !std::string( r.message.c_str() ).empty(), "refusal carries a message" );
		// The original binding is untouched.
		Env e;
		Check( ctrl.GetEnvironment( e ) && std::string( e.painterName.c_str() ) == "pnt_env",
			"original binding intact after refused Add" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E4: swap the HDRI file.
	//------------------------------------------------------------------
	void TestFileSwap()
	{
		std::printf( "E4: swap the HDRI file...\n" );
		const std::string tmp = TempPath( "env_e4.RISEscene" );
		Job* pJob = LoadScene( EnvBoundScene(), tmp );
		Check( pJob != nullptr, "E4 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		const std::string newFile = "/tmp/rise_env_test_probe2.hdr";
		Check( ctrl.SetEnvironmentFile( String( newFile.c_str() ) ), "SetEnvironmentFile applied" );

		Env e;
		Check( ctrl.GetEnvironment( e ), "GetEnvironment after file swap" );
		Check( std::string( e.file.c_str() ) == newFile, "resolved file == swapped path" );

		const std::string ser = SerializedScene( *pJob );
		Check( Contains( ser, newFile ), "CST painter `file` updated (persists)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E5: MLT rasterizer refuses env editing.
	//------------------------------------------------------------------
	void TestMltRefusal()
	{
		std::printf( "E5: MLT rasterizer refuses env editing...\n" );
		const std::string tmp = TempPath( "env_e5.RISEscene" );
		Job* pJob = LoadScene( kMltScene, tmp );
		Check( pJob != nullptr, "E5 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		Env e;
		Check( ctrl.GetEnvironment( e ), "GetEnvironment on MLT scene" );
		Check( !e.editable, "editable == false (MLT takes no radiance map)" );
		Check( !ctrl.SetEnvironmentScale( 3.0 ), "SetEnvironmentScale refused on MLT" );
		Check( !ctrl.SetEnvironmentBackground( false ), "SetEnvironmentBackground refused on MLT" );
		Check( !ctrl.SetEnvironmentOrient( 1, 2, 3 ), "SetEnvironmentOrient refused on MLT" );
		String nm;
		Check( !ctrl.AddEnvironment( String( kHdriFile.c_str() ), &nm ).applied, "AddEnvironment refused on MLT" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E7: AddEnvironment after a LIVE-ONLY rasterizer switch still persists.
	// A live SetActiveRasterizer to a kind absent from the scene text does
	// NOT touch the CST; AddEnvironment's painter insert then runs a D2
	// re-derive that rebuilds the scene from the CST, resetting the active
	// rasterizer to the CST-authored one -- so the radiance_map binding lands
	// on a chunk that actually saves.  This documents that the "chunkless
	// active rasterizer" path self-heals into an honest, persisting bind
	// (rather than a silent applied-but-dropped-on-save).
	//------------------------------------------------------------------
	void TestAddAfterLiveRasterizerSwitch()
	{
		std::printf( "E7: AddEnvironment after a live-only rasterizer switch persists (re-derive rebinds to the CST rasterizer)...\n" );
		const std::string tmp = TempPath( "env_e7.RISEscene" );
		Job* pJob = LoadScene( kNoEnvScene, tmp );   // authors only pathtracing_pel_rasterizer
		Check( pJob != nullptr, "E7 fixture loads" );
		if( !pJob ) return;

		// Live-only switch to a kind absent from the scene text (lazy-built).
		Check( pJob->SetActiveRasterizer( "vcm_pel_rasterizer" ), "live switch active rasterizer to vcm_pel" );

		SceneEditController ctrl( *pJob, nullptr );
		String nm;
		const SceneEditController::AgentCommitResult r =
			ctrl.AddEnvironment( String( kHdriFile.c_str() ), &nm );
		// The insert's re-derive resets active to the CST-authored pathtracing_pel,
		// so the bind lands on a chunk that persists -> honest "applied".
		Check( r.applied, "AddEnvironment applied (bind lands on the re-derived CST rasterizer)" );
		Check( std::string( r.status.c_str() ) == "applied", "status == applied (not a dropped partial)" );
		Env e;
		Check( ctrl.GetEnvironment( e ) && e.hasEnvironment, "environment bound after add" );
		const std::string ser = SerializedScene( *pJob );
		Check( Contains( ser, "radiance_map " + std::string( nm.c_str() ) ),
			"CST records the binding on the (re-derived) rasterizer chunk -- no silent loss on save" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// E6: pixelpel_rasterizer reports editable==false (allow-list P1).
	//------------------------------------------------------------------
	void TestPixelPelNotEditable()
	{
		std::printf( "E6: pixelpel_rasterizer reports editable==false (no RebuildRasterizer case)...\n" );
		const std::string tmp = TempPath( "env_e6.RISEscene" );
		Job* pJob = LoadScene( PixelPelEnvScene(), tmp );
		Check( pJob != nullptr, "E6 fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		Env e;
		Check( ctrl.GetEnvironment( e ), "GetEnvironment on pixelpel scene" );
		// The env is BOUND (read path works) but NOT editable (the live rebuild
		// path can't apply an edit) -- the GUI keeps the controls disabled.
		Check( e.hasEnvironment, "hasEnvironment == true (binding is readable)" );
		Check( !e.editable, "editable == false (pixelpel has no RebuildRasterizer case)" );
		Check( !ctrl.SetEnvironmentScale( 3.0 ), "SetEnvironmentScale refused on pixelpel" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== EnvironmentEditTest ===\n" );
	TestReadBoundEnvironment();
	TestWriteAndPersist();
	TestAddAndRemove();
	TestAddWhenAlreadyBound();
	TestFileSwap();
	TestMltRefusal();
	TestPixelPelNotEditable();
	TestAddAfterLiveRasterizerSwitch();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
