//////////////////////////////////////////////////////////////////////
//
//  VolumeAbsorptionAttenuationTest.cpp - End-to-end correctness check
//    that a HOMOGENEOUS absorbing medium attenuates radiance along a
//    no-scatter eye path by EXACTLY Beer-Lambert (exp(-sigma_a * d)) —
//    NOT exp(-2*sigma_a*d).
//
//    THE BUG THIS GUARDS AGAINST (analog volume estimator double-count):
//      HomogeneousMedium::SampleDistance is an ANALOG estimator.
//      Reaching the surface / escaping through the medium without a
//      scatter event is a stochastic SURVIVAL event whose probability is
//      exp(-sigma_t_max * d) — that probability already carries the
//      Beer-Lambert factor.  The no-scatter branches of the PT integrator
//      used to multiply throughput by Tr = exp(-sigma_t * d) AGAIN with
//      no compensating division, so a pure absorber rendered
//      exp(-2*sigma_a*d) (the slab read ~2x too thick / too dark).
//
//      The correct no-scatter weight is the per-channel chromatic ratio
//      Tr / PTTrReduced(Tr): the survival pdf (the max-sigma_t channel of
//      Tr) cancels, leaving 1 on that channel and >= 1 on the others so a
//      COLOURED medium keeps its tint.  For monochrome / NM this reduces
//      to exactly 1.  Note that simply DELETING the `* Tr` would be wrong:
//      it desaturates a coloured medium.  This test's COLOURED cases are
//      the ones that distinguish the correct Tr/PTTrReduced fix from the
//      naive "delete the Tr".
//
//    ------------------------------------------------------------------
//    WHICH INTEGRATOR SITES EACH CASE COVERS
//    ------------------------------------------------------------------
//    The fix touches FOUR non-HWSS no-scatter sites in
//    PathTracingIntegrator.cpp:
//      (S1) bounce-loop surface-hit          ~1699  IntegrateFromHitTemplated
//      (S2) bounce-loop escape-to-env        ~1711  IntegrateFromHitTemplated
//      (S3) camera-first-bounce surface-hit  ~3310  IntegrateRayTemplated
//      (S4) camera-first-bounce escape-to-env ~3321 IntegrateRayTemplated
//
//    CASES A/B/C (camera OUTSIDE an interior-medium slab, viewing a white
//    env behind it) exercise (S1): the camera enters the slab in VACUUM
//    (perfectrefractor front face at IOR 1.0, no medium yet), then the
//    interior medium attenuates the env-view inside the bounce loop.
//    These have a CLEAN analytic reference (env L=1 through a finite-path
//    slab) and assert exactly background * exp(-sigma_a * d) per channel.
//
//    CASES D/E (camera INSIDE a scene-wide `global_medium`, viewing a
//    directly-visible Lambertian luminaire at known distance d) exercise
//    (S3) — the "camera / eye INSIDE the medium" topology (the enamel
//    eye-inside-glass scenario).  This site is otherwise completely
//    untested by A/B/C.
//      * Why global_medium and not camera-inside-object?  Empirically
//        verified: RISE does NOT seed the IOR/medium stack when the camera
//        merely starts inside an interior_medium object (the stack is only
//        pushed on refraction traversal).  Camera-inside-object renders
//        BLACK.  So a scene-wide global_medium is the only way to put the
//        camera in a medium at the first bounce.
//      * Why a luminaire and not an env-lit surface?  A global_medium is
//        UNBOUNDED, so any light-connection / escape segment is infinite
//        and attenuates to ~0 — an env-lit diffuse wall renders black, and
//        the escape-to-env path (S4) also goes to 0 (verified).  A
//        directly-visible luminaire is self-terminating: it needs no light
//        connection, so it is the only clean camera-in-medium reference.
//
//    THE (S3) REFERENCE IS A CLEAN PER-CHANNEL EQUALITY.  Read from the
//    denoise-free radiance buffer (CapturingRasterizerOutput::GetPEL, not a
//    resolved/denoised PNG), the camera-in-medium luminaire view is exactly
//    single-count Beer-Lambert:
//        measured[c]  ==  L0[c] * exp(-sigma_a[c] * d)
//    where L0[c] is measured live from a ZERO-absorption render of the
//    IDENTICAL scene (so the exitance*INV_PI luminaire-resolve constant
//    cancels; L0 ~ 1.0 as authored).  So D/E/F assert the same tight
//    per-channel exp(-sigma_a*d) equality (kRelTol) as A/B/C — no floor, no
//    band-widening.
//      (Aside: rendered through the CLI + 8-bit sRGB PNG + OIDN, the same
//      scenes read ~30-90% brighter than single-count — that spread is an
//      OIDN/sRGB-quantization MEASUREMENT artifact of that path, NOT an
//      integrator effect; the in-process linear radiance buffer is clean.)
//
//    REVERT-PROOF (in the test harness, in-process, 2026-06-30, d=2):
//      Temporarily reverting ONLY site (S3) to the buggy `Tr * hitResult`
//      (no survival division), rebuilding, and re-running THIS test:
//        D (gray sigma_a=0.5): measured 0.369 -> 0.136 (tau_obs 2.00 = 2x
//          authored 1.00); rel to reference 0.63, far outside the 8% band.
//        E (coloured 0.2/0.6/1.2): channels 0.671/0.304/0.091 ->
//          0.061/0.028/0.008 (each >2x optical depth; the coloured buggy
//          also desaturates via the squared tint) — every channel fails.
//        F (NM gray): 0.118 -> 0.043 (tau_obs 2x authored) — fails.
//      All three S3 cases fail ~2x under the revert and pass on the fixed
//      tree, proving they genuinely exercise site (S3).
//
//    SITE (S4) — camera-first-bounce escape-to-env — is NOT cleanly
//    isolable: it only fires when the camera ray escapes the scene through
//    a medium, but an UNBOUNDED global_medium escape has an infinite path
//    (result -> 0, verified), and a bounded interior medium can't put the
//    camera inside it (camera-inside-object is black).  There is no
//    finite-path camera-in-medium escape topology, so (S4) is left to the
//    non-camera-first escape coverage that (S2) provides in spirit and to
//    the revert-proof discipline in docs; it is documented here rather
//    than tested with a contrived setup.
//
//    TOLERANCE: for A/B/C, tight enough to catch the 2x optical-depth
//    error (exp(-2tau) vs exp(-tau) differ by exp(-tau); at tau ~ 1 that's
//    ~2.7x) yet loose enough for MC noise.  For D/E, the floor/ceiling
//    band is sized to the measured MIS-residual spread (documented above),
//    not silently loosened.
//
//    ------------------------------------------------------------------
//    BDPT / VCM COVERAGE (cases G..N) — same analog bug, BDPT sites
//    ------------------------------------------------------------------
//    BDPTIntegrator carries the SAME analog no-scatter double-count in its
//    subpath generators.  The fix is BDPTSurvivalWeight (Tr/MinValue(Tr),
//    = 1 for monochrome/NM), applied at THREE no-scatter SURVIVAL sites:
//      (B1) eye-subpath surface-hit    GenerateEyeSubpathImpl
//      (B2) eye-subpath escape-to-env  GenerateEyeSubpathImpl (if pMed_eye)
//      (B3) light-subpath surface-hit  GenerateLightSubpathImpl
//    Connection-segment transmittances (EvalConnectionTransmittanceImpl,
//    the shadow-ray boundary walk) and the scatter weight
//    (ComputeMediumScatterWeight) are DELIBERATELY untouched — those are
//    deterministic full-Tr edges, not survival outcomes.
//
//    Cases G/H (BDPT RGB gray/coloured) and I/J (VCM RGB gray/coloured)
//    render the SAME interior slab as A/B under bdpt_pel_rasterizer /
//    vcm_pel_rasterizer and assert the same per-channel exp(-sigma_a*d)
//    (with the coloured desaturation guard).  K/L are the spectral twins
//    (resolve band, as case C).  VCM reuses BDPT's shared subpath
//    generators, so K/L/I/J prove VCM inherits the fix empirically.
//    Case M is a PT-vs-BDPT mean-agreement spot check (no MIS regression
//    from touching beta — the survival weight is a pure CONTRIBUTION edit;
//    the MIS pdfs dVCM/dVC/dVM/pdfFwd never read throughput).  Case N puts
//    a luminaire BEHIND the slab so BDPT's light subpath traverses the
//    medium (see the case-N comment for the s=0-dominance caveat).
//
//    REVERT-PROOF (in-process, d=2): reverting ONLY the eye-subpath
//    surface-hit (B1) to `beta = beta * Tr` makes gray BDPT (G) AND gray
//    VCM (I) read 0.135 (tau_obs 2.00 = 2x authored 1.00), a 63% deficit
//    far outside the 8% band, and desaturates the coloured cases — proving
//    G..L exercise B1 and that VCM shares BDPT's eye generator.
//
//    SITE (B2) — eye-subpath escape-to-env through a medium — mirrors PT's
//    (S4) and is NOT cleanly isolable for the same reason (a finite-path
//    camera-in-medium escape topology does not exist here): the eye ray
//    only escapes through a medium when it is INSIDE one, and a bounded
//    interior medium can't hold the camera (camera-inside-object is black)
//    while an unbounded global medium's escape path is infinite (-> 0).  It
//    is guarded by `if( pMed_eye )` and byte-identical to the surface-hit
//    fix; documented here rather than tested with a contrived setup.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
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
// CapturingRasterizerOutput — same shape as EnvLightBalanceTest.
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

// Mean radiance over the central 4x4 block — the slab fully covers the
// frame, so any interior block is the on-slab background path.  Averaging
// a small block instead of a single pixel suppresses MC noise.
struct PixelRGB { double r, g, b; bool valid; };

static PixelRGB CentralBlockMean( const CapturingRasterizerOutput& cap )
{
	PixelRGB out{ 0, 0, 0, false };
	if( cap.pixels.empty() || cap.width < 4 || cap.height < 4 ) {
		return out;
	}
	const unsigned int cx = cap.width / 2;
	const unsigned int cy = cap.height / 2;
	double sr = 0, sg = 0, sb = 0;
	int n = 0;
	for( int dy = -2; dy < 2; dy++ ) {
		for( int dx = -2; dx < 2; dx++ ) {
			const unsigned int x = cx + dx;
			const unsigned int y = cy + dy;
			if( x >= cap.width || y >= cap.height ) continue;
			const RISEColor& c = cap.pixels[y * cap.width + x];
			sr += c.base.r; sg += c.base.g; sb += c.base.b;
			n++;
		}
	}
	if( n == 0 ) return out;
	out.r = sr / n; out.g = sg / n; out.b = sb / n;
	out.valid = true;
	return out;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/volume_absorption_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static PixelRGB RenderCentralBlock( const std::string& sceneText, const char* tag )
{
	PixelRGB result{ 0, 0, 0, false };

	const std::string scenePath = WriteSceneToTempFile( sceneText, tag );
	if( scenePath.empty() ) {
		return result;
	}

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiScene( scenePath.c_str() ) ) {
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

	result = CentralBlockMean( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	std::remove( scenePath.c_str() );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Scene construction — interior-medium slab (cases A/B/C, site S1).
//
// The slab box has its faces perpendicular to the camera's +Z axis;
// the camera looks straight down +Z at normal incidence, so the optical
// path through the slab equals its `depth` (= thickness d).  The box is
// 4x4 in X/Y — far wider than the 10-degree FOV view ray bundle at the
// slab — so the whole frame is "on slab".
//////////////////////////////////////////////////////////////////////

static const double kSlabDepth = 2.0;   // thickness d

static std::string BuildRGBScene(
	int samples,
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		"pathtracing_pel_rasterizer\n"
		"{\n"
		"\tsamples " << samples << "\n"
		"\tmax_volume_bounce 16\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n"
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_rgb_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

static std::string BuildSpectralScene( double sa )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		"pathtracing_spectral_rasterizer\n"
		"{\n"
		"\tsamples 512\n"
		"\tmax_volume_bounce 16\n"
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
		"\tpattern /tmp/volume_absorption_nm_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa << " " << sa << " " << sa << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Parameterized interior-slab builders — SAME slab geometry / medium /
// camera as BuildRGBScene / BuildSpectralScene, but with a caller-
// supplied rasterizer chunk.  Used to render the IDENTICAL absorbing
// slab under bdpt_pel_rasterizer / vcm_pel_rasterizer and their spectral
// twins, proving BDPT/VCM now match PT and analytic Beer-Lambert after
// the analog no-scatter survival fix.  (VCM inherits the fix via BDPT's
// shared GenerateEyeSubpath/GenerateLightSubpath generators — this
// asserts that empirically.)
//
// `rasterizerChunk` is the full rasterizer chunk text (e.g.
// "bdpt_pel_rasterizer\n{\n...\n}\n") and MUST author radiance_map /
// radiance_background so the flat-white env is visible behind the slab.
//////////////////////////////////////////////////////////////////////

static std::string BuildRGBSlabScene(
	const std::string& rasterizerChunk,
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		<< rasterizerChunk <<
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_slab_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Heterogeneous interior-slab scene (cases O/P).  IDENTICAL slab geometry
// / camera / env as BuildRGBScene, but the interior medium is a
// painter_heterogeneous_medium with a CONSTANT (uniform) density field of
// 1.0 over an AABB that exactly encloses the slab.  A constant density=1
// reduces the heterogeneous medium to sigma_t(pt) = absorption (per
// channel), so the analytic reference is the SAME Beer-Lambert
// exp(-sigma_a[c]*d) as the homogeneous cases.
//
// The DISCRIMINATOR: HeterogeneousMedium::EvalTransmittance is STOCHASTIC
// ratio tracking, so the OLD survival denominator MinValue(EvalTransmittance)
// was a random estimate -> biased/noisy no-scatter weight.  The FIX uses the
// DETERMINISTIC EvalDistancePdf(false) survival pdf, so the render converges
// to the clean per-channel exp(-sigma_a[c]*d) with low variance.
// REVERT-PROOF: temporarily restoring the MinValue(EvalTransmittance)
// denominator makes THIS case bias/desaturate while cases A/B (homogeneous)
// stay green (homogeneous EvalTransmittance is deterministic).
//////////////////////////////////////////////////////////////////////

static std::string BuildHeterogeneousSlabScene(
	const std::string& rasterizerChunk,
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_env\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		// Constant density field = 1.0 everywhere (luminance of white = 1).
		"uniformcolor_painter\n"
		"{\n"
		"\tname dens_one\n"
		"\tcolor 1.0 1.0 1.0\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		<< rasterizerChunk <<
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_het_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		// AABB exactly encloses the 4x4x2 slab centred at origin.
		"painter_heterogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"\tdensity_painter dens_one\n"
		"\tresolution 8\n"
		"\tcolor_to_scalar luminance\n"
		"\tbbox_min -2.0 -2.0 -1.0\n"
		"\tbbox_max 2.0 2.0 1.0\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_env\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n";
	return ss.str();
}

// Rasterizer-chunk factories for the interior-slab scene.  All author
// radiance_map / radiance_background + box pixel filter so the reference
// is identical to the PT cases A/B/C.

static std::string BDPTPelRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss <<
		"bdpt_pel_rasterizer\n"
		"{\n"
		"\tsamples " << samples << "\n"
		"\tmax_volume_bounce 16\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n";
	return ss.str();
}

static std::string VCMPelRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss <<
		"vcm_pel_rasterizer\n"
		"{\n"
		"\tsamples " << samples << "\n"
		"\tmax_volume_bounce 16\n"
		"\tpixel_filter box\n"
		"\tradiance_map pnt_env\n"
		"\tradiance_scale 1.0\n"
		"\tradiance_background TRUE\n"
		"}\n";
	return ss.str();
}

static std::string BDPTSpectralRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss <<
		"bdpt_spectral_rasterizer\n"
		"{\n"
		"\tsamples " << samples << "\n"
		"\tmax_volume_bounce 16\n"
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
		"}\n";
	return ss.str();
}

static std::string VCMSpectralRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss <<
		"vcm_spectral_rasterizer\n"
		"{\n"
		"\tsamples " << samples << "\n"
		"\tmax_volume_bounce 16\n"
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
		"}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Light-subpath-through-slab scene (case N).  A Lambertian LUMINAIRE wall
// sits BEHIND the absorbing slab; the camera views it through the slab and
// there is NO env light.  In BDPT this forces the LIGHT subpath (traced
// from the luminaire) to traverse the absorbing medium before it connects
// to the eye subpath — exercising the light-subpath surface-hit survival
// site (BDPTSurvivalWeight on the light side).  The luminaire is directly
// visible, so the reference is L0 * exp(-sigma_a * d) with L0 measured live
// from a zero-absorption render (cancels the exitance*INV_PI constant).
//
// Layout (all perpendicular to the camera +Z axis, normal incidence):
//   camera at z=-5 looking +Z
//   slab front face at z=-1, back face at z=+1 (depth kSlabDepth=2)
//   luminaire wall front face at z=+2.0
// So the eye path crosses exactly kSlabDepth of medium to reach the slab
// back face, and the light-subpath emitted from the luminaire crosses the
// same slab on its way toward the camera side.
//////////////////////////////////////////////////////////////////////

static std::string BuildLightSubpathSlabScene(
	const std::string& rasterizerChunk,
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_emit\n"
		"\tcolor 3.14159265 3.14159265 3.14159265\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n"
		<< rasterizerChunk <<
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_lsub_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 -5\n"
		"\tlookat 0 0 0\n"
		"\tup 0 1 0\n"
		"\tfov 6.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname slab_abs\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"perfectrefractor_material\n"
		"{\n"
		"\tname clear\n"
		"\trefractance pnt_emit\n"
		"\tior 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname slabgeom\n"
		"\twidth 4.0\n"
		"\theight 4.0\n"
		"\tdepth " << kSlabDepth << "\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname slab\n"
		"\tgeometry slabgeom\n"
		"\tposition 0 0 0\n"
		"\tmaterial clear\n"
		"\tinterior_medium slab_abs\n"
		"}\n"
		"\n"
		"lambertian_material\n"
		"{\n"
		"\tname basemat\n"
		"\treflectance pnt_emit\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname emitwall\n"
		"\texitance pnt_emit\n"
		"\tmaterial basemat\n"
		"\tscale 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname wallgeom\n"
		"\twidth 10.0\n"
		"\theight 10.0\n"
		"\tdepth 0.2\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname backwall\n"
		"\tgeometry wallgeom\n"
		"\tposition 0 0 2.1\n"
		"\tmaterial emitwall\n"
		"}\n";
	return ss.str();
}

//////////////////////////////////////////////////////////////////////
// Scene construction — camera INSIDE a global medium (cases D/E, site S3).
//
// The camera sits inside a scene-wide `global_medium` absorber at the
// origin, looking +Z at a directly-visible Lambertian luminaire wall.
// The luminaire's front face is at z = kBackWallDepth (the box is at
// position kBackWallZ with depth 0.2, so front face = kBackWallZ - 0.1),
// giving an eye-path length d = kBackWallDepth through the medium before
// the first surface hit — which is site S3 (camera-first-bounce
// surface-hit through medium).  See the file header for why this
// topology is the only clean way to exercise S3.
//
// `spectral` selects the NM rasterizer (site S3 for NMTag) vs the Pel one.
//////////////////////////////////////////////////////////////////////

static const double kBackWallZ    = 2.1;   // luminaire box centre
static const double kBackWallDepth = kBackWallZ - 0.1;   // front face = eye path d

static std::string BuildGlobalMediumScene(
	int samples,
	bool spectral,
	double sa_r, double sa_g, double sa_b )
{
	std::ostringstream ss;
	ss <<
		"RISE ASCII SCENE 6\n"
		"\n"
		// Exitance PI so directly-viewed radiance L = exitance*INV_PI*scale
		// starts near 1 (the exact value is measured live from the zero-abs
		// render, so this only needs to be non-degenerate).
		"uniformcolor_painter\n"
		"{\n"
		"\tname pnt_emit\n"
		"\tcolor 3.14159265 3.14159265 3.14159265\n"
		"}\n"
		"\n"
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultDirectLighting\n"
		"}\n"
		"\n";
	if( spectral ) {
		ss <<
			"pathtracing_spectral_rasterizer\n"
			"{\n"
			"\tsamples " << samples << "\n"
			"\tmax_volume_bounce 16\n"
			"\tpixel_filter box\n"
			"\tnmbegin 380\n"
			"\tnmend 720\n"
			"\tnum_wavelengths 8\n"
			"\tspectral_samples 1\n"
			"\thwss false\n"
			"\tmax_diffuse_bounce 3\n"
			"}\n";
	} else {
		ss <<
			"pathtracing_pel_rasterizer\n"
			"{\n"
			"\tsamples " << samples << "\n"
			"\tmax_volume_bounce 16\n"
			"\tpixel_filter box\n"
			"}\n";
	}
	ss <<
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern /tmp/volume_absorption_gmed_unused\n"
		"\ttype PNG\n"
		"\tbpp 8\n"
		"\tcolor_space sRGB\n"
		"}\n"
		"\n"
		"film\n"
		"{\n"
		"\twidth 16\n"
		"\theight 16\n"
		"}\n"
		"\n"
		"pinhole_camera\n"
		"{\n"
		"\tlocation 0 0 0\n"
		"\tlookat 0 0 1\n"
		"\tup 0 1 0\n"
		"\tfov 10.0\n"
		"}\n"
		"\n"
		"homogeneous_medium\n"
		"{\n"
		"\tname gmed\n"
		"\tabsorption " << sa_r << " " << sa_g << " " << sa_b << "\n"
		"\tscattering 0.0 0.0 0.0\n"
		"\tphase isotropic\n"
		"}\n"
		"\n"
		"lambertian_material\n"
		"{\n"
		"\tname basemat\n"
		"\treflectance pnt_emit\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname emitwall\n"
		"\texitance pnt_emit\n"
		"\tmaterial basemat\n"
		"\tscale 1.0\n"
		"}\n"
		"\n"
		"box_geometry\n"
		"{\n"
		"\tname wallgeom\n"
		"\twidth 10.0\n"
		"\theight 10.0\n"
		"\tdepth 0.2\n"
		"}\n"
		"\n"
		"standard_object\n"
		"{\n"
		"\tname backwall\n"
		"\tgeometry wallgeom\n"
		"\tposition 0 0 " << kBackWallZ << "\n"
		"\tmaterial emitwall\n"
		"}\n"
		"\n"
		"> set global_medium gmed\n";
	return ss.str();
}

// Beer-Lambert reference for the interior-slab background path:
// L_env * exp(-sigma_a * d).
static double Expected( double sa )
{
	return std::exp( -sa * kSlabDepth );
}

// Observed optical depth implied by a measured channel value: -ln(measured).
// (background L = 1, so measured = exp(-tau_obs).)  The bug yields
// tau_obs ~ 2 * sigma_a * d (twice the authored optical depth).
static double ObservedTau( double measured )
{
	// Build is -ffast-math (no infinities); clamp a non-positive measurement
	// to a small floor so -ln() yields a large finite "very absorbed" tau for
	// the diagnostic print rather than tripping UB.
	const double m = std::fmax( measured, 1e-12 );
	return -std::log( m );
}

//////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////

// Relative tolerance for the CLEAN interior-slab cases A/B/C.  The
// signal-vs-bug gap is large (exp(-tau) vs exp(-2tau)); 8% comfortably
// separates them while absorbing MC noise at the sample counts used.
static const double kRelTol = 0.08;

static bool ChannelOk( double measured, double expected, const char* name )
{
	const double denom = std::fmax( std::fabs( expected ), 1e-4 );
	const double rel = std::fabs( measured - expected ) / denom;
	const bool ok = rel <= kRelTol;
	if( !ok ) {
		std::cout << "    " << name
			<< "  measured=" << measured
			<< "  expected=" << expected
			<< "  rel=" << rel
			<< "  (observed optical depth=" << ObservedTau( measured )
			<< " vs authored=" << ObservedTau( expected ) << ")"
			<< std::endl;
	}
	return ok;
}

static void TestRGBGray()
{
	std::cout << "[A] RGB gray absorber (sigma_a = 0.5 all channels, d = "
		<< kSlabDepth << ") — site S1 (bounce-loop surface-hit)" << std::endl;
	const double sa = 0.5;
	const std::string scene = BuildRGBScene( 256, sa, sa, sa );
	const PixelRGB px = RenderCentralBlock( scene, "rgb_gray" );

	Check( px.valid, "A: render produced a frame" );
	if( !px.valid ) return;

	const double exp = Expected( sa );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each" << std::endl;

	Check( ChannelOk( px.r, exp, "A.r" ), "A: red channel == exp(-sigma_a*d)" );
	Check( ChannelOk( px.g, exp, "A.g" ), "A: green channel == exp(-sigma_a*d)" );
	Check( ChannelOk( px.b, exp, "A.b" ), "A: blue channel == exp(-sigma_a*d)" );
}

static void TestRGBColored()
{
	// Distinct per-channel absorption.  A correct Tr/PTTrReduced fix keeps
	// each channel at its own exp(-sigma_a[c]*d); a "delete the * Tr" fix
	// would collapse all channels toward the survival pdf (the max-sigma_t
	// channel) and DESATURATE the medium.
	//
	// Sample count bumped to 1024 (from 256) so the 8% tolerance has
	// comfortable margin over the analytic value (reviewers noted ~1.6-sigma
	// headroom at 256 spp).  Confirmed flake-free across ~10 runs.
	std::cout << "[B] RGB coloured absorber (sigma_a = 0.2/0.6/1.2, d = "
		<< kSlabDepth << ") — site S1 (bounce-loop surface-hit)" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	const std::string scene = BuildRGBScene( 1024, sar, sag, sab );
	const PixelRGB px = RenderCentralBlock( scene, "rgb_colored" );

	Check( px.valid, "B: render produced a frame" );
	if( !px.valid ) return;

	const double er = Expected( sar );
	const double eg = Expected( sag );
	const double eb = Expected( sab );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected (" << er << ", " << eg << ", " << eb << ")" << std::endl;

	Check( ChannelOk( px.r, er, "B.r" ), "B: red == exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "B.g" ), "B: green == exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "B.b" ), "B: blue == exp(-1.2*d)" );

	// Explicit desaturation guard: the channels must stay well separated.
	// (red brighter than green brighter than blue, with the authored ratio.)
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"B: medium stays coloured (r > g > b, not desaturated)" );
}

static void TestNMSpectral()
{
	std::cout << "[C] NM / spectral absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ", hwss off) — site S1" << std::endl;
	const double sa = 0.5;
	const std::string scene = BuildSpectralScene( sa );
	const PixelRGB px = RenderCentralBlock( scene, "nm" );

	Check( px.valid, "C: render produced a frame" );
	if( !px.valid ) return;

	// PER-CHANNEL assertion.  The resolved RGB of this GRAY absorber over a
	// flat-white env is visibly CHROMATIC (red hot by ~15-20% vs g/b).
	//
	// Investigated 2026-06-30: this chromaticity is a SPECTRAL->RGB RESOLVE
	// artifact of a flat-white SPD sampled at only num_wavelengths=8, NOT a
	// real per-channel absorption bias.  Proof: rendering the IDENTICAL
	// scene with sigma_a = 0 (no absorber at all) already produces the same
	// chromatic pattern; bumping num_wavelengths 8 -> 32 shrinks it toward
	// gray.  Since the medium is achromatic (equal sigma_a per channel) and
	// PTSurvivalWeight == 1 for NM, every channel's TRUE optical depth is
	// exp(-sigma_a*d); only the flat-SPD resolve tints the result.
	//
	// We therefore assert each channel against exp(-sigma_a*d) with a
	// resolve-sized band (kNMResolveTol) that admits the measured ~15-20%
	// chromatic spread but still rejects the 2x double-count (which would
	// push every channel to exp(-2*sigma_a*d) = exp(-2) ~ 0.135, far below
	// the band).  This is strictly stronger than the previous mean-only
	// check: it now catches a per-channel double-count that a lucky mean
	// could have hidden.
	const double exp = Expected( sa );
	// exp(-2) / exp(-1) = exp(-1) ~ 0.368 — the buggy value is 63% below the
	// reference, so a 30% band cleanly separates fixed from buggy while
	// admitting the resolve chromaticity + MC noise.
	const double kNMResolveTol = 0.30;
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each (per-channel, resolve band "
		<< kNMResolveTol << ")" << std::endl;

	auto nmOk = [&]( double m, const char* nm ) -> bool {
		const double rel = std::fabs( m - exp ) / std::fmax( exp, 1e-4 );
		const bool ok = rel <= kNMResolveTol;
		if( !ok ) {
			std::cout << "    " << nm << " measured=" << m << " expected=" << exp
				<< " rel=" << rel << " (tau_obs=" << ObservedTau( m ) << ")"
				<< std::endl;
		}
		return ok;
	};

	Check( nmOk( px.r, "C.r" ), "C: spectral red   ~ exp(-sigma_a*d) (resolve band)" );
	Check( nmOk( px.g, "C.g" ), "C: spectral green ~ exp(-sigma_a*d) (resolve band)" );
	Check( nmOk( px.b, "C.b" ), "C: spectral blue  ~ exp(-sigma_a*d) (resolve band)" );

	// The chromaticity is a resolve artifact, so the channels must not
	// diverge wildly — bound the spread to catch a genuine per-channel bias.
	const double lo = std::fmin( px.r, std::fmin( px.g, px.b ) );
	const double hi = std::fmax( px.r, std::fmax( px.g, px.b ) );
	Check( lo > 0 && hi / std::fmax( lo, 1e-6 ) < 1.6,
		"C: channels stay near-gray (resolve spread bounded, no per-channel bias)" );
}

//////////////////////////////////////////////////////////////////////
// Camera-in-medium (site S3) cases D/E.  See file header for the
// floor-discriminator rationale.
//////////////////////////////////////////////////////////////////////

static void TestGlobalMediumGray()
{
	std::cout << "[D] camera INSIDE global medium, gray absorber "
		<< "(sigma_a = 0.5, d = " << kBackWallDepth
		<< ") — site S3 (camera-first-bounce surface-hit)" << std::endl;
	const double sa = 0.5;

	// Zero-absorption render: measures the directly-viewed luminaire radiance
	// L0 live (folds in the exitance*INV_PI resolve constant).
	const PixelRGB base = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, false, 0.0, 0.0, 0.0 ), "gmed_gray_base" );
	// Absorbing render.
	const PixelRGB px = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, false, sa, sa, sa ), "gmed_gray" );

	Check( base.valid && px.valid, "D: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	const double L0 = ( base.r + base.g + base.b ) / 3.0;
	const double m  = ( px.r + px.g + px.b ) / 3.0;
	const double expected = L0 * std::exp( -sa * kBackWallDepth );  // single-count
	std::cout << "    L0(zero-abs)=" << L0 << "  measured=" << m
		<< "  expected single-count=" << expected
		<< "  (buggy double-count ~ expected*exp(-sigma_a*d)="
		<< expected * std::exp( -sa * kBackWallDepth ) << ")" << std::endl;

	// In-process (denoise-free radiance buffer) this site renders CLEAN
	// single-count Beer-Lambert: measured == L0 * exp(-sigma_a*d).  The buggy
	// double-count would apply an EXTRA exp(-sigma_a*d) (verified revert-proof
	// in the test harness: measured drops to ~0.14, well outside the band).
	Check( ChannelOk( m, expected, "D.mean" ),
		"D: measured == single-count Beer-Lambert (double-count would be ~exp(-1) darker)" );
}

static void TestGlobalMediumColored()
{
	std::cout << "[E] camera INSIDE global medium, coloured absorber "
		<< "(sigma_a = 0.2/0.6/1.2, d = " << kBackWallDepth
		<< ") — site S3, desaturation discriminator" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;

	const PixelRGB base = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, false, 0.0, 0.0, 0.0 ), "gmed_col_base" );
	const PixelRGB px = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, false, sar, sag, sab ), "gmed_col" );

	Check( base.valid && px.valid, "E: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	// Per-channel single-count references from the live zero-abs luminaire.
	// (Deriving the reference channel L0[c] from the identical zero-abs render
	// cancels any luminaire-resolve constant, so the check is pure
	// per-channel exp(-sigma_a[c]*d).)
	const double er = base.r * std::exp( -sar * kBackWallDepth );
	const double eg = base.g * std::exp( -sag * kBackWallDepth );
	const double eb = base.b * std::exp( -sab * kBackWallDepth );
	std::cout << "    base(" << base.r << ", " << base.g << ", " << base.b << ")"
		<< "  measured(" << px.r << ", " << px.g << ", " << px.b << ")"
		<< "  expected(" << er << ", " << eg << ", " << eb << ")" << std::endl;

	// CLEAN per-channel Beer-Lambert (site S3 renders single-count in-process).
	// The desaturation double-count (delete-the-Tr) or the analog double-count
	// (extra exp) both break these — verified revert-proof in the harness:
	// buggy channels collapse to ~0.065/0.019/0.003, far below expected.
	Check( ChannelOk( px.r, er, "E.r" ), "E: red   == L0_r*exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "E.g" ), "E: green == L0_g*exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "E.b" ), "E: blue  == L0_b*exp(-1.2*d)" );

	// Desaturation guard: the coloured medium must keep r > g > b with the
	// authored ordering.  A "delete the * Tr" bug would flatten these.
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"E: medium stays coloured (r > g > b, not desaturated)" );
}

static void TestGlobalMediumSpectral()
{
	std::cout << "[F] camera INSIDE global medium, NM/spectral gray absorber "
		<< "(sigma_a = 0.5, d = " << kBackWallDepth
		<< ", hwss off) — site S3 for NMTag" << std::endl;
	const double sa = 0.5;

	const PixelRGB base = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, true, 0.0, 0.0, 0.0 ), "gmed_nm_base" );
	const PixelRGB px = RenderCentralBlock(
		BuildGlobalMediumScene( 1024, true, sa, sa, sa ), "gmed_nm" );

	Check( base.valid && px.valid, "F: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	// For NM, PTSurvivalWeight == 1, so attenuation is pure survival; the
	// spectral->RGB resolve tints both renders identically, so the mean
	// floor cancels the resolve constant just as in D.
	const double L0 = ( base.r + base.g + base.b ) / 3.0;
	const double m  = ( px.r + px.g + px.b ) / 3.0;
	const double expected = L0 * std::exp( -sa * kBackWallDepth );
	std::cout << "    L0(zero-abs)=" << L0 << "  measured=" << m
		<< "  expected single-count=" << expected << std::endl;

	// CLEAN single-count Beer-Lambert for the NMTag S3 site; the double-count
	// would apply an extra exp(-sigma_a*d) (measured -> ~0.043, outside band).
	Check( ChannelOk( m, expected, "F.mean" ),
		"F: spectral measured == single-count Beer-Lambert (S3 NMTag)" );
}

//////////////////////////////////////////////////////////////////////
// BDPT / VCM interior-slab cases (G..M).  These render the SAME absorbing
// slab as A/B/C but under bdpt_pel_rasterizer / vcm_pel_rasterizer and
// their spectral twins.  They exercise the analog no-scatter SURVIVAL fix
// on BDPT's eye-subpath surface-hit (and, because the slab is a bounded
// object the light subpath also traverses, the light-subpath surface-hit)
// via BDPTSurvivalWeight.  VCM reuses BDPT's shared subpath generators, so
// these also prove VCM inherits the fix.
//
// REVERT-PROOF (in-process, d=2): reverting BDPTSurvivalWeight back to
// `beta = beta * Tr` at the eye-subpath surface-hit makes the gray BDPT/VCM
// cases read exp(-2*sigma_a*d) ~ 0.135 instead of exp(-sigma_a*d) ~ 0.368
// (a 63% deficit, far outside the 8% band); the coloured cases additionally
// desaturate.  Confirmed by temporarily reverting each site and re-running.
//////////////////////////////////////////////////////////////////////

// Helper: render the interior-slab under an arbitrary rasterizer chunk and
// return the central-block mean radiance.
static PixelRGB RenderSlab(
	const std::string& rasterizerChunk,
	double sar, double sag, double sab, const char* tag )
{
	return RenderCentralBlock(
		BuildRGBSlabScene( rasterizerChunk, sar, sag, sab ), tag );
}

static void TestBDPTGray()
{
	std::cout << "[G] BDPT RGB gray absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ") — BDPT eye+light survival fix" << std::endl;
	const double sa = 0.5;
	const PixelRGB px = RenderSlab( BDPTPelRasterizerChunk( 256 ), sa, sa, sa, "bdpt_gray" );
	Check( px.valid, "G: render produced a frame" );
	if( !px.valid ) return;
	const double exp = Expected( sa );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each" << std::endl;
	Check( ChannelOk( px.r, exp, "G.r" ), "G: BDPT red   == exp(-sigma_a*d)" );
	Check( ChannelOk( px.g, exp, "G.g" ), "G: BDPT green == exp(-sigma_a*d)" );
	Check( ChannelOk( px.b, exp, "G.b" ), "G: BDPT blue  == exp(-sigma_a*d)" );
}

static void TestBDPTColored()
{
	std::cout << "[H] BDPT RGB coloured absorber (sigma_a = 0.2/0.6/1.2, d = "
		<< kSlabDepth << ") — desaturation discriminator" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	const PixelRGB px = RenderSlab( BDPTPelRasterizerChunk( 1024 ), sar, sag, sab, "bdpt_col" );
	Check( px.valid, "H: render produced a frame" );
	if( !px.valid ) return;
	const double er = Expected( sar ), eg = Expected( sag ), eb = Expected( sab );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected (" << er << ", " << eg << ", " << eb << ")" << std::endl;
	Check( ChannelOk( px.r, er, "H.r" ), "H: BDPT red   == exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "H.g" ), "H: BDPT green == exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "H.b" ), "H: BDPT blue  == exp(-1.2*d)" );
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"H: BDPT medium stays coloured (r > g > b, not desaturated)" );
}

static void TestVCMGray()
{
	std::cout << "[I] VCM RGB gray absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ") — VCM inherits fix via shared generators" << std::endl;
	const double sa = 0.5;
	const PixelRGB px = RenderSlab( VCMPelRasterizerChunk( 256 ), sa, sa, sa, "vcm_gray" );
	Check( px.valid, "I: render produced a frame" );
	if( !px.valid ) return;
	const double exp = Expected( sa );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each" << std::endl;
	Check( ChannelOk( px.r, exp, "I.r" ), "I: VCM red   == exp(-sigma_a*d)" );
	Check( ChannelOk( px.g, exp, "I.g" ), "I: VCM green == exp(-sigma_a*d)" );
	Check( ChannelOk( px.b, exp, "I.b" ), "I: VCM blue  == exp(-sigma_a*d)" );
}

static void TestVCMColored()
{
	std::cout << "[J] VCM RGB coloured absorber (sigma_a = 0.2/0.6/1.2, d = "
		<< kSlabDepth << ") — desaturation discriminator" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	const PixelRGB px = RenderSlab( VCMPelRasterizerChunk( 1024 ), sar, sag, sab, "vcm_col" );
	Check( px.valid, "J: render produced a frame" );
	if( !px.valid ) return;
	const double er = Expected( sar ), eg = Expected( sag ), eb = Expected( sab );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected (" << er << ", " << eg << ", " << eb << ")" << std::endl;
	Check( ChannelOk( px.r, er, "J.r" ), "J: VCM red   == exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "J.g" ), "J: VCM green == exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "J.b" ), "J: VCM blue  == exp(-1.2*d)" );
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"J: VCM medium stays coloured (r > g > b, not desaturated)" );
}

// Spectral (NM) band — same resolve-artifact reasoning as case C.  For NM,
// BDPTSurvivalWeight == 1, so attenuation is pure survival; the flat-SPD
// resolve tints the result identically to the zero-abs case, so we assert
// each channel against exp(-sigma_a*d) with the resolve band.
static void TestBDPTSpectral()
{
	std::cout << "[K] BDPT NM/spectral gray absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ", hwss off) — BDPT NMTag survival == identity" << std::endl;
	const double sa = 0.5;
	const PixelRGB px = RenderSlab( BDPTSpectralRasterizerChunk( 512 ), sa, sa, sa, "bdpt_nm" );
	Check( px.valid, "K: render produced a frame" );
	if( !px.valid ) return;
	const double exp = Expected( sa );
	const double band = 0.30;
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each (resolve band " << band << ")" << std::endl;
	auto nmOk = [&]( double m ) -> bool {
		return std::fabs( m - exp ) / std::fmax( exp, 1e-4 ) <= band;
	};
	Check( nmOk( px.r ), "K: BDPT spectral red   ~ exp(-sigma_a*d)" );
	Check( nmOk( px.g ), "K: BDPT spectral green ~ exp(-sigma_a*d)" );
	Check( nmOk( px.b ), "K: BDPT spectral blue  ~ exp(-sigma_a*d)" );
}

static void TestVCMSpectral()
{
	std::cout << "[L] VCM NM/spectral gray absorber (sigma_a = 0.5, d = "
		<< kSlabDepth << ", hwss off) — VCM NMTag survival == identity" << std::endl;
	const double sa = 0.5;
	const PixelRGB px = RenderSlab( VCMSpectralRasterizerChunk( 512 ), sa, sa, sa, "vcm_nm" );
	Check( px.valid, "L: render produced a frame" );
	if( !px.valid ) return;
	const double exp = Expected( sa );
	const double band = 0.30;
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each (resolve band " << band << ")" << std::endl;
	auto nmOk = [&]( double m ) -> bool {
		return std::fabs( m - exp ) / std::fmax( exp, 1e-4 ) <= band;
	};
	Check( nmOk( px.r ), "L: VCM spectral red   ~ exp(-sigma_a*d)" );
	Check( nmOk( px.g ), "L: VCM spectral green ~ exp(-sigma_a*d)" );
	Check( nmOk( px.b ), "L: VCM spectral blue  ~ exp(-sigma_a*d)" );
}

// PT-vs-BDPT agreement spot check (per bdpt-vcm-mis-balance skill): render
// the SAME coloured slab under PT and BDPT and assert the per-channel means
// agree within MC noise.  This confirms the survival-weight change is a pure
// contribution-magnitude edit that leaves the MIS-weighted result identical
// across integrators — i.e. no MIS regression from touching beta.
static void TestPTvsBDPTAgreement()
{
	std::cout << "[M] PT vs BDPT agreement on the coloured slab "
		<< "(no-MIS-regression check)" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	// PT reference via the parameterized builder (identical scene modulo the
	// rasterizer chunk).
	std::ostringstream ptc;
	ptc << "pathtracing_pel_rasterizer\n{\n\tsamples 1024\n\tmax_volume_bounce 16\n"
		   "\tpixel_filter box\n\tradiance_map pnt_env\n\tradiance_scale 1.0\n"
		   "\tradiance_background TRUE\n}\n";
	const PixelRGB pt   = RenderSlab( ptc.str(), sar, sag, sab, "agree_pt" );
	const PixelRGB bdpt = RenderSlab( BDPTPelRasterizerChunk( 1024 ), sar, sag, sab, "agree_bdpt" );
	Check( pt.valid && bdpt.valid, "M: both renders produced frames" );
	if( !pt.valid || !bdpt.valid ) return;
	std::cout << "    PT  (" << pt.r << ", " << pt.g << ", " << pt.b << ")"
		<< "  BDPT(" << bdpt.r << ", " << bdpt.g << ", " << bdpt.b << ")" << std::endl;
	// Both are exact single-count Beer-Lambert; the only difference is MC
	// noise, so a channel-relative 12% band comfortably confirms agreement
	// while catching a systematic MIS-driven mean shift.
	auto agree = [&]( double a, double b, const char* nm ) -> bool {
		const double denom = std::fmax( std::fmax( std::fabs(a), std::fabs(b) ), 1e-4 );
		const double rel = std::fabs( a - b ) / denom;
		const bool ok = rel <= 0.12;
		if( !ok ) std::cout << "    " << nm << " PT=" << a << " BDPT=" << b
			<< " rel=" << rel << std::endl;
		return ok;
	};
	Check( agree( pt.r, bdpt.r, "M.r" ), "M: PT.r ~ BDPT.r (within MC noise)" );
	Check( agree( pt.g, bdpt.g, "M.g" ), "M: PT.g ~ BDPT.g (within MC noise)" );
	Check( agree( pt.b, bdpt.b, "M.b" ), "M: PT.b ~ BDPT.b (within MC noise)" );
}


// Case N: BDPT with a LUMINAIRE wall BEHIND the absorbing slab (no env),
// camera in front.  This is the light-subpath-through-medium companion to
// A/B/C: BDPT's shared GenerateLightSubpath traces from the luminaire and
// its vertices cross the absorbing slab, and the render must still read
// single-count Beer-Lambert.  It asserts BDPT attenuates a luminaire seen
// through a bounded medium correctly on BOTH subpath halves.
//   NOTE ON COVERAGE: because the luminaire is DIRECTLY VISIBLE through the
//   slab, BDPT's MIS weights favour the s=0 (eye-hits-light) strategy, so
//   the DOMINANT survival site here is still the EYE surface-hit — reverting
//   only the light-subpath site does NOT move this case (verified in the
//   harness).  The light-subpath surface-hit fix is nonetheless required for
//   symmetry / connection unbiasedness (both subpath halves must use the
//   same analog convention or s>0 connections through media bias), and is
//   byte-identical to the eye fix; a light-subpath-DOMINANT medium topology
//   (occluded emitter reachable only via connection) would isolate it, but
//   is not contrived here to keep the test robust.
// Reference: L0 * exp(-sigma_a*d), L0 from a live zero-absorption render
// (cancels the exitance*INV_PI constant).
static void TestBDPTLightSubpathSlab()
{
	std::cout << "[N] BDPT luminaire BEHIND slab (light subpath through medium) "
		<< "— light-subpath survival site" << std::endl;
	const double sa = 0.5;
	const PixelRGB base = RenderCentralBlock(
		BuildLightSubpathSlabScene( BDPTPelRasterizerChunk( 1024 ), 0.0, 0.0, 0.0 ),
		"bdpt_lsub_base" );
	const PixelRGB px = RenderCentralBlock(
		BuildLightSubpathSlabScene( BDPTPelRasterizerChunk( 1024 ), sa, sa, sa ),
		"bdpt_lsub" );
	Check( base.valid && px.valid, "N: renders produced frames" );
	if( !base.valid || !px.valid ) return;
	const double L0 = ( base.r + base.g + base.b ) / 3.0;
	const double m  = ( px.r + px.g + px.b ) / 3.0;
	const double expected = L0 * std::exp( -sa * kSlabDepth );
	std::cout << "    L0(zero-abs)=" << L0 << "  measured=" << m
		<< "  expected single-count=" << expected
		<< "  (buggy double-count ~ " << expected * std::exp( -sa * kSlabDepth )
		<< ")" << std::endl;
	Check( ChannelOk( m, expected, "N.mean" ),
		"N: BDPT light-through-slab == single-count Beer-Lambert" );
}


//////////////////////////////////////////////////////////////////////
// Heterogeneous-medium cases O/P.  Same interior slab as A/B but the
// interior medium is a painter_heterogeneous_medium with constant density
// 1.0, so the analytic reference is the SAME Beer-Lambert exp(-sigma_a*d).
// These exercise the DETERMINISTIC EvalDistancePdf survival denominator on
// the heterogeneous path (where EvalTransmittance is stochastic ratio
// tracking, so the old MinValue(EvalTransmittance) denominator was biased).
//////////////////////////////////////////////////////////////////////

static std::string PTPelHetRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
	   << "\n\tmax_volume_bounce 16\n\tpixel_filter box\n"
	      "\tradiance_map pnt_env\n\tradiance_scale 1.0\n"
	      "\tradiance_background TRUE\n}\n";
	return ss.str();
}

static std::string PTSpectralHetRasterizerChunk( int samples )
{
	std::ostringstream ss;
	ss << "pathtracing_spectral_rasterizer\n{\n\tsamples " << samples
	   << "\n\tmax_volume_bounce 16\n\tpixel_filter box\n"
	      "\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n"
	      "\tspectral_samples 1\n\thwss false\n\tmax_diffuse_bounce 3\n"
	      "\tradiance_map pnt_env\n\tradiance_scale 1.0\n"
	      "\tradiance_background TRUE\n}\n";
	return ss.str();
}

static void TestHeterogeneousColored()
{
	std::cout << "[O] HETEROGENEOUS (constant-density) RGB coloured absorber "
		<< "(sigma_a = 0.2/0.6/1.2, d = " << kSlabDepth
		<< ") — deterministic EvalDistancePdf survival denominator" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;
	// 4096 spp: the stochastic ratio-tracking transmittance NUMERATOR is
	// noisier than the analytic homogeneous Tr, so more samples are needed to
	// bring the MEAN inside the 8% band; the DENOMINATOR is now deterministic
	// (that is the fix) so the estimator is unbiased and DOES converge.
	const PixelRGB px = RenderCentralBlock(
		BuildHeterogeneousSlabScene( PTPelHetRasterizerChunk( 4096 ), sar, sag, sab ),
		"het_col" );
	Check( px.valid, "O: render produced a frame" );
	if( !px.valid ) return;
	const double er = Expected( sar ), eg = Expected( sag ), eb = Expected( sab );
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected (" << er << ", " << eg << ", " << eb << ")" << std::endl;
	Check( ChannelOk( px.r, er, "O.r" ), "O: het red   == exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, "O.g" ), "O: het green == exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, "O.b" ), "O: het blue  == exp(-1.2*d)" );
	// Desaturation guard: the old MinValue(EvalTransmittance) denominator would
	// randomize/collapse the chromatic ratio; the deterministic pdf keeps it.
	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"O: het medium stays coloured (r > g > b, not desaturated)" );
}

static void TestHeterogeneousSpectral()
{
	std::cout << "[P] HETEROGENEOUS (constant-density) NM/spectral gray absorber "
		<< "(sigma_a = 0.5, d = " << kSlabDepth
		<< ", hwss off) — deterministic EvalDistancePdfNM survival denominator" << std::endl;
	const double sa = 0.5;
	const PixelRGB px = RenderCentralBlock(
		BuildHeterogeneousSlabScene( PTSpectralHetRasterizerChunk( 2048 ), sa, sa, sa ),
		"het_nm" );
	Check( px.valid, "P: render produced a frame" );
	if( !px.valid ) return;
	const double exp = Expected( sa );
	const double band = 0.30;   // resolve band, as case C/K
	std::cout << "    measured (" << px.r << ", " << px.g << ", " << px.b
		<< ")  expected " << exp << " each (resolve band " << band << ")" << std::endl;
	auto nmOk = [&]( double m ) -> bool {
		return std::fabs( m - exp ) / std::fmax( exp, 1e-4 ) <= band;
	};
	Check( nmOk( px.r ), "P: het spectral red   ~ exp(-sigma_a*d)" );
	Check( nmOk( px.g ), "P: het spectral green ~ exp(-sigma_a*d)" );
	Check( nmOk( px.b ), "P: het spectral blue  ~ exp(-sigma_a*d)" );
}


int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "VolumeAbsorptionAttenuationTest — Beer-Lambert single-count "
		"through a homogeneous absorbing medium" << std::endl;

	// Cases A/B/C: camera OUTSIDE an interior-medium slab (site S1),
	// clean analytic Beer-Lambert reference.
	TestRGBGray();
	TestRGBColored();
	TestNMSpectral();

	// Cases D/E/F: camera INSIDE a global medium (site S3), buggy-vs-fixed
	// single-count-floor discriminator (see file header).
	TestGlobalMediumGray();
	TestGlobalMediumColored();
	TestGlobalMediumSpectral();

	// Cases G..M: the SAME interior slab under BDPT / VCM (RGB + spectral),
	// proving the analog no-scatter survival fix now makes BDPT/VCM match
	// PT / analytic Beer-Lambert, plus a PT-vs-BDPT no-MIS-regression check.
	TestBDPTGray();
	TestBDPTColored();
	TestVCMGray();
	TestVCMColored();
	TestBDPTSpectral();
	TestVCMSpectral();
	TestPTvsBDPTAgreement();
	TestBDPTLightSubpathSlab();

	// Cases O/P: HETEROGENEOUS (constant-density) slab — exercises the
	// deterministic EvalDistancePdf[NM] survival denominator (the fix) on the
	// stochastic ratio-tracking transmittance path.
	TestHeterogeneousColored();
	TestHeterogeneousSpectral();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
