//////////////////////////////////////////////////////////////////////
//
//  BDPTPelRasterizer.cpp - Implementation of the Pel (RGB) BDPT
//    rasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BDPTPelRasterizer.h"
#include "../Utilities/IndependentSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

// Maximum per-strategy contribution to prevent fireflies from imperfect
// MIS weights at volumetric vertices (SSS).  The value must be high
// enough to preserve energy for caustic paths through glass where
// individual contributions can legitimately reach several hundred.
static const Scalar BDPT_MAX_CONTRIBUTION = 1000.0;

BDPTPelRasterizer::BDPTPelRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const ManifoldSolverConfig& smsConfig
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig ),
  PixelBasedPelRasterizer( pCaster_ )
{
}

BDPTPelRasterizer::~BDPTPelRasterizer()
{
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelRGB - RGB integration for a single pixel sample.
// Generates subpaths, evaluates all (s,t) strategies, and returns
// the non-splat contribution.  Splats are written to pSplatFilm.
//////////////////////////////////////////////////////////////////////

RISEPel BDPTPelRasterizer::IntegratePixelRGB(
	const RuntimeContext& rc,
	const Point2& ptOnScreen,
	const IScene& pScene,
	const ICamera& camera
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return RISEPel( 0, 0, 0 );
	}

	IndependentSampler* pSampler = new IndependentSampler( rc.random );

	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpath( pScene, *pCaster, *pSampler, lightVerts );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, *pSampler, eyeVerts );

	std::vector<BDPTIntegrator::ConnectionResult> results =
		pIntegrator->EvaluateAllStrategies( lightVerts, eyeVerts, pScene, *pCaster, camera );

	safe_release( pSampler );

	RISEPel sampleColor( 0, 0, 0 );

	for( unsigned int r=0; r<results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResult& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		RISEPel weighted = cr.contribution * cr.misWeight;

		// Clamp per-strategy contribution to prevent fireflies from
		// imperfect MIS weights at volumetric vertices.
		const Scalar maxVal = ColorMath::MaxValue( weighted );
		if( maxVal > BDPT_MAX_CONTRIBUTION ) {
			weighted = weighted * (BDPT_MAX_CONTRIBUTION / maxVal);
		}

		if( cr.needsSplat && pSplatFilm )
		{
			const int sx = static_cast<int>( cr.rasterPos.x );
			const int sy = static_cast<int>( cr.rasterPos.y );

			if( sx >= 0 && sy >= 0 &&
				static_cast<unsigned int>(sx) < camera.GetWidth() &&
				static_cast<unsigned int>(sy) < camera.GetHeight() )
			{
				pSplatFilm->Splat( sx, sy, weighted );
			}
		}
		else
		{
			sampleColor = sampleColor + weighted;
		}
	}

	// SMS contributions for specular caustic chains
	if( pIntegrator ) {
		std::vector<BDPTIntegrator::ConnectionResult> smsResults =
			pIntegrator->EvaluateSMSStrategies(
				eyeVerts, pScene, *pCaster, camera, rc.random );

		for( unsigned int r=0; r<smsResults.size(); r++ ) {
			const BDPTIntegrator::ConnectionResult& cr = smsResults[r];
			if( !cr.valid ) continue;

			RISEPel weighted = cr.contribution * cr.misWeight;
			const Scalar maxVal = ColorMath::MaxValue( weighted );
			if( maxVal > BDPT_MAX_CONTRIBUTION ) {
				weighted = weighted * (BDPT_MAX_CONTRIBUTION / maxVal);
			}
			sampleColor = sampleColor + weighted;
		}
	}

	return sampleColor;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Pel (RGB) pixel integration.
// Sample loop with pixel filtering, calls IntegratePixelRGB per sample.
//////////////////////////////////////////////////////////////////////

void BDPTPelRasterizer::IntegratePixel(
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
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return;
	}

	// Determine how many samples to take
	ISampling2D::SamplesList2D samples;
	bool bMultiSample = false;

	if( pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL ) {
		pSampling->GenerateSamplePoints( rc.random, samples );
		bMultiSample = true;
	} else {
		samples.push_back( Point2( 0, 0 ) );
	}

	RISEPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;
	Scalar alphas = 0;

	ISampling2D::SamplesList2D::const_iterator m, n;
	for( m=samples.begin(), n=samples.end(); m!=n; m++ )
	{
		Point2 ptOnScreen;
		Scalar weight = 1.0;

		if( bMultiSample ) {
			weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
		} else {
			ptOnScreen = Point2( x, height-y );
		}
		weights += weight;

		if( temporal_samples ) {
			pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
		}

		colAccrued = colAccrued + IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera ) * weight;
		alphas += weight;
	}

	if( alphas > 0 ) {
		colAccrued = colAccrued * (1.0 / alphas);
		cret = RISEColor( colAccrued, alphas / weights );
	}
}
