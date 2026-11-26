//////////////////////////////////////////////////////////////////////
//
//  OslVectorTest.cpp - Golden-value, closed-form, VM, compile-time,
//    robustness and memo-worthiness tests for OslVector.{h,cpp} and its
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

#include "../src/Library/Utilities/OslVector.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionOslBuiltins.h"

using namespace RISE;
using namespace RISE::OslVector;
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

	struct OslVecCompRow
	{
		int op;
		float a[3], b[3], c[3];
		float simd[3], scalar[3], ieee[3];
	};

	struct OslVecRotRow
	{
		int type, invert;
		float v[3], center[3], axis[3];
		float angle;
		float euler[3];
		float simd[3], scalar[3], ieee[3];
	};

#include "data/osl_vector_golden.inc"

	struct DistanceStats
	{
		int exact = 0;
		int signZeroOnly = 0;
		int rounding = 0;
		int nonFiniteDiff = 0;
		double maxRel = 0.0;
	};

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

	void TestGoldenCompOps()
	{
		const int n = static_cast<int>( sizeof( kVecCompRows ) / sizeof( kVecCompRows[0] ) );
		int bad = 0, libmNonExact = 0;
		DistanceStats vsSimd, vsScalar;
		for( int i = 0; i < n; ++i ) {
			const OslVecCompRow& r = kVecCompRows[i];
			const Scalar a[3] = { r.a[0], r.a[1], r.a[2] };
			const Scalar b[3] = { r.b[0], r.b[1], r.b[2] };
			const Scalar c[3] = { r.c[0], r.c[1], r.c[2] };
			Scalar out[3] = {};
			switch( r.op ) {
			case 0: VecMin( a, b, out ); break;
			case 1: VecMax( a, b, out ); break;
			case 2: VecAbs( a, out ); break;
			case 3: VecFloor( a, out ); break;
			case 4: VecFract( a, out ); break;
			case 5: VecPow( a, b, out ); break;
			case 6: VecClamp( a, b, c, out ); break;
			}
			const bool usesLibm = ( r.op == 5 );
			bool exact = true, ok = true;
			float gf[3];
			for( int k = 0; k < 3; ++k ) {
				gf[k] = static_cast<float>( out[k] );
				const bool ex = Same( out[k], r.ieee[k] );
				exact = exact && ex;
				ok = ok && ( ( kLibmIsGoldenPlatform || !usesLibm ) ? ex : CloseOrSame( out[k], r.ieee[k], 1e-6 ) );
			}
			if( !exact && usesLibm ) ++libmNonExact;
			if( !ok ) {
				++bad;
				if( bad <= 10 ) std::cerr << "VecComp mismatch row " << i << " op=" << r.op << "\n";
			}
			ClassifyVec3( gf, r.simd, vsSimd );
			ClassifyVec3( gf, r.scalar, vsScalar );
		}
		Check( bad == 0, "OslVector componentwise ops match strict IEEE golden table (" + std::to_string( n ) + " rows)" );
		std::printf( "OslVector componentwise (%d rows, libm non-exact=%d):\n", n, libmNonExact );
		PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
		PrintStats( "vs Cycles scalar (Blender)", vsScalar );
	}

	void TestGoldenRotate()
	{
		const int n = static_cast<int>( sizeof( kVecRotRows ) / sizeof( kVecRotRows[0] ) );
		int bad = 0, libmNonExact = 0;
		DistanceStats vsSimd, vsScalar;
		for( int i = 0; i < n; ++i ) {
			const OslVecRotRow& r = kVecRotRows[i];
			const Scalar v[3] = { r.v[0], r.v[1], r.v[2] };
			const Scalar ctr[3] = { r.center[0], r.center[1], r.center[2] };
			const Scalar ax[3] = { r.axis[0], r.axis[1], r.axis[2] };
			const Scalar eul[3] = { r.euler[0], r.euler[1], r.euler[2] };
			Scalar out[3] = {};
			VectorRotate( r.type, r.invert != 0, v, ctr, ax, r.angle, eul, out );
			bool exact = true, ok = true;
			float gf[3];
			for( int k = 0; k < 3; ++k ) {
				gf[k] = static_cast<float>( out[k] );
				const bool ex = Same( out[k], r.ieee[k] );
				exact = exact && ex;
				ok = ok && ( kLibmIsGoldenPlatform ? ex : CloseOrSame( out[k], r.ieee[k], 1e-6 ) );
			}
			if( !exact ) ++libmNonExact;
			if( !ok ) {
				++bad;
				if( bad <= 10 ) {
					std::cerr << "VecRot mismatch row " << i << " type=" << r.type << " inv=" << r.invert << "\n";
				}
			}
			ClassifyVec3( gf, r.simd, vsSimd );
			ClassifyVec3( gf, r.scalar, vsScalar );
		}
		Check( bad == 0, "OslVector::VectorRotate matches strict IEEE golden table (" + std::to_string( n ) + " rows)" );
		std::printf( "OslVector::VectorRotate (%d rows, libm non-exact=%d):\n", n, libmNonExact );
		PrintStats( "vs Cycles SIMD (Blender)", vsSimd );
		PrintStats( "vs Cycles scalar (Blender)", vsScalar );
	}

	void TestClosedFormAndVM()
	{
		// Rodrigues: rotate (1,0,0) around (0,0,1) by pi/2 -> (0,1,0); norm preserved; inverse round-trips.
		const Scalar v[3] = { 1.0, 2.0, -3.0 };
		const Scalar ax[3] = { 1.0, 2.0, 2.0 };
		const Scalar ctr[3] = { 0.5, -1.0, 0.25 };
		const Scalar eul[3] = { 0.4, -0.7, 1.1 };
		Scalar rot[3] = {}, back[3] = {};
		RotateAroundAxis( v, ax, 1.23, rot );
		RotateAroundAxis( rot, ax, -1.23, back );
		for( int k = 0; k < 3; ++k ) {
			Check( std::fabs( back[k] - v[k] ) < 1e-5, "RotateAroundAxis round-trip" );
		}
		// Zero axis leaves vector unchanged
		const Scalar z[3] = { 0.0, 0.0, 0.0 };
		RotateAroundAxis( v, z, 1.5, rot );
		Check( rot[0] == v[0] && rot[1] == v[1] && rot[2] == v[2], "RotateAroundAxis zero axis -> unchanged" );

		// Euler XYZ forward + invert round-trip around non-zero center
		VectorRotate( eRotateEulerXYZ, false, v, ctr, z, 0.0, eul, rot );
		VectorRotate( eRotateEulerXYZ, true, rot, ctr, z, 0.0, eul, back );
		for( int k = 0; k < 3; ++k ) {
			Check( std::fabs( back[k] - v[k] ) < 1e-5, "VectorRotate EULER_XYZ invert round-trip" );
		}

		// VecPow UB-guard tests: negative base with NaN, +/-Inf, or out-of-int32 exponent
		{
			const Scalar negBase[3] = { -2.0, -2.0, -2.0 };
			const Scalar badExp[3] = {
				std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity(),
				3e9
			};
			Scalar powOut[3] = { 1.0, 1.0, 1.0 };
			VecPow( negBase, badExp, powOut );
			Check( powOut[0] == 0.0 && powOut[1] == 0.0 && powOut[2] == 0.0,
				"VecPow guards negative base with NaN/+Inf/out-of-int32 -> 0" );
			const Scalar negInfExp[3] = { -std::numeric_limits<double>::infinity(), 2147483648.0, -2147483904.0 };
			VecPow( negBase, negInfExp, powOut );
			Check( powOut[0] == 0.0 && powOut[1] == 0.0 && powOut[2] == 0.0,
				"VecPow guards negative base with -Inf / exact int32 boundaries -> 0" );
		}

		// VM checks
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
		double out[3] = {};
		Check( evalVec3( "osl_vmin(vec3(1,-2,3), vec3(-1,2,0))", out ) &&
			out[0] == -1.0 && out[1] == -2.0 && out[2] == 0.0, "VM osl_vmin" );
		Check( evalVec3( "osl_vmax(vec3(1,-2,3), vec3(-1,2,0))", out ) &&
			out[0] == 1.0 && out[1] == 2.0 && out[2] == 3.0, "VM osl_vmax" );
		Check( evalVec3( "osl_vabs(vec3(-1,0,-3.5))", out ) &&
			out[0] == 1.0 && out[1] == 0.0 && out[2] == 3.5, "VM osl_vabs" );
		Check( evalVec3( "osl_vfloor(vec3(-1.2,2.8,0))", out ) &&
			out[0] == -2.0 && out[1] == 2.0 && out[2] == 0.0, "VM osl_vfloor" );
		Check( evalVec3( "osl_vfract(vec3(-0.25,1.75,2))", out ) &&
			std::fabs( out[0] - 0.75 ) < 1e-6 && std::fabs( out[1] - 0.75 ) < 1e-6 && out[2] == 0.0, "VM osl_vfract" );
		Check( evalVec3( "osl_vpow(vec3(2,-2,-2), vec3(3,3,0.5))", out ) &&
			out[0] == 8.0 && out[1] == -8.0 && out[2] == 0.0, "VM osl_vpow" );
		Check( evalVec3( "osl_vclamp(vec3(-1,0.5,3), vec3(0,0,0), vec3(1,1,1))", out ) &&
			out[0] == 0.0 && out[1] == 0.5 && out[2] == 1.0, "VM osl_vclamp" );
		Check( evalVec3( "osl_rotate(vec3(1,0,0), vec3(0,0,2), 1.5707963268)", out ) &&
			std::fabs( out[0] ) < 1e-6 && std::fabs( out[1] - 1.0 ) < 1e-6 && std::fabs( out[2] ) < 1e-6, "VM osl_rotate" );
		Check( evalVec3( "osl_vector_rotate(3, 0, vec3(2,0,0), vec3(1,0,0), vec3(0,0,0), 1.5707963268, vec3(0,0,0))", out ) &&
			std::fabs( out[0] - 1.0 ) < 1e-6 && std::fabs( out[1] - 1.0 ) < 1e-6 && std::fabs( out[2] ) < 1e-6, "VM osl_vector_rotate Z" );
		// VM Finite() clamping on overflow (+Inf -> 0.0)
		Check( evalVec3( "osl_vpow(vec3(10, 10, 10), vec3(1000, 1000, 1000))", out ) &&
			out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0,
			"VM osl_vpow overflow clamps +Inf to 0 via Finite()" );
		// Group 6 builtins compile and evaluate with EnableContextVars(false)
		{
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( false );
			Check( b.Finalize( "osl_vmin(vec3(1,2,3), vec3(3,1,2))", prog ),
				"osl_vmin compiles with EnableContextVars(false)" );
			ExprEvalContext ctx;
			const Vector3 r = prog.EvalVec3( ctx );
			Check( r.x == 1.0 && r.y == 1.0 && r.z == 2.0,
				"osl_vmin evaluates with EnableContextVars(false)" );
		}

		// Compile-time rejections
		const char* badExprs[] = {
			"osl_vector_rotate(5, 0, vec3(1,0,0), vec3(0,0,0), vec3(0,0,1), 1, vec3(0,0,0))",
			"osl_vector_rotate(0, 2, vec3(1,0,0), vec3(0,0,0), vec3(0,0,1), 1, vec3(0,0,0))",
			"osl_vector_rotate(u, 0, vec3(1,0,0), vec3(0,0,0), vec3(0,0,1), 1, vec3(0,0,0))",
			"osl_rotate(1, vec3(0,0,1), 1)",
			"osl_vmin(1, vec3(0,0,0))",
			// Wave C L3: scalar-only ops (7=DOT, 8=DISTANCE, 9=LENGTH) rejected in osl_vmath
			"osl_vmath(7, vec3(1,0,0), vec3(0,1,0), vec3(0,0,0), 1.0)",
			"osl_vmath(8, vec3(1,0,0), vec3(0,1,0), vec3(0,0,0), 1.0)",
			"osl_vmath(9, vec3(1,0,0), vec3(0,1,0), vec3(0,0,0), 1.0)",
		};
		for( const char* e : badExprs ) {
			ExpressionProgram prog = ExpressionProgram::Invalid();
			ExpressionProgram::Builder b;
			b.EnableContextVars( true );
			Check( !b.Finalize( e, prog ), std::string( "compile-time rejects: " ) + e );
		}

		const char* names[] = {
			"osl_vmin", "osl_vmax", "osl_vabs", "osl_vfloor", "osl_vfract",
			"osl_vpow", "osl_vclamp", "osl_rotate", "osl_vector_rotate"
		};
		for( const char* nm : names ) {
			const OslBuiltins::Sig* sig = OslBuiltins::Find( nm );
			Check( sig && !sig->memoWorthy, std::string( nm ) + " is not memoWorthy" );
		}
	}
}

int main()
{
	TestGoldenCompOps();
	TestGoldenRotate();
	TestClosedFormAndVM();

	std::printf( "OslVectorTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
