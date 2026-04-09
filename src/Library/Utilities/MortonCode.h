//////////////////////////////////////////////////////////////////////
//
//  MortonCode.h - 2D Morton code (Z-curve) utilities for
//    blue-noise screen-space error distribution.
//
//    Morton codes bit-interleave two coordinates into a single
//    index that preserves 2D spatial locality.  When used to
//    assign Sobol sample indices to pixels, Morton ordering
//    ensures that spatially adjacent pixels receive consecutive
//    sub-sequences of the (0,2)-net, producing blue-noise error
//    distribution across screen space.
//
//    All functions are static and stateless, matching the
//    SobolSequence utility style.
//
//  References:
//    - Ahmed and Wonka, "Screen-Space Blue-Noise Diffusion of
//      Monte Carlo Sampling Error via Hierarchical Ordering of
//      Pixels", SIGGRAPH Asia 2020
//    - Pharr, Jakob, Humphreys, "Physically Based Rendering" (4e),
//      Section 8.7: ZSobolSampler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MORTON_CODE_H
#define MORTON_CODE_H

#include <stdint.h>
#include <cassert>

namespace RISE
{
	//
	// 2D Morton code (Z-curve) utilities.
	//
	// All functions are static and stateless.
	//
	class MortonCode
	{
	public:

		//////////////////////////////////////////////////////////////
		// Part1By1 - spread the low 16 bits of x into even bit
		// positions of the result.
		//
		//   input:  ---- ---- ---- ---- fedc ba98 7654 3210
		//   output: -f-e -d-c -b-a -9-8 -7-6 -5-4 -3-2 -1-0
		//
		// Standard 5-step shift-and-mask approach.
		//////////////////////////////////////////////////////////////
		static inline uint32_t Part1By1( uint32_t x )
		{
			x &= 0x0000ffffu;
			x = (x ^ (x << 8)) & 0x00ff00ffu;
			x = (x ^ (x << 4)) & 0x0f0f0f0fu;
			x = (x ^ (x << 2)) & 0x33333333u;
			x = (x ^ (x << 1)) & 0x55555555u;
			return x;
		}

		//////////////////////////////////////////////////////////////
		// Compact1By1 - inverse of Part1By1.  Extract every other
		// bit and compact them into the low 16 bits.
		//
		//   input:  -f-e -d-c -b-a -9-8 -7-6 -5-4 -3-2 -1-0
		//   output: ---- ---- ---- ---- fedc ba98 7654 3210
		//////////////////////////////////////////////////////////////
		static inline uint32_t Compact1By1( uint32_t x )
		{
			x &= 0x55555555u;
			x = (x ^ (x >> 1)) & 0x33333333u;
			x = (x ^ (x >> 2)) & 0x0f0f0f0fu;
			x = (x ^ (x >> 4)) & 0x00ff00ffu;
			x = (x ^ (x >> 8)) & 0x0000ffffu;
			return x;
		}

		//////////////////////////////////////////////////////////////
		// CanEncode2D - returns true if both coordinates fit in 16
		// bits, which is the requirement for Morton2D to produce a
		// unique code.  Coordinates >= 65536 would be silently
		// truncated by Part1By1, aliasing distinct pixels.
		//////////////////////////////////////////////////////////////
		static inline bool CanEncode2D( uint32_t x, uint32_t y )
		{
			return (x <= 0xffffu) && (y <= 0xffffu);
		}

		//////////////////////////////////////////////////////////////
		// Morton2D - encode (x, y) into a 2D Morton code.
		//
		// Bit-interleaves x and y: x occupies even bit positions,
		// y occupies odd bit positions.
		//
		// Supports coordinates up to 65535 (16 bits each), producing
		// a 32-bit Morton code.  Callers should check CanEncode2D()
		// first; coordinates >= 65536 are silently masked.
		//////////////////////////////////////////////////////////////
		static inline uint32_t Morton2D( uint32_t x, uint32_t y )
		{
			return Part1By1( x ) | (Part1By1( y ) << 1);
		}

		//////////////////////////////////////////////////////////////
		// InverseMorton2D - decode a 2D Morton code back to (x, y).
		//
		// Extracts x from even bits, y from odd bits.
		//////////////////////////////////////////////////////////////
		static inline void InverseMorton2D(
			uint32_t code,
			uint32_t& x,
			uint32_t& y
			)
		{
			x = Compact1By1( code );
			y = Compact1By1( code >> 1 );
		}

		//////////////////////////////////////////////////////////////
		// RoundUpPow2 - round up to the next power of 2.
		//
		// Returns v if v is already a power of 2.
		// Returns 1 for v == 0.
		//////////////////////////////////////////////////////////////
		static inline uint32_t RoundUpPow2( uint32_t v )
		{
			if( v == 0 ) return 1;
			v--;
			v |= v >> 1;
			v |= v >> 2;
			v |= v >> 4;
			v |= v >> 8;
			v |= v >> 16;
			return v + 1;
		}

		//////////////////////////////////////////////////////////////
		// Log2Int - floor of log base 2.
		//
		// Returns 0 for v <= 1.
		// Uses __builtin_clz when available (GCC/Clang), otherwise
		// a portable bit-scan fallback.
		//////////////////////////////////////////////////////////////
		static inline uint32_t Log2Int( uint32_t v )
		{
			if( v <= 1 ) return 0;
#if defined(__GNUC__) || defined(__clang__)
			return 31u - static_cast<uint32_t>(__builtin_clz( v ));
#else
			uint32_t r = 0;
			while( v >>= 1 ) r++;
			return r;
#endif
		}
	};
}

#endif
