//////////////////////////////////////////////////////////////////////
//
//  RefractiveRadianceScalingTest.cpp - Closed-form guard on the
//    eta^2 basic-radiance factor at dielectric interfaces (debt 30).
//
//    THE PHYSICS.  Radiance is not invariant across a smooth interface
//    between media of different refractive index; L / n^2 is (the
//    "basic radiance", Preisendorfer 1965; PBRT-v4 9.5.2 / Veach 1997
//    5.2).  A radiance-mode walk -- one that starts at a camera and
//    gathers radiance -- must therefore multiply its throughput by
//    (eta_before / eta_after)^2 every time the scattered ray's medium
//    changes.  Camera in air entering water: x 1/1.33^2.  Camera in
//    water exiting to air: x 1.33^2.  An importance-mode walk (a light
//    subpath, a photon tracer) gets NO factor -- that asymmetry IS the
//    non-symmetry of refractive scattering.
//
//    WHAT WAS BROKEN (found 2026-09-12, debt 30).  RISE applied the
//    factor NOWHERE.  `DielectricSPF::GenerateScatteredRay` sets
//    kray = (1 - Fresnel) * tau^dist and no consumer added an eta term.
//    The defect hid for years because, with the camera AND the light
//    both in air, an eye path that enters water (should be x 1/n^2) and
//    later exits it toward the emitter (should be x n^2) nets x1 -- and
//    a BSDF-sampled emitter seen through the interface has its hit
//    probability compressed by 1/n^2 while its radiance is not boosted,
//    so the irradiance is under by n^2 and the viewing over by n^2.
//    Two errors cancelling.  Strategies whose LIGHT side is flux-based
//    never got the cancellation -- VCM merges, photon maps, BDPT
//    light-tracing splats, transparent-shadow NEE -- so they read n^2
//    too bright relative to PT/BDPT whenever the camera sat outside the
//    medium the vertex was in.  That n^2 = 1.77 discrepancy is what the
//    ledger recorded as "VCM 1.53x over PT on a submerged floor".
//
//    WHY THE CLOSED FORMS HERE ARE REFERENCE-FREE.  Rows A and B do not
//    compare one integrator against another; they compare against an
//    analytically known number.  Prior debt-30-adjacent investigations
//    (docs/CAUSTIC_PHOTONMAP_NORMALIZATION.md 9-11) burned four sessions
//    on references that could not reach, or contaminated, the quantity
//    under test.  A Lambertian luminaire viewed through one flat delta
//    interface has no such escape hatch.
//
//    ROWS
//      A. Submerged Lambertian luminaire (exitance 1 => L = 1/pi)
//         inside a water box, pinhole camera in air directly above at
//         near-normal incidence.  Every eye path is
//         camera -> (one delta refraction) -> emitter.
//           mean == T * L / n^2   exactly.
//         Run at n = 1.33 (factor 1.7689) AND n = 1.5 (factor 2.25) so a
//         factor applied in the WRONG DIRECTION, or a hard-coded
//         constant, cannot pass both.  Four rasterizers: PT, BDPT, VCM,
//         and the legacy `pixelpel_rasterizer` (DefaultRefraction's
//         kray consumer is a separate code path from the integrators').
//
//      B. Camera INSIDE a closed dielectric box (ior 1.5), uniform
//         environment of radiance 1 outside.  This is topology J of
//         tests/EnvLightBalanceTest.cpp with ior 1.0 -> 1.5, i.e. the
//         EXIT direction of the factor.
//           mean == n^2 == 2.25   exactly.
//         The closed form is n^2 and not n^2*T because the box is
//         lossless: the Fresnel-reflected fraction R bounces between the
//         parallel faces and escapes later at the same incidence, so the
//         series is n^2*L*T*(1 + R + R^2 + ...) = n^2*L*T/(1-T) ... more
//         precisely T/(1-R) = 1, giving exactly n^2*L.  That is the
//         textbook equilibrium statement: the radiance inside a medium of
//         index n in equilibrium with an external field of radiance L is
//         n^2 L.  Independent of the Fresnel curve, which makes this row
//         immune to any disagreement about RISE's Fresnel formulation.
//
//      C. Submerged Lambertian floor, TINY SPHERE EMITTER in air, camera
//         in air -- the row where VCM's merge and PT's eye path
//         disagreed.  Cross-integrator (no closed form: the refracted
//         irradiance on the floor has no elementary expression), so it
//         asserts AGREEMENT: VCM within 8% of PT, BDPT within 8% of PT.
//         Pre-fix VCM/PT = 1.149 on this geometry (1.55 on the
//         supervisor's r=0.03 probe, where merges carry more of the
//         MIS mixture).
//
//         DL-308 (2026-09-28): BDPT read 0.905-0.922 of PT here from
//         the DL-210 merge on.  Not transport: this row ran BDPT/VCM at
//         max_eye_depth 5, which since DL-210 admits exactly one floor
//         bounce under the water's total internal reflection, against an
//         untruncated PT.  Rows C and D now run BDPT/VCM at kSlabDepth
//         (16) and row C's band is 5% (was 8%); row E is the closed form
//         that decided which side was right.
//
//      D. Same slab with a DELTA omni light.  PT and BDPT are exactly 0
//         here (no strategy can connect through a delta interface to a
//         delta light -- structural, not a bug), so the two estimators
//         that CAN see it are transparent-shadow PT (a straight shadow
//         ray that ignores refraction entirely) and VCM (merges).  They
//         must agree to within the transparent-shadow approximation.
//         Band set from measurement, see the row.
//
//      E. (DL-308) Row C's direct term in CLOSED FORM: a small
//         Lambertian patch under the water surface, looked at straight
//         down, lit by a sphere emitter in air; the refracted irradiance
//         is integrated numerically in the test.  PT, BDPT, VCM and a
//         gathering legacy pixelpel chain against it.
//
//      F. (DL-308) Reference-free scale invariance of row E's scene:
//         every index x1.5 inside an ideal index-1.5 enclosure (a black
//         room around everything) must render the same image.
//
//    RED-PROOF -- this file's own run on the UNFIXED library at
//    b6c12301, seed base 1000, 18 passed / 12 FAILED:
//
//      A ior 1.33, closed form 0.176338:
//        PT       mean 0.311782   1.768x   FAIL
//        BDPT     mean 0.311782   1.768x   FAIL
//        VCM      mean 0.311979   1.769x   FAIL
//        pixelpel mean 0.311924   1.769x   FAIL
//      A ior 1.50, closed form 0.135812:
//        PT       mean 0.305177   2.247x   FAIL
//        BDPT     mean 0.305177   2.247x   FAIL
//        VCM      mean 0.305674   2.251x   FAIL
//        pixelpel mean 0.305577   2.250x   FAIL
//      B closed form 2.25:
//        PT       mean 1.00000    0.4444x  FAIL
//        BDPT     mean 1.00000    0.4444x  FAIL
//        VCM      mean 1.00001    0.4444x  FAIL
//      C PT 0.00464271, BDPT/PT 1.014, VCM/PT 1.149   VCM FAIL
//      D PT-opaque 0, PT-ts 0.716394, VCM 0.711239,
//        PT-ts/VCM 1.007                              pass
//
//    Every row A / row B failure is the missing factor read straight
//    off the closed form: exactly n^2 too bright looking IN, exactly
//    1/n^2 too dim looking OUT.  Row C is the mixed case -- only VCM's
//    merge share of the MIS mixture carries the error, so the ratio
//    lands between 1 and n^2 rather than at n^2.  Rows C and D are
//    Monte Carlo, so their figures are a shape, not a bit-exact
//    expectation.
//
//    SEEDING.  Like tests/SignalEmitterRecordTest.cpp, this binary does
//    not go through commandconsole.cpp's `srand( GetMilliseconds() )`,
//    so RISE renders here are seeded by the unsynchronized libc `rand()`
//    race across worker threads.  `std::srand( g_seedBase + g_renderIndex++ )`
//    runs immediately before every `Rasterize()`; argv[1] overrides the
//    base for an independent sample.  Since DL-308 every render also
//    salts the Sobol value scramble from the same index
//    (SobolSamplerTestHooks::ValueSalt), so a different argv[1] is an
//    independent randomized-QMC replicate rather than the identical
//    Sobol' points under a different libc seed.
//
//    See docs/REFRACTIVE_RADIANCE_SCALING.md for the derivation, the
//    site table, and the list of scene classes whose look changes.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <limits>
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
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- the in-process capture the balance
// tests use.  Reads the raw linear radiance buffer, no file I/O, no
// LDR encode.
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
	double max[3];
	bool   valid;
};

//! Mean of the three channels of `mean` -- every scene in this file is
//! achromatic by construction, so the grey mean is the signal and a
//! per-channel split would only add noise to the printout.
static double GreyMean( const ImageStats& s )
{
	return ( s.mean[0] + s.mean[1] + s.mean[2] ) / 3.0;
}

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	// Composite over black (base * coverage alpha) -- PT and BDPT resolve
	// partial-coverage pixels with different alpha conventions, and
	// base*alpha is the convention-independent quantity.  See the same
	// comment in tests/BDPTStrategyBalanceTest.cpp.
	for( int c = 0; c < 3; c++ ) { s.mean[c] = 0; s.max[c] = 0; }
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		const double v[3] = { c.base.r * cov, c.base.g * cov, c.base.b * cov };
		// DL-40: a nonfinite (NaN/Inf) composited component is a broken
		// render, not a statistic -- reject the whole capture before it
		// reaches sum/max.  Without this, "if( v[ch] > s.max[ch] )"
		// silently discards a NaN v[ch] (NaN > s.max[ch] is always false).
		if( !std::isfinite( v[0] ) || !std::isfinite( v[1] ) || !std::isfinite( v[2] ) ) {
			return ImageStats{};   // valid stays false
		}
		for( int ch = 0; ch < 3; ch++ ) {
			s.mean[ch] += v[ch];
			if( v[ch] > s.max[ch] ) s.max[ch] = v[ch];
		}
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] /= double( cap.pixels.size() );
	s.valid = true;
	return s;
}

//////////////////////////////////////////////////////////////////////
// DL-40 red-proof: ComputeStats must reject a capture containing a
// nonfinite composited component (return valid==false) rather than
// silently folding NaN/Inf into the mean/max.  Explicit malformed
// CapturingRasterizerOutput fixtures, not a live render.
//////////////////////////////////////////////////////////////////////
static void TestNonfiniteCandidateRejected()
{
	std::cout << std::endl << "-- DL-40: nonfinite candidate statistics are rejected --" << std::endl;

	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 2; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		cap->pixels.push_back( RISEColor( RISEPel( std::nan(""), 0.2, 0.2 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with a NaN pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.3, std::numeric_limits<double>::infinity(), 0.3 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with an Inf pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.4, 0.5, 0.6 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( s.valid, "DL-40: ComputeStats control -- an all-finite capture stays valid" );
		cap->release();
	}
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/refractive_radiance_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

//! Per-render Sobol VALUE salt (DL-308).  Without it every render of one
//! scene reuses the IDENTICAL Sobol' points (the pixel seed is a function
//! of the pixel alone), so argv[1] changed nothing but libc rand() and a
//! repeat could not tell a real offset from the point set's own QMC error.
//! Salting makes every render an independent randomized-QMC replicate,
//! exactly as tests/MediumInsideOutsideInvariantTest.cpp does.
static const uint32_t kSaltTag = 0x308u;

//! Render one scene and composite its mean.  `salt` is the Sobol value
//! salt for this render; `seedCheck`, when non-null, is filled with the
//! IOR-stack top `IORStackSeeding::SeedFromPoint` seeds at `seedPoint`
//! (row F asserts the enclosure really is the camera's exterior).
static ImageStats RenderWithSalt(
	const std::string& sceneText, const char* tag, const uint32_t salt,
	const Point3* seedPoint = nullptr, double* seedCheck = nullptr )
{
	ImageStats result{};

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) {
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( scenePath.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	SobolSamplerTestHooks::ValueSalt().store( salt );
	std::srand( g_seedBase + g_renderIndex++ );
	const bool bRendered = pJob->Rasterize();
	SobolSamplerTestHooks::ValueSalt().store( 0u );
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	// After the render, so the scene's acceleration structure is already
	// built (probing first builds it lazily and logs a warning).
	if( seedPoint && seedCheck && pJob->GetScene() ) {
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, *seedPoint, *pJob->GetScene() );
		*seedCheck = stack.top();
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( scenePath.c_str() );
	return result;
}

//! The ordinary per-render entry point: a fresh salt per render, derived
//! from the seed base and the running render index.
static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
{
	const uint32_t salt = SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag );
	return RenderWithSalt( sceneText, tag, salt );
}

//////////////////////////////////////////////////////////////////////
// Rasterizer strings.
//
// Every one of them sets `oidn_denoise FALSE` and `pixel_filter box`:
// the suite must measure the integrator, not OIDN's CNN (the trap that
// made tests/EnvLightBalanceTest.cpp measure the denoiser for months --
// see its topology J block) and not a filter's reconstruction.
//////////////////////////////////////////////////////////////////////

static std::string RasterizerPT( const char* samples, bool transparentShadows = false )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " ) + samples + "\n"
		+ ( transparentShadows ? "\ttransparent_shadows TRUE\n" : "" ) +
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"}\n";
}

static std::string RasterizerBDPT( const char* samples, unsigned int depth = 5 )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"bdpt_pel_rasterizer\n{\n"
		"\tmax_eye_depth " ) + std::to_string( depth ) + "\n"
		"\tmax_light_depth " + std::to_string( depth ) + "\n"
		"\tsamples " + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"}\n";
}

static std::string RasterizerVCM( const char* samples, unsigned int depth = 5, const char* mergeRadius = "0.0" )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"vcm_pel_rasterizer\n{\n"
		"\tmax_eye_depth " ) + std::to_string( depth ) + "\n"
		"\tmax_light_depth " + std::to_string( depth ) + "\n"
		"\tsamples " + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tmerge_radius " + mergeRadius + "\n"
		"\tvc_enabled true\n"
		"\tvm_enabled true\n"
		"}\n";
}

//! Legacy shader-dispatch pipeline.  DefaultRefraction is the shader op
//! that consumes `ScatteredRay::kray` for the transmission lobe -- a
//! separate consumer from PathTracingIntegrator's, which is exactly why
//! row A runs it.
static std::string RasterizerPixelPel( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n"
		"\tshaderop DefaultEmission\n"
		"\tshaderop DefaultDirectLighting\n"
		"\tshaderop DefaultRefraction\n"
		"\tshaderop DefaultReflection\n}\n\n"
		"pixelpel_rasterizer\n{\n\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"}\n";
}

//! Env-lit PT: topology B needs the uniform radiance map wired in and
//! visible to escaping camera rays.
static std::string RasterizerPTEnv( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n";
}

static std::string RasterizerBDPTEnv( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"bdpt_pel_rasterizer\n{\n"
		"\tmax_eye_depth 5\n\tmax_light_depth 5\n"
		"\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n";
}

static std::string RasterizerVCMEnv( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"vcm_pel_rasterizer\n{\n"
		"\tmax_eye_depth 5\n\tmax_light_depth 5\n"
		"\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"\tmerge_radius 0.0\n"
		"\tvc_enabled true\n"
		"\tvm_enabled true\n"
		"}\n";
}

//////////////////////////////////////////////////////////////////////
// Row A -- submerged Lambertian luminaire seen from air.
//
// GEOMETRY.  Emitting quad at y = 0.05 spanning [-1,1] in x and z,
// inside a 4 x 0.3 x 4 water box whose top face is y = 0.3.  Pinhole
// camera at y = 2.5, fov 15 deg, looking straight down (`location`
// carries a 0.001 z offset so `lookat` and `up` are not parallel).  At
// fov 15 the largest incidence angle on the interface is ~10.6 deg
// (image diagonal), where the Fresnel transmittance differs from its
// normal-incidence value by R(10.6)-R(0) = 1.31e-5 at ior 1.33 and
// 1.96e-5 at ior 1.5 (exact unpolarized Fresnel, the same formula as
// Optics::CalculateDielectricReflectance) -- roughly three orders of
// magnitude (1020x-1522x) below the 2% band -- so T(0) is the right
// closed form for the whole frame.
//
// The camera sees ONLY the emitter through the top face: the quad's
// +-1 extent subtends far more than the +-0.33 the fov reaches at
// y=0.05.  The Fresnel-reflected lobe at the top face leaves upward
// into an unlit background and contributes exactly 0, so there is no
// internal series to sum here -- unlike row B.
//////////////////////////////////////////////////////////////////////
static std::string SceneSubmergedLuminaire( const char* ior )
{
	return std::string(
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0.0 2.5 0.001\n"
		"\tlookat 0 0 0\n"
		"\tup 0 0 1\n"
		"\tfov 15.0\n"
		"}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -1 0.05 1\n\tptb 1 0.05 1\n\tptc 1 0.05 -1\n\tptd -1 0.05 -1\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n"
		"dielectric_material\n{\n\tname mat_water\n\tior " ) + ior + "\n"
		"\ttau 1.0\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_water\n\twidth 4\n\theight 0.3\n\tdepth 4\n}\n\n"
		"standard_object\n{\n\tname water\n\tgeometry geo_water\n"
		"\tmaterial mat_water\n\tposition 0 0.15 0\n}\n";
}

//! Normal-incidence Fresnel reflectance between air and `n`.
static double R0( double n )
{
	const double r = ( n - 1.0 ) / ( n + 1.0 );
	return r * r;
}

static void RunRowA( const char* ior, double n )
{
	const double L        = 1.0 / 3.14159265358979323846;	// exitance 1 => L = M/pi
	const double expected = ( 1.0 - R0( n ) ) * L / ( n * n );

	std::cout << "Row A: submerged Lambertian luminaire, ior " << ior
	          << "  (closed form T*L/n^2 = " << expected << ")" << std::endl;

	const std::string common = SceneSubmergedLuminaire( ior );
	const std::string head( "RISE ASCII SCENE 7\n" );

	struct Row { const char* name; std::string scene; };
	const Row rows[] = {
		{ "PT",       head + RasterizerPT( "16" )       + common },
		{ "BDPT",     head + RasterizerBDPT( "16" )     + common },
		{ "VCM",      head + RasterizerVCM( "32" )      + common },
		{ "pixelpel", head + RasterizerPixelPel( "4" )  + common },
	};

	for( const Row& r : rows ) {
		const ImageStats s = RenderAndComputeStats( r.scene, "rowA" );
		const std::string label = std::string( "row A ior " ) + ior + " " + r.name;
		Check( s.valid, label + ": render produced output" );
		if( !s.valid ) continue;
		const double m = GreyMean( s );
		std::cout << "    " << r.name << "  mean=" << m
		          << "  ratio-to-closed-form=" << ( m / expected ) << std::endl;
		Check( std::fabs( m - expected ) <= 0.02 * expected,
			label + ": mean == T*L/n^2 within 2%" );
	}
}

//////////////////////////////////////////////////////////////////////
// Row A -- PerfectRefractorSPF variant (review round 3, C3).
//
// Same geometry and closed form as row A above; `mat_water` swapped
// from `dielectric_material` to `perfectrefractor_material`.  That
// chunk has no `tau` / `scattering` knobs (PerfectRefractorSPF has no
// attenuation or rough-transmission path at all -- it is a pure delta
// refractor), so `refractance` is bound to an explicit white painter to
// match row A's `tau 1.0`; the chunk's own default for `refractance` is
// "none", which resolves to a BLACK uniformcolor_painter
// (Job::InitializeContainers), not white -- binding it explicitly
// avoids a silently-dark scene.
//
// This row is GREEN on the current (already-fixed) library: it does
// not exercise new code.  `PerfectRefractorSPF::DoSingleRGBComponent`
// pushes/pops the IOR stack the exact same way
// `DielectricSPF::GenerateScatteredRay` does (docs/
// REFRACTIVE_RADIANCE_SCALING.md section 6.1's `PerfectRefractorSPF.cpp`
// row: "yes, via the consumer"), and the eta^2 factor is applied
// entirely at the consumer sites (PathTracingIntegrator.cpp /
// BDPTIntegrator.cpp / RefractionShaderOp.cpp), which read the factor
// off the two IORStacks the SPF handed them and do not distinguish
// which SPF produced the ray.  So this row pins a SECOND producer
// through the already-fixed consumer path, not a second code path.
//////////////////////////////////////////////////////////////////////
static std::string SceneSubmergedLuminairePerfectRefractor( const char* ior )
{
	return std::string(
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0.0 2.5 0.001\n"
		"\tlookat 0 0 0\n"
		"\tup 0 0 1\n"
		"\tfov 15.0\n"
		"}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -1 0.05 1\n\tptb 1 0.05 1\n\tptc 1 0.05 -1\n\tptd -1 0.05 -1\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"perfectrefractor_material\n{\n\tname mat_water\n\trefractance pnt_white\n\tior " ) + ior + "\n}\n\n"
		"box_geometry\n{\n\tname geo_water\n\twidth 4\n\theight 0.3\n\tdepth 4\n}\n\n"
		"standard_object\n{\n\tname water\n\tgeometry geo_water\n"
		"\tmaterial mat_water\n\tposition 0 0.15 0\n}\n";
}

static void RunRowAPerfectRefractor( const char* ior, double n )
{
	const double L        = 1.0 / 3.14159265358979323846;	// exitance 1 => L = M/pi
	const double expected = ( 1.0 - R0( n ) ) * L / ( n * n );

	std::cout << "Row A (perfectrefractor_material): submerged Lambertian luminaire, ior " << ior
	          << "  (closed form T*L/n^2 = " << expected << ")" << std::endl;

	const std::string common = SceneSubmergedLuminairePerfectRefractor( ior );
	const std::string head( "RISE ASCII SCENE 7\n" );

	struct Row { const char* name; std::string scene; };
	const Row rows[] = {
		{ "PT",       head + RasterizerPT( "16" )       + common },
		{ "BDPT",     head + RasterizerBDPT( "16" )     + common },
		{ "VCM",      head + RasterizerVCM( "32" )      + common },
		{ "pixelpel", head + RasterizerPixelPel( "4" )  + common },
	};

	for( const Row& r : rows ) {
		const ImageStats s = RenderAndComputeStats( r.scene, "rowA_pr" );
		const std::string label = std::string( "row A-PR ior " ) + ior + " " + r.name;
		Check( s.valid, label + ": render produced output" );
		if( !s.valid ) continue;
		const double m = GreyMean( s );
		std::cout << "    " << r.name << "  mean=" << m
		          << "  ratio-to-closed-form=" << ( m / expected ) << std::endl;
		Check( std::fabs( m - expected ) <= 0.02 * expected,
			label + ": mean == T*L/n^2 within 2%" );
	}
}

//////////////////////////////////////////////////////////////////////
// Row B -- camera inside the dielectric, uniform env outside.
//
// tests/EnvLightBalanceTest.cpp topology J with ior 1.0 -> 1.5.  That
// topology stays at ior 1.0 on purpose (its closed form is exactly 1.0
// and must stay bit-exact); this row is its refracting sibling and
// pins the EXIT direction of the factor, which row A cannot reach.
//
// CLOSED FORM n^2 = 2.25, NOT n^2*T -- see the file header.  Measured
// (re-run for this round): PT mean=2.25 (ratio 1.00000), BDPT
// mean=2.25 (ratio 1.00000), VCM mean=2.25002 (ratio 1.00001) -- all
// three read n^2 to within 0.001%, not "a few tenths of a percent
// under" as an earlier draft of this comment claimed.  The 3% band is
// generous headroom for the integrator's depth/RR truncation of the
// internal series and lateral escape from a 4x4x4 box at these angles,
// not a correction the measurement is actually using; it is still 20x
// tighter than the 2.25x error the missing factor produces.
//////////////////////////////////////////////////////////////////////
static const char* kSceneSubmergedCameraIor15 =
	"film\n{\n\twidth 32\n\theight 32\n}\n\n"
	"pinhole_camera\n{\n"
	"\tlocation 0 0 0\n"
	"\tlookat 0 0 -1\n"
	"\tup 0 1 0\n"
	"\tfov 20.0\n"
	"}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"dielectric_material\n{\n"
	"\tname mat_shell\n\ttau 1.0 1.0 1.0\n\tior 1.5\n\tscattering 1000000.0\n}\n\n"
	"box_geometry\n{\n\tname shell_box\n\twidth 4.0\n\theight 4.0\n\tdepth 4.0\n}\n\n"
	"standard_object\n{\n\tname obj_shell\n\tgeometry shell_box\n\tmaterial mat_shell\n}\n";

static void RunRowB()
{
	const double n        = 1.5;
	const double expected = n * n;

	std::cout << "Row B: submerged camera, ior 1.5, uniform env L=1"
	          << "  (closed form n^2 = " << expected << ")" << std::endl;

	const std::string common( kSceneSubmergedCameraIor15 );
	const std::string head( "RISE ASCII SCENE 7\n" );

	// The scene body has to come FIRST here: `radiance_map pnt_env` is
	// resolved by name at the moment the rasterizer chunk is finalized, so
	// the painter chunk must already have been parsed.  Rasterizer-first
	// ordering (fine for rows A/C/D, which reference nothing) silently
	// yields "Global Radiance Map painter not found" and a black frame.
	struct Row { const char* name; std::string scene; };
	const Row rows[] = {
		{ "PT",   head + common + RasterizerPTEnv( "32" )   },
		{ "BDPT", head + common + RasterizerBDPTEnv( "32" ) },
		{ "VCM",  head + common + RasterizerVCMEnv( "32" )  },
	};

	for( const Row& r : rows ) {
		const ImageStats s = RenderAndComputeStats( r.scene, "rowB" );
		const std::string label = std::string( "row B " ) + r.name;
		Check( s.valid, label + ": render produced output" );
		if( !s.valid ) continue;
		const double m = GreyMean( s );
		std::cout << "    " << r.name << "  mean=" << m
		          << "  ratio-to-closed-form=" << ( m / expected ) << std::endl;
		Check( std::fabs( m - expected ) <= 0.03 * expected,
			label + ": mean == n^2 within 3%" );
	}
}

//////////////////////////////////////////////////////////////////////
// Rows C and D -- the submerged-floor slab.
//
// Lambertian floor (rho = 0.5) at y = 0.05 inside a water box
// y in [0, 0.3]; camera in AIR at (2, 2.5, 0) looking at the origin.
// Row C lights it with a small sphere emitter in air at (0, 2.5, 0);
// row D with a delta omni at the same place.
//
// There is no elementary closed form for the irradiance a point source
// delivers through a flat refracting interface to a point at depth, so
// these rows assert cross-integrator AGREEMENT, which is exactly the
// property the missing factor broke: the eye path (radiance) and the
// merge / transparent shadow ray (flux) disagreed by n^2.
//////////////////////////////////////////////////////////////////////
static const char* kSlabBody =
	"film\n{\n\twidth 64\n\theight 64\n}\n\n"
	"pinhole_camera\n{\n"
	"\tlocation 2.0 2.5 0\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n"
	"\tpta -1 0.05 1\n\tptb 1 0.05 1\n\tptc 1 0.05 -1\n\tptd -1 0.05 -1\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
	"dielectric_material\n{\n\tname mat_water\n\tior 1.33\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
	"box_geometry\n{\n\tname geo_water\n\twidth 2.2\n\theight 0.3\n\tdepth 2.2\n}\n\n"
	"standard_object\n{\n\tname water\n\tgeometry geo_water\n"
	"\tmaterial mat_water\n\tposition 0 0.15 0\n}\n";

//! Row C light: a small sphere emitter.  Radius 0.08 rather than the
//! 0.03 of the supervisor's probe -- at 0.03 the frame carries visible
//! specular fireflies (max ~2.0 against a mean of 0.005) that make the
//! mean itself a noisy statistic.  Total power is held constant by
//! scaling the exitance with 1/r^2 (0.03^2/0.08^2 * 300 = 42.2).
static const char* kSlabLightArea =
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n"
	"\tname mat_emit\n\texitance pnt_emit\n\tscale 42.2\n\tmaterial none\n}\n\n"
	"sphere_geometry\n{\n\tname geo_emit\n\tradius 0.08\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n"
	"\tmaterial mat_emit\n\tposition 0 2.5 0\n}\n";

static const char* kSlabLightDelta =
	"omni_light\n{\n\tname light\n\tpower 30.0\n\tcolor 1.0 1.0 1.0\n"
	"\tposition 0 2.5 0\n}\n";

//! DL-308: BDPT/VCM eye/light depth for rows C and D, where the floor's
//! light is trapped under the water surface by total internal reflection
//! and the path-space series runs deep.  Each extra floor bounce costs two
//! SURFACE vertices (the floor and the internal reflection at the top),
//! and since DL-210 (`e361fa45`) `max_eye_depth` really caps the RGB eye
//! walk at that many surface vertices -- before it the Pel walk ran up to
//! `max_volume_bounce` (64) further surface vertices past it.  At the 5
//! these rows used to share with row A, BDPT's eye walk admits exactly ONE
//! floor bounce: it read 0.905-0.922 of PT, and PT with
//! `max_diffuse_bounce 1` reads the same number (identity measured in
//! docs/REFRACTIVE_RADIANCE_SCALING.md section 11).  16 reads the same as
//! 32 within noise (8 is still 0.65% low); PT is untruncated.
static const unsigned int kSlabDepth = 16;

static void RunRowC()
{
	std::cout << "Row C: submerged floor, sphere emitter in air, camera in air"
	          << "  (cross-integrator agreement)" << std::endl;

	const std::string head( "RISE ASCII SCENE 7\n" );
	const std::string common = std::string( kSlabBody ) + kSlabLightArea;

	const ImageStats pt   = RenderAndComputeStats( head + RasterizerPT( "4096" )   + common, "rowC" );
	const ImageStats bdpt = RenderAndComputeStats( head + RasterizerBDPT( "2048", kSlabDepth ) + common, "rowC" );
	const ImageStats vcm  = RenderAndComputeStats( head + RasterizerVCM( "2048", kSlabDepth )  + common, "rowC" );

	Check( pt.valid && bdpt.valid && vcm.valid, "row C: all three renders produced output" );
	if( !pt.valid || !bdpt.valid || !vcm.valid ) return;

	const double mPT   = GreyMean( pt );
	const double mBDPT = GreyMean( bdpt );
	const double mVCM  = GreyMean( vcm );

	std::cout << "    PT   mean=" << mPT << std::endl;
	std::cout << "    BDPT mean=" << mBDPT << "  BDPT/PT=" << ( mBDPT / mPT ) << std::endl;
	std::cout << "    VCM  mean=" << mVCM  << "  VCM/PT ="  << ( mVCM  / mPT ) << std::endl;

	Check( mPT > 1e-6, "row C: PT mean is non-trivial" );
	// 5% band (DL-308, was 8%): salted single-render ratios at depth 16,
	// n = 8 each, read BDPT/PT 1.0044 (sd 1.18%) and VCM/PT 1.0063
	// (sd 1.35%), so the band sits >= 3.2 sd from either mean; the
	// depth-5 truncation it replaces reads -9.5% (BDPT) / -6.4% (VCM).
	Check( std::fabs( mBDPT - mPT ) <= 0.05 * mPT, "row C: BDPT within 5% of PT" );
	Check( std::fabs( mVCM  - mPT ) <= 0.05 * mPT, "row C: VCM within 5% of PT" );
}

static void RunRowD()
{
	std::cout << "Row D: submerged floor, DELTA omni in air"
	          << "  (transparent-shadow PT vs VCM)" << std::endl;

	const std::string head( "RISE ASCII SCENE 7\n" );
	const std::string common = std::string( kSlabBody ) + kSlabLightDelta;

	const ImageStats ptPlain = RenderAndComputeStats( head + RasterizerPT( "64" ) + common, "rowD" );
	const ImageStats ptTs    = RenderAndComputeStats(
		head + RasterizerPT( "512", /*transparentShadows*/ true ) + common, "rowD" );
	const ImageStats vcm     = RenderAndComputeStats( head + RasterizerVCM( "1024", kSlabDepth ) + common, "rowD" );

	Check( ptPlain.valid && ptTs.valid && vcm.valid, "row D: all three renders produced output" );
	if( !ptPlain.valid || !ptTs.valid || !vcm.valid ) return;

	const double mPlain = GreyMean( ptPlain );
	const double mTs    = GreyMean( ptTs );
	const double mVCM   = GreyMean( vcm );

	std::cout << "    PT (opaque shadows) mean=" << mPlain << std::endl;
	std::cout << "    PT (transparent)    mean=" << mTs << std::endl;
	std::cout << "    VCM                 mean=" << mVCM
	          << "  PTts/VCM=" << ( mVCM > 0 ? mTs / mVCM : 0.0 ) << std::endl;

	// Structural, not a bug: an opaque shadow ray through the delta water
	// surface kills every NEE connection, and no other PT strategy can
	// reach a delta light through a delta interface.  Pinned so a future
	// change that silently makes it non-zero is noticed.
	Check( mPlain <= 1e-6, "row D: PT without transparent shadows is exactly dark" );

	Check( mVCM > 1e-4, "row D: VCM mean is non-trivial" );
	// 15% band: the transparent-shadow estimator ignores refraction at the
	// interface entirely (it is a straight-line transmittance), so it
	// misses the refractive concentration of the source, which for this
	// geometry (source 2.2 above the surface, floor 0.25 below) is
	// ((h+d)/(h+d/n))^2 = 1.05.  VCM's merges carry the real refracted
	// geometry.  The band is sized around that 5% modelling gap plus MC
	// noise, and is 12x tighter than the n^2 = 1.77 error the missing
	// factor introduced (docs/REFRACTIVE_RADIANCE_SCALING.md section 4:
	// both estimators moved, PTts/VCM 0.929 -> 0.949, not just one of
	// them) -- this band is a forward-looking guard against an
	// ASYMMETRIC regression (e.g. a future change that re-applies the
	// factor to only one of the two flux-side estimators), not a record
	// of the historical bug having hit only one side.
	Check( std::fabs( mTs - mVCM ) <= 0.15 * mVCM, "row D: transparent-shadow PT within 15% of VCM" );
}

//////////////////////////////////////////////////////////////////////
// Row E -- DL-308: the submerged floor's DIRECT term in closed form.
//
// Row C can only assert agreement, and agreement cannot say which side
// is wrong -- DL-308 (BDPT 0.905-0.922 of PT) needed a number that does
// not come from any integrator.  This row shrinks row C's floor to a
// small Lambertian PATCH under a flat water surface, looked at straight
// down through it, lit by a sphere emitter in air.  Everything that is
// not the direct term is either absent or computed:
//
//   L_pixel = T_in(theta_cam) / n^2 * rho/pi * E(p)
//   E(p)    = integral over water directions of T(theta_w) n^2 L_e cos
//
// (the n^2 factors are docs/REFRACTIVE_RADIANCE_SCALING.md section 1's:
// radiance entering the water from the emitter is n^2 brighter, the
// floor's radiance leaving it for the camera n^2 dimmer).  E is
// integrated numerically over the refraction point q on the surface
// plane -- d(omega_w) = cos(theta_w) dA_q / |q - p|^2 -- with an exact
// ray-sphere test of the refracted air ray, so the refraction geometry
// (the emitter's image is displaced and compressed) is exact, not
// paraxial.  The camera footprint is averaged on a 16 x 16 grid through
// the same pinhole model PinholeCamera uses (square film, half-extent
// tan(fov/2)).
//
// What the closed form leaves out, and why it may:
//  - the floor re-lit by its own light reflected at the surface: the
//    patch (0.14 wide, 0.25 under the surface) is too small for a
//    total-internal-reflection return to land on it (a TIR return
//    travels >= 0.57 sideways), and the sub-critical Fresnel return is
//    COMPUTED (a first-order term, +0.024%, `SelfReturn` below);
//  - light passing beside the patch, reflecting off the water's bottom
//    face 2.05 below and coming back via a second surface reflection:
//    two Fresnel reflections (~1e-3) times the patch's solid angle from
//    the bottom, < 1e-4;
//  - the water box's side faces, 3 units away: nothing reaches the patch
//    through them.
// The camera's own Fresnel reflection off the surface goes up into an
// unlit sky: the emitter's mirror image sits at x ~ 0.4 on the surface,
// far outside the 2-degree field.
//
// MEASURED (salted independent renders, 32x32; ratio to the closed form
// 0.00351454, +/- the standard error of the mean):
//   PT       256 spp, n = 10   1.0024 +/- 0.0021
//   BDPT     512 spp, n = 10   1.0013 +/- 0.0020
//   VCM      512 spp, n = 10   1.0015 +/- 0.0020  (CONNECTIONS only)
//   pixelpel 4 spp x 128 gather samples, n = 6   0.9975 +/- 0.0050
// Every integrator carries the direct term through the interface
// correctly -- which is what attributes row C's old deficit to the eye
// DEPTH (kSlabDepth above) rather than to transport.  The VCM figure is
// its connection strategies alone: the auto merge-radius pre-pass lands
// 0-3 light segments on this small patch (it needs 8) and disables
// merging in every row-E render, so this row does NOT test VCM merging
// (row C does, against PT).  Bands: the mean of n = 4 renders within 2%.
// PT's per-render sd is 0.65-0.85% (0.85% at n = 12 in an independent
// review), i.e. 0.43% for the mean, and PT sits ~+0.27% high -- ~4 sd of
// margin; BDPT/VCM (sd 0.6%) have >= 6 sd.  One pixelpel render (sd 1.2%)
// within 5%.
//
// The legacy chain needs a gathering op to see this at all: the default
// DefaultDirectLighting chain is exactly dark here (its shadow ray is
// opaque to the delta water surface, row D's structural zero), so the
// pixelpel row runs DefaultEmission + a diffuse-only
// `distributiontracing_shaderop` + DefaultRefraction -- the op pair
// whose eta^2 consumers (DistributionTracingShaderOp, RefractionShaderOp)
// section 6.1 of the doc lists.  DefaultReflection is left out: with it
// every Fresnel reflection of a gather ray at the surface falls back on
// the patch and re-gathers, doubling the cost for the +0.024% term the
// closed form adds anyway.
//////////////////////////////////////////////////////////////////////
namespace RowE
{
	const double kPi      = 3.14159265358979323846;
	const double kN       = 1.33;		// water
	const double kYFloor  = 0.05;		// the patch
	const double kYSurf   = 0.3;		// the water surface
	const double kPatch   = 0.07;		// patch half-width
	const double kEmitX   = 0.8, kEmitY = 2.5, kEmitZ = 0.0;
	const double kEmitR   = 0.4;
	const double kEmitScale = 1.688;	// exitance => L_e = kEmitScale / pi
	const double kRho     = 0.5;
	const double kCamY    = 2.5, kCamZ = 0.001;
	const double kFovDeg  = 2.0;

	//! Unpolarized dielectric reflectance, the formula of
	//! Optics::CalculateDielectricReflectance (1 past the critical angle).
	double Fresnel( const double cosI, const double n1, const double n2 )
	{
		const double s2 = ( n1 / n2 ) * ( n1 / n2 ) * ( 1.0 - cosI * cosI );
		if( s2 >= 1.0 ) return 1.0;
		const double cosT = std::sqrt( 1.0 - s2 );
		const double rs = ( n1 * cosI - n2 * cosT ) / ( n1 * cosI + n2 * cosT );
		const double rp = ( n2 * cosI - n1 * cosT ) / ( n2 * cosI + n1 * cosT );
		return 0.5 * ( rs * rs + rp * rp );
	}

	//! Refraction point on the surface of the chief ray from floor point
	//! (px, pz) to the emitter centre: Snell in the vertical plane through
	//! both, solved by bisection on the horizontal offset.
	void ChiefPoint( const double px, const double pz, double& qx, double& qz )
	{
		const double dx = kEmitX - px, dz = kEmitZ - pz;
		const double D = std::sqrt( dx * dx + dz * dz );
		const double hw = kYSurf - kYFloor, ha = kEmitY - kYSurf;
		double lo = 0.0, hi = D;
		for( int i = 0; i < 100; i++ ) {
			const double s = 0.5 * ( lo + hi );
			const double f = kN * s / std::hypot( s, hw ) - ( D - s ) / std::hypot( D - s, ha );
			if( f > 0 ) hi = s; else lo = s;
		}
		const double s = 0.5 * ( lo + hi );
		qx = px + ( D > 0 ? s * dx / D : 0.0 );
		qz = pz + ( D > 0 ? s * dz / D : 0.0 );
	}

	//! Irradiance at floor point (px, pz), integrated over the refraction
	//! point q on the surface plane (midpoint rule, N x N over a box of
	//! half-width w about the chief point, widened until no border cell
	//! hits the emitter).  Returns a negative value if it cannot bracket.
	double Irradiance( const double px, const double pz )
	{
		const double Le = kEmitScale / kPi;
		const double hw = kYSurf - kYFloor;
		double qx0, qz0;
		ChiefPoint( px, pz, qx0, qz0 );
		const int N = 400;
		for( double w = 0.05; w < 1.0; w *= 1.5 ) {
			const double cell = 2.0 * w / N;
			double sum = 0.0;
			bool borderHit = false;
			for( int i = 0; i < N; i++ ) {
				const double qx = qx0 - w + ( i + 0.5 ) * cell;
				for( int j = 0; j < N; j++ ) {
					const double qz = qz0 - w + ( j + 0.5 ) * cell;
					const double vx = qx - px, vz = qz - pz;
					const double L2 = vx * vx + hw * hw + vz * vz;
					const double L = std::sqrt( L2 );
					const double cw = hw / L;
					// Refract into air: the horizontal component scales by n.
					const double hx = vx / L * kN, hz = vz / L * kN;
					const double h2 = hx * hx + hz * hz;
					if( h2 >= 1.0 ) continue;			// total internal reflection
					const double ay = std::sqrt( 1.0 - h2 );
					const double ox = qx - kEmitX, oy = kYSurf - kEmitY, oz = qz - kEmitZ;
					const double b = ox * hx + oy * ay + oz * hz;
					const double c = ox * ox + oy * oy + oz * oz - kEmitR * kEmitR;
					const double disc = b * b - c;
					if( disc <= 0.0 || -b - std::sqrt( disc ) <= 0.0 ) continue;
					const double T = 1.0 - Fresnel( cw, kN, 1.0 );
					sum += T * kN * kN * Le * cw * cw / L2;
					if( i == 0 || j == 0 || i == N - 1 || j == N - 1 ) borderHit = true;
				}
			}
			if( !borderHit ) {
				return sum * cell * cell;
			}
		}
		return -1.0;
	}

	//! First-order self-return: the patch's own radiance Fresnel-reflected
	//! back onto it by the surface above (unfolded: the patch's mirror
	//! image 2 hw above), relative to the direct irradiance.
	double SelfReturn()
	{
		const double h2 = 2.0 * ( kYSurf - kYFloor );
		const int N = 200;
		const double cell = 2.0 * kPatch / N;
		double sum = 0.0;
		for( int i = 0; i < N; i++ ) {
			const double x = -kPatch + ( i + 0.5 ) * cell;
			for( int j = 0; j < N; j++ ) {
				const double z = -kPatch + ( j + 0.5 ) * cell;
				const double d2 = x * x + z * z + h2 * h2;
				const double c = h2 / std::sqrt( d2 );
				sum += Fresnel( c, kN, 1.0 ) * c * c / d2;
			}
		}
		return kRho / kPi * sum * cell * cell;
	}

	//! The closed-form image mean (see the row header).  Fills
	//! `footprintInside` with whether every footprint sample landed on
	//! the patch (a geometry guard for the scene text below).
	double ClosedForm( bool& footprintInside )
	{
		footprintInside = true;
		const double t = std::tan( 0.5 * kFovDeg * kPi / 180.0 );
		// Camera frame: forward = lookat(0,0,0) - location, up (0,0,1).
		double fx = 0.0, fy = -kCamY, fz = -kCamZ;
		const double fl = std::sqrt( fx * fx + fy * fy + fz * fz );
		fx /= fl; fy /= fl; fz /= fl;
		// right = forward x up, up2 = right x forward.
		double rx = fy * 1.0 - fz * 0.0, ry = fz * 0.0 - fx * 1.0, rz = fx * 0.0 - fy * 0.0;
		const double rl = std::sqrt( rx * rx + ry * ry + rz * rz );
		rx /= rl; ry /= rl; rz /= rl;
		const double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
		const int G = 16;
		double sum = 0.0;
		for( int i = 0; i < G; i++ ) {
			for( int j = 0; j < G; j++ ) {
				const double u = ( ( i + 0.5 ) / G * 2.0 - 1.0 ) * t;
				const double v = ( ( j + 0.5 ) / G * 2.0 - 1.0 ) * t;
				double dx = fx + u * rx + v * ux, dy = fy + u * ry + v * uy, dz = fz + u * rz + v * uz;
				const double dl = std::sqrt( dx * dx + dy * dy + dz * dz );
				dx /= dl; dy /= dl; dz /= dl;
				const double s = ( kYSurf - kCamY ) / dy;
				const double qx = 0.0 + s * dx, qz = kCamZ + s * dz;
				const double Tin = 1.0 - Fresnel( -dy, 1.0, kN );
				// Refracted direction in water.
				const double wx = dx / kN, wz = dz / kN;
				const double wy = -std::sqrt( 1.0 - wx * wx - wz * wz );
				const double s2 = ( kYFloor - kYSurf ) / wy;
				const double px = qx + s2 * wx, pz = qz + s2 * wz;
				if( std::fabs( px ) >= kPatch || std::fabs( pz ) >= kPatch ) footprintInside = false;
				const double E = Irradiance( px, pz );
				if( E < 0 ) { footprintInside = false; return -1.0; }
				sum += Tin * E;
			}
		}
		const double meanTE = sum / double( G * G );
		return meanTE / ( kN * kN ) * kRho / kPi * ( 1.0 + SelfReturn() );
	}

	std::string Scene( const char* waterIor )
	{
		char buf[4096];
		std::snprintf( buf, sizeof(buf),
			"film\n{\n\twidth 32\n\theight 32\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0.0 %g %g\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov %g\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor %g %g %g\n}\n\n"
			"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
			"clippedplane_geometry\n{\n\tname quad\n"
			"\tpta -%g %g %g\n\tptb %g %g %g\n\tptc %g %g -%g\n\tptd -%g %g -%g\n}\n\n"
			"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
			"dielectric_material\n{\n\tname mat_water\n\tior %s\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			// Top face at kYSurf, bottom face far below (y = -2).
			"box_geometry\n{\n\tname geo_water\n\twidth 6.0\n\theight 2.3\n\tdepth 6.0\n}\n\n"
			"standard_object\n{\n\tname water\n\tgeometry geo_water\n\tmaterial mat_water\n\tposition 0 -0.85 0\n}\n\n"
			"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale %g\n\tmaterial none\n}\n\n"
			"sphere_geometry\n{\n\tname geo_emit\n\tradius %g\n}\n\n"
			"standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n\tposition %g %g %g\n}\n",
			kCamY, kCamZ, kFovDeg, kRho, kRho, kRho,
			kPatch, kYFloor, kPatch, kPatch, kYFloor, kPatch, kPatch, kYFloor, kPatch, kPatch, kYFloor, kPatch,
			waterIor, kEmitScale, kEmitR, kEmitX, kEmitY, kEmitZ );
		return std::string( buf );
	}

	//! Legacy chain for row E: emission + a diffuse-only gather + the
	//! refraction consumer (see the row header for why no reflection op).
	std::string RasterizerPixelPelGather( const char* samples, const char* gatherSamples )
	{
		return std::string(
			"distributiontracing_shaderop\n{\n\tname dt\n\tsamples " ) + gatherSamples + "\n"
			"\treflections FALSE\n\trefractions FALSE\n\tdiffuse TRUE\n\ttranslucents FALSE\n}\n\n"
			"standard_shader\n{\n\tname global\n"
			"\tshaderop DefaultEmission\n\tshaderop dt\n\tshaderop DefaultRefraction\n}\n\n"
			"pixelpel_rasterizer\n{\n\tsamples " + samples + "\n"
			"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
	}
}

static void RunRowE()
{
	bool inside = false;
	const double expected = RowE::ClosedForm( inside );
	std::cout << "Row E: submerged Lambertian patch, sphere emitter in air, camera in air"
	          << "  (closed form " << expected << ")" << std::endl;
	Check( expected > 0 && inside, "row E: closed form evaluated with the whole footprint on the patch" );
	// The quadrature is independent of the renders; pin it so a change to
	// it (or to the scene constants) is a visible, deliberate edit.
	Check( std::fabs( expected - 0.0035147 ) <= 1e-3 * 0.0035147,
		"row E: closed form reproduces the value the bands were measured against" );
	if( !( expected > 0 ) ) return;

	const std::string head( "RISE ASCII SCENE 7\n" );
	const std::string common = RowE::Scene( "1.33" );

	struct Row { const char* name; std::string scene; int n; double band; };
	const Row rows[] = {
		{ "PT",       head + RasterizerPT( "256" )                   + common, 4, 0.02 },
		{ "BDPT",     head + RasterizerBDPT( "512", kSlabDepth )     + common, 4, 0.02 },
		{ "VCM",      head + RasterizerVCM( "512", kSlabDepth )      + common, 4, 0.02 },
		{ "pixelpel", head + RowE::RasterizerPixelPelGather( "4", "128" ) + common, 1, 0.05 },
	};

	for( const Row& r : rows ) {
		double sum = 0.0;
		bool ok = true;
		for( int i = 0; i < r.n; i++ ) {
			const ImageStats s = RenderAndComputeStats( r.scene, "rowE" );
			if( !s.valid ) { ok = false; break; }
			sum += GreyMean( s );
		}
		const std::string label = std::string( "row E " ) + r.name;
		Check( ok, label + ": every render produced output" );
		if( !ok ) continue;
		const double m = sum / double( r.n );
		std::cout << "    " << r.name << "  mean=" << m << " (n=" << r.n << ")"
		          << "  ratio-to-closed-form=" << ( m / expected ) << std::endl;
		char buf[160];
		std::snprintf( buf, sizeof(buf), ": mean == closed-form direct term within %.0f%%", r.band * 100.0 );
		Check( std::fabs( m - expected ) <= r.band * expected, label + buf );
	}
}

//////////////////////////////////////////////////////////////////////
// Row F -- DL-308: scale invariance of row E's scene (reference-free).
//
// Multiply every index by 1.5 -- the camera, the emitter and the water
// (1.33 -> 1.995) all sitting inside an ideal non-reflecting index-1.5
// enclosure -- and every relative index, every Fresnel factor, every
// Snell direction and every basic-radiance ratio is unchanged, so the
// image must be too.  The SSSExteriorIndexInvarianceTest idiom: a black
// absorbing room (radius 20) sits around everything on BOTH sides, so no
// path ever reaches the enclosure's wall, whose total internal reflection
// would otherwise return grazing escapes as real extra light.  Each pair
// renders the air and the enclosed scene with ONE salt (common random
// numbers: the invariance says they are the same function), so the ratio
// is tight.  MEASURED (paired, n = 4): PT 1.0004 +/- 0.0017 (sd),
// BDPT 0.9999 +/- 0.0011, VCM 1.0002 +/- 0.0003.  An eta factor taken
// from an ABSOLUTE index anywhere reads 1.5^2 = 2.25 or its inverse.
//
// VCM runs with an explicit merge radius here: the room's long light
// segments set its auto radius to 0.19, larger than the whole patch, and
// the merge's kernel estimate then reads ~0.6 of the truth on both sides
// alike (DL-319).  The invariance holds either way; the explicit radius
// keeps the row's absolute value meaningful.
//////////////////////////////////////////////////////////////////////
static const char* kBlackRoom =
	"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n}\n\n"
	"lambertian_material\n{\n\tname mat_black\n\treflectance pnt_black\n}\n\n"
	"sphere_geometry\n{\n\tname room_geo\n\tradius 20\n}\n\n"
	"standard_object\n{\n\tname room\n\tgeometry room_geo\n\tmaterial mat_black\n}\n\n";

static const char* kEnclosure15 =
	"uniformcolor_painter\n{\n\tname pnt_white_enc\n\tcolor 1 1 1\n}\n\n"
	"perfectrefractor_material\n{\n\tname mat_enc\n\tior 1.5\n\trefractance pnt_white_enc\n}\n\n"
	"box_geometry\n{\n\tname enc_geo\n\twidth 60\n\theight 60\n\tdepth 60\n}\n\n"
	"standard_object\n{\n\tname enclosure\n\tgeometry enc_geo\n\tmaterial mat_enc\n}\n\n";

static void RunRowF()
{
	std::cout << "Row F: row E's scene in air vs inside an index-1.5 enclosure (every index x1.5)"
	          << "  (reference-free: ratio 1)" << std::endl;

	const std::string head( "RISE ASCII SCENE 7\n" );
	const std::string air = RowE::Scene( "1.33" )  + kBlackRoom;
	const std::string enc = RowE::Scene( "1.995" ) + kBlackRoom + kEnclosure15;
	const Point3 camPos( 0.0, RowE::kCamY, RowE::kCamZ );

	struct Row { const char* name; std::string ras; };
	const Row rows[] = {
		{ "PT",   RasterizerPT( "256" ) },
		{ "BDPT", RasterizerBDPT( "256", kSlabDepth ) },
		{ "VCM",  RasterizerVCM( "256", kSlabDepth, "0.002" ) },
	};
	const int kPairs = 2;

	for( const Row& r : rows ) {
		double sumAir = 0.0, sumEnc = 0.0;
		bool ok = true;
		double seededAir = 0.0, seededEnc = 0.0;
		for( int i = 0; i < kPairs && ok; i++ ) {
			const uint32_t salt = SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag );
			const ImageStats a = RenderWithSalt( head + r.ras + air, "rowF_air", salt,
				i == 0 ? &camPos : nullptr, &seededAir );
			const ImageStats e = RenderWithSalt( head + r.ras + enc, "rowF_enc", salt,
				i == 0 ? &camPos : nullptr, &seededEnc );
			if( !a.valid || !e.valid ) { ok = false; break; }
			sumAir += GreyMean( a );
			sumEnc += GreyMean( e );
		}
		const std::string label = std::string( "row F " ) + r.name;
		Check( ok, label + ": every render produced output" );
		if( !ok ) continue;
		Check( std::fabs( seededAir - 1.0 ) < 1e-12 && std::fabs( seededEnc - 1.5 ) < 1e-12,
			label + ": the camera's seeded exterior index is 1 in air and 1.5 enclosed" );
		const double ratio = sumEnc / sumAir;
		std::cout << "    " << r.name << "  air=" << sumAir / kPairs << "  enclosed=" << sumEnc / kPairs
		          << "  enclosed/air=" << ratio << std::endl;
		Check( std::fabs( ratio - 1.0 ) <= 0.01, label + ": enclosed/air image mean within 1% of 1" );
	}
}

int main( int argc, char** argv )
{
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "=== RefractiveRadianceScalingTest ===" << std::endl;
	std::cout << "seed base = " << g_seedBase
	          << "  (pass a different one as argv[1] for an independent sample)"
	          << std::endl;

	RunRowA( "1.33", 1.33 );
	RunRowA( "1.5",  1.5  );
	RunRowAPerfectRefractor( "1.33", 1.33 );
	RunRowB();
	RunRowC();
	RunRowD();
	RunRowE();
	RunRowF();
	TestNonfiniteCandidateRejected();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
