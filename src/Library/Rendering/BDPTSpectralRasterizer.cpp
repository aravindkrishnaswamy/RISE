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
#include "../Utilities/Color/SampledWavelengths.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/ZSobolSampler.h"
#include "../Utilities/MortonCode.h"
#include "../Sampling/SobolSequence.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"
#include "ProgressiveFilm.h"

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
	const StabilityConfig& stabilityConfig,
	bool useZSobol_,
	bool useHWSS_
	) :
  PixelBasedRasterizerHelper( pCaster_ ),
  BDPTRasterizerBase( pCaster_, maxEyeDepth, maxLightDepth, smsConfig, guidingConfig, stabilityConfig ),
  PixelBasedSpectralIntegratingRasterizer( pCaster_, lambda_begin_, lambda_end_, num_wavelengths_, spectralSamples, StabilityConfig(), false, useHWSS_ )
{
	useZSobol = useZSobol_;
}

BDPTSpectralRasterizer::~BDPTSpectralRasterizer()
{
}

void BDPTSpectralRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	PixelBasedRasterizerHelper::PrepareRuntimeContext( rc );
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

	// Thread-local scratch — persist allocations across pixel samples,
	// matching the Pel rasterizer's hot-path pattern.
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
	// Non-HWSS single-wavelength NM — pSwlHWSS=nullptr disables the
	// max-over-wavelengths RR gate (only hero wavelength exists here).
	pIntegrator->GenerateLightSubpathNM( pScene, *pCaster, sampler, lightVerts, lightSubpathStarts, nm, rc.random, nullptr );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, nm, nullptr );

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

	Scalar sampleValue = 0;

	// Single-subpath EvaluateAllStrategiesNM call (no branching).
	if( !eyeVerts.empty() )
	{
		std::vector<BDPTIntegrator::ConnectionResultNM> results =
			pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, pScene, *pCaster, camera, nm );

		for( unsigned int r = 0; r < results.size(); r++ )
		{
			const BDPTIntegrator::ConnectionResultNM& cr = results[r];
			if( !cr.valid ) {
				continue;
			}

			Scalar weighted = cr.contribution * cr.misWeight;

			// Clamp per-strategy contribution.  directClamp for s==1,
			// indirectClamp for the rest. 0 = disabled.
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
				XYZPel thisXYZ( 0, 0, 0 );
				if( weighted > 0 && ColorUtils::XYZFromNM( thisXYZ, nm ) ) {
					thisXYZ = thisXYZ * weighted;
					const Scalar fx = cr.rasterPos.x;
					const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - cr.rasterPos.y;
					SplatContributionToFilm( fx, fy,
						RISEPel( thisXYZ.X, thisXYZ.Y, thisXYZ.Z ),
						camera.GetWidth(), camera.GetHeight() );
				}
			}
			else
			{
				sampleValue += weighted;
			}
		}
	}

	// SMS contributions for the single eye subpath.  Double-counting
	// with BDPT's (s==0) strategy is prevented by the matching
	// suppression in BDPTIntegrator::ConnectAndEvaluateNM — see the
	// long comment in the RGB Pel rasterizer for the MIS analysis.
	if( pIntegrator && !eyeVerts.empty() ) {
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
	uint32_t mortonIndex,
	uint32_t log2SPP,
	PixelAOV* pAOV
	) const
{
	XYZPel spectralSum( 0, 0, 0 );

	if( bUseHWSS )
	{
		// HWSS path: generate subpaths ONCE at the hero wavelength,
		// then re-evaluate spectral transport for companion wavelengths
		// using stored vertex geometry.  This shares the expensive
		// ray intersection and subpath construction across wavelengths
		// while correctly adjusting spectral throughput.
		//
		// For each companion wavelength, vertex throughputNM is
		// recomputed by multiplying the hero's throughput by per-vertex
		// BSDF ratios (companion/hero).  Connection evaluation in
		// EvaluateAllStrategiesNM re-evaluates BSDF and emitter
		// radiance at the companion wavelength automatically.
		unsigned int totalActive = 0;

		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			const Scalar u = rc.random.CanonicalRandom();
			SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );

			const Scalar heroNM = swl.HeroLambda();

			// Sobol index for subpath generation (hero wavelength)
			const uint32_t combinedIndex0 = (pixelSampleIndex * nSpectralSamples + ss) * SampledWavelengths::N;
			const uint32_t sampleIndex0 = useZSobol
				? ((mortonIndex << log2SPP) | combinedIndex0)
				: combinedIndex0;

			// Generate camera ray
			Ray cameraRay;
			if( !camera.GenerateRay( rc, cameraRay, ptOnScreen ) ) {
				continue;
			}

			SobolSampler sampler( sampleIndex0, pixelSeed );

			// Generate subpaths ONCE at hero wavelength.  Single
			// subpath each (path-tree branching was excised in
			// 2026-05); companions recompute throughputNM on the
			// shared geometry (safe for non-dispersive materials;
			// dispersive paths already terminate secondaries via
			// HasDispersiveDeltaVertex).  Thread-local scratch —
			// persists capacity across spectral samples and pixel
			// samples to avoid per-call heap traffic.
			static thread_local std::vector<BDPTVertex> lightVerts;
			static thread_local std::vector<BDPTVertex> eyeVerts;
			static thread_local std::vector<uint32_t> lightSubpathStarts;
			static thread_local std::vector<uint32_t> eyeSubpathStarts;
			lightVerts.clear();
			eyeVerts.clear();
			// HWSS: pass &swl so the NM generator uses max-over-
			// wavelengths RR to avoid hero-driven firefly amplification.
			pIntegrator->GenerateLightSubpathNM( pScene, *pCaster, sampler, lightVerts, lightSubpathStarts, heroNM, rc.random, &swl );
			pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, ptOnScreen, pScene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, heroNM, &swl );

			// Extract AOV from hero evaluation of first bundle
			if( ss == 0 && pAOV ) {
				for( unsigned int iv = 1; iv < eyeVerts.size(); iv++ ) {
					const BDPTVertex& v = eyeVerts[iv];
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

			// Evaluate all strategies at hero wavelength.  Single
			// subpath each (no branching) — one EvaluateAllStrategiesNM
			// call.
			{
				Scalar heroValue = 0;
				if( !eyeVerts.empty() )
				{
					std::vector<BDPTIntegrator::ConnectionResultNM> heroResults =
						pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, pScene, *pCaster, camera, heroNM );

					for( unsigned int r = 0; r < heroResults.size(); r++ ) {
						const BDPTIntegrator::ConnectionResultNM& cr = heroResults[r];
						if( !cr.valid ) continue;
						Scalar weighted = cr.contribution * cr.misWeight;
						{
							const Scalar clampVal = (cr.s == 1)
								? BDPTRasterizerBase::stabilityConfig.directClamp
								: BDPTRasterizerBase::stabilityConfig.indirectClamp;
							if( clampVal > 0 && fabs(weighted) > clampVal )
								weighted = (weighted > 0) ? clampVal : -clampVal;
						}
						if( cr.needsSplat && pSplatFilm ) {
							XYZPel splatXYZ( 0, 0, 0 );
							if( weighted > 0 && ColorUtils::XYZFromNM( splatXYZ, heroNM ) ) {
								splatXYZ = splatXYZ * weighted;
								const Scalar fx = cr.rasterPos.x;
								const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - cr.rasterPos.y;
								SplatContributionToFilm( fx, fy,
									RISEPel( splatXYZ.X, splatXYZ.Y, splatXYZ.Z ),
									camera.GetWidth(), camera.GetHeight() );
							}
						} else {
							heroValue += weighted;
						}
					}
				}

				XYZPel heroXYZ( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( heroXYZ, heroNM ) ) {
					spectralSum = spectralSum + heroXYZ * heroValue;
					totalActive++;
				}
			}

			// Check for dispersive delta vertices in either subpath.
			// If any delta vertex has wavelength-dependent IOR,
			// companions cannot share the hero's geometric path.
			for( unsigned int w = 1; w < SampledWavelengths::N && !swl.SecondaryTerminated(); w++ )
			{
				if( BDPTIntegrator::HasDispersiveDeltaVertex( lightVerts, heroNM, swl.lambda[w] ) ||
					BDPTIntegrator::HasDispersiveDeltaVertex( eyeVerts, heroNM, swl.lambda[w] ) )
				{
					swl.TerminateSecondary();
					break;
				}
			}

			// Evaluate companion wavelengths using stored subpaths
			for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) {
					// Still count terminated wavelengths with zero contribution
					XYZPel compXYZ( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( compXYZ, swl.lambda[w] ) )
						totalActive++;
					continue;
				}

				const Scalar companionNM = swl.lambda[w];

				// Single eye + light subpath (no branching).  Copy into
				// thread-local scratch, recompute throughputNM at the
				// companion wavelength, evaluate strategies.  Capacity
				// persists across spectral samples for zero-alloc hot path.
				static thread_local std::vector<BDPTVertex> compEye;
				static thread_local std::vector<BDPTVertex> compLight;
				Scalar compValue = 0;
				if( !eyeVerts.empty() )
				{
					compEye.assign( eyeVerts.begin(), eyeVerts.end() );
					compLight.assign( lightVerts.begin(), lightVerts.end() );

					pIntegrator->RecomputeSubpathThroughputNM(
						compLight, true, heroNM, companionNM, pScene, *pCaster );
					pIntegrator->RecomputeSubpathThroughputNM(
						compEye, false, heroNM, companionNM, pScene, *pCaster );

					std::vector<BDPTIntegrator::ConnectionResultNM> compResults =
						pIntegrator->EvaluateAllStrategiesNM(
							compLight, compEye, pScene, *pCaster, camera, companionNM );

					for( unsigned int r = 0; r < compResults.size(); r++ ) {
						const BDPTIntegrator::ConnectionResultNM& cr = compResults[r];
						if( !cr.valid ) continue;
						Scalar weighted = cr.contribution * cr.misWeight;
						{
							const Scalar clampVal = (cr.s == 1)
								? BDPTRasterizerBase::stabilityConfig.directClamp
								: BDPTRasterizerBase::stabilityConfig.indirectClamp;
							if( clampVal > 0 && fabs(weighted) > clampVal )
								weighted = (weighted > 0) ? clampVal : -clampVal;
						}
						if( cr.needsSplat && pSplatFilm ) {
							XYZPel splatXYZ( 0, 0, 0 );
							if( weighted > 0 && ColorUtils::XYZFromNM( splatXYZ, companionNM ) ) {
								splatXYZ = splatXYZ * weighted;
								const Scalar fx = cr.rasterPos.x;
								const Scalar fy = static_cast<Scalar>( camera.GetHeight() ) - cr.rasterPos.y;
								SplatContributionToFilm( fx, fy,
									RISEPel( splatXYZ.X, splatXYZ.Y, splatXYZ.Z ),
									camera.GetWidth(), camera.GetHeight() );
							}
						} else {
							compValue += weighted;
						}
					}
				}

				XYZPel compXYZ( 0, 0, 0 );
				if( ColorUtils::XYZFromNM( compXYZ, companionNM ) ) {
					spectralSum = spectralSum + compXYZ * compValue;
					totalActive++;
				}
			}

			// SMS contributions at hero wavelength only (SMS geometry
			// is specular-chain dependent and cannot be shared across
			// wavelengths the way EvaluateAllStrategiesNM can via
			// RecomputeSubpathThroughputNM).  Double-counting with
			// BDPT's (s==0) strategy is prevented by the matching
			// suppression in BDPTIntegrator::ConnectAndEvaluateNM.
			if( pIntegrator && !eyeVerts.empty() ) {
				sampler.StartStream( 31 );
				Scalar smsValue = 0;
				std::vector<BDPTIntegrator::ConnectionResultNM> smsResults =
					pIntegrator->EvaluateSMSStrategiesNM(
						eyeVerts, pScene, *pCaster, camera, sampler, heroNM );
				for( unsigned int r = 0; r < smsResults.size(); r++ ) {
					const BDPTIntegrator::ConnectionResultNM& cr = smsResults[r];
					if( !cr.valid ) continue;
					Scalar weighted = cr.contribution * cr.misWeight;
					{
						const Scalar clampVal = BDPTRasterizerBase::stabilityConfig.directClamp;
						if( clampVal > 0 && fabs(weighted) > clampVal )
							weighted = (weighted > 0) ? clampVal : -clampVal;
					}
					smsValue += weighted;
				}
				XYZPel smsXYZ( 0, 0, 0 );
				if( smsValue > 0 && ColorUtils::XYZFromNM( smsXYZ, heroNM ) ) {
					spectralSum = spectralSum + smsXYZ * smsValue;
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
		// Sample a random wavelength
		const Scalar nm = num_wavelengths < 10000 ?
			(lambda_begin + int(rc.random.CanonicalRandom() * Scalar(num_wavelengths)) * wavelength_steps) :
			(lambda_begin + rc.random.CanonicalRandom() * lambda_diff);

		// Each (pixel sample, spectral sample) pair gets a unique Sobol index.
		// The combined index is computed from the raw pixelSampleIndex, then
		// Morton-remapped if ZSobol is active.  This ensures the Morton
		// encoding is applied to the final sample index and is not destroyed
		// by the multiplication with nSpectralSamples.
		const uint32_t combinedIndex = pixelSampleIndex * nSpectralSamples + ss;
		const uint32_t sampleIndex = useZSobol
			? ((mortonIndex << log2SPP) | combinedIndex)
			: combinedIndex;

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
	const bool bMultiSample = pSampling && rc.UsesPixelSampling();
	const unsigned int batchSize = bMultiSample ? pSampling->GetNumSamples() : 1;
	const unsigned int maxSamples = batchSize;

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
	// ZSobol (blue-noise): seed from Morton index.
	// If the Morton-shifted index would overflow uint32_t, fall back
	// to standard Sobol (white-noise) for this pixel.
	uint32_t pixelSeed;
	uint32_t mortonIndex = 0;
	uint32_t log2SPP = 0;
	if( useZSobol &&
		MortonCode::CanEncode2D( static_cast<uint32_t>(x), static_cast<uint32_t>(y) ) )
	{
		const uint32_t mi = MortonCode::Morton2D(
			static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
		// The effective per-pixel sample count includes both pixel
		// samples and spectral samples, since IntegratePixelSpectral
		// derives Sobol indices as (pixelSampleIndex * nSpectralSamples + ss).
		const uint32_t zSobolPixelSPP = rc.totalProgressiveSPP > 0 ? rc.totalProgressiveSPP : maxSamples;
		const uint32_t l2 = MortonCode::Log2Int( MortonCode::RoundUpPow2(
			zSobolPixelSPP * nSpectralSamples ) );
		// Check overflow: if the Morton-shifted index exceeds uint32_t,
		// fall back to standard Sobol (mortonIndex=0, log2SPP=0 gives
		// the identity mapping).
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
	Scalar wMean = 0;
	Scalar wM2 = 0;
	unsigned int wN = 0;

	uint32_t pixelSampleIndex = 0;
	bool converged = false;

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		colAccrued = px.colorSum;
		weights = px.weightSum;
		alphas = px.alphaSum;
		wMean = px.wMean;
		wM2 = px.wM2;
		wN = px.wN;
		pixelSampleIndex = px.sampleIndex;
	}

	const uint32_t targetSamples = pProgFilm && rc.totalProgressiveSPP > 0
		? rc.totalProgressiveSPP
		: maxSamples;
	uint32_t passEndIndex = targetSamples;
	if( pProgFilm ) {
		const uint64_t desiredEnd = static_cast<uint64_t>( pixelSampleIndex ) + static_cast<uint64_t>( batchSize );
		passEndIndex = desiredEnd < targetSamples ? static_cast<uint32_t>( desiredEnd ) : targetSamples;
	}

	while( pixelSampleIndex < passEndIndex && !converged )
	{
		ISampling2D::SamplesList2D samples;
		if( bMultiSample ) {
			pSampling->GenerateSamplePoints( rc.random, samples );
		} else {
			samples.push_back( Point2( 0, 0 ) );
		}

		ISampling2D::SamplesList2D::const_iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n && pixelSampleIndex<passEndIndex; m++, pixelSampleIndex++ )
		{
			Point2 ptOnScreen;
			Scalar weight = 1.0;

			// Uniform sub-pixel jitter for eye-subpath samples.
			// See BDPTPelRasterizer::IntegratePixel for the full
			// rationale — short version: filter.warp here double-filters
			// against SplatFilm::SplatFiltered and blurs the result
			// with wide-support kernels (gaussian/mitchell).
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

			// The Morton remapping is done inside IntegratePixelSpectral per
			// spectral sample, so pass the raw pixelSampleIndex here.

#ifdef RISE_ENABLE_OIDN
			PixelAOV aov;
			const XYZPel sampleXYZ = IntegratePixelSpectral( rc, ptOnScreen, pScene, *pCamera,
				pixelSampleIndex, pixelSeed, mortonIndex, log2SPP, pAOVBuffers ? &aov : 0 );
			if( pAOVBuffers && aov.valid ) {
				pAOVBuffers->AccumulateAlbedo( x, y, aov.albedo, weight );
				pAOVBuffers->AccumulateNormal( x, y, aov.normal, weight );
			}
#else
			const XYZPel sampleXYZ = IntegratePixelSpectral( rc, ptOnScreen, pScene, *pCamera,
				pixelSampleIndex, pixelSeed, mortonIndex, log2SPP, 0 );
#endif
			const RISEPel samplePel( sampleXYZ.X, sampleXYZ.Y, sampleXYZ.Z );

			// Approach C: cross-pixel filter-weighted splat — see
			// BDPTPelRasterizer::IntegratePixel for the rationale.
			if( pFilteredFilm ) {
				pFilteredFilm->Splat(
					ptOnScreen.x,
					static_cast<Scalar>(height) - ptOnScreen.y,
					samplePel,
					*pPixelFilter );
			}

			colAccrued = colAccrued + samplePel * weight;
			alphas += weight;

			if( pProgFilm ) {
				const Scalar lum = ColorMath::MaxValue(samplePel);
				wN++;
				const Scalar delta = lum - wMean;
				wMean += delta / Scalar(wN);
				const Scalar delta2 = lum - wMean;
				wM2 += delta * delta2;
			}
		}

		if( pProgFilm && wN >= 32 )
		{
			const Scalar variance = wM2 / Scalar(wN - 1);
			const Scalar stdError = sqrt( variance / Scalar(wN) );
			const Scalar meanAbs = fabs( wMean );

			if( meanAbs > NEARZERO ) {
				const Scalar confidence = 1.0 - 4.0 / Scalar(wN);
				if( stdError / meanAbs < 0.01 * confidence ) {
					converged = true;
				}
			} else if( wM2 < NEARZERO && wN >= 64 ) {
				converged = true;
			}
		}
	}

#ifdef RISE_ENABLE_OIDN
	if( pAOVBuffers && weights > 0 && !pProgFilm ) {
		pAOVBuffers->Normalize( x, y, 1.0 / weights );
	}
#endif

	if( pProgFilm ) {
		ProgressivePixel& px = pProgFilm->Get( x, y );
		px.colorSum = colAccrued;
		px.weightSum = weights;
		px.alphaSum = alphas;
		px.wMean = wMean;
		px.wM2 = wM2;
		px.wN = wN;
		px.sampleIndex = pixelSampleIndex;
		px.converged = converged;
	}

	if( alphas > 0 ) {
		cret = RISEColor( colAccrued * (1.0 / alphas), alphas / weights );
	}
}
