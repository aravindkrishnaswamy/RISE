//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizer.cpp - Implements the basic pixel based
//  rasterizer which rasterizers RISEColor PELs
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 23, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedPelRasterizer.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedPelRasterizer::PixelBasedPelRasterizer( 
	IRayCaster* pCaster_
	) : 
  PixelBasedRasterizerHelper( pCaster_ )
{
}

PixelBasedPelRasterizer::~PixelBasedPelRasterizer( )
{
}

void PixelBasedPelRasterizer::IntegratePixel(
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
		RISEPel			colAccrued( 0, 0, 0 );

		ISampling2D::SamplesList2D samples;
		pSampling->GenerateSamplePoints(rc.random, samples);

		Scalar weights = 0;
		Scalar alphas = 0;

		ISampling2D::SamplesList2D::const_iterator		m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			RISEPel			c;
			Point2		ptOnScreen; 
			const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			Ray ray;
			if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
				if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
					colAccrued = colAccrued + c*weight;
					alphas += weight;
				}
			}
		}

		// Divide out by the number of samples
		colAccrued = colAccrued * (alphas>0?(1.0/alphas):0);
		cret = RISEColor(colAccrued, alphas/weights);
	}
	else
	{
		RISEPel	c;
		Ray ray;
		if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
			if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
				cret = RISEColor( c, 1.0 );
			}
		}
	}
}

