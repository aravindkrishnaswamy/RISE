//////////////////////////////////////////////////////////////////////
//
//  SobolSelectionChannelBiasTest.cpp - render-level regression guard
//    for DL-81 (docs/DEBT_LEDGER.md).
//
//  The physical symptom
//  --------------------
//    An RGB dispersive dielectric splits each hit into three
//    per-channel refractions (plus their three Fresnel reflections),
//    and the path tracer picks ONE of those scattered rays
//    stochastically with a 1/selectProb correction.  Two channels
//    given the IDENTICAL index of refraction are then the same physics
//    reached through two DIFFERENT slots of the scattered-ray
//    container, so the only thing that can make them disagree is a
//    defect in the selection draws.
//
//    They disagreed by 12.1%.  `SobolSequence::Sample` reduced the
//    requested dimension to `dimension & 1`, and
//    `SobolSampler::kStreamStride` is 32 (EVEN), so the selection draw
//    at the entry hit and the selection draw at the exit hit one bounce
//    later were two Owen-scrambled copies of ONE base value -- their
//    leading digits related by a fixed bit.  See
//    SobolDimensionParityTest for the sampler-level statement and
//    docs/DL81_SOBOL_DIMENSION_PARITY.md for the mechanism.
//
//  The fixture
//  -----------
//    A lossless (`tau 1.0`, `scattering 1000000`) 2-deep dielectric box
//    with a white Lambertian luminaire (exitance 1) directly behind it
//    and a pinhole camera on axis at fov 10.  At normal incidence every
//    pixel reads the closed form
//        L * T^2 / (1 - R^2),   L = 1/pi,  R = R0(n) = ((n-1)/(n+1))^2,
//                               T = 1 - R
//    per channel: T^2 for the straight-through path, times the geometric
//    series over the paths that internally reflect off the front face,
//    off the back face, and exit -- at normal incidence every one of
//    those re-emerges along the same ray and lands in the same pixel.
//    (Dropping the series costs 0.81% on the shared index; the pre-DL-81
//    sampler's non-dispersive control read 0.26% ABOVE the series-free
//    form and so looked "right", but it was in fact 0.54% BELOW the true
//    value -- the correlated selection draws were losing the
//    multiply-reflected paths.)
//
//    The index triple gives channels 0 and 1 the SAME index and channel
//    2 a different one, so:
//      * R and G must agree (the sharp statement -- it needs no closed
//        form and no absolute calibration at all), and
//      * all three must sit on the closed form to within the band the
//        NON-DISPERSIVE control establishes for this harness.
//
//  Rows
//  ----
//    A  dielectric_material, dispersive       (RED pre-fix)
//    B  dielectric_material, non-dispersive control at the shared index
//    C  perfectrefractor_material, dispersive (the DL-81 row's first
//       named sibling: PerfectRefractorSPF.cpp's own per-channel loop)
//    D  polished_material, dispersive mirror coat at normal incidence
//       (the second named sibling, PolishedSPF.cpp): closed form
//       L * R0(n_c), reflectance black, delta coat.  Honestly a
//       consistency pin and not a red-proof: a mirror coat at normal
//       incidence makes ONE stochastic lobe selection, and the defect
//       needs two selections at successive bounces to express, so this
//       row was green before the fix too (R/G -0.065%).
//
//  Sample count
//  ------------
//    1024 spp.  The residual is QMC truncation, not noise -- the
//    sampler is deterministic, so the seed base does not move any
//    number here at all; only the sample count does.  Measured row A
//    R/G departure: 1.15% at 256 spp, 0.29% at 1024, 0.22% at 4096.
//    Pre-fix the same quantity was 11.9% and did NOT converge, which is
//    the difference between a bias and a truncation error.
//
//  Author: Claude (debt-sobol slice, DL-81)
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

static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;

//! Samples per pixel for every row.  Overridable from argv[2] so the
//! rows can be re-measured at other sample counts: the sampler is
//! deterministic QMC, so the seed base does NOT move these numbers and
//! sample count is the only convergence knob.
static const char* g_samples = "1024";

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

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;

	CapturingRasterizerOutput() {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		const unsigned int width  = pImage.GetWidth();
		const unsigned int height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

struct Means
{
	double rgb[3];
	bool   valid;
};

static std::string WriteToTempFile( const std::string& text, const char* suffix )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/sobol_chanbias_%s_%d", suffix,
		static_cast<int>( ::getpid() ) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << text;
	ofs.close();
	return std::string( path );
}

static Means RenderMeans( const std::string& sceneText, const char* tag )
{
	Means m{};

	const std::string scenePath = WriteToTempFile( sceneText, tag );
	if( scenePath.empty() ) return m;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( scenePath.c_str() );
		return m;
	}
	if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
		safe_release( pJob );
		std::remove( scenePath.c_str() );
		return m;
	}

	pJob->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( g_seedBase + g_renderIndex++ );
	if( pJob->Rasterize() && !pCap->pixels.empty() ) {
		for( const RISEColor& c : pCap->pixels ) {
			const double cov = c.a;
			m.rgb[0] += c.base.r * cov;
			m.rgb[1] += c.base.g * cov;
			m.rgb[2] += c.base.b * cov;
		}
		const double n = double( pCap->pixels.size() );
		for( int i = 0; i < 3; ++i ) m.rgb[i] /= n;
		m.valid = true;
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( scenePath.c_str() );
	return m;
}

//! Normal-incidence Fresnel reflectance between air and `n`.
static double R0( double n )
{
	const double r = ( n - 1.0 ) / ( n + 1.0 );
	return r * r;
}

//! Total normal-incidence transmittance of a lossless slab with two
//! air interfaces of reflectance R0(n): T^2 summed over every even
//! number of internal reflections, T^2 * sum_k (R^2)^k.
static double SlabTransmittance( double n )
{
	const double r = R0( n );
	const double t = 1.0 - r;
	return t * t / ( 1.0 - r * r );
}

static const double kL = 1.0 / 3.14159265358979323846;	// Lambertian luminaire, exitance 1

// The two indices.  `kNShared` is on channels 0 AND 1; `kNOdd` on
// channel 2.  Values are the DL-81 row's, which are in turn the DL-29
// curve sampled at ScalarPainterRGB::kChannelNM {611,549,465} nm --
// kept so the two rows' numbers stay comparable.
static const double kNShared = 1.8532352941;
static const double kNOdd    = 2.125;

//! Transmissive skeleton: pinhole camera on +Z, white Lambertian quad
//! at z = -2, a 2-deep box of `matChunk` (named `mat_slab`) at origin.
static std::string TransmitScene( const std::string& matChunk, const char* samples )
{
	return std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -1 1 -2\n\tptb 1 1 -2\n\tptc 1 -1 -2\n\tptd -1 -1 -2\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n"
		+ matChunk +
		"box_geometry\n{\n\tname geo_slab\n\twidth 4\n\theight 4\n\tdepth 2\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n"
		"\tmaterial mat_slab\n\tposition 0 0 0\n}\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " + samples + "\n"
		"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

//! Reflective skeleton for row D: the camera sits BETWEEN the mirror
//! box and the luminaire, so a camera ray reflects off the box's front
//! face at normal incidence and travels back past the camera into the
//! quad at z = +4.5.
static std::string ReflectScene( const std::string& matChunk, const char* samples )
{
	return std::string( "RISE ASCII SCENE 7\n" ) +
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n"
		"\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0.0 0.0 0.0\n}\n\n"
		"lambertian_luminaire_material\n{\n"
		"\tname mat_emit\n\texitance pnt_emit\n\tscale 1.0\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname quad\n"
		"\tpta -3 3 4.5\n\tptb 3 3 4.5\n\tptc 3 -3 4.5\n\tptd -3 -3 4.5\n}\n\n"
		"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_emit\n}\n\n"
		+ matChunk +
		"box_geometry\n{\n\tname geo_slab\n\twidth 4\n\theight 4\n\tdepth 2\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n"
		"\tmaterial mat_slab\n\tposition 0 0 0\n}\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " + samples + "\n"
		"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static void Report( const char* label, const Means& m, const double expect[3] )
{
	std::cout << "  " << label << std::endl;
	std::cout << "      closed form  R=" << expect[0] << " G=" << expect[1]
	          << " B=" << expect[2] << std::endl;
	std::cout << "      render       R=" << m.rgb[0] << " G=" << m.rgb[1]
	          << " B=" << m.rgb[2] << std::endl;
	std::cout << "      dev vs form  R=" << ( m.rgb[0] / expect[0] - 1.0 ) * 100.0 << "%"
	          << "  G=" << ( m.rgb[1] / expect[1] - 1.0 ) * 100.0 << "%"
	          << "  B=" << ( m.rgb[2] / expect[2] - 1.0 ) * 100.0 << "%" << std::endl;
	std::cout << "      R/G - 1      " << ( m.rgb[0] / m.rgb[1] - 1.0 ) * 100.0 << "%"
	          << "   (channels 0 and 1 carry the IDENTICAL index)" << std::endl;
}

//! The two assertions every dispersive row makes.
static void CheckDispersiveRow(
	const char* label,
	const Means& m,
	const double expect[3],
	double sharedTol,
	double absTol )
{
	char msg[256];

	std::snprintf( msg, sizeof(msg),
		"%s: R == G within %.1f%% (identical ior on channels 0 and 1)",
		label, sharedTol * 100.0 );
	Check( m.rgb[1] > 1e-12 &&
		std::fabs( m.rgb[0] / m.rgb[1] - 1.0 ) <= sharedTol, msg );

	static const char* chan[3] = { "R", "G", "B" };
	for( int c = 0; c < 3; ++c ) {
		std::snprintf( msg, sizeof(msg), "%s: %s on the closed form within %.1f%%",
			label, chan[c], absTol * 100.0 );
		Check( std::fabs( m.rgb[c] - expect[c] ) <= absTol * expect[c], msg );
	}
}

static std::string InlineTriple( double a, double b, double c )
{
	char buf[160];
	std::snprintf( buf, sizeof(buf), "%.17g %.17g %.17g", a, b, c );
	return std::string( buf );
}

int main( int argc, char** argv )
{
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	static char samplesBuf[32];
	if( argc > 2 && argv[2] ) {
		const long v = std::strtol( argv[2], nullptr, 10 );
		if( v > 0 ) {
			std::snprintf( samplesBuf, sizeof(samplesBuf), "%ld", v );
			g_samples = samplesBuf;
		}
	}

	std::cout << "=== SobolSelectionChannelBiasTest ===" << std::endl;
	std::cout << "samples per pixel = " << g_samples << std::endl;
	std::cout << "seed base = " << g_seedBase << std::endl;
	std::cout << "ior triple = " << kNShared << " " << kNShared << " " << kNOdd << std::endl;

	// Closed form for the two transmissive rows.
	double transmit[3];
	transmit[0] = kL * SlabTransmittance( kNShared );
	transmit[1] = transmit[0];
	transmit[2] = kL * SlabTransmittance( kNOdd );

	const std::string ior3 = InlineTriple( kNShared, kNShared, kNOdd );

	// ----------------------------------------------------------------
	// ROW A -- dielectric_material, dispersive.
	// ----------------------------------------------------------------
	{
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior " + ior3 + "\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( mat, g_samples ), "dielA" );
		Check( m.valid, "row A: render produced output" );
		if( m.valid ) {
			Report( "row A  dielectric_material, dispersive", m, transmit );
			CheckDispersiveRow( "row A", m, transmit, 0.015, 0.015 );
			const double sum    = m.rgb[0] + m.rgb[1] + m.rgb[2];
			const double sumRef = transmit[0] + transmit[1] + transmit[2];
			std::cout << "      sum=" << sum << " vs " << sumRef
			          << "  (" << ( sum / sumRef - 1.0 ) * 100.0 << "%)" << std::endl;
			Check( std::fabs( sum - sumRef ) <= 0.01 * sumRef,
				"row A: R+G+B on the closed-form total within 1% (no dropped scattered ray)" );
		}
	}

	// ----------------------------------------------------------------
	// ROW B -- the non-dispersive control at the shared index.  This is
	// the calibration for rows A/C: it establishes what this harness
	// can do when NO per-channel selection is involved at all.
	// ----------------------------------------------------------------
	{
		char uniform[64];
		std::snprintf( uniform, sizeof(uniform), "%.17g", kNShared );
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior " + std::string( uniform ) + "\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( mat, g_samples ), "dielB" );
		Check( m.valid, "row B: render produced output" );
		if( m.valid ) {
			std::cout << "  row B  dielectric_material, NON-dispersive control" << std::endl;
			std::cout << "      closed form  " << transmit[0] << std::endl;
			std::cout << "      render       R=" << m.rgb[0] << " G=" << m.rgb[1]
			          << " B=" << m.rgb[2] << std::endl;
			std::cout << "      dev vs form  " << ( m.rgb[0] / transmit[0] - 1.0 ) * 100.0
			          << "%" << std::endl;
			// Achromatic to within the film's float32 storage, not to the
			// last double bit: the three channels travel the same lobe but
			// are accumulated and read back through a float raster image.
			Check( std::fabs( m.rgb[0] - m.rgb[1] ) <= 1e-4 * m.rgb[0] &&
			       std::fabs( m.rgb[1] - m.rgb[2] ) <= 1e-4 * m.rgb[0],
				"row B: control is achromatic to 1e-4 (one achromatic lobe)" );
			Check( std::fabs( m.rgb[0] - transmit[0] ) <= 0.005 * transmit[0],
				"row B: control on the closed form within 0.5% (internal reflections included)" );
		}
	}

	// ----------------------------------------------------------------
	// ROW C -- perfectrefractor_material, dispersive.
	// ----------------------------------------------------------------
	{
		const std::string mat =
			"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n"
			"perfectrefractor_material\n{\n\tname mat_slab\n"
			"\trefractance pnt_white\n\tior " + ior3 + "\n}\n\n";
		const Means m = RenderMeans( TransmitScene( mat, g_samples ), "refrC" );
		Check( m.valid, "row C: render produced output" );
		if( m.valid ) {
			Report( "row C  perfectrefractor_material, dispersive", m, transmit );
			CheckDispersiveRow( "row C", m, transmit, 0.015, 0.015 );
		}
	}

	// ----------------------------------------------------------------
	// ROW D -- polished_material, dispersive delta mirror coat over a
	// black substrate, seen at normal incidence: L * R0(n_c).
	// ----------------------------------------------------------------
	{
		double reflect[3];
		reflect[0] = kL * R0( kNShared );
		reflect[1] = reflect[0];
		reflect[2] = kL * R0( kNOdd );

		const std::string mat =
			"polished_material\n{\n\tname mat_slab\n\treflectance pnt_black\n"
			"\ttau 1.0\n\tior " + ior3 + "\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( ReflectScene( mat, g_samples ), "polD" );
		Check( m.valid, "row D: render produced output" );
		if( m.valid ) {
			Report( "row D  polished_material, dispersive mirror coat", m, reflect );
			CheckDispersiveRow( "row D", m, reflect, 0.005, 0.005 );
		}
	}

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
