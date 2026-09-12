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
//      D. Same slab with a DELTA omni light.  PT and BDPT are exactly 0
//         here (no strategy can connect through a delta interface to a
//         delta light -- structural, not a bug), so the two estimators
//         that CAN see it are transparent-shadow PT (a straight shadow
//         ray that ignores refraction entirely) and VCM (merges).  They
//         must agree to within the transparent-shadow approximation.
//         Band set from measurement, see the row.
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
//    base for an independent sample.
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
		for( int ch = 0; ch < 3; ch++ ) {
			s.mean[ch] += v[ch];
			if( v[ch] > s.max[ch] ) s.max[ch] = v[ch];
		}
	}
	for( int c = 0; c < 3; c++ ) s.mean[c] /= double( cap.pixels.size() );
	s.valid = true;
	return s;
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

static ImageStats RenderAndComputeStats( const std::string& sceneText, const char* tag )
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

	std::srand( g_seedBase + g_renderIndex++ );
	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( scenePath.c_str() );
	return result;
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

static std::string RasterizerBDPT( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"bdpt_pel_rasterizer\n{\n"
		"\tmax_eye_depth 5\n"
		"\tmax_light_depth 5\n"
		"\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"}\n";
}

static std::string RasterizerVCM( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"vcm_pel_rasterizer\n{\n"
		"\tmax_eye_depth 5\n"
		"\tmax_light_depth 5\n"
		"\tsamples " ) + samples + "\n"
		"\toidn_denoise FALSE\n"
		"\tpixel_filter box\n"
		"\tmerge_radius 0.0\n"
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

static void RunRowC()
{
	std::cout << "Row C: submerged floor, sphere emitter in air, camera in air"
	          << "  (cross-integrator agreement)" << std::endl;

	const std::string head( "RISE ASCII SCENE 7\n" );
	const std::string common = std::string( kSlabBody ) + kSlabLightArea;

	const ImageStats pt   = RenderAndComputeStats( head + RasterizerPT( "4096" )   + common, "rowC" );
	const ImageStats bdpt = RenderAndComputeStats( head + RasterizerBDPT( "2048" ) + common, "rowC" );
	const ImageStats vcm  = RenderAndComputeStats( head + RasterizerVCM( "2048" )  + common, "rowC" );

	Check( pt.valid && bdpt.valid && vcm.valid, "row C: all three renders produced output" );
	if( !pt.valid || !bdpt.valid || !vcm.valid ) return;

	const double mPT   = GreyMean( pt );
	const double mBDPT = GreyMean( bdpt );
	const double mVCM  = GreyMean( vcm );

	std::cout << "    PT   mean=" << mPT << std::endl;
	std::cout << "    BDPT mean=" << mBDPT << "  BDPT/PT=" << ( mBDPT / mPT ) << std::endl;
	std::cout << "    VCM  mean=" << mVCM  << "  VCM/PT ="  << ( mVCM  / mPT ) << std::endl;

	Check( mPT > 1e-6, "row C: PT mean is non-trivial" );
	Check( std::fabs( mBDPT - mPT ) <= 0.08 * mPT, "row C: BDPT within 8% of PT" );
	Check( std::fabs( mVCM  - mPT ) <= 0.08 * mPT, "row C: VCM within 8% of PT" );
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
	const ImageStats vcm     = RenderAndComputeStats( head + RasterizerVCM( "1024" ) + common, "rowD" );

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

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
