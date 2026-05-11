//////////////////////////////////////////////////////////////////////
//
//  JobScaleFilmToFitTest.cpp - Unit tests for Job::ScaleFilmToFit.
//
//    The GUI bridges (Windows/Qt, macOS/SwiftUI, Android/Compose) call
//    this after scene-parse to shrink the scene Film to a screen-
//    appropriate size capped at maxLongEdge px.  The contract:
//
//      - never upscales (a tiny scene stays tiny)
//      - preserves the scene's aspect ratio AND pixelAR
//      - both axes <= maxSurfaceW / maxSurfaceH respectively
//      - long edge <= maxLongEdge
//      - returns true on success or no-op; false on zero arguments
//
//    These tests pin each branch of the scale-factor min selection
//    (surface-width-limited, surface-height-limited, long-edge-cap-
//    limited, no-op) plus the rejection paths.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <iostream>

#include "../src/Library/Job.h"
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

struct JobHandle {
	Job* p;
	JobHandle() : p( new Job() ) {}
	~JobHandle() { if( p ) p->release(); }
	Job& operator*() { return *p; }
	Job* operator->() { return p; }
	JobHandle( const JobHandle& ) = delete;
	JobHandle& operator=( const JobHandle& ) = delete;
};

//////////////////////////////////////////////////////////////////////
// 1. Long-edge cap dominates on a typical 1920x1080 scene + 4K screen.
//////////////////////////////////////////////////////////////////////

static void TestLongEdgeCapDominates()
{
	std::cout << "Testing long-edge cap dominates over surface size..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.0 );

	// 4K screen surface (3840 x 2160) with a 800-px cap.  The cap is
	// the binding constraint — scale factor = 800 / 1920 ≈ 0.4167.
	Check( job->ScaleFilmToFit( 3840, 2160, 800 ),
		"ScaleFilmToFit(3840x2160, 800) succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film->GetWidth()  == 800, "width capped at 800" );
	Check( film->GetHeight() == 450, "height computed via aspect (450)" );
	Check( film->GetPixelAR() == Scalar( 1.0 ), "pixelAR preserved" );
}

//////////////////////////////////////////////////////////////////////
// 2. Surface width binds on a narrow surface.
//////////////////////////////////////////////////////////////////////

static void TestSurfaceWidthBinds()
{
	std::cout << "Testing surface width binds when narrower than the long-edge cap..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.0 );

	// Narrow tablet portrait (600x900) with the 800 cap.  Width is
	// 600 — tighter than 800/1.78 = 449.  Scale factor = 600/1920 = 0.3125.
	Check( job->ScaleFilmToFit( 600, 900, 800 ),
		"ScaleFilmToFit(600x900, 800) succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film->GetWidth()  == 600, "width = surface width (600)" );
	// 1080 * 600/1920 = 337.5 -> rounds to 338.
	Check( film->GetHeight() == 338, "height rounded via aspect (338)" );
}

//////////////////////////////////////////////////////////////////////
// 3. Surface height binds on a short surface for a tall scene.
//////////////////////////////////////////////////////////////////////

static void TestSurfaceHeightBinds()
{
	std::cout << "Testing surface height binds for a portrait scene..." << std::endl;
	JobHandle job;
	job->SetFilm( 540, 1920, 1.0 );  // portrait, 0.281 aspect

	// Wide-short surface (2000 x 400), 800 cap.
	// Constraints:  s <= 2000/540 = 3.7;  s <= 400/1920 = 0.208;  s <= 800/1920 = 0.417;  s <= 1.
	// Surface height binds at 0.208.
	Check( job->ScaleFilmToFit( 2000, 400, 800 ),
		"ScaleFilmToFit(2000x400, 800) succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film->GetHeight() == 400, "height = surface height (400)" );
	// 540 * 400/1920 = 112.5 -> 113.
	Check( film->GetWidth()  == 113, "width rounded via aspect (113)" );
}

//////////////////////////////////////////////////////////////////////
// 4. Never upscales — a tiny scene stays at its authored size.
//////////////////////////////////////////////////////////////////////

static void TestNoUpscale()
{
	std::cout << "Testing tiny scenes are not upscaled..." << std::endl;
	JobHandle job;
	job->SetFilm( 128, 128, 1.0 );

	// Big screen, 800 cap.  All constraints would allow upscale
	// (6.25, 6.25, 6.25), but the 1.0 cap pins scale at 1.0.
	Check( job->ScaleFilmToFit( 2000, 1500, 800 ),
		"ScaleFilmToFit on tiny scene succeeds" );

	const IFilm* film = job->GetScene()->GetFilm();
	Check( film->GetWidth()  == 128, "width unchanged (128)" );
	Check( film->GetHeight() == 128, "height unchanged (128)" );
}

//////////////////////////////////////////////////////////////////////
// 5. pixelAR is preserved exactly across the resync.
//////////////////////////////////////////////////////////////////////

static void TestPixelARPreserved()
{
	std::cout << "Testing pixelAR preserved across ScaleFilmToFit..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.5 );  // anamorphic
	Check( job->ScaleFilmToFit( 3840, 2160, 800 ),
		"ScaleFilmToFit succeeds" );
	const IFilm* film = job->GetScene()->GetFilm();
	Check( film->GetPixelAR() == Scalar( 1.5 ),
		"pixelAR preserved at 1.5" );
}

//////////////////////////////////////////////////////////////////////
// 6. Cameras resync through the underlying SetFilm path.
//////////////////////////////////////////////////////////////////////

static void TestCameraResyncedViaSetFilm()
{
	std::cout << "Testing cameras resync through the SetFilm path..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.0 );

	const double loc[3]    = {0,0,5};
	const double lookat[3] = {0,0,0};
	const double up[3]     = {0,1,0};
	const double orient[3] = {0,0,0};
	const double tgt[2]    = {0,0};
	Check( job->AddPinholeCamera( "cam", loc, lookat, up,
		Scalar( 0.785398 ), 0, 0, 0, orient, tgt ),
		"AddPinholeCamera succeeds" );

	Check( job->ScaleFilmToFit( 3840, 2160, 800 ),
		"ScaleFilmToFit succeeds" );

	const ICamera* pCam = job->GetScene()->GetCameras()->GetItem( "cam" );
	const CameraCommon* cc = dynamic_cast<const CameraCommon*>( pCam );
	Check( cc != nullptr,                  "camera is CameraCommon" );
	Check( cc && cc->GetWidth()  == 800,   "camera width resynced to 800" );
	Check( cc && cc->GetHeight() == 450,   "camera height resynced to 450" );
}

//////////////////////////////////////////////////////////////////////
// 7. Zero arguments rejected, leave Film unchanged.
//////////////////////////////////////////////////////////////////////

static void TestZeroArgsRejected()
{
	std::cout << "Testing zero-argument rejection..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.0 );
	const IFilm* film = job->GetScene()->GetFilm();

	Check( !job->ScaleFilmToFit( 0,    1000, 800 ), "zero surfaceW rejected" );
	Check( !job->ScaleFilmToFit( 1000, 0,    800 ), "zero surfaceH rejected" );
	Check( !job->ScaleFilmToFit( 1000, 1000, 0   ), "zero maxLongEdge rejected" );

	Check( film->GetWidth()  == 1920, "Film width unchanged after rejects" );
	Check( film->GetHeight() == 1080, "Film height unchanged after rejects" );
}

//////////////////////////////////////////////////////////////////////
// 8. Idempotent — second call with same args is a no-op.
//////////////////////////////////////////////////////////////////////

static void TestIdempotent()
{
	std::cout << "Testing idempotence of repeated ScaleFilmToFit..." << std::endl;
	JobHandle job;
	job->SetFilm( 1920, 1080, 1.0 );

	Check( job->ScaleFilmToFit( 3840, 2160, 800 ), "first call succeeds" );
	const IFilm* film = job->GetScene()->GetFilm();
	const unsigned int w1 = film->GetWidth();
	const unsigned int h1 = film->GetHeight();

	Check( job->ScaleFilmToFit( 3840, 2160, 800 ), "second call succeeds" );
	Check( film->GetWidth()  == w1, "width stable across repeated calls" );
	Check( film->GetHeight() == h1, "height stable across repeated calls" );
}

int main()
{
	std::cout << "=== Job::ScaleFilmToFit Test ===" << std::endl;

	TestLongEdgeCapDominates();
	TestSurfaceWidthBinds();
	TestSurfaceHeightBinds();
	TestNoUpscale();
	TestPixelARPreserved();
	TestCameraResyncedViaSetFilm();
	TestZeroArgsRejected();
	TestIdempotent();

	std::cout << std::endl;
	std::cout << "=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
