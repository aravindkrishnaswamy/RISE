//////////////////////////////////////////////////////////////////////
//
//  PixelBasedSpectralIntegratingRasterizer.cpp - Implementation of the 
//    PixelBasedSpectralIntegratingRasterizer class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 19, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedSpectralIntegratingRasterizer.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedSpectralIntegratingRasterizer::PixelBasedSpectralIntegratingRasterizer( 
		IRayCaster* pCaster_,
		const Scalar lambda_begin_,
		const Scalar lambda_end_,
		const unsigned int num_wavelengths_,
		const unsigned int specsamp
		) : 
  PixelBasedRasterizerHelper( pCaster_ ),
	  lambda_begin( lambda_begin_ ),
	  lambda_end( lambda_end_ ),
	  lambda_diff( lambda_end_-lambda_begin_ ),
	  num_wavelengths( num_wavelengths_ ),
	  wavelength_steps( (lambda_diff)/Scalar(num_wavelengths) ),
	  nSpectralSamples( specsamp )
{
	vecSpectralSamples.reserve( nSpectralSamples );
}

PixelBasedSpectralIntegratingRasterizer::~PixelBasedSpectralIntegratingRasterizer( )
{
}

bool PixelBasedSpectralIntegratingRasterizer::TakeSingleSample( 
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	ColorXYZ& c
	) const
{
	bool bHit = false;
	c = ColorXYZ(0,0,0,0);

	if( nSpectralSamples == 1 )
	{
		// Special case if only one spectral sample
		const Scalar nm = num_wavelengths < 10000 ? 
				(lambda_begin + int(rc.random.CanonicalRandom()*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);
		Scalar nmvalue = 0;
		bHit = pCaster->CastRayNM( rc, rast, ray, nmvalue, IRayCaster::RAY_STATE(), nm, 0, 0 );

		if( bHit && nmvalue > 0 ) {
			XYZPel thisNM( 0, 0, 0 );
			if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
				thisNM = thisNM * nmvalue;
				c = ColorXYZ( thisNM, 1.0 );
			}
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
				(lambda_begin + int(rc.random.CanonicalRandom()*Scalar(num_wavelengths)) * wavelength_steps) : 
				(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

			// Now shoot a ray into the scene at that wavelength, and accrue the values
			SPECTRAL_SAMPLE	samp;
			samp.nm = nm;

			bool bThisHit = pCaster->CastRayNM( rc, rast, ray, samp.value, IRayCaster::RAY_STATE(), nm, 0, 0 );

			if( bThisHit ) {
				bHit = true;
				vecSpectralSamples.push_back( samp );
			}
		}

		// Now take out spectral samples and convert it to an XYZ value
		// For each wavelength, get the XYZ value, scale it by the value at that wavelength in the spectral
		// packet, and sum... then divide out the total by the wavelength range...
		XYZPel	sum( 0, 0, 0 );

		std::vector<SPECTRAL_SAMPLE>::const_iterator m, n;

		for( m=vecSpectralSamples.begin(), n=vecSpectralSamples.end(); m!=n; m++ )
		{
			const SPECTRAL_SAMPLE& samp = *m;

			if( samp.value > 0 ) {
				XYZPel thisNM( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( thisNM, samp.nm ) ) {
					thisNM = thisNM * samp.value;
					sum = sum + thisNM;
				}
			}
		}

		sum = sum * (1.0/Scalar(nSpectralSamples));

		if( bHit ) {
			c = ColorXYZ( sum, 1.0 );
		}
	}

	return bHit;
}

void PixelBasedSpectralIntegratingRasterizer::IntegratePixel(
		const RuntimeContext& rc,
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
	RasterizerState rast = {x,y};

	// If we have a sampling object, then we want to sub-sample each pixel, so
	// do that
	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		ColorXYZ	colAccrued( 0, 0, 0, 0 );

		ISampling2D::SamplesList2D	samples;
		pSampling->GenerateSamplePoints( rc.random, samples );

		Scalar weights = 0;

		ISampling2D::SamplesList2D::const_iterator		m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			ColorXYZ	c;
			Point2		ptOnScreen; 
			const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			Ray ray;
			if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
				TakeSingleSample( rc, rast, ray, c );
				colAccrued = colAccrued + c*weight;
			}
		}

		// Divide out by the number of samples
		cret = RISEColor( colAccrued.base * (1.0/colAccrued.a), colAccrued.a/weights );
	}
	else
	{
		Ray ray;
		if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
			ColorXYZ	c;
			TakeSingleSample( rc, rast, ray, c );
			cret = RISEColor( c.base, c.a );
		}
	}
}



