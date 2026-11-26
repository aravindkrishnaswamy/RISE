//////////////////////////////////////////////////////////////////////
//
//  SceneEditorAddCameraTest.cpp — End-to-end coverage for the
//  multi-camera "Add Camera" flow added in Phase 3 of the
//  interactive editor:
//
//    - CameraIntrospection::CaptureCameraSnapshot reads every field
//      from each of the four built-in camera types into a value-typed
//      snapshot.
//    - CameraIntrospection::AddCameraFromSnapshot rebuilds the camera
//      under a new name and the result is field-by-field identical
//      to the source.
//    - SceneEditor::Apply for SceneEdit::AddCamera promotes the new
//      camera to active and pushes an undo entry.
//    - Undo removes the new camera and restores the prior active.
//    - Redo re-creates the camera from the captured snapshot.
//    - Editing the source camera AFTER Apply does NOT change the
//      Redo'd camera — snapshot is captured at Apply time, not
//      re-read at Redo time.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Cameras/FisheyeCamera.h"
#include "../src/Library/Cameras/OrthographicCamera.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/SceneEditor/SceneEditor.h"
#include "../src/Library/SceneEditor/SceneEdit.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static bool ApproxEq( Scalar a, Scalar b, Scalar eps = 1e-9 )
{
	return std::fabs( a - b ) <= eps;
}

static bool ApproxEq( const Point3& a, const Point3& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps ) && ApproxEq( a.y, b.y, eps ) && ApproxEq( a.z, b.z, eps );
}

static bool ApproxEq( const Vector3& a, const Vector3& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps ) && ApproxEq( a.y, b.y, eps ) && ApproxEq( a.z, b.z, eps );
}

static bool ApproxEq( const Vector2& a, const Vector2& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps ) && ApproxEq( a.y, b.y, eps );
}

//////////////////////////////////////////////////////////////////////

// Attach a Film so the Add*Camera path can read default dims.
static void AttachFilm( Job& job )
{
	job.SetFilm( 64, 64, 1.0 );
}

static void AddPinhole( Job& job, const char* name,
                        const Point3& loc, const Point3& lookAt )
{
	double l[3]  = { loc.x, loc.y, loc.z };
	double la[3] = { lookAt.x, lookAt.y, lookAt.z };
	double up[3] = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	job.AddPinholeCamera( name, l, la, up,
		Scalar( 0.785398 ),    // fov
		Scalar( 0.5 ),         // exposure
		Scalar( 0 ),           // scanningRate
		Scalar( 0 ),           // pixelRate
		orient, target,
		Scalar( 0 ),           // iso (off)
		Scalar( 0 ) );         // fstop
}

//////////////////////////////////////////////////////////////////////

static void TestPinholeSnapshotRoundTrip()
{
	std::cout << "Testing pinhole CaptureCameraSnapshot → AddCameraFromSnapshot field-for-field..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );
	AddPinhole( *pJob, "src", Point3( 1, 2, 3 ), Point3( 4, 5, 6 ) );

	ICamera* src = pJob->GetScene()->GetCameras()->GetItem( "src" );
	Check( src != 0, "source camera registered" );

	CameraSnapshot snap;
	Check( CameraIntrospection::CaptureCameraSnapshot( *src, snap ), "capture pinhole snapshot" );
	Check( snap.type == CameraSnapshot::Pinhole, "snapshot type = Pinhole" );
	Check( ApproxEq( Point3(snap.location[0], snap.location[1], snap.location[2]), Point3( 1, 2, 3 ) ),
	       "location captured" );
	Check( ApproxEq( Point3(snap.lookat[0], snap.lookat[1], snap.lookat[2]), Point3( 4, 5, 6 ) ),
	       "lookat captured" );
	Check( ApproxEq( snap.fov, 0.785398 ), "fov captured" );
	Check( ApproxEq( snap.exposure, 0.5 ), "exposure captured" );

	Check( CameraIntrospection::AddCameraFromSnapshot( *pJob, String( "clone" ), snap ),
	       "add clone from snapshot" );

	ICamera* cln = pJob->GetScene()->GetCameras()->GetItem( "clone" );
	Check( cln != 0, "clone registered" );
	const CameraCommon* srcC = dynamic_cast<const CameraCommon*>( src );
	const CameraCommon* clnC = dynamic_cast<const CameraCommon*>( cln );
	const PinholeCamera* srcP = dynamic_cast<const PinholeCamera*>( src );
	const PinholeCamera* clnP = dynamic_cast<const PinholeCamera*>( cln );
	Check( srcC && clnC && srcP && clnP, "both clones are PinholeCamera" );
	if( srcC && clnC ) {
		Check( ApproxEq( srcC->GetRestLocation(), clnC->GetRestLocation() ), "rest location identical" );
		Check( ApproxEq( srcC->GetStoredLookAt(), clnC->GetStoredLookAt() ), "stored lookat identical" );
		Check( ApproxEq( srcC->GetStoredUp(),     clnC->GetStoredUp() ),     "stored up identical" );
		Check( ApproxEq( srcC->GetExposureTimeStored(), clnC->GetExposureTimeStored() ),
		       "exposure identical" );
	}
	if( srcP && clnP ) {
		Check( ApproxEq( srcP->GetFovStored(), clnP->GetFovStored() ), "fov identical" );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestThinLensSnapshotRoundTrip()
{
	std::cout << "Testing thinlens CaptureCameraSnapshot field round-trip..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );

	double loc[3] = { 0, 0, 5 };
	double la[3]  = { 0, 0, 0 };
	double up[3]  = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	pJob->AddThinlensCamera( "src", loc, la, up,
		36.0,      // sensorSize (mm)
		50.0,      // focalLength (mm)
		2.8,       // fstop
		5.0,       // focusDistance
		1.0,       // sceneUnitMeters
		0.5, 0, 0, // exposure / scanningRate / pixelRate
		orient, target,
		6,         // apertureBlades
		0.1,       // apertureRotation (rad)
		1.0,       // anamorphicSqueeze
		0, 0,      // tiltX, tiltY
		0, 0,      // shiftX, shiftY
		100.0 );   // iso

	ICamera* src = pJob->GetScene()->GetCameras()->GetItem( "src" );
	Check( src != 0, "thinlens source registered" );

	CameraSnapshot snap;
	Check( CameraIntrospection::CaptureCameraSnapshot( *src, snap ), "capture thinlens snapshot" );
	Check( snap.type == CameraSnapshot::ThinLens, "snapshot type = ThinLens" );
	Check( ApproxEq( snap.sensorSize, 36.0 ), "sensorSize captured" );
	Check( ApproxEq( snap.focalLength, 50.0 ), "focalLength captured" );
	Check( ApproxEq( snap.fstop, 2.8 ), "fstop captured" );
	Check( ApproxEq( snap.focusDistance, 5.0 ), "focusDistance captured" );
	Check( snap.apertureBlades == 6u, "apertureBlades captured" );
	Check( ApproxEq( snap.iso, 100.0 ), "iso captured" );

	Check( CameraIntrospection::AddCameraFromSnapshot( *pJob, String( "clone" ), snap ),
	       "add thinlens clone from snapshot" );
	const ThinLensCamera* clnT = dynamic_cast<const ThinLensCamera*>(
		pJob->GetScene()->GetCameras()->GetItem( "clone" ) );
	Check( clnT != 0, "clone is ThinLensCamera" );
	if( clnT ) {
		Check( ApproxEq( clnT->GetSensorSize(), 36.0 ), "clone sensorSize identical" );
		Check( ApproxEq( clnT->GetFstop(), 2.8 ), "clone fstop identical" );
		Check( clnT->GetApertureBlades() == 6u, "clone apertureBlades identical" );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestFisheyeSnapshotRoundTrip()
{
	std::cout << "Testing fisheye CaptureCameraSnapshot field round-trip..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );

	double loc[3] = { 0, 0, 0 };
	double la[3]  = { 1, 0, 0 };
	double up[3]  = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	pJob->AddFisheyeCamera( "src", loc, la, up,
		1.0, 0, 0,    // exposure / scanningRate / pixelRate
		orient, target,
		2.5 );        // fisheye scale

	CameraSnapshot snap;
	Check( CameraIntrospection::CaptureCameraSnapshot(
		*pJob->GetScene()->GetCameras()->GetItem( "src" ), snap ), "capture fisheye" );
	Check( snap.type == CameraSnapshot::Fisheye, "snapshot type = Fisheye" );
	Check( ApproxEq( snap.fisheyeScale, 2.5 ), "fisheyeScale captured" );

	Check( CameraIntrospection::AddCameraFromSnapshot( *pJob, String( "clone" ), snap ),
	       "add fisheye clone" );
	const FisheyeCamera* clnF = dynamic_cast<const FisheyeCamera*>(
		pJob->GetScene()->GetCameras()->GetItem( "clone" ) );
	Check( clnF != 0, "clone is FisheyeCamera" );
	if( clnF ) {
		Check( ApproxEq( clnF->GetScaleStored(), 2.5 ), "clone scale identical" );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestOrthographicSnapshotRoundTrip()
{
	std::cout << "Testing orthographic CaptureCameraSnapshot field round-trip..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );

	double loc[3] = { 0, 0, 10 };
	double la[3]  = { 0, 0, 0 };
	double up[3]  = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	double vp[2] = { 2.0, 3.0 };
	pJob->AddOrthographicCamera( "src", loc, la, up,
		vp, 0.25, 0, 0, orient, target );

	CameraSnapshot snap;
	Check( CameraIntrospection::CaptureCameraSnapshot(
		*pJob->GetScene()->GetCameras()->GetItem( "src" ), snap ), "capture ortho" );
	Check( snap.type == CameraSnapshot::Orthographic, "snapshot type = Orthographic" );
	Check( ApproxEq( snap.viewportScale[0], 2.0 ) && ApproxEq( snap.viewportScale[1], 3.0 ),
	       "viewportScale captured" );

	Check( CameraIntrospection::AddCameraFromSnapshot( *pJob, String( "clone" ), snap ),
	       "add ortho clone" );
	const OrthographicCamera* clnO = dynamic_cast<const OrthographicCamera*>(
		pJob->GetScene()->GetCameras()->GetItem( "clone" ) );
	Check( clnO != 0, "clone is OrthographicCamera" );
	if( clnO ) {
		const Vector2 vs = clnO->GetViewportScaleStored();
		Check( ApproxEq( vs, Vector2( 2.0, 3.0 ) ), "clone viewportScale identical" );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestAddCameraUndoRedo()
{
	std::cout << "Testing SceneEdit::AddCamera Apply / Undo / Redo round-trip..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );
	AddPinhole( *pJob, "main", Point3( 0, 0, 5 ), Point3( 0, 0, 0 ) );
	pJob->SetActiveCamera( "main" );

	SceneEditor editor( *pJob->GetScene() );
	editor.SetJob( pJob );

	// Build the snapshot from the active camera.
	CameraSnapshot snap;
	Check( CameraIntrospection::CaptureCameraSnapshot(
		*pJob->GetScene()->GetCamera(), snap ), "capture active for clone" );

	SceneEdit edit;
	edit.op = SceneEdit::AddCamera;
	edit.objectName = String( "main_copy" );
	edit.cameraSnapshot = snap;
	Check( editor.Apply( edit ), "Apply AddCamera succeeds" );

	// New camera is registered and active.
	Check( pJob->GetScene()->GetCameras()->GetItem( "main_copy" ) != 0,
	       "post-Apply clone is registered" );
	Check( pJob->GetActiveCameraName() == std::string( "main_copy" ),
	       "post-Apply active camera = new clone" );

	// Undo removes the clone and restores prior active.
	Check( editor.Undo(), "Undo AddCamera succeeds" );
	Check( pJob->GetScene()->GetCameras()->GetItem( "main_copy" ) == 0,
	       "post-Undo clone is removed" );
	Check( pJob->GetActiveCameraName() == std::string( "main" ),
	       "post-Undo active restored to prior" );

	// Redo recreates the clone with the captured field set.
	Check( editor.Redo(), "Redo AddCamera succeeds" );
	Check( pJob->GetScene()->GetCameras()->GetItem( "main_copy" ) != 0,
	       "post-Redo clone is back" );
	Check( pJob->GetActiveCameraName() == std::string( "main_copy" ),
	       "post-Redo active = clone" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

// Regression: snapshot is captured at Apply time, so editing the
// source camera AFTER Apply must NOT change the clone's Redo'd state.
// This is the property that justifies the embedded snapshot vs
// re-reading from a source-name reference at Redo time.
static void TestRedoUsesCapturedSnapshot()
{
	std::cout << "Testing Redo uses captured snapshot, not re-read source state..." << std::endl;

	Job* pJob = new Job();
	AttachFilm( *pJob );
	AddPinhole( *pJob, "main", Point3( 1, 2, 3 ), Point3( 0, 0, 0 ) );
	pJob->SetActiveCamera( "main" );

	SceneEditor editor( *pJob->GetScene() );
	editor.SetJob( pJob );

	CameraSnapshot snap;
	CameraIntrospection::CaptureCameraSnapshot( *pJob->GetScene()->GetCamera(), snap );

	SceneEdit edit;
	edit.op = SceneEdit::AddCamera;
	edit.objectName = String( "main_copy" );
	edit.cameraSnapshot = snap;
	editor.Apply( edit );

	// Source camera was cloned with location (1,2,3).
	const PinholeCamera* clone =
		dynamic_cast<const PinholeCamera*>( pJob->GetScene()->GetCameras()->GetItem( "main_copy" ) );
	Check( clone && ApproxEq( clone->GetRestLocation(), Point3( 1, 2, 3 ) ),
	       "fresh clone matches source (1,2,3)" );

	// Undo, then mutate the source camera to a different location.
	editor.Undo();
	if( PinholeCamera* src =
		dynamic_cast<PinholeCamera*>( pJob->GetScene()->GetCameras()->GetItem( "main" ) ) )
	{
		src->SetLocation( Point3( 99, 99, 99 ) );
		src->RegenerateData();
	}

	// Redo — clone should STILL be at (1,2,3), NOT (99,99,99).
	editor.Redo();
	clone = dynamic_cast<const PinholeCamera*>( pJob->GetScene()->GetCameras()->GetItem( "main_copy" ) );
	Check( clone && ApproxEq( clone->GetRestLocation(), Point3( 1, 2, 3 ) ),
	       "Redo'd clone retains Apply-time snapshot, ignores post-Apply edits to source" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	TestPinholeSnapshotRoundTrip();
	TestThinLensSnapshotRoundTrip();
	TestFisheyeSnapshotRoundTrip();
	TestOrthographicSnapshotRoundTrip();
	TestAddCameraUndoRedo();
	TestRedoUsesCapturedSnapshot();

	std::cout << std::endl;
	std::cout << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
