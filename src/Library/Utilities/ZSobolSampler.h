//////////////////////////////////////////////////////////////////////
//
//  ZSobolSampler.h - Morton-indexed Sobol' sampler for blue-noise
//    screen-space error distribution (Ahmed & Wonka 2020; PBRT 4e §8.7).
//
//  Computes global Sobol index `(mortonPixelIndex << log2SPP) | sampleIndex`
//  so adjacent pixels receive complementary (0,2)-net sub-sequences.
//  Caller supplies sampleIndex in [0, SPP), mortonPixelIndex = Morton2D(x,y),
//  log2SPP = Log2Int(RoundUpPow2(SPP)), and scramble seed.
//  Falls back to plain sampleIndex if the shifted index overflows uint32_t.
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
			// Morton-remapped global Sobol index; falls back to sampleIndex
			// on uint32_t overflow so pixels degrade to white noise rather than alias.
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
