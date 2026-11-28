//////////////////////////////////////////////////////////////////////
//
//  SobolSampler.h - ISampler backed by Owen-scrambled Sobol' sequences.
//
//  Caller supplies per-pixel sampleIndex (0, 1, ...) and scramble seed.
//  StartStream() partitions dimensions into fixed kStreamStride phases
//  so subpath generators stay stratified across pixels.
//
//  The direction table holds SobolSequence::kNumDimensions (8192 = 256
//  streams); draws past it re-index cleanly (see SobolSequence.h and
//  docs/DEBT_LEDGER.md).  Stream families that intentionally live past
//  the table to remain disjoint from per-vertex streams: PT volume walk
//  (4096..8191), BDPT/VCM medium distance (8192..139263), and deep walk
//  iterations (139264..141265).
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
#include <atomic>
#include <cstdlib>

namespace RISE
{
	namespace Implementation
	{
		// Test-only hooks (read once at SobolSampler construction; set only
		// between renders):
		//   ValueSalt   - XORed into the value-side Owen scramble seed for
		//                 independent randomized-QMC replicates.
		//   Independent - Replaces draws with stateful splitmix64 while
		//                 preserving stream/dimension bookkeeping.
		struct SobolSamplerTestHooks
		{
			static std::atomic<uint32_t>& ValueSalt()
			{
				static std::atomic<uint32_t> v( 0u );
				return v;
			}
			static std::atomic<bool>& Independent()
			{
				static std::atomic<bool> v( false );
				return v;
			}
		};

		class SobolSampler :
			public ISampler
		{
		protected:
			uint32_t sampleIndex;		// Which sample in the sequence
			uint32_t seed;				// Per-pixel base scramble seed
			unsigned int alphaDimension = 0;
			unsigned int dimension;		// Current dimension counter
			bool independent;			// SobolSamplerTestHooks::Independent at construction
			uint64_t rngState;			// splitmix64 state, independent mode only

			//! splitmix64 -> [0,1) with 53 bits (independent test mode).
			double NextIndependent()
			{
				uint64_t z = ( rngState += 0x9E3779B97F4A7C15ull );
				z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
				z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
				z = z ^ ( z >> 31 );
				return double( z >> 11 ) * ( 1.0 / 9007199254740992.0 );
			}

			// Fixed-size phase layout (cross-pixel stratification; full map in
			// SobolDimensionBudgetTest G2):
			//   0                  Light source sampling
			//   1 + d  (d < 15)    Light bounces (deep d >= 15: 139264 + (d - 15))
			//   16 + d (d < 31)    Eye bounces   (deep d >= 31: 140273 + (d - 31))
			//   47                 BDPT strategy select
			//   48 + i (i >= 1)    VCM NEE, eye vertex i
			//   141312 + 4*d + l   PT HWSS SMS lanes [141312, 145408) via ForkStream
		public:
			// Must be >= max dimensions per phase (~25 max for BioSpecSkinSPF + BSSRDF).
			// Public for static_assert in stream layouts (e.g. MediumDistanceStream).
			static const unsigned int kStreamStride = 32;

			virtual ~SobolSampler(){};

		public:
			SobolSampler(
				uint32_t sampleIndex_,
				uint32_t seed_
				) :
				sampleIndex( sampleIndex_ ),
				seed( seed_ ^ SobolSamplerTestHooks::ValueSalt().load( std::memory_order_relaxed ) ),
				dimension( 0 ),
				independent( SobolSamplerTestHooks::Independent().load( std::memory_order_relaxed ) ),
				rngState( ( uint64_t( seed ) << 32 ) ^ uint64_t( sampleIndex_ ) ^ 0xD1B54A32D192ED03ull )
			{
			}

			//! Returns a single Owen-scrambled Sobol sample in [0,1)
			Scalar Get1D()
			{
				if( independent ) { dimension++; return Scalar( NextIndependent() ); }
				return Scalar( SobolSequence::Sample( sampleIndex, dimension++, seed ) );
			}

            //! Reserved dimensions [2^29,2^30), reset only with a new sample.
            //! Variable-length alpha walks cannot overrun into a bounce stream.
            Scalar GetAlpha1D()
            {
                if (alphaDimension >= (1u << 29)) std::abort();
                if (independent) { ++alphaDimension; return Scalar(NextIndependent()); }
                return Scalar(SobolSequence::Sample(sampleIndex, (1u << 29) + alphaDimension++, seed));
            }

			//! Returns a 2D Owen-scrambled Sobol sample in [0,1)^2 as a padded
			//! (0,2)-net pair (Sobol' dims 0 & 1 at a group-permuted index; t = 0).
			Point2 Get2D()
			{
				double u = 0.0, v = 0.0;
				if( independent ) {
					u = NextIndependent();
					v = NextIndependent();
					dimension += 2;
					return Point2( Scalar( u ), Scalar( v ) );
				}
				SobolSequence::SamplePair( sampleIndex, dimension, seed, u, v );
				dimension += 2;
				return Point2( Scalar( u ), Scalar( v ) );
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

			//! Fork a sampler on the same (sampleIndex, seed) at `streamIndex`
			//! without moving this sampler's dimension (re-keying rngState in
			//! independent mode).  Call JoinAlpha() afterward if the fork draws alpha.
			SobolSampler ForkStream( int streamIndex ) const
			{
				SobolSampler fork( *this );
				fork.StartStream( streamIndex );
				fork.rngState ^= ( uint64_t( static_cast<uint32_t>( streamIndex ) ) + 1ull ) * 0x9E3779B97F4A7C15ull;
				return fork;
			}

			//! Advance parent's alphaDimension past draws made by `fork`.
			//! Precondition: parent draws no alpha between ForkStream and JoinAlpha.
			void JoinAlpha( const SobolSampler& fork )
			{
				if( fork.alphaDimension > alphaDimension ) {
					alphaDimension = fork.alphaDimension;
				}
			}
		};
	}
}

#endif
