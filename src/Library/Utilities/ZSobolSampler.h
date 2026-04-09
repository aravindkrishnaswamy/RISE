//////////////////////////////////////////////////////////////////////
//
//  ZSobolSampler.h - ISampler implementation for blue-noise
//    screen-space error distribution using Morton-indexed Sobol
//    sequences.
//
//    Extends SobolSampler by computing the global Sobol sample
//    index from the pixel's Morton code (Z-curve index) rather
//    than using a simple per-pixel counter.  This ensures that
//    spatially adjacent pixels (in Morton order) receive
//    consecutive sub-sequences of the (0,2)-net, producing
//    blue-noise error distribution across screen space.
//
//    At low sample counts (16-32 SPP), the error appears as
//    high-frequency (blue) noise rather than white noise, which
//    is both visually superior and produces better input for
//    denoising (OIDN).  At high sample counts, convergence is
//    equivalent to standard Owen-scrambled Sobol.
//
//    The caller must supply:
//      sampleIndex       - sample within this pixel [0, SPP)
//      mortonPixelIndex  - MortonCode::Morton2D(x, y)
//      log2SPP           - MortonCode::Log2Int(RoundUpPow2(SPP))
//      seed              - scramble seed (e.g., HashCombine(mortonIndex, 0))
//
//    The global Sobol index is computed as:
//      (mortonPixelIndex << log2SPP) | sampleIndex
//
//    This encodes the pixel's spatial position in the high bits
//    and the sample number in the low bits, so the (0,2)-net
//    stratification property guarantees cross-pixel sample
//    complementarity.
//
//  References:
//    - Ahmed and Wonka, "Screen-Space Blue-Noise Diffusion of
//      Monte Carlo Sampling Error via Hierarchical Ordering of
//      Pixels", SIGGRAPH Asia 2020
//    - Pharr, Jakob, Humphreys, "Physically Based Rendering" (4e),
//      Section 8.7: ZSobolSampler
//
//  Limits:
//    The global index must fit in uint32_t.  For a 2048x2048
//    image (mortonIndex up to ~2^22) with 1024 SPP (log2SPP=10),
//    the global index reaches 2^32 — at the limit.  If the
//    shifted index would overflow, ComputeIndex falls back to
//    returning sampleIndex directly (standard Sobol behavior),
//    so rendering degrades gracefully to white-noise error
//    distribution instead of silently aliasing pixels.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ZSOBOL_SAMPLER_H
#define ZSOBOL_SAMPLER_H

#include "SobolSampler.h"
#include "MortonCode.h"

namespace RISE
{
	namespace Implementation
	{
		class ZSobolSampler :
			public SobolSampler
		{
		public:
			virtual ~ZSobolSampler(){};

		public:
			ZSobolSampler(
				uint32_t sampleIndex_,
				uint32_t mortonPixelIndex_,
				uint32_t log2SPP_,
				uint32_t seed_
				) :
				SobolSampler(
					ComputeIndex( sampleIndex_, mortonPixelIndex_, log2SPP_ ),
					seed_
				)
			{
			}

		private:
			// Compute the Morton-remapped global Sobol index.
			// If the result would overflow uint32_t (large images or
			// high SPP), falls back to the plain sampleIndex so that
			// rendering degrades gracefully to white-noise error
			// distribution instead of silently aliasing pixels.
			static inline uint32_t ComputeIndex(
				uint32_t sampleIndex_,
				uint32_t mortonPixelIndex_,
				uint32_t log2SPP_
				)
			{
				if( log2SPP_ >= 32 ||
					(uint64_t(mortonPixelIndex_) << log2SPP_) >=
					(uint64_t(1) << 32) )
				{
					return sampleIndex_;
				}
				return (mortonPixelIndex_ << log2SPP_) | sampleIndex_;
			}
		};
	}
}

#endif
