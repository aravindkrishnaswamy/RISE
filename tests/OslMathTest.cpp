//////////////////////////////////////////////////////////////////////
//
//  OslMathTest.cpp - Golden-value, closed-form, VM, compile-time,
//    robustness and memo-worthiness tests for OslMath.{h,cpp} and its
//    `osl_` expression VM builtins.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "../src/Library/Utilities/OslMath.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionOslBuiltins.h"

using namespace RISE;
using namespace RISE::OslMath;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check( const bool cond, const std::string& msg )
	{
		if( cond ) {
			++g_pass;
		} else {
			++g_fail;
			std::cerr << "FAIL: " << msg << "\n";
		}
	}

	uint32_t FloatBits( const float f )
	{
		uint32_t u = 0;
		std::memcpy( &u, &f, sizeof( u ) );
		return u;
	}

	// IEEE-754 bit classifiers (sound even when the test TU is compiled with -ffast-math).
	bool IsNaNF( const float f )
	{
		const uint32_t u = FloatBits( f );
		return ( ( u & 0x7f800000u ) == 0x7f800000u ) && ( ( u & 0x007fffffu ) != 0u );
	}

	bool IsInfF( const float f )
	{
		const uint32_t u = FloatBits( f );
		return ( u & 0x7fffffffu ) == 0x7f800000u;
	}

	// Sign-of-zero aware bit equality; NaN equals NaN.
	bool Same( const double actual, const float expected )
	{
		const float af = static_cast<float>( actual );
		if( IsNaNF( af ) && IsNaNF( expected ) ) return true;
		return FloatBits( af ) == FloatBits( expected );
	}

	bool CloseOrSame( const double actual, const float expected, const double relTol )
	{
		if( Same( actual, expected ) ) return true;
		const float af = static_cast<float>( actual );
		if( IsNaNF( af ) || IsNaNF( expected ) ) return false;
		if( IsInfF( af ) && IsInfF( expected ) ) return ( ( FloatBits( af ) >> 31 ) == ( FloatBits( expected ) >> 31 ) );
		if( IsInfF( af ) || IsInfF( expected ) ) return false;
		const double scale = std::fmax( 1.0, std::fabs( static_cast<double>( expected ) ) );
		return std::fabs( static_cast<double>( af ) - static_cast<double>( expected ) ) <= relTol * scale;
	}

#if defined(__APPLE__) && ( defined(__aarch64__) || defined(__arm64__) )
	static const bool kLibmIsGoldenPlatform = true;
#else
	static const bool kLibmIsGoldenPlatform = false;
#endif

	bool MathOpUsesLibm( const int op )
	{
		switch( op ) {
		case eMathSine: case eMathCosine: case eMathTangent:
		case eMathArcsine: case eMathArccosine: case eMathArctangent:
		case eMathArctan2: case eMathPower: case eMathLogarithm:
		case eMathExponent: case eMathSinh: case eMathCosh: case eMathTanh:
			return true;
		default:
			return false;
		}
	}

	bool VecOpUsesLibm( const int op )
	{
		return op == eVecSine || op == eVecCosine || op == eVecTangent || op == eVecPower;
	}

	struct OslMathRow
	{
		int op;
		float a, b, c;
		float simd, scalar, ieee;
	};

	struct OslVecMathRow
	{
		int op;
		float a[3], b[3], c[3];
		float scale;
		float valSimd, valScalar, valIeee;
		float vecSimd[3], vecScalar[3], vecIeee[3];
	};

	struct OslClampRow
	{
		int type;
		float v, mn, mx;
		float simd, scalar, ieee;
	};

	struct OslMapRangeRow
	{
		int type, clamp;
		float v, fmin, fmax, tmin, tmax, steps;
		float simd, scalar, ieee;
	};

	struct OslVecMapRangeRow
	{
		int type, clamp;
		float v[3], fmin[3], fmax[3], tmin[3], tmax[3], steps[3];
		float simd[3], scalar[3], ieee[3];
	};

#include "data/osl_math_golden.inc"

	struct DistanceStats
	{
		int exact = 0;
		int signZeroOnly = 0;
		int rounding = 0;
		int nonFiniteDiff = 0;
		double maxRel = 0.0;
	};

	void ClassifyScalar( const float actual, const float ref, DistanceStats& st )
	{
		if( Same( actual, ref ) ) {
			++st.exact;
			return;
		}
		if( IsNaNF( actual ) != IsNaNF( ref ) || IsInfF( actual ) != IsInfF( ref ) ) {
			++st.nonFiniteDiff;
			return;
		}
		if( actual == 0.0f && ref == 0.0f ) {
			++st.signZeroOnly;
			return;
		}
		++st.rounding;
		const double scale = std::fmax( 1.0, std::fabs( static_cast<double>( ref ) ) );
		const double rel = std::fabs( static_cast<double>( actual ) - static_cast<double>( ref ) ) / scale;
		if( rel > st.maxRel ) st.maxRel = rel;
	}

	void ClassifyVec3( const float actual[3], const float ref[3], DistanceStats& st )
	{
		bool allExact = true, anyNonFin = false, anyNonZeroDiff = false;
		double rowMax = 0.0;
		for( int k = 0; k < 3; ++k ) {
			if( Same( actual[k], ref[k] ) ) continue;
			allExact = false;
			if( IsNaNF( actual[k] ) != IsNaNF( ref[k] ) || IsInfF( actual[k] ) != IsInfF( ref[k] ) ) {
				anyNonFin = true;
			} else if( !( actual[k] == 0.0f && ref[k] == 0.0f ) ) {
				anyNonZeroDiff = true;
				const double scale = std::fmax( 1.0, std::fabs( static_cast<double>( ref[k] ) ) );
				const double rel = std::fabs( static_cast<double>( actual[k] ) - static_cast<double>( ref[k] ) ) / scale;
				if( rel > rowMax ) rowMax = rel;
			}
		}
		if( allExact ) { ++st.exact; return; }
		if( anyNonFin ) { ++st.nonFiniteDiff; return; }
		if( !anyNonZeroDiff ) { ++st.signZeroOnly; return; }
		++st.rounding;
		if( rowMax > st.maxRel ) st.maxRel = rowMax;
	}

	void PrintStats( const char* label, const DistanceStats& st )
	{
		std::printf( "  %-28s exact=%d  sign0=%d  round=%d (maxRel=%.3e)  nonfin=%d\n",
			label, st.exact, st.signZeroOnly, st.rounding, st.maxRel, st.nonFiniteDiff );
	}

	void TestGoldenMath()
	{
		const int n = static_cast<int>( sizeof( kMathRows ) / sizeof( kMathRows[0] ) );
		int bad = 0, libmNonExact = 0;
		DistanceStats vsSimd, vsScalar;
		for( int i = 0; i < n; ++i ) {
			const OslMathRow& r = kMathRows[i];
			const Scalar got = Math( r.op, r.a, r.b, r.c );
			const bool exact = Same( got, r.ieee );
			const bool ok = ( kLibmIsGoldenPlatform || !MathOpUsesLibm( r.op ) )
				? exact
				: CloseOrSame( got, r.ieee, 1e-6 );
			if( !exact && MathOpUsesLibm( r.op ) ) ++libmNonExact;
			if( !ok ) {
				++bad;
				if( bad <= 10 ) {
					std::cerr << "Math mismatch row " << i << " op=" << r.op
						<< " got=" << got << " (0x" << std::hex << FloatBits( static_cast<float>( got ) )
						<< ") ieee=" << r.ieee << " (0x" << FloatBits( r.ieee ) << std::dec << ")\n";
				}
			}
			ClassifyScalar( static_cast<float>( got ), r.simd, vsSimd );
			ClassifyScalar( static_cast<float>( got ), r.scalar, vsScalar );
		}
		Check( bad == 0, "OslMath::Math matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
		std::printf( "OslMath::Math (%d rows, libm non-exact=%d):\n", n, libmNonExact );
		PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
		PrintStats( "vs Cycles scalar (Blender)", vsScalar );
	}

	void TestGoldenVectorMath()
	{
		const int n = static_cast<int>( sizeof( kVecMathRows ) / sizeof( kVecMathRows[0] ) );
		int bad = 0, libmNonExact = 0;
		DistanceStats vsSimd, vsScalar;
		for( int i = 0; i < n; ++i ) {
			const OslVecMathRow& r = kVecMathRows[i];
			const Scalar a[3] = { r.a[0], r.a[1], r.a[2] };
			const Scalar b[3] = { r.b[0], r.b[1], r.b[2] };
			const Scalar c[3] = { r.c[0], r.c[1], r.c[2] };
			const Scalar gotVal = VectorMathScalar( r.op, a, b );
			Scalar gotVec[3] = { 0, 0, 0 };
			VectorMathVector( r.op, a, b, c, r.scale, gotVec );

			const bool isScalarOp = ( r.op == eVecDot || r.op == eVecDistance || r.op == eVecLength );
			const bool exactVal = Same( gotVal, r.valIeee );
			bool exactVec = true;
			bool okVec = true;
			for( int k = 0; k < 3; ++k ) {
				const bool ex = Same( gotVec[k], r.vecIeee[k] );
				exactVec = exactVec && ex;
				const bool okk = ( kLibmIsGoldenPlatform || !VecOpUsesLibm( r.op ) )
					? ex
					: CloseOrSame( gotVec[k], r.vecIeee[k], 1e-6 );
				okVec = okVec && okk;
			}
			if( !exactVec && VecOpUsesLibm( r.op ) ) ++libmNonExact;
			if( !( exactVal && okVec ) ) {
				++bad;
				if( bad <= 10 ) {
					std::cerr << "VecMath mismatch row " << i << " op=" << r.op << "\n";
				}
			}
			if( isScalarOp ) {
				ClassifyScalar( static_cast<float>( gotVal ), r.valSimd, vsSimd );
				ClassifyScalar( static_cast<float>( gotVal ), r.valScalar, vsScalar );
			} else {
				const float gv[3] = { static_cast<float>( gotVec[0] ), static_cast<float>( gotVec[1] ), static_cast<float>( gotVec[2] ) };
				ClassifyVec3( gv, r.vecSimd, vsSimd );
				ClassifyVec3( gv, r.vecScalar, vsScalar );
			}
		}
		Check( bad == 0, "OslMath::VectorMath matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
		std::printf( "OslMath::VectorMath (%d rows, libm non-exact=%d):\n", n, libmNonExact );
		PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
		PrintStats( "vs Cycles scalar (Blender)", vsScalar );
	}

	void TestGoldenClampAndMapRange()
	{
		// Clamp
		{
			const int n = static_cast<int>( sizeof( kClampRows ) / sizeof( kClampRows[0] ) );
			int bad = 0;
			DistanceStats vsSimd, vsScalar;
			for( int i = 0; i < n; ++i ) {
				const OslClampRow& r = kClampRows[i];
				const Scalar got = Clamp( r.type, r.v, r.mn, r.mx );
				if( !Same( got, r.ieee ) ) ++bad;
				ClassifyScalar( static_cast<float>( got ), r.simd, vsSimd );
				ClassifyScalar( static_cast<float>( got ), r.scalar, vsScalar );
			}
			Check( bad == 0, "OslMath::Clamp matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
			std::printf( "OslMath::Clamp (%d rows):\n", n );
			PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
			PrintStats( "vs Cycles scalar (Blender)", vsScalar );
		}
		// Scalar MapRange
		{
			const int n = static_cast<int>( sizeof( kMapRangeRows ) / sizeof( kMapRangeRows[0] ) );
			int bad = 0;
			DistanceStats vsSimd, vsScalar;
			for( int i = 0; i < n; ++i ) {
				const OslMapRangeRow& r = kMapRangeRows[i];
				const Scalar got = MapRange( r.type, r.clamp != 0, r.v, r.fmin, r.fmax, r.tmin, r.tmax, r.steps );
				if( !Same( got, r.ieee ) ) ++bad;
				ClassifyScalar( static_cast<float>( got ), r.simd, vsSimd );
				ClassifyScalar( static_cast<float>( got ), r.scalar, vsScalar );
			}
			Check( bad == 0, "OslMath::MapRange matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
			std::printf( "OslMath::MapRange (%d rows):\n", n );
			PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
			PrintStats( "vs Cycles scalar (Blender)", vsScalar );
		}
		// Vector MapRange
		{
			const int n = static_cast<int>( sizeof( kVecMapRangeRows ) / sizeof( kVecMapRangeRows[0] ) );
			int bad = 0;
			DistanceStats vsSimd, vsScalar;
			for( int i = 0; i < n; ++i ) {
				const OslVecMapRangeRow& r = kVecMapRangeRows[i];
				const Scalar v[3] = { r.v[0], r.v[1], r.v[2] };
				const Scalar fmin[3] = { r.fmin[0], r.fmin[1], r.fmin[2] };
				const Scalar fmax[3] = { r.fmax[0], r.fmax[1], r.fmax[2] };
				const Scalar tmin[3] = { r.tmin[0], r.tmin[1], r.tmin[2] };
				const Scalar tmax[3] = { r.tmax[0], r.tmax[1], r.tmax[2] };
				const Scalar steps[3] = { r.steps[0], r.steps[1], r.steps[2] };
				Scalar got[3] = {};
				VectorMapRange( r.type, r.clamp != 0, v, fmin, fmax, tmin, tmax, steps, got );
				bool rowOk = true;
				float gf[3];
				for( int k = 0; k < 3; ++k ) {
					gf[k] = static_cast<float>( got[k] );
					if( !Same( got[k], r.ieee[k] ) ) rowOk = false;
				}
				if( !rowOk ) ++bad;
				ClassifyVec3( gf, r.simd, vsSimd );
				ClassifyVec3( gf, r.scalar, vsScalar );
			}
			Check( bad == 0, "OslMath::VectorMapRange matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
			std::printf( "OslMath::VectorMapRange (%d rows):\n", n );
			PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
			PrintStats( "vs Cycles scalar (Blender)", vsScalar );
		}
	}

	void TestClosedFormAndRobustness()
	{
		// Domain-guard zero-on-undefined checks (independent of Cycles)
		Check( Math( eMathDivide, 5.0, 0.0, 0.0 ) == 0.0, "safe divide by 0 -> 0" );
		Check( Math( eMathSqrt, -4.0, 0.0, 0.0 ) == 0.0, "safe sqrt(-4) -> 0" );
		Check( Math( eMathInvSqrt, 0.0, 0.0, 0.0 ) == 0.0, "safe invsqrt(0) -> 0" );
		Check( Math( eMathInvSqrt, -2.0, 0.0, 0.0 ) == 0.0, "safe invsqrt(-2) -> 0" );
		Check( Math( eMathLogarithm, -1.0, 2.0, 0.0 ) == 0.0, "safe log(-1, 2) -> 0" );
		Check( Math( eMathLogarithm, 8.0, 1.0, 0.0 ) == 0.0, "safe log(8, 1) -> 0 (log(1)==0)" );
		Check( std::fabs( Math( eMathLogarithm, 8.0, 2.0, 0.0 ) - 3.0 ) < 1e-6, "log_2(8) == 3" );
		Check( Math( eMathPower, 0.0, 0.0, 0.0 ) == 1.0, "0^0 == 1" );
		Check( Math( eMathPower, -2.0, 0.5, 0.0 ) == 0.0, "(-2)^0.5 -> 0" );
		Check( Math( eMathPower, -2.0, 3.0, 0.0 ) == -8.0, "(-2)^3 == -8" );
		// UB-guard: negative base with NaN, +/-Inf, or out-of-int32-range exponent
		const double qnan = std::numeric_limits<double>::quiet_NaN();
		const double inf = std::numeric_limits<double>::infinity();
		Check( Math( eMathPower, -2.0, qnan, 0.0 ) == 0.0, "(-2)^NaN guarded -> 0" );
		Check( Math( eMathPower, -2.0, inf, 0.0 ) == 0.0, "(-2)^+Inf guarded -> 0" );
		Check( Math( eMathPower, -2.0, -inf, 0.0 ) == 0.0, "(-2)^-Inf guarded -> 0" );
		Check( Math( eMathPower, -2.0, 3e9, 0.0 ) == 0.0, "(-2)^3e9 out-of-int32 guarded -> 0" );
		Check( Math( eMathPower, -2.0, 2147483648.0, 0.0 ) == 0.0, "(-2)^+2^31 exact int32 upper boundary guarded -> 0" );
		Check( Math( eMathPower, -2.0, -2147483648.0, 0.0 ) == 0.0, "(-2)^-2^31 exact int32 lower boundary -> 0 (underflow, no UB)" );
		Check( Math( eMathPower, -2.0, -2147483904.0, 0.0 ) == 0.0, "(-2)^(-2^31-256) below int32 guarded -> 0" );
		Check( Math( eMathArctan2, 0.0, 0.0, 0.0 ) == 0.0, "atan2(0, 0) -> 0" );
		Check( Math( eMathSign, 0.0, 0.0, 0.0 ) == 0.0, "sign(0) -> 0" );
		Check( Math( eMathSign, -3.0, 0.0, 0.0 ) == -1.0, "sign(-3) -> -1" );
		Check( Math( eMathModulo, 5.0, 0.0, 0.0 ) == 0.0, "mod(5, 0) -> 0" );
		Check( Math( eMathFlooredModulo, -1.0, 4.0, 0.0 ) == 3.0, "floored_mod(-1, 4) == 3" );
		Check( Clamp( eClampRange, 0.5, 2.0, 0.0 ) == 0.5, "Clamp RANGE swaps reversed bounds" );
		Check( Clamp( eClampMinMax, 0.5, 2.0, 0.0 ) == 0.0, "Clamp MINMAX keeps min>max order" );
		Check( Math( 999, 1.0, 2.0, 3.0 ) == 0.0, "unknown Math op -> 0" );

		// Vector math closed-form
		const Scalar vx[3] = { 1.0, 0.0, 0.0 };
		const Scalar vy[3] = { 0.0, 1.0, 0.0 };
		const Scalar vz[3] = { 0.0, 0.0, 0.0 };
		Scalar out[3] = {};
		VectorMathVector( eVecCross, vx, vy, vz, 0.0, out );
		Check( out[0] == 0.0 && out[1] == 0.0 && out[2] == 1.0, "cross(x, y) == z" );
		VectorMathVector( eVecNormalize, vz, vz, vz, 0.0, out );
		Check( out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0, "safe_normalize(0) == 0" );
	}

	void TestVMAndCompileTime()
	{
		auto evalScalar = []( const std::string& expr, double& outVal ) -> bool {
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			if( !b.Finalize( expr, prog ) ) return false;
			ExprEvalContext ctx;
			outVal = prog.Eval( ctx );
			return true;
		};
		auto evalVec3 = []( const std::string& expr, double outVal[3] ) -> bool {
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			if( !b.Finalize( expr, prog ) || prog.ResultType() != ExpressionProgram::kVec3 ) return false;
			ExprEvalContext ctx;
			const Vector3 r = prog.EvalVec3( ctx );
			outVal[0] = r.x; outVal[1] = r.y; outVal[2] = r.z;
			return true;
		};

		double s = 0.0;
		Check( evalScalar( "osl_math(3, 7.0, 2.0, 0.0)", s ) && s == Math( eMathDivide, 7.0, 2.0, 0.0 ),
			"VM osl_math matches kernel" );
		Check( evalScalar( "osl_vmath_scalar(8, vec3(1,2,3), vec3(4,6,3))", s ) && std::fabs( s - 5.0 ) < 1e-6,
			"VM osl_vmath_scalar distance == 5" );
		double v[3] = {};
		Check( evalVec3( "osl_vmath(4, vec3(1,0,0), vec3(0,1,0), vec3(0,0,0), 0)", v ) &&
			v[0] == 0.0 && v[1] == 0.0 && v[2] == 1.0,
			"VM osl_vmath cross == (0,0,1)" );
		Check( evalScalar( "osl_clamp(1, 0.5, 2.0, 0.0)", s ) && s == 0.5,
			"VM osl_clamp RANGE matches kernel" );
		Check( evalScalar( "osl_map_range(0, 1, 0.5, 0.0, 1.0, 10.0, 20.0, 4.0)", s ) && s == 15.0,
			"VM osl_map_range linear == 15" );
		Check( evalVec3( "osl_vmap_range(0, 1, vec3(0.5,2,-1), vec3(0,0,0), vec3(1,1,1), vec3(0,0,0), vec3(10,10,10), vec3(4,4,4))", v ) &&
			v[0] == 5.0 && v[1] == 10.0 && v[2] == 0.0,
			"VM osl_vmap_range linear+clamp matches kernel" );
		// VM Finite() clamping on overflow (+Inf -> 0.0)
		Check( evalScalar( "osl_math(26, 1000.0, 0.0, 0.0)", s ) && s == 0.0,
			"VM osl_math exp(1000) clamps +Inf to 0 via Finite()" );
		// Group 5 builtins compile and evaluate with EnableContextVars(false)
		{
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( false );
			Check( b.Finalize( "osl_math(0, 2, 3, 0)", prog ), "osl_math compiles with EnableContextVars(false)" );
			ExprEvalContext ctx;
			Check( prog.Eval( ctx ) == 5.0, "osl_math evaluates with EnableContextVars(false)" );
		}

		// Compile-time rejections (non-literal, out-of-range, wrong arity, wrong type)
		const char* badExprs[] = {
			"osl_math(u, 1, 2, 3)",
			"osl_math(41, 1, 2, 3)",
			"osl_math(-1, 1, 2, 3)",
			"osl_math(0, 1, 2)",
			"osl_vmath_scalar(0, vec3(1,0,0), vec3(0,1,0))",
			"osl_vmath_scalar(10, vec3(1,0,0), vec3(0,1,0))",
			"osl_vmath(30, vec3(1,0,0), vec3(0,1,0), vec3(0,0,0), 1)",
			"osl_clamp(2, 0.5, 0, 1)",
			"osl_map_range(4, 0, 0.5, 0, 1, 0, 1, 4)",
			"osl_map_range(0, 2, 0.5, 0, 1, 0, 1, 4)",
			"osl_vmap_range(0, 0, 1, vec3(0,0,0), vec3(1,1,1), vec3(0,0,0), vec3(1,1,1), vec3(4,4,4))",
		};
		for( const char* e : badExprs ) {
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( !b.Finalize( e, prog ), std::string( "compile-time rejects: " ) + e );
		}

		// Memo-worthiness: none of Group 5 should be memo-worthy
		const char* names[] = { "osl_math", "osl_vmath_scalar", "osl_vmath", "osl_clamp", "osl_map_range", "osl_vmap_range" };
		for( const char* nm : names ) {
			const OslBuiltins::Sig* sig = OslBuiltins::Find( nm );
			Check( sig && !sig->memoWorthy, std::string( nm ) + " is not memoWorthy" );
		}
	}
}

int main()
{
	TestGoldenMath();
	TestGoldenVectorMath();
	TestGoldenClampAndMapRange();
	TestClosedFormAndRobustness();
	TestVMAndCompileTime();

	std::printf( "OslMathTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
