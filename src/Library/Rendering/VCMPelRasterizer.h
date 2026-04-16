//////////////////////////////////////////////////////////////////////
//
//  VCMPelRasterizer.h - Pel (RGB) VCM rasterizer.
//
//    Inherits VCM infrastructure from VCMRasterizerBase and the
//    standard pixel-based sample loop from PixelBasedPelRasterizer.
//
//    Step 0 overrides IntegratePixel to write a solid RGB color
//    through the pipeline so the scaffolding can be end-to-end
//    verified before any algorithm work lands.  Steps 7-9 replace
//    the body with the real VC and VM evaluation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_PEL_RASTERIZER_
#define VCM_PEL_RASTERIZER_

#include "VCMRasterizerBase.h"
#include "PixelBasedPelRasterizer.h"
#include "../Utilities/AdaptiveSamplingConfig.h"

namespace RISE
{
	namespace Implementation
	{
		class VCMPelRasterizer :
			public VCMRasterizerBase,
			public PixelBasedPelRasterizer
		{
		protected:
			virtual ~VCMPelRasterizer();

			const char* GetProgressTitle() const { return "VCM Rasterizing: "; }

			/// Override to use VCMRasterizerBase::stabilityConfig
			/// instead of the default from PixelBasedPelRasterizer.
			void PrepareRuntimeContext( RuntimeContext& rc ) const;

			/// Override to return the adaptive sampling budget when active.
			unsigned int GetProgressiveTotalSPP() const;

			/// Diamond-inheritance disambiguation: both
			/// VCMRasterizerBase and PixelBasedPelRasterizer
			/// override PixelBasedRasterizerHelper::PreRenderSetup.
			/// Resolve to VCMRasterizerBase's override (the VCM
			/// light pass) but also invoke
			/// PixelBasedPelRasterizer's side effects (OpenPGL
			/// guiding training plumbing) so nothing the base
			/// rasterizer needs is silently skipped.
			void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

			/// Diamond-inheritance disambiguation: forward all three
			/// flush overrides to the VCMRasterizerBase implementations
			/// so the splat film gets composited before output.
			void FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			void FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			void FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const;
			IRasterImage& GetIntermediateOutputImage( IRasterImage& primary ) const;

			void IntegratePixel(
				const RuntimeContext& rc,
				const unsigned int x,
				const unsigned int y,
				const unsigned int height,
				const IScene& pScene,
				RISEColor& cret,
				const bool temporal_samples,
				const Scalar temporal_start,
				const Scalar temporal_exposure
				) const;

		public:
			VCMPelRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const Scalar mergeRadius,
				const bool enableVC,
				const bool enableVM,
				const PathGuidingConfig& guidingConfig,
				const AdaptiveSamplingConfig& adaptiveConfig,
				const StabilityConfig& stabilityConfig,
				const bool useZSobol
				);
		};
	}
}

#endif
