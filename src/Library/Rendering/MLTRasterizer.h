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

#include <climits>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		//! Mutation budget split shared by MLTRasterizer and
		//! MLTSpectralRasterizer.  All counts are 64-bit: the budget
		//! mutations/pixel * width * height exceeds 2^32 for ordinary
		//! production settings (3840x2160 at 518 mutations/pixel).
		//!
		//! Every chain always runs at least one mutation, so for a budget
		//! smaller than the chain count (or not divisible by it) the work
		//! actually PERFORMED is perChain * effectiveChains, which differs
		//! from the requested budget: it is below it by fewer than
		//! effectiveChains mutations (truncation), or above it by fewer than
		//! effectiveChains mutations when budget < chains.  Splats are
		//! therefore normalized by `performed`, never by `budget`, so the
		//! resolved image is correctly exposed for every accepted budget.
		struct MLTMutationSchedule
		{
			unsigned int	effectiveChains;	///< max(1, nChains)
			std::uint64_t	budget;				///< mutations/pixel * width * height (saturating)
			std::uint64_t	perChain;			///< max(1, budget / effectiveChains)
			std::uint64_t	performed;			///< perChain * effectiveChains
		};

		//! Saturating 64-bit product (saturates at UINT64_MAX; unreachable
		//! for 32-bit width/height/mutations unless all three are huge).
		inline std::uint64_t MLTSaturatingMul( const std::uint64_t a, const std::uint64_t b )
		{
			if( a != 0 && b > UINT64_MAX / a ) {
				return UINT64_MAX;
			}
			return a * b;
		}

		inline MLTMutationSchedule ComputeMLTMutationSchedule(
			const unsigned int mutationsPerPixel,
			const unsigned int width,
			const unsigned int height,
			const unsigned int nChains
			)
		{
			MLTMutationSchedule s;
			s.effectiveChains = nChains > 0 ? nChains : 1;
			s.budget = MLTSaturatingMul(
				MLTSaturatingMul( mutationsPerPixel, width ), height );
			const std::uint64_t q = s.budget / s.effectiveChains;
			s.perChain = q > 0 ? q : 1;
			// perChain <= UINT64_MAX / effectiveChains whenever q > 0, and
			// equals 1 otherwise, so this product cannot wrap.
			s.performed = s.perChain * s.effectiveChains;
			return s;
		}

		//! Clamp a desired round count to [1, perChain] so every round has
		//! at least one mutation per chain.
		inline std::uint64_t ClampMLTRoundCount( const std::uint64_t desired, const std::uint64_t perChain )
		{
			const std::uint64_t pc = perChain > 0 ? perChain : 1;
			const std::uint64_t r = desired > 0 ? desired : 1;
			return r < pc ? r : pc;
		}

		//! Convert a (possibly huge, non-finite or negative) floating-point
		//! round estimate to a round count without an out-of-range
		//! float->integer conversion, then clamp to [lo, perChain].
		inline std::uint64_t MLTRoundCountFromEstimate( const double estimate, const std::uint64_t lo, const std::uint64_t perChain )
		{
			std::uint64_t desired = lo;
			if( estimate >= 18446744073709549568.0 ) {	// largest double below 2^64
				desired = UINT64_MAX;
			} else if( estimate > static_cast<double>( lo ) ) {
				desired = static_cast<std::uint64_t>( estimate );
			}
			return ClampMLTRoundCount( desired, perChain );
		}

		//! Per-chain mutations per round: max(1, perChain / numRounds),
		//! clamped to UINT_MAX (the chain-segment count is unsigned int).
		inline unsigned int MLTMutationsPerRound( const std::uint64_t perChain, const std::uint64_t numRounds )
		{
			const std::uint64_t q = perChain / ( numRounds > 0 ? numRounds : 1 );
			if( q == 0 ) {
				return 1;
			}
			return q > UINT_MAX ? UINT_MAX : static_cast<unsigned int>( q );
		}

		//! Mutations to run this round.  The round loop is driven by the
		//! remaining per-chain work, so this never exceeds `remaining` and
		//! never underflows.  The planned last round takes the whole
		//! remainder (preserving the original cadence, where the final
		//! round absorbs the perChain % numRounds leftover); any other round
		//! takes min(perRound, remaining).  Both are clamped to UINT_MAX; a
		//! clamped round simply leaves work for further rounds.
		inline unsigned int MLTMutationsThisRound(
			const std::uint64_t remaining,
			const unsigned int perRound,
			const bool plannedLastRound
			)
		{
			std::uint64_t m = plannedLastRound ? remaining
				: ( remaining < perRound ? remaining : static_cast<std::uint64_t>( perRound ) );
			return m > UINT_MAX ? UINT_MAX : static_cast<unsigned int>( m );
		}

		//! Adaptive round resize after round 0: from a desired per-round
		//! count (floating point, any value) and the remaining per-chain
		//! work, produce the new per-round count (>= 1, <= UINT_MAX) and
		//! the new total round count (1 already done + ceil(remaining /
		//! newPerRound)), computed without overflow.
		inline void MLTAdaptiveRoundResize(
			const double desiredMutPerRound,
			const std::uint64_t remaining,
			unsigned int& newPerRound,
			std::uint64_t& newNumRounds
			)
		{
			if( !( desiredMutPerRound >= 1.0 ) ) {
				newPerRound = 1;
			} else if( desiredMutPerRound >= static_cast<double>( UINT_MAX ) ) {
				newPerRound = UINT_MAX;
			} else {
				newPerRound = static_cast<unsigned int>( desiredMutPerRound );
			}
			const std::uint64_t remainingRounds = remaining / newPerRound +
				( remaining % newPerRound != 0 ? 1 : 0 );
			newNumRounds = 1 + remainingRounds;
		}

		class MLTRasterizer : public virtual Rasterizer
		{
		protected:

			IRayCaster*				pCaster;
			BDPTIntegrator*			pIntegrator;

			// Pixel reconstruction kernel (Mitchell-Netravali, Lanczos,
			// box, …).  MLT does not use a conventional pixel sampler
			// — the Markov chain generates film samples directly — so
			// pSampling is stored for API symmetry but never consumed.
			// pPixelFilter IS consumed: every splat is distributed
			// across the filter's footprint at a fractional raster
			// position so the output has proper sub-pixel
			// reconstruction instead of the hard edges that came from
			// truncating to an integer pixel.
			ISampling2D*			pSampling;
			IPixelFilter*			pPixelFilter;

			// MLT-specific parameters
			unsigned int			nBootstrap;			///< Number of bootstrap samples (default 100000)
			unsigned int			nChains;			///< Number of independent Markov chains (default 512)
			unsigned int			nMutationsPerPixel;	///< Mutations per pixel budget (default 100)
			Scalar					largeStepProb;		///< Probability of large step mutation (default 0.3)
			/// A single strategy's contribution with its own pixel position.
			/// Strategies with t<=1 (light-to-camera connections) produce
			/// contributions at arbitrary pixel positions, so each strategy
			/// must be splatted independently.
			struct MLTStrategySplat
			{
				RISEPel		color;			///< MIS-weighted contribution for this strategy
				Point2		rasterPos;		///< Pixel position for splatting

				MLTStrategySplat() :
				color( RISEPel( 0, 0, 0 ) ),
				rasterPos( Point2( 0, 0 ) )
				{
				}
			};

			/// Result of evaluating a single BDPT sample through the MLT pipeline.
			/// Contains per-strategy contributions (each with its own pixel
			/// position) and the aggregate scalar luminance for Metropolis
			/// acceptance computation.
			struct MLTSample
			{
				std::vector<MLTStrategySplat>	splats;		///< Per-strategy contributions with pixel positions
				Scalar		luminance;		///< Total scalar luminance for acceptance ratio
				bool		valid;			///< False if the path produced zero contribution

				MLTSample() :
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

			/// Generate a camera ray using the PSSMLT lens sample when
			/// the camera supports it (currently only ThinLensCamera);
			/// otherwise fall back to the standard ICamera::GenerateRay
			/// path which pulls lens samples from rc.random.
			///
			/// Implemented as a plain static method that dynamic_casts
			/// to the concrete ThinLensCamera rather than a virtual
			/// method on ICamera — adding a virtual to the camera
			/// interface (even appended) would break out-of-tree
			/// camera implementations compiled against the old vtable
			/// the moment this new caller dispatched past the end of
			/// their table.  dynamic_cast is a single pointer check
			/// per pixel sample and cost is negligible next to the
			/// path evaluation that follows.
			static bool GenerateCameraRayWithLensSample(
				const ICamera& camera,
				const RuntimeContext& rc,
				Ray& ray,
				const Point2& ptOnScreen,
				const Point2& lensSample
				);

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
			///
			/// chainIndex is mixed into the RNG seeds so that two chains
			/// selecting the same bootstrap sample do not become identical
			/// Markov trajectories (which would concentrate splats and
			/// create fireflies).  See the implementation for details.
			void InitChain(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				const BootstrapSample& seed,
				const unsigned int chainIndex,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Render a single MLT frame end-to-end: bootstrap → CDF →
			/// chain init → round-based mutations → final resolve.
			/// Shared between still-image RasterizeScene and per-frame
			/// RasterizeSceneAnimation so both observe identical chain
			/// semantics ("independent chains" — every call allocates
			/// fresh PSSMLTSampler state and re-bootstraps against the
			/// current scene).
			///
			/// Returns true if all rounds completed.  Returns false if
			/// the user cancelled mid-render OR the bootstrap found no
			/// luminance (degenerate scene).  In either case `pImageOut`
			/// may be NULL or hold the last partially-resolved snapshot
			/// — callers in animation context discard it; the still
			/// image caller flushes it (cancel-still-denoises).
			///
			/// On success, `pImageOut` is a refcount-1 image owned by
			/// the caller; the caller must safe_release it after flush.
			/// AttachScene + LightSampler setup must be done by the
			/// caller before invoking — they are scene-wide, not per-
			/// frame.
			bool RenderFrameOfMLT(
				const IScene& pScene,
				const ICamera& pCamera,
				const unsigned int width,
				const unsigned int height,
				IRasterImage*& pImageOut
				) const;

			/// Explicit per-call progress sink; animation supplies a stack
			/// frame-slot adapter without changing the authored callback.
			bool RenderFrameOfMLT(
				const IScene& pScene,
				const ICamera& pCamera,
				const unsigned int width,
				const unsigned int height,
				IRasterImage*& pImageOut,
				IProgressCallback* progress
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
#ifdef RISE_ENABLE_OIDN
			double EstimateDenoiseWorkPerMegapixel() const { return nMutationsPerPixel * 0.4; }
#endif
			MLTRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const unsigned int nBootstrap_,
				const unsigned int nChains_,
				const unsigned int nMutationsPerPixel_,
				const Scalar largeStepProb_,
				RISE::Implementation::FrameStore* frameStore = nullptr
				);

			//////////////////////////////////////////////////////////////
			// MLT does not inherit from PixelBasedRasterizerHelper so it
			// defines its own SubSampleRays.  MLT ignores the sampler
			// (the Markov chain generates film samples itself) but DOES
			// consume the pixel filter — without this hook the filter
			// would never reach the splat path and the output would
			// have visible pixel-aligned hard edges.
			//////////////////////////////////////////////////////////////
			virtual void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );

			//////////////////////////////////////////////////////////////
			// IRasterizer interface
			//////////////////////////////////////////////////////////////

			void AttachToScene( const IScene* ){};
			void DetachFromScene( const IScene* ){};

			// L6d-2b — MLT now opts BACK INTO the FrameStore push.
			// L6d-2b copies each per-round local image into
			// `mFrameStore` before flushing (see
			// `MLTRasterizer::Flush*` implementations) so direct
			// FrameStore observers receive `OnFrameComplete` /
			// `OnPreDenoiseComplete` / `OnDenoiseComplete` reading
			// the up-to-date splatted final.
			//
			// Pre-L6d-2b: `AcceptsFrameStorePush` returned `false`
			// (L6e-1.1 string-match-replacement) so Job never
			// pushed a FrameStore to MLT and MLT's per-round Resolve
			// allocated a fresh local `RISERasterImage` per round
			// without ever reaching the canonical store.  Bound-VFS
			// observers attached to MLT's `mFrameStore` would have
			// stayed perpetually empty.
			//
			// Inheriting the default `true` from `Rasterizer` —
			// override removed entirely.

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

			//! MLT's splat film is global — pRect is accepted but not
			//! honored (see RasterizeScene's "Fields and pRect are not
			//! yet honored" note).  Report that honestly so region-render
			//! UIs can label the state instead of pretending.
			bool HonorsRegion() const { return false; }

			//! Render an animation by re-running the full MLT pipeline
			//! per frame: bootstrap → CDF → chain init → round-based
			//! mutations.  Markov chains do NOT carry across frames
			//! ("independent chains per frame" / Strategy A — see
			//! docs/OIDN.md decision log "2026-04-29 MLT animation").
			//! Each frame's bootstrap re-estimates the normalization
			//! constant against the current scene state, so changes in
			//! camera, transforms, or lights do not poison chains
			//! warmed up against a previous frame's geometry.  Per-frame
			//! OIDN denoise mirrors PixelBasedRasterizerHelper::
			//! RasterizeSceneAnimation: BeginRenderTimer + AOV reset +
			//! cancel-guarded ApplyDenoise + FlushPreDenoised /
			//! FlushDenoised at the actual frameIdx.  Cancel mid-frame
			//! abandons the frame entirely (no flush, no denoise) so
			//! the MOV writer's tail stays clean.
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
				) const;

			/// Thread procedure for parallel round-based chain execution
			static void* RoundThread_ThreadProc( void* lpParameter );

		protected:
			// Dispatch helpers mirroring the pattern used by other
			// rasterizers.  File-based outputs write pattern.ext via the
			// first two and pattern_denoised.ext via the third; non-file
			// outputs observe a single OutputImage call with the denoised
			// final (preserving pre-change behavior).
			void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			void FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			void FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			// L6d-2b — Copy a fully-rendered local image into the
			// rasterizer's canonical `mFrameStore` (per-tile,
			// bracketed via `FrameStore::CopyTileFromRasterImage`).
			// Returns true on a SUCCESSFUL copy (mFrameStore present
			// + dims match); false when no copy happened (mFrameStore
			// null or dim-mismatch).  Callers gate Mark* fires on
			// the return value — firing MarkFrameComplete on a
			// stale-content store would mislead observers.
			// See impl in MLTRasterizer.cpp for the contract.
			bool CopyToFrameStore_( const IRasterImage& src ) const;
		};
	}
}

#endif
