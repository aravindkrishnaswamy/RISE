//////////////////////////////////////////////////////////////////////
//
//  SelfHitFloorCoincidentOriginTest.cpp - DL-120.
//
//  WHAT THIS FILE SETTLES.  DL-120 was filed as a ~118x RENDER
//  REGRESSION: the agent-eval replay fixture `constant_materials_polish`
//  renders at meanLuma 0.001549 today against the 0.182353 measured at
//  the 2026-08-20 commit its own render checkpoint was graded against,
//  and a `git bisect run` pinned the change to `d01a320a`
//  ("fix(intersection): scale-relative self-hit floor in the
//  bilinear-patch solver", 2026-09-03).
//
//  The bisect is right about WHICH commit moved the number and wrong
//  about which side of it is correct.  The scenario's scene puts the
//  pinhole camera at (0, 0.3, 6.5) and its ONLY light -- a
//  `clippedplane_geometry` + `lambertian_luminaire_material` quad --
//  in the plane z = 6.5, spanning x, y in [-1.4, 1.4].  The camera is
//  therefore EXACTLY IN THE EMITTER'S SURFACE, and inside its extent.
//  Every camera ray leaves that plane immediately (all of them travel
//  with dir.z < 0), so the emitter's only root along a camera ray is
//  the mathematically ZERO one at the ray's own origin, and a pinhole
//  lying in an infinitely thin emitter sees it edge-on -- zero
//  projected area, no contribution.  Before `d01a320a` the solver's
//  acceptance test was the bare `hit.dRange > 0`, and `computet`'s
//  recovered `t` for that zero root is a pure rounding residue of
//  `(srfpos.z - origin.z) / dir.z` with `srfpos.z` the bilinear
//  evaluation of four corners all at z = 6.5: about 6.3 % of camera
//  samples landed on the POSITIVE side of that coin flip and
//  registered a full-radiance emitter hit at t ~ 1e-15, filling the
//  frame.  0.182353 is 6.3 % of the luminaire's own radiance
//  (9.0 / pi = 2.8648) plus the scene's real, much darker lighting;
//  it is a self-intersection artifact, and a coin flip in the last
//  bit of a sum at that -- not a number any other compiler, libm or
//  platform is obliged to reproduce.
//
//  So `d01a320a` did not break this scene; it FIXED it, and the
//  scenario's committed render band was calibrated on the artifact.
//  The first three cases pin that conclusion from three independent
//  directions; the fourth guards the floor's scale-relativity across
//  the producers that share it.
//
//    [1] COPLANARITY IS THE WHOLE EFFECT.  Render the byte-identical
//        committed scene, then re-render it with the camera nudged
//        0.001 off the emitter's plane (1/2800 of the quad's own
//        width -- far too small to change any real lighting) and
//        again with it nudged 0.3.  All three must agree to within MC
//        noise.  Pre-`d01a320a` the as-authored render is ~100x the
//        other two: that is the red-proof, and it is why this case
//        would FAIL if the floor were ever reverted.
//
//    [2] THE LUMINAIRE ITSELF IS HEALTHY, closed-form.  Put the camera
//        BEHIND the emitter plane looking at its back face (the quad
//        is `doublesided` by descriptor default) so the frame is
//        nothing but emitter, and check the measured radiance against
//        `exitance * scale / pi` exactly.  This separates "the light
//        is broken" from "the light is edge-on", and it is unaffected
//        by the floor.
//
//    [3] THE debt-21 SELF-HIT IS STILL REJECTED.  A direct solver
//        probe: a ray whose origin lies ON the patch, fanned toward
//        tangential, must never report a hit at its own origin, at
//        every world scale.  This is the property `d01a320a` bought
//        and that any future change here must keep.
//
//    [4] THE FLOOR IS SCALE-RELATIVE, NOT ABSOLUTE.  `tMin` is
//        `NEARZERO * (1 + coordScale)` -- a CONSTANT count of ulps
//        (NEARZERO = 1e-12 is ~4500 x DBL_EPSILON), so a genuine root
//        further off the surface than that relative band must be
//        accepted at ANY world position, and the adversarial
//        "origin ~1e-3 from the surface" configuration must hit for
//        every producer carrying this floor (bilinear patch, sphere,
//        infinite plane, triangle).  What this case does NOT assert is
//        that an ARBITRARILY small root survives: the floor exists to
//        swallow the root of a point `Object::IntersectRay` published
//        `SURFACE_INTERSEC_ERROR` (1e-12) off the surface, whose own
//        root is `1e-12 * |cos in| / |cos out|` and is therefore a
//        genuine root of that offset origin -- "the floor is below
//        every true root" is NOT an achievable invariant here and is
//        not claimed (see docs/DL120_SELFHIT_FLOOR_REGRESSION.md
//        section 5).
//
//  SEEDING.  A binary that calls `RISE_CreateJobPriv` directly never
//  runs `commandconsole.cpp`'s `srand`, so `std::srand(seedBase + n)`
//  is set per render here for the reason FabricRenderTest's header
//  spells out; argv[1] overrides the base for an independent sample.
//  Every scene sets `oidn_denoise false` and `pixel_filter box`.
//
//  SELFHIT_TEST_FILTER (optional): substring-match the case keywords
//  `coplanar`, `luminaire`, `selfhit`, `scale`.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Intersection/RayPrimitiveIntersections.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static const unsigned int kDefaultSeedBase = 7100u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  pass: " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Render harness (shape shared with FabricRenderTest / HairRenderTest).
// Deliberately does NOT override OutputDenoisedImage; every scene below
// disables OIDN instead, so what is measured is the integrator.
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

struct ImageStats
{
	double mean[3];
	double meanLuma;	//!< Rec.709 weights, matching AgentEvalRunner's render checkpoint
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}
	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] = sum[c] / double(cap.pixels.size());
	s.meanLuma = 0.2126 * s.mean[0] + 0.7152 * s.mean[1] + 0.0722 * s.mean[2];
	s.valid = true;
	return s;
}

static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
{
	ImageStats result{};

	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/selfhit_floor_test_%s_%d.RISEscene", tag, static_cast<int>(::getpid()) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return result;
		ofs << sceneText;
	}

	std::srand( g_seedBase + g_renderIndex );
	g_renderIndex++;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path );
		return result;
	}
	if( !pJob->LoadAsciiSceneViaCst( path ) ) {
		std::remove( path );
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	if( pJob->Rasterize() ) {
		result = ComputeStats( *pCap );
	}

	std::remove( path );
	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// The fixture scene.
//
// BYTE-FOR-BYTE the `scene.inline` text committed in
// evals/scenarios/constant_materials_polish.json, with ONE deliberate
// difference: `samples` is the render checkpoint's pinned 256 rather
// than the scene-authored 8.  AgentEvalRunner applies that pin through
// IRasterizer::SetSampleCountOverride; writing it into the scene text
// is the same measurement without the agent harness.  `film` already
// carries the checkpoint's pinned 32x24.
//
// The camera's `location` line is passed in so the coplanarity sweep
// can substitute it without a fragile search-and-replace.
//////////////////////////////////////////////////////////////////////
static const char* kCameraLine = "\tlocation 0 0.3 6.5\n";

static std::string FixtureScene( const char* cameraLine )
{
	std::string s =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"\n"
		"pathtracing_pel_rasterizer\n"
		"{\n"
		"\tsamples 256\n"
		"\tpixel_filter box\n"
		"\toidn_denoise false\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 32\n"
		"\theight 24\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n";
	s += cameraLine;
	s +=
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 45.0\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_dark\n"
		"\tcolor 0.12 0.10 0.09\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_spec\n"
		"\tcolor 0.6 0.6 0.6\n"
		"}\n"
		"\n"
		"ggx_material\n"
		"{\n"
		"\tname mat_a\n"
		"\trd pnt_dark\n"
		"\trs pnt_spec\n"
		"\talphax 0.3\n"
		"\talphay 0.3\n"
		"\tior 1.5\n"
		"\textinction 0.0\n"
		"}\n"
		"\n"
		"ggx_material\n"
		"{\n"
		"\tname mat_b\n"
		"\trd pnt_dark\n"
		"\trs pnt_spec\n"
		"\talphax 0.25\n"
		"\talphay 0.25\n"
		"\tior 1.5\n"
		"\textinction 0.0\n"
		"}\n"
		"\n"
		"ggx_material\n"
		"{\n"
		"\tname mat_c\n"
		"\trd pnt_dark\n"
		"\trs pnt_spec\n"
		"\talphax 0.4\n"
		"\talphay 0.4\n"
		"\tior 1.5\n"
		"\textinction 0.0\n"
		"}\n"
		"\n"
		"sphere_geometry\n"
		"{\n"
		"\tname sph\n"
		"\tradius 0.7\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_a\n"
		"\tgeometry sph\n"
		"\tmaterial mat_a\n"
		"\tposition -1.8 0 0\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_b\n"
		"\tgeometry sph\n"
		"\tmaterial mat_b\n"
		"\tposition 0 0 0\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_c\n"
		"\tgeometry sph\n"
		"\tmaterial mat_c\n"
		"\tposition 1.8 0 0\n"
		"}\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_emit\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname mat_emit\n"
		"\texitance pnt_emit\n"
		"\tscale 9.0\n"
		"\tmaterial none\n"
		"}\n"
		"\n"
		"clippedplane_geometry\n"
		"{\n"
		"\tname quad_emit\n"
		"\tpta -1.4 1.4 6.5\n"
		"\tptb 1.4 1.4 6.5\n"
		"\tptc 1.4 -1.4 6.5\n"
		"\tptd -1.4 -1.4 6.5\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname obj_emit\n"
		"\tgeometry quad_emit\n"
		"\tmaterial mat_emit\n"
		"}\n";
	return s;
}

//////////////////////////////////////////////////////////////////////
// [1] Coplanarity is the whole effect.
//////////////////////////////////////////////////////////////////////
static void TestCoplanarCameraSeesNoEmitter()
{
	std::cout << "\n[1] camera coplanar with the emitter plane (DL-120)" << std::endl;

	const ImageStats onPlane = RenderAndComputeStats( FixtureScene( kCameraLine ), "onplane" );
	const ImageStats nudged  = RenderAndComputeStats( FixtureScene( "\tlocation 0 0.3 6.499\n" ), "nudge" );
	const ImageStats moved   = RenderAndComputeStats( FixtureScene( "\tlocation 0 0.3 6.2\n" ), "moved" );

	Check( onPlane.valid && nudged.valid && moved.valid, "all three camera positions rendered" );
	if( !onPlane.valid || !nudged.valid || !moved.valid ) return;

	std::printf( "    camera z = 6.5   (IN the emitter plane)  meanLuma = %.6f\n", onPlane.meanLuma );
	std::printf( "    camera z = 6.499 (0.001 off the plane)   meanLuma = %.6f\n", nudged.meanLuma );
	std::printf( "    camera z = 6.2   (0.3 off the plane)     meanLuma = %.6f\n", moved.meanLuma );

	// THE red-proof.  A 0.001 nudge is 1/2800 of the quad's own width and
	// changes the emitter's subtended solid angle at the spheres by well
	// under 0.1 %, so if the coplanar frame carried a LEGITIMATE emitter
	// contribution the nudge could not remove it.  Pre-d01a320a the two
	// differ by ~100x.  The 8 % band is a generous multiple of the MC
	// spread of a 32x24 / 256 spp pair, widened to absorb the genuine but
	// tiny framing change the nudge does make.
	const double relNudge = std::fabs( onPlane.meanLuma - nudged.meanLuma ) /
		( 0.5 * ( onPlane.meanLuma + nudged.meanLuma ) );
	std::printf( "    |onPlane - nudged| / mean = %.4f\n", relNudge );
	Check( relNudge <= 0.08,
		"a 0.001 camera nudge off the emitter plane does NOT change the frame "
		"(the coplanar frame carries no emitter contribution)" );

	// The 0.3 move is a real framing change, so it gets a looser band; it
	// is here to show the agreement is not an accident of the tiny nudge.
	const double relMoved = std::fabs( onPlane.meanLuma - moved.meanLuma ) /
		( 0.5 * ( onPlane.meanLuma + moved.meanLuma ) );
	std::printf( "    |onPlane - moved|  / mean = %.4f\n", relMoved );
	Check( relMoved <= 0.25,
		"moving the camera 0.3 off the emitter plane changes the frame only by its framing" );

	// An absolute anchor so a future change that moves BOTH sides together
	// still trips.  0.001549 measured on master e290fc64 at seed base 7100;
	// the +-40 % band is far wider than the MC spread and still 100x
	// tighter than the artifact it replaces.
	Check( std::fabs( onPlane.meanLuma - 0.001549 ) <= 0.40 * 0.001549,
		"as-authored fixture meanLuma is the scene's genuine lit level (0.001549 +- 40%)" );
}

//////////////////////////////////////////////////////////////////////
// [2] The luminaire itself, closed-form.
//////////////////////////////////////////////////////////////////////
static void TestLuminaireRadianceClosedForm()
{
	std::cout << "\n[2] the luminaire's own radiance, closed-form" << std::endl;

	// Camera at z = 7.5 looks back at the quad's far face from 1.0 away;
	// the quad is 2.8 wide and `doublesided` (the chunk descriptor's
	// default), and at fov 45 the frame at that distance is 0.83 x 1.10 --
	// entirely inside the quad, so every pixel is emitter and nothing
	// else, and the mean IS the emitted radiance.
	const ImageStats behind = RenderAndComputeStats( FixtureScene( "\tlocation 0 0.3 7.5\n" ), "behind" );
	Check( behind.valid, "behind-the-emitter frame rendered" );
	if( !behind.valid ) return;

	// Lambertian luminaire: L = exitance * scale / pi, with exitance the
	// white `pnt_emit` painter (1.0) and scale 9.0.
	const double kExpected = 9.0 / 3.14159265358979323846;
	std::printf( "    measured L = %.6f   expected exitance*scale/pi = %.6f   ratio = %.6f\n",
		behind.meanLuma, kExpected, behind.meanLuma / kExpected );

	Check( std::fabs( behind.meanLuma / kExpected - 1.0 ) <= 0.01,
		"emitter radiance matches exitance*scale/pi within 1% (the light is healthy; "
		"the as-authored frame is dark because the light is EDGE-ON, not because it is broken)" );
}

//////////////////////////////////////////////////////////////////////
// [3] The debt-21 self-hit must stay rejected.
//////////////////////////////////////////////////////////////////////
struct PatchProbe
{
	bool   bHit;
	Scalar dRange;
};

static PatchProbe ProbePatch(
	const Point3 (&corners)[4],
	const Point3& origin,
	const Vector3& dir )
{
	BilinearPatch patch;
	for( int i = 0; i < 4; i++ ) patch.pts[i] = corners[i];

	Ray ray;
	ray.origin = origin;
	ray.SetDir( Vector3Ops::Normalize( dir ) );

	BILINEAR_HIT h;
	RayBilinearPatchIntersection( ray, h, patch );

	PatchProbe p;
	p.bHit = h.bHit;
	p.dRange = h.dRange;
	return p;
}

//! RISE-bilinear corner order is pts[0]->(0,0), pts[1]->(0,1),
//! pts[2]->(1,0), pts[3]->(1,1) -- NOT the row-major order
//! ClippedPlaneGeometry authors; see its `ToBilinearPatch`.
static void UnitQuadAtZ( Scalar Z, Point3 (&corners)[4] )
{
	corners[0] = Point3( -1, -1, Z );
	corners[1] = Point3( -1,  1, Z );
	corners[2] = Point3(  1, -1, Z );
	corners[3] = Point3(  1,  1, Z );
}

static void TestDebt21SelfHitStillRejected()
{
	std::cout << "\n[3] the debt-21 self-hit is still rejected" << std::endl;

	// The debt-21 configuration: a ray whose origin lies exactly ON the
	// patch (a shading point that just came from a hit here) leaving
	// toward a light behind it.  Its only root is the mathematically-zero
	// one at its own origin; `computet` recovers it with coordinate-scale
	// round-off, so it must be floored away -- at every world scale and,
	// critically, for outgoing directions fanned toward TANGENTIAL, which
	// is where the `1e-12 * |cos in| / |cos out|` self root grows.
	int spurious = 0, total = 0;
	const Scalar zs[] = { 1.0, 1e3, 1e6, 1e9 };
	for( size_t zi = 0; zi < sizeof(zs)/sizeof(zs[0]); zi++ ) {
		const Scalar Z = zs[zi];
		Point3 corners[4];
		UnitQuadAtZ( Z, corners );

		for( int k = 0; k < 64; k++ ) {
			// A point on the patch, at coordinates that do not round exactly.
			const Scalar u = -1.0 + 2.0 * ( Scalar(k % 8) + 0.3137 ) / 8.0;
			const Scalar v = -1.0 + 2.0 * ( Scalar(k / 8) + 0.7913 ) / 8.0;
			const Point3 org( u, v, Z );

			for( int d = 0; d < 6; d++ ) {
				const Scalar tilt = std::pow( 10.0, -Scalar(d) );	// 1 .. 1e-5
				const Vector3 dir( 0.37, 0.61, -tilt );

				const PatchProbe p = ProbePatch( corners, org, dir );
				total++;
				// "Spurious" = a hit so close to the origin that it can only
				// be the ray's own starting point.  1e-6 is far above the
				// floor and far below any real geometry in these rigs.
				if( p.bHit && p.dRange < 1e-6 ) {
					spurious++;
					if( spurious <= 5 ) {
						std::printf( "    SPURIOUS self-hit: Z=%g tilt=%g t=%g\n",
							(double)Z, (double)tilt, (double)p.dRange );
					}
				}
			}
		}
	}
	std::printf( "    spurious self-hits: %d / %d\n", spurious, total );
	Check( spurious == 0,
		"a ray leaving the patch never re-hits it at its own origin (debt-21 holds)" );
}

//////////////////////////////////////////////////////////////////////
// [4] The floor is scale-relative, not absolute -- across producers.
//
// `tMin = NEARZERO * (1 + coordScale)` is a CONSTANT count of ulps
// (NEARZERO = 1e-12 ~ 4500 x DBL_EPSILON), so the assertion is
// RELATIVE: a genuine root further than `kRelGapFloor` (1e-9, three
// decades of headroom over the floor's own 1e-12 relative band) off the
// surface must be accepted wherever in world space the surface sits.  A
// regression that made any of these floors ABSOLUTE-large, or
// super-linear in the coordinate scale, fails here.
//////////////////////////////////////////////////////////////////////
static const Scalar kRelGapFloor = 1e-9;

static void TestFloorIsScaleRelative()
{
	std::cout << "\n[4] the self-hit floor is scale-relative across producers" << std::endl;

	// Includes the adversarial "origin within ~1e-3 of the surface"
	// configuration the audit calls for (C = 1, gap = 1e-3 down to 1e-9)
	// and sweeps the surface's world position over nine decades.
	const Scalar cs[] = { 1.0, 1e3, 1e6, 1e9 };
	const Scalar relGaps[] = { 1e-3, 1e-5, 1e-7, kRelGapFloor };

	// --- bilinear patch (the DL-120 producer) -------------------------
	{
		int hit = 0, total = 0;
		for( size_t ci = 0; ci < 4; ci++ ) for( size_t gi = 0; gi < 4; gi++ ) {
			const Scalar C = cs[ci];
			const Scalar gap = relGaps[gi] * C;
			Point3 corners[4];
			UnitQuadAtZ( C, corners );
			const PatchProbe p = ProbePatch(
				corners, Point3( 0.25, -0.25, C - gap ), Vector3( 0, 0, 1 ) );
			total++;
			if( p.bHit && std::fabs( p.dRange - gap ) <= 1e-6 * gap ) hit++;
			else std::printf( "    patch MISS: C=%g gap=%g -> bHit=%d t=%g\n",
				(double)C, (double)gap, (int)p.bHit, (double)p.dRange );
		}
		std::printf( "    bilinear patch: %d / %d\n", hit, total );
		Check( hit == total,
			"bilinear patch: a root above the relative floor is accepted at any world position" );
	}

	// --- sphere -------------------------------------------------------
	{
		int hit = 0, total = 0;
		for( size_t ci = 0; ci < 4; ci++ ) for( size_t gi = 0; gi < 4; gi++ ) {
			const Scalar C = cs[ci];
			const Scalar gap = relGaps[gi] * C;
			// Sphere of radius C centred at (0, 0, C), so its near pole is
			// the world origin; the ray starts `gap` short of that pole
			// heading in, and the true root is exactly `gap`.
			Ray ray; ray.origin = Point3( 0, 0, -gap ); ray.SetDir( Vector3( 0, 0, 1 ) );
			HIT h;
			RaySphereIntersection( ray, h, C, Point3( 0, 0, C ) );
			total++;
			if( h.bHit && std::fabs( h.dRange - gap ) <= 1e-6 * gap ) hit++;
			else std::printf( "    sphere MISS: C=%g gap=%g -> bHit=%d t=%g\n",
				(double)C, (double)gap, (int)h.bHit, (double)h.dRange );
		}
		std::printf( "    sphere: %d / %d\n", hit, total );
		Check( hit == total,
			"sphere: a root above the relative floor is accepted at any world scale" );
	}

	// --- infinite plane ----------------------------------------------
	{
		// RayPlaneIntersection solves for the plane through the ORIGIN with
		// the given normal, so the world position that drives its floor is
		// carried by the ray origin's own magnitude -- which is exactly
		// what its `coordScale` reads.
		int hit = 0, total = 0;
		for( size_t ci = 0; ci < 4; ci++ ) for( size_t gi = 0; gi < 4; gi++ ) {
			const Scalar C = cs[ci];
			const Scalar gap = relGaps[gi] * C;
			Ray ray; ray.origin = Point3( C, C, -gap ); ray.SetDir( Vector3( 0, 0, 1 ) );
			HIT h;
			RayPlaneIntersection( ray, h, Vector3( 0, 0, 1 ) );
			total++;
			if( h.bHit && std::fabs( h.dRange - gap ) <= 1e-6 * gap ) hit++;
			else std::printf( "    plane MISS: C=%g gap=%g -> bHit=%d t=%g\n",
				(double)C, (double)gap, (int)h.bHit, (double)h.dRange );
		}
		std::printf( "    infinite plane: %d / %d\n", hit, total );
		Check( hit == total,
			"infinite plane: a root above the relative floor is accepted at any world scale" );
	}

	// --- triangle -----------------------------------------------------
	{
		int hit = 0, total = 0;
		for( size_t ci = 0; ci < 4; ci++ ) for( size_t gi = 0; gi < 4; gi++ ) {
			const Scalar C = cs[ci];
			const Scalar gap = relGaps[gi] * C;
			const Point3  vPt1( -C, -C, C );
			const Vector3 edgeA( 3*C, 0, 0 );
			const Vector3 edgeB( 0, 3*C, 0 );
			Ray ray; ray.origin = Point3( 0.1*C, 0.1*C, C - gap ); ray.SetDir( Vector3( 0, 0, 1 ) );
			TRIANGLE_HIT h;
			RayTriangleIntersection( ray, h, vPt1, edgeA, edgeB );
			total++;
			if( h.bHit && std::fabs( h.dRange - gap ) <= 1e-6 * gap ) hit++;
			else std::printf( "    triangle MISS: C=%g gap=%g -> bHit=%d t=%g\n",
				(double)C, (double)gap, (int)h.bHit, (double)h.dRange );
		}
		std::printf( "    triangle: %d / %d\n", hit, total );
		Check( hit == total,
			"triangle: a root above the relative floor is accepted at any world scale" );
	}
}

//////////////////////////////////////////////////////////////////////
int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "SelfHitFloorCoincidentOriginTest -- DL-120" << std::endl;
	std::cout << "seed base = " << g_seedBase
		<< "  (pass a different one as argv[1] for an independent sample)" << std::endl;
	std::cout << "==========================================================" << std::endl;

	const char* filter = getenv( "SELFHIT_TEST_FILTER" );
	if( !filter || std::strstr( filter, "coplanar" ) )  TestCoplanarCameraSeesNoEmitter();
	if( !filter || std::strstr( filter, "luminaire" ) ) TestLuminaireRadianceClosedForm();
	if( !filter || std::strstr( filter, "selfhit" ) )   TestDebt21SelfHitStillRejected();
	if( !filter || std::strstr( filter, "scale" ) )     TestFloorIsScaleRelative();

	std::cout << "==========================================================" << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
