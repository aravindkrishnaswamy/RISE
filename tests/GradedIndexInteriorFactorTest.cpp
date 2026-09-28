//////////////////////////////////////////////////////////////////////
//
//  GradedIndexInteriorFactorTest.cpp - DL-09: the interior-segment
//    basic-radiance factor inside a graded-index (position-dependent
//    `ior`) medium.  Derivation and every closed form used below:
//    docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md.
//
//    THE INVARIANT (reference-free).  A graded medium and a stack of
//    nested CONSTANT-index boxes approximating it must render the same
//    image as the box count grows.  Every nested-box boundary is a real
//    interface, where RISE's per-crossing (eta_before/eta_after)^2 factor
//    is already correct by construction (debt 30), so the stack converges
//    to the continuum physics; the graded object has no interface inside
//    it, so it is right only if the walk pays `(n_start/n_end)^2` along
//    every segment inside it.  The fixture is chosen so ray BENDING, which
//    a straight-ray tracer cannot model, changes nothing: an orthographic
//    camera looks straight down the gradient, and the floor is lit by an
//    emitter plane covering its hemisphere with the index rising
//    monotonically from floor to emitter (derivation doc section 5).
//
//    ROWS.
//      A  camera OUTSIDE, gather at an interior floor
//         (scenes/Tests/Materials/graded_index_interior_gather.RISEscene):
//         PT graded vs nested-box references K = 4,7,10,16 and the closed
//         form T_A*rho*L_e*cov/n_E^2; BDPT and VCM graded vs the closed form.
//         Pre-DL-09: every graded render reads (n_E/n_A)^2 = 2.25x.
//      B  camera INSIDE the graded medium at z = 1/3 (seeded walk)
//         (scenes/Tests/Materials/graded_index_seeded_inside.RISEscene):
//         same comparisons, closed form rho*L_e*cov*(n_S/n_E)^2.
//         Pre-DL-09: 1.65x (no interior factor, and the seed recorded the
//         probe's first-hit index 1.2 instead of n(camera) = 1.4).
//      C  camera outside, SMALL emitter: NEE and BDPT/VCM s=1 carry the
//         weight, so this row gates the CONNECTION-segment factor, against
//         the straight-ray model's own closed form (point-to-rectangle form
//         factor) -- a model-consistency row, not a physics reference.
//      D  PINHOLE camera inside, small emitter, extra diffuse sphere: PT vs
//         BDPT vs VCM.  Pins the IMPORTANCE-walk and splat/connection
//         factors (derivation doc section 3 (iv)).  Not red before DL-09 --
//         every integrator was consistently wrong then -- it guards the
//         fix's COMPLETENESS (measured: dropping the importance factor reads
//         VCM/PT 1.19; dropping the BDPT connection factor BDPT/PT 2.08).
//      E  IORStackSeeding::SeedFromPoint at a point inside the graded box
//         records n(point), not the probe's boundary hit; and a seed inside
//         6 of 17 nested boxes is not truncated by the probe table.
//      F  uniform-ior control on scene A's geometry: its own closed form.
//      G  the spectral pipes (PT NM hero, PT HWSS, BDPT HWSS; num_wavelengths
//         160): scene A vs its closed form, and scene D PT-vs-BDPT.
//
//    Replicas: every render is split into 16 tiles; where the view is a
//    uniformly lit floor (rows A-C, F, G scene A) the tiles are independent
//    replicas (disjoint per-pixel Sobol seeds) and `sd` is their spread.
//    Row D's pinhole image is not uniform: its tile `sd` is spatial spread,
//    and the row compares integrators on the SAME pixels instead.
//    Each render also prints a hash of its pixel buffer, for bit-identity
//    A/Bs of the constant-index rows between builds.
//
//    Seeding: every render reseeds libc rand() from argv[1] (default 1000)
//    plus a running index (docs: RISE renders are seeded from libc rand()).
//
//  Author: Claude (debt-dl09 slice, DL-09)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
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
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static unsigned int g_seedBase = 1000u;
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
// Capture scaffolding (same shape as RadianceEtaScaleGradedIndexTest).
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

//! Per-TILE grey means of one render (kTiles x kTiles equal tiles), or an
//! empty vector when the render failed or produced a nonfinite pixel.
//!
//! WHY TILES, NOT REPEATS.  RISE's Sobol path sampler is seeded per pixel
//! from the pixel coordinates alone, so re-rendering the same scene gives a
//! bit-identical image -- repeats would report sd == 0.  Every fixture here
//! looks at a UNIFORMLY lit Lambertian floor, so every pixel has the same
//! expected value and disjoint tiles are independent replicas (disjoint
//! pixel seeds).  The sd quoted is the across-tile sample sd.
static const int kTiles = 4;
static std::vector<double> TileMeansOf( const CapturingRasterizerOutput& cap, int w, int h )
{
	std::vector<double> out;
	if( cap.pixels.size() != std::size_t( w ) * std::size_t( h ) ) return out;
	const int tw = w / kTiles, th = h / kTiles;
	for( int ty = 0; ty < kTiles; ty++ ) {
		for( int tx = 0; tx < kTiles; tx++ ) {
			double sum = 0;
			for( int y = ty * th; y < ( ty + 1 ) * th; y++ ) {
				for( int x = tx * tw; x < ( tx + 1 ) * tw; x++ ) {
					const RISEColor& c = cap.pixels[ std::size_t( y ) * w + x ];
					const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
					if( !std::isfinite( v ) ) return std::vector<double>();
					sum += v;
				}
			}
			out.push_back( sum / double( tw * th ) );
		}
	}
	return out;
}

static std::string WriteTemp( const std::string& text, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/graded_index_interior_%s_%d.RISEscene", tag, (int)::getpid() );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << text;
	return std::string( path );
}

static const int kFilm = 32;

//! FNV-1a over the captured pixel buffer's raw doubles.  Printed with every
//! render so a constant-index row can be compared BIT FOR BIT between a
//! pre-DL-09 and a post-DL-09 build: these fixtures render deterministically
//! (uniformly lit floor, per-pixel-seeded Sobol, pinned libc seed).
static unsigned long long g_lastHash = 0;
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

static std::vector<double> RenderTiles( const std::string& sceneText, const char* tag )
{
	std::vector<double> tiles;
	const std::string path = WriteTemp( sceneText, tag );
	if( path.empty() ) return tiles;
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path.c_str() ); return tiles; }
	if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );
		std::srand( g_seedBase + g_renderIndex++ );
		if( pJob->Rasterize() ) {
			tiles = TileMeansOf( *pCap, kFilm, kFilm );
			g_lastHash = HashPixels( *pCap );
		}
		safe_release( pCap );
	}
	safe_release( pJob );
	std::remove( path.c_str() );
	return tiles;
}

struct Stat { double mean; double sd; int n; bool ok; };

//! One render; mean and across-tile sd over its kTiles^2 tiles.
static Stat RenderStat( const std::string& sceneText, const char* tag )
{
	Stat s{ 0, 0, 0, false };
	const std::vector<double> v = RenderTiles( sceneText, tag );
	if( v.empty() ) return s;
	s.ok = true;
	s.n = (int)v.size();
	for( double x : v ) s.mean += x;
	s.mean /= double( v.size() );
	double ss = 0;
	for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
	s.sd = std::sqrt( ss / double( v.size() - 1 ) );
	return s;
}

//////////////////////////////////////////////////////////////////////
// Scene text plumbing
//////////////////////////////////////////////////////////////////////
static std::string ReadFile( const std::string& path )
{
	std::ifstream f( path.c_str() );
	if( !f.is_open() ) return std::string();
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

//! Replace the text strictly between the `# BEGIN <tag>` and `# END <tag>`
//! marker lines (both kept).  Returns an empty string if a marker is missing.
static std::string ReplaceSpan( const std::string& text, const std::string& tag, const std::string& body )
{
	const std::string b = "# BEGIN " + tag + "\n";
	const std::string e = "# END " + tag + "\n";
	const std::size_t ib = text.find( b );
	const std::size_t ie = text.find( e );
	if( ib == std::string::npos || ie == std::string::npos || ie < ib ) return std::string();
	return text.substr( 0, ib + b.size() ) + body + text.substr( ie );
}

static std::string ShaderBlock()
{
	return "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
}

static std::string RasterizerPT( int spp )
{
	return ShaderBlock() + "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( spp ) +
		"\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static std::string RasterizerBDPT( int spp )
{
	return ShaderBlock() + "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " +
		std::to_string( spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static std::string RasterizerVCM( int spp )
{
	return ShaderBlock() + "vcm_pel_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " +
		std::to_string( spp ) + "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n"
		"\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

//! Spectral twins.  `num_wavelengths 160` on every spectral rasterizer
//! (the tree's convention for hero-vs-bundle comparisons).  The graded
//! field is a single scalar, so every lane pays the same factor.
static std::string RasterizerPTSpectral( int spp, bool hwss )
{
	return ShaderBlock() + "pathtracing_spectral_rasterizer\n{\n\tsamples " + std::to_string( spp ) +
		"\n\thwss " + ( hwss ? "TRUE" : "FALSE" ) + "\n\tnum_wavelengths 160\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static std::string RasterizerBDPTSpectral( int spp, bool hwss )
{
	return ShaderBlock() + "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 8\n\tmax_light_depth 8\n\tsamples " +
		std::to_string( spp ) + "\n\thwss " + ( hwss ? "TRUE" : "FALSE" ) +
		"\n\tnum_wavelengths 160\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

//! The tent profile n(z) = 1.8 - 0.6*|z - 1| on z in [0, 2].
static double TentN( double z ) { return 1.8 - 0.6 * std::fabs( z - 1.0 ); }

//! K+1 nested constant-index boxes approximating the tent: box k spans
//! z in [k*d, 2 - k*d] with d = 1/(K + 0.5), so the innermost box is the
//! band [1 - d/2, 1 + d/2] around the mid-plane.  Box k's index is the tent
//! at the centre of its own lower band, (k + 0.5)*d, which makes the
//! innermost box exactly 1.8 (the emitter's index).  Laterally each box is
//! 0.5 narrower than its parent so no two faces coincide.
static std::string NestedBoxes( int K )
{
	const double d = 1.0 / ( double( K ) + 0.5 );
	std::string s;
	char buf[1024];
	for( int k = 0; k <= K; k++ ) {
		const double n = TentN( ( double( k ) + 0.5 ) * d );
		const double depth = 2.0 - 2.0 * double( k ) * d;
		const double w = 60.0 - 0.5 * double( k );
		std::snprintf( buf, sizeof(buf),
			"dielectric_material\n{\n\tname mat_nb%d\n\tior %.9g\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"box_geometry\n{\n\tname geo_nb%d\n\twidth %.9g\n\theight %.9g\n\tdepth %.9g\n}\n\n"
			"standard_object\n{\n\tname nb%d\n\tgeometry geo_nb%d\n\tmaterial mat_nb%d\n\tposition 0 0 1\n}\n\n",
			k, n, k, w, w, depth, k, k, k );
		s += buf;
	}
	return s;
}

static std::string UniformBox( double ior )
{
	char buf[512];
	std::snprintf( buf, sizeof(buf),
		"dielectric_material\n{\n\tname mat_u\n\tior %.9g\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_u\n\twidth 60\n\theight 60\n\tdepth 2\n}\n\n"
		"standard_object\n{\n\tname u\n\tgeometry geo_u\n\tmaterial mat_u\n\tposition 0 0 1\n}\n\n", ior );
	return std::string( buf );
}

static double R0( double n ) { const double r = ( n - 1.0 ) / ( n + 1.0 ); return r * r; }

//! Fraction of the cosine-weighted upward hemisphere at the floor centre
//! that the 56x56 emitter plane 0.98 above it covers (straight rays):
//! 1 - cos^2(atan(28/0.98)).  Curved rays cover all of it (n rises toward
//! the emitter); the two differ by the 0.12% this leaves out.
static double Coverage()
{
	const double h = 0.98, r = 28.0;
	const double c = h / std::sqrt( h * h + r * r );
	return 1.0 - c * c;
}

static const double kPi = 3.14159265358979323846;
static const double kRho = 0.5;
static const double kLe = 1.0 / kPi;

static void Report( const char* label, const Stat& s, double closed )
{
	std::printf( "    %-34s mean=%.6f sd=%.6f n=%d  ratio-to-closed-form=%.4f  pixels=%016llx\n",
		label, s.mean, s.sd, s.n, s.ok ? s.mean / closed : -1.0, g_lastHash );
}

//////////////////////////////////////////////////////////////////////
// Row A / B common driver
//////////////////////////////////////////////////////////////////////
static void RunGatherRows( const char* rowName, const std::string& sceneFile, double closed, double preFixValue )
{
	std::cout << std::endl << "-- Row " << rowName << ": " << sceneFile << " --" << std::endl;
	const std::string base = ReadFile( sceneFile );
	Check( !base.empty(), std::string( rowName ) + ": fixture scene readable (run from the repo root)" );
	if( base.empty() ) return;
	std::printf( "    closed form (after DL-09) = %.6f   pre-DL-09 value = %.6f (x%.4f)\n",
		closed, preFixValue, preFixValue / closed );

	// PT graded.
	const std::string gradedPT = ReplaceSpan( base, "RASTERIZER", RasterizerPT( 64 ) );
	const Stat g = RenderStat( gradedPT, "graded_pt" );
	Report( "PT graded", g, closed );
	Check( g.ok, std::string( rowName ) + ": PT graded renders produced output" );

	// Nested-box references: the convergence table.  K = 3k+1 so the
	// seeded camera's z = 1/3 is a band CENTRE for every K (NestedBoxes).
	const int Ks[] = { 4, 7, 10, 16 };
	Stat ref16{ 0, 0, 0, false };
	for( int K : Ks ) {
		const std::string refText = ReplaceSpan( gradedPT, "MEDIUM", NestedBoxes( K ) );
		Check( !refText.empty(), std::string( rowName ) + ": reference scene built" );
		const Stat r = RenderStat( refText, "ref" );
		char lbl[64];
		std::snprintf( lbl, sizeof(lbl), "PT nested boxes K=%d", K );
		Report( lbl, r, closed );
		if( K == 16 ) ref16 = r;
	}
	Check( ref16.ok, std::string( rowName ) + ": K=16 reference renders produced output" );

	// The references are physics by construction (debt-30 interface
	// factors): K=16 must already sit on the closed form.  This is the
	// check that the FIXTURE is bending-free and the closed form right,
	// and it is green before and after DL-09.
	Check( ref16.ok && std::fabs( ref16.mean / closed - 1.0 ) < 0.03,
		std::string( rowName ) + ": K=16 nested-box reference == closed form within 3%" );

	// THE INVARIANT: graded == piecewise-constant limit.
	Check( g.ok && ref16.ok && std::fabs( g.mean / ref16.mean - 1.0 ) < 0.03,
		std::string( rowName ) + ": PT graded == K=16 nested-box reference within 3%" );
	Check( g.ok && std::fabs( g.mean / closed - 1.0 ) < 0.03,
		std::string( rowName ) + ": PT graded == closed form within 3%" );
	Check( g.ok && std::fabs( g.mean / preFixValue - 1.0 ) > 0.20,
		std::string( rowName ) + ": PT graded is NOT the pre-DL-09 value" );

	// BDPT / VCM on the graded scene: the eye walk's factor AND the
	// connection factor at the interior vertex (NEE-like s=1).
	const Stat b = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerBDPT( 64 ) ), "graded_bdpt" );
	Report( "BDPT graded", b, closed );
	Check( b.ok && std::fabs( b.mean / closed - 1.0 ) < 0.03,
		std::string( rowName ) + ": BDPT graded == closed form within 3%" );
	const Stat v = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerVCM( 64 ) ), "graded_vcm" );
	Report( "VCM graded", v, closed );
	Check( v.ok && std::fabs( v.mean / closed - 1.0 ) < 0.05,
		std::string( rowName ) + ": VCM graded == closed form within 5%" );
}

//////////////////////////////////////////////////////////////////////
// Small-emitter variants (rows C and D).  With the fixtures' 56 x 56
// emitter the floor's BSDF-sampled continuation carries essentially the
// whole power-heuristic weight (the light's solid-angle density is
// ~d^2/A, three orders below the cosine lobe's), so NEE and every
// connection strategy are near-silent there.  A small emitter hands the
// weight to the NEE / connection strategies, which is where the
// CONNECTION-segment factor (n_C/n_E)^2 and the light-walk factor live.
//////////////////////////////////////////////////////////////////////
static const double kSmallHalf = 0.25;		// emitter half-width

static std::string WithSmallEmitter( const std::string& text )
{
	const std::string big =
		"\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n";
	char buf[256];
	std::snprintf( buf, sizeof(buf),
		"\tpta %g %g 1.0\n\tptb %g %g 1.0\n\tptc %g %g 1.0\n\tptd %g %g 1.0\n",
		-kSmallHalf, -kSmallHalf, -kSmallHalf, kSmallHalf, kSmallHalf, kSmallHalf, kSmallHalf, -kSmallHalf );
	const std::size_t at = text.find( big );
	if( at == std::string::npos ) return std::string();
	std::string out = text;
	out.replace( at, big.size(), buf );
	return out;
}

//! Point-to-parallel-rectangle form factor for a differential area directly
//! below the CORNER of an a x b rectangle at height h (Howell catalogue
//! B-2); the centred square is four such corners.
static double CornerFormFactor( double a, double b, double h )
{
	const double A = a / h, B = b / h;
	const double sa = std::sqrt( 1.0 + A * A ), sb = std::sqrt( 1.0 + B * B );
	return ( A / sa * std::atan( B / sa ) + B / sb * std::atan( A / sb ) ) / ( 2.0 * kPi );
}

//////////////////////////////////////////////////////////////////////
// Row C: camera OUTSIDE (ortho, tiny viewport), SMALL emitter.  NEE and
// the BDPT/VCM s=1 connection now carry the weight, so this row gates the
// connection-segment factor (derivation doc section 3 (ii)).  The closed
// form is the STRAIGHT-RAY model's own (point-to-rectangle form factor at
// straight-line geometry): a small emitter is exactly where ray bending
// would change the solid angle (derivation doc section 5), so this row is
// a model-consistency check, not a physics reference -- the reference-free
// invariant is rows A and B.
//////////////////////////////////////////////////////////////////////
static void RunSmallEmitterOutsideRow()
{
	std::cout << std::endl << "-- Row C: camera outside, small emitter (NEE / s=1 connection carry the weight) --" << std::endl;
	std::string base = WithSmallEmitter( ReadFile( "scenes/Tests/Materials/graded_index_interior_gather.RISEscene" ) );
	Check( !base.empty(), "C: small-emitter fixture built" );
	if( base.empty() ) return;
	const std::string vp = "\tviewport_scale 0.2 0.2\n";
	const std::size_t at = base.find( vp );
	Check( at != std::string::npos, "C: viewport found" );
	if( at == std::string::npos ) return;
	base.replace( at, vp.size(), "\tviewport_scale 0.01 0.01\n" );

	const double h = 1.0 - 0.02;
	const double F = 4.0 * CornerFormFactor( kSmallHalf, kSmallHalf, h );
	const double nA = 1.2, nE = 1.8;
	const double closed = ( 1.0 - R0( nA ) ) * kRho * kLe * F / ( nE * nE );
	const double pre    = ( 1.0 - R0( nA ) ) * kRho * kLe * F / ( nA * nA );
	std::printf( "    form factor F = %.6f   closed form = %.7f   pre-DL-09 value = %.7f (x%.4f)\n", F, closed, pre, pre / closed );

	struct R { const char* name; std::string ras; double tol; };
	const R rs[] = {
		{ "PT",   RasterizerPT( 256 ),   0.03 },
		{ "BDPT", RasterizerBDPT( 128 ), 0.03 },
		{ "VCM",  RasterizerVCM( 128 ),  0.05 },
	};
	for( const R& r : rs ) {
		const Stat st = RenderStat( ReplaceSpan( base, "RASTERIZER", r.ras ), "small_out" );
		char lbl[64];
		std::snprintf( lbl, sizeof(lbl), "%s graded, small emitter", r.name );
		Report( lbl, st, closed );
		Check( st.ok && std::fabs( st.mean / closed - 1.0 ) < r.tol,
			std::string( "C: " ) + r.name + " graded small-emitter gather == straight-ray closed form" );
	}
}

//////////////////////////////////////////////////////////////////////
// Row D: PINHOLE camera INSIDE the graded medium, small emitter, plus a
// diffuse sphere so interior light-subpath vertices exist.  PT vs BDPT vs
// VCM.  BDPT's light-tracing splats (t=1) and s>=2 connections reach this
// camera, so this row pins the IMPORTANCE-walk and connection factors
// (derivation doc section 3 (iv)).  Not red before DL-09 -- every
// integrator was consistently wrong then -- it guards the fix's
// COMPLETENESS: an eye-walk factor without the light-walk/connection
// factors makes BDPT/VCM disagree with PT.
//////////////////////////////////////////////////////////////////////
static void RunPinholeConsistencyRow()
{
	std::cout << std::endl << "-- Row D: pinhole camera inside the graded medium, small emitter, PT vs BDPT vs VCM --" << std::endl;
	std::string base = WithSmallEmitter( ReadFile( "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene" ) );
	Check( !base.empty(), "D: fixture built" );
	if( base.empty() ) return;
	const std::string ortho = "orthographic_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 0.2 0.2\n}\n";
	const std::string pin = "pinhole_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60\n}\n";
	const std::size_t at = base.find( ortho );
	Check( at != std::string::npos, "D: fixture camera block found" );
	if( at == std::string::npos ) return;
	base.replace( at, ortho.size(), pin );
	base += "\nsphere_geometry\n{\n\tname geo_ball\n\tradius 0.08\n}\n\n"
		"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tmaterial mat_floor\n\tposition 0.06 0.03 0.12\n}\n";

	const Stat p = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerPT( 256 ) ), "pin_pt" );
	const Stat b = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerBDPT( 128 ) ), "pin_bdpt" );
	const Stat v = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerVCM( 128 ) ), "pin_vcm" );
	std::printf( "    PT   mean=%.6f sd=%.6f n=%d\n    BDPT mean=%.6f sd=%.6f n=%d  BDPT/PT=%.4f\n    VCM  mean=%.6f sd=%.6f n=%d  VCM/PT=%.4f\n",
		p.mean, p.sd, p.n, b.mean, b.sd, b.n, b.mean / p.mean, v.mean, v.sd, v.n, v.mean / p.mean );
	Check( p.ok && b.ok && std::fabs( b.mean / p.mean - 1.0 ) < 0.03, "D: BDPT == PT within 3% (pinhole inside graded medium)" );
	Check( p.ok && v.ok && std::fabs( v.mean / p.mean - 1.0 ) < 0.05, "D: VCM == PT within 5% (pinhole inside graded medium)" );
}

//////////////////////////////////////////////////////////////////////
// Row G: the spectral pipes (NM hero walk, HWSS bundle, BDPT spectral),
// which carry the factor through separate code (PT's HWSS body multiplies
// every lane; BDPT's generators scale hwssBetaNM).  Scene A's large
// emitter (bending-free) against the SAME closed form; scene D's pinhole
// inside the medium with the small emitter for spectral PT-vs-BDPT
// agreement (light-tracing splats reach that camera).
//////////////////////////////////////////////////////////////////////
static void RunSpectralRow( const double closedA )
{
	std::cout << std::endl << "-- Row G: spectral (num_wavelengths 160) --" << std::endl;
	const std::string baseA = ReadFile( "scenes/Tests/Materials/graded_index_interior_gather.RISEscene" );
	Check( !baseA.empty(), "G: fixture readable" );
	if( baseA.empty() ) return;
	struct R { const char* name; std::string ras; };
	const R rs[] = {
		{ "PT spectral hwss FALSE",   RasterizerPTSpectral( 64, false ) },
		{ "PT spectral hwss TRUE",    RasterizerPTSpectral( 32, true ) },
		{ "BDPT spectral hwss TRUE",  RasterizerBDPTSpectral( 32, true ) },
	};
	for( const R& r : rs ) {
		const Stat st = RenderStat( ReplaceSpan( baseA, "RASTERIZER", r.ras ), "spec_a" );
		Report( r.name, st, closedA );
		Check( st.ok && std::fabs( st.mean / closedA - 1.0 ) < 0.03,
			std::string( "G: " ) + r.name + " graded (scene A) == closed form within 3%" );
	}

	std::string baseD = WithSmallEmitter( ReadFile( "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene" ) );
	const std::string ortho = "orthographic_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 0.2 0.2\n}\n";
	const std::size_t at = baseD.find( ortho );
	Check( at != std::string::npos, "G: fixture camera block found" );
	if( at == std::string::npos ) return;
	baseD.replace( at, ortho.size(), "pinhole_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60\n}\n" );
	const Stat p = RenderStat( ReplaceSpan( baseD, "RASTERIZER", RasterizerPTSpectral( 64, true ) ), "spec_d_pt" );
	const Stat b = RenderStat( ReplaceSpan( baseD, "RASTERIZER", RasterizerBDPTSpectral( 32, true ) ), "spec_d_bdpt" );
	std::printf( "    pinhole inside, small emitter: PT spectral hwss mean=%.6f  BDPT spectral hwss mean=%.6f  BDPT/PT=%.4f\n",
		p.mean, b.mean, b.mean / p.mean );
	Check( p.ok && b.ok && std::fabs( b.mean / p.mean - 1.0 ) < 0.04,
		"G: BDPT spectral hwss == PT spectral hwss within 4% (pinhole inside graded medium)" );
}

//////////////////////////////////////////////////////////////////////
// Row H: row D's scene with a scattering medium filling the graded box.
// Medium vertices do not Advance -- the next surface vertex's Advance
// telescopes over them -- but the NEE and connections made FROM a medium
// vertex still price their segment from the tracked index (PT volume NEE
// via MediumTransport, BDPT/VCM via the vertex's recorded index).  PT vs
// BDPT vs VCM; an unpriced volume-NEE segment makes PT's own MIS partition
// inconsistent and moves it off the bidirectional estimators.
//////////////////////////////////////////////////////////////////////
static void RunMediumRow()
{
	std::cout << std::endl << "-- Row H: scattering medium inside the graded box, pinhole inside, PT vs BDPT vs VCM --" << std::endl;
	std::string base = WithSmallEmitter( ReadFile( "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene" ) );
	Check( !base.empty(), "H: fixture built" );
	if( base.empty() ) return;
	const std::string ortho = "orthographic_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 0.2 0.2\n}\n";
	const std::size_t at = base.find( ortho );
	Check( at != std::string::npos, "H: fixture camera block found" );
	if( at == std::string::npos ) return;
	base.replace( at, ortho.size(), "pinhole_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60\n}\n" );
	const std::string obj = "\tmaterial mat_graded\n\tposition 0 0 1\n}\n";
	const std::size_t ao = base.find( obj );
	Check( ao != std::string::npos, "H: graded object block found" );
	if( ao == std::string::npos ) return;
	base.replace( ao, obj.size(), "\tmaterial mat_graded\n\tposition 0 0 1\n\tinterior_medium fog\n}\n" );
	// The medium chunk must precede its use: insert it right after the
	// scene-file header line.
	base.insert( base.find( '\n' ) + 1,
		"\nhomogeneous_medium\n{\n\tname fog\n\tabsorption 0.05 0.05 0.05\n\tscattering 0.6 0.6 0.6\n\tphase isotropic\n}\n" );

	const Stat p = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerPT( 256 ) ), "med_pt" );
	const Stat b = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerBDPT( 128 ) ), "med_bdpt" );
	const Stat v = RenderStat( ReplaceSpan( base, "RASTERIZER", RasterizerVCM( 128 ) ), "med_vcm" );
	std::printf( "    PT   mean=%.6f\n    BDPT mean=%.6f  BDPT/PT=%.4f\n    VCM  mean=%.6f  VCM/PT=%.4f\n",
		p.mean, b.mean, b.mean / p.mean, v.mean, v.mean / p.mean );
	Check( p.ok && b.ok && p.mean > 0 && std::fabs( b.mean / p.mean - 1.0 ) < 0.04, "H: BDPT == PT within 4% (medium inside graded box)" );
	Check( p.ok && v.ok && p.mean > 0 && std::fabs( v.mean / p.mean - 1.0 ) < 0.06, "H: VCM == PT within 6% (medium inside graded box)" );
}

//////////////////////////////////////////////////////////////////////
// Row F: uniform-ior control on row A's geometry.
//////////////////////////////////////////////////////////////////////
static void RunUniformControlRow()
{
	std::cout << std::endl << "-- Row F: uniform ior 1.5 control (row A geometry) --" << std::endl;
	const std::string base = ReadFile( "scenes/Tests/Materials/graded_index_interior_gather.RISEscene" );
	Check( !base.empty(), "F: fixture readable" );
	if( base.empty() ) return;
	const double closed = ( 1.0 - R0( 1.5 ) ) * kRho * kLe * Coverage() / ( 1.5 * 1.5 );
	const std::string text = ReplaceSpan( ReplaceSpan( base, "RASTERIZER", RasterizerPT( 64 ) ), "MEDIUM", UniformBox( 1.5 ) );
	const Stat s = RenderStat( text, "uniform" );
	Report( "PT uniform 1.5", s, closed );
	Check( s.ok && std::fabs( s.mean / closed - 1.0 ) < 0.03, "F: uniform-ior control == T*rho*L_e*cov/n^2 within 3%" );
}

//////////////////////////////////////////////////////////////////////
// Row E: SeedFromPoint records the index AT the seed point.
//////////////////////////////////////////////////////////////////////
static void RunSeedRow()
{
	std::cout << std::endl << "-- Row E: IORStackSeeding::SeedFromPoint inside the graded box --" << std::endl;
	const std::string path = "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene";
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { Check( false, "E: job created" ); return; }
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) { Check( false, "E: fixture loaded" ); safe_release( pJob ); return; }
	IScenePriv* pScene = pJob->GetScene();
	pScene->GetObjects()->PrepareForRendering();

	const double zs[] = { 0.3, 0.5, 1.4 };
	for( double z : zs ) {
		IORStack stack( 1.0 );
		IORStackSeeding::SeedFromPoint( stack, Point3( 0.1, -0.2, z ), *pScene );
		const double want = TentN( z );
		std::printf( "    seed z=%.2f  top=%.6f  n(z)=%.6f\n", z, stack.top(), want );
		char lbl[160];
		std::snprintf( lbl, sizeof(lbl), "E: seed at z=%.2f records n(seed point) = %.3f (not the probe's boundary hit)", z, want );
		Check( std::fabs( stack.top() - want ) < 1e-9, lbl );
		Check( stack.topObject() != 0, "E: seed found the containing graded object" );
	}
	safe_release( pJob );

	// The nested-box REFERENCE of row B puts the seed inside 6 of 17 boxes
	// with 11 more above it on the +z probe.  TallyProbe used to record only
	// the FIRST 8 distinct trackable objects a probe met, so the
	// non-containing boxes in front used up the table and the containing
	// ones it only reaches afterwards were never seen: the seed came back as
	// bare air (1.0) and the K=16 reference rendered at 0.39x its closed
	// form.  Containment depth is 6 here -- it is the PROBE's object count
	// that overflowed, not the nesting.
	{
		const int K = 16;
		const double d = 1.0 / ( double( K ) + 0.5 );
		const std::string refText = ReplaceSpan( ReadFile( path ), "MEDIUM", NestedBoxes( K ) );
		const std::string p = WriteTemp( refText, "seedref" );
		IJobPriv* pRef = nullptr;
		if( !p.empty() && RISE_CreateJobPriv( &pRef ) && pRef && pRef->LoadAsciiSceneViaCst( p.c_str() ) ) {
			IScenePriv* pRS = pRef->GetScene();
			pRS->GetObjects()->PrepareForRendering();
			IORStack stack( 1.0 );
			IORStackSeeding::SeedFromPoint( stack, Point3( 0.1, -0.2, 1.0 / 3.0 ), *pRS );
			// Innermost box containing z = 1/3 is k = 5 (5.5*d == 1/3).
			const double want = TentN( ( 5.0 + 0.5 ) * d );
			std::printf( "    seed inside K=16 nested boxes z=1/3  top=%.9f  want box-5 ior %.9f\n", stack.top(), want );
			// The scene text prints each box's ior at %.9g.
			Check( std::fabs( stack.top() - want ) < 1e-7,
				"E: seed inside 6 of 17 nested constant boxes records the innermost box's ior (probe object count > 8)" );
		} else {
			Check( false, "E: nested-box seeding fixture loaded" );
		}
		safe_release( pRef );
		if( !p.empty() ) std::remove( p.c_str() );
	}
}

int main( int argc, char** argv )
{
	if( argc > 1 && argv[1] ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}
	std::cout << "=== GradedIndexInteriorFactorTest (DL-09) ===  seed base " << g_seedBase << std::endl;

	const double cov = Coverage();
	const double nA = 1.2, nE = 1.8, nS = TentN( 1.0 / 3.0 );
	// Row A: T_A*rho*L_e*cov/n_E^2; pre-DL-09 charged n_A instead of n_E.
	const double closedA = ( 1.0 - R0( nA ) ) * kRho * kLe * cov / ( nE * nE );
	const double preA    = ( 1.0 - R0( nA ) ) * kRho * kLe * cov / ( nA * nA );
	// Row B: rho*L_e*cov*(n_S/n_E)^2; pre-DL-09 carried no factor at all.
	const double closedB = kRho * kLe * cov * ( nS / nE ) * ( nS / nE );
	const double preB    = kRho * kLe * cov;

	RunSeedRow();
	RunGatherRows( "A", "scenes/Tests/Materials/graded_index_interior_gather.RISEscene", closedA, preA );
	RunGatherRows( "B", "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene", closedB, preB );
	RunSmallEmitterOutsideRow();
	RunPinholeConsistencyRow();
	RunSpectralRow( closedA );
	RunMediumRow();
	RunUniformControlRow();

	std::cout << std::endl << "Passed: " << passCount << std::endl << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
