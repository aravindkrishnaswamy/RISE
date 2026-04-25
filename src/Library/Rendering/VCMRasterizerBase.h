//////////////////////////////////////////////////////////////////////
//
//  VCMRasterizerBase.h - Base class for the VCM rasterizers.
//
//    Mirrors BDPTRasterizerBase: holds the VCMIntegrator pointer,
//    the splat film used for t=1 light-to-camera contributions, and
//    (new for VCM) the LightVertexStore that merging queries during
//    the eye pass.
//
//    Step 0 ships the ownership scaffolding only; RasterizeScene is
//    inherited unchanged from PixelBasedRasterizerHelper so the Pel
//    / Spectral subclasses can write a solid color via IntegratePixel
//    and still exercise the full parse + create + dispatch + output
//    pipeline.
//
//    Step 6 overrides RasterizeScene at this layer to implement the
//    two-pass (light subpaths -> tree build -> eye subpaths) VCM
//    iteration loop.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_RASTERIZER_BASE_
#define VCM_RASTERIZER_BASE_

#include "BidirectionalRasterizerBase.h"
#include "../Shaders/VCMIntegrator.h"
#include "../Shaders/VCMLightVertexStore.h"

namespace RISE
{
	namespace Implementation
	{
		class VCMRasterizerBase : public BidirectionalRasterizerBase
		{
		protected:
			VCMIntegrator*			pIntegrator;
			mutable LightVertexStore*	pLightVertexStore;

			// pSplatFilm, pScratchImage, mSplatTotalSamples,
			// mTotalAdaptiveSamples, stabilityConfig, and the
			// splat-film helpers all live in BidirectionalRasterizerBase.

			/// Per-iteration VCM normalization.  Recomputed at the
			/// start of every light pass and read by the eye pass.
			mutable VCMNormalization	mVCMNormalization;

			///////////////////////////////////////////////////////////
			// Progressive radius reduction state (SPPM-style with
			// adaptive floor).  r_{n+1} = r_n * sqrt((n-1+alpha)/n),
			// clamped to mMergeRadiusFloor so we don't shrink past
			// the point where Poisson noise on photon count drowns
			// out bias reduction.
			///////////////////////////////////////////////////////////
			mutable Scalar mBaseMergeRadius;			///< r_0, fixed for the render (auto-radius or user-supplied)
			mutable Scalar mCurrentMergeRadius;			///< r_n, shrinks each progressive pass
			mutable Scalar mMergeRadiusFloor;			///< Adaptive lower bound on r_n (recomputed per pass)
			mutable Scalar mGeometricRadiusFloor;		///< Scene-derived safety floor (0.001 * medianSegment)
			mutable unsigned int mMergeRadiusPassCount;	///< n in the Hachisuka shrinkage formula

			// Tunable constants (exposed via scene params in a later step).
			Scalar mRadiusShrinkAlpha;			///< Hachisuka alpha; 2/3 gives optimal rate
			Scalar mTargetPhotonsPerQuery;		///< Expected photons-per-query used to derive density floor
			bool   mProgressiveRadiusEnabled;	///< Kill-switch: false => fixed radius (legacy behavior)

			// Outlier light-vertex throughput clamp.  Each pass, photons
			// whose Rec.709 luminance exceeds `multiplier × percentile`
			// of the per-pass distribution are rescaled down (color
			// preserved).  Mitigates fireflies from rare bright photons
			// in high-variance paths (notably SSS where Rd/pdfSurface
			// fluctuates).  Biased — the bias decreases with smaller
			// merge radius and more iterations, matching production
			// photon-mapping practice.  Set multiplier=0 to disable.
			Scalar mThroughputClampPercentile;	///< 0..1; 0.99 = 99th percentile of luminance
			Scalar mThroughputClampMultiplier;	///< threshold = multiplier × percentile_value; 0 disables

			virtual ~VCMRasterizerBase();

			/// Override called by PixelBasedRasterizerHelper::RasterizeScene
			/// BEFORE the per-pixel block dispatch.  We use it to run
			/// the VCM light pass: generate all light subpaths, walk
			/// each through ConvertLightSubpath, fill the
			/// LightVertexStore, and build the KD-tree.  The eye pass
			/// then queries this store from IntegratePixel.
			virtual void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

		/// VCM runs 1 SPP per pass (paper-correct iteration model).
		/// Per-block intermediate output is wasted I/O; the end-of-pass
		/// flush in the progressive loop still runs and gives the user
		/// one progress update per iteration.
		bool SkipPerBlockIntermediateOutput() const { return true; }

		/// Rebuild the light vertex store at each progressive pass
		/// so merge-query noise averages across passes instead of
		/// persisting from a single fixed store.  Uses passIdx as
		/// a seed offset so each pass generates different photon
		/// positions.
		virtual void OnProgressivePassBegin(
			const IScene& pScene,
			const unsigned int passIdx ) const;

		/// Override the final flush to resolve the splat film
			/// into a scratch copy of the primary image before
			/// forwarding to the rasterizer outputs.  Mirrors BDPT's
			/// final-flush splat resolve.
			virtual void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

			/// When OIDN denoising is enabled, RasterizeScene routes
			/// the main flush through FlushPreDenoisedToOutputs and
			/// FlushDenoisedToOutputs instead of FlushToOutputs.
			/// Both overrides apply the same splat-resolve composition.
			virtual void FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			virtual void FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;

		public:
			VCMRasterizerBase(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const Scalar mergeRadius,
				const bool enableVC,
				const bool enableVM,
				const StabilityConfig& stabilityCfg
				);

			/// Normalization snapshot read by the eye pass.  Public
			/// so IntegratePixel can reach it via a cast in Step 7.
			const VCMNormalization& GetNormalization() const { return mVCMNormalization; }

			/// Light vertex store built during the light pass and
			/// queried by EvaluateMerges during the eye pass.
			const LightVertexStore* GetLightVertexStore() const { return pLightVertexStore; }
		};
	}
}

#endif
