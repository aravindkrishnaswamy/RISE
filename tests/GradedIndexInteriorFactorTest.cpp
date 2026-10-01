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
//      I-O  DL-292 (debt-dl292 slice) coverage rows -- see the block
//         comment above RunSSSEntryNEERow: PT's SSS entry NEE (I), the
//         NEE shadow segment's factor keyed on boundary crossings, not on
//         the transmittance value (J weave gap, K nested slab, L exit into
//         an index-matched enclosure), RayCaster's own volume walk (M),
//         the legacy shader-op chain (N), composite / refused ior forms (O).
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
#include <cstring>
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

//////////////////////////////////////////////////////////////////////
// DL-292 rows (debt-dl292 slice).  The coverage gaps DL-09 left open:
// walks that do not Advance, and an NEE shadow segment whose graded
// factor was keyed on the transmittance value instead of on a boundary
// crossing.  Every row is REFERENCE-FREE -- it compares two renders that
// must agree (a cross-integrator pair, a with/without-occluder ratio
// against the same ratio in a constant-index control, or a boundary
// against the one-object scene with the same index field) -- and prints
// its pre-DL-292 prediction beside the measurement.
//////////////////////////////////////////////////////////////////////
static const char* kSeededScene = "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene";
static const std::string kOrthoInside =
	"orthographic_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 0.2 0.2\n}\n";
static const std::string kPinholeInside =
	"pinhole_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 60\n}\n";
static const std::string kBigFloorCorners =
	"\tpta -28 -28 0.02\n\tptb 28 -28 0.02\n\tptc 28 28 0.02\n\tptd -28 28 0.02\n";
static const std::string kEmitterObject =
	"standard_object\n{\n\tname emitter\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n";

//! Replace the first occurrence of `from` by `to`; empty string if absent
//! (every caller Checks the result, so a fixture drift fails loudly).
static std::string ReplaceOnce( const std::string& text, const std::string& from, const std::string& to )
{
	const std::size_t at = text.find( from );
	if( text.empty() || at == std::string::npos ) return std::string();
	std::string out = text;
	out.replace( at, from.size(), to );
	return out;
}

//! Insert chunk text right after the scene header line (so a medium /
//! painter chunk precedes every use).
static std::string AfterHeader( const std::string& text, const std::string& chunk )
{
	if( text.empty() ) return std::string();
	std::string out = text;
	out.insert( out.find( '\n' ) + 1, "\n" + chunk + "\n" );
	return out;
}

static std::string PTRasterizerOpts( int spp, const std::string& extra )
{
	return ShaderBlock() + "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( spp ) +
		"\n\toidn_denoise FALSE\n\tpixel_filter box\n" + extra + "}\n";
}

//////////////////////////////////////////////////////////////////////
// Row I (DL-292 item 1): PT's BSSRDF / random-walk SSS ENTRY NEE inside a
// graded medium.  The entry-point NEE used to pass no graded stack while
// the SSS continuation (and BDPT's entry vertex) priced the medium: the
// NEE arm carried (n_S/n_exit)^2 instead of (n_S/n_E)^2.  A small emitter
// hands that arm most of the MIS weight.  PT vs BDPT on the graded box
// must agree as closely as on a constant-index control.
//////////////////////////////////////////////////////////////////////
static std::string SSSSlabScene( const std::string& matChunk, bool graded, const std::string& ras )
{
	std::string s = WithSmallEmitter( ReadFile( kSeededScene ) );
	if( !graded ) s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
	s = ReplaceSpan( s, "RASTERIZER", ras );
	// Drop the floor object (keep its material chunk for nothing) and put
	// an SSS slab z in [0.01, 0.11] under the camera.
	s = ReplaceOnce( s, "standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n",
		matChunk +
		"box_geometry\n{\n\tname geo_slab\n\twidth 1.2\n\theight 1.2\n\tdepth 0.1\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tmaterial mat_sss\n\tposition 0 0 0.06\n}\n" );
	return s;
}

static void RunSSSEntryNEERow()
{
	std::cout << std::endl << "-- Row I (DL-292 item 1): SSS entry NEE inside the graded box, PT vs BDPT --" << std::endl;
	const std::string rw =
		"randomwalk_sss_material\n{\n\tname mat_sss\n\tior 1.3\n\tabsorption 0.3 0.3 0.3\n\tscattering 20\n"
		"\tg 0.0\n\troughness 0.0\n\tmax_bounces 64\n}\n\n";
	const std::string diff =
		"subsurfacescattering_material\n{\n\tname mat_sss\n\tior 1.3\n\tabsorption 0.3 0.3 0.3\n\tscattering 20\n"
		"\tg 0.0\n\troughness 0.0\n}\n\n";
	struct V { const char* name; const std::string* mat; };
	const V vs[] = { { "random-walk SSS", &rw }, { "diffusion-profile SSS", &diff } };
	for( const V& v : vs ) {
		double ratio[2] = { 0, 0 };
		for( int graded = 1; graded >= 0; graded-- ) {
			const std::string pt = SSSSlabScene( *v.mat, graded != 0, RasterizerPT( 256 ) );
			const std::string bd = SSSSlabScene( *v.mat, graded != 0, RasterizerBDPT( 128 ) );
			Check( !pt.empty() && !bd.empty(), std::string( "I: " ) + v.name + " fixture built" );
			if( pt.empty() || bd.empty() ) return;
			const Stat p = RenderStat( pt, "sss_pt" );
			const Stat b = RenderStat( bd, "sss_bdpt" );
			ratio[graded] = ( p.ok && b.ok && p.mean > 0 ) ? b.mean / p.mean : -1;
			std::printf( "    %-22s %-9s PT mean=%.6f sd=%.6f  BDPT mean=%.6f sd=%.6f  BDPT/PT=%.4f\n",
				v.name, graded ? "graded" : "constant", p.mean, p.sd, b.mean, b.sd, ratio[graded] );
		}
		Check( ratio[0] > 0 && std::fabs( ratio[0] - 1.0 ) < 0.03,
			std::string( "I: " ) + v.name + ": constant-index control BDPT == PT within 3%" );
		Check( ratio[1] > 0 && std::fabs( ratio[1] - 1.0 ) < 0.03,
			std::string( "I: " ) + v.name + ": graded BDPT == PT within 3% (entry NEE carries the graded stack)" );
	}

	// The HWSS twin delegates SSS to the per-wavelength walk; one spectral
	// check on the random-walk variant keeps that delegation covered.
	const std::string pt = SSSSlabScene( rw, true, RasterizerPTSpectral( 128, true ) );
	const std::string bd = SSSSlabScene( rw, true, RasterizerBDPTSpectral( 64, true ) );
	const Stat p = RenderStat( pt, "sss_pt_hwss" );
	const Stat b = RenderStat( bd, "sss_bdpt_hwss" );
	std::printf( "    random-walk SSS graded, spectral hwss: PT mean=%.6f  BDPT mean=%.6f  BDPT/PT=%.4f\n",
		p.mean, b.mean, ( p.ok && p.mean > 0 ) ? b.mean / p.mean : -1.0 );
	Check( p.ok && b.ok && p.mean > 0 && std::fabs( b.mean / p.mean - 1.0 ) < 0.04,
		"I: random-walk SSS graded, spectral hwss: BDPT == PT within 4%" );
}

//////////////////////////////////////////////////////////////////////
// Rows J / K / L (DL-292 item 2): the NEE shadow segment's graded factor
// must follow the segment's BOUNDARY CROSSINGS, not the value of its
// transmittance.  Fixture: row B's seeded-inside camera looking down at a
// small floor patch (so no floor-to-floor interreflection) lit by an omni
// light at (2, 0, 1), n_L = 1.8, from the floor at n_C = n(0.02) = 1.212;
// the occluder is a vertical sheet / slab at x = 1, met by the shadow ray
// 26 deg off its normal (below the 41.8 deg at which the straight
// transparent-shadow walk TIRs out of a 1.5 slab) and by no camera ray.
//
//   J  a `transmission thin` weave between floor and light: its delta gap
//      returns t != 1 with NO boundary crossing.  The with/without-weave
//      ratio in the graded box must equal the same ratio in a
//      constant-index control (t, the weave's gap expectation at the hit,
//      is what the control measures).  Pre-fix the graded ratio read
//      t * (n_L/n_C)^2 = 2.2 t: the shadow ray's transmittance != 1 turned
//      the graded factor off.
//   K  the same with a thin NESTED constant-glass slab and
//      `transparent_shadows TRUE`: the ray enters and leaves the slab, the
//      light is still inside the graded medium.  Same ratio test; the
//      Fresnel transmittance cancels between graded and control.
//   L  the shadow ray EXITS the graded box into an enclosing constant box H
//      whose index 1.2 matches the graded box's face: the graded segment up
//      to the face must be priced.  Reference: ONE box whose index field
//      max(1.2, tent) is the same function of position -- no interface, so
//      an ordinary NEE.  Ratio must be the face's Fresnel transmittance
//      T = 1 - R0(1.2) (the transparent-shadow walk starts its own stack in
//      air); pre-fix it read T*(1.2/1.8)^2.
//////////////////////////////////////////////////////////////////////
static const double kShadowGap = 0.4;

static std::string ShadowFixture( bool graded, double constIor, const std::string& occluder,
	bool transparentShadows )
{
	std::string s = ReadFile( kSeededScene );
	if( !graded ) s = ReplaceSpan( s, "MEDIUM", UniformBox( constIor ) );
	s = ReplaceSpan( s, "RASTERIZER", PTRasterizerOpts( 64,
		transparentShadows ? "\ttransparent_shadows TRUE\n" : "" ) );
	s = ReplaceOnce( s, kBigFloorCorners,
		"\tpta -0.2 -0.2 0.02\n\tptb 0.2 -0.2 0.02\n\tptc 0.2 0.2 0.02\n\tptd -0.2 0.2 0.02\n" );
	s = ReplaceOnce( s, kEmitterObject,
		"omni_light\n{\n\tname lgt\n\tposition 2.0 0 1.0\n\tcolor 1 1 1\n\tpower 16\n}\n\n" + occluder );
	return s;
}

static std::string WeaveOccluder()
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n}\n\n"
		"weave_material\n{\n\tname mat_sheet\n\tfabric custom\n\ttransmission thin\n\tgap %g\n"
		"\twarp_color pnt_black\n\tweft_color pnt_black\n}\n\n"
		"clippedplane_geometry\n{\n\tname geo_sheet\n\tpta 1.0 -2 0.03\n\tptb 1.0 2 0.03\n"
		"\tptc 1.0 2 1.9\n\tptd 1.0 -2 1.9\n\tdoublesided TRUE\n}\n\n"
		"standard_object\n{\n\tname obj_sheet\n\tgeometry geo_sheet\n\tmaterial mat_sheet\n}\n\n", kShadowGap );
	return buf;
}

static std::string NestedSlabOccluder()
{
	return
		"dielectric_material\n{\n\tname mat_slab\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
		"box_geometry\n{\n\tname geo_slab\n\twidth 0.02\n\theight 4\n\tdepth 1.8\n}\n\n"
		"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tmaterial mat_slab\n\tposition 1.0 0 0.95\n}\n\n";
}

static void RunOccluderRatioRow( const char* row, const char* what, const std::string& occluder,
	bool transparentShadows, const double closedRatio )
{
	const double nC = TentN( 0.02 ), nL = TentN( 1.0 );
	double r[2] = { -1, -1 };
	for( int graded = 1; graded >= 0; graded-- ) {
		const std::string without = ShadowFixture( graded != 0, 1.3, "", transparentShadows );
		const std::string with = ShadowFixture( graded != 0, 1.3, occluder, transparentShadows );
		Check( !without.empty() && !with.empty(), std::string( row ) + ": fixture built" );
		if( without.empty() || with.empty() ) return;
		const Stat a = RenderStat( without, "occ_without" );
		const Stat b = RenderStat( with, "occ_with" );
		r[graded] = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
		std::printf( "    %-9s without mean=%.6f sd=%.6f   with %s mean=%.6f sd=%.6f   with/without=%.5f\n",
			graded ? "graded" : "constant", a.mean, a.sd, what, b.mean, b.sd, r[graded] );
	}
	std::printf( "    pre-DL-292 graded/control prediction x(n_L/n_C)^2 = %.4f; authored gap %.3f\n",
		( nL / nC ) * ( nL / nC ), closedRatio );
	// The control's ratio is the weave's gap expectation at the hit -- at
	// most the authored gap (the threads' projected width grows off the
	// sheet normal).  A sanity bound, not the gate.
	Check( r[0] > 0.05 && r[0] <= closedRatio * 1.02,
		std::string( row ) + ": constant-index control with/without ratio is a transmittance in (0.05, gap]" );
	Check( r[0] > 0 && r[1] > 0 && std::fabs( r[1] / r[0] - 1.0 ) < 0.02,
		std::string( row ) + ": graded with/without ratio == constant-control ratio within 2% (factor keyed on crossings, not on T)" );
}

static void RunShadowSkipRows()
{
	std::cout << std::endl << "-- Row J (DL-292 item 2, DL-05 merge): thin-weave gap between an interior floor and a delta light --" << std::endl;
	RunOccluderRatioRow( "J", "weave", WeaveOccluder(), false, kShadowGap );

	std::cout << std::endl << "-- Row K (DL-292 item 2): nested constant-glass slab, transparent shadows, light still inside the graded medium --" << std::endl;
	// The two Fresnel crossings of the slab.  The walk starts its OWN stack
	// in air (RayCaster::WalkShadowSegment), so each crossing reads 1 <-> 1.5
	// in both builds and in both the graded and the control scene; the
	// shadow direction from the floor centre to the light is 26.1 deg off
	// the slab normal.  The closed form is used for the report only -- the
	// gate is graded ratio == control ratio, which does not depend on it.
	{
		const double dx = 2.0, dz = 0.98;
		const double cosI = dx / std::sqrt( dx * dx + dz * dz );
		const double n = 1.5;
		const double sinT = std::sqrt( 1.0 - cosI * cosI ) / n;
		const double cosT = std::sqrt( 1.0 - sinT * sinT );
		const double rs = ( cosI - n * cosT ) / ( cosI + n * cosT );
		const double rp = ( n * cosI - cosT ) / ( n * cosI + cosT );
		const double T1 = 1.0 - 0.5 * ( rs * rs + rp * rp );
		std::cout << "    (per-crossing Fresnel T at the ROI centre: " << T1 << ", two crossings " << T1 * T1 << ")" << std::endl;
		double r[2] = { -1, -1 };
		for( int graded = 1; graded >= 0; graded-- ) {
			const Stat a = RenderStat( ShadowFixture( graded != 0, 1.3, "", true ), "slab_without" );
			const Stat b = RenderStat( ShadowFixture( graded != 0, 1.3, NestedSlabOccluder(), true ), "slab_with" );
			r[graded] = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
			std::printf( "    %-9s without mean=%.6f   with slab mean=%.6f   with/without=%.5f\n",
				graded ? "graded" : "constant", a.mean, b.mean, r[graded] );
		}
		const double nC = TentN( 0.02 ), nL = TentN( 1.0 );
		std::printf( "    pre-DL-292 graded/control prediction x%.4f\n", ( nL / nC ) * ( nL / nC ) );
		Check( r[0] > 0 && std::fabs( r[0] / ( T1 * T1 ) - 1.0 ) < 0.03,
			"K: constant-index control with/without ratio == two-crossing Fresnel T^2 within 3%" );
		Check( r[0] > 0 && r[1] > 0 && std::fabs( r[1] / r[0] - 1.0 ) < 0.02,
			"K: graded ratio == constant-control ratio within 2% (nested through-trip keeps the graded factor)" );
	}

	std::cout << std::endl << "-- Row L (DL-292 item 2): shadow ray EXITS the graded box into an index-matched enclosure --" << std::endl;
	{
		// Camera inside the graded box at z = 4/3 (n = 1.6) looking down at a
		// floor patch at the tent's peak z = 1 (n_C = 1.8); omni light at
		// z = 3, above the graded box's top face (n = 1.2).
		const std::string h =
			"dielectric_material\n{\n\tname mat_h\n\tior 1.2\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"box_geometry\n{\n\tname geo_h\n\twidth 64\n\theight 64\n\tdepth 4.5\n}\n\n"
			"standard_object\n{\n\tname h\n\tgeometry geo_h\n\tmaterial mat_h\n\tposition 0 0 1.75\n}\n\n";
		const std::string merged =
			"scalar_painter\n{\n\tname ior_merged\n\texpression max(1.2,1.8-0.6*abs(P.z-1.0))\n}\n\n"
			"dielectric_material\n{\n\tname mat_merged\n\tior ior_merged\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"box_geometry\n{\n\tname geo_merged\n\twidth 64\n\theight 64\n\tdepth 4.5\n}\n\n"
			"standard_object\n{\n\tname merged\n\tgeometry geo_merged\n\tmaterial mat_merged\n\tposition 0 0 1.75\n}\n\n";
		auto build = [&]( bool split ) {
			std::string s = ReadFile( kSeededScene );
			if( !split ) s = ReplaceSpan( s, "MEDIUM", merged );
			s = ReplaceSpan( s, "RASTERIZER", PTRasterizerOpts( 64, "\ttransparent_shadows TRUE\n" ) );
			s = ReplaceOnce( s, kOrthoInside,
				"orthographic_camera\n{\n\tlocation 0 0 1.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 0.2 0.2\n}\n" );
			s = ReplaceOnce( s, kBigFloorCorners,
				"\tpta -0.2 -0.2 1.0\n\tptb 0.2 -0.2 1.0\n\tptc 0.2 0.2 1.0\n\tptd -0.2 0.2 1.0\n" );
			s = ReplaceOnce( s, kEmitterObject,
				"omni_light\n{\n\tname lgt\n\tposition 0 0 3.0\n\tcolor 1 1 1\n\tpower 4\n}\n\n" + ( split ? h : std::string() ) );
			return s;
		};
		const std::string split = build( true ), one = build( false );
		Check( !split.empty() && !one.empty(), "L: fixtures built" );
		if( split.empty() || one.empty() ) return;
		const Stat a = RenderStat( one, "exit_ref" );
		const Stat b = RenderStat( split, "exit_split" );
		const double T = 1.0 - R0( 1.2 );		// normal incidence within 3 deg over the ROI
		const double r = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
		std::printf( "    one box max(1.2,tent) mean=%.6f sd=%.6f   graded box inside 1.2 box mean=%.6f sd=%.6f\n"
			"    split/one=%.5f  closed form T=%.5f  pre-DL-292 prediction T*(1.2/1.8)^2=%.5f\n",
			a.mean, a.sd, b.mean, b.sd, r, T, T * ( 1.2 / 1.8 ) * ( 1.2 / 1.8 ) );
		Check( r > 0 && std::fabs( r / T - 1.0 ) < 0.02,
			"L: shadow ray leaving the graded box prices the graded segment up to the face (split == one-box reference x T)" );
	}
}

//////////////////////////////////////////////////////////////////////
// Row M (DL-292 item 3): RayCaster's OWN volume walk.  `pixelpel_rasterizer`
// with a `DefaultPathTracing` shader traces its camera ray through
// RayCaster::CastRay, whose medium walk does NEE at the scatter vertex and
// continues with CastRay; the continuation's first surface hit Advances,
// the NEE used to pass no stack.  The same scene under
// `pathtracing_pel_rasterizer` (PT's own volume walk, row H) is the
// reference; a constant-index control shows the two rasterizers agree
// when no graded factor is involved.
//////////////////////////////////////////////////////////////////////
static std::string FogScene( bool graded, const std::string& ras )
{
	std::string s = WithSmallEmitter( ReadFile( kSeededScene ) );
	s = ReplaceOnce( s, kOrthoInside, kPinholeInside );
	if( graded ) {
		s = ReplaceOnce( s, "\tmaterial mat_graded\n\tposition 0 0 1\n}\n",
			"\tmaterial mat_graded\n\tposition 0 0 1\n\tinterior_medium fog\n}\n" );
	} else {
		std::string u = UniformBox( 1.4 );
		u = ReplaceOnce( u, "\tmaterial mat_u\n\tposition 0 0 1\n}\n",
			"\tmaterial mat_u\n\tposition 0 0 1\n\tinterior_medium fog\n}\n" );
		s = ReplaceSpan( s, "MEDIUM", u );
	}
	s = ReplaceSpan( s, "RASTERIZER", ras );
	return AfterHeader( s,
		"homogeneous_medium\n{\n\tname fog\n\tabsorption 0.05 0.05 0.05\n\tscattering 0.6 0.6 0.6\n\tphase isotropic\n}\n" );
}

static std::string RasterizerPixelPel( int spp, const std::string& shader )
{
	return shader + "pixelpel_rasterizer\n{\n\tsamples " + std::to_string( spp ) +
		"\n\tmax_recursion 10\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n";
}

static void RunRayCasterVolumeRow()
{
	std::cout << std::endl << "-- Row M (DL-292 item 3): RayCaster's own volume walk (pixelpel + pt shader op, fog in the graded box) --" << std::endl;
	double r[2] = { -1, -1 };
	for( int graded = 1; graded >= 0; graded-- ) {
		const std::string pp = FogScene( graded != 0, RasterizerPixelPel( 256, ShaderBlock() ) );
		const std::string pt = FogScene( graded != 0, RasterizerPT( 256 ) );
		Check( !pp.empty() && !pt.empty(), "M: fixture built" );
		if( pp.empty() || pt.empty() ) return;
		const Stat a = RenderStat( pt, "fog_pt" );
		const Stat b = RenderStat( pp, "fog_pixelpel" );
		r[graded] = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
		std::printf( "    %-9s pathtracing_pel mean=%.6f   pixelpel+pt mean=%.6f   pixelpel/PT=%.4f\n",
			graded ? "graded" : "constant", a.mean, b.mean, r[graded] );
	}
	Check( r[0] > 0 && std::fabs( r[0] - 1.0 ) < 0.03, "M: constant-index control pixelpel+pt == pathtracing_pel within 3%" );
	Check( r[1] > 0 && std::fabs( r[1] - 1.0 ) < 0.03,
		"M: graded pixelpel+pt == pathtracing_pel within 3% (RayCaster volume NEE carries the walk's stack)" );
}

//////////////////////////////////////////////////////////////////////
// Row N (DL-292 item 4): the legacy shader-op chain.  Row B's scene
// (camera inside, big emitter; closed form rho*L_e*cov*(n_S/n_E)^2 --
// the value the PT nested-box reference of row B converges to) rendered
// with pixelpel_rasterizer and the chain Emission + DirectLighting +
// DistributionTracing.  Before DL-292 NO op in that chain paid the graded
// factor (RayCaster::CastRay's hit did not Advance, DirectLighting's NEE
// passed no stack), so the whole render read rho*L_e*cov = 1.65x.
//////////////////////////////////////////////////////////////////////
static void RunLegacyChainRow( const double closedB, const double preB )
{
	std::cout << std::endl << "-- Row N (DL-292 item 4): legacy chain Emission + DirectLighting + DistributionTracing, camera inside --" << std::endl;
	const std::string shader =
		"distributiontracing_shaderop\n{\n\tname dt\n\tsamples 1\n}\n\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultEmission\n\tshaderop DefaultDirectLighting\n\tshaderop dt\n}\n\n";
	const std::string base = ReadFile( kSeededScene );
	const std::string text = ReplaceSpan( base, "RASTERIZER", RasterizerPixelPel( 256, shader ) );
	Check( !text.empty(), "N: fixture built" );
	if( text.empty() ) return;
	const Stat s = RenderStat( text, "legacy_dt" );
	Report( "pixelpel legacy chain graded", s, closedB );
	std::printf( "    pre-DL-292 prediction %.6f (x%.4f)\n", preB, preB / closedB );
	Check( s.ok && std::fabs( s.mean / closedB - 1.0 ) < 0.03,
		"N: legacy Emission+DirectLighting+dt chain graded == closed form within 3%" );

	// The same chain on the uniform-ior control of row F, camera inside:
	// its own closed form rho*L_e*cov (camera and emitter in the same
	// constant medium).
	const std::string ctrl = ReplaceSpan( text, "MEDIUM", UniformBox( 1.4 ) );
	const Stat c = RenderStat( ctrl, "legacy_dt_ctrl" );
	const double closedCtrl = kRho * kLe * Coverage();
	Report( "pixelpel legacy chain uniform 1.4", c, closedCtrl );
	Check( c.ok && std::fabs( c.mean / closedCtrl - 1.0 ) < 0.03,
		"N: legacy chain uniform-1.4 control == rho*L_e*cov within 3%" );
}

//////////////////////////////////////////////////////////////////////
// Row O (DL-292 item 6): `ior` forms.  A composite of a world-position
// field (`scalar_painter { base ... scale ... }`, `multiply`, `add`) IS a
// world-position field and must be priced like the expression itself;
// before DL-292 the composites did not forward IsWorldPositionField and
// every graded render read the pre-DL-09 2.25x.  A form that READS the
// world position but cannot be a single-scalar field (a per-channel vec3
// expression of P, or P mixed with u/v) is refused at load with a
// diagnostic instead of silently rendering the pre-DL-09 accounting.
//////////////////////////////////////////////////////////////////////
static bool LoadsOk( const std::string& text )
{
	const std::string path = WriteTemp( text, "load" );
	if( path.empty() ) return false;
	IJobPriv* pJob = nullptr;
	bool ok = false;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		ok = pJob->LoadAsciiSceneViaCst( path.c_str() );
	}
	safe_release( pJob );
	std::remove( path.c_str() );
	return ok;
}

static void RunIorFormRows( const double closedA )
{
	std::cout << std::endl << "-- Row O (DL-292 item 6): composite ior painters and refused forms --" << std::endl;
	const std::string base = ReplaceSpan( ReadFile( "scenes/Tests/Materials/graded_index_interior_gather.RISEscene" ),
		"RASTERIZER", RasterizerPT( 64 ) );
	const std::string tentExpr = "scalar_painter\n{\n\tname ior_tent\n\texpression 1.8-0.6*abs(P.z-1.0)\n}\n";
	struct F { const char* name; std::string painters; };
	const F forms[] = {
		{ "scaled (base half-tent scale 2)",
			"scalar_painter\n{\n\tname half_tent\n\texpression 0.9-0.3*abs(P.z-1.0)\n}\n\n"
			"scalar_painter\n{\n\tname ior_tent\n\tbase half_tent\n\tscale 2\n}\n" },
		{ "add (1.2 + ramp)",
			"scalar_painter\n{\n\tname c12\n\tvalue 1.2\n}\n\n"
			"scalar_painter\n{\n\tname ramp\n\texpression 0.6-0.6*abs(P.z-1.0)\n}\n\n"
			"scalar_painter\n{\n\tname ior_tent\n\tadd c12 ramp\n}\n" },
		{ "multiply (2 x half-tent)",
			"scalar_painter\n{\n\tname two\n\tvalue 2.0\n}\n\n"
			"scalar_painter\n{\n\tname half_tent\n\texpression 0.9-0.3*abs(P.z-1.0)\n}\n\n"
			"scalar_painter\n{\n\tname ior_tent\n\tmultiply two half_tent\n}\n" },
	};
	for( const F& f : forms ) {
		const std::string text = ReplaceOnce( base, tentExpr, f.painters );
		Check( !text.empty(), std::string( "O: " ) + f.name + " fixture built" );
		if( text.empty() ) continue;
		const Stat s = RenderStat( text, "ior_form" );
		char lbl[96];
		std::snprintf( lbl, sizeof(lbl), "PT graded, ior %s", f.name );
		Report( lbl, s, closedA );
		Check( s.ok && std::fabs( s.mean / closedA - 1.0 ) < 0.03,
			std::string( "O: composite ior " ) + f.name + " is a world-position field (== closed form within 3%)" );
	}

	struct R { const char* name; std::string painters; bool expectLoad; };
	const R refused[] = {
		{ "vec3 expression of P",
			"scalar_painter\n{\n\tname ior_tent\n\texpression vec3(1.8-0.6*abs(P.z-1.0),1.81-0.6*abs(P.z-1.0),1.82-0.6*abs(P.z-1.0))\n}\n", false },
		{ "P mixed with u",
			"scalar_painter\n{\n\tname ior_tent\n\texpression 1.8-0.6*abs(P.z-1.0)+0.01*u\n}\n", false },
		{ "composite of P and u",
			"scalar_painter\n{\n\tname p_part\n\texpression 1.8-0.6*abs(P.z-1.0)\n}\n\n"
			"scalar_painter\n{\n\tname u_part\n\texpression 0.01*u\n}\n\n"
			"scalar_painter\n{\n\tname ior_tent\n\tadd p_part u_part\n}\n", false },
		{ "u-driven (no P): surface index, loads", "scalar_painter\n{\n\tname ior_tent\n\texpression 1.5+0.1*u\n}\n", true },
		{ "constant expression: loads", "scalar_painter\n{\n\tname ior_tent\n\texpression 1.5\n}\n", true },
	};
	for( const R& r : refused ) {
		const std::string text = ReplaceOnce( base, tentExpr, r.painters );
		Check( !text.empty(), std::string( "O: " ) + r.name + " fixture built" );
		if( text.empty() ) continue;
		const bool loaded = LoadsOk( text );
		std::printf( "    ior %-40s loads=%d (expected %d)\n", r.name, loaded ? 1 : 0, r.expectLoad ? 1 : 0 );
		Check( loaded == r.expectLoad, std::string( "O: ior " ) + r.name + ( r.expectLoad ? " loads" : " is refused at load" ) );
	}
}


//////////////////////////////////////////////////////////////////////
// Row P (DL-292 item 5): the photon tracers.  A global photon map
// gathered directly at the floor of a SHRUNK row-B scene (box, floor and
// emitter 8 / 8 / 6 units wide, so a few million photons resolve the
// camera's footprint), camera inside, rendered by `pixelpel_rasterizer`
// with the global-map gather op.  Reference: the same scene under
// `pathtracing_pel_rasterizer` (both estimate the straight-segment model;
// a finite emitter is not bending-free, so the closed form is not the
// reference here).  A constant-index control fixes the photon estimator's
// own density-estimation bias; the gate is graded ratio == control ratio.
// Before DL-292 the photon walk paid no importance-order factor, so the
// gather priced the floor at (1/n_E)^2 short of the eye walk's pairing.
//////////////////////////////////////////////////////////////////////
static std::string ShrunkSeededScene( bool graded, const std::string& ras )
{
	std::string s = ReadFile( kSeededScene );
	if( graded ) {
		s = ReplaceOnce( s, "\twidth 60\n\theight 60\n\tdepth 2\n", "\twidth 8\n\theight 8\n\tdepth 2\n" );
	} else {
		std::string u = UniformBox( 1.4 );
		u = ReplaceOnce( u, "\twidth 60\n\theight 60\n\tdepth 2\n", "\twidth 8\n\theight 8\n\tdepth 2\n" );
		s = ReplaceSpan( s, "MEDIUM", u );
	}
	s = ReplaceOnce( s, kBigFloorCorners,
		"\tpta -3.9 -3.9 0.02\n\tptb 3.9 -3.9 0.02\n\tptc 3.9 3.9 0.02\n\tptd -3.9 3.9 0.02\n" );
	s = ReplaceOnce( s, "\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n",
		"\tpta -3 -3 1.0\n\tptb -3 3 1.0\n\tptc 3 3 1.0\n\tptd 3 -3 1.0\n" );
	return ReplaceSpan( s, "RASTERIZER", ras );
}

static void RunPhotonMapRow()
{
	std::cout << std::endl << "-- Row P (DL-292 item 5): global photon map gathered at an interior floor, camera inside --" << std::endl;
	const std::string photonRas =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultGlobalPelPhotonMap\n}\n\n"
		"pixelpel_rasterizer\n{\n\tsamples 16\n\tmax_recursion 10\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n"
		"global_pel_photonmap\n{\n\tnum 3000000\n\tmax_recursion 10\n\tmin_importance 0.0001\n}\n\n"
		"global_pel_gather\n{\n\tmax_photons 400\n\tradius 0.3\n}\n";
	double r[2] = { -1, -1 };
	for( int graded = 1; graded >= 0; graded-- ) {
		const std::string pt = ShrunkSeededScene( graded != 0, RasterizerPT( 256 ) );
		const std::string ph = ShrunkSeededScene( graded != 0, photonRas );
		Check( !pt.empty() && !ph.empty(), "P: fixture built" );
		if( pt.empty() || ph.empty() ) return;
		const Stat a = RenderStat( pt, "photon_ref_pt" );
		const Stat b = RenderStat( ph, "photon_gather" );
		r[graded] = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
		std::printf( "    %-9s pathtracing_pel mean=%.6f sd=%.6f   global photon gather mean=%.6f sd=%.6f   photon/PT=%.4f\n",
			graded ? "graded" : "constant", a.mean, a.sd, b.mean, b.sd, r[graded] );
	}
	const double nC = TentN( 0.02 ), nE = TentN( 1.0 ), nS = TentN( 1.0 / 3.0 );
	// Direct-light predictions (the gather is dominated by the emitter's
	// first-hit photons): master, where neither the legacy eye walk nor the
	// photons paid a factor, x(n_E/n_S)^2; the eye walk fixed (item 4) but
	// the photons not, x(n_E/n_C)^2.
	std::printf( "    graded/control predictions: master x(n_E/n_S)^2 = %.4f; eye walk fixed, photons not x(n_E/n_C)^2 = %.4f\n",
		( nE / nS ) * ( nE / nS ), ( nE / nC ) * ( nE / nC ) );
	// Tolerances: the photon gather's own run-to-run spread at 3M photons
	// is ~2-3 % (seed bases 1000/2000/3000: control photon/PT 1.0006 /
	// 1.0335 / 0.9778), so the gate sits at 8 % -- still 15x inside the
	// 2.24 the unfixed photon walk reads.
	Check( r[0] > 0 && std::fabs( r[0] - 1.0 ) < 0.08, "P: constant-index control photon gather == PT within 8%" );
	Check( r[0] > 0 && r[1] > 0 && std::fabs( r[1] / r[0] - 1.0 ) < 0.08,
		"P: graded photon/PT ratio == constant-control photon/PT ratio within 8% (photon walk Advances in importance order)" );
}


//////////////////////////////////////////////////////////////////////
// Row Q (DL-292 item 5, OPT-IN: GRADED_ROWS must name Q explicitly;
// prints, does not gate).  SMS inside a graded medium: a small glass
// sphere nested in the graded box focuses a small area emitter onto the
// floor under the camera.  PT with SMS vs PT without (the latter reaches
// the caustic by BSDF sampling through the sphere and is DL-09-priced),
// graded vs a constant-index control.  A ratio-of-ratios != 1 is the
// graded factor SMS's chain evaluation misses.
//////////////////////////////////////////////////////////////////////
static void RunSMSMeasurementRow()
{
	std::cout << std::endl << "-- Row Q (DL-292 item 5, measurement only): PT+SMS vs PT through a glass sphere nested in the graded box --" << std::endl;
	// mode 1 = graded box, 0 = constant-index box, 2 = no box (air).
	auto build = [&]( int mode, bool sms ) {
		std::string s = ReadFile( kSeededScene );
		if( mode == 0 ) s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
		if( mode == 2 ) s = ReplaceSpan( s, "MEDIUM", "" );
		s = ReplaceOnce( s, "\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n",
			"\tpta 0.5 -0.1 1.0\n\tptb 0.5 0.1 1.0\n\tptc 0.7 0.1 1.0\n\tptd 0.7 -0.1 1.0\n" );
		s = ReplaceOnce( s, "\tscale 1.0\n", "\tscale 40.0\n" );
		s = ReplaceOnce( s, kEmitterObject, kEmitterObject +
			"\ndielectric_material\n{\n\tname mat_ball\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"sphere_geometry\n{\n\tname geo_ball\n\tradius 0.15\n}\n\n"
			"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tmaterial mat_ball\n\tposition 0.3 0 0.55\n}\n" );
		return ReplaceSpan( s, "RASTERIZER", PTRasterizerOpts( 1024, sms ?
			"\tsms_enabled TRUE\n\tsms_max_iterations 30\n\tsms_threshold 1e-4\n\tsms_max_chain_depth 10\n\tsms_biased TRUE\n" : "" ) );
	};
	double r[3] = { -1, -1, -1 };
	const char* names[3] = { "constant", "graded", "air" };
	for( int mode = 2; mode >= 0; mode-- ) {
		const Stat a = RenderStat( build( mode, false ), "sms_off" );
		const Stat b = RenderStat( build( mode, true ), "sms_on" );
		// A third estimator: VCM (merges reach the caustic without SMS).
		const Stat v = RenderStat( ReplaceSpan( build( mode, false ), "RASTERIZER", RasterizerVCM( 512 ) ), "sms_vcm" );
		const Stat d = RenderStat( ReplaceSpan( build( mode, false ), "RASTERIZER", RasterizerBDPT( 512 ) ), "sms_bdpt" );
		r[mode] = ( a.ok && b.ok && a.mean > 0 ) ? b.mean / a.mean : -1;
		std::printf( "    %-9s PT %.6f  PT+SMS %.6f  BDPT %.6f  VCM %.6f   SMS/PT=%.4f  BDPT/PT=%.4f  VCM/PT=%.4f\n",
			names[mode], a.mean, b.mean, d.mean, v.mean, r[mode],
			( a.ok && a.mean > 0 ) ? d.mean / a.mean : -1.0, ( a.ok && a.mean > 0 ) ? v.mean / a.mean : -1.0 );
	}
	std::printf( "    graded/control SMS/PT ratio = %.4f (1 = SMS prices the graded medium like PT)\n",
		( r[0] > 0 && r[1] > 0 ) ? r[1] / r[0] : -1.0 );
}


//////////////////////////////////////////////////////////////////////
// Row R (DL-292, OPT-IN: GRADED_ROWS must name R explicitly; prints, does
// not gate).  The two L-sized residuals the row leaves open, measured:
//   R1  a ROUGH graded dielectric (`scattering 3`, a non-delta
//       transmission lobe) with the camera outside: a BDPT/VCM connection
//       from an eye vertex ON the graded object's own surface (whose walk
//       arrived through air, so it recorded no graded medium) to a light
//       vertex inside prices no graded factor.  PT vs BDPT vs VCM, graded
//       vs a constant-index control with the same roughness.
//   R2  BDPT's t==1 splat prices the camera end with the camera VERTEX's
//       recorded index, not n at the separately sampled thin-lens point: a
//       wide-aperture thin-lens camera inside a medium whose index varies
//       ACROSS the lens (a lateral gradient), BDPT vs PT, graded vs the
//       same camera in a constant-index control.
//////////////////////////////////////////////////////////////////////
static void RunOpenResidualMeasurements()
{
	std::cout << std::endl << "-- Row R1 (measurement only): rough graded dielectric, camera outside --" << std::endl;
	for( int graded = 1; graded >= 0; graded-- ) {
		// A small emitter OFF the camera's axis (x in [0.3, 0.5]) so the
		// camera sees it only through the rough top face's broad lobe --
		// the strategies that connect the face vertex to the emitter carry
		// real weight -- and never sees its unlit back.
		std::string s = ReadFile( "scenes/Tests/Materials/graded_index_interior_gather.RISEscene" );
		s = ReplaceOnce( s, "\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n",
			"\tpta 0.3 -0.1 1.0\n\tptb 0.3 0.1 1.0\n\tptc 0.5 0.1 1.0\n\tptd 0.5 -0.1 1.0\n" );
		if( !graded ) s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
		s = graded ? ReplaceOnce( s, "\tname mat_graded\n\tior ior_tent\n\ttau 1.0\n\tscattering 1000000\n",
				"\tname mat_graded\n\tior ior_tent\n\ttau 1.0\n\tscattering 3\n" )
			: ReplaceOnce( s, "\tname mat_u\n\tior 1.4\n\ttau 1.0\n\tscattering 1000000\n",
				"\tname mat_u\n\tior 1.4\n\ttau 1.0\n\tscattering 3\n" );
		const Stat p = RenderStat( ReplaceSpan( s, "RASTERIZER", RasterizerPT( 512 ) ), "r1_pt" );
		const Stat b = RenderStat( ReplaceSpan( s, "RASTERIZER", RasterizerBDPT( 256 ) ), "r1_bdpt" );
		const Stat v = RenderStat( ReplaceSpan( s, "RASTERIZER", RasterizerVCM( 256 ) ), "r1_vcm" );
		std::printf( "    %-9s PT %.6f  BDPT %.6f  VCM %.6f   BDPT/PT=%.4f  VCM/PT=%.4f\n",
			graded ? "graded" : "constant", p.mean, b.mean, v.mean,
			p.mean > 0 ? b.mean / p.mean : -1.0, p.mean > 0 ? v.mean / p.mean : -1.0 );
	}

	std::cout << std::endl << "-- Row R2 (measurement only): wide thin-lens camera inside, index varying across the lens --" << std::endl;
	for( int graded = 1; graded >= 0; graded-- ) {
		std::string s = WithSmallEmitter( ReadFile( kSeededScene ) );
		s = ReplaceOnce( s, kOrthoInside,
			"thinlens_camera\n{\n\tlocation 0 0 0.333333333333\n\tlookat 0 0 0\n\tup 0 1 0\n"
			"\tsensor_size 36\n\tfocal_length 150\n\tfstop 0.5\n\tfocus_distance 0.313\n}\n" );
		if( graded ) {
			// Lateral gradient, clamped so the 60-wide box never reads a
			// non-positive index: 0.5 per unit across the lens aperture.
			s = ReplaceOnce( s, "\texpression 1.8-0.6*abs(P.z-1.0)\n",
				"\texpression 1.8-0.6*abs(P.z-1.0)+0.5*clamp(P.x,-0.5,0.5)\n" );
		} else {
			s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
		}
		const Stat p = RenderStat( ReplaceSpan( s, "RASTERIZER", RasterizerPT( 512 ) ), "r2_pt" );
		const Stat b = RenderStat( ReplaceSpan( s, "RASTERIZER", RasterizerBDPT( 256 ) ), "r2_bdpt" );
		std::printf( "    %-9s thin lens: PT %.6f  BDPT %.6f   BDPT/PT=%.4f\n",
			graded ? "graded" : "constant", p.mean, b.mean, p.mean > 0 ? b.mean / p.mean : -1.0 );
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

	// GRADED_ROWS (optional env var): run only the rows whose letters it
	// contains, e.g. GRADED_ROWS=JKL.  Unset runs every row (the CI run).
	const char* rowsEnv = std::getenv( "GRADED_ROWS" );
	auto on = [rowsEnv]( char c ) { return !rowsEnv || std::strchr( rowsEnv, c ) != nullptr; };

	if( on( 'E' ) ) RunSeedRow();
	if( on( 'A' ) ) RunGatherRows( "A", "scenes/Tests/Materials/graded_index_interior_gather.RISEscene", closedA, preA );
	if( on( 'B' ) ) RunGatherRows( "B", "scenes/Tests/Materials/graded_index_seeded_inside.RISEscene", closedB, preB );
	if( on( 'C' ) ) RunSmallEmitterOutsideRow();
	if( on( 'D' ) ) RunPinholeConsistencyRow();
	if( on( 'G' ) ) RunSpectralRow( closedA );
	if( on( 'H' ) ) RunMediumRow();
	if( on( 'F' ) ) RunUniformControlRow();

	// DL-292 (debt-dl292 slice): coverage rows.
	if( on( 'I' ) ) RunSSSEntryNEERow();
	if( on( 'J' ) || on( 'K' ) || on( 'L' ) ) RunShadowSkipRows();
	if( on( 'M' ) ) RunRayCasterVolumeRow();
	if( on( 'N' ) ) RunLegacyChainRow( closedB, preB );
	if( on( 'O' ) ) RunIorFormRows( closedA );
	if( on( 'P' ) ) RunPhotonMapRow();
	// Opt-in measurement row (named explicitly, never in the default run).
	if( rowsEnv && std::strchr( rowsEnv, 'Q' ) ) RunSMSMeasurementRow();
	if( rowsEnv && std::strchr( rowsEnv, 'R' ) ) RunOpenResidualMeasurements();

	std::cout << std::endl << "Passed: " << passCount << std::endl << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
