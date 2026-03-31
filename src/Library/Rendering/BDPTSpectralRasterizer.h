//////////////////////////////////////////////////////////////////////
//
//  BDPTSpectralRasterizer.h - Spectral BDPT rasterizer.
//
//    Inherits BDPT infrastructure from BDPTRasterizerBase and
//    spectral wavelength parameters from
//    PixelBasedSpectralIntegratingRasterizer.  Wavelength range
//    and sample count are immutable constructor arguments.
//
//    Each pixel sample generates nSpectralSamples wavelength
//    samples.  Each wavelength runs a complete BDPT evaluation
//    and the scalar results are converted to XYZ via color
//    matching functions.  Splats are also XYZ-converted before
//    being added to the SplatFilm.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_SPECTRAL_RASTERIZER_
#define BDPT_SPECTRAL_RASTERIZER_

#include "BDPTRasterizerBase.h"
#include "PixelBasedSpectralIntegratingRasterizer.h"
#include "../Utilities/Color/CIE_XYZ.h"
#include <stdint.h>

namespace RISE
{
	namespace Implementation
	{
		class BDPTSpectralRasterizer :
			public BDPTRasterizerBase,
			public PixelBasedSpectralIntegratingRasterizer
		{
		protected:
			virtual ~BDPTSpectralRasterizer();

			const char* GetProgressTitle() const { return "BDPT Spectral Rasterizing: "; }

			/// Override to use BDPTRasterizerBase::stabilityConfig instead of
			/// the default from PixelBasedRasterizerHelper.
			void PrepareRuntimeContext( RuntimeContext& rc ) const;

			Scalar GetSplatSampleScale() const { return static_cast<Scalar>( nSpectralSamples ); }

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

			/// Spectral integration for a single pixel sample.
			/// Samples nSpectralSamples wavelengths and returns accumulated XYZ.
			/// Splats are XYZ-converted and written to pSplatFilm.
			/// pixelSampleIndex and pixelSeed drive the Owen-scrambled Sobol sampler.
			/// When pAOV is non-null, extracts first-hit AOV from the first wavelength.
			XYZPel IntegratePixelSpectral(
				const RuntimeContext& rc,
				const Point2& ptOnScreen,
				const IScene& pScene,
				const ICamera& camera,
				uint32_t pixelSampleIndex,
				uint32_t pixelSeed,
				PixelAOV* pAOV
				) const;

			/// Evaluates BDPT at a specific wavelength for a single camera ray.
			/// Returns the scalar radiance estimate.
			/// sampleIndex and pixelSeed drive the Owen-scrambled Sobol sampler.
			/// When pAOV is non-null, extracts first-hit albedo and normal.
			Scalar IntegratePixelNM(
				const RuntimeContext& rc,
				const Point2& ptOnScreen,
				const IScene& pScene,
				const ICamera& camera,
				const Scalar nm,
				uint32_t sampleIndex,
				uint32_t pixelSeed,
				PixelAOV* pAOV
				) const;

		public:
			BDPTSpectralRasterizer(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const Scalar lambda_begin,
				const Scalar lambda_end,
				const unsigned int num_wavelengths,
				const unsigned int spectralSamples,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingConfig,
				const StabilityConfig& stabilityConfig
				);
		};
	}
}

#endif
