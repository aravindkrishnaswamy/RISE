//////////////////////////////////////////////////////////////////////
//
//  RasterAccessorNonFiniteUVTest.cpp - Regression guard for the
//    non-finite UV coordinate guards in the raster image accessor
//    family (NNB / Bilin / Bicubic), the UV sibling of the
//    GetPELwithLOD NaN-lod bug.
//
//    Without the guards, a NaN coordinate survives ApplyWrapMode
//    (ClampToEdge returns uv unchanged) and BOTH pre-clamps
//    (`u < 0` and `u > dim-1` are false for NaN), then hits the
//    int/unsigned conversion (UB) and the unchecked raster GetPEL
//    (raw pData[y*stride+x]) — a wild read; SetPel's twin corrupts
//    texels (out-of-bounds where the conversion yields a huge index,
//    e.g. x86-64's INT_MIN; in-bounds at texel 0 where it saturates
//    to zero, e.g. arm64).  ±inf is in the same class: under Repeat /
//    MirroredRepeat it becomes inf - inf = NaN before the
//    conversion, and under ClampToEdge it previously saturated to an
//    edge texel — the guards deliberately trade that mode-dependent
//    salvage for one uniform zero-pel outcome.  Reachable via any
//    geometry that produces a NaN ri.ptCoord (TexturePainter
//    forwards ptCoord to the accessor unchecked, including the NaN
//    footprint Jacobian in lowmem mode).
//
//    Contract under test: every accessor entry point returns the
//    zero pel (or drops the write) for a non-finite coordinate, on
//    every build — the guards use the optimisation-safe FiniteMath
//    bit tests, so the contract holds under the -ffast-math configs
//    (Config.OSX, legacy SGI/Solaris, Xcode "Opto") where plain FP
//    comparisons on non-finite operands can be folded away.
//
//    The non-finite values are materialised through strtod at
//    runtime (FiniteMathTest.cpp pattern) so no non-finite literal
//    exists in this TU for a fast-math optimiser to constant-fold,
//    and each is asserted genuinely non-finite before use so a
//    failed materialisation cannot silently turn the checks into
//    tautologies.  The image is filled with 1s so the expected zero
//    pel is distinguishable from any legitimately sampled texel.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstdlib>
#include <string>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageAccessor.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/FiniteMath.h"

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
static const Scalar kFill = 1.0;	// every texel; a zero result cannot be a real sample

static IRasterImage* MakeOnesImage()
{
	IRasterImage* img = 0;
	Check( RISE_API_CreateRISEColorRasterImage( &img, W, H,
			RISEColor( RISEPel( kFill, kFill, kFill ), 1.0 ) ) && img,
		"ones raster image created" );
	return img;
}

static bool IsZeroPel( const RISEColor& c )
{
	return c.base[0] == 0.0 && c.base[1] == 0.0 && c.base[2] == 0.0 && c.a == 0.0;
}

static bool IsFillPel( const RISEColor& c )
{
	// Exact comparison is safe because the controls sample at dyadic
	// positions (ut = vt = 0.5 at base; the mip pre-clamp pins
	// ut = vt = 0), where bilinear weights are exact; Catmull-Rom is
	// exact for ANY parameter here because kFill is dyadic (1.0), so
	// its a0..a2 coefficient rows cancel to zero bit-exactly (a
	// non-dyadic fill would leave ~1e-17 residues); NNB is a single
	// texel fetch.
	return c.base[0] == kFill && c.a == 1.0;
}

// The three accessor flavours under test.
enum eAccessor { eNNB, eBilin, eBicubic };
static const char* kAccessorName[3] = { "NNB", "Bilin", "CatmullRomBicubic" };

static IRasterImageAccessor* MakeAccessor( IRasterImage& img, const eAccessor which, const char wrapMode )
{
	IRasterImageAccessor* ria = 0;
	bool ok = false;
	switch( which ) {
		case eNNB:     ok = RISE_API_CreateNNBRasterImageAccessor( &ria, img, wrapMode, wrapMode ); break;
		case eBilin:   ok = RISE_API_CreateBiLinRasterImageAccessor( &ria, img, wrapMode, wrapMode,
		                                                            /*mipmap*/ false, /*supersample*/ false ); break;
		case eBicubic: ok = RISE_API_CreateCatmullRomBicubicRasterImageAccessor( &ria, img, wrapMode, wrapMode ); break;
	}
	if( !ok || !ria ) {
		++s_fail;
		std::cout << "  FAIL: accessor factory failed (" << kAccessorName[which] << ")\n";
		return 0;
	}
	return ria;
}

int main()
{
	std::cout << "=== RasterAccessorNonFiniteUVTest -- NaN/inf UV guards across the accessor family ===\n";
	GlobalLog();	// initialize the global log

	// Materialise non-finite values at runtime; assert they really
	// are non-finite so a bad materialisation fails loudly instead
	// of downgrading every later check to a tautology.
	const Scalar nan    = std::strtod( "nan", nullptr );
	const Scalar posInf = std::strtod( "1e999", nullptr );	// overflows to +inf
	const Scalar negInf = std::strtod( "-1e999", nullptr );
	Check( !RISE::IsFiniteDouble( nan ) && !RISE::IsPositiveInfinityDouble( nan ),
		"strtod NaN is non-finite and not +inf" );
	Check( !RISE::IsFiniteDouble( posInf ) && RISE::IsPositiveInfinityDouble( posInf ),
		"strtod materialised +inf" );
	Check( !RISE::IsFiniteDouble( negInf ) && !RISE::IsPositiveInfinityDouble( negInf ),
		"strtod -inf is non-finite and not +inf" );

	const Scalar badCoords[3] = { nan, posInf, negInf };
	const char* badNames[3] = { "NaN", "+inf", "-inf" };
	const char wrapModes[3] = { eRasterWrap_ClampToEdge, eRasterWrap_Repeat, eRasterWrap_MirroredRepeat };
	const char* wrapNames[3] = { "Clamp", "Repeat", "Mirrored" };

	IRasterImage* img = MakeOnesImage();

	for( int a = 0; a < 3; ++a ) {
		for( int w = 0; w < 3; ++w ) {
			IRasterImageAccessor* ria = MakeAccessor( *img, (eAccessor)a, wrapModes[w] );
			if( !ria ) {
				continue;
			}
			const std::string tag = std::string( kAccessorName[a] ) + "/" + wrapNames[w];

			// Control: a finite sample really returns the fill value,
			// so the zero pel below is unambiguously the guard firing.
			RISEColor c;
			ria->GetPEL( 0.5, 0.5, c );
			Check( IsFillPel( c ), tag + ": finite sample returns the fill value" );

			for( int b = 0; b < 3; ++b ) {
				RISEColor bad( RISEPel( 9, 9, 9 ), 9.0 );	// poisoned; must be overwritten
				ria->GetPEL( badCoords[b], 0.5, bad );
				Check( IsZeroPel( bad ), tag + ": x=" + badNames[b] + " returns the zero pel" );

				bad = RISEColor( RISEPel( 9, 9, 9 ), 9.0 );
				ria->GetPEL( 0.5, badCoords[b], bad );
				Check( IsZeroPel( bad ), tag + ": y=" + badNames[b] + " returns the zero pel" );
			}

			RISEColor both( RISEPel( 9, 9, 9 ), 9.0 );
			ria->GetPEL( badCoords[0], badCoords[1], both );
			Check( IsZeroPel( both ), tag + ": both coords non-finite returns the zero pel" );

			ria->release();
		}
	}

	// Mip path: BilinearSampleMip bypasses GetPel, so it carries its
	// own guard.  lod = 1.0 deterministically selects mip level 1
	// (frac == 0), routing the NaN coordinate into that guard.
	{
		IRasterImageAccessor* ria = 0;
		if( RISE_API_CreateBiLinRasterImageAccessor( &ria, *img,
				eRasterWrap_ClampToEdge, eRasterWrap_ClampToEdge,
				/*mipmap*/ true, /*supersample*/ false ) && ria ) {
			RISEColor c( RISEPel( 9, 9, 9 ), 9.0 );
			ria->GetPELwithLOD( nan, 0.5, 1.0, c );
			Check( IsZeroPel( c ), "mip path: x=NaN at lod=1 returns the zero pel" );
			c = RISEColor( RISEPel( 9, 9, 9 ), 9.0 );
			ria->GetPELwithLOD( 0.5, nan, 1.0, c );
			Check( IsZeroPel( c ), "mip path: y=NaN at lod=1 returns the zero pel" );
			ria->GetPELwithLOD( 0.5, 0.5, 1.0, c );
			Check( IsFillPel( c ), "mip path: finite sample at lod=1 returns the fill value" );
			ria->release();
		} else {
			++s_fail;
			std::cout << "  FAIL: mipmap accessor factory failed\n";
		}
	}

	// SetPel twins: a non-finite coordinate must DROP the write (a
	// wild write is strictly worse than the wild read).  Verify no
	// texel changed anywhere in the image.
	for( int a = 0; a < 3; ++a ) {
		IRasterImageAccessor* ria = MakeAccessor( *img, (eAccessor)a, eRasterWrap_ClampToEdge );
		if( !ria ) {
			continue;
		}
		RISEColor poison( RISEPel( 5, 5, 5 ), 1.0 );
		ria->SetPEL( nan, 0.5, poison );
		ria->SetPEL( 0.5, posInf, poison );
		ria->SetPEL( negInf, nan, poison );
		bool untouched = true;
		for( int py = 0; py < H; ++py ) {
			for( int px = 0; px < W; ++px ) {
				const RISEColor t = img->GetPEL( px, py );
				untouched = untouched && t.base[0] == kFill && t.a == 1.0;
			}
		}
		// (Bicubic inherits Bilin's guarded SetPel — its iteration
		// re-exercises that shared path rather than a third guard.)
		Check( untouched, std::string( kAccessorName[a] ) + ": non-finite SetPEL drops the write (image untouched)" );
		ria->release();
	}

	// Footprint path: the subtlest producer — FINITE coordinates with
	// a NaN Jacobian entry.  GetPELwithFootprint computes
	// offU/offV = sx*dudx + sy*dudy, so one NaN derivative poisons the
	// offset and the shifted coordinate; the guard in the downstream
	// virtual GetPEL must absorb it (offV = sx*dvdx + sy*dvdy feeds
	// the accessor's x slot).  This is exactly the call shape
	// TexturePainter drives in lowmem (supersample) mode.  jitter 1.0
	// keeps sx = 0.5 nonzero so the non-finite derivative is never
	// multiplied by a zero coefficient a fast-math pass could fold.
	{
		IRasterImageAccessor* ria = MakeAccessor( *img, eBilin, eRasterWrap_ClampToEdge );
		if( ria ) {
			RISEColor c( RISEPel( 9, 9, 9 ), 9.0 );
			ria->GetPELwithFootprint( 0.5, 0.5, nan, 0.0, 0.0, 0.0, 1.0, 1.0, c );
			Check( IsZeroPel( c ), "footprint path: NaN dudx with finite coords returns the zero pel" );
			c = RISEColor( RISEPel( 9, 9, 9 ), 9.0 );
			ria->GetPELwithFootprint( 0.5, 0.5, 0.0, 0.0, negInf, 0.0, 1.0, 1.0, c );
			Check( IsZeroPel( c ), "footprint path: -inf dvdx with finite coords returns the zero pel" );
			ria->GetPELwithFootprint( 0.5, 0.5, 0.0, 0.0, 0.0, 0.0, 0.5, 0.5, c );
			Check( IsFillPel( c ), "footprint path: finite zero-extent footprint returns the fill value" );
			ria->release();
		}
	}

	img->release();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
