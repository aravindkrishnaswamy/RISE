//////////////////////////////////////////////////////////////////////
//
//  PathTracingPelRasterizer.h - Pure path tracing rasterizer (RGB).
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing,
//    inheriting the standard pixel-based sample loop from
//    PixelBasedPelRasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PATHTRACING_PEL_RASTERIZER_
#define PATHTRACING_PEL_RASTERIZER_

#include "PixelBasedPelRasterizer.h"
#include "AOVBuffers.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/ManifoldSolver.h"
#include <stdint.h>

namespace RISE
{
	namespace Implementation
	{
		class PathTracingIntegrator;

		class PathTracingPelRasterizer :
			public PixelBasedPelRasterizer
		{
		protected:
			virtual ~PathTracingPelRasterizer();

			PathTracingIntegrator*	pIntegrator;

			/// Progressive rendering should run to adaptive_max_samples
			/// when adaptive sampling is enabled.
			unsigned int GetProgressiveTotalSPP() const;

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

			/// Single-sample RGB integration using the iterative integrator.
			/// Optionally populates first-hit AOV data for the denoiser.
			RISEPel IntegratePixelRGB(
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Point2& ptOnScreen,
				const IScene& pScene,
				ISampler& sampler,
				const IRadianceMap* pRadianceMap,
				PixelAOV* pAOV
				) const;

		public:
			PathTracingPelRasterizer(
				IRayCaster* pCaster_,
				const ManifoldSolverConfig& smsConfig,
				const PathGuidingConfig& guidingConfig,
				const AdaptiveSamplingConfig& adaptiveConfig,
				const StabilityConfig& stabilityConfig,
				bool useZSobol_
				);
		};
	}
}

#endif
