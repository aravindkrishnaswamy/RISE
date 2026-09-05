//////////////////////////////////////////////////////////////////////
//
//  PrimitiveSelfHitTest.cpp - docs/CLOTH_FABRIC_DESIGN.md section 15
//    debt 25 sibling audit: end-to-end regression guard for the
//    scale-relative self-hit floor added to
//    src/Library/Intersection/RaySphereIntersection.cpp,
//    RayQuadricIntersection.cpp, RayPlaneIntersection.cpp,
//    RayTriangleIntersection.cpp, RayCylinderIntersection.cpp, and
//    src/Library/Geometry/CylinderGeometry.cpp's capped-solid path.
//
//  THE BUG, ONE SENTENCE.  Every one of those routines gated its
//    candidate root(s) with the fixed absolute `t > NEARZERO` (1e-12),
//    but a shadow ray PT casts is published unadvanced from the hit
//    point (`Object::IntersectRay` backs it off by
//    `SURFACE_INTERSEC_ERROR` = 1e-12 along the incoming ray, no more),
//    so a shadow ray that CROSSES the surface it was cast from -- the
//    only way a full-sphere transmissive material's far side gets lit
//    from behind -- has a mathematically ~1e-12 self-root that straddles
//    the fixed floor whenever the outgoing ray is more grazing than the
//    incoming one, or whenever the coordinates involved are large enough
//    (~1e3) that the published point's own round-off lands inside/
//    outside the surface either way. `BoxGeometry::DropSelfHitRoot`
//    (debt 25) and `RayBilinearPatchIntersection` (debt 21) already had a
//    scale-relative fix; this session's sibling audit (the brief that
//    produced the patch this file guards) found the same fixed-NEARZERO
//    pattern in sphere, quadric (ellipsoid), plane, triangle and
//    cylinder, and BDPT is the reference throughout -- it `Advance()`s
//    every shadow/continuation ray by 1e-6 before casting, six orders of
//    magnitude clear of the ~1e-12 self-root, so it was never wrong on
//    any of these scenes.  This is the same "PT may be the broken one"
//    trap docs/skills/bdpt-vcm-mis-balance.md's step 0 pre-flight names,
//    and the same one debt 20/21/25 record for other primitives.
//
//  WHY THESE SEVEN PRIMITIVES (+ two controls).  Each is chosen to run
//    through exactly one of the five fixed intersection routines, so a
//    regression in any one of them shows up in an isolated case rather
//    than only in a compound scene:
//        sphere              -> RaySphereIntersection
//        ellipsoid           -> RayQuadricIntersection
//        capped cylinder     -> CylinderGeometry::IntersectCappedSolid
//        open tube, side-on  -> RayCylinderIntersection (side-wall path)
//        circular disk       -> RayPlaneIntersection
//        infinite plane      -> RayPlaneIntersection
//        two-triangle mesh   -> RayTriangleIntersection
//    `box_geometry` (fixed separately under debt 25,
//    `BoxGeometry::DropSelfHitRoot`) and `clippedplane_geometry` (fixed
//    under debt 21, `RayBilinearPatchIntersection`) are rendered as
//    CONTROLS: both were already on a scale-relative floor before this
//    patch, so they should read close to 1.0 both before and after --
//    proving the harness itself is sound and the other seven rows' large
//    pre-fix deviation is attributable to the primitives under test, not
//    to the scene shape.
//
//  THE SCENE.  A `weave_material { fabric custom transmission thin gap
//    0.0 warp_transmit 0.25 weft_transmit 0.25 }` -- linen's full-sphere
//    transmissive configuration, ScattersFullSphere() TRUE -- wrapping a
//    1-unit-scale primitive, lit from BEHIND by a point `omni_light` at
//    (0,0,-3), camera at (0,0,3.2) looking at the origin.  Every
//    camera-visible point is lit only through the material's diffuse
//    transmission lobe, which needs a shadow ray that starts on the near
//    face and CROSSES to see the light behind the far face -- exactly
//    the self-root topology the header above describes.  Scene byte
//    layout (geometry chunk aside) and the omni light/camera framing are
//    lifted verbatim from the measurement session's own scene family
//    (docs/CLOTH_FABRIC_DESIGN.md section 15 debt 25's sibling-audit
//    scenes), not reinvented here.
//
//  RED-PROOF: with `src/Library/Intersection/*.cpp` and
//    `CylinderGeometry.cpp` checked out to their pre-fix (8d97645f) state
//    and the library rebuilt, this binary reads 37 passed / 8 failed --
//    every row below except the two controls and the unit-scale twin
//    fails its band by an order of magnitude or worse.  Measured
//    2026-09-05 at seed base 1000 (BDPT/PT; the sweep that motivated the
//    fix read the same rows at 3.12 / 2.80 / 2.19 / 2.82 / 393 / 354 /
//    157 / 1.61 -- the differences are Monte Carlo noise on ratios whose
//    denominators are near-zero PT images):
//
//        sphere                3.155   FAIL      box (control)          0.965   pass
//        ellipsoid             2.787   FAIL      clippedplane (control) 1.000   pass
//        capped cylinder       2.175   FAIL      Lambertian S=1 twin    0.9999  pass
//        open tube (side-on)   2.873   FAIL
//        circular disk         429.7   FAIL
//        infinite plane        344.2   FAIL
//        triangle mesh         163.2   FAIL
//        Lambertian S=1000     1.613   FAIL
//
//    Post-fix (this tree): 45 passed / 0 failed in ~5 s.
//
//  SEEDING.  Identical convention and identical justification to
//    tests/FabricRenderTest.cpp (read that file's header for the full
//    argument): this binary is not wall-clock seeded, so `main` takes an
//    OPTIONAL SEED BASE (argv[1], default `kDefaultSeedBase`) and
//    `RenderAndComputeStats` calls `std::srand( seedBase + n )` before
//    each render, `n` counting renders within the run.  Different bases
//    (1000, 2000, ... 5000) are independent BY CONSTRUCTION, which is how
//    the n = 5 spreads quoted below were produced.
//
//  OIDN DENOISE IS EXPLICITLY DISABLED in every rasterizer chunk below,
//    for the same reason FabricRenderTest's and HairRenderTest's headers
//    give: `IRasterizerOutput::OutputDenoisedImage`'s default forwards
//    POST-DENOISE pixels to `OutputImage`, and OIDN is free to smooth
//    away exactly the near-surface self-occlusion artifact this test is
//    looking for.
//
//  PRIM_TEST_FILTER (optional environment variable), mirroring
//    FabricRenderTest's FABRIC_TEST_FILTER convention.  When set, only
//    the cases whose keyword is a substring of its value run; unset (the
//    normal invocation) runs all of them.  Keywords, one per case:
//        crossing   TestTransmissiveCrossingAllPrimitives
//        lambertian TestLambertianSphereGrazing
//    e.g. `PRIM_TEST_FILTER=crossing ./bin/tests/PrimitiveSelfHitTest 3000`
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>			// std::strstr — PRIM_TEST_FILTER matching
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <sstream>
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

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

//! Seed base for this run; see the file header.
static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;

//! Renders completed so far in this run.
static unsigned int g_renderIndex = 0;

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
// CapturingRasterizerOutput / ImageStats / WriteSceneToTempFile /
// RenderAndComputeStats -- identical in shape to FabricRenderTest's
// (see that file's header for the OIDN and seeding rationale these
// mirror).
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
	double luminance;	// mean(mean[0..2]) -- the scalar every check below uses
	double maxLum;		// Max per-pixel luminance (firefly guard)
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	double maxLum = 0.0;
	for( const RISEColor& c : cap.pixels ) {
		sum[0] += c.base.r;
		sum[1] += c.base.g;
		sum[2] += c.base.b;
		const double pixLum = (c.base.r + c.base.g + c.base.b) / 3.0;
		if( pixLum > maxLum ) maxLum = pixLum;
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] = sum[c] / double(cap.pixels.size());
	s.luminance = (s.mean[0] + s.mean[1] + s.mean[2]) / 3.0;
	s.maxLum = maxLum;
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/prim_selfhit_test_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//! Writes the two-triangle quad PLY the mesh case needs, at an ABSOLUTE
//! /tmp path so `plymesh_geometry`'s `file` parameter resolves without
//! any RISE_MEDIA_PATH dependency: `MediaPathLocator::Find` (see
//! src/Library/Utilities/MediaPathLocator.cpp) checks `FileExists(file)`
//! FIRST and returns an existing absolute path verbatim before ever
//! consulting the media-path search list.
static std::string WritePlyQuadToTempFile()
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/prim_selfhit_test_quad_%d.ply",
		static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	// Same two-triangle unit-scale quad (x,y in [-1.4,1.4], z=0) as the
	// measurement session's own `quad_s1.ply`.
	ofs <<
		"ply\n"
		"format ascii 1.0\n"
		"element vertex 4\n"
		"property float x\n"
		"property float y\n"
		"property float z\n"
		"element face 2\n"
		"property list uchar int vertex_indices\n"
		"end_header\n"
		"-1.4 -1.4 0\n"
		"1.4 -1.4 0\n"
		"1.4 1.4 0\n"
		"-1.4 1.4 0\n"
		"3 0 1 2\n"
		"3 0 2 3\n";
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
{
	ImageStats result{};

	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return result;
	}

	// See the file header: this binary is not wall-clock seeded, so the
	// starting state is set here rather than left to the worker-side
	// `rand()` race.
	std::srand( g_seedBase + g_renderIndex );
	g_renderIndex++;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		std::remove( path.c_str() );
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	std::remove( path.c_str() );
	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene assembly.
//////////////////////////////////////////////////////////////////////

static std::string RasterizerPTRgbNoEnv( unsigned int samples, unsigned int rrMinDepth )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/prim_selfhit_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string RasterizerBDPTRgbNoEnv( unsigned int samples, unsigned int maxEyeDepth, unsigned int maxLightDepth )
{
	std::ostringstream ss;
	ss <<
		"bdpt_pel_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\tmax_eye_depth " << maxEyeDepth << "\n\tmax_light_depth " << maxLightDepth << "\n"
		"\toidn_denoise FALSE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/prim_selfhit_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// 1. TRANSMISSIVE-CROSSING SELF-HIT -- one row per fixed intersection
// routine, plus two controls that were already scale-relative before
// this patch.
//
// Common scene shape for every row (only the geometry chunk differs):
// a `weave_material { fabric custom transmission thin gap 0.0
// warp_transmit 0.25 weft_transmit 0.25 }` primitive at 1-unit scale,
// `omni_light` behind at (0,0,-3) power 6, camera at (0,0,3.2) fov 34,
// 24x24, 256 spp, `oidn_denoise FALSE` -- lifted verbatim from the
// measurement session's own scene family
// (docs/CLOTH_FABRIC_DESIGN.md section 15 debt 25 sibling audit).
//
// TOLERANCE DERIVATION.  Measured on this machine (`PRIM_TEST_FILTER=crossing`),
// over n = 5 independent seed bases 1000/2000/3000/4000/5000
// (post-fix tree, this file's own commit; `for b in 1000 2000 3000 4000
// 5000; do PRIM_TEST_FILTER=crossing ./bin/tests/PrimitiveSelfHitTest $b;
// done`):
//
//   row (BDPT/PT)          seed 1000  2000    3000    4000    5000    mean     sigma    worst |dev-1|
//   sphere                 0.98848 0.97702 0.98141 0.97381 0.97266  0.97868  0.00576   0.02734 (2.73%)
//   ellipsoid              0.99703 1.01242 1.01817 1.01243 1.00725  1.00946  0.00711   0.01817 (1.82%)
//   capped cylinder        0.95844 0.95822 0.96284 0.95629 0.97046  0.96125  0.00508   0.04371 (4.37%)
//   open tube (side-on)    0.96261 0.94597 0.96023 0.94710 0.94857  0.95290  0.00705   0.05403 (5.40%)
//   circular disk          1.00001 1.00001 1.00000 1.00001 1.00002  1.00001  0.00001   0.00002 (0.00%)
//   infinite plane         1.00000 1.00000 1.00000 1.00000 1.00000  1.00000  0.00000   0.00000 (0.00%)
//   triangle mesh (PLY)    1.00000 1.00000 1.00000 1.00000 1.00000  1.00000  0.00000   0.00000 (0.00%)
//   box (control)          0.96754 0.96972 0.97150 0.96735 0.97010  0.96924  0.00158   0.03265 (3.26%)
//   clippedplane (control) 1.00000 1.00000 1.00000 1.00000 1.00000  1.00000  0.00000   0.00000 (0.00%)
//
// The four CLOSED-SHELL rows (sphere, ellipsoid, capped cylinder, open
// tube) share the box control's documented ~3% BDPT/VCM-under-PT
// residual (docs/CLOTH_FABRIC_DESIGN.md section 15 debt 25's "What this
// does NOT close" paragraph: BDPT reads the box ~0.969-0.972 of PT for a
// reason independent of this fix, disclosed and left open there) -- and
// the box control measured here (mean 0.96924) confirms it reproduces on
// this exact scene family.  Banded at +/-8%, matching
// `TestClosedBoxThinWeave`'s own band and for the same reason: loose
// enough to absorb that disclosed residual (worst observed here 5.40%,
// on the open tube) and machine-to-machine RNG movement, tight enough
// that the pre-fix self-hit bug -- which the BACKGROUND measurement
// (this file's header) recorded at 2.19-3.21x BDPT/PT on these exact
// primitives -- trips it by well over an order of magnitude (a 2.19x
// ratio is 0.08 away from 1.0 by a factor of ~15).  The three FLAT/OPEN
// rows (disk, plane, mesh) and the two controls have no such residual
// (BDPT/PT measures exactly 1.000 to 5 digits on every one of them,
// worst observed 0.02%) and are banded tighter, at +/-3% -- ~150x
// headroom over what was actually observed, matching the pre-existing
// `kHwssTol`-style precedent in FabricRenderTest for a "should read
// exactly 1.0" invariant, and still an order of magnitude under the
// pre-fix 354x (plane) / 157x (mesh) / 393x (disk) deviations the
// BACKGROUND measurement recorded.
//////////////////////////////////////////////////////////////////////
static const double kClosedShellTol = 0.08;
static const double kFlatSurfaceTol = 0.03;

enum PrimKind
{
	kPrimSphere,
	kPrimEllipsoid,
	kPrimCylCapped,
	kPrimCylOpen,
	kPrimDisk,
	kPrimPlane,
	kPrimMesh,
	kPrimBoxControl,
	kPrimClippedPlaneControl
};

struct PrimRow
{
	PrimKind    kind;
	const char* label;
	const char* tag;
	double      tol;
};

static const PrimRow kPrimRows[] = {
	{ kPrimSphere,               "sphere",                        "sphere",      kClosedShellTol },
	{ kPrimEllipsoid,            "ellipsoid",                     "ellipsoid",   kClosedShellTol },
	{ kPrimCylCapped,            "capped cylinder (axis x)",      "cylcapped",   kClosedShellTol },
	{ kPrimCylOpen,              "open tube, side-on (axis x)",   "cylopen",     kClosedShellTol },
	{ kPrimDisk,                 "circular disk",                 "disk",        kFlatSurfaceTol },
	{ kPrimPlane,                "infinite plane",                "plane",       kFlatSurfaceTol },
	{ kPrimMesh,                 "two-triangle PLY mesh",         "mesh",        kFlatSurfaceTol },
	{ kPrimBoxControl,           "box (control)",                 "boxctl",      kClosedShellTol },
	{ kPrimClippedPlaneControl,  "clippedplane (control)",        "clipctl",     kFlatSurfaceTol },
};

static std::string PrimitiveGeometryChunk( PrimKind kind, const std::string& plyPath )
{
	switch( kind ) {
	case kPrimSphere:
		return "sphere_geometry\n{\n\tname g\n\tradius 1.0\n}\n\n";
	case kPrimEllipsoid:
		return "ellipsoid_geometry\n{\n\tname g\n\tradii 1.0 1.2 0.8\n}\n\n";
	case kPrimCylCapped:
		return "cylinder_geometry\n{\n\tname g\n\taxis x\n\tradius 1.0\n\theight 1.0\n\tcapped TRUE\n}\n\n";
	case kPrimCylOpen:
		return "cylinder_geometry\n{\n\tname g\n\taxis x\n\tradius 1.0\n\theight 1.0\n\tcapped FALSE\n}\n\n";
	case kPrimDisk:
		return "circulardisk_geometry\n{\n\tname g\n\tradius 1.4\n\taxis z\n}\n\n";
	case kPrimPlane:
		return "infiniteplane_geometry\n{\n\tname g\n\txtile 1.0\n\tytile 1.0\n}\n\n";
	case kPrimMesh:
		return "plymesh_geometry\n{\n\tname g\n\tfile " + plyPath + "\n\tdouble_sided TRUE\n}\n\n";
	case kPrimBoxControl:
		return "box_geometry\n{\n\tname g\n\twidth 2.8\n\theight 2.8\n\tdepth 1.0\n}\n\n";
	case kPrimClippedPlaneControl:
		return
			"clippedplane_geometry\n{\n\tname g\n"
			"\tpta -1.4 -1.4 0\n\tptb 1.4 -1.4 0\n\tptc 1.4 1.4 0\n\tptd -1.4 1.4 0\n"
			"\tdoublesided TRUE\n}\n\n";
	}
	return std::string();
}

static std::string PrimitiveXmitCommon( PrimKind kind, const std::string& plyPath, unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		<< PrimitiveGeometryChunk( kind, plyPath ) <<
		"weave_material\n{\n\tname m\n\tfabric custom\n\ttransmission thin\n\tgap 0.0\n"
			"\twarp_transmit 0.25\n\tweft_transmit 0.25\n}\n\n"
		"standard_object\n{\n\tname o\n\tgeometry g\n\tmaterial m\n\tposition 0 0 0\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34\n}\n\n"
		"omni_light\n{\n\tname L\n\tposition 0 0 -3.0\n\tcolor 1 1 1\n\tpower 6.0\n}\n\n";
	return ss.str();
}

static void TestTransmissiveCrossingAllPrimitives()
{
	std::cout << "=== 1. Transmissive-crossing self-hit (debt 25 sibling audit) ===" << std::endl;

	const unsigned int W = 24, H = 24;
	const unsigned int samples = 256;

	// Written once, shared by the mesh row's PT and BDPT renders.
	const std::string plyPath = WritePlyQuadToTempFile();
	Check( !plyPath.empty(), "mesh row: PLY temp file written" );

	for( const PrimRow& row : kPrimRows ) {
		const std::string common = PrimitiveXmitCommon( row.kind, plyPath, W, H );

		char tagPt[64], tagBd[64], buf[256];
		std::snprintf( tagPt, sizeof(tagPt), "%s_pt",   row.tag );
		std::snprintf( tagBd, sizeof(tagBd), "%s_bdpt", row.tag );

		const ImageStats sPt = RenderAndComputeStats(
			AssembleScene( common, RasterizerPTRgbNoEnv( samples, 8 ) ), tagPt );
		const ImageStats sBdpt = RenderAndComputeStats(
			AssembleScene( common, RasterizerBDPTRgbNoEnv( samples, 8, 8 ) ), tagBd );

		std::snprintf( buf, sizeof(buf), "%s: PT render produced output", row.label );
		Check( sPt.valid, buf );
		std::snprintf( buf, sizeof(buf), "%s: BDPT render produced output", row.label );
		Check( sBdpt.valid, buf );
		if( !sPt.valid || !sBdpt.valid ) continue;

		std::snprintf( buf, sizeof(buf), "%s: PT render is non-degenerate (not a black frame)", row.label );
		Check( sPt.luminance > 1e-6, buf );

		const double ratio = sBdpt.luminance / std::fmax( sPt.luminance, 1e-12 );
		std::cout << "  " << row.label
			<< ": PT = " << sPt.luminance
			<< "   BDPT = " << sBdpt.luminance
			<< "   BDPT/PT = " << ratio
			<< "   (tolerance +/-" << row.tol << ")" << std::endl;

		std::snprintf( buf, sizeof(buf), "%s: BDPT/PT within tolerance (self-hit floor holding)", row.label );
		Check( std::fabs( ratio - 1.0 ) <= row.tol, buf );
	}

	if( !plyPath.empty() ) std::remove( plyPath.c_str() );
}

//////////////////////////////////////////////////////////////////////
// 2. LAMBERTIAN SPHERE, GRAZING OMNI LIGHT -- exercises the SAME
// RaySphereIntersection self-hit floor from the OTHER direction: not a
// shadow ray crossing a transmissive shell, but the eye ray's own
// published hit point (at large coordinates, ~1000 units, where the
// point's own round-off is comparable to the fixed NEARZERO) sitting
// fractionally inside the sphere, so the NEE shadow ray toward a
// near-grazing light self-occludes for a share of pixels.
//
// A radius-1000 Lambertian sphere lit by an oblique `omni_light` at
// (4000, 0, 1200) (i.e. well off-axis from the camera at (0,0,4000)),
// rendered at 48x48 / 256 spp, `oidn_denoise FALSE`. Its UNIT-SCALE TWIN
// (radius 1, light at (4,0,1.2), power scaled by 1/1e6 to hold the same
// solid angle and irradiance) shares every byte of scene shape except
// the /1000 coordinate scale and is the reference this row is judged
// against: PT and BDPT should agree at BOTH scales, and any divergence
// that shows up ONLY at the 1000x scale is the self-hit floor's
// coordinate-scale term doing its job (or not).
//
// TOLERANCE DERIVATION.  Measured on this machine (`PRIM_TEST_FILTER=lambertian`),
// over n = 5 independent seed bases 1000/2000/3000/4000/5000 (post-fix
// tree, this file's own commit):
//
//   BDPT/PT at S=1000:                0.99969 1.00026 0.99982 0.99982 0.99993
//                                     (mean 0.99991, sigma 1.9e-4, worst |dev-1| 0.031%)
//   BDPT/PT at S=1 (unit-scale twin): 1.00039 0.99979 0.99924 0.99993 1.00026
//                                     (mean 0.99992, sigma 4.0e-4, worst |dev-1| 0.076%)
//
// Both rows are banded at +/-3%, the same flat-surface-class tolerance
// used above: a Lambertian sphere under a single point light has no
// disclosed BDPT/PT residual (unlike the closed-shell weave rows), so
// BDPT/PT reads within a tenth of a percent of 1.000 at BOTH scales once
// the self-hit floor holds -- ~40x headroom under the chosen band. The
// BACKGROUND measurement (this file's header) recorded a pre-fix
// BDPT/PT of 1.61 at S=1000 (self-occluded PT reading well under BDPT)
// against 1.0003 post-fix, an order of magnitude clear of a 3% band
// either way.
//////////////////////////////////////////////////////////////////////
static const double kLambertianSphereTol = 0.03;

static std::string LambertianSphereGrazingCommon(
	double sphereScale, double lightPower, unsigned int width, unsigned int height )
{
	const double camZ   = 4.0   * sphereScale;
	const double lightX = 4.0   * sphereScale;
	const double lightZ = 1.2   * sphereScale;

	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 " << camZ << "\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname mat_color\n\tcolor 0.7 0.7 0.7\n}\n\n"
		"lambertian_material\n{\n\tname mat\n\treflectance mat_color\n}\n\n"
		"sphere_geometry\n{\n\tname geom\n\tradius " << sphereScale << "\n}\n\n"
		"standard_object\n{\n\tname obj\n\tgeometry geom\n\tmaterial mat\n\tposition 0 0 0\n}\n\n"
		"omni_light\n{\n\tname L\n\tpower " << lightPower << "\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n"
			"\tposition " << lightX << " 0 " << lightZ << "\n}\n\n";
	return ss.str();
}

static void RunLambertianSphereRow( double sphereScale, double lightPower, const char* label, const char* tag )
{
	const unsigned int W = 48, H = 48;
	const unsigned int samples = 256;
	const std::string common = LambertianSphereGrazingCommon( sphereScale, lightPower, W, H );

	char tagPt[64], tagBd[64], buf[256];
	std::snprintf( tagPt, sizeof(tagPt), "%s_pt",   tag );
	std::snprintf( tagBd, sizeof(tagBd), "%s_bdpt", tag );

	const ImageStats sPt = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTRgbNoEnv( samples, 8 ) ), tagPt );
	const ImageStats sBdpt = RenderAndComputeStats(
		AssembleScene( common, RasterizerBDPTRgbNoEnv( samples, 8, 8 ) ), tagBd );

	std::snprintf( buf, sizeof(buf), "%s: PT render produced output", label );
	Check( sPt.valid, buf );
	std::snprintf( buf, sizeof(buf), "%s: BDPT render produced output", label );
	Check( sBdpt.valid, buf );
	if( !sPt.valid || !sBdpt.valid ) return;

	std::snprintf( buf, sizeof(buf), "%s: PT render is non-degenerate (not a black frame)", label );
	Check( sPt.luminance > 1e-6, buf );

	const double ratio = sBdpt.luminance / std::fmax( sPt.luminance, 1e-12 );
	std::cout << "  " << label
		<< ": PT = " << sPt.luminance
		<< "   BDPT = " << sBdpt.luminance
		<< "   BDPT/PT = " << ratio
		<< "   (tolerance +/-" << kLambertianSphereTol << ")" << std::endl;

	std::snprintf( buf, sizeof(buf), "%s: BDPT/PT within tolerance (self-hit floor holding)", label );
	Check( std::fabs( ratio - 1.0 ) <= kLambertianSphereTol, buf );
}

static void TestLambertianSphereGrazing()
{
	std::cout << "=== 2. Lambertian sphere, grazing omni light (radius-1000 self-hit + unit-scale twin) ===" << std::endl;

	// power 150e6 at radius 1000 vs power 150 at radius 1 -- the same
	// setup the BACKGROUND measurement used, scaled uniformly (position
	// and radius by 1000x, power to hold irradiance at the sphere).
	RunLambertianSphereRow( 1000.0, 150000000.0, "radius 1000 (self-hit regime)", "lam1000" );
	RunLambertianSphereRow( 1.0,    150.0,       "radius 1 (unit-scale twin)",    "lam1" );
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////
int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "PrimitiveSelfHitTest -- docs/CLOTH_FABRIC_DESIGN.md 15 debt 25 sibling audit" << std::endl;
	std::cout << "seed base = " << g_seedBase
		<< "  (pass a different one as argv[1] for an independent sample)" << std::endl;
	std::cout << "==========================================================" << std::endl;

	const char* filter = getenv("PRIM_TEST_FILTER");
	if( !filter || std::strstr( filter, "crossing" ) )   TestTransmissiveCrossingAllPrimitives();
	if( !filter || std::strstr( filter, "lambertian" ) ) TestLambertianSphereGrazing();

	std::cout << "==========================================================" << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
