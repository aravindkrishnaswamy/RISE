//////////////////////////////////////////////////////////////////////
//
//  RadianceEtaScaleGradedIndexTest.cpp - closed-form guard on DL-09
//    (docs/DEBT_LEDGER.md): `RadianceEtaScale` reads the IOR stack's
//    push/pop-time (ENTRY) value for `before`, while `DielectricSPF`
//    (and `PerfectRefractorSPF`) re-fetch the `ior` painter FRESH at
//    every hit, entry or exit.  For a spatially-VARYING `ior` (bound to
//    a `scalar_painter` that depends on the hit position), the value
//    fetched at an EXIT hit can differ from the value pushed at the
//    matching ENTRY hit -- and it is the EXIT-hit value that the SPF's
//    own Fresnel/Snell computation actually used.
//
//    THE SCENE.  A single `box_geometry` dielectric slab, `ior` bound
//    to a `scalar_painter { expression "1.8 - 2.0*P.y" }`.  Box height
//    0.3, centred so the top face sits at world Y = 0.3 (ior 1.2, the
//    ENTRY hit for a camera looking straight down) and the bottom face
//    at Y = 0.0 (ior 1.8, the EXIT hit).  A Lambertian luminaire quad
//    (exitance 1, L = 1/pi) sits just below the box; a pinhole camera
//    looks straight down through the slab from above at a narrow FOV
//    (5 degrees) so every ray is within ~1.7 degrees of normal
//    incidence -- Snell bending and the angular Fresnel departure from
//    the closed form are negligible (matches the same convention
//    tests/RefractiveRadianceScalingTest.cpp Row A uses at 15 degrees,
//    here tightened further since this row does not have that row's
//    margin to spare).
//
//    THE CLOSED FORM.  With T1 = 1 - R0(1.2), T2 = 1 - R0(1.8) the two
//    interfaces' normal-incidence Fresnel transmittances (R0(n) =
//    ((n-1)/(n+1))^2, UNCHANGED by this fix -- both DielectricSPF
//    branches already price their OWN Fresnel with the fresh per-hit
//    ior; only the SEPARATE eta^2 throughput multiplier is at stake),
//    and etaScale the net (eta_before/eta_after)^2 factor the two
//    crossings contribute:
//
//      BUGGY   (before this fix): etaScale = 1 EXACTLY.  The entry
//        event's (air/1.2)^2 and the exit event's (1.2/air)^2 --
//        using the STALE entry value, not the fresh 1.8 the SPF's own
//        Optics::CalculateRefractedRay/CalculateDielectricReflectance
//        calls used at exit -- telescope to 1 regardless of how the
//        ior actually varies between the two hits.
//
//      CORRECT (after this fix): etaScale = (1.8/1.2)^2 = 2.25.  The
//        exit event's eta_before is now the SAME fresh value (1.8)
//        the SPF's own exit Fresnel calculation used, not the stale
//        entry-time stack top.
//
//    L_expected = (1/pi) * T1 * T2 * etaScale.  The two predictions
//    differ by exactly 2.25x -- unmistakable against any plausible MC
//    noise or angular-incidence correction at this geometry.  A second-
//    order internal-bounce correction (the ray reflecting once inside
//    the slab before re-exiting) is bounded by R0(1.2)*R0(1.8) =
//    0.00068 (0.068%), three orders of magnitude below the signal.
//
//    CONTROL ROW.  The same scene with `ior` a plain uniform 1.5 (no
//    spatial variation, entry == exit == 1.5): etaScale telescopes to
//    1 exactly regardless of the fix (before.top() and the fresh value
//    agree everywhere), so this row must be UNCHANGED before and after
//    -- the fix's "keep the uniform-ior behaviour byte-identical"
//    requirement, independent of tests/RefractiveRadianceScalingTest.cpp
//    (which this file does not re-run; see the gate list in this row's
//    commit message instead).
//
//    RED-PROOF -- this file's own run on the UNFIXED library (before
//    the etaBeforeOverride fix), seed base 1000: see the fix commit
//    message for the captured numbers.
//
//  Author: Claude (debt-precision slice, DL-09)
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
// Capture / stats scaffolding -- same shape as
// tests/RefractiveRadianceScalingTest.cpp (independently duplicated;
// this file must stand alone as its own regression).
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

	for( int c = 0; c < 3; c++ ) { s.mean[c] = 0; }
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		const double v[3] = { c.base.r * cov, c.base.g * cov, c.base.b * cov };
		for( int ch = 0; ch < 3; ch++ ) {
			s.mean[ch] += v[ch];
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
		"/tmp/radiance_eta_graded_%s_%d.RISEscene",
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

//! Normal-incidence Fresnel reflectance between air (n=1) and `n`.
static double R0( double n )
{
	const double r = ( n - 1.0 ) / ( n + 1.0 );
	return r * r;
}

static std::string RasterizerPT( const char* samples )
{
	return std::string(
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " ) + samples + "\n"
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

//////////////////////////////////////////////////////////////////////
// Scene: single dielectric slab, `ior` bound to a `scalar_painter`.
// `iorBody` is the scalar_painter's `expression` body (or, for the
// control row, a plain numeric literal bound directly with no
// scalar_painter chunk at all, exercising the ordinary uniform-ior
// path unchanged).
//////////////////////////////////////////////////////////////////////
static std::string SceneGradedSlab( const char* iorRef, bool needsPainterChunk, const char* exprBody )
{
	std::string s =
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0.0 2.5 0.001\n"
		"\tlookat 0 0 0\n"
		"\tup 0 0 1\n"
		"\tfov 5.0\n"
		"}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -1 -0.05 1\n\tptb 1 -0.05 1\n\tptc 1 -0.05 -1\n\tptd -1 -0.05 -1\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n";

	if( needsPainterChunk ) {
		// `expression` takes the REST OF THE LINE unquoted (see
		// scenes/Tests/Painters/scalar_painter_expression_roughness.RISEscene) --
		// no surrounding quotes.
		s += std::string( "scalar_painter\n{\n\tname ior_graded\n\texpression " ) + exprBody + "\n}\n\n";
	}

	s += std::string(
		"dielectric_material\n{\n\tname mat_slab\n\tior " ) + iorRef + "\n"
		"\ttau 1.0\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_slab\n\twidth 4\n\theight 0.3\n\tdepth 4\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n"
		"\tmaterial mat_slab\n\tposition 0 0.15 0\n}\n";

	return s;
}

static void RunGradedRow()
{
	// Entry (top face, world Y = 0.3) ior = 1.2, exit (bottom face,
	// world Y = 0.0) ior = 1.8:  ior_graded = 1.8 - 2.0*P.y.
	const double iorEntry = 1.2;
	const double iorExit  = 1.8;
	const double L = 1.0 / 3.14159265358979323846;
	const double T1 = 1.0 - R0( iorEntry );
	const double T2 = 1.0 - R0( iorExit );

	const double expectedBuggy   = L * T1 * T2 * 1.0;
	const double expectedCorrect = L * T1 * T2 * ( iorExit / iorEntry ) * ( iorExit / iorEntry );

	std::cout << "Graded slab: ior_graded = 1.8 - 2.0*P.y  (entry " << iorEntry
	          << " -> exit " << iorExit << ")" << std::endl;
	std::cout << "    closed form BUGGY   (etaScale=1)    = " << expectedBuggy << std::endl;
	std::cout << "    closed form CORRECT (etaScale=2.25) = " << expectedCorrect << std::endl;

	const std::string scene = SceneGradedSlab( "ior_graded", true, "1.8-2.0*P.y" );
	const std::string head( "RISE ASCII SCENE 7\n" );

	struct Row { const char* name; std::string sceneText; };
	const Row rows[] = {
		{ "PT",   head + RasterizerPT( "64" )   + scene },
		{ "BDPT", head + RasterizerBDPT( "64" ) + scene },
	};

	for( const Row& r : rows ) {
		const ImageStats s = RenderAndComputeStats( r.sceneText, "graded" );
		const std::string label = std::string( "graded slab " ) + r.name;
		Check( s.valid, label + ": render produced output" );
		if( !s.valid ) continue;
		const double m = GreyMean( s );
		std::cout << "    " << r.name << "  mean=" << m
		          << "  ratio-to-buggy=" << ( m / expectedBuggy )
		          << "  ratio-to-correct=" << ( m / expectedCorrect ) << std::endl;
		// The asserted expectation: mean matches the CORRECT closed form
		// (post-fix behaviour).  On the unfixed library this fails --
		// see the fix commit message for the captured red-proof numbers,
		// where mean instead matches expectedBuggy.
		Check( std::fabs( m - expectedCorrect ) <= 0.05 * expectedCorrect,
			label + ": mean == L*T1*T2*(iorExit/iorEntry)^2 within 5%" );
		// Companion assertion, framed the other way: the buggy
		// (etaScale==1) prediction must NOT match once the fix is in --
		// the two closed forms are 2.25x apart, so "within 5%" of one
		// excludes the other by construction; this check exists to make
		// that exclusion explicit in the failure output rather than
		// relying on a reader to notice.
		Check( std::fabs( m - expectedBuggy ) > 0.30 * expectedBuggy,
			label + ": mean is NOT the buggy etaScale==1 prediction" );
	}
}

//////////////////////////////////////////////////////////////////////
// Control row: uniform ior 1.5 (entry == exit).  etaScale telescopes
// to 1 exactly regardless of the fix -- this row must read the SAME
// closed form (T1*T2*L, etaScale omitted since it is exactly 1) both
// before and after, pinning "keep the uniform-ior behaviour
// byte-identical" independently of RefractiveRadianceScalingTest.cpp.
//////////////////////////////////////////////////////////////////////
static void RunUniformControlRow()
{
	const double ior = 1.5;
	const double L = 1.0 / 3.14159265358979323846;
	const double T = 1.0 - R0( ior );
	const double expected = L * T * T;		// etaScale == 1, entry == exit

	std::cout << "Uniform control: ior = 1.5 (entry == exit, etaScale == 1 always)" << std::endl;
	std::cout << "    closed form = " << expected << std::endl;

	const std::string scene = SceneGradedSlab( "1.5", false, "" );
	const std::string head( "RISE ASCII SCENE 7\n" );

	struct Row { const char* name; std::string sceneText; };
	const Row rows[] = {
		{ "PT",   head + RasterizerPT( "64" )   + scene },
		{ "BDPT", head + RasterizerBDPT( "64" ) + scene },
	};

	for( const Row& r : rows ) {
		const ImageStats s = RenderAndComputeStats( r.sceneText, "uniform" );
		const std::string label = std::string( "uniform control " ) + r.name;
		Check( s.valid, label + ": render produced output" );
		if( !s.valid ) continue;
		const double m = GreyMean( s );
		std::cout << "    " << r.name << "  mean=" << m
		          << "  ratio=" << ( m / expected ) << std::endl;
		Check( std::fabs( m - expected ) <= 0.05 * expected,
			label + ": mean == L*T^2 within 5%, unaffected by the fix" );
	}
}

int main( int argc, char** argv )
{
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "=== RadianceEtaScaleGradedIndexTest ===" << std::endl;
	std::cout << "seed base = " << g_seedBase
	          << "  (pass a different one as argv[1] for an independent sample)"
	          << std::endl;

	RunGradedRow();
	RunUniformControlRow();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
