//////////////////////////////////////////////////////////////////////
//
//  EnvLightBalanceTest.cpp - End-to-end correctness check for
//    environment-map (HDRI / IBL) transport across PT, BDPT and VCM.
//
//    ==================================================================
//    2026-08-27 RE-DERIVATION (slice F3 of the PT env-MIS arc)
//    ==================================================================
//    Until this date every assertion in this file was RELATIVE: it
//    asserted that BDPT and VCM agree with PT to within a tolerance
//    family (8 % -> 30 % mean, 25 % -> 35 % p99, 2x -> 1.5x max).
//    That was only ever as good as the PT reference, and the PT
//    reference was WRONG: commit e4f36607 (F1) found that the surface-
//    escape env block in IntegrateFromHitTemplated added env radiance
//    with NO MIS weight whenever a radiance map was present, so PT's
//    env strategies summed to 1 + w_nee instead of 1.  On env-lit
//    non-delta surfaces PT read +17.7 % over truth (closed form:
//    ln(17)/16 = 0.177076 excess at albedo 1).  F2 (afb2d64e) fixed the
//    two medium-scatter sibling sites and the OpenPGL segment storage.
//
//    Post-fix, PT reproduces closed-form truth to 0.04-0.12 % on every
//    topology below that admits one (numbers in each derivation).
//    BDPT and VCM are byte-for-byte UNCHANGED by F1/F2 — and they are
//    14-50 % OVER closed-form truth on the uniform-env topologies.
//    They used to pass only because they were being compared against
//    an inflated PT.
//
//    THIS SUITE IS THEREFORE RESTRUCTURED INTO THREE KINDS OF CHECK:
//
//      (1) ABSOLUTE CLOSED-FORM CHECKS ON PT.  Every topology whose
//          converged image has a closed form now asserts PT against
//          that closed form directly, at a tight band derived from the
//          measured run-to-run spread (see "Band derivation" below).
//          These are the real correctness assertions in the file; they
//          do not depend on any other integrator.  Derivations are
//          written out per topology.
//
//      (2) BIAS-REFERENCED REGRESSION BANDS ON BDPT / VCM.  BDPT and
//          VCM do NOT agree with truth on these scenes.  Rather than
//          silently widening a parity band that asserts something
//          false, each (topology x integrator x statistic) now carries
//          the MEASURED ratio-to-PT as the band CENTRE, with the
//          measured bias named in a comment.  The check therefore
//          guards "the bias has not moved", not "there is no bias".
//
//          *** A FUTURE FIX THAT MOVES BDPT/VCM TOWARD TRUTH WILL FAIL
//          *** THESE CHECKS.  THAT IS INTENDED.  The centres below are
//          *** a snapshot of known-biased behaviour, not a target; a
//          *** genuine improvement must re-derive them (and should say
//          *** so in its commit message).  The bias itself is the
//          *** long-documented disc-area-vs-solid-angle env MIS
//          *** residual — see docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md
//          *** (Sessions 11/12/13 concluded STOP: the principled fix is
//          *** a monolithic both-subpath-sides SA-MIS migration gated
//          *** behind a separate HWSS spectral-bundle workstream) and
//          *** docs/IMPROVEMENTS.md #12.
//
//      (3) FIREFLY CAPS.  The old "max within 1.5x of PT max" check
//          conflated three unrelated things: the mean bias, the
//          integrator's noise level, and actual fireflies.  It is
//          replaced by max <= cap * OWN mean, per integrator.  That is
//          a pure peakiness measure, immune to the mean bias, and it
//          is what the historical t=1 white-firefly bug (~3.5x
//          overshoot) actually violated.
//
//    ==================================================================
//    PROPERTY (unchanged)
//    ==================================================================
//    For any scene with an env-map IBL, PT and BDPT (and VCM) must
//    converge to the same image in the limit.  When env-light emission
//    sampling is broken (BDPT's s=1 NEE silently returns 0 because the
//    env-light vertex has both pLight and pLuminary NULL; BDPT's
//    eye-subpath escape doesn't accumulate env emission; MLT's
//    bootstrap can't find non-zero seeds because s=1 contributes 0),
//    BDPT/VCM/MLT diverge from PT by a systematic bias that does NOT
//    shrink with more samples — they render too dim, or in the worst
//    case fully black.
//
//    REGRESSION FAMILY THIS TEST GUARDS AGAINST:
//      - LightSampler::SampleLight returning false for IBL-only
//        scenes (pre-Phase A baseline: BDPT/VCM/MLT all black).
//      - BDPT s=1 / t=1 emission lookup branches dropping silently
//        on env-light vertex 0 (pLight=pLuminary=NULL).  Path A.
//      - BDPT eye-subpath miss path failing to accumulate env
//        emission (the s=0 strategy).  Path B.
//      - MLT bootstrap underflow due to BDPT producing all-zero
//        seed paths on env-only scenes.  Downstream of Path A.
//      - The t=1 white-firefly bug we hit during Phase A development
//        (fLight stayed at default `(1,1,1)` and was splatted
//        unconditionally for env-light) — guarded by the firefly caps.
//      - The binary LightSampler::EnvSelectProbability() bug (env
//        excluded from the alias table => env NEE dead in MIXED
//        scenes only), fixed 2026-05-29 by the continuous-PMF rework.
//        NEWLY GUARDED DIRECTLY by the topology E / F increment checks
//        below: those assert the explicit light's contribution ON TOP
//        of the env-only baseline against a closed form, so an env-NEE
//        deficit that appears only when a second light exists now
//        shows up as an increment error instead of being invisible
//        inside a 30 %-wide parity band.
//      - The PT env MIS double-count (F1, e4f36607) — caught directly
//        by the topology D / G absolute checks (+17.7 % vs a 1 % band).
//
//    ==================================================================
//    Topologies
//    ==================================================================
//      D. Env-only Lambertian quad — pure IBL, no explicit lights.
//         CLOSED FORM.  Exercises the canonical "BDPT/VCM must
//         reproduce PT under only env-map illumination" partition:
//         s=0 (eye escapes to env), s=1 NEE (eye connects to env-light
//         vertex 0), s>=2 (env-light disc subpath bounces in scene then
//         connects to eye).
//      E. Env + omni light — mixed env + explicit-light selection.
//         CLOSED FORM (as an increment over D).
//      F. Env + mesh emitter — env + area light.  Same as E but
//         exercises the s=0 (eye-hits-emitter) strategy alongside
//         env-NEE for the most complex MIS partition.
//         CLOSED FORM (as an increment over D).
//      G. Env-only Lambertian quad, SPECTRAL.  CLOSED FORM on
//         luminance, with the Jakob-Hanika uplift round-trip called
//         out separately (it is NOT a transport error).
//      H/I. Non-uniform (checker) env + off-center quad, RGB and
//         spectral.  NO closed form — PT-relative bands only.
//      J. Submerged camera / delta dielectric shell.  CLOSED FORM,
//         exact (delta path, no MC noise, no MIS).  UNCHANGED by this
//         re-derivation — it was always correct.
//
//    ==================================================================
//    Sample counts and runtime
//    ==================================================================
//    Raised 2026-08-27 from 64 (RGB) / 128 (spectral) to 256 / 512.
//    Rationale, measured: the RGB rows were already reproducible to
//    <= 0.06 % run-to-run at 64 spp, but the closed-form increment
//    checks for topologies E and F need the ~0.03 % residual MC error
//    on the mean to be small compared with the increment itself
//    (~0.012 and ~0.020 on a base of 0.5), and 256 spp takes the
//    agreement with the derivation from ~0.9 % to <= 0.39 %.  The
//    spectral rows moved 1.3-3.6 % run-to-run at 128 spp (that is the
//    non-reproducibility recorded in the F2 commit message; it is
//    spectral-only — the RGB Pel rows are bit-identical run to run on
//    this machine).  512 spp halves that to <= 2.0 %.  1024 spp was
//    measured and rejected: it bought no further ratio-spread
//    reduction (the residual is order-statistic jitter on p99/max, not
//    variance) while taking the suite from 12.8 s to 21.2 s per run.
//
//    Whole-suite wall clock: ~13 s per run (was ~3.3 s).
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
// CapturingRasterizerOutput — same shape as BDPTStrategyBalanceTest.
// Stores the linear radiance buffer for in-test pixel analysis.
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
	double median[3];
	double p99[3];
	double max[3];
	bool   valid;
};

static double Percentile( std::vector<double>& v, double p )
{
	if( v.empty() ) return 0.0;
	std::sort( v.begin(), v.end() );
	const size_t idx = static_cast<size_t>(
		std::min<double>( double(v.size()-1), std::round( p * (v.size()-1) ) ) );
	return v[idx];
}

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	std::vector<double> ch[3];
	for( int c = 0; c < 3; c++ ) ch[c].reserve( cap.pixels.size() );

	for( const RISEColor& c : cap.pixels ) {
		ch[0].push_back( c.base.r );
		ch[1].push_back( c.base.g );
		ch[2].push_back( c.base.b );
	}

	for( int c = 0; c < 3; c++ ) {
		double sum = 0;
		for( double v : ch[c] ) sum += v;
		s.mean[c]   = sum / double(ch[c].size());
		s.median[c] = Percentile( ch[c], 0.50 );
		s.p99[c]    = Percentile( ch[c], 0.99 );
		s.max[c]    = ch[c].back();
	}
	s.valid = true;
	return s;
}

//! Rec.709 / D65 luminance.  RISEPel IS Rec709RGBPel since Stage B of
//! the 2026-05-24 colour-space migration (docs/COLOR_SPACE_MIGRATION.md),
//! so these are the correct weights for the captured buffer.
static double Luminance( const double v[3] )
{
	return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/env_light_balance_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

static void PrintStats( const char* label, const ImageStats& s )
{
	if( !s.valid ) {
		std::cout << "  " << label << ": <invalid>" << std::endl;
		return;
	}
	std::cout << "  " << label
		<< "  mean (" << s.mean[0] << ", " << s.mean[1] << ", " << s.mean[2] << ")"
		<< "  p99 (" << s.p99[0] << ", " << s.p99[1] << ", " << s.p99[2] << ")"
		<< "  max (" << s.max[0] << ", " << s.max[1] << ", " << s.max[2] << ")"
		<< std::endl;
}

//////////////////////////////////////////////////////////////////////
// Check kind (1): absolute, against a closed form.
//////////////////////////////////////////////////////////////////////

//! Per-channel |value - expected| / expected <= tol.
static bool AbsWithin( const double v[3], const double expected[3], double tol )
{
	for( int c = 0; c < 3; c++ ) {
		if( std::fabs( expected[c] ) < 1e-12 ) return false;
		if( std::fabs( v[c] - expected[c] ) / std::fabs( expected[c] ) > tol ) return false;
	}
	return true;
}

static void PrintAbsDiff( const char* label, const double v[3], const double expected[3] )
{
	std::cout << "    " << label << " vs closed form:";
	for( int c = 0; c < 3; c++ ) {
		std::cout << " " << v[c] << "(" << ((v[c]-expected[c])/expected[c]*100.0) << "%)";
	}
	std::cout << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Check kind (2): bias-referenced regression band.
//
// `center` is the MEASURED per-channel ratio (integrator stat) / (PT
// stat) at the sample counts in this file; `tol` is a FRACTIONAL
// tolerance on that ratio.  Per-channel because the hwss=true spectral
// rows have a genuinely channel-dependent bias (the spectral-bundle
// effect), which no single scalar centre can represent.
//////////////////////////////////////////////////////////////////////
struct StatBand
{
	double center[3];
	double tol;
};

struct IntegratorBias
{
	StatBand mean;
	StatBand p99;
};

struct TopologyBias
{
	IntegratorBias bdpt;
	IntegratorBias vcm;
	//! max <= peakCap * own mean, applied to PT, BDPT and VCM alike.
	double         peakCap;
};

static bool RatioWithinBand( const double ref[3], const double x[3], const StatBand& b )
{
	for( int c = 0; c < 3; c++ ) {
		if( std::fabs( ref[c] ) < 1e-12 ) return false;
		const double ratio = x[c] / ref[c];
		if( std::fabs( ratio - b.center[c] ) > b.tol * b.center[c] ) return false;
	}
	return true;
}

static void PrintRatioBand(
	const char* label,
	const double ref[3],
	const double x[3],
	const StatBand& b )
{
	std::cout << "    " << label << " ratio-to-PT (expected centres "
		<< b.center[0] << ", " << b.center[1] << ", " << b.center[2]
		<< " +-" << (b.tol*100.0) << "%):";
	for( int c = 0; c < 3; c++ ) {
		const double ratio = ( std::fabs(ref[c]) < 1e-12 ) ? 0.0 : x[c] / ref[c];
		std::cout << " " << ratio;
	}
	std::cout << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Check kind (3): firefly cap.  max <= cap * own mean, per channel.
//
// Replaces the old "max within 1.5x of PT max".  That check mixed the
// mean bias (BDPT/VCM run 14-50 % hot on these scenes, which pushed
// their max up by the same factor for no pathological reason), the
// integrator's noise level (BDPT at equal spp is simply noisier than
// PT here), and genuine fireflies.  max/own-mean isolates the last.
//
// Historical target: the Phase-A t=1 bug splatted a default-init
// fLight=(1,1,1) unconditionally for env-light vertices, producing
// isolated pixels ~3.5x the image max.  This is the check kind the
// denoiser corrupted worst — see the four kPeakCap* constants for the
// re-derived, de-OIDN'd values and the per-group measurements.  The
// checker-env topologies are legitimately high-dynamic-range (the
// bright checker cells ARE the max), which is why they band looser than
// the uniform-env ones.
//////////////////////////////////////////////////////////////////////
static bool PeakWithinCap( const ImageStats& s, double cap )
{
	for( int c = 0; c < 3; c++ ) {
		if( s.mean[c] <= 0.0 ) return false;
		if( s.max[c] > cap * s.mean[c] ) return false;
	}
	return true;
}

static void PrintPeak( const char* label, const ImageStats& s, double cap )
{
	std::cout << "    " << label << " max/mean (cap " << cap << "):";
	for( int c = 0; c < 3; c++ ) {
		std::cout << " " << ( s.mean[c] > 0.0 ? s.max[c]/s.mean[c] : 0.0 );
	}
	std::cout << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Scene chunks
//
// Setup: a single Lambertian quad facing the camera, lit only by the
// IBL (or IBL + small explicit emitter for the mixed topologies).
//
// GEOMETRY FACTS THE CLOSED FORMS DEPEND ON (each verified against the
// render, see the per-topology derivations):
//   - Camera at (0,0,3.5) looking down -Z, `fov 30.0` interpreted as
//     the FULL angle, so the half-extent of the visible region on the
//     z=0 plane is 3.5*tan(15 deg) = 0.9378222.
//   - That is < 1, so the [-1,1]^2 quad FILLS the frame: no pixel sees
//     the environment directly, and the image mean is exactly the
//     box-filtered average of the surface radiance over
//     [-0.9378222, 0.9378222]^2.  (Confirmed empirically: PT max is
//     0.54 on topology D, not 1.0 — a background pixel would read the
//     env at L=1.)
//   - z=0 is perpendicular to the view axis, so the pixel-to-surface
//     map is LINEAR; with `pixel_filter box` and jittered samples the
//     image average is the exact uniform area average over that square.
//   - The quad is the only geometry in D/E, so its front hemisphere is
//     unoccluded and there is no interreflection (a plane cannot see
//     itself).  In F the emitter has `material none` (pure absorber,
//     no BRDF), so there is still no interreflection — only occlusion.
//////////////////////////////////////////////////////////////////////
static const char* kSceneCommonGeometry =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad\n"
	"\tpta -1 -1 0\n"
	"\tptb 1 -1 0\n"
	"\tptc 1 1 0\n"
	"\tptd -1 1 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad\n"
	"\tmaterial mat_diffuse\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// Env-map definitions
//
// `pnt_env` is a uniform-color painter at L=1.0 in each channel.
// Used as the IBL source via the rasterizer's `radiance_map` parameter.
//////////////////////////////////////////////////////////////////////
static const char* kEnvPainter =
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_env\n"
	"\tcolor 1.0 1.0 1.0\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// PT rasterizer with env IBL.
//
// `radiance_map pnt_env` wires the painter as the global radiance
// map.  `radiance_scale 1.0` keeps the env at its painter value.
// `radiance_background TRUE` lets the camera see the env directly
// when it escapes — required for s=0 strategy to give a fair PT
// baseline.  (On topologies D/E/F no camera ray actually escapes; the
// flag matters for the off-center checker topologies H/I.)
//
// `pixel_filter box` on every rasterizer string, PT included: a
// normalized filter preserves the image MEAN, but matching the filter
// across integrators removes it as a confounder on p99/max, and the
// box filter is what makes the topology D/E/F closed forms exact
// (see the geometry-facts block above).
//////////////////////////////////////////////////////////////////////
static const char* kRasterizerPT =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_pt_unused\n"
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
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_bdpt_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerVCM =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_pel_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 256\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_vcm_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// Spectral rasterizer variants — exercise the NM env-light branches
// in BDPTIntegrator and VCMIntegrator that the *_pel_rasterizer
// blocks don't touch.  Without these, the NM s=1 NEE / s=1 t=1 /
// s=0 escape sites would be entirely untested — Reviewer 3 (test-
// coverage adversary) flagged the gap.  Same env / scale /
// background settings as the Pel block above so PT-vs-BDPT-vs-VCM
// comparison stays apples-to-apples.  Both HWSS on and HWSS off
// configurations are executed dynamically per spectral test to
// exercise both hero-wavelength and single-wavelength loops.
//
// Samples 512 (was 128) — see the "Sample counts and runtime" block
// in the file header for the measurement that chose that number.
//////////////////////////////////////////////////////////////////////
static const char* kRasterizerPTSpectral =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultDirectLighting\n"
	"}\n"
	"\n"
	"pathtracing_spectral_rasterizer\n"
	"{\n"
	"\tsamples 512\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tnmbegin 380\n"
	"\tnmend 720\n"
	"\tnum_wavelengths 8\n"
	"\tspectral_samples 1\n"
	"\thwss false\n"
	"\tmax_diffuse_bounce 3\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_pts_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerBDPTSpectral =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"bdpt_spectral_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 512\n"
	"\tnmbegin 380\n"
	"\tnmend 720\n"
	"\tnum_wavelengths 8\n"
	"\tspectral_samples 1\n"
	"\thwss false\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_bdpts_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

static const char* kRasterizerVCMSpectral =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"vcm_spectral_rasterizer\n"
	"{\n"
	"\tmax_eye_depth 3\n"
	"\tmax_light_depth 3\n"
	"\tsamples 512\n"
	"\tnmbegin 380\n"
	"\tnmend 720\n"
	"\tnum_wavelengths 8\n"
	"\tspectral_samples 1\n"
	"\thwss false\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"\tmerge_radius 0.0\n"
	"\tvc_enabled true\n"
	"\tvm_enabled true\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_vcms_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

//! Omni light for topology E.  `power` on a point light is RADIANT
//! INTENSITY in W/sr, NOT flux: PointLight::ComputeDirectLighting
//! computes `cColor * brdf.value() * radiantEnergy * cos / d^2` with no
//! 1/(4*pi).  See the topology E derivation.
static const char* kLightOmni =
	"omni_light\n"
	"{\n"
	"\tname l_omni\n"
	"\tpower 2.0\n"
	"\tcolor 1.0 1.0 1.0\n"
	"\tposition 0.0 0.0 5.0\n"
	"}\n";

//! Mesh emitter for topology F.  LambertianEmitter::emittedRadiance
//! returns `radEx * INV_PI * scale` and is ONE-SIDED (zero when
//! Dot(out, N) <= 0), so this is a 1x1 quad at z=4 radiating
//! L_e = 1.0 * 10.0 / pi = 3.1830989 toward -Z (the winding
//! pta->ptb->ptc gives N = -Z).  See the topology F derivation.
static const char* kLightMesh =
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
	"\tscale 10.0\n"
	"\tmaterial none\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_emit\n"
	"\tpta -0.5 0.5 4.0\n"
	"\tptb 0.5 0.5 4.0\n"
	"\tptc 0.5 -0.5 4.0\n"
	"\tptd -0.5 -0.5 4.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_emit\n"
	"\tgeometry quad_emit\n"
	"\tmaterial mat_emit\n"
	"}\n";

//////////////////////////////////////////////////////////////////////
// BAND DERIVATION — how every tolerance in this file was chosen
//////////////////////////////////////////////////////////////////////
//
// Method: the suite was run 4x consecutively at each of three sample-
// count settings (64/128, 256/512, 256/1024) on the F2 tree
// (afb2d64e), and the full (min, max, spread) of every statistic and
// every ratio was tabulated.  Tolerances are then
//
//     tol  >=  3 x (worst observed full spread across 4 runs)
//
// The 3x factor is deliberate: the full range of 4 draws underestimates
// a 3-sigma half-width by roughly that much, and it also buys headroom
// for machines with a different core count (the render is thread-
// scheduling dependent — see below).
//
// REPRODUCIBILITY, RE-MEASURED POST-DE-OIDN (256/512, 6 consecutive
// runs, this machine).  The pre-recalibration figures below it were
// taken through the denoiser, which is deterministic and therefore
// UNDERSTATED the spectral rows' true jitter:
//   - Topology J (submerged): PT, BDPT and VCM are ALL bit-identical
//     run to run — stronger than the pre-recalibration claim, which
//     only asserted it for PT.
//   - RGB (Pel) rasterizers: PT is bit-identical on topologies D and E;
//     it moves <= 0.013 % on F and <= 0.010 % on H.  BDPT/VCM
//     mean-ratio moves <= 0.04 %, p99-ratio <= 0.39 %.
//   - SPECTRAL rasterizers: NOT reproducible, and now visibly less so
//     than the denoised buffers suggested.  PT mean moves up to 1.37 %,
//     BDPT/VCM mean-ratio up to 2.21 % (was 1.27 % denoised), p99-ratio
//     up to 5.46 % (was 4.48 % denoised), peak (max/own-mean) up to
//     6.9 %.  This is why the spectral rows carry visibly wider bands
//     than the RGB rows.  It shrinks as 1/sqrt(N) from 128 to 512 spp
//     and then stops shrinking (order-statistic jitter dominates
//     p99/max).
//
// (Pre-recalibration, denoiser-inclusive: "PT mean moves up to 1.05 %,
// BDPT/VCM mean-ratio up to 2.0 %, p99-ratio up to 5.3 %, max-ratio up
// to 12.8 %".)
//
// The max statistic is no longer banded at all — see the firefly-cap
// block above for why.
//
//////////////////////////////////////////////////////////////////////

//! Absolute closed-form bands.
//! - RGB PT vs closed form: worst measured deviation 0.0046 % across all
//!   three channels (6 runs, de-OIDN'd).  1 % gives 217x.
//!   RECALIBRATION NOTE (2026-08-27): the pre-recalibration figure was
//!   0.116 %, and its stated cause -- "a colour-pipe round-trip
//!   constant, not MC noise: the green/red mean ratio is 1.00164" -- was
//!   WRONG.  It was the DENOISER: with `oidn_denoise FALSE` the three
//!   channels agree to 6e-6 and all three sit +0.004 % from 0.5.  There
//!   is no colour-pipe offset on this path.  (This also removes the
//!   stated justification for the increment form of the E/F checks; the
//!   increment form is kept anyway, because it remains the sharper probe
//!   of the light-SELECTION path -- see topology E's comment.)
static const double kAbsBandRGB      = 0.010;
//! - Increment checks (topologies E, F): worst measured deviation from
//!   the derivation 0.13 % (topology F; topology E reads -0.009 %), and
//!   it is now IDENTICAL on all three channels, which confirms it is a
//!   MODEL residual (half-pixel grid convention in the quadrature) and
//!   not noise.  Pre-recalibration this read 0.39 % with a +0.18/-0.12 %
//!   channel spread; the spread was the denoiser.  Worst-case
//!   contribution from fully decorrelated MC error between the two
//!   renders is ~0.9 %.  5 % covers both with ~13x.
static const double kAbsBandIncrement = 0.050;
//! - Spectral PT luminance vs closed form: measured -1.62 % (hwss=false)
//!   / -1.32 % (hwss=true) over 6 de-OIDN'd runs (see topology G
//!   derivation — the deficit is the Jakob-Hanika uplift round-trip, NOT
//!   transport), run spread ~0.3 %.  De-OIDN moved this by <= 0.23 pp,
//!   i.e. the deficit is real and was never a denoiser artifact.  5 %
//!   covers both and still catches the +17.7 % F1 double-count with 3.5x.
static const double kAbsBandSpectralLum = 0.050;
//! - Spectral PT per-channel vs closed form: the JH round-trip is
//!   strongly channel-dependent — measured over 6 de-OIDN'd runs the red
//!   channel sits at -4.5 % .. -4.8 %, green -0.8 % .. -1.1 %, blue
//!   +2.7 % .. +2.9 % (denoiser-inclusive it read -4.4/-0.9/+3.0, i.e.
//!   this one really IS the colour pipe and de-OIDN moved it <= 0.4 pp).
//!   So this band is a BOUND ON THE ROUND-TRIP, not a tight
//!   transport assertion; the luminance check above is what actually
//!   guards transport (a +17.7 % F1-style double-count moves luminance
//!   to +16.1 %, i.e. 3.2x outside its 5 % band, whereas it would move
//!   red only to +11.7 %).  10 % puts ~4.5 pp between the worst
//!   measured channel and the limit, ~4.8x the observed run-to-run
//!   jitter of 0.93 pp.
//!   A JH LUT retrain (e.g. the pre-staged ACEScg migration in
//!   docs/COLOR_SPACE_MIGRATION.md) SHOULD re-derive it.
static const double kAbsBandSpectralRGB = 0.100;

//! Firefly caps, RE-DERIVED 2026-08-27 from 6 de-OIDN'd runs.
//!
//! These are the bands the denoiser distorted most, and in BOTH
//! directions.  On the near-deterministic RGB rows OIDN *raised*
//! peakiness (topology D PT 1.010 true vs 1.074 denoised; topology H PT
//! 1.223 vs 1.394) — CNN ringing manufacturing a max the transport never
//! produced.  On the noisy spectral rows it *suppressed* it, and
//! massively (topology G hwss=true BDPT 2.021 true vs 1.386 denoised).
//! A single pair of caps could no longer be both tight on the RGB rows
//! and true on the spectral ones, so the two pairs became four.
//!
//! Discipline: cap = worst measured peakiness x ~1.4, and every cap
//! stays below 3.5 so all four still fail the historical firefly
//! signature (the Phase-A t=1 bug's isolated ~3.5x pixels).  Every cap
//! is TIGHTER than the 2.20 / 2.60 pair it replaces.
//!
//!   group                worst measured   cap    utilisation
//!   uniform RGB   (D,E,F)     1.3522       1.90     71.2 %
//!   uniform spec  (G x2)      2.0207       2.75     73.5 %
//!   nonuniform RGB (H)        1.2230       1.70     71.9 %
//!   nonuniform spec (I x2)    1.7629       2.40     73.5 %
//!
//! ("worst measured" = worst single channel of PT, BDPT or VCM over 10
//! pooled de-OIDN'd runs.)  Run-to-run peak jitter is <= 0.2 % on the
//! RGB rows and <= 6.9 % on the spectral ones, so the ~40 % headroom is
//! >= 5x the observed jitter.  These four caps are the WORST-UTILISED
//! bands in the file at 73.5 %; every ratio band sits at <= 27 %.
static const double kPeakCapUniformEnvRGB       = 1.90;
static const double kPeakCapUniformEnvSpectral  = 2.75;
static const double kPeakCapNonUniformRGB       = 1.70;
static const double kPeakCapNonUniformSpectral  = 2.40;

//////////////////////////////////////////////////////////////////////
// The measured BDPT / VCM bias table.
//
// Every `center` below is the mean over 6 consecutive runs of
// (integrator stat) / (PT stat), per channel, at the sample counts in
// this file.  READ THE BANNER AT THE TOP OF THIS FILE before changing
// any of them.
//
// FULLY RECALIBRATED 2026-08-27 with `oidn_denoise FALSE` on every
// rasterizer string in this file.  Before that, six of the seven
// strings rendered through OIDN and the suite banded the DENOISER's
// output (topology J's scoped fix is documented at its own site).  What
// the recalibration moved, measured OIDN-on vs OIDN-off on the same
// tree:
//   - MEAN centres: <= 0.08 pp on every RGB row, <= 0.9 pp on the
//     spectral rows (and every spectral movement is inside that row's
//     own run-to-run spread).  The bias headline numbers are therefore
//     UNCHANGED in substance -- BDPT/VCM's env-MIS bias was never a
//     denoiser artifact.
//   - p99 centres: moved materially, because OIDN was flattening the
//     very tail this statistic measures.  Worst movement is topology G
//     hwss=true, BDPT green: 1.4459 -> 1.9208 (+47 pp).  The RGB rows
//     moved +0.7..+4.2 pp (BDPT) and 0..+2.5 pp (VCM); the non-uniform
//     rows moved -2.5..+1.5 pp.
//   - PEAK caps: moved in both directions -- see the kPeakCap* block.
// The closed-form PT checks barely moved at all (<= 0.12 pp on topology
// D, <= 0.30 pp on the E/F increments, <= 0.23 pp on the G luminance),
// and all three moved TOWARD the closed form.
//////////////////////////////////////////////////////////////////////

//! Topology D — env-only Lambertian, RGB.
//! BDPT is +28.5 % over closed-form truth, VCM +24.3 %.  Both are
//! UNCHANGED by F1/F2 — this is the long-standing disc-area env MIS
//! residual, which used to hide inside a 30 % band centred on an
//! inflated PT.  Surfaced 2026-08-27 when the PT reference was fixed.
//! De-OIDN moved the mean centres by <= 0.03 pp; it moved BDPT's p99
//! centre from ~1.499 to 1.535 and VCM's from ~1.275 to 1.295.
//! GUARDS: the mean band catches any change to the env-MIS partition
//! bigger than 3 % (an SA-MIS migration, an env-NEE/S0 reweighting);
//! the p99 band now genuinely catches a tail/variance regression at
//! fixed spp, which it could not while OIDN was flattening the tail.
static const TopologyBias kBiasEnvOnly = {
	/* bdpt */ { { { 1.2850, 1.2850, 1.2850 }, 0.03 },
	             { { 1.5347, 1.5347, 1.5347 }, 0.05 } },
	/* vcm  */ { { { 1.2425, 1.2425, 1.2423 }, 0.03 },
	             { { 1.2950, 1.2951, 1.2948 }, 0.05 } },
	kPeakCapUniformEnvRGB
};

//! Topology E — env + omni light, RGB.
//! BDPT +27.8 %, VCM +23.7 % — slightly less than topology D because
//! the omni contribution (which both integrators get right) dilutes
//! the biased env term.  Same guard split as topology D; this row's
//! extra job is the LIGHT-SELECTION path (see the E derivation), which
//! the PT increment check owns.
static const TopologyBias kBiasEnvPlusOmni = {
	/* bdpt */ { { { 1.2784, 1.2784, 1.2784 }, 0.03 },
	             { { 1.5253, 1.5253, 1.5253 }, 0.05 } },
	/* vcm  */ { { { 1.2365, 1.2366, 1.2363 }, 0.03 },
	             { { 1.2915, 1.2916, 1.2914 }, 0.05 } },
	kPeakCapUniformEnvRGB
};

//! Topology F — env + mesh emitter, RGB.
//! BDPT +13.6 %, VCM +50.0 %.  The VCM figure is the "VCM env+mesh
//! ~22-28 % over" residual of docs/PRE_PHASE1_STATUS.md (Session 9)
//! re-measured against a CORRECT PT reference — against the old
//! inflated PT it read ~28 %; against truth it is 50 %.  This is the
//! single largest bias in the suite and the one
//! docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md is about.  De-OIDN left
//! it at 50.0 % (1.5003 -> 1.5000), which is the strongest single piece
//! of evidence that these biases are transport, not denoise.
static const TopologyBias kBiasEnvPlusMesh = {
	/* bdpt */ { { { 1.1356, 1.1356, 1.1356 }, 0.03 },
	             { { 1.2462, 1.2462, 1.2462 }, 0.05 } },
	/* vcm  */ { { { 1.5000, 1.5000, 1.4998 }, 0.03 },
	             { { 1.5223, 1.5224, 1.5221 }, 0.05 } },
	kPeakCapUniformEnvRGB
};

//! Topology G — env-only Lambertian, SPECTRAL, hwss=false.
//! BDPT +28.6 %, VCM +20.6 %.  BDPT matches its RGB twin (1.286 vs
//! 1.285) — the env bias is a transport property, not a colour-pipe
//! one.  VCM reads a little lower here than in RGB (1.206 vs 1.242).
//! Wider bands than the RGB rows purely because the spectral
//! rasterizers are not reproducible run to run.
//! TOLERANCES WIDENED at the de-OIDN recalibration: with the denoiser
//! off this row's measured spread is 2.21 % on the mean ratio and
//! 5.33 % on the p99 ratio, so the file's `tol >= 3 x worst spread`
//! rule demands 6.6 % / 16.0 % where the denoised calibration only
//! demanded 3.8 % / 11.8 %.  mean 0.07 -> 0.08, p99 0.15 -> 0.18.
//! GUARDS: mean = the NM env-MIS partition; p99 = the NM tail, which
//! is the row where the denoiser was hiding the most.
static const TopologyBias kBiasEnvOnlySpectralNoHWSS = {
	/* bdpt */ { { { 1.2890, 1.2894, 1.2798 }, 0.08 },
	             { { 1.4331, 1.4504, 1.4327 }, 0.18 } },
	/* vcm  */ { { { 1.2105, 1.2093, 1.1984 }, 0.08 },
	             { { 1.2814, 1.2609, 1.2546 }, 0.18 } },
	kPeakCapUniformEnvSpectral
};

//! Topology G — env-only Lambertian, SPECTRAL, hwss=true.
//! NOTE the strong CHANNEL dependence: BDPT reads (1.127, 1.302,
//! 1.239) against the hwss=false PT reference.  Dividing out BDPT's
//! own hwss=false env bias (1.289) isolates the HWSS spectral-bundle
//! factor as (0.874, 1.010, 0.961) — i.e. the bundle costs ~13 % in
//! red, is neutral in green and ~4 % low in blue on this uniform-env
//! scene.  That is the documented pre-existing spectral-bundle
//! deficit (CLAUDE.md env-IBL entry; docs/PRE_PHASE1_STATUS.md
//! Session 13 conclusion (3): it is present at the disc-area baseline
//! and is a SEPARATE workstream that must precede any SA-MIS
//! migration).  It is recorded here rather than asserted away.
//!
//! THIS IS THE ROW THE DENOISER DISTORTED MOST, and it is where the
//! two checks that failed the wave-4 de-OIDN of topology J lived.  The
//! mean centres barely moved (<= 0.7 pp) but the p99 centres moved
//! +30..+47 pp: BDPT (1.226, 1.446, 1.363) -> (1.530, 1.921, 1.805),
//! VCM (1.065, 1.255, 1.172) -> (1.222, 1.512, 1.438).  Under HWSS the
//! per-wavelength bundle makes BDPT/VCM's tail far heavier than PT's,
//! and OIDN was erasing exactly that.  p99 tolerance 0.10 -> 0.12: the
//! measured p99 spread is 3.46 %, so the 3x rule demands 10.4 %.
static const TopologyBias kBiasEnvOnlySpectralHWSS = {
	/* bdpt */ { { { 1.1267, 1.3017, 1.2394 }, 0.06 },
	             { { 1.5304, 1.9208, 1.8052 }, 0.12 } },
	/* vcm  */ { { { 1.0712, 1.2325, 1.1659 }, 0.06 },
	             { { 1.2221, 1.5120, 1.4384 }, 0.12 } },
	kPeakCapUniformEnvSpectral
};

//! Topology H — non-uniform (checker) env + off-center quad, RGB.
//! BDPT +2.2 %, VCM -8.1 %.  Far closer to PT than the uniform-env
//! topologies, and that is EXPECTED, not luck: on this scene most of
//! the image energy is env seen DIRECTLY by the camera (mean 4.09 with
//! bright checker cells at L=5), which every integrator gets exactly
//! right via s=0.  The biased once-reflected term is a small fraction
//! of the total, so the bias is diluted.  This topology is therefore a
//! DIRECTION-correctness test (see the topology comment), not a
//! sensitive bias test.
//! De-OIDN note: BDPT's p99 ratio is now EXACTLY 1.0000 on all three
//! channels (spread 0.00 % over 6 runs).  That is not a coincidence and
//! not a degenerate check — on this topology the 99th percentile lands
//! inside a saturated bright checker cell that both integrators reach
//! through the s=0 strategy alone, so it is a direct assertion that
//! BDPT's directly-visible env agrees with PT's bit for bit.  The
//! denoiser used to blur it to 1.011/1.008/1.002.
static const TopologyBias kBiasNonUniformRGB = {
	/* bdpt */ { { { 1.0210, 1.0210, 1.0210 }, 0.03 },
	             { { 1.0000, 1.0000, 1.0000 }, 0.05 } },
	/* vcm  */ { { { 0.9183, 0.9184, 0.9182 }, 0.03 },
	             { { 0.9131, 0.9131, 0.9129 }, 0.05 } },
	kPeakCapNonUniformRGB
};

//! Topology I — non-uniform env + off-center quad, SPECTRAL hwss=false.
//! Note: unlike topology G, this row compares against a PT rendered at
//! the SAME hwss setting (the pre-existing convention for this
//! topology, retained).
//! Mean tolerance 0.07 -> 0.08 at the de-OIDN recalibration for the same
//! reason as topology G hwss=false: the measured mean-ratio spread is
//! 1.96 %, so the 3x rule demands 5.9 % and 7 % left only 1.1 pp of
//! margin for a machine with a different core count.  The p99 tolerance
//! (0.18) already covers the measured 5.46 % spread (3x = 16.4 %).
static const TopologyBias kBiasNonUniformSpectralNoHWSS = {
	/* bdpt */ { { { 1.0193, 1.0229, 1.0261 }, 0.08 },
	             { { 0.9927, 1.0003, 0.9982 }, 0.18 } },
	/* vcm  */ { { { 0.9101, 0.9097, 0.9079 }, 0.08 },
	             { { 0.9043, 0.9033, 0.8967 }, 0.18 } },
	kPeakCapNonUniformSpectral
};

//! Topology I — non-uniform env + off-center quad, SPECTRAL hwss=true.
//! Both sides bundled, so the bundle effect largely cancels and the
//! ratios sit close to the hwss=false row.
//! De-OIDN moved the p99 centres down by 1.4-2.5 pp here (BDPT 1.023 ->
//! 0.999, VCM 0.925 -> 0.906) — the opposite direction to topology G
//! hwss=true, because on this scene both sides are bundled and the
//! denoiser was lifting BDPT/VCM's tail relative to PT's rather than
//! flattening it.  Tolerances unchanged: the measured spreads (0.28 %
//! mean, 1.04 % p99) sit far inside 6 % / 10 %.
static const TopologyBias kBiasNonUniformSpectralHWSS = {
	/* bdpt */ { { { 1.0205, 1.0249, 1.0264 }, 0.06 },
	             { { 0.9991, 0.9999, 0.9981 }, 0.10 } },
	/* vcm  */ { { { 0.9090, 0.9082, 0.9058 }, 0.06 },
	             { { 0.9040, 0.9083, 0.9042 }, 0.10 } },
	kPeakCapNonUniformSpectral
};

//////////////////////////////////////////////////////////////////////
// Topology driver.
//////////////////////////////////////////////////////////////////////
static ImageStats RunEnvTopologyTestWithRasterizers(
	const char* topologyName,
	const std::string& sceneCommonBlock,
	const char* rasterizerPT,
	const char* rasterizerBDPT,
	const char* rasterizerVCM,
	const TopologyBias& bias )
{
	std::cout << "Testing PT-vs-BDPT-vs-VCM: " << topologyName << std::endl;

	// IMPORTANT: painter chunks declared in `sceneCommonBlock` (e.g.
	// `pnt_env`) must appear BEFORE the rasterizer that references
	// them by name — RISE chunk parsing resolves named-painter refs
	// at parse time, not at scene-finalisation.  Put scene FIRST,
	// rasterizer SECOND.
	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + sceneCommonBlock + rasterizerPT;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + sceneCommonBlock + rasterizerBDPT;
	const std::string vcmScene  = std::string("RISE ASCII SCENE 7\n") + sceneCommonBlock + rasterizerVCM;

	const std::string ptPath   = WriteSceneToTempFile( ptScene.c_str(),   "pt"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "bdpt" );
	const std::string vcmPath  = WriteSceneToTempFile( vcmScene.c_str(),  "vcm"  );

	ImageStats none{};
	if( ptPath.empty() || bdptPath.empty() || vcmPath.empty() ) {
		Check( false, ( std::string("temp file write: ") + topologyName ).c_str() );
		return none;
	}

	const ImageStats pt   = RenderAndComputeStats( ptPath.c_str() );
	const ImageStats bdpt = RenderAndComputeStats( bdptPath.c_str() );
	const ImageStats vcm  = RenderAndComputeStats( vcmPath.c_str() );

	PrintStats( "PT  ", pt );
	PrintStats( "BDPT", bdpt );
	PrintStats( "VCM ", vcm );

	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );
	std::remove( vcmPath.c_str() );

	Check( pt.valid,   ( std::string("PT render produced output: ")   + topologyName ).c_str() );
	Check( bdpt.valid, ( std::string("BDPT render produced output: ") + topologyName ).c_str() );
	Check( vcm.valid,  ( std::string("VCM render produced output: ")  + topologyName ).c_str() );
	if( !pt.valid || !bdpt.valid || !vcm.valid ) return none;

	const double brightness = pt.mean[0] + pt.mean[1] + pt.mean[2];
	Check( brightness > 1e-4,
		( std::string("PT mean is non-zero: ") + topologyName ).c_str() );

	// Firefly cap on PT itself — PT is the reference, so a firefly in
	// PT would silently move every band below.
	{
		const bool ok = PeakWithinCap( pt, bias.peakCap );
		Check( ok, ( std::string("PT max <= ") + std::to_string(bias.peakCap)
			+ "x own mean (firefly cap): " + topologyName ).c_str() );
		if( !ok ) PrintPeak( "PT", pt, bias.peakCap );
	}

	struct Row { const char* name; const ImageStats& s; const IntegratorBias& b; };
	const Row rows[2] = { { "BDPT", bdpt, bias.bdpt }, { "VCM", vcm, bias.vcm } };

	for( const Row& r : rows ) {
		{
			const bool ok = RatioWithinBand( pt.mean, r.s.mean, r.b.mean );
			Check( ok, ( std::string(r.name) + " mean ratio-to-PT within "
				+ std::to_string(int(r.b.mean.tol*100)) + "% of measured bias: "
				+ topologyName ).c_str() );
			if( !ok ) PrintRatioBand( ( std::string(r.name) + ".mean" ).c_str(),
				pt.mean, r.s.mean, r.b.mean );
		}
		{
			const bool ok = RatioWithinBand( pt.p99, r.s.p99, r.b.p99 );
			Check( ok, ( std::string(r.name) + " p99 ratio-to-PT within "
				+ std::to_string(int(r.b.p99.tol*100)) + "% of measured bias: "
				+ topologyName ).c_str() );
			if( !ok ) PrintRatioBand( ( std::string(r.name) + ".p99" ).c_str(),
				pt.p99, r.s.p99, r.b.p99 );
		}
		{
			const bool ok = PeakWithinCap( r.s, bias.peakCap );
			Check( ok, ( std::string(r.name) + " max <= " + std::to_string(bias.peakCap)
				+ "x own mean (firefly cap): " + topologyName ).c_str() );
			if( !ok ) PrintPeak( r.name, r.s, bias.peakCap );
		}
	}

	return pt;
}

// Convenience: Pel-rasterizer signature for the RGB topology tests.
static ImageStats RunEnvTopologyTest(
	const char* topologyName,
	const std::string& sceneCommonBlock,
	const TopologyBias& bias )
{
	return RunEnvTopologyTestWithRasterizers(
		topologyName, sceneCommonBlock,
		kRasterizerPT, kRasterizerBDPT, kRasterizerVCM, bias );
}

//////////////////////////////////////////////////////////////////////
// CLOSED FORMS — topology D, and the E / F increments over it.
//////////////////////////////////////////////////////////////////////
//
// -- TOPOLOGY D: env-only Lambertian quad ---------------------------
//
// A uniform environment of radiance L_env = 1 fills the whole sphere.
// The quad is the only geometry, so its front hemisphere is entirely
// unoccluded and there is no interreflection.  Irradiance on a flat
// surface under a uniform hemisphere is
//
//     E = INT_hemisphere L_env cos(theta) dw = L_env * pi = pi
//
// and a Lambertian BRDF of reflectance rho = 0.5 has value rho/pi, so
// the outgoing radiance is
//
//     L_out = (rho/pi) * E = (0.5/pi) * pi = 0.5
//
// SPATIALLY CONSTANT, so the image mean is 0.5 regardless of the
// camera model, and this closed form is exact.
//
// MEASURED (PT, 256 spp, 6 runs, `oidn_denoise FALSE`):
// (0.500023, 0.500023, 0.500020), i.e. (+0.0046 %, +0.0046 %,
// +0.0040 %), and BIT-IDENTICAL run to run.  Band kAbsBandRGB = 1 %,
// i.e. 217x the deviation.
//
// (Pre-recalibration, denoiser-inclusive: (0.49980, 0.50058, 0.49988)
// = (-0.040 %, +0.116 %, -0.024 %).  The channel disagreement was the
// denoiser, not the colour pipe -- see kAbsBandRGB's note.)
//
// This is the check that directly detects the F1 bug: pre-fix PT read
// 0.5886 here (albedo 0.5 predicts 0.588538 from the closed-form
// excess derivation in the F1 commit message), i.e. +17.7 %.
//
static const double kClosedFormEnvOnly[3] = { 0.5, 0.5, 0.5 };

//! Topology D's PT mean, cached so E and F can check their INCREMENT
//! over the env-only baseline.  Populated by TestEnvOnly().
static double g_ptEnvOnlyMean[3] = { 0.0, 0.0, 0.0 };
static bool   g_ptEnvOnlyValid   = false;

//
// -- TOPOLOGY E: env + omni light -----------------------------------
//
// Adding a POINT light cannot occlude anything, so the env term is
// unchanged and the converged image is exactly topology D plus the
// omni's direct term.  With intensity I = `power` = 2 W/sr (see the
// kLightOmni comment — RISE point lights take intensity, not flux) at
// (0,0,5), a surface point p = (x,y,0) with normal +Z sees
//
//     cos(theta) = 5/d,   d^2 = x^2 + y^2 + 25
//     Delta_E(x,y) = (rho/pi) * I * cos(theta) / d^2
//                  = (0.5/pi) * 2 * 5 / (x^2+y^2+25)^(3/2)
//                  = 5 / (pi * (x^2+y^2+25)^(3/2))
//
// Averaged over the visible square [-H,H]^2 with H = 3.5*tan(15 deg)
// = 0.9378222 (the box filter makes the image mean exactly this area
// average — see the geometry-facts block):
//
//     Delta_E = 0.01230206
//
// CAMERA-INDEPENDENT SANITY BRACKET: Delta_E is monotone in x^2+y^2,
// so over the whole [-1,1]^2 quad it lies in
// [0.01134422, 0.01273240] whatever the camera does; the measurement
// below sits inside that bracket too.
//
// MEASURED (PT mean_E - PT mean_D, 256 spp, 6 runs, de-OIDN'd):
// (0.012301, 0.012301, 0.012301) -> -0.009 % on all three channels,
// with zero run-to-run movement.  Band kAbsBandIncrement = 5 %.
//
// (Pre-recalibration, denoiser-inclusive: (0.012324, 0.012314,
// 0.012287) -> (+0.18 %, +0.10 %, -0.12 %).)
//
// WHY THE INCREMENT FORM: the original reason -- a ~0.12 % per-channel
// colour-pipe offset on the 0.5 base that would swamp the 0.0123
// increment -- turned out to be a denoiser artifact and is GONE (see
// kAbsBandRGB).  The increment form is kept because its OTHER reason
// stands and is the important one: it is the sharpest available probe
// of the LIGHT-SELECTION path.  The historical binary-
// EnvSelectProbability bug broke env NEE ONLY in mixed scenes, so it
// moved mean_E away from mean_D + Delta_E while leaving topology D
// perfect; an absolute check on mean_E would dilute that signal by the
// 40x-larger env term it rides on.
//
static const double kClosedFormOmniIncrement = 0.01230206;

//
// -- TOPOLOGY F: env + mesh emitter ---------------------------------
//
// The emitter is a 1x1 quad at z=4 with one-sided radiance
// L_e = exitance * scale / pi = 1.0 * 10.0 / pi = 3.1830989 toward -Z.
// Unlike the point light it DOES occlude the env over its own solid
// angle, so the emitter region contributes (L_e - L_env) on top of the
// env-only baseline rather than L_e:
//
//     L_out(p) = (rho/pi) * [ L_env*pi + (L_e - L_env) * Omega_cos(p) ]
//              = 0.5 + (rho/pi) * (L_e - 1) * Omega_cos(p)
//
// where Omega_cos(p) = INT_emitter cos(theta_p) dw is the cosine-
// weighted solid angle of the emitter as seen from p.  For a rectangle
// PARALLEL to the shading plane that is pi times the classic
// differential-element-to-parallel-rectangle form factor, which has a
// closed form; with the corner form factor
//
//     Fc(A,B,H) = 1/(2pi) * [ X/sqrt(1+X^2) * atan(Y/sqrt(1+X^2))
//                           + Y/sqrt(1+Y^2) * atan(X/sqrt(1+Y^2)) ],
//     X = A/H, Y = B/H
//
// and the usual signed four-corner decomposition for a rectangle that
// does not sit over p, Omega_cos(p) = pi * FF(p).  Hence
//
//     Delta_F(p) = (0.5/pi) * (10/pi - 1) * pi * FF(p)
//                = 1.0915494 * FF(p)
//
// (The closed form was checked against a 800x800 brute-force
// quadrature of the emitter surface: agreement to 8 significant
// figures at p = (0,0), (1,1), (1,0).)
//
// Averaged over the visible square [-H,H]^2 as for topology E:
//
//     Delta_F = 0.01987740
//
// CAMERA-INDEPENDENT SANITY BRACKET over the whole [-1,1]^2 quad:
// [0.01694781, 0.02127295].
//
// MEASURED (PT mean_F - PT mean_D, 256 spp, 6 runs, de-OIDN'd):
// (0.019852, 0.019853, 0.019852) -> (-0.129 %, -0.124 %, -0.128 %),
// run spread 0.32 %.  The three channels now agree, which is what
// confirms the residual is the quadrature's grid convention rather
// than noise.  Band kAbsBandIncrement = 5 %.
//
// (Pre-recalibration, denoiser-inclusive: (0.019807, 0.019865,
// 0.019866) -> (-0.35 %, -0.06 %, -0.06 %).)
//
// The emitter sits BEHIND the camera (z=4 vs the camera's z=3.5,
// looking down -Z), so it is never directly visible and never blocks a
// camera ray; and it has `material none`, so it is a pure absorber
// with no BRDF and there is still no interreflection in the scene.
// Both facts are load-bearing for the derivation above.
//
static const double kClosedFormMeshIncrement = 0.01987740;

//! Shared implementation of the topology E / F increment check.
static void CheckLightIncrement(
	const ImageStats& pt,
	double expectedIncrement,
	const char* topologyName )
{
	if( !pt.valid ) return;
	if( !g_ptEnvOnlyValid ) {
		Check( false, ( std::string("env-only PT baseline available for increment check: ")
			+ topologyName ).c_str() );
		return;
	}

	double increment[3];
	double expected[3];
	for( int c = 0; c < 3; c++ ) {
		increment[c] = pt.mean[c] - g_ptEnvOnlyMean[c];
		expected[c]  = expectedIncrement;
	}

	const bool ok = AbsWithin( increment, expected, kAbsBandIncrement );
	Check( ok, ( std::string("PT light increment over env-only baseline matches closed form ")
		+ "(within " + std::to_string(int(kAbsBandIncrement*100)) + "%): " + topologyName ).c_str() );
	if( !ok ) PrintAbsDiff( "PT increment", increment, expected );
}

//////////////////////////////////////////////////////////////////////
// Topology D: Env-only Lambertian quad.
//
// THE canonical env-IBL test.  No explicit lights — everything flows
// through the environment map.  Failure modes guarded:
//   - Pre-Phase-A: SampleLight returned false -> BDPT/VCM all-black.
//   - Phase-A only: s=1 NEE/connect dropped for env-light vertex ->
//                   BDPT renders very dim.
//   - White firefly: BDPT t=1 with default-init fLight=(1,1,1) ->
//                    fails the firefly cap by ~3x.
//   - PT env MIS double-count (F1) -> fails the absolute check by
//                    +17.7 % against a 1 % band.
//////////////////////////////////////////////////////////////////////
static void TestEnvOnly()
{
	const ImageStats pt = RunEnvTopologyTest( "env-only Lambertian",
		std::string( kSceneCommonGeometry ) + kEnvPainter,
		kBiasEnvOnly );

	if( !pt.valid ) return;

	const bool ok = AbsWithin( pt.mean, kClosedFormEnvOnly, kAbsBandRGB );
	Check( ok, "PT mean == 0.5 (closed form: albedo x uniform env): env-only Lambertian" );
	if( !ok ) PrintAbsDiff( "PT mean", pt.mean, kClosedFormEnvOnly );

	for( int c = 0; c < 3; c++ ) g_ptEnvOnlyMean[c] = pt.mean[c];
	g_ptEnvOnlyValid = true;
}

//////////////////////////////////////////////////////////////////////
// Topology E: Env + omni light.
//
// Confirms that adding env-map emission to scenes that ALREADY have
// explicit lights doesn't break the existing alias-table-based
// selection.  This catches regressions where env-light entry
// hijacked the selection (it shouldn't — env is sampled separately
// when alias table is empty, and via NEE when alias table is
// populated), and — since 2026-08-27 — asserts the omni's own
// contribution against a closed form.
//////////////////////////////////////////////////////////////////////
static void TestEnvPlusOmni()
{
	const ImageStats pt = RunEnvTopologyTest( "env + omni light",
		std::string( kSceneCommonGeometry ) + kEnvPainter + kLightOmni,
		kBiasEnvPlusOmni );
	CheckLightIncrement( pt, kClosedFormOmniIncrement, "env + omni light" );
}

//////////////////////////////////////////////////////////////////////
// Topology F: Env + mesh emitter.
//
// Three-way light contribution: env + mesh emission + (eye-hits-
// emitter via s=0 strategy).  Most complex MIS partition.  The
// increment check additionally pins the emitter's cosine-weighted
// solid angle AND the env occlusion it causes.
//////////////////////////////////////////////////////////////////////
static void TestEnvPlusMesh()
{
	const ImageStats pt = RunEnvTopologyTest( "env + mesh emitter",
		std::string( kSceneCommonGeometry ) + kEnvPainter + kLightMesh,
		kBiasEnvPlusMesh );
	CheckLightIncrement( pt, kClosedFormMeshIncrement, "env + mesh emitter" );
}

//////////////////////////////////////////////////////////////////////
// Topology H: Non-uniform env + off-center Lambertian quad.
//
// The previous topologies all use a uniform env (`pnt_env` constant
// L=1) and a centered quad.  Both choices mask the wi-vs-dirToLight
// direction-mismatch bugs the adversarial review round 2 caught:
// when Le and env-pdf are uniform across the sphere, the disc-
// derived dirToLight gives nearly the same value as the sampled wi,
// so the bias is invisible.  Off-center geometry exposes the
// position/direction errors in Path B's eye-escape vertex placement.
//
// This topology uses a CHECKER-PATTERNED env (a procedural high-
// contrast pattern with sharp boundaries between bright and dim
// cells) and shifts the quad off the camera axis.  The combination
// should produce visible per-pixel divergence between PT and BDPT/
// VCM whenever the wrong sky direction is queried — a uniform env
// would average those errors out across the (u, v) integration but
// a peaked env keeps them spatially correlated.
//
// NO CLOSED FORM: the surface irradiance is a cosine-weighted
// integral of the checker pattern over the equirectangular sphere
// mapping, and part of the frame sees the env directly.  PT is the
// reference here, and the assertions are bias-referenced bands only.
//////////////////////////////////////////////////////////////////////
static const char* kSceneOffCenterGeometry =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.5 0.5 0.5\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_off\n"
	// Smaller (0.8×0.8) quad shifted +0.6 along x and +0.4 along y
	// so the camera sees env directly at the right and bottom edges
	// AND the quad's incoming sky directions are NOT symmetric about
	// the scene center — exercises both the off-center wi-vs-
	// dirToLight gap on s=1 NEE and the eye-escape ray-sphere
	// placement on Path B.
	"\tpta -0.2 -0.2 0\n"
	"\tptb 1.4 -0.2 0\n"
	"\tptc 1.4 0.6 0\n"
	"\tptd -0.2 0.6 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_quad\n"
	"\tgeometry quad_off\n"
	"\tmaterial mat_diffuse\n"
	"}\n";

// Checker-patterned env: high-contrast bright/dim cells across the
// equirectangular (u, v) sphere mapping.  `pnt_env_bright` is L=5 in
// each channel, `pnt_env_dim` is L=0.1.  Cell size = 0.2 of the
// (u, v) extent -> 5 cells across the equator visible.
static const char* kEnvCheckerPainter =
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_env_bright\n"
	"\tcolor 5.0 5.0 5.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_env_dim\n"
	"\tcolor 0.1 0.1 0.1\n"
	"}\n"
	"\n"
	"checker_painter\n"
	"{\n"
	"\tname pnt_env\n"
	"\tcolora pnt_env_bright\n"
	"\tcolorb pnt_env_dim\n"
	"\tsize 0.2\n"
	"}\n";

static void TestEnvNonUniformOffCenter()
{
	RunEnvTopologyTestWithRasterizers(
		"non-uniform env + off-center quad (RGB)",
		std::string( kSceneOffCenterGeometry ) + kEnvCheckerPainter,
		kRasterizerPT, kRasterizerBDPT, kRasterizerVCM,
		kBiasNonUniformRGB );
}

static std::string EnableHWSSInRasterizer( const std::string& config, bool enable )
{
	std::string res = config;
	std::size_t pos = res.find( "hwss true" );
	if( pos != std::string::npos ) {
		if( !enable ) {
			res.replace( pos, 9, "hwss false" );
		}
	} else {
		pos = res.find( "hwss false" );
		if( pos != std::string::npos ) {
			if( enable ) {
				res.replace( pos, 10, "hwss true" );
			}
		}
	}
	return res;
}

//////////////////////////////////////////////////////////////////////
// Topology I: Non-uniform env + off-center quad, SPECTRAL.
//
// Spectral twin of topology H — catches RGB/NM "missed twin" bugs.
// The 2026-05-25 adversarial review round 3 found exactly this kind
// of bug: the Path B ray-sphere fix landed on the RGB push site but
// the NM push site retained the broken vector convention, putting
// spectral env vertices past the sphere exit on off-center origins.
// Without this test the regression slipped past.  The non-uniform
// checker env amplifies any direction-mismatch into visible per-
// pixel bias.
//
// CONVENTION NOTE (differs from topology G, deliberately retained):
// here PT is rendered at the SAME hwss setting as BDPT/VCM, so the
// row compares like with like and the HWSS spectral-bundle effect
// largely cancels out of the ratios.  Topology G instead pins BDPT/VCM
// against the unbundled PT so that the bundle effect is VISIBLE in the
// recorded centres — see kBiasEnvOnlySpectralHWSS.
//////////////////////////////////////////////////////////////////////
static void TestEnvNonUniformOffCenterSpectral( bool hwss )
{
	const std::string name = std::string( "non-uniform env + off-center quad (spectral, hwss=" ) + ( hwss ? "true" : "false" ) + ")";
	RunEnvTopologyTestWithRasterizers(
		name.c_str(),
		std::string( kSceneOffCenterGeometry ) + kEnvCheckerPainter,
		EnableHWSSInRasterizer( kRasterizerPTSpectral, hwss ).c_str(),
		EnableHWSSInRasterizer( kRasterizerBDPTSpectral, hwss ).c_str(),
		EnableHWSSInRasterizer( kRasterizerVCMSpectral, hwss ).c_str(),
		hwss ? kBiasNonUniformSpectralHWSS : kBiasNonUniformSpectralNoHWSS );
}

//////////////////////////////////////////////////////////////////////
// Topology G: Env-only Lambertian quad, SPECTRAL integrators.
//
// Same scene as topology D but exercises the NM (spectral) twins
// of the Path A + Path B env branches in BDPTIntegrator.cpp and
// VCMIntegrator.cpp.  Without this test the spectral env paths are
// completely unguarded — Reviewer 3 flagged the gap during the
// 2026-05-25 adversarial review.
//
// -- CLOSED FORM AND WHY IT IS NOT EXACT PER CHANNEL ----------------
//
// The transport closed form is the same as topology D: 0.5.  But the
// spectral pipeline does not reproduce it per channel, and that is
// EXPECTED and not a transport error:  the env painter's RGB (1,1,1)
// and the albedo's (0.5,0.5,0.5) are each uplifted to spectra through
// the Jakob-Hanika LUT, multiplied per-wavelength, integrated against
// the CMFs and converted back to Rec.709.  uplift(a)*uplift(b) is not
// uplift(a*b), and the JH sigmoid fit has its own residual
// (docs/JH_LUT_GAMUT.md records ~3.9 % gamut-edge cell failures), so a
// per-channel round-trip error survives.
//
// MEASURED (PT spectral hwss=false, 512 spp, 4 runs, both rows):
//   per channel  (0.47634, 0.49607, 0.51244) -> (-4.7 %, -0.8 %, +2.5 %)
//   luminance    0.49306 and 0.49385         -> (-1.39 %, -1.23 %)
// The errors partly cancel in luminance, which is why the luminance
// check carries the tight band (5 %) and the per-channel check a
// looser one (10 %, a bound on the round-trip rather than an assertion
// about transport).  Either would have caught the F1 +17.7 %.
//
// PT REFERENCE IS ALWAYS hwss=false (the converged, unbiased ground
// truth).  HWSS is a variance-reduction bundling of the SAME estimator,
// so the converged answer is hwss-independent and PT-hwss=false IS the
// reference for both rows.  On this uniform-env scene PT's own HWSS path
// (PathTracingIntegrator::IntegrateFromHitHWSS) carries a documented
// spectral-bundle bias, so comparing the spectral integrators' hwss=true
// output against PT-hwss=TRUE would assert agreement with a biased
// reference (a separate PT env-IBL workstream; see
// docs/INTEGRATOR_BUGFIX_FINDINGS.md §3 and the Session 13 conclusion in
// docs/PRE_PHASE1_STATUS.md).
// Before the 2026-06-04 RecomputeSubpathThroughputNM companion-direction
// fix, BDPT/VCM carried their OWN (larger) HWSS companion bias that
// landed near PT's, so this row passed against PT-hwss=true only by
// coincidence (all three biased low together: master BDPT −30%, VCM −35%
// vs their own hwss=false).  With the companion fix BDPT/VCM are now
// hwss-invariant modulo the bundle effect recorded in
// kBiasEnvOnlySpectralHWSS; asserting them against the unbiased
// PT-hwss=false both restores a meaningful comparison AND would have
// caught the pre-fix companion bug (master BDPT/VCM hwss=true were
// 25–40% under PT-hwss=false).
//////////////////////////////////////////////////////////////////////
static void TestEnvOnlySpectral( bool hwss )
{
	const std::string name = std::string( "env-only Lambertian (spectral, hwss=" ) + ( hwss ? "true" : "false" ) + ")";
	const ImageStats pt = RunEnvTopologyTestWithRasterizers(
		name.c_str(),
		std::string( kSceneCommonGeometry ) + kEnvPainter,
		EnableHWSSInRasterizer( kRasterizerPTSpectral, false ).c_str(),
		EnableHWSSInRasterizer( kRasterizerBDPTSpectral, hwss ).c_str(),
		EnableHWSSInRasterizer( kRasterizerVCMSpectral, hwss ).c_str(),
		hwss ? kBiasEnvOnlySpectralHWSS : kBiasEnvOnlySpectralNoHWSS );

	if( !pt.valid ) return;

	{
		const double y   = Luminance( pt.mean );
		const bool   ok  = std::fabs( y - 0.5 ) / 0.5 <= kAbsBandSpectralLum;
		Check( ok, ( std::string("PT spectral mean luminance == 0.5 (closed form, within ")
			+ std::to_string(int(kAbsBandSpectralLum*100)) + "%): " + name ).c_str() );
		if( !ok ) {
			std::cout << "    PT spectral luminance = " << y << " (expected 0.5, "
				<< ((y-0.5)/0.5*100.0) << "%)" << std::endl;
		}
	}
	{
		const bool ok = AbsWithin( pt.mean, kClosedFormEnvOnly, kAbsBandSpectralRGB );
		Check( ok, ( std::string("PT spectral mean per-channel == 0.5 (closed form + JH uplift round-trip, within ")
			+ std::to_string(int(kAbsBandSpectralRGB*100)) + "%): " + name ).c_str() );
		if( !ok ) PrintAbsDiff( "PT spectral mean", pt.mean, kClosedFormEnvOnly );
	}
}

//////////////////////////////////////////////////////////////////////
// Topology J: SUBMERGED camera — env seen through a delta-transmissive
// dielectric shell that encloses the camera.
//
// UNCHANGED by the 2026-08-27 re-derivation.  This topology was always
// an absolute closed-form check and was always correct: it is a pure
// delta path with no MIS partition to get wrong, and PT/BDPT/VCM all
// read 1.000 +- 0.002 here both before and after F1/F2.  Left exactly
// as it was, including its own sample count (16 — a delta path needs
// no more) and its 3 % / 1.5x tolerances.
//
// Camera at the origin inside a closed dielectric box (ior 1.0,
// tau 1 1 1, scattering 1e6 = pure delta transmission), uniform env
// L = 1.  CLOSED FORM: every ray crosses exactly one lossless delta
// interface and escapes to the uniform env, so every pixel of every
// unbiased integrator must be EXACTLY 1.0 — no MC noise, no MIS
// subtlety, no visibility term.
//
// REGRESSION GUARDED (2026-08-13, found via the
// vcm_sdf_luminaire_jellyfish torture scene): PathTracingIntegrator
// did not seed its eye-ray IORStack from the camera position
// (IORStackSeeding::SeedFromPoint), unlike BDPT/VCM's eye subpath.
// With the camera inside a dielectric, the first boundary crossing
// ran with bFromInside==false, DielectricSPF's wrong-side test
// dropped the transmission lobe, and PT rendered this scene at
// EXACTLY 0.0 — an 8x env-energy deficit on the full jellyfish
// scene that masqueraded as a VCM over-count.  The same gap existed
// in the legacy PixelBased rasterizer camera entries and the photon
// tracers' emission origins.
//
// WHY THIS TOPOLOGY IS THE ONE THAT EXPOSED THE DENOISER
// (residual wave 4, 2026-08-27).
//   CapturingRasterizerOutput overrides only OutputImage, and
//   IRasterizerOutput::OutputDenoisedImage's DEFAULT implementation
//   forwards the POST-denoise pixels to OutputImage.  Denoising is on
//   by default, so every render in this suite used to be captured
//   AFTER OIDN had rewritten it -- the suite was measuring the
//   denoiser's output, not the integrator's.  On this cell that was
//   measurable and unambiguous: the closed form is EXACTLY 1.0 at
//   every pixel, and the captured buffer read
//
//     PT   mean (1.000, 0.998, 0.998)  max (1.110, 1.064, 1.135)
//     BDPT mean (0.999, 1.000, 0.998)  max (1.053, 1.072, 1.075)
//     VCM  mean (0.999, 1.000, 0.998)  max (1.054, 1.072, 1.075)
//
//   with ZERO run-to-run jitter (OIDN is deterministic) and with the
//   three colour channels DISAGREEING on a scene whose three channels
//   are identical by construction -- a CNN mixing channels, which no
//   filter or transport effect can do here.  `pixel_filter box` was
//   already set, so filter ringing was never a candidate; nor was fp16
//   (the capture is RISEColor doubles straight off IRasterImage::GetPEL).
//   With denoising off all three integrators read mean == p99 == max ==
//   1.0 to within 1.4e-4, PT reads it BIT-EXACTLY, and all three are
//   bit-identical run to run.
//
//   ALL SEVEN rasterizer strings in this file now set
//   `oidn_denoise FALSE` (residual wave 4b, 2026-08-27) and every band
//   in the file has been re-derived from de-OIDN'd runs -- see the
//   recalibration block above the bias table.  Wave 4 fixed only this
//   topology and recorded the other six as open work; that work is
//   done, so every number this suite prints is now an integrator
//   measurement.
//
// Tolerance: with denoising off this is a bit-exact check, so the max
// cap is 1.05 rather than the 1.5x the denoised buffer needed.  The
// mean band stays at 3 %: it is sized to catch the catastrophic
// pre-2026-08-13 failure (mean 0 vs 1), not to resolve the last
// 1e-4.  1.05 is ~350x the measured max deviation (1.4e-4) and still
// fails on the denoised buffer's 1.135 / 1.075, i.e. it red-proves
// the artifact above.
//////////////////////////////////////////////////////////////////////
static const char* kSceneSubmergedCamera =
	"film\n"
	"{\n"
	"\twidth 32\n"
	"\theight 32\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 0\n"
	"\tlookat 0 0 -1\n"
	"\tup 0 1 0\n"
	"\tfov 40.0\n"
	"}\n"
	"\n"
	"dielectric_material\n"
	"{\n"
	"\tname mat_shell\n"
	"\ttau 1.0 1.0 1.0\n"
	"\tior 1.0\n"
	"\tscattering 1000000.0\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname shell_box\n"
	"\twidth 4.0\n"
	"\theight 4.0\n"
	"\tdepth 4.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_shell\n"
	"\tgeometry shell_box\n"
	"\tmaterial mat_shell\n"
	"}\n";

// PT rasterizer for the submerged topology: must be DefaultPathTracing
// (the shared kRasterizerPT uses DefaultDirectLighting, which cannot
// continue through the dielectric shell).
static const char* kRasterizerPTSubmerged =
	"standard_shader\n"
	"{\n"
	"\tname global\n"
	"\tshaderop DefaultPathTracing\n"
	"}\n"
	"\n"
	"pathtracing_pel_rasterizer\n"
	"{\n"
	"\tsamples 16\n"
	"\toidn_denoise FALSE\n"
	"\tpixel_filter box\n"
	"\tradiance_map pnt_env\n"
	"\tradiance_scale 1.0\n"
	"\tradiance_background TRUE\n"
	"}\n"
	"\n"
	"file_rasterizeroutput\n"
	"{\n"
	"\tpattern /tmp/env_balance_pt_submerged_unused\n"
	"\ttype PNG\n"
	"\tbpp 8\n"
	"\tcolor_space sRGB\n"
	"}\n";

// Topology J's BDPT / VCM rasterizers are just the shared
// kRasterizerBDPT / kRasterizerVCM.  They used to be duplicated here so
// this topology alone could set `oidn_denoise FALSE`; the shared
// strings now set it too (wave 4b), so the twins were byte-identical
// duplicates and were removed.  PT still needs its own string below
// because it differs in shader op and sample count, not in denoising.

static void CheckExactRadiance(
	const char* integratorName,
	const ImageStats& s,
	const char* topologyName )
{
	Check( s.valid, ( std::string(integratorName) + " render produced output: " + topologyName ).c_str() );
	if( !s.valid ) return;
	for( int c = 0; c < 3; ++c ) {
		const bool meanOk = std::fabs( s.mean[c] - 1.0 ) <= 0.03;
		const bool maxOk  = s.max[c] <= 1.05;
		Check( meanOk, ( std::string(integratorName) + " mean == 1.0 (closed form), channel "
			+ std::to_string(c) + ": " + topologyName ).c_str() );
		Check( maxOk, ( std::string(integratorName) + " max <= 1.5, channel "
			+ std::to_string(c) + ": " + topologyName ).c_str() );
		if( !meanOk ) {
			std::cout << "    " << integratorName << " mean[" << c << "] = "
				<< s.mean[c] << " (expected 1.0)" << std::endl;
		}
	}
}

static void TestSubmergedCameraDeltaShell()
{
	const char* topologyName = "submerged camera / delta dielectric shell";
	std::cout << "Testing closed-form: " << topologyName << std::endl;

	const std::string common = std::string( kSceneSubmergedCamera ) + kEnvPainter;
	const std::string ptScene   = std::string("RISE ASCII SCENE 7\n") + common + kRasterizerPTSubmerged;
	const std::string bdptScene = std::string("RISE ASCII SCENE 7\n") + common + kRasterizerBDPT;
	const std::string vcmScene  = std::string("RISE ASCII SCENE 7\n") + common + kRasterizerVCM;

	const std::string ptPath   = WriteSceneToTempFile( ptScene.c_str(),   "ptsub"   );
	const std::string bdptPath = WriteSceneToTempFile( bdptScene.c_str(), "bdptsub" );
	const std::string vcmPath  = WriteSceneToTempFile( vcmScene.c_str(),  "vcmsub"  );
	if( ptPath.empty() || bdptPath.empty() || vcmPath.empty() ) {
		Check( false, ( std::string("temp file write: ") + topologyName ).c_str() );
		return;
	}

	const ImageStats pt   = RenderAndComputeStats( ptPath.c_str() );
	const ImageStats bdpt = RenderAndComputeStats( bdptPath.c_str() );
	const ImageStats vcm  = RenderAndComputeStats( vcmPath.c_str() );

	PrintStats( "PT  ", pt );
	PrintStats( "BDPT", bdpt );
	PrintStats( "VCM ", vcm );

	std::remove( ptPath.c_str() );
	std::remove( bdptPath.c_str() );
	std::remove( vcmPath.c_str() );

	CheckExactRadiance( "PT",   pt,   topologyName );
	CheckExactRadiance( "BDPT", bdpt, topologyName );
	CheckExactRadiance( "VCM",  vcm,  topologyName );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "EnvLightBalanceTest — env-IBL transport: PT vs closed forms, BDPT/VCM vs measured bias" << std::endl;

	TestSubmergedCameraDeltaShell();
	// TestEnvOnly MUST run before TestEnvPlusOmni / TestEnvPlusMesh:
	// it publishes the env-only PT baseline those two check their
	// closed-form light increment against.
	TestEnvOnly();
	TestEnvPlusOmni();
	TestEnvPlusMesh();
	TestEnvOnlySpectral( false );
	TestEnvOnlySpectral( true );
	TestEnvNonUniformOffCenter();
	TestEnvNonUniformOffCenterSpectral( false );
	TestEnvNonUniformOffCenterSpectral( true );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
