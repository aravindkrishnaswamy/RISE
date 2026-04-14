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
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../RasterImages/RasterImage.h"
#include "../Utilities/Profiling.h"
#include "../Utilities/RTime.h"

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

				const unsigned int sx = static_cast<unsigned int>( s.rasterPos.x );
				const unsigned int sy = static_cast<unsigned int>( s.rasterPos.y );

				if( sx < width && sy < height ) {
					splatFilm.Splat( sx, sy, splatColor );
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

				const unsigned int sx = static_cast<unsigned int>( s.rasterPos.x );
				const unsigned int sy = static_cast<unsigned int>( s.rasterPos.y );

				if( sx < width && sy < height ) {
					splatFilm.Splat( sx, sy, splatColor );
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

	pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, nm, rc.random );
	pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, nm );

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

	// Pick film position (first 2D sample, same as RGB MLT)
	const Point2 filmSample = sampler.Get2D();
	const unsigned int px = std::min( static_cast<unsigned int>( filmSample.x * width ), width - 1 );
	const unsigned int py = std::min( static_cast<unsigned int>( filmSample.y * height ), height - 1 );
	const Point2 screenPos( static_cast<Scalar>(px), static_cast<Scalar>(height - py) );
	const Point2 cameraRasterPos( static_cast<Scalar>(px), static_cast<Scalar>(py) );

	// Generate camera ray
	RandomNumberGenerator localRNG( px * 65537 + py );
	RuntimeContext rc( localRNG, RuntimeContext::PASS_NORMAL, false );

	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, screenPos ) ) {
		return result;
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
			const Scalar u = sampler.Get1D();
			SampledWavelengths swl = SampledWavelengths::SampleEquidistant( u, lambda_begin, lambda_end );

			const Scalar heroNM = swl.HeroLambda();

			// Generate subpaths once at hero wavelength.
			// BDPTIntegrator manages its own streams internally (0-47).
			std::vector<BDPTVertex> lightVerts;
			std::vector<BDPTVertex> eyeVerts;
			pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, heroNM, rc.random );

			pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, heroNM );

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
		// Standard spectral: nSpectralSamples independent wavelengths
		for( unsigned int ss = 0; ss < nSpectralSamples; ss++ )
		{
			const Scalar u = sampler.Get1D();
			const Scalar nm = lambda_begin + u * range;

			// BDPTIntegrator manages its own streams internally (0-47).
			std::vector<BDPTIntegrator::ConnectionResultNM> results;
			std::vector<BDPTVertex> lightVerts;
			std::vector<BDPTVertex> eyeVerts;

			pIntegrator->GenerateLightSubpathNM( scene, *pCaster, sampler, lightVerts, nm, rc.random );

			pIntegrator->GenerateEyeSubpathNM( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, nm );

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
// RasterizeScene - Spectral MLT rendering entry point.
//
// Identical structure to MLTRasterizer::RasterizeScene but uses
// EvaluateSampleSpectral instead of EvaluateSample for both
// bootstrap and chain mutations.
//
// The bootstrap phase estimates normalization using spectral samples.
// Chain initialization and round-based execution use the same
// infrastructure, with the spectral evaluation replacing RGB.
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

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	// AttachScene creates and Prepare()s the unified LightSampler
	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();

	// Share the RayCaster's prepared LightSampler with the integrator
	const LightSampler* pLS = pCaster->GetLightSampler();
	pIntegrator->SetLightSampler( pLS );

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

	for( unsigned int i = 0; i < nBootstrap; i++ )
	{
		RandomNumberGenerator bootRNG( i );
		IndependentSampler bootSampler( bootRNG );

		MLTSample sample = EvaluateSampleSpectral( pScene, *pCamera, bootSampler, width, height );

		bootstrapSamples[i].luminance = sample.luminance;
		bootstrapSamples[i].seed = i;
		luminanceSum += sample.luminance;

		if( pProgressFunc && (i % 1000 == 0) ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(nBootstrap) ) ) {
				return;
			}
		}
	}

	bootstrapTimer.stop();

	const Scalar b_mean = luminanceSum / static_cast<Scalar>( nBootstrap );

	if( b_mean <= 0 ) {
		GlobalLog()->PrintSourceError( "MLTSpectralRasterizer:: Bootstrap found zero luminance -- scene produces no visible light!", __FILE__, __LINE__ );
		return;
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
		const double targetRoundMs = 5000.0;
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
		numRounds = 10;
		GlobalLog()->PrintEx( eLog_Event, "MLTSpectralRasterizer:: Bootstrap too fast to time, using %u rounds", numRounds );
	}

	const unsigned int mutationsPerChainPerRound = mutationsPerChain / numRounds;
	const unsigned int effectiveMutPerRound = mutationsPerChainPerRound > 0 ? mutationsPerChainPerRound : 1;

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

		// Initialize chain — but we need spectral evaluation
		// We manually do what InitChain does, but with EvaluateSampleSpectral
		chainStates[c].pSampler = new PSSMLTSampler( seed.seed, largeStepProb );
		chainStates[c].chainRNG = RandomNumberGenerator( seed.seed + 1000000 );

		chainStates[c].pSampler->StartIteration();
		chainStates[c].currentSample = EvaluateSampleSpectral( pScene, *pCamera, *chainStates[c].pSampler, width, height );
		chainStates[c].pSampler->Accept();

		if( !chainStates[c].currentSample.valid )
		{
			for( unsigned int attempt = 0; attempt < 64; attempt++ )
			{
				chainStates[c].pSampler->StartIteration();
				chainStates[c].currentSample = EvaluateSampleSpectral( pScene, *pCamera, *chainStates[c].pSampler, width, height );
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
				return;
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

	for( unsigned int round = 0; round < numRounds && !cancelled; round++ )
	{
		const unsigned int mutThisRound = ( round == numRounds - 1 )
			? ( mutationsPerChain - effectiveMutPerRound * ( numRounds - 1 ) )
			: effectiveMutPerRound;

		if( threads > 1 )
		{
			// Multi-threaded: distribute chains across threads
			const unsigned int chainsPerThread = effectiveChains / threads;

			std::vector<SpectralRoundThreadData> threadData( threads );
			RISETHREADID* thread_ids = new RISETHREADID[threads];

			for( int t = 0; t < threads; t++ )
			{
				threadData[t].pRasterizer = this;
				threadData[t].pScene = &pScene;
				threadData[t].pSplatFilm = pSplatFilm;
				threadData[t].pChainStates = &chainStates[0];
				threadData[t].chainStart = t * chainsPerThread;
				threadData[t].chainEnd = ( t == threads - 1 ) ? effectiveChains : ( t + 1 ) * chainsPerThread;
				threadData[t].mutationsPerChain = mutThisRound;
				threadData[t].normalization = b;
				threadData[t].width = width;
				threadData[t].height = height;

				Threading::riseCreateThread( SpectralRoundThread_ThreadProc, (void*)&threadData[t], 0, 0, &thread_ids[t] );
			}

			for( int t = 0; t < threads; t++ ) {
				Threading::riseWaitUntilThreadFinishes( thread_ids[t], 0 );
			}

			delete[] thread_ids;
		}
		else
		{
			// Single-threaded: run all chains sequentially
			for( unsigned int c = 0; c < effectiveChains; c++ )
			{
				RunChainSegmentSpectral( chainStates[c], pScene, *pCamera, *pSplatFilm,
					mutThisRound, b, width, height );
			}
		}

		// Snapshot: resolve film and output
		const Scalar fraction = static_cast<Scalar>( round + 1 ) / static_cast<Scalar>( numRounds );
		const bool isFinalRound = ( round + 1 == numRounds );

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
			if( !pProgressFunc->Progress( static_cast<double>(round + 1), static_cast<double>(numRounds) ) ) {
				cancelled = true;
			}
		}

		if( isFinalRound || cancelled )
		{
#ifdef RISE_ENABLE_OIDN
			if( bDenoisingEnabled ) {
				// Pre-denoised but fully splatted image goes to file
				// outputs under the normal filename first.
				FlushPreDenoisedToOutputs( *pImage, 0, 0 );

				AOVBuffers aovBuffers( width, height );
				OIDNDenoiser::CollectFirstHitAOVs( pScene, *pCaster, aovBuffers );
				OIDNDenoiser::ApplyDenoise( *pImage, aovBuffers, width, height );

				FlushDenoisedToOutputs( *pImage, 0, 0 );
			} else
#endif
			{
				FlushToOutputs( *pImage, 0, 0 );
			}
		}

		safe_release( pImage );
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
}
