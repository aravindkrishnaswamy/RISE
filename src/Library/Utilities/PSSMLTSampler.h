//////////////////////////////////////////////////////////////////////
//
//  PSSMLTSampler.h - Primary Sample Space Metropolis Light Transport
//    sampler (Kelemen et al. 2002; Veach 1997).
//
//  Implements ISampler by maintaining a lazily-grown primary sample
//  vector in [0,1) with large-step (fresh uniform) and small-step
//  (symmetric Kelemen log-uniform) mutations and Accept()/Reject()
//  rollback for Metropolis-Hastings path-space exploration.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PSSMLT_SAMPLER_
#define PSSMLT_SAMPLER_

#include "ISampler.h"
#include "Reference.h"
#include "RandomNumbers.h"
#include <vector>
#include <utility>
#include <cmath>
#include <atomic>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		// Test-only seed salt mixed into every PSSMLT RNG seed for independent
		// MLT replicates.  Zero (default) preserves bit-identical determinism;
		// set only between renders.
		struct PSSMLTSamplerTestHooks
		{
			static std::atomic<uint32_t>& SeedSalt()
			{
				static std::atomic<uint32_t> v( 0u );
				return v;
			}
			static unsigned int Salted( const unsigned int seed )
			{
				const uint32_t salt = SeedSalt().load( std::memory_order_relaxed );
				return salt ? ( seed ^ ( salt * 0x9E3779B9u + 0x7F4A7C15u ) ) : seed;
			}
		};

		/// PSSMLTSampler records the random number stream consumed by BDPT
		/// and mutates it for Metropolis-Hastings exploration of path space.
		///
		/// INVARIANTS:
		/// - Between StartIteration() and Accept()/Reject(), the sampler is
		///   in "proposal" state: Get1D()/Get2D() return mutated values and
		///   backup the originals.
		/// - After Accept(), proposed values become the new current state.
		/// - After Reject(), all modified samples revert to their pre-mutation
		///   values.
		/// - sampleIndex resets to 0 at each StartIteration(), ensuring the
		///   same consumption order regardless of path outcomes.
		class PSSMLTSampler :
			public virtual ISampler,
			public virtual Reference
		{
		protected:

			/// Single primary sample slot with rollback backup and 64-bit
			/// iteration stamps (matching MLTMutationSchedule::perChain so
			/// lazy catch-up never wraps).
			struct PrimarySample
			{
				Scalar		value;				///< Current accepted value in [0,1)
				Scalar		backup;				///< Saved value for rejection rollback
				std::uint64_t lastModIteration;	///< Iteration when last mutated
				std::uint64_t backupIteration;	///< Iteration of the backup

				PrimarySample() :
				value( 0 ),
				backup( 0 ),
				lastModIteration( 0 ),
				backupIteration( 0 )
				{
				}
			};

			// Two-tier stream storage (docs/DEBT_LEDGER.md):
			//  - X: flat vector with fixed row width kLegacyNumStreams (49) for
			//    streams 0..48 (idx = streamIndex + 49 * sampleIndex), keeping
			//    shallow BDPT walks bit-identical without multiplicative blow-up.
			//  - XExtra: linear (stream, vector) list for streamIndex >= 49
			//    (MLT film/lens 2048, alpha 3072, deep walk lanes 2049..4050).
			//    Collision-free with O(drawn) memory; linear scan beats hash maps
			//    for the tiny per-instance stream count (revisit if >~32 streams).
			//  - modifiedIndices tags (tier, location) for O(k) Reject() rollback.
			std::vector<PrimarySample>		X;					///< Legacy flat vector, streamIndex < kLegacyNumStreams only
			std::vector<std::pair<int, std::vector<PrimarySample> > > XExtra; ///< (stream, vector) list for streamIndex >= kLegacyNumStreams, searched linearly

			/// Finds or creates the per-stream vector in `XExtra` (linear scan).
			std::vector<PrimarySample>& FindOrCreateExtraStream( int stream )
			{
				for( size_t i = 0; i < XExtra.size(); i++ )
				{
					if( XExtra[i].first == stream ) {
						return XExtra[i].second;
					}
				}
				XExtra.push_back( std::make_pair( stream, std::vector<PrimarySample>() ) );
				return XExtra.back().second;
			}

			/// Identifies one touched (stream, sampleIndex) lane so
			/// Accept()/Reject() can find its PrimarySample back
			/// without re-deriving which storage tier owns it.
			struct ModifiedLane
			{
				bool			legacy;			///< true: lane lives in X; false: lives in XExtra
				unsigned int	legacyIdx;		///< valid iff legacy: flat index into X
				int				stream;			///< valid iff !legacy: key into XExtra
				unsigned int	sampleIdx;		///< valid iff !legacy: index into XExtra's vector for `stream`
			};
			std::vector<ModifiedLane>		modifiedIndices;	///< Lanes modified in current proposal (for fast rollback)
			unsigned int alphaIndex = 0;
			unsigned int					sampleIndex;		///< Current consumption position within current stream
			std::uint64_t					currentIteration;	///< Global mutation counter (64-bit: see PrimarySample)

			// Fixed row width of flat vector `X` (streams 0..48).
			static const int				kLegacyNumStreams = 49;

			// Per-instance runtime sanity bound on streamIndex (not a collision
			// bound — XExtra keys by literal stream ID).  Subclasses may raise
			// this via the protected constructor.
			static const int				kDefaultNumStreams = 4096;
			int								kNumStreams;		///< Sanity bound on streamIndex (per-instance)
			int								streamIndex;		///< Current active stream

			// Mutation parameters
			Scalar							largeStepProb;		///< Probability of a large (independent) step
			bool							isLargeStep;		///< Whether the current proposal is a large step
			std::uint64_t					lastLargeStepIteration;	///< Last iteration that was a large step

			// Exponential perturbation range (Kelemen's log-uniform trick).
			// s1 and s2 define the min/max perturbation magnitudes.
			// Typical values: s1=1/1024, s2=1/64.
			static const Scalar				s1;					///< Minimum perturbation magnitude
			static const Scalar				s2;					///< Maximum perturbation magnitude
			static const Scalar				logRatio;			///< Precomputed -log(s2/s1)

			RandomNumberGenerator			rng;				///< Source of fresh randomness

			virtual ~PSSMLTSampler();

			/// Apply a small exponential perturbation to a value in [0,1).
			/// Uses the Kelemen log-uniform distribution:
			///   delta = s2 * exp(logRatio * u),  u ~ Uniform[0,1)
			///   result = (value +/- delta) mod 1.0
			/// This concentrates most mutations near the current value
			/// while still allowing occasional larger jumps within the
			/// perturbation range [s1, s2].
			Scalar Mutate( const Scalar value );

		protected:
			/// Subclass-only constructor that allows reserving additional
			/// sample streams beyond the 49 PSSMLT needs.  MMLTSampler
			/// uses this to allocate streams 49 ((s,t) selection) and
			/// 50 (lens position).  Pass numStreams_ = 49 to behave
			/// identically to the public PSSMLTSampler constructor.
			PSSMLTSampler(
				const unsigned int seed,
				const Scalar largeStepProb_,
				const int numStreams_
				);

		public:
			/// Construct a PSSMLTSampler.
			/// \param seed          RNG seed for this sampler instance
			/// \param largeStepProb_ Probability of taking a large (independent) step.
			///                       Default 0.3 balances exploration vs exploitation.
			PSSMLTSampler(
				const unsigned int seed,
				const Scalar largeStepProb_ = 0.3
				);

			//////////////////////////////////////////////////////////////
			// ISampler interface
			//////////////////////////////////////////////////////////////

			/// Returns a single uniform random sample in [0,1).
			/// If this is a large step, returns a fresh random.
			/// If this is a small step, returns a mutated version of
			/// the stored value at the current index.
			/// Lazily grows the sample vector if needed.
			Scalar Get1D();

			/// Returns a 2D uniform random sample in [0,1)^2.
			Point2 Get2D();

			/// Switch to a new sample stream and reset sampleIndex.
			/// BDPT uses 0..47; MLT film/lens uses 2048; alpha uses 3072.
            Scalar GetAlpha1D();

			void StartStream( int streamIndex );

			//////////////////////////////////////////////////////////////
			// MLT-specific interface
			//////////////////////////////////////////////////////////////

			/// Begin a new mutation proposal.  Decides whether this
			/// iteration will be a large step (fresh random) or small
			/// step (perturbation), resets sampleIndex to 0.
			void StartIteration();

			/// Accept the current proposal: discard backups, advance
			/// the iteration counter.  Called when the Metropolis
			/// acceptance test passes.
			void Accept();

			/// Reject the current proposal: restore all modified samples
			/// from their backups.  Called when the acceptance test fails.
			void Reject();

			/// \return The large step probability for this sampler.
			Scalar GetLargeStepProb() const { return largeStepProb; }

			/// \return The current iteration number.
			std::uint64_t GetCurrentIteration() const { return currentIteration; }

			/// Re-seed mutation RNG without disturbing X or iteration counters.
			/// Called by MLTRasterizer::InitChain after the first Accept() so
			/// chains diverge from the same bootstrap path.
			void ReSeedRNG( const unsigned int newSeed )
			{
				rng = RandomNumberGenerator( PSSMLTSamplerTestHooks::Salted( newSeed ) );
			}
		};
	}
}

#endif
