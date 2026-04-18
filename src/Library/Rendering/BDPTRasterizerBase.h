//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizerBase.h - Base class for BDPT rasterizers.
//
//    Provides the common BDPT infrastructure shared by both the
//    Pel (RGB) and Spectral variants:
//    - BDPTIntegrator ownership
//    - SplatFilm management
//    - RasterizeScene override (splat film lifecycle, block dispatch)
//
//    Subclasses override IntegratePixel() to implement mode-specific
//    pixel integration (RGB accumulation vs spectral/XYZ).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_RASTERIZER_BASE_
#define BDPT_RASTERIZER_BASE_

#include "PixelBasedRasterizerHelper.h"
#include "AOVBuffers.h"
#include "SplatFilm.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/CompletePathGuide.h"
#include "../Utilities/StabilityConfig.h"
#include <atomic>

namespace RISE
{
	namespace Implementation
	{
		class AOVBuffers;

		class BDPTRasterizerBase : public virtual PixelBasedRasterizerHelper
		{
		protected:
			BDPTIntegrator*			pIntegrator;
			ManifoldSolver*			pManifoldSolver;
			mutable SplatFilm*		pSplatFilm;
			mutable IRasterImage*	pScratchImage;		///< Scratch buffer for progressive output with splats
			mutable Scalar			mSplatTotalSamples;	///< Cached for progressive resolve
			mutable std::atomic<uint64_t>	mTotalAdaptiveSamples;	///< Total camera samples across all pixels (adaptive)

			// pAOVBuffers is inherited from PixelBasedRasterizerHelper

#ifdef RISE_ENABLE_OPENPGL
			mutable PathGuidingField*	pGuidingField;	///< Learned radiance distribution for eye subpath guided sampling
			mutable PathGuidingField*	pLightGuidingField;	///< Separate field for light subpath guided sampling (Option B)
			mutable CompletePathGuide*	pCompletePathGuide;	///< Experimental BDPT complete-path recorder
			mutable Scalar				guidingAlphaScale;
#endif
			PathGuidingConfig		guidingConfig;		///< Path guiding configuration
			StabilityConfig			stabilityConfig;	///< Production stability controls

			virtual ~BDPTRasterizerBase();

			/// Returns a scaling factor for splat film resolution.
			/// Pel returns 1; Spectral returns nSpectralSamples.
			virtual Scalar GetSplatSampleScale() const { return 1.0; }

			/// Returns the progress title string for this rasterizer variant.
			virtual const char* GetProgressTitle() const = 0;

			/// Returns a scratch image with resolved splats composited
			/// on top of the current primary, for progressive display.
			IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

			/// Splat a t==1 contribution at a FRACTIONAL raster position,
			/// spread across the pixel filter's footprint so caustic /
			/// light-to-camera splats reconstruct with the same
			/// Mitchell-Netravali / Lanczos / box kernel the rest of
			/// BDPT uses for t>=2 samples.  Falls back to a round-to-
			/// nearest point splat when no filter is configured.
			///
			/// `fx`, `fy` are in image-buffer coordinates (y=0 at
			/// top) — callers convert from `cr.rasterPos` by flipping
			/// y with `camera.GetHeight() - cr.rasterPos.y`.
			void SplatContributionToFilm(
				const Scalar fx,
				const Scalar fy,
				const RISEPel& contribution,
				const unsigned int imageWidth,
				const unsigned int imageHeight
				) const;

		public:
			BDPTRasterizerBase(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingCfg,
				const StabilityConfig& stabilityCfg
				);

			/// Thread-safe: adds to the total adaptive sample counter
			void AddAdaptiveSamples( uint64_t count ) const;

			/// Returns the effective SPP for splat film resolution,
			/// accounting for adaptive sampling if active
			Scalar GetEffectiveSplatSPP( unsigned int width, unsigned int height ) const;

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;
		};
	}
}

#endif
