//////////////////////////////////////////////////////////////////////
//
//  IORStackSeedingRegressionTest.cpp - Regression guard for the
//  per-object PARITY logic in IORStackSeeding::SeedFromPoint.
//
//  THE BUG THIS TEST GUARDS AGAINST
//
//    `IORStackSeeding::SeedFromPoint` (called at the start of every
//    BDPT / VCM / MLT eye AND light subpath) shoots a probe ray in
//    +Z from the seed point to determine which dielectric objects
//    physically contain it.  An earlier version counted each surface
//    hit with `cosN > 0` (outward normal aligned with probe) as an
//    "exit" and treated the object as containing the seed — with a
//    dedup-by-object to avoid pushing the same object twice.
//
//    That logic is correct for a seed strictly inside an object but
//    INCORRECT for a probe that simply traverses an object — e.g.
//    a camera in front of a glass sphere whose +Z probe enters at
//    the near surface (cosN < 0, "ignored") and exits at the far
//    surface (cosN > 0, "counted").  The dedup didn't compensate
//    because there's only one exit per object — so the sphere was
//    erroneously pushed onto the seed's IOR stack.
//
//    Symptom: every BDPT-family eye / light subpath starting at the
//    affected position treats its FIRST hit on the sphere as an exit
//    (refraction direction computed glass→air instead of air→glass).
//    Path goes the wrong way, exits the scene without hitting
//    anything, sample contributes 0.  In rgb_dispersive_caustic the
//    sphere annulus converges to ~34% of the PT-correct value with
//    no improvement at higher spp.
//
//    The fix tracks per-object PARITY: +1 per exit (cosN > 0), -1
//    per entry (cosN < 0).  An object only counts as containing the
//    seed if its net parity is positive.  A traverse-through pattern
//    (one entry + one exit) cancels to net zero.
//
//  TEST COVERAGE
//
//    PART 1 — direct unit tests of SeedFromPoint with a programmatic
//    minimal scene (one glass sphere at the origin).  Calls the
//    function at five positions and asserts the resulting stack.
//    `SeedFromPoint` is the shared code path for camera-side seeding
//    (BDPTIntegrator.cpp:2389, 5708) AND light-side seeding (1383,
//    4731) — so every case below pins the function regardless of
//    which caller it was reached from:
//
//      (A) Seed outside the sphere whose +Z probe passes through
//          the sphere.  Expected: stack EMPTY.  Pre-fix bug: stack
//          contains sphere (the bug case).  This is the typical
//          "camera in front of glass sphere" geometry but it
//          equally pins the light-side case where a light area's
//          +Z probe traverses a dielectric.
//      (B) Seed outside the sphere whose +Z probe goes away from
//          the sphere.  Expected: stack EMPTY.
//      (C) Seed strictly INSIDE the sphere.  Expected: stack
//          contains sphere.  Sanity check that the original purpose
//          of the seeding (luminaires inside dielectric shells) still
//          works.
//      (D) Seed to the side of the sphere whose +Z probe goes
//          parallel to the sphere and never touches it.  Expected:
//          stack EMPTY.
//      (E) Seed at a typical AREA LIGHT location (just behind a
//          dielectric, +Z toward it).  Expected: stack EMPTY.
//          Same root case as (A) but anchored to the geometry of
//          the light-side seeding bug specifically — documents
//          explicit defensive coverage for the light call sites.
//
//    PART 2 — render-based end-to-end test for the CAMERA-side
//    code path.  Renders the same scene with PT
//    (pathtracing_pel_rasterizer) and BDPT (bdpt_pel_rasterizer),
//    asserting the means agree on three regions of interest.
//
//    PART 3 — render-based end-to-end test for the LIGHT-side
//    code path.  Same skeleton, but the geometry is arranged so the
//    LIGHT's +Z probe (not the camera's) traverses the sphere.  The
//    camera position is well off-axis so its own probe doesn't see
//    the sphere.  This isolates the light-subpath seeding fix.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

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

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput — stores rendered pixels in memory.
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

//////////////////////////////////////////////////////////////////////
// Region helpers.
//////////////////////////////////////////////////////////////////////
struct RegionStats
{
	double mean[3];
	unsigned int pixelCount;
};

static RegionStats RegionMean(
	const CapturingRasterizerOutput& cap,
	double cx, double cy,
	double rMin, double rMax )
{
	RegionStats s{};
	double sum[3] = {0, 0, 0};
	unsigned int count = 0;
	for( unsigned int y = 0; y < cap.height; y++ ) {
		for( unsigned int x = 0; x < cap.width; x++ ) {
			const double dx = double(x) - cx;
			const double dy = double(y) - cy;
			const double r = std::sqrt( dx*dx + dy*dy );
			if( r >= rMin && r <= rMax ) {
				const RISEColor& p = cap.pixels[y * cap.width + x];
				sum[0] += p.base.r;
				sum[1] += p.base.g;
				sum[2] += p.base.b;
				count++;
			}
		}
	}
	if( count > 0 ) {
		s.mean[0] = sum[0] / double(count);
		s.mean[1] = sum[1] / double(count);
		s.mean[2] = sum[2] / double(count);
	}
	s.pixelCount = count;
	return s;
}

static RegionStats FullImageMean( const CapturingRasterizerOutput& cap )
{
	RegionStats s{};
	double sum[3] = {0, 0, 0};
	for( const RISEColor& p : cap.pixels ) {
		sum[0] += p.base.r;
		sum[1] += p.base.g;
		sum[2] += p.base.b;
	}
	const unsigned int n = static_cast<unsigned int>( cap.pixels.size() );
	if( n > 0 ) {
		s.mean[0] = sum[0] / double(n);
		s.mean[1] = sum[1] / double(n);
		s.mean[2] = sum[2] / double(n);
	}
	s.pixelCount = n;
	return s;
}

static bool ChannelsAgree(
	const double a[3], const double b[3],
	double relTol, double absFloor )
{
	for( int c = 0; c < 3; c++ ) {
		const double denom = std::fmax( std::fabs(a[c]), absFloor );
		if( std::fabs(a[c] - b[c]) / denom > relTol ) return false;
	}
	return true;
}

//////////////////////////////////////////////////////////////////////
// Scene IO plumbing.
//////////////////////////////////////////////////////////////////////
static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/ior_seed_regression_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static CapturingRasterizerOutput* RenderAndCapture( const char* scenePath )
{
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return nullptr;
	}

	if( !pJob->LoadAsciiScene( scenePath ) ) {
		safe_release( pJob );
		return nullptr;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pCap->addref();
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	safe_release( pJob );

	if( !bRendered ) {
		safe_release( pCap );
		return nullptr;
	}
	return pCap;
}

//////////////////////////////////////////////////////////////////////
// PART 1 — Direct unit tests of SeedFromPoint with a minimal scene.
//
// We load a scene with exactly ONE glass sphere at the origin and
// call SeedFromPoint with four hand-picked positions, asserting the
// resulting stack contents.  This pins the parity logic at the
// function level — independent of any integrator's use of it.
//////////////////////////////////////////////////////////////////////
static const char* kUnitSceneText =
	"RISE ASCII SCENE 6\n"
	"\n"
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 1\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_ior\n"
	"\tcolor 1.5 1.5 1.5\n"
	"\tcolorspace RISERGB\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_refr\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance pnt_refr\n"
	"\tior pnt_ior\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sphere_geom\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_sphere\n"
	"\tgeometry sphere_geom\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n";

static void RunUnitTests()
{
	std::cout << "PART 1: Direct SeedFromPoint unit tests" << std::endl;

	const std::string scenePath = WriteSceneToTempFile( kUnitSceneText, "unit" );
	if( scenePath.empty() ) {
		Check( false, "unit scene write" );
		return;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, "unit job create" );
		std::remove( scenePath.c_str() );
		return;
	}
	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) {
		Check( false, "unit scene load" );
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return;
	}

	IScenePriv* pScene = pJob->GetScene();
	if( !pScene ) {
		Check( false, "unit scene get" );
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return;
	}

	// Case (A): outside sphere, +Z probe traverses sphere.  This is
	// the bug case.  Pre-fix stack contained sphere; post-fix empty.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0, 0, -2), *pScene );
		Check( !stack.containsCurrent() && stack.top() == 1.0,
			"(A) probe-traverses: stack stays empty" );
	}

	// Case (B): outside sphere, +Z probe goes away from sphere.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0, 0, 2), *pScene );
		Check( !stack.containsCurrent() && stack.top() == 1.0,
			"(B) probe-misses-outward: stack stays empty" );
	}

	// Case (C): inside sphere.  +Z probe exits once (top of sphere).
	// Original design intent: stack should contain the sphere.
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0, 0, 0), *pScene );
		Check( stack.containsCurrent() && std::fabs(stack.top() - 1.5) < 1e-6,
			"(C) seed-inside: stack contains sphere with ior=1.5" );
	}

	// Case (D): outside sphere, +Z probe goes parallel (misses).
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(2, 0, 0), *pScene );
		Check( !stack.containsCurrent() && stack.top() == 1.0,
			"(D) probe-parallel: stack stays empty" );
	}

	// Case (E): typical area-light location.  An area emitter on
	// the -Z side of a glass sphere — its +Z probe traverses the
	// sphere.  Anchors the same bug pattern as (A) to the light-
	// side call sites (BDPTIntegrator.cpp:1383, :4731).
	{
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3(0, 0, -0.9), *pScene );
		Check( !stack.containsCurrent() && stack.top() == 1.0,
			"(E) light-position-probe-traverses: stack stays empty" );
	}

	safe_release( pJob );
	std::remove( scenePath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// PART 2 — Render-based: camera in front of a glass sphere.
//
// Layout:
//   - Camera at (0, 0, -2)  looking +Z.  CAMERA +Z probe traverses
//     the sphere — the trigger geometry for the camera-side bug.
//   - Glass sphere at origin, radius 0.5.
//   - Lambertian back wall at z = +2, floor at y = -1.
//   - Area light at y = +1.5 facing -y.  The light is OUTSIDE every
//     dielectric and its +Z probe goes straight up — it does not
//     trigger the light-side bug, so this scene isolates the camera
//     code path.
//////////////////////////////////////////////////////////////////////
static const char* kCameraSceneGeometry =
	"film\n"
	"{\n"
	"\twidth 64\n"
	"\theight 64\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 -2\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.7 0.7 0.7\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_ior\n"
	"\tcolor 1.5 1.5 1.5\n"
	"\tcolorspace RISERGB\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_light\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_refr\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_light\n"
	"\texitance pnt_light\n"
	"\tscale 40.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance pnt_refr\n"
	"\tior pnt_ior\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sphere_geom\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname floor_geom\n"
	"\tpta -3 -1 -3\n"
	"\tptb 3 -1 -3\n"
	"\tptc 3 -1 3\n"
	"\tptd -3 -1 3\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname wall_geom\n"
	"\tpta -3 -1 2\n"
	"\tptb 3 -1 2\n"
	"\tptc 3 3 2\n"
	"\tptd -3 3 2\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname light_geom\n"
	"\tpta -0.5 1.5 -0.5\n"
	"\tptb 0.5 1.5 -0.5\n"
	"\tptc 0.5 1.5 0.5\n"
	"\tptd -0.5 1.5 0.5\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_sphere\n"
	"\tgeometry sphere_geom\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_floor\n"
	"\tgeometry floor_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_wall\n"
	"\tgeometry wall_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_light\n"
	"\tgeometry light_geom\n"
	"\tmaterial mat_light\n"
	"}\n";

static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 64\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/ior_seed_regression_pt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerBDPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 6\n"
	"\tmax_light_depth 6\n"
	"\tsamples 64\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/ior_seed_regression_bdpt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static void RunCameraSideRenderTest()
{
	std::cout << "PART 2: Camera-side render test (probe traverses sphere)" << std::endl;

	std::string ptScene   = std::string("RISE ASCII SCENE 6\n") + kRasterizerPT   + kCameraSceneGeometry;
	std::string bdptScene = std::string("RISE ASCII SCENE 6\n") + kRasterizerBDPT + kCameraSceneGeometry;

	const std::string ptPath   = WriteSceneToTempFile( ptScene.c_str(),   "cam_pt"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "cam_bdpt" );

	CapturingRasterizerOutput* pPt   = RenderAndCapture( ptPath.c_str() );
	CapturingRasterizerOutput* pBdpt = RenderAndCapture( bdptPath.c_str() );

	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( pPt   != nullptr, "camera test: PT render succeeded" );
	Check( pBdpt != nullptr, "camera test: BDPT render succeeded" );
	if( !pPt || !pBdpt ) {
		safe_release( pPt );
		safe_release( pBdpt );
		return;
	}

	// Sphere image-space centroid ≈ (32, 32) at fov=30°, dist=2,
	// radius=0.5 → silhouette radius ≈ 24 pixels.
	const double cx = 32.0, cy = 32.0;
	const RegionStats ptAnnulus   = RegionMean( *pPt,   cx, cy, 18.0, 24.0 );
	const RegionStats bdptAnnulus = RegionMean( *pBdpt, cx, cy, 18.0, 24.0 );
	const RegionStats ptCenter    = RegionMean( *pPt,   cx, cy,  0.0,  8.0 );
	const RegionStats bdptCenter  = RegionMean( *pBdpt, cx, cy,  0.0,  8.0 );
	const RegionStats ptWall      = RegionMean( *pPt,   cx, cy, 28.0, 1000.0 );
	const RegionStats bdptWall    = RegionMean( *pBdpt, cx, cy, 28.0, 1000.0 );

	std::cout << "  Annulus: PT=("   << ptAnnulus.mean[0]   << "," << ptAnnulus.mean[1]   << "," << ptAnnulus.mean[2]   << ")"
	          << " BDPT=("           << bdptAnnulus.mean[0] << "," << bdptAnnulus.mean[1] << "," << bdptAnnulus.mean[2] << ")" << std::endl;
	std::cout << "  Center:  PT=("   << ptCenter.mean[0]    << "," << ptCenter.mean[1]    << "," << ptCenter.mean[2]    << ")"
	          << " BDPT=("           << bdptCenter.mean[0]  << "," << bdptCenter.mean[1]  << "," << bdptCenter.mean[2]  << ")" << std::endl;
	std::cout << "  Wall:    PT=("   << ptWall.mean[0]      << "," << ptWall.mean[1]      << "," << ptWall.mean[2]      << ")"
	          << " BDPT=("           << bdptWall.mean[0]    << "," << bdptWall.mean[1]    << "," << bdptWall.mean[2]    << ")" << std::endl;

	const double absFloor = 1e-6;
	Check( ChannelsAgree( ptWall.mean,    bdptWall.mean,    0.05, absFloor ),
		"camera test: wall PT == BDPT (no sphere influence)" );
	Check( ChannelsAgree( ptCenter.mean,  bdptCenter.mean,  0.10, absFloor ),
		"camera test: sphere center PT == BDPT" );
	Check( ChannelsAgree( ptAnnulus.mean, bdptAnnulus.mean, 0.25, absFloor ),
		"camera test: sphere annulus PT == BDPT (parity-fix bites here)" );

	safe_release( pPt );
	safe_release( pBdpt );
}

//////////////////////////////////////////////////////////////////////
// PART 3 — Render-based: LIGHT in front of a high-IOR glass sphere.
//
// SCOPE: smoke test for the light-side seeding code path.  The
// LIGHT-SIDE seeding bug is the same root cause as the camera-side
// bug — same `SeedFromPoint` function, same +Z probe behaviour —
// so the DEFINITIVE diagnostic is PART 1 case (A) which exercises
// the function directly.  This render test is an end-to-end smoke
// check that the light-side seeding doesn't crash on a glass-light
// scene and converges to something close to PT.
//
// We deliberately do NOT assert a tight tolerance here: BDPT's
// light-subpath strategies that the light-side bug breaks (the
// s>=2 light-tracing strategies through the sphere) are typically
// a small fraction of the total contribution in scenes where s=1
// NEE works — and a render test that doesn't fail on the bug at
// realistic spp would be misleading.  Keep this test broad: it
// guards against gross regressions (e.g. light-subpath generation
// crashing, returning NaN, or returning 0 for the whole image) and
// leaves the precise parity check to PART 1.
//
// Layout:
//   - Camera at (1.6, 0, 0) looking at origin.  Camera +Z probe
//     goes parallel to the sphere axis, lateral distance 1.6 >
//     sphere radius 0.5 — camera-side seeding is a no-op.
//   - Glass sphere at origin, radius 0.5, IOR = 2.0.
//   - Area light at (0, 0, -0.7) facing +Z — every light sample's
//     +Z probe goes through the sphere, exercising the light-side
//     seeding code.
//   - Lambertian receiver wall, floor, back wall.
//////////////////////////////////////////////////////////////////////
static const char* kLightSceneGeometry =
	"film\n"
	"{\n"
	"\twidth 48\n"
	"\theight 48\n"
	"}\n"
	"\n"
	// Camera at (1.6, 0, 0) looking -X.  Camera's +Z probe goes
	// from (1.6, 0, 0) in +Z direction — sphere at origin has
	// radius 0.5, so the probe's lateral distance (1.6) exceeds
	// the sphere radius and the probe never traverses anything.
	"pinhole_camera\n"
	"{\n"
	"\tlocation 1.6 0 0\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 60.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.7 0.7 0.7\n"
	"}\n"
	"\n"
	// High IOR magnifies wrong-direction refraction at glancing angles.
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_ior\n"
	"\tcolor 2.0 2.0 2.0\n"
	"\tcolorspace RISERGB\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_light\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_refr\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"lambertian_luminaire_material\n"
	"{\n"
	"\tname mat_light\n"
	"\texitance pnt_light\n"
	"\tscale 100.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"perfectrefractor_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\trefractance pnt_refr\n"
	"\tior pnt_ior\n"
	"}\n"
	"\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sphere_geom\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname floor_geom\n"
	"\tpta -1.6 -1 -1.5\n"
	"\tptb 1.6 -1 -1.5\n"
	"\tptc 1.6 -1 1.5\n"
	"\tptd -1.6 -1 1.5\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname wall_geom\n"
	"\tpta -1.6 -1 1.0\n"
	"\tptb 1.6 -1 1.0\n"
	"\tptc 1.6 1.5 1.0\n"
	"\tptd -1.6 1.5 1.0\n"
	"}\n"
	"\n"
	// Back wall at x = -1.6: occludes direct line of sight from
	// the camera (at x = +1.6) to the light area (centered at
	// x = 0, behind the sphere from camera POV).  This forces all
	// of the visible image to be illuminated via paths through
	// the glass sphere — making the light-side seeding bug the
	// dominant signal.
	"clippedplane_geometry\n"
	"{\n"
	"\tname back_wall_geom\n"
	"\tpta -1.6 -1 -1.5\n"
	"\tptb -1.6 -1 1.0\n"
	"\tptc -1.6 1.5 1.0\n"
	"\tptd -1.6 1.5 -1.5\n"
	"}\n"
	"\n"
	// Area light at (0, 0, -0.7) facing +Z.  Close to sphere
	// (distance 0.2 from sphere_front at z=-0.5).  Probe +Z from
	// any light sample (lateral offset ≤ 0.3 < sphere radius 0.5)
	// traverses the sphere → light-side seeding bug fires every
	// time.  Cosine emission cone is wide at this proximity so
	// many emissions hit the sphere at glancing angles, where the
	// wrong-direction refraction sends rays radically off course.
	"clippedplane_geometry\n"
	"{\n"
	"\tname light_geom\n"
	"\tpta -0.3 -0.3 -0.7\n"
	"\tptb 0.3 -0.3 -0.7\n"
	"\tptc 0.3 0.3 -0.7\n"
	"\tptd -0.3 0.3 -0.7\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_sphere\n"
	"\tgeometry sphere_geom\n"
	"\tposition 0 0 0\n"
	"\tmaterial mat_glass\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_floor\n"
	"\tgeometry floor_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_wall\n"
	"\tgeometry wall_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_back_wall\n"
	"\tgeometry back_wall_geom\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_light\n"
	"\tgeometry light_geom\n"
	"\tmaterial mat_light\n"
	"}\n";

static const char* kRasterizerPT_LightTest =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 128\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/ior_seed_regression_light_pt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerBDPT_LightTest =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 6\n"
	"\tmax_light_depth 6\n"
	"\tsamples 128\n"
	"\tpixel_filter box\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/ior_seed_regression_light_bdpt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static void RunLightSideRenderTest()
{
	std::cout << "PART 3: Light-side render test (light probe traverses sphere)" << std::endl;

	std::string ptScene   = std::string("RISE ASCII SCENE 6\n") + kRasterizerPT_LightTest   + kLightSceneGeometry;
	std::string bdptScene = std::string("RISE ASCII SCENE 6\n") + kRasterizerBDPT_LightTest + kLightSceneGeometry;

	const std::string ptPath   = WriteSceneToTempFile( ptScene.c_str(),   "light_pt"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "light_bdpt" );

	CapturingRasterizerOutput* pPt   = RenderAndCapture( ptPath.c_str() );
	CapturingRasterizerOutput* pBdpt = RenderAndCapture( bdptPath.c_str() );

	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );

	Check( pPt   != nullptr, "light test: PT render succeeded" );
	Check( pBdpt != nullptr, "light test: BDPT render succeeded" );
	if( !pPt || !pBdpt ) {
		safe_release( pPt );
		safe_release( pBdpt );
		return;
	}

	const RegionStats ptFull   = FullImageMean( *pPt );
	const RegionStats bdptFull = FullImageMean( *pBdpt );

	std::cout << "  Full image:" << std::endl;
	std::cout << "    PT   = (" << ptFull.mean[0]   << ", " << ptFull.mean[1]   << ", " << ptFull.mean[2]   << ")" << std::endl;
	std::cout << "    BDPT = (" << bdptFull.mean[0] << ", " << bdptFull.mean[1] << ", " << bdptFull.mean[2] << ")" << std::endl;

	// Sanity floor: render must produce a non-trivially bright image.
	// Catches gross light-subpath regressions (NaN, zero-everything).
	Check( ptFull.mean[0]   > 1e-3, "light test: PT  image is non-trivially bright" );
	Check( bdptFull.mean[0] > 1e-3, "light test: BDPT image is non-trivially bright" );

	// Wide tolerance check: catches gross divergence between PT and
	// BDPT but is intentionally NOT a tight bug-detector for the
	// IORStackSeeding parity bug (see scope comment above).
	const double absFloor = 1e-6;
	Check( ChannelsAgree( ptFull.mean, bdptFull.mean, 0.25, absFloor ),
		"light test: PT and BDPT means within 25% (gross regression guard)" );

	safe_release( pPt );
	safe_release( pBdpt );
}

//////////////////////////////////////////////////////////////////////
int main()
{
	std::cout << "IORStackSeedingRegressionTest: SeedFromPoint per-object parity guard" << std::endl;

	RunUnitTests();
	RunCameraSideRenderTest();
	RunLightSideRenderTest();

	std::cout << "\nResults: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
