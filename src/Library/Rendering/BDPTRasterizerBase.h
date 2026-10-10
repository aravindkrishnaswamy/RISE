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

#include "BidirectionalRasterizerBase.h"
#include "AOVBuffers.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/CompletePathGuide.h"

namespace RISE
{
	namespace Implementation
	{
		class AOVBuffers;

		class BDPTRasterizerBase : public BidirectionalRasterizerBase
		{
		protected:
			//! DL-465: motion-blurred animation frames render multi-threaded
			//! through per-thread time-indexed scene state.
			bool SupportsTimeIndexedMotionBlur() const { return true; }

			BDPTIntegrator*			pIntegrator;

			// pAOVBuffers is inherited from PixelBasedRasterizerHelper.
			// pSplatFilm, pScratchImage, mSplatTotalSamples,
			// mTotalAdaptiveSamples, stabilityConfig, and the
			// splat-film helpers all live in BidirectionalRasterizerBase.

#ifdef RISE_ENABLE_OPENPGL
			mutable PathGuidingField*	pGuidingField;	///< Learned radiance distribution for eye subpath guided sampling
			mutable PathGuidingField*	pLightGuidingField;	///< Separate field for light subpath guided sampling (Option B)
			mutable CompletePathGuide*	pCompletePathGuide;	///< Experimental BDPT complete-path recorder
			mutable Scalar				guidingAlphaScale;
#endif
			PathGuidingConfig		guidingConfig;		///< Path guiding configuration

			virtual ~BDPTRasterizerBase();

			/// Per-render setup (DL-458): hand the integrator the
			/// RayCaster's prepared LightSampler, (re)create the t==1
			/// SplatFilm, configure the splat region, reset the adaptive
			/// sample counter and the splat sample divisor.  Runs from
			/// RasterizeScene AND, per frame, from
			/// PixelBasedRasterizerHelper::RenderFrameOfAnimation -- an
			/// animation frame used to skip all of it, so BDPT animation
			/// frames carried no NEE, no light subpaths and no splats.
			/// Requires pCaster->AttachScene to have run.  The Pel /
			/// Spectral subclasses' diamond disambiguators forward here.
			virtual void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

		public:
			BDPTRasterizerBase(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const PathGuidingConfig& guidingCfg,
				const StabilityConfig& stabilityCfg,
				RISE::Implementation::FrameStore* frameStore = nullptr
				);

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;
		};
	}
}

#endif
