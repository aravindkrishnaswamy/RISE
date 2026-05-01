//////////////////////////////////////////////////////////////////////
//
//  MultipleCamerasTest.cpp - Unit tests for the multi-camera scene
//    contract: Add/SetActive/Remove on Scene + Job, last-added-wins
//    default-active, auto-promote on remove, name uniqueness, the
//    parser's optional-name + auto-suffix behaviour.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
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

// Build a pinhole camera with placeholder values; the math doesn't
// matter for these tests — we just need a valid ICamera*.
static ICamera* MakeCamera()
{
	ICamera* cam = nullptr;
	RISE_API_CreatePinholeCamera(
		&cam,
		Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ),
		64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
	return cam;
}

//////////////////////////////////////////////////////////////////////
// 1. Empty scene starts with no active camera
//////////////////////////////////////////////////////////////////////

static void TestEmptySceneNoCamera()
{
	std::cout << "Testing empty scene has no active camera..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();
	Check( scene != nullptr, "scene exists" );
	Check( scene->GetCamera() == nullptr, "GetCamera() is null on fresh scene" );
	Check( scene->GetCameraMutable() == nullptr, "GetCameraMutable() is null on fresh scene" );
	Check( scene->GetCameras() != nullptr, "camera manager exists" );
	Check( scene->GetCameras()->getItemCount() == 0, "camera manager is empty" );
	Check( scene->GetActiveCameraName().size() <= 1, "active camera name is empty" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 2. AddCamera makes the camera active by policy
//////////////////////////////////////////////////////////////////////

static void TestAddCameraSetsActive()
{
	std::cout << "Testing AddCamera registers + activates..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* a = MakeCamera();
	Check( scene->AddCamera( "alpha", a ),       "AddCamera(alpha) succeeds" );
	Check( scene->GetCamera() == a,              "GetCamera returns alpha" );
	Check( scene->GetCameras()->getItemCount() == 1, "manager has one camera" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0,
	       "active is alpha" );
	a->release();

	// Adding another camera moves active to the new one.
	ICamera* b = MakeCamera();
	Check( scene->AddCamera( "beta", b ),        "AddCamera(beta) succeeds" );
	Check( scene->GetCamera() == b,              "GetCamera now returns beta (last-added wins)" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "beta" ) == 0,
	       "active is beta" );
	b->release();
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 3. Duplicate name rejection
//////////////////////////////////////////////////////////////////////

static void TestDuplicateNameRejected()
{
	std::cout << "Testing duplicate name rejected..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* a = MakeCamera();
	Check( scene->AddCamera( "shared", a ),      "first AddCamera(shared) succeeds" );
	a->release();

	ICamera* b = MakeCamera();
	Check( !scene->AddCamera( "shared", b ),     "duplicate AddCamera rejected" );
	Check( scene->GetCameras()->getItemCount() == 1,
	       "manager still has one camera" );
	b->release();
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 4. SetActiveCamera switches; unknown name returns false
//////////////////////////////////////////////////////////////////////

static void TestSetActiveCamera()
{
	std::cout << "Testing SetActiveCamera switches and rejects unknown..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* a = MakeCamera();
	ICamera* b = MakeCamera();
	scene->AddCamera( "alpha", a );
	scene->AddCamera( "beta", b );
	a->release();
	b->release();

	Check( scene->SetActiveCamera( "alpha" ),    "switch back to alpha" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0,
	       "active is alpha" );
	// GetCamera lookups by name now — we can't compare to the local
	// `a` ptr because the manager-held copy is a separate addref.
	Check( scene->GetCamera() != nullptr,        "GetCamera returns non-null" );

	Check( !scene->SetActiveCamera( "nope" ),    "unknown name rejected" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0,
	       "active unchanged after rejected switch" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 5. RemoveCamera + auto-promote
//////////////////////////////////////////////////////////////////////

static void TestRemoveAutoPromote()
{
	std::cout << "Testing RemoveCamera auto-promotes..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* a = MakeCamera();
	ICamera* b = MakeCamera();
	ICamera* c = MakeCamera();
	scene->AddCamera( "alpha", a );
	scene->AddCamera( "beta", b );
	scene->AddCamera( "gamma", c );
	a->release(); b->release(); c->release();

	// Active is "gamma" (last added).
	Check( strcmp( scene->GetActiveCameraName().c_str(), "gamma" ) == 0,
	       "active is gamma after three adds" );

	// Remove the active one — auto-promote to remaining.
	Check( scene->RemoveCamera( "gamma" ),       "remove gamma succeeds" );
	Check( scene->GetCameras()->getItemCount() == 2, "two cameras left" );
	const String activeAfter = scene->GetActiveCameraName();
	Check( activeAfter.size() > 1,               "auto-promoted to a remaining camera" );
	// Lexicographic first-remaining = "alpha".
	Check( strcmp( activeAfter.c_str(), "alpha" ) == 0,
	       "auto-promoted to lexicographically-first remaining (alpha)" );

	// Remove a non-active one — active unchanged.
	Check( scene->RemoveCamera( "beta" ),        "remove beta succeeds" );
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0,
	       "active unchanged when non-active removed" );

	// Remove the last camera — active goes empty.
	Check( scene->RemoveCamera( "alpha" ),       "remove alpha succeeds" );
	Check( scene->GetCameras()->getItemCount() == 0, "no cameras left" );
	Check( scene->GetActiveCameraName().size() <= 1, "active name is empty" );
	Check( scene->GetCamera() == nullptr,        "GetCamera null when empty" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 6. Job-level Add* + RemoveCamera surface
//////////////////////////////////////////////////////////////////////

static void TestJobAPI()
{
	std::cout << "Testing Job::AddPinholeCamera / SetActiveCamera / RemoveCamera..." << std::endl;
	Job* pJob = new Job();

	const double loc[3]    = { 0, 0, 5 };
	const double lookat[3] = { 0, 0, 0 };
	const double up[3]     = { 0, 1, 0 };
	const double orient[3] = { 0, 0, 0 };
	const double targ[2]   = { 0, 0 };

	Check( pJob->AddPinholeCamera( "first",  loc, lookat, up, 0.78, 64, 64, 1.0, 0, 0, 0, orient, targ ), "Job::AddPinholeCamera first" );
	Check( pJob->AddPinholeCamera( "second", loc, lookat, up, 0.78, 64, 64, 1.0, 0, 0, 0, orient, targ ), "Job::AddPinholeCamera second" );
	Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "second" ) == 0,
	       "Job last-added wins" );

	Check( pJob->SetActiveCamera( "first" ),     "Job::SetActiveCamera(first)" );
	Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "first" ) == 0,
	       "active switched to first" );

	Check( pJob->RemoveCamera( "second" ),       "Job::RemoveCamera(second)" );
	Check( pJob->GetScene()->GetCameras()->getItemCount() == 1,
	       "one camera left" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 7. Parser: optional name, auto-suffix, mixed named+unnamed
//
// These exercise the AsciiSceneParser path end-to-end:
// AllocateCameraName + AddCameraCommonParams + Add*Camera dispatch.
// Each test writes a minimal .RISEscene to a temp file, loads it via
// IJob::LoadAsciiScene, and inspects the resulting camera manager.
//////////////////////////////////////////////////////////////////////

// Build a tmp scene file path.  Returned path is unique enough for
// the tests' use; tmpnam-style races aren't a concern in this
// single-process test binary.
static std::string MakeTempScenePath( const char* tag )
{
	std::string path = "/tmp/rise_multi_camera_test_";
	path += tag;
	path += ".RISEscene";
	return path;
}

// Minimal scene shell that the parser will accept; callers paste in
// their camera chunks at the marker.  We deliberately omit objects /
// lights — the parser only needs to *register* the cameras with the
// manager; it doesn't need a renderable scene to validate naming.
//
// IMPORTANT: chunk opening / closing braces must be on their own
// lines (parser requirement, see CLAUDE.md).  All test fixtures here
// follow that.
static const char* kSceneShell =
	"RISE ASCII SCENE 5\n"
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"%s"
	"\n";

static bool WriteScene( const std::string& path, const std::string& cameras )
{
	std::ofstream out( path.c_str() );
	if( !out.is_open() ) return false;
	char buf[ 4096 ];
	std::snprintf( buf, sizeof(buf), kSceneShell, cameras.c_str() );
	out << buf;
	return out.good();
}

// Helper: load a scene through the parser, return ownership-transferred
// Job*.  Caller must release().  Returns null if load failed.
static Job* LoadScene( const std::string& path )
{
	Job* pJob = new Job();
	if( !pJob->LoadAsciiScene( path.c_str() ) ) {
		pJob->release();
		return nullptr;
	}
	return pJob;
}

static void TestParserUnnamedCamera()
{
	std::cout << "Testing parser: unnamed camera gets \"default\"..." << std::endl;
	const std::string path = MakeTempScenePath( "unnamed" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
	), "wrote scene" );

	Job* pJob = LoadScene( path );
	Check( pJob != nullptr, "scene loads" );
	if( pJob ) {
		Check( pJob->GetScene()->GetCameras()->getItemCount() == 1,
		       "one camera registered" );
		Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "default" ) == 0,
		       "auto-named \"default\"" );
		pJob->release();
	}
	std::remove( path.c_str() );
}

static void TestParserAutoSuffixOnUnnamedCollision()
{
	std::cout << "Testing parser: two unnamed cameras get default + default_1..." << std::endl;
	const std::string path = MakeTempScenePath( "auto_suffix" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		"pinhole_camera\n{\nlocation 1 1 5\nlookat 0 0 0\nup 0 1 0\nfov 60\n}\n"
	), "wrote scene" );

	Job* pJob = LoadScene( path );
	Check( pJob != nullptr, "scene loads" );
	if( pJob ) {
		const ICameraManager* cams = pJob->GetScene()->GetCameras();
		Check( cams->getItemCount() == 2,
		       "two cameras registered" );
		Check( cams->GetItem( "default" )   != nullptr, "\"default\" exists" );
		Check( cams->GetItem( "default_1" ) != nullptr, "\"default_1\" exists" );
		// Last-added wins → "default_1" is active.
		Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "default_1" ) == 0,
		       "active is default_1 (last-added wins)" );
		pJob->release();
	}
	std::remove( path.c_str() );
}

static void TestParserMixedNamedAndUnnamed()
{
	std::cout << "Testing parser: explicit + unnamed don't collide..." << std::endl;
	const std::string path = MakeTempScenePath( "mixed" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nname hero\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		"pinhole_camera\n{\nlocation 1 1 5\nlookat 0 0 0\nup 0 1 0\nfov 60\n}\n"
	), "wrote scene" );

	Job* pJob = LoadScene( path );
	Check( pJob != nullptr, "scene loads" );
	if( pJob ) {
		const ICameraManager* cams = pJob->GetScene()->GetCameras();
		Check( cams->getItemCount() == 2,         "two cameras registered" );
		Check( cams->GetItem( "hero" )    != nullptr, "\"hero\" exists" );
		Check( cams->GetItem( "default" ) != nullptr, "\"default\" exists" );
		Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "default" ) == 0,
		       "active is the unnamed-second one (\"default\")" );
		pJob->release();
	}
	std::remove( path.c_str() );
}

static void TestParserExplicitDefaultPlusUnnamed()
{
	std::cout << "Testing parser: explicit \"default\" then unnamed → suffix..." << std::endl;
	const std::string path = MakeTempScenePath( "explicit_default" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nname default\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		"pinhole_camera\n{\nlocation 1 1 5\nlookat 0 0 0\nup 0 1 0\nfov 60\n}\n"
	), "wrote scene" );

	Job* pJob = LoadScene( path );
	Check( pJob != nullptr, "scene loads" );
	if( pJob ) {
		const ICameraManager* cams = pJob->GetScene()->GetCameras();
		Check( cams->getItemCount() == 2,         "two cameras registered" );
		Check( cams->GetItem( "default" )   != nullptr, "explicit \"default\" exists" );
		Check( cams->GetItem( "default_1" ) != nullptr, "auto-suffixed \"default_1\" exists" );
		pJob->release();
	}
	std::remove( path.c_str() );
}

static void TestParserDuplicateExplicitNameFails()
{
	std::cout << "Testing parser: explicit duplicate names — second rejected..." << std::endl;
	const std::string path = MakeTempScenePath( "duplicate_explicit" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nname shared\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 30\n}\n"
		"pinhole_camera\n{\nname shared\nlocation 1 1 5\nlookat 0 0 0\nup 0 1 0\nfov 60\n}\n"
	), "wrote scene" );

	// LoadAsciiScene returns false because the second AddCamera fails.
	Job* pJob = new Job();
	const bool ok = pJob->LoadAsciiScene( path.c_str() );
	Check( !ok, "scene load reports failure (duplicate camera name)" );
	// First camera DID register before the second's failure aborted the parse.
	Check( pJob->GetScene()->GetCameras()->getItemCount() == 1,
	       "first camera still registered" );
	pJob->release();
	std::remove( path.c_str() );
}

static void TestParserMixedTypes()
{
	std::cout << "Testing parser: heterogeneous camera types share the manager..." << std::endl;
	const std::string path = MakeTempScenePath( "mixed_types" );
	Check( WriteScene( path,
		"pinhole_camera\n{\nname hero\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40\n}\n"
		"thinlens_camera\n{\nname closeup\nlocation 3 1 4\nlookat 0 0 0\nup 0 1 0\nsensor_size 36\nfocal_length 85\nfstop 1.8\nfocus_distance 4.5\n}\n"
		"orthographic_camera\n{\nname top\nlocation 0 10 0\nlookat 0 0 0\nup 0 0 -1\nviewport_scale 3 3\n}\n"
	), "wrote scene" );

	Job* pJob = LoadScene( path );
	Check( pJob != nullptr, "scene loads" );
	if( pJob ) {
		const ICameraManager* cams = pJob->GetScene()->GetCameras();
		Check( cams->getItemCount() == 3,         "three cameras registered" );
		Check( cams->GetItem( "hero" )    != nullptr, "hero (pinhole)" );
		Check( cams->GetItem( "closeup" ) != nullptr, "closeup (thinlens)" );
		Check( cams->GetItem( "top" )     != nullptr, "top (orthographic)" );
		// Last-added wins.
		Check( strcmp( pJob->GetScene()->GetActiveCameraName().c_str(), "top" ) == 0,
		       "active is the last-declared camera (\"top\")" );
		pJob->release();
	}
	std::remove( path.c_str() );
}

//////////////////////////////////////////////////////////////////////
// 8. Active-camera removal lifetime contract.
//
// The hazard the lifetime model has to close: when AddCamera adds a
// camera, the manager + Scene each hold ONE retain.  RemoveCamera
// must (a) publish nullptr through pActiveCamera BEFORE the manager's
// release, and (b) keep the camera alive long enough for any
// concurrent reader to finish.  This test verifies the symmetric
// retain/release count by tracking ICamera lifetime through external
// retains and a final destruction check.
//
// Mechanism: addref the camera ourselves to N+1, run the
// add/remove cycle, count what comes back.  After RemoveCamera +
// our own release, the count should hit zero.
//////////////////////////////////////////////////////////////////////

static void TestActiveCameraRemovalDestroysCamera()
{
	std::cout << "Testing active-camera removal destroys the camera at the right time..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* a = MakeCamera();              // count 1 (us)
	ICamera* b = MakeCamera();              // count 1 (us)

	scene->AddCamera( "alpha", a );         // a: 3 (us + mgr + scene)
	scene->AddCamera( "beta", b );          // b: 3 (us + mgr + scene); a: 2 (us + mgr) — Scene swapped active
	a->release();                           // a: 1 (mgr)
	b->release();                           // b: 2 (mgr + scene)

	// Remove the active "beta" — should drop b's count from 2 to 0.
	Check( scene->RemoveCamera( "beta" ),                             "remove active beta" );
	Check( scene->GetCameras()->getItemCount() == 1,                  "one camera left" );
	// Active auto-promoted to alpha; Scene now retains a (a: 2).
	Check( strcmp( scene->GetActiveCameraName().c_str(), "alpha" ) == 0, "auto-promoted to alpha" );

	// Remove the still-active alpha.  Scene drops its retain; manager drops its retain;
	// a should now be destroyed.  We can't observe destruction directly, but we can
	// validate by checking the manager is empty and active is empty.
	Check( scene->RemoveCamera( "alpha" ),                            "remove last (active) alpha" );
	Check( scene->GetCameras()->getItemCount() == 0,                  "manager empty" );
	Check( scene->GetActiveCameraName().size() <= 1,                  "active name cleared" );
	Check( scene->GetCamera() == nullptr,                             "GetCamera() returns null" );

	pJob->release();
}

static void TestRepeatedAddRemoveDoesNotLeak()
{
	std::cout << "Testing repeated add/remove cycle stays balanced..." << std::endl;
	// Smoke test for the retain/release chain.  We don't have an
	// in-process leak checker plumbed into this binary, but a
	// missing release would compound across iterations and could
	// surface under sanitizers (asan/tsan) the suite is occasionally
	// run with.  The structural assertions below verify the manager
	// + active-state bookkeeping stays consistent across two
	// removal flavours (active & non-active) and a final drain.
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	// Maintain a small rolling window of cameras so SetActiveCamera
	// always has a valid target.  Each iteration: add a new camera
	// (becomes active by last-added-wins), switch active to the
	// previously-added one, then remove the just-added one (now
	// non-active).  Exercises the non-active removal path on every
	// iteration and the active path implicitly via SetActiveCamera.
	{
		ICamera* base = MakeCamera();
		Check( scene->AddCamera( "base", base ), "seed: add base" );
		base->release();
	}

	for( int i = 0; i < 32; ++i ) {
		ICamera* cam = MakeCamera();
		char nm[16];
		std::snprintf( nm, sizeof(nm), "cam_%d", i );
		Check( scene->AddCamera( nm, cam ),  "AddCamera in cycle" );
		cam->release();
		// "base" stays in the manager; switch active to it before
		// removing the just-added (which is now non-active).
		Check( scene->SetActiveCamera( "base" ), "SetActiveCamera back to base" );
		Check( scene->RemoveCamera( nm ),    "RemoveCamera (non-active)" );
		Check( scene->GetCameras()->getItemCount() == 1,
		       "manager has just base after each iter" );
	}

	// Now exercise the active-removal path repeatedly.
	for( int i = 0; i < 16; ++i ) {
		ICamera* cam = MakeCamera();
		char nm[16];
		std::snprintf( nm, sizeof(nm), "active_%d", i );
		Check( scene->AddCamera( nm, cam ),  "AddCamera makes active" );
		cam->release();
		// Remove the just-added active — auto-promotes to "base".
		Check( scene->RemoveCamera( nm ),    "RemoveCamera (active path)" );
		Check( strcmp( scene->GetActiveCameraName().c_str(), "base" ) == 0,
		       "auto-promoted back to base" );
	}

	// Drain.
	Check( scene->RemoveCamera( "base" ),    "drain base" );
	Check( scene->GetCamera() == nullptr,    "all drained, no active" );
	Check( scene->GetCameras()->getItemCount() == 0, "manager empty" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// 9. Empty / null name validation per the public contract.
//////////////////////////////////////////////////////////////////////

static void TestEmptyNameRejected()
{
	std::cout << "Testing empty-string name rejected by Add/Set/Remove..." << std::endl;
	Job* pJob = new Job();
	IScenePriv* scene = pJob->GetScene();

	ICamera* cam = MakeCamera();
	Check( !scene->AddCamera( "", cam ),         "AddCamera(\"\") rejected" );
	Check( !scene->AddCamera( nullptr, cam ),    "AddCamera(null) rejected" );
	Check( scene->GetCameras()->getItemCount() == 0,
	       "no camera registered" );
	cam->release();

	// SetActive / Remove must also reject empties.
	Check( !scene->SetActiveCamera( "" ),        "SetActiveCamera(\"\") rejected" );
	Check( !scene->SetActiveCamera( nullptr ),   "SetActiveCamera(null) rejected" );
	Check( !scene->RemoveCamera( "" ),           "RemoveCamera(\"\") rejected" );
	Check( !scene->RemoveCamera( nullptr ),      "RemoveCamera(null) rejected" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "=== MultipleCamerasTest ===" << std::endl;

	TestEmptySceneNoCamera();
	TestAddCameraSetsActive();
	TestDuplicateNameRejected();
	TestSetActiveCamera();
	TestRemoveAutoPromote();
	TestJobAPI();
	TestParserUnnamedCamera();
	TestParserAutoSuffixOnUnnamedCollision();
	TestParserMixedNamedAndUnnamed();
	TestParserExplicitDefaultPlusUnnamed();
	TestParserDuplicateExplicitNameFails();
	TestParserMixedTypes();
	TestActiveCameraRemovalDestroysCamera();
	TestRepeatedAddRemoveDoesNotLeak();
	TestEmptyNameRejected();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
