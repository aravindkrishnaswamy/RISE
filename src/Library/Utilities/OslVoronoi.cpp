//////////////////////////////////////////////////////////////////////
//
//  OslVoronoi.cpp - A port of Cycles' Voronoi Texture node.  See
//    OslVoronoi.h for the source, the precision contract and the places
//    the port departs from Cycles.
//
//  Each function below names the Cycles function it ports and keeps its
//  order of operations, so a reader can check one against the other
//  line by line.  Cycles writes each feature four times, once per
//  dimension; here 2D to 4D share one template over the dimension N, and
//  the three dimension-dependent reductions (dot, reduce_add,
//  reduce_max) keep Cycles' per-dimension grouping.  1D has its own
//  functions, as in Cycles, because it hashes float bits and measures
//  every metric as |a - b|.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/kernel/svm/voronoi.h, util/hash.h (the PCG hashes),
//    and the scalar and float2 helpers (mix, smoothstep, min, max, clamp;
//    float2's dot, len_squared, distance, reduce_add, reduce_max, fabs,
//    power, normalize) from util/math_base.h and util/math_float2.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    the same helpers for float3 and float4 (dot, len_squared, distance,
//    reduce_add, reduce_max, fabs, power, normalize, mix, safe_divide)
//    from util/math_float3.h and util/math_float4.h --
//      SPDX-FileCopyrightText: 2011-2013 Intel Corporation
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//
//    all of them --
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    The Smooth F1 and Distance to Edge code in voronoi.h --
//      SPDX-License-Identifier: MIT
//      Original code is copyright (c) 2013 Inigo Quilez.
//      The licence text is licenses/MIT-Inigo-Quilez.txt.
//
//    MODIFIED FOR RISE: see OslVoronoi.h.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslVoronoi.h"
#include "OslNoise.h"	// the lookup3 float hashes 1D uses
#include <cfloat>
#include <cmath>
#include <cstring>

// The kernels are strict IEEE ports, compiled by every build with
// -fno-fast-math -ffp-contract=off (or /fp:strict); OslNoise.cpp's
// "HOW THE STRICT ARITHMETIC IS ENFORCED" explains why the per-file flag
// is needed and what this guard catches.
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslVoronoi.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' Voronoi"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslVoronoi
	{
		namespace
		{
			//////////////////////////////////////////////////////////
			// Integer helpers.  Cycles' PCG hashes and cell arithmetic
			// use signed int and rely on wrap-around; here the
			// arithmetic is unsigned, which wraps by definition, and
			// the conversions back are exact bit copies.
			//////////////////////////////////////////////////////////

			inline int32_t ToInt( const uint32_t u )
			{
				int32_t i;
				std::memcpy( &i, &u, sizeof( i ) );
				return i;
			}

			inline uint32_t ToUint( const int32_t i )
			{
				uint32_t u;
				std::memcpy( &u, &i, sizeof( u ) );
				return u;
			}

			//! Signed `v >> 16` on the bit pattern: an arithmetic shift.
			inline uint32_t Sra16( const uint32_t u )
			{
				return ( u >> 16 ) | ( ( u & 0x80000000u ) ? 0xFFFF0000u : 0u );
			}

			inline int32_t AddWrap( const int32_t a, const int32_t b )
			{
				return ToInt( ToUint( a ) + ToUint( b ) );
			}

			//! make_int{2,3,4} of a floored coordinate.  Cycles converts with an
			//! (int) cast (2D, and 3D/4D on its scalar path) or sse2neon's
			//! _mm_cvtps_epi32 (3D/4D on its SIMD path), both undefined outside
			//! int range.  This is the saturation arm64 compiles the (int) cast
			//! to; the SIMD conversion gives INT32_MIN instead (OslVoronoi.h).
			//! NaN never reaches here (VoronoiTexture guards it) but reads 0, as
			//! arm64's (int) cast would.
			inline int32_t CellId( const float f )
			{
				if( !( f == f ) ) return 0;
				if( f >= 2147483648.0f ) return INT32_MAX;
				if( f < -2147483648.0f ) return INT32_MIN;
				return (int32_t)f;
			}

			//////////////////////////////////////////////////////////
			// util/math_base.h
			//////////////////////////////////////////////////////////

			inline float Min( const float a, const float b ) { return ( a < b ) ? a : b; }
			inline float Max( const float a, const float b ) { return ( a > b ) ? a : b; }
			inline float Clamp( const float a, const float mn, const float mx ) { return Min( Max( a, mn ), mx ); }
			inline float Mix( const float a, const float b, const float t ) { return a + t * ( b - a ); }

			//! smoothstep(0.0f, 1.0f, x)
			inline float SmoothStep01( const float x )
			{
				float result;
				if( x < 0.0f ) {
					result = 0.0f;
				}
				else if( x >= 1.0f ) {
					result = 1.0f;
				}
				else {
					const float t = ( x - 0.0f ) / ( 1.0f - 0.0f );
					result = ( 3.0f - 2.0f * t ) * ( t * t );
				}
				return result;
			}
		}

		//////////////////////////////////////////////////////////////
		// util/hash.h -- the PCG hashes (Jarzynski and Olano 2020, in
		// Blender's signed-integer form)
		//////////////////////////////////////////////////////////////

		void HashInt2ToFloat2( const int32_t kx, const int32_t ky, float out[2] )
		{
			// hash_pcg2d_i
			uint32_t x = ToUint( kx ) * 1664525u + 1013904223u;
			uint32_t y = ToUint( ky ) * 1664525u + 1013904223u;
			x += y * 1664525u;
			y += x * 1664525u;
			x ^= Sra16( x );
			y ^= Sra16( y );
			x += y * 1664525u;
			y += x * 1664525u;
			x &= 0x7FFFFFFFu;
			y &= 0x7FFFFFFFu;
			// hash_int2_to_float2
			out[0] = (float)ToInt( x ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[1] = (float)ToInt( y ) * ( 1.0f / (float)0x7FFFFFFFu );
		}

		void HashInt3ToFloat3( const int32_t kx, const int32_t ky, const int32_t kz, float out[3] )
		{
			// hash_pcg3d_i
			uint32_t x = ToUint( kx ) * 1664525u + 1013904223u;
			uint32_t y = ToUint( ky ) * 1664525u + 1013904223u;
			uint32_t z = ToUint( kz ) * 1664525u + 1013904223u;
			x += y * z;
			y += z * x;
			z += x * y;
			x ^= Sra16( x );
			y ^= Sra16( y );
			z ^= Sra16( z );
			x += y * z;
			y += z * x;
			z += x * y;
			x &= 0x7FFFFFFFu;
			y &= 0x7FFFFFFFu;
			z &= 0x7FFFFFFFu;
			// hash_int3_to_float3
			out[0] = (float)ToInt( x ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[1] = (float)ToInt( y ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[2] = (float)ToInt( z ) * ( 1.0f / (float)0x7FFFFFFFu );
		}

		void HashInt4ToFloat4( const int32_t kx, const int32_t ky, const int32_t kz, const int32_t kw, float out[4] )
		{
			// hash_pcg4d_i
			uint32_t x = ToUint( kx ) * 1664525u + 1013904223u;
			uint32_t y = ToUint( ky ) * 1664525u + 1013904223u;
			uint32_t z = ToUint( kz ) * 1664525u + 1013904223u;
			uint32_t w = ToUint( kw ) * 1664525u + 1013904223u;
			x += y * w;
			y += z * x;
			z += x * y;
			w += y * z;
			x ^= Sra16( x );
			y ^= Sra16( y );
			z ^= Sra16( z );
			w ^= Sra16( w );
			x += y * w;
			y += z * x;
			z += x * y;
			w += y * z;
			x &= 0x7FFFFFFFu;
			y &= 0x7FFFFFFFu;
			z &= 0x7FFFFFFFu;
			w &= 0x7FFFFFFFu;
			// hash_int4_to_float4
			out[0] = (float)ToInt( x ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[1] = (float)ToInt( y ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[2] = (float)ToInt( z ) * ( 1.0f / (float)0x7FFFFFFFu );
			out[3] = (float)ToInt( w ) * ( 1.0f / (float)0x7FFFFFFFu );
		}

		namespace
		{
			//! VoronoiParams
			struct Params
			{
				float scale;
				float detail;
				float roughness;
				float lacunarity;
				float smoothness;
				float exponent;
				float randomness;
				float max_distance;
				bool normalize;
				int feature;
				int metric;
			};

			//! VoronoiOutput
			struct Octave
			{
				float distance;
				float color[3];
				float position[4];
				Octave() : distance( 0.0f )
				{
					color[0] = color[1] = color[2] = 0.0f;
					position[0] = position[1] = position[2] = position[3] = 0.0f;
				}
			};

			//////////////////////////////////////////////////////////
			// Per-dimension reductions (util/math_float{2,3,4}.h,
			// scalar path).  The grouping is Cycles': float4's dot pairs
			// its terms, its reduce_add does not.
			//////////////////////////////////////////////////////////

			template<int N> inline float Dot( const float* a, const float* b );
			template<> inline float Dot<2>( const float* a, const float* b ) { return a[0] * b[0] + a[1] * b[1]; }
			template<> inline float Dot<3>( const float* a, const float* b ) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
			template<> inline float Dot<4>( const float* a, const float* b ) { return ( a[0] * b[0] + a[1] * b[1] ) + ( a[2] * b[2] + a[3] * b[3] ); }

			template<int N> inline float ReduceAdd( const float* a );
			template<> inline float ReduceAdd<2>( const float* a ) { return a[0] + a[1]; }
			template<> inline float ReduceAdd<3>( const float* a ) { return ( a[0] + a[1] + a[2] ); }
			template<> inline float ReduceAdd<4>( const float* a ) { return a[0] + a[1] + a[2] + a[3]; }

			template<int N> inline float ReduceMax( const float* a );
			template<> inline float ReduceMax<2>( const float* a ) { return Max( a[0], a[1] ); }
			template<> inline float ReduceMax<3>( const float* a ) { return Max( Max( a[0], a[1] ), a[2] ); }
			template<> inline float ReduceMax<4>( const float* a ) { return Max( Max( a[0], a[1] ), Max( a[2], a[3] ) ); }

			//! voronoi_distance (the template, 2D..4D)
			template<int N>
			float Distance( const float* a, const float* b, const Params& params )
			{
				float d[N];
				for( int c = 0; c < N; ++c ) d[c] = a[c] - b[c];
				if( params.metric == eEuclidean ) {
					return std::sqrt( Dot<N>( d, d ) );
				}
				if( params.metric == eManhattan ) {
					for( int c = 0; c < N; ++c ) d[c] = std::fabs( d[c] );
					return ReduceAdd<N>( d );
				}
				if( params.metric == eChebychev ) {
					for( int c = 0; c < N; ++c ) d[c] = std::fabs( d[c] );
					return ReduceMax<N>( d );
				}
				if( params.metric == eMinkowski ) {
					for( int c = 0; c < N; ++c ) d[c] = std::pow( std::fabs( d[c] ), params.exponent );
					return std::pow( ReduceAdd<N>( d ), 1.0f / params.exponent );
				}
				return 0.0f;
			}

			//! voronoi_distance_bound: the same ordering, without the final
			//! square root or root.
			template<int N>
			float DistanceBound( const float* a, const float* b, const Params& params )
			{
				float d[N];
				for( int c = 0; c < N; ++c ) d[c] = a[c] - b[c];
				if( params.metric == eEuclidean ) {
					return Dot<N>( d, d );
				}
				if( params.metric == eManhattan ) {
					for( int c = 0; c < N; ++c ) d[c] = std::fabs( d[c] );
					return ReduceAdd<N>( d );
				}
				if( params.metric == eChebychev ) {
					for( int c = 0; c < N; ++c ) d[c] = std::fabs( d[c] );
					return ReduceMax<N>( d );
				}
				if( params.metric == eMinkowski ) {
					for( int c = 0; c < N; ++c ) d[c] = std::pow( std::fabs( d[c] ), params.exponent );
					return ReduceAdd<N>( d );
				}
				return 0.0f;
			}

			//! The feature point of cell `cell + off`: hash_intN_to_floatN.
			template<int N>
			inline void PointHash( const int32_t* cell, const int32_t* off, float* h )
			{
				int32_t k[4] = { 0, 0, 0, 0 };
				for( int c = 0; c < N; ++c ) k[c] = AddWrap( cell[c], off[c] );
				if constexpr( N == 2 ) {
					HashInt2ToFloat2( k[0], k[1], h );
				} else if constexpr( N == 3 ) {
					HashInt3ToFloat3( k[0], k[1], k[2], h );
				} else {
					HashInt4ToFloat4( k[0], k[1], k[2], k[3], h );
				}
			}

			//! The colour of cell `cell + off`: hash_int2_to_float3 (the 3D
			//! hash with z = 0), hash_int3_to_float3, hash_int4_to_float3 (the
			//! 4D hash's first three components).
			template<int N>
			inline void ColorHash( const int32_t* cell, const int32_t* off, float* color )
			{
				int32_t k[4] = { 0, 0, 0, 0 };
				for( int c = 0; c < N; ++c ) k[c] = AddWrap( cell[c], off[c] );
				if constexpr( N == 4 ) {
					float h[4];
					HashInt4ToFloat4( k[0], k[1], k[2], k[3], h );
					color[0] = h[0]; color[1] = h[1]; color[2] = h[2];
				} else {
					HashInt3ToFloat3( k[0], k[1], k[2], color );
				}
			}

			//! voronoi_position: float2 -> (x, y, 0, 0); float3 ->
			//! make_float4(float3) = (x, y, z, 1); float4 as is.
			template<int N>
			inline void ToPosition( const float* v, float* position )
			{
				position[0] = v[0];
				position[1] = v[1];
				if constexpr( N == 2 ) {
					position[2] = 0.0f;
					position[3] = 0.0f;
				} else if constexpr( N == 3 ) {
					position[2] = v[2];
					position[3] = 1.0f;
				} else {
					position[2] = v[2];
					position[3] = v[3];
				}
			}

			//! The neighbour offsets, (-R .. R)^N, in Cycles' loop order:
			//! x innermost (fastest), then y, z, w.  Ties keep the first,
			//! so the order is part of the result.
			template<int N>
			inline void OffsetAt( int idx, const int R, int32_t* off )
			{
				const int W = 2 * R + 1;
				for( int c = 0; c < N; ++c ) {
					off[c] = (int32_t)( idx % W - R );
					idx /= W;
				}
			}

			//! (2R + 1)^N
			template<int N> constexpr int CellCount( const int R )
			{
				int n = 1;
				for( int c = 0; c < N; ++c ) n *= 2 * R + 1;
				return n;
			}

			//! The cell, its integer id and the position within it: the three
			//! lines that open every 2D..4D feature.
			template<int N>
			inline void Locate( const float* coord, float* cellF, float* local, int32_t* cell )
			{
				for( int c = 0; c < N; ++c ) {
					cellF[c] = std::floor( coord[c] );
					local[c] = coord[c] - cellF[c];
					cell[c] = CellId( cellF[c] );
				}
			}

			//! `make_floatN(cellOffset) + hash * params.randomness`
			template<int N>
			inline void FeaturePoint( const int32_t* cell, const int32_t* off, const float randomness, float* point )
			{
				float h[4];
				PointHash<N>( cell, off, h );
				for( int c = 0; c < N; ++c ) point[c] = (float)off[c] + h[c] * randomness;
			}

			//////////////////////////////////////////////////////////
			// 2D .. 4D Voronoi
			//////////////////////////////////////////////////////////

			//! voronoi_f1
			template<int N>
			Octave F1( const Params& params, const float* coord )
			{
				float cellF[N], local[N];
				int32_t cell[N];
				Locate<N>( coord, cellF, local, cell );

				float minDistance = FLT_MAX;
				int32_t targetOffset[N];
				float targetPosition[N];
				for( int c = 0; c < N; ++c ) { targetOffset[c] = 0; targetPosition[c] = 0.0f; }
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					float point[N];
					FeaturePoint<N>( cell, off, params.randomness, point );
					const float distanceToPoint = DistanceBound<N>( point, local, params );
					if( distanceToPoint < minDistance ) {
						for( int c = 0; c < N; ++c ) targetOffset[c] = off[c];
						minDistance = distanceToPoint;
						for( int c = 0; c < N; ++c ) targetPosition[c] = point[c];
					}
				}

				Octave octave;
				octave.distance = Distance<N>( targetPosition, local, params );
				ColorHash<N>( cell, targetOffset, octave.color );
				float p[N];
				for( int c = 0; c < N; ++c ) p[c] = targetPosition[c] + cellF[c];
				ToPosition<N>( p, octave.position );
				return octave;
			}

			//! voronoi_smooth_f1
			template<int N>
			Octave SmoothF1( const Params& params, const float* coord )
			{
				float cellF[N], local[N];
				int32_t cell[N];
				Locate<N>( coord, cellF, local, cell );

				float smoothDistance = 0.0f;
				float smoothColor[3] = { 0.0f, 0.0f, 0.0f };
				float smoothPosition[N];
				for( int c = 0; c < N; ++c ) smoothPosition[c] = 0.0f;
				float h = -1.0f;
				for( int idx = 0; idx < CellCount<N>( 2 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 2, off );
					float point[N];
					FeaturePoint<N>( cell, off, params.randomness, point );
					const float distanceToPoint = Distance<N>( point, local, params );
					h = h == -1.0f ?
						1.0f :
						SmoothStep01( 0.5f + 0.5f * ( smoothDistance - distanceToPoint ) / params.smoothness );
					float correctionFactor = params.smoothness * h * ( 1.0f - h );
					smoothDistance = Mix( smoothDistance, distanceToPoint, h ) - correctionFactor;
					correctionFactor /= 1.0f + 3.0f * params.smoothness;
					float cellColor[3];
					ColorHash<N>( cell, off, cellColor );
					for( int c = 0; c < 3; ++c ) smoothColor[c] = Mix( smoothColor[c], cellColor[c], h ) - correctionFactor;
					for( int c = 0; c < N; ++c ) smoothPosition[c] = Mix( smoothPosition[c], point[c], h ) - correctionFactor;
				}

				Octave octave;
				octave.distance = smoothDistance;
				for( int c = 0; c < 3; ++c ) octave.color[c] = smoothColor[c];
				float p[N];
				for( int c = 0; c < N; ++c ) p[c] = cellF[c] + smoothPosition[c];
				ToPosition<N>( p, octave.position );
				return octave;
			}

			//! voronoi_f2
			template<int N>
			Octave F2( const Params& params, const float* coord )
			{
				float cellF[N], local[N];
				int32_t cell[N];
				Locate<N>( coord, cellF, local, cell );

				float distanceF1 = FLT_MAX;
				float distanceF2 = FLT_MAX;
				int32_t offsetF1[N], offsetF2[N];
				float positionF1[N], positionF2[N];
				for( int c = 0; c < N; ++c ) {
					offsetF1[c] = offsetF2[c] = 0;
					positionF1[c] = positionF2[c] = 0.0f;
				}
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					float point[N];
					FeaturePoint<N>( cell, off, params.randomness, point );
					const float distanceToPoint = Distance<N>( point, local, params );
					if( distanceToPoint < distanceF1 ) {
						distanceF2 = distanceF1;
						distanceF1 = distanceToPoint;
						for( int c = 0; c < N; ++c ) {
							offsetF2[c] = offsetF1[c];
							offsetF1[c] = off[c];
							positionF2[c] = positionF1[c];
							positionF1[c] = point[c];
						}
					}
					else if( distanceToPoint < distanceF2 ) {
						distanceF2 = distanceToPoint;
						for( int c = 0; c < N; ++c ) {
							offsetF2[c] = off[c];
							positionF2[c] = point[c];
						}
					}
				}

				Octave octave;
				octave.distance = distanceF2;
				ColorHash<N>( cell, offsetF2, octave.color );
				float p[N];
				for( int c = 0; c < N; ++c ) p[c] = positionF2[c] + cellF[c];
				ToPosition<N>( p, octave.position );
				return octave;
			}

			//! voronoi_distance_to_edge
			template<int N>
			float DistanceToEdge( const Params& params, const float* coord )
			{
				float cellF[N], local[N];
				int32_t cell[N];
				Locate<N>( coord, cellF, local, cell );

				float vectorToClosest[N];
				for( int c = 0; c < N; ++c ) vectorToClosest[c] = 0.0f;
				float minDistance = FLT_MAX;
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					float vectorToPoint[N];
					FeaturePoint<N>( cell, off, params.randomness, vectorToPoint );
					for( int c = 0; c < N; ++c ) vectorToPoint[c] = vectorToPoint[c] - local[c];
					const float distanceToPoint = Dot<N>( vectorToPoint, vectorToPoint );
					if( distanceToPoint < minDistance ) {
						minDistance = distanceToPoint;
						for( int c = 0; c < N; ++c ) vectorToClosest[c] = vectorToPoint[c];
					}
				}

				minDistance = FLT_MAX;
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					float vectorToPoint[N];
					FeaturePoint<N>( cell, off, params.randomness, vectorToPoint );
					for( int c = 0; c < N; ++c ) vectorToPoint[c] = vectorToPoint[c] - local[c];
					float perpendicularToEdge[N];
					for( int c = 0; c < N; ++c ) perpendicularToEdge[c] = vectorToPoint[c] - vectorToClosest[c];
					if( Dot<N>( perpendicularToEdge, perpendicularToEdge ) > 0.0001f ) {
						// (vectorToClosest + vectorToPoint) / 2.0f and
						// normalize(perpendicularToEdge) = v / len(v): a vector
						// divided by a scalar, which Cycles' scalar path
						// evaluates as a multiply by the reciprocal.
						const float halfInv = 1.0f / 2.0f;
						float mid[N];
						for( int c = 0; c < N; ++c ) mid[c] = ( vectorToClosest[c] + vectorToPoint[c] ) * halfInv;
						const float lenInv = 1.0f / std::sqrt( Dot<N>( perpendicularToEdge, perpendicularToEdge ) );
						float unit[N];
						for( int c = 0; c < N; ++c ) unit[c] = perpendicularToEdge[c] * lenInv;
						const float distanceToEdge = Dot<N>( mid, unit );
						minDistance = Min( minDistance, distanceToEdge );
					}
				}

				return minDistance;
			}

			//! voronoi_n_sphere_radius
			template<int N>
			float NSphereRadius( const Params& params, const float* coord )
			{
				float cellF[N], local[N];
				int32_t cell[N];
				Locate<N>( coord, cellF, local, cell );

				float closestPoint[N];
				int32_t closestPointOffset[N];
				for( int c = 0; c < N; ++c ) { closestPoint[c] = 0.0f; closestPointOffset[c] = 0; }
				float minDistanceSq = FLT_MAX;
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					float point[N];
					FeaturePoint<N>( cell, off, params.randomness, point );
					float d[N];
					for( int c = 0; c < N; ++c ) d[c] = point[c] - local[c];
					const float distanceToPointSq = Dot<N>( d, d );
					if( distanceToPointSq < minDistanceSq ) {
						minDistanceSq = distanceToPointSq;
						for( int c = 0; c < N; ++c ) {
							closestPoint[c] = point[c];
							closestPointOffset[c] = off[c];
						}
					}
				}

				minDistanceSq = FLT_MAX;
				float closestPointToClosestPoint[N];
				for( int c = 0; c < N; ++c ) closestPointToClosestPoint[c] = 0.0f;
				for( int idx = 0; idx < CellCount<N>( 1 ); ++idx ) {
					int32_t off[N];
					OffsetAt<N>( idx, 1, off );
					bool centre = true;
					for( int c = 0; c < N; ++c ) centre = centre && off[c] == 0;
					if( centre ) {
						continue;
					}
					int32_t cellOffset[N];
					for( int c = 0; c < N; ++c ) cellOffset[c] = off[c] + closestPointOffset[c];
					float point[N];
					FeaturePoint<N>( cell, cellOffset, params.randomness, point );
					float d[N];
					for( int c = 0; c < N; ++c ) d[c] = closestPoint[c] - point[c];
					const float distanceToPointSq = Dot<N>( d, d );
					if( distanceToPointSq < minDistanceSq ) {
						minDistanceSq = distanceToPointSq;
						for( int c = 0; c < N; ++c ) closestPointToClosestPoint[c] = point[c];
					}
				}

				float d[N];
				for( int c = 0; c < N; ++c ) d[c] = closestPointToClosestPoint[c] - closestPoint[c];
				return std::sqrt( Dot<N>( d, d ) ) / 2.0f;
			}

			//////////////////////////////////////////////////////////
			// 1D Voronoi: lookup3 on the float bits, |a - b| for every
			// metric.
			//////////////////////////////////////////////////////////

			inline float Hash1( const float k ) { return OslNoise::HashFloatToFloat( k ); }

			//! hash_float_to_float3
			inline void Hash1Color( const float k, float* color )
			{
				color[0] = OslNoise::HashFloatToFloat( k );
				color[1] = OslNoise::HashFloat2ToFloat( k, 1.0f );
				color[2] = OslNoise::HashFloat2ToFloat( k, 2.0f );
			}

			//! voronoi_position(float) = (0, 0, 0, coord)
			inline void ToPosition1( const float v, float* position )
			{
				position[0] = 0.0f; position[1] = 0.0f; position[2] = 0.0f; position[3] = v;
			}

			Octave F1_1D( const Params& params, const float coord )
			{
				const float cellPosition = std::floor( coord );
				const float localPosition = coord - cellPosition;

				float minDistance = FLT_MAX;
				float targetOffset = 0.0f;
				float targetPosition = 0.0f;
				for( int i = -1; i <= 1; i++ ) {
					const float cellOffset = (float)i;
					const float pointPosition = cellOffset + Hash1( cellPosition + cellOffset ) * params.randomness;
					const float distanceToPoint = std::fabs( localPosition - pointPosition );
					if( distanceToPoint < minDistance ) {
						targetOffset = cellOffset;
						minDistance = distanceToPoint;
						targetPosition = pointPosition;
					}
				}

				Octave octave;
				octave.distance = minDistance;
				Hash1Color( cellPosition + targetOffset, octave.color );
				ToPosition1( targetPosition + cellPosition, octave.position );
				return octave;
			}

			Octave SmoothF1_1D( const Params& params, const float coord )
			{
				const float cellPosition = std::floor( coord );
				const float localPosition = coord - cellPosition;

				float smoothDistance = 0.0f;
				float smoothPosition = 0.0f;
				float smoothColor[3] = { 0.0f, 0.0f, 0.0f };
				float h = -1.0f;
				for( int i = -2; i <= 2; i++ ) {
					const float cellOffset = (float)i;
					const float pointPosition = cellOffset + Hash1( cellPosition + cellOffset ) * params.randomness;
					const float distanceToPoint = std::fabs( localPosition - pointPosition );
					h = h == -1.0f ?
						1.0f :
						SmoothStep01( 0.5f + 0.5f * ( smoothDistance - distanceToPoint ) / params.smoothness );
					float correctionFactor = params.smoothness * h * ( 1.0f - h );
					smoothDistance = Mix( smoothDistance, distanceToPoint, h ) - correctionFactor;
					correctionFactor /= 1.0f + 3.0f * params.smoothness;
					float cellColor[3];
					Hash1Color( cellPosition + cellOffset, cellColor );
					for( int c = 0; c < 3; ++c ) smoothColor[c] = Mix( smoothColor[c], cellColor[c], h ) - correctionFactor;
					smoothPosition = Mix( smoothPosition, pointPosition, h ) - correctionFactor;
				}

				Octave octave;
				octave.distance = smoothDistance;
				for( int c = 0; c < 3; ++c ) octave.color[c] = smoothColor[c];
				ToPosition1( cellPosition + smoothPosition, octave.position );
				return octave;
			}

			Octave F2_1D( const Params& params, const float coord )
			{
				const float cellPosition = std::floor( coord );
				const float localPosition = coord - cellPosition;

				float distanceF1 = FLT_MAX;
				float distanceF2 = FLT_MAX;
				float offsetF1 = 0.0f;
				float positionF1 = 0.0f;
				float offsetF2 = 0.0f;
				float positionF2 = 0.0f;
				for( int i = -1; i <= 1; i++ ) {
					const float cellOffset = (float)i;
					const float pointPosition = cellOffset + Hash1( cellPosition + cellOffset ) * params.randomness;
					const float distanceToPoint = std::fabs( localPosition - pointPosition );
					if( distanceToPoint < distanceF1 ) {
						distanceF2 = distanceF1;
						distanceF1 = distanceToPoint;
						offsetF2 = offsetF1;
						offsetF1 = cellOffset;
						positionF2 = positionF1;
						positionF1 = pointPosition;
					}
					else if( distanceToPoint < distanceF2 ) {
						distanceF2 = distanceToPoint;
						offsetF2 = cellOffset;
						positionF2 = pointPosition;
					}
				}

				Octave octave;
				octave.distance = distanceF2;
				Hash1Color( cellPosition + offsetF2, octave.color );
				ToPosition1( positionF2 + cellPosition, octave.position );
				return octave;
			}

			float DistanceToEdge_1D( const Params& params, const float coord )
			{
				const float cellPosition = std::floor( coord );
				const float localPosition = coord - cellPosition;

				const float midPointPosition = Hash1( cellPosition ) * params.randomness;
				const float leftPointPosition = -1.0f + Hash1( cellPosition - 1.0f ) * params.randomness;
				const float rightPointPosition = 1.0f + Hash1( cellPosition + 1.0f ) * params.randomness;
				const float distanceToMidLeft = std::fabs( ( midPointPosition + leftPointPosition ) / 2.0f - localPosition );
				const float distanceToMidRight = std::fabs( ( midPointPosition + rightPointPosition ) / 2.0f - localPosition );

				return Min( distanceToMidLeft, distanceToMidRight );
			}

			float NSphereRadius_1D( const Params& params, const float coord )
			{
				const float cellPosition = std::floor( coord );
				const float localPosition = coord - cellPosition;

				float closestPoint = 0.0f;
				float closestPointOffset = 0.0f;
				float minDistance = FLT_MAX;
				for( int i = -1; i <= 1; i++ ) {
					const float cellOffset = (float)i;
					const float pointPosition = cellOffset + Hash1( cellPosition + cellOffset ) * params.randomness;
					const float distanceToPoint = std::fabs( pointPosition - localPosition );
					if( distanceToPoint < minDistance ) {
						minDistance = distanceToPoint;
						closestPoint = pointPosition;
						closestPointOffset = cellOffset;
					}
				}

				minDistance = FLT_MAX;
				float closestPointToClosestPoint = 0.0f;
				for( int i = -1; i <= 1; i++ ) {
					if( i == 0 ) {
						continue;
					}
					const float cellOffset = (float)i + closestPointOffset;
					const float pointPosition = cellOffset + Hash1( cellPosition + cellOffset ) * params.randomness;
					const float distanceToPoint = std::fabs( closestPoint - pointPosition );
					if( distanceToPoint < minDistance ) {
						minDistance = distanceToPoint;
						closestPointToClosestPoint = pointPosition;
					}
				}

				return std::fabs( closestPointToClosestPoint - closestPoint ) / 2.0f;
			}

			//////////////////////////////////////////////////////////
			// Dispatch by dimension.  D == 1 is the 1D code; the
			// coordinate is always an array.
			//////////////////////////////////////////////////////////

			template<int D>
			Octave OctaveAt( const Params& params, const float* coord )
			{
				if constexpr( D == 1 ) {
					if( params.feature == eF2 ) return F2_1D( params, coord[0] );
					if( params.feature == eSmoothF1 && params.smoothness != 0.0f ) return SmoothF1_1D( params, coord[0] );
					return F1_1D( params, coord[0] );
				} else {
					if( params.feature == eF2 ) return F2<D>( params, coord );
					if( params.feature == eSmoothF1 && params.smoothness != 0.0f ) return SmoothF1<D>( params, coord );
					return F1<D>( params, coord );
				}
			}

			template<int D>
			float EdgeAt( const Params& params, const float* coord )
			{
				if constexpr( D == 1 ) return DistanceToEdge_1D( params, coord[0] );
				else return DistanceToEdge<D>( params, coord );
			}

			template<int D>
			float RadiusAt( const Params& params, const float* coord )
			{
				if constexpr( D == 1 ) return NSphereRadius_1D( params, coord[0] );
				else return NSphereRadius<D>( params, coord );
			}

			//! `coord * scale`, or false when a component is not finite (the
			//! port's one guard inside the fractal loop; see OslVoronoi.h).
			template<int D>
			inline bool ScaledCoord( const float* coord, const float scale, float* scaled )
			{
				bool finite = true;
				for( int c = 0; c < D; ++c ) {
					scaled[c] = coord[c] * scale;
					finite = finite && std::isfinite( scaled[c] );
				}
				return finite;
			}

			//////////////////////////////////////////////////////////
			// Fractal Voronoi
			//////////////////////////////////////////////////////////

			//! fractal_voronoi_x_fx
			template<int D>
			Octave FractalXFx( const Params& params, const float* coord )
			{
				float amplitude = 1.0f;
				float max_amplitude = 0.0f;
				float scale = 1.0f;

				Octave output;
				const bool zero_input = params.detail == 0.0f || params.roughness == 0.0f;

				for( int i = 0; (float)i <= std::ceil( params.detail ); ++i ) {
					float scaled[D];
					if( !ScaledCoord<D>( coord, scale, scaled ) ) {
						break;
					}
					const Octave octave = OctaveAt<D>( params, scaled );

					if( zero_input ) {
						max_amplitude = 1.0f;
						output = octave;
						break;
					}
					if( (float)i <= params.detail ) {
						max_amplitude += amplitude;
						output.distance += octave.distance * amplitude;
						for( int c = 0; c < 3; ++c ) output.color[c] += octave.color[c] * amplitude;
						// mix(output.position, octave.position / scale, amplitude)
						const float invScale = 1.0f / scale;
						for( int c = 0; c < 4; ++c ) output.position[c] = Mix( output.position[c], octave.position[c] * invScale, amplitude );
						scale *= params.lacunarity;
						amplitude *= params.roughness;
					}
					else {
						const float remainder = params.detail - std::floor( params.detail );
						if( remainder != 0.0f ) {
							max_amplitude = Mix( max_amplitude, max_amplitude + amplitude, remainder );
							output.distance = Mix( output.distance, output.distance + octave.distance * amplitude, remainder );
							for( int c = 0; c < 3; ++c ) output.color[c] = Mix( output.color[c], output.color[c] + octave.color[c] * amplitude, remainder );
							const float invScale = 1.0f / scale;
							for( int c = 0; c < 4; ++c ) {
								output.position[c] = Mix( output.position[c],
									Mix( output.position[c], octave.position[c] * invScale, amplitude ), remainder );
							}
						}
					}
				}

				if( params.normalize ) {
					output.distance /= max_amplitude * params.max_distance;
					// output.color /= max_amplitude: a multiply by the reciprocal
					const float inv = 1.0f / max_amplitude;
					for( int c = 0; c < 3; ++c ) output.color[c] = output.color[c] * inv;
				}

				// safe_divide(output.position, params.scale)
				if( params.scale != 0.0f ) {
					const float inv = 1.0f / params.scale;
					for( int c = 0; c < 4; ++c ) output.position[c] = output.position[c] * inv;
				} else {
					for( int c = 0; c < 4; ++c ) output.position[c] = 0.0f;
				}

				return output;
			}

			//! fractal_voronoi_distance_to_edge
			template<int D>
			float FractalDistanceToEdge( const Params& params, const float* coord )
			{
				float amplitude = 1.0f;
				float max_amplitude = params.max_distance;
				float scale = 1.0f;
				float distance = 8.0f;

				const bool zero_input = params.detail == 0.0f || params.roughness == 0.0f;

				for( int i = 0; (float)i <= std::ceil( params.detail ); ++i ) {
					float scaled[D];
					if( !ScaledCoord<D>( coord, scale, scaled ) ) {
						break;
					}
					const float octave_distance = EdgeAt<D>( params, scaled );

					if( zero_input ) {
						distance = octave_distance;
						break;
					}
					if( (float)i <= params.detail ) {
						max_amplitude = Mix( max_amplitude, params.max_distance / scale, amplitude );
						distance = Mix( distance, Min( distance, octave_distance / scale ), amplitude );
						scale *= params.lacunarity;
						amplitude *= params.roughness;
					}
					else {
						const float remainder = params.detail - std::floor( params.detail );
						if( remainder != 0.0f ) {
							const float lerp_amplitude = Mix( max_amplitude, params.max_distance / scale, amplitude );
							max_amplitude = Mix( max_amplitude, lerp_amplitude, remainder );
							const float lerp_distance = Mix( distance, Min( distance, octave_distance / scale ), amplitude );
							distance = Mix( distance, Min( distance, lerp_distance ), remainder );
						}
					}
				}

				if( params.normalize ) {
					distance /= max_amplitude;
				}

				return distance;
			}

			//! The node's max_distance for the F1 / F2 / Smooth F1 features:
			//! voronoi_distance(zero, (m, m, ...), params), doubled for F2,
			//! where m = 0.5 + 0.5 * randomness; 1D is m itself.
			template<int D>
			float MaxDistance( const Params& params )
			{
				const float f2Factor = ( params.feature == eF2 ) ? 2.0f : 1.0f;
				if constexpr( D == 1 ) {
					return ( 0.5f + 0.5f * params.randomness ) * f2Factor;
				} else {
					float zero[D], m[D];
					for( int c = 0; c < D; ++c ) {
						zero[c] = 0.0f;
						m[c] = 0.5f + 0.5f * params.randomness;
					}
					return Distance<D>( zero, m, params ) * f2Factor;
				}
			}

			inline void Zero( Result& out )
			{
				out.distance = Scalar( 0 );
				out.color[0] = out.color[1] = out.color[2] = Scalar( 0 );
				out.position[0] = out.position[1] = out.position[2] = Scalar( 0 );
				out.w = Scalar( 0 );
				out.radius = Scalar( 0 );
			}

			//! svm_node_tex_voronoi after its parameters are read: the
			//! feature switch, for dimension D.
			template<int D>
			void Node( Params& params, const float* coord, Result& out )
			{
				Zero( out );
				switch( params.feature ) {
				case eDistanceToEdge:
					params.max_distance = 0.5f + 0.5f * params.randomness;
					out.distance = FractalDistanceToEdge<D>( params, coord );
					return;
				case eNSphereRadius:
					out.radius = RadiusAt<D>( params, coord );
					return;
				default:
				{
					params.max_distance = MaxDistance<D>( params );
					const Octave output = FractalXFx<D>( params, coord );
					out.distance = output.distance;
					for( int c = 0; c < 3; ++c ) out.color[c] = output.color[c];
					for( int c = 0; c < 3; ++c ) out.position[c] = output.position[c];
					out.w = output.position[3];
					return;
				}
				}
			}
		}

		void VoronoiTexture( const int dims, const Scalar* p, const int feature, const int metric,
			const Scalar scale, const Scalar detail, const Scalar roughness, const Scalar lacunarity,
			const Scalar smoothness, const Scalar exponent, const Scalar randomness, const bool normalize,
			Result& out )
		{
			Zero( out );
			if( dims < 1 || dims > 4 || feature < 0 || feature >= kFeatureCount ||
				metric < 0 || metric >= kMetricCount ) {
				return;
			}

			Params params;
			params.feature = feature;
			params.metric = metric;
			params.scale = (float)scale;
			params.detail = (float)detail;
			params.roughness = (float)roughness;
			params.lacunarity = (float)lacunarity;
			params.smoothness = (float)smoothness;
			params.exponent = (float)exponent;
			params.randomness = (float)randomness;
			params.max_distance = 0.0f;
			params.normalize = normalize;

			params.detail = Clamp( params.detail, 0.0f, 15.0f );
			params.roughness = Clamp( params.roughness, 0.0f, 1.0f );
			params.randomness = Clamp( params.randomness, 0.0f, 1.0f );
			params.smoothness = Clamp( params.smoothness / 2.0f, 0.0f, 0.5f );

			// coord *= params.scale; w *= params.scale
			float coord[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for( int c = 0; c < dims; ++c ) {
				coord[c] = (float)p[c] * params.scale;
				if( !std::isfinite( coord[c] ) ) return;
			}

			switch( dims ) {
			case 1: Node<1>( params, coord, out ); break;
			case 2: Node<2>( params, coord, out ); break;
			case 3: Node<3>( params, coord, out ); break;
			default: Node<4>( params, coord, out ); break;
			}
		}
	}
}
