//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizerContrastAA.cpp - Implements the 
//    contrast based anti-aliasing pixel pel rasterizer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 16, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedPelRasterizerContrastAA.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedPelRasterizerContrastAA::PixelBasedPelRasterizerContrastAA( 
	IRayCaster* pCaster_,
	const RISEPel& threshold_,
	const bool bShowSamples_
	) : 
  PixelBasedRasterizerHelper( pCaster_ ),
  threshold( threshold_ ),
  bShowSamples( bShowSamples_ )
{
}

PixelBasedPelRasterizerContrastAA::~PixelBasedPelRasterizerContrastAA( )
{
}

void PixelBasedPelRasterizerContrastAA::SingleSamplePixel(
	const RuntimeContext& rc,
	const unsigned int x,
	const unsigned int y,
	const unsigned int height,
	const IScene& pScene,	
	RISEColor& cret
	) const
{
	RasterizerState rast = {x,y};
	RISEPel	c;
	Ray ray;
	if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
		if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
			cret = RISEColor( c, 1.0 );
		}
	}
}

bool PixelBasedPelRasterizerContrastAA::TakeOneOfManySamples( 
	const RuntimeContext& rc,
	const Point2& ptsample,
	const unsigned int x, 
	const unsigned int y,
	const unsigned int height,
	const IScene& pScene,
	RISEPel& c,
	Scalar& weight
	) const
{
	RasterizerState rast = {x,y};
	Point2		ptOnScreen; 
	weight = pPixelFilter->warpOnScreen( rc.random, ptsample, ptOnScreen, x, height-y );

	Ray ray;
	if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
		if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
			return true;
		}
	}

	return false;
}

static inline bool ExcessiveContrast(
	const RISEPel& a, 
	const RISEPel& b, 
	const RISEPel& threshold
	)
{
	for( int i=0; i<3; i++ ) {
		const Scalar maximum = std::max<Scalar>(a[i], b[i] );
		const Scalar minimum = std::min<Scalar>(a[i], b[i] );
		const Scalar diff = maximum - minimum;
		const Scalar sum = maximum + minimum;

		if( sum > NEARZERO && diff/sum > threshold[i] ) {
			return true;
		}
	}

	return false;
}

void PixelBasedPelRasterizerContrastAA::DoFinalSubregion(
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
	) const
{
	if( !subregion.size() ) {
		return;
	}

	// Take a uniform sample from this subregion and do the contrast check
	Scalar weightRegion = 0;
	RISEPel cRegion;

	if( temporalSamples.size() ) {
		pScene.GetAnimator()->EvaluateAtTime( temporal_start + rc.random.CanonicalRandom()*temporal_exposure );
	}

	const bool hitRegion = TakeOneOfManySamples(
					rc,
					Point2((endx-startx)*0.5,(endy-starty)*0.5),
					x, y, height, pScene,
					cRegion, 
					weightRegion
					);

	weights += weightRegion;
	if( hitRegion ) {
		alphas += weightRegion;
		colAccrued = colAccrued + cRegion * weightRegion;
	}

	// Now we can determine if this region needs to continue being super sampled
	if( ExcessiveContrast( cCenter, cRegion, threshold ) )
	{
		// Take all the samples in this region
		for( unsigned int i=0; i<subregion.size(); i++ ) {
			// Sample hasn't been taken yet, so take it
			Scalar weight = 0;
			RISEPel c;

			const bool hit = TakeOneOfManySamples(
							rc,
							samples[subregion[i]],
							x, y, height, pScene,
							c, 
							weight
							);

			samplesTaken++;

			weights += weight;
			if( hit ) {
				alphas += weight;
				colAccrued = colAccrued + c * weight;
			}
		}
	}
}

void PixelBasedPelRasterizerContrastAA::IntegratePixel(
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
	// If we have a sampling object, then we want to sub-sample each pixel, so
	// do that
	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		RISEPel			colAccrued( 0, 0, 0 );

		ISampling2D::SamplesList2D	samples;
		pSampling->GenerateSamplePoints( rc.random, samples );

		Scalar weights = 0;
		Scalar alphas = 0;

		// Note: We assume that a sufficient number of sample points have been set to adequate populate the
		//   specified adaptive level, ie. if we are to do 16 regions, then we better have >> 16 samples, similarily
		//   if we have 64 regions, then we better have >> 64 samples.  It also assumed and recommended that the
		//   user use something like a halton points sequence, stratified, multijittered or nrooks sample points
		//   generator to get proper stratification and optimal results.

		int samplesTaken = 0;

		// List of all the temporal samples 
		std::vector<Scalar> temporalSamples;
		if( temporal_samples ) {
			temporalSamples.resize( samples.size() );
			for( unsigned int i=0; i<samples.size(); i++ ) {
				temporalSamples.push_back( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}
		}

		// Start by subdividing the sample space

		// Contains an index into the master list for each sample in the region
		RegionSamplesList regions2[2][2];						// 4 regions
		RegionSamplesList regions4[4][4];						// 16 regions
		{
			for( unsigned int m=0; m<samples.size(); m++ )
			{
				// Figure out which region each sample is in
				Scalar sy = samples[m].y;
				Scalar sx = samples[m].x;
				if( sy < 0 ) sy = 0;
				if( sx < 0 ) sx = 0;
				if( sy > 1 ) sy = 1;
				if( sx > 1 ) sx = 1;
				regions4[ int(floor(sy*3.9999)) ][ int(floor(sx*3.9999)) ].push_back( m );
				regions2[ int(floor(sy*1.9999)) ][ int(floor(sx*1.9999)) ].push_back( m );
			}
		}

		// Take the sample at the center of the over all region
		bool hitCenter = false;
		Scalar weightCenter = 0;
		RISEPel cCenter;
		{
			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + 0.5*temporal_exposure );
			}

			hitCenter = TakeOneOfManySamples(
							rc,
							Point2(0.5,0.5),
							x, y, height, pScene,
							cCenter, 
							weightCenter
							);

			weights += weightCenter;
			if( hitCenter ) {
				alphas += weightCenter;
				colAccrued = colAccrued + cCenter * weightCenter;
			}
		}

        // Take the four base samples
		{
			// Check the center of each region
			for( int i=0; i<2; i++ ) {
				for( int j=0; j<2; j++ ) {

					bool hitRegion = false;
					Scalar weightRegion = 0;
					RISEPel cRegion;

					const RegionSamplesList& region = regions2[i][j];

					if( !region.size() ) {
						continue;
					}

					if( temporalSamples.size() ) {
						pScene.GetAnimator()->EvaluateAtTime( temporal_start + rc.random.CanonicalRandom()*temporal_exposure );
					}

					const Scalar startx = Scalar(j)/2.0;
					const Scalar endx = Scalar(j+1)/2.0;
					const Scalar starty = Scalar(i)/2.0;
					const Scalar endy = Scalar(i+1)/2.0;

					const Scalar midx = startx + (endx-startx)*0.5;
					const Scalar midy = starty + (endy-starty)*0.5;
                    
					hitRegion = TakeOneOfManySamples(
									rc,
									Point2(midx, midy),
									x, y, height, pScene,
									cRegion, 
									weightRegion
									);

					weights += weightRegion;
					if( hitRegion ) {
						alphas += weightRegion;
						colAccrued = colAccrued + cRegion * weightRegion;
					}

					// Now we can determine if this region needs to continue being super sampled
					if( ExcessiveContrast( cCenter, cRegion, threshold ) ) {
						// Supersample this region
						DoFinalSubregion( rc, startx, midx, starty, midy, x, y, height, pScene, samplesTaken, temporalSamples, samples, weights, colAccrued, alphas, regions4[i*2][j*2], cRegion, temporal_start, temporal_exposure );
						DoFinalSubregion( rc, midx, endx, starty, midy, x, y, height, pScene, samplesTaken, temporalSamples, samples, weights, colAccrued, alphas, regions4[i*2][j*2+1], cRegion, temporal_start, temporal_exposure );
						DoFinalSubregion( rc, startx, midx, midy, endy, x, y, height, pScene, samplesTaken, temporalSamples, samples, weights, colAccrued, alphas, regions4[i*2+1][j*2], cRegion, temporal_start, temporal_exposure );
						DoFinalSubregion( rc, midx, endx, midy, endy, x, y, height, pScene, samplesTaken, temporalSamples, samples, weights, colAccrued, alphas, regions4[i*2+1][j*2+1], cRegion, temporal_start, temporal_exposure );
					}
				}
			}
		}


		if( bShowSamples ) {
			const Scalar ns = Scalar(samplesTaken)/Scalar(samples.size());
			cret = RISEColor( ns, ns, ns, 1.0 );
		} else {
			// Divide out by the number of samples
			colAccrued = colAccrued * (alphas>0?(1.0/alphas):0);
			cret = RISEColor(colAccrued, alphas/weights);
		}
	} else {
		SingleSamplePixel( rc, x, y, height, pScene, cret );
	}
}

