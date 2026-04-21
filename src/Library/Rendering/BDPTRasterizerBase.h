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
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/SMSPhotonMap.h"
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
			BDPTIntegrator*			pIntegrator;
			ManifoldSolver*			pManifoldSolver;

			/// SMS photon-aided seeding store.  Lazily built on the first
			/// PreRenderSetup when smsConfig.photonCount > 0; null when
			/// photon-aided seeding is disabled.  Attached to the manifold
			/// solver after Build() completes so render workers get
			/// read-only access to a frozen kd-tree.
			mutable SMSPhotonMap*	pSMSPhotonMap;
			unsigned int			mSMSPhotonCount;

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

		public:
			BDPTRasterizerBase(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingCfg,
				const StabilityConfig& stabilityCfg
				);

			void RasterizeScene(
				const IScene& pScene,
				const Rect* pRect,
				IRasterizeSequence* pRasterSequence
				) const;

			/// Run the one-time SMS photon-aided seeding pass if enabled.
			/// Invoked from RasterizeScene BEFORE the eye-pass dispatch
			/// so every render worker sees a completed, read-only map.
			///
			/// No-op when smsConfig.photonCount was 0; safe to call repeatedly
			/// — subsequent calls short-circuit once the map is built.
			virtual void PreRenderSetup(
				const IScene& pScene,
				const Rect* pRect
				) const;
		};
	}
}

#endif
