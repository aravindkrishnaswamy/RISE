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
#include "SplatFilm.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/PathGuidingField.h"

namespace RISE
{
	/// First-hit AOV data extracted from an eye subpath sample.
	/// Used to populate the denoiser's auxiliary buffers.
	struct PixelAOV
	{
		RISEPel		albedo;
		Vector3		normal;
		bool		valid;

		PixelAOV() : valid( false ) {}
	};

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

#ifdef RISE_ENABLE_OIDN
			mutable AOVBuffers*		pAOVBuffers;		///< First-hit albedo + normal buffers for OIDN
#endif

#ifdef RISE_ENABLE_OPENPGL
			mutable PathGuidingField*	pGuidingField;	///< Learned radiance distribution for guided sampling
#endif
			PathGuidingConfig		guidingConfig;		///< Path guiding configuration

			virtual ~BDPTRasterizerBase();

			/// Returns a scaling factor for splat film resolution.
			/// Pel returns 1; Spectral returns nSpectralSamples.
			virtual Scalar GetSplatSampleScale() const { return 1.0; }

			/// Returns the progress title string for this rasterizer variant.
			virtual const char* GetProgressTitle() const = 0;

			/// Returns a scratch image with resolved splats composited
			/// on top of the current primary, for progressive display.
			IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

		public:
			BDPTRasterizerBase(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingCfg
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
