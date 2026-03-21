//////////////////////////////////////////////////////////////////////
//
//  MLTRasterizer.h - Metropolis Light Transport rasterizer using
//    Primary Sample Space MLT (PSSMLT, Kelemen et al. 2002).
//
//    CONTEXT:
//    While BDPT efficiently handles most light transport scenarios,
//    scenes with extremely sparse important paths (light through a
//    keyhole, caustics through chains of glass, SDS paths) converge
//    very slowly because even BDPT discovers these paths rarely.
//    MLT uses Metropolis-Hastings sampling to exploit discovered
//    high-contribution paths by exploring their neighborhoods
//    through small mutations.
//
//    ARCHITECTURE:
//    MLTRasterizer is a standalone rasterizer that extends Rasterizer
//    directly (NOT PixelBasedRasterizerHelper) because its rendering
//    loop is fundamentally different from pixel-based rasterizers:
//
//    1. BOOTSTRAP: Generate many independent BDPT samples, compute
//       the normalization constant b (expected image luminance),
//       and build a CDF for selecting initial Markov chain states.
//
//    2. MARKOV CHAINS (round-based): Run multiple independent chains
//       in parallel, dividing the total mutation budget into rounds.
//       Between rounds, the accumulated film is resolved and output
//       as a progressive image, allowing the user to see the render
//       converge and to cancel early with a valid (noisy) result.
//       Chain state (PSSMLTSampler + current sample) persists across
//       rounds so the Markov chains are never interrupted — only the
//       observation points are inserted.
//
//    3. SPLATTING: Both the current and proposed paths contribute
//       to the film with complementary weights (Veach's expected-
//       value technique), ensuring unbiased results even when
//       samples are rejected.
//
//    4. RESOLVE: At each round boundary and at the end, the
//       accumulated splat film is normalized and written to output.
//
//    KEY DESIGN: BDPTIntegrator is used as a pure black box.  The
//    only difference from standard BDPT is that the ISampler passed
//    to it is a PSSMLTSampler instead of an IndependentSampler.
//    No modifications to existing BDPT code are required.
//
//    PROGRESSIVE RENDERING:
//    The number of progressive rounds is automatically determined
//    by timing the bootstrap phase: each bootstrap sample evaluates
//    the same BDPT path as a mutation, so the bootstrap time lets
//    us estimate total render time and choose a round count that
//    targets ~5 seconds per round.  After each round completes:
//      - The film is resolved with sampleCount = (r+1)/numRounds
//        to compensate for incomplete accumulation, giving a
//        correctly-exposed (but noisy) image at every stage.
//      - The image is output via OutputIntermediateImage for
//        preview, and OutputImage only on final round or cancel.
//      - Progress is reported as round/numRounds.
//    If the user cancels at any point, the last output image is
//    a valid render at that quality level.
//
//    REFERENCES:
//    - Kelemen et al. "A Simple and Robust Mutation Strategy for
//      the Metropolis Light Transport Algorithm." CGF 2002.
//    - Veach, E. "Robust Monte Carlo Methods for Light Transport
//      Simulation." PhD Thesis, Stanford, 1997. Chapter 11.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MLT_RASTERIZER_
#define MLT_RASTERIZER_

#include "Rasterizer.h"
#include "SplatFilm.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Interfaces/IRayCaster.h"
#include "../Utilities/PSSMLTSampler.h"

namespace RISE
{
	namespace Implementation
	{
		class MLTRasterizer : public virtual Rasterizer
		{
		protected:

			IRayCaster*				pCaster;
			BDPTIntegrator*			pIntegrator;

			// MLT-specific parameters
			unsigned int			nBootstrap;			///< Number of bootstrap samples (default 100000)
			unsigned int			nChains;			///< Number of independent Markov chains (default 512)
			unsigned int			nMutationsPerPixel;	///< Mutations per pixel budget (default 100)
			Scalar					largeStepProb;		///< Probability of large step mutation (default 0.3)
			/// Result of evaluating a single BDPT sample through the MLT pipeline.
			/// Contains the path's contribution, its film position, and scalar
			/// luminance for Metropolis acceptance computation.
			struct MLTSample
			{
				RISEPel		color;			///< RGB path contribution
				Point2		rasterPos;		///< Pixel position for splatting
				Scalar		luminance;		///< Scalar luminance for acceptance ratio
				bool		valid;			///< False if the path produced zero contribution

				MLTSample() :
				color( RISEPel( 0, 0, 0 ) ),
				rasterPos( Point2( 0, 0 ) ),
				luminance( 0 ),
				valid( false )
				{
				}
			};

			/// Bootstrap sample record: stores the luminance and RNG seed
			/// needed to reproduce a high-contribution path for Markov
			/// chain initialization.
			struct BootstrapSample
			{
				Scalar			luminance;		///< Scalar contribution of this sample
				unsigned int	seed;			///< RNG seed that produced this sample

				BootstrapSample() :
				luminance( 0 ),
				seed( 0 )
				{
				}
			};

			/// Persistent state for a single Markov chain that survives
			/// across progressive rendering rounds.  This is the key to
			/// round-based progressive rendering: the chain can be paused
			/// (for a film snapshot) and resumed without affecting the
			/// Markov chain's statistical properties.
			struct ChainState
			{
				PSSMLTSampler*			pSampler;		///< PSSMLT sampler holding the primary sample vector
				MLTSample				currentSample;	///< Current accepted path (for acceptance ratio)
				RandomNumberGenerator	chainRNG;		///< RNG for accept/reject decisions

				ChainState() :
				pSampler( 0 ),
				chainRNG( 0 )
				{
				}
			};

			/// Thread dispatch data for parallel round-based chain execution.
			/// Each thread receives a contiguous range of chain states and
			/// runs a fixed number of mutations per chain for one round.
			struct RoundThreadData
			{
				const MLTRasterizer*	pRasterizer;
				const IScene*			pScene;
				SplatFilm*				pSplatFilm;
				ChainState*				pChainStates;		///< Array of all chain states
				unsigned int			chainStart;			///< First chain index for this thread
				unsigned int			chainEnd;			///< One-past-last chain index for this thread
				unsigned int			mutationsPerChain;	///< Mutations to run this round
				Scalar					normalization;		///< Normalization constant b
				unsigned int			width;
				unsigned int			height;
			};

			virtual ~MLTRasterizer();

			/// Evaluate a single BDPT path using the given sampler.
			/// This is the bridge between MLT and BDPT: it uses the
			/// sampler to pick a film position, generates light and eye
			/// subpaths, evaluates all (s,t) connection strategies, and
			/// returns the aggregated result.
			MLTSample EvaluateSample(
				const IScene& scene,
				const ICamera& camera,
				ISampler& sampler,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Initialize a Markov chain: create the PSSMLTSampler from
			/// a bootstrap seed and evaluate the initial path.  If the
			/// initial path has zero luminance, try additional large steps
			/// to find a valid starting point.
			void InitChain(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				const BootstrapSample& seed,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Run a segment of mutations on an existing chain.  This is
			/// the inner Metropolis-Hastings loop, identical to the old
			/// RunChain but operating on externally-owned state so the
			/// chain can be paused and resumed across rounds.
			void RunChainSegment(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				SplatFilm& splatFilm,
				const unsigned int numMutations,
				const Scalar normalization,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Select an initial chain state from the bootstrap CDF using
			/// luminance-weighted importance sampling.
			unsigned int SelectFromCDF(
				const std::vector<Scalar>& cdf,
				const Scalar u
				) const;

		public:
			MLTRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const unsigned int nBootstrap_,
				const unsigned int nChains_,
				const unsigned int nMutationsPerPixel_,
				const Scalar largeStepProb_
				);

			//////////////////////////////////////////////////////////////
			// IRasterizer interface
			//////////////////////////////////////////////////////////////

			void AttachToScene( const IScene* ){};
			void DetachFromScene( const IScene* ){};

			unsigned int PredictTimeToRasterizeScene(
				const IScene& pScene,
				const ISampling2D& pSampling,
				unsigned int* pActualTime
				) const;

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;

			void RasterizeSceneAnimation(
				const IScene& pScene,
				const Scalar time_start,
				const Scalar time_end,
				const unsigned int num_frames,
				const bool do_fields,
				const bool invert_fields,
				const Rect* pRect,
				const unsigned int* specificFrame,
				IRasterizeSequence* pRasterSequence
				) const {};

			/// Thread procedure for parallel round-based chain execution
			static void* RoundThread_ThreadProc( void* lpParameter );
		};
	}
}

#endif
