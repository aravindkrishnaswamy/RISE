//////////////////////////////////////////////////////////////////////
//
//  SceneEditorGizmoHandlesTest.cpp — B2 unit test for the
//  SceneEditController's gizmo handle math: world-to-screen
//  projection + per-tool handle layout.
//
//  Covers:
//    1. World-to-screen projection round-trips a point on the camera's
//       forward axis to screen-center.
//    2. World +X projects to the right of center; world +Y to above
//       (image-pixel space: +Y in `Point2(x, height-y)` convention,
//       not pixel-row space).
//    3. Behind-camera points fail to project (returns false).
//    4. RefreshGizmoHandles emits NO handles when no Object is
//       selected or the tool isn't an Object-transform tool.
//    5. Translate tool emits 1 center + 3 axis planes + 3 axis arrows
//       (all axes visible when the camera is off-axis).
//    6. Rotate tool emits 1 screen ring + 3 axis rings.
//    7. Scale tool emits 1 uniform cube + 3 axis cubes.
//    8. AxisArrow handles for the Translate tool are placed
//       `kAxisArrowLengthPx` (= 80 px) from the pivot's screen-space
//       projection along the screen-direction of the world axis.
//    9. Camera on the world-Z axis (looking at origin from +Z) leaves
//       the Z-axis arrow handle missing — the world axis degenerates
//       to a single screen point and the layout helper skips it.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Utilities/FiniteMath.h"

using namespace RISE;

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

namespace {

Scalar QuietNaNForTest()
{
	const std::uint64_t bits = 0x7FF8000000000000ULL;
	Scalar value = 0;
	std::memcpy( &value, &bits, sizeof( value ) );
	return value;
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool FiniteForMatrixCompare( const double value )
{
	return RISE::IsFiniteDouble( value );
}

// Builds a Job with: an `IFilm` 256×256, a pinhole camera at
// `camLoc` looking at the origin, one Lambertian material, and one
// sphere object named "sphere" at world position (0,0,0).  Returns
// the Job; caller releases.
Job* BuildJob( double camX, double camY, double camZ,
	double objX = 0, double objY = 0, double objZ = 0 )
{
	Job* job = new Job();
	job->SetFilm( 256, 256, 1.0 );

	double loc[3]    = { camX, camY, camZ };
	double la[3]     = { 0, 0, 0 };
	double up[3]     = { 0, 1, 0 };
	double orient[3] = { 0, 0, 0 };
	double target[2] = { 0, 0 };
	job->AddPinholeCamera( "default", loc, la, up,
		0.785398, 1.0, 0, 0, orient, target, 0.0, 0.0 );

	const double red[3] = { 1.0, 0.2, 0.2 };
	job->AddUniformColorPainter( "red_paint", red, "Rec709RGB_Linear" );
	job->AddLambertianMaterial( "red", "red_paint" );

	job->AddSphereGeometry( "sphere_geom", 1.0 );
	double pos[3]      = { objX, objY, objZ };
	double objOrient[3] = { 0, 0, 0 };
	double objScale[3]  = { 1, 1, 1 };
	RadianceMapConfig rm;
	job->AddObject( "sphere", "sphere_geom", "red", 0, 0, rm,
		pos, objOrient, objScale, true, true );

	return job;
}

bool MatrixClose( const Matrix4& a, const Matrix4& b, double epsilon = 1e-8 )
{
	const Scalar* av = &a._00;
	const Scalar* bv = &b._00;
	for( int i = 0; i < 16; ++i ) {
		const double actual = static_cast<double>( av[i] );
		const double expected = static_cast<double>( bv[i] );
		const double difference = actual - expected;
		if( !FiniteForMatrixCompare( actual ) || !FiniteForMatrixCompare( expected )
		 || !FiniteForMatrixCompare( difference ) || std::fabs( difference ) > epsilon ) return false;
	}
	return true;
}

}  // namespace

//////////////////////////////////////////////////////////////////////
//
// Test 1: ProjectWorldToScreen — pivot on the camera's forward axis
// maps to (W/2, H/2).
//
//////////////////////////////////////////////////////////////////////

static void TestProjectionPivotAtScreenCenter()
{
	std::cout << "Test 1: pivot on forward axis projects to screen center" << std::endl;

	Job* job = BuildJob( 0, 0, 10 );  // looking at origin down -Z
	SceneEditController c( *job, 0 );

	double sx = 0, sy = 0;
	const bool ok = c.ForTest_ProjectWorldToScreen( 0, 0, 0, sx, sy );
	Check( ok, "projection succeeds for in-front point at origin" );
	// 256x256 film, screen center = (128, 128).  Allow a small
	// tolerance for the matrix-inverse round-trip.
	Check( std::fabs( sx - 128.0 ) < 1e-6, "sx == 128 at screen center" );
	Check( std::fabs( sy - 128.0 ) < 1e-6, "sy == 128 at screen center" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 2: World +X projects to the right of center; world +Y
// projects ABOVE center in WIDGET-Y-DOWN pointer coordinates (the
// space `ViewportCanvas` delivers pointer events in — `isFlipped =
// true`, origin at top-left).  "Above center" in widget-Y-DOWN means
// `sy < H/2`.  The controller's projection helper flips Y around the
// image height internally so gizmo math, pointer events, and overlay
// drawing all live in the same coordinate space.
//
//////////////////////////////////////////////////////////////////////

static void TestProjectionAxisDirections()
{
	std::cout << "Test 2: world +X/+Y project to right/up of screen center" << std::endl;

	// Off-axis camera so all three world axes have well-defined
	// screen-space directions (camera ON-axis collapses one of them).
	Job* job = BuildJob( 0, 0, 10 );
	SceneEditController c( *job, 0 );

	double cx = 0, cy = 0, ax = 0, ay = 0, by = 0, bx = 0;
	c.ForTest_ProjectWorldToScreen( 0, 0, 0, cx, cy );
	const bool okX = c.ForTest_ProjectWorldToScreen( 1, 0, 0, ax, ay );
	const bool okY = c.ForTest_ProjectWorldToScreen( 0, 1, 0, bx, by );

	Check( okX, "projection of (+X) succeeds" );
	Check( okY, "projection of (+Y) succeeds" );
	Check( ax > cx, "world +X projects to sx > center.sx (right in widget space)" );
	Check( by < cy, "world +Y projects to sy < center.sy (+Y = up in widget-Y-DOWN)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 3: Behind-camera points fail to project.
//
//////////////////////////////////////////////////////////////////////

static void TestProjectionRejectsBehindCamera()
{
	std::cout << "Test 3: behind-camera point rejected" << std::endl;

	Job* job = BuildJob( 0, 0, 10 );  // looking at origin from +Z; -Z is forward
	SceneEditController c( *job, 0 );

	double sx = 0, sy = 0;
	// (0, 0, 15) is BEHIND the camera (camera at z=10 looking toward
	// origin at z=0).
	const bool ok = c.ForTest_ProjectWorldToScreen( 0, 0, 15, sx, sy );
	Check( !ok, "behind-camera point returns false" );

	// Point exactly AT the eye is degenerate.
	const bool okEye = c.ForTest_ProjectWorldToScreen( 0, 0, 10, sx, sy );
	Check( !okEye, "point at the eye position returns false" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 4: RefreshGizmoHandles emits NO handles when:
//   (a) no Object is selected, OR
//   (b) the active tool is not an Object-transform tool.
//
//////////////////////////////////////////////////////////////////////

static void TestNoHandlesWhenInapplicable()
{
	std::cout << "Test 4: gizmo refresh is a no-op when not applicable" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );

	// (a) no Object selected.
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();
	Check( c.GizmoHandleCount() == 0,
	       "no handles when no Object selected" );

	// (b) Object selected, but tool is Select (not Object-transform).
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::Select );
	c.RefreshGizmoHandles();
	Check( c.GizmoHandleCount() == 0,
	       "no handles when Select tool active" );

	// (c) Object + Object-transform tool: handles emitted.
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();
	Check( c.GizmoHandleCount() > 0,
	       "handles emitted with Object + TranslateObject" );

	// (d) Remembering the object selection must not draw its gizmo after the
	// primary inspector selection switches to another category.
	c.SetSelection( SceneEditController::Category::Material, String( "red" ) );
	c.RefreshGizmoHandles();
	Check( c.GizmoHandleCount() == 0,
	       "remembered Object does not leave an inert gizmo for another category" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 5: Translate tool — center + 3 axis planes + 3 axis arrows.
// Pivot at origin, camera off-axis at (5, 5, 10).
//
//////////////////////////////////////////////////////////////////////

static void TestTranslateHandleSet()
{
	std::cout << "Test 5: Translate tool emits center + 3 planes + 3 arrows" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );  // off-axis so all 3 world axes are visible
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;

	int nCenter = 0, nPlane = 0, nArrow = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		const int k = c.GizmoHandleKind( i );
		if( k == static_cast<int>( K::ScreenCenter ) ) ++nCenter;
		else if( k == static_cast<int>( K::AxisPlane ) ) ++nPlane;
		else if( k == static_cast<int>( K::AxisArrow ) ) ++nArrow;
	}
	Check( nCenter == 1, "1 ScreenCenter handle" );
	Check( nPlane  == 3, "3 AxisPlane handles" );
	Check( nArrow  == 3, "3 AxisArrow handles" );
	Check( c.GizmoHandleCount() == 7, "7 total handles" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 6: Rotate tool — screen ring + 3 axis rings.
//
//////////////////////////////////////////////////////////////////////

static void TestRotateHandleSet()
{
	std::cout << "Test 6: Rotate tool emits screen ring + 3 axis rings" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::RotateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;

	int nScreenRing = 0, nAxisRing = 0;
	double axisRadius[3] = { 0, 0, 0 };
	double screenRadius = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		const int k = c.GizmoHandleKind( i );
		if( k == static_cast<int>( K::ScreenRing ) ) {
			++nScreenRing;
			screenRadius = c.GizmoHandleScreenRadius( i );
		} else if( k == static_cast<int>( K::AxisRing ) ) {
			++nAxisRing;
			const int axis = c.GizmoHandleAxis( i );
			if( axis >= 0 && axis < 3 ) axisRadius[axis] = c.GizmoHandleScreenRadius( i );
		}
	}
	Check( nScreenRing == 1, "1 ScreenRing handle" );
	Check( nAxisRing   == 3, "3 AxisRing handles" );
	Check( c.GizmoHandleCount() == 4, "4 total handles" );
	Check( axisRadius[0] > 0 && axisRadius[0] < axisRadius[1]
	    && axisRadius[1] < axisRadius[2] && axisRadius[2] < screenRadius,
	       "X/Y/Z/view rotation rings have distinct selectable radii" );

	double pivotX = 0, pivotY = 0;
	c.ForTest_ProjectWorldToScreen( 0, 0, 0, pivotX, pivotY );
	for( int axis = 0; axis < 3; ++axis ) {
		const int hit = c.GizmoHandleAt( Point2( pivotX + axisRadius[axis], pivotY ) );
		const bool exactAxis = hit >= 0
			&& c.GizmoHandleKind( static_cast<unsigned int>( hit ) ) == static_cast<int>( K::AxisRing )
			&& c.GizmoHandleAxis( static_cast<unsigned int>( hit ) ) == axis;
		Check( exactAxis,
			axis == 0 ? "X ring is independently hittable"
		  : axis == 1 ? "Y ring is independently hittable"
		              : "Z ring is independently hittable" );
	}

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 7: Scale tool — uniform cube + 3 axis cubes.
//
//////////////////////////////////////////////////////////////////////

static void TestScaleHandleSet()
{
	std::cout << "Test 7: Scale tool emits uniform cube + 3 axis cubes" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::ScaleObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;

	int nUniform = 0, nAxisScale = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		const int k = c.GizmoHandleKind( i );
		if( k == static_cast<int>( K::UniformScaleCube ) ) ++nUniform;
		else if( k == static_cast<int>( K::AxisScaleHandle ) ) ++nAxisScale;
	}
	Check( nUniform   == 1, "1 UniformScaleCube handle" );
	Check( nAxisScale == 3, "3 AxisScaleHandle handles" );
	Check( c.GizmoHandleCount() == 4, "4 total handles" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 8: AxisArrow handles for Translate sit `kAxisArrowLengthPx`
// (= 80 px in code) from the pivot's screen projection.  Verifies
// the screen-space radial layout: tip-distance = 80 px regardless
// of the world-space axis direction at the pivot.
//
//////////////////////////////////////////////////////////////////////

static void TestTranslateAxisArrowDistance()
{
	std::cout << "Test 8: AxisArrow handles sit ~80 px from pivot" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	double pivotX = 0, pivotY = 0;
	const bool pivotOk = c.ForTest_ProjectWorldToScreen( 0, 0, 0, pivotX, pivotY );
	Check( pivotOk, "pivot projects" );

	using K = SceneEditController::GizmoHandle::Kind;
	int arrowsChecked = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) != static_cast<int>( K::AxisArrow ) ) continue;
		const double dx = c.GizmoHandleScreenX( i ) - pivotX;
		const double dy = c.GizmoHandleScreenY( i ) - pivotY;
		const double mag = std::sqrt( dx*dx + dy*dy );
		// Layout helper places tip at exactly kAxisArrowLengthPx (= 80).
		Check( std::fabs( mag - 80.0 ) < 1e-6,
		       "axis arrow at exactly 80 px from pivot" );
		++arrowsChecked;
	}
	Check( arrowsChecked == 3, "all 3 axis arrows checked" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 9: Camera on the world-Z axis collapses world-Z to a single
// screen point.  The layout helper detects this (zero-length screen
// direction) and omits the Z arrow.  Test passes if the Translate
// handle set has only 2 AxisArrows (X and Y) when the camera looks
// straight down -Z at the origin.
//
//////////////////////////////////////////////////////////////////////

static void TestColinearAxisOmitted()
{
	std::cout << "Test 9: world-axis colinear with camera-forward is omitted" << std::endl;

	Job* job = BuildJob( 0, 0, 10 );  // camera on +Z, looking at origin
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int nArrow = 0;
	int axesSeen = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) != static_cast<int>( K::AxisArrow ) ) continue;
		++nArrow;
		axesSeen |= ( 1 << c.GizmoHandleAxis( i ) );
	}
	// X and Y axes visible (bits 0,1 set); Z axis (bit 2) omitted.
	Check( nArrow == 2, "only 2 axis arrows when camera on +Z" );
	Check( ( axesSeen & 0x3 ) == 0x3, "X and Y arrows present" );
	Check( ( axesSeen & 0x4 ) == 0,   "Z arrow omitted" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// Test 10: Pivot tracking — moving the object via ITransformable
// surface and refreshing handles must shift the center handle.
//
//////////////////////////////////////////////////////////////////////

static void TestPivotTracksObjectTranslation()
{
	std::cout << "Test 10: gizmo center tracks object translation" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	// First refresh: pivot at origin.
	using K = SceneEditController::GizmoHandle::Kind;
	double cx0 = -1, cy0 = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::ScreenCenter ) ) {
			cx0 = c.GizmoHandleScreenX( i );
			cy0 = c.GizmoHandleScreenY( i );
		}
	}
	Check( cx0 >= 0 && cy0 >= 0, "first center handle found" );

	// Move the object — uses the edit pipeline.  After Apply, the
	// object's FinalTransformMatrix's translation column shifts to
	// (1, 0, 0).  SetObjectPosition carries the new ABSOLUTE world
	// position in `v3a`; Apply captures the prior transform into
	// `prevTransform` for undo (we don't exercise undo here).
	SceneEdit move;
	move.op = SceneEdit::SetObjectPosition;
	move.objectName = String( "sphere" );
	move.v3a = Vector3( 1, 0, 0 );
	c.Editor().Apply( move );

	// Confirm pivot moved (via the test hook).
	double wx = 0, wy = 0, wz = 0;
	const bool pok = c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( pok, "pivot accessor succeeds" );
	Check( std::fabs( wx - 1.0 ) < 1e-9, "object pivot world.x == 1" );

	// Re-refresh: center handle should now be elsewhere.
	c.RefreshGizmoHandles();
	double cx1 = -1, cy1 = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::ScreenCenter ) ) {
			cx1 = c.GizmoHandleScreenX( i );
			cy1 = c.GizmoHandleScreenY( i );
		}
	}
	Check( cx1 >= 0 && cy1 >= 0, "second center handle found" );
	Check( std::fabs( cx1 - cx0 ) > 1.0 || std::fabs( cy1 - cy0 ) > 1.0,
	       "center handle moved after object translation" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
//
// B3 tests — pointer hit-test + drag dispatch.
//
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// Test 11: GizmoHandleAt returns the index of a handle whose disc
// the pointer falls within, or -1 on miss.
//////////////////////////////////////////////////////////////////////

static void TestHitTestPicksHandle()
{
	std::cout << "Test 11: hit-test picks handle under cursor" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	// Click directly on each Translate-tool handle and confirm
	// GizmoHandleAt returns a sane index for that click.  The pointer
	// position is taken from the cached handle screen coords — i.e.
	// the test passes if hit-test agrees with handle layout.
	int hits = 0;
	int misses = 0;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		const Point2 px( c.GizmoHandleScreenX( i ), c.GizmoHandleScreenY( i ) );
		const int hit = c.GizmoHandleAt( px );
		if( hit >= 0 ) ++hits;
		else ++misses;
	}
	Check( misses == 0, "every handle is hit when clicked at its centre" );
	Check( hits == static_cast<int>( c.GizmoHandleCount() ),
	       "every handle's centre falls within its own hit radius" );

	// Click WAY outside any handle — should miss.
	const Point2 farMiss( 5000.0, 5000.0 );
	Check( c.GizmoHandleAt( farMiss ) == -1, "pointer far from gizmo misses" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 12: OnPointerDown captures the drag state when the pointer
// lands on a handle, and clears it when the pointer lands off any
// handle.
//////////////////////////////////////////////////////////////////////

static void TestPointerDownArmsGizmoDrag()
{
	std::cout << "Test 12: OnPointerDown arms gizmo drag on handle hit" << std::endl;

	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	// Find any AxisArrow handle and click it.
	int targetIdx = -1;
	using K = SceneEditController::GizmoHandle::Kind;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisArrow ) ) {
			targetIdx = static_cast<int>( i );
			break;
		}
	}
	Check( targetIdx >= 0, "an AxisArrow handle was located" );

	const Point2 px( c.GizmoHandleScreenX( targetIdx ),
	                 c.GizmoHandleScreenY( targetIdx ) );
	c.OnPointerDown( px );
	Check( c.IsGizmoDragActive(), "drag state armed after pointer-down on handle" );
	Check( c.ActiveGizmoKind() == static_cast<int>( K::AxisArrow ),
	       "active kind is AxisArrow" );
	Check( c.ActiveGizmoAxis() == c.GizmoHandleAxis( targetIdx ),
	       "active axis matches the picked handle's axis" );

	c.OnPointerUp( px );
	Check( !c.IsGizmoDragActive(), "drag state cleared after pointer-up" );
	Check( c.ActiveGizmoKind() == -1, "active kind reads -1 after pointer-up" );

	// Pointer-down OFF any handle: drag stays disarmed.
	c.OnPointerDown( Point2( 5000.0, 5000.0 ) );
	Check( !c.IsGizmoDragActive(),
	       "drag stays disarmed when pointer-down misses every handle" );
	c.OnPointerUp( Point2( 5000.0, 5000.0 ) );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 13: AxisArrow drag dispatches a TranslateObject edit ALONG
// the captured world axis only — the constrained drag must leave
// the other two axes untouched, and a "drag to pivot" motion (i.e.
// from the +1-unit arrow tip back to the pivot's screen position)
// must produce a -1 world-unit shift along that axis.
//
// The exact pixel-to-world ratio depends on camera FOV / distance,
// so the test asserts direction + constraint + the special "drag to
// pivot" case where the math reduces to closed form rather than a
// magic numeric magnitude.
//////////////////////////////////////////////////////////////////////

static void TestAxisArrowDragTranslatesAlongAxis()
{
	std::cout << "Test 13: AxisArrow drag translates along the matching world axis" << std::endl;

	// Camera at (0, 0, 10) looking at origin: world +X projects to
	// screen +X, world +Y projects to screen +Y.  World +Z degenerates
	// (colinear with view), so only the X / Y arrows are emitted (see
	// Test 9), and the off-axis test for the Y arrow is meaningful.
	Job* job = BuildJob( 0, 0, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int xArrowIdx = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisArrow )
		 && c.GizmoHandleAxis( i ) == 0 )
		{
			xArrowIdx = static_cast<int>( i );
			break;
		}
	}
	Check( xArrowIdx >= 0, "X-axis arrow located" );

	// Pivot screen position (captured BEFORE the drag) and the X
	// arrow's screen position.  The displacement (arrow - pivot)
	// equals `kAxisArrowLengthPx` (80 px) along the screen-space
	// direction of world +X.  Dragging from `arrow → pivot` is a
	// pointer delta of `-(arrow - pivot)`, which projects onto the
	// axis direction with magnitude `kAxisArrowLengthPx`.  Combined
	// with the axisDir-velocity normalisation in OnPointerMove this
	// is EXACTLY the inverse motion: -1 world unit along +X (because
	// the handle sat 1 world unit out from the pivot at probe-time —
	// the captured axisDir has magnitude `pixels-per-world-unit`,
	// and the handle is placed at `(axisDir/|axisDir|) * 80`, so
	// dragging by `-(axisDir/|axisDir|) * 80` gives wa = -80/|axisDir|
	// = -kAxisArrowLengthPx / |axisDir|; the closed form depends on
	// the per-camera projection density and only equals -1 by
	// coincidence — assert direction + axis-constraint instead).
	double pivotX0 = 0, pivotY0 = 0;
	c.ForTest_ProjectWorldToScreen( 0, 0, 0, pivotX0, pivotY0 );
	const double arrowX = c.GizmoHandleScreenX( xArrowIdx );
	const double arrowY = c.GizmoHandleScreenY( xArrowIdx );
	Check( arrowX > pivotX0, "X arrow is right of pivot in screen space" );

	const Point2 down( arrowX, arrowY );
	c.OnPointerDown( down );
	Check( c.IsGizmoDragActive(), "X-arrow drag armed" );
	Check( c.ActiveGizmoAxis() == 0, "X-arrow drag axis is 0" );

	// Drag from the X arrow's tip BACK to the pivot's screen position.
	// Whatever the per-camera pixel-to-world density, the constrained
	// math projects this onto world +X with NEGATIVE wa.
	const Point2 move( pivotX0, pivotY0 );
	c.OnPointerMove( move );
	c.OnPointerUp( move );

	double wx = 0, wy = 0, wz = 0;
	const bool ok = c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( ok, "pivot readable after drag" );
	Check( wx < -1e-3, "object moved along -X (drag retracted the arrow)" );
	Check( std::fabs( wy ) < 1e-3, "object Y unchanged (axis-constrained)" );
	Check( std::fabs( wz ) < 1e-3, "object Z unchanged (axis-constrained)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 14: AxisPlane drag — the YZ plane handle (axis=0) on an
// off-axis camera moves the object in BOTH Y and Z but leaves X
// untouched.
//////////////////////////////////////////////////////////////////////

static void TestAxisPlaneDragStaysInPlane()
{
	std::cout << "Test 14: AxisPlane drag respects the plane constraint" << std::endl;

	// Off-axis camera so all three world axes have non-degenerate
	// screen projections — the YZ plane handle requires both Y and Z
	// to project sensibly.
	Job* job = BuildJob( 5, 5, 10 );
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int planeIdx = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisPlane )
		 && c.GizmoHandleAxis( i ) == 0 )
		{
			planeIdx = static_cast<int>( i );
			break;
		}
	}
	Check( planeIdx >= 0, "YZ plane handle located" );

	const Point2 down( c.GizmoHandleScreenX( planeIdx ),
	                   c.GizmoHandleScreenY( planeIdx ) );
	c.OnPointerDown( down );
	Check( c.IsGizmoDragActive(), "YZ-plane drag armed" );
	Check( c.ActiveGizmoKind() == static_cast<int>( K::AxisPlane ),
	       "active kind is AxisPlane" );

	c.OnPointerMove( Point2( down.x + 30.0, down.y - 20.0 ) );
	c.OnPointerUp( Point2( down.x + 30.0, down.y - 20.0 ) );

	double wx = 0, wy = 0, wz = 0;
	c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	// X must be untouched (plane axis is 0 = X, so YZ plane drag
	// solves for Δy and Δz only).
	Check( std::fabs( wx ) < 1e-6, "world X unchanged on YZ-plane drag" );
	// Some movement must have happened in Y or Z (the off-axis camera
	// guarantees both directions are visible).
	Check( std::fabs( wy ) > 1e-3 || std::fabs( wz ) > 1e-3,
	       "object moved in Y/Z plane" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 15: ScreenCenter drag emits a TranslateObject edit using the
// minimum-norm 3-DoF solve.  Pivot under camera-on-axis collapses Z
// so the result moves only in X and Y.
//////////////////////////////////////////////////////////////////////

static void TestScreenCenterDragTranslates()
{
	std::cout << "Test 15: ScreenCenter drag translates the object" << std::endl;

	Job* job = BuildJob( 0, 0, 10 );  // on-axis camera, Z degenerate
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::TranslateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int centerIdx = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::ScreenCenter ) ) {
			centerIdx = static_cast<int>( i );
			break;
		}
	}
	Check( centerIdx >= 0, "ScreenCenter handle located" );

	const Point2 down( c.GizmoHandleScreenX( centerIdx ),
	                   c.GizmoHandleScreenY( centerIdx ) );
	c.OnPointerDown( down );
	Check( c.IsGizmoDragActive(), "ScreenCenter drag armed" );

	// 40 px right + 30 px DOWN in widget-Y-DOWN pointer space.
	// Object should move +X (right) and -Y (world-Y down, since
	// dragging the gizmo center down should pull the object down
	// in world).  Z stays zero because the on-axis camera makes
	// world-Z degenerate.
	c.OnPointerMove( Point2( down.x + 40.0, down.y + 30.0 ) );
	c.OnPointerUp( Point2( down.x + 40.0, down.y + 30.0 ) );

	double wx = 0, wy = 0, wz = 0;
	c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( wx > 0,   "object moved +X (drag right)" );
	Check( wy < 0,   "object moved -Y (drag down in widget-Y-DOWN)" );
	Check( std::fabs( wz ) < 1e-6, "object Z unchanged (degenerate axis)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 16: AxisRing drag rotates around the world axis.  A drag
// across the projected ring perimeter from "+x screen" to "+y screen"
// is +π/2 of pointer-angle around the projected pivot, so the X ring
// should emit a rotation around world +X by +π/2 ± wrap. The
// object is deliberately translated away from the world origin: rotating
// an object must change its basis while leaving its own pivot fixed.
//////////////////////////////////////////////////////////////////////

static void TestAxisRingDragRotates()
{
	std::cout << "Test 16: AxisRing drag rotates around world axis" << std::endl;

	const double objectX = 2.0, objectY = 1.0, objectZ = 0.0;
	Job* job = BuildJob( 5, 5, 10, objectX, objectY, objectZ );
	IObjectPriv* sphere = job->GetObjects() ? job->GetObjects()->GetItem( "sphere" ) : 0;
	Check( sphere != 0, "rotation fixture object resolves" );
	if( sphere ) {
		sphere->SetOrientation( Vector3( 0.2, -0.3, 0.4 ) );
		sphere->SetStretch( Vector3( 1.5, 0.75, 2.0 ) );
		sphere->FinalizeTransformations();
	}
	const Matrix4 before = sphere ? sphere->GetFinalTransformMatrix() : Matrix4Ops::Identity();
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::RotateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int ringIdx = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisRing )
		 && c.GizmoHandleAxis( i ) == 0 )
		{
			ringIdx = static_cast<int>( i );
			break;
		}
	}
	Check( ringIdx >= 0, "X-axis ring located" );

	// Click on the +X side of the ring (projected pivot + (radius, 0)),
	// then move to the +Y side (projected pivot + (0, radius)).
	double pivotX = 0, pivotY = 0;
	c.ForTest_ProjectWorldToScreen( objectX, objectY, objectZ, pivotX, pivotY );
	const double r = c.GizmoHandleScreenRadius( ringIdx );
	const Point2 down( pivotX + r, pivotY );
	c.OnPointerDown( down );
	Check( c.IsGizmoDragActive(), "ring drag armed" );
	Check( c.ActiveGizmoAxis() == 0, "X ring drag routes to world X" );

	const Point2 move( pivotX, pivotY + r );
	c.OnPointerMove( move );
	c.OnPointerUp( move );

	// The dispatched RotateObjectArb edits should have rotated the
	// object's transform. Concrete state read-back via the
	// FinalTransformMatrix: the upper-left 3x3 must be non-identity while
	// the translated pivot remains exactly where it started. The old bug
	// pre-multiplied a bare world rotation and orbited this pivot around
	// (0,0,0), which an origin-centred fixture could never detect.
	double wx = 0, wy = 0, wz = 0;
	c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( std::fabs( wx - objectX ) < 1e-9
	    && std::fabs( wy - objectY ) < 1e-9
	    && std::fabs( wz - objectZ ) < 1e-9,
	       "translated pivot remains fixed (rotation is about the object's origin)" );
	Check( sphere != 0, "rotated object still resolves" );
	Matrix4 rotated = Matrix4Ops::Identity();
	if( sphere ) {
		const Matrix4 m = sphere->GetFinalTransformMatrix();
		const Vector3 pivot( before._30, before._31, before._32 );
		const Matrix4 expected = Matrix4Ops::Translation( pivot )
			* Matrix4Ops::XRotation( PI / 2.0 )
			* Matrix4Ops::Translation( Vector3( -pivot.x, -pivot.y, -pivot.z ) )
			* before;
		Check( MatrixClose( m, expected ),
		       "world-X ring pre-multiplies the complete non-identity transform" );
		Check( std::fabs( static_cast<double>( m._12 ) ) > 0.5
		    || std::fabs( static_cast<double>( m._21 ) ) > 0.5,
		       "rotation changes the object's basis (not a false-green fixed-pivot no-op)" );
		rotated = m;
	}

	Check( c.Editor().Undo(), "rotation composite undoes" );
	Check( sphere && MatrixClose( sphere->GetFinalTransformMatrix(), before ),
	       "rotation undo restores the exact complete transform" );
	Check( c.Editor().Redo(), "rotation composite redoes" );
	Check( sphere && MatrixClose( sphere->GetFinalTransformMatrix(), rotated ),
	       "rotation redo restores the exact rotated transform" );

	// The rotation lives in an outer pivot-conjugated stack entry. A later
	// absolute position edit must still land exactly at the requested world
	// point instead of being rotated around the old pivot.
	SceneEdit setPos;
	setPos.op = SceneEdit::SetObjectPosition;
	setPos.objectName = String( "sphere" );
	setPos.v3a = Vector3( -3, 4, 2 );
	Check( c.Editor().Apply( setPos ), "absolute position applies after rotation" );
	c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( std::fabs( wx + 3.0 ) < 1e-9
	    && std::fabs( wy - 4.0 ) < 1e-9
	    && std::fabs( wz - 2.0 ) < 1e-9,
	       "absolute position after rotation lands at the requested world point" );
	Matrix4 repositioned = Matrix4Ops::Identity();
	if( sphere ) {
		repositioned = sphere->GetFinalTransformMatrix();
		bool basisPreserved = true;
		const Scalar* rv = &rotated._00;
		const Scalar* mv = &repositioned._00;
		for( int column = 0; column < 3; ++column ) {
			for( int row = 0; row < 3; ++row ) {
				const int index = column * 4 + row;
				basisPreserved = basisPreserved
					&& std::fabs( static_cast<double>( rv[index] - mv[index] ) ) < 1e-9;
			}
		}
		Check( basisPreserved, "absolute position preserves rotated/stretched basis" );
	}
	Check( c.Editor().Undo(), "absolute position undo succeeds" );
	Check( sphere && MatrixClose( sphere->GetFinalTransformMatrix(), rotated ),
	       "absolute position undo restores the rotated transform" );
	Check( c.Editor().Redo(), "absolute position redo succeeds" );
	Check( sphere && MatrixClose( sphere->GetFinalTransformMatrix(), repositioned ),
	       "absolute position redo restores the exact repositioned transform" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 17 (F6): after a scale-gizmo (ScaleObjectFromAnchor) drag is
// UNDONE, a subsequent ABSOLUTE SetObjectPosition must land at the
// requested value -- not compose with the drag-start baseline that the
// old SFA undo collapsed onto the transform stack.
//////////////////////////////////////////////////////////////////////
static void TestScaleGizmoUndoThenAbsoluteSet()
{
	std::cout << "Test 17 (F6): scale-gizmo undo + absolute set composes correctly" << std::endl;
	Job* job = BuildJob( 5, 5, 10 );   // off-axis so the uniform cube projects
	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );

	// Move to a NON-identity baseline (x=8) so the composition bug is visible.
	{
		SceneEdit mv; mv.op = SceneEdit::SetObjectPosition; mv.objectName = String( "sphere" ); mv.v3a = Vector3( 8, 0, 0 );
		c.Editor().Apply( mv );
	}

	// Scale-gizmo drag on the uniform cube (a ScaleObjectFromAnchor gesture).
	c.SetTool( SceneEditController::Tool::ScaleObject );
	c.RefreshGizmoHandles();
	using K = SceneEditController::GizmoHandle::Kind;
	int cubeIdx = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i )
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::UniformScaleCube ) ) { cubeIdx = static_cast<int>( i ); break; }
	Check( cubeIdx >= 0, "[f6] uniform scale cube located" );
	const Point2 down( c.GizmoHandleScreenX( cubeIdx ), c.GizmoHandleScreenY( cubeIdx ) );
	c.OnPointerDown( down );
	Check( c.IsGizmoDragActive(), "[f6] scale drag armed" );
	c.OnPointerMove( Point2( down.x + 40.0, down.y ) );   // scale up
	c.OnPointerUp( Point2( down.x + 40.0, down.y ) );

	// Undo the scale gesture (composite), then set an ABSOLUTE position.
	Check( c.Editor().Undo(), "[f6] undo scale gesture" );
	{
		SceneEdit mv; mv.op = SceneEdit::SetObjectPosition; mv.objectName = String( "sphere" ); mv.v3a = Vector3( -12, 0, 0 );
		c.Editor().Apply( mv );
	}
	double wx = 0, wy = 0, wz = 0;
	c.ForTest_GetSelectionPivotWorld( wx, wy, wz );
	Check( std::fabs( wx - ( -12.0 ) ) < 1e-6,
	       "[f6] absolute SetPosition after scale-gizmo undo lands at -12, not composed" );
	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 18: a free scale drag (pointer misses every handle) composes a
// cumulative factor with the drag-start transform instead of resetting a
// pre-scaled object to a near-one absolute stretch on every move.
//////////////////////////////////////////////////////////////////////
static void TestFreeScaleDragAccumulatesFromBaseline()
{
	std::cout << "Test 18: free scale drag preserves and accumulates baseline scale" << std::endl;
	Job* job = BuildJob( 5, 5, 10 );
	IObjectPriv* sphere = job->GetObjects() ? job->GetObjects()->GetItem( "sphere" ) : 0;
	Check( sphere != 0, "[free-scale] object resolves" );
	if( sphere ) {
		sphere->SetStretch( Vector3( 10, 5, 2 ) );
		sphere->FinalizeTransformations();
	}
	const Matrix4 baseline = sphere ? sphere->GetFinalTransformMatrix() : Matrix4Ops::Identity();

	SceneEditController c( *job, 0 );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::ScaleObject );
	c.RefreshGizmoHandles();
	double pivotX = 0, pivotY = 0;
	c.ForTest_ProjectWorldToScreen( 0, 0, 0, pivotX, pivotY );
	const Point2 down( pivotX + 35.0, pivotY + 35.0 );
	Check( c.GizmoHandleAt( down ) == -1, "[free-scale] pointer misses all handles" );
	c.OnPointerDown( down );
	Check( !c.IsGizmoDragActive(), "[free-scale] uses the unconstrained path" );
	c.OnPointerMove( Point2( down.x, down.y + 40.0 ) );
	c.OnPointerMove( Point2( down.x, down.y + 80.0 ) );
	c.OnPointerUp( Point2( down.x, down.y + 80.0 ) );

	if( sphere ) {
		const Matrix4 m = sphere->GetFinalTransformMatrix();
		const double lx = std::sqrt( static_cast<double>( m._00*m._00 + m._01*m._01 + m._02*m._02 ) );
		const double ly = std::sqrt( static_cast<double>( m._10*m._10 + m._11*m._11 + m._12*m._12 ) );
		const double lz = std::sqrt( static_cast<double>( m._20*m._20 + m._21*m._21 + m._22*m._22 ) );
		Check( std::fabs( lx - 20.0 ) < 1e-8
		    && std::fabs( ly - 10.0 ) < 1e-8
		    && std::fabs( lz - 4.0 ) < 1e-8,
		       "[free-scale] 80px cumulative drag doubles the 10/5/2 baseline" );

		// The completed scale gesture must leave an authoritative final
		// transform. A later public/keyframed absolute setter replaces world
		// position instead of composing beneath the scaled anchor.
		sphere->SetPosition( Point3( 3, -4, 5 ) );
		sphere->FinalizeTransformations();
		const Matrix4 repositioned = sphere->GetFinalTransformMatrix();
		Check( std::fabs( repositioned._30 - 3.0 ) < 1e-9
		    && std::fabs( repositioned._31 + 4.0 ) < 1e-9
		    && std::fabs( repositioned._32 - 5.0 ) < 1e-9,
		       "[free-scale] public absolute position replaces after completed drag" );
	}
	Check( c.Editor().Undo(), "[free-scale] gesture undoes" );
	Check( sphere && MatrixClose( sphere->GetFinalTransformMatrix(), baseline ),
	       "[free-scale] undo restores the exact baseline transform" );
	job->release();
}

//////////////////////////////////////////////////////////////////////
// Test 19: handle geometry is specified in displayed surface pixels,
// not authored film pixels. A 4x larger film shown in the same pane must
// keep the same visible ring radius and clickable tolerance.
//////////////////////////////////////////////////////////////////////
static void TestGizmoSizeTracksViewportSurface()
{
	std::cout << "Test 19: gizmo size and hit tolerance track viewport surface" << std::endl;
	Job* job = BuildJob( 5, 5, 10 );
	job->SetFilm( 1024, 1024, 1.0 );
	SceneEditController c( *job, 0 );
	Check( c.SetPaneSurfaceDims( 0, 256, 256 ),
	       "[surface-scale] primary pane surface dimensions accepted" );
	c.SetSelection( SceneEditController::Category::Object, String( "sphere" ) );
	c.SetTool( SceneEditController::Tool::RotateObject );
	c.RefreshGizmoHandles();

	using K = SceneEditController::GizmoHandle::Kind;
	int xRing = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisRing )
		 && c.GizmoHandleAxis( i ) == 0 ) {
			xRing = static_cast<int>( i );
			break;
		}
	}
	Check( xRing >= 0, "[surface-scale] X ring resolves" );
	if( xRing >= 0 ) {
		const double filmRadius = c.GizmoHandleScreenRadius(
			static_cast<unsigned int>( xRing ) );
		const double visibleSurfaceRadius = filmRadius * 256.0 / 1024.0;
		Check( std::fabs( visibleSurfaceRadius - 58.0 ) < 1e-9,
		       "[surface-scale] 4K-style film keeps the 58px displayed X-ring radius" );

		double pivotX = 0, pivotY = 0;
		c.ForTest_ProjectWorldToScreen( 0, 0, 0, pivotX, pivotY );
		// Nine surface pixels outside the circumference is inside the
		// advertised 10px band. In film coordinates this is 9 * 4 = 36.
		const int hit = c.GizmoHandleAt(
			Point2( pivotX + filmRadius + 36.0, pivotY ) );
		Check( hit == xRing,
		       "[surface-scale] ring retains its 10px displayed hit tolerance" );
	}

	// Scale sensitivity uses the same display-space conversion: moving 80
	// displayed pixels (320 film pixels at 4x) must double, not scale by 16x.
	c.SetTool( SceneEditController::Tool::ScaleObject );
	c.RefreshGizmoHandles();
	int uniformCube = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::UniformScaleCube ) ) {
			uniformCube = static_cast<int>( i );
			break;
		}
	}
	Check( uniformCube >= 0, "[surface-scale] uniform scale handle resolves" );
	if( uniformCube >= 0 ) {
		const Point2 down( c.GizmoHandleScreenX( uniformCube ),
		                   c.GizmoHandleScreenY( uniformCube ) );
		c.OnPointerDown( down );
		c.OnPointerMove( Point2( down.x + 320.0, down.y ) );
		c.OnPointerUp( Point2( down.x + 320.0, down.y ) );
		IObjectPriv* sphere = job->GetObjects() ? job->GetObjects()->GetItem( "sphere" ) : 0;
		const Matrix4 scaled = sphere ? sphere->GetFinalTransformMatrix() : Matrix4Ops::Identity();
		const double lx = std::sqrt( static_cast<double>(
			scaled._00 * scaled._00 + scaled._01 * scaled._01 + scaled._02 * scaled._02 ) );
		Check( std::fabs( lx - 2.0 ) < 1e-8,
		       "[surface-scale] 80 displayed pixels doubles scale at 4x film mapping" );
	}
	Check( c.Editor().Undo(), "[surface-scale] first scale gesture undoes" );

	// A backing/surface change may refresh the overlay while the pointer is
	// still down. The cumulative delta remains relative to the original anchor,
	// so scale sensitivity must remain frozen at pointer-down as well.
	Check( c.SetPaneSurfaceDims( 0, 256, 256 ),
	       "[surface-scale] restore 4x mapping before resize-during-drag" );
	c.RefreshGizmoHandles();
	uniformCube = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::UniformScaleCube ) ) {
			uniformCube = static_cast<int>( i );
			break;
		}
	}
	if( uniformCube >= 0 ) {
		const Point2 down( c.GizmoHandleScreenX( uniformCube ),
		                   c.GizmoHandleScreenY( uniformCube ) );
		c.OnPointerDown( down );
		Check( c.SetPaneSurfaceDims( 0, 512, 512 ),
		       "[surface-scale] mid-drag surface resize accepted" );
		c.RefreshGizmoHandles();
		c.OnPointerMove( Point2( down.x + 320.0, down.y ) );
		c.OnPointerUp( Point2( down.x + 320.0, down.y ) );
		IObjectPriv* sphere = job->GetObjects() ? job->GetObjects()->GetItem( "sphere" ) : 0;
		const Matrix4 scaled = sphere ? sphere->GetFinalTransformMatrix() : Matrix4Ops::Identity();
		const double lx = std::sqrt( static_cast<double>(
			scaled._00 * scaled._00 + scaled._01 * scaled._01 + scaled._02 * scaled._02 ) );
		Check( std::fabs( lx - 2.0 ) < 1e-8,
		       "[surface-scale] mid-drag resize does not change anchored sensitivity" );
	}
	Check( c.Editor().Undo(), "[surface-scale] resize-during-drag gesture undoes" );

	// Quad pane dimensions are stale when the UI returns to its larger Single
	// surface. The core clears them synchronously until the shell reports the
	// new Single draw area.
	Check( c.SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
	       "[surface-scale] Quad layout accepted" );
	Check( c.SetPaneSurfaceDims( 0, 256, 256 ),
	       "[surface-scale] Quad pane dimensions accepted" );
	Check( c.SetViewportLayout( SceneEditController::ViewportLayout::Single ),
	       "[surface-scale] return to Single accepted" );
	c.SetTool( SceneEditController::Tool::RotateObject );
	c.RefreshGizmoHandles();
	xRing = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisRing )
		 && c.GizmoHandleAxis( i ) == 0 ) {
			xRing = static_cast<int>( i );
			break;
		}
	}
	Check( xRing >= 0 && std::fabs( c.GizmoHandleScreenRadius( xRing ) - 58.0 ) < 1e-9,
	       "[surface-scale] Quad-to-Single clears stale pane-0 surface scaling" );
	Check( c.SetPaneSurfaceDims( 0, 512, 512 ),
	       "[surface-scale] shell republishes the Single draw surface" );
	c.RefreshGizmoHandles();
	xRing = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisRing )
		 && c.GizmoHandleAxis( i ) == 0 ) {
			xRing = static_cast<int>( i );
			break;
		}
	}
	Check( xRing >= 0 && std::fabs( c.GizmoHandleScreenRadius( xRing ) - 116.0 ) < 1e-9,
	       "[surface-scale] Single re-publication restores display-space scaling" );
	Check( c.SetViewportLayout( SceneEditController::ViewportLayout::Quad ),
	       "[surface-scale] return to Quad accepted" );
	Check( c.SetPaneSurfaceDims( 0, 256, 256 ),
	       "[surface-scale] shell publishes the resized Quad pane" );
	c.RefreshGizmoHandles();
	xRing = -1;
	for( unsigned int i = 0; i < c.GizmoHandleCount(); ++i ) {
		if( c.GizmoHandleKind( i ) == static_cast<int>( K::AxisRing )
		 && c.GizmoHandleAxis( i ) == 0 ) {
			xRing = static_cast<int>( i );
			break;
		}
	}
	Check( xRing >= 0 && std::fabs( c.GizmoHandleScreenRadius( xRing ) - 232.0 ) < 1e-9,
	       "[surface-scale] Single-to-Quad publication updates gizmo scaling" );
	job->release();
}

int main()
{
	std::cout << "SceneEditorGizmoHandlesTest — B2 + B3 gizmo math + dispatch" << std::endl;
	Matrix4 nonFinite = Matrix4Ops::Identity();
	nonFinite._00 = QuietNaNForTest();
	Check( !MatrixClose( nonFinite, nonFinite ), "matrix comparisons reject NaN entries" );

	// B2 — gizmo math.
	TestProjectionPivotAtScreenCenter();
	TestProjectionAxisDirections();
	TestProjectionRejectsBehindCamera();
	TestNoHandlesWhenInapplicable();
	TestTranslateHandleSet();
	TestRotateHandleSet();
	TestScaleHandleSet();
	TestTranslateAxisArrowDistance();
	TestColinearAxisOmitted();
	TestPivotTracksObjectTranslation();

	// B3 — pointer hit-test + drag dispatch.
	TestHitTestPicksHandle();
	TestPointerDownArmsGizmoDrag();
	TestAxisArrowDragTranslatesAlongAxis();
	TestAxisPlaneDragStaysInPlane();
	TestScreenCenterDragTranslates();
	TestAxisRingDragRotates();
	TestScaleGizmoUndoThenAbsoluteSet();
	TestFreeScaleDragAccumulatesFromBaseline();
	TestGizmoSizeTracksViewportSurface();

	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
