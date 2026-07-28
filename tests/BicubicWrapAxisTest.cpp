//////////////////////////////////////////////////////////////////////
//
//  BicubicWrapAxisTest.cpp - Regression guard for the wrap-mode axis
//    convention in BicubicRasterImageAccessor's 4x4 neighbour fetch.
//
//    Axis convention (shared by NNB / Bilin / Bicubic accessors):
//      input `y`  --wrap_s-->  u  -->  raster X (width axis, px)
//      input `x`  --wrap_t-->  v  -->  raster Y (height axis, py)
//
//    BilinRasterImageAccessor keys its seam partners on this convention
//    (xhi on wrap_s, yhi on wrap_t; note those branches are currently
//    weight-inert because the pre-clamp forces t == 0 whenever they
//    fire — they record the convention, which is what matters here).
//    Bicubic's 4x4 footprint loop wrapped px with wrap_t and py with
//    wrap_s -- swapped -- so any texture with wrap_s != wrap_t sampled
//    bicubically near a seam pulled neighbours from the wrong edge rule.
//
//    The checks:
//
//      1. On an image whose pixels vary ONLY along the width axis,
//         the output cannot depend on wrap_t (the height-axis rule):
//         (Repeat, Clamp) and (Repeat, Mirrored) must match
//         (Repeat, Repeat) exactly, sampled across the width seam.
//         Under the swap, wrap_t drives the width-axis neighbours and
//         the results diverge.
//      2. Symmetrically, on an image varying ONLY along the height
//         axis, the output cannot depend on wrap_s: (Clamp, Repeat)
//         and (Mirrored, Repeat) must match (Repeat, Repeat) across
//         the height seam.
//      3. Positive controls: (Clamp, Clamp) differs CLEARLY (by more
//         than 1e-3) from (Repeat, Repeat) at the same sample points
//         -- proving the 4x4 footprint actually crosses the seam,
//         i.e. the invariant checks have discriminating power.
//      4. Absolute golden values at the low width seam pin the
//         tap->value mapping, the collapse order, and the Catmull-Rom
//         weights (an invariant-only test would still pass if, e.g.,
//         the 4x4 tap array were transposed).
//      5. The pre-scale ApplyWrapMode axis pairing is covered with
//         out-of-[0,1] UVs: wrapping y=1.25 under wrap_s Repeat must
//         reproduce y=0.25 even when wrap_t is Clamp (and the
//         symmetric check for x / wrap_t) -- this would catch a swap
//         of the two ApplyWrapMode lines, which the in-[0,1] checks
//         above cannot see.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
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

static const int W = 8;
static const int H = 4;

// Distinct per-index values, chosen so every column (or row) differs --
// especially the two edge columns/rows, which is what the seam fetch
// swaps between under the wrong wrap rule.
static Scalar AxisValue( const int i, const int n )
{
	return Scalar( i + 1 ) / Scalar( n + 1 );
}

// Image whose pixels depend ONLY on the raster X (width-axis) index.
static IRasterImage* MakeWidthRampImage()
{
	IRasterImage* img = 0;
	Check( RISE_API_CreateRISEColorRasterImage( &img, W, H, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) ) && img,
		"width-ramp raster image created" );
	for( int py = 0; py < H; ++py ) {
		for( int px = 0; px < W; ++px ) {
			const Scalar v = AxisValue( px, W );
			img->SetPEL( px, py, RISEColor( RISEPel( v, v, v ), 1.0 ) );
		}
	}
	return img;
}

// Image whose pixels depend ONLY on the raster Y (height-axis) index.
static IRasterImage* MakeHeightRampImage()
{
	IRasterImage* img = 0;
	Check( RISE_API_CreateRISEColorRasterImage( &img, W, H, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) ) && img,
		"height-ramp raster image created" );
	for( int py = 0; py < H; ++py ) {
		for( int px = 0; px < W; ++px ) {
			const Scalar v = AxisValue( py, H );
			img->SetPEL( px, py, RISEColor( RISEPel( v, v, v ), 1.0 ) );
		}
	}
	return img;
}

static RISEColor SampleBicubic( IRasterImage& img, const char wrapS, const char wrapT,
                                const Scalar x, const Scalar y )
{
	IRasterImageAccessor* ria = 0;
	if( !RISE_API_CreateCatmullRomBicubicRasterImageAccessor( &ria, img, wrapS, wrapT ) || !ria ) {
		++s_fail;
		std::cout << "  FAIL: bicubic accessor factory failed\n";
		return RISEColor();
	}
	RISEColor c;
	ria->GetPEL( x, y, c );
	ria->release();
	return c;
}

static bool NearlyEqual( const RISEColor& a, const RISEColor& b )
{
	// Same arithmetic path is expected when the invariant holds, but
	// allow fp slack rather than demanding bitwise identity.
	const Scalar eps = 1e-9;
	return fabs( a.base[0] - b.base[0] ) < eps
	    && fabs( a.base[1] - b.base[1] ) < eps
	    && fabs( a.base[2] - b.base[2] ) < eps
	    && fabs( a.a - b.a ) < eps;
}

// For the positive controls: require a clear separation, not merely
// "beyond the equality eps" -- the controls exist to prove the seam
// contribution is large enough for the invariant checks to have
// discriminating power.
static bool ClearlyDiffers( const RISEColor& a, const RISEColor& b )
{
	const Scalar floor_ = 1e-3;
	return fabs( a.base[0] - b.base[0] ) > floor_
	    || fabs( a.base[1] - b.base[1] ) > floor_
	    || fabs( a.base[2] - b.base[2] ) > floor_;
}

// Accessor-input coordinates whose 4x4 footprint crosses the WIDTH seam.
// The width-axis pixel coord is derived from input `y`.  Input `x` = 0.5
// is a fixed point of ApplyWrapMode for every mode, so the height-axis
// PRE-WRAP cannot differ between accessors; the height-axis 4x4
// neighbours still leave the image (v = 2.5 for H = 4 puts py = 4 in
// the footprint), which is harmless to the invariant only because the
// width-ramp image is constant along the height axis.
//
// The high-side value is 0.80, not something like 0.98: u = wy*W + 0.5
// saturates to W-1 for wy > (W-1.5)/W, which makes the interpolation
// parameter exactly 0 — Catmull-Rom then returns the centre texel and
// the off-image neighbours (the very thing under test) stop mattering.
// wy = 0.80 gives u = 6.9 (W = 8): centre texel 6, footprint 5..8, and
// px = 8 crosses the seam with t = 0.9.
static const Scalar kWidthSeamY[2] = { 0.02, 0.80 };

// And the mirror case: footprint crosses the HEIGHT seam (from input
// `x`).  Same saturation reasoning: wx = 0.60 gives v = 2.9 (H = 4),
// centre texel 2, footprint 1..4, py = 4 crossing the seam at t = 0.9.
// (Here the inert width-axis footprint is fully in-image: y = 0.5
// gives u = 4.5, px = 3..6.)
static const Scalar kHeightSeamX[2] = { 0.02, 0.60 };

static void TestWidthAxisIgnoresWrapT()
{
	std::cout << "Width-varying image: wrap_t must be irrelevant (width-axis neighbours follow wrap_s)\n";
	IRasterImage* img = MakeWidthRampImage();
	for( int i = 0; i < 2; ++i ) {
		const Scalar y = kWidthSeamY[i];
		const RISEColor ref     = SampleBicubic( *img, eRasterWrap_Repeat, eRasterWrap_Repeat,         0.5, y );
		const RISEColor clampT  = SampleBicubic( *img, eRasterWrap_Repeat, eRasterWrap_ClampToEdge,    0.5, y );
		const RISEColor mirrorT = SampleBicubic( *img, eRasterWrap_Repeat, eRasterWrap_MirroredRepeat, 0.5, y );
		Check( NearlyEqual( clampT, ref ),
			"(Repeat, ClampToEdge) == (Repeat, Repeat) at y=" + std::to_string( y ) );
		Check( NearlyEqual( mirrorT, ref ),
			"(Repeat, MirroredRepeat) == (Repeat, Repeat) at y=" + std::to_string( y ) );

		// Positive control: the width seam genuinely matters here.
		const RISEColor allClamp = SampleBicubic( *img, eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge, 0.5, y );
		Check( ClearlyDiffers( allClamp, ref ),
			"control: (Clamp, Clamp) differs from (Repeat, Repeat) at y=" + std::to_string( y ) );
	}
	img->release();
}

static void TestHeightAxisIgnoresWrapS()
{
	std::cout << "Height-varying image: wrap_s must be irrelevant (height-axis neighbours follow wrap_t)\n";
	IRasterImage* img = MakeHeightRampImage();
	for( int i = 0; i < 2; ++i ) {
		const Scalar x = kHeightSeamX[i];
		const RISEColor ref     = SampleBicubic( *img, eRasterWrap_Repeat,         eRasterWrap_Repeat, x, 0.5 );
		const RISEColor clampS  = SampleBicubic( *img, eRasterWrap_ClampToEdge,    eRasterWrap_Repeat, x, 0.5 );
		const RISEColor mirrorS = SampleBicubic( *img, eRasterWrap_MirroredRepeat, eRasterWrap_Repeat, x, 0.5 );
		Check( NearlyEqual( clampS, ref ),
			"(ClampToEdge, Repeat) == (Repeat, Repeat) at x=" + std::to_string( x ) );
		Check( NearlyEqual( mirrorS, ref ),
			"(MirroredRepeat, Repeat) == (Repeat, Repeat) at x=" + std::to_string( x ) );

		// Positive control: the height seam genuinely matters here.
		const RISEColor allClamp = SampleBicubic( *img, eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge, x, 0.5 );
		Check( ClearlyDiffers( allClamp, ref ),
			"control: (Clamp, Clamp) differs from (Repeat, Repeat) at x=" + std::to_string( x ) );
	}
	img->release();
}

// Absolute golden values at the low width seam (y = 0.02, x = 0.5).
// Hand-derived from the Catmull-Rom weight form (CubicInterpolator.h):
//   u = 0.02*8 + 0.5 = 0.66, xlo = 0, mu = 0.66
//   w0 = -mu^3/2 + mu^2 - mu/2 = -0.038148
//   w1 = 1.5mu^3 - 2.5mu^2 + 1 =  0.342244
//   w2 = -1.5mu^3 + 2mu^2 + mu/2 = 0.769956
//   w3 = mu^3/2 - mu^2/2 = -0.074052
// Width-ramp texel values are (px+1)/9; footprint px = {-1, 0, 1, 2}.
//   Repeat:   px=-1 -> 7:  (8*w0 + 1*w1 + 2*w2 + 3*w3)/9 = 0.15053511
//   Mirrored: px=-1 -> 0:  (1*w0 + 1*w1 + 2*w2 + 3*w3)/9 = 0.18020578
// (At a one-texel overhang MirroredRepeat and ClampToEdge coincide by
// construction — reflection off the edge lands on the edge texel — so
// the mirrored golden also covers the clamp value.  They only diverge
// two texels out, which the 4x4 footprint reaches solely in the
// degenerate t == 0 saturation case.)
// These pin the tap->value mapping, the x-then-y collapse order, and
// the weight polynomial; the invariant tests alone cannot see a
// transposed tap array because each ramp is constant along one axis.
static void TestGoldenValuesAtLowWidthSeam()
{
	std::cout << "Golden values at the low width seam (pin taps, collapse order, weights)\n";
	IRasterImage* img = MakeWidthRampImage();
	const Scalar tol = 1e-4;	// goldens truncated near 1e-9; margin to the nearest wrong answer is ~3e-2

	const RISEColor rep = SampleBicubic( *img, eRasterWrap_Repeat, eRasterWrap_Repeat, 0.5, 0.02 );
	Check( fabs( rep.base[0] - 0.15053511 ) < tol,
		"golden (Repeat, Repeat) at y=0.02 == 0.15053511, got " + std::to_string( rep.base[0] ) );

	const RISEColor mir = SampleBicubic( *img, eRasterWrap_MirroredRepeat, eRasterWrap_Repeat, 0.5, 0.02 );
	Check( fabs( mir.base[0] - 0.18020578 ) < tol,
		"golden (MirroredRepeat, Repeat) at y=0.02 == 0.18020578, got " + std::to_string( mir.base[0] ) );

	img->release();
}

// ApplyWrapMode axis pairing with out-of-[0,1] UVs: the pre-scale wrap
// of input `y` must use wrap_s and of input `x` must use wrap_t.  With
// the "other" axis set to Clamp, a swap of the two ApplyWrapMode calls
// would leave the out-of-range UV unwrapped (clamping it at the far
// edge) instead of reproducing the in-range equivalent.
static void TestPreScaleWrapAxisPairing()
{
	std::cout << "Pre-scale ApplyWrapMode axis pairing (UV > 1)\n";

	IRasterImage* wimg = MakeWidthRampImage();
	const RISEColor tiled  = SampleBicubic( *wimg, eRasterWrap_Repeat, eRasterWrap_ClampToEdge, 0.5, 1.25 );
	const RISEColor inTile = SampleBicubic( *wimg, eRasterWrap_Repeat, eRasterWrap_ClampToEdge, 0.5, 0.25 );
	Check( NearlyEqual( tiled, inTile ),
		"(Repeat, Clamp): y=1.25 wraps to y=0.25 via wrap_s" );
	const RISEColor clamped = SampleBicubic( *wimg, eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge, 0.5, 1.25 );
	Check( ClearlyDiffers( clamped, inTile ),
		"control: (Clamp, Clamp) at y=1.25 saturates instead of wrapping" );
	wimg->release();

	IRasterImage* himg = MakeHeightRampImage();
	const RISEColor tiledX  = SampleBicubic( *himg, eRasterWrap_ClampToEdge, eRasterWrap_Repeat, 1.25, 0.5 );
	const RISEColor inTileX = SampleBicubic( *himg, eRasterWrap_ClampToEdge, eRasterWrap_Repeat, 0.25, 0.5 );
	Check( NearlyEqual( tiledX, inTileX ),
		"(Clamp, Repeat): x=1.25 wraps to x=0.25 via wrap_t" );
	const RISEColor clampedX = SampleBicubic( *himg, eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge, 1.25, 0.5 );
	Check( ClearlyDiffers( clampedX, inTileX ),
		"control: (Clamp, Clamp) at x=1.25 saturates instead of wrapping" );
	himg->release();
}

int main()
{
	std::cout << "=== BicubicWrapAxisTest -- 4x4 neighbour wrap axis convention ===\n";
	GlobalLog();	// initialize the global log

	TestWidthAxisIgnoresWrapT();
	TestHeightAxisIgnoresWrapS();
	TestGoldenValuesAtLowWidthSeam();
	TestPreScaleWrapAxisPairing();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
