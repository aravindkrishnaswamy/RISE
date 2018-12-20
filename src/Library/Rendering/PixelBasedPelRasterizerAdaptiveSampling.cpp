//////////////////////////////////////////////////////////////////////
//
//  PixelBasedPelRasterizerAdaptiveSampling.cpp - Implements the
//  adaptive kernel pixel tracer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PixelBasedPelRasterizerAdaptiveSampling.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

PixelBasedPelRasterizerAdaptiveSampling::PixelBasedPelRasterizerAdaptiveSampling( 
	const unsigned int maxS,
	const Scalar var,
	const unsigned int numSteps,
	IRayCaster* pCaster_,
	const bool bOutputSamples_
	) : 
  PixelBasedRasterizerHelper( pCaster_ ),
  maxSamples( maxS ),
  variance_threshold( var ),
  nNumKernels( numSteps ),
  bOutputSamples( bOutputSamples_ ),
  dOVMinSamples( 1.0 ),
  dOVMaxSamples( 1.0 )
{
	pAdaptiveKernels = new ISampling2D*[ nNumKernels ];
	GlobalLog()->PrintNew( pAdaptiveKernels, __FILE__, __LINE__, "adaptive kernels" );

	pOVTotalSamples = new Scalar[ nNumKernels ];
	GlobalLog()->PrintNew( pOVTotalSamples, __FILE__, __LINE__, "samples lookup" );

	for( unsigned int i=0; i<nNumKernels; i++ ) {
		pAdaptiveKernels[i] = 0;
		pOVTotalSamples[i] = 0;
	}

	dOVMaxSamples = 1.0 / Scalar(maxSamples);
}

PixelBasedPelRasterizerAdaptiveSampling::~PixelBasedPelRasterizerAdaptiveSampling( )
{
	for( unsigned int i=0; i<nNumKernels; i++ ) {
		safe_release( pAdaptiveKernels[i] );
	}

	GlobalLog()->PrintDelete( pAdaptiveKernels, __FILE__, __LINE__ );
	delete [] pAdaptiveKernels;
	pAdaptiveKernels = 0;

	GlobalLog()->PrintDelete( pOVTotalSamples, __FILE__, __LINE__ );
	delete [] pOVTotalSamples;
	pOVTotalSamples = 0;
}

// Integrates a single pixel
void PixelBasedPelRasterizerAdaptiveSampling::IntegratePixel(
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
	// If we have a sampling object (which specifies the kernel and size of the 
	// minimum amount of sampling we have to do, then do that...
	if( pSampling && rc.pass == RuntimeContext::PASS_NORMAL )
	{
		ISampling2D::SamplesList2D	samples;
		pSampling->GenerateSamplePoints( rc.random, samples );

		RISEColor	colAccrued = RISEColor( 0, 0, 0, 0 );

		// Accrue sum of squares
		RISEColor	sumofSq = RISEColor( 0, 0, 0, 0 );

		Scalar weights = 0;

		ISampling2D::SamplesList2D::const_iterator		m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ )
		{
			Point2		ptOnScreen; 
			const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			weights += weight;
			
			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			Ray ray;
			RasterizerState rast = {x,y};
			if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
				RISEPel		c;
				if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
					const RISEColor cr = RISEColor( c, 1.0 )*weight;
					colAccrued = colAccrued + cr;
					sumofSq = sumofSq + (cr*cr);
				}
			}
		}

		// Now we have the value of the pixel with the minimum amount of sampling, 
		// now we adaptively continue
		Scalar			weightsSum = weights;
		unsigned int	numSamplesTaken = samples.size();
		unsigned int	adaptiveKernelToUse = 0;

		// Compute the variance before we process this kernel, so that we have something to compare it to later
		Scalar	OVweightsSum = 1.0 / weightsSum;
		RISEColor	cLastVariance = (sumofSq - (colAccrued*colAccrued*OVweightsSum)) * (1.0/Scalar(numSamplesTaken-1));
		Scalar		dLastVariance = fabs(ColorMath::MaxValue(cLastVariance.base));

		if( dLastVariance == 0 ) {
			// No variance at all, must be totally empty part of screen
			cret = (bOutputSamples ? RISEColor( 0, 0, 0, 1 ) : (colAccrued*OVweightsSum));
			return;
		}

		for(;;)
		{
			// Divide out by the number of samples
			OVweightsSum = 1.0 / weightsSum;
			RISEColor cCurrent;
			cCurrent.base = colAccrued.base * (colAccrued.a>0?(1.0/colAccrued.a):0);
			cCurrent.a = colAccrued.a*OVweightsSum; 

			// First lets look at the exit conditions:
			
			// If we've already taken the maximum samples, then bail
			if( numSamplesTaken >= maxSamples ) {
				cret = (bOutputSamples ? RISEColor( 1.0, 1.0, 1.0, 1.0 ) : cCurrent);
				return;
			}

			// Lets take more samples
			// Use one of the adaptive kernels
			if( adaptiveKernelToUse >= nNumKernels ) {
				cret = (bOutputSamples ? RISEColor( 1.0, 1.0, 1.0, 1.0 ) : cCurrent);
				return;
			}

			ISampling2D* pSamp = pAdaptiveKernels[adaptiveKernelToUse];

			ISampling2D::SamplesList2D	thissamps;
			pSampling->GenerateSamplePoints( rc.random, thissamps );

			ISampling2D::SamplesList2D::const_iterator		q, r;
			for( q = thissamps.begin(), r = thissamps.end(); q!=r; q++ )
			{
				Point2		ptOnScreen; 

				const Scalar weight = pPixelFilter->warpOnScreen( rc.random, *q, ptOnScreen, x, height-y );
				weightsSum += weight;

				if( temporal_samples ) {
					pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}
				
				Ray ray;
				RasterizerState rast = {x,y};
				if( pScene.GetCamera()->GenerateRay( rc, ray, ptOnScreen ) ) {
					RISEPel		c;
					if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
						const RISEColor cr = RISEColor( c, 1.0 )*weight;
						colAccrued = colAccrued + cr;
						sumofSq = sumofSq + (cr*cr);
					}
				}
			}

			numSamplesTaken += thissamps.size();
			adaptiveKernelToUse++;

			// Compute the variance again, this time
			RISEColor	cVariance = (sumofSq - (colAccrued*colAccrued*OVweightsSum)) * (1.0/Scalar(numSamplesTaken-1));
			Scalar		dVariance = fabs(ColorMath::MaxValue(cVariance.base));

			// Our threshold is for how much the variance has dropped since last time
			if( fabs(dLastVariance-dVariance) <= variance_threshold ) {
				if( bOutputSamples ) {
					double ns = (double)(numSamplesTaken-samples.size())/(double)maxSamples;
					cret = RISEColor( ns, ns, ns, 1.0 );
				} else {
					cret = cCurrent;
				}

				return;
			}

			dLastVariance = dVariance;
		}
	}
	else
	{
		Ray ray;
		RasterizerState rast = {x,y};
		if( pScene.GetCamera()->GenerateRay( rc, ray, Point2(x, height-y) ) ) {
			RISEPel	c;
			if( pCaster->CastRay( rc, rast, ray, c, IRayCaster::RAY_STATE(), 0, 0 ) ) {
				cret = RISEColor(c,1.0);
			} else {
				cret = RISEColor(0,0,0,0);
			}
		}
	}
}

void PixelBasedPelRasterizerAdaptiveSampling::SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ )
{
	// Call the helper first, the do our own work
	PixelBasedRasterizerHelper::SubSampleRays( pSampling_, pPixelFilter_ );

	// Figure out how many samples / jump for adaptive
	unsigned int numSamplesPerStep = (maxSamples - pSampling->GetNumSamples()) / nNumKernels;

	numSamplesPerStep = numSamplesPerStep >= 1 ? numSamplesPerStep : 1;

	unsigned int nSamplesSoFar = pSampling->GetNumSamples();
	dOVMinSamples = 1.0 / Scalar(nSamplesSoFar);

	if( pAdaptiveKernels )
	{
		for( unsigned int i=0; i<nNumKernels; i++ )
		{
			safe_release( pAdaptiveKernels[i] );

			pAdaptiveKernels[i] = pSampling->Clone();
			pAdaptiveKernels[i]->SetNumSamples( numSamplesPerStep );

			nSamplesSoFar += pAdaptiveKernels[i]->GetNumSamples();
			pOVTotalSamples[i] = 1.0 / Scalar( nSamplesSoFar );
		}				
	}
}


