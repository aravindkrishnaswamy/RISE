//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizer.h - A class that we can use for
//    pixel based non real time rasterizers.  Uses the core ray
//    casting engine for every pixel sample
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 23, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_PEL_RASTERIZER_
#define PIXEL_BASED_PEL_RASTERIZER_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "PixelBasedRasterizerHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class PixelBasedPelRasterizer : public virtual PixelBasedRasterizerHelper
		{
		protected:
			virtual ~PixelBasedPelRasterizer( );

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
			PixelBasedPelRasterizer( 
				IRayCaster* pCaster_
				);

		};
	}
}

#endif
