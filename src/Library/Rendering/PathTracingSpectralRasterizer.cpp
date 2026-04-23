//////////////////////////////////////////////////////////////////////
//
//  PathTracingSpectralRasterizer.cpp - Pure spectral path tracing
//    rasterizer.
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing.
//    Each pixel sample takes nSpectralSamples random wavelengths
//    (or HWSS bundles), converts to XYZ, and accumulates.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingSpectralRasterizer.h"
#include "../Shaders/PathTracingIntegrator.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/Color/SampledWavelengths.h"
#include "../Interfaces/IScene.h"
#include "ProgressiveFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

PathTracingSpectralRasterizer::PathTracingSpectralRasterizer(
	IRayCaster* pCaster_,
	const Scalar lambda_begin_,
	const Scalar lambda_end_,
	const unsigned int num_wavelengths_,
	const unsigned int spectralSamples,
	const ManifoldSolverConfig& smsConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityCfg,
	bool useZSobol_,
	bool useHWSS_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  PixelBasedSpectralIntegratingRasterizer( pCaster_, lambda_begin_, lambda_end_, num_wavelengths_, spectralSamples, stabilityCfg, useZSobol_, useHWSS_ ),
  pIntegrator( 0 ),
  adaptiveConfig( adaptiveConfig ),
  pSMSPhotonMap( 0 ),
  mSMSPhotonCount( smsConfig.enabled ? smsConfig.photonCount : 0 )
{
	pIntegrator = new PathTracingIntegrator(
		smsConfig,
		stabilityCfg
		);
	pIntegrator->addref();
}

PathTracingSpectralRasterizer::~PathTracingSpectralRasterizer()
{
	safe_release( pIntegrator );
	if( pSMSPhotonMap ) {
		delete pSMSPhotonMap;
		pSMSPhotonMap = 0;
	}
}

// Delegates to PixelBasedSpectralIntegratingRasterizer::PreRenderSetup
// for the path guiding training phase before (optionally) building
// the SMS photon map.  See PathTracingPelRasterizer::PreRenderSetup
// for the full rationale — missing the super-class call silently
// disables path guiding.
void PathTracingSpectralRasterizer::PreRenderSetup(
	const IScene& pScene,
	const Rect* pRect
	) const
{
	PixelBasedSpectralIntegratingRasterizer::PreRenderSetup( pScene, pRect );

	if( mSMSPhotonCount == 0 || !pIntegrator ) {
		return;
	}
	ManifoldSolver* pSolver = pIntegrator->GetSolver();
	if( !pSolver ) {
		return;
	}

	if( !pSMSPhotonMap ) {
		pSMSPhotonMap = new SMSPhotonMap();
	}
	const unsigned int stored = pSMSPhotonMap->Build( pScene, mSMSPhotonCount );
	pSolver->SetPhotonMap( stored > 0 ? pSMSPhotonMap : 0 );
}

unsigned int PathTracingSpectralRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelSpectral - spectral integration for a single pixel
// sample.  Samples nSpectralSamples wavelengths (or HWSS bundles),
// converts each to XYZ, and returns the averaged result.
//////////////////////////////////////////////////////////////////////

XYZPel PathTracingSpectralRasterizer::IntegratePixelSpectral(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Point2& ptOnScreen,
	const IScene& pScene,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return XYZPel( 0, 0, 0 );
	}

	Ray cameraRay;
	if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return XYZPel( 0, 0, 0 );
	}

	XYZPel spectralSum( 0, 0, 0 );

	if( bUseHWSS )
	{
		unsigned int totalActive = 0;

		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			const Scalar u = rc.random.CanonicalRandom();
			SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );

			Scalar result[SampledWavelengths::N] = {0};
			pIntegrator->IntegrateRayHWSS(
				rc, rast, cameraRay, swl, pScene, *pCaster, sampler, pRadianceMap, result );

			for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
				if( !swl.terminated[i] ) {
					XYZPel thisNM( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( thisNM, swl.lambda[i] ) ) {
						spectralSum = spectralSum + thisNM * result[i];
						totalActive++;
					}
				}
			}
		}

		if( totalActive > 0 ) {
			return spectralSum * (1.0 / Scalar(totalActive));
		}
		return spectralSum;
	}

	// Standard (non-HWSS) path: independent random wavelengths
	for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
	{
		const Scalar nm = num_wavelengths < 10000 ?
			(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
			(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

		const Scalar nmvalue = pIntegrator->IntegrateRayNM(
			rc, rast, cameraRay, nm, pScene, *pCaster, sampler, pRadianceMap );

		if( nmvalue > 0 ) {
			XYZPel thisNM( 0, 0, 0 );
			if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
				spectralSum = spectralSum + thisNM * nmvalue;
			}
		}
	}

	return spectralSum * (1.0 / Scalar(nSpectralSamples));
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Spectral pixel integration.
// Sample loop with pixel filtering, calls IntegratePixelSpectral
// per sample.
//////////////////////////////////////////////////////////////////////

void PathTracingSpectralRasterizer::IntegratePixel(
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

	RasterizerState rast = {x,y};
	const IRadianceMap* pRadianceMap = pScene.GetGlobalRadianceMap();

	const bool bMultiSample = pSampling && rc.UsesPixelSampling();

	// Derive a per-pixel seed for Owen scrambling.
	uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	if( bMultiSample )
	{
		ProgressiveFilm* pProgFilm = rc.pProgressiveFilm;
		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			if( px.converged ) {
				if( px.alphaSum > 0 ) {
					cret = RISEColor( px.colorSum * (1.0/px.alphaSum), px.alphaSum / px.weightSum );
				}
				return;
			}
		}

		const bool adaptive = adaptiveConfig.maxSamples > 0 && rc.AllowsAdaptiveSampling();
		const unsigned int batchSize = pSampling->GetNumSamples();
		const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;
		const unsigned int zSobolSPP = rc.totalProgressiveSPP > 0 ? rc.totalProgressiveSPP : maxSamples;

		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			const uint32_t mi = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2( zSobolSPP ) );
			if( l2 < 32 &&
				(uint64_t(mi) << l2) < (uint64_t(1) << 32) )
			{
				mortonIndex = mi;
				log2SPP = l2;
				pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
			} else {
				pixelSeed = SobolSequence::HashCombine(
					static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			}
		}

		RISEPel colAccrued( 0, 0, 0 );
		Scalar weights = 0;
		Scalar alphas = 0;
		Scalar wMean = 0;
		Scalar wM2 = 0;
		unsigned int wN = 0;

		uint32_t sampleIndex = 0;
		bool converged = false;

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			colAccrued = px.colorSum;
			weights = px.weightSum;
			alphas = px.alphaSum;
			wMean = px.wMean;
			wM2 = px.wM2;
			wN = px.wN;
			sampleIndex = px.sampleIndex;
		}

		const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
			? rc.totalProgressiveSPP
			: maxSamples;
		uint32_t passEndIndex = targetSamples;
		if( pProgFilm ) {
			const uint64_t desiredEnd = static_cast<uint64_t>( sampleIndex ) + static_cast<uint64_t>( batchSize );
			passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
		}

		while( sampleIndex < passEndIndex && !converged )
		{
			ISampling2D::SamplesList2D samples;
			pSampling->GenerateSamplePoints( rc.random, samples );

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n && sampleIndex<passEndIndex; m++, sampleIndex++ )
			{
				Point2 ptOnScreen;

				const bool filmMode = (pFilteredFilm != 0);
				Scalar weight;
				if( filmMode ) {
					ptOnScreen = Point2(
						static_cast<Scalar>(x) + (*m).x - 0.5,
						static_cast<Scalar>(height-y) + (*m).y - 0.5 );
					weight = 1.0;
				} else if( pPixelFilter ) {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
				} else {
					ptOnScreen = Point2( x, height-y );
					weight = 1.0;
				}
				weights += weight;

				if( temporal_samples ) {
					pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
				}

				const uint32_t effectiveIndex = useZSobol
					? ((mortonIndex << log2SPP) | sampleIndex)
					: sampleIndex;

				SobolSampler stdSampler( effectiveIndex, pixelSeed );
				ZSobolSampler zSampler( effectiveIndex, mortonIndex, log2SPP, pixelSeed );
				ISampler& sampler = useZSobol
					? static_cast<ISampler&>(zSampler)
					: static_cast<ISampler&>(stdSampler);

				rc.pSampler = &sampler;

				const XYZPel sampleXYZ = IntegratePixelSpectral(
					rc, rast, ptOnScreen, pScene, sampler, pRadianceMap );
				const RISEPel samplePel( sampleXYZ.X, sampleXYZ.Y, sampleXYZ.Z );

				if( filmMode ) {
					pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, samplePel, *pPixelFilter );
					colAccrued = colAccrued + samplePel;
					alphas += 1.0;
				} else {
					colAccrued = colAccrued + samplePel * weight;
					alphas += weight;
				}

				if( adaptive || pProgFilm ) {
					const Scalar lum = ColorMath::MaxValue(samplePel);
					wN++;
					const Scalar delta = lum - wMean;
					wMean += delta / Scalar(wN);
					const Scalar delta2 = lum - wMean;
					wM2 += delta * delta2;
				}

				rc.pSampler = 0;
			}

			if( (adaptive || pProgFilm) && wN >= 32 )
			{
				const Scalar variance = wM2 / Scalar(wN - 1);
				const Scalar stdError = sqrt( variance / Scalar(wN) );
				const Scalar meanAbs = fabs( wMean );

				if( meanAbs > NEARZERO ) {
					const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
					const Scalar threshold = adaptiveConfig.maxSamples > 0
						? adaptiveConfig.threshold
						: 0.01;
					if( stdError / meanAbs < threshold * confidence ) {
						converged = true;
					}
				} else if( wM2 < NEARZERO && wN >= 64 ) {
					converged = true;
				}
			}
		}

		if( pProgFilm ) {
			ProgressivePixel& px = pProgFilm->Get( x, y );
			px.colorSum = colAccrued;
			px.weightSum = weights;
			px.alphaSum = alphas;
			px.wMean = wMean;
			px.wM2 = wM2;
			px.wN = wN;
			px.sampleIndex = sampleIndex;
			px.converged = converged;
		}

		if( adaptive && adaptiveConfig.showMap ) {
			const Scalar t = Scalar(sampleIndex) / Scalar(targetSamples);
			cret = RISEColor( RISEPel(t, t, t), 1.0 );
		} else if( alphas > 0 ) {
			cret = RISEColor( colAccrued * (1.0 / alphas), alphas / weights );
		}
	}
	else
	{
		if( useZSobol &&
			MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
		{
			mortonIndex = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			pixelSeed = SobolSequence::HashCombine( mortonIndex, 0u );
		}

		SobolSampler stdSampler( 0, pixelSeed );
		ZSobolSampler zSampler( 0, mortonIndex, 0, pixelSeed );
		ISampler& sampler = useZSobol
			? static_cast<ISampler&>(zSampler)
			: static_cast<ISampler&>(stdSampler);

		rc.pSampler = &sampler;

		const XYZPel sampleXYZ = IntegratePixelSpectral(
			rc, rast, Point2(x, height-y), pScene, sampler, pRadianceMap );

		cret = RISEColor( RISEPel( sampleXYZ.X, sampleXYZ.Y, sampleXYZ.Z ), 1.0 );

		rc.pSampler = 0;
	}
}
