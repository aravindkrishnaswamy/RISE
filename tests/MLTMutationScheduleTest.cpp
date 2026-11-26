//////////////////////////////////////////////////////////////////////
//
//  MLTMutationScheduleTest.cpp - MLT mutation budget, round schedule and
//    splat normalization (MLTRasterizer + MLTSpectralRasterizer).
//
//    Three defects this locks down, in both rasterizers:
//      (a) mutationsPerChain = budget / chains could be 0 (2x2 frame,
//          8 chains, 1 mutation/pixel); the timed branch clamped the round
//          count to it and then divided by 0.
//      (b) the "bootstrap too fast to time" branch used 20 rounds
//          regardless of budget; with a 1-mutation-per-round floor, 19
//          rounds over-ran the per-chain budget and the last round's
//          `mutationsPerChain - mutationsDonePerChain` wrapped unsigned
//          (a ~4 billion mutation round: a hang).
//      (c) the budget was a 32-bit product (wraps above 2^32, e.g.
//          3840x2160 at 518 mutations/pixel), and splats were normalized
//          by the requested budget while only perChain * chains mutations
//          ran, so the image was dark by performed / budget.
//
//    Sections:
//      1. Pure helper sweep (ComputeMLTMutationSchedule and the round
//         helpers) over small and > 2^32 budgets, plus a simulation of the
//         rasterizers' round loop under both initial round-count branches
//         and an adaptive resize: the per-round counts sum to perChain
//         exactly and the loop terminates.
//      2. Tiny real renders (2x2, 8 chains and 2 chains, 1 mutation/pixel)
//         for RGB MLT, spectral MLT (hwss FALSE) and spectral MLT HWSS:
//         they complete and every pixel is finite and nonzero; a
//         64-sample-bootstrap variant normally takes the "too fast to
//         time" branch.  Pre-fix (arm64, where integer division by zero
//         yields 0 instead of trapping) the 8-chain renders ran zero rounds
//         and produced a black frame, and the tiny-bootstrap variant hung
//         in a ~4e9-mutation round.
//      3. Normalization.  Every on-film splat set of one mutation carries
//         exactly invTotal * b of luminance (accept + (1 - accept) = 1, each
//         sample's splat weights divide by its own luminance), so when no
//         splat can land off the film the resolved image's luminance sum is
//         performed * invTotal * b -- deterministic, independent of the
//         chains' random walk.  The scene makes every splat land on film
//         (orthographic camera whose viewport is exactly the floor; the
//         emitter is behind the camera), and the bootstrap is shared
//         between runs (seed i is sample i), so b is identical.  Renders at
//         budget 4 / 3 chains (performed 3), budget 4 / 4 chains (performed
//         4), budget 64 / 1 chain and budget 4 / 8 chains (performed 8)
//         must have the same mean luminance; before the fix the first read
//         0.75 and the last hung (defect b).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <climits>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <limits>
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
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Rendering/MLTRasterizer.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& name )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

static std::string Fmt( const char* f, ... )
{
	char buf[16384];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return std::string( buf );
}

//////////////////////////////////////////////////////////////////////
// 1. Helper sweep + round-loop simulation
//////////////////////////////////////////////////////////////////////

//! Mirror of the rasterizers' round loop (MLTRasterizer.cpp /
//! MLTSpectralRasterizer.cpp Phase 3) using the same helpers.  Returns
//! false if an invariant breaks.  `resizeDesired` < 0 means no resize.
static bool SimulateRounds( const std::uint64_t perChain, std::uint64_t numRounds,
	const double resizeDesired, std::uint64_t& roundsRun, std::string& why )
{
	unsigned int perRound = MLTMutationsPerRound( perChain, numRounds );
	std::uint64_t done = 0;
	roundsRun = 0;
	const std::uint64_t cap = 50000000ull;
	for( std::uint64_t round = 0; done < perChain; round++ ) {
		const std::uint64_t remaining = perChain - done;
		const unsigned int m = MLTMutationsThisRound( remaining, perRound, round + 1 >= numRounds );
		if( m == 0 ) { why = "zero-mutation round"; return false; }
		if( m > remaining ) { why = "round exceeds remaining work"; return false; }
		done += m;
		roundsRun++;
		if( roundsRun > cap ) { why = "did not terminate"; return false; }
		if( round == 0 && resizeDesired >= 0 ) {
			const std::uint64_t rem = perChain - done;
			unsigned int newPer = 0;
			std::uint64_t newNum = 0;
			MLTAdaptiveRoundResize( resizeDesired, rem, newPer, newNum );
			if( newPer < 1 ) { why = "resize produced 0 per round"; return false; }
			if( rem > 0 && newPer != perRound ) {
				perRound = newPer;
				numRounds = newNum;
			}
		}
	}
	if( done != perChain ) { why = "sum != perChain"; return false; }
	return true;
}

static void TestHelpers()
{
	std::cout << "\n-- 1: schedule helper sweep + round-loop simulation --" << std::endl;

	unsigned int combos = 0;
	bool scheduleOk = true;
	bool loopOk = true;
	std::string firstBad;
	const double resizes[] = { -1.0, 0.0, 0.3, 1.0, 2.0, 7.5, 1000.0, 1e12,
		std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() };
	const double estimates[] = { 0.0, 0.4, 2.6, 17.0, 1e6, 1e30,
		std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() };

	for( unsigned int mpp = 1; mpp <= 3; mpp++ )
	for( unsigned int w = 1; w <= 4; w++ )
	for( unsigned int h = 1; h <= 4; h++ )
	for( unsigned int chains = 0; chains <= 16; chains++ ) {
		combos++;
		const MLTMutationSchedule s = ComputeMLTMutationSchedule( mpp, w, h, chains );
		const std::uint64_t budget = std::uint64_t( mpp ) * w * h;
		const unsigned int ec = chains > 0 ? chains : 1;
		const bool ok = s.effectiveChains == ec && s.budget == budget && s.perChain >= 1 &&
			s.performed == s.perChain * ec &&
			s.performed + ( ec - 1 ) >= budget && s.performed <= budget + ( ec - 1 ) &&
			( budget < ec ? s.perChain == 1 : s.perChain == budget / ec );
		if( !ok && scheduleOk ) {
			scheduleOk = false;
			firstBad = Fmt( "schedule mpp %u %ux%u chains %u", mpp, w, h, chains );
		}
		// Both initial round-count branches x every resize.
		std::vector<std::uint64_t> starts;
		starts.push_back( ClampMLTRoundCount( 20, s.perChain ) );
		for( double e : estimates ) {
			starts.push_back( MLTRoundCountFromEstimate( e, 2, s.perChain ) );
		}
		for( std::uint64_t nr : starts ) {
			if( nr < 1 || nr > s.perChain ) {
				if( loopOk ) firstBad = Fmt( "round count %llu outside [1, %llu]", (unsigned long long)nr, (unsigned long long)s.perChain );
				loopOk = false;
			}
			for( double r : resizes ) {
				std::uint64_t ran = 0;
				std::string why;
				if( !SimulateRounds( s.perChain, nr, r, ran, why ) ) {
					if( loopOk ) firstBad = Fmt( "loop mpp %u %ux%u chains %u rounds %llu resize %g: %s",
						mpp, w, h, chains, (unsigned long long)nr, r, why.c_str() );
					loopOk = false;
				}
			}
		}
	}
	if( !firstBad.empty() ) std::cout << "    first failure: " << firstBad << std::endl;
	std::printf( "    small sweep: %u (mpp, w, h, chains) combinations\n", combos );
	Check( scheduleOk, "small sweep: perChain >= 1, performed == perChain * chains, |performed - budget| < chains" );
	Check( loopOk, "small sweep: round loop sums to perChain exactly and terminates (both branches, all resizes)" );

	// The specific defect cases.
	{
		const MLTMutationSchedule s = ComputeMLTMutationSchedule( 1, 2, 2, 8 );
		Check( s.perChain == 1 && s.performed == 8 && ClampMLTRoundCount( 20, s.perChain ) == 1 &&
			MLTRoundCountFromEstimate( 5.0, 2, s.perChain ) == 1,
			"2x2 / 8 chains / 1 mpp: perChain 1, performed 8, one round in both branches" );
		const MLTMutationSchedule t = ComputeMLTMutationSchedule( 1, 2, 2, 3 );
		Check( t.perChain == 1 && t.performed == 3 && t.budget == 4,
			"2x2 / 3 chains / 1 mpp: performed 3 of budget 4 (the normalization constant)" );
	}

	// Large budgets > 2^32.
	bool largeOk = true;
	const unsigned int bigChains[] = { 1, 4, 1000, 0 };
	const unsigned int bigMpp[] = { 518, 1000, UINT_MAX };
	for( unsigned int mpp : bigMpp )
	for( unsigned int chains : bigChains ) {
		const MLTMutationSchedule s = ComputeMLTMutationSchedule( mpp, 3840, 2160, chains );
		const std::uint64_t budget = std::uint64_t( mpp ) * 3840ull * 2160ull;
		const unsigned int ec = chains > 0 ? chains : 1;
		const bool ok = s.budget == budget && budget > 0xFFFFFFFFull && s.perChain == budget / ec &&
			s.performed == s.perChain * ec && s.performed + ( ec - 1 ) >= budget && s.performed <= budget;
		std::printf( "    3840x2160 x %u mpp, %u chains: budget %llu, perChain %llu, performed %llu\n",
			mpp, chains, (unsigned long long)s.budget, (unsigned long long)s.perChain, (unsigned long long)s.performed );
		if( !ok ) largeOk = false;
		// Loop simulation: 20-round fallback, a minimal timed estimate, a
		// single planned round (perRound clamps to UINT_MAX), a resize to a
		// per-round count above UINT_MAX, and an arbitrary resize.
		const std::uint64_t starts[] = { ClampMLTRoundCount( 20, s.perChain ),
			MLTRoundCountFromEstimate( 0.0, 2, s.perChain ), 1 };
		// Resize to an arbitrary non-divisor per-round count that keeps the
		// simulated round count under the cap (~1000 rounds).
		const double rs[] = { -1.0, 1e12, std::numeric_limits<double>::infinity(),
			double( s.perChain ) / 997.3 };
		for( std::uint64_t nr : starts )
		for( double r : rs ) {
			std::uint64_t ran = 0;
			std::string why;
			if( !SimulateRounds( s.perChain, nr, r, ran, why ) ) {
				largeOk = false;
				std::printf( "      FAIL loop rounds %llu resize %g: %s\n", (unsigned long long)nr, r, why.c_str() );
			}
		}
	}
	Check( largeOk, "budgets > 2^32: exact 64-bit budget, no wrap, loop sums exactly and terminates" );

	// Saturation: a product beyond 2^64 saturates rather than wraps.
	{
		const MLTMutationSchedule s = ComputeMLTMutationSchedule( UINT_MAX, UINT_MAX, UINT_MAX, 7 );
		Check( s.budget == UINT64_MAX && s.perChain == UINT64_MAX / 7 && s.performed == s.perChain * 7,
			"a budget beyond 2^64 saturates (no wrap) and performed stays perChain * chains" );
	}

	// MLTMutationsThisRound never exceeds the remaining work.
	Check( MLTMutationsThisRound( 3, 10, false ) == 3 && MLTMutationsThisRound( 30, 10, false ) == 10 &&
		MLTMutationsThisRound( 30, 10, true ) == 30 &&
		MLTMutationsThisRound( 0x200000000ull, 10, true ) == UINT_MAX,
		"MLTMutationsThisRound: min(perRound, remaining); planned last round takes the remainder, clamped to UINT_MAX" );
}

//////////////////////////////////////////////////////////////////////
// Rendering fixture
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

	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		width = img.GetWidth();
		height = img.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = img.GetPEL( x, y );
			}
		}
	}
};

static const unsigned kRes = 2;
static const unsigned kBootstrap = 20000;

struct RenderResult
{
	bool ok = false;
	bool finite = false;
	double meanLum = 0;		// Rec.709 luminance mean over the frame
};

static RenderResult RenderOnce( const std::string& scene, const char* tag )
{
	RenderResult out;
	const std::string path = Fmt( "%s/mlt_schedule_%s_%d.RISEscene", RISE::TestSupport::ScratchBaseDir().c_str(), tag, int( getpid() ) );
	{
		std::ofstream ofs( path.c_str() );
		ofs << scene;
	}
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob ) {
		if( pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			std::srand( 0x5C4Eu );
			if( pJob->Rasterize() && pCap->width == kRes && pCap->height == kRes ) {
				out.ok = true;
				out.finite = true;
				double s = 0;
				for( const RISEColor& c : pCap->pixels ) {
					const double r = c.base.r * c.a, g = c.base.g * c.a, b = c.base.b * c.a;
					if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) {
						out.finite = false;
					}
					s += 0.2126 * r + 0.7152 * g + 0.0722 * b;
				}
				out.meanLum = s / double( pCap->pixels.size() );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path.c_str() );
	return out;
}

enum Family { kRGB = 0, kSpectral = 1, kHWSS = 2 };
static const char* FamilyName( int f ) { return f == kRGB ? "RGB" : f == kSpectral ? "spectral" : "spectral HWSS"; }

//! A grey floor that is EXACTLY the orthographic viewport (so every eye
//! and light-tracing splat lands on the film) lit by a down-facing square
//! emitter behind the camera (never directly visible).
static std::string Scene( int family, unsigned chains, unsigned mpp, unsigned bootstrap = kBootstrap )
{
	std::string ras;
	const char* shader = "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	if( family == kRGB ) {
		ras = Fmt( "mlt_rasterizer\n{\n\tbootstrap_samples %u\n\tchains %u\n\tmutations_per_pixel %u\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n",
			bootstrap, chains, mpp );
	} else {
		ras = Fmt( "mlt_spectral_rasterizer\n{\n\tbootstrap_samples %u\n\tchains %u\n\tmutations_per_pixel %u\n\tlarge_step_prob 0.3\n\thwss %s\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n",
			bootstrap, chains, mpp, family == kHWSS ? "TRUE" : "FALSE" );
	}
	const double half = 0.5;
	return std::string( "RISE ASCII SCENE 7\n\n" ) + shader + ras +
		Fmt( "film\n{\n\twidth %u\n\theight %u\n}\n\n", kRes, kRes ) +
		"orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 1 1\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n\n" +
		Fmt( "clippedplane_geometry\n{\n\tname geo_floor\n\tpta %g %g 0\n\tptb %g %g 0\n\tptc %g %g 0\n\tptd %g %g 0\n\tdoublesided FALSE\n}\n\n",
			-half, -half, half, -half, half, half, -half, half ) +
		"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial mat_floor\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_e\n\tcolor 1 1 1\n}\n\n"
		"lambertian_luminaire_material\n{\n\tname mat_e\n\texitance pnt_e\n\tscale 6\n\tmaterial none\n}\n\n" +
		Fmt( "clippedplane_geometry\n{\n\tname geo_e\n\tpta %g %g %g\n\tptb %g %g %g\n\tptc %g %g %g\n\tptd %g %g %g\n\tdoublesided FALSE\n}\n\n",
			0.2, 0.4, 1.4, 1.0, 0.4, 1.4, 1.0, -0.4, 1.4, 0.2, -0.4, 1.4 ) +
		"standard_object\n{\n\tname emit\n\tgeometry geo_e\n\tmaterial mat_e\n}\n\n";
}

//////////////////////////////////////////////////////////////////////
// 2. Tiny renders complete with finite pixels
//////////////////////////////////////////////////////////////////////
static void TestTinyRenders()
{
	std::cout << "\n-- 2: tiny renders (2x2, 1 mutation/pixel) complete with finite pixels --" << std::endl;
	const unsigned chainCounts[] = { 8, 2 };
	for( int f = kRGB; f <= kHWSS; f++ ) {
		for( unsigned chains : chainCounts ) {
			const RenderResult r = RenderOnce( Scene( f, chains, 1 ), "tiny" );
			std::printf( "    %-14s chains %u: ok %d finite %d mean luminance %.6f\n",
				FamilyName( f ), chains, int( r.ok ), int( r.finite ), r.meanLum );
			Check( r.ok && r.finite && r.meanLum > 0,
				Fmt( "%s 2x2 / %u chains / 1 mpp renders, finite, nonzero", FamilyName( f ), chains ) );
		}
		// A tiny bootstrap usually finishes in < 1 ms, which takes the
		// "bootstrap too fast to time" branch (20 rounds before the fix:
		// with 8 chains and perChain 0 the last round wrapped unsigned and
		// ran ~4e9 mutations).  Which branch fires is timing-dependent, so
		// only completion is asserted; the branch itself is covered
		// deterministically by section 1's simulation.
		const RenderResult r = RenderOnce( Scene( f, 8, 1, 64 ), "tinyboot" );
		std::printf( "    %-14s chains 8, bootstrap 64: ok %d finite %d mean luminance %.6f\n",
			FamilyName( f ), int( r.ok ), int( r.finite ), r.meanLum );
		Check( r.ok && r.finite && r.meanLum > 0,
			Fmt( "%s 2x2 / 8 chains / 1 mpp / 64 bootstrap renders, finite, nonzero", FamilyName( f ) ) );
	}
}

//////////////////////////////////////////////////////////////////////
// 3. Normalization by the performed mutation total
//////////////////////////////////////////////////////////////////////
static void TestNormalization()
{
	std::cout << "\n-- 3: image luminance is independent of performed vs requested budget --" << std::endl;
	struct Case { unsigned chains; unsigned mpp; const char* what; };
	// Budget = mpp * 4 pixels.
	const Case cases[] = {
		{ 4, 1,  "budget 4 / 4 chains (performed 4, reference)" },
		{ 3, 1,  "budget 4 / 3 chains (performed 3)" },
		{ 1, 16, "budget 64 / 1 chain (performed 64)" },
		{ 8, 1,  "budget 4 / 8 chains (performed 8)" },
	};
	// RGB: the resolved luminance sum is performed * invTotal * b exactly,
	// up to floating-point summation order (measured ratio 1.000000).
	// Spectral: splats carry XYZ and the frame is converted to Rec.709
	// RGB, which clamps out-of-gamut (negative) channels of single-
	// wavelength samples, so Rec.709 Y of the output differs from the
	// splatted Y by a sample-dependent ~1e-4 (measured 2.9e-4 worst
	// without HWSS, 2.1e-5 with).  2e-3 covers that and is still two
	// orders below the 25 % / 2x pre-fix errors this row detects.
	for( int f = kRGB; f <= kHWSS; f++ ) {
		const double tol = f == kRGB ? 1e-4 : 2e-3;
		double ref = 0;
		for( const Case& c : cases ) {
			const RenderResult r = RenderOnce( Scene( f, c.chains, c.mpp ), "norm" );
			if( &c == &cases[0] ) ref = r.meanLum;
			const double ratio = ref > 0 ? r.meanLum / ref : 0;
			std::printf( "    %-14s %-46s mean luminance %.8f  ratio %.6f\n", FamilyName( f ), c.what, r.meanLum, ratio );
			if( &c != &cases[0] ) {
				Check( r.ok && std::fabs( ratio - 1.0 ) < tol,
					Fmt( "%s %s matches the reference exposure", FamilyName( f ), c.what ) );
			} else {
				Check( r.ok && r.meanLum > 0, Fmt( "%s reference render", FamilyName( f ) ) );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// 4. Zero bootstrap samples: rejected at parse time; a rasterizer
//    constructed with 0 through the API fails the render cleanly.
//    (Before the guard the bootstrap mean was 0/0 and the CDF read
//    cdf[-1] of an empty vector.)
//////////////////////////////////////////////////////////////////////
static void TestZeroBootstrap()
{
	std::cout << "\n-- 4: bootstrap_samples 0 --" << std::endl;
	for( int f = kRGB; f <= kHWSS; f += 1 ) {
		const std::string path = Fmt( "%s/mlt_schedule_zero_%d_%d.RISEscene", RISE::TestSupport::ScratchBaseDir().c_str(), f, int( getpid() ) );
		{ std::ofstream ofs( path.c_str() ); ofs << Scene( f, 4, 1, 0 ); }
		IJobPriv* pJob = nullptr;
		bool loaded = true;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			loaded = pJob->LoadAsciiSceneViaCst( path.c_str() );
		}
		Check( pJob && !loaded, Fmt( "%s: a scene with bootstrap_samples 0 fails to load", FamilyName( f ) ) );
		safe_release( pJob );
		std::remove( path.c_str() );

		// The derive diagnostic names the parameter (not the generic
		// "apply failed").
		Job* job = new Job();
		job->addref();
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( Scene( f, 4, 1, 0 ) );
		std::vector<std::string> diags;
		RISE::Cst::DeriveToJob( doc, *job, &diags );
		job->release();
		std::string joined;
		for( const std::string& d : diags ) joined += d + "\n";
		std::printf( "    %-14s bootstrap_samples 0 diagnostic: %s", FamilyName( f ), joined.empty() ? "(none)\n" : joined.c_str() );
		Check( joined.find( "bootstrap_samples" ) != std::string::npos,
			Fmt( "%s: the load diagnostic names bootstrap_samples", FamilyName( f ) ) );
	}

	// API construction with 0: load a valid scene, then replace its
	// rasterizer through IJob with nBootstrap = 0 and render.
	for( int f = kRGB; f <= kHWSS; f++ ) {
		const std::string path = Fmt( "%s/mlt_schedule_zeroapi_%d_%d.RISEscene", RISE::TestSupport::ScratchBaseDir().c_str(), f, int( getpid() ) );
		{ std::ofstream ofs( path.c_str() ); ofs << Scene( f, 4, 1, 16 ); }
		IJobPriv* pJob = nullptr;
		bool set = false, rendered = true;
		unsigned int outW = 0;
		if( RISE_CreateJobPriv( &pJob ) && pJob && pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			PixelFilterConfig filter;
			filter.filter = String( "box" );
			if( f == kRGB ) {
				set = pJob->SetMLTRasterizer( 4, 4, 0, 4, 1, 0.3, "global", false, false,
					OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast, filter, StabilityConfig() );
			} else {
				SpectralConfig spectral;
				spectral.useHWSS = f == kHWSS;
				set = pJob->SetMLTSpectralRasterizer( 4, 4, 0, 4, 1, 0.3, "global", false, spectral, false,
					OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast, filter, StabilityConfig() );
			}
			if( set && pJob->GetRasterizer() ) {
				pJob->RemoveRasterizerOutputs();
				CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
				GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
				pJob->GetRasterizer()->AddRasterizerOutput( pCap );
				pJob->Rasterize();
				// The animation entry point takes the same refusal (logged
				// as a failure, not as a user cancel).
				pJob->RasterizeAnimation( 0, 0, 1, false, false );
				rendered = pCap->width != 0;
				outW = pCap->width;
				safe_release( pCap );
			}
		}
		std::printf( "    %-14s API nBootstrap 0: rasterizer set %d, final image width %u\n", FamilyName( f ), int( set ), outW );
		Check( set && !rendered, Fmt( "%s: nBootstrap 0 through the API (still and animation) renders nothing and returns cleanly", FamilyName( f ) ) );
		safe_release( pJob );
		std::remove( path.c_str() );
	}
}

int main( int argc, char** argv )
{
	std::cout << "=== MLTMutationScheduleTest ===" << std::endl;
	const bool helpersOnly = argc == 2 && std::strcmp( argv[1], "--helpers-only" ) == 0;
	TestHelpers();
	if( !helpersOnly ) {
		TestTinyRenders();
		TestNormalization();
		TestZeroBootstrap();
	}
	std::cout << "\n=== MLTMutationScheduleTest: " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
