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

#include "PixelBasedRasterizerHelper.h"
#include "SplatFilm.h"
#include "../Shaders/VCMIntegrator.h"
#include "../Shaders/VCMLightVertexStore.h"
#include "../Utilities/StabilityConfig.h"

#include <atomic>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		class VCMRasterizerBase : public virtual PixelBasedRasterizerHelper
		{
		protected:
			VCMIntegrator*			pIntegrator;
			mutable SplatFilm*		pSplatFilm;
			mutable LightVertexStore*	pLightVertexStore;
			mutable IRasterImage*	pScratchImage;		///< Scratch buffer for progressive output with splats
			mutable Scalar			mSplatTotalSamples;	///< Cached for progressive resolve
			mutable std::atomic<uint64_t>	mTotalAdaptiveSamples;	///< Total camera samples across all pixels (adaptive)

			/// Per-iteration VCM normalization.  Recomputed at the
			/// start of every light pass and read by the eye pass.
			mutable VCMNormalization	mVCMNormalization;

			StabilityConfig			stabilityConfig;

			virtual ~VCMRasterizerBase();

			/// Subclasses report a short rasterizer name used for the
			/// progress title string.  Mirrors BDPTRasterizerBase.
			virtual const char* GetProgressTitle() const = 0;

			/// Returns a scaling factor for splat film resolution.  Pel
			/// returns 1; Spectral returns nSpectralSamples.  Mirrors
			/// BDPTRasterizerBase::GetSplatSampleScale.
			virtual Scalar GetSplatSampleScale() const { return 1.0; }

			/// Override called by PixelBasedRasterizerHelper::RasterizeScene
			/// BEFORE the per-pixel block dispatch.  We use it to run
			/// the VCM light pass: generate all light subpaths, walk
			/// each through ConvertLightSubpath, fill the
			/// LightVertexStore, and build the KD-tree.  The eye pass
			/// then queries this store from IntegratePixel.
			virtual void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

			/// Override of the progressive preview hook to composite
			/// the splat film on top of the primary image.  BDPT uses
			/// the same pattern — see BDPTRasterizerBase::GetIntermediateOutputImage.
			virtual IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

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

			/// Thread-safe: adds to the total adaptive sample counter.
			void AddAdaptiveSamples( uint64_t count ) const;

			/// Returns the effective SPP for splat film resolution,
			/// accounting for adaptive / progressive sampling if active.
			Scalar GetEffectiveSplatSPP( unsigned int width, unsigned int height ) const;

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
