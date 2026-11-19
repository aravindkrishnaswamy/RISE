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
//  true, so the eye subpath's FIRST hit -- every ray after the first
//  scattering bounce carries no differentials at all, by design,
//  everywhere in this renderer -- gets the identical, non-zero
//  footprint PT's own primary ray gets.  (Since 2026-11-17 a second
//  record carries one too: the light-traced copy described below.)
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
//  Rendered under `pathtracing_pel_rasterizer` (64 spp: a point light
//  and deterministic NEE make PT noise-free here), and
//  `bdpt_pel_rasterizer` and `vcm_pel_rasterizer` at 1024 spp
//  (`oidn_denoise FALSE`, `pixel_filter box`, a single point light --
//  no dependence on VCM's directional-light gap, which is unrelated).
//  Two kinds of vertex carry a footprint here: the depth-0 eye vertex
//  (the camera ray's differentials, DL-14) and, since 2026-11-17, the
//  copy of a surface light endpoint a light-traced (t == 1) connection
//  shades, stamped with the footprint of the pixel it lands in
//  (PathVertexEval::WithPixelFootprint; docs/DL14_..., section 10; this
//  fixes DL-636, pending review, which had pinned BDPT at
//  max_light_depth 0 instead).
//
//    (1) PT vs BDPT, CONVERGED: whole-image mean within 0.1% and
//        whole-image HF energy within 25%.
//    (2) PT vs VCM, CONVERGED: mean within 0.1%, HF energy within 50%.
//        VCM has its own light-traced splat site, so it needs its own
//        check rather than inheriting BDPT's.
//
//  Both the mean and the HF energy move when the footprint is wrong:
//  an unfiltered share mixes un-faded fbm octaves into the image.  The
//  mean check is the sharp one -- the footprint is the only defect this
//  scene was seen to move it by (any other MIS bias would move it too, and
//  should fail it) -- while the HF bands absorb the light-traced share's
//  own noise.
//
//  RED-PROOF (1024 spp).  Pre-fix (no t == 1 stamp): BDPT mean +0.32%,
//  HF 1.70x; VCM mean +0.23%, HF 1.80x.  Dropping the eye ray's
//  differentials (`currentRay.hasDifferentials = false;` after
//  `Ray currentRay = cameraRay;` in GenerateEyeSubpathImpl): BDPT mean
//  -0.41%, HF 2.33x.  Reverting VCMIntegrator.cpp's stamp alone fails
//  the VCM mean check (+0.23%).
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
#include "TestTempDir.h"

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
	char path[1024];
	std::snprintf( path, sizeof(path),
		"%s/dl14_footprint_parity_%s_%d.RISEscene", RISE::TestSupport::ScratchBaseDir().c_str(),
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
// BDPT renders at 1024 spp (DL-402): see the comparison block in
// TestFootprintParity for why its checks compare converged quantities.
static const char* kBdptRasterizer =
	"bdpt_pel_rasterizer\n{\n\tmax_eye_depth\t\t2\n\tmax_light_depth\t\t2\n\tsamples\t\t\t1024\n"
	"\toidn_denoise\t\tFALSE\n\tpixel_filter\t\tbox\n}\n";
// VCM also renders at 1024 spp: its splat (t == 1) site has its own copy of
// the footprint stamp (VCMIntegrator.cpp), so it needs its own converged check.
static const char* kVcmRasterizer =
	"vcm_pel_rasterizer\n{\n\tmax_eye_depth\t\t2\n\tmax_light_depth\t\t2\n\tsamples\t\t\t1024\n"
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

	// (1) PT vs BDPT, CONVERGED.  Since DL-402 (6dec90813, whole-film
	// camera density in BDPT's MIS) the light-tracing (t == 1) strategy
	// carries a real share of this point-lit floor.  That share is (a)
	// genuinely noisier than PT, which is noise-free here (a point light,
	// deterministic NEE: its 64- and 1024-spp HF energies agree to five
	// digits), and (b) was shaded with NO pixel footprint -- a light
	// vertex was never traced from the camera -- so it mixed the
	// unfiltered fbm into the filtered one.  (b) is a bias, not noise:
	// at 1024 spp BDPT read mean +0.32% and HF energy 1.70x PT's, and a
	// t == 1 connection now shades its surface point with the footprint
	// of the pixel it lands in (PathVertexEval::StampPixelFootprint),
	// after which BDPT reads mean 0.000% and HF 1.18x.  The remaining HF
	// excess is (a): at 4096 spp it reads 1.042x, shrinking ~4x for 4x the
	// samples as a variance term does.  So these checks compare converged quantities
	// at 1024 spp: the mean tightly (a footprint or any other bias moves
	// it), the HF energy within 25%.  The eye footprint
	// itself (DL-14's original subject) is still pinned: dropping the
	// eye ray's differentials moves both by more than either band.
	Check( RelClose( ptStats.mean,     bdptStats.mean,     0.001 ), "PT vs BDPT: whole-image mean (converged, 0.1%)" );
	Check( RelClose( ptStats.hfEnergy, bdptStats.hfEnergy, 0.25 ), "PT vs BDPT: whole-image HF energy (converged)" );

	// (2) PT vs VCM, CONVERGED (1024 spp).  VCM shares BDPT's eye subpath
	// (the depth-0 footprint) but has its OWN light-traced splat site
	// (VCMIntegrator.cpp SplatLightSubpathToCameraImpl), which needs the
	// same pixel-footprint stamp; its balance-heuristic MIS gives that
	// strategy a large share here.  Measured at 1024 spp: without the
	// stamp VCM read mean +0.23% and HF 1.80x PT's, with it +0.001% and
	// 1.32x -- so much of VCM's long-documented HF excess on this fixture
	// was the unfiltered light-traced share, not noise; the remainder is
	// noise (1.080x at 4096 spp).  The mean is held as tightly as
	// BDPT's; the HF band is 50% (BDPT's is 25%) because VCM is noisier
	// than BDPT at matched spp.
	Check( RelClose( ptStats.mean,     vcmStats.mean,     0.001 ), "PT vs VCM: whole-image mean (converged, 0.1%)" );
	Check( RelClose( ptStats.hfEnergy, vcmStats.hfEnergy, 0.50 ), "PT vs VCM: whole-image HF energy (converged)" );

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
