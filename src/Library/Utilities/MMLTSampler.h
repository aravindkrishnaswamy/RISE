//////////////////////////////////////////////////////////////////////
//
//  MMLTSampler.h - Per-depth sampler for Multiplexed Metropolis Light
//    Transport (MMLT, Hachisuka et al. 2014).
//
//  CONTEXT:
//    MMLT addresses the dim-strategy starvation that limits PSSMLT
//    convergence on SDS scenes.  Where PSSMLT runs ONE chain whose
//    density adapts to the SUMMED luminance over all (s,t) strategies
//    (so dim strategies like diffuse-wall paths get visited rarely
//    even when their per-strategy luminance is non-trivial), MMLT
//    runs SEPARATE chain pools per path length d = s + t - 2.  Each
//    chain in pool d only ever evaluates strategies at that d, and
//    its density adapts to the per-DEPTH luminance b_d, so depth-7
//    indirect paths get exactly the budget they earn from their
//    fraction of the total path-space integral.
//
//  WHY A NEW SAMPLER (not just "use PSSMLTSampler"):
//    MMLT chains need TWO additional primary-sample-space dimensions
//    that PSSMLT does not allocate:
//      Stream 49: discrete (s,t) selection inside the depth.  A
//                 continuous primary sample mapped to the integer
//                 strategy index, independent of film position so
//                 small-step film mutations do NOT also kick the
//                 strategy choice random.
//      Stream 50: lens position.  PSSMLT folded the lens sample into
//                 the film stream (kStreamFilm = 48) since it had no
//                 reason to keep them apart; MMLT splits them out so
//                 the discrete strategy and the continuous lens move
//                 evolve as independent dimensions of the chain.
//    A 51-stream sampler is wider than PSSMLT's 49-stream layout, so
//    we cannot just bump kNumStreams in PSSMLTSampler — that would
//    re-shuffle PSSMLT's primary-sample-vector indices and break
//    every existing PSSMLT render's bit-stable output.  Instead
//    PSSMLTSampler exposes a protected constructor that takes
//    kNumStreams as a parameter; MMLTSampler subclasses it and asks
//    for 51 streams while leaving the public PSSMLTSampler ctor
//    (still 49 streams) untouched.
//
//  DEPTH SEMANTICS:
//    depth = s + t - 2  where s,t are subpath VERTEX counts.
//    depth = 0  → s+t = 2, the trivial light-directly-at-camera path.
//                 Strategies (0,2) and (1,1) both valid (subject to
//                 maxLight/maxEye caps).
//    depth = 1  → s+t = 3, one bounce.  Up to 3 strategies.
//    ...
//    depth = D  → s+t = D+2.  Up to D+2 strategies (subject to caps).
//    The chain is BOUND to its depth at construction; mutations only
//    re-pick (s,t) within the valid set for that d.
//
//  REFERENCES:
//    Hachisuka, Kaplanyan, Dachsbacher.  "Multiplexed Metropolis
//    Light Transport."  ACM Trans. Graph. 33(4), 2014.
//    PBRT v3 Section 16.4.5 — the canonical implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 18, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MMLT_SAMPLER_
#define MMLT_SAMPLER_

#include "PSSMLTSampler.h"

namespace RISE
{
	namespace Implementation
	{
		/// MMLTSampler extends PSSMLTSampler with:
		///   - Two extra reserved streams (kStreamST, kStreamLens)
		///   - A bound depth `d = s+t-2` for the chain
		///   - PickStrategyST helper that draws a uniform (s,t) from
		///     the valid set for the bound depth
		class MMLTSampler :
			public PSSMLTSampler
		{
		public:
			// Stream layout (must agree with consumers):
			//   0..47  : BDPT subpath generation streams (light source,
			//            light bounces, eye bounces, SMS) — same as
			//            PSSMLT/BDPTIntegrator.
			//   48     : film position (continuous, 2D).
			//   49     : (s,t) strategy selection (continuous u in [0,1)
			//            mapped to a discrete index).
			//   50     : lens position (continuous, 2D, used only for
			//            ThinLensCamera; ignored otherwise).
			static const int kStreamFilm = 48;
			static const int kStreamST   = 49;
			static const int kStreamLens = 50;
			static const int kMMLTNumStreams = 51;

			/// Construct an MMLT sampler bound to a specific depth.
			/// `seed` and `largeStepProb_` follow the same semantics as
			/// the PSSMLT base.  `depth_` is `s + t - 2` for the path
			/// length this chain explores; the rasterizer pools chains
			/// by depth.
			MMLTSampler(
				const unsigned int seed,
				const Scalar largeStepProb_,
				const unsigned int depth_
				);

			/// The path-length depth this chain is bound to.  Read-only
			/// after construction.
			unsigned int GetDepth() const { return depth; }

			/// Count valid (s,t) strategies at path-length depth `d`,
			/// honouring maxLight/maxEye caps.  Pure function — exposed
			/// so the rasterizer can distribute chain budgets across
			/// depths and skip depths with zero strategies.  `outSlo`
			/// and `outShi` receive the inclusive range of valid s.
			static unsigned int CountStrategiesForDepth(
				const unsigned int d,
				const unsigned int maxLight,
				const unsigned int maxEye,
				unsigned int& outSlo,
				unsigned int& outShi
				);

			/// Draw a uniform (s,t) for THIS sampler's bound depth from
			/// the strategy-selection stream.  Returns false if the
			/// depth has zero valid strategies under the given caps
			/// (the caller should skip that mutation entirely).  On
			/// success, sets:
			///   outS, outT          : the chosen (s,t) pair
			///   outNStrategies      : count of valid strategies for
			///                         this depth — the CALLER must
			///                         multiply the contribution by
			///                         this value to undo the
			///                         1/nStrategies selection PDF.
			///                         (Standard MMLT step from PBRT
			///                         v3 MLT Integrator::L.)
			///
			/// Side effect: advances the kStreamST stream by one sample
			/// and leaves the sampler with kStreamST as the active
			/// stream — callers that need to draw from another stream
			/// afterwards must call StartStream() again.
			bool PickStrategyST(
				const unsigned int maxLight,
				const unsigned int maxEye,
				unsigned int& outS,
				unsigned int& outT,
				unsigned int& outNStrategies
				);

		protected:
			virtual ~MMLTSampler();

		private:
			unsigned int depth;
		};
	}
}

#endif
