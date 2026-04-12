//////////////////////////////////////////////////////////////////////
//
//  PSSMLTSampler.h - Primary Sample Space Metropolis Light Transport
//    sampler.  Implements the ISampler interface by recording,
//    replaying, and mutating the random number stream consumed by
//    BDPT.  This is the heart of PSSMLT (Kelemen et al. 2002).
//
//    CONTEXT AND APPROACH:
//    Every BDPT path is fully determined by the sequence of random
//    numbers consumed during subpath generation and connection.
//    Instead of drawing fresh i.i.d. samples each time (as
//    IndependentSampler does), PSSMLTSampler maintains a "primary
//    sample vector" X = {x_0, x_1, ...} in [0,1) and produces
//    new candidate paths by mutating this vector:
//
//      - Large step: replace all consumed samples with fresh
//        uniform randoms.  This ensures ergodicity -- the chain
//        can jump to any path in the space.
//      - Small step: perturb each consumed sample by a small
//        amount using an exponential distribution (Kelemen's
//        log-uniform trick), wrapped to [0,1).  This explores
//        the neighborhood of the current path for correlated
//        improvements.
//
//    After BDPT evaluates the candidate path, the MLTRasterizer
//    calls Accept() or Reject().  On rejection, all proposed
//    mutations are reverted from backups.
//
//    The vector grows lazily: if BDPT requests sample index i
//    and i >= X.size(), a fresh uniform value is appended.  This
//    means PSSMLTSampler automatically adapts to paths of any
//    length without pre-allocation.
//
//    REFERENCES:
//    - Kelemen, C., Szirmay-Kalos, L., Antal, G., Csonka, F.
//      "A Simple and Robust Mutation Strategy for the Metropolis
//      Light Transport Algorithm." Computer Graphics Forum, 2002.
//    - Veach, E. "Robust Monte Carlo Methods for Light Transport
//      Simulation." PhD Thesis, Stanford University, 1997.
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
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
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

			/// A single element of the primary sample vector.
			/// Tracks the current accepted value, a backup for rejection,
			/// and the iteration at which it was last modified (for lazy
			/// catch-up of mutations across iterations).
			struct PrimarySample
			{
				Scalar		value;				///< Current accepted value in [0,1)
				Scalar		backup;				///< Saved value for rejection rollback
				unsigned int lastModIteration;	///< Iteration when last mutated
				unsigned int backupIteration;	///< Iteration of the backup

				PrimarySample() :
				value( 0 ),
				backup( 0 ),
				lastModIteration( 0 ),
				backupIteration( 0 )
				{
				}
			};

			std::vector<PrimarySample>		X;					///< The primary sample vector
			std::vector<unsigned int>		modifiedIndices;	///< Indices modified in current proposal (for fast rollback)
			unsigned int					sampleIndex;		///< Current consumption position within current stream
			unsigned int					currentIteration;	///< Global mutation counter

			// Stream multiplexing: each stream gets its own lane in the
			// primary sample vector so that mutations to one part of the
			// path don't disturb others.  BDPTIntegrator uses streams
			// 0-47 internally (light source=0, light bounces=1-16,
			// eye bounces=16-31, SMS=47).  Stream 48 is reserved for
			// the MLT film position.  kNumStreams must exceed the
			// maximum stream index used by any consumer.
			static const int				kNumStreams = 49;	///< Number of sample streams
			int								streamIndex;		///< Current active stream

			// Mutation parameters
			Scalar							largeStepProb;		///< Probability of a large (independent) step
			bool							isLargeStep;		///< Whether the current proposal is a large step
			unsigned int					lastLargeStepIteration;	///< Last iteration that was a large step

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

			/// Switch to a new sample stream and reset the within-stream
			/// sample index.  Streams 0-47 are used by BDPTIntegrator;
			/// stream 48 is reserved for the MLT film position.
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
			unsigned int GetCurrentIteration() const { return currentIteration; }
		};
	}
}

#endif
