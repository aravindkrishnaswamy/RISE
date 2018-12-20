//////////////////////////////////////////////////////////////////////
//
//  PixelBasedSpectralIntegratingRasterizerRGB.h - A rasterizer that
//  performs monte carlo sampling over the spectrum, but does
//  its accumulation in RGB with a spectral power distribution
//  supplied by the user
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 23, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXEL_BASED_SPECTRAL_INTEGRATING_RASTERIZER_RGB_
#define PIXEL_BASED_SPECTRAL_INTEGRATING_RASTERIZER_RGB_

#include "../Interfaces/ISampling2D.h"
#include "../Interfaces/IScene.h"
#include "PixelBasedRasterizerHelper.h"

namespace RISE
{
	class Ray;

	namespace Implementation
	{
		class PixelBasedSpectralIntegratingRasterizerRGB : public PixelBasedRasterizerHelper
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
			const bool								bSpectralHalton;
			const bool								bTimeHalton;
			const IFunction1D&						spd_r;
			const IFunction1D&						spd_g;
			const IFunction1D&						spd_b;

			virtual ~PixelBasedSpectralIntegratingRasterizerRGB();

			bool TakeSingleSample( 
				const RASTERIZER_STATE& rast,
				const Ray& ray,
				ColorRGB& c
				) const;	

			bool TakeSingleSample( 
				const RASTERIZER_STATE& rast,
				const Ray& ray,
				RISEPel& c
				) const
			{
				ColorRGB crgb;
				bool bRet = TakeSingleSample( rast, ray, crgb );
				c = RISEPel( crgb.base );
				return bRet;
			};

			void IntegratePixel(
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
			PixelBasedSpectralIntegratingRasterizerRGB( 
				IRayCaster* pCaster_,
				const Scalar lambda_begin_,
				const Scalar lambda_end_,
				const unsigned int num_wavelengths_,
				const unsigned int specsamp,
				const IFunction1D& spd_r_,
				const IFunction1D& spd_g_,
				const IFunction1D& spd_b_,
				const bool bSpectralHalton_,
				const bool bTimeHalton_
				);
		};
	}
}

#endif

