//////////////////////////////////////////////////////////////////////
//
//  FilmCameraResyncTest.cpp - Regression test for the Camera/Film/
//    Output split's "Film and camera Frame stay in lockstep"
//    invariant.  The bug class this test guards against is the
//    rasterizer enumerating Film-many pixels while the camera's
//    GenerateRay still projects through stale Frame dims, producing
//    a re-FRAMED image instead of a re-RESOLVED one.
//
//    The contract we test:
//      Job::SetFilm(W, H, pAR) installs a new Film AND re-syncs
//      every camera in the manager so its CameraCommon::GetWidth /
//      GetHeight / GetPixelAR all match the new Film.
//
//    Coverage:
//      1. CLI-style override flow: SetFilm called AFTER AddCamera
//         updates the existing camera.
//      2. Multi-camera resync: SetFilm updates EVERY camera in the
//         manager, not just the active one (so SetActiveCamera
//         can switch safely afterwards).
//      3. Multiple successive SetFilm calls leave the camera always
//         matching the most recent Film.
//      4. SetFilm before any camera (the parser flow) is a no-op
//         on cameras (there are none) and still installs the Film.
//      5. The `film`-after-camera scene-file flow: parsing a scene
//         where the `film` chunk appears AFTER the camera chunk
//         leaves the camera matching the film chunk's value (not
//         the camera-chunk's authored dims).
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IScenePriv.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Inspect a camera's cached Frame dims via the CameraCommon non-public
// accessors.  Used to assert the resync touched the camera's
// projection state, not just Scene's Film.
static bool GetCameraDims(
	const ICamera* pCam,
	unsigned int& outW, unsigned int& outH, Scalar& outPAR )
{
	const CameraCommon* cc = dynamic_cast<const CameraCommon*>( pCam );
	if( !cc ) return false;
	outW   = cc->GetWidth();
	outH   = cc->GetHeight();
	outPAR = cc->GetPixelAR();
	return true;
}

//////////////////////////////////////////////////////////////////////
// 1. CLI-style override: SetFilm AFTER AddCamera updates the camera.
//////////////////////////////////////////////////////////////////////

static void TestCLIStyleOverride()
{
	std::cout << "Testing CLI-style SetFilm-after-AddCamera resync..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	// Phase B1 contract: programmatic AddCamera does NOT auto-sync
	// Film (the parser path does it, but only for v5 scenes that
	// author width/height in the camera chunk).  Programmatic callers
	// must call SetFilm before AddXxxCamera — verify here that the
	// SetFilm-then-Add flow leaves Film and the camera in sync.
	const double loc[3]    = {0,0,5};
	const double lookat[3] = {0,0,0};
	const double up[3]     = {0,1,0};
	const double orientation[3] = {0,0,0};
	const double target_orientation[2] = {0,0};

	pJob->SetFilm( 100, 80, 1.0 );
	Check( pJob->AddPinholeCamera( "main", loc, lookat, up,
		Scalar( 0.785398 ),
		0, 0, 0, orientation, target_orientation ),
		"AddPinholeCamera(100x80) after SetFilm(100x80) succeeds" );

	// Sanity: Film and camera both at 100x80.
	const IFilm* film0 = scene->GetFilm();
	Check( film0 && film0->GetWidth() == 100 && film0->GetHeight() == 80,
	       "Film at 100x80 after SetFilm + Add" );

	unsigned int w, h; Scalar pAR;
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 100 && h == 80 && pAR == 1.0,
	       "Camera Frame at 100x80 after SetFilm + Add" );

	// CLI override: SetFilm to a different resolution.  The camera's
	// Frame must follow.
	Check( pJob->SetFilm( 480, 270, 1.0 ), "SetFilm(480x270) succeeds" );

	const IFilm* film1 = scene->GetFilm();
	Check( film1 && film1->GetWidth() == 480 && film1->GetHeight() == 270,
	       "Film at 480x270 after override" );

	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 480 && h == 270 && pAR == 1.0,
	       "Camera Frame at 480x270 after override (resynced)" );

	// pixelAR override too.
	Check( pJob->SetFilm( 1920, 1080, 2.0 ), "SetFilm with pixelAR=2.0 succeeds" );
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 1920 && h == 1080 && pAR == 2.0,
	       "Camera Frame + pixelAR resynced" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 2. Multi-camera resync: SetFilm updates EVERY camera, not just
//    the currently-active one.
//////////////////////////////////////////////////////////////////////

static void TestMultiCameraResync()
{
	std::cout << "Testing SetFilm resyncs ALL cameras..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	const double loc[3]    = {0,0,5};
	const double lookat[3] = {0,0,0};
	const double up[3]     = {0,1,0};
	const double orientation[3] = {0,0,0};
	const double target_orientation[2] = {0,0};

	// Alpha gets 100x100 Film, then beta resizes Film to 200x200
	// (which also resyncs alpha — that's the Phase B1 invariant the
	// "all cameras follow Film" multi-camera section below tests).
	pJob->SetFilm( 100, 100, 1.0 );
	pJob->AddPinholeCamera( "alpha", loc, lookat, up,
		Scalar( 0.785398 ),
		0, 0, 0, orientation, target_orientation );
	pJob->SetFilm( 200, 200, 1.0 );
	pJob->AddPinholeCamera( "beta", loc, lookat, up,
		Scalar( 0.785398 ),
		0, 0, 0, orientation, target_orientation );

	// "beta" is active (last-added wins); "alpha" is NOT active.
	Check( strcmp( scene->GetActiveCameraName().c_str(), "beta" ) == 0,
	       "beta is active after both added" );

	// SetFilm should update BOTH cameras.
	pJob->SetFilm( 640, 360, 1.0 );

	// Active camera (beta) follows Film.
	unsigned int w, h; Scalar pAR;
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 640 && h == 360,
	       "active camera (beta) resynced to 640x360" );

	// Switch to alpha and verify IT also follows Film.
	scene->SetActiveCamera( "alpha" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0,
	       "alpha is now active" );
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 640 && h == 360,
	       "alpha (previously inactive) ALSO resynced to 640x360" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 3. Repeated SetFilm leaves the camera always matching latest.
//////////////////////////////////////////////////////////////////////

static void TestRepeatedSetFilm()
{
	std::cout << "Testing repeated SetFilm tracks latest..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	const double loc[3] = {0,0,5}, lookat[3] = {0,0,0}, up[3] = {0,1,0};
	const double orientation[3] = {0,0,0}, target_orientation[2] = {0,0};
	pJob->SetFilm( 100, 100, 1.0 );
	pJob->AddPinholeCamera( "main", loc, lookat, up,
		Scalar( 0.785398 ),
		0, 0, 0, orientation, target_orientation );

	pJob->SetFilm( 200, 200, 1.0 );
	pJob->SetFilm( 300, 300, 1.0 );
	pJob->SetFilm( 400, 400, 1.0 );

	unsigned int w, h; Scalar pAR;
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ) &&
	       w == 400 && h == 400,
	       "camera tracks latest SetFilm (400x400)" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 4. SetFilm with no cameras yet is a no-op on cameras (manager
//    iteration runs zero times) and still installs the Film.
//////////////////////////////////////////////////////////////////////

static void TestSetFilmBeforeAddCamera()
{
	std::cout << "Testing SetFilm before any AddCamera..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	Check( pJob->SetFilm( 1024, 768, 1.0 ), "SetFilm before AddCamera succeeds" );
	Check( scene->GetFilm()->GetWidth() == 1024 &&
	       scene->GetFilm()->GetHeight() == 768,
	       "Film installed at 1024x768" );

	// Now adding a camera with different authored dims would call
	// SetFilm internally (the parser flow does this) — verify the
	// LATEST SetFilm wins.  Skipping the parser-call here to keep the
	// test focused; the camera-Frame and Film just need to match
	// after each SetFilm.

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 5. Scene file with `film` chunk AFTER the camera chunk:
//    the camera's Frame must end up matching the film chunk's dims.
//    In v6 cameras don't carry their own dims at all — they read
//    from the active Film at construction — but a `film` chunk after
//    the camera still needs to RESYNC the already-added camera (via
//    Scene::ResizeFilm's resync logic).  Locks down the resync
//    invariant for the late-film-chunk authoring order.
//////////////////////////////////////////////////////////////////////

static void TestFilmChunkAfterCamera()
{
	std::cout << "Testing parsing `film` chunk AFTER `pinhole_camera` chunk..." << std::endl;

	const char* sceneText =
		"RISE ASCII SCENE 6\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth   100\n"
		"\theight  100\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tname     main\n"
		"\tlocation 0 0 5\n"
		"\tlookat   0 0 0\n"
		"\tup       0 1 0\n"
		"\tfov      30\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth   1920\n"
		"\theight  1080\n"
		"\tpixelAR 1.5\n"
		"}\n";

	// Write the scene to a pid-scoped temp file so the test is robust
	// to the CWD (worktrees / fresh clones don't have a `rendered/`
	// dir relative to CWD).  Matches the idiom in
	// VCMStrategyBalanceTest / BDPTStrategyBalanceTest.
	char tmpPath[ 512 ];
#ifdef _WIN32
	const char* tmpDir = std::getenv( "TEMP" );
	if( !tmpDir ) tmpDir = std::getenv( "TMP" );
	if( !tmpDir ) tmpDir = ".";
	std::snprintf( tmpPath, sizeof(tmpPath),
		"%s\\_film_after_camera_test_%d.RISEscene",
		tmpDir, static_cast<int>(::getpid()) );
#else
	std::snprintf( tmpPath, sizeof(tmpPath),
		"/tmp/_film_after_camera_test_%d.RISEscene",
		static_cast<int>(::getpid()) );
#endif
	{
		std::ofstream f( tmpPath );
		f << sceneText;
	}

	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();
	Check( pJob->LoadAsciiScene( tmpPath ), "scene loads" );

	// First `film` chunk installs 100x100, camera adds at 100x100,
	// second `film` chunk overrides to 1920x1080 and resyncs the
	// already-added camera.
	const IFilm* film = scene->GetFilm();
	Check( film && film->GetWidth() == 1920 && film->GetHeight() == 1080,
	       "Film matches the second (last) film chunk (1920x1080)" );

	unsigned int w, h; Scalar pAR;
	Check( GetCameraDims( scene->GetCamera(), w, h, pAR ),
	       "camera is CameraCommon-derived" );
	Check( w == 1920 && h == 1080,
	       "camera Frame matches latest Film (1920x1080), not initial 100x100" );
	Check( pAR == 1.5,
	       "camera pixelAR matches latest film chunk (1.5)" );

	std::remove( tmpPath );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "=== FilmCameraResyncTest ===" << std::endl;

	TestCLIStyleOverride();
	TestMultiCameraResync();
	TestRepeatedSetFilm();
	TestSetFilmBeforeAddCamera();
	TestFilmChunkAfterCamera();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
