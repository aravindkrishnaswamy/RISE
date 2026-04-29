//////////////////////////////////////////////////////////////////////
//
//  VCMSpectralRasterizer.h - Spectral VCM rasterizer.
//
//    Inherits VCM infrastructure from VCMRasterizerBase and the
//    spectral integration loop from PixelBasedSpectralIntegratingRasterizer.
//
//    Step 0 writes a solid color through IntegratePixel so the
//    spectral pipeline (parser -> factory -> dispatch) can be
//    verified end-to-end before algorithmic work lands.  Step 10
//    replaces this with the HWSS-aware VCM evaluation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_SPECTRAL_RASTERIZER_
#define VCM_SPECTRAL_RASTERIZER_

#include "VCMRasterizerBase.h"
#include "PixelBasedSpectralIntegratingRasterizer.h"
#include "../Utilities/AdaptiveSamplingConfig.h"

namespace RISE
{
	namespace Implementation
	{
		class VCMSpectralRasterizer :
			public VCMRasterizerBase,
			public PixelBasedSpectralIntegratingRasterizer
		{
		protected:
			AdaptiveSamplingConfig		adaptiveConfig;

			virtual ~VCMSpectralRasterizer();

			const char* GetProgressTitle() const { return "VCM Spectral Rasterizing: "; }

			Scalar GetSplatSampleScale() const { return bUseHWSS ? static_cast<Scalar>( nSpectralSamples * SampledWavelengths::N ) : static_cast<Scalar>( nSpectralSamples ); }

			/// Override to use VCMRasterizerBase::stabilityConfig
			/// instead of the default from PixelBasedRasterizerHelper.
			void PrepareRuntimeContext( RuntimeContext& rc ) const;

			/// Override to return the adaptive sampling budget when active.
			/// Mirrors VCMPelRasterizer.
			unsigned int GetProgressiveTotalSPP() const;

			/// Diamond-inheritance disambiguation — see the same
			/// override on VCMPelRasterizer for the rationale.
			/// Spectral rasterizers inherit
			/// PixelBasedSpectralIntegratingRasterizer as the Pel
			/// counterpart, which does not itself override
			/// PreRenderSetup, so this wrapper just delegates to
			/// VCMRasterizerBase.
			void PreRenderSetup( const IScene& pScene, const Rect* pRect ) const;

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
			VCMSpectralRasterizer(
				IRayCaster* pCaster_,
				const unsigned int maxEyeDepth,
				const unsigned int maxLightDepth,
				const Scalar lambda_begin,
				const Scalar lambda_end,
				const unsigned int num_wavelengths,
				const unsigned int spectralSamples,
				const Scalar mergeRadius,
				const bool enableVC,
				const bool enableVM,
				const PathGuidingConfig& guidingConfig,
				const AdaptiveSamplingConfig& adaptiveCfg,
				const StabilityConfig& stabilityConfig,
				const bool useZSobol,
				const bool useHWSS
				);
		};
	}
}

#endif
