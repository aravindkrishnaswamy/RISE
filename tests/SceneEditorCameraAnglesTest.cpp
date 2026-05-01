//////////////////////////////////////////////////////////////////////
//
//  SceneEditorCameraAnglesTest.cpp — End-to-end tests for the
//  angle-driven camera ops (OrbitCamera mutates target_orientation,
//  RollCamera mutates orientation.z) and their undo round-trip.
//
//  These ops are the source of truth for orbit and roll: the
//  parameters they mutate (target_orientation and orientation) are
//  declared in the .RISEscene parser descriptor, are keyframable
//  through CameraCommon's existing TARGET_ORIENTATION_ID and
//  ORIENTATION_ID animator IDs, and round-trip through the parser.
//  The key correctness property tested here is that:
//
//    - OrbitCamera changes only target_orientation and not vPosition.
//    - RollCamera changes only orientation.z and not the rest.
//    - Undo restores the previous angular state byte-for-byte.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IScenePriv.h"
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

static bool ApproxEq( const Vector2& a, const Vector2& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps ) && ApproxEq( a.y, b.y, eps );
}

static bool ApproxEq( const Vector3& a, const Vector3& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps )
	    && ApproxEq( a.y, b.y, eps )
	    && ApproxEq( a.z, b.z, eps );
}

static bool ApproxEq( const Point3& a, const Point3& b, Scalar eps = 1e-9 )
{
	return ApproxEq( a.x, b.x, eps )
	    && ApproxEq( a.y, b.y, eps )
	    && ApproxEq( a.z, b.z, eps );
}

static void AttachDefaultCamera( Job& job )
{
	ICamera* pCam = nullptr;
	if( !RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 0, 5 ),
		Point3( 0, 0, 0 ),
		Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ),
		64, 64,
		Scalar( 1 ),
		Scalar( 0 ),
		Scalar( 0 ),
		Scalar( 0 ),
		Vector3( 0, 0, 0 ),   // orientation (pitch, yaw, roll)
		Vector2( 0, 0 ) ) )   // target_orientation (theta, phi)
	{
		return;
	}
	if( IScenePriv* scene = job.GetScene() ) {
		scene->AddCamera( "default", pCam );
	}
	pCam->release();
}

//////////////////////////////////////////////////////////////////////

static void TestOrbitMutatesTargetOrientation()
{
	std::cout << "Testing OrbitCamera mutates target_orientation, not the rest position..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );
	Check( cam != nullptr, "camera is CameraCommon" );

	// Two positions are exposed by CameraCommon and must be
	// distinguished here:
	//   - GetLocation()      = frame.GetOrigin()  (POST-orbit, what
	//                          the rasterizer renders from).  WILL
	//                          change under OrbitCamera because the
	//                          orbit rotates the eye around lookat.
	//   - GetRestLocation()  = vPosition          (PRE-orbit rest).
	//                          Must NOT change under OrbitCamera —
	//                          orbit is parametrised as angles, NOT
	//                          baked into vPosition.  This is the
	//                          field shown in the editor properties
	//                          panel as "location".
	const Point3  rest0 = cam->GetRestLocation();
	const Point3  loc0  = cam->GetLocation();
	const Vector2 t0    = cam->GetTargetOrientation();
	const Vector3 o0    = cam->GetEulerOrientation();

	SceneEdit edit;
	edit.op  = SceneEdit::OrbitCamera;
	edit.v3a = Vector3( 100, 50, 0 );   // 100px phi delta, 50px theta delta
	Check( editor.Apply( edit ), "Apply OrbitCamera succeeds" );

	const Vector2 t1 = cam->GetTargetOrientation();
	const Vector3 o1 = cam->GetEulerOrientation();

	Check( ApproxEq( o0, o1 ),  "OrbitCamera does NOT mutate orientation (pitch/yaw/roll)" );
	Check( !ApproxEq( t0, t1 ), "OrbitCamera DOES mutate target_orientation" );
	// 100 px × 0.0087 rad/px = 0.87 rad delta on phi (.y).
	// X axis is NEGATED for grab-the-scene feel — drag-right
	// (positive v3a.x) yields a NEGATIVE phi delta so the scene
	// appears to rotate toward the pointer.
	Check( ApproxEq( t1.y - t0.y, -100 * 0.0087, 1e-6 ), "phi delta matches -v3a.x × 0.0087 (grab-world X)" );
	Check( ApproxEq( t1.x - t0.x,  50 * 0.0087, 1e-6 ), "theta delta matches v3a.y × 0.0087" );

	// The rest position (vPosition, what the panel shows as "location")
	// must NOT change.  This is the regression test for the user's
	// bug report: "orbit appears to change camera location".
	Check( ApproxEq( cam->GetRestLocation(), rest0 ),
	       "OrbitCamera does NOT mutate vPosition (panel 'location' field)" );

	// Roundtrip: zero out target_orientation manually and verify
	// GetLocation returns to its original value.  If OrbitCamera had
	// baked the orbit into vPosition, this would NOT happen — the
	// camera would still be at the orbited location.
	cam->SetTargetOrientation( Vector2( 0, 0 ) );
	cam->RegenerateData();
	Check( ApproxEq( cam->GetLocation(), loc0, 1e-6 ),
	       "zeroing target_orientation restores GetLocation — orbit is angle-stored, not position-baked" );

	// Restore the orbited state and verify Undo brings the angles
	// back to the original.
	cam->SetTargetOrientation( t1 );
	cam->RegenerateData();
	Check( editor.Undo(), "Undo OrbitCamera succeeds" );
	Check( ApproxEq( cam->GetTargetOrientation(), t0 ), "Undo restores target_orientation" );
	Check( ApproxEq( cam->GetLocation(), loc0, 1e-6 ), "post-Undo GetLocation equals pre-Apply location" );
	Check( ApproxEq( cam->GetRestLocation(), rest0 ), "post-Undo rest position unchanged" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
//  Regression for the orbit↔pan/zoom interaction.  Before the fix,
//  Pan/Zoom mutated `vPosition` based on the POST-orbit eye position;
//  Recompute then re-applied the orbit on top of the new vPosition,
//  double-applying the rotation.  Symptoms: any non-zero
//  target_orientation made pan/zoom drift unpredictably and broke
//  Undo (which captured POST-orbit GetLocation as the "previous"
//  vPosition).
//
//  The post-fix invariant is: with non-zero target_orientation, the
//  identity Apply(Pan delta) / Undo() round-trips both rest position
//  and post-orbit eye position to within FP epsilon.
//
static void TestPanComposesWithOrbit()
{
	std::cout << "Testing PanCamera composes correctly with non-zero orbit..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	// Pre-orbit the camera so target_orientation is non-zero.
	cam->SetTargetOrientation( Vector2( 0.3, 0.7 ) );
	cam->RegenerateData();

	const Point3  rest0  = cam->GetRestLocation();
	const Point3  eye0   = cam->GetLocation();
	const Vector2 t0     = cam->GetTargetOrientation();

	SceneEdit pan;
	pan.op  = SceneEdit::PanCamera;
	pan.v3a = Vector3( 25, -10, 0 );
	Check( editor.Apply( pan ), "Apply PanCamera succeeds" );

	// target_orientation must NOT change under pan.
	Check( ApproxEq( cam->GetTargetOrientation(), t0 ),
	       "Pan does not mutate target_orientation" );
	// Rest position must move (the pan delta in screen space).
	Check( !ApproxEq( cam->GetRestLocation(), rest0 ),
	       "Pan moves vPosition" );

	// Undo: rest position AND post-orbit eye must round-trip exactly.
	// Before the fix, Undo would write the POST-orbit eye back into
	// vPosition, then Recompute would re-rotate it — the pre-edit
	// eye position would NOT be recoverable.
	Check( editor.Undo(), "Undo PanCamera succeeds" );
	Check( ApproxEq( cam->GetRestLocation(), rest0, 1e-9 ),
	       "Undo restores vPosition (rest) exactly" );
	Check( ApproxEq( cam->GetLocation(), eye0, 1e-6 ),
	       "Undo restores post-orbit eye exactly under non-zero target_orientation" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
//  Regression for zoom-after-orbit.  Pre-fix, ZoomCamera computed
//  the dolly direction from the POST-orbit eye → look-at vector and
//  applied it to vPosition.  Recompute then re-rotated that delta
//  by target_orientation, so the post-orbit eye moved in a direction
//  that was the orbit applied TWICE — for a 90° orbit that's
//  perpendicular to "toward look-at", i.e. the camera dollies
//  sideways.
//
//  Post-fix invariant: zoom under non-zero target_orientation moves
//  the post-orbit eye toward (or away from) look-at.  The signed
//  distance |post_eye - lookAt| should DECREASE by approximately
//  dollyD when the dolly is positive (drag down → move closer).
//
static void TestZoomComposesWithOrbit()
{
	std::cout << "Testing ZoomCamera composes correctly with non-zero orbit..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	// Non-trivial orbit so the post-orbit eye is rotated relative to
	// vPosition.  At t=(0.5, 0.7) rad the orbit is well off-axis,
	// large enough to expose any direction-rotation bug.
	cam->SetTargetOrientation( Vector2( 0.5, 0.7 ) );
	cam->RegenerateData();

	const Point3 eye0   = cam->GetLocation();
	const Point3 lookAt = cam->GetStoredLookAt();

	Vector3 toLA0;
	toLA0.x = lookAt.x - eye0.x;
	toLA0.y = lookAt.y - eye0.y;
	toLA0.z = lookAt.z - eye0.z;
	const Scalar dist0 = Vector3Ops::Magnitude( toLA0 );

	SceneEdit zoom;
	zoom.op  = SceneEdit::ZoomCamera;
	// Positive dy (top-left-origin → "drag down") = move closer.
	zoom.v3a = Vector3( 0, 5, 0 );
	Check( editor.Apply( zoom ), "Apply ZoomCamera succeeds" );

	const Point3 eye1 = cam->GetLocation();
	Vector3 toLA1;
	toLA1.x = lookAt.x - eye1.x;
	toLA1.y = lookAt.y - eye1.y;
	toLA1.z = lookAt.z - eye1.z;
	const Scalar dist1 = Vector3Ops::Magnitude( toLA1 );

	// Distance decreased — zoom moved the post-orbit eye CLOSER
	// to look-at.  Pre-fix on a strong orbit, the eye would have
	// moved sideways and dist1 would be approximately dist0 (zoom
	// moved perpendicular instead of toward).
	Check( dist1 < dist0,
	       "Zoom under non-zero orbit decreases post-orbit eye→lookAt distance" );

	// Direction check: the eye moved along the OLD (post-orbit)
	// forward direction.  Compute the unit displacement and dot
	// against unit forward; should be > 0.99 (cosine of small angle).
	Vector3 disp;
	disp.x = eye1.x - eye0.x;
	disp.y = eye1.y - eye0.y;
	disp.z = eye1.z - eye0.z;
	Vector3 dispUnit = Vector3Ops::Normalize( disp );
	Vector3 fwdUnit  = Vector3Ops::Normalize( toLA0 );
	const Scalar cosAngle =
		dispUnit.x * fwdUnit.x +
		dispUnit.y * fwdUnit.y +
		dispUnit.z * fwdUnit.z;
	Check( cosAngle > 0.999,
	       "Zoom moves post-orbit eye along post-orbit forward (cos > 0.999)" );

	// Lookat must NOT move under zoom — only the rest position
	// translates along the dolly axis.
	Check( ApproxEq( cam->GetStoredLookAt(), lookAt, 1e-9 ),
	       "Zoom does not mutate vLookAt" );

	pJob->release();
}

static void TestRollMutatesOrientationZ()
{
	std::cout << "Testing RollCamera mutates orientation.z, not other fields..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	const Point3  pos0  = cam->GetLocation();
	const Point3  rest0 = cam->GetRestLocation();
	const Point3  la0   = cam->GetStoredLookAt();
	const Vector2 t0    = cam->GetTargetOrientation();
	const Vector3 o0    = cam->GetEulerOrientation();

	SceneEdit edit;
	edit.op = SceneEdit::RollCamera;
	edit.s  = 200;   // 200 px → 200 × 0.0087 ≈ 1.74 rad roll
	Check( editor.Apply( edit ), "Apply RollCamera succeeds" );

	const Point3  pos1  = cam->GetLocation();
	const Point3  rest1 = cam->GetRestLocation();
	const Point3  la1   = cam->GetStoredLookAt();
	const Vector2 t1    = cam->GetTargetOrientation();
	const Vector3 o1    = cam->GetEulerOrientation();

	Check( ApproxEq( pos0, pos1 ),   "RollCamera does NOT mutate post-orbit eye (GetLocation)" );
	Check( ApproxEq( rest0, rest1 ), "RollCamera does NOT mutate vPosition (GetRestLocation)" );
	Check( ApproxEq( la0, la1 ),     "RollCamera does NOT mutate vLookAt" );
	Check( ApproxEq( t0, t1 ),       "RollCamera does NOT mutate target_orientation" );
	Check( ApproxEq( o0.x, o1.x ),   "RollCamera leaves orientation.x (pitch) alone" );
	Check( ApproxEq( o0.y, o1.y ),   "RollCamera leaves orientation.y (yaw) alone" );
	Check( ApproxEq( o1.z - o0.z, 200 * 0.0087, 1e-6 ), "roll delta matches s × 0.0087" );

	Check( editor.Undo(), "Undo RollCamera succeeds" );
	const Vector3 o2 = cam->GetEulerOrientation();
	Check( ApproxEq( o2, o0 ), "Undo restores orientation" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
//  Roll under a non-trivial camera setup (orbit + offset position).
//  This is the closest match to a real .RISEscene-loaded camera with
//  the user mid-drag: vPosition and vLookAt are non-axis-aligned,
//  target_orientation is non-zero, and orientation already has a
//  pitch component.  The roll should ONLY add to orientation.z and
//  leave both rest and post-orbit positions untouched.
//
static void TestRollPositionInvariance()
{
	std::cout << "Testing RollCamera leaves position invariant in a non-trivial camera setup..." << std::endl;

	Job* pJob = new Job();

	ICamera* pCam = nullptr;
	if( !RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 2.5, 4 ),       // tidepool-like setup
		Point3( 0, -0.1, 0 ),
		Vector3( 0, 1, 0 ),
		Scalar( 40.0 * 3.14159265358979323846 / 180.0 ),
		800, 600,
		Scalar( 1 ),
		Scalar( 0 ),
		Scalar( 0 ),
		Scalar( 0 ),
		Vector3( 0.1, 0.0, 0.0 ),  // small pitch baseline
		Vector2( 0.2, 0.3 ) ) )    // non-zero theta+phi orbit baseline
	{
		std::cout << "  FAIL: failed to construct pinhole camera" << std::endl;
		failCount++;
		pJob->release();
		return;
	}
	if( IScenePriv* scene = pJob->GetScene() ) {
		scene->AddCamera( "default", pCam );
	}
	pCam->release();

	SceneEditor editor( *pJob->GetScene() );
	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	const Point3  pos0  = cam->GetLocation();
	const Point3  rest0 = cam->GetRestLocation();
	const Point3  la0   = cam->GetStoredLookAt();
	const Vector3 up0   = cam->GetStoredUp();

	// Roll a meaningful amount — corresponds to ~50 px of mouse drag
	// in the controller, well past the edge of "tiny" but well below
	// a full revolution.  ~26 deg.
	SceneEdit edit;
	edit.op = SceneEdit::RollCamera;
	edit.s  = 50;
	Check( editor.Apply( edit ), "Apply RollCamera (non-trivial) succeeds" );

	// Position invariants — the user's complaint was that Roll
	// "moves the camera location".  Under the post-fix wiring it
	// must NOT.  Both the rest position vPosition (panel "location"
	// field) AND the post-orbit eye GetLocation() (rasterizer eye)
	// must round-trip exactly.  vLookAt and vUp also untouched.
	Check( ApproxEq( cam->GetRestLocation(), rest0, 1e-9 ),
	       "Roll leaves vPosition unchanged under non-trivial camera" );
	Check( ApproxEq( cam->GetLocation(), pos0, 1e-9 ),
	       "Roll leaves post-orbit eye unchanged under non-trivial camera" );
	Check( ApproxEq( cam->GetStoredLookAt(), la0 ),
	       "Roll leaves vLookAt unchanged" );
	Check( ApproxEq( cam->GetStoredUp(), up0 ),
	       "Roll leaves vUp unchanged" );

	// And the orientation vector: only .z should be different from
	// the baseline; .x (pitch) and .y (whatever the parser called
	// it — math interprets as yaw) must be untouched.
	Vector3 o1 = cam->GetEulerOrientation();
	Check( ApproxEq( o1.x, 0.1 ),
	       "Roll leaves orientation.x (pitch baseline) untouched" );
	Check( ApproxEq( o1.y, 0.0 ),
	       "Roll leaves orientation.y untouched" );
	Check( ApproxEq( o1.z, 50 * 0.0087, 1e-6 ),
	       "Roll added to orientation.z (~26 deg)" );

	pJob->release();
}

static void TestPanZoomDoNotMutateAngles()
{
	std::cout << "Testing PanCamera/ZoomCamera leave target_orientation and orientation alone..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	// Pre-set non-zero angles to make sure pan/zoom don't clobber them.
	cam->SetTargetOrientation( Vector2( 0.3, 0.7 ) );
	cam->SetEulerOrientation(  Vector3( 0.1, 0.2, 0.4 ) );
	const Vector2 t0 = cam->GetTargetOrientation();
	const Vector3 o0 = cam->GetEulerOrientation();

	{
		SceneEdit pan;
		pan.op  = SceneEdit::PanCamera;
		pan.v3a = Vector3( 50, -30, 0 );
		Check( editor.Apply( pan ), "Apply PanCamera succeeds" );
	}
	Check( ApproxEq( cam->GetTargetOrientation(), t0 ), "Pan leaves target_orientation alone" );
	Check( ApproxEq( cam->GetEulerOrientation(),  o0 ), "Pan leaves orientation alone" );

	{
		SceneEdit zoom;
		zoom.op  = SceneEdit::ZoomCamera;
		zoom.v3a = Vector3( 0, 25, 0 );
		Check( editor.Apply( zoom ), "Apply ZoomCamera succeeds" );
	}
	Check( ApproxEq( cam->GetTargetOrientation(), t0 ), "Zoom leaves target_orientation alone" );
	Check( ApproxEq( cam->GetEulerOrientation(),  o0 ), "Zoom leaves orientation alone" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
//
//  Regression for "orbit eventually produces NaN camera location".
//  The user reported that holding orbit and dragging the camera
//  past the pole eventually made the panel's location field read
//  "nan nan nan".  Root cause: target_orientation.x (theta)
//  accumulated past ±π/2, where the post-orbit forward becomes
//  parallel to vUp, the basis becomes degenerate, and subsequent
//  pan/zoom edits propagated NaN through vPosition.
//
//  Post-fix, OrbitCamera clamps theta to [-kThetaLimit, kThetaLimit]
//  ≈ ±88°, keeping the basis well-conditioned.  This test fires
//  many orbit edits in one direction and asserts theta stays in
//  the band AND no field of the camera goes NaN.
//
static void TestOrbitClampedToAvoidGimbalLock()
{
	std::cout << "Testing OrbitCamera clamps theta to avoid gimbal-lock NaN..." << std::endl;

	Job* pJob = new Job();
	AttachDefaultCamera( *pJob );
	SceneEditor editor( *pJob->GetScene() );

	auto* cam = dynamic_cast<CameraCommon*>( pJob->GetScene()->GetCameraMutable() );

	// Drive orbit hard in the +theta direction — many large pixel
	// deltas that without clamping would push theta past π/2 and
	// further (each edit adds 200 × 0.0087 ≈ 1.74 rad to theta;
	// after 10 edits we'd be at ~17 rad ≈ 5.5 full revolutions).
	for( int i = 0; i < 10; ++i ) {
		SceneEdit e;
		e.op  = SceneEdit::OrbitCamera;
		e.v3a = Vector3( 0, 200, 0 );
		Check( editor.Apply( e ), "Orbit edit applies" );
	}

	const Vector2 t = cam->GetTargetOrientation();
	Check( t.x <  1.6 && t.x > 1.4,
	       "theta clamped near +89°, not unbounded" );

	// Same in the negative direction — pre-fix the negative side
	// was unclamped even by AdjustCameraForThetaPhi, so this is
	// the more important regression.
	for( int i = 0; i < 20; ++i ) {
		SceneEdit e;
		e.op  = SceneEdit::OrbitCamera;
		e.v3a = Vector3( 0, -200, 0 );
		Check( editor.Apply( e ), "Negative orbit edit applies" );
	}
	const Vector2 t2 = cam->GetTargetOrientation();
	Check( t2.x > -1.6 && t2.x < -1.4,
	       "theta clamped near -89°, not unbounded" );

	// Camera position must be finite throughout.  Note we use
	// `GetLocation` (post-orbit) — this is what most NaN propagation
	// would touch first.  vPosition (rest) must also be finite.
	Point3 pos = cam->GetLocation();
	Check( !std::isnan( pos.x ) && !std::isnan( pos.y ) && !std::isnan( pos.z ),
	       "post-orbit eye is finite (no NaN)" );
	Point3 rest = cam->GetRestLocation();
	Check( !std::isnan( rest.x ) && !std::isnan( rest.y ) && !std::isnan( rest.z ),
	       "rest position is finite (no NaN)" );

	// Now do a pan after the heavy orbit — pre-fix this could
	// propagate NaN into vPosition because the post-orbit basis
	// might have been degenerate.  Post-fix, the basis is
	// well-conditioned (theta clamped to ~89°) AND we have the
	// HasNaN guard before SetLocation.
	SceneEdit pan;
	pan.op  = SceneEdit::PanCamera;
	pan.v3a = Vector3( 30, -20, 0 );
	Check( editor.Apply( pan ), "Pan after heavy orbit succeeds" );
	rest = cam->GetRestLocation();
	Check( !std::isnan( rest.x ) && !std::isnan( rest.y ) && !std::isnan( rest.z ),
	       "Pan after heavy orbit leaves vPosition finite" );

	pJob->release();
}

static void TestRollOpClassifiedAsCameraOp()
{
	std::cout << "Testing RollCamera classifies as a camera op..." << std::endl;
	Check(  SceneEdit::IsCameraOp( SceneEdit::RollCamera ),   "RollCamera is a camera op" );
	Check( !SceneEdit::IsObjectOp( SceneEdit::RollCamera ),   "RollCamera is NOT an object op" );
	Check( !SceneEdit::IsCompositeMarker( SceneEdit::RollCamera ),
	       "RollCamera is NOT a composite marker" );
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneEditor Camera-Angle Op Test ===" << std::endl;

	TestRollOpClassifiedAsCameraOp();
	TestOrbitMutatesTargetOrientation();
	TestPanComposesWithOrbit();
	TestZoomComposesWithOrbit();
	TestRollMutatesOrientationZ();
	TestRollPositionInvariance();
	TestPanZoomDoNotMutateAngles();
	TestOrbitClampedToAvoidGimbalLock();

	std::cout << "\n=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;

	return failCount == 0 ? 0 : 1;
}
