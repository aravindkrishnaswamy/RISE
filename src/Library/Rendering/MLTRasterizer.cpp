//////////////////////////////////////////////////////////////////////
//
//  MLTRasterizer.cpp - Implementation of the PSSMLT rasterizer.
//
//  ALGORITHM OVERVIEW:
//    This file implements the complete PSSMLT rendering pipeline
//    with round-based progressive rendering:
//
//    1. Bootstrap Phase:
//       Generate nBootstrap independent BDPT samples using different
//       RNG seeds.  Record each sample's luminance and seed.  The
//       average luminance across all bootstrap samples gives us the
//       normalization constant b, which represents the expected
//       luminance of the image.  We also build a CDF over the
//       bootstrap luminances so we can importance-sample initial
//       states for the Markov chains.
//
//    2. Chain Initialization:
//       Create all nChains Markov chains up front: each gets a
//       PSSMLTSampler seeded from a CDF-selected bootstrap sample,
//       and its initial path is evaluated.  The chain states
//       (ChainState structs) persist across all rendering rounds.
//
//    3. Round-Based Execution:
//       The total mutation budget is divided into nProgressiveRounds
//       rounds.  Each round:
//       (a) Dispatch chains to threads; each thread runs its assigned
//           chains for mutationsPerRound mutations.
//       (b) After all threads finish, resolve the accumulated splat
//           film to a fresh image, scaling by the fraction of total
//           mutations completed so far.
//       (c) Output the progressive image and report progress.
//
//       This design preserves full Markov chain continuity — the
//       PSSMLTSampler and currentSample persist across rounds, so
//       the chains are never interrupted.  The round boundaries are
//       pure observation points: we snapshot the film and continue.
//
//       If the user cancels mid-render, the last output image is a
//       valid (noisy) render at the quality achieved so far.
//
//    4. Final Resolve:
//       After all rounds complete, the last output is the final
//       fully-converged image.
//
//  SPLATTING MATH:
//    Each splat is pre-weighted by invTotalMutations = 1/totalMutations.
//    After round r of R total rounds, the film contains approximately
//    (r/R) of the final accumulated value.  To produce a correctly-
//    exposed image at round r, we resolve with:
//      sampleCount = r / R
//    so that:
//      pixel = accumulated / (r/R) = accumulated * R/r
//    At the final round (r=R), sampleCount=1.0, which is the exact
//    final result.
//
//  THREADING:
//    Each round dispatches threads using the same pattern as before.
//    Each thread runs its assigned chains for mutationsPerRound
//    mutations.  SplatFilm's row-level mutexes handle concurrent
//    writes.  Between rounds, all threads are joined before the
//    main thread snapshots the film — no races during resolve.
//
//  REFERENCES:
//    - Kelemen et al. 2002, Section 3: PSSMLT algorithm
//    - Veach 1997, Chapter 11.2.4: Expected values technique
//    - PBRT v3, Chapter 16.4: MLT implementation reference
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
#include "MLTRasterizer.h"
#include "../Utilities/IndependentSampler.h"
#include "../RasterImages/RasterImage.h"
#include "../Utilities/Profiling.h"
#include "../Utilities/RTime.h"

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Constructor / Destructor
//////////////////////////////////////////////////////////////////////

MLTRasterizer::MLTRasterizer(
	IRayCaster* pCaster_,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const unsigned int nBootstrap_,
	const unsigned int nChains_,
	const unsigned int nMutationsPerPixel_,
	const Scalar largeStepProb_
	) :
  pCaster( pCaster_ ),
  pIntegrator( 0 ),
  nBootstrap( nBootstrap_ ),
  nChains( nChains_ ),
  nMutationsPerPixel( nMutationsPerPixel_ ),
  largeStepProb( largeStepProb_ )
{
	if( pCaster ) {
		pCaster->addref();
	}

	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth );
	pIntegrator->addref();
}

MLTRasterizer::~MLTRasterizer()
{
	safe_release( pIntegrator );
	safe_release( pCaster );
}

//////////////////////////////////////////////////////////////////////
// EvaluateSample - Bridge between MLT and BDPT.
//
// Uses the sampler to:
// 1. Pick a random film position (first 2D sample)
// 2. Generate a camera ray for that position
// 3. Build light and eye subpaths using the sampler
// 4. Evaluate all (s,t) connection strategies
// 5. Collect per-strategy contributions with correct pixel positions
//
// Each strategy's contribution is stored as a separate MLTStrategySplat
// with its own rasterPos.  Strategies with t<=1 (light-to-camera
// connections) produce contributions at pixel positions determined by
// projecting the light vertex through the camera, NOT the camera ray's
// pixel.  The aggregate luminance (over ALL strategies) is used for
// the Metropolis acceptance ratio.
//////////////////////////////////////////////////////////////////////

MLTRasterizer::MLTSample MLTRasterizer::EvaluateSample(
	const IScene& scene,
	const ICamera& camera,
	ISampler& sampler,
	const unsigned int width,
	const unsigned int height
	) const
{
	MLTSample result;

	// Stream 0: film position samples
	sampler.StartStream( 0 );

	// Use the first 2D sample to pick a film position.
	// This means mutations to these two values move the path
	// to a different pixel -- important for film coverage.
	const Point2 filmSample = sampler.Get2D();
	const unsigned int px = std::min( static_cast<unsigned int>( filmSample.x * width ), width - 1 );
	const unsigned int py = std::min( static_cast<unsigned int>( filmSample.y * height ), height - 1 );
	const Point2 screenPos( static_cast<Scalar>(px), static_cast<Scalar>(height - 1 - py) );

	const Point2 cameraRasterPos( static_cast<Scalar>(px), static_cast<Scalar>(py) );

	// Generate camera ray for this pixel
	RandomNumberGenerator localRNG( px * 65537 + py );
	RuntimeContext rc( localRNG, RuntimeContext::PASS_NORMAL, false );

	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, screenPos ) ) {
		return result;
	}

	// Stream 1: light subpath samples
	sampler.StartStream( 1 );

	// Generate light and eye subpaths using the MLT sampler.
	// BDPTIntegrator consumes samples from the sampler in a
	// deterministic order, which is exactly what PSSMLT requires.
	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpath( scene, *pCaster, sampler, lightVerts );

	// Stream 2: eye subpath and connection samples
	sampler.StartStream( 2 );

	pIntegrator->GenerateEyeSubpath( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts );

	// Evaluate all (s,t) connection strategies via MIS
	std::vector<BDPTIntegrator::ConnectionResult> results =
		pIntegrator->EvaluateAllStrategies( lightVerts, eyeVerts, scene, *pCaster, camera );

	// Collect per-strategy contributions, each with its correct pixel
	// position.  Strategies with t<=1 (needsSplat=true) use the pixel
	// position from the light-to-camera projection.  All others use
	// the camera ray's pixel position.  The aggregate luminance across
	// ALL strategies is used for the Metropolis acceptance ratio.
	Scalar totalLuminance = 0;

	for( unsigned int r = 0; r < results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResult& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		RISEPel weighted = cr.contribution * cr.misWeight;

		// Clamp negative components from numerical precision issues
		for( int ch = 0; ch < 3; ch++ ) {
			if( weighted[ch] < 0 ) {
				weighted[ch] = 0;
			}
		}

		const Scalar lum = 0.2126 * weighted[0] + 0.7152 * weighted[1] + 0.0722 * weighted[2];
		if( lum <= 0 ) {
			continue;
		}

		MLTStrategySplat splat;
		splat.color = weighted;

		if( cr.needsSplat ) {
			// t<=1 strategies: use the strategy's own pixel position
			splat.rasterPos = cr.rasterPos;
		} else {
			// t>=2 strategies: use the camera pixel position
			splat.rasterPos = cameraRasterPos;
		}

		result.splats.push_back( splat );
		totalLuminance += lum;
	}

	result.luminance = totalLuminance;
	result.valid = ( totalLuminance > 0 );

	return result;
}

//////////////////////////////////////////////////////////////////////
// SelectFromCDF - Binary search in the bootstrap CDF to find the
// index corresponding to uniform random u.  Used to importance-
// sample initial chain states weighted by their luminance.
//////////////////////////////////////////////////////////////////////

unsigned int MLTRasterizer::SelectFromCDF(
	const std::vector<Scalar>& cdf,
	const Scalar u
	) const
{
	// Standard binary search in a monotonically increasing CDF
	unsigned int lo = 0;
	unsigned int hi = static_cast<unsigned int>( cdf.size() ) - 1;

	while( lo < hi )
	{
		const unsigned int mid = ( lo + hi ) / 2;
		if( cdf[mid] < u ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	return lo;
}

//////////////////////////////////////////////////////////////////////
// InitChain - Initialize a single Markov chain.
//
// Creates the PSSMLTSampler from a bootstrap seed, evaluates the
// initial path, and stores the result in the persistent ChainState.
// If the initial sample has zero luminance (path hit nothing),
// try additional large steps to find a productive starting point.
//
// This is called once per chain during setup, before the round
// loop begins.  The resulting ChainState persists across all rounds.
//////////////////////////////////////////////////////////////////////

void MLTRasterizer::InitChain(
	ChainState& state,
	const IScene& scene,
	const ICamera& camera,
	const BootstrapSample& seed,
	const unsigned int width,
	const unsigned int height
	) const
{
	state.pSampler = new PSSMLTSampler( seed.seed, largeStepProb );
	state.pSampler->addref();
	state.chainRNG = RandomNumberGenerator( seed.seed + 1000000 );

	// Evaluate the initial state
	state.pSampler->StartIteration();
	state.currentSample = EvaluateSample( scene, camera, *state.pSampler, width, height );
	state.pSampler->Accept();

	// If zero luminance, try additional large steps to find a valid state
	if( !state.currentSample.valid )
	{
		for( unsigned int attempt = 0; attempt < 64; attempt++ )
		{
			state.pSampler->StartIteration();
			state.currentSample = EvaluateSample( scene, camera, *state.pSampler, width, height );
			if( state.currentSample.valid ) {
				state.pSampler->Accept();
				break;
			}
			state.pSampler->Reject();
		}
	}
}

//////////////////////////////////////////////////////////////////////
// RunChainSegment - Run a fixed number of mutations on an existing
// chain.
//
// This is the core Metropolis-Hastings loop, identical in logic to
// the old RunChain but operating on externally-owned ChainState.
// The chain can be paused after this call (for a film snapshot)
// and resumed with another call — the state is fully preserved.
//
// Each iteration:
// 1. PSSMLTSampler.StartIteration() decides large vs small step
// 2. EvaluateSample() evaluates the proposed path through BDPT
// 3. Compute acceptance probability a = min(1, f_proposed/f_current)
// 4. Splat both current and proposed with complementary weights
//    (Veach's expected-value technique)
// 5. Accept or reject based on random draw vs a
//
// SPLATTING MATH:
//   The normalization constant b = b_mean * numPixels where b_mean
//   is the average per-sample luminance from bootstrap.
//   Each splat is weighted by invTotalMutations * b / luminance,
//   where totalMutations is the FULL budget across ALL rounds.
//   This ensures that after all rounds complete, the film contains
//   the correct total accumulation.
//////////////////////////////////////////////////////////////////////

void MLTRasterizer::RunChainSegment(
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
	// invTotalMutations is over the FULL budget, not per-round.
	// This ensures correct accumulation across all rounds.
	const Scalar totalMutations = static_cast<Scalar>( nMutationsPerPixel ) *
		static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar invTotalMutations = 1.0 / totalMutations;

	for( unsigned int m = 0; m < numMutations; m++ )
	{
		// Propose a mutation
		state.pSampler->StartIteration();
		MLTSample proposedSample = EvaluateSample( scene, camera, *state.pSampler, width, height );

		// Compute Metropolis-Hastings acceptance probability.
		// For symmetric mutations (PSSMLT small steps are symmetric),
		// this is simply the ratio of scalar contributions.
		Scalar accept = 0;
		if( state.currentSample.luminance > 0 ) {
			accept = proposedSample.luminance / state.currentSample.luminance;
			if( accept > 1.0 ) {
				accept = 1.0;
			}
		} else if( proposedSample.luminance > 0 ) {
			// Current has zero luminance, always accept non-zero proposal
			accept = 1.0;
		}

		// Splat with expected-value weights (Veach's technique).
		// Each strategy's contribution is splatted to its own pixel
		// position, weighted by the acceptance probability (proposed)
		// or its complement (current).  The weight includes 1/f(x)
		// which cancels the visit frequency of the Markov chain,
		// producing an unbiased image.
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

		// Accept or reject the proposed mutation
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
// RoundThread_ThreadProc - Thread entry point for round-based
// parallel chain execution.  Each thread runs its assigned range
// of chains for the round's mutation budget.
//////////////////////////////////////////////////////////////////////

void* MLTRasterizer::RoundThread_ThreadProc( void* lpParameter )
{
	RoundThreadData* pData = (RoundThreadData*)lpParameter;

	for( unsigned int c = pData->chainStart; c < pData->chainEnd; c++ )
	{
		pData->pRasterizer->RunChainSegment(
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
// PredictTimeToRasterizeScene - stub implementation
//////////////////////////////////////////////////////////////////////

unsigned int MLTRasterizer::PredictTimeToRasterizeScene(
	const IScene& /*pScene*/,
	const ISampling2D& /*pSampling*/,
	unsigned int* pActualTime
	) const
{
	if( pActualTime ) {
		*pActualTime = 0;
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////
// RasterizeScene - Main entry point for MLT rendering.
//
// Orchestrates the full PSSMLT pipeline with progressive rendering:
// 1. Initialize light sampler from scene
// 2. Bootstrap: generate independent samples, estimate normalization
// 3. Build CDF for chain initialization
// 4. Initialize all chain states
// 5. Run rounds: dispatch chains → join → snapshot → output → repeat
// 6. Clean up chain states
//
// The round-based structure ensures that:
// - Markov chains are never interrupted (state persists across rounds)
// - Progress is accurately reported (round / numRounds)
// - Cancellation produces a valid image (last snapshot)
// - Film snapshots are race-free (taken after all threads join)
//////////////////////////////////////////////////////////////////////

void MLTRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* /*pRect*/,
	IRasterizeSequence* /*pRasterSequence*/
	) const
{
	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		GlobalLog()->PrintSourceError( "MLTRasterizer::RasterizeScene:: Scene contains no camera!", __FILE__, __LINE__ );
		return;
	}

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	// Initialize the light sampler from the scene
	LightSampler* pLS = new LightSampler();
	pIntegrator->SetLightSampler( pLS );
	safe_release( pLS );

	pCaster->AttachScene( &pScene );

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Starting PSSMLT render (%ux%u)", width, height );
	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap samples: %u, Chains: %u, Mutations/pixel: %u, Large step prob: %.2f",
		nBootstrap, nChains, nMutationsPerPixel, largeStepProb );

	//////////////////////////////////////////////////////////////////
	// Phase 1: Bootstrap
	//
	// Generate nBootstrap independent BDPT samples.  Each uses a
	// fresh RNG seed and an IndependentSampler.  We record the
	// luminance and seed for each sample.
	//
	// The normalization constant b = mean(luminances) * numPixels.
	//////////////////////////////////////////////////////////////////

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Bootstrap: " );
	}

	std::vector<BootstrapSample> bootstrapSamples( nBootstrap );
	Scalar luminanceSum = 0;

	Timer bootstrapTimer;
	bootstrapTimer.start();

	for( unsigned int i = 0; i < nBootstrap; i++ )
	{
		RandomNumberGenerator bootRNG( i );
		IndependentSampler bootSampler( bootRNG );

		MLTSample sample = EvaluateSample( pScene, *pCamera, bootSampler, width, height );

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

	// Normalization constant b:
	// b_mean = mean luminance per bootstrap sample.  The full
	// normalization is b_mean * numPixels, because the primary
	// sample space integral covers all pixels and we divide by
	// totalMutations (= numPixels * mutationsPerPixel).
	const Scalar b_mean = luminanceSum / static_cast<Scalar>( nBootstrap );

	if( b_mean <= 0 ) {
		GlobalLog()->PrintSourceError( "MLTRasterizer:: Bootstrap found zero luminance -- scene produces no visible light!", __FILE__, __LINE__ );
		return;
	}

	const Scalar numPixels = static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar b = b_mean * numPixels;

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap complete. Mean luminance = %f, Normalization b = %f", b_mean, b );

	//////////////////////////////////////////////////////////////////
	// Phase 1b: Build CDF for importance-sampling initial states
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
	// Phase 2: Initialize all chain states
	//
	// Create PSSMLTSampler and evaluate initial path for each chain.
	// These states persist across all progressive rendering rounds.
	//////////////////////////////////////////////////////////////////

	const unsigned int effectiveChains = nChains > 0 ? nChains : 1;
	const unsigned int totalMutations = nMutationsPerPixel * width * height;
	const unsigned int mutationsPerChain = totalMutations / effectiveChains;

	//////////////////////////////////////////////////////////////////
	// Auto-calculate progressive rounds from bootstrap timing.
	//
	// Each bootstrap sample evaluates one full BDPT path — the same
	// core operation as one mutation.  We time the bootstrap phase
	// (single-threaded) and use it to estimate how long the rendering
	// phase will take per mutation (multi-threaded).
	//
	// Target: ~5 seconds per round, so the user gets frequent
	// progressive updates without excessive snapshot overhead.
	//////////////////////////////////////////////////////////////////

	int threads = HowManyThreadsToSpawn();
	static const int MAX_THREADS = 10000;
	if( threads > MAX_THREADS ) {
		threads = MAX_THREADS;
	}

	unsigned int numRounds = 1;
	const unsigned int bootstrapMs = bootstrapTimer.getInterval();

	if( bootstrapMs > 0 && nBootstrap > 0 )
	{
		// Time per single-threaded sample (milliseconds)
		const double msPerSample = static_cast<double>( bootstrapMs ) / static_cast<double>( nBootstrap );

		// Estimated total render time in ms (divide by thread count for parallelism)
		const int effectiveThreads = threads > 0 ? threads : 1;
		const double estTotalMs = msPerSample * static_cast<double>( totalMutations ) / static_cast<double>( effectiveThreads );

		// Target ~5000ms per round
		const double targetRoundMs = 5000.0;
		const unsigned int computedRounds = static_cast<unsigned int>( estTotalMs / targetRoundMs + 0.5 );

		// Clamp to reasonable range: at least 2 rounds (so we get at
		// least one intermediate update), at most totalMutations/chains
		// (each round must have at least 1 mutation per chain).
		numRounds = computedRounds < 2 ? 2 : computedRounds;
		if( numRounds > mutationsPerChain ) {
			numRounds = mutationsPerChain;
		}

		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap took %u ms for %u samples (%.3f ms/sample)",
			bootstrapMs, nBootstrap, msPerSample );
		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Estimated total render time: %.1f s, targeting %.1f s/round -> %u rounds",
			estTotalMs / 1000.0, targetRoundMs / 1000.0, numRounds );
	}
	else
	{
		// Bootstrap was too fast to measure — use a reasonable default
		numRounds = 10;
		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap too fast to time, using %u rounds", numRounds );
	}

	const unsigned int mutationsPerChainPerRound = mutationsPerChain / numRounds;

	// Ensure at least 1 mutation per chain per round
	const unsigned int effectiveMutPerRound = mutationsPerChainPerRound > 0 ? mutationsPerChainPerRound : 1;

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Total mutations: %u, Per chain: %u, Per chain per round: %u, Rounds: %u",
		totalMutations, mutationsPerChain, effectiveMutPerRound, numRounds );

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Init Chains: " );
	}

	std::vector<ChainState> chainStates( effectiveChains );

	for( unsigned int c = 0; c < effectiveChains; c++ )
	{
		// Select initial state from bootstrap CDF
		RandomNumberGenerator selRNG( c * 31337 );
		const Scalar u = selRNG.CanonicalRandom();
		const unsigned int bootstrapIdx = SelectFromCDF( cdf, u );
		const BootstrapSample& seed = bootstrapSamples[bootstrapIdx];

		InitChain( chainStates[c], pScene, *pCamera, seed, width, height );

		if( pProgressFunc && (c % 10 == 0) ) {
			if( !pProgressFunc->Progress( static_cast<double>(c), static_cast<double>(effectiveChains) ) ) {
				// Clean up on cancel
				for( unsigned int j = 0; j <= c; j++ ) {
					safe_release( chainStates[j].pSampler );
				}
				return;
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 3: Round-based rendering
	//
	// Each round:
	// 1. Dispatch chains to threads for effectiveMutPerRound mutations
	// 2. Wait for all threads to finish
	// 3. Resolve the film to a fresh image, scaling by the fraction
	//    of total work completed: sampleCount = (r+1) / numRounds
	// 4. Output the progressive image
	// 5. Report progress
	//
	// The scaling math: each splat is weighted by 1/totalMutations.
	// After round r+1 of R, approximately ((r+1)/R) of all mutations
	// have run, so the film holds ((r+1)/R) of the final value.
	// Dividing by sampleCount = (r+1)/R gives the correct image.
	//////////////////////////////////////////////////////////////////

	SplatFilm* pSplatFilm = new SplatFilm( width, height );
	pSplatFilm->addref();

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Rendering: " );
	}

	bool cancelled = false;
    
    // Before starting update the progress function because each round takes time
    // Report progress and check for cancellation
    if( pProgressFunc ) {
        if( !pProgressFunc->Progress( 0, static_cast<double>(numRounds) ) ) {
            cancelled = true;
        }
    }

	for( unsigned int round = 0; round < numRounds && !cancelled; round++ )
	{
		// For the last round, use remaining mutations to avoid rounding loss
		const unsigned int mutThisRound = ( round == numRounds - 1 )
			? ( mutationsPerChain - effectiveMutPerRound * ( numRounds - 1 ) )
			: effectiveMutPerRound;

		if( threads > 1 )
		{
			// Multi-threaded: distribute chains across threads
			const unsigned int chainsPerThread = effectiveChains / threads;

			std::vector<RoundThreadData> threadData( threads );
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

				Threading::riseCreateThread( RoundThread_ThreadProc, (void*)&threadData[t], 0, 0, &thread_ids[t] );
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
				RunChainSegment( chainStates[c], pScene, *pCamera, *pSplatFilm,
					mutThisRound, b, width, height );
			}
		}

		//////////////////////////////////////////////////////////////
		// Snapshot: resolve the film and send intermediate update
		//
		// sampleCount = fraction of total mutations completed.
		// This scales up the partially-accumulated film to produce
		// a correctly-exposed image at any stage.
		//
		// Between rounds we call OutputIntermediateImage so outputs
		// can display a preview without writing the final file.
		// OutputImage is called only once: after all rounds finish,
		// or when the user cancels (so the last valid image is saved).
		//////////////////////////////////////////////////////////////

		const Scalar fraction = static_cast<Scalar>( round + 1 ) / static_cast<Scalar>( numRounds );
		const bool isFinalRound = ( round + 1 == numRounds );

		IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 1.0 ) );
		pSplatFilm->Resolve( *pImage, fraction );

		if( !isFinalRound )
		{
			// Intermediate round: send preview update only
			RasterizerOutputListType::const_iterator r, s;
			for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
				(*r)->OutputIntermediateImage( *pImage, 0 );
			}
		}

		// Report progress and check for cancellation
		if( pProgressFunc ) {
			if( !pProgressFunc->Progress( static_cast<double>(round + 1), static_cast<double>(numRounds) ) ) {
				cancelled = true;
			}
		}

		// Write the final file on completion or cancellation.
		// On cancellation we still output so the user gets a
		// valid image at whatever quality was reached.
		if( isFinalRound || cancelled )
		{
			RasterizerOutputListType::const_iterator r, s;
			for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
				(*r)->OutputImage( *pImage, 0, 0 );
			}
		}

		safe_release( pImage );
	}

	//////////////////////////////////////////////////////////////////
	// Phase 4: Cleanup
	//////////////////////////////////////////////////////////////////

	RISE_PROFILE_REPORT(GlobalLog());

	if( cancelled ) {
		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Rendering cancelled by user" );
	} else {
		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Rendering complete" );
	}

	// Release all chain states
	for( unsigned int c = 0; c < effectiveChains; c++ ) {
		safe_release( chainStates[c].pSampler );
	}

	safe_release( pSplatFilm );
}
