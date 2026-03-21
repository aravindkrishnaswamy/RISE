//////////////////////////////////////////////////////////////////////
//
//  MLTRasterizer.cpp - Implementation of the PSSMLT rasterizer.
//
//  ALGORITHM OVERVIEW:
//    This file implements the complete PSSMLT rendering pipeline:
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
//    2. Chain Execution Phase:
//       Launch nChains independent Markov chains, distributed across
//       available threads.  Each chain:
//       (a) Picks an initial state from the bootstrap CDF
//       (b) Creates a PSSMLTSampler seeded from that state
//       (c) Evaluates BDPT to get the initial path
//       (d) Runs mutationsPerChain iterations of Metropolis-Hastings:
//           - PSSMLTSampler proposes a mutation (large or small step)
//           - BDPT evaluates the proposed path
//           - Acceptance probability a = min(1, f_proposed/f_current)
//           - Both paths contribute to the film with weights:
//             proposed: a * weight * b / f_proposed
//             current:  (1-a) * weight * b / f_current
//             This is Veach's "expected values" technique, which
//             ensures unbiased results regardless of accept/reject.
//           - Accept with probability a; reject otherwise
//
//    3. Resolve Phase:
//       The SplatFilm accumulated all contributions with pre-baked
//       Metropolis weights.  We resolve it with sampleCount=1.0
//       to add the results to the output image.
//
//  THREADING:
//    Chains are dispatched to threads using the same pattern as
//    BDPTRasterizer's block dispatch.  Each thread runs a contiguous
//    range of chains independently.  SplatFilm's row-level mutexes
//    handle concurrent writes to the same scanline.
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
#include "../Utilities/PSSMLTSampler.h"
#include "../Utilities/IndependentSampler.h"
#include "../RasterImages/RasterImage.h"
#include "../Utilities/Profiling.h"

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
// 5. Sum contributions and return the aggregate result
//
// The key insight: because PSSMLTSampler records/replays the sample
// vector, the same sequence of Get1D()/Get2D() calls always maps
// to the same path.  Mutating the vector explores nearby paths.
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

	// Use the first 2D sample to pick a film position.
	// This means mutations to these two values move the path
	// to a different pixel -- important for film coverage.
	const Point2 filmSample = sampler.Get2D();
	const unsigned int px = std::min( static_cast<unsigned int>( filmSample.x * width ), width - 1 );
	const unsigned int py = std::min( static_cast<unsigned int>( filmSample.y * height ), height - 1 );
	const Point2 screenPos( static_cast<Scalar>(px), static_cast<Scalar>(height - 1 - py) );

	result.rasterPos = Point2( static_cast<Scalar>(px), static_cast<Scalar>(py) );

	// Generate camera ray for this pixel
	RandomNumberGenerator localRNG( px * 65537 + py );
	RuntimeContext rc( localRNG, RuntimeContext::PASS_NORMAL, false );

	Ray cameraRay;
	if( !camera.GenerateRay( rc, cameraRay, screenPos ) ) {
		return result;
	}

	// Generate light and eye subpaths using the MLT sampler.
	// BDPTIntegrator consumes samples from the sampler in a
	// deterministic order, which is exactly what PSSMLT requires.
	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;

	pIntegrator->GenerateLightSubpath( scene, *pCaster, sampler, lightVerts );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts );

	// Evaluate all (s,t) connection strategies via MIS
	std::vector<BDPTIntegrator::ConnectionResult> results =
		pIntegrator->EvaluateAllStrategies( lightVerts, eyeVerts, scene, *pCaster, camera );

	// Sum contributions from all strategies.
	// For s<=1 strategies that naturally splat to different pixels,
	// we still sum them here -- in MLT all contributions from a
	// given sample vector are treated as one aggregate sample.
	RISEPel totalColor( 0, 0, 0 );

	for( unsigned int r = 0; r < results.size(); r++ )
	{
		const BDPTIntegrator::ConnectionResult& cr = results[r];
		if( !cr.valid ) {
			continue;
		}

		const RISEPel weighted = cr.contribution * cr.misWeight;
		totalColor = totalColor + weighted;
	}

	// Clamp negative components that can arise from numerical precision
	// issues in MIS weight calculations (extremely rare, very small values).
	for( int ch = 0; ch < 3; ch++ ) {
		if( totalColor[ch] < 0 ) {
			totalColor[ch] = 0;
		}
	}

	// Compute scalar luminance for the acceptance ratio.
	// Using standard Rec.709 luminance weights.
	result.luminance = 0.2126 * totalColor[0] + 0.7152 * totalColor[1] + 0.0722 * totalColor[2];
	result.color = totalColor;
	result.valid = ( result.luminance > 0 );

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
// RunChain - Execute a single Markov chain.
//
// This is the core Metropolis-Hastings loop.  Each iteration:
// 1. PSSMLTSampler.StartIteration() decides large vs small step
// 2. EvaluateSample() evaluates the proposed path through BDPT
// 3. Compute acceptance probability a = min(1, f_proposed/f_current)
// 4. Splat both current and proposed with complementary weights
//    (Veach's expected-value technique -- ensures unbiased image
//    even though we're randomly accepting/rejecting)
// 5. Accept or reject based on random draw vs a
//
// SPLATTING MATH:
//   The normalization constant b = b_mean * numPixels where b_mean
//   is the average per-sample luminance from bootstrap.  This accounts
//   for the Metropolis estimator integrating over the full primary
//   sample space (which includes uniform pixel selection).
//
//   For each mutation, the contribution to the film is:
//     weight = (1/totalMutations) * b / luminance * accept_factor
//   where totalMutations = nMutationsPerPixel * numPixels.
//
//   Expected per-pixel value:
//     E[I_p] = (b/M) * M * P(at p) * E[color/lum | at p]
//            = b * (L_p / total_lum) * (color_p / L_p)
//            = b * color_p / total_lum
//            = (b_mean * W*H) * color_p / (b_mean * W*H) = color_p  ✓
//////////////////////////////////////////////////////////////////////

void MLTRasterizer::RunChain(
	const IScene& scene,
	const ICamera& camera,
	SplatFilm& splatFilm,
	const unsigned int chainMutations,
	const BootstrapSample& initialSeed,
	const Scalar normalization,
	const unsigned int width,
	const unsigned int height
	) const
{
	// Create the PSSMLT sampler for this chain, seeded from the
	// bootstrap sample that was selected by importance sampling.
	PSSMLTSampler* pSampler = new PSSMLTSampler( initialSeed.seed, largeStepProb );
	pSampler->addref();

	// Evaluate the initial state by consuming samples from the
	// sampler in its initial (large-step) configuration.
	pSampler->StartIteration();
	MLTSample currentSample = EvaluateSample( scene, camera, *pSampler, width, height );
	pSampler->Accept();

	// If the initial state has zero luminance, try a few more large steps
	// to find a valid starting point
	if( !currentSample.valid )
	{
		for( unsigned int attempt = 0; attempt < 64; attempt++ )
		{
			pSampler->StartIteration();
			currentSample = EvaluateSample( scene, camera, *pSampler, width, height );
			if( currentSample.valid ) {
				pSampler->Accept();
				break;
			}
			pSampler->Reject();
		}
	}

	// Per-mutation weight: each mutation contributes 1/totalMutations
	// of the image, scaled by the normalization constant b.
	const Scalar totalMutations = static_cast<Scalar>( nMutationsPerPixel ) *
		static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar invTotalMutations = 1.0 / totalMutations;

	RandomNumberGenerator chainRNG( initialSeed.seed + 1000000 );

	for( unsigned int m = 0; m < chainMutations; m++ )
	{
		// Propose a mutation
		pSampler->StartIteration();
		MLTSample proposedSample = EvaluateSample( scene, camera, *pSampler, width, height );

		// Compute Metropolis-Hastings acceptance probability.
		// For symmetric mutations (PSSMLT small steps are symmetric),
		// this is simply the ratio of scalar contributions.
		Scalar accept = 0;
		if( currentSample.luminance > 0 ) {
			accept = proposedSample.luminance / currentSample.luminance;
			if( accept > 1.0 ) {
				accept = 1.0;
			}
		} else if( proposedSample.luminance > 0 ) {
			// Current has zero luminance, always accept non-zero proposal
			accept = 1.0;
		}

		// Splat with expected-value weights (Veach's technique).
		// This contributes both paths proportionally to how much time
		// the chain "spends" at each state, producing an unbiased image.
		if( proposedSample.valid && proposedSample.luminance > 0 )
		{
			// Proposed contribution: weighted by acceptance probability
			const Scalar proposedWeight = accept * invTotalMutations * normalization / proposedSample.luminance;
			const RISEPel proposedSplat = proposedSample.color * proposedWeight;

			const unsigned int sx = static_cast<unsigned int>( proposedSample.rasterPos.x );
			const unsigned int sy = static_cast<unsigned int>( proposedSample.rasterPos.y );

			if( sx < width && sy < height ) {
				splatFilm.Splat( sx, sy, proposedSplat );
			}
		}

		if( currentSample.valid && currentSample.luminance > 0 )
		{
			// Current contribution: weighted by rejection probability (1-a)
			const Scalar currentWeight = ( 1.0 - accept ) * invTotalMutations * normalization / currentSample.luminance;
			const RISEPel currentSplat = currentSample.color * currentWeight;

			const unsigned int sx = static_cast<unsigned int>( currentSample.rasterPos.x );
			const unsigned int sy = static_cast<unsigned int>( currentSample.rasterPos.y );

			if( sx < width && sy < height ) {
				splatFilm.Splat( sx, sy, currentSplat );
			}
		}

		// Accept or reject the proposed mutation
		if( chainRNG.CanonicalRandom() < accept )
		{
			pSampler->Accept();
			currentSample = proposedSample;
		}
		else
		{
			pSampler->Reject();
		}
	}

	safe_release( pSampler );
}

//////////////////////////////////////////////////////////////////////
// ChainThread_ThreadProc - Thread entry point for parallel chain
// execution.  Each thread runs its assigned range of chains.
//////////////////////////////////////////////////////////////////////

void* MLTRasterizer::ChainThread_ThreadProc( void* lpParameter )
{
	ChainThreadData* pData = (ChainThreadData*)lpParameter;

	for( unsigned int c = pData->chainStart; c < pData->chainEnd; c++ )
	{
		// Select initial state from bootstrap CDF
		RandomNumberGenerator threadRNG( c * 31337 );
		const Scalar u = threadRNG.CanonicalRandom();
		const unsigned int bootstrapIdx = pData->pRasterizer->SelectFromCDF( *pData->pCDF, u );
		const BootstrapSample& seed = (*pData->pBootstrapSamples)[bootstrapIdx];

		pData->pRasterizer->RunChain(
			*pData->pScene,
			*pData->pScene->GetCamera(),
			*pData->pSplatFilm,
			pData->mutationsPerChain,
			seed,
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
// Orchestrates the full PSSMLT pipeline:
// 1. Initialize light sampler from scene
// 2. Bootstrap: generate independent samples, estimate normalization
// 3. Build CDF for chain initialization
// 4. Dispatch chains to threads
// 5. Resolve splat film to output image
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
	// The normalization constant b = mean(luminances) represents
	// the expected luminance of a random pixel.  This is used to
	// correctly weight the Metropolis contributions so the final
	// image brightness matches the ground truth.
	//////////////////////////////////////////////////////////////////

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Bootstrap: " );
	}

	std::vector<BootstrapSample> bootstrapSamples( nBootstrap );
	Scalar luminanceSum = 0;

	for( unsigned int i = 0; i < nBootstrap; i++ )
	{
		RandomNumberGenerator bootRNG( i );
		IndependentSampler* pBootSampler = new IndependentSampler( bootRNG );
		pBootSampler->addref();

		MLTSample sample = EvaluateSample( pScene, *pCamera, *pBootSampler, width, height );

		safe_release( pBootSampler );

		bootstrapSamples[i].luminance = sample.luminance;
		bootstrapSamples[i].seed = i;
		luminanceSum += sample.luminance;

		if( pProgressFunc && (i % 1000 == 0) ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(nBootstrap) ) ) {
				return;
			}
		}
	}

	// Normalization constant b:
	// b_mean = mean luminance per bootstrap sample (= mean path contribution
	// when pixel is chosen uniformly).  The full normalization for the
	// Metropolis estimator is b_mean * numPixels, because the primary
	// sample space integral covers all pixels and we divide by totalMutations
	// (= numPixels * mutationsPerPixel).  This ensures each pixel gets
	// its correct expected value:
	//   E[I_p] = (b*W*H / totalMutations) * totalMutations * P(pixel=p) * E[color/lum | p]
	//          = b*W*H * (L_p / (b*W*H)) * (color_p / L_p) = color_p  ✓
	const Scalar b_mean = luminanceSum / static_cast<Scalar>( nBootstrap );

	if( b_mean <= 0 ) {
		GlobalLog()->PrintSourceError( "MLTRasterizer:: Bootstrap found zero luminance -- scene produces no visible light!", __FILE__, __LINE__ );
		return;
	}

	const Scalar numPixels = static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	const Scalar b = b_mean * numPixels;

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap complete. Mean luminance = %f, Normalization b = %f", b_mean, b );

	//////////////////////////////////////////////////////////////////
	// Phase 1b: Build CDF for importance-sampling initial chain states
	//
	// Higher-luminance bootstrap samples are more likely to be
	// selected as chain starting points, which means chains start
	// in productive regions of path space.
	//////////////////////////////////////////////////////////////////

	std::vector<Scalar> cdf( nBootstrap );
	cdf[0] = bootstrapSamples[0].luminance;
	for( unsigned int i = 1; i < nBootstrap; i++ ) {
		cdf[i] = cdf[i-1] + bootstrapSamples[i].luminance;
	}

	// Normalize the CDF to [0,1]
	if( cdf[nBootstrap-1] > 0 ) {
		const Scalar invTotal = 1.0 / cdf[nBootstrap-1];
		for( unsigned int i = 0; i < nBootstrap; i++ ) {
			cdf[i] *= invTotal;
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 2: Markov Chain Execution
	//
	// Total mutations = nMutationsPerPixel * width * height
	// Distributed evenly across nChains chains, each running on
	// a thread.
	//////////////////////////////////////////////////////////////////

	const unsigned int totalMutations = nMutationsPerPixel * width * height;
	const unsigned int mutationsPerChain = totalMutations / nChains;

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Total mutations: %u, Per chain: %u", totalMutations, mutationsPerChain );

	// Create splat film
	SplatFilm* pSplatFilm = new SplatFilm( width, height );
	pSplatFilm->addref();

	// Create output image
	// Initialize image with zero color but alpha=1.0, so the PNG writer
	// doesn't treat pixels as transparent (Resolve adds splat color but
	// preserves the existing alpha value).
	IRasterImage* pImage = new RISERasterImage( width, height, RISEColor( 0, 0, 0, 1.0 ) );
	GlobalLog()->PrintNew( pImage, __FILE__, __LINE__, "MLT image" );

	if( pProgressFunc ) {
		pProgressFunc->SetTitle( "MLT Rendering: " );
	}

	int threads = HowManyThreadsToSpawn();
	static const int MAX_THREADS = 10000;
	if( threads > MAX_THREADS ) {
		threads = MAX_THREADS;
	}

	// Ensure we have at least as many chains as threads
	const unsigned int effectiveChains = nChains > 0 ? nChains : 1;

	if( threads > 1 )
	{
		// Multi-threaded: distribute chains across threads
		const unsigned int chainsPerThread = effectiveChains / threads;

		std::vector<ChainThreadData> threadData( threads );
		RISETHREADID* thread_ids = new RISETHREADID[threads];

		for( int t = 0; t < threads; t++ )
		{
			threadData[t].pRasterizer = this;
			threadData[t].pScene = &pScene;
			threadData[t].pSplatFilm = pSplatFilm;
			threadData[t].pBootstrapSamples = &bootstrapSamples;
			threadData[t].pCDF = &cdf;
			threadData[t].normalization = b;
			threadData[t].chainStart = t * chainsPerThread;
			threadData[t].chainEnd = ( t == threads - 1 ) ? effectiveChains : ( t + 1 ) * chainsPerThread;
			threadData[t].mutationsPerChain = mutationsPerChain;
			threadData[t].width = width;
			threadData[t].height = height;

			Threading::riseCreateThread( ChainThread_ThreadProc, (void*)&threadData[t], 0, 0, &thread_ids[t] );
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
			RandomNumberGenerator selRNG( c * 31337 );
			const Scalar u = selRNG.CanonicalRandom();
			const unsigned int bootstrapIdx = SelectFromCDF( cdf, u );
			const BootstrapSample& seed = bootstrapSamples[bootstrapIdx];

			RunChain( pScene, *pCamera, *pSplatFilm, mutationsPerChain, seed, b, width, height );

			if( pProgressFunc ) {
				if( !pProgressFunc->Progress( static_cast<double>(c), static_cast<double>(effectiveChains) ) ) {
					break;
				}
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	// Phase 3: Resolve
	//
	// The splat film contains pre-weighted contributions.  Resolve
	// with sampleCount=1.0 since the Metropolis weights are already
	// baked into each splat.
	//////////////////////////////////////////////////////////////////

	pSplatFilm->Resolve( *pImage, 1.0 );

	RISE_PROFILE_REPORT(GlobalLog());

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Rendering complete" );

	// Output final image
	{
		RasterizerOutputListType::const_iterator r, s;
		for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
			(*r)->OutputImage( *pImage, 0, 0 );
		}
	}

	safe_release( pImage );
	safe_release( pSplatFilm );
}
