//////////////////////////////////////////////////////////////////////
//
//  MMLTRasterizer.h - Multiplexed Metropolis Light Transport
//    rasterizer (Hachisuka et al. 2014).
//
//  CONTEXT:
//    PSSMLT (the existing MLTRasterizer) runs ONE Markov chain whose
//    density adapts to the SUMMED luminance over all (s,t)
//    connection strategies for each path it proposes.  On SDS scenes
//    the dim strategies (e.g., diffuse-wall paths) get visited
//    rarely because the chain spends most of its time on the bright
//    caustic strategy that dominates the sum — so 5x more PSSMLT
//    samples reduce noise by only ~30% (vs ~56% expected from i.i.d.
//    1/sqrt(N) scaling).  The dominant residual variance is chain
//    autocorrelation from being trapped in dim-strategy starvation.
//
//    MMLT addresses this by partitioning the chain budget into one
//    pool PER PATH-LENGTH DEPTH d = s + t - 2.  Each chain in pool d
//    only ever evaluates strategies (s,t) with s+t = d+2, and its
//    density adapts to the per-DEPTH luminance b_d.  Diffuse-wall
//    contributions at depth 6 get exactly the budget they earn from
//    their share of the total path-space integral, independent of
//    how bright the depth-3 caustic is.
//
//  ARCHITECTURE (Phase 3 — single-depth force mode):
//    For initial bring-up we restrict every chain to ONE specific
//    depth ("force_depth N") so we can validate the bootstrap +
//    chain + splat machinery for one depth in isolation, before
//    layering on the per-depth distribution logic that Phase 4 adds.
//    The class structure is the same shape as MLTRasterizer (so the
//    diff is reviewable) but every call into the BDPT integrator
//    goes through ConnectAndEvaluateForMMLT(s, t, ...) — single
//    strategy per mutation, contribution multiplied by nStrategies.
//
//  REFERENCES:
//    Hachisuka, Kaplanyan, Dachsbacher.  "Multiplexed Metropolis
//    Light Transport."  ACM Trans. Graph. 33(4), 2014.
//    PBRT v3 Chapter 16.4.5 — canonical implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 18, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MMLT_RASTERIZER_
#define MMLT_RASTERIZER_

#include "Rasterizer.h"
#include "SplatFilm.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Interfaces/IRayCaster.h"
#include "../Utilities/MMLTSampler.h"
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		class MMLTRasterizer : public virtual Rasterizer
		{
		protected:

			IRayCaster*			pCaster;
			BDPTIntegrator*		pIntegrator;

			// The pixel filter is consumed by the splat path identical
			// to MLTRasterizer; pSampling is stored only for API symmetry
			// with the rest of the rasterizer family.
			ISampling2D*		pSampling;
			IPixelFilter*		pPixelFilter;

			// Algorithm parameters
			unsigned int		maxEyeDepth;
			unsigned int		maxLightDepth;
			unsigned int		nBootstrap;
			unsigned int		nChains;
			unsigned int		nMutationsPerPixel;
			Scalar				largeStepProb;

			// Phase 3: forceDepth >= 0 restricts every chain to exactly
			// this path-length depth (d = s + t - 2).  Phase 4 will
			// remove this restriction by populating per-depth pools;
			// for now any depth other than forceDepth contributes
			// nothing, so the rendered image only contains paths whose
			// total vertex count is forceDepth + 2.  Useful for
			// isolating the bootstrap/chain machinery.
			int					forceDepth;

			/// Single-strategy evaluation result for one MMLT chain
			/// proposal.  Unlike PSSMLT (which evaluates ALL strategies
			/// per sample and produces a vector of per-strategy splats),
			/// MMLT picks ONE (s,t) per iteration and produces a single
			/// splat — the chain density adapts to per-strategy
			/// luminance which is what fixes the dim-strategy starvation.
			struct MMLTSample
			{
				RISEPel				color;			///< MIS-weighted strategy contribution * nStrategies
				Point2				rasterPos;		///< Splat target pixel (camera-projected for t=1, camera-ray pixel otherwise)
				Scalar				luminance;		///< Scalar luminance of color, for MH acceptance
				bool				valid;			///< false if subpath couldn't reach (s,t) or contribution is zero
				unsigned int		s;				///< Picked s, for diagnostics
				unsigned int		t;				///< Picked t, for diagnostics
				unsigned int		nStrategies;	///< nStrategies(d) at the time of selection (for splat math review)

				MMLTSample() :
					color( RISEPel( 0, 0, 0 ) ),
					rasterPos( Point2( 0, 0 ) ),
					luminance( 0 ),
					valid( false ),
					s( 0 ),
					t( 0 ),
					nStrategies( 0 )
				{
				}
			};

			/// Bootstrap record per sample.  Same shape as MLTRasterizer
			/// because the bootstrap is also luminance-CDF-importance-
			/// sampled to seed the Markov chain.
			struct BootstrapSample
			{
				Scalar				luminance;
				unsigned int		seed;

				BootstrapSample() : luminance( 0 ), seed( 0 ) {}
			};

			/// Per-chain persistent state across rounds (Phase 5 will
			/// add the round-based progressive loop; for Phase 3 the
			/// chain runs to completion in one pass).
			///
			/// `pSampler` is an owning raw pointer to a refcounted
			/// MMLTSampler; the rasterizer release()s it explicitly at
			/// end of RasterizeScene.  Copy and assignment are deleted
			/// so a future `chainStates.push_back` / `resize` cannot
			/// silently shallow-copy the pointer and cause a
			/// double-release at cleanup (Reviewer B latent-bug
			/// finding).  The default ctor remains so
			/// `std::vector<ChainState>(N)` works for the one-shot
			/// allocation in RasterizeScene.
			struct ChainState
			{
				MMLTSampler*			pSampler;
				MMLTSample				currentSample;
				RandomNumberGenerator	chainRNG;

				ChainState() : pSampler( 0 ), chainRNG( 0 ) {}

				ChainState( const ChainState& ) = delete;
				ChainState& operator=( const ChainState& ) = delete;
			};

			virtual ~MMLTRasterizer();

			/// Same lens-aware camera-ray helper as MLTRasterizer (the
			/// PSSMLT lens sample drives aperture motion continuously
			/// for ThinLensCamera).  Plain static so it doesn't grow
			/// the ICamera vtable.
			static bool GenerateCameraRayWithLensSample(
				const ICamera& camera,
				const RuntimeContext& rc,
				Ray& ray,
				const Point2& ptOnScreen,
				const Point2& lensSample
				);

			/// Evaluate a single proposed path.  Picks (s,t) for this
			/// sampler's bound depth, generates light + eye subpaths,
			/// and evaluates ONLY the chosen strategy via
			/// BDPTIntegrator::ConnectAndEvaluateForMMLT.  The
			/// strategy's contribution is scaled by nStrategies to undo
			/// the 1/nStrategies selection PDF.
			MMLTSample EvaluateSample(
				const IScene& scene,
				const ICamera& camera,
				MMLTSampler& sampler,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Initialize a single chain.  Mirrors the two-phase init
			/// from MLTRasterizer::InitChain — first iteration uses the
			/// bootstrap-selected seed so the chain starts at a high-
			/// luminance path the CDF picked, then the proposal RNG is
			/// re-seeded for chain-specific divergence.
			void InitChain(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				const BootstrapSample& seed,
				const unsigned int chainIndex,
				const unsigned int width,
				const unsigned int height,
				const unsigned int chainDepth
				) const;

			/// Run a fixed number of mutations on an existing chain.
			/// `normalization` is the per-depth b_d for this chain's
			/// depth.  Both `numMutations` and `totalMutationsAtThisDepth`
			/// are u64 because nMutationsPerPixel × W × H can exceed
			/// 2^32 for high-resolution renders (Reviewer E Phase 4 R2
			/// finding: 1080p × 1000 mut/pixel ≈ 2.07B; 4K × 1000
			/// overflows).  Phase 5 R1 Reviewer A found that the
			/// per-round `mutationsPerChain / numRounds` cast back to
			/// u32 silently truncated and re-introduced the bug; widening
			/// numMutations to u64 closes that gap end-to-end.
			void RunChainSegment(
				ChainState& state,
				const IScene& scene,
				const ICamera& camera,
				SplatFilm& splatFilm,
				const std::uint64_t numMutations,
				const Scalar normalization,
				const std::uint64_t totalMutationsAtThisDepth,
				const unsigned int width,
				const unsigned int height
				) const;

			/// Binary search a CDF for importance-sampled chain init.
			/// Identical to MLTRasterizer::SelectFromCDF.
			unsigned int SelectFromCDF(
				const std::vector<Scalar>& cdf,
				const Scalar u
				) const;

			//////////////////////////////////////////////////////////////
			// Phase 4 helpers — extracted from RasterizeScene so the
			// per-depth loop body is small and the same code paths are
			// exercised whether forceDepth >= 0 (one depth) or < 0 (all
			// depths).  Reviewer E's recommendation from Round 2.
			//////////////////////////////////////////////////////////////

			/// Run `nSamples` independent MMLT samples bound to one
			/// specific depth, using seeds [seedOffset, seedOffset+nSamples).
			/// Appends per-sample luminances/indexes to `outSamples` (so
			/// the same out vector can collect samples from a probe pass
			/// + a main pass), and returns the per-depth normalization
			/// b_d = mean(luminance over THIS CALL ONLY) * numPixels.
			/// Returns 0 if every sample produced zero luminance.
			///
			/// `seedOffset` lets the caller stitch multiple bootstrap
			/// passes together without seed collisions: a probe pass uses
			/// [0, nProbe) and the main pass uses [nProbe, nProbe + nMain).
			/// Cross-depth rescue uses an out-of-band offset (e.g. starting
			/// at 1<<24) so rescued seeds never collide with regular seeds
			/// of any other depth.
			Scalar BootstrapAtDepth(
				const IScene& scene,
				const ICamera& camera,
				const unsigned int depth,
				const unsigned int width,
				const unsigned int height,
				const unsigned int nSamples,
				const unsigned int seedOffset,
				std::vector<BootstrapSample>& outSamples
				) const;

			/// Cross-depth rescue: try evaluating `seeds` (each an existing
			/// MMLTSampler seed value) AT a different depth than where they
			/// originated.  Useful when one depth's bootstrap produces
			/// b_d=0 due to variance: a successful film position from a
			/// neighbouring depth often also supports paths at the dead
			/// depth (same camera ray hits the same caustic feeder), and
			/// we can recover signal that pure i.i.d. bootstrap missed.
			///
			/// Each input seed produces one BootstrapSample appended to
			/// `outSamples`.  Samples whose evaluated luminance is zero
			/// are still appended so the caller can build a CDF that
			/// covers the full seed list, but the returned b_d only
			/// counts the non-zero successes.
			Scalar RescueAtDepth(
				const IScene& scene,
				const ICamera& camera,
				const unsigned int depth,
				const unsigned int width,
				const unsigned int height,
				const std::vector<unsigned int>& seeds,
				std::vector<BootstrapSample>& outSamples
				) const;

			/// Build a normalized CDF from a set of bootstrap luminances.
			/// CDF is empty if the input has zero total weight.
			void BuildCDF(
				const std::vector<BootstrapSample>& samples,
				std::vector<Scalar>& outCDF
				) const;

			/// Distribute a fixed chain budget across depths, proportional
			/// to per-depth normalizations.  Depths with bd[d] > 0 get at
			/// least one chain; over- or under-allocation from rounding is
			/// fixed by trimming the smallest-bd depths or adding to the
			/// largest-bd depth.  Output size = bd.size().
			void AllocateChainsPerDepth(
				const std::vector<Scalar>& bd,
				const unsigned int totalChains,
				std::vector<unsigned int>& outChainsPerDepth
				) const;

		public:
			MMLTRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const unsigned int nBootstrap_,
				const unsigned int nChains_,
				const unsigned int nMutationsPerPixel_,
				const Scalar largeStepProb_,
				const int forceDepth_
				);

			//////////////////////////////////////////////////////////////
			// MMLT does not inherit from PixelBasedRasterizerHelper, so
			// it overrides SubSampleRays directly to install the pixel
			// filter that the splat path consumes.  See the matching
			// MLTRasterizer comment for the rationale.
			//////////////////////////////////////////////////////////////
			virtual void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );

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

		protected:
			/// File-output dispatch helpers — same shape as MLTRasterizer.
			void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
		};
	}
}

#endif
