//////////////////////////////////////////////////////////////////////
//
//  BDPTRasterizer.h - Pel (RGB) BDPT rasterizer.
//
//    Inherits BDPT infrastructure from BDPTRasterizerBase and the
//    standard pixel-based sample loop from PixelBasedPelRasterizer.
//    Overrides IntegratePixel() to perform RGB bidirectional path
//    tracing with splat film accumulation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_PEL_RASTERIZER_
#define BDPT_PEL_RASTERIZER_

#include "BDPTRasterizerBase.h"
#include "PixelBasedPelRasterizer.h"

namespace RISE
{
	namespace Implementation
	{
		class BDPTPelRasterizer :
			public BDPTRasterizerBase,
			public PixelBasedPelRasterizer
		{
		protected:
			virtual ~BDPTPelRasterizer();

			const char* GetProgressTitle() const { return "BDPT Rasterizing: "; }

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

			/// RGB integration for a single pixel sample.
			/// Returns the non-splat contribution; splats are written to pSplatFilm.
			RISEPel IntegratePixelRGB(
				const RuntimeContext& rc,
				const Point2& ptOnScreen,
				const IScene& pScene,
				const ICamera& camera
				) const;

		public:
			BDPTPelRasterizer(
				IRayCaster* pCaster_,
				unsigned int maxEyeDepth,
				unsigned int maxLightDepth,
				const ManifoldSolverConfig& smsConfig
				);
		};
	}
}

#endif
