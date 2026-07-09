//////////////////////////////////////////////////////////////////////
//
//  RayCasterVolumeAbsorptionTest.cpp - End-to-end correctness check that
//    a HOMOGENEOUS absorbing medium attenuates radiance along a no-scatter
//    eye path by EXACTLY Beer-Lambert (exp(-sigma_a * d)) — NOT
//    exp(-2*sigma_a*d) — when rendered through the LEGACY RayCaster
//    shader-dispatch path.
//
//    THE BUG THIS GUARDS AGAINST (analog volume estimator double-count):
//      HomogeneousMedium::SampleDistance{,NM} is an ANALOG estimator.
//      Reaching a surface / escaping through the medium WITHOUT a scatter
//      event is a stochastic SURVIVAL outcome whose probability
//      exp(-sigma_t_max * d) already carries the Beer-Lambert factor.
//      RayCaster::CastRay{,NM}'s no-scatter fall-through used to multiply
//      the shading result by the full per-channel transmittance
//      Tr = exp(-sigma_t * d) AGAIN, with no compensating division — so a
//      pure absorber rendered exp(-2*sigma_a*d) (~2x too thick / too dark).
//
//      Distance sampling uses m_sigma_t_max = ColorMath::MaxValue(sigma_t),
//      so the survival pdf is the MIN transmittance channel MinValue(Tr) =
//      exp(-sigma_t_max*d).  The correct no-scatter weight is therefore the
//      chromatic ratio Tr / MinValue(Tr): the survival pdf cancels on the
//      max-sigma_t channel (leaving 1 there) and >= 1 on the others so a
//      COLOURED medium keeps its tint.  For a single-wavelength (NM) sample
//      the ratio is Tr/Tr == 1, so no transmittance is applied at all.
//      Simply DELETING the `* Tr` for RGB would be wrong (it desaturates a
//      coloured medium): the coloured case below distinguishes the correct
//      Tr/MinValue fix from the naive delete.
//
//    ------------------------------------------------------------------
//    WHICH RAYCASTER SITES EACH CASE COVERS
//    ------------------------------------------------------------------
//    RayCaster.cpp has SIX no-scatter survival sites (three RGB in CastRay,
//    three NM in CastRayNM): surface-hit, radiance-map, and global-env.
//    These cases exercise the surface-hit site for each variant:
//      * pixelpel_rasterizer                 -> CastRay   surface-hit (RGB)
//      * pixelintegratingspectral_rasterizer -> CastRayNM surface-hit (NM)
//    (Both rasterizers dispatch shading through RayCaster; the PT-integrator
//    rasterizers do NOT — they have their own no-scatter fix in
//    PathTracingIntegrator.cpp.  This test is the RayCaster twin.)
//
//    TOPOLOGY (mirrors VolumeAbsorptionAttenuationTest cases D/E/F): the
//    camera sits INSIDE a scene-wide `global_medium` absorber at the origin,
//    looking +Z at a directly-visible Lambertian luminaire wall whose front
//    face is at distance d.  The eye path traverses length d of medium before
//    the first surface hit, which is exactly the RayCaster no-scatter
//    surface-hit survival site.
//
//    Why this topology and NOT the interior-medium slab of A/B/C?  Empirically
//    verified in this worktree: the RayCaster shader-dispatch path does NOT
//    continue a refracted camera ray through a perfectrefractor slab to the
//    background (a zero-absorption slab renders BLACK under
//    pixelpel_rasterizer), so the slab topology is unreachable here.  A
//    scene-wide global_medium + directly-visible luminaire is the reachable
//    camera-in-medium reference (no light connection needed; self-terminating).
//
//    Each case renders the IDENTICAL scene twice: a zero-absorption render
//    measures the directly-viewed luminaire radiance L0 live (folding in the
//    exitance*INV_PI luminaire-resolve constant), and the absorbing render is
//    asserted against L0 * exp(-sigma_a * d) — single-count Beer-Lambert.
//
//    REVERT-PROOF: temporarily restoring the buggy full-Tr multiply at the
//    RayCaster surface-hit site (801 RGB / removing the NM survival comment
//    and re-adding `c *= EvalTransmittanceNM`) drives the measurement to
//    L0 * exp(-2*sigma_a*d), i.e. an extra exp(-sigma_a*d) ~ 0.37 darker at
//    sigma_a*d = 1, far outside the 8% band — see the report accompanying
//    this change for the measured numbers.
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
// CapturingRasterizerOutput — stores the linear radiance buffer for
// in-test pixel analysis (denoise-free).
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
		"/tmp/raycaster_volabs_%s_%d.RISEscene",
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

	if( !pJob->LoadAsciiSceneAuto( scenePath.c_str() ) ) {
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
// Scene construction — camera INSIDE a scene-wide global medium, viewing a
// directly-visible Lambertian luminaire at distance d through the RayCaster
// rasterizers.  `spectral` selects pixelintegratingspectral_rasterizer
// (NM survival site) vs pixelpel_rasterizer (RGB survival site).
//////////////////////////////////////////////////////////////////////

// Luminaire box centre at kBackWallZ, depth 0.2 -> front face (eye path) at
// kBackWallZ - 0.1.
static const double kBackWallZ     = 2.1;
static const double kBackWallDepth = kBackWallZ - 0.1;   // eye path length d

static std::string BuildScene(
	bool spectral,
	int samples,
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
			"pixelintegratingspectral_rasterizer\n"
			"{\n"
			"\tsamples " << samples << "\n"
			"\tmax_volume_bounce 16\n"
			"\tpixel_filter box\n"
			"\tnmbegin 380\n"
			"\tnmend 720\n"
			"\tnum_wavelengths 8\n"
			"\tspectral_samples 1\n"
			"\thwss false\n"
			"}\n";
	} else {
		ss <<
			"pixelpel_rasterizer\n"
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
		"\tpattern /tmp/raycaster_volabs_unused\n"
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
		"global_medium\n{\nmedium gmed\n}\n";
	return ss.str();
}

static double ObservedTau( double measured )
{
	const double m = std::fmax( measured, 1e-12 );
	return -std::log( m );
}

//////////////////////////////////////////////////////////////////////
// Tolerance.  The signal-vs-bug gap is exp(-tau) vs exp(-2tau) (a factor of
// exp(-tau) ~ 0.37 at tau = 1); 8% cleanly separates them.  For the coloured
// and spectral cases the reference is derived from a live zero-absorption
// render of the identical scene, so any luminaire-resolve / spectral-resolve
// constant cancels and the check is a tight per-channel / mean equality.
//////////////////////////////////////////////////////////////////////
static const double kRelTol = 0.08;

static bool ChannelOk( double measured, double expected, double tol, const char* name )
{
	const double denom = std::fmax( std::fabs( expected ), 1e-4 );
	const double rel = std::fabs( measured - expected ) / denom;
	const bool ok = rel <= tol;
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

//////////////////////////////////////////////////////////////////////
// Case A: RGB gray absorber — pixelpel_rasterizer, CastRay surface-hit.
// The reference L0 is measured live from a zero-absorption render of the
// identical scene, so the luminaire-resolve constant cancels.
//////////////////////////////////////////////////////////////////////
static void TestRGBGray()
{
	std::cout << "[A] pixelpel_rasterizer, RGB gray absorber "
		"(sigma_a = 0.5, d = " << kBackWallDepth
		<< ") — RayCaster::CastRay surface-hit survival" << std::endl;
	const double sa = 0.5;

	const PixelRGB base = RenderCentralBlock( BuildScene( false, 256, 0.0, 0.0, 0.0 ), "rgb_gray_base" );
	const PixelRGB px   = RenderCentralBlock( BuildScene( false, 256, sa, sa, sa ),   "rgb_gray" );

	Check( base.valid && px.valid, "A: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	const double L0 = ( base.r + base.g + base.b ) / 3.0;
	const double m  = ( px.r + px.g + px.b ) / 3.0;
	const double expected = L0 * std::exp( -sa * kBackWallDepth );  // single-count
	std::cout << "    L0(zero-abs)=" << L0 << "  measured=" << m
		<< "  expected single-count=" << expected
		<< "  (buggy double-count ~ " << expected * std::exp( -sa * kBackWallDepth ) << ")"
		<< std::endl;

	Check( ChannelOk( m, expected, kRelTol, "A.mean" ),
		"A: measured == single-count Beer-Lambert (double-count would be ~exp(-1) darker)" );
}

//////////////////////////////////////////////////////////////////////
// Case B: RGB coloured absorber — desaturation discriminator.  A correct
// Tr/MinValue(Tr) fix keeps each channel at L0[c]*exp(-sigma_a[c]*d); the
// naive "delete the * Tr" fix would collapse the channels toward the survival
// pdf and desaturate.
//////////////////////////////////////////////////////////////////////
static void TestRGBColored()
{
	std::cout << "[B] pixelpel_rasterizer, RGB coloured absorber "
		"(sigma_a = 0.2/0.6/1.2, d = " << kBackWallDepth
		<< ") — CastRay surface-hit, desaturation discriminator" << std::endl;
	const double sar = 0.2, sag = 0.6, sab = 1.2;

	const PixelRGB base = RenderCentralBlock( BuildScene( false, 1024, 0.0, 0.0, 0.0 ), "rgb_col_base" );
	const PixelRGB px   = RenderCentralBlock( BuildScene( false, 1024, sar, sag, sab ), "rgb_col" );

	Check( base.valid && px.valid, "B: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	const double er = base.r * std::exp( -sar * kBackWallDepth );
	const double eg = base.g * std::exp( -sag * kBackWallDepth );
	const double eb = base.b * std::exp( -sab * kBackWallDepth );
	std::cout << "    base(" << base.r << ", " << base.g << ", " << base.b << ")"
		<< "  measured(" << px.r << ", " << px.g << ", " << px.b << ")"
		<< "  expected(" << er << ", " << eg << ", " << eb << ")" << std::endl;

	Check( ChannelOk( px.r, er, kRelTol, "B.r" ), "B: red   == L0_r*exp(-0.2*d)" );
	Check( ChannelOk( px.g, eg, kRelTol, "B.g" ), "B: green == L0_g*exp(-0.6*d)" );
	Check( ChannelOk( px.b, eb, kRelTol, "B.b" ), "B: blue  == L0_b*exp(-1.2*d)" );

	Check( px.r > px.g * 1.2 && px.g > px.b * 1.2,
		"B: medium stays coloured (r > g > b, not desaturated)" );
}

//////////////////////////////////////////////////////////////////////
// Case C: NM / spectral gray absorber — pixelintegratingspectral_rasterizer,
// CastRayNM surface-hit.  For NM the survival weight is exactly 1, so the
// no-scatter branch applies NO transmittance; the buggy branch would apply
// EvalTransmittanceNM and double-count.  The spectral->RGB resolve tints both
// renders identically, so the mean-floor reference cancels the resolve
// constant just as in A.
//////////////////////////////////////////////////////////////////////
static void TestNMSpectral()
{
	std::cout << "[C] pixelintegratingspectral_rasterizer, NM gray absorber "
		"(sigma_a = 0.5, d = " << kBackWallDepth
		<< ", hwss off) — RayCaster::CastRayNM surface-hit survival" << std::endl;
	const double sa = 0.5;

	const PixelRGB base = RenderCentralBlock( BuildScene( true, 512, 0.0, 0.0, 0.0 ), "nm_base" );
	const PixelRGB px   = RenderCentralBlock( BuildScene( true, 512, sa, sa, sa ),   "nm" );

	Check( base.valid && px.valid, "C: renders produced frames" );
	if( !base.valid || !px.valid ) return;

	const double L0 = ( base.r + base.g + base.b ) / 3.0;
	const double m  = ( px.r + px.g + px.b ) / 3.0;
	const double expected = L0 * std::exp( -sa * kBackWallDepth );
	std::cout << "    L0(zero-abs)=" << L0 << "  measured=" << m
		<< "  expected single-count=" << expected << std::endl;

	Check( ChannelOk( m, expected, kRelTol, "C.mean" ),
		"C: spectral measured == single-count Beer-Lambert (CastRayNM survival)" );
}

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "RayCasterVolumeAbsorptionTest — Beer-Lambert single-count "
		"through the legacy RayCaster shader-dispatch path" << std::endl;

	TestRGBGray();
	TestRGBColored();
	TestNMSpectral();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
