//////////////////////////////////////////////////////////////////////
//
//  CurvePainterRGBDispersionTest.cpp - render-level regression guard
//    for DL-82 (docs/DEBT_LEDGER.md).
//
//  The physical symptom
//  --------------------
//    `SellmeierScalarPainter`, `PolynomialScalarPainter`, and
//    `Function1DScalarPainter` each broadcast ONE representative-
//    wavelength sample into all three RGB channels from `GetValuesAt`
//    and reported `HasPerChannelVariation() == false` unconditionally
//    -- the exact pattern DL-29 removed from
//    `PiecewiseLinearScalarPainter`.  A Sellmeier/polynomial/Function1D
//    dispersion FORMULA bound to `ior` therefore rendered perfectly
//    achromatic and non-dispersive under every RGB rasterizer:
//    `DielectricSPF`/`PolishedSPF`/`PerfectRefractorSPF` gate their
//    three-channel dispersion loop on `HasPerChannelVariation()`, and a
//    painter that always reports `false` never engages it, regardless
//    of how strongly its underlying curve actually varies with
//    wavelength.
//
//  The fixture
//  -----------
//    Same lossless (`tau 1.0`, `scattering 1000000`) 2-deep dielectric
//    box, white Lambertian luminaire (exitance 1) directly behind, and
//    pinhole camera on axis at fov 10 that `SobolSelectionChannelBiasTest`
//    (DL-81) uses -- self-contained here rather than shared, since this
//    row's `ior` slot is bound to a `scalar_painter` curve chunk instead
//    of an inline `r g b` triple.  At normal incidence every pixel reads
//    the closed form `L*T^2/(1-R^2)`, `L = 1/pi`, `R = R0(n) =
//    ((n-1)/(n+1))^2`, `T = 1-R` -- `T^2` for the straight-through path
//    times the geometric series over paths that internally reflect off
//    both faces and exit (they re-emerge along the same ray at normal
//    incidence).
//
//  Rows
//  ----
//    A  `sellmeier` BK7 (the exact in-tree coefficients used by
//       `scenes/Tests/Spectral/phase6_scalar_painter_forms.RISEscene`
//       and `phase3_dielectric_iscalarpainter.RISEscene`) bound to
//       `ior`.  BK7's visible-band dispersion is physically mild (index
//       spread ~0.005 across 465-611nm, closed-form transmittance
//       spread ~0.2% (review-recomputed from the test's own n-values: T(611/549/465)=0.91930/0.91866/0.91736, spread 0.212%)) -- too small to resolve cleanly against this
//       fixture's QMC residual (DL-81: ~0.1-0.3% at 1024spp), so this
//       row's hard gate is the qualitative one ("no longer exactly
//       achromatic") plus an informational closed-form comparison, not
//       a tight quantitative band.
//    B  `polynomial` with a real slope (`2.0 -0.001`, i.e.
//       n(nm) = 2.0 - 0.001*nm -- unlike the in-tree phase6 example
//       `1.5 0 0`, which is a CONSTANT and stays correctly non-dispersive,
//       see row D) giving index 1.389/1.451/1.535 at 611/549/465nm, a
//       ~3.6% closed-form transmittance spread -- large enough to gate
//       quantitatively.
//    C  `function1d` wrapping a `piecewise_linear_function` whose control
//       points sit EXACTLY at `ScalarPainterRGB::kChannelNM`
//       (465->1.3, 549->1.5, 611->1.7), so the per-channel closed form is
//       exact, not interpolated -- a ~10.6% spread, the tightest
//       quantitative gate of the three.
//    D  `polynomial` CONSTANT (`1.5 0 0`, the in-tree phase6 example) --
//       a control: a genuinely flat curve must stay non-dispersive
//       (achromatic) both before AND after the fix, since its three
//       `kChannelNM` samples are bit-identical and `IsUniform()` is an
//       exact comparison.  This row is unaffected by DL-82 and should
//       read the same both pre- and post-fix.
//
//  Sample count
//  ------------
//    1024spp, matching `SobolSelectionChannelBiasTest`'s DL-81 fixture.
//    Row A's informational closed-form deviation is expected to be
//    swamped by QMC residual at this spp; rows B/C's spread is 100-500x
//    that residual and gates cleanly.
//
//  Author: Claude (debt-dl82 slice, DL-82)
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

static const unsigned int kDefaultSeedBase = 2000u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;
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
	std::snprintf( path, sizeof(path), "/tmp/curvepainter_disp_%s_%d", suffix,
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
//! air interfaces of reflectance R0(n) (see SobolSelectionChannelBiasTest
//! for the derivation).
static double SlabTransmittance( double n )
{
	const double r = R0( n );
	const double t = 1.0 - r;
	return t * t / ( 1.0 - r * r );
}

static const double kL = 1.0 / 3.14159265358979323846;	// Lambertian luminaire, exitance 1

//! Transmissive skeleton: pinhole camera on +Z, white Lambertian quad
//! at z = -2, a 2-deep box of `matChunk` (named `mat_slab`), preceded
//! by `painterChunk` (the `scalar_painter`/`piecewise_linear_function`
//! definitions the material references).
static std::string TransmitScene(
	const std::string& painterChunk,
	const std::string& matChunk,
	const char* samples )
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
		+ painterChunk
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

	std::cout << "=== CurvePainterRGBDispersionTest ===" << std::endl;
	std::cout << "samples per pixel = " << g_samples << std::endl;
	std::cout << "seed base = " << g_seedBase << std::endl;

	// ----------------------------------------------------------------
	// ROW A -- sellmeier (BK7), the in-tree coefficients.
	// ----------------------------------------------------------------
	{
		const std::string painter =
			"scalar_painter\n{\n\tname ior_bk7\n"
			"\tsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n"
			"}\n\n";
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior ior_bk7\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( painter, mat, g_samples ), "sellmeier" );
		Check( m.valid, "row A: render produced output" );
		if( m.valid ) {
			// BK7 Sellmeier evaluated at ScalarPainterRGB::kChannelNM
			// {611, 549, 465}: n = 1.5158711781837417 / 1.518572836436827
			// / 1.5240123784970663.
			const double expect[3] = {
				kL * SlabTransmittance( 1.5158711781837417 ),
				kL * SlabTransmittance( 1.518572836436827 ),
				kL * SlabTransmittance( 1.5240123784970663 )
			};
			Report( "row A  sellmeier (BK7), dispersive", m, expect );
			// Qualitative gate: BK7's mild visible-band dispersion is
			// too small (~0.2% closed-form spread) to reliably clear
			// this fixture's QMC residual (~0.1-0.3% at 1024spp per
			// DL-81) with a tight quantitative band, but the DEFECT this
			// row exists to catch is binary -- pre-fix, `HasPerChannelVariation()`
			// is unconditionally false so DielectricSPF's single-ray
			// non-dispersive path fires and R/G/B are the SAME ray,
			// achromatic to float32 storage (<1e-4 relative).  Post-fix,
			// the per-channel dispersion loop fires three INDEPENDENT
			// QMC streams even for a mild curve, so R/G/B are never
			// float32-achromatic again.
			const double spreadRG = std::fabs( m.rgb[0] - m.rgb[1] ) / m.rgb[1];
			const double spreadGB = std::fabs( m.rgb[1] - m.rgb[2] ) / m.rgb[1];
			std::cout << "      spread       R/G-1=" << spreadRG * 100.0 << "%"
			          << "  G/B-1=" << spreadGB * 100.0 << "%" << std::endl;
			Check( spreadRG > 1e-4 || spreadGB > 1e-4,
				"row A: no longer float32-achromatic (dispersion loop engaged)" );
		}
	}

	// ----------------------------------------------------------------
	// ROW B -- polynomial with a real slope: n(nm) = 2.0 - 0.001*nm.
	// ----------------------------------------------------------------
	{
		const std::string painter =
			"scalar_painter\n{\n\tname ior_poly\n\tpolynomial 2.0 -0.001\n}\n\n";
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior ior_poly\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( painter, mat, g_samples ), "polynomial" );
		Check( m.valid, "row B: render produced output" );
		if( m.valid ) {
			// n(611)=1.389, n(549)=1.451, n(465)=1.535.
			const double expect[3] = {
				kL * SlabTransmittance( 1.389 ),
				kL * SlabTransmittance( 1.451 ),
				kL * SlabTransmittance( 1.535 )
			};
			Report( "row B  polynomial (2.0 - 0.001*nm), dispersive", m, expect );
			Check( m.rgb[0] > m.rgb[1] && m.rgb[1] > m.rgb[2],
				"row B: R > G > B (pre-fix: exactly achromatic)" );
			for( int c = 0; c < 3; ++c ) {
				char msg[128];
				std::snprintf( msg, sizeof(msg),
					"row B: channel %d on the closed form within 8%%", c );
				Check( std::fabs( m.rgb[c] - expect[c] ) <= 0.08 * expect[c], msg );
			}
		}
	}

	// ----------------------------------------------------------------
	// ROW C -- function1d wrapping a piecewise_linear_function whose
	// control points sit exactly at kChannelNM.
	// ----------------------------------------------------------------
	{
		const std::string painter =
			"piecewise_linear_function\n{\n\tname f_ior\n"
			"\tcp 400 1.3\n\tcp 465 1.3\n\tcp 549 1.5\n\tcp 611 1.7\n\tcp 700 1.7\n}\n\n"
			"scalar_painter\n{\n\tname ior_f1d\n\tfunction1d f_ior\n}\n\n";
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior ior_f1d\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( painter, mat, g_samples ), "function1d" );
		Check( m.valid, "row C: render produced output" );
		if( m.valid ) {
			const double expect[3] = {
				kL * SlabTransmittance( 1.7 ),
				kL * SlabTransmittance( 1.5 ),
				kL * SlabTransmittance( 1.3 )
			};
			Report( "row C  function1d (piecewise_linear_function), dispersive", m, expect );
			// Control points give n(611)=1.7 > n(549)=1.5 > n(465)=1.3, so
			// R0 is HIGHEST (transmittance LOWEST) at 611nm -- the closed
			// form order is R < G < B (opposite of row B's polynomial,
			// which slopes the other way).
			Check( m.rgb[0] < m.rgb[1] && m.rgb[1] < m.rgb[2],
				"row C: R < G < B (pre-fix: exactly achromatic)" );
			for( int c = 0; c < 3; ++c ) {
				char msg[128];
				std::snprintf( msg, sizeof(msg),
					"row C: channel %d on the closed form within 5%%", c );
				Check( std::fabs( m.rgb[c] - expect[c] ) <= 0.05 * expect[c], msg );
			}
		}
	}

	// ----------------------------------------------------------------
	// ROW D -- polynomial CONSTANT control (the in-tree phase6 example,
	// `1.5 0 0`).  A genuinely flat curve's three kChannelNM samples are
	// bit-identical, so this row must stay non-dispersive (achromatic)
	// both BEFORE and AFTER the fix -- unaffected by DL-82.
	// ----------------------------------------------------------------
	{
		const std::string painter =
			"scalar_painter\n{\n\tname ior_flat\n\tpolynomial 1.5 0 0\n}\n\n";
		const std::string mat =
			"dielectric_material\n{\n\tname mat_slab\n\tior ior_flat\n"
			"\ttau 1.0\n\tscattering 1000000\n}\n\n";
		const Means m = RenderMeans( TransmitScene( painter, mat, g_samples ), "constant" );
		Check( m.valid, "row D: render produced output" );
		if( m.valid ) {
			const double expect = kL * SlabTransmittance( 1.5 );
			std::cout << "  row D  polynomial CONSTANT (1.5 0 0), control" << std::endl;
			std::cout << "      closed form  " << expect << std::endl;
			std::cout << "      render       R=" << m.rgb[0] << " G=" << m.rgb[1]
			          << " B=" << m.rgb[2] << std::endl;
			Check( std::fabs( m.rgb[0] - m.rgb[1] ) <= 1e-4 * m.rgb[0] &&
			       std::fabs( m.rgb[1] - m.rgb[2] ) <= 1e-4 * m.rgb[0],
				"row D: a flat curve stays achromatic to 1e-4 (unaffected by DL-82)" );
			Check( std::fabs( m.rgb[0] - expect ) <= 0.01 * expect,
				"row D: control on the closed form within 1%" );
		}
	}

	std::cout << "\nResults: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
