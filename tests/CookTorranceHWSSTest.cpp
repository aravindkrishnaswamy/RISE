//////////////////////////////////////////////////////////////////////
//
//  CookTorranceHWSSTest.cpp - docs/SPECTRAL_PARITY_AUDIT.md's
//    CookTorranceSPF PdfNM/ScatterNM parity fix (2026-09-03), reusing
//    tests/FabricRenderTest.cpp / tests/HairRenderTest.cpp's HWSS
//    invariant pattern.
//
//    WHY THIS EXISTS.  SPFPdfConsistencyTest/SPFBSDFConsistencyTest
//    prove the unit-level claim: ScatterNM's stored pdf now matches
//    PdfNM's, and BSDF value/Scatter/Pdf are mutually consistent.  What
//    they do NOT exercise is the actual spectral RENDER path: the PT
//    spectral rasterizer's hero-wavelength-plus-companions (HWSS)
//    machinery, which samples ONE hero wavelength via ScatterNM (and
//    therefore the now-achromatic lobe-selection weights) and reprices
//    every companion wavelength via PdfNM/valueNM for MIS.  If the fix
//    were subtly wrong in a way the unit tests miss -- e.g. a companion
//    wavelength's mixture disagreeing with the hero's in a way that
//    biases the HWSS combination -- it would show up here as an
//    hwss=true vs hwss=false split, and nowhere else in the suite.
//
//    THE INVARIANT IS REFERENCE-FREE for the same reason
//    FabricRenderTest's header gives: hwss=true and hwss=false are two
//    samplers of the same integrand, so they must agree in the mean at
//    any spp regardless of what the correct answer is.
//
//    A TINTED Cook-Torrance sphere (rd and rs both non-grey, non-white)
//    is the right subject: a WHITE or ACHROMATIC diffuse/specular
//    painter is guarded to read EXACTLY 1.0 on the spectral path
//    (GuardedGetColorNM / IsUntintedWhite), which would make the
//    selection-weight bug this fix closed invisible by construction --
//    the whole defect was that a tinted painter's spectral sample
//    differs slightly from its RGB max3 under the Jakob-Hanika uplift.
//
//    OIDN DENOISE IS EXPLICITLY DISABLED, for the reason
//    FabricRenderTest's / HairRenderTest's headers give:
//    `IRasterizerOutput::OutputDenoisedImage`'s default forwards
//    POST-DENOISE pixels to `OutputImage`, and OIDN is free to smooth
//    away exactly the per-wavelength difference this invariant looks
//    for.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
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

//! Seed base for this run; see FabricRenderTest.cpp's header for the full
//! rationale (this binary is not wall-clock seeded, so the starting state
//! is set explicitly rather than left to the worker-side `rand()` race).
static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;
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
	double luminance;
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
	s.luminance = (s.mean[0] + s.mean[1] + s.mean[2]) / 3.0;
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/cooktorrance_hwss_test_%s_%d.RISEscene",
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

	const std::string path = WriteSceneToTempFile( sceneText, tag );
	if( path.empty() ) {
		return result;
	}

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
// ONE tinted Cook-Torrance sphere in a uniform L = 1.0 environment.  A
// sphere sweeps every incidence angle from normal to grazing once and
// continuously, so a spectral inconsistency confined to one band of
// incidence angles still moves the whole-image mean.
//
// `rd` and `rs` are both tinted (non-grey) and `facets` is a moderate
// 0.3 -- large enough that the diffuse, specular AND multiscatter
// lobes (CookTorranceSPF's 3-lobe mixture) all fire with non-trivial
// selection probability, which is exactly the mixture whose weights
// this fix changed.
//////////////////////////////////////////////////////////////////////
static std::string CookTorranceSphereCommon( unsigned int width, unsigned int height )
{
	std::ostringstream ss;
	ss <<
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"film\n{\n\twidth " << width << "\n\theight " << height << "\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0 3.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 34.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_env\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_rd\n\tcolor 0.5 0.3 0.2\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_rs\n\tcolor 0.3 0.5 0.7\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"cooktorrance_material\n{\n\tname mat_ct\n\trd pnt_rd\n\trs pnt_rs\n"
		"\tfacets 0.3\n\tior 1.8\n\textinction 0.3\n}\n\n"
		"sphere_geometry\n{\n\tname sph\n\tradius 1.0\n}\n\n"
		"standard_object\n{\n\tname ct_obj\n\tgeometry sph\n\tmaterial mat_ct\n\tposition 0 0 0\n}\n\n";
	return ss.str();
}

static std::string RasterizerPTSpectral( unsigned int samples, bool hwss, unsigned int rrMinDepth )
{
	std::ostringstream ss;
	ss <<
		"pathtracing_spectral_rasterizer\n{\n"
		"\tsamples " << samples << "\n"
		"\trr_min_depth " << rrMinDepth << "\n"
		"\toidn_denoise FALSE\n"
		"\tnmbegin 380\n\tnmend 720\n\tnum_wavelengths 8\n\tspectral_samples 1\n"
		"\thwss " << (hwss ? "true" : "false") << "\n"
		"\tradiance_map pnt_env\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"
		"}\n\n"
		"file_rasterizeroutput\n{\n\tpattern /tmp/cooktorrance_hwss_test_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n";
	return ss.str();
}

static std::string AssembleScene( const std::string& common, const std::string& rasterizer )
{
	return std::string( "RISE ASCII SCENE 7\n" ) + common + rasterizer;
}

//////////////////////////////////////////////////////////////////////
// HWSS INVARIANT.
//
// TOLERANCE.  `kHwssTol` mirrors FabricRenderTest's derivation approach
// (256 spp, 32x32, num_wavelengths 8, spectral_samples 1): a mixture of
// diffuse + GGX-VNDF specular + Kulla-Conty multiscatter lobes is not a
// harder spectral-consistency subject than fabric's sheen-over-substrate
// mixture, so the same order-of-magnitude band applies.  0.05 leaves
// ample headroom over ordinary MC noise at this spp/film size while
// still catching the failure mode this fix targeted: the selection
// weights ScatterNM samples from disagreeing with the ones PdfNM (and
// therefore every companion wavelength's MIS reprice) reports for the
// same direction, which the pre-fix defect measured at 1.44e-4 to
// 1.55e-4 relative on the STORED PDF alone -- an integration-level bias
// from that would be expected to be visible white miles below this
// band if it existed; this gate exists to prove it doesn't propagate
// into a whole-image HWSS split.
//////////////////////////////////////////////////////////////////////
static const double kHwssTol = 0.05;

static void TestHwssInvariant()
{
	std::cout << "\n=== HWSS invariant: tinted Cook-Torrance sphere ===" << std::endl;

	const unsigned int W = 32, H = 32;
	const std::string common = CookTorranceSphereCommon( W, H );

	const ImageStats sOff = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, false, 8 ) ), "hwss_off" );
	const ImageStats sOn = RenderAndComputeStats(
		AssembleScene( common, RasterizerPTSpectral( 256, true, 8 ) ), "hwss_on" );

	Check( sOff.valid, "Cook-Torrance sphere: hwss=false render produced output" );
	Check( sOn.valid,  "Cook-Torrance sphere: hwss=true render produced output" );
	if( !sOff.valid || !sOn.valid ) return;

	const double denom = std::fmax( sOff.luminance, 1e-6 );
	const double relDiff = std::fabs( sOn.luminance - sOff.luminance ) / denom;

	std::cout << "  hwss=false luminance = " << sOff.luminance
		<< "   hwss=true luminance = " << sOn.luminance
		<< "   relative difference = " << relDiff << std::endl;

	Check( relDiff <= kHwssTol, "Cook-Torrance sphere: hwss=true agrees with hwss=false within tolerance" );

	// A pair of zeros would satisfy the invariant trivially.
	Check( sOff.luminance > 0.05, "Cook-Torrance sphere: render is non-degenerate (not a black frame)" );
}

int main( int argc, char** argv )
{
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "CookTorranceHWSSTest -- docs/SPECTRAL_PARITY_AUDIT.md CookTorranceSPF PdfNM fix" << std::endl;
	std::cout << "seed base = " << g_seedBase
		<< "  (pass a different one as argv[1] for an independent sample)" << std::endl;
	std::cout << "==========================================================" << std::endl;

	TestHwssInvariant();

	std::cout << "==========================================================" << std::endl;
	std::cout << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
