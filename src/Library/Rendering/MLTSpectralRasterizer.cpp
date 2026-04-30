//////////////////////////////////////////////////////////////////////
//
//  MLTSpectralRasterizer.cpp - Spectral PSSMLT implementation.
//
//  ALGORITHM OVERVIEW:
//    Extends the PSSMLT pipeline from MLTRasterizer with spectral
//    wavelength integration:
//
//    Each EvaluateSampleSpectral call:
//    1. Consumes a wavelength coordinate from the PSSMLTSampler
//       (in primary sample space, so mutations explore wavelength)
//    2. If HWSS: creates 4 equidistant wavelengths via
//       SampledWavelengths; otherwise uses a single wavelength
//    3. For each wavelength: generates BDPT subpaths via NM methods,
//       evaluates all (s,t) strategies, collects contributions
//    4. Converts each wavelength's contribution to CIE XYZ, weighted
//       by the CIE color matching functions and 1/pdf
//    5. Aggregates per-strategy XYZ contributions across wavelengths
//    6. Computes luminance as Y component for Metropolis acceptance
//
//    HWSS SPECTRAL STRATIFICATION:
//    With HWSS, a single uniform sample u generates 4 equidistantly
//    spaced wavelengths spanning [lambda_begin, lambda_end].  Each
//    wavelength runs an independent BDPT evaluation (equidistant
//    stratification without path sharing).  This gives ~4x spectral
//    variance reduction at ~15% overhead.  When nSpectralSamples > 1,
//    each spectral sample generates its own HWSS bundle.
//
//    XYZ SPLATTING:
//    The SplatFilm accumulates XYZ values stored as RISEPel(X,Y,Z).
//    The rasterizer output chain converts XYZ → working color space.
//    This matches BDPTSpectralRasterizer's approach.
//
//    RENDERING PIPELINE:
//    RasterizeScene overrides the base MLTRasterizer to replace
//    EvaluateSample calls with EvaluateSampleSpectral.  All other
//    infrastructure (bootstrap, chain init, round-based execution,
//    splatting math) is inherited unchanged.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 9, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MLTSpectralRasterizer.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../RasterImages/RasterImage.h"
#include "../Utilities/Profiling.h"
#include "../Utilities/RTime.h"
#include "../Utilities/ThreadPool.h"
#include "ThreadLocalSplatBuffer.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/Threads/Threads.h"
#include <atomic>

#ifdef RISE_ENABLE_OIDN
#include "AOVBuffers.h"
#include "OIDNDenoiser.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Constructor / Destructor
//////////////////////////////////////////////////////////////////////

MLTSpectralRasterizer::MLTSpectralRasterizer(
	IRayCaster* pCaster_,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const unsigned int nBootstrap_,
	const unsigned int nChains_,
	const unsigned int nMutationsPerPixel_,
	const Scalar largeStepProb_,
	const Scalar lambda_begin_,
	const Scalar lambda_end_,
	const unsigned int nSpectralSamples_,
	const bool useHWSS_
	) :
  MLTRasterizer( pCaster_, maxEyeDepth, maxLightDepth, nBootstrap_, nChains_, nMutationsPerPixel_, largeStepProb_ ),
  lambda_begin( lambda_begin_ ),
  lambda_end( lambda_end_ ),
  nSpectralSamples( nSpectralSamples_ ),
  bUseHWSS( useHWSS_ )
{
}

MLTSpectralRasterizer::~MLTSpectralRasterizer()
{
}

//////////////////////////////////////////////////////////////////////
// RunChainSegmentSpectral - Run a fixed number of spectral mutations
// on an existing chain.  Mirrors RunChainSegment but calls
// EvaluateSampleSpectral instead of EvaluateSample.
//////////////////////////////////////////////////////////////////////

void MLTSpectralRasterizer::RunChainSegmentSpectral(
	ChainState& state,
	const IScene& scene,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const unsigned int numMutations,
	const Scalar normalization,
	const unsigned int width,
	const unsigned int height
	) const
{
	const Scalar totalMutations = static_cast<Scalar>( nMutationsPerPixel ) *
		static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar invTotalMutations = 1.0 / totalMutations;

	for( unsigned int m = 0; m < numMutations; m++ )
	{
		// Propose mutation
		state.pSampler->StartIteration();
		MLTSample proposedSample = EvaluateSampleSpectral( scene, camera, *state.pSampler, width, height );

		// Acceptance probability
		Scalar accept = 0;
		if( state.currentSample.luminance > 0 ) {
			accept = proposedSample.luminance / state.currentSample.luminance;
			if( accept > 1.0 ) {
				accept = 1.0;
			}
		} else if( proposedSample.luminance > 0 ) {
			accept = 1.0;
		}

		// Splat proposed with acceptance weight
		if( proposedSample.valid && proposedSample.luminance > 0 )
		{
			const Scalar proposedWeight = accept * invTotalMutations * normalization / proposedSample.luminance;

			for( unsigned int k = 0; k < proposedSample.splats.size(); k++ )
			{
				const MLTStrategySplat& s = proposedSample.splats[k];
				const RISEPel splatColor = s.color * proposedWeight;

				if( pPixelFilter ) {
					// Distribute across the filter footprint at the
					// fractional raster position — see MLTRasterizer
					// for the detailed rationale.
					splatFilm.SplatFiltered( s.rasterPos.x, s.rasterPos.y,
						splatColor, *pPixelFilter );
				} else {
					// No filter: round-to-nearest point splat.
					const Scalar rx = s.rasterPos.x + static_cast<Scalar>( 0.5 );
					const Scalar ry = s.rasterPos.y + static_cast<Scalar>( 0.5 );
					if( rx >= 0 && ry >= 0 ) {
						const unsigned int sx = static_cast<unsigned int>( rx );
						const unsigned int sy = static_cast<unsigned int>( ry );
						if( sx < width && sy < height ) {
							splatFilm.Splat( sx, sy, splatColor );
						}
					}
				}
			}
		}

		// Splat current with (1-accept) weight
		if( state.currentSample.valid && state.currentSample.luminance > 0 )
		{
			const Scalar currentWeight = ( 1.0 - accept ) * invTotalMutations * normalization / state.currentSample.luminance;

			for( unsigned int k = 0; k < state.currentSample.splats.size(); k++ )
			{
				const MLTStrategySplat& s = state.currentSample.splats[k];
				const RISEPel splatColor = s.color * currentWeight;

				if( pPixelFilter ) {
					splatFilm.SplatFiltered( s.rasterPos.x, s.rasterPos.y,
						splatColor, *pPixelFilter );
				} else {
					const Scalar rx = s.rasterPos.x + static_cast<Scalar>( 0.5 );
					const Scalar ry = s.rasterPos.y + static_cast<Scalar>( 0.5 );
					if( rx >= 0 && ry >= 0 ) {
						const unsigned int sx = static_cast<unsigned int>( rx );
						const unsigned int sy = static_cast<unsigned int>( ry );
						if( sx < width && sy < height ) {
							splatFilm.Splat( sx, sy, splatColor );
						}
					}
				}
			}
		}

		// Accept or reject
		if( state.chainRNG.CanonicalRandom() < accept )
		{
			state.pSampler->Accept();
			state.currentSample = proposedSample;
		}
		else
		{
			state.pSampler->Reject();
		}
	}
}

//////////////////////////////////////////////////////////////////////
// SpectralRoundThread_ThreadProc - Thread entry point for parallel
// spectral chain execution.
//////////////////////////////////////////////////////////////////////

void* MLTSpectralRasterizer::SpectralRoundThread_ThreadProc( void* lpParameter )
{
	SpectralRoundThreadData* pData = (SpectralRoundThreadData*)lpParameter;

	for( unsigned int c = pData->chainStart; c < pData->chainEnd; c++ )
	{
		pData->pRasterizer->RunChainSegmentSpectral(
			pData->pChainStates[c],
			*pData->pScene,
			*pData->pScene->GetCamera(),
			*pData->pSplatFilm,
			pData->mutationsPerChain,
			pData->normalization,
			pData->width,
			pData->height
		);
	}

	return 0;
}

//////////////////////////////////////////////////////////////////////
// EvaluateSingleWavelength - Evaluates BDPT at one wavelength.
//
// Generates light and eye subpaths using the NM methods, evaluates
// all (s,t) connection strategies, and returns the results.
// The sampler streams are managed by the caller.
//////////////////////////////////////////////////////////////////////

void MLTSpectralRasterizer::EvaluateSingleWavelength(
	const IScene& scene,
	const ICamera& camera,
	ISampler& sampler,
	const unsigned int width,
	const unsigned int height,
	const Ray& cameraRay,
	const Point2& screenPos,
	const Point2& cameraRasterPos,
	const Scalar nm,
	const RuntimeContext& rc,
	std::vector<BDPTIntegrator::ConnectionResultNM>& results,
	std::vector<BDPTVertex>& lightVerts,
	std::vector<BDPTVertex>& eyeVerts
	) const
{
	lightVerts.clear();
	eyeVerts.clear();
	std::vector<uint32_t> lightSubpathStarts;
	std::vector<uint32_t> eyeSubpathStarts;

	// MLT's Markov-chain proposal measure assumes a single subpath —
	// force threshold=1.0 on both sides to keep the NM generators
	// emitting single-branch output (matches RGB MLTRasterizer).
	// Single-wavelength MLT path: pSwlHWSS = nullptr.
	pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, lightSubpathStarts, nm, rc.random, Scalar( 1.0 ), nullptr );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, nm, Scalar( 1.0 ), nullptr );

	results = pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, scene, *pCaster, camera, nm );
}

//////////////////////////////////////////////////////////////////////
// EvaluateSampleSpectral - Spectral bridge between MLT and BDPT.
//
// Consumes samples from the PSSMLTSampler to:
// 1. Pick a film position (stream 0)
// 2. Sample wavelength(s) (stream 0, after film position)
// 3. For each wavelength: generate BDPT NM subpaths and evaluate
//    all strategies (streams 1, 2)
// 4. Convert per-strategy contributions to XYZ
// 5. Aggregate across wavelengths
//
// For HWSS: one uniform sample generates 4 equidistant wavelengths.
// For standard: nSpectralSamples independent wavelengths.
//
// The aggregate luminance (Y component of total XYZ) is used for
// the Metropolis acceptance ratio.
//////////////////////////////////////////////////////////////////////

MLTRasterizer::MLTSample MLTSpectralRasterizer::EvaluateSampleSpectral(
	const IScene& scene,
	const ICamera& camera,
	ISampler& sampler,
	const unsigned int width,
	const unsigned int height
	) const
{
	MLTSample result;

	// Stream 48: film position + wavelength samples.  Must not
	// conflict with BDPTIntegrator's internal streams (0-47).
	sampler.StartStream( 48 );

	// Pick film position + lens position (two independent 2D
	// primary samples).  See MLTRasterizer::EvaluateSample for
	// the full rationale on the -0.5 pixel-center offset and on
	// seeding localRNG from the lens sample so the PSSMLT stream
	// drives the camera aperture as an independent dimension.
	const Point2 filmSample = sampler.Get2D();
	const Point2 lensSample = sampler.Get2D();
	const Scalar fx = filmSample.x * static_cast<Scalar>( width  ) - static_cast<Scalar>( 0.5 );
	const Scalar fy = filmSample.y * static_cast<Scalar>( height ) - static_cast<Scalar>( 0.5 );
	const Point2 screenPos( fx, static_cast<Scalar>( height ) - fy );
	const Point2 cameraRasterPos( fx, fy );

	// See MLTRasterizer::EvaluateSample — localRNG seeded from film
	// bits so the camera has a reproducible stream for any random
	// it might consume beyond the lens, and the actual lens sample
	// reaches the aperture via GenerateRayWithLensSample to keep
	// small PSSMLT mutations continuous.
	const unsigned int fxBits = static_cast<unsigned int>(
		filmSample.x * static_cast<Scalar>( 4294967296.0 ) );
	const unsigned int fyBits = static_cast<unsigned int>(
		filmSample.y * static_cast<Scalar>( 4294967296.0 ) );
	RandomNumberGenerator localRNG( fxBits * 2654435761u + fyBits );
	RuntimeContext rc( localRNG, RuntimeContext::PASS_NORMAL, false );

	Ray cameraRay;
	// See MLTRasterizer::GenerateCameraRayWithLensSample for why this
	// is a non-virtual helper rather than an ICamera method.
	if( !GenerateCameraRayWithLensSample( camera, rc, cameraRay, screenPos, lensSample ) ) {
		return result;
	}

	// Pre-consume all wavelength samples from stream 48 BEFORE any
	// subpath generation runs.  GenerateLight/EyeSubpathNM switch to
	// streams 0..47 internally for their own sampling dimensions, so
	// a `sampler.Get1D()` inside the per-spectral-sample loop would
	// see whatever BDPT stream was left active from the previous
	// iteration — making wavelength #N a leak of path-sampling
	// randomness from wavelength #N-1 instead of an independent
	// PSSMLT primary dimension.  Pre-consuming locks every
	// wavelength to stream 48 at the correct offset.
	std::vector<Scalar> wavelengthSamples;
	wavelengthSamples.reserve( nSpectralSamples );
	for( unsigned int ss = 0; ss < nSpectralSamples; ss++ ) {
		wavelengthSamples.push_back( sampler.Get1D() );
	}

	// Spectral range
	const Scalar range = lambda_end - lambda_begin;

	// Accumulate XYZ contributions per strategy.
	// We use a map from (rasterPos, needsSplat) -> accumulated XYZ.
	// For simplicity, collect all per-strategy XYZ splats in a vector
	// and merge at the end.

	struct StrategyXYZ
	{
		XYZPel		color;
		Point2		rasterPos;
		bool		needsSplat;
	};

	// For each strategy index across all wavelengths, accumulate XYZ.
	// We'll build the final splats after all wavelengths are processed.

	XYZPel totalXYZ( 0, 0, 0 );
	std::vector<StrategyXYZ> allStrategyXYZ;

	if( bUseHWSS )
	{
		// HWSS path: generate subpaths ONCE at hero wavelength,
		// then re-evaluate companion wavelengths using stored vertex
		// geometry (same approach as BDPTSpectralRasterizer HWSS).
		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			// Wavelength sample was pre-consumed above so the BDPT
			// stream switches inside the generator calls don't alias
			// into the next iteration's wavelength.
			const Scalar u = wavelengthSamples[ss];
			SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );

			const Scalar heroNM = swl.HeroLambda();

			// Generate subpaths once at hero wavelength.
			// BDPTIntegrator manages its own streams internally (0-47).
			// MLT forces threshold=1.0 (single-branch) — see note at the
			// top-level helper above.
			std::vector<BDPTVertex> lightVerts;
			std::vector<BDPTVertex> eyeVerts;
			std::vector<uint32_t> lightSubpathStarts;
			std::vector<uint32_t> eyeSubpathStarts;
			// HWSS path in MLT: pass &swl so the NM generator uses
			// max-over-wavelengths RR (PT HWSS firefly fix applies here
			// too — hero-driven RR still amplifies companions on rare
			// survivors regardless of MLT's chain acceptance).
			pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, lightSubpathStarts, heroNM, rc.random, Scalar( 1.0 ), &swl );

			pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, heroNM, Scalar( 1.0 ), &swl );

			// Helper: accumulate strategy results into allStrategyXYZ
			auto accumulateResults = [&]( const std::vector<BDPTIntegrator::ConnectionResultNM>& results,
				const Scalar nm )
			{
				for( unsigned int r = 0; r < results.size(); r++ )
				{
					const BDPTIntegrator::ConnectionResultNM& cr = results[r];
					if( !cr.valid ) continue;

					Scalar weighted = cr.contribution * cr.misWeight;
					if( weighted <= 0 ) continue;

					XYZPel thisXYZ( 0, 0, 0 );
					if( !ColorUtils::XYZFromNM( thisXYZ, nm ) ) continue;
					thisXYZ = thisXYZ * weighted;

					StrategyXYZ sxyz;
					sxyz.color = thisXYZ;
					sxyz.needsSplat = cr.needsSplat;

					if( cr.needsSplat ) {
						sxyz.rasterPos = Point2( cr.rasterPos.x,
							static_cast<Scalar>(height) - cr.rasterPos.y );
					} else {
						sxyz.rasterPos = cameraRasterPos;
					}

					allStrategyXYZ.push_back( sxyz );
					totalXYZ = totalXYZ + thisXYZ;
				}
			};

			// Evaluate hero wavelength
			{
				std::vector<BDPTIntegrator::ConnectionResultNM> heroResults =
					pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, scene, *pCaster, camera, heroNM );
				accumulateResults( heroResults, heroNM );
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

			// Evaluate companion wavelengths via throughput re-evaluation
			for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) continue;

				const Scalar companionNM = swl.lambda[w];

				// Copy vertex arrays and recompute throughputNM
				std::vector<BDPTVertex> compLight = lightVerts;
				std::vector<BDPTVertex> compEye = eyeVerts;
				pIntegrator->RecomputeSubpathThroughputNM(
					compLight, true, heroNM, companionNM, scene, *pCaster );
				pIntegrator->RecomputeSubpathThroughputNM(
					compEye, false, heroNM, companionNM, scene, *pCaster );

				std::vector<BDPTIntegrator::ConnectionResultNM> compResults =
					pIntegrator->EvaluateAllStrategiesNM(
						compLight, compEye, scene, *pCaster, camera, companionNM );
				accumulateResults( compResults, companionNM );
			}
		}
	}
	else
	{
		// Standard spectral: nSpectralSamples independent wavelengths.
		// Wavelength samples were pre-consumed above — see the top of
		// this function for why.
		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			const Scalar u = wavelengthSamples[ss];
			const Scalar nm = lambda_begin + u * range;

			// BDPTIntegrator manages its own streams internally (0-47).
			std::vector<BDPTIntegrator::ConnectionResultNM> results;
			std::vector<BDPTVertex> lightVerts;
			std::vector<BDPTVertex> eyeVerts;
			std::vector<uint32_t> lightSubpathStarts;
			std::vector<uint32_t> eyeSubpathStarts;

			// MLT forces threshold=1.0 (single-branch) — see top-level note.
			// Non-HWSS single-wavelength path: pSwlHWSS = nullptr.
			pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, lightSubpathStarts, nm, rc.random, Scalar( 1.0 ), nullptr );

			pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, nm, Scalar( 1.0 ), nullptr );

			results = pIntegrator->EvaluateAllStrategiesNM( lightVerts, eyeVerts, scene, *pCaster, camera, nm );

			for( unsigned int r = 0; r < results.size(); r++ )
			{
				const BDPTIntegrator::ConnectionResultNM& cr = results[r];
				if( !cr.valid ) {
					continue;
				}

				Scalar weighted = cr.contribution * cr.misWeight;
				if( weighted <= 0 ) {
					continue;
				}

				XYZPel thisXYZ( 0, 0, 0 );
				if( !ColorUtils::XYZFromNM( thisXYZ, nm ) ) {
					continue;
				}
				thisXYZ = thisXYZ * weighted;

				StrategyXYZ sxyz;
				sxyz.color = thisXYZ;
				sxyz.needsSplat = cr.needsSplat;

				if( cr.needsSplat ) {
					sxyz.rasterPos = Point2( cr.rasterPos.x,
						static_cast<Scalar>(height) - cr.rasterPos.y );
				} else {
					sxyz.rasterPos = cameraRasterPos;
				}

				allStrategyXYZ.push_back( sxyz );
				totalXYZ = totalXYZ + thisXYZ;
			}
		}
	}

	// Normalize by number of wavelength evaluations
	const unsigned int totalWavelengths = bUseHWSS
		? nSpectralSamples * SampledWavelengths::N
		: nSpectralSamples;

	const Scalar invWavelengths = 1.0 / static_cast<Scalar>( totalWavelengths );

	// Build final MLTSample splats from accumulated strategy XYZ
	Scalar totalLuminance = 0;

	for( unsigned int i = 0; i < allStrategyXYZ.size(); i++ )
	{
		const StrategyXYZ& sxyz = allStrategyXYZ[i];

		// Scale by 1/totalWavelengths for proper averaging
		XYZPel scaled = sxyz.color * invWavelengths;

		// Luminance = Y component of XYZ
		const Scalar lum = scaled.Y;
		if( lum <= 0 ) {
			continue;
		}

		// Convert XYZ → ROMM RGB (the internal working color space)
		// so that the SplatFilm accumulates in the correct color space.
		// Using the ROMMRGBPel(XYZPel) constructor which calls
		// ColorUtils::XYZtoROMMRGB.
		ROMMRGBPel rommRGB( scaled );

		MLTStrategySplat splat;
		splat.color = rommRGB;
		splat.rasterPos = sxyz.rasterPos;

		result.splats.push_back( splat );
		totalLuminance += lum;
	}

	result.luminance = totalLuminance;
	result.valid = ( totalLuminance > 0 );

	return result;
}

//////////////////////////////////////////////////////////////////////
// RenderFrameOfMLTSpectral - Spectral analog of MLTRasterizer::
// RenderFrameOfMLT.  Same structure, but every BDPT path is evaluated
// at one or more wavelengths via EvaluateSampleSpectral.  Shared by
// RasterizeScene and RasterizeSceneAnimation; see the base helper
// for the contract.
//
// Hero wavelength stratification (HWSS) is part of the primary sample
// space — the wavelength coordinate evolves as another dimension of
// the Markov chain, so independent-chains-per-frame works the same
// way as RGB MLT.  No special spectral handling for animation; the
// per-frame chain restart picks a fresh hero wavelength via the
// re-bootstrapped PSSMLTSampler.
//////////////////////////////////////////////////////////////////////

bool MLTSpectralRasterizer::RenderFrameOfMLTSpectral(
	const IScene& pScene,
	const ICamera& pCamera,
	const unsigned int width,
	const unsigned int height,
	IRasterImage*& pImageOut
	) const
{
	pImageOut = 0;

	GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Starting spectral PSSMLT render (%ux%u)", width, height );
	GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Lambda range: [%.1f, %.1f] nm, Spectral samples: %u, HWSS: %s",
		lambda_begin, lambda_end, nSpectralSamples, bUseHWSS ? "yes" : "no" );
	GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Bootstrap: %u, Chains: %u, Mutations/pixel: %u, Large step: %.2f",
		nBootstrap, nChains, nMutationsPerPixel, largeStepProb );

	//////////////////////////////////////////////////////////////////
	// Phase 1: Bootstrap (spectral)
	//////////////////////////////////////////////////////////////////

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Spectral Bootstrap: " );
	}

	std::vector<BootstrapSample> bootstrapSamples( nBootstrap );
	Scalar luminanceSum = 0;

	Timer bootstrapTimer;
	bootstrapTimer.start();

	// Bootstrap uses a PSSMLTSampler so the path generated for seed i
	// can be EXACTLY reproduced later by the chain init creating a
	// PSSMLTSampler with the same seed.  See MLTRasterizer::RasterizeScene
	// bootstrap loop for the full rationale.
	for( unsigned int i = 0; i < nBootstrap; i++ )
	{
		PSSMLTSampler* pBootSampler = new PSSMLTSampler( i, largeStepProb );
		pBootSampler->StartIteration();

		MLTSample sample = EvaluateSampleSpectral( pScene, pCamera, *pBootSampler, width, height );

		bootstrapSamples[i].luminance = sample.luminance;
		bootstrapSamples[i].seed = i;
		luminanceSum += sample.luminance;

		safe_release( pBootSampler );

		if( pProgressFunc && (i % 1000 == 0) ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(nBootstrap) ) ) {
				return false;
			}
		}
	}

	bootstrapTimer.stop();

	const Scalar b_mean = luminanceSum / static_cast<Scalar>( nBootstrap );

	if( b_mean <= 0 ) {
		GlobalLog()->PrintSourceError( "MLTSpectralRasterizer:: Bootstrap found zero luminance -- scene produces no visible light!", __FILE__, __LINE__ );
		return false;
	}

	const Scalar numPixels = static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar b = b_mean * numPixels;

	GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Bootstrap complete. Mean luminance = %f, Normalization b = %f", b_mean, b );

	//////////////////////////////////////////////////////////////////
	// Phase 1b: Build CDF
	//////////////////////////////////////////////////////////////////

	std::vector<Scalar> cdf( nBootstrap );
	cdf[0] = bootstrapSamples[0].luminance;
	for( unsigned int i = 1; i < nBootstrap; i++ ) {
		cdf[i] = cdf[i-1] + bootstrapSamples[i].luminance;
	}

	if( cdf[nBootstrap-1] > 0 ) {
		const Scalar invTotal = 1.0 / cdf[nBootstrap-1];
		for( unsigned int i = 0; i < nBootstrap; i++ ) {
			cdf[i] *= invTotal;
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 2: Initialize chain states (using spectral evaluation)
	//////////////////////////////////////////////////////////////////

	const unsigned int effectiveChains = nChains > 0 ? nChains : 1;
	const unsigned int totalMutations = nMutationsPerPixel * width * height;
	const unsigned int mutationsPerChain = totalMutations / effectiveChains;

	// Auto-calculate progressive rounds from bootstrap timing
	int threads = HowManyThreadsToSpawn();
	static const int MAX_THREADS = 10000;
	if( threads > MAX_THREADS ) {
		threads = MAX_THREADS;
	}

	unsigned int numRounds = 1;
	const unsigned int bootstrapMs = bootstrapTimer.getInterval();

	if( bootstrapMs > 0 && nBootstrap > 0 )
	{
		const double msPerSample = static_cast<double>( bootstrapMs ) / static_cast<double>( nBootstrap );
		const int effectiveThreads = threads > 0 ? threads : 1;
		const double estTotalMs = msPerSample * static_cast<double>( totalMutations ) / static_cast<double>( effectiveThreads );
		// Target 2500 ms per round (instead of 5000 ms) — see
		// MLTRasterizer.cpp for the rationale.  Adaptive resize in
		// the render loop below corrects for inaccurate bootstrap
		// estimates.
		const double targetRoundMs = 2500.0;
		const unsigned int computedRounds = static_cast<unsigned int>( estTotalMs / targetRoundMs + 0.5 );

		numRounds = computedRounds < 2 ? 2 : computedRounds;
		if( numRounds > mutationsPerChain ) {
			numRounds = mutationsPerChain;
		}

		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Bootstrap took %u ms (%.3f ms/sample), est total: %.1f s -> %u rounds",
			bootstrapMs, msPerSample, estTotalMs / 1000.0, numRounds );
	}
	else
	{
		numRounds = 20;
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Bootstrap too fast to time, using %u rounds", numRounds );
	}

	unsigned int mutationsPerChainPerRound = mutationsPerChain / numRounds;
	unsigned int effectiveMutPerRound = mutationsPerChainPerRound > 0 ? mutationsPerChainPerRound : 1;

	GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Total mutations: %u, Per chain: %u, Per chain per round: %u, Rounds: %u",
		totalMutations, mutationsPerChain, effectiveMutPerRound, numRounds );

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Spectral Init Chains: " );
	}

	// Initialize chains using spectral evaluation
	std::vector<ChainState> chainStates( effectiveChains );

	for( unsigned int c = 0; c < effectiveChains; c++ )
	{
		// Select initial state from bootstrap CDF
		RandomNumberGenerator selRNG( c * 31337 );
		const Scalar u = selRNG.CanonicalRandom();
		const unsigned int bootstrapIdx = SelectFromCDF( cdf, u );
		const BootstrapSample& seed = bootstrapSamples[bootstrapIdx];

		// Two-phase chain initialization (mirrors MLTRasterizer::InitChain):
		// Phase 1 — seed PSSMLTSampler with the bootstrap index so its
		//   first iteration EXACTLY reproduces the bootstrap path the
		//   CDF selected (preserving importance sampling).
		// Phase 2 — after the first Accept(), re-seed the proposal RNG
		//   to a chain-specific value so two chains that picked the
		//   same bootstrap index immediately diverge via different
		//   mutations.  The X vector is preserved across the re-seed.
		const unsigned int chainSeed    = seed.seed * 2654435761u + c;
		const unsigned int proposalSeed = chainSeed ^ 0xA55A5AA5u;
		chainStates[c].pSampler = new PSSMLTSampler( seed.seed, largeStepProb );
		chainStates[c].chainRNG = RandomNumberGenerator( chainSeed );

		// Phase 1: reproduce the bootstrap path
		chainStates[c].pSampler->StartIteration();
		chainStates[c].currentSample = EvaluateSampleSpectral( pScene, pCamera, *chainStates[c].pSampler, width, height );
		chainStates[c].pSampler->Accept();

		// Phase 2: re-seed proposal RNG for divergence
		chainStates[c].pSampler->ReSeedRNG( proposalSeed );

		if( !chainStates[c].currentSample.valid )
		{
			for( unsigned int attempt = 0; attempt < 64; attempt++ )
			{
				chainStates[c].pSampler->StartIteration();
				chainStates[c].currentSample = EvaluateSampleSpectral( pScene, pCamera, *chainStates[c].pSampler, width, height );
				if( chainStates[c].currentSample.valid ) {
					chainStates[c].pSampler->Accept();
					break;
				}
				chainStates[c].pSampler->Reject();
			}
		}

		if( pProgressFunc && (c % 10 == 0) ) {
			if( !pProgressFunc->Progress( static_cast<double>(c), static_cast<double>(effectiveChains) ) ) {
				for( unsigned int j = 0; j <= c; j++ ) {
					safe_release( chainStates[j].pSampler );
				}
				return false;
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 3: Round-based rendering (spectral)
	//
	// Uses RunChainSegmentSpectral for spectral evaluation.
	// Multi-threaded: each thread runs its assigned chains via
	// SpectralRoundThread_ThreadProc, mirroring the base class
	// MLTRasterizer pattern.
	//////////////////////////////////////////////////////////////////

	SplatFilm* pSplatFilm = new SplatFilm( width, height );

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Spectral Rendering: " );
	}

	bool cancelled = false;

	if( pProgressFunc ) {
		if( !pProgressFunc->Progress( 0, static_cast<double>(numRounds) ) ) {
			cancelled = true;
		}
	}

	// Adaptive round sizing — see MLTRasterizer.cpp.
	const double targetRoundWallMs   = 2500.0;
	unsigned int mutationsDonePerChain = 0;

	for( unsigned int round = 0; round < numRounds && !cancelled; round++ )
	{
		const unsigned int mutThisRound = ( round == numRounds - 1 )
			? ( mutationsPerChain - mutationsDonePerChain )
			: effectiveMutPerRound;

		Timer roundTimer;
		roundTimer.start();

		if( threads > 1 )
		{
			// Work-stealing chain dispatch — see MLTRasterizer.cpp
			// for the rationale (highly variable per-chain cost wrecks
			// static partitioning).  Each worker MUST flush its splat
			// buffer before returning so the round's Resolve() sees
			// every splat below the auto-flush threshold.
			std::atomic<unsigned int> nextChain( 0 );
			ThreadPool& pool = GlobalThreadPool();

			pool.ParallelFor( static_cast<unsigned int>( threads ),
				[&]( unsigned int /*workerIdx*/ ) {
					for( ;; ) {
						const unsigned int c = nextChain.fetch_add(
							1, std::memory_order_relaxed );
						if( c >= effectiveChains ) {
							break;
						}
						RunChainSegmentSpectral(
							chainStates[c], pScene, pCamera, *pSplatFilm,
							mutThisRound, b, width, height );
					}
					FlushCallingThreadSplatBuffer();
				} );
		}
		else
		{
			// Legacy low-priority mode: see MLTRasterizer.cpp SP
			// branch for the rationale.
			if( round == 0 &&
			    GlobalOptions().ReadBool( "force_all_threads_low_priority", false ) ) {
				Threading::riseSetThreadLowPriority( 0 );
			}

			// Single-threaded: run all chains sequentially
			for( unsigned int c = 0; c < effectiveChains; c++ )
			{
				RunChainSegmentSpectral( chainStates[c], pScene, pCamera, *pSplatFilm,
					mutThisRound, b, width, height );
			}
			FlushCallingThreadSplatBuffer();
		}

		roundTimer.stop();
		mutationsDonePerChain += mutThisRound;
		const unsigned int roundWallMs = roundTimer.getInterval();

		// Adaptive round-size correction after round 0 — mirrors
		// MLTRasterizer.cpp.  See the comment there for rationale.
		if( round == 0 && roundWallMs > 50 && !cancelled ) {
			const double actualMsPerChainMut =
				static_cast<double>( roundWallMs ) /
				static_cast<double>( mutThisRound );
			const double desiredMutPerRound =
				targetRoundWallMs / actualMsPerChainMut;
			unsigned int newMutPerRound = desiredMutPerRound < 1.0
				? 1u
				: static_cast<unsigned int>( desiredMutPerRound );
			if( newMutPerRound < 1 ) newMutPerRound = 1;

			const unsigned int remainingMut = mutationsPerChain > mutationsDonePerChain
				? mutationsPerChain - mutationsDonePerChain
				: 0u;
			if( remainingMut > 0 && newMutPerRound != effectiveMutPerRound ) {
				const unsigned int newRemainingRounds =
					( remainingMut + newMutPerRound - 1 ) / newMutPerRound;
				const unsigned int newNumRounds = 1 + newRemainingRounds;

				GlobalLog()->PrintEx( eLog_Event,
					"MLTSpectralRasterizer:: Adaptive round resize — round 0 took %u ms "
					"(%.4f ms/mut), adjusting effectiveMutPerRound %u → %u, "
					"numRounds %u → %u for a ~%.1f s UI cadence",
					roundWallMs, actualMsPerChainMut,
					effectiveMutPerRound, newMutPerRound,
					numRounds, newNumRounds,
					targetRoundWallMs / 1000.0 );

				effectiveMutPerRound = newMutPerRound;
				numRounds            = newNumRounds;
			}
		}

		// Snapshot: resolve film and output.  Fraction uses
		// mutations-done (stable across adaptive resize).
		const Scalar fraction =
			static_cast<Scalar>( mutationsDonePerChain ) /
			static_cast<Scalar>( mutationsPerChain > 0 ? mutationsPerChain : 1 );
		const bool isFinalRound = ( mutationsDonePerChain >= mutationsPerChain );

		// Each round allocates a fresh image — SplatFilm::Resolve is
		// ADDITIVE; reusing one image across rounds would double-count.
		// Final-round image is transferred to pImageOut for the caller
		// to denoise + flush at the appropriate frame index.
		IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 1.0 ) );
		pSplatFilm->Resolve( *pImage, fraction );

		if( !isFinalRound )
		{
			RasterizerOutputListType::const_iterator r, s;
			for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
				(*r)->OutputIntermediateImage( *pImage, 0 );
			}
		}

		if( pProgressFunc ) {
			if( !pProgressFunc->Progress(
					static_cast<double>( mutationsDonePerChain ),
					static_cast<double>( mutationsPerChain ) ) ) {
				cancelled = true;
			}
		}

		if( isFinalRound || cancelled )
		{
			if( pImageOut ) {
				safe_release( pImageOut );
			}
			pImageOut = pImage;	// transfer refcount to caller
		}
		else
		{
			safe_release( pImage );
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 4: Cleanup
	//////////////////////////////////////////////////////////////////

	RISE_PROFILE_REPORT(GlobalLog());

	if( cancelled ) {
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Rendering cancelled by user" );
	} else {
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Rendering complete" );
	}

	for( unsigned int c = 0; c < effectiveChains; c++ ) {
		safe_release( chainStates[c].pSampler );
	}

	safe_release( pSplatFilm );

	return !cancelled;
}

//////////////////////////////////////////////////////////////////////
// RasterizeScene - Spectral still-image entry point.  Thin wrapper
// around RenderFrameOfMLTSpectral; flushes the result with frameIdx=0.
//////////////////////////////////////////////////////////////////////

void MLTSpectralRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* /*pRect*/,
	IRasterizeSequence* /*pRasterSequence*/
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		GlobalLog()->PrintSourceError( "MLTSpectralRasterizer::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

#ifdef RISE_ENABLE_OIDN
	BeginRenderTimer();
#endif

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();
	pIntegrator->SetLightSampler( pCaster->GetLightSampler() );

	IRasterImage* pImage = 0;
	(void) RenderFrameOfMLTSpectral( pScene, *pCamera, width, height, pImage );

	if( pImage )
	{
#ifdef RISE_ENABLE_OIDN
		if( bDenoisingEnabled ) {
			FlushPreDenoisedToOutputs( *pImage, 0, 0 );

			AOVBuffers aovBuffers( width, height );
			OIDNDenoiser::CollectFirstHitAOVs( pScene, *pCaster, aovBuffers );
			mDenoiser->ApplyDenoise( *pImage, aovBuffers, width, height,
				mDenoisingQuality, mDenoisingDevice, OidnPrefilter::Fast,
				GetRenderElapsedSeconds() );

			FlushDenoisedToOutputs( *pImage, 0, 0 );
		} else
#endif
		{
			FlushToOutputs( *pImage, 0, 0 );
		}

		safe_release( pImage );
	}
}

//////////////////////////////////////////////////////////////////////
// RasterizeSceneAnimation - Spectral animation entry point.
//
// Same independent-chains-per-frame strategy as the RGB MLT path.
// The hero wavelength evolves as part of the primary sample space,
// so per-frame chain restart picks a fresh wavelength via the
// re-bootstrapped PSSMLTSampler — no special spectral handling
// needed.  See MLTRasterizer::RasterizeSceneAnimation for the full
// rationale.
//////////////////////////////////////////////////////////////////////

void MLTSpectralRasterizer::RasterizeSceneAnimation(
	const IScene& pScene,
	const Scalar time_start,
	const Scalar time_end,
	const unsigned int num_frames,
	const bool /*do_fields*/,
	const bool /*invert_fields*/,
	const Rect* /*pRect*/,
	const unsigned int* specificFrame,
	IRasterizeSequence* /*pRasterSequence*/
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		GlobalLog()->PrintSourceError( "MLTSpectralRasterizer::RasterizeSceneAnimation:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();
	const Scalar step_size = num_frames > 1
		? ( time_end - time_start ) / Scalar( num_frames - 1 )
		: 0;

	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();
	pIntegrator->SetLightSampler( pCaster->GetLightSampler() );

	const bool bHasKeyframedObjects = pScene.GetAnimator()->AreThereAnyKeyframedObjects();
	const unsigned int total_frames = specificFrame ? 1 : num_frames;

	GlobalLog()->PrintEx( eLog_Event,
		"MLTSpectralRasterizer:: Animation render: %u frame(s), time [%.4f, %.4f]",
		total_frames, time_start, time_end );

	bool cancelled = false;
	for( unsigned int i = 0; i < total_frames && !cancelled; i++ )
	{
		const unsigned int frameIdx = specificFrame ? *specificFrame : i;
		const Scalar curtime = time_start + Scalar( frameIdx ) * step_size;

		pScene.GetAnimator()->EvaluateAtTime( curtime );
		if( bHasKeyframedObjects ) {
			pScene.GetObjects()->InvalidateSpatialStructure();
		}
		pScene.GetObjects()->PrepareForRendering();
		pScene.SetSceneTime( curtime );

		GlobalLog()->PrintEx( eLog_Event,
			"MLTSpectralRasterizer:: Rasterizing frame %u of %u (t=%.4f)",
			frameIdx + 1, num_frames, curtime );

#ifdef RISE_ENABLE_OIDN
		BeginRenderTimer();
#endif

		IRasterImage* pImage = 0;
		const bool completed = RenderFrameOfMLTSpectral( pScene, *pCamera, width, height, pImage );

		if( !completed )
		{
			if( pImage ) {
				safe_release( pImage );
			}
			cancelled = true;
			GlobalLog()->PrintEx( eLog_Event,
				"MLTSpectralRasterizer:: Animation cancelled during frame %u of %u; "
				"skipping remaining frames",
				frameIdx + 1, num_frames );
			break;
		}

		if( pImage )
		{
#ifdef RISE_ENABLE_OIDN
			if( bDenoisingEnabled ) {
				FlushPreDenoisedToOutputs( *pImage, 0, frameIdx );

				AOVBuffers aovBuffers( width, height );
				OIDNDenoiser::CollectFirstHitAOVs( pScene, *pCaster, aovBuffers );
				mDenoiser->ApplyDenoise( *pImage, aovBuffers, width, height,
					mDenoisingQuality, mDenoisingDevice, OidnPrefilter::Fast,
					GetRenderElapsedSeconds() );

				FlushDenoisedToOutputs( *pImage, 0, frameIdx );
			} else
#endif
			{
				FlushToOutputs( *pImage, 0, frameIdx );
			}

			safe_release( pImage );
		}
	}

	if( cancelled ) {
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Animation cancelled by user" );
	} else {
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Animation complete" );
	}
}
