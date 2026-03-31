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
#include "AOVBuffers.h"
#include "../Utilities/SobolSampler.h"
#include "../Sampling/SobolSequence.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

// Default safety clamp for per-strategy contribution.  Used when no
// user-specified stability clamp is active.  Prevents fireflies from
// imperfect MIS weights at volumetric vertices (SSS).  High enough
// to preserve energy for caustic paths through glass where individual
// contributions can legitimately reach several hundred.


BDPTPelRasterizer::BDPTPelRasterizer(
	IRayCaster* pCaster_,
	unsigned int maxEyeDepth,
	unsigned int maxLightDepth,
	const ManifoldSolverConfig& smsConfig,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveCfg,
	const StabilityConfig& stabilityCfg
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig, guidingConfig, stabilityCfg ),
  PixelBasedPelRasterizer( pCaster_, PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig() ),
  adaptiveConfig( adaptiveCfg )
{
}

BDPTPelRasterizer::~BDPTPelRasterizer()
{
}

void BDPTPelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// Delegate to PixelBasedPelRasterizer for guiding setup, then
	// override the stability config pointer with the authoritative
	// BDPTRasterizerBase copy (the PixelBasedPelRasterizer copy is
	// default-constructed due to diamond inheritance).
	PixelBasedPelRasterizer::PrepareRuntimeContext( rc );
	const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
	rc.pStabilityConfig = &sc;
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
	const ICamera& camera,
	uint32_t sampleIndex,
	uint32_t pixelSeed,
	PixelAOV* pAOV
	) const
{
	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
		return RISEPel( 0, 0, 0 );
	}

	SobolSampler sampler( sampleIndex, pixelSeed );

	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpath( pScene, *pCaster, sampler, lightVerts );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts );

	// Extract first-hit AOV data for the denoiser
	if( pAOV ) {
		for( unsigned int i = 1; i < eyeVerts.size(); i++ ) {
			const BDPTVertex& v = eyeVerts[i];
			if( v.type == BDPTVertex::SURFACE && !v.isDelta && v.pMaterial ) {
				pAOV->normal = v.normal;
				pAOV->albedo = RISEPel( 0, 0, 0 );
				if( v.pMaterial->GetBSDF() ) {
					// Construct a synthetic intersection for BSDF evaluation
					// at normal incidence to approximate material albedo.
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

	std::vector<BDPTIntegrator::ConnectionResult> results =
		pIntegrator->EvaluateAllStrategies(
			lightVerts,
			eyeVerts,
			pScene,
			*pCaster,
			camera,
			&sampler );

	RISEPel sampleColor( 0, 0, 0 );

	for( unsigned int r=0; r<results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResult& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		RISEPel weighted = cr.contribution * cr.misWeight;

		// Clamp per-strategy contribution.  Use directClamp for s==1
		// (direct lighting connections) and indirectClamp for all
		// other strategies.  A value of 0 means disabled (no clamping).
		{
			const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
			const Scalar clampVal = (cr.s == 1)
				? sc.directClamp
				: sc.indirectClamp;
			if( clampVal > 0 ) {
				const Scalar maxVal = ColorMath::MaxValue( weighted );
				if( maxVal > clampVal ) {
					weighted = weighted * (clampVal / maxVal);
				}
			}
		}

		if( cr.needsSplat && pSplatFilm )
		{
			// Rasterize returns screen coordinates where y=0 is the
			// image bottom (matching ptOnScreen = Point2(x, height-y)).
			// Convert to image buffer coordinates where y=0 is the top.
			const int sx = static_cast<int>( cr.rasterPos.x );
			const int sy = static_cast<int>( camera.GetHeight() - cr.rasterPos.y );

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
		sampler.StartStream( 31 );
		std::vector<BDPTIntegrator::ConnectionResult> smsResults =
			pIntegrator->EvaluateSMSStrategies(
				eyeVerts, pScene, *pCaster, camera, sampler );

		for( unsigned int r=0; r<smsResults.size(); r++ ) {
			const BDPTIntegrator::ConnectionResult& cr = smsResults[r];
			if( !cr.valid ) continue;

			RISEPel weighted = cr.contribution * cr.misWeight;
			{
				const StabilityConfig& sc = BDPTRasterizerBase::stabilityConfig;
				const Scalar clampVal = sc.directClamp;
				if( clampVal > 0 ) {
					const Scalar maxVal = ColorMath::MaxValue( weighted );
					if( maxVal > clampVal ) {
						weighted = weighted * (clampVal / maxVal);
					}
				}
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

	const bool bMultiSample = pSampling && pPixelFilter && rc.pass == RuntimeContext::PASS_NORMAL;

	// Derive a per-pixel seed for Owen scrambling from pixel coordinates
	const uint32_t pixelSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x),
		static_cast<uint32_t>(y) );

	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample;
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
	const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

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
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				globalSampleIndex, pixelSeed, pAOVBuffers ? &aov : 0 );
			if( pAOVBuffers && aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
			}
#else
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				globalSampleIndex, pixelSeed, 0 );
#endif

			colAccrued = colAccrued + sampleColor * weight;
			alphas += weight;

			// Welford update on luminance of non-splat contribution
			if( adaptive ) {
				const Scalar lum = ColorMath::MaxValue(sampleColor);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Check convergence after enough batches for reliable statistics.
		// Require at least 4 batches so the variance estimate has some
		// stability (n >= 16 with typical batch sizes).
		if( adaptive && globalSampleIndex >= batchSize * 4 && wN >= 16 )
		{
			const Scalar variance = wM2 / Scalar(wN - 1);
			const Scalar stdError = sqrt( variance / Scalar(wN) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				// Apply a small-sample confidence correction: scale the
				// threshold down when n is low so we are conservative
				// about early stopping.  The factor approaches 1.0 as
				// n grows and equals ~0.5 at n=16.
				const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
				if( stdError / meanAbs < adaptiveConfig.threshold * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= batchSize * 8 ) {
				// Near-zero pixel: require many more samples before
				// declaring convergence, since rare bright contributions
				// (e.g. caustics, indirect light) might not have appeared
				// yet in a small sample set.
				converged = true;
			}
		}

		// For non-adaptive, the single batch covers everything
		if( !bMultiSample ) {
			break;
		}
	}

	// Track total adaptive samples for splat film normalization
	if( adaptive ) {
		AddAdaptiveSamples( globalSampleIndex );
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
