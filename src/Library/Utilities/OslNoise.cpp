//////////////////////////////////////////////////////////////////////
//
//  OslNoise.cpp - Ports of Cycles' gradient noise, fractal noise, Noise
//    Texture and White Noise kernels.  See OslNoise.h for the sources and the
//    precision contract.
//
//  Each function below names the Cycles function it ports and keeps its
//  order of operations, so a reader can check one against the other
//  line by line.
//
//  LICENCES.  This file contains code ported, with modifications, from
//  Blender's Cycles renderer (commit afc6d7a4):
//
//    intern/cycles/kernel/svm/noise.h (the Perlin core) -- adapted by
//    Blender from Open Shading Language:
//      SPDX-FileCopyrightText: 2009-2010 Sony Pictures Imageworks Inc., et al.
//                              All Rights Reserved.
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: BSD-3-Clause
//      The full licence text is licenses/BSD-3-Clause.txt.
//
//    intern/cycles/kernel/svm/fractal_noise.h, kernel/svm/noisetex.h,
//    kernel/svm/white_noise.h, util/hash.h, and the helpers mix /
//    floorfrac / saturatef from util/math_base.h --
//      SPDX-FileCopyrightText: 2011-2022 Blender Foundation
//      SPDX-License-Identifier: Apache-2.0
//      Licensed under the Apache License, Version 2.0; a copy is in
//      licenses/Apache-2.0.txt.  Distributed on an "AS IS" BASIS,
//      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
//
//    The lookup3 hash is Bob Jenkins's public-domain code
//    (http://burtleburtle.net/bob/c/lookup3.c).
//
//    MODIFIED FOR RISE: rewritten as standalone functions, the four
//    per-dimension overloads of each fractal merged into one, and guards
//    added for non-finite coordinates (gradient noise) and runaway detail.
//
//  Tabs: 4
//
//  License Information: the code ported from Cycles is under the
//  licences above; RISE's own code is under LICENSE.TXT.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OslNoise.h"
#include "FiniteMath.h"
#include <cmath>
#include <cstring>

namespace RISE
{
	namespace OslNoise
	{
		//////////////////////////////////////////////////////////////
		// util/hash.h -- Bob Jenkins's lookup3 (public domain)
		//////////////////////////////////////////////////////////////

		static inline uint32_t Rot( const uint32_t x, const int k )
		{
			return ( x << k ) | ( x >> ( 32 - k ) );
		}

		static inline void Lookup3Mix( uint32_t& a, uint32_t& b, uint32_t& c )
		{
			a -= c; a ^= Rot( c, 4 );  c += b;
			b -= a; b ^= Rot( a, 6 );  a += c;
			c -= b; c ^= Rot( b, 8 );  b += a;
			a -= c; a ^= Rot( c, 16 ); c += b;
			b -= a; b ^= Rot( a, 19 ); a += c;
			c -= b; c ^= Rot( b, 4 );  b += a;
		}

		static inline void Lookup3Final( uint32_t& a, uint32_t& b, uint32_t& c )
		{
			c ^= b; c -= Rot( b, 14 );
			a ^= c; a -= Rot( c, 11 );
			b ^= a; b -= Rot( a, 25 );
			c ^= b; c -= Rot( b, 16 );
			a ^= c; a -= Rot( c, 4 );
			b ^= a; b -= Rot( a, 14 );
			c ^= b; c -= Rot( b, 24 );
		}

		uint32_t HashUint( const uint32_t kx )
		{
			uint32_t a, b, c;
			a = b = c = 0xdeadbeefu + ( 1u << 2 ) + 13u;
			a += kx;
			Lookup3Final( a, b, c );
			return c;
		}

		uint32_t HashUint2( const uint32_t kx, const uint32_t ky )
		{
			uint32_t a, b, c;
			a = b = c = 0xdeadbeefu + ( 2u << 2 ) + 13u;
			b += ky;
			a += kx;
			Lookup3Final( a, b, c );
			return c;
		}

		uint32_t HashUint3( const uint32_t kx, const uint32_t ky, const uint32_t kz )
		{
			uint32_t a, b, c;
			a = b = c = 0xdeadbeefu + ( 3u << 2 ) + 13u;
			c += kz;
			b += ky;
			a += kx;
			Lookup3Final( a, b, c );
			return c;
		}

		uint32_t HashUint4( const uint32_t kx, const uint32_t ky, const uint32_t kz, const uint32_t kw )
		{
			uint32_t a, b, c;
			a = b = c = 0xdeadbeefu + ( 4u << 2 ) + 13u;
			a += kx;
			b += ky;
			c += kz;
			Lookup3Mix( a, b, c );
			a += kw;
			Lookup3Final( a, b, c );
			return c;
		}

		float UintToFloatIncl( const uint32_t n )
		{
			return (float)n * ( 1.0f / (float)0xFFFFFFFFu );
		}

		static inline uint32_t FloatBits( const float f )
		{
			uint32_t u;
			std::memcpy( &u, &f, sizeof( u ) );
			return u;
		}

		float HashFloatToFloat( const float k )
		{
			return UintToFloatIncl( HashUint( FloatBits( k ) ) );
		}

		float HashFloat2ToFloat( const float kx, const float ky )
		{
			return UintToFloatIncl( HashUint2( FloatBits( kx ), FloatBits( ky ) ) );
		}

		float HashFloat3ToFloat( const float kx, const float ky, const float kz )
		{
			return UintToFloatIncl( HashUint3( FloatBits( kx ), FloatBits( ky ), FloatBits( kz ) ) );
		}

		float HashFloat4ToFloat( const float kx, const float ky, const float kz, const float kw )
		{
			return UintToFloatIncl( HashUint4( FloatBits( kx ), FloatBits( ky ), FloatBits( kz ), FloatBits( kw ) ) );
		}

		//////////////////////////////////////////////////////////////
		// kernel/svm/white_noise.h (Apache-2.0): svm_node_tex_white_noise
		// Integer lookup3 hashing of 32-bit float bit patterns.
		//////////////////////////////////////////////////////////////

		void WhiteNoise( const int dims, const Scalar* p, Scalar& value, Scalar* color )
		{
			float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for( int k = 0; k < dims && k < 4; ++k ) c[k] = (float)p[k];
			switch( dims ) {
			case 1:
				value = HashFloatToFloat( c[0] );
				if( color ) {
					// hash_float_to_float3
					color[0] = HashFloatToFloat( c[0] );
					color[1] = HashFloat2ToFloat( c[0], 1.0f );
					color[2] = HashFloat2ToFloat( c[0], 2.0f );
				}
				return;
			case 2:
				value = HashFloat2ToFloat( c[0], c[1] );
				if( color ) {
					// hash_float2_to_float3
					color[0] = HashFloat2ToFloat( c[0], c[1] );
					color[1] = HashFloat3ToFloat( c[0], c[1], 1.0f );
					color[2] = HashFloat3ToFloat( c[0], c[1], 2.0f );
				}
				return;
			case 3:
				value = HashFloat3ToFloat( c[0], c[1], c[2] );
				if( color ) {
					// hash_float3_to_float3
					color[0] = HashFloat3ToFloat( c[0], c[1], c[2] );
					color[1] = HashFloat4ToFloat( c[0], c[1], c[2], 1.0f );
					color[2] = HashFloat4ToFloat( c[0], c[1], c[2], 2.0f );
				}
				return;
			case 4:
				value = HashFloat4ToFloat( c[0], c[1], c[2], c[3] );
				if( color ) {
					// hash_float4_to_float3: the components permuted, not extended
					color[0] = HashFloat4ToFloat( c[0], c[1], c[2], c[3] );
					color[1] = HashFloat4ToFloat( c[2], c[0], c[3], c[1] );
					color[2] = HashFloat4ToFloat( c[3], c[2], c[1], c[0] );
				}
				return;
			default:
				// Cycles' default: Value 0, Color magenta.
				value = Scalar(0);
				if( color ) { color[0] = Scalar(1); color[1] = Scalar(0); color[2] = Scalar(1); }
				return;
			}
		}

	}
}

// Strict IEEE float32 contract (no FMA, reassociation, or reciprocal math):
// Clang ignores `#pragma clang fp contract(off)` under -ffast-math, so every
// build compiles this TU with -fno-fast-math -ffp-contract=off (or /fp:strict).
// The #error guard rejects fast-math builds; the pragma catches clang builds
// that omit -ffp-contract=off.
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error "OslNoise.cpp must be compiled without fast-math (-fno-fast-math -ffp-contract=off, or /fp:strict): its kernels are strict IEEE ports of Cycles' noise"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RISE
{
	namespace OslNoise
	{
		//////////////////////////////////////////////////////////////
		// kernel/svm/noise.h -- Perlin core (BSD-3-Clause, adapted by
		// Blender from Open Shading Language; see OslNoise.h).
		// Cycles' scalar-path float32 operation order with fmodf wrap.
		//////////////////////////////////////////////////////////////

		static inline float Fade( const float t )
		{
			return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f );
		}

		static inline float NegateIf( const float val, const uint32_t condition )
		{
			return condition ? -val : val;
		}

		//! floorfrac: the integer lattice cell and the fraction within it.
		static inline float FloorFrac( const float x, int& i )
		{
			const float f = std::floor( x );
			i = (int)f;
			return x - f;
		}

		static inline float Mix( const float a, const float b, const float t )
		{
			return a + t * ( b - a );
		}

		static inline float Grad1( const uint32_t hash, const float x )
		{
			const uint32_t h = hash & 15u;
			const float g = float( 1 + (int)( h & 7u ) );
			return NegateIf( g, h & 8u ) * x;
		}

		static inline float Grad2( const uint32_t hash, const float x, const float y )
		{
			const uint32_t h = hash & 7u;
			const float u = h < 4u ? x : y;
			const float v = 2.0f * ( h < 4u ? y : x );
			return NegateIf( u, h & 1u ) + NegateIf( v, h & 2u );
		}

		static inline float Grad3( const uint32_t hash, const float x, const float y, const float z )
		{
			const uint32_t h = hash & 15u;
			const float u = h < 8u ? x : y;
			const float vt = ( ( h == 12u ) || ( h == 14u ) ) ? x : z;
			const float v = h < 4u ? y : vt;
			return NegateIf( u, h & 1u ) + NegateIf( v, h & 2u );
		}

		static inline float Grad4( const uint32_t hash, const float x, const float y, const float z, const float w )
		{
			const uint32_t h = hash & 31u;
			const float u = h < 24u ? x : y;
			const float v = h < 16u ? y : z;
			const float s = h < 8u ? z : w;
			return NegateIf( u, h & 1u ) + NegateIf( v, h & 2u ) + NegateIf( s, h & 4u );
		}

		static inline float BiMix( const float v0, const float v1, const float v2, const float v3,
			const float x, const float y )
		{
			const float x1 = 1.0f - x;
			return ( 1.0f - y ) * ( v0 * x1 + v1 * x ) + y * ( v2 * x1 + v3 * x );
		}

		//! tri_mix over corners v[0..7] in Cycles' order: bit 0 of the index
		//! is +x, bit 1 is +y, bit 2 is +z.
		static inline float TriMix( const float* v, const float x, const float y, const float z )
		{
			const float x1 = 1.0f - x;
			const float y1 = 1.0f - y;
			const float z1 = 1.0f - z;
			return z1 * ( y1 * ( v[0] * x1 + v[1] * x ) + y * ( v[2] * x1 + v[3] * x ) ) +
			       z  * ( y1 * ( v[4] * x1 + v[5] * x ) + y * ( v[6] * x1 + v[7] * x ) );
		}

		static float Perlin1D( const float x )
		{
			int X;
			const float fx = FloorFrac( x, X );
			const float u = Fade( fx );
			return Mix( Grad1( HashUint( (uint32_t)X ), fx ),
			            Grad1( HashUint( (uint32_t)( X + 1 ) ), fx - 1.0f ), u );
		}

		static float Perlin2D( const float x, const float y )
		{
			int X, Y;
			const float fx = FloorFrac( x, X );
			const float fy = FloorFrac( y, Y );
			const float u = Fade( fx );
			const float v = Fade( fy );
			return BiMix(
				Grad2( HashUint2( (uint32_t)X,         (uint32_t)Y ),         fx,        fy ),
				Grad2( HashUint2( (uint32_t)( X + 1 ), (uint32_t)Y ),         fx - 1.0f, fy ),
				Grad2( HashUint2( (uint32_t)X,         (uint32_t)( Y + 1 ) ), fx,        fy - 1.0f ),
				Grad2( HashUint2( (uint32_t)( X + 1 ), (uint32_t)( Y + 1 ) ), fx - 1.0f, fy - 1.0f ),
				u, v );
		}

		static float Perlin3D( const float x, const float y, const float z )
		{
			int X, Y, Z;
			const float fx = FloorFrac( x, X );
			const float fy = FloorFrac( y, Y );
			const float fz = FloorFrac( z, Z );
			const float u = Fade( fx );
			const float v = Fade( fy );
			const float w = Fade( fz );
			float g[8];
			for( int c = 0; c < 8; ++c ) {
				const int dx = c & 1, dy = ( c >> 1 ) & 1, dz = ( c >> 2 ) & 1;
				g[c] = Grad3( HashUint3( (uint32_t)( X + dx ), (uint32_t)( Y + dy ), (uint32_t)( Z + dz ) ),
					fx - float( dx ), fy - float( dy ), fz - float( dz ) );
			}
			return TriMix( g, u, v, w );
		}

		static float Perlin4D( const float x, const float y, const float z, const float w )
		{
			int X, Y, Z, W;
			const float fx = FloorFrac( x, X );
			const float fy = FloorFrac( y, Y );
			const float fz = FloorFrac( z, Z );
			const float fw = FloorFrac( w, W );
			const float u = Fade( fx );
			const float v = Fade( fy );
			const float t = Fade( fz );
			const float s = Fade( fw );
			// quad_mix: v0..v7 on the W face, v8..v15 on the W+1 face,
			// each face in tri_mix's corner order.
			float g[16];
			for( int c = 0; c < 16; ++c ) {
				const int dx = c & 1, dy = ( c >> 1 ) & 1, dz = ( c >> 2 ) & 1, dw = ( c >> 3 ) & 1;
				g[c] = Grad4( HashUint4( (uint32_t)( X + dx ), (uint32_t)( Y + dy ), (uint32_t)( Z + dz ), (uint32_t)( W + dw ) ),
					fx - float( dx ), fy - float( dy ), fz - float( dz ), fw - float( dw ) );
			}
			return Mix( TriMix( g, u, v, t ), TriMix( g + 8, u, v, t ), s );
		}

		static float PerlinF( const int dims, const float* p )
		{
			switch( dims ) {
			case 1: return Perlin1D( p[0] );
			case 2: return Perlin2D( p[0], p[1] );
			case 3: return Perlin3D( p[0], p[1], p[2] );
			case 4: return Perlin4D( p[0], p[1], p[2], p[3] );
			default: return 0.0f;
			}
		}

		// noise_scale1 .. noise_scale4: OSL's experimentally measured
		// remaps to [-1, 1].
		static const float kNoiseScale[5] = { 0.0f, 0.2500f, 0.6616f, 0.9820f, 0.8344f };

		//! snoise_Nd on float coordinates.  0 for a coordinate that is not
		//! finite, checked BEFORE the wrap: fmod of an infinity is NaN,
		//! and a NaN reaching FloorFrac's int conversion is undefined.
		static float SNoiseF( const int dims, const float* p )
		{
			if( dims < 1 || dims > 4 ) return 0.0f;
			float q[4];
			for( int k = 0; k < dims; ++k ) {
				if( !IsFiniteDouble( (double)p[k] ) ) return 0.0f;
				// Repeat every 100000 on each axis; the 0.5 shift at
				// |p| >= 1e6 is Cycles' precision correction, decided on
				// each component BEFORE its wrap, as Cycles does.
				const float precisionCorrection = 0.5f * float( std::fabs( p[k] ) >= 1000000.0f );
				q[k] = std::fmod( p[k], 100000.0f ) + precisionCorrection;
			}
			return kNoiseScale[dims] * PerlinF( dims, q );
		}

		static inline void ToFloat( const int dims, const Scalar* p, float* f )
		{
			for( int k = 0; k < 4; ++k ) f[k] = ( k < dims ) ? (float)p[k] : 0.0f;
		}

		Scalar SNoise( const int dims, const Scalar* p )
		{
			if( dims < 1 || dims > 4 ) return Scalar(0);
			float f[4];
			ToFloat( dims, p, f );
			return Scalar( SNoiseF( dims, f ) );
		}

		//////////////////////////////////////////////////////////////
		// kernel/svm/fractal_noise.h (Apache-2.0) -- unified 1D..4D
		//////////////////////////////////////////////////////////////

		namespace
		{
			//! A point of 1..4 float coordinates, scaled and offset the way
			//! the Cycles bodies scale their float/float2/float3/float4 `p`.
			struct Pt
			{
				float c[4];
				int dims;
			};

			inline float SN( const Pt& p ) { return SNoiseF( p.dims, p.c ); }

			inline Pt Scaled( const Pt& p, const float s )
			{
				Pt r = p;
				for( int k = 0; k < p.dims; ++k ) r.c[k] = s * p.c[k];
				return r;
			}

			inline void ScaleInPlace( Pt& p, const float s )
			{
				for( int k = 0; k < p.dims; ++k ) p.c[k] *= s;
			}

			inline Pt Plus( const Pt& a, const Pt& b )
			{
				Pt r = a;
				for( int k = 0; k < a.dims; ++k ) r.c[k] = a.c[k] + b.c[k];
				return r;
			}

			//! Octave count and fractional remainder matching Cycles, clamped to
			//! [kMinDetail, kMaxDetail] (NaN -> 0) to keep int conversion defined
			//! and bound loop iterations.
			inline void OctaveCount( float detail, int& octaves, float& rmd )
			{
				if( detail != detail ) detail = 0.0f;	// NaN
				if( detail > float( kMaxDetail ) ) detail = float( kMaxDetail );
				if( detail < float( kMinDetail ) ) detail = float( kMinDetail );
				octaves = (int)detail;
				rmd = detail - std::floor( detail );
			}

			float Fbm( const Pt& p, const float detail, const float roughness, const float lacunarity, const bool normalize )
			{
				int n; float rmd;
				OctaveCount( detail, n, rmd );
				float fscale = 1.0f, amp = 1.0f, maxamp = 0.0f, sum = 0.0f;
				for( int i = 0; i <= n; ++i ) {
					const float t = SN( Scaled( p, fscale ) );
					sum += t * amp;
					maxamp += amp;
					amp *= roughness;
					fscale *= lacunarity;
				}
				if( rmd != 0.0f ) {
					const float t = SN( Scaled( p, fscale ) );
					const float sum2 = sum + t * amp;
					return normalize
						? Mix( 0.5f * sum / maxamp + 0.5f, 0.5f * sum2 / ( maxamp + amp ) + 0.5f, rmd )
						: Mix( sum, sum2, rmd );
				}
				return normalize ? 0.5f * sum / maxamp + 0.5f : sum;
			}

			float MultiFractal( Pt p, const float detail, const float roughness, const float lacunarity )
			{
				int n; float rmd;
				OctaveCount( detail, n, rmd );
				float value = 1.0f, pwr = 1.0f;
				for( int i = 0; i <= n; ++i ) {
					value *= ( pwr * SN( p ) + 1.0f );
					pwr *= roughness;
					ScaleInPlace( p, lacunarity );
				}
				if( rmd != 0.0f ) {
					value *= ( rmd * pwr * SN( p ) + 1.0f );
				}
				return value;
			}

			float HeteroTerrain( Pt p, const float detail, const float roughness, const float lacunarity, const float offset )
			{
				int n; float rmd;
				OctaveCount( detail, n, rmd );
				float pwr = roughness;
				// first unscaled octave of function; later octaves are scaled
				float value = offset + SN( p );
				ScaleInPlace( p, lacunarity );
				for( int i = 1; i <= n; ++i ) {
					const float increment = ( SN( p ) + offset ) * pwr * value;
					value += increment;
					pwr *= roughness;
					ScaleInPlace( p, lacunarity );
				}
				if( rmd != 0.0f ) {
					const float increment = ( SN( p ) + offset ) * pwr * value;
					value += rmd * increment;
				}
				return value;
			}

			float HybridMultiFractal( Pt p, const float detail, const float roughness, const float lacunarity,
				const float offset, const float gain )
			{
				int n; float rmd;
				OctaveCount( detail, n, rmd );
				float pwr = 1.0f, value = 0.0f, weight = 1.0f;
				for( int i = 0; ( weight > 0.001f ) && ( i <= n ); ++i ) {
					weight = std::fmin( weight, 1.0f );
					const float signal = ( SN( p ) + offset ) * pwr;
					pwr *= roughness;
					value += weight * signal;
					weight *= gain * signal;
					ScaleInPlace( p, lacunarity );
				}
				if( ( rmd != 0.0f ) && ( weight > 0.001f ) ) {
					weight = std::fmin( weight, 1.0f );
					const float signal = ( SN( p ) + offset ) * pwr;
					value += rmd * weight * signal;
				}
				return value;
			}

			float RidgedMultiFractal( Pt p, const float detail, const float roughness, const float lacunarity,
				const float offset, const float gain )
			{
				int n; float rmd;
				OctaveCount( detail, n, rmd );	// ridged has no partial octave in Cycles; rmd is unused
				float pwr = roughness;
				float signal = offset - std::fabs( SN( p ) );
				signal *= signal;
				float value = signal;
				float weight = 1.0f;
				for( int i = 1; i <= n; ++i ) {
					ScaleInPlace( p, lacunarity );
					// saturatef = clamp(x, 0, 1) = min(max(x, 0), 1) with
					// Cycles' comparison min/max.
					const float sg = signal * gain;
					weight = ( sg > 0.0f ) ? sg : 0.0f;
					weight = ( weight < 1.0f ) ? weight : 1.0f;
					signal = offset - std::fabs( SN( p ) );
					signal *= signal;
					signal *= weight;
					value += signal * pwr;
					pwr *= roughness;
				}
				return value;
			}

			float Select( const Pt& p, const int type, const float detail, const float roughness,
				const float lacunarity, const float offset, const float gain, const bool normalize )
			{
				switch( type ) {
				case eMultifractal:       return MultiFractal( p, detail, roughness, lacunarity );
				case eFbm:                return Fbm( p, detail, roughness, lacunarity, normalize );
				case eHybridMultifractal: return HybridMultiFractal( p, detail, roughness, lacunarity, offset, gain );
				case eRidgedMultifractal: return RidgedMultiFractal( p, detail, roughness, lacunarity, offset, gain );
				case eHeteroTerrain:      return HeteroTerrain( p, detail, roughness, lacunarity, offset );
				default:                  return 0.0f;
				}
			}

			//! Every coordinate finite, after the conversion to float (so a
			//! double beyond float range counts as infinite).
			bool AllFinite( const Pt& p )
			{
				for( int k = 0; k < p.dims; ++k ) if( !IsFiniteDouble( (double)p.c[k] ) ) return false;
				return true;
			}

			Pt MakePt( const int dims, const Scalar* p )
			{
				Pt r;
				r.dims = dims;
				ToFloat( dims, p, r.c );
				return r;
			}

			//! noisetex.h's random_float_offset (1D) and
			//! random_float{2,3,4}_offset: 100 + hash * 100 per component,
			//! in float as Cycles computes it.
			Pt RandomOffset( const int dims, const float seed )
			{
				Pt r;
				r.dims = dims;
				for( int k = 0; k < 4; ++k ) r.c[k] = 0.0f;
				if( dims == 1 ) {
					r.c[0] = 100.0f + HashFloatToFloat( seed ) * 100.0f;
					return r;
				}
				for( int k = 0; k < dims; ++k ) {
					r.c[k] = 100.0f + HashFloat2ToFloat( seed, (float)k ) * 100.0f;
				}
				return r;
			}
		}

		Scalar Fractal( const int dims, const Scalar* p, const int type,
			const Scalar detail, const Scalar roughness, const Scalar lacunarity,
			const Scalar offset, const Scalar gain, const bool normalize )
		{
			if( dims < 1 || dims > 4 ) return Scalar(0);
			const Pt pt = MakePt( dims, p );
			if( !AllFinite( pt ) ) return Scalar(0);
			return Scalar( Select( pt, type, (float)detail, (float)roughness, (float)lacunarity,
				(float)offset, (float)gain, normalize ) );
		}

		//////////////////////////////////////////////////////////////
		// kernel/svm/noisetex.h (Apache-2.0): noise_texture_{1,2,3,4}d
		// and the two clamps svm_node_tex_noise applies before them.
		//////////////////////////////////////////////////////////////

		void NoiseTexture( const int dims, const Scalar* pIn, const int type,
			const Scalar detailIn, const Scalar roughnessIn, const Scalar lacunarityIn,
			const Scalar offsetIn, const Scalar gainIn, const Scalar distortionIn, const bool normalize,
			Scalar& fac, Scalar* color )
		{
			fac = Scalar(0);
			if( color ) color[0] = color[1] = color[2] = Scalar(0);
			if( dims < 1 || dims > 4 ) return;

			// svm_node_tex_noise clamps: detail in [0, 15] (NaN -> 0), roughness >= 0.
			float detail = (float)detailIn;
			if( detail != detail || detail < 0.0f ) detail = 0.0f;
			if( detail > 15.0f ) detail = 15.0f;
			const float roughness = std::fmax( (float)roughnessIn, 0.0f );
			const float lacunarity = (float)lacunarityIn;
			const float offset = (float)offsetIn;
			const float gain = (float)gainIn;
			const float distortion = (float)distortionIn;

			Pt p = MakePt( dims, pIn );
			if( !AllFinite( p ) ) return;
			if( distortion != 0.0f ) {
				// Every warp component reads the UNWARPED point: Cycles
				// builds the whole offset vector before adding it.
				Pt warp;
				warp.dims = dims;
				for( int k = 0; k < 4; ++k ) warp.c[k] = 0.0f;
				for( int k = 0; k < dims; ++k ) {
					warp.c[k] = SN( Plus( p, RandomOffset( dims, (float)k ) ) ) * distortion;
				}
				p = Plus( p, warp );
			}

			const float f = Select( p, type, detail, roughness, lacunarity, offset, gain, normalize );
			fac = Scalar( f );
			if( color ) {
				// The Color output's seeds: 1D uses 1 and 2, 2D 2 and 3,
				// 3D 3 and 4, 4D 4 and 5 -- the first `dims` seeds having
				// gone to the distortion.
				color[0] = Scalar( f );
				color[1] = Scalar( Select( Plus( p, RandomOffset( dims, (float)dims ) ),
					type, detail, roughness, lacunarity, offset, gain, normalize ) );
				color[2] = Scalar( Select( Plus( p, RandomOffset( dims, (float)( dims + 1 ) ) ),
					type, detail, roughness, lacunarity, offset, gain, normalize ) );
			}
		}
	}
}
