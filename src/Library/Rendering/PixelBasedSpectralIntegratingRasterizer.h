//////////////////////////////////////////////////////////////////////
//
//  PixelBasedSpectralIntegratingRasterizer.h - A rasterizer that
//  performs monte carlo sampling over the spectrum
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 19, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_SPECTRAL_INTEGRATING_RASTERIZER_
#define PIXEL_BASED_SPECTRAL_INTEGRATING_RASTERIZER_

#include "../Interfaces/ISampling2D.h"
#include "../Interfaces/IScene.h"
#include "PixelBasedRasterizerHelper.h"

namespace RISE
{
	class Ray;

	namespace Implementation
	{
		class PixelBasedSpectralIntegratingRasterizer : public PixelBasedRasterizerHelper
		{
		protected:
			struct SPECTRAL_SAMPLE
			{
				Scalar	nm;
				Scalar	value;
			};

			mutable std::vector<SPECTRAL_SAMPLE>	vecSpectralSamples;
			const Scalar							lambda_begin;
			const Scalar							lambda_end;
			const Scalar							lambda_diff;
			const unsigned int						num_wavelengths;
			const Scalar							wavelength_steps;
			const unsigned int						nSpectralSamples;

			virtual ~PixelBasedSpectralIntegratingRasterizer();

			bool TakeSingleSample( 
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& ray,
				ColorXYZ& c
				) const;	

			bool TakeSingleSample( 
				const RuntimeContext& rc,
				const RasterizerState& rast,
				const Ray& ray,
				RISEPel& c
				) const
			{
				ColorXYZ cxyz;
				bool bRet = TakeSingleSample( rc, rast, ray, cxyz );
				c = RISEPel( cxyz.base );
				return bRet;
			};

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
			PixelBasedSpectralIntegratingRasterizer( 
				IRayCaster* pCaster_,
				const Scalar lambda_begin_,
				const Scalar lambda_end_,
				const unsigned int num_wavelengths_,
				const unsigned int specsamp
				);
		};
	}
}

#endif

