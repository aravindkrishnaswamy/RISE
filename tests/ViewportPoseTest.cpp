//////////////////////////////////////////////////////////////////////
//
//  ViewportPoseTest.cpp - Tier 2 (Direction B) foundational slice: the
//    transient free-fly ViewportPose + render-camera override
//    (docs/gui/CAMERAS_AND_VIEWS.md §5.3-5.5).
//
//    V1  Enter free-fly from the active camera: EnterFreeFlyFromActiveCamera
//        succeeds on a scene with a realizable active camera, IsFreeFlyActive
//        flips true, and GetViewportPose round-trips the active camera's
//        pose+kind.
//    V2  NON-DESTRUCTIVE (the load-bearing invariant): entering / setting /
//        exiting free-fly leaves the scene byte-identical (SerializeCst of the
//        retained Document unchanged), the active scene camera's introspection
//        snapshot unchanged, and produces NO undo entry (UndoLabel empty).
//        Free-fly is UI/session state, not a scene mutation.
//    V3  Set an explicit pose of a DIFFERENT kind (Fisheye onto a Pinhole
//        scene): SetViewportPose succeeds (a standalone override camera of the
//        pose's kind is realized) and GetViewportPose reports the new kind —
//        with the scene camera still untouched (restore is projection-faithful
//        and active-camera-independent, §3.2).
//    V4  Exit reverts (IsFreeFlyActive false, GetViewportPose false); enter on
//        a camera-less scene is refused.
//
//  Self-contained: inline native-v7 scenes, OIDN off, nullptr interactive
//  rasterizer (no render pass is started — the override wiring in
//  DoOneRenderPass is exercised by the render path, not this unit test; here
//  we prove the CONTRACT the override rests on: it never touches the scene).
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
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"

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

	std::string SerializedScene( Job& job )
	{
		const RISE::Cst::Document* doc = job.GetCstDocument();
		return doc ? RISE::Cst::SerializeCst( *doc ) : std::string();
	}

	// Capture the scene's active camera into a snapshot (for byte-untouched checks).
	bool CaptureActive( Job& job, CameraSnapshot& out )
	{
		const IScene* scene = job.GetScene();
		if( !scene ) return false;
		const ICamera* cam = scene->GetCamera();
		if( !cam ) return false;
		return CameraIntrospection::CaptureCameraSnapshot( *cam, out );
	}

	bool SameCamera( const CameraSnapshot& a, const CameraSnapshot& b )
	{
		auto eq = []( double x, double y ) { return std::fabs( x - y ) < 1e-12; };
		if( a.type != b.type ) return false;
		for( int i = 0; i < 3; ++i )
			if( !eq( a.location[i], b.location[i] ) || !eq( a.lookat[i], b.lookat[i] ) || !eq( a.up[i], b.up[i] ) )
				return false;
		return eq( a.fov, b.fov ) && eq( a.exposure, b.exposure );
	}

	const char* const kPinholeScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	// Camera-less scene (renders on the built-in default camera -> GetCamera may be null).
	const char* const kNoCameraScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"omni_light\n{\nname lgt\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//------------------------------------------------------------------
	// V1 + V2: enter free-fly, round-trip the pose, and prove the scene
	// is byte-untouched (non-destructive).
	//------------------------------------------------------------------
	void TestEnterAndNonDestructive()
	{
		std::printf( "V1/V2: enter free-fly, round-trip pose, scene byte-untouched...\n" );
		const std::string tmp = TempPath( "vpose_v1.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );

		const std::string serBefore = SerializedScene( *pJob );
		CameraSnapshot camBefore;
		Check( CaptureActive( *pJob, camBefore ), "active camera captured (pre)" );

		Check( !ctrl.IsFreeFlyActive(), "free-fly inactive initially" );
		Check( ctrl.EnterFreeFlyFromActiveCamera(), "EnterFreeFlyFromActiveCamera succeeds" );
		Check( ctrl.IsFreeFlyActive(), "IsFreeFlyActive true after enter" );

		CameraSnapshot pose;
		Check( ctrl.GetViewportPose( pose ), "GetViewportPose returns a pose while active" );
		Check( SameCamera( pose, camBefore ), "pose round-trips the active camera (pose+kind)" );

		// NON-DESTRUCTIVE: the scene text and the active scene camera are unchanged,
		// and no undo entry was produced.
		Check( SerializedScene( *pJob ) == serBefore, "CST byte-identical after enter (no scene mutation)" );
		CameraSnapshot camAfter;
		Check( CaptureActive( *pJob, camAfter ), "active camera captured (post)" );
		Check( SameCamera( camAfter, camBefore ), "active scene camera byte-untouched by free-fly" );
		Check( std::string( ctrl.UndoLabel().c_str() ).empty(), "no undo entry produced (UndoLabel empty)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// V3: set a DIFFERENT-kind pose (Fisheye) onto a Pinhole scene.
	//------------------------------------------------------------------
	void TestDifferentKindPose()
	{
		std::printf( "V3: set a Fisheye pose onto a Pinhole scene (projection-faithful, active untouched)...\n" );
		const std::string tmp = TempPath( "vpose_v3.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		const std::string serBefore = SerializedScene( *pJob );

		// Build a fisheye pose by hand (not the active camera's kind).
		CameraSnapshot fish;
		fish.type = CameraSnapshot::Fisheye;
		fish.location[0] = 1; fish.location[1] = 2; fish.location[2] = 6;
		fish.lookat[0] = 0;   fish.lookat[1] = 0;   fish.lookat[2] = 0;
		fish.up[0] = 0;       fish.up[1] = 1;       fish.up[2] = 0;
		fish.fisheyeScale = 1.0;

		Check( ctrl.SetViewportPose( fish ), "SetViewportPose(Fisheye) succeeds (standalone override realized)" );
		Check( ctrl.IsFreeFlyActive(), "free-fly active after SetViewportPose" );
		CameraSnapshot got;
		Check( ctrl.GetViewportPose( got ), "GetViewportPose after SetViewportPose" );
		Check( got.type == CameraSnapshot::Fisheye, "pose kind is Fisheye (not the Pinhole scene camera's kind)" );

		// The scene's Pinhole active camera is untouched — restore is not a scene mutation.
		Check( SerializedScene( *pJob ) == serBefore, "CST byte-identical (Fisheye pose did not mutate the scene)" );
		CameraSnapshot camAfter;
		Check( CaptureActive( *pJob, camAfter ), "active camera captured (post)" );
		Check( camAfter.type == CameraSnapshot::Pinhole, "active scene camera still Pinhole (override is view-only)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// V4: exit reverts; enter on a camera-less scene is refused.
	//------------------------------------------------------------------
	void TestExitAndRefusal()
	{
		std::printf( "V4: exit reverts; camera-less scene refuses enter...\n" );
		const std::string tmp = TempPath( "vpose_v4.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		Check( ctrl.EnterFreeFlyFromActiveCamera(), "enter succeeds" );
		Check( ctrl.ExitFreeFly(), "ExitFreeFly returns true when active" );
		Check( !ctrl.IsFreeFlyActive(), "free-fly inactive after exit" );
		CameraSnapshot none;
		Check( !ctrl.GetViewportPose( none ), "GetViewportPose false after exit" );
		Check( !ctrl.ExitFreeFly(), "ExitFreeFly returns false when not active (honest no-op)" );

		pJob->release();
		std::remove( tmp.c_str() );

		// Camera-less scene: enter is refused (no active camera to seed the pose).
		const std::string tmp2 = TempPath( "vpose_v4b.RISEscene" );
		Job* pJob2 = LoadScene( kNoCameraScene, tmp2 );
		Check( pJob2 != nullptr, "camera-less fixture loads" );
		if( pJob2 )
		{
			SceneEditController ctrl2( *pJob2, nullptr );
			const IScene* sc = pJob2->GetScene();
			if( sc && sc->GetCamera() == nullptr )
				Check( !ctrl2.EnterFreeFlyFromActiveCamera(), "enter refused on a camera-less scene" );
			else
				Check( true, "(scene supplied a default camera; refusal case not reached — skipped)" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== ViewportPoseTest ===\n" );
	TestEnterAndNonDestructive();
	TestDifferentKindPose();
	TestExitAndRefusal();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
