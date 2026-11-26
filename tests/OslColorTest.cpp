//////////////////////////////////////////////////////////////////////
//
//  OslColorTest.cpp - The `osl_` colour-conversion and Mix Color
//    builtins against Cycles.
//
//  1. Golden values.  tests/data/osl_color_golden.inc was dumped from
//     Cycles' own svm_node_separate_color, svm_node_combine_color and
//     svm_node_mix_color (tools/osl_golden/dump_color_golden.sh): all
//     four RGB/HSV/HSL conversions (interior, HDR/negative, ties,
//     achromatic, signed zeros, NaN, inf) and all 19 Mix Color blend
//     modes across every (clamp_factor, clamp_result) pair plus branch
//     edges, signed zeros, NaN and inf.  Each output comes from three
//     builds of Cycles' sources (SIMD and scalar as Blender builds
//     them, scalar as strict IEEE); the port is gated EXACTLY against
//     the strict build (bit for bit, sign of zero included; every
//     operation is a correctly rounded IEEE 754 operation with no libm
//     transcendentals, so the bit-exact gate holds on every platform),
//     and its distance to the other two builds is printed, split by
//     cause.
//  2. Independent closed-form checks that do not use Cycles: RGB <->
//     HSV and RGB <-> HSL round-trips and known primaries/secondaries;
//     analytic formulas for all 19 Mix modes at t = 0 and t = 1;
//     clamp_factor and clamp_result behaviour; in-place aliasing safety.
//  3. The VM.  Each builtin compiled from expression text returns the
//     kernel's output bit for bit (with non-finite channels mapped to
//     0 by the VM boundary), including pure-expression compilation
//     without context variables.
//  4. Compile-time checks: literal arguments, ranges, arity, types.
//  5. Robustness: out-of-range MixMode, NaN/inf inputs through the VM.
//  6. Memo-worthiness: the colour/Mix builtins are arithmetic-only and
//     do not trigger MemoWorthy().
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OslColor.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionOslBuiltins.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void Check( const bool ok, const std::string& what )
{
	if( ok ) { ++g_pass; return; }
	++g_fail;
	std::printf( "  FAIL: %s\n", what.c_str() );
}

//////////////////////////////////////////////////////////////////////
// Golden table row types (the .inc holds only initialisers).
//////////////////////////////////////////////////////////////////////

struct OslColorConvRow
{
	int op;		// 0 rgb2hsv, 1 hsv2rgb, 2 rgb2hsl, 3 hsl2rgb
	double in[3];
	double simd[3], scalar[3], ieee[3];
};

struct OslMixColorRow
{
	int mode;
	int clampFactor;
	int clampResult;
	double t;
	double a[3], b[3];
	double simd[3], scalar[3], ieee[3];
};

#include "data/osl_color_golden.inc"

#define COUNT( a ) ( (int)( sizeof( a ) / sizeof( ( a )[0] ) ) )

static const char* const kConvName[4] = { "rgb2hsv", "hsv2rgb", "rgb2hsl", "hsl2rgb" };
static const char* const kModeName[19] = {
	"blend", "add", "mul", "sub", "screen", "div", "diff", "dark", "light",
	"overlay", "dodge", "burn", "hue", "sat", "val", "color", "soft",
	"linear", "exclusion"
};

static void RunConv( const int op, const double in[3], double out[3] )
{
	switch( op ) {
	case 0: OslColor::RgbToHsv( in, out ); break;
	case 1: OslColor::HsvToRgb( in, out ); break;
	case 2: OslColor::RgbToHsl( in, out ); break;
	default: OslColor::HslToRgb( in, out ); break;
	}
}

// Keep comparisons exact down to the sign of zero (the test build uses
// -ffast-math).
#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(precise, on, push)
#endif

//! Bit-for-bit equal, with every NaN equal to every NaN (the golden table
//! writes NaN by name, so its sign and payload are not recorded).  The
//! sign of a zero counts.
static bool Same( const double a, const double b )
{
	if( std::isnan( a ) || std::isnan( b ) ) return std::isnan( a ) && std::isnan( b );
	return a == b && std::signbit( a ) == std::signbit( b );
}

static bool SameValIgnoreZeroSign( const double a, const double b )
{
	if( std::isnan( a ) || std::isnan( b ) ) return std::isnan( a ) && std::isnan( b );
	return a == b;
}

static double RelErr( const double a, const double b )
{
	if( std::isnan( a ) || std::isnan( b ) || std::isinf( a ) || std::isinf( b ) ) {
		return Same( a, b ) ? 0.0 : 1.0;
	}
	return std::fabs( a - b ) / std::fmax( 1.0, std::fabs( b ) );
}

static bool IsFiniteExact( const double a )
{
	return std::isfinite( a );
}

static bool IsInfExact( const double a )
{
	return std::isinf( a );
}

#if defined(__clang__) || defined(_MSC_VER)
#pragma float_control(pop)
#endif

namespace
{
	const double kBeyondRounding = 1e-5;

	struct BuildStats
	{
		int total;
		int exact;
		int signZeroOnly;
		int roundingWithin;		// finite value diff <= kBeyondRounding
		int roundingDeparts;	// finite value diff > kBeyondRounding
		int nonFiniteDiff;		// NaN/inf disagreement on an output channel
		double maxRoundingErr;
		std::string worstRounding;
		BuildStats()
			: total( 0 ), exact( 0 ), signZeroOnly( 0 ), roundingWithin( 0 ),
			  roundingDeparts( 0 ), nonFiniteDiff( 0 ), maxRoundingErr( 0.0 ) {}
	};

	void RecordVec3( const double got[3], const double built[3],
		const std::string& label, BuildStats& b )
	{
		++b.total;
		bool allSame = true;
		bool allSameIgnoreSign = true;
		bool anyNonFiniteMismatch = false;
		double rowErr = 0.0;
		for( int k = 0; k < 3; ++k ) {
			if( !Same( got[k], built[k] ) ) allSame = false;
			if( !SameValIgnoreZeroSign( got[k], built[k] ) ) allSameIgnoreSign = false;
			if( ( !IsFiniteExact( got[k] ) || !IsFiniteExact( built[k] ) ) &&
				!Same( got[k], built[k] ) ) {
				anyNonFiniteMismatch = true;
			}
			const double e = RelErr( got[k], built[k] );
			if( e > rowErr ) rowErr = e;
		}
		if( allSame ) {
			++b.exact;
			return;
		}
		if( allSameIgnoreSign ) {
			++b.signZeroOnly;
			return;
		}
		if( anyNonFiniteMismatch ) {
			++b.nonFiniteDiff;
			std::printf( "    [non-finite diff] %s: got=(%g,%g,%g) built=(%g,%g,%g)\n",
				label.c_str(), got[0], got[1], got[2], built[0], built[1], built[2] );
			return;
		}
		if( !( rowErr <= kBeyondRounding ) ) {
			++b.roundingDeparts;
			std::printf( "    [>1e-5 diff] %s: err=%.3e got=(%g,%g,%g) built=(%g,%g,%g)\n",
				label.c_str(), rowErr, got[0], got[1], got[2], built[0], built[1], built[2] );
		} else {
			++b.roundingWithin;
			if( rowErr > b.maxRoundingErr ) {
				b.maxRoundingErr = rowErr;
				b.worstRounding = label;
			}
		}
	}

	void PrintStats( const char* group, const char* buildName, const BuildStats& b )
	{
		std::printf( "  %s vs Cycles %s (%d rows): %d bit-exact, %d sign-of-zero-only, "
			"%d rounding <=1e-5 (max rel err %.3e at %s), %d >1e-5, %d non-finite diff\n",
			group, buildName, b.total, b.exact, b.signZeroOnly,
			b.roundingWithin, b.maxRoundingErr,
			b.worstRounding.empty() ? "none" : b.worstRounding.c_str(),
			b.roundingDeparts, b.nonFiniteDiff );
	}
}

//////////////////////////////////////////////////////////////////////
// 1. Golden values
//////////////////////////////////////////////////////////////////////

static void TestGoldenConversions()
{
	std::printf( "Test 1a: colour conversions match Cycles strict IEEE bit for bit\n" );
	BuildStats simdStats, scalarStats;
	int simdVsScalarDiff = 0;
	auto runTable = [&]( const OslColorConvRow* rows, const int n, const char* tag ) {
		for( int i = 0; i < n; ++i ) {
			const OslColorConvRow& r = rows[i];
			double got[3];
			RunConv( r.op, r.in, got );
			bool ok = true;
			bool ssSame = true;
			for( int k = 0; k < 3; ++k ) {
				ok = ok && Same( got[k], r.ieee[k] );
				ssSame = ssSame && Same( r.simd[k], r.scalar[k] );
			}
			if( !ssSame ) ++simdVsScalarDiff;
			const std::string lbl = std::string( tag ) + " row " + std::to_string( i ) +
				" (" + kConvName[r.op] + ")";
			if( !ok ) {
				std::printf( "    mismatch %s: got=(%a,%a,%a) ieee=(%a,%a,%a)\n",
					lbl.c_str(), got[0], got[1], got[2], r.ieee[0], r.ieee[1], r.ieee[2] );
			}
			Check( ok, lbl );
			RecordVec3( got, r.simd, lbl + " [simd]", simdStats );
			RecordVec3( got, r.scalar, lbl + " [scalar]", scalarStats );
		}
	};
	runTable( kConvRows, COUNT( kConvRows ), "conv" );
	runTable( kConvEdgeRows, COUNT( kConvEdgeRows ), "conv_edge" );
	PrintStats( "conversions", "SIMD", simdStats );
	PrintStats( "conversions", "scalar", scalarStats );
	std::printf( "  conversions SIMD vs scalar: %d differing rows\n", simdVsScalarDiff );
}

static void TestGoldenMix()
{
	std::printf( "Test 1b: Mix Color (19 modes, clamps, edges) matches Cycles strict IEEE bit for bit\n" );
	BuildStats simdStats, scalarStats;
	int simdVsScalarDiff = 0;
	auto runTable = [&]( const OslMixColorRow* rows, const int n, const char* tag ) {
		for( int i = 0; i < n; ++i ) {
			const OslMixColorRow& r = rows[i];
			double got[3];
			OslColor::MixColor( r.mode, r.t, r.a, r.b,
				r.clampFactor != 0, r.clampResult != 0, got );
			bool ok = true;
			bool ssSame = true;
			for( int k = 0; k < 3; ++k ) {
				ok = ok && Same( got[k], r.ieee[k] );
				ssSame = ssSame && Same( r.simd[k], r.scalar[k] );
			}
			if( !ssSame ) ++simdVsScalarDiff;
			const std::string lbl = std::string( tag ) + " row " + std::to_string( i ) +
				" (" + kModeName[r.mode] + " cf=" + std::to_string( r.clampFactor ) +
				" cr=" + std::to_string( r.clampResult ) + ")";
			if( !ok ) {
				std::printf( "    mismatch %s: got=(%a,%a,%a) ieee=(%a,%a,%a)\n",
					lbl.c_str(), got[0], got[1], got[2], r.ieee[0], r.ieee[1], r.ieee[2] );
			}
			Check( ok, lbl );
			RecordVec3( got, r.simd, lbl + " [simd]", simdStats );
			RecordVec3( got, r.scalar, lbl + " [scalar]", scalarStats );
		}
	};
	runTable( kMixRows, COUNT( kMixRows ), "mix" );
	runTable( kMixEdgeRows, COUNT( kMixEdgeRows ), "mix_edge" );
	PrintStats( "mix_color", "SIMD", simdStats );
	PrintStats( "mix_color", "scalar", scalarStats );
	std::printf( "  mix_color SIMD vs scalar: %d differing rows\n", simdVsScalarDiff );
}

//////////////////////////////////////////////////////////////////////
// 2. Independent closed-form checks (no Cycles)
//////////////////////////////////////////////////////////////////////

static bool Near3( const double a[3], const double b[3], const double tol )
{
	return std::fabs( a[0] - b[0] ) <= tol &&
		std::fabs( a[1] - b[1] ) <= tol &&
		std::fabs( a[2] - b[2] ) <= tol;
}

static void TestClosedForms()
{
	std::printf( "Test 2: independent closed-form colour and Mix identities\n" );

	// Known primaries, secondaries, and achromatic colours.
	struct Known { double rgb[3]; double hsv[3]; double hsl[3]; const char* name; };
	const Known kKnown[] = {
		{ { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0 },       { 0.0, 0.0, 0.0 },       "black" },
		{ { 1.0, 1.0, 1.0 }, { 0.0, 0.0, 1.0 },       { 0.0, 0.0, 1.0 },       "white" },
		{ { 0.5, 0.5, 0.5 }, { 0.0, 0.0, 0.5 },       { 0.0, 0.0, 0.5 },       "gray" },
		{ { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 1.0 },       { 0.0, 1.0, 0.5 },       "red" },
		{ { 1.0, 1.0, 0.0 }, { 1.0 / 6.0, 1.0, 1.0 }, { 1.0 / 6.0, 1.0, 0.5 }, "yellow" },
		{ { 0.0, 1.0, 0.0 }, { 2.0 / 6.0, 1.0, 1.0 }, { 2.0 / 6.0, 1.0, 0.5 }, "green" },
		{ { 0.0, 1.0, 1.0 }, { 3.0 / 6.0, 1.0, 1.0 }, { 3.0 / 6.0, 1.0, 0.5 }, "cyan" },
		{ { 0.0, 0.0, 1.0 }, { 4.0 / 6.0, 1.0, 1.0 }, { 4.0 / 6.0, 1.0, 0.5 }, "blue" },
		{ { 1.0, 0.0, 1.0 }, { 5.0 / 6.0, 1.0, 1.0 }, { 5.0 / 6.0, 1.0, 0.5 }, "magenta" },
	};
	for( int i = 0; i < COUNT( kKnown ); ++i ) {
		double hsv[3], hsl[3], backHsv[3], backHsl[3];
		OslColor::RgbToHsv( kKnown[i].rgb, hsv );
		OslColor::RgbToHsl( kKnown[i].rgb, hsl );
		OslColor::HsvToRgb( hsv, backHsv );
		OslColor::HslToRgb( hsl, backHsl );
		Check( Near3( hsv, kKnown[i].hsv, 1e-6 ), std::string( "known rgb2hsv " ) + kKnown[i].name );
		Check( Near3( hsl, kKnown[i].hsl, 1e-6 ), std::string( "known rgb2hsl " ) + kKnown[i].name );
		Check( Near3( backHsv, kKnown[i].rgb, 1e-6 ), std::string( "known hsv round-trip " ) + kKnown[i].name );
		Check( Near3( backHsl, kKnown[i].rgb, 1e-6 ), std::string( "known hsl round-trip " ) + kKnown[i].name );
	}

	// Round-trip over a 5x5x5 grid in [0, 1]^3 (all 6 hue sectors, low/high L).
	bool gridHsvOk = true, gridHslOk = true;
	for( int r = 0; r <= 4; ++r ) {
		for( int g = 0; g <= 4; ++g ) {
			for( int b = 0; b <= 4; ++b ) {
				const double rgb[3] = { r * 0.25, g * 0.25, b * 0.25 };
				double tmp[3], out[3];
				OslColor::RgbToHsv( rgb, tmp );
				OslColor::HsvToRgb( tmp, out );
				if( !Near3( out, rgb, 1e-6 ) ) gridHsvOk = false;
				OslColor::RgbToHsl( rgb, tmp );
				OslColor::HslToRgb( tmp, out );
				if( !Near3( out, rgb, 1e-6 ) ) gridHslOk = false;
			}
		}
	}
	Check( gridHsvOk, "5x5x5 grid RGB->HSV->RGB round-trip within 1e-6" );
	Check( gridHslOk, "5x5x5 grid RGB->HSL->RGB round-trip within 1e-6" );

	// Hue wrap: hsv2rgb(1, s, v) == hsv2rgb(0, s, v) bit for bit.
	{
		const double h0[3] = { 0.0, 0.7, 0.8 };
		const double h1[3] = { 1.0, 0.7, 0.8 };
		double r0[3], r1[3];
		OslColor::HsvToRgb( h0, r0 );
		OslColor::HsvToRgb( h1, r1 );
		Check( Same( r0[0], r1[0] ) && Same( r0[1], r1[1] ) && Same( r0[2], r1[2] ),
			"hsv2rgb wraps h == 1 to h == 0 exactly" );
	}

	// Closed forms for all 19 Mix modes at t = 0 and t = 1 on in-gamut colours.
	const double ca[3] = { 0.25, 0.75, 0.50 };
	const double cb[3] = { 0.80, 0.40, 0.20 };
	for( int mode = 0; mode < OslColor::kMixModeCount; ++mode ) {
		double r0[3];
		OslColor::MixColor( mode, 0.0, ca, cb, false, false, r0 );
		Check( Near3( r0, ca, 1e-6 ), std::string( "t=0 returns a for " ) + kModeName[mode] );
	}

	auto expectT1 = [&]( const int mode, const double e0, const double e1, const double e2 ) {
		double r[3];
		const double e[3] = { e0, e1, e2 };
		OslColor::MixColor( mode, 1.0, ca, cb, false, false, r );
		Check( Near3( r, e, 1e-6 ), std::string( "t=1 closed form for " ) + kModeName[mode] );
	};
	expectT1( OslColor::eBlend,     cb[0], cb[1], cb[2] );
	expectT1( OslColor::eAdd,       ca[0] + cb[0], ca[1] + cb[1], ca[2] + cb[2] );
	expectT1( OslColor::eMul,       ca[0] * cb[0], ca[1] * cb[1], ca[2] * cb[2] );
	expectT1( OslColor::eSub,       ca[0] - cb[0], ca[1] - cb[1], ca[2] - cb[2] );
	expectT1( OslColor::eScreen,
		1.0 - ( 1.0 - ca[0] ) * ( 1.0 - cb[0] ),
		1.0 - ( 1.0 - ca[1] ) * ( 1.0 - cb[1] ),
		1.0 - ( 1.0 - ca[2] ) * ( 1.0 - cb[2] ) );
	expectT1( OslColor::eDiv,       ca[0] / cb[0], ca[1] / cb[1], ca[2] / cb[2] );
	expectT1( OslColor::eDiff,      std::fabs( ca[0] - cb[0] ), std::fabs( ca[1] - cb[1] ), std::fabs( ca[2] - cb[2] ) );
	expectT1( OslColor::eDark,      std::fmin( ca[0], cb[0] ), std::fmin( ca[1], cb[1] ), std::fmin( ca[2], cb[2] ) );
	expectT1( OslColor::eLight,     std::fmax( ca[0], cb[0] ), std::fmax( ca[1], cb[1] ), std::fmax( ca[2], cb[2] ) );
	expectT1( OslColor::eOverlay,
		2.0 * ca[0] * cb[0],
		1.0 - 2.0 * ( 1.0 - ca[1] ) * ( 1.0 - cb[1] ),
		1.0 - 2.0 * ( 1.0 - ca[2] ) * ( 1.0 - cb[2] ) );	// ca[2] == 0.5 takes the >= 0.5 branch
	expectT1( OslColor::eDodge,
		std::fmin( 1.0, ca[0] / ( 1.0 - cb[0] ) ),
		std::fmin( 1.0, ca[1] / ( 1.0 - cb[1] ) ),
		std::fmin( 1.0, ca[2] / ( 1.0 - cb[2] ) ) );
	expectT1( OslColor::eBurn,
		std::fmax( 0.0, 1.0 - ( 1.0 - ca[0] ) / cb[0] ),
		std::fmax( 0.0, 1.0 - ( 1.0 - ca[1] ) / cb[1] ),
		std::fmax( 0.0, 1.0 - ( 1.0 - ca[2] ) / cb[2] ) );
	// Pegtop Soft Light: (1 - 2b)*a^2 + 2ab
	expectT1( OslColor::eSoft,
		( 1.0 - 2.0 * cb[0] ) * ca[0] * ca[0] + 2.0 * ca[0] * cb[0],
		( 1.0 - 2.0 * cb[1] ) * ca[1] * ca[1] + 2.0 * ca[1] * cb[1],
		( 1.0 - 2.0 * cb[2] ) * ca[2] * ca[2] + 2.0 * ca[2] * cb[2] );
	expectT1( OslColor::eLinear,
		ca[0] + 2.0 * cb[0] - 1.0,
		ca[1] + 2.0 * cb[1] - 1.0,
		ca[2] + 2.0 * cb[2] - 1.0 );
	expectT1( OslColor::eExclusion,
		std::fmax( 0.0, ca[0] + cb[0] - 2.0 * ca[0] * cb[0] ),
		std::fmax( 0.0, ca[1] + cb[1] - 2.0 * ca[1] * cb[1] ),
		std::fmax( 0.0, ca[2] + cb[2] - 2.0 * ca[2] * cb[2] ) );

	// HSV blend modes at t = 1: verify exact HSV channel substitution.
	{
		double ha[3], hb[3], out[3], ho[3];
		OslColor::RgbToHsv( ca, ha );
		OslColor::RgbToHsv( cb, hb );

		OslColor::MixColor( OslColor::eHue, 1.0, ca, cb, false, false, out );
		OslColor::RgbToHsv( out, ho );
		const double expHue[3] = { hb[0], ha[1], ha[2] };
		Check( Near3( ho, expHue, 1e-5 ), "t=1 Hue takes H from b and S,V from a" );

		OslColor::MixColor( OslColor::eSat, 1.0, ca, cb, false, false, out );
		OslColor::RgbToHsv( out, ho );
		const double expSat[3] = { ha[0], hb[1], ha[2] };
		Check( Near3( ho, expSat, 1e-5 ), "t=1 Sat takes S from b and H,V from a" );

		OslColor::MixColor( OslColor::eVal, 1.0, ca, cb, false, false, out );
		OslColor::RgbToHsv( out, ho );
		const double expVal[3] = { ha[0], ha[1], hb[2] };
		Check( Near3( ho, expVal, 1e-5 ), "t=1 Val takes V from b and H,S from a" );

		OslColor::MixColor( OslColor::eCol, 1.0, ca, cb, false, false, out );
		OslColor::RgbToHsv( out, ho );
		const double expCol[3] = { hb[0], hb[1], ha[2] };
		Check( Near3( ho, expCol, 1e-5 ), "t=1 Color takes H,S from b and V from a" );
	}

	// Clamp options: clamp_factor and clamp_result.
	{
		double rUnclamped[3], rClampF[3], rClampR[3];
		OslColor::MixColor( OslColor::eBlend, 2.0, ca, cb, false, false, rUnclamped );
		const double expExt[3] = {
			ca[0] + 2.0 * ( cb[0] - ca[0] ),
			ca[1] + 2.0 * ( cb[1] - ca[1] ),
			ca[2] + 2.0 * ( cb[2] - ca[2] )
		};
		Check( Near3( rUnclamped, expExt, 1e-6 ), "clampFactor=false extrapolates at t=2" );

		OslColor::MixColor( OslColor::eBlend, 2.0, ca, cb, true, false, rClampF );
		Check( Near3( rClampF, cb, 1e-6 ), "clampFactor=true clamps t=2 to t=1" );

		OslColor::MixColor( OslColor::eBlend, 2.0, ca, cb, false, true, rClampR );
		const double expClamped[3] = { 1.0, 0.05, 0.0 };
		Check( Near3( rClampR, expClamped, 1e-6 ), "clampResult=true clamps channels to [0, 1]" );
	}

	// Out-of-[0, 1] a at t = 0 with clampResult = false: Burn clamps to [0, 1],
	// Dodge clamps above 1 (keeping < 0 negative), Exclusion clamps below 0.
	{
		const double outA[3] = { -0.5, 0.5, 1.5 };
		double r[3];
		OslColor::MixColor( OslColor::eBurn, 0.0, outA, cb, false, false, r );
		const double expBurn0[3] = { 0.0, 0.5, 1.0 };
		Check( Near3( r, expBurn0, 1e-6 ), "t=0 Burn clamps out-of-range a to [0, 1]" );

		OslColor::MixColor( OslColor::eDodge, 0.0, outA, cb, false, false, r );
		const double expDodge0[3] = { -0.5, 0.5, 1.0 };
		Check( Near3( r, expDodge0, 1e-6 ), "t=0 Dodge clamps a>1 to 1 and keeps a<0" );

		OslColor::MixColor( OslColor::eExclusion, 0.0, outA, cb, false, false, r );
		const double expExcl0[3] = { 0.0, 0.5, 1.5 };
		Check( Near3( r, expExcl0, 1e-6 ), "t=0 Exclusion clamps a<0 to 0 and keeps a>1" );
	}

	// Mode-specific edge branches (independent of Cycles):
	{
		double r[3];
		// Dodge: c1 == 0 stays 0 even when 1 - t*c2 <= 0; c1 != 0 with 1 - t*c2 <= 0 -> 1.
		const double dA[3] = { 0.0, 0.25, 0.8 };
		const double dB[3] = { 1.0, 1.0, 0.5 };
		OslColor::MixColor( OslColor::eDodge, 1.0, dA, dB, false, false, r );
		const double expDodge[3] = { 0.0, 1.0, 1.0 };
		Check( Near3( r, expDodge, 1e-6 ), "Dodge c1==0->0 and 1-t*c2<=0 / overflow->1" );

		// Burn: tm + t*c2 <= 0 -> 0; tmp > 1 (c1 > 1) -> 1; tmp < 0 -> 0.
		const double bA[3] = { 0.8, 1.5, 0.1 };
		const double bB[3] = { 0.0, 0.5, 0.5 };
		OslColor::MixColor( OslColor::eBurn, 1.0, bA, bB, false, false, r );
		const double expBurn[3] = { 0.0, 1.0, 0.0 };
		Check( Near3( r, expBurn, 1e-6 ), "Burn zero-denom->0, tmp>1->1, tmp<0->0" );

		// Div: c2 == 0 preserves c1.
		const double divB[3] = { 0.0, 0.5, 0.0 };
		OslColor::MixColor( OslColor::eDiv, 1.0, ca, divB, false, false, r );
		const double expDiv[3] = { ca[0], ca[1] / 0.5, ca[2] };
		Check( Near3( r, expDiv, 1e-6 ), "Div preserves c1 where c2 == 0" );

		// Exclusion: negative target clamps to 0.
		const double exA[3] = { 2.0, 0.5, 0.25 };
		const double exB[3] = { 2.0, 0.5, 0.25 };
		OslColor::MixColor( OslColor::eExclusion, 1.0, exA, exB, false, false, r );
		const double expExcl[3] = { 0.0, 0.5, 0.375 };
		Check( Near3( r, expExcl, 1e-6 ), "Exclusion clamps negative result to 0" );

		// Hue/Color: achromatic col2 (hsv2.y == 0) leaves col1 unchanged;
		// Sat: achromatic col1 (hsv.y == 0) leaves col1 unchanged.
		const double gray[3] = { 0.4, 0.4, 0.4 };
		OslColor::MixColor( OslColor::eHue, 1.0, ca, gray, false, false, r );
		Check( Near3( r, ca, 1e-6 ), "Hue with achromatic b returns a unchanged" );
		OslColor::MixColor( OslColor::eCol, 1.0, ca, gray, false, false, r );
		Check( Near3( r, ca, 1e-6 ), "Color with achromatic b returns a unchanged" );
		OslColor::MixColor( OslColor::eSat, 1.0, gray, cb, false, false, r );
		Check( Near3( r, gray, 1e-6 ), "Sat with achromatic a returns a unchanged" );
	}

	// Aliasing safety: in-place output matches out-of-place bit for bit.
	{
		double ref[3], aliased[3];
		for( int op = 0; op < 4; ++op ) {
			RunConv( op, ca, ref );
			aliased[0] = ca[0]; aliased[1] = ca[1]; aliased[2] = ca[2];
			RunConv( op, aliased, aliased );
			Check( Same( ref[0], aliased[0] ) && Same( ref[1], aliased[1] ) && Same( ref[2], aliased[2] ),
				std::string( "aliasing safety for " ) + kConvName[op] );
		}
		OslColor::MixColor( OslColor::eSoft, 0.6, ca, cb, true, false, ref );
		aliased[0] = ca[0]; aliased[1] = ca[1]; aliased[2] = ca[2];
		OslColor::MixColor( OslColor::eSoft, 0.6, aliased, cb, true, false, aliased );
		Check( Same( ref[0], aliased[0] ) && Same( ref[1], aliased[1] ) && Same( ref[2], aliased[2] ),
			"aliasing safety for MixColor (out aliases a)" );
		aliased[0] = cb[0]; aliased[1] = cb[1]; aliased[2] = cb[2];
		OslColor::MixColor( OslColor::eSoft, 0.6, ca, aliased, true, false, aliased );
		Check( Same( ref[0], aliased[0] ) && Same( ref[1], aliased[1] ) && Same( ref[2], aliased[2] ),
			"aliasing safety for MixColor (out aliases b)" );
	}
}

//////////////////////////////////////////////////////////////////////
// 3. The VM: compiled expression text equals the kernel bit for bit
//////////////////////////////////////////////////////////////////////

struct Prog
{
	ExpressionProgram prog;
	ExpressionProgram::Builder builder;
	bool ok;
	explicit Prog( const std::string& expr, const bool allowContextVars = true )
		: prog( ExpressionProgram::Invalid() )
	{
		builder.EnableContextVars( allowContextVars );
		ok = builder.Finalize( expr, prog );
	}
};

static bool EvalExprVec3( const std::string& expr, Vector3& out,
	const bool allowContextVars = true )
{
	Prog pr( expr, allowContextVars );
	if( !pr.ok ) {
		std::printf( "    compile failed (%s): %s\n", expr.c_str(), pr.builder.Error().c_str() );
		return false;
	}
	ExprEvalContext ctx;
	ctx.u = 0.25; ctx.v = 0.75;
	ctx.P = Vector3( 0.2, 0.5, 0.8 );
	ctx.Po = Vector3( 0.9, 0.3, 0.1 );
	ctx.N = Vector3( 0.0, 1.0, 0.0 );
	out = pr.prog.EvalVec3( ctx );
	return true;
}

static void TestVM()
{
	std::printf( "Test 3: VM execution equals OslColor kernel bit for bit\n" );

	const double p[3] = { 0.2, 0.5, 0.8 };
	const double po[3] = { 0.9, 0.3, 0.1 };
	const char* const kBuiltinConv[4] = { "osl_rgb2hsv", "osl_hsv2rgb", "osl_rgb2hsl", "osl_hsl2rgb" };
	for( int op = 0; op < 4; ++op ) {
		Vector3 vm;
		const bool okCompile = EvalExprVec3( std::string( kBuiltinConv[op] ) + "(P)", vm );
		double ref[3];
		RunConv( op, p, ref );
		Check( okCompile && Same( vm.x, ref[0] ) && Same( vm.y, ref[1] ) && Same( vm.z, ref[2] ),
			std::string( "VM " ) + kBuiltinConv[op] + "(P)" );
	}

	// All 19 Mix modes x all 4 clamp pairs through the VM.
	for( int mode = 0; mode < OslColor::kMixModeCount; ++mode ) {
		for( int cf = 0; cf <= 1; ++cf ) {
			for( int cr = 0; cr <= 1; ++cr ) {
				const std::string expr = "osl_mix_color(" + std::to_string( mode ) +
					", 1.25, P, Po, " + std::to_string( cf ) + ", " + std::to_string( cr ) + ")";
				Vector3 vm;
				const bool okCompile = EvalExprVec3( expr, vm );
				double ref[3];
				OslColor::MixColor( mode, 1.25, p, po, cf != 0, cr != 0, ref );
				Check( okCompile && Same( vm.x, ref[0] ) && Same( vm.y, ref[1] ) && Same( vm.z, ref[2] ),
					std::string( "VM osl_mix_color " ) + kModeName[mode] +
					" cf=" + std::to_string( cf ) + " cr=" + std::to_string( cr ) );
			}
		}
	}

	// Sign of zero survives the VM (e.g. MixColor Dodge preserves -0 in a).
	{
		Prog pr( "osl_mix_color(10, 0.5, vec3(-0.0, 0.5, 0.0), vec3(0.2, 0.3, 0.4), 0, 0)" );
		ExprEvalContext ctx;
		const Vector3 v = pr.ok ? pr.prog.EvalVec3( ctx ) : Vector3( 1, 1, 1 );
		Check( pr.ok && Same( v.x, -0.0 ) && Same( v.z, 0.0 ),
			"osl_mix_color Dodge preserves -0 through the VM" );
	}

	// Pure expression (allowContextVars = false) compiles and runs.
	{
		Vector3 vm;
		const bool okCompile = EvalExprVec3(
			"osl_mix_color(9, 0.5, osl_rgb2hsv(vec3(0.2,0.6,0.4)), osl_hsl2rgb(vec3(0.3,0.8,0.5)), 1, 1)",
			vm, false );
		double a[3], b[3], ref[3];
		const double c1[3] = { 0.2, 0.6, 0.4 };
		const double c2[3] = { 0.3, 0.8, 0.5 };
		OslColor::RgbToHsv( c1, a );
		OslColor::HslToRgb( c2, b );
		OslColor::MixColor( 9, 0.5, a, b, true, true, ref );
		Check( okCompile && Same( vm.x, ref[0] ) && Same( vm.y, ref[1] ) && Same( vm.z, ref[2] ),
			"pure-expression nested osl_ colour/mix call (allowContextVars=false)" );
	}

	// Composition with def and .x/.y/.z swizzles.
	{
		ExprEvalContext ctx;
		ctx.P = Vector3( 0.2, 0.5, 0.8 );
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddDef( "hsv", "osl_rgb2hsv(P)" ), "def with osl_rgb2hsv" );
		Check( b.Finalize( "hsv.x + osl_hsv2rgb(hsv).z", prog ), "final expression over hsv def: " + b.Error() );
		double hsv[3], back[3];
		OslColor::RgbToHsv( p, hsv );
		OslColor::HsvToRgb( hsv, back );
		Check( Same( prog.Eval( ctx ), hsv[0] + back[2] ), "composed def + swizzle matches kernel" );
	}
}

//////////////////////////////////////////////////////////////////////
// 4. Compile-time checks: literals, ranges, arity, types
//////////////////////////////////////////////////////////////////////

static void ExpectError( const std::string& expr, const std::string& fragment )
{
	Prog pr( expr );
	const std::string err = pr.builder.Error();
	Check( !pr.ok && err.find( fragment ) != std::string::npos,
		"`" + expr + "` refused with \"" + fragment + "\" (got: " + ( pr.ok ? std::string( "compiled" ) : err ) + ")" );
}

static void TestCompileChecks()
{
	std::printf( "Test 4: compile-time argument, literal, range and type checks\n" );

	// Valid extremes.
	Check( Prog( "osl_rgb2hsv(P)" ).ok, "valid osl_rgb2hsv" );
	Check( Prog( "osl_hsv2rgb(P)" ).ok, "valid osl_hsv2rgb" );
	Check( Prog( "osl_rgb2hsl(P)" ).ok, "valid osl_rgb2hsl" );
	Check( Prog( "osl_hsl2rgb(P)" ).ok, "valid osl_hsl2rgb" );
	Check( Prog( "osl_mix_color(0, u, P, Po, 0, 0)" ).ok, "valid osl_mix_color min literals" );
	Check( Prog( "osl_mix_color(18, u, P, Po, 1, 1)" ).ok, "valid osl_mix_color max literals" );
	Check( Prog( "osl_rgb2hsv(P)" ).prog.ResultType() == ExpressionProgram::kVec3, "osl_rgb2hsv returns vec3" );
	Check( Prog( "osl_mix_color(0, u, P, Po, 0, 0)" ).prog.ResultType() == ExpressionProgram::kVec3, "osl_mix_color returns vec3" );
	Check( Prog( "osl_rgb2hsv(P).x" ).prog.ResultType() == ExpressionProgram::kScalar, "vec3 return allows .x swizzle" );

	// Arity errors.
	ExpectError( "osl_rgb2hsv()", "expects 1 argument(s)" );
	ExpectError( "osl_rgb2hsv(P, P)", "expects 1 argument(s)" );
	ExpectError( "osl_hsv2rgb()", "expects 1 argument(s)" );
	ExpectError( "osl_rgb2hsl(P, 1)", "expects 1 argument(s)" );
	ExpectError( "osl_hsl2rgb(P, 1)", "expects 1 argument(s)" );
	ExpectError( "osl_mix_color(0, u, P, Po, 1)", "expects 6 argument(s)" );
	ExpectError( "osl_mix_color(0, u, P, Po, 1, 0, 0)", "expects 6 argument(s)" );

	// Type errors.
	ExpectError( "osl_rgb2hsv(u)", "argument 1 must be vec3" );
	ExpectError( "osl_hsv2rgb(0.5)", "argument 1 must be vec3" );
	ExpectError( "osl_rgb2hsl(u)", "argument 1 must be vec3" );
	ExpectError( "osl_hsl2rgb(u)", "argument 1 must be vec3" );
	ExpectError( "osl_mix_color(0, P, P, Po, 1, 0)", "argument 2 must be scalar" );
	ExpectError( "osl_mix_color(0, u, u, Po, 1, 0)", "argument 3 must be vec3" );
	ExpectError( "osl_mix_color(0, u, P, u, 1, 0)", "argument 4 must be vec3" );

	// Non-literal or out-of-range integer-literal slots.
	ExpectError( "osl_mix_color(u, u, P, Po, 1, 0)", "argument 1 must be an integer literal from 0 to 18" );
	ExpectError( "osl_mix_color(1 + 1, u, P, Po, 1, 0)", "argument 1 must be an integer literal from 0 to 18" );
	ExpectError( "osl_mix_color(-1, u, P, Po, 1, 0)", "argument 1 must be an integer literal from 0 to 18" );
	ExpectError( "osl_mix_color(19, u, P, Po, 1, 0)", "argument 1 must be an integer literal from 0 to 18" );
	ExpectError( "osl_mix_color(1.5, u, P, Po, 1, 0)", "argument 1 must be an integer literal from 0 to 18" );
	ExpectError( "osl_mix_color(0, u, P, Po, u, 0)", "argument 5 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, -1, 0)", "argument 5 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, 2, 0)", "argument 5 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, 0.5, 0)", "argument 5 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, 1, u)", "argument 6 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, 1, -1)", "argument 6 must be an integer literal from 0 to 1" );
	ExpectError( "osl_mix_color(0, u, P, Po, 1, 2)", "argument 6 must be an integer literal from 0 to 1" );

	// A param is refused in an integer-literal slot.
	{
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		b.AddParam( "m", 0.0 );
		const bool ok = b.Finalize( "osl_mix_color(m, u, P, Po, 1, 0)", prog );
		Check( !ok && b.Error().find( "argument 1 must be an integer literal from 0 to 18" ) != std::string::npos,
			"param refused in mode literal slot" );
	}
}

//////////////////////////////////////////////////////////////////////
// 5. Robustness: out-of-range MixMode, VM Finite() guard on NaN/inf
//////////////////////////////////////////////////////////////////////

static void TestRobustness()
{
	std::printf( "Test 5: robustness (out-of-range MixMode and VM Finite() boundary)\n" );

	const double ca[3] = { 0.3, 0.6, 0.9 };
	const double cb[3] = { 0.1, 0.4, 0.7 };
	for( const int bad : { -1, 19, 100 } ) {
		double out[3] = { 9.0, 9.0, 9.0 };
		OslColor::MixColor( bad, 0.5, ca, cb, true, true, out );
		Check( out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0,
			"MixColor out-of-range mode " + std::to_string( bad ) + " writes (0,0,0)" );
	}

	// Non-finite kernel outputs become 0 at the VM boundary (Finite()),
	// while finite sibling channels are preserved.
	{
		// rgb2hsl(0.5, 0, -0.5) has cmax + cmin == 0 with cmax != cmin, so
		// s = +inf in the kernel while h and l are finite.
		const double edge[3] = { 0.5, 0.0, -0.5 };
		double kOut[3];
		OslColor::RgbToHsl( edge, kOut );
		Check( IsInfExact( kOut[1] ) && IsFiniteExact( kOut[0] ) && IsFiniteExact( kOut[2] ),
			"kernel rgb2hsl(0.5,0,-0.5) produces +inf s as Cycles does" );

		Vector3 vm;
		const bool okCompile = EvalExprVec3( "osl_rgb2hsl(vec3(0.5, 0.0, -0.5))", vm );
		Check( okCompile && Same( vm.x, kOut[0] ) && vm.y == 0.0 && Same( vm.z, kOut[2] ),
			"VM osl_rgb2hsl maps +inf s to 0 and preserves finite h, l" );
	}

	// NaN and inf fed via ExprEvalContext into all 5 VM builtins never
	// leak a non-finite output.
	{
		const double kNan = std::numeric_limits<double>::quiet_NaN();
		const double kInf = std::numeric_limits<double>::infinity();
		const char* const kExprs[] = {
			"osl_rgb2hsv(P)",
			"osl_hsv2rgb(P)",
			"osl_rgb2hsl(P)",
			"osl_hsl2rgb(P)",
			"osl_mix_color(5, u, P, Po, 0, 0)",
			"osl_mix_color(12, u, P, Po, 0, 0)",
			"osl_mix_color(10, u, P, Po, 0, 0)",
			"osl_mix_color(11, u, P, Po, 0, 0)",
		};
		for( int i = 0; i < COUNT( kExprs ); ++i ) {
			Prog pr( kExprs[i] );
			ExprEvalContext ctx;
			ctx.u = kNan;
			ctx.P = Vector3( kNan, kInf, -kInf );
			ctx.Po = Vector3( kInf, kNan, 0.0 );
			const Vector3 v = pr.ok ? pr.prog.EvalVec3( ctx ) : Vector3( kNan, kNan, kNan );
			Check( pr.ok && IsFiniteExact( v.x ) && IsFiniteExact( v.y ) && IsFiniteExact( v.z ),
				std::string( "VM finite output on NaN/inf inputs for " ) + kExprs[i] );
		}
	}

	// Signature table sanity: Find, FindById, ScalarArity.
	{
		const OslBuiltins::Sig* s = OslBuiltins::Find( "osl_mix_color" );
		Check( s != 0 && OslBuiltins::FindById( s->id ) == s && OslBuiltins::ScalarArity( *s ) == 10,
			"OslBuiltins::Find/FindById/ScalarArity for osl_mix_color" );
		const OslBuiltins::Sig* c = OslBuiltins::Find( "osl_rgb2hsv" );
		Check( c != 0 && OslBuiltins::FindById( c->id ) == c && OslBuiltins::ScalarArity( *c ) == 3,
			"OslBuiltins::Find/FindById/ScalarArity for osl_rgb2hsv" );
	}
}

//////////////////////////////////////////////////////////////////////
// 6. Memo-worthiness
//////////////////////////////////////////////////////////////////////

static void TestMemoWorthiness()
{
	std::printf( "Test 6: colour and Mix builtins are not marked memo-worthy\n" );
	const char* const kCheap[] = {
		"osl_rgb2hsv(P)",
		"osl_hsv2rgb(P)",
		"osl_rgb2hsl(P)",
		"osl_hsl2rgb(P)",
		"osl_mix_color(12, u, P, Po, 1, 0)",
	};
	for( int i = 0; i < COUNT( kCheap ); ++i ) {
		Prog pr( kCheap[i] );
		Check( pr.ok && !pr.prog.MemoWorthy(), std::string( "not memo-worthy: " ) + kCheap[i] );
	}
	{
		Prog pr( "osl_mix_color(0, osl_snoise(P, 0, 3), P, Po, 1, 0)" );
		Check( pr.ok && pr.prog.MemoWorthy(), "osl_mix_color wrapping osl_snoise is memo-worthy" );
	}
}

int main()
{
	TestGoldenConversions();
	TestGoldenMix();
	TestClosedForms();
	TestVM();
	TestCompileChecks();
	TestRobustness();
	TestMemoWorthiness();

	std::printf( "\nOslColorTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
