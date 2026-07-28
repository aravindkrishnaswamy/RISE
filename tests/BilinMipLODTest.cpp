//////////////////////////////////////////////////////////////////////
//
//  BilinMipLODTest.cpp - Coverage for BilinRasterImageAccessor's
//    LOD-aware sampling path (GetPELwithLOD + the lazy mip pyramid),
//    which previously had no test at all.  Guards two recent
//    memory-safety fixes in particular:
//
//      - the NaN-safe early-out (`!(lod > 0)`): a NaN lod must take
//        the base-sample path instead of flowing into
//        (int)std::floor(NaN) (UB) and a garbage pyramid index;
//      - the BuildMipPyramid zero-dimension guard: a 0-dim image
//        with mipmapping enabled must not run the level-1 box
//        filter (whose y0 = min(0, -1) would reach the unchecked
//        raster GetPEL) — the pyramid stays empty, LOD clamps to 0,
//        and sampling falls through to the guarded base path.
//
//    Determinism note: the stochastic bracket pick only engages for
//    FRACTIONAL lod (chosenLevel = lvlFloor + (MipHash < frac)).
//    Integer lods have frac == 0, so level selection is exact and
//    the expectations below are deterministic.  The one fractional
//    check asserts membership in the two bracketing levels' values
//    rather than a specific pick.
//
//    Image: 4x4 width-axis ramp, texel value px/3 (0, 1/3, 2/3, 1),
//    constant along the height axis.  Pyramid: level 1 is 2x2 with
//    row-constant texels {1/6, 5/6}; level 2 (coarsest) is 1x1 with
//    value 1/2.  Sampled at (x, y) = (0.5, 0.125) — input y is the
//    width-axis UV in this accessor family:
//      base:    u = 0.125*4 + 0.5 = 1.0  -> texel column 1 = 1/3
//      level 1: u = 0.125*2 + 0.5 = 0.75 -> 0.25*(1/6) + 0.75*(5/6) = 2/3
//      level 2: 1x1                       -> 1/2
//    All three are pairwise distinct, so level selection is
//    observable from the sampled value alone.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <limits>
#include <string>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageAccessor.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;

static int s_pass = 0;
static int s_fail = 0;

static void Check( bool ok, const std::string& what )
{
	if( ok ) {
		++s_pass;
		std::cout << "  PASS: " << what << "\n";
	} else {
		++s_fail;
		std::cout << "  FAIL: " << what << "\n";
	}
}

static const int W = 4;
static const int H = 4;

// Sample point: input y drives the width axis (wrap_s/u), input x the
// height axis.  y = 0.125 pins the base sample to texel column 1
// exactly (u = 1.0, t = 0); x = 0.5 lands mid-image on the constant
// axis, so the height-axis blend is between equal values everywhere.
static const Scalar kSampleX = 0.5;
static const Scalar kSampleY = 0.125;

static const Scalar kBaseExpected   = 1.0 / 3.0;	// texel column 1
static const Scalar kLevel1Expected = 2.0 / 3.0;	// 0.25*(1/6) + 0.75*(5/6)
static const Scalar kLevel2Expected = 0.5;			// 1x1 average of everything

// Width-axis ramp, constant along height: texel (px, py) = px/3.
static IRasterImage* MakeRampImage()
{
	IRasterImage* img = 0;
	Check( RISE_API_CreateRISEColorRasterImage( &img, W, H, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) ) && img,
		"ramp raster image created" );
	for( int py = 0; py < H; ++py ) {
		for( int px = 0; px < W; ++px ) {
			const Scalar v = Scalar( px ) / Scalar( W - 1 );
			img->SetPEL( px, py, RISEColor( RISEPel( v, v, v ), 1.0 ) );
		}
	}
	return img;
}

static IRasterImageAccessor* MakeBilin( IRasterImage& img, const bool mipmap )
{
	IRasterImageAccessor* ria = 0;
	if( !RISE_API_CreateBiLinRasterImageAccessor( &ria, img,
			eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge,
			mipmap, /*supersample*/ false ) || !ria ) {
		++s_fail;
		std::cout << "  FAIL: bilin accessor factory failed\n";
		return 0;
	}
	return ria;
}

static Scalar SampleLOD( IRasterImageAccessor& ria, const Scalar lod )
{
	RISEColor c;
	ria.GetPELwithLOD( kSampleX, kSampleY, lod, c );
	return c.base[0];
}

static bool Near( const Scalar a, const Scalar b )
{
	// Expectations are exact modulo a handful of FP rounding steps in
	// the box filter; margins between levels are >= 1/6.
	return fabs( a - b ) < 1e-9;
}

static void TestLevelSelection()
{
	std::cout << "Integer-LOD level selection is deterministic and box-filtered\n";
	IRasterImage* img = MakeRampImage();
	IRasterImageAccessor* ria = MakeBilin( *img, true );
	if( ria ) {
		Check( ria->SupportsLOD(), "mipmap accessor reports SupportsLOD()" );

		RISEColor base;
		ria->GetPEL( kSampleX, kSampleY, base );
		Check( Near( base.base[0], kBaseExpected ),
			"plain GetPEL == 1/3 (base texel), got " + std::to_string( base.base[0] ) );

		Check( Near( SampleLOD( *ria, 0.0 ), kBaseExpected ),
			"lod=0 returns the base sample" );
		Check( Near( SampleLOD( *ria, -0.0 ), kBaseExpected ),
			"lod=-0.0 returns the base sample" );
		Check( Near( SampleLOD( *ria, -3.5 ), kBaseExpected ),
			"negative lod returns the base sample" );

		const Scalar l1 = SampleLOD( *ria, 1.0 );
		Check( Near( l1, kLevel1Expected ),
			"lod=1 samples the 2x2 mip (2/3), got " + std::to_string( l1 ) );
		Check( !Near( l1, kBaseExpected ),
			"lod=1 result is distinguishable from the base sample" );

		const Scalar l2 = SampleLOD( *ria, 2.0 );
		Check( Near( l2, kLevel2Expected ),
			"lod=2 samples the 1x1 mip (1/2), got " + std::to_string( l2 ) );

		// Fractional lod picks one of the two bracketing levels
		// (deterministic per (x, y, lod) via MipHash, but which
		// bracket is implementation detail — assert membership).
		const Scalar l15 = SampleLOD( *ria, 1.5 );
		Check( Near( l15, kLevel1Expected ) || Near( l15, kLevel2Expected ),
			"lod=1.5 lands on one of the bracketing levels, got " + std::to_string( l15 ) );

		ria->release();
	}
	img->release();
}

static void TestOverAndNonFiniteLODs()
{
	std::cout << "Over-range and non-finite LODs are clamped or take the base path\n";
	IRasterImage* img = MakeRampImage();
	IRasterImageAccessor* ria = MakeBilin( *img, true );
	if( ria ) {
		Check( Near( SampleLOD( *ria, 7.0 ), kLevel2Expected ),
			"lod beyond the pyramid clamps to the coarsest level" );
		Check( Near( SampleLOD( *ria, 1e300 ), kLevel2Expected ),
			"huge finite lod clamps to the coarsest level" );

		// Non-finite LODs are only assertable where FP comparisons have
		// IEEE semantics.  The macOS release build compiles with
		// -ffast-math (Config.OSX; legacy SGI/Solaris too), which makes
		// any NaN/inf UB in BOTH this TU and the library — the
		// accessor's !(lod > 0) early-out is source-level hardening
		// whose guarantee holds in IEEE builds (Config.Linux's plain
		// -O3, MSVC /fp:precise via run_all_tests.ps1, debug builds).
		// Asserting UB would be a false test, so these checks compile
		// out under fast-math.
#if !defined(__FAST_MATH__) && !defined(_M_FP_FAST)
		Check( Near( SampleLOD( *ria, std::numeric_limits<Scalar>::infinity() ), kLevel2Expected ),
			"lod=+inf clamps to the coarsest level" );

		const Scalar nanLod = std::numeric_limits<Scalar>::quiet_NaN();
		RISEColor viaNaN, viaBase;
		ria->GetPELwithLOD( kSampleX, kSampleY, nanLod, viaNaN );
		ria->GetPEL( kSampleX, kSampleY, viaBase );
		Check( viaNaN.base[0] == viaBase.base[0]
		    && viaNaN.base[1] == viaBase.base[1]
		    && viaNaN.base[2] == viaBase.base[2]
		    && viaNaN.a == viaBase.a,
			"lod=NaN takes the base path (bitwise-equal to GetPEL)" );
#else
		std::cout << "  SKIP: non-finite LOD checks (fast-math build: NaN/inf are UB by design)\n";
#endif

		ria->release();
	}
	img->release();
}

static void TestMipmapDisabledForwards()
{
	std::cout << "mipmap=false accessor forwards every LOD to the base sample\n";
	IRasterImage* img = MakeRampImage();
	IRasterImageAccessor* ria = MakeBilin( *img, false );
	if( ria ) {
		Check( !ria->SupportsLOD(), "non-mipmap accessor reports !SupportsLOD()" );
		Check( Near( SampleLOD( *ria, 2.0 ), kBaseExpected ),
			"lod=2 with mipmap disabled returns the base sample" );
		ria->release();
	}
	img->release();
}

// Regression guard for the BuildMipPyramid zero-dimension guard: the
// partial-zero 4x0 shape is the dangerous one (the level-1 loop body
// would run with y0 = min(0, -1) = -1 without the guard); 0x0 covers
// the fully-degenerate corner.  Both must return a default (zero) pel
// from the guarded base path without crashing.
static void TestZeroDimensionImages()
{
	std::cout << "Zero-dimension images with mipmapping do not crash and return zero\n";
	const unsigned int dims[2][2] = { { 0, 0 }, { W, 0 } };
	for( int i = 0; i < 2; ++i ) {
		IRasterImage* img = 0;
		Check( RISE_API_CreateRISEColorRasterImage( &img, dims[i][0], dims[i][1],
				RISEColor( RISEPel( 1, 1, 1 ), 1.0 ) ) && img,
			"degenerate raster image created (" + std::to_string( dims[i][0] ) + "x"
				+ std::to_string( dims[i][1] ) + ")" );
		IRasterImageAccessor* ria = MakeBilin( *img, true );
		if( ria ) {
			RISEColor c;
			ria->GetPELwithLOD( kSampleX, kSampleY, 1.0, c );	// forces the pyramid build attempt
			Check( c.base[0] == 0.0 && c.base[1] == 0.0 && c.base[2] == 0.0 && c.a == 0.0,
				"GetPELwithLOD on degenerate image returns the zero pel" );
			ria->GetPEL( kSampleX, kSampleY, c );
			Check( c.base[0] == 0.0 && c.base[1] == 0.0 && c.base[2] == 0.0 && c.a == 0.0,
				"GetPEL on degenerate image returns the zero pel" );
			ria->release();
		}
		img->release();
	}
}

int main()
{
	std::cout << "=== BilinMipLODTest -- GetPELwithLOD / mip pyramid coverage ===\n";
	GlobalLog();	// initialize the global log

	TestLevelSelection();
	TestOverAndNonFiniteLODs();
	TestMipmapDisabledForwards();
	TestZeroDimensionImages();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
