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
	static thread_local std::vector<uint32_t> lightSubpathStarts;
	static thread_local std::vector<uint32_t> eyeSubpathStarts;
	lightVerts.clear();
	eyeVerts.clear();

	// Single-subpath generation (no per-vertex branching at multi-lobe
	// delta vertices).  Branching was excised in 2026-05; the
	// `subpathStarts` outparam is retained for Phase-2 integrator
	// cleanup but always contains exactly one [0, size) range.
	pIntegrator->GenerateLightSubpath( pScene, *pCaster, sampler, lightVerts, lightSubpathStarts, rc.random );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, eyeSubpathStarts );

	// Extract AOV data for the denoiser.  Walks the eye subpath from
	// the camera until it finds the first non-delta SURFACE vertex
	// (skipping glass / mirror, whose `pScat->isDelta` was recorded
	// per-sample on each vertex's `isDelta` field by GenerateEyeSubpath).
	// This is the BDPT analogue of PathTracingIntegrator's
	// first-non-delta hook — for OIDN-P1-1 v2, see docs/OIDN.md.
	//
	// Rough dielectrics handled correctly: each sample's Fresnel
	// decision sets `isDelta` on the chosen scatter, so per-sample
	// the AOV is recorded at the rough surface or behind it
	// depending on each sample's outcome.  Averaged across samples
	// (via AccumulateAlbedo / AccumulateNormal at the per-sample
	// caller above), this gives a Fresnel-weighted mix that matches
	// the beauty signal.
	//
	// When no non-delta surface is found (whole subpath is glass /
	// mirror), pAOV stays !valid and the caller skips accumulation,
	// which OIDN handles via its empty-aux path.
	if( pAOV ) {
		for( size_t iv = 1; iv < eyeVerts.size(); iv++ ) {
			const BDPTVertex& v = eyeVerts[iv];
			if( v.type == BDPTVertex::SURFACE && !v.isDelta && v.pMaterial ) {
				pAOV->normal = v.normal;
				if( v.pMaterial->GetBSDF() ) {
					// Synthesize the camera-ray RayIntersectionGeometric so
					// the BSDF's albedo() can read the real view direction
					// via rig.ray.Dir() (BDPTVertex doesn't carry a ray).
					const Vector3 viewDir = Vector3Ops::Normalize(
						Vector3Ops::mkVector3( v.position, eyeVerts[0].position ) );
					const Ray cameraRay( eyeVerts[0].position, viewDir );
					RayIntersectionGeometric rig( cameraRay, nullRasterizerState );
					rig.ptIntersection = v.position;
					rig.vNormal = v.normal;
					rig.onb = v.onb;
					pAOV->albedo = v.pMaterial->GetBSDF()->albedo( rig );
				} else {
					pAOV->albedo = RISEPel( 1, 1, 1 );
				}
				pAOV->valid = true;
				break;
			}
		}
	}

	RISEPel sampleColor( 0, 0, 0 );

	// Single-subpath EvaluateAllStrategies call.  No per-branch loop —
	// the integrator emits one eye and (at most) one light subpath
	// since path-tree branching at multi-lobe delta vertices was
	// excised in 2026-05.  Each strategy's MIS weights sum to 1 over
	// the (s,t) pairs for the pair of subpaths.
	if( !eyeVerts.empty() )
	{
		std::vector<BDPTIntegrator::ConnectionResult> results =
			pIntegrator->EvaluateAllStrategies(
				lightVerts,
				eyeVerts,
				pScene,
				*pCaster,
				camera,
				&sampler );

		for( unsigned int r=0; r<results.size(); r++ )
		{
			const BDPTIntegrator::ConnectionResult& cr = results[r];
			if( !cr.valid ) {
				continue;
			}

			RISEPel weighted = cr.contribution * cr.misWeight;

			// Clamp per-strategy contribution.  Use directClamp for s==1
			// (direct lighting connections) and indirectClamp for all
			// other strategies.  A value of 0 means disabled.
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
				const Scalar fx = cr.rasterPos.x;
				const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - cr.rasterPos.y;
				SplatContributionToFilm( fx, fy, weighted,
					camera.GetWidth(), camera.GetHeight() );
			}
			else
			{
				sampleColor = sampleColor + weighted;
			}
		}
	}

	// SMS contributions for specular caustic chains.
	//
	// SMS results are added with SMS's internal misWeight only (no
	// cross-strategy BDPT MIS).  The two estimators DO sample
	// overlapping path spaces — BDPT's (s==0) strategy generates
	// delta-through caustic paths whenever an eye subpath naturally
	// BSDF-samples a delta lobe and terminates at an emitter.
	// Double-counting is prevented upstream in
	// BDPTIntegrator::ConnectAndEvaluate's (s==0) branch, which
	// suppresses emission when the eye subpath has the SMS-reachable
	// topology (non-delta shading point followed by a delta chain
	// to the emitter).  That mirrors PT's
	// `bPassedThroughSpecular && bHadNonSpecularShading` rule.
	//
	// Single eye subpath (no branching) — one SMS evaluation per pixel
	// sample, anchored at the first non-specular eye vertex.
	if( pIntegrator && !eyeVerts.empty() ) {
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

	const bool bMultiSample = pSampling && rc.UsesPixelSampling();

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

			// Uniform sub-pixel jitter for eye-subpath samples.  We
			// deliberately do NOT call pPixelFilter->warpOnScreen here:
			// that was the pre-existing behaviour but it aims rays at
			// screen positions dictated by the filter's importance
			// density (e.g. GaussianPixelFilter::warp lands samples
			// outside the pixel), which then get accumulated into this
			// pixel's bucket — a double-filtering that produces severe
			// blur with wide-support kernels.  Uniform jitter gives the
			// correct Monte-Carlo estimate of the per-pixel integral;
			// light-subpath splats go through SplatContributionToFilm
			// → SplatFilm::SplatFiltered which still uses the filter's
			// EvaluateFilter for cross-pixel reconstruction.
			if( bMultiSample ) {
				ptOnScreen = Point2(
					static_cast<Scalar>(x) + (*m).x - 0.5,
					static_cast<Scalar>(height-y) + (*m).y - 0.5 );
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

			// Approach C: cross-pixel filter-weighted splat.  With a
			// wide-support filter, this spreads every eye-subpath
			// sample across the neighbouring pixels using the filter's
			// EvaluateFilter weights — mirroring PathTracingPelRasterizer.
			// When pFilteredFilm is null (narrow filter or filter off)
			// we fall through to per-pixel box accumulation below.
			if( pFilteredFilm ) {
				pFilteredFilm->Splat(
					ptOnScreen.x,
					static_cast<Scalar>(height) - ptOnScreen.y,
					sampleColor,
					*pPixelFilter );
			}

			colAccrued = colAccrued + sampleColor * weight;
			alphas += weight;

			// Welford update on luminance of non-splat contribution.
			// Gated on `adaptive` only: progressive multi-pass mode
			// MUST NOT fire convergence-based termination, otherwise
			// pixels whose 32-sample empirical variance/mean falls
			// below threshold get frozen at "lucky-low" realizations,
			// while pixels that hit a firefly continue and regress to
			// truth — a selection bias that darkens the image by
			// ~1% per pass on hard scenes.  See bdpt_jewel_vault
			// 1024 SPP unguided test: bias ~5.4% at 32 passes.
			if( adaptive ) {
				const Scalar lum = ColorMath::MaxValue(sampleColor);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		// Check convergence after enough cumulative samples
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
	// across progressive passes.  Gated on `adaptive` only: in non-
	// adaptive progressive mode every pixel takes the full configured
	// SPP, so the static mSplatTotalSamples set in BDPTRasterizerBase::
	// RasterizeScene is the correct splat divisor.  Tracking the live
	// per-pass count made sense only when adaptive convergence could
	// give pixels different sample counts.
	if( adaptive ) {
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
