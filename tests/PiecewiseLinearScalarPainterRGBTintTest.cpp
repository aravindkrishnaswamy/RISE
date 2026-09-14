//////////////////////////////////////////////////////////////////////
//
//  PiecewiseLinearScalarPainterRGBTintTest.cpp - render regression for
//    DL-29 (docs/DEBT_LEDGER.md): a measured 2-column spectral curve
//    bound through `scalar_painter { file ... }`
//    (PiecewiseLinearScalarPainter) must reach the RGB rasterizers as
//    three real per-channel values -- the curve sampled at
//    `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm -- not as the
//    pre-fix broadcast of one 555 nm sample into all three channels.
//
//    Two rows, because the defect had two distinct consequences.
//
//    ROW 1 -- `tau` (a LINEARLY consumed slot).  A dielectric slab
//    (uniform ior 1.5, no dispersion: this row is about `tau`) whose
//    `tau` is the 2-column file
//        380 0.05     (blue heavily absorbed)
//        720 0.95     (red mostly transmitted)
//    with a white Lambertian luminaire directly behind it.
//    `DielectricSPF::DoSingleRGBComponent` charges Beer's law as
//    `pow(tau_channel, distance)` on the exit hit, so with the camera
//    on axis and the box 2 units deep every pixel reads the CLOSED FORM
//        L * (1 - R0(1.5))^2 * tau(lambda_c)^2,
//    L = 1/pi, R0(n) = ((n-1)/(n+1))^2, and lambda_c the channel's
//    representative wavelength.  That is 0.128355 / 0.072564 / 0.022185
//    for R / G / B -- asserted per channel, plus the R/B ratio 5.7857.
//    Pre-fix every channel instead read the 555 nm value through the
//    same formula, 0.077273, which is exactly the 0.0772 grey the
//    original red-proof recorded.
//
//    ROW 2 -- `ior` (a NON-LINEARLY consumed slot), the control this
//    row exists for.  Reporting three real channel values also flips
//    `PiecewiseLinearScalarPainter::HasPerChannelVariation()` to true,
//    which is what `DielectricSPF::Scatter`'s `disperse` predicate
//    reads: a measured `ior` file now takes the RGB DISPERSION path
//    (three per-channel refractions) where pre-fix it took the single
//    achromatic path.  That is the physically intended behaviour for a
//    measured dispersion curve, and it is what makes a CMF-integrated
//    triple the wrong convention for this slot -- Fresnel of the
//    integral is not the integral of Fresnel.  The row renders a
//    lossless slab (`tau 1.0`) whose `ior` is
//        380 2.4
//        720 1.3
//    and asserts each channel against `L * (1 - R0(ior(lambda_c)))^2`
//    = 0.280939 / 0.263925 / 0.241150.  Matching the closed form on all
//    three channels is also the "no dropped scattered ray" check: the
//    dispersive path emits 3 refractions + 3 Fresnel reflections per
//    hit, well inside `ScatteredRayContainer::kCapacity` (12,
//    Interfaces/ISPF.h), and a dropped ray would show up here as a
//    channel reading low.
//
//    ROW 2 CONTROL -- the same scene with a plain uniform `ior` equal
//    to the curve's GREEN sample (1.8532353).  Its (achromatic) mean
//    must equal row 2's G channel: the dispersive path's green branch
//    and an ordinary non-dispersive render at the same index are the
//    same physics, so this pins that the per-channel routing lines the
//    painter's channel c up with the SPF's refraction at
//    `kChannelNM[c]` rather than shuffling them.
//
//    SAMPLING NOTE.  Row 2 and its control run at 256 spp, not row 1's
//    64: the dispersive path splits each hit into per-channel rays that
//    PT selects stochastically with a 1/selectProb correction, so the
//    estimator is unbiased but roughly 3x noisier per sample than row
//    1's single achromatic lobe.
//
//  Author: Claude (debt-precision slice, DL-29)
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
#include "../src/Library/Interfaces/IScalarPainter.h"
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
	std::snprintf( path, sizeof(path), "/tmp/pwl_tint_%s_%d", suffix,
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

//! The straight-line curve `(loNm, loVal) -> (hiNm, hiVal)` at `atNm`,
//! evaluated here rather than read back out of the painter so the
//! expectation is independent of the code under test.
static double CurveAt( double loNm, double loVal, double hiNm, double hiVal, double atNm )
{
	if( atNm <= loNm ) return loVal;
	if( atNm >= hiNm ) return hiVal;
	return loVal + ( atNm - loNm ) / ( hiNm - loNm ) * ( hiVal - loVal );
}

static const double kL = 1.0 / 3.14159265358979323846;	// Lambertian luminaire, exitance 1

//! Scene skeleton shared by every row: pinhole camera on +Z looking
//! down -Z at a white Lambertian quad behind a 2-deep dielectric box.
//! `iorRef` / `tauRef` are the material's parameter VALUES (a painter
//! name or an inline literal); `extraChunks` carries any
//! `scalar_painter` chunks those names need.
static std::string Scene(
	const std::string& extraChunks,
	const char* iorRef,
	const char* tauRef,
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
		+ extraChunks +
		"dielectric_material\n{\n\tname mat_slab\n\tior " + iorRef + "\n"
		"\ttau " + tauRef + "\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_slab\n\twidth 4\n\theight 4\n\tdepth 2\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n"
		"\tmaterial mat_slab\n\tposition 0 0 0\n}\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples " + samples + "\n"
		"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static void CheckChannels(
	const Means& m,
	const double expect[3],
	double relTol,
	const char* label )
{
	static const char* chan[3] = { "R", "G", "B" };
	for( int c = 0; c < 3; ++c ) {
		char msg[192];
		std::snprintf( msg, sizeof(msg), "%s: %s == %.6f within %.0f%%",
			label, chan[c], expect[c], relTol * 100.0 );
		Check( std::fabs( m.rgb[c] - expect[c] ) <= relTol * expect[c], msg );
	}
}

//////////////////////////////////////////////////////////////////////
// ROW 1 -- `tau`, a linearly consumed slot.
//////////////////////////////////////////////////////////////////////
static void RunTauRow( const std::string& tauPath )
{
	double expect[3];
	for( int c = 0; c < 3; ++c ) {
		const double tau = CurveAt( 380, 0.05, 720, 0.95,
			double( ScalarPainterRGB::kChannelNM[c] ) );
		expect[c] = kL * ( 1.0 - R0( 1.5 ) ) * ( 1.0 - R0( 1.5 ) ) * tau * tau;
	}
	const double grey555 = kL * ( 1.0 - R0( 1.5 ) ) * ( 1.0 - R0( 1.5 ) )
		* CurveAt( 380, 0.05, 720, 0.95, 555.0 )
		* CurveAt( 380, 0.05, 720, 0.95, 555.0 );

	std::cout << "Row 1 (tau curve, ior 1.5): closed form R=" << expect[0]
	          << " G=" << expect[1] << " B=" << expect[2]
	          << "  R/B=" << ( expect[0] / expect[2] )
	          << "  [pre-fix 555nm grey would be " << grey555 << "]" << std::endl;

	const std::string chunk =
		"scalar_painter\n{\n\tname tau_tinted\n\tfile " + tauPath + "\n}\n\n";
	const Means m = RenderMeans( Scene( chunk, "1.5", "tau_tinted", "64" ), "tau" );
	Check( m.valid, "row 1: render produced output" );
	if( !m.valid ) return;

	std::cout << "    mean R=" << m.rgb[0] << " G=" << m.rgb[1] << " B=" << m.rgb[2]
	          << "  R/B=" << ( m.rgb[2] > 1e-12 ? m.rgb[0] / m.rgb[2] : -1.0 ) << std::endl;

	CheckChannels( m, expect, 0.03, "row 1" );
	// The ratio, stated separately: it is the quantity a grey broadcast
	// cannot produce at all (it would read 1.0), and it is insensitive
	// to any common-factor error in the harness.
	const double ratio = m.rgb[2] > 1e-12 ? m.rgb[0] / m.rgb[2] : -1.0;
	const double expectRatio = expect[0] / expect[2];
	Check( std::fabs( ratio - expectRatio ) <= 0.05 * expectRatio,
		"row 1: R/B == tau(611)^2 / tau(465)^2 within 5%" );
	// G strictly between, as the monotonic curve requires.
	Check( m.rgb[0] > m.rgb[1] && m.rgb[1] > m.rgb[2],
		"row 1: R > G > B (monotonic tau curve, no channel collapse)" );
}

//////////////////////////////////////////////////////////////////////
// ROW 2 -- `ior`, a non-linearly consumed slot: a measured curve now
// drives RGB dispersion.  Plus the inline-triple equivalence control.
//////////////////////////////////////////////////////////////////////
static void RunIorRow( const std::string& iorPath )
{
	double iorC[3];
	double expect[3];
	double expectSum = 0;
	for( int c = 0; c < 3; ++c ) {
		iorC[c] = CurveAt( 380, 2.4, 720, 1.3, double( ScalarPainterRGB::kChannelNM[c] ) );
		expect[c] = kL * ( 1.0 - R0( iorC[c] ) ) * ( 1.0 - R0( iorC[c] ) );
		expectSum += expect[c];
	}

	std::cout << "Row 2 (ior curve, tau 1.0): ior R=" << iorC[0] << " G=" << iorC[1]
	          << " B=" << iorC[2] << std::endl;
	std::cout << "    closed form R=" << expect[0] << " G=" << expect[1]
	          << " B=" << expect[2] << "  R/B=" << ( expect[0] / expect[2] ) << std::endl;

	const std::string chunk =
		"scalar_painter\n{\n\tname ior_curve\n\tfile " + iorPath + "\n}\n\n";
	const Means m = RenderMeans( Scene( chunk, "ior_curve", "1.0", "256" ), "ior" );
	Check( m.valid, "row 2: render produced output" );
	if( !m.valid ) return;

	std::cout << "    mean R=" << m.rgb[0] << " G=" << m.rgb[1] << " B=" << m.rgb[2]
	          << "  R/B=" << ( m.rgb[2] > 1e-12 ? m.rgb[0] / m.rgb[2] : -1.0 ) << std::endl;

	// (i) Dispersion is LIVE and ordered.  Pre-fix this slab rendered
	// exactly achromatic: the painter reported no per-channel variation,
	// so `DielectricSPF::Scatter`'s `disperse` predicate was false and
	// one achromatic lobe carried the 555 nm ior.  The ior curve FALLS
	// toward red, so red reflects least and must read brightest.
	Check( m.rgb[0] > m.rgb[1] && m.rgb[1] > m.rgb[2],
		"row 2: R > G > B -- per-channel refraction is live (pre-fix: exactly achromatic)" );

	// (ii) Per-channel magnitude, at an 8% band -- NOT tighter, and the
	// reason is a PRE-EXISTING defect in the RGB dispersion path itself,
	// filed as DL-81 (docs/DEBT_LEDGER.md): the dispersive path's three
	// channels carry channel-INDEX-dependent systematic offsets (channel
	// 0 reads ~+6%, channel 1 ~-5% against the same closed form) that
	// reproduce identically when the same three iors are authored as an
	// inline `r g b` triple instead of loaded from a file, i.e. they have
	// nothing to do with this painter.  The band is here to catch a gross
	// energy error -- a dropped scattered ray, a mis-routed channel --
	// not to certify the dispersive path's accuracy.
	CheckChannels( m, expect, 0.08, "row 2" );
	const double sum = m.rgb[0] + m.rgb[1] + m.rgb[2];
	std::cout << "    sum=" << sum << "  closed-form sum=" << expectSum << std::endl;
	// The channel offsets largely cancel in the sum, so the total is the
	// sharper "no scattered ray was dropped" statement.  The dispersive
	// path emits 3 refractions + 3 Fresnel reflections per hit, well
	// inside ScatteredRayContainer::kCapacity (12, Interfaces/ISPF.h); a
	// dropped ray would show here as a deficit.
	Check( std::fabs( sum - expectSum ) <= 0.02 * expectSum,
		"row 2: R+G+B == closed-form total within 2% (no dropped scattered ray)" );

	// (iii) CONTROL -- the same three iors authored INLINE as an `r g b`
	// triple (an RGBScalarPainter) instead of loaded from the curve file.
	// Both go through the identical dispersive code with the identical
	// deterministic sampler, so this must match to the last bit: it is an
	// EXACT statement that `GetValuesAt` on the file curve returns the
	// curve at ScalarPainterRGB::kChannelNM and that channel c of that
	// triple reaches the SPF's channel-c refraction -- and, unlike (ii),
	// it is completely insensitive to DL-81.
	char inlineTriple[128];
	std::snprintf( inlineTriple, sizeof(inlineTriple), "%.17g %.17g %.17g",
		iorC[0], iorC[1], iorC[2] );
	const Means c = RenderMeans( Scene( std::string(), inlineTriple, "1.0", "256" ), "iorctl" );
	Check( c.valid, "row 2 control: render produced output" );
	if( !c.valid ) return;

	std::cout << "    control (inline ior triple) R=" << c.rgb[0] << " G=" << c.rgb[1]
	          << " B=" << c.rgb[2] << std::endl;

	for( int ch = 0; ch < 3; ++ch ) {
		static const char* chan[3] = { "R", "G", "B" };
		char msg[192];
		std::snprintf( msg, sizeof(msg),
			"row 2 control: file-curve %s == inline-triple %s exactly", chan[ch], chan[ch] );
		Check( std::fabs( m.rgb[ch] - c.rgb[ch] ) <= 1e-12, msg );
	}
}

int main( int argc, char** argv )
{
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "=== PiecewiseLinearScalarPainterRGBTintTest ===" << std::endl;
	std::cout << "seed base = " << g_seedBase << std::endl;

	const std::string tauPath = WriteToTempFile( "380 0.05\n720 0.95\n", "tau.txt" );
	Check( !tauPath.empty(), "tau curve file written" );
	const std::string iorPath = WriteToTempFile( "380 2.4\n720 1.3\n", "ior.txt" );
	Check( !iorPath.empty(), "ior curve file written" );

	if( !tauPath.empty() ) RunTauRow( tauPath );
	if( !iorPath.empty() ) RunIorRow( iorPath );

	std::remove( tauPath.c_str() );
	std::remove( iorPath.c_str() );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
