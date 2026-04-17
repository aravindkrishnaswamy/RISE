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
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "ProgressiveFilm.h"
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
	const StabilityConfig& stabilityCfg,
	bool useZSobol_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig, guidingConfig, stabilityCfg ),
  PixelBasedPelRasterizer( pCaster_, PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), false ),
  adaptiveConfig( adaptiveCfg )
{
	useZSobol = useZSobol_;
}

BDPTPelRasterizer::~BDPTPelRasterizer()
{
}

unsigned int BDPTPelRasterizer::GetProgressiveTotalSPP() const
{
	if( adaptiveConfig.maxSamples > 0 ) {
		return adaptiveConfig.maxSamples;
	}

	return PixelBasedRasterizerHelper::GetProgressiveTotalSPP();
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

	// Per-thread scratch — eliminates per-sample allocator traffic
	// in the BDPT hot path.
	static thread_local std::vector<BDPTVertex> lightVerts;
	static thread_local std::vector<BDPTVertex> eyeVerts;
	lightVerts.clear();
	eyeVerts.clear();

	pIntegrator->GenerateLightSubpath( pScene, *pCaster, sampler, lightVerts, rc.random );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts );

	// Extract first-hit AOV data for the denoiser.
	// Use the actual first eye-subpath surface vertex (even if delta)
	// so the albedo/normal buffers match what the unidirectional PT
	// produces.  For delta (specular) surfaces like glass, GetBSDF()
	// returns NULL; per OIDN documentation, transparent surfaces
	// should use white albedo (1,1,1) rather than zero, since the
	// beauty signal at those pixels is pure illumination with no
	// surface modulation.  Zero albedo causes OIDN to misinterpret
	// the energy balance, especially for BDPT where light subpath
	// connections can deposit significant energy through glass.
	if( pAOV && eyeVerts.size() > 1 ) {
		const BDPTVertex& v = eyeVerts[1];
		if( v.type == BDPTVertex::SURFACE && v.pMaterial ) {
			pAOV->normal = v.normal;
			if( v.pMaterial->GetBSDF() ) {
				Ray aovRay( Point3Ops::mkPoint3( v.position, v.normal ), -v.normal );
				RayIntersectionGeometric rig( aovRay, nullRasterizerState );
				rig.ptIntersection = v.position;
				rig.vNormal = v.normal;
				rig.onb = v.onb;
				pAOV->albedo = v.pMaterial->GetBSDF()->value( v.normal, rig ) * PI;
			} else {
				// Delta/transparent surface: white albedo per OIDN spec
				pAOV->albedo = RISEPel( 1, 1, 1 );
			}
			pAOV->valid = true;
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

	// SMS contributions for specular caustic chains.
	//
	// SMS results are added directly to the BDPT result without
	// cross-strategy MIS.  This is correct because the path spaces
	// are disjoint: BDPT only connects non-delta vertices, while
	// SMS paths pass exclusively through delta (specular) surfaces.
	// BDPT cannot generate caustic paths through perfect specular
	// geometry, so no double-counting occurs.
	//
	// If SMS is extended to glossy (non-delta) specular materials,
	// cross-MIS with BDPT would be required.  See docs/SMS.md for
	// the full analysis.
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

	// Progressive rendering: check if this pixel has already converged
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

	const bool bMultiSample = pSampling && pPixelFilter && rc.UsesPixelSampling();

	// Derive a per-pixel seed for Owen scrambling.
	// ZSobol (blue-noise): seed from Morton index for spatially
	// correlated scrambles across neighboring pixels.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;

	const bool adaptive = adaptiveConfig.maxSamples > 0 && bMultiSample && rc.AllowsAdaptiveSampling();
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
	const unsigned int maxSamples = adaptive ? adaptiveConfig.maxSamples : batchSize;

	// For ZSobol, compute log2SPP from the full progressive SPP budget
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

	// In progressive mode, restore persistent state from the film.
	RISEPel colAccrued( 0, 0, 0 );
	Scalar weights = 0;
	Scalar alphas = 0;
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

	const uint32_t passStartSampleIndex = globalSampleIndex;
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
				weight = pPixelFilter->warpOnScreen( rc.random, *m, ptOnScreen, x, height-y );
			} else {
				ptOnScreen = Point2( x, height-y );
			}
			weights += weight;

			if( temporal_samples ) {
				pScene.GetAnimator()->EvaluateAtTime( temporal_start + (rc.random.CanonicalRandom()*temporal_exposure) );
			}

			// For ZSobol, remap the sample index via Morton code so that
			// IntegratePixelRGB (which constructs the SobolSampler from
			// these parameters) gets the blue-noise-distributed index.
			const uint32_t effectiveIndex = useZSobol
				? ((mortonIndex << log2SPP) | globalSampleIndex)
				: globalSampleIndex;

#ifdef RISE_ENABLE_OIDN
			PixelAOV aov;
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				effectiveIndex, pixelSeed, pAOVBuffers ? &aov : 0 );
			if( pAOVBuffers && aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
			}
#else
			const RISEPel sampleColor = IntegratePixelRGB( rc, ptOnScreen, pScene, *pCamera,
				effectiveIndex, pixelSeed, 0 );
#endif

			colAccrued = colAccrued + sampleColor * weight;
			alphas += weight;

			// Welford update on luminance of non-splat contribution
			if( adaptive || pProgFilm ) {
				const Scalar lum = ColorMath::MaxValue(sampleColor);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Check convergence after enough cumulative samples
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

		// For non-adaptive single-pass, the single batch covers everything
		if( !bMultiSample && !pProgFilm ) {
			break;
		}
	}

	// Write back persistent state for progressive rendering
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

	// Track total adaptive samples for splat film normalization.
	// Use the delta (samples rendered THIS pass) to avoid double-counting
	// across progressive passes.
	if( adaptive || pProgFilm ) {
		AddAdaptiveSamples( globalSampleIndex - passStartSampleIndex );
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
}
