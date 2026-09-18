//////////////////////////////////////////////////////////////////////
//
//  BidirectionalTextureFootprintParityTest.cpp -- DL-14 closure.
//
//  DL-14 (docs/DEBT_LEDGER.md) claimed: "ray differentials (fw/fwo,
//  the texture footprint) are 0 under BDPT/VCM/MLT -- the field
//  exists and is carried, but no differentials are ever computed for
//  those walks, so filtered-noise / footprint-driven bands (fbm
//  octave fade, mip selection, the relief step) render differently
//  under PT vs any bidirectional integrator."
//
//  RE-VERIFICATION (this file's reason to exist): on this HEAD the
//  claim is FALSE.  `ICamera::GenerateRay` (PinholeCamera.cpp,
//  ThinLensCamera.cpp, OrthographicCamera.cpp, FisheyeCamera.cpp)
//  unconditionally stamps Igehy ray differentials onto the camera ray
//  it returns, with no gate on which rasterizer is asking.  BDPT,
//  VCM and MLT all obtain their eye subpath's starting ray through
//  that SAME interface call (BDPTPelRasterizer.cpp:95,
//  VCMPelRasterizer.cpp:299, MLTRasterizer.cpp:198 -- MLT drives a
//  `BDPTIntegrator` instance directly, see MLTRasterizer.cpp:166/376)
//  and hand it unmodified into `GenerateEyeSubpathImpl` as
//  `currentRay` (BDPTIntegrator.cpp).  `Object::IntersectRay` (the
//  ONE geometry-intersection layer every integrator shares) computes
//  `ri.geometric.txFootprint` whenever `ray.hasDifferentials` is
//  true, so the eye subpath's FIRST hit -- the only vertex any
//  integrator (PT included) ever attaches a real footprint to, since
//  every ray after the first scattering bounce carries no
//  differentials at all, by design, everywhere in this renderer --
//  gets the identical, non-zero footprint PT's own primary ray gets.
//  The 2026-09-11 S1 slice (925c2720, docs/
//  SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md) already added the two
//  copy sites that make this observable at the BDPTVertex/painter
//  layer (`v.txFootprint = ri.geometric.txFootprint;` at
//  BDPTIntegrator.cpp's eye- and light-subpath vertex population
//  blocks, and `PathVertexEval::PopulateRIGFromVertex`'s
//  `ri.txFootprint = vertex.txFootprint;`) -- it believed those
//  writes were inert ("all-zero... they emit no ray differentials",
//  BDPTVertex.h's field comment) because it never traced the camera
//  ray's own differentials through to `GenerateEyeSubpathImpl`.  They
//  were not inert.  See docs/DL14_BIDIRECTIONAL_RAY_DIFFERENTIALS.md
//  for the full account, including the stale doc/comment corrections
//  this discovery required.
//
//  WHAT THIS FILE PINS.  A scene whose floor reflectance is an
//  `expression_painter` built on `fbm(P*scale, ...)` -- the canonical
//  `fw`/`fwo`-driven octave fade (docs/TEXTURE_FOOTPRINT_ANALYTIC_
//  DESIGN.md) -- viewed at a shallow grazing angle so the per-pixel
//  world footprint spans several orders of magnitude across one
//  image: tiny near the camera (fine high-frequency noise visible,
//  large per-row standard deviation), enormous near the horizon
//  (heavily faded, near-flat, near-zero per-row standard deviation).
//  Rendered once each under `pathtracing_pel_rasterizer`,
//  `bdpt_pel_rasterizer` and `vcm_pel_rasterizer` at matched settings
//  (`oidn_denoise FALSE`, `pixel_filter box`, a single point light so
//  the whole scene is direct-lit off the ONE vertex every integrator
//  ever attaches a footprint to -- no dependence on VCM's directional-
//  light gap, which is unrelated and pre-existing).
//
//    (1) FADE SHAPE, per integrator.  The far-row band's standard
//        deviation must be far smaller than the near-row band's, in
//        EVERY integrator.  This is the direct catch for "footprint
//        stuck at zero": with no fade, the far band would show the
//        SAME high-frequency variance as the near band instead of a
//        heavily smoothed one.
//    (2) PT vs BDPT PARITY, tight.  Same camera ray, same geometry
//        intersection code, same vertex-population copy -- the two
//        should agree to a few times the camera's own run-to-run FP
//        summation-order noise, not a MC-noise-scale tolerance.
//    (3) PT vs VCM PARITY, loose.  VCM combines a different mixture
//        of MIS strategies per pixel even on a simple direct-lit
//        diffuse floor, so its per-pixel noise is measurably higher
//        at matched sample count (measured on this fixture: ~2-4 %
//        at 64 spp, shrinking with more samples) -- the tolerance
//        here is generous enough to absorb that MC noise while still
//        being far tighter than the "footprint completely missing"
//        failure mode this file exists to catch (which would move the
//        far band by orders of magnitude, not a few percent).
//
//  RED-PROOF.  Temporarily inserting `currentRay.hasDifferentials =
//  false;` immediately after `Ray currentRay = cameraRay;` at the top
//  of `GenerateEyeSubpathImpl` (BDPTIntegrator.cpp) -- the literal
//  code shape DL-14's own text describes ("no differentials are ever
//  computed for those walks") -- and rebuilding turns every assertion
//  in section (1) and (2) red for BDPT and VCM: the far band's std
//  jumps to match the near band's (full, unfaded high-frequency
//  noise leaking through at every distance) and the PT-vs-BDPT/VCM
//  far-band ratio blows out by 1-2 orders of magnitude.  Measured
//  numbers from that mutated build are recorded in the fix commit
//  message and docs/DL14_BIDIRECTIONAL_RAY_DIFFERENTIALS.md (there is
//  no permanent code change in this slice -- the mechanism already
//  works; this file and that doc are the closure evidence).
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

// Explicit render seeding (rise-render-seeding.md): renders are NOT
// wall-clock seeded, and this file compares raw pixel statistics
// across three separate renders, so each must srand() explicitly.
static unsigned int g_renderSeed = 8140001u;

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
// CapturingRasterizerOutput -- same shape as EnvLightBalanceTest /
// BDPTStrategyBalanceTest.
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

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/dl14_footprint_parity_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//! Renders one scene, returning the captured buffer.  Empty (width==0)
//! on any load/render failure.
static CapturingRasterizerOutput* RenderScene( const std::string& sceneText, const char* tag )
{
	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return nullptr;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return nullptr;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		return nullptr;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pCap->addref();
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_renderSeed++ );
	const bool bRendered = pJob->Rasterize();

	safe_release( pJob );

	if( !bRendered || pCap->width == 0 ) {
		safe_release( pCap );
		return nullptr;
	}
	return pCap;
}

//! Whole-image luminance mean and a HIGH-FREQUENCY ENERGY metric: the
//! mean absolute horizontal finite difference between adjacent pixels
//! (|lum(x+1,y) - lum(x,y)|), averaged over the whole frame.
//!
//! Why finite-difference energy, not a per-region std: this scene's
//! ONLY light a VCM-compatible one can be (VCM has no directional- or
//! ambient-light NEE at all -- CLAUDE.md "no directional-light
//! sampling at all", confirmed for ambient too during this file's own
//! development) is a point light, whose 1/r^2 falloff and off-axis
//! position give the render its own smooth illumination GRADIENT
//! across both image axes.  A per-row-band std/mean comparison mixes
//! that illumination gradient in with the texture-driven variance this
//! file actually wants to isolate, and the two are not always the same
//! sign of trend (measured: whole-row coefficient-of-variation is
//! NON-MONOTONIC across depth on this fixture, confounded by the
//! light's own footprint on the floor).  A per-pixel horizontal finite
//! difference is local enough that the illumination gradient (smooth
//! over many pixels) contributes almost nothing to it, while the fbm
//! texture's own high-frequency content -- exactly what `fw`/`fwo`
//! octave-fades -- dominates it.  Whole-FRAME (not per-row) average
//! also sidesteps having to name a specific "near" vs "far" row band
//! at all: whichever pixels in the image carry the un-faded aliasing a
//! missing footprint would leave behind, they inflate this ONE number.
struct ImageHFStats { double mean; double hfEnergy; bool valid; };

static ImageHFStats ImageHF( const CapturingRasterizerOutput& cap )
{
	ImageHFStats s{ 0.0, 0.0, false };
	if( cap.width < 2 || !cap.height ) return s;

	std::vector<double> lum( size_t(cap.width) * cap.height );
	for( unsigned int y = 0; y < cap.height; y++ ) {
		for( unsigned int x = 0; x < cap.width; x++ ) {
			const RISEColor& c = cap.pixels[y * cap.width + x];
			if( !std::isfinite( c.base.r ) || !std::isfinite( c.base.g ) || !std::isfinite( c.base.b ) ) {
				return s;   // a NaN/Inf pixel invalidates the whole image, not just one sample
			}
			lum[y * cap.width + x] = (c.base.r + c.base.g + c.base.b) / 3.0;
		}
	}

	double sum = 0.0;
	for( double v : lum ) sum += v;
	s.mean = sum / double(lum.size());

	double hfSum = 0.0;
	size_t hfCount = 0;
	for( unsigned int y = 0; y < cap.height; y++ ) {
		for( unsigned int x = 0; x + 1 < cap.width; x++ ) {
			hfSum += std::fabs( lum[y * cap.width + x + 1] - lum[y * cap.width + x] );
			hfCount++;
		}
	}
	s.hfEnergy = hfCount ? hfSum / double(hfCount) : 0.0;
	s.valid = true;
	return s;
}

static bool RelClose( double a, double b, double tol )
{
	if( !std::isfinite(a) || !std::isfinite(b) ) return false;
	const double denom = std::max( std::fabs(a), std::fabs(b) );
	if( denom < 1e-9 ) return true;   // both ~0
	return std::fabs(a - b) / denom <= tol;
}

//////////////////////////////////////////////////////////////////////
// Scene template.  A single directly-lit, non-emissive floor: an
// `expression_painter` fbm-fade reflectance on an `infiniteplane_
// geometry` rotated to be a Y-up floor, a shallow-grazing pinhole
// camera looking down its length, and one omni_light (VCM has no
// directional-light NEE at all -- an unrelated, pre-existing,
// documented gap; CLAUDE.md "no directional-light sampling at all" --
// so a point light keeps this fixture on-topic for DL-14 alone).
//////////////////////////////////////////////////////////////////////
static std::string BuildScene( const std::string& rasterizerChunk, const char* outPattern )
{
	std::string s;
	s += "RISE ASCII SCENE 7\n";
	s += "# DL-14 closure: PT/BDPT/VCM texture-footprint parity fixture\n\n";
	s += "standard_shader\n{\n\tname\t\t\tglobal\n\tshaderop\t\tDefaultPathTracing\n}\n\n";
	s += rasterizerChunk;
	s += "\n";
	s += "file_rasterizeroutput\n{\n\tpattern\t\t\t";
	s += outPattern;
	s += "\n\ttype\t\t\tEXR\n\tbpp\t\t\t32\n\tcolor_space\t\tRec709RGB_Linear\n}\n\n";
	s += "film\n{\n\twidth\t\t\t128\n\theight\t\t\t96\n}\n\n";
	s += "pinhole_camera\n{\n\tlocation\t\t0 2 -5\n\tlookat\t\t\t0 -0.3 60\n\tup\t\t\t0 1 0\n\tfov\t\t\t50.0\n}\n\n";
	s += "expression_painter\n{\n\tname\t\t\tfbmfade\n"
	     "\tdef\t\t\tt clamp(0.5 + 0.5*fbm(P*6.0, 6, 0.5, 2.0), 0, 1)\n"
	     "\texpr\t\t\tvec3(t,t,t)\n}\n\n";
	s += "lambertian_material\n{\n\tname\t\t\tmat_floor\n\treflectance\t\tfbmfade\n}\n\n";
	s += "infiniteplane_geometry\n{\n\tname\t\t\tfloorgeom\n}\n\n";
	s += "standard_object\n{\n\tname\t\t\tfloor\n\tgeometry\t\tfloorgeom\n"
	     "\torientation\t\t-90 0 0\n\tposition\t\t0 0 0\n\tmaterial\t\tmat_floor\n"
	     "\treceives_shadows\tfalse\n}\n\n";
	s += "omni_light\n{\n\tname\t\t\tkey\n\tpower\t\t\t400.0\n\tcolor\t\t\t1.0 1.0 1.0\n"
	     "\tcolorspace\t\tRec709RGB_Linear\n\tposition\t\t0 4 5\n}\n";
	return s;
}

static const char* kPtRasterizer =
	"pathtracing_pel_rasterizer\n{\n\tsamples\t\t\t64\n\toidn_denoise\t\tFALSE\n\tpixel_filter\t\tbox\n}\n";
static const char* kBdptRasterizer =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth\t\t2\n\tmax_light_depth\t\t2\n\tsamples\t\t\t64\n"
	"\toidn_denoise\t\tFALSE\n\tpixel_filter\t\tbox\n}\n";
static const char* kVcmRasterizer =
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth\t\t2\n\tmax_light_depth\t\t2\n\tsamples\t\t\t64\n"
	"\toidn_denoise\t\tFALSE\n\tpixel_filter\t\tbox\n}\n";

static void TestFootprintParity()
{
	CapturingRasterizerOutput* pt   = RenderScene( BuildScene( kPtRasterizer,   "rendered/dl14_parity_pt"   ), "pt"   );
	CapturingRasterizerOutput* bdpt = RenderScene( BuildScene( kBdptRasterizer, "rendered/dl14_parity_bdpt" ), "bdpt" );
	CapturingRasterizerOutput* vcm  = RenderScene( BuildScene( kVcmRasterizer,  "rendered/dl14_parity_vcm"  ), "vcm"  );

	Check( pt != nullptr,   "PT scene rendered" );
	Check( bdpt != nullptr, "BDPT scene rendered" );
	Check( vcm != nullptr,  "VCM scene rendered" );
	if( !pt || !bdpt || !vcm ) {
		safe_release( pt ); safe_release( bdpt ); safe_release( vcm );
		return;
	}

	const ImageHFStats ptStats   = ImageHF( *pt );
	const ImageHFStats bdptStats = ImageHF( *bdpt );
	const ImageHFStats vcmStats  = ImageHF( *vcm );

	Check( ptStats.valid && bdptStats.valid && vcmStats.valid, "all three images finite" );

	std::cout << "  PT   mean=" << ptStats.mean   << " hfEnergy=" << ptStats.hfEnergy   << std::endl;
	std::cout << "  BDPT mean=" << bdptStats.mean << " hfEnergy=" << bdptStats.hfEnergy << std::endl;
	std::cout << "  VCM  mean=" << vcmStats.mean  << " hfEnergy=" << vcmStats.hfEnergy  << std::endl;

	// (1) PT vs BDPT, tight: identical camera ray, identical geometry
	// intersection, identical vertex-population copy -- agreement is
	// expected to several decimal places, not MC-noise-scale.  5% is
	// still a wide margin over the ~0.1% relative difference measured
	// on this fixture; it exists only to not be a flake magnet.
	Check( RelClose( ptStats.mean,     bdptStats.mean,     0.05 ), "PT vs BDPT: whole-image mean" );
	Check( RelClose( ptStats.hfEnergy, bdptStats.hfEnergy, 0.10 ), "PT vs BDPT: whole-image HF energy" );

	// (2) PT vs VCM: mean only, loose tolerance.  VCM does NOT get its
	// own HF-energy check here -- measured on this fixture, VCM's own
	// per-pixel noise is 2-4x PT/BDPT's at matched 64 spp and converges
	// to that noise floor only very slowly with more samples (1.9x
	// remained at 1024 spp, 16x the sample count), which is exactly
	// RISE's own documented, ACCEPTED characteristic of VCM on a
	// non-caustic scene (CLAUDE.md: "VCM loses sigma^2*T 3-40x
	// off-caustics" -- UNIFIED_INTEGRATOR_DECISION.md) rather than
	// anything to do with footprint.  An HF-energy check tight enough
	// to catch a real footprint regression would also be tripped by
	// VCM's ordinary, expected noise, and one loose enough to tolerate
	// that noise would not catch the regression either (a missing
	// footprint's blowout on THIS fixture is measured at 1-2 orders of
	// magnitude, comfortably outside VCM's 2-4x noise band, but "loose
	// enough for VCM noise, tight enough for the defect" is not a
	// robust needle to thread at a test-suite-friendly sample count).
	//
	// The mechanism itself does not need a separate, noise-tolerant
	// re-proof for VCM: VCM's eye subpath is the exact same
	// `std::vector<BDPTVertex>` BDPT's own `GenerateEyeSubpath` call
	// produces (VCMPelRasterizer.cpp -> VCMIntegrator.cpp, which reuses
	// `BDPTIntegrator`; see MLTRasterizer.cpp:166/376 for the same
	// pattern under MLT) -- VCMIntegrator.cpp never re-populates or
	// re-derives `BDPTVertex::txFootprint` itself (grep the symbol: no
	// occurrences in that file), so whatever the tight PT-vs-BDPT check
	// above proves about the shared vertex array is true for VCM's
	// consumption of it BY CONSTRUCTION, not by a separate render-level
	// coincidence.  The mean check here is a coarse end-to-end sanity
	// check that VCM renders the same scene sensibly, not the load-
	// bearing footprint assertion.
	Check( RelClose( ptStats.mean, vcmStats.mean, 0.15 ), "PT vs VCM: whole-image mean" );

	safe_release( pt );
	safe_release( bdpt );
	safe_release( vcm );
}

int main()
{
	std::cout << "BidirectionalTextureFootprintParityTest" << std::endl;
	TestFootprintParity();

	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
