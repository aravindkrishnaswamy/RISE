//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizerContrastAA.h - Uses contrast to subdivide
//    the pixel space for antialiasing.  This will work best for
//    antialiasing edges and should not be used in conjuction with
//    the path tracer as the primary root shader.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 16, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_PEL_RASTERIZER_CONTRAST_AA_H
#define PIXEL_BASED_PEL_RASTERIZER_CONTRAST_AA_H

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "PixelBasedRasterizerHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class PixelBasedPelRasterizerContrastAA : public virtual PixelBasedRasterizerHelper
		{
		private:
			typedef std::vector<int> RegionSamplesList;

			void SingleSamplePixel(
				const RuntimeContext& rc,
				const unsigned int x,
				const unsigned int y,
				const unsigned int height,
				const IScene& pScene,	
				RISEColor& cret
				) const;

			bool TakeOneOfManySamples( 
				const RuntimeContext& rc,
				const Point2& ptsample,
				const unsigned int x, 
				const unsigned int y,
				const unsigned int height,
				const IScene& pScene,
				RISEPel& c,
				Scalar& weight
				) const;

			void DoFinalSubregion(
				const RuntimeContext& rc,
				const Scalar startx,
				const Scalar endx,
				const Scalar starty,
				const Scalar endy,
				const unsigned int x,
				const unsigned int y,
				const unsigned int height,
				const IScene& pScene,
				int& samplesTaken,
				const std::vector<Scalar>& temporalSamples,
				const ISampling2D::SamplesList2D& samples,
				Scalar& weights,
				RISEPel& colAccrued,
				Scalar& alphas,
				RegionSamplesList& subregion,
				const RISEPel& cCenter,
				const Scalar temporal_start,
				const Scalar temporal_exposure
				) const;

		protected:
			const RISEPel threshold;
			const bool bShowSamples;

			virtual ~PixelBasedPelRasterizerContrastAA( );

			inline bool TakeSingleSample( 
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& ray,
				RISEPel& c
				) const
			{
				return pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 );
			}

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
			PixelBasedPelRasterizerContrastAA( 
				IRayCaster* pCaster_, 
				const RISEPel& threshold_,
				const bool bShowSamples_
				);

		};
	}
}

#endif
