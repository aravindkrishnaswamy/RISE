//////////////////////////////////////////////////////////////////////
//
//  DoubleSidedEmitterTest.cpp - DL-320: a DOUBLE-SIDED emitter emits
//    from BOTH faces for every strategy, not only for the ones that
//    HIT it.  docs/DL320_DOUBLE_SIDED_EMITTER.md.
//
//    THE CONTRACT (docs/SCENE_CONVENTIONS.md section 3.5, "Sidedness is
//    the corner winding plus `doublesided`"): with `doublesided TRUE`
//    (the `clippedplane_geometry` default) a back-face hit flips the
//    normal toward the ray, so a Lambertian luminaire -- one-sided about
//    the normal it is handed -- emits from BOTH faces.  Every strategy
//    that HITS the emitter (PT's BSDF-sampled emission, `EmissionShaderOp`,
//    BDPT s = 0, VCM `EvaluateS0Impl`) always honoured that.  Every
//    strategy that SAMPLES it (NEE in `LightSampler`, the light-subpath
//    root in `LightSampler::SampleLight`, the photon tracers) treated it
//    as one-sided about the winding normal, while the hit side's MIS
//    weights still counted them as able to reach the back face.
//
//    THE FIXTURE.  A 3x3 quad at height h = 2.6 above a 400 x 400
//    Lambertian floor (rho 0.5), nothing else in the scene.  An
//    ORTHOGRAPHIC camera at z = 1 looks straight down, so it sees only
//    the floor, every pixel's footprint is a known floor square, and the
//    only transport is emitter -> floor -> camera (the emitter carries
//    `material none`; the floor cannot see itself).  "Face" winds the
//    quad's normal DOWN (toward the floor); "back" winds it UP, so the
//    floor is lit by the BACK face.  The expected image is identical.
//
//    THE CLOSED FORM.  A Lambertian emitter of exitance M has radiance
//    M / pi; a receiver at (x, y, 0) under a parallel rectangle
//    [x1, x2] x [y1, y2] at height h receives E = M * F, F the point-to-
//    parallel-rectangle form factor (Howell's catalog, superposed over
//    the four corners):
//        f(a, b) = 1/(2 pi) [ A/sqrt(1+A^2) atan(B/sqrt(1+A^2))
//                           + B/sqrt(1+B^2) atan(A/sqrt(1+B^2)) ],
//        A = a/h, B = b/h,
//        F = f(x2-x, y2-y) - f(x1-x, y2-y) - f(x2-x, y1-y) + f(x1-x, y1-y),
//    and the floor's outgoing radiance is rho/pi * E.  With a box pixel
//    filter the image mean is that averaged over the camera's square
//    footprint, integrated here on a 512 x 512 midpoint grid.  The
//    analytic F is itself cross-checked against a brute-force quadrature
//    of h^2 / (pi d^4) over the emitter.  The formula does not know which
//    face is lit -- that is the point.
//
//    ROWS (every render salted, n renders per cell, mean and sd quoted):
//      Z1  back face vs closed form, and face-down vs closed form, for
//          PT, BDPT, VCM (connections only), the legacy `pixelpel`
//          direct-lighting chain (NEE only), and the legacy chain with a
//          `distributiontracing_shaderop` sibling (NEE and BSDF-sampled
//          emission MIS-combined -- the hit-side partner must describe
//          the same two-faced density NEE now samples).
//      Z2  a MIXED scene: the back-lit double-sided quad plus a small
//          single-sided face-down quad.  The double-sided emitter's
//          selection weight doubles with its power; every consumer of
//          the selection PMF must read the same value or the two
//          lights' MIS partitions stop summing to one.
//      Z3  spectral (reference-free, back / face): PT hero, PT HWSS,
//          BDPT HWSS -- the NM NEE arm and the NM hero / HWSS companion
//          rebuilds of the light-subpath root.
//      Z4  VCM WITH merging and the legacy global photon map
//          (reference-free, back / face): the light subpath's emission
//          and the photon tracer's emission.
//      Z5  SINGLE-SIDED control (`doublesided FALSE`, winding up): the
//          back face must be black under every integrator, exactly.
//          The pixel hashes printed here are the bit-identity A/B for
//          the single-sided path between builds.
//
//    Pre-fix (red-proof, docs/DL320_DOUBLE_SIDED_EMITTER.md): the back
//    face read PT 0.09 / BDPT 0.09 / VCM 0.23 of the closed form and the
//    direct-lighting legacy chain exactly 0.
//
//    Seeding: every render reseeds libc rand() from argv[1] (default
//    3200) plus a running index, and carries its own Sobol VALUE salt
//    (SobolSamplerTestHooks::ValueSalt) so repeats are independent
//    randomized-QMC replicates rather than one point set.
//
//  Author: Claude (debt-dl320 slice, DL-320)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <algorithm>
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
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_seedBase = 3200u;
static unsigned int g_renderIndex = 0;
static int g_repeats = 4;
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
// Capture
//////////////////////////////////////////////////////////////////////
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
	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		pixels.resize( img.GetWidth() * img.GetHeight() );
		for( unsigned int y = 0; y < img.GetHeight(); y++ ) {
			for( unsigned int x = 0; x < img.GetWidth(); x++ ) {
				pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
			}
		}
	}
};

struct RenderResult
{
	double mean;				//!< grey mean, composited over black
	unsigned long long hash;	//!< FNV-1a of the raw pixel doubles
	bool ok;
};

//! FNV-1a over the captured buffer -- the bit-identity A/B for the
//! single-sided control rows.
static unsigned long long HashPixels( const CapturingRasterizerOutput& cap )
{
	unsigned long long h = 1469598103934665603ULL;
	for( const RISEColor& c : cap.pixels ) {
		const double v[4] = { c.base.r, c.base.g, c.base.b, c.a };
		const unsigned char* b = reinterpret_cast<const unsigned char*>( v );
		for( std::size_t k = 0; k < sizeof( v ); k++ ) {
			h ^= b[k];
			h *= 1099511628211ULL;
		}
	}
	return h;
}

static const uint32_t kSaltTag = 0x320u;

static RenderResult Render( const std::string& sceneText, const char* tag )
{
	RenderResult r{ 0, 0, false };
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/dl320_%s_%d.RISEscene", tag, static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return r;
		ofs << sceneText;
	}
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path ); return r; }
	if( pJob->LoadAsciiSceneViaCst( path ) ) {
		pJob->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );
		const uint32_t salt = SobolSequence::HashCombine( g_seedBase + g_renderIndex, kSaltTag );
		SobolSamplerTestHooks::ValueSalt().store( salt );
		std::srand( g_seedBase + g_renderIndex++ );
		const bool bRendered = pJob->Rasterize();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		if( bRendered && !pCap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : pCap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) {
				r.mean = sum / double( pCap->pixels.size() );
				r.hash = HashPixels( *pCap );
				r.ok = true;
			}
		}
		safe_release( pCap );
	}
	safe_release( pJob );
	std::remove( path );
	return r;
}

struct Stat { double mean; double sd; int n; bool ok; unsigned long long firstHash; };

static Stat RenderN( const std::string& sceneText, const char* tag, const int n )
{
	Stat s{ 0, 0, 0, true, 0 };
	std::vector<double> v;
	for( int i = 0; i < n; i++ ) {
		const RenderResult r = Render( sceneText, tag );
		if( !r.ok ) { s.ok = false; return s; }
		if( i == 0 ) s.firstHash = r.hash;
		v.push_back( r.mean );
	}
	for( double x : v ) s.mean += x;
	s.mean /= double( v.size() );
	double ss = 0;
	for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
	s.sd = v.size() > 1 ? std::sqrt( ss / double( v.size() - 1 ) ) : 0;
	s.n = int( v.size() );
	return s;
}

//////////////////////////////////////////////////////////////////////
// Closed form
//////////////////////////////////////////////////////////////////////
static const double kPi = 3.14159265358979323846;
static const double kRho = 0.5;
static const double kH = 2.6;			// emitter height above the floor
static const double kHalf = 1.5;		// emitter half-width (3 x 3 quad)
static const double kScale = 10.0;		// exitance M (white painter x scale)
static const double kView = 4.0;		// orthographic viewport width (floor square [-2, 2]^2)

// The second, SINGLE-sided emitter of row Z2.
static const double kH2 = 1.4;
static const double kCx2 = 1.2;
static const double kHalf2 = 0.4;
static const double kScale2 = 6.0;

static double CornerF( const double a, const double b, const double h )
{
	const double A = a / h, B = b / h;
	const double sA = std::sqrt( 1.0 + A * A ), sB = std::sqrt( 1.0 + B * B );
	return ( A / sA * std::atan( B / sA ) + B / sB * std::atan( A / sB ) ) / ( 2.0 * kPi );
}

//! Point-to-parallel-rectangle form factor at receiver (x, y), rectangle
//! [x1,x2]x[y1,y2] at height h.
static double RectF( double x, double y, double x1, double x2, double y1, double y2, double h )
{
	return CornerF( x2 - x, y2 - y, h ) - CornerF( x1 - x, y2 - y, h )
		- CornerF( x2 - x, y1 - y, h ) + CornerF( x1 - x, y1 - y, h );
}

//! Brute-force cross-check of RectF: integral of h^2/(pi d^4) dA.
static double RectFQuadrature( double x, double y, double x1, double x2, double y1, double y2, double h )
{
	const int N = 600;
	double sum = 0;
	const double dx = ( x2 - x1 ) / N, dy = ( y2 - y1 ) / N;
	for( int j = 0; j < N; j++ ) {
		for( int i = 0; i < N; i++ ) {
			const double u = x1 + ( i + 0.5 ) * dx - x;
			const double v = y1 + ( j + 0.5 ) * dy - y;
			const double d2 = u * u + v * v + h * h;
			sum += h * h / ( kPi * d2 * d2 );
		}
	}
	return sum * dx * dy;
}

//! Image mean of the floor under the given lights: rho/pi * mean(E) over
//! the orthographic footprint [-kView/2, kView/2]^2.
static double ClosedFormImageMean( const bool withSecond )
{
	const int N = 512;
	double sum = 0;
	for( int j = 0; j < N; j++ ) {
		for( int i = 0; i < N; i++ ) {
			const double x = -kView / 2 + ( i + 0.5 ) * kView / N;
			const double y = -kView / 2 + ( j + 0.5 ) * kView / N;
			double E = kScale * RectF( x, y, -kHalf, kHalf, -kHalf, kHalf, kH );
			if( withSecond ) {
				E += kScale2 * RectF( x, y, kCx2 - kHalf2, kCx2 + kHalf2, -kHalf2, kHalf2, kH2 );
			}
			sum += E;
		}
	}
	return kRho / kPi * sum / double( N * N );
}

//////////////////////////////////////////////////////////////////////
// Scene text
//////////////////////////////////////////////////////////////////////
static std::string Fmt( const char* f, ... )
{
	char buf[4096];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

//! The quad at height `h`, half-width `half`, centred at (cx, 0).
//! `faceDown` winds the normal to -Z (toward the floor); otherwise +Z,
//! so the floor sees the BACK face.
static std::string EmitterQuad( const char* name, double cx, double half, double h, bool faceDown, bool doubleSided, double scale )
{
	const double x1 = cx - half, x2 = cx + half, y1 = -half, y2 = half;
	// normal = normalize( Cross( ptb - pta, ptd - pta ) )
	std::string corners;
	if( faceDown ) {
		corners = Fmt( "\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n",
			x1, y2, h,  x2, y2, h,  x2, y1, h,  x1, y1, h );
	} else {
		corners = Fmt( "\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n",
			x1, y1, h,  x2, y1, h,  x2, y2, h,  x1, y2, h );
	}
	return Fmt(
		"uniformcolor_painter\n{\n\tname pnt_%s\n\tcolor 1 1 1\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_%s\n\texitance pnt_%s\n\tscale %g\n\tmaterial none\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_%s\n%s\tdoublesided %s\n}\n\n"
		"standard_object\n{\n\tname %s\n\tgeometry geo_%s\n\tmaterial mat_%s\n}\n\n",
		name, name, name, scale, name, corners.c_str(), doubleSided ? "TRUE" : "FALSE", name, name, name );
}

static std::string SceneBody( bool faceDown, bool doubleSided, bool withSecond )
{
	std::string s =
		"film\n{\n\twidth 32\n\theight 32\n}\n\n"
		"orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n"
		+ Fmt( "\tviewport_scale %g %g\n}\n\n", kView, kView ) +
		Fmt( "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor %g %g %g\n}\n\n", kRho, kRho, kRho ) +
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_floor\n"
		"\tpta -200 -200 0\n\tptb 200 -200 0\n\tptc 200 200 0\n\tptd -200 200 0\n\tdoublesided FALSE\n}\n\n"
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n";
	s += EmitterQuad( "emit", 0.0, kHalf, kH, faceDown, doubleSided, kScale );
	if( withSecond ) {
		s += EmitterQuad( "emit2", kCx2, kHalf2, kH2, true, false, kScale2 );
	}
	return s;
}

static std::string Header() { return "RISE ASCII SCENE 7\n\n"; }

static std::string Shader( const char* ops )
{
	return std::string( "standard_shader\n{\n\tname global\n" ) + ops + "}\n\n";
}

static std::string RasPT( int spp )
{
	return Shader( "\tshaderop DefaultPathTracing\n" ) +
		Fmt( "pathtracing_pel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", spp );
}
static std::string RasBDPT( int spp )
{
	return Shader( "\tshaderop DefaultPathTracing\n" ) +
		Fmt( "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", spp );
}
static std::string RasVCM( int spp, bool merging )
{
	return Shader( "\tshaderop DefaultPathTracing\n" ) +
		Fmt( "vcm_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n"
			"\tmerge_radius %s\n\tvc_enabled true\n\tvm_enabled %s\n}\n\n", spp, merging ? "0.05" : "0.0", merging ? "true" : "false" );
}
static std::string RasPixelDirect( int spp )
{
	return Shader( "\tshaderop DefaultDirectLighting\n" ) +
		Fmt( "pixelpel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", spp );
}
static std::string RasPixelDirectPlusDT( int spp )
{
	return std::string( "distributiontracing_shaderop\n{\n\tname dt\n\tsamples 4\n}\n\n" ) +
		Shader( "\tshaderop DefaultDirectLighting\n\tshaderop dt\n" ) +
		Fmt( "pixelpel_rasterizer\n{\n\tsamples %d\n\tmax_recursion 2\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", spp );
}
static std::string RasPTSpectral( int spp, bool hwss )
{
	return Shader( "\tshaderop DefaultPathTracing\n" ) +
		Fmt( "pathtracing_spectral_rasterizer\n{\n\tsamples %d\n\thwss %s\n\tnum_wavelengths 16\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n",
			spp, hwss ? "TRUE" : "FALSE" );
}
static std::string RasBDPTSpectral( int spp, bool hwss )
{
	return Shader( "\tshaderop DefaultPathTracing\n" ) +
		Fmt( "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples %d\n\thwss %s\n\tnum_wavelengths 16\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n",
			spp, hwss ? "TRUE" : "FALSE" );
}
static std::string RasPhotonGlobal( int spp )
{
	return std::string(
		"global_pel_photonmap\n{\n\tnum 200000\n\tmax_recursion 2\n\tmin_importance 0.001\n\tbranch FALSE\n}\n\n"
		"global_pel_gather\n{\n\tmax_photons 200\n\tradius 0.3\n}\n\n" ) +
		Shader( "\tshaderop DefaultGlobalPelPhotonMap\n" ) +
		Fmt( "pixelpel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n", spp );
}

//////////////////////////////////////////////////////////////////////
// Rows
//////////////////////////////////////////////////////////////////////
struct Ras { const char* name; std::string text; double tol; };

static void Print( const char* label, const Stat& s, const double ref )
{
	std::printf( "    %-44s mean=%.6f sd=%.6f n=%d  ratio=%.4f  hash0=%016llx\n",
		label, s.mean, s.sd, s.n, ( s.ok && ref > 0 ) ? s.mean / ref : -1.0, s.firstHash );
}

static void TestClosedFormCrossCheck()
{
	std::cout << "\n-- closed form: analytic form factor vs brute-force quadrature --" << std::endl;
	const double pts[3][2] = { { 0, 0 }, { 1.7, -0.4 }, { -2.0, 2.0 } };
	for( const auto& p : pts ) {
		const double a = RectF( p[0], p[1], -kHalf, kHalf, -kHalf, kHalf, kH );
		const double q = RectFQuadrature( p[0], p[1], -kHalf, kHalf, -kHalf, kHalf, kH );
		std::printf( "    F(%g, %g): analytic %.8f quadrature %.8f\n", p[0], p[1], a, q );
		Check( std::fabs( a - q ) < 1e-5 * std::max( 1.0, a ), "closed form: analytic F matches quadrature" );
	}
}

//! Row Z1: back face and face-down vs the closed form.
static void TestZ1()
{
	std::cout << "\n-- Z1: double-sided quad, floor lit by the BACK face vs FACE-down, vs closed form --" << std::endl;
	const double cf = ClosedFormImageMean( false );
	std::printf( "    closed form image mean = %.6f\n", cf );
	const std::vector<Ras> rs = {
		{ "PT",                          RasPT( 64 ),               0.02 },
		{ "BDPT",                        RasBDPT( 64 ),             0.02 },
		{ "VCM (connections only)",      RasVCM( 64, false ),       0.02 },
		{ "pixelpel [DirectLighting]",   RasPixelDirect( 64 ),      0.02 },
		{ "pixelpel [DirectLighting, dt]", RasPixelDirectPlusDT( 16 ), 0.03 },
	};
	for( const Ras& r : rs ) {
		const Stat f = RenderN( Header() + r.text + SceneBody( true, true, false ), "z1f", g_repeats );
		const Stat b = RenderN( Header() + r.text + SceneBody( false, true, false ), "z1b", g_repeats );
		Print( ( std::string( r.name ) + " face-down" ).c_str(), f, cf );
		Print( ( std::string( r.name ) + " BACK face" ).c_str(), b, cf );
		Check( f.ok && std::fabs( f.mean / cf - 1.0 ) < r.tol, std::string( "Z1 " ) + r.name + ": face-down matches the closed form" );
		Check( b.ok && std::fabs( b.mean / cf - 1.0 ) < r.tol, std::string( "Z1 " ) + r.name + ": BACK face matches the closed form" );
	}
}

//! Row Z2: mixed lights -- the back-lit double-sided quad plus a small
//! single-sided face-down quad.
static void TestZ2()
{
	std::cout << "\n-- Z2: back-lit double-sided quad + single-sided quad (selection PMF consistency) --" << std::endl;
	const double cf = ClosedFormImageMean( true );
	std::printf( "    closed form image mean = %.6f\n", cf );
	const std::vector<Ras> rs = {
		{ "PT",                     RasPT( 64 ),           0.02 },
		{ "BDPT",                   RasBDPT( 64 ),         0.02 },
		{ "VCM (connections only)", RasVCM( 64, false ),   0.02 },
	};
	for( const Ras& r : rs ) {
		const Stat b = RenderN( Header() + r.text + SceneBody( false, true, true ), "z2b", g_repeats );
		Print( ( std::string( r.name ) + " mixed, BACK face" ).c_str(), b, cf );
		Check( b.ok && std::fabs( b.mean / cf - 1.0 ) < r.tol, std::string( "Z2 " ) + r.name + ": mixed scene matches the closed form" );
	}
}

//! Row Z3/Z4: reference-free back / face ratios.
static void TestBackOverFace( const char* title, const std::vector<Ras>& rs )
{
	std::cout << "\n-- " << title << " --" << std::endl;
	for( const Ras& r : rs ) {
		const Stat f = RenderN( Header() + r.text + SceneBody( true, true, false ), "rff", g_repeats );
		const Stat b = RenderN( Header() + r.text + SceneBody( false, true, false ), "rfb", g_repeats );
		Print( ( std::string( r.name ) + " face-down" ).c_str(), f, f.mean );
		Print( ( std::string( r.name ) + " BACK face (ratio = back/face)" ).c_str(), b, f.mean );
		Check( f.ok && b.ok && f.mean > 0 && std::fabs( b.mean / f.mean - 1.0 ) < r.tol,
			std::string( title ) + " " + r.name + ": back face renders like face-down" );
	}
}

//! Row Z5: single-sided control.  The back face emits nothing.
static void TestZ5()
{
	std::cout << "\n-- Z5: SINGLE-sided quad (doublesided FALSE), floor under the back face: exactly black --" << std::endl;
	const std::vector<Ras> rs = {
		{ "PT",                        RasPT( 16 ),           0 },
		{ "BDPT",                      RasBDPT( 16 ),         0 },
		{ "VCM (merging on)",          RasVCM( 16, true ),    0 },
		{ "pixelpel [DirectLighting]", RasPixelDirect( 16 ),  0 },
		{ "PT spectral hwss",          RasPTSpectral( 16, true ), 0 },
	};
	for( const Ras& r : rs ) {
		const Stat b = RenderN( Header() + r.text + SceneBody( false, false, false ), "z5b", 1 );
		Print( ( std::string( r.name ) + " single-sided, back face" ).c_str(), b, 1.0 );
		Check( b.ok && b.mean == 0.0, std::string( "Z5 " ) + r.name + ": single-sided back face is exactly black" );
	}
	// The single-sided FACE-down twin: a nonzero render whose hash is the
	// bit-identity A/B between builds (print only -- a hash cannot be
	// gated inside one build).
	for( const Ras& r : rs ) {
		const Stat f = RenderN( Header() + r.text + SceneBody( true, false, false ), "z5f", 1 );
		Print( ( std::string( r.name ) + " single-sided, face-down (hash A/B)" ).c_str(), f, 1.0 );
		Check( f.ok && f.mean > 0, std::string( "Z5 " ) + r.name + ": single-sided face-down renders" );
	}
}

int main( int argc, char** argv )
{
	std::string only;
	for( int i = 1; i < argc; i++ ) {
		if( std::strcmp( argv[i], "--only" ) == 0 && i + 1 < argc ) { only = argv[++i]; }
		else if( std::strcmp( argv[i], "--repeats" ) == 0 && i + 1 < argc ) { g_repeats = std::atoi( argv[++i] ); }
		else { g_seedBase = unsigned( std::strtoul( argv[i], nullptr, 10 ) ); }
	}
	std::cout << "=== DoubleSidedEmitterTest (DL-320), seed base " << g_seedBase << ", repeats " << g_repeats << " ===" << std::endl;

	if( only.empty() || only == "cf" ) TestClosedFormCrossCheck();
	if( only.empty() || only == "Z1" ) TestZ1();
	if( only.empty() || only == "Z2" ) TestZ2();
	if( only.empty() || only == "Z3" ) {
		TestBackOverFace( "Z3 spectral", {
			{ "PT spectral hero",  RasPTSpectral( 64, false ),   0.03 },
			{ "PT spectral hwss",  RasPTSpectral( 64, true ),    0.03 },
			{ "BDPT spectral hwss", RasBDPTSpectral( 32, true ), 0.03 },
		} );
	}
	if( only.empty() || only == "Z4" ) {
		TestBackOverFace( "Z4 light-subpath / photon emission", {
			{ "VCM (merging on)",   RasVCM( 64, true ),     0.03 },
			{ "global photon map",  RasPhotonGlobal( 4 ),   0.05 },
		} );
	}
	if( only.empty() || only == "Z5" ) TestZ5();

	std::cout << "\nPassed: " << passCount << "\nFailed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
