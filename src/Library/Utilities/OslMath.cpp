//////////////////////////////////////////////////////////////////////
//
//  OslMath.cpp - A port of Cycles' Safe Math (41 ops), Vector Math
//    (30 ops), Clamp, and Map Range (Float & Vector) kernels.  See
//    OslMath.h for the source, precision contract and safe_powf guard.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/kernel/svm/math_util.h, math.h, clamp.h, map_range.h,
//    and util/math_base.h (including wrapf adapted from godot-engine
//    math_funcs.h) --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    intern/cycles/util/math_float3.h --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    MODIFIED FOR RISE: see OslMath.h.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslMath.h"
#include <cfloat>
#include <cmath>

// Strict IEEE enforcement matching OslNoise.cpp / OslVoronoi.cpp / OslColor.cpp.
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslMath.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' math functions"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslMath
	{
		namespace
		{
			static const float kPi = 3.1415926535897932f;

			//////////////////////////////////////////////////////////
			// Scalar helpers matching Cycles' util/math_base.h.
			//////////////////////////////////////////////////////////

			inline float CclMin( const float a, const float b )
			{
				return ( a < b ) ? a : b;
			}

			inline float CclMax( const float a, const float b )
			{
				return ( a > b ) ? a : b;
			}

			inline float CclClamp( const float a, const float mn, const float mx )
			{
				return CclMin( CclMax( a, mn ), mx );
			}

			inline float Fractf( const float x )
			{
				return x - std::floor( x );
			}

			inline float SafeDivide( const float a, const float b )
			{
				return ( b != 0.0f ) ? a / b : 0.0f;
			}

			inline float SafeSqrtf( const float f )
			{
				return std::sqrt( CclMax( f, 0.0f ) );
			}

			inline float InverseSqrtf( const float f )
			{
				return ( f > 0.0f ) ? 1.0f / std::sqrt( f ) : 0.0f;
			}

			inline float SafeAsinf( const float a )
			{
				return std::asin( CclClamp( a, -1.0f, 1.0f ) );
			}

			inline float SafeAcosf( const float a )
			{
				return std::acos( CclClamp( a, -1.0f, 1.0f ) );
			}

			inline float CompatiblePowf( const float x, const float y )
			{
				if( y == 0.0f ) {
					return 1.0f;
				}
				if( x == 0.0f ) {
					return 0.0f;
				}
				return std::pow( x, y );
			}

			inline float SafePowf( const float a, const float b )
			{
				// Cycles checks `a < 0.0f && b != float_to_int(b)`.  Guard the
				// float->int cast against NaN / +/-inf / out-of-int32-range UB.
				if( a < 0.0f ) {
					if( !( b >= -2147483648.0f && b < 2147483648.0f ) ) {
						return 0.0f;
					}
					if( b != static_cast<float>( static_cast<int>( b ) ) ) {
						return 0.0f;
					}
				}
				return CompatiblePowf( a, b );
			}

			inline float SafeLogf( const float a, const float b )
			{
				if( a <= 0.0f || b <= 0.0f ) {
					return 0.0f;
				}
				return SafeDivide( std::log( a ), std::log( b ) );
			}

			inline float SafeModulo( const float a, const float b )
			{
				return ( b != 0.0f ) ? std::fmod( a, b ) : 0.0f;
			}

			inline float SafeFlooredModulo( const float a, const float b )
			{
				return ( b != 0.0f ) ? a - std::floor( a / b ) * b : 0.0f;
			}

			inline float Wrapf( const float value, const float mx, const float mn )
			{
				const float range = mx - mn;
				return ( range != 0.0f ) ? value - ( range * std::floor( ( value - mn ) / range ) ) : mn;
			}

			inline float PingPongf( const float a, const float b )
			{
				return ( b != 0.0f ) ? std::fabs( Fractf( ( a - b ) / ( b * 2.0f ) ) * b * 2.0f - b ) : 0.0f;
			}

			inline float SmoothMinf( const float a, const float b, const float k )
			{
				if( k != 0.0f ) {
					const float h = std::fmax( k - std::fabs( a - b ), 0.0f ) / k;
					return std::fmin( a, b ) - h * h * h * k * ( 1.0f / 6.0f );
				}
				return std::fmin( a, b );
			}

			inline float Signf( const float f )
			{
				return ( f < 0.0f ) ? -1.0f : 1.0f;
			}

			inline float CompatibleSignf( const float f )
			{
				if( f == 0.0f ) {
					return 0.0f;
				}
				return Signf( f );
			}

			inline float CompatibleAtan2( const float y, const float x )
			{
				return ( x == 0.0f && y == 0.0f ) ? 0.0f : std::atan2( y, x );
			}

			inline float Smoothstep( const float edge0, const float edge1, const float x )
			{
				float result;
				if( x < edge0 ) {
					result = 0.0f;
				} else if( x >= edge1 ) {
					result = 1.0f;
				} else {
					const float t = ( x - edge0 ) / ( edge1 - edge0 );
					result = ( 3.0f - 2.0f * t ) * ( t * t );
				}
				return result;
			}

			inline float Smootherstep( const float edge0, const float edge1, float x )
			{
				x = CclClamp( SafeDivide( x - edge0, edge1 - edge0 ), 0.0f, 1.0f );
				return x * x * x * ( x * ( x * 6.0f - 15.0f ) + 10.0f );
			}

			//////////////////////////////////////////////////////////
			// float3 helpers matching Cycles' scalar path in
			// util/math_float3.h.
			//////////////////////////////////////////////////////////

			struct F3
			{
				float x, y, z;
			};

			inline F3 MakeF3( const float x, const float y, const float z )
			{
				F3 r = { x, y, z };
				return r;
			}

			inline F3 LoadF3( const Scalar a[3] )
			{
				return MakeF3(
					static_cast<float>( a[0] ),
					static_cast<float>( a[1] ),
					static_cast<float>( a[2] ) );
			}

			inline void StoreF3( const F3 v, Scalar out[3] )
			{
				out[0] = static_cast<Scalar>( v.x );
				out[1] = static_cast<Scalar>( v.y );
				out[2] = static_cast<Scalar>( v.z );
			}

			inline F3 ZeroF3()
			{
				return MakeF3( 0.0f, 0.0f, 0.0f );
			}

			inline F3 NegF3( const F3 a )
			{
				return MakeF3( -a.x, -a.y, -a.z );
			}

			inline F3 AddF3( const F3 a, const F3 b )
			{
				return MakeF3( a.x + b.x, a.y + b.y, a.z + b.z );
			}

			inline F3 SubF3( const F3 a, const F3 b )
			{
				return MakeF3( a.x - b.x, a.y - b.y, a.z - b.z );
			}

			inline F3 MulF3( const F3 a, const F3 b )
			{
				return MakeF3( a.x * b.x, a.y * b.y, a.z * b.z );
			}

			inline F3 MulF3S( const F3 a, const float f )
			{
				return MakeF3( a.x * f, a.y * f, a.z * f );
			}

			inline float DotF3( const F3 a, const F3 b )
			{
				return a.x * b.x + a.y * b.y + a.z * b.z;
			}

			inline float LenF3( const F3 a )
			{
				return std::sqrt( DotF3( a, a ) );
			}

			inline F3 CrossF3( const F3 a, const F3 b )
			{
				return MakeF3(
					a.y * b.z - a.z * b.y,
					a.z * b.x - a.x * b.z,
					a.x * b.y - a.y * b.x );
			}

			inline F3 SafeNormalizeF3( const F3 a )
			{
				const float t = LenF3( a );
				return ( t != 0.0f ) ? MulF3S( a, 1.0f / t ) : a;
			}

			inline F3 SafeDivideF3( const F3 a, const F3 b )
			{
				return MakeF3(
					( b.x != 0.0f ) ? a.x / b.x : 0.0f,
					( b.y != 0.0f ) ? a.y / b.y : 0.0f,
					( b.z != 0.0f ) ? a.z / b.z : 0.0f );
			}

			inline F3 ProjectF3( const F3 v, const F3 vProj )
			{
				const float lenSq = DotF3( vProj, vProj );
				return ( lenSq != 0.0f ) ? MulF3S( vProj, DotF3( v, vProj ) / lenSq ) : ZeroF3();
			}

			inline F3 ReflectF3( const F3 incident, const F3 unitNormal )
			{
				return SubF3( incident, MulF3S( MulF3S( unitNormal, 2.0f ), DotF3( incident, unitNormal ) ) );
			}

			inline F3 RefractF3( const F3 incident, const F3 normal, const float eta )
			{
				const float ndoti = DotF3( normal, incident );
				const float k = 1.0f - eta * eta * ( 1.0f - ndoti * ndoti );
				if( k < 0.0f ) {
					return ZeroF3();
				}
				return SubF3( MulF3S( incident, eta ), MulF3S( normal, eta * ndoti + std::sqrt( k ) ) );
			}

			inline F3 FaceforwardF3( const F3 vector, const F3 incident, const F3 reference )
			{
				return ( DotF3( reference, incident ) < 0.0f ) ? vector : NegF3( vector );
			}

			inline F3 FloorF3( const F3 a )
			{
				return MakeF3( std::floor( a.x ), std::floor( a.y ), std::floor( a.z ) );
			}

			inline F3 CeilF3( const F3 a )
			{
				return MakeF3( std::ceil( a.x ), std::ceil( a.y ), std::ceil( a.z ) );
			}

			inline F3 SafeFmodF3( const F3 a, const F3 b )
			{
				return MakeF3(
					( b.x == 0.0f ) ? 0.0f : std::fmod( a.x, b.x ),
					( b.y == 0.0f ) ? 0.0f : std::fmod( a.y, b.y ),
					( b.z == 0.0f ) ? 0.0f : std::fmod( a.z, b.z ) );
			}

			inline F3 SafeFlooredFmodF3( const F3 a, const F3 b )
			{
				return MakeF3(
					( b.x == 0.0f ) ? 0.0f : a.x - std::floor( a.x / b.x ) * b.x,
					( b.y == 0.0f ) ? 0.0f : a.y - std::floor( a.y / b.y ) * b.y,
					( b.z == 0.0f ) ? 0.0f : a.z - std::floor( a.z / b.z ) * b.z );
			}

			inline F3 WrapF3( const F3 value, const F3 mx, const F3 mn )
			{
				return AddF3( SafeFlooredFmodF3( SubF3( value, mn ), SubF3( mx, mn ) ), mn );
			}

			inline F3 ClampF3( const F3 a, const F3 mn, const F3 mx )
			{
				return MakeF3(
					CclClamp( a.x, mn.x, mx.x ),
					CclClamp( a.y, mn.y, mx.y ),
					CclClamp( a.z, mn.z, mx.z ) );
			}

			inline float ClampRangeScalar( const int type, const float value, const float mn, const float mx )
			{
				if( type == eClampRange && ( mn > mx ) ) {
					return CclClamp( value, mx, mn );
				}
				return CclClamp( value, mn, mx );
			}
		}

		Scalar Math( const int op, const Scalar aIn, const Scalar bIn, const Scalar cIn )
		{
			const float a = static_cast<float>( aIn );
			const float b = static_cast<float>( bIn );
			const float c = static_cast<float>( cIn );
			float r = 0.0f;

			switch( op )
			{
			case eMathAdd:           r = a + b; break;
			case eMathSubtract:      r = a - b; break;
			case eMathMultiply:      r = a * b; break;
			case eMathDivide:        r = SafeDivide( a, b ); break;
			case eMathPower:         r = SafePowf( a, b ); break;
			case eMathLogarithm:     r = SafeLogf( a, b ); break;
			case eMathSqrt:          r = SafeSqrtf( a ); break;
			case eMathInvSqrt:       r = InverseSqrtf( a ); break;
			case eMathAbsolute:      r = std::fabs( a ); break;
			case eMathRadians:       r = a * ( kPi / 180.0f ); break;
			case eMathDegrees:       r = a * ( 180.0f / kPi ); break;
			case eMathMinimum:       r = std::fmin( a, b ); break;
			case eMathMaximum:       r = std::fmax( a, b ); break;
			case eMathLessThan:      r = ( a < b ) ? 1.0f : 0.0f; break;
			case eMathGreaterThan:   r = ( a > b ) ? 1.0f : 0.0f; break;
			case eMathRound:         r = std::floor( a + 0.5f ); break;
			case eMathFloor:         r = std::floor( a ); break;
			case eMathCeil:          r = std::ceil( a ); break;
			case eMathFraction:      r = a - std::floor( a ); break;
			case eMathModulo:        r = SafeModulo( a, b ); break;
			case eMathFlooredModulo: r = SafeFlooredModulo( a, b ); break;
			case eMathTrunc:         r = ( a >= 0.0f ) ? std::floor( a ) : std::ceil( a ); break;
			case eMathSnap:          r = std::floor( SafeDivide( a, b ) ) * b; break;
			case eMathWrap:          r = Wrapf( a, b, c ); break;
			case eMathPingPong:      r = PingPongf( a, b ); break;
			case eMathSine:          r = std::sin( a ); break;
			case eMathCosine:        r = std::cos( a ); break;
			case eMathTangent:       r = std::tan( a ); break;
			case eMathSinh:          r = std::sinh( a ); break;
			case eMathCosh:          r = std::cosh( a ); break;
			case eMathTanh:          r = std::tanh( a ); break;
			case eMathArcsine:       r = SafeAsinf( a ); break;
			case eMathArccosine:     r = SafeAcosf( a ); break;
			case eMathArctangent:    r = std::atan( a ); break;
			case eMathArctan2:       r = CompatibleAtan2( a, b ); break;
			case eMathSign:          r = CompatibleSignf( a ); break;
			case eMathExponent:      r = std::exp( a ); break;
			case eMathCompare:       r = ( ( a == b ) || ( std::fabs( a - b ) <= std::fmax( c, FLT_EPSILON ) ) ) ? 1.0f : 0.0f; break;
			case eMathMultiplyAdd:   r = a * b + c; break;
			case eMathSmoothMin:     r = SmoothMinf( a, b, c ); break;
			case eMathSmoothMax:     r = -SmoothMinf( -a, -b, c ); break;
			default:                 r = 0.0f; break;
			}
			return static_cast<Scalar>( r );
		}

		Scalar VectorMathScalar( const int op, const Scalar aIn[3], const Scalar bIn[3] )
		{
			const F3 a = LoadF3( aIn );
			const F3 b = LoadF3( bIn );
			float r = 0.0f;

			switch( op )
			{
			case eVecDot:      r = DotF3( a, b ); break;
			case eVecDistance: r = LenF3( SubF3( a, b ) ); break;
			case eVecLength:   r = LenF3( a ); break;
			default:           r = 0.0f; break;
			}
			return static_cast<Scalar>( r );
		}

		void VectorMathVector(
			const int op,
			const Scalar aIn[3],
			const Scalar bIn[3],
			const Scalar cIn[3],
			const Scalar scaleIn,
			Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			const F3 b = LoadF3( bIn );
			const F3 c = LoadF3( cIn );
			const float param1 = static_cast<float>( scaleIn );
			F3 v = ZeroF3();

			switch( op )
			{
			case eVecAdd:         v = AddF3( a, b ); break;
			case eVecSubtract:    v = SubF3( a, b ); break;
			case eVecMultiply:    v = MulF3( a, b ); break;
			case eVecDivide:      v = SafeDivideF3( a, b ); break;
			case eVecCross:       v = CrossF3( a, b ); break;
			case eVecProject:     v = ProjectF3( a, b ); break;
			case eVecReflect:     v = ReflectF3( a, SafeNormalizeF3( b ) ); break;
			case eVecRefract:     v = RefractF3( a, SafeNormalizeF3( b ), param1 ); break;
			case eVecFaceforward: v = FaceforwardF3( a, b, c ); break;
			case eVecMultiplyAdd: v = AddF3( MulF3( a, b ), c ); break;
			case eVecScale:       v = MulF3S( a, param1 ); break;
			case eVecNormalize:   v = SafeNormalizeF3( a ); break;
			case eVecSnap:        v = MulF3( FloorF3( SafeDivideF3( a, b ) ), b ); break;
			case eVecRound:       v = FloorF3( MakeF3( a.x + 0.5f, a.y + 0.5f, a.z + 0.5f ) ); break;
			case eVecFloor:       v = FloorF3( a ); break;
			case eVecCeil:        v = CeilF3( a ); break;
			case eVecModulo:      v = SafeFmodF3( a, b ); break;
			case eVecWrap:        v = WrapF3( a, b, c ); break;
			case eVecFraction:    v = SubF3( a, FloorF3( a ) ); break;
			case eVecAbsolute:    v = MakeF3( std::fabs( a.x ), std::fabs( a.y ), std::fabs( a.z ) ); break;
			case eVecPower:       v = MakeF3( SafePowf( a.x, b.x ), SafePowf( a.y, b.y ), SafePowf( a.z, b.z ) ); break;
			case eVecSign:        v = MakeF3( CompatibleSignf( a.x ), CompatibleSignf( a.y ), CompatibleSignf( a.z ) ); break;
			case eVecMinimum:     v = MakeF3( CclMin( a.x, b.x ), CclMin( a.y, b.y ), CclMin( a.z, b.z ) ); break;
			case eVecMaximum:     v = MakeF3( CclMax( a.x, b.x ), CclMax( a.y, b.y ), CclMax( a.z, b.z ) ); break;
			case eVecSine:        v = MakeF3( std::sin( a.x ), std::sin( a.y ), std::sin( a.z ) ); break;
			case eVecCosine:      v = MakeF3( std::cos( a.x ), std::cos( a.y ), std::cos( a.z ) ); break;
			case eVecTangent:     v = MakeF3( std::tan( a.x ), std::tan( a.y ), std::tan( a.z ) ); break;
			default:              v = ZeroF3(); break;
			}
			StoreF3( v, out );
		}

		Scalar Clamp( const int type, const Scalar valueIn, const Scalar mnIn, const Scalar mxIn )
		{
			return static_cast<Scalar>( ClampRangeScalar(
				type,
				static_cast<float>( valueIn ),
				static_cast<float>( mnIn ),
				static_cast<float>( mxIn ) ) );
		}

		Scalar MapRange(
			const int type,
			const bool clampResult,
			const Scalar valueIn,
			const Scalar fromMinIn,
			const Scalar fromMaxIn,
			const Scalar toMinIn,
			const Scalar toMaxIn,
			const Scalar stepsIn )
		{
			const float value   = static_cast<float>( valueIn );
			const float fromMin = static_cast<float>( fromMinIn );
			const float fromMax = static_cast<float>( fromMaxIn );
			const float toMin   = static_cast<float>( toMinIn );
			const float toMax   = static_cast<float>( toMaxIn );
			const float steps   = static_cast<float>( stepsIn );

			float result = 0.0f;
			if( fromMax != fromMin ) {
				float factor = value;
				switch( type )
				{
				default:
				case eMapLinear:
					factor = ( value - fromMin ) / ( fromMax - fromMin );
					break;
				case eMapStepped:
					factor = ( value - fromMin ) / ( fromMax - fromMin );
					factor = ( steps > 0.0f ) ? std::floor( factor * ( steps + 1.0f ) ) / steps : 0.0f;
					break;
				case eMapSmoothstep:
					factor = ( fromMin > fromMax ) ? 1.0f - Smoothstep( fromMax, fromMin, factor ) :
					                                 Smoothstep( fromMin, fromMax, factor );
					break;
				case eMapSmootherstep:
					factor = ( fromMin > fromMax ) ? 1.0f - Smootherstep( fromMax, fromMin, factor ) :
					                                 Smootherstep( fromMin, fromMax, factor );
					break;
				}
				result = toMin + factor * ( toMax - toMin );
			}
			// Blender's MapRangeNode::expand appends a RANGE Clamp node only when
			// clamp is enabled and interpolation is LINEAR or STEPPED.
			if( clampResult && type != eMapSmoothstep && type != eMapSmootherstep ) {
				result = ClampRangeScalar( eClampRange, result, toMin, toMax );
			}
			return static_cast<Scalar>( result );
		}

		void VectorMapRange(
			const int type,
			const bool clampResult,
			const Scalar valueIn[3],
			const Scalar fromMinIn[3],
			const Scalar fromMaxIn[3],
			const Scalar toMinIn[3],
			const Scalar toMaxIn[3],
			const Scalar stepsIn[3],
			Scalar out[3] )
		{
			const F3 value   = LoadF3( valueIn );
			const F3 fromMin = LoadF3( fromMinIn );
			const F3 fromMax = LoadF3( fromMaxIn );
			const F3 toMin   = LoadF3( toMinIn );
			const F3 toMax   = LoadF3( toMaxIn );
			const F3 steps   = LoadF3( stepsIn );

			const bool useClamp = ( type == eMapSmoothstep || type == eMapSmootherstep ) ? false : clampResult;
			F3 factor = value;

			switch( type )
			{
			default:
			case eMapLinear:
				factor = SafeDivideF3( SubF3( value, fromMin ), SubF3( fromMax, fromMin ) );
				break;
			case eMapStepped:
				factor = SafeDivideF3( SubF3( value, fromMin ), SubF3( fromMax, fromMin ) );
				factor = MakeF3(
					( steps.x > 0.0f ) ? std::floor( factor.x * ( steps.x + 1.0f ) ) / steps.x : 0.0f,
					( steps.y > 0.0f ) ? std::floor( factor.y * ( steps.y + 1.0f ) ) / steps.y : 0.0f,
					( steps.z > 0.0f ) ? std::floor( factor.z * ( steps.z + 1.0f ) ) / steps.z : 0.0f );
				break;
			case eMapSmoothstep:
				factor = SafeDivideF3( SubF3( value, fromMin ), SubF3( fromMax, fromMin ) );
				factor = ClampF3( factor, ZeroF3(), MakeF3( 1.0f, 1.0f, 1.0f ) );
				factor = MulF3(
					SubF3( MakeF3( 3.0f, 3.0f, 3.0f ), MulF3S( factor, 2.0f ) ),
					MulF3( factor, factor ) );
				break;
			case eMapSmootherstep:
				factor = SafeDivideF3( SubF3( value, fromMin ), SubF3( fromMax, fromMin ) );
				factor = ClampF3( factor, ZeroF3(), MakeF3( 1.0f, 1.0f, 1.0f ) );
				factor = MakeF3(
					factor.x * factor.x * factor.x * ( factor.x * ( factor.x * 6.0f - 15.0f ) + 10.0f ),
					factor.y * factor.y * factor.y * ( factor.y * ( factor.y * 6.0f - 15.0f ) + 10.0f ),
					factor.z * factor.z * factor.z * ( factor.z * ( factor.z * 6.0f - 15.0f ) + 10.0f ) );
				break;
			}

			F3 result = AddF3( toMin, MulF3( factor, SubF3( toMax, toMin ) ) );
			if( useClamp ) {
				result.x = ClampRangeScalar( eClampRange, result.x, toMin.x, toMax.x );
				result.y = ClampRangeScalar( eClampRange, result.y, toMin.y, toMax.y );
				result.z = ClampRangeScalar( eClampRange, result.z, toMin.z, toMax.z );
			}
			StoreF3( result, out );
		}
	}
}
