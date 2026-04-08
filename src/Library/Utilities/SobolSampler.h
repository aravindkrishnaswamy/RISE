//////////////////////////////////////////////////////////////////////
//
//  SobolSampler.h - ISampler implementation backed by Owen-scrambled
//    padded Sobol (0,2)-sequences.
//
//    Replaces IndependentSampler in BDPT (and any future integrator
//    that consumes ISampler).  Each call to Get1D() / Get2D() draws
//    the next dimension(s) from the Sobol sequence for the current
//    sample index, with Owen scrambling seeded by pixel coordinates.
//
//    The caller must supply:
//      sampleIndex - which sample within this pixel (0, 1, 2, ...)
//      seed        - a per-pixel base seed (e.g., hash of pixel coords)
//
//    StartStream() advances the dimension counter so that different
//    sub-path generators (light, eye, connection) draw from
//    independent dimension ranges — same mechanism as PSSMLTSampler.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 27, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SOBOL_SAMPLER_H
#define SOBOL_SAMPLER_H

#include "ISampler.h"
#include "../Sampling/SobolSequence.h"

namespace RISE
{
	namespace Implementation
	{
		class SobolSampler :
			public ISampler
		{
		protected:
			uint32_t sampleIndex;		// Which sample in the sequence
			uint32_t seed;				// Per-pixel base scramble seed
			unsigned int dimension;		// Current dimension counter

			// Dimensions are partitioned into fixed-size phases so
			// that each bounce always starts at the same dimension
			// offset regardless of how many dimensions previous
			// bounces consumed (material-dependent).  This preserves
			// cross-pixel Sobol stratification.
			//
			// Encoding: phase = streamBase + bounceIndex
			//   Light source sampling: phase 0
			//   Light bounces 0..14:   phases 1..15
			//   Eye bounces 0..14:     phases 16..30
			//   SMS:                   phases 31..46
			//
			// kStreamStride must be >= max dimensions consumed by
			// any single phase (BioSpecSkinSPF uses ~20 + lobe
			// selection + BSSRDF = ~25 max).
			static const unsigned int kStreamStride = 32;

		public:
			virtual ~SobolSampler(){};

		public:
			SobolSampler(
				uint32_t sampleIndex_,
				uint32_t seed_
				) :
				sampleIndex( sampleIndex_ ),
				seed( seed_ ),
				dimension( 0 )
			{
			}

			//! Returns a single Owen-scrambled Sobol sample in [0,1)
			Scalar Get1D()
			{
				return Scalar( SobolSequence::Sample( sampleIndex, dimension++, seed ) );
			}

			//! Returns a 2D Owen-scrambled Sobol sample in [0,1)^2
			Point2 Get2D()
			{
				Scalar u = Scalar( SobolSequence::Sample( sampleIndex, dimension, seed ) );
				Scalar v = Scalar( SobolSequence::Sample( sampleIndex, dimension + 1, seed ) );
				dimension += 2;
				return Point2( u, v );
			}

			//! SobolSampler uses fixed-size phases (kStreamStride dimensions
			//! each).  Variable-length algorithms must use IndependentSampler.
			bool HasFixedDimensionBudget() const { return true; }

			//! Advance to stream's dimension range.
			//! Stream 0 = film/camera, 1 = light subpath, 2 = eye subpath, etc.
			void StartStream( int streamIndex )
			{
				dimension = static_cast<unsigned int>(streamIndex) * kStreamStride;
			}
		};
	}
}

#endif
