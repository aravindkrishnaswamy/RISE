//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizerAdaptiveSampling.h - A Class
//  for pixel based rasterizers where rather than using just a static
//  kernel for sampling the image pixel, an adaptive kernel is used
//  The kernel provided by the SubSampleRays functions is used
//  as a minimum sampling amount.  This kernel is cloned, and 
//  the size of the jumps inbetween are detected.  Thus we precompute
//  and store a bunch of kernels around.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_PEL_RASTERIZER_ADAPTIVESAMPLING_
#define PIXEL_BASED_PEL_RASTERIZER_ADAPTIVESAMPLING_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "PixelBasedRasterizerHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class PixelBasedPelRasterizerAdaptiveSampling : public PixelBasedRasterizerHelper
		{
		protected:
			const unsigned int	maxSamples;
			const Scalar		variance_threshold;

			ISampling2D**		pAdaptiveKernels;			// A series of kernels of the same size, to use as we increase the number of samples
			Scalar*				pOVTotalSamples;			// Keep around the one over the total samples so far, since this is easy to precalculate and does per pixel
			const unsigned int	nNumKernels;				// This is the number of steps to take from the base sampling amount to maximum, its also the number of kernels we have

			const bool			bOutputSamples;				// Output the number of samples rather than the image ?

			Scalar				dOVMinSamples;
			Scalar				dOVMaxSamples;

			virtual ~PixelBasedPelRasterizerAdaptiveSampling( );

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
			PixelBasedPelRasterizerAdaptiveSampling( 
				const unsigned int maxS,
				const Scalar var,
				const unsigned int numSteps,
				IRayCaster* pCaster_,
				const bool bOutputSamples_
				);

			// Rasterizer interface implementations
			virtual void AttachToScene( const IScene* ){};		// We don't need to do anything to attach
			virtual void DetachFromScene( const IScene* ){};		// We don't need to do anything to detach

			void SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ );
		};
	}
}

#endif
