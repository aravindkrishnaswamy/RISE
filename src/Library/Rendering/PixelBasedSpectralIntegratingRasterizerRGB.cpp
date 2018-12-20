//////////////////////////////////////////////////////////////////////
//
//  PixelBasedSpectralIntegratingRasterizerRGB.cpp - Implementation of the 
//    PixelBasedSpectralIntegratingRasterizerRGB class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 23, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedSpectralIntegratingRasterizerRGB.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedSpectralIntegratingRasterizerRGB::PixelBasedSpectralIntegratingRasterizerRGB( 
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
		) : 
  PixelBasedRasterizerHelper( pCaster_ ),
	  lambda_begin( lambda_begin_ ),
	  lambda_end( lambda_end_ ),
	  lambda_diff( lambda_end_-lambda_begin_ ),
	  num_wavelengths( num_wavelengths_ ),
	  wavelength_steps( (lambda_diff)/Scalar(num_wavelengths) ),
	  nSpectralSamples( specsamp ),
	  bSpectralHalton( bSpectralHalton_ ),
	  bTimeHalton( bTimeHalton_ ),
	  spd_r( spd_r_ ),
	  spd_g( spd_g_ ),
	  spd_b( spd_b_ )
{
	vecSpectralSamples.reserve( nSpectralSamples );
	spd_r.addref();
	spd_g.addref();
	spd_b.addref();
}

PixelBasedSpectralIntegratingRasterizerRGB::~PixelBasedSpectralIntegratingRasterizerRGB( )
{
	spd_r.release();
	spd_g.release();
	spd_b.release();
}

bool PixelBasedSpectralIntegratingRasterizerRGB::TakeSingleSample( 
	const RASTERIZER_STATE& rast,
	const Ray& ray,
	ColorRGB& c
	) const
{
	bool bHit = false;
	c = ColorRGB(0,0,0,0);

	if( nSpectralSamples == 1 )
	{
		// Special case if only one spectral sample
		const Scalar nm = num_wavelengths < 10000 ? 
				(lambda_begin + int((bSpectralHalton?mh.next(1):random.CanonicalRandom())*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + (bSpectralHalton?mh.next(1):random.CanonicalRandom()) * (lambda_diff));
		Scalar nmvalue = 0;
		bHit = pCaster->CastRayNM( rast, ray, nmvalue, IRayCaster::RAY_STATE(), nm, 0, 0 );

		if( bHit && nmvalue > 0 ) {
			RGBPel thisNM( 0, 0, 0 );
			ColorUtils::RGBFromNM( thisNM, nm, spd_r, spd_g, spd_b );
			thisNM = thisNM * nmvalue;
			c = ColorRGB( thisNM, 1.0 );
		} else if( bHit ) {
			c.a = 1.0;
		}
	}
	else 
	{
		vecSpectralSamples.clear();

		// Take n samples
		for( unsigned int i=0; i<nSpectralSamples; i++ )
		{
			const Scalar nm = num_wavelengths < 10000 ? 
				(lambda_begin + int((bSpectralHalton?mh.next(1):random.CanonicalRandom())*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + (bSpectralHalton?mh.next(1):random.CanonicalRandom()) * (lambda_diff));

			// Now shoot a ray into the scene at that wavelength, and accrue the values
			SPECTRAL_SAMPLE	samp;
			samp.nm = nm;

			bool bThisHit = pCaster->CastRayNM( rast, ray, samp.value, IRayCaster::RAY_STATE(), nm, 0, 0 );

			if( bThisHit ) {
				bHit = true;
				vecSpectralSamples.push_back( samp );
			}
		}

		// Now take out spectral samples and convert it to an RGB value
		// For each wavelength, get the RGB value, scale it by the value at that wavelength in the spectral
		// packet, and sum... then divide out the total by the wavelength range...
		RGBPel	sum( 0, 0, 0 );

		std::vector<SPECTRAL_SAMPLE>::const_iterator m, n;

		for( m=vecSpectralSamples.begin(), n=vecSpectralSamples.end(); m!=n; m++ )
		{
			const SPECTRAL_SAMPLE& samp = *m;

			if( samp.value > 0 ) {
				RGBPel thisNM( 0, 0, 0 );
				ColorUtils::RGBFromNM( thisNM, samp.nm, spd_r, spd_g, spd_b );
				thisNM = thisNM * samp.value;
				sum = sum + thisNM;
			}
		}

		sum = sum * (1.0/Scalar(nSpectralSamples));

		if( bHit ) {
			c = ColorRGB( sum, 1.0 );
		}
	}

	return bHit;
}

void PixelBasedSpectralIntegratingRasterizerRGB::IntegratePixel(
		const unsigned int x,
		const unsigned int y, 
		const unsigned int height,
		const IScene& pScene,
		RISEColor& cret,
		const bool temporal_samples,
		const Scalar temporal_start,
		const Scalar temporal_exposure
	) const
{
	RASTERIZER_STATE rast = {x,y};

	// If we have a sampling object, then we want to sub-sample each pixel, so
	// do that
	if( pSampling && pPixelFilter )
	{
		ColorRGB	colAccrued( 0, 0, 0, 0 );

		pSampling->RegenerateSamplePoints();

		const ISampling2D::SamplesList2D&	samples = pSampling->GetSamplePoints();
		ISampling2D::SamplesList2D::const_iterator		m, n;

		Scalar weights = 0;

		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			ColorRGB	c;
			Point2		ptOnScreen; 
			const Scalar weight = pPixelFilter->warpOnScreen( *m, ptOnScreen, x, height-y );
			weights += fabs(weight);

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + ((bTimeHalton?mh.next(0):random.CanonicalRandom())*temporal_exposure) );
			}

			Ray ray;
			if( pScene.GetCamera()->GenerateRay( ray, ptOnScreen ) ) {
				TakeSingleSample( rast, ray, c );
				colAccrued = colAccrued + c*weight;
			}
		}

		// Divide out by the sum of the weights
		cret = RISEColor( colAccrued.base * (1.0/colAccrued.a), colAccrued.a/weights );
	}
	else
	{
		Ray ray;
		if( pScene.GetCamera()->GenerateRay( ray, Point2(x, height-y) ) ) {
			ColorRGB	c;
			TakeSingleSample( rast, ray, c );
			cret = RISEColor( c.base, c.a );
		}
	}
}



