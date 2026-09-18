//////////////////////////////////////////////////////////////////////
//
//  MLTSpectralHWSSNormalizationTest.cpp - Regression guard for DL-126
//    review round 4 (P1-1): MLTSpectralRasterizer::EvaluateSampleSpectral
//    used to normalize its HWSS branch by the CONSTANT
//    `nSpectralSamples * SampledWavelengths::N`, independent of how many
//    companion wavelengths a bundle's mid-loop `swl.TerminateSecondary()`
//    actually excluded (a dispersive delta vertex, pre-existing; a
//    null-BSDF continuation vertex, DL-126's own round-2 addition).  A
//    fixed denominator makes every termination a pure DARKENING with no
//    compensating change below it -- BDPT/VCM's own spectral rasterizers
//    already divided by an ACTIVE-wavelength count (`totalActive`) and
//    so were immune; MLT was not, and this is the row the round-3/4
//    review asked to have "gated" by an automated test rather than only
//    an ad-hoc scene.
//
//    Fixed by `activeWavelengthCount` (the hero, plus every companion
//    NOT excluded by a mid-loop termination) replacing the constant
//    `nSpectralSamples * SampledWavelengths::N` in the final
//    normalization.  See MLTSpectralRasterizer.cpp's own comments at
//    that counter's declaration and the final `totalWavelengths`
//    computation for the full derivation, and
//    docs/DL126_BDPT_NULL_BSDF_CONTINUATION.md sec 7.6 (P1-1) for the
//    measured before/after numbers this test's rows reproduce.
//
//    APPROACH: mirror BDPTStrategyBalanceTest.cpp's own
//    RunSpectralHWSSLadder pattern (hwss FALSE vs TRUE, achromatic mean
//    ratio) but under `mlt_spectral_rasterizer` instead of
//    `bdpt_spectral_rasterizer` -- MLT's own PSSMLTSampler-driven bundle
//    loop is a genuinely different code path from BDPT/VCM's per-pixel
//    HWSS loop (different file, different normalization bug), so it
//    needs its own render-level red-proof rather than inheriting BDPT's.
//
//    Two rows, matching the two termination sources the fix touches:
//      1. Null-BSDF receiver (`biospec_skin_material`) -- the DL-126
//         addition to `TerminateSecondary()`'s call sites.
//      2. Dispersive glass sphere (wavelength-dependent `ior`) -- the
//         PRE-EXISTING `HasDispersiveDeltaVertex` termination this fix
//         also happens to cure, since both share one normalization
//         denominator.
//
//    Tolerance: MLT's PSSMLT mutation chains are a different variance
//    process from BDPT/VCM's independent per-pixel samples, and at the
//    small image / mutation counts this test uses to stay fast, run to
//    run noise is larger than BDPT's own 8-10% band.  25% catches the
//    pre-fix defect (measured 65-73% deficit on both rows) with wide
//    margin while tolerating MLT's own noise floor.
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
// CapturingRasterizerOutput -- see BDPTStrategyBalanceTest.cpp for the
// full rationale; duplicated here per this codebase's existing
// convention of each test .cpp being a self-contained translation unit.
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
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}

	double sum[3] = { 0, 0, 0 };
	bool allFinite = true;
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		const double r = c.base.r * cov, g = c.base.g * cov, b = c.base.b * cov;
		if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) {
			allFinite = false;
			break;
		}
		sum[0] += r; sum[1] += g; sum[2] += b;
	}
	if( !allFinite ) {
		return ImageStats{};
	}

	for( int c = 0; c < 3; c++ ) {
		s.mean[c] = sum[c] / double( cap.pixels.size() );
	}
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const char* sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/mlt_hwss_norm_%s_%d.RISEscene",
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

	// Fresh libc seed per render; worker scheduling still makes repeats
	// non-bit-reproducible (see rise-render-seeding memory note).
	static unsigned renderIndex = 0;
	std::srand( 4126u + renderIndex++ );
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
		std::cout << "    " << label << ": INVALID (render failed)" << std::endl;
		return;
	}
	std::cout << "    " << label
	          << ": mean=(" << s.mean[0] << "," << s.mean[1] << "," << s.mean[2] << ")"
	          << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Shared scene fragments.
//////////////////////////////////////////////////////////////////////

// Mesh emitter, identical to BDPTStrategyBalanceTest.cpp's kLightMesh
// (small flat luminary at z=4, facing the receiver below).
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
	"\tscale 20.0\n"
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

// Receiver: biospec_skin_material at defaults -- GetBSDF()==0, the exact
// null-BSDF continuation DL-126 exists for.  Camera/film match
// BDPTStrategyBalanceTest.cpp's kSceneNullBSDFSkin.
static const char* kSceneNullBSDFSkin =
	"film\n"
	"{\n"
	"\twidth 24\n"
	"\theight 24\n"
	"}\n\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n\n"
	"biospec_skin_material\n"
	"{\n"
	"\tname mat_skin\n"
	"}\n\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_skin\n"
	"\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname obj_skin\n"
	"\tgeometry quad_skin\n"
	"\tmaterial mat_skin\n"
	"}\n";

// Receiver: dispersive dielectric sphere (wavelength-dependent ior),
// the PRE-EXISTING `HasDispersiveDeltaVertex` termination source this
// same normalization fix also cures.  Small resolution/depth to keep
// the test fast; the sphere sits between camera and the mesh emitter
// so most eye subpaths pass through it.
static const char* kSceneDispersiveGlass =
	"film\n"
	"{\n"
	"\twidth 24\n"
	"\theight 24\n"
	"}\n\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n\n"
	"scalar_painter\n"
	"{\n"
	"\tname pnt_trans_scalar\n"
	"\tfile colors/flat_1.spectra\n"
	"}\n\n"
	"dielectric_material\n"
	"{\n"
	"\tname mat_glass\n"
	"\ttau pnt_trans_scalar\n"
	"\tior 1.3 1.5 2\n"
	"\tscattering 1000000\n"
	"}\n\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_floor\n"
	"\tcolor 0.6 0.6 0.6\n"
	"}\n\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_floor\n"
	"\treflectance pnt_floor\n"
	"}\n\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname quad_floor\n"
	"\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname obj_floor\n"
	"\tgeometry quad_floor\n"
	"\tmaterial mat_floor\n"
	"}\n\n"
	"sphere_geometry\n"
	"{\n"
	"\tname sph_glass\n"
	"\tradius 0.6\n"
	"}\n\n"
	"standard_object\n"
	"{\n"
	"\tname obj_glass\n"
	"\tgeometry sph_glass\n"
	"\tposition 0 0 1.2\n"
	"\tmaterial mat_glass\n"
	"}\n";

static std::string MakeMLTRasterizer( bool hwss, const char* patternTag )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"\n"
		"mlt_spectral_rasterizer\n"
		"{\n"
		"\tmax_eye_depth 5\n"
		"\tmax_light_depth 5\n"
		"\tbootstrap_samples 20000\n"
		"\tchains 32\n"
		"\tmutations_per_pixel 400\n"
		"\tlarge_step_prob 0.3\n"
		"\tnmbegin 405\n"
		"\tnmend 705\n"
		"\tspectral_samples 1\n"
		"\thwss %s\n"
		"\tpixel_filter box\n"
		"\toidn_denoise FALSE\n"
		"}\n"
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern rendered/mlt_hwss_norm_%s_unused\n"
		"\ttype EXR\n"
		"\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n"
		"}\n",
		hwss ? "TRUE" : "FALSE", patternTag );
	return std::string( buf );
}

//////////////////////////////////////////////////////////////////////
// RunMLTHWSSLadder -- renders the same scene body under
// mlt_spectral_rasterizer hwss FALSE and TRUE, and checks the
// achromatic mean ratio stays within tolerance of 1.0.  A pre-fix
// build (constant `totalWavelengths` denominator) darkens the hwss
// TRUE render by the fraction of companions a mid-bundle
// TerminateSecondary() excluded -- 65-73% on these two scenes,
// measured directly against an isolated pre-fix build during this
// row's own red-proof.
//////////////////////////////////////////////////////////////////////
static void RunMLTHWSSLadder( const char* label, const std::string& sceneBody )
{
	std::cout << "Testing MLT spectral hwss FALSE vs TRUE on " << label << std::endl;

	const std::string sceneNo = std::string( "RISE ASCII SCENE 7\n" )
		+ MakeMLTRasterizer( false, "false" ) + sceneBody;
	const std::string sceneHW = std::string( "RISE ASCII SCENE 7\n" )
		+ MakeMLTRasterizer( true, "true" ) + sceneBody;

	const std::string pathNo = WriteSceneToTempFile( sceneNo.c_str(), "nohwss" );
	const std::string pathHW = WriteSceneToTempFile( sceneHW.c_str(), "hwss" );
	if( pathNo.empty() || pathHW.empty() ) {
		Check( false, ( std::string("temp file write: ") + label ).c_str() );
		return;
	}

	const ImageStats noHWSS = RenderAndComputeStats( pathNo.c_str() );
	const ImageStats hwss   = RenderAndComputeStats( pathHW.c_str() );

	PrintStats( "hwss FALSE", noHWSS );
	PrintStats( "hwss TRUE ", hwss );

	std::remove( pathNo.c_str() );
	std::remove( pathHW.c_str() );

	Check( noHWSS.valid, ( std::string("MLT spectral hwss FALSE render produced output: ") + label ).c_str() );
	Check( hwss.valid,   ( std::string("MLT spectral hwss TRUE render produced output: ") + label ).c_str() );
	if( !noHWSS.valid || !hwss.valid ) return;

	const double achroNo = ( noHWSS.mean[0] + noHWSS.mean[1] + noHWSS.mean[2] ) / 3.0;
	const double achroHW = ( hwss.mean[0]   + hwss.mean[1]   + hwss.mean[2]   ) / 3.0;
	Check( achroNo > 1e-6, ( std::string("hwss FALSE achromatic mean is non-zero: ") + label ).c_str() );
	if( achroNo <= 1e-6 ) return;

	const double ratio = achroHW / achroNo;
	std::cout << "    ACHROMATIC mean: hwss FALSE = " << achroNo
	          << ", hwss TRUE = " << achroHW
	          << ", ratio = " << ratio
	          << "  (" << ( ( ratio - 1.0 ) * 100.0 ) << "%)" << std::endl;

	// See file header for the 25% band rationale.
	Check( ratio > 0.75 && ratio < 1.25,
		( std::string("DL-126/DL-200 P1-1: MLT spectral hwss TRUE achromatic mean stays within 25% of hwss FALSE: ") + label ).c_str() );
}

static void TestMLTHWSSNullBSDFNormalization()
{
	RunMLTHWSSLadder( "null-BSDF skin receiver (DL-126 P1-1)",
		std::string( kSceneNullBSDFSkin ) + kLightMesh );
}

static void TestMLTHWSSDispersiveGlassNormalization()
{
	RunMLTHWSSLadder( "dispersive glass sphere (DL-126 P1-1, pre-existing dispersion-dilution instance)",
		std::string( kSceneDispersiveGlass ) + kLightMesh );
}

int main()
{
	std::cout << "=== MLTSpectralHWSSNormalizationTest ===" << std::endl;

	TestMLTHWSSNullBSDFNormalization();
	TestMLTHWSSDispersiveGlassNormalization();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
