//////////////////////////////////////////////////////////////////////
//
//  OslVector.cpp - A port of Cycles' componentwise float3 operations
//    and Vector Rotate node (Rodrigues axis-angle and Euler XYZ).
//    See OslVector.h for the source, precision contract and notices.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/kernel/svm/vector_rotate.h, util/transform.h,
//    util/math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    intern/cycles/util/math_float3.h, util/math_float4.h --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    MODIFIED FOR RISE: see OslVector.h.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslVector.h"
#include <cmath>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslVector.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' vector and rotation functions"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslVector
	{
		namespace
		{
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

			inline float SafePowf( const float a, const float b )
			{
				if( a < 0.0f ) {
					if( !( b >= -2147483648.0f && b < 2147483648.0f ) ) {
						return 0.0f;
					}
					if( b != static_cast<float>( static_cast<int>( b ) ) ) {
						return 0.0f;
					}
				}
				if( b == 0.0f ) {
					return 1.0f;
				}
				if( a == 0.0f ) {
					return 0.0f;
				}
				return std::pow( a, b );
			}

			inline float DotF3( const F3 a, const F3 b )
			{
				return a.x * b.x + a.y * b.y + a.z * b.z;
			}

			inline float LenF3( const F3 a )
			{
				return std::sqrt( DotF3( a, a ) );
			}

			// Cycles' util/math_float3.h: rotate_around_axis (exact order).
			inline F3 RotateAroundUnitAxis( const F3 p, const F3 axis, const float angle )
			{
				const float costheta = std::cos( angle );
				const float sintheta = std::sin( angle );
				F3 r;

				r.x = ( ( costheta + ( 1.0f - costheta ) * axis.x * axis.x ) * p.x ) +
				      ( ( ( 1.0f - costheta ) * axis.x * axis.y - axis.z * sintheta ) * p.y ) +
				      ( ( ( 1.0f - costheta ) * axis.x * axis.z + axis.y * sintheta ) * p.z );

				r.y = ( ( ( 1.0f - costheta ) * axis.x * axis.y + axis.z * sintheta ) * p.x ) +
				      ( ( costheta + ( 1.0f - costheta ) * axis.y * axis.y ) * p.y ) +
				      ( ( ( 1.0f - costheta ) * axis.y * axis.z - axis.x * sintheta ) * p.z );

				r.z = ( ( ( 1.0f - costheta ) * axis.x * axis.z - axis.y * sintheta ) * p.x ) +
				      ( ( ( 1.0f - costheta ) * axis.y * axis.z + axis.x * sintheta ) * p.y ) +
				      ( ( costheta + ( 1.0f - costheta ) * axis.z * axis.z ) * p.z );

				return r;
			}

			// Cycles' util/transform.h: euler_to_transform + transform_direction /
			// transform_direction_transposed (scalar path).
			struct Tfm3x3
			{
				float xx, xy, xz;
				float yx, yy, yz;
				float zx, zy, zz;
			};

			inline Tfm3x3 EulerToTransform( const F3 euler )
			{
				const float cx = std::cos( euler.x );
				const float cy = std::cos( euler.y );
				const float cz = std::cos( euler.z );
				const float sx = std::sin( euler.x );
				const float sy = std::sin( euler.y );
				const float sz = std::sin( euler.z );

				Tfm3x3 t;
				t.xx = cy * cz;
				t.yx = cy * sz;
				t.zx = -sy;

				t.xy = sy * sx * cz - cx * sz;
				t.yy = sy * sx * sz + cx * cz;
				t.zy = cy * sx;

				t.xz = sy * cx * cz + sx * sz;
				t.yz = sy * cx * sz - sx * cz;
				t.zz = cy * cx;
				return t;
			}

			inline F3 TransformDirection( const Tfm3x3& t, const F3 a )
			{
				// Scalar path in util/transform.h uses dot(float4(a, 0), t->row):
				// (a.x * rx + a.y * ry) + (a.z * rz + 0.0f * 0.0f).
				const float zeroTerm = 0.0f * 0.0f;
				return MakeF3(
					( a.x * t.xx + a.y * t.xy ) + ( a.z * t.xz + zeroTerm ),
					( a.x * t.yx + a.y * t.yy ) + ( a.z * t.yz + zeroTerm ),
					( a.x * t.zx + a.y * t.zy ) + ( a.z * t.zz + zeroTerm ) );
			}

			inline F3 TransformDirectionTransposed( const Tfm3x3& t, const F3 a )
			{
				// Scalar path in util/transform.h uses to_local(a, colX, colY, colZ)
				// -> dot(float3, float3) = (a.x * cx + a.y * cy) + a.z * cz.
				return MakeF3(
					a.x * t.xx + a.y * t.yx + a.z * t.zx,
					a.x * t.xy + a.y * t.yy + a.z * t.zy,
					a.x * t.xz + a.y * t.yz + a.z * t.zz );
			}
		}

		void VecMin( const Scalar aIn[3], const Scalar bIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			const F3 b = LoadF3( bIn );
			StoreF3( MakeF3( CclMin( a.x, b.x ), CclMin( a.y, b.y ), CclMin( a.z, b.z ) ), out );
		}

		void VecMax( const Scalar aIn[3], const Scalar bIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			const F3 b = LoadF3( bIn );
			StoreF3( MakeF3( CclMax( a.x, b.x ), CclMax( a.y, b.y ), CclMax( a.z, b.z ) ), out );
		}

		void VecAbs( const Scalar aIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			StoreF3( MakeF3( std::fabs( a.x ), std::fabs( a.y ), std::fabs( a.z ) ), out );
		}

		void VecFloor( const Scalar aIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			StoreF3( MakeF3( std::floor( a.x ), std::floor( a.y ), std::floor( a.z ) ), out );
		}

		void VecFract( const Scalar aIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			StoreF3( MakeF3( a.x - std::floor( a.x ), a.y - std::floor( a.y ), a.z - std::floor( a.z ) ), out );
		}

		void VecPow( const Scalar aIn[3], const Scalar bIn[3], Scalar out[3] )
		{
			const F3 a = LoadF3( aIn );
			const F3 b = LoadF3( bIn );
			StoreF3( MakeF3( SafePowf( a.x, b.x ), SafePowf( a.y, b.y ), SafePowf( a.z, b.z ) ), out );
		}

		void VecClamp( const Scalar aIn[3], const Scalar mnIn[3], const Scalar mxIn[3], Scalar out[3] )
		{
			const F3 a  = LoadF3( aIn );
			const F3 mn = LoadF3( mnIn );
			const F3 mx = LoadF3( mxIn );
			StoreF3( MakeF3( CclClamp( a.x, mn.x, mx.x ), CclClamp( a.y, mn.y, mx.y ), CclClamp( a.z, mn.z, mx.z ) ), out );
		}

		void RotateAroundAxis( const Scalar vIn[3], const Scalar axisIn[3], const Scalar angleIn, Scalar out[3] )
		{
			const F3 v = LoadF3( vIn );
			const F3 axis = LoadF3( axisIn );
			const float angle = static_cast<float>( angleIn );
			const float axisLen = LenF3( axis );
			if( axisLen != 0.0f ) {
				const float invLen = 1.0f / axisLen;
				const F3 unitAxis = MakeF3( axis.x * invLen, axis.y * invLen, axis.z * invLen );
				StoreF3( RotateAroundUnitAxis( v, unitAxis, angle ), out );
			} else {
				StoreF3( v, out );
			}
		}

		void VectorRotate(
			const int type,
			const bool invert,
			const Scalar vIn[3],
			const Scalar centerIn[3],
			const Scalar axisIn[3],
			const Scalar angleIn,
			const Scalar eulerIn[3],
			Scalar out[3] )
		{
			const F3 vector = LoadF3( vIn );
			const F3 center = LoadF3( centerIn );
			const F3 d = MakeF3( vector.x - center.x, vector.y - center.y, vector.z - center.z );
			F3 result = MakeF3( 0.0f, 0.0f, 0.0f );

			if( type == eRotateEulerXYZ ) {
				const F3 rotation = LoadF3( eulerIn );
				const Tfm3x3 t = EulerToTransform( rotation );
				const F3 rot = invert ? TransformDirectionTransposed( t, d ) : TransformDirection( t, d );
				result = MakeF3( rot.x + center.x, rot.y + center.y, rot.z + center.z );
			} else {
				F3 axis;
				float axisLen;
				switch( type )
				{
				case eRotateAxisX:
					axis = MakeF3( 1.0f, 0.0f, 0.0f );
					axisLen = 1.0f;
					break;
				case eRotateAxisY:
					axis = MakeF3( 0.0f, 1.0f, 0.0f );
					axisLen = 1.0f;
					break;
				case eRotateAxisZ:
					axis = MakeF3( 0.0f, 0.0f, 1.0f );
					axisLen = 1.0f;
					break;
				default:
					axis = LoadF3( axisIn );
					axisLen = LenF3( axis );
					break;
				}
				float angle = static_cast<float>( angleIn );
				angle = invert ? -angle : angle;
				if( axisLen != 0.0f ) {
					// Cycles' scalar `axis / axis_length` multiplies by `1.0f / axis_length`.
					const float invLen = 1.0f / axisLen;
					const F3 unitAxis = MakeF3( axis.x * invLen, axis.y * invLen, axis.z * invLen );
					const F3 rot = RotateAroundUnitAxis( d, unitAxis, angle );
					result = MakeF3( rot.x + center.x, rot.y + center.y, rot.z + center.z );
				} else {
					result = vector;
				}
			}

			StoreF3( result, out );
		}
	}
}
