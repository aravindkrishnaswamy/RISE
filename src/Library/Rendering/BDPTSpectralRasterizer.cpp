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
#include "AOVBuffers.h"
#include "../Utilities/SobolSampler.h"
#include "../Sampling/SobolSequence.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

// Default safety clamp for per-strategy contribution.  Used when no
// user-specified stability clamp is active.  Prevents fireflies from
// imperfect MIS weights at volumetric vertices (SSS).  High enough
// to preserve energy for caustic paths through glass where individual
// contributions can legitimately reach several hundred.


BDPTSpectralRasterizer::BDPTSpectralRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const Scalar lambda_begin_,
	const Scalar lambda_end_,
	const unsigned int num_wavelengths_,
	const unsigned int spectralSamples,
	const ManifoldSolverConfig& smsConfig,
	const PathGuidingConfig& guidingConfig,
	const StabilityConfig& stabilityConfig
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig, guidingConfig, stabilityConfig ),
  PixelBasedSpectralIntegratingRasterizer( pCaster_, lambda_begin_, lambda_end_, num_wavelengths_, spectralSamples, StabilityConfig() )
{
}

BDPTSpectralRasterizer::~BDPTSpectralRasterizer()
{
}

void BDPTSpectralRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// The spectral rasterizer's pixel-based base does not set up
	// stability config, so set it from the authoritative
	// BDPTRasterizerBase copy here.
	const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
	rc.pStabilityConfig = &sc;
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
	const Scalar nm,
	uint32_t sampleIndex,
	uint32_t pixelSeed,
	PixelAOV* pAOV
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return 0;
	}

	SobolSampler sampler( sampleIndex, pixelSeed );

	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpathNM( pScene, *pCaster, sampler, lightVerts, nm, rc.random );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, nm );

	// Extract first-hit AOV data for the denoiser (only on first wavelength)
	if( pAOV ) {
		for( unsigned int i = 1; i < eyeVerts.size(); i++ ) {
			const BDPTVertex& v = eyeVerts[i];
			if( v.type == BDPTVertex::SURFACE && !v.isDelta && v.pMaterial ) {
				pAOV->normal = v.normal;
				pAOV->albedo = RISEPel( 0, 0, 0 );
				if( v.pMaterial->GetBSDF() ) {
					Ray aovRay( Point3Ops::mkPoint3( v.position, v.normal ), -v.normal );
					RayIntersectionGeometric rig( aovRay, nullRasterizerState );
					rig.ptIntersection = v.position;
					rig.vNormal = v.normal;
					rig.onb = v.onb;
					pAOV->albedo = v.pMaterial->GetBSDF()->value( v.normal, rig ) * PI;
				}
				pAOV->valid = true;
				break;
			}
		}
	}

	std::vector<BDPTIntegrator::ConnectionResultNM> results =
		pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, pScene, *pCaster, camera, nm );

	Scalar sampleValue = 0;

	for( unsigned int r = 0; r < results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResultNM& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		Scalar weighted = cr.contribution * cr.misWeight;

		// Clamp per-strategy contribution.  Use directClamp for s==1
		// (direct lighting connections), indirectClamp for others.
		// A value of 0 means disabled (no clamping).
		{
			const Scalar clampVal = (cr.s == 1)
				? BDPTRasterizerBase::stabilityConfig.directClamp
				: BDPTRasterizerBase::stabilityConfig.indirectClamp;
			if( clampVal > 0 && fabs(weighted) > clampVal ) {
				weighted = (weighted > 0) ? clampVal : -clampVal;
			}
		}

		if( cr.needsSplat && pSplatFilm )
		{
			// For spectral splatting, convert the scalar at this wavelength
			// to XYZ before splatting
			XYZPel thisXYZ( 0, 0, 0 );
			if( weighted > 0 && ColorUtils::XYZFromNM( thisXYZ, nm ) ) {
				thisXYZ = thisXYZ * weighted;
				// Rasterize returns screen coordinates where y=0 is the
				// image bottom.  Convert to image buffer y=0 at top.
				const int sx = static_cast<int>( cr.rasterPos.x );
				const int sy = static_cast<int>( camera.GetHeight() - cr.rasterPos.y );

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
		sampler.StartStream( 31 );
		std::vector<BDPTIntegrator::ConnectionResultNM> smsResults =
			pIntegrator->EvaluateSMSStrategiesNM(
				eyeVerts, pScene, *pCaster, camera, sampler, nm );

		for( unsigned int r=0; r<smsResults.size(); r++ ) {
			const BDPTIntegrator::ConnectionResultNM& cr = smsResults[r];
			if( !cr.valid ) continue;

			Scalar weighted = cr.contribution * cr.misWeight;
			{
				const Scalar clampVal = BDPTRasterizerBase::stabilityConfig.directClamp;
				if( clampVal > 0 && fabs(weighted) > clampVal ) {
					weighted = (weighted > 0) ? clampVal : -clampVal;
				}
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
	const ICamera& camera,
	uint32_t pixelSampleIndex,
	uint32_t pixelSeed,
	PixelAOV* pAOV
	) const
{
	XYZPel spectralSum( 0, 0, 0 );

	for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
	{
		// Sample a random wavelength
		const Scalar nm = num_wavelengths < 10000 ?
			(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
			(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

		// Each (pixel sample, spectral sample) pair gets a unique Sobol index
		const uint32_t sampleIndex = pixelSampleIndex * nSpectralSamples + ss;

		// Extract AOV only on the first wavelength sample
		const Scalar nmvalue = IntegratePixelNM( rc, ptOnScreen, pScene, camera, nm,
			sampleIndex, pixelSeed, (ss == 0) ? pAOV : 0 );

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

	// Derive a per-pixel seed for Owen scrambling from pixel coordinates
	const uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x),
		static_cast<uint32_t>(y) );

	XYZPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;

	uint32_t pixelSampleIndex = 0;
	ISampling2D::SamplesList2D::const_iterator m, n;
	for( m=samples.begin(), n=samples.end(); m!=n; m++, pixelSampleIndex++ )
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

#ifdef RISE_ENABLE_OIDN
		PixelAOV aov;
		colAccrued = colAccrued + IntegratePixelSpectral( rc, ptOnScreen, pScene, *pCamera,
			pixelSampleIndex, pixelSeed, pAOVBuffers ? &aov : 0 ) * weight;
		if( pAOVBuffers && aov.valid ) {
			pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
			pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
		}
#else
		colAccrued = colAccrued + IntegratePixelSpectral( rc, ptOnScreen, pScene, *pCamera,
			pixelSampleIndex, pixelSeed, 0 ) * weight;
#endif
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && weights > 0 ) {
		pAOVBuffers->Normalize( x, y, 1.0 / weights );
	}
#endif

	if( weights > 0 ) {
		colAccrued = colAccrued * (1.0 / weights);
		cret = RISEColor( RISEPel( colAccrued.X, colAccrued.Y, colAccrued.Z ), 1.0 );
	}
}
