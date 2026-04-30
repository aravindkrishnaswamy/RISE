//////////////////////////////////////////////////////////////////////
//
//  CameraUnitConversionTest.cpp — exercises Phase 1.2 unit handling
//    on ThinLensCamera.  Verifies:
//
//    (1) `sensor_size`, `focal_length`, `shift_x/y` are stored as mm
//        on the camera (the editor reads / writes in mm directly).
//    (2) `focus_distance` is stored in scene units (matches geometry).
//    (3) FOV is unit-invariant: same mm-input + different scene_unit
//        gives the same horizontal FOV.
//    (4) Halved-aperture (used by the disk sampler) is in scene units
//        and scales with `1 / scene_unit_meters`, so the same logical
//        aperture in mm produces 1000× the value in a mm-scale scene
//        as in a metres-scale scene.
//    (5) Ray generation is unit-invariant: a "metres-scale" camera and
//        a "mm-scale" camera with all positions scaled by 1000 produce
//        rays whose origins differ by exactly that factor and whose
//        directions match.
//
//  Author: Generated for Phase 1.2 unit-conversion work
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Interfaces/IReference.h"
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"

using namespace RISE;
using namespace RISE::Implementation;

// Local helper to release a Reference-counted object created via `new`
// in tests.  ThinLensCamera (via Reference) has a protected destructor;
// callers normally hand the camera to a Job which calls release().  In
// tests we own a ref ourselves and drop it manually.
template<typename T>
static void release( T* p ) { if( p ) p->release(); }

static int g_fail = 0;
static int g_pass = 0;

#define EXPECT(cond) do {                                          \
		if( !(cond) ) {                                            \
			std::cerr << "FAIL " << __FILE__ << ":" << __LINE__    \
			          << " " #cond << std::endl;                   \
			++g_fail;                                              \
		} else { ++g_pass; }                                       \
	} while(0)

#define EXPECT_NEAR(a, b, tol) do {                                \
		const double _aa = (a);                                    \
		const double _bb = (b);                                    \
		if( fabs( _aa - _bb ) > (tol) ) {                          \
			std::cerr << "FAIL " << __FILE__ << ":" << __LINE__    \
			          << " |" #a " - " #b "| > " #tol               \
			          << "  (" << _aa << " vs " << _bb << ")"      \
			          << std::endl;                                \
			++g_fail;                                              \
		} else { ++g_pass; }                                       \
	} while(0)


// Construct a thin-lens camera with sensible defaults for the tests.
// `sceneUnitMeters` and `focusDistance` differ between the metres and
// mm variants; everything else is constant.  Pure helper, no globals.
static ThinLensCamera* makeCam(
	double sceneUnitMeters,
	const Point3& location,
	double focusDistance )
{
	const Point3 lookAt(0, 0, 0);
	const Vector3 up(0, 1, 0);
	const double sensor       = 36.0;     // mm (full-frame)
	const double focal        = 35.0;     // mm
	const double fstop        = 2.8;
	const unsigned int width  = 256;
	const unsigned int height = 256;
	const double pixelAR      = 1.0;
	const double exposure     = 0.0;
	const double scanRate     = 0.0;
	const double pixelRate    = 0.0;
	const Vector3 orient(0, 0, 0);
	const Vector2 targetOrient(0, 0);
	const unsigned int blades = 0;        // disk
	const double rot          = 0.0;
	const double squeeze      = 1.0;
	const double tiltX        = 0.0;
	const double tiltY        = 0.0;
	const double shiftX       = 0.0;      // mm
	const double shiftY       = 0.0;      // mm
	return new ThinLensCamera( location, lookAt, up,
		sensor, focal, fstop, focusDistance, sceneUnitMeters,
		width, height, pixelAR, exposure, scanRate, pixelRate,
		orient, targetOrient,
		blades, rot, squeeze, tiltX, tiltY, shiftX, shiftY );
}


// Test 1: stored mm fields are unchanged from input regardless of scene_unit.
static void TestStoredFieldsAreMM()
{
	std::cout << "TestStoredFieldsAreMM\n";
	ThinLensCamera* camA = makeCam( 1.0,    Point3(0,0,3),    3.0    );
	ThinLensCamera* camB = makeCam( 0.001,  Point3(0,0,3000), 3000.0 );

	// sensor / focal stored as mm on both cameras.
	EXPECT_NEAR( camA->GetSensorSize(),        36.0, 1e-12 );
	EXPECT_NEAR( camB->GetSensorSize(),        36.0, 1e-12 );
	EXPECT_NEAR( camA->GetFocalLengthStored(), 35.0, 1e-12 );
	EXPECT_NEAR( camB->GetFocalLengthStored(), 35.0, 1e-12 );

	// fstop is dimensionless.
	EXPECT_NEAR( camA->GetFstop(), 2.8, 1e-12 );
	EXPECT_NEAR( camB->GetFstop(), 2.8, 1e-12 );

	// Shifts default to 0 in both; setter stores raw mm.
	EXPECT_NEAR( camA->GetShiftX(), 0.0, 1e-12 );
	EXPECT_NEAR( camB->GetShiftX(), 0.0, 1e-12 );

	// scene_unit_meters round-trips as-is.
	EXPECT_NEAR( camA->GetSceneUnitMeters(), 1.0,    1e-12 );
	EXPECT_NEAR( camB->GetSceneUnitMeters(), 0.001,  1e-12 );

	release( camA );
	release( camB );
}


// Test 2: focus_distance stored as scene units (matches geometry coords),
// so the user's input passes through unchanged.
static void TestFocusDistanceIsSceneUnits()
{
	std::cout << "TestFocusDistanceIsSceneUnits\n";
	ThinLensCamera* camA = makeCam( 1.0,    Point3(0,0,3),    3.0    );
	ThinLensCamera* camB = makeCam( 0.001,  Point3(0,0,3000), 3000.0 );

	EXPECT_NEAR( camA->GetFocusDistanceStored(), 3.0,    1e-12 );
	EXPECT_NEAR( camB->GetFocusDistanceStored(), 3000.0, 1e-12 );

	release( camA );
	release( camB );
}


// Test 3: FOV is unit-invariant — both cameras yield 2*atan(36/(2*35)).
// We can read FOV indirectly via image-plane geometry: the chief ray for
// the rightmost pixel should hit the focal plane at x = focusDistance *
// tan(fov_h / 2), in scene units.
static void TestFOVIsUnitInvariant()
{
	std::cout << "TestFOVIsUnitInvariant\n";
	ThinLensCamera* camA = makeCam( 1.0,    Point3(0,0,3),    3.0    );
	ThinLensCamera* camB = makeCam( 0.001,  Point3(0,0,3000), 3000.0 );

	// Generate central-pixel chief ray with a deterministic lens sample
	// at the disk centre (which yields a chief ray from the lens centre).
	// Camera ray generation uses `rc.random` only on the disk-sample
	// path (GenerateRay).  GenerateRayWithLensSample doesn't touch
	// `rc.random`, so a stub RNG with a fixed seed suffices.
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	const Point2 centerPixel( 128.5, 128.5 );
	const Point2 centerLens(  0.5,   0.5   );

	Ray rayA, rayB;
	camA->GenerateRayWithLensSample( rc, rayA, centerPixel, centerLens );
	camB->GenerateRayWithLensSample( rc, rayB, centerPixel, centerLens );

	// Both rays' directions should be the camera-forward direction
	// (looking toward (0,0,0) from (0,0,3) in metres or (0,0,3000) in
	// mm), which is +z * -1 = -z.  Direction is unit-free, so identical.
	EXPECT_NEAR( rayA.Dir().x, rayB.Dir().x, 1e-9 );
	EXPECT_NEAR( rayA.Dir().y, rayB.Dir().y, 1e-9 );
	EXPECT_NEAR( rayA.Dir().z, rayB.Dir().z, 1e-9 );

	// FOV is intrinsic to (sensor/focal): both cameras share the
	// expected horizontal FOV = 2*atan(36/(2*35)) ≈ 0.9522 rad.
	// Rather than reach into private state, verify it indirectly via
	// the right-edge chief ray.  At the right edge, the chief ray hits
	// the focal plane at x = focusDistance * tan(fov_h / 2).
	const Point2 rightEdgePixel( 256.0, 128.5 );
	camA->GenerateRayWithLensSample( rc, rayA, rightEdgePixel, centerLens );
	camB->GenerateRayWithLensSample( rc, rayB, rightEdgePixel, centerLens );

	// Project the chief ray to focusDistance along z (camera looks down
	// -z, so subtract).  ray = origin + t * dir; we want world z = 0
	// (the lookat point).
	auto projectToZ0 = []( const Ray& r ) -> double {
		const double t = -r.origin.z / r.Dir().z;
		return r.origin.x + t * r.Dir().x;
	};

	const double xA = projectToZ0( rayA );
	const double xB = projectToZ0( rayB );

	// xA is in metres; xB is in mm.  In metres, the right-edge chief
	// ray hits at x = 3 m * tan(fov/2) ≈ 1.6346 m.
	const double expectedX_metres = 3.0 * tan( atan( 36.0 / (2.0 * 35.0) ) );
	EXPECT_NEAR( xA, expectedX_metres,        1e-3 );
	EXPECT_NEAR( xB, expectedX_metres * 1000, 1.0 );  // 1mm tolerance

	release( camA );
	release( camB );
}


// Test 4: ray-generation invariance across scaled scenes.  Two cameras
// representing "the same physical camera in different unit systems"
// should produce rays where positions scale by the unit factor and
// directions are identical.
static void TestRaysScaleInvariant()
{
	std::cout << "TestRaysScaleInvariant\n";
	// camA: metres scene, camera at 3m, focus at 3m.
	ThinLensCamera* camA = makeCam( 1.0,   Point3(0,0,3),    3.0    );
	// camB: mm scene, camera at 3000mm, focus at 3000mm — same physical
	// camera, just expressed in mm.
	ThinLensCamera* camB = makeCam( 0.001, Point3(0,0,3000), 3000.0 );

	// Camera ray generation uses `rc.random` only on the disk-sample
	// path (GenerateRay).  GenerateRayWithLensSample doesn't touch
	// `rc.random`, so a stub RNG with a fixed seed suffices.
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	// Sweep a few representative pixel + lens-sample combinations.
	// Use deterministic lensSample (no RNG) so both cameras see
	// identical primary samples.
	const Point2 pixels[] = {
		Point2(0.0,   0.0),       // top-left corner
		Point2(128.5, 128.5),     // centre
		Point2(255.0, 255.0),     // bottom-right corner
		Point2(64.0,  192.0),     // off-axis
	};
	const Point2 lensSamples[] = {
		Point2(0.5, 0.5),         // disk centre
		Point2(0.1, 0.9),         // off-centre on aperture
		Point2(0.9, 0.1),
	};

	for( const Point2& px : pixels ) {
		for( const Point2& ls : lensSamples ) {
			Ray rayA, rayB;
			camA->GenerateRayWithLensSample( rc, rayA, px, ls );
			camB->GenerateRayWithLensSample( rc, rayB, px, ls );

			// Origins differ by exactly the scale factor (metres → mm = ×1000).
			EXPECT_NEAR( rayA.origin.x * 1000.0, rayB.origin.x, 1e-6 );
			EXPECT_NEAR( rayA.origin.y * 1000.0, rayB.origin.y, 1e-6 );
			EXPECT_NEAR( rayA.origin.z * 1000.0, rayB.origin.z, 1e-6 );

			// Directions are unit-free (normalised) — should match.
			EXPECT_NEAR( rayA.Dir().x, rayB.Dir().x, 1e-9 );
			EXPECT_NEAR( rayA.Dir().y, rayB.Dir().y, 1e-9 );
			EXPECT_NEAR( rayA.Dir().z, rayB.Dir().z, 1e-9 );
		}
	}

	release( camA );
	release( camB );
}


// Test 5: non-zero shift scales correctly across scene_units.  The
// camera caches `shiftX_sceneUnits = shiftX_mm * mm_to_scene` in
// Recompute(), so two cameras with the same physical shift but
// different unit scales must produce rays whose origins differ
// only by the unit factor (same as Test 4 but exercising the cached
// path on a non-zero shift).
static void TestShiftScalesAcrossSceneUnits()
{
	std::cout << "TestShiftScalesAcrossSceneUnits\n";
	// Build two cameras at the same physical layout (camera 3 m back,
	// focus 3 m, lens shifted 5 mm right + 2 mm up), expressed in
	// metres-scene and mm-scene respectively.
	const Point3 locA(0, 0, 3),     locB(0, 0, 3000);
	const Point3 lookAt(0, 0, 0);
	const Vector3 up(0, 1, 0);
	const Vector3 orient(0, 0, 0);
	const Vector2 targetOrient(0, 0);
	const double shiftX_mm = 5.0;     // physical 5mm shift right
	const double shiftY_mm = 2.0;     // physical 2mm shift up
	ThinLensCamera* camA = new ThinLensCamera( locA, lookAt, up,
		36.0, 35.0, 2.8, 3.0, 1.0,            // metres scene
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, 0.0, shiftX_mm, shiftY_mm );
	ThinLensCamera* camB = new ThinLensCamera( locB, lookAt, up,
		36.0, 35.0, 2.8, 3000.0, 0.001,       // mm scene
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, 0.0, shiftX_mm, shiftY_mm );

	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	// Sweep a few representative pixels.  The shift should produce an
	// off-axis chief ray; verify origin scales 1000× and direction is
	// invariant.
	const Point2 pixels[] = {
		Point2(128.5, 128.5),     // centre (most diagnostic)
		Point2(64.0,  64.0),
		Point2(192.0, 192.0),
	};
	const Point2 ls( 0.5, 0.5 );  // disk centre

	for( const Point2& px : pixels ) {
		Ray rayA, rayB;
		camA->GenerateRayWithLensSample( rc, rayA, px, ls );
		camB->GenerateRayWithLensSample( rc, rayB, px, ls );

		EXPECT_NEAR( rayA.origin.x * 1000.0, rayB.origin.x, 1e-6 );
		EXPECT_NEAR( rayA.origin.y * 1000.0, rayB.origin.y, 1e-6 );
		EXPECT_NEAR( rayA.origin.z * 1000.0, rayB.origin.z, 1e-6 );

		EXPECT_NEAR( rayA.Dir().x, rayB.Dir().x, 1e-9 );
		EXPECT_NEAR( rayA.Dir().y, rayB.Dir().y, 1e-9 );
		EXPECT_NEAR( rayA.Dir().z, rayB.Dir().z, 1e-9 );
	}

	// Concrete sanity-check: at the centre pixel, +shiftY should make
	// the camera look upward (per Cycles convention; Phase 1.1 sign
	// derivation).  In camera-local coords for the regression scene
	// (W = (0,0,-1), V = (0,1,0)), "looks up" = world +y.
	Ray ray;
	camA->GenerateRayWithLensSample( rc, ray, Point2(128.5, 128.5), ls );
	EXPECT( ray.Dir().y > 0.0 );  // shiftY_mm > 0 → camera looks up

	release( camA );
	release( camB );
}


// Test 6: tilt-shift cache invariance — non-zero tilt with non-default
// scene_unit.  The focal-plane equation cache (kFocus, nFocus*) lives
// in scene units, so tilt+shift+scene_unit interactions need to round
// trip correctly across scaled scenes.
static void TestTiltCacheUnitInvariant()
{
	std::cout << "TestTiltCacheUnitInvariant\n";
	const Point3 locA(0, 0, 3),     locB(0, 0, 3000);
	const Point3 lookAt(0, 0, 0);
	const Vector3 up(0, 1, 0);
	const Vector3 orient(0, 0, 0);
	const Vector2 targetOrient(0, 0);
	const double tilt_y_rad = 25.0 * 3.14159265358979323846 / 180.0;
	ThinLensCamera* camA = new ThinLensCamera( locA, lookAt, up,
		36.0, 35.0, 2.0, 3.0, 1.0,
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, tilt_y_rad, 0.0, 0.0 );
	ThinLensCamera* camB = new ThinLensCamera( locB, lookAt, up,
		36.0, 35.0, 2.0, 3000.0, 0.001,
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, tilt_y_rad, 0.0, 0.0 );

	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	// Off-axis pixels — these are where tilt makes the focal plane
	// non-perpendicular to the optical axis.  With tilt_y=25° on the
	// edge pixel (x=255), the chief ray hits the focal plane at a
	// noticeably different focus depth than it would without tilt.
	const Point2 pixels[] = {
		Point2(0.0,   128.5),     // left edge
		Point2(255.0, 128.5),     // right edge (most affected by tilt_y)
		Point2(128.5, 0.0),
		Point2(128.5, 255.0),
	};
	const Point2 ls( 0.3, 0.7 );  // off-centre lens sample

	for( const Point2& px : pixels ) {
		Ray rayA, rayB;
		camA->GenerateRayWithLensSample( rc, rayA, px, ls );
		camB->GenerateRayWithLensSample( rc, rayB, px, ls );

		EXPECT_NEAR( rayA.origin.x * 1000.0, rayB.origin.x, 1e-5 );
		EXPECT_NEAR( rayA.origin.y * 1000.0, rayB.origin.y, 1e-5 );
		EXPECT_NEAR( rayA.origin.z * 1000.0, rayB.origin.z, 1e-5 );

		EXPECT_NEAR( rayA.Dir().x, rayB.Dir().x, 1e-9 );
		EXPECT_NEAR( rayA.Dir().y, rayB.Dir().y, 1e-9 );
		EXPECT_NEAR( rayA.Dir().z, rayB.Dir().z, 1e-9 );
	}

	release( camA );
	release( camB );
}


// Test: every camera parameter the panel surfaces must carry the
// expected unit label so the user can tell at a glance whether "35"
// means "35 mm" or "35 metres".  This is the structural guard that
// would have caught the user's confusion from the field-not-labeled
// review.
//
// Conventions, in one place:
//   sensor_size, focal_length, shift_x, shift_y  -> "mm"
//   focus_distance                               -> "scene units"
//   tilt_x, tilt_y, aperture_rotation            -> "°"
//   pitch, yaw, roll, theta, phi, orientation,
//     target_orientation                         -> "°"  (CameraCommon)
//   fstop, anamorphic_squeeze, aperture_blades,
//     pixelAR, exposure, scanning_rate, pixel_rate,
//     width, height, location, lookat, up        -> ""   (no unit label)
//
// If anyone changes the camera storage convention without updating
// the unit label, this test fails immediately.
static void TestPanelUnitLabels()
{
	std::cout << "TestPanelUnitLabels\n";
	const Point3 loc(0, 0, 5), lookAt(0, 0, 0);
	const Vector3 up(0, 1, 0);
	const Vector3 orient(0, 0, 0);
	const Vector2 targetOrient(0, 0);
	ThinLensCamera* cam = new ThinLensCamera( loc, lookAt, up,
		36.0, 35.0, 2.8, 5.0, 1.0,
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

	std::vector<CameraProperty> props = CameraIntrospection::Inspect( *cam );
	std::map<std::string, std::string> unit;
	for( const CameraProperty& p : props ) {
		unit[ std::string( p.name.c_str() ) ] = std::string( p.unitLabel.c_str() );
	}

	// Lens lengths the user expected in mm — must surface "mm" so
	// the small numbers don't read as metres.
	EXPECT( unit["sensor_size"]  == "mm" );
	EXPECT( unit["focal_length"] == "mm" );
	EXPECT( unit["shift_x"]      == "mm" );
	EXPECT( unit["shift_y"]      == "mm" );

	// focus_distance is in scene units (matches geometry coords),
	// but the introspection layer resolves the generic placeholder
	// to a concrete unit name based on the camera's
	// scene_unit_meters.  This camera was constructed with
	// scene_unit_meters = 1.0 → label becomes "m"; the
	// per-scale-mapping is exercised end-to-end in
	// TestFocusDistanceUnitLabelResolvesPerScale.
	EXPECT( unit["focus_distance"] == "m" );

	// Angle params — degrees in the panel surface (deg/rad
	// conversion happens at the editor introspection layer).
	// Note: pitch/yaw/roll/orientation/target_orientation are
	// filtered out by IsRedundantParameter (they're scalar shadows
	// of `orientation` already shown as a Vec3); only theta/phi
	// remain as scalar angle rows.
	EXPECT( unit["tilt_x"]            == "°" );
	EXPECT( unit["tilt_y"]            == "°" );
	EXPECT( unit["aperture_rotation"] == "°" );
	EXPECT( unit["theta"]             == "°" );
	EXPECT( unit["phi"]               == "°" );

	// Dimensionless / no-unit fields.  Empty string is the contract
	// for "don't render a unit label at all".
	EXPECT( unit["fstop"]              == "" );
	EXPECT( unit["anamorphic_squeeze"] == "" );
	EXPECT( unit["aperture_blades"]    == "" );

	release( cam );
}


// Test: focus_distance unit label resolves to a real unit name
// based on the camera's scene_unit_meters.  The user reads the
// real unit at a glance ("3000 mm" or "3 m") rather than the
// generic "scene units".  Locks the conversion table so the GUI
// can't drift.
static void TestFocusDistanceUnitLabelResolvesPerScale()
{
	std::cout << "TestFocusDistanceUnitLabelResolvesPerScale\n";
	struct Case { double sceneUnitMeters; double focusValue; const char* expectedLabel; };
	const Case cases[] = {
		{ 1.0,    3.0,    "m"           },   // metres scene
		{ 0.001,  3000.0, "mm"          },   // mm scene
		{ 0.01,   300.0,  "cm"          },   // cm scene
		{ 0.0254, 118.0,  "in"          },   // inches scene
		{ 0.3048, 9.84,   "ft"          },   // feet scene
		{ 0.05,   60.0,   "scene units" },   // unrecognised → fall back
	};
	for( const Case& c : cases ) {
		const Point3 loc(0, 0, 5), lookAt(0, 0, 0);
		const Vector3 up(0, 1, 0);
		const Vector3 orient(0, 0, 0);
		const Vector2 targetOrient(0, 0);
		ThinLensCamera* cam = new ThinLensCamera( loc, lookAt, up,
			36.0, 35.0, 2.8, c.focusValue, c.sceneUnitMeters,
			256, 256, 1.0, 0.0, 0.0, 0.0,
			orient, targetOrient,
			0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

		std::vector<CameraProperty> props = CameraIntrospection::Inspect( *cam );
		std::string label;
		for( const CameraProperty& p : props ) {
			if( std::string( p.name.c_str() ) == "focus_distance" ) {
				label = std::string( p.unitLabel.c_str() );
				break;
			}
		}
		EXPECT( label == c.expectedLabel );

		release( cam );
	}
}


// Test 7: editor introspection roundtrip — what the panel ACTUALLY
// shows for sensor_size, focal_length, shift_x, shift_y must equal
// the raw mm value the constructor was given, BIT-EXACT.  This is
// the contract that prevents the panel from silently displaying
// scene-unit-converted values when the user expects mm.  If this
// test ever fails, the editor is showing the wrong unit.
//
// Also verifies that focus_distance comes through as scene-units
// (matches the geometry coords) and that the scene_unit_meters
// factor is exposed correctly.
static void TestPanelShowsMMNotMetres()
{
	std::cout << "TestPanelShowsMMNotMetres\n";

	// Five representative configurations exercising the unit story
	// from both ends — small mm-scale numbers (the migrated
	// FeatureBased scenes) and big metres-scale-with-mm-camera
	// (the redesigned tilt-shift demo).  Each tuple is (label,
	// sceneUnitMeters, sensor_mm, focal_mm, fstop, focus_scene,
	// shift_x_mm, shift_y_mm).
	struct Setup {
		const char*  label;
		double sceneUnitMeters;
		double sensor_mm, focal_mm, fstop;
		double focus_scene;
		double shift_x_mm, shift_y_mm;
	};
	const Setup setups[] = {
		{ "metres scene + 35mm lens",       1.0,    36.0,   35.0, 2.8,  3.0,    0.0,  0.0 },
		{ "mm scene + 4mm lens (FeatureBased)", 0.001, 3.07, 4.0, 13.33, 15.0,  0.0,  0.0 },
		{ "metres scene + non-zero shift",  1.0,    36.0,   50.0, 4.0,  2.0,    5.5, -2.5 },
		{ "inches scene + 24mm lens",       0.0254, 36.0,   24.0, 5.6,  10.0,   0.0,  0.0 },
		{ "Cornell-mm + 18mm",              0.001, 16.79,  18.0,  0.225, 490.0, 0.0,  0.0 },
	};

	for( const Setup& s : setups ) {
		std::cout << "  setup: " << s.label << "\n";
		const Point3 loc(0, 0, 5);
		const Point3 lookAt(0, 0, 0);
		const Vector3 up(0, 1, 0);
		const Vector3 orient(0, 0, 0);
		const Vector2 targetOrient(0, 0);
		ThinLensCamera* cam = new ThinLensCamera( loc, lookAt, up,
			s.sensor_mm, s.focal_mm, s.fstop, s.focus_scene, s.sceneUnitMeters,
			256, 256, 1.0, 0.0, 0.0, 0.0,
			orient, targetOrient,
			0, 0.0, 1.0,
			0.0, 0.0,
			s.shift_x_mm, s.shift_y_mm );

		std::vector<CameraProperty> props = CameraIntrospection::Inspect( *cam );

		// Build a quick name → value map so we don't depend on row order.
		std::map<std::string, std::string> v;
		for( const CameraProperty& p : props ) {
			v[ std::string( p.name.c_str() ) ] = std::string( p.value.c_str() );
		}

		// Helper: parse a FormatDouble("%.6g") string back to double
		// for numeric comparison.  We only need approximate equality
		// because %.6g loses 7+ digits of precision.
		auto val = []( const std::string& s ) -> double {
			char* end = nullptr;
			return std::strtod( s.c_str(), &end );
		};

		// THE CORE INVARIANT — the panel must show the mm value the
		// user entered, NOT a scene-unit-converted value.  This is
		// what would catch the regression the user reported.
		EXPECT_NEAR( val( v["sensor_size"]   ), s.sensor_mm,  1e-4 );
		EXPECT_NEAR( val( v["focal_length"]  ), s.focal_mm,   1e-4 );
		EXPECT_NEAR( val( v["fstop"]         ), s.fstop,      1e-4 );
		EXPECT_NEAR( val( v["focus_distance"]), s.focus_scene,1e-4 );
		EXPECT_NEAR( val( v["shift_x"]       ), s.shift_x_mm, 1e-4 );
		EXPECT_NEAR( val( v["shift_y"]       ), s.shift_y_mm, 1e-4 );

		// If anyone ever introduces an mm→scene-unit conversion at
		// the introspection layer, sensor_size for a metres-scale
		// scene would show as `s.sensor_mm * 0.001 / 1` = 0.036, not
		// 36.  This stricter check catches that specifically.
		EXPECT( val( v["sensor_size"] )   > 1.0 );  // mm always >1 for a real sensor
		EXPECT( val( v["focal_length"] )  > 1.0 );  // mm always >1 for a real lens

		release( cam );
	}
}


// Test 8: editor setter "round-trips" through the same path the
// preset-pick uses.  Picking "Full-frame 35mm" (preset.value = "36")
// drives Bridge.setProperty("sensor_size", "36") → SetProperty in
// CameraIntrospection → SetSensorSize(36) → camera stores 36 mm.
// On the next Inspect, the panel sees "36" again — BIT-EXACT round
// trip, no implicit unit drift.
//
// This is the exact path the user reported as broken: pick a mm
// preset, value gets stored, and the panel must continue to show
// mm.  If anyone introduces an "mm to metres" conversion anywhere,
// either Set or Get will diverge from "36" and this test catches it.
static void TestPresetPickRoundTripsAsMM()
{
	std::cout << "TestPresetPickRoundTripsAsMM\n";

	const Point3 loc(0, 0, 5), lookAt(0, 0, 0);
	const Vector3 up(0, 1, 0);
	const Vector3 orient(0, 0, 0);
	const Vector2 targetOrient(0, 0);
	ThinLensCamera* cam = new ThinLensCamera( loc, lookAt, up,
		36.0, 35.0, 2.8, 5.0, 1.0,         // metres scene, full-frame 35mm
		256, 256, 1.0, 0.0, 0.0, 0.0,
		orient, targetOrient,
		0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

	// Sensor presets, exactly as declared in the descriptor.  Picking
	// any of these via the panel produces a SetProperty call with the
	// preset's literal value string.
	const struct { const char* label; const char* value_mm; double expected_mm; } presets[] = {
		{ "Full-frame 35mm",         "36",    36.0   },
		{ "APS-C (Sony/Nikon/Fuji)", "23.6",  23.6   },
		{ "Super 35 (cinema)",       "24.89", 24.89  },
		{ "Micro Four Thirds",       "17.3",  17.3   },
		{ "8×10 large format",       "254",   254.0  },
	};

	for( const auto& p : presets ) {
		// Drive the editor's SetProperty path with the preset's mm
		// literal — same as what the panel does when the user picks
		// the entry.
		const bool ok = CameraIntrospection::SetProperty(
			*cam, String("sensor_size"), String(p.value_mm) );
		EXPECT( ok );

		// Camera storage MUST be the mm value, BIT-EXACT.  No metres
		// conversion, no scene-unit conversion.
		EXPECT_NEAR( cam->GetSensorSize(), p.expected_mm, 1e-9 );

		// The next panel snapshot must show the same mm value.
		std::vector<CameraProperty> props = CameraIntrospection::Inspect( *cam );
		std::string displayed;
		for( const CameraProperty& pr : props ) {
			if( std::string( pr.name.c_str() ) == "sensor_size" ) {
				displayed = std::string( pr.value.c_str() );
				break;
			}
		}
		// Allow %.6g rounding but NOT a 1000× factor.  If anyone
		// converts to metres, displayed would parse as 0.036 rather
		// than 36 for the full-frame case.
		const double back = std::strtod( displayed.c_str(), nullptr );
		EXPECT_NEAR( back, p.expected_mm, 1e-4 );
	}

	release( cam );
}


// Test 9: focal_length presets — same roundtrip story for focal.
// Even though focal doesn't have a hard-coded preset list today,
// users will scrub-edit the field with mm values, so the same
// invariant must hold.
static void TestFocalLengthSetterRoundTripsAsMM()
{
	std::cout << "TestFocalLengthSetterRoundTripsAsMM\n";

	// Both metres and mm scenes — confirm the roundtrip works at
	// either end of the unit spectrum.  Same bug-pattern guard:
	// if focal_length is silently scaled by scene_unit on either
	// the get or set side, this test catches it.
	struct Case { double sceneUnitMeters; const char* input_mm; double expected_mm; };
	const Case cases[] = {
		{ 1.0,    "35",   35.0 },     // metres scene, 35mm lens
		{ 1.0,    "85",   85.0 },     // metres scene, 85mm portrait lens
		{ 0.001,  "200",  200.0 },    // mm scene, 200mm telephoto
		{ 0.0254, "50",   50.0 },     // inches scene, 50mm normal lens
	};

	for( const Case& c : cases ) {
		const Point3 loc(0, 0, 5);
		const Point3 lookAt(0, 0, 0);
		const Vector3 up(0, 1, 0);
		const Vector3 orient(0, 0, 0);
		const Vector2 targetOrient(0, 0);
		// focus_distance must be > focal_in_scene_units; pick something
		// safe per scene_unit.  For mm-scene with 200mm focal: focus
		// must be > 200 (scene-units = mm); pick 5000mm.  For metres:
		// focal_scene = mm * 0.001; focus must be > 0.035 (for 35mm)
		// or 0.085 (for 85mm); pick 3 metres.
		const double focus_safe =
			(c.sceneUnitMeters >= 0.99) ? 3.0 :
			(c.sceneUnitMeters <= 0.0011) ? 5000.0 :
			10.0;  // inches
		ThinLensCamera* cam = new ThinLensCamera( loc, lookAt, up,
			36.0, 35.0, 2.8, focus_safe, c.sceneUnitMeters,
			256, 256, 1.0, 0.0, 0.0, 0.0,
			orient, targetOrient,
			0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );

		const bool ok = CameraIntrospection::SetProperty(
			*cam, String("focal_length"), String(c.input_mm) );
		EXPECT( ok );

		// Stored mm value must match input bit-exactly.
		EXPECT_NEAR( cam->GetFocalLengthStored(), c.expected_mm, 1e-9 );

		// And the editor introspection must emit the same mm value.
		std::vector<CameraProperty> props = CameraIntrospection::Inspect( *cam );
		std::string displayed;
		for( const CameraProperty& pr : props ) {
			if( std::string( pr.name.c_str() ) == "focal_length" ) {
				displayed = std::string( pr.value.c_str() );
				break;
			}
		}
		const double back = std::strtod( displayed.c_str(), nullptr );
		EXPECT_NEAR( back, c.expected_mm, 1e-4 );

		release( cam );
	}
}


// Test 10: editor setter stores mm directly — no implicit conversion.
// (The editor's SetX path is the same as the constructor input, so
// any conversion would show up here as a stored-value mismatch.)
static void TestEditorSetterIsMMDirect()
{
	std::cout << "TestEditorSetterIsMMDirect\n";
	ThinLensCamera* cam = makeCam( 1.0, Point3(0,0,3), 3.0 );

	cam->SetSensorSize( 24.89 );        // Super 35
	cam->SetFocalLengthStored( 50.0 );  // 50mm
	cam->SetShiftX( 5.0 );              // 5mm shift
	cam->SetShiftY( -2.0 );

	EXPECT_NEAR( cam->GetSensorSize(),        24.89, 1e-12 );
	EXPECT_NEAR( cam->GetFocalLengthStored(), 50.0,  1e-12 );
	EXPECT_NEAR( cam->GetShiftX(),             5.0,  1e-12 );
	EXPECT_NEAR( cam->GetShiftY(),            -2.0,  1e-12 );

	release( cam );
}


// Scene-file consistency guard: every in-tree scene that uses
// thinlens_camera should pick a `scene_unit` that matches the
// physical scale of the geometry — metres for stage / architectural
// scenes (default scene_unit = 1.0; usually no scene_options block
// at all), millimetres only when the scene is genuinely millimetre-
// scale (e.g. the canonical 552 mm Cornell box).  Each scene below
// has an expected `scene_unit` that the test pins; deviation forces
// a justifying comment in this table.
//
// `expectedNaN == true` means the scene MUST NOT declare a
// scene_options block (it relies on the default scene_unit = 1.0).
// This is the case for metres-scale scenes — they pick the
// quietest possible convention so the lens specs read in mm and
// the geometry / focus_distance read in metres.
static bool FileExists( const std::string& p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0;
}

static std::string FindRepoRoot()
{
	const char* anchor = "scenes/FeatureBased/Geometry/teapot.RISEscene";
	if( const char* env = std::getenv( "RISE_MEDIA_PATH" ) ) {
		std::string root( env );
		while( !root.empty() && root.back() == '/' ) root.pop_back();
		if( FileExists( root + "/" + anchor ) ) return root;
	}
	char cwd[4096];
	if( !getcwd( cwd, sizeof(cwd) ) ) return std::string();
	std::string p = cwd;
	for( int depth = 0; depth < 16; ++depth ) {
		if( FileExists( p + "/" + anchor ) ) return p;
		const size_t slash = p.find_last_of( '/' );
		if( slash == std::string::npos || slash == 0 ) break;
		p.resize( slash );
	}
	return std::string();
}

// Sentinel returned by ParseSceneUnitFromFile when no scene_unit is
// found.  We use a negative number rather than NaN because the build
// is compiled with -ffast-math (Config.OSX), which optimises away
// std::isnan checks — the equivalent NaN sentinel would silently
// pass every comparison and break this test.  scene_unit must be
// strictly positive, so any value < 0 is an unambiguous "missing".
static constexpr double kSceneUnitMissing = -1.0;

// Tiny ad-hoc parser — looks for `scene_options { ... scene_unit X ... }`
// and returns X.  Returns kSceneUnitMissing if no scene_options block
// is found (scene relies on the default of 1.0).
static double ParseSceneUnitFromFile( const std::string& path )
{
	std::ifstream in( path );
	if( !in.is_open() ) return kSceneUnitMissing;
	std::string line;
	bool inSceneOptions = false;
	while( std::getline( in, line ) ) {
		// Strip leading whitespace for keyword matching.
		size_t s = line.find_first_not_of( " \t" );
		if( s == std::string::npos ) continue;
		const std::string trim = line.substr( s );
		if( !inSceneOptions ) {
			if( trim.rfind( "scene_options", 0 ) == 0 ) {
				inSceneOptions = true;
			}
		} else {
			if( trim.rfind( "}", 0 ) == 0 ) break;
			if( trim.rfind( "scene_unit", 0 ) == 0 ) {
				std::istringstream iss( trim );
				std::string key;
				double value = kSceneUnitMissing;
				iss >> key >> value;
				return value;
			}
		}
	}
	return kSceneUnitMissing;  // no scene_options or no scene_unit declared
}

static void TestScenesUseSensibleScale()
{
	std::cout << "TestScenesUseSensibleScale\n";
	const std::string root = FindRepoRoot();
	if( root.empty() ) {
		std::cerr << "  WARN: cannot locate repo root, skipping consistency check\n";
		return;
	}

	// Every in-tree scene that constructs a thinlens_camera should
	// pick a scale matching the physical content.  `expected = 1.0`
	// with `expectMissingBlock = true` means the scene relies on
	// the default scene_unit (no scene_options block).  When a
	// scene legitimately needs a different scale, add an entry
	// here with a justifying note.
	struct Entry { const char* relPath; double expected; bool expectMissingBlock; const char* note; };
	const Entry scenes[] = {
		// Metres-scale stage / architectural / studio scenes — no scene_options block.
		{ "scenes/FeatureBased/Combined/spotlight_drama.RISEscene",       1.0,   true,  "metres (stage)"        },
		{ "scenes/FeatureBased/Combined/glass_pavilion.RISEscene",        1.0,   true,  "metres (architecture)" },
		{ "scenes/FeatureBased/Combined/crystal_lens.RISEscene",          1.0,   true,  "metres (studio)"       },
		{ "scenes/FeatureBased/Combined/tidepools.RISEscene",             1.0,   true,  "metres (outdoor)"      },
		{ "scenes/Tests/Cameras/thinlens.RISEscene",                      1.0,   true,  "metres (test prop)"    },
		{ "scenes/Tests/Cameras/realistic.RISEscene",                     1.0,   true,  "metres (test prop)"    },
		{ "scenes/Tests/Cameras/camera_defaults.RISEscene",               1.0,   true,  "metres (test)"         },
		{ "scenes/Tests/Cameras/thinlens_tiltshift.RISEscene",            1.0,   true,  "metres (test row)"     },
		// Genuine mm-scale scene — the canonical 552 mm Cornell box.
		{ "scenes/Tests/BDPT/cornellbox_bdpt_thinlens.RISEscene",         0.001, false, "mm (Cornell box)"      },
	};
	for( const Entry& e : scenes ) {
		const std::string path = root + "/" + e.relPath;
		const double got = ParseSceneUnitFromFile( path );
		const bool isMissing = ( got <= 0.0 );
		if( e.expectMissingBlock ) {
			if( !isMissing ) {
				std::cerr << "FAIL " << e.relPath
				          << ": expected NO scene_options block (default scene_unit = "
				          << e.expected << ", " << e.note
				          << ") but found scene_unit = " << got << "\n";
				++g_fail;
			} else {
				++g_pass;
			}
		} else {
			if( isMissing ) {
				std::cerr << "FAIL " << e.relPath
				          << ": no scene_options { scene_unit ... } block found ("
				          << e.note << " expected, scene_unit = " << e.expected << ")\n";
				++g_fail;
			} else if( fabs( got - e.expected ) > 1e-9 ) {
				std::cerr << "FAIL " << e.relPath
				          << ": scene_unit = " << got
				          << " but expected " << e.expected
				          << " (" << e.note << ")\n";
				++g_fail;
			} else {
				++g_pass;
			}
		}
	}
}


int main()
{
	std::cout << "Running CameraUnitConversionTest..." << std::endl;
	TestStoredFieldsAreMM();
	TestFocusDistanceIsSceneUnits();
	TestFOVIsUnitInvariant();
	TestRaysScaleInvariant();
	TestShiftScalesAcrossSceneUnits();
	TestTiltCacheUnitInvariant();
	TestPanelUnitLabels();
	TestFocusDistanceUnitLabelResolvesPerScale();
	TestPanelShowsMMNotMetres();
	TestPresetPickRoundTripsAsMM();
	TestFocalLengthSetterRoundTripsAsMM();
	TestEditorSetterIsMMDirect();
	TestScenesUseSensibleScale();

	std::cout << "Passed: " << g_pass << "  Failed: " << g_fail << std::endl;
	return g_fail == 0 ? 0 : 1;
}
