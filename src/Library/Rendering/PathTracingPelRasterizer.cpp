//////////////////////////////////////////////////////////////////////
//
//  PathTracingPelRasterizer.cpp - Pure path tracing rasterizer (RGB).
//
//    Bypasses the shader op pipeline entirely.  Uses
//    PathTracingIntegrator for direct iterative path tracing,
//    inheriting the standard pixel-based sample loop from
//    PixelBasedPelRasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingPelRasterizer.h"
#include "../Shaders/PathTracingIntegrator.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "../Interfaces/IScene.h"

using namespace RISE;
using namespace RISE::Implementation;

PathTracingPelRasterizer::PathTracingPelRasterizer(
	IRayCaster* pCaster_,
	const ManifoldSolverConfig& smsConfig,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	bool useZSobol_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  PixelBasedPelRasterizer( pCaster_, guidingConfig, adaptiveConfig, stabilityConfig, useZSobol_ ),
  pIntegrator( 0 )
{
	pIntegrator = new PathTracingIntegrator(
		false,
		smsConfig,
		stabilityConfig
		);
	pIntegrator->addref();
}

PathTracingPelRasterizer::~PathTracingPelRasterizer()
{
	safe_release( pIntegrator );
}

//////////////////////////////////////////////////////////////////////
// IntegratePixelRGB - Single-sample RGB integration.
// Generates a camera ray, constructs a Sobol sampler, and calls
// the iterative integrator.
//////////////////////////////////////////////////////////////////////

RISEPel PathTracingPelRasterizer::IntegratePixelRGB(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Point2& ptOnScreen,
	const IScene& pScene,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	PixelAOV* pAOV
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return RISEPel( 0, 0, 0 );
	}

	Ray cameraRay;
	if( !pCamera->GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return RISEPel( 0, 0, 0 );
	}

	return pIntegrator->IntegrateRay(
		rc, rast, cameraRay, pScene, *pCaster, sampler, pRadianceMap, pAOV );
}

//////////////////////////////////////////////////////////////////////
// IntegratePixel - Pel (RGB) pixel integration.
// Sample loop with pixel filtering, Sobol/ZSobol sampling,
// adaptive convergence, and calls IntegratePixelRGB per sample.
//////////////////////////////////////////////////////////////////////

void PathTracingPelRasterizer::IntegratePixel(
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

	const bool bMultiSample = pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL;

	// Derive a per-pixel seed for Owen scrambling.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample;
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
	const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

	if( useZSobol &&
		MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
	{
		const uint32_t mi = MortonCode::Morton2D(
			static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
		const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2( maxSamples ) );
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
	} else {
		pixelSeed = SobolSequence::HashCombine(
			static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	}

	RISEPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;
	Scalar alphas = 0;

	// Welford online variance state (luminance-based)
	Scalar wMean = 0;
	Scalar wM2 = 0;
	unsigned int wN = 0;

	uint32_t globalSampleIndex = 0;
	bool converged = false;

	while( globalSampleIndex < maxSamples && !converged )
	{
		ISampling2D::SamplesList2D samples;
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n && globalSampleIndex<maxSamples; m++, globalSampleIndex++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;

			if( bMultiSample ) {
				const bool filmMode = (pFilteredFilm != 0);
				if( filmMode ) {
					ptOnScreen = Point2(
						static_cast<Scalar>(x) + (*m).x - 0.5,
						static_cast<Scalar>(height-y) + (*m).y - 0.5 );
					weight = 1.0;
				} else {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
				}
			} else {
				ptOnScreen = Point2( x, height-y );
			}
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			// For ZSobol, remap the sample index via Morton code for
			// blue-noise-distributed index.
			const uint32_t effectiveIndex = useZSobol
				? ((mortonIndex << log2SPP) | globalSampleIndex)
				: globalSampleIndex;

			SobolSampler stdSampler( effectiveIndex, pixelSeed );
			ZSobolSampler zSampler( effectiveIndex, mortonIndex, log2SPP, pixelSeed );
			ISampler& sampler = useZSobol
				? static_cast<ISampler&>(zSampler)
				: static_cast<ISampler&>(stdSampler);

			rc.pSampler = &sampler;

#ifdef RISE_ENABLE_OIDN
			PixelAOV aov;
			const RISEPel sampleColor = IntegratePixelRGB(
				rc, rast, ptOnScreen, pScene, sampler, pRadianceMap,
				pAOVBuffers ? &aov : 0 );
			if( pAOVBuffers && aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
			}
#else
			const RISEPel sampleColor = IntegratePixelRGB(
				rc, rast, ptOnScreen, pScene, sampler, pRadianceMap, 0 );
#endif

			if( pFilteredFilm ) {
				pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, sampleColor, *pPixelFilter );
				colAccrued = colAccrued + sampleColor;
				alphas += 1.0;
			} else {
				colAccrued = colAccrued + sampleColor * weight;
				alphas += weight;
			}

			// Welford update on luminance
			if( adaptive ) {
				const Scalar lum = ColorMath::MaxValue(sampleColor);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}

			rc.pSampler = 0;
		}

		// Check convergence after enough batches for reliable statistics.
		if( adaptive && globalSampleIndex >= batchSize * 4 && wN >= 16 )
		{
			const Scalar variance = wM2 / Scalar(wN - 1);
			const Scalar stdError = sqrt( variance / Scalar(wN) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
				if( stdError / meanAbs < adaptiveConfig.threshold * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= batchSize * 8 ) {
				converged = true;
			}
		}

		if( !bMultiSample ) {
			break;
		}
	}


#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && alphas > 0 ) {
		pAOVBuffers->Normalize( x, y, 1.0 / alphas );
	}
#endif

	if( adaptive && adaptiveConfig.showMap ) {
		const Scalar t = Scalar(globalSampleIndex) / Scalar(maxSamples);
		cret = RISEColor( RISEPel(t, t, t), 1.0 );
	} else if( alphas > 0 ) {
		colAccrued = colAccrued * (1.0 / alphas);
		cret = RISEColor( colAccrued, alphas / weights );
	}
}
