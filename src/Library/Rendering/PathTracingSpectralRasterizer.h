//////////////////////////////////////////////////////////////////////
//
//  PathTracingSpectralRasterizer.h - Pure spectral path tracing
//    rasterizer.
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing,
//    inheriting wavelength parameters from
//    PixelBasedSpectralIntegratingRasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_SPECTRAL_RASTERIZER_
#define PATHTRACING_SPECTRAL_RASTERIZER_

#include "PixelBasedSpectralIntegratingRasterizer.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/ManifoldSolver.h"
#include "../Utilities/Color/CIE_XYZ.h"
#include <stdint.h>

namespace RISE
{
	namespace Implementation
	{
		class PathTracingIntegrator;

		class PathTracingSpectralRasterizer :
			public PixelBasedSpectralIntegratingRasterizer
		{
		protected:
			virtual ~PathTracingSpectralRasterizer();

			PathTracingIntegrator*	pIntegrator;

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

			/// Single spectral sample integration using the iterative integrator.
			/// Returns XYZ accumulated over nSpectralSamples wavelengths.
			XYZPel IntegratePixelSpectral(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Point2& ptOnScreen,
				const IScene& pScene,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap
				) const;

		public:
			PathTracingSpectralRasterizer(
				IRayCaster* pCaster_,
				const Scalar lambda_begin,
				const Scalar lambda_end,
				const unsigned int num_wavelengths,
				const unsigned int spectralSamples,
				const ManifoldSolverConfig& smsConfig,
				const AdaptiveSamplingConfig& adaptiveConfig,
				const StabilityConfig& stabilityConfig,
				bool useZSobol_,
				bool useHWSS_
				);
		};
	}
}

#endif
