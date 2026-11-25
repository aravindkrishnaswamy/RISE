//////////////////////////////////////////////////////////////////////
//
//  OslShaderNodes.cpp - Strict IEEE single-precision ports of Cycles'
//    Gradient Texture, Brick Texture, Fresnel, and Layer Weight nodes.
//    See OslShaderNodes.h for the source, precision contract and notices.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/kernel/svm/gradient.h, kernel/svm/brick.h,
//    kernel/svm/fresnel.h, kernel/closure/bsdf_util.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    MODIFIED FOR RISE: see OslShaderNodes.h.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslShaderNodes.h"
#include "FiniteMath.h"
#include <cmath>
#include <cstdint>

#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslShaderNodes.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' shader nodes"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslShaderNodes
	{
		float Gradient(
			const float x,
			const float y,
			const float z,
			const int type )
		{
			if( !IsFiniteDouble( static_cast<double>( x ) ) ||
				!IsFiniteDouble( static_cast<double>( y ) ) ||
				!IsFiniteDouble( static_cast<double>( z ) ) )
			{
				return 0.0f;
			}

			float f = 0.0f;
			switch( type ) {
			case 0: // NODE_BLEND_LINEAR
				f = x;
				break;
			case 1: // NODE_BLEND_QUADRATIC
			{
				const float r = x > 0.0f ? x : 0.0f;
				f = r * r;
				break;
			}
			case 2: // NODE_BLEND_EASING
			{
				const float r = x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
				const float t = r * r;
				f = 3.0f * t - 2.0f * t * r;
				break;
			}
			case 3: // NODE_BLEND_DIAGONAL
				f = ( x + y ) * 0.5f;
				break;
			case 4: // NODE_BLEND_RADIAL
			{
				const float kTwoPiF = 6.28318530717958647692f;
				f = std::atan2( y, x ) / kTwoPiF + 0.5f;
				break;
			}
			case 5: // NODE_BLEND_QUADRATIC_SPHERE
			{
				const float r0 = 0.999999f - std::sqrt( x * x + y * y + z * z );
				const float r = r0 > 0.0f ? r0 : 0.0f;
				f = r * r;
				break;
			}
			case 6: // NODE_BLEND_SPHERICAL
			{
				const float r = 0.999999f - std::sqrt( x * x + y * y + z * z );
				f = r > 0.0f ? r : 0.0f;
				break;
			}
			default:
				f = 0.0f;
				break;
			}

			if( !IsFiniteDouble( static_cast<double>( f ) ) ) {
				return 0.0f;
			}
			if( f < 0.0f ) return 0.0f;
			if( f > 1.0f ) return 1.0f;
			return f;
		}

		void Brick(
			const float px,
			const float py,
			const float mortar_size,
			const float mortar_smooth,
			const float bias,
			const float brick_width_in,
			const float row_height,
			const float offset_amount,
			const int offset_frequency,
			const float squash_amount,
			const int squash_frequency,
			float& outTint,
			float& outMortar )
		{
			outTint = 0.0f;
			outMortar = 0.0f;

			if( !IsFiniteDouble( static_cast<double>( px ) ) ||
				!IsFiniteDouble( static_cast<double>( py ) ) ||
				!IsFiniteDouble( static_cast<double>( mortar_size ) ) ||
				!IsFiniteDouble( static_cast<double>( mortar_smooth ) ) ||
				!IsFiniteDouble( static_cast<double>( bias ) ) ||
				!IsFiniteDouble( static_cast<double>( brick_width_in ) ) ||
				!IsFiniteDouble( static_cast<double>( row_height ) ) ||
				!IsFiniteDouble( static_cast<double>( offset_amount ) ) ||
				!IsFiniteDouble( static_cast<double>( squash_amount ) ) )
			{
				return;
			}

			float brick_width = brick_width_in;
			if( !( std::fabs( row_height ) > 1e-8f ) || !( std::fabs( brick_width ) > 1e-8f ) ) {
				return;
			}

			const float row_f = std::floor( py / row_height );
			if( !IsFiniteDouble( static_cast<double>( row_f ) ) || std::fabs( row_f ) > 1e9f ) {
				return;
			}
			const int rownum = static_cast<int>( row_f );

			float offset = 0.0f;
			if( offset_frequency > 0 && squash_frequency > 0 ) {
				brick_width *= ( rownum % squash_frequency ) ? 1.0f : squash_amount;
				if( !( std::fabs( brick_width ) > 1e-8f ) ) {
					return;
				}
				offset = ( rownum % offset_frequency ) ? 0.0f : ( brick_width * offset_amount );
			}

			const float brick_f = std::floor( ( px + offset ) / brick_width );
			if( !IsFiniteDouble( static_cast<double>( brick_f ) ) || std::fabs( brick_f ) > 1e9f ) {
				return;
			}
			const int bricknum = static_cast<int>( brick_f );

			const float x = ( px + offset ) - brick_width * static_cast<float>( bricknum );
			const float y = py - row_height * static_cast<float>( rownum );

			uint32_t n = ( ( static_cast<uint32_t>( rownum ) << 16 ) + ( static_cast<uint32_t>( bricknum ) & 0xFFFFu ) );
			n = ( n + 1013u ) & 0x7fffffffu;
			n = ( n >> 13 ) ^ n;
			const uint32_t nn = ( n * ( n * n * 60493u + 19990303u ) + 1376312589u ) & 0x7fffffffu;
			const float noise = 0.5f * ( static_cast<float>( nn ) / 1073741824.0f );

			float tint = noise + bias;
			if( tint < 0.0f ) tint = 0.0f;
			else if( tint > 1.0f ) tint = 1.0f;

			const float bw_x = brick_width - x;
			const float rh_y = row_height - y;
			const float min_xy = x < y ? x : y;
			const float min_rem = bw_x < rh_y ? bw_x : rh_y;
			const float min_dist = min_xy < min_rem ? min_xy : min_rem;

			float mortar = 0.0f;
			if( min_dist >= mortar_size ) {
				mortar = 0.0f;
			} else if( mortar_smooth == 0.0f || !( mortar_size > 0.0f ) ) {
				mortar = 1.0f;
			} else {
				const float t0 = ( 1.0f - min_dist / mortar_size ) / mortar_smooth;
				const float t = t0 < 0.0f ? 0.0f : ( t0 > 1.0f ? 1.0f : t0 );
				mortar = t * t * ( 3.0f - 2.0f * t );
			}

			outTint = IsFiniteDouble( static_cast<double>( tint ) ) ? tint : 0.0f;
			outMortar = IsFiniteDouble( static_cast<double>( mortar ) ) ? mortar : 0.0f;
		}

		float FresnelDielectricCos(
			const float cosi,
			const float eta )
		{
			const float c = std::fabs( cosi );
			float g = eta * eta - 1.0f + c * c;
			if( g > 0.0f ) {
				g = std::sqrt( g );
				const float A = ( g - c ) / ( g + c );
				const float B = ( c * ( g + c ) - 1.0f ) / ( c * ( g - c ) + 1.0f );
				const float r = 0.5f * A * A * ( 1.0f + B * B );
				return IsFiniteDouble( static_cast<double>( r ) ) ? r : 1.0f;
			}
			return 1.0f;
		}

		float FresnelNode(
			const float ior,
			const float cosi,
			const bool backfacing )
		{
			const float param = IsFiniteDouble( static_cast<double>( ior ) ) ? ior : 1e-5f;
			float eta = param > 1e-5f ? param : 1e-5f;
			eta = backfacing ? ( 1.0f / eta ) : eta;
			const float f = FresnelDielectricCos( cosi, eta );
			return IsFiniteDouble( static_cast<double>( f ) ) ? f : 0.0f;
		}

		float LayerWeightNode(
			const float blend,
			const float cosi,
			const bool backfacing,
			const bool isFacing )
		{
			const float param = IsFiniteDouble( static_cast<double>( blend ) ) ? blend : 0.5f;
			float f = 0.0f;
			if( !isFacing ) {
				// Layer Weight: Fresnel
				float eta = ( 1.0f - param ) > 1e-5f ? ( 1.0f - param ) : 1e-5f;
				eta = backfacing ? eta : ( 1.0f / eta );
				f = FresnelDielectricCos( cosi, eta );
			} else {
				// Layer Weight: Facing
				f = std::fabs( cosi );
				if( f > 1.0f ) f = 1.0f;
				if( param != 0.5f ) {
					float b = param < 0.0f ? 0.0f : ( param > ( 1.0f - 1e-5f ) ? ( 1.0f - 1e-5f ) : param );
					b = ( b < 0.5f ) ? ( 2.0f * b ) : ( 0.5f / ( 1.0f - b ) );
					f = std::pow( f, b );
				}
				f = 1.0f - f;
			}
			return IsFiniteDouble( static_cast<double>( f ) ) ? f : 0.0f;
		}
	}
}
