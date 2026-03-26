//////////////////////////////////////////////////////////////////////
//
//  BDPTSpectralRasterizer.cpp - Implementation of the spectral
//    BDPT rasterizer.
//
//  SPECTRAL RENDERING:
//    Each pixel sample takes nSpectralSamples random wavelength
//    samples.  For each wavelength:
//    - Full BDPT subpath generation and connection at that nm.
//    - Scalar result converted to XYZ via color matching functions.
//    - XYZ contributions accumulated and averaged over wavelengths.
//    The splat film sample count is scaled by nSpectralSamples
//    since each pixel sample produces that many splat contributions.
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
#include "BDPTSpectralRasterizer.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

// Maximum per-strategy contribution to prevent fireflies from imperfect
// MIS weights at volumetric vertices (SSS).  The value must be high
// enough to preserve energy for caustic paths through glass where
// individual contributions can legitimately reach several hundred.
static const Scalar BDPT_MAX_CONTRIBUTION = 1000.0;

BDPTSpectralRasterizer::BDPTSpectralRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const Scalar lambda_begin_,
	const Scalar lambda_end_,
	const unsigned int num_wavelengths_,
	const unsigned int spectralSamples,
	const ManifoldSolverConfig& smsConfig
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig ),
  PixelBasedSpectralIntegratingRasterizer( pCaster_, lambda_begin_, lambda_end_, num_wavelengths_, spectralSamples )
{
}

BDPTSpectralRasterizer::~BDPTSpectralRasterizer()
{
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelNM - evaluates BDPT at a specific wavelength for
// a single camera ray.  Returns the scalar radiance estimate.
//////////////////////////////////////////////////////////////////////

Scalar BDPTSpectralRasterizer::IntegratePixelNM(
	const RuntimeContext& rc,
	const Point2& ptOnScreen,
	const IScene& pScene,
	const ICamera& camera,
	const Scalar nm
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return 0;
	}

	IndependentSampler* pSampler = new IndependentSampler( rc.random );

	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpathNM( pScene, *pCaster, *pSampler, lightVerts, nm );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, pScene, *pCaster, *pSampler, eyeVerts, nm );

	std::vector<BDPTIntegrator::ConnectionResultNM> results =
		pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, pScene, *pCaster, camera, nm );

	safe_release( pSampler );

	Scalar sampleValue = 0;

	for( unsigned int r = 0; r < results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResultNM& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		Scalar weighted = cr.contribution * cr.misWeight;

		// Clamp per-strategy contribution
		if( fabs(weighted) > BDPT_MAX_CONTRIBUTION ) {
			weighted = (weighted > 0) ? BDPT_MAX_CONTRIBUTION : -BDPT_MAX_CONTRIBUTION;
		}

		if( cr.needsSplat && pSplatFilm )
		{
			// For spectral splatting, convert the scalar at this wavelength
			// to XYZ before splatting
			XYZPel thisXYZ( 0, 0, 0 );
			if( weighted > 0 && ColorUtils::XYZFromNM( thisXYZ, nm ) ) {
				thisXYZ = thisXYZ * weighted;
				const int sx = static_cast<int>( cr.rasterPos.x );
				const int sy = static_cast<int>( cr.rasterPos.y );

				if( sx >= 0 && sy >= 0 &&
					static_cast<unsigned int>(sx) < camera.GetWidth() &&
					static_cast<unsigned int>(sy) < camera.GetHeight() )
				{
					pSplatFilm->Splat( sx, sy, RISEPel( thisXYZ.X, thisXYZ.Y, thisXYZ.Z ) );
				}
			}
		}
		else
		{
			sampleValue += weighted;
		}
	}

	// SMS contributions for specular caustic chains (spectral)
	if( pIntegrator ) {
		std::vector<BDPTIntegrator::ConnectionResultNM> smsResults =
			pIntegrator->EvaluateSMSStrategiesNM(
				eyeVerts, pScene, *pCaster, camera, rc.random, nm );

		for( unsigned int r=0; r<smsResults.size(); r++ ) {
			const BDPTIntegrator::ConnectionResultNM& cr = smsResults[r];
			if( !cr.valid ) continue;

			Scalar weighted = cr.contribution * cr.misWeight;
			if( fabs(weighted) > BDPT_MAX_CONTRIBUTION ) {
				weighted = (weighted > 0) ? BDPT_MAX_CONTRIBUTION : -BDPT_MAX_CONTRIBUTION;
			}
			sampleValue += weighted;
		}
	}

	return sampleValue;
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelSpectral - spectral integration for a single pixel
// sample.  Samples nSpectralSamples wavelengths, converts each to
// XYZ, and returns the averaged result.
//////////////////////////////////////////////////////////////////////

XYZPel BDPTSpectralRasterizer::IntegratePixelSpectral(
	const RuntimeContext& rc,
	const Point2& ptOnScreen,
	const IScene& pScene,
	const ICamera& camera
	) const
{
	XYZPel spectralSum( 0, 0, 0 );

	for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
	{
		// Sample a random wavelength
		const Scalar nm = num_wavelengths < 10000 ?
			(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
			(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

		const Scalar nmvalue = IntegratePixelNM( rc, ptOnScreen, pScene, camera, nm );

		if( nmvalue > 0 ) {
			XYZPel thisNM( 0, 0, 0 );
			if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
				thisNM = thisNM * nmvalue;
				spectralSum = spectralSum + thisNM;
			}
		}
	}

	// Average over spectral samples
	return spectralSum * (1.0 / Scalar(nSpectralSamples));
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Spectral pixel integration.
// Sample loop with pixel filtering, calls IntegratePixelSpectral
// per sample, accumulates XYZ.
//////////////////////////////////////////////////////////////////////

void BDPTSpectralRasterizer::IntegratePixel(
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

	XYZPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;

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

		colAccrued = colAccrued + IntegratePixelSpectral( rc, ptOnScreen, pScene, *pCamera ) * weight;
	}

	if( weights > 0 ) {
		colAccrued = colAccrued * (1.0 / weights);
		cret = RISEColor( RISEPel( colAccrued.X, colAccrued.Y, colAccrued.Z ), 1.0 );
	}
}
