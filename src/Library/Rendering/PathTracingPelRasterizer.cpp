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
#include "../Utilities/Profiling.h"
#include "ProgressiveFilm.h"

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
  pIntegrator( 0 ),
  pSMSPhotonMap( 0 ),
  mSMSPhotonCount( smsConfig.enabled ? smsConfig.photonCount : 0 )
{
	pIntegrator = new PathTracingIntegrator(
		smsConfig,
		stabilityConfig
		);
	pIntegrator->addref();
}

PathTracingPelRasterizer::~PathTracingPelRasterizer()
{
	safe_release( pIntegrator );
	if( pSMSPhotonMap ) {
		delete pSMSPhotonMap;
		pSMSPhotonMap = 0;
	}
}

//////////////////////////////////////////////////////////////////////
// PreRenderSetup — path-guiding training + SMS photon-aided seeding.
//
// Delegates to PixelBasedPelRasterizer::PreRenderSetup for the path
// guiding training phase, then (optionally) builds the SMS photon
// map.  Missing the super-class call was a silent bug: guiding was
// configured, OpenPGL linked, but the training loop never ran, so
// pGuidingField stayed null and IntegratePixelRGB fell back to BRDF
// sampling.
//
// PathTracing rasterizers reach the manifold solver through
// pIntegrator->GetSolver() rather than holding their own
// ManifoldSolver* member.
//////////////////////////////////////////////////////////////////////
void PathTracingPelRasterizer::PreRenderSetup(
	const IScene& pScene,
	const Rect* pRect
	) const
{
	PixelBasedPelRasterizer::PreRenderSetup( pScene, pRect );

	if( !pIntegrator ) {
		return;
	}
	ManifoldSolver* pSolver = pIntegrator->GetSolver();
	if( !pSolver ) {
		return;
	}

	// Cache the scene's specular casters once for the SMS uniform-seeding
	// path (Mitsuba-faithful single-/multi-scatter; consumed in a later
	// implementation phase).  Cheap to do unconditionally — runs once per
	// render and only iterates objects that have a material attached.
	std::vector<const IObject*> casters;
	ManifoldSolver::EnumerateSpecularCasters( pScene, casters );
	pSolver->SetSpecularCasters( std::move( casters ) );

	if( mSMSPhotonCount == 0 ) {
		return;
	}

	if( !pSMSPhotonMap ) {
		pSMSPhotonMap = new SMSPhotonMap();
	}
	const unsigned int stored = pSMSPhotonMap->Build( pScene, mSMSPhotonCount );
	pSolver->SetPhotonMap( stored > 0 ? pSMSPhotonMap : 0 );
}

unsigned int PathTracingPelRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
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

	const bool bMultiSample = pSampling && rc.UsesPixelSampling();

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

	// Derive a per-pixel seed for Owen scrambling.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample && rc.AllowsAdaptiveSampling();
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
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

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		colAccrued = px.colorSum;
		weights = px.weightSum;
		alphas = px.alphaSum;
		wMean = px.wMean;
		wM2 = px.wM2;
		wN = px.wN;
		globalSampleIndex = px.sampleIndex;
	}

	const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
		? rc.totalProgressiveSPP
		: maxSamples;
	uint32_t passEndIndex = targetSamples;
	if( pProgFilm ) {
		const uint64_t desiredEnd = static_cast<uint64_t>( globalSampleIndex ) + static_cast<uint64_t>( batchSize );
		passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
	}

	while( globalSampleIndex < passEndIndex && !converged )
	{
		ISampling2D::SamplesList2D samples;
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n && globalSampleIndex<passEndIndex; m++, globalSampleIndex++ )
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
				} else if( pPixelFilter ) {
					weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
				} else {
					ptOnScreen = Point2( x, height-y );
					weight = 1.0;
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

			RISE_PROFILE_INC(nSamplesAccumulated);

			if( pFilteredFilm ) {
				pFilteredFilm->Splat( ptOnScreen.x, static_cast<Scalar>(height) - ptOnScreen.y, sampleColor, *pPixelFilter );
				colAccrued = colAccrued + sampleColor;
				alphas += 1.0;
			} else {
				colAccrued = colAccrued + sampleColor * weight;
				alphas += weight;
			}

			// Welford update on luminance.  Gated on `adaptive` only:
			// see BDPTPelRasterizer for the multi-pass selection-bias
			// rationale — convergence-based termination must NOT fire
			// in non-adaptive progressive mode.
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
		if( adaptive && wN >= 32 )
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

		if( !bMultiSample && !pProgFilm ) {
			break;
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
		px.sampleIndex = globalSampleIndex;
		px.converged = converged;
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && alphas > 0 && !pProgFilm ) {
		pAOVBuffers->Normalize( x, y, 1.0 / alphas );
	}
#endif

	if( adaptive && adaptiveConfig.showMap ) {
		const Scalar t = Scalar(globalSampleIndex) / Scalar(targetSamples);
		cret = RISEColor( RISEPel(t, t, t), 1.0 );
	} else if( alphas > 0 ) {
		colAccrued = colAccrued * (1.0 / alphas);
		cret = RISEColor( colAccrued, alphas / weights );
	}

	RISE_PROFILE_INC(nPixelsResolved);
}
