//////////////////////////////////////////////////////////////////////
//
//  RadianceEtaScaleGradedIndexTest.cpp - closed-form guard on the
//    through-slab eta^2 invariant for a SPATIALLY-VARYING `ior`
//    (docs/DEBT_LEDGER.md DL-09; docs/REFRACTIVE_RADIANCE_SCALING.md
//    §10.2).
//
//    WHAT THIS FILE PINS.  An emitter seen through a passive, lossless
//    graded-index slab must read `L * T1 * T2` -- the two interfaces'
//    Fresnel transmittances and NOTHING else.  No net eta^2 factor
//    survives the round trip, no matter how strongly the slab's `ior`
//    varies between the entry face and the exit face.
//
//    WHY, IN FULL.  The invariant along a ray is the BASIC radiance
//    `L / n^2`, and it is conserved in TWO places, not one:
//      (a) ACROSS an interface (the textbook statement,
//          docs/REFRACTIVE_RADIANCE_SCALING.md §1), and
//      (b) ALONG the ray INSIDE a medium whose index varies with
//          position -- the part that is easy to forget, and the part
//          this file exists to keep someone from forgetting again.
//    Following the physical light from the emitter (air, radiance `L`)
//    up through a slab of index `n_B` at the bottom face and `n_A` at
//    the top face:
//      bottom interface  : L_in        = T2 * n_B^2 * L
//      interior segment  : L_at_top    = L_in * (n_A/n_B)^2 = T2*n_A^2*L
//      top interface     : L_out       = T1 * L_at_top / n_A^2
//                                      = L * T1 * T2.
//    The `n_A^2` and `n_B^2` cancel completely.  Physically obvious in
//    hindsight: a slab that neither emits nor absorbs cannot amplify.
//
//    WHAT RISE DOES, AND WHY IT IS RIGHT TODAY.  RISE applies the
//    walk-order factor `(eta_before/eta_after)^2` only at SCATTER
//    events (`RISE::RadianceEtaScale`, Utilities/IORStack.h) and has NO
//    interior-segment factor at all.  A camera-rooted eye walk through
//    this slab therefore multiplies:
//      entry hit (top face, air -> slab):  (1 / n_A)^2
//      exit  hit (bottom face, slab -> air): (n_A / 1)^2
//    -- because `RadianceEtaScale`'s `before.top()` reads the value
//    PUSHED at the ENTRY hit (`n_A`), not the exit hit's freshly
//    re-fetched `n_B`.  The product is exactly 1, which is exactly
//    right: that stale `n_A` is silently standing in for the product of
//    the missing interior-segment factor `(n_A/n_B)^2` and the true
//    exit factor `(n_B/1)^2`.  The mismatch and the omission cancel.
//
//    THE REGRESSION THIS GUARDS AGAINST (tried and reverted
//    2026-09-14).  Substituting the SPF's fresh exit-hit `n_B` for the
//    stack's `n_A` at the exit read -- on the reasonable-sounding
//    grounds that `n_B` is what the SPF's own Snell/Fresnel math used
//    -- WITHOUT also adding the interior-segment factor turns the net
//    1 into `(n_B/n_A)^2`.  At this file's geometry that is 2.25: the
//    emitter behind a passive lossless slab would read 2.25x BRIGHTER
//    than the emitter.  The `overbright` closed form below is that
//    wrong answer, asserted to be far from what we read.
//
//    WHAT IS STILL OPEN (DL-09, re-opened 2026-09-14).  A contribution
//    GATHERED AT AN INTERIOR VERTEX C -- an NEE connection or a bounce
//    while the walk is still inside the graded object, before it exits
//    -- never reaches the cancelling exit event.  Its throughput
//    carries `(1/n_A)^2` from the entry crossing where physics wants
//    `(1/n_C)^2`, an error of `(n_C/n_A)^2`.  The same gap applies to a
//    camera seeded INSIDE a graded medium (`IORStackSeeding::
//    SeedFromPoint` records the index at the camera position; the first
//    segment to a hit at a different index pays no factor).  The
//    principled fix is the missing interior-segment factor
//    `(n_prev/n_C)^2` applied along the walk, NOT a substitution at the
//    exit read.  This file does not exercise that case: both rows here
//    are pure through-transmission with a diffuse-free slab, so every
//    contribution completes the round trip.
//
//    THE SCENE.  A single `box_geometry` dielectric slab, `ior` bound
//    to a `scalar_painter { expression 1.8-2.0*P.y }`.  Box height 0.3,
//    centred so the top face sits at world Y = 0.3 (ior 1.2, the ENTRY
//    hit for a camera looking straight down) and the bottom face at
//    Y = 0.0 (ior 1.8, the EXIT hit).  A Lambertian luminaire quad
//    (exitance 1, L = 1/pi) sits just below the box; a pinhole camera
//    looks straight down through the slab from above at a narrow FOV
//    (5 degrees) so every ray is within ~1.7 degrees of normal
//    incidence -- Snell bending and the angular Fresnel departure from
//    the closed form are negligible (the same convention
//    tests/RefractiveRadianceScalingTest.cpp Row A uses at 15 degrees,
//    tightened further here).  A second-order internal-bounce
//    correction (the ray reflecting once inside the slab before
//    re-exiting) is bounded by R0(1.2)*R0(1.8) = 0.00067 (0.067%),
//    three orders of magnitude below the 5% assertion band.
//
//    CONTROL ROW.  The same scene with `ior` a plain uniform 1.5 (no
//    spatial variation, entry == exit == 1.5): `before.top()` and the
//    fresh value agree everywhere, so this row is insensitive to the
//    whole question and reads `L * T^2`.  It is here so a failure on
//    the graded row can be attributed to the graded-ness rather than to
//    the scene, the rasterizer, or the harness.
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

	// The physics (see this file's header): a passive lossless slab
	// contributes its two Fresnel transmittances and no net eta^2
	// factor, however strongly its `ior` varies inside.
	const double expectedPhysics = L * T1 * T2;
	// The reverted-regression answer: reading the exit hit's FRESH ior
	// as `eta_before` without adding the missing interior-segment
	// factor.  2.25x brighter than the emitter behind a slab that
	// neither emits nor absorbs.
	const double overbright = expectedPhysics * ( iorExit / iorEntry ) * ( iorExit / iorEntry );

	std::cout << "Graded slab: ior_graded = 1.8 - 2.0*P.y  (entry " << iorEntry
	          << " -> exit " << iorExit << ")" << std::endl;
	std::cout << "    closed form PHYSICS     (L*T1*T2, net etaScale 1) = " << expectedPhysics << std::endl;
	std::cout << "    reverted-regression value ((iorExit/iorEntry)^2)  = " << overbright << std::endl;

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
		          << "  ratio-to-physics=" << ( m / expectedPhysics )
		          << "  ratio-to-overbright=" << ( m / overbright ) << std::endl;
		// The asserted expectation: the through-slab trip carries the
		// two Fresnel transmittances and NO net eta^2 factor.
		Check( std::fabs( m - expectedPhysics ) <= 0.05 * expectedPhysics,
			label + ": mean == L*T1*T2 within 5% (no net eta^2 through a passive slab)" );
		// Companion assertion, framed the other way: the reverted
		// regression's value must NOT match.  The two closed forms are
		// 2.25x apart, so "within 5%" of one excludes the other by
		// construction; this check exists to make that exclusion
		// explicit in the failure output -- a failure on THIS line, with
		// the row above also failing high, is the specific signature of
		// re-introducing a fresh-exit-ior substitution at
		// `RadianceEtaScale`'s `eta_before` without an interior-segment
		// factor to pay for it.
		Check( std::fabs( m - overbright ) > 0.30 * overbright,
			label + ": mean is NOT the (iorExit/iorEntry)^2 over-bright value" );
	}
}

//////////////////////////////////////////////////////////////////////
// Control row: uniform ior 1.5 (entry == exit).  `before.top()` and
// the SPF's freshly re-fetched value agree everywhere here, so this row
// is insensitive to the graded-index question entirely and reads
// `L * T^2`.  It attributes a graded-row failure to the graded-ness
// rather than to the scene, the rasterizer or the harness, and does so
// independently of RefractiveRadianceScalingTest.cpp (which this file
// does not re-run; see this row's gate list instead).
//////////////////////////////////////////////////////////////////////
static void RunUniformControlRow()
{
	const double ior = 1.5;
	const double L = 1.0 / 3.14159265358979323846;
	const double T = 1.0 - R0( ior );
	const double expected = L * T * T;		// net etaScale == 1, entry == exit

	std::cout << "Uniform control: ior = 1.5 (entry == exit, net etaScale == 1 always)" << std::endl;
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
			label + ": mean == L*T^2 within 5% (uniform ior, no graded-index question)" );
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
