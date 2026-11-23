//////////////////////////////////////////////////////////////////////
//
//  OslNoiseTest.cpp - The `osl_` noise builtins against Cycles.
//
//  1. Golden values.  tests/data/osl_noise_golden.inc was dumped from
//     Cycles' own kernels (tools/osl_golden/dump_noise_golden.sh): the
//     lookup3 hashes, the White Noise node, snoise_1d..4d, every fractal
//     type in every dimension, and the Noise Texture node's Fac and Color.
//     The hashes and White Noise are integer arithmetic, so they carry one
//     value each (the dumper checks that its three builds agree on it).
//     Each noise output
//     comes from three builds of Cycles' sources: its SIMD path and its
//     scalar "standard implementation" as Blender builds them, and the
//     scalar path as strict IEEE single precision -- the arithmetic
//     RISE's port reproduces on every platform.  The hashes must match
//     exactly; the noise is gated against the strict-IEEE build, and its
//     distance to the two Blender builds is printed.
//  2. The VM.  Each builtin compiled from expression text returns
//     exactly what the kernel returns for the same arguments (the VM
//     only moves the values), for scalar and vec3 results, from every
//     coordinate layout the dimension count selects.
//  3. Compile-time checks: literal arguments, ranges, arity, types.
//  4. Robustness: non-finite coordinates, a runaway detail, NaN detail.
//     (White Noise has no such guard: like Cycles it hashes any bit
//     pattern, and Test 1f covers its infinities.)
//  5. The memo gate sees the new opcodes.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OslNoise.h"
#include "../src/Library/Painters/ExpressionEval.h"

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
// The golden table's row types (the .inc only holds initialisers).
// Every output is given as { SIMD as built, scalar as built, scalar as
// strict IEEE }.
//////////////////////////////////////////////////////////////////////

struct OslHashRow { uint32_t kx, ky, kz, kw, h1, h2, h3, h4; };
struct OslFloatHashRow { double kx, ky, h1, h2; };
struct OslWhiteNoiseRow { int dims; double c[4]; double value; double color[3]; };
struct OslSNoiseRow { int dims; double c[4]; double simd, scalar, ieee; };
struct OslFractalRow { int dims; double c[4]; int type; double detail, roughness, lacunarity, offset, gain; int normalize; double simd, scalar, ieee; };
struct OslTextureRow { int dims; double c[4]; int type; double detail, roughness, lacunarity, offset, gain, distortion; int normalize; double simd[4]; double scalar[4]; double ieee[4]; };

#include "data/osl_noise_golden.inc"

#define COUNT( a ) ( (int)( sizeof( a ) / sizeof( ( a )[0] ) ) )

//////////////////////////////////////////////////////////////////////
// 1. Golden values
//////////////////////////////////////////////////////////////////////

// GATE.  RISE's kernels are strict IEEE single precision in the order of
// Cycles' scalar path (OslNoise.cpp), and so is the table's `ieee`
// column, so the two should agree EXACTLY -- the gate is equality.  A
// difference means a porting error, or a build that did not compile
// OslNoise.cpp strictly (see the enforcement comment in that file).
//
// AGAINST CYCLES WITH BLENDER'S FLAGS -- printed, not gated.  The other two
// columns are Cycles' sources rebuilt here with Blender's math flags
// (dump_noise_golden.sh), not a shipped Blender binary.  Blender's clang builds
// fuse multiply-adds where the hardware has FMA, and its SIMD path (what
// shipping builds run) groups the interpolation differently, so neither
// is strict IEEE.  The fractals and an undistorted Fac agree with the
// SIMD build to float rounding.  Outputs evaluated at shifted coordinates
// -- Color G/B and anything distorted -- amplify Cycles' rounding by
// roughly (roughness * lacunarity) per octave, to order one at roughness 1
// with high detail and more above it; that is the gap between a fused and
// a strict build of the same Cycles source.  The snoise rows' large SIMD
// distance is the SIMD path's 3D/4D wrap, which can stop matching fmodf above |p| of
// about 5.4e8 (first seen in this table at 1.1e9).  Errors are relative
// above 1, because hetero terrain and the multifractals grow past 1.
// Tests 1c-1e also print the one-ulp input sensitivity, and Test 1e the
// detail-15 roughness sweep OslNoise.h tabulates.

//! The roughness that splits the printed breakdowns.  The table stores
//! 0.6 as the float 0.6000000238, so the split sits just above it.
static const double kRoughSplit = 0.6 + 1e-6;

namespace
{
	struct Stats
	{
		double vsIeee, vsScalar, vsSimd, cyclesSpread;
		Stats() : vsIeee( 0 ), vsScalar( 0 ), vsSimd( 0 ), cyclesSpread( 0 ) {}
	};

	double RelErr( const double a, const double b ) { return std::fabs( a - b ) / std::fmax( 1.0, std::fabs( b ) ); }

	//! One output: equal to the strict-IEEE build (gated); distances to the
	//! two Blender builds (recorded).
	bool CheckGolden( const double got, const double simd, const double scalar, const double ieee, Stats& st, std::string& why )
	{
		st.vsIeee = std::fmax( st.vsIeee, RelErr( got, ieee ) );
		st.vsScalar = std::fmax( st.vsScalar, RelErr( got, scalar ) );
		st.vsSimd = std::fmax( st.vsSimd, RelErr( got, simd ) );
		st.cyclesSpread = std::fmax( st.cyclesSpread, RelErr( scalar, simd ) );
		char buf[160];
		std::snprintf( buf, sizeof buf, "got %a, strict-IEEE Cycles %a (|d| %.3g); Cycles scalar %.9g, SIMD %.9g",
			got, ieee, std::fabs( got - ieee ), scalar, simd );
		why = buf;
		return got == ieee;
	}

	void PrintStats( const char* what, const Stats& st )
	{
		std::printf( "  %s: vs strict-IEEE Cycles %.3g (gate: equal); vs Cycles' scalar build %.3g and SIMD build (Blender's flags) %.3g; those two differ by up to %.3g\n",
			what, st.vsIeee, st.vsScalar, st.vsSimd, st.cyclesSpread );
	}
}

static void TestHashes()
{
	std::printf( "Test 1a: lookup3 hashes and float hashes match Cycles exactly\n" );
	for( int i = 0; i < COUNT( kHashRows ); ++i ) {
		const OslHashRow& r = kHashRows[i];
		Check( OslNoise::HashUint( r.kx ) == r.h1, "hash_uint row " + std::to_string( i ) );
		Check( OslNoise::HashUint2( r.kx, r.ky ) == r.h2, "hash_uint2 row " + std::to_string( i ) );
		Check( OslNoise::HashUint3( r.kx, r.ky, r.kz ) == r.h3, "hash_uint3 row " + std::to_string( i ) );
		Check( OslNoise::HashUint4( r.kx, r.ky, r.kz, r.kw ) == r.h4, "hash_uint4 row " + std::to_string( i ) );
	}
	for( int i = 0; i < COUNT( kFloatHashRows ); ++i ) {
		const OslFloatHashRow& r = kFloatHashRows[i];
		Check( (double)OslNoise::HashFloatToFloat( (float)r.kx ) == r.h1, "hash_float_to_float row " + std::to_string( i ) );
		Check( (double)OslNoise::HashFloat2ToFloat( (float)r.kx, (float)r.ky ) == r.h2, "hash_float2_to_float row " + std::to_string( i ) );
	}
}

static void TestWhiteNoiseGolden()
{
	std::printf( "Test 1f: the White Noise node's Value and Color against Cycles, exactly (%d rows)\n", COUNT( kWhiteNoiseRows ) );
	for( int i = 0; i < COUNT( kWhiteNoiseRows ); ++i ) {
		const OslWhiteNoiseRow& r = kWhiteNoiseRows[i];
		Scalar value = -1, color[3] = { -1, -1, -1 };
		OslNoise::WhiteNoise( r.dims, r.c, value, color );
		char buf[200];
		std::snprintf( buf, sizeof buf, "white noise row %d (dims %d, c %a %a %a %a): Value %a want %a",
			i, r.dims, r.c[0], r.c[1], r.c[2], r.c[3], value, r.value );
		Check( value == r.value, buf );
		Check( color[0] == r.color[0] && color[1] == r.color[1] && color[2] == r.color[2],
			"white noise row " + std::to_string( i ) + ": Color matches Cycles" );
		Scalar valueOnly = -1;
		OslNoise::WhiteNoise( r.dims, r.c, valueOnly, 0 );
		Check( valueOnly == value && color[0] == value, "white noise row " + std::to_string( i ) + ": Value without Color is the same, and Color.r is Value" );
	}
	// +0 and -0 are different bit patterns, so they hash differently, as
	// in Cycles (both are rows of the table above).
	const Scalar pz[4] = { 0.0, 0, 0, 0 }, nz[4] = { -0.0, 0, 0, 0 };
	Scalar vz = 0, vn = 0;
	OslNoise::WhiteNoise( 1, pz, vz, 0 );
	OslNoise::WhiteNoise( 1, nz, vn, 0 );
	Check( vz != vn, "white noise distinguishes -0 from +0, as Cycles does" );
	// A dimension count outside 1..4 gives Cycles' fallback.
	Scalar vb = -1, cb[3] = { -1, -1, -1 };
	OslNoise::WhiteNoise( 5, pz, vb, cb );
	Check( vb == 0 && cb[0] == 1 && cb[1] == 0 && cb[2] == 1, "white noise with dims outside 1..4 gives Value 0, Color magenta" );
}

static void TestSNoiseGolden()
{
	std::printf( "Test 1b: snoise_1d..4d against Cycles (%d rows)\n", COUNT( kSNoiseRows ) );
	Stats st;
	double belowWrap = 0;	// strict vs the SIMD build where every |p| < 5e8
	for( int i = 0; i < COUNT( kSNoiseRows ); ++i ) {
		const OslSNoiseRow& r = kSNoiseRows[i];
		std::string why;
		const bool ok = CheckGolden( OslNoise::SNoise( r.dims, r.c ), r.simd, r.scalar, r.ieee, st, why );
		double m = 0;
		for( int k = 0; k < r.dims; ++k ) m = std::fmax( m, std::fabs( r.c[k] ) );
		if( m < 5.0e8 ) belowWrap = std::fmax( belowWrap, RelErr( r.ieee, r.simd ) );
		Check( ok, "snoise row " + std::to_string( i ) + " (dims " + std::to_string( r.dims ) + "): " + why );
	}
	PrintStats( "snoise", st );
	std::printf( "    strict vs SIMD build with every |p| < 5e8: %.2g\n", belowWrap );
}

static void TestFractalGolden()
{
	std::printf( "Test 1c: noise_select, every type and dimension, against Cycles (%d rows)\n", COUNT( kFractalRows ) );
	Stats st;
	// INPUT SENSITIVITY, printed for reference (OslNoise.h quotes Test 1e's
	// sweep only): how far the fractal moves
	// when one coordinate moves by one float ulp, relative to max(1, |v|),
	// by roughness (<= 0.6 or above) and type.  A render does not feed RISE
	// and Cycles bit-identical float coordinates (RISE rounds its double
	// position to float, Cycles computes it in float), so this is the size
	// of disagreement to expect from that alone.
	double sens[2][OslNoise::kFractalTypeCount] = {};
	for( int i = 0; i < COUNT( kFractalRows ); ++i ) {
		const OslFractalRow& r = kFractalRows[i];
		const double got = OslNoise::Fractal( r.dims, r.c, r.type, r.detail, r.roughness, r.lacunarity, r.offset, r.gain, r.normalize != 0 );
		for( int k = 0; k < r.dims; ++k ) {
			for( int dir = -1; dir <= 1; dir += 2 ) {
				Scalar c[4] = { r.c[0], r.c[1], r.c[2], r.c[3] };
				c[k] = (double)std::nextafter( (float)c[k], dir > 0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity() );
				const double moved = OslNoise::Fractal( r.dims, c, r.type, r.detail, r.roughness, r.lacunarity, r.offset, r.gain, r.normalize != 0 );
				double& sv = sens[ r.roughness > kRoughSplit ? 1 : 0 ][ r.type ];
				sv = std::fmax( sv, RelErr( moved, got ) );
			}
		}
		std::string why;
		const bool ok = CheckGolden( got, r.simd, r.scalar, r.ieee, st, why );
		char tag[120];
		std::snprintf( tag, sizeof tag, "fractal row %d (dims %d type %d detail %g roughness %g gain %g): ",
			i, r.dims, r.type, r.detail, r.roughness, r.gain );
		Check( ok, tag + why );
	}
	PrintStats( "fractal", st );
	static const char* kTypeName[OslNoise::kFractalTypeCount] = { "multifractal", "fBM", "hybrid", "ridged", "hetero" };
	for( int r1 = 0; r1 < 2; ++r1 ) {
		std::printf( "    one-float-ulp input sensitivity, roughness %s:", r1 ? " > 0.6 " : "<= 0.6" );
		for( int t = 0; t < OslNoise::kFractalTypeCount; ++t ) std::printf( "  %s %.2g", kTypeName[t], sens[r1][t] );
		std::printf( "\n" );
	}
}

//! Evaluate all four outputs of a texture row at coordinates `c`.
static void TextureOutputs( const OslTextureRow& r, const Scalar* c, double out[4] )
{
	Scalar fac = 0, color[3] = { 0, 0, 0 };
	OslNoise::NoiseTexture( r.dims, c, r.type, r.detail, r.roughness, r.lacunarity, r.offset, r.gain,
		r.distortion, r.normalize != 0, fac, color );
	out[0] = fac; out[1] = color[0]; out[2] = color[1]; out[3] = color[2];
}

//! One-float-ulp input sensitivity of each output of a texture row: the
//! largest relative change (against max(1, |v|)) when one coordinate
//! moves by one float ulp up or down.
static void TextureSensitivity( const OslTextureRow& r, const double base[4], double sens[4] )
{
	for( int ch = 0; ch < 4; ++ch ) sens[ch] = 0;
	for( int k = 0; k < r.dims; ++k ) {
		for( int dir = -1; dir <= 1; dir += 2 ) {
			Scalar c[4] = { r.c[0], r.c[1], r.c[2], r.c[3] };
			c[k] = (double)std::nextafter( (float)c[k], dir > 0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity() );
			double moved[4];
			TextureOutputs( r, c, moved );
			for( int ch = 0; ch < 4; ++ch ) sens[ch] = std::fmax( sens[ch], RelErr( moved[ch], base[ch] ) );
		}
	}
}

//! Output `ch` of a row is evaluated at a SHIFTED point: Color G/B (offset
//! by 100..200), or any output once the point is distorted.
static bool ShiftedOutput( const OslTextureRow& r, const int ch ) { return ch >= 2 || r.distortion != 0; }

static void TestTextureGolden()
{
	std::printf( "Test 1d: the Noise Texture node's Fac and Color against Cycles (%d rows)\n", COUNT( kTextureRows ) );
	Stats st;
	// A breakdown printed for reference (OslNoise.h quotes Test 1e's sweep
	// only): |strict - SIMD build| relative to max(1, |value|), by output
	// (Fac at the unshifted point, or an output evaluated at a SHIFTED
	// point: Color G/B, or anything distorted), by roughness (<= 0.6 or
	// above; kRoughSplit), by fractal type; plus the absolute maximum of
	// the shifted outputs at roughness <= 0.6.
	double brk[2][2][OslNoise::kFractalTypeCount] = {};
	double shiftedAbsModerate = 0;
	double sensBrk[2][2] = { { 0, 0 }, { 0, 0 } };	// [shifted][roughness > 0.6]
	for( int i = 0; i < COUNT( kTextureRows ); ++i ) {
		const OslTextureRow& r = kTextureRows[i];
		Scalar fac = 0, color[3] = { 0, 0, 0 };
		OslNoise::NoiseTexture( r.dims, r.c, r.type, r.detail, r.roughness, r.lacunarity, r.offset, r.gain,
			r.distortion, r.normalize != 0, fac, color );
		const double got[4] = { fac, color[0], color[1], color[2] };
		double sens[4];
		TextureSensitivity( r, got, sens );
		for( int ch = 0; ch < 4; ++ch ) {
			const int shifted = ShiftedOutput( r, ch ) ? 1 : 0;
			double& sb = sensBrk[shifted][ r.roughness > kRoughSplit ? 1 : 0 ];
			sb = std::fmax( sb, sens[ch] );
			const int rough1 = ( r.roughness > kRoughSplit ) ? 1 : 0;
			double& b = brk[shifted][rough1][r.type];
			b = std::fmax( b, RelErr( r.ieee[ch], r.simd[ch] ) );
			if( shifted && !rough1 ) shiftedAbsModerate = std::fmax( shiftedAbsModerate, std::fabs( r.ieee[ch] - r.simd[ch] ) );
			std::string why;
			const bool ok = CheckGolden( got[ch], r.simd[ch], r.scalar[ch], r.ieee[ch], st, why );
			char tag[160];
			std::snprintf( tag, sizeof tag, "texture row %d output %d (dims %d type %d detail %g roughness %g distortion %g): ",
				i, ch, r.dims, r.type, r.detail, r.roughness, r.distortion );
			Check( ok, tag + why );
		}
		// Fac-only evaluation is the same Fac, and Color.r is Fac.
		Scalar facOnly = 0;
		OslNoise::NoiseTexture( r.dims, r.c, r.type, r.detail, r.roughness, r.lacunarity, r.offset, r.gain,
			r.distortion, r.normalize != 0, facOnly, 0 );
		Check( facOnly == fac, "texture row " + std::to_string( i ) + ": Fac without Color is the same Fac" );
		Check( color[0] == fac, "texture row " + std::to_string( i ) + ": Color.r is Fac" );
	}
	PrintStats( "texture", st );
	static const char* kTypeName[OslNoise::kFractalTypeCount] = { "multifractal", "fBM", "hybrid", "ridged", "hetero" };
	for( int sh = 0; sh < 2; ++sh ) {
		for( int r1 = 0; r1 < 2; ++r1 ) {
			std::printf( "    strict vs SIMD build, %s, roughness %s:", sh ? "shifted outputs (Color G/B, distorted)" : "unshifted Fac", r1 ? " > 0.6" : "<= 0.6" );
			for( int t = 0; t < OslNoise::kFractalTypeCount; ++t ) std::printf( "  %s %.2g", kTypeName[t], brk[sh][r1][t] );
			std::printf( "\n" );
		}
	}
	std::printf( "    shifted outputs at roughness <= 0.6, absolute: %.2g\n", shiftedAbsModerate );
	std::printf( "    one-float-ulp input sensitivity: unshifted Fac %.2g (roughness <= 0.6) / %.2g (> 0.6); shifted outputs %.2g / %.2g\n",
		sensBrk[0][0], sensBrk[0][1], sensBrk[1][0], sensBrk[1][1] );
}

//////////////////////////////////////////////////////////////////////
// 1e. The roughness sweep at detail 15: gated exactly like the other
// tables, and the source of the figures OslNoise.h quotes.
//////////////////////////////////////////////////////////////////////

static void TestSweepGolden()
{
	std::printf( "Test 1e: the Noise Texture node at detail 15, lacunarity 2, by roughness (%d rows)\n", COUNT( kSweepRows ) );
	Stats st;
	const double roughs[] = { 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1 };
	const int nR = (int)( sizeof( roughs ) / sizeof( roughs[0] ) );
	double vsSimd[7][2] = {}, sensMax[7][2] = {};	// [roughness][shifted]
	for( int i = 0; i < COUNT( kSweepRows ); ++i ) {
		const OslTextureRow& r = kSweepRows[i];
		double got[4];
		TextureOutputs( r, r.c, got );
		double sens[4];
		TextureSensitivity( r, got, sens );
		int ri = 0;
		while( ri < nR - 1 && std::fabs( roughs[ri] - r.roughness ) > 1e-6 ) ++ri;
		Check( std::fabs( roughs[ri] - r.roughness ) <= 1e-6, "sweep row " + std::to_string( i ) + " has a roughness the sweep lists" );
		for( int ch = 0; ch < 4; ++ch ) {
			std::string why;
			const bool ok = CheckGolden( got[ch], r.simd[ch], r.scalar[ch], r.ieee[ch], st, why );
			Check( ok, "sweep row " + std::to_string( i ) + " output " + std::to_string( ch ) + ": " + why );
			const int sh = ShiftedOutput( r, ch ) ? 1 : 0;
			vsSimd[ri][sh] = std::fmax( vsSimd[ri][sh], RelErr( r.ieee[ch], r.simd[ch] ) );
			sensMax[ri][sh] = std::fmax( sensMax[ri][sh], sens[ch] );
		}
	}
	PrintStats( "sweep", st );
	std::printf( "    roughness | strict vs SIMD: unshifted Fac, shifted | one-ulp sensitivity: unshifted Fac, shifted\n" );
	for( int ri = 0; ri < nR; ++ri ) {
		std::printf( "    %9.1f | %10.2g  %8.2g | %10.2g  %8.2g\n",
			roughs[ri], vsSimd[ri][0], vsSimd[ri][1], sensMax[ri][0], sensMax[ri][1] );
	}
}

//////////////////////////////////////////////////////////////////////
// 2. The VM
//////////////////////////////////////////////////////////////////////

struct Prog
{
	ExpressionProgram prog;
	ExpressionProgram::Builder builder;
	bool ok;
	explicit Prog( const std::string& expr ) : prog( ExpressionProgram::Invalid() )
	{
		builder.EnableContextVars( true );
		ok = builder.Finalize( expr, prog );
	}
};

static std::string Num( const double x )
{
	char buf[64];
	std::snprintf( buf, sizeof buf, "%.17g", x );
	return buf;
}

static void TestVMMatchesKernels()
{
	std::printf( "Test 2: osl_ builtins through the VM equal the kernels exactly\n" );
	const Vector3 pts[] = { Vector3( 0.3, 1.7, -2.2 ), Vector3( -5.25, 0.125, 9.5 ), Vector3( 41.0, -13.5, 0.75 ) };
	const double wv[] = { 0.4, -2.75, 17.0 };
	for( int ip = 0; ip < 3; ++ip ) {
		ExprEvalContext ctx;
		ctx.P = pts[ip];
		const double w = wv[ip];
		for( int dims = 1; dims <= 4; ++dims ) {
			// The coordinates each dimension count reads (the header's table).
			Scalar c[4] = { 0, 0, 0, 0 };
			if( dims == 1 ) c[0] = w;
			else { c[0] = ctx.P.x; c[1] = ctx.P.y; if( dims >= 3 ) c[2] = ctx.P.z; if( dims == 4 ) c[3] = w; }
			const std::string tag = " (point " + std::to_string( ip ) + ", dims " + std::to_string( dims ) + ")";

			Prog wn( "osl_white_noise(P, " + Num( w ) + ", " + std::to_string( dims ) + ")" );
			Prog wc( "osl_white_noise_color(P, " + Num( w ) + ", " + std::to_string( dims ) + ")" );
			Check( wn.ok && wc.ok, "osl_white_noise[_color] compile" + tag );
			{
				Scalar value = 0, color[3] = { 0, 0, 0 };
				OslNoise::WhiteNoise( dims, c, value, color );
				Check( wn.ok && wn.prog.Eval( ctx ) == value, "osl_white_noise == WhiteNoise" + tag );
				const Vector3 vc = wc.ok ? wc.prog.EvalVec3( ctx ) : Vector3( 0, 0, 0 );
				Check( wc.ok && wc.prog.ResultType() == ExpressionProgram::kVec3 && vc.x == color[0] && vc.y == color[1] && vc.z == color[2],
					"osl_white_noise_color == WhiteNoise's Color" + tag );
			}
			Prog sn( "osl_snoise(P, " + Num( w ) + ", " + std::to_string( dims ) + ")" );
			Check( sn.ok, "osl_snoise compiles" + tag + ": " + sn.builder.Error() );
			Check( sn.ok && sn.prog.Eval( ctx ) == OslNoise::SNoise( dims, c ), "osl_snoise == SNoise" + tag );

			for( int type = 0; type < OslNoise::kFractalTypeCount; ++type ) {
				const std::string args = "P, " + Num( w ) + ", " + std::to_string( dims ) + ", " + std::to_string( type )
					+ ", 3.5, 0.55, 2.1, 0.8, 1.6";
				Prog fr( "osl_fractal(" + args + ", 1)" );
				Check( fr.ok, "osl_fractal compiles" + tag + ": " + fr.builder.Error() );
				Check( fr.ok && fr.prog.Eval( ctx ) == OslNoise::Fractal( dims, c, type, 3.5, 0.55, 2.1, 0.8, 1.6, true ),
					"osl_fractal == Fractal" + tag + " type " + std::to_string( type ) );

				Prog tx( "osl_noise_texture(" + args + ", 0.6, 0)" );
				Prog tc( "osl_noise_texture_color(" + args + ", 0.6, 0)" );
				Check( tx.ok && tc.ok, "osl_noise_texture[_color] compile" + tag );
				Scalar fac = 0, color[3] = { 0, 0, 0 };
				OslNoise::NoiseTexture( dims, c, type, 3.5, 0.55, 2.1, 0.8, 1.6, 0.6, false, fac, color );
				Check( tx.ok && tx.prog.Eval( ctx ) == fac, "osl_noise_texture == Fac" + tag + " type " + std::to_string( type ) );
				Check( tc.ok && tc.prog.ResultType() == ExpressionProgram::kVec3, "osl_noise_texture_color is vec3" + tag );
				const Vector3 vc = tc.ok ? tc.prog.EvalVec3( ctx ) : Vector3( 0, 0, 0 );
				Check( vc.x == color[0] && vc.y == color[1] && vc.z == color[2],
					"osl_noise_texture_color == Color" + tag + " type " + std::to_string( type ) );
			}
		}
	}

	// Composition: the result is an ordinary scalar / vec3 value.  A
	// distorted coordinate, a def, and arithmetic around the calls.
	ExprEvalContext ctx;
	ctx.P = Vector3( 0.7, -1.1, 2.4 );
	ExpressionProgram prog = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	Check( b.AddDef( "q", "P*5 + osl_noise_texture_color(P, 0, 3, 1, 2, 0.5, 2, 0, 1, 0, 1)" ), "def with an osl_ vec3 call" );
	Check( b.Finalize( "0.5 + 0.5*osl_snoise(q, 0, 3)", prog ), "final expression over the def: " + b.Error() );
	Scalar cc[4] = { 0, 0, 0, 0 }, col[3], fac;
	cc[0] = ctx.P.x; cc[1] = ctx.P.y; cc[2] = ctx.P.z;
	OslNoise::NoiseTexture( 3, cc, 1, 2, 0.5, 2, 0, 1, 0, true, fac, col );
	Scalar q[4] = { ctx.P.x * 5 + col[0], ctx.P.y * 5 + col[1], ctx.P.z * 5 + col[2], 0 };
	const double want = 0.5 + 0.5 * OslNoise::SNoise( 3, q );
	Check( std::fabs( prog.Eval( ctx ) - want ) <= 1e-15, "composed program matches the hand-composed kernels" );

	// An osl_ call inside fbm's position argument: the domain-scale
	// analysis treats it as a non-affine term and keeps compiling.
	Prog warp( "fbm(P*4 + vec3(osl_snoise(P, 0, 3), 0, 0), 4, 0.5, 2)" );
	Check( warp.ok, "fbm over an osl_-warped position compiles: " + warp.builder.Error() );
}

//////////////////////////////////////////////////////////////////////
// 3. Compile-time checks
//////////////////////////////////////////////////////////////////////

static void ExpectError( const std::string& expr, const std::string& fragment )
{
	Prog p( expr );
	const std::string err = p.builder.Error();
	Check( !p.ok && err.find( fragment ) != std::string::npos,
		"`" + expr + "` is refused with \"" + fragment + "\" (got: " + ( p.ok ? std::string( "compiled" ) : err ) + ")" );
}

static void TestCompileChecks()
{
	std::printf( "Test 3: literal, range, arity and type checks\n" );
	ExpectError( "osl_snoise(P, 0, 5)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_white_noise(P, 0, 0)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_white_noise_color(u, 0, 3)", "argument 1 must be vec3" );
	ExpectError( "osl_snoise(P, 0, 0)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_snoise(P, 0, 2.5)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_snoise(P, 0, u)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_snoise(P, 0, 1+2)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_fractal(P, 0, 3, 5, 2, 0.5, 2, 0, 1, 1)", "argument 4 must be an integer literal from 0 to 4" );
	ExpectError( "osl_fractal(P, 0, 3, 1, 2, 0.5, 2, 0, 1, 2)", "argument 10 must be an integer literal from 0 to 1" );
	ExpectError( "osl_snoise(P, 0)", "expects 3 argument(s)" );
	ExpectError( "osl_snoise(P, 0, 3, 1)", "expects 3 argument(s)" );
	ExpectError( "osl_snoise(u, 0, 3)", "argument 1 must be vec3" );
	ExpectError( "osl_snoise(P, P, 3)", "argument 2 must be scalar" );
	ExpectError( "osl_nope(P)", "unknown function `osl_nope`" );
	// A valid literal followed by stray text is a syntax error, and says so.
	ExpectError( "osl_snoise(P, 0, 3 4)", "missing `,` or `)` after argument 3" );
	ExpectError( "osl_snoise(P, 0, 3", "missing `,` or `)` after argument 3" );
	ExpectError( "osl_snoise(P, 0, 3+1)", "must be an integer literal from 1 to 4" );
	ExpectError( "osl_snoise(P, 0 4, 3)", "missing `,` or `)` after argument 2" );
	ExpectError( "osl_snoise(P, 0, 3,)", "unexpected `,` before `)`" );
	ExpectError( "osl_snoise(P, 0,)", "unexpected `,` before `)`" );
	ExpectError( "osl_snoise(P,)", "unexpected `,` before `)`" );
	{
		// The trailing-comma diagnostic points at the comma, at any position.
		Prog t1( "osl_snoise(P,)" ), t2( "osl_snoise(P, 0,)" ), t3( "osl_snoise(P, 0, 3,)" );
		Check( !t1.ok && t1.builder.ErrorOffset() == 12, "trailing comma after argument 1 is located at the comma" );
		Check( !t2.ok && t2.builder.ErrorOffset() == 15, "trailing comma after argument 2 is located at the comma" );
		Check( !t3.ok && t3.builder.ErrorOffset() == 18, "trailing comma after argument 3 is located at the comma" );
	}
	{
		// Both "missing `,`" paths point at the stray token.
		Prog lit( "osl_snoise(P, 0, 3 4)" ), gen( "osl_snoise(P 1, 0, 3)" );
		Check( !lit.ok && lit.builder.ErrorOffset() == 19, "the literal-slot diagnostic points at the stray `4`" );
		Check( !gen.ok && gen.builder.ErrorOffset() == 13, "the generic diagnostic points at the stray `1`" );
	}
	// A signed literal and a literal written as 3.0 are both one integer.
	Check( Prog( "osl_snoise(P, 0, 3.0)" ).ok, "3.0 is an integer literal" );
	Check( Prog( "osl_fractal(P, -1, 1, +1, 2, 0.5, 2, 0, 1, 0)" ).ok, "a signed literal is accepted" );
	// The builtins are pure: they compile where context vars are off.
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	Check( b.Finalize( "osl_snoise(vec3(u, v, 0), 0, 2)", p ), "osl_ builtins need no surface context: " + b.Error() );
}

//////////////////////////////////////////////////////////////////////
// 4. Robustness
//////////////////////////////////////////////////////////////////////

static void TestRobustness()
{
	std::printf( "Test 4: non-finite coordinates, runaway and NaN detail\n" );
	const double inf = std::numeric_limits<double>::infinity();
	const double nan = std::numeric_limits<double>::quiet_NaN();
	// A double beyond float range is infinite once rounded to float.
	{
		const Scalar huge3[4] = { 1e300, 0.2, 0.3, 0 };
		Check( OslNoise::SNoise( 3, huge3 ) == 0 && OslNoise::Fractal( 3, huge3, OslNoise::eFbm, 3, 0.5, 2, 0, 1, true ) == 0,
			"a coordinate beyond float range gives 0" );
	}
	const Scalar bad[3][4] = { { inf, 0, 0, 0 }, { 0, nan, 0, 0 }, { 0, 0, -inf, 1 } };
	for( int i = 0; i < 3; ++i ) {
		for( int dims = 1; dims <= 4; ++dims ) {
			const Scalar v = OslNoise::SNoise( dims, bad[i] );
			const bool readsBad = ( i == 0 ) || ( i == 1 && dims >= 2 ) || ( i == 2 && dims >= 3 );
			if( readsBad ) {
				const std::string tag = " (case " + std::to_string( i ) + ", dims " + std::to_string( dims ) + ")";
				Check( v == 0, "snoise: a non-finite coordinate gives 0" + tag );
				Check( OslNoise::Fractal( dims, bad[i], OslNoise::eHeteroTerrain, 4, 0.5, 2, 0.5, 1, true ) == 0,
					"fractal: a non-finite coordinate gives 0" + tag );
				Scalar fac = 1, col[3] = { 1, 1, 1 };
				OslNoise::NoiseTexture( dims, bad[i], OslNoise::eMultifractal, 4, 0.5, 2, 0.5, 1, 0.3, true, fac, col );
				Check( fac == 0 && col[0] == 0 && col[1] == 0 && col[2] == 0, "noise texture: a non-finite coordinate gives 0" + tag );
			} else {
				// The slot this dimension count does not read changes nothing.
				Scalar zeroed[4] = { bad[i][0], bad[i][1], bad[i][2], bad[i][3] };
				for( int k = dims; k < 4; ++k ) zeroed[k] = 0;
				Check( v == OslNoise::SNoise( dims, zeroed ), "an unread non-finite slot does not change the result" );
			}
		}
	}
	// Lacunarity large enough that the octave scale overflows float within
	// 15 octaves (1e20^2 > FLT_MAX): an overflowing octave's snoise reads 0,
	// so the result stays finite.  (1e300 itself would round to an
	// infinite lacunarity at the interface, a different path; it is checked
	// too.)
	const Scalar p[4] = { 0.3, 0.4, 0.5, 0.6 };
	for( int type = 0; type < OslNoise::kFractalTypeCount; ++type ) {
		Check( std::isfinite( OslNoise::Fractal( 3, p, type, 15, 0.5, 1e20, 0.5, 1, true ) ),
			"lacunarity 1e20 (octave scale overflows) stays finite, type " + std::to_string( type ) );
		Check( std::isfinite( OslNoise::Fractal( 3, p, type, 15, 0.5, 1e300, 0.5, 1, true ) ),
			"an infinite lacunarity stays finite, type " + std::to_string( type ) );
	}
	// A huge detail, +infinity included, is clamped to 15 (it must not
	// loop 1e9 times), and a NaN detail reads as 0.  (A very negative
	// detail is clamped to kMinDetail, which only keeps the int conversion
	// defined.)  The Noise Texture node's own clamp maps +infinity to 15
	// and NaN to 0 as Cycles' comparison clamp does.
	const Scalar huge = OslNoise::Fractal( 3, p, OslNoise::eFbm, 1e9, 0.5, 2, 0, 1, true );
	Check( huge == OslNoise::Fractal( 3, p, OslNoise::eFbm, 15, 0.5, 2, 0, 1, true ), "detail 1e9 evaluates as detail 15" );
	Check( OslNoise::Fractal( 3, p, OslNoise::eFbm, inf, 0.5, 2, 0, 1, true ) == OslNoise::Fractal( 3, p, OslNoise::eFbm, 15, 0.5, 2, 0, 1, true ),
		"a +infinity detail evaluates as detail 15" );
	{
		Scalar fInf = 0, f15 = 0;
		OslNoise::NoiseTexture( 3, p, OslNoise::eFbm, inf, 0.5, 2, 0, 1, 0, true, fInf, 0 );
		OslNoise::NoiseTexture( 3, p, OslNoise::eFbm, 15, 0.5, 2, 0, 1, 0, true, f15, 0 );
		Check( fInf == f15, "the Noise Texture node reads a +infinity detail as 15" );
	}
	Check( OslNoise::Fractal( 3, p, OslNoise::eFbm, nan, 0.5, 2, 0, 1, true ) == OslNoise::Fractal( 3, p, OslNoise::eFbm, 0, 0.5, 2, 0, 1, true ),
		"a NaN detail evaluates as detail 0" );
	Scalar fac = 0;
	OslNoise::NoiseTexture( 3, p, OslNoise::eFbm, nan, 0.5, 2, 0, 1, 0, true, fac, 0 );
	Scalar fac0 = 0;
	OslNoise::NoiseTexture( 3, p, OslNoise::eFbm, 0, 0.5, 2, 0, 1, 0, true, fac0, 0 );
	Check( fac == fac0, "the Noise Texture node reads a NaN detail as 0" );
	// Unknown dims / type give 0.
	Check( OslNoise::SNoise( 0, p ) == 0 && OslNoise::SNoise( 5, p ) == 0, "dims outside 1..4 give 0" );
	Check( OslNoise::Fractal( 3, p, 7, 2, 0.5, 2, 0, 1, true ) == 0, "an unknown fractal type gives 0" );
	// A non-finite RESULT reads 0 through the VM.  Negative roughness
	// makes fBM's normalising amplitude sum to exactly zero at detail 1
	// (1 + -1), as it does in Cycles.
	{
		const Scalar q[4] = { 0.3, 0.4, 0.5, 0 };
		Check( !std::isfinite( OslNoise::Fractal( 3, q, OslNoise::eFbm, 1, -1, 2, 0, 1, true ) ),
			"the kernel's own result for that case is not finite (so the VM check below is meaningful)" );
	}
	Prog div( "osl_fractal(P, 0, 3, 1, 1, -1, 2, 0, 1, 1)" );
	ExprEvalContext ctx;
	ctx.P = Vector3( 0.3, 0.4, 0.5 );
	Check( div.ok && div.prog.Eval( ctx ) == 0, "a non-finite fractal result reads 0 through the VM" );
}

//////////////////////////////////////////////////////////////////////
// 5. The memo gate
//////////////////////////////////////////////////////////////////////

static void TestMemoWorthiness()
{
	std::printf( "Test 5: an osl_ noise call makes a short body memo-worthy\n" );
	Check( Prog( "osl_snoise(P, 0, 3)" ).prog.MemoWorthy(), "osl_snoise" );
	Check( Prog( "osl_noise_texture_color(P, 0, 3, 1, 2, 0.5, 2, 0, 1, 0, 1).x" ).prog.MemoWorthy(), "osl_noise_texture_color" );
	Check( !Prog( "osl_white_noise(P, 0, 3)" ).prog.MemoWorthy(), "osl_white_noise alone is not (a few integer hashes)" );
	Check( !Prog( "u*v" ).prog.MemoWorthy(), "a short arithmetic body is still not" );
}

int main()
{
	TestHashes();
	TestWhiteNoiseGolden();
	TestSNoiseGolden();
	TestFractalGolden();
	TestTextureGolden();
	TestSweepGolden();
	TestVMMatchesKernels();
	TestCompileChecks();
	TestRobustness();
	TestMemoWorthiness();
	std::printf( "\nOslNoiseTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
