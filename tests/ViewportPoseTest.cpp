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

	//------------------------------------------------------------------
	// V5: axis snaps — auto-enter free-fly, re-pose to an axis, preserve
	// distance, and leave the scene camera untouched.
	//------------------------------------------------------------------
	void TestAxisSnaps()
	{
		std::printf( "V5: axis snaps (auto-enter, distance-preserving, non-destructive)...\n" );
		const std::string tmp = TempPath( "vpose_v5.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );   // active cam at (0,0,5) looking at origin, dist 5
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		const std::string serBefore = SerializedScene( *pJob );
		CameraSnapshot camBefore;
		Check( CaptureActive( *pJob, camBefore ), "active camera captured (pre)" );

		// Snap +X from a NON-free-fly state -> auto-enters free-fly.
		Check( !ctrl.IsFreeFlyActive(), "not in free-fly before snap" );
		Check( ctrl.SnapViewToAxis( /*axis X*/0, /*negative*/false ), "SnapViewToAxis(+X) succeeds" );
		Check( ctrl.IsFreeFlyActive(), "free-fly auto-entered by the snap" );
		CameraSnapshot px;
		Check( ctrl.GetViewportPose( px ), "pose after +X snap" );
		auto near = []( double a, double b ) { return std::fabs( a - b ) < 1e-9; };
		Check( near( px.location[0], 5.0 ) && near( px.location[1], 0.0 ) && near( px.location[2], 0.0 ),
			"+X snap positions the eye at pivot + (dist,0,0)" );
		Check( near( px.lookat[0], 0.0 ) && near( px.lookat[1], 0.0 ) && near( px.lookat[2], 0.0 ),
			"+X snap looks at the pivot" );
		Check( near( px.up[1], 1.0 ) && near( px.up[0], 0.0 ) && near( px.up[2], 0.0 ),
			"+X snap up is world-Y" );

		// Snap +Y (top) -> up must be -Z (not parallel to the view).
		Check( ctrl.SnapViewToAxis( /*axis Y*/1, /*negative*/false ), "SnapViewToAxis(+Y) succeeds" );
		CameraSnapshot py;
		Check( ctrl.GetViewportPose( py ), "pose after +Y snap" );
		Check( near( py.location[1], 5.0 ) && near( py.location[0], 0.0 ) && near( py.location[2], 0.0 ),
			"+Y snap positions the eye at pivot + (0,dist,0)" );
		Check( near( py.up[2], -1.0 ) && near( py.up[1], 0.0 ),
			"+Y (top) snap up is -Z (not parallel to the down-axis view)" );

		// −Y (bottom) up flips to +Z.
		Check( ctrl.SnapViewToAxis( 1, /*negative*/true ), "SnapViewToAxis(-Y) succeeds" );
		CameraSnapshot ny;
		Check( ctrl.GetViewportPose( ny ), "pose after -Y snap" );
		Check( near( ny.location[1], -5.0 ), "-Y snap positions the eye at pivot + (0,-dist,0)" );
		Check( near( ny.up[2], 1.0 ), "-Y (bottom) snap up is +Z" );

		// NON-DESTRUCTIVE throughout: the scene camera + CST are untouched.
		Check( SerializedScene( *pJob ) == serBefore, "CST byte-identical after snaps (no scene mutation)" );
		CameraSnapshot camAfter;
		Check( CaptureActive( *pJob, camAfter ), "active camera captured (post)" );
		Check( SameCamera( camAfter, camBefore ), "active scene camera untouched by snaps" );

		// Bad axis is refused.
		Check( !ctrl.SnapViewToAxis( 3, false ), "SnapViewToAxis(bad axis) refused" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// V6: Home — capture, navigate away, restore.
	//------------------------------------------------------------------
	void TestHomeView()
	{
		std::printf( "V6: Home view capture + restore...\n" );
		const std::string tmp = TempPath( "vpose_v6.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		auto near = []( double a, double b ) { return std::fabs( a - b ) < 1e-9; };

		Check( !ctrl.HasHomeView(), "no home view initially" );
		Check( !ctrl.GoToHomeView(), "GoToHomeView refused when no home set" );

		// Capture the active camera (at 0,0,5) as home.
		Check( ctrl.SetHomeView(), "SetHomeView captures the active camera" );
		Check( ctrl.HasHomeView(), "HasHomeView true after capture" );

		// Navigate away (snap +X -> eye at 5,0,0).
		Check( ctrl.SnapViewToAxis( 0, false ), "snap +X (navigate away)" );
		CameraSnapshot away;
		Check( ctrl.GetViewportPose( away ) && near( away.location[0], 5.0 ), "moved to +X" );

		// Home restores the captured pose (eye back at 0,0,5).
		Check( ctrl.GoToHomeView(), "GoToHomeView restores" );
		CameraSnapshot home;
		Check( ctrl.GetViewportPose( home ), "pose after GoToHomeView" );
		Check( near( home.location[0], 0.0 ) && near( home.location[1], 0.0 ) && near( home.location[2], 5.0 ),
			"home restored the captured eye position (0,0,5)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	//------------------------------------------------------------------
	// V7: nav axis-ball gizmo — layout, facing, hit-test, and the
	// view-aligned-axis degenerate (looking straight down an axis must
	// still emit six nubs, not make the gizmo vanish).
	//------------------------------------------------------------------
	void TestNavGizmo()
	{
		std::printf( "V7: nav axis-ball gizmo (layout + facing + hit-test)...\n" );
		const std::string tmp = TempPath( "vpose_v7.RISEscene" );
		Job* pJob = LoadScene( kPinholeScene, tmp );   // cam (0,0,5) -> origin, up +Y (down -Z)
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;

		SceneEditController ctrl( *pJob, nullptr );
		auto near = []( double a, double b ) { return std::fabs( a - b ) < 1e-6; };

		// Guard: non-positive geometry is refused, nothing drawn.
		Check( !ctrl.RefreshNavGizmo( 100, 100, 0.0, 10.0 ), "RefreshNavGizmo refuses zero ball radius" );
		Check( ctrl.NavGizmoNubCount() == 0, "no nubs after a refused refresh" );

		const double cx = 100, cy = 100, R = 40, nub = 10;
		Check( ctrl.RefreshNavGizmo( cx, cy, R, nub ), "RefreshNavGizmo succeeds on a pinhole scene" );
		Check( ctrl.NavGizmoNubCount() == 6, "six nubs (even looking straight down -Z)" );

		struct N { int axis; bool neg; double x, y, r; bool facing; };
		std::vector<N> ns;
		for( unsigned i = 0; i < ctrl.NavGizmoNubCount(); ++i ) {
			N n;
			Check( ctrl.NavGizmoNubInfo( i, n.axis, n.neg, n.x, n.y, n.r, n.facing ), "NavGizmoNubInfo in range" );
			ns.push_back( n );
		}
		{
			int a; bool ng, f; double x, y, r;
			Check( !ctrl.NavGizmoNubInfo( 6, a, ng, x, y, r, f ), "NavGizmoNubInfo out of range is false" );
		}

		// Axis coverage: two nubs per axis, exactly one + per axis.
		int axisCount[3] = { 0, 0, 0 }, posCount[3] = { 0, 0, 0 };
		for( auto& n : ns ) if( n.axis >= 0 && n.axis < 3 ) { ++axisCount[n.axis]; if( !n.neg ) ++posCount[n.axis]; }
		Check( axisCount[0] == 2 && axisCount[1] == 2 && axisCount[2] == 2, "two nubs per axis" );
		Check( posCount[0] == 1 && posCount[1] == 1 && posCount[2] == 1, "one +axis nub per axis" );

		bool within = true, radiusOK = true;
		for( auto& n : ns ) {
			const double d = std::sqrt( ( n.x - cx ) * ( n.x - cx ) + ( n.y - cy ) * ( n.y - cy ) );
			if( d > R + 1e-6 ) within = false;
			if( !near( n.r, nub ) ) radiusOK = false;
		}
		Check( within, "every nub sits within the ball radius of center" );
		Check( radiusOK, "nub radius passes through to screenRadius" );

		auto find = [&]( int axis, bool neg ) -> const N* {
			for( auto& n : ns ) if( n.axis == axis && n.neg == neg ) return &n;
			return nullptr;
		};
		const N* xp = find( 0, false );
		const N* yp = find( 1, false );
		const N* zp = find( 2, false );
		const N* zn = find( 2, true );
		Check( xp && xp->x > cx + 1.0, "+X nub is to screen-right of center" );
		Check( yp && yp->y < cy - 1.0, "+Y nub is above center (widget-Y-down)" );

		// Camera looks down -Z from +Z, so +Z points toward the viewer.
		Check( zp && zp->facing, "+Z nub faces the viewer" );
		Check( zn && !zn->facing, "-Z nub faces away" );
		// View-aligned Z axis: both nubs collapse onto the ball center.
		Check( zp && near( zp->x, cx ) && near( zp->y, cy ), "+Z nub sits at the ball center (view-aligned)" );

		// Hit-test: on the +X nub returns it; far away misses.
		if( xp ) {
			const int hit = ctrl.NavGizmoNubAt( xp->x, xp->y );
			Check( hit >= 0 && ns[hit].axis == 0 && !ns[hit].neg, "NavGizmoNubAt on +X returns the +X nub" );
		}
		Check( ctrl.NavGizmoNubAt( 10000, 10000 ) == -1, "NavGizmoNubAt far away misses" );
		// At the center both Z nubs overlap; the FRONT-facing (+Z) one wins.
		const int cHit = ctrl.NavGizmoNubAt( cx, cy );
		Check( cHit >= 0 && ns[cHit].axis == 2 && !ns[cHit].neg, "center hit returns the front-facing +Z nub" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== ViewportPoseTest ===\n" );
	TestEnterAndNonDestructive();
	TestDifferentKindPose();
	TestExitAndRefusal();
	TestAxisSnaps();
	TestHomeView();
	TestNavGizmo();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
