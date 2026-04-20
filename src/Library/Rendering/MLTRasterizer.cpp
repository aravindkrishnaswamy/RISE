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
#include "../RasterImages/RasterImage.h"
#include "../Utilities/Profiling.h"
#include "../Utilities/RTime.h"
#include "../Utilities/ThreadPool.h"
#include "ThreadLocalSplatBuffer.h"
#include "../Interfaces/IOptions.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Interfaces/ISampling2D.h"
#include "../Cameras/ThinLensCamera.h"
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
  pSampling( 0 ),
  pPixelFilter( 0 ),
  nBootstrap( nBootstrap_ ),
  nChains( nChains_ ),
  nMutationsPerPixel( nMutationsPerPixel_ ),
  largeStepProb( largeStepProb_ )
{
	if( pCaster ) {
		pCaster->addref();
	}

	pIntegrator = new BDPTIntegrator( maxEyeDepth, maxLightDepth, StabilityConfig() );
}

MLTRasterizer::~MLTRasterizer()
{
	safe_release( pPixelFilter );
	safe_release( pSampling );
	safe_release( pIntegrator );
	safe_release( pCaster );
}

bool MLTRasterizer::GenerateCameraRayWithLensSample(
	const ICamera& camera,
	const RuntimeContext& rc,
	Ray& ray,
	const Point2& ptOnScreen,
	const Point2& lensSample )
{
	// dynamic_cast to the concrete thin-lens camera.  If the concrete
	// type matches, use its non-virtual lens-sample entry point so
	// the PSSMLT lensSample drives aperture sampling CONTINUOUSLY
	// (small Markov mutation → small aperture move).  For every
	// other camera (pinhole, fisheye, orthographic, any out-of-tree
	// implementation), fall through to the standard GenerateRay
	// path — those cameras either don't have an aperture to sample
	// or do their own thing, and we MUST NOT touch their vtable.
	if( const ThinLensCamera* thinLens =
			dynamic_cast<const ThinLensCamera*>( &camera ) )
	{
		return thinLens->GenerateRayWithLensSample(
			rc, ray, ptOnScreen, lensSample );
	}
	return camera.GenerateRay( rc, ray, ptOnScreen );
}

//////////////////////////////////////////////////////////////////////
// SubSampleRays — install the sampler and pixel filter produced by
// Job::GetSamplingAndFilterElements.  MLT never reads from pSampling
// (the Markov chain picks its own film positions), but we store it
// for API symmetry with the rest of the rasterizer family so future
// code doesn't reach for a null pointer.  pPixelFilter IS consumed:
// every splat is distributed across the filter footprint in
// RunChainSegment, which is the entire point of this hook.
//////////////////////////////////////////////////////////////////////

void MLTRasterizer::SubSampleRays( ISampling2D* pSampling_, IPixelFilter* pPixelFilter_ )
{
	if( pSampling_ )
	{
		safe_release( pSampling );
		pSampling = pSampling_;
		pSampling->addref();
	}

	if( pPixelFilter_ )
	{
		safe_release( pPixelFilter );
		pPixelFilter = pPixelFilter_;
		pPixelFilter->addref();
	}
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

	// Stream 48: film position samples.  Must not conflict with
	// BDPTIntegrator's internal streams (0-47).
	sampler.StartStream( 48 );

	// Use the first 2D sample to pick a film position.
	// This means mutations to these two values move the path
	// to a different pixel -- important for film coverage.
	//
	// Keep fractional coordinates end-to-end: the previous code
	// truncated filmSample to integer px/py, which (a) made sub-
	// pixel mutations visit the same pixel center over and over,
	// (b) quantised the camera ray origin to pixel corners so the
	// pixel filter could not reconstruct sub-pixel detail, and
	// (c) produced the pixel-aligned hard edges the user reported.
	//
	// The -0.5 offset is the RISE convention for pixel centers:
	// BoxPixelFilter::warpOnScreen returns `canonical.x + x - 0.5`
	// for canonical ∈ [0,1), so pixel i's CENTER is at integer x=i.
	// FilteredFilm::Splat (and our SplatFilm::SplatFiltered) read
	// `dx = px - screenX`, which only evaluates the filter correctly
	// when the caller's screenX uses the same pixel-center-at-integer
	// convention.  Without this offset, MLT's camera rays and splats
	// land half a pixel toward the +X/+Y corner of each pixel
	// relative to the other rasterizers — visible as a consistent
	// shift in side-by-side comparisons.
	//
	// Ranges after the offset:
	//   fx ∈ [-0.5, W - 0.5)      fy ∈ [-0.5, H - 0.5)
	// The round-to-nearest fallback in RunChainSegment guards fx<-0.5
	// and fy<-0.5 (both impossible for filmSample ∈ [0,1), but the
	// guard survives any future filmSample pathology from the PSSMLT
	// sampler as well).
	const Point2 filmSample = sampler.Get2D();
	// The lens sample is consumed from the SAME stream so it
	// advances the PSSMLT primary-sample vector by two more
	// dimensions.  Because PSSMLT mutates each primary sample
	// independently, the film position and the lens position
	// now evolve as independent dimensions of the Markov chain
	// — small film mutations leave the lens fixed, small lens
	// mutations leave the film fixed, and large steps resample
	// both.  This is what makes thin-lens MLT converge: without
	// it, aperture samples were tied to the RNG seed rather than
	// to the PSSMLT state, so lens rotations could only happen
	// as a side effect of film jitter.
	const Point2 lensSample = sampler.Get2D();
	const Scalar fx = filmSample.x * static_cast<Scalar>( width  ) - static_cast<Scalar>( 0.5 );
	const Scalar fy = filmSample.y * static_cast<Scalar>( height ) - static_cast<Scalar>( 0.5 );
	// screenPos in RISE screen convention (y=0 at bottom, fractional).
	const Point2 screenPos( fx, static_cast<Scalar>( height ) - fy );
	// cameraRasterPos in image-buffer convention (y=0 at top, fractional).
	const Point2 cameraRasterPos( fx, fy );

	// Seed the camera-ray RNG from the FILM sample bits (used only
	// for degenerate fall-through paths through the camera; the
	// PSSMLT-driven lens sample goes via GenerateRayWithLensSample
	// so a small Markov mutation produces a small, CONTINUOUS
	// aperture move).  Hashing the film sample bits ensures two
	// film positions that happen to land in the same integer pixel
	// still get different local RNG states — without that, any
	// non-lens random the camera happened to consume would reuse
	// the same stream across sub-pixel mutations.
	const unsigned int fxBits = static_cast<unsigned int>(
		filmSample.x * static_cast<Scalar>( 4294967296.0 ) );
	const unsigned int fyBits = static_cast<unsigned int>(
		filmSample.y * static_cast<Scalar>( 4294967296.0 ) );
	RandomNumberGenerator localRNG( fxBits * 2654435761u + fyBits );
	RuntimeContext rc( localRNG, RuntimeContext::PASS_NORMAL, false );

	Ray cameraRay;
	// Route through the anonymous-namespace helper instead of calling
	// a virtual method on ICamera.  The helper dynamic_casts to
	// ThinLensCamera and uses its non-virtual lens-sample method when
	// applicable, falling back to GenerateRay for every other camera.
	// Keeping this off the ICamera vtable is what protects ABI for
	// out-of-tree camera implementations compiled against an older
	// interface — their vtables are unchanged.
	if( !GenerateCameraRayWithLensSample( camera, rc, cameraRay, screenPos, lensSample ) ) {
		return result;
	}

	// Generate light and eye subpaths using the MLT sampler.
	// BDPTIntegrator manages its own streams internally (0-47).
	std::vector<BDPTVertex> lightVerts;
	std::vector<BDPTVertex> eyeVerts;
	std::vector<uint32_t> lightSubpathStarts;
	std::vector<uint32_t> eyeSubpathStarts;

	// MLT's Markov-chain proposal measure requires a single subpath
	// per proposal — PSSMLT's primary-sample vector can't mutate
	// branch choices at delta vertices.  Force threshold=1.0 on both
	// sides permanently.
	pIntegrator->GenerateLightSubpath( scene, *pCaster, sampler, lightVerts, lightSubpathStarts, rc.random, Scalar( 1.0 ) );
	pIntegrator->GenerateEyeSubpath( rc, cameraRay, screenPos, scene, *pCaster, sampler, eyeVerts, eyeSubpathStarts, Scalar( 1.0 ) );

	// Evaluate all (s,t) connection strategies via MIS
	std::vector<BDPTIntegrator::ConnectionResult> results =
		pIntegrator->EvaluateAllStrategies(
			lightVerts,
			eyeVerts,
			scene,
			*pCaster,
			camera,
			&sampler );

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

		// Do NOT clamp negative channels to zero.  A previous version
		// did so "to handle numerical precision", but that silently
		// converts a near-zero contribution with a slightly-negative
		// channel (say R = -0.003, G = +0.03, B = +0.02) into a
		// positive-only contribution (R = 0, G = +0.03, B = +0.02).
		// When the MLT chain explores paths where a particular
		// strategy consistently produces this pattern, every visit
		// deposits the inflated contribution at the same pixel.  The
		// result is a SYSTEMATIC firefly that does not average out —
		// it grows over rounds as chain clustering gives it more
		// splats.  BDPT does not clamp (it lets negatives cancel in
		// the pixel sum), and once MLT matches that behaviour the
		// firefly goes away.
		//
		// We still skip strategies with non-positive total luminance
		// so the splat weight (which divides by totalLuminance) stays
		// well-defined.  A strategy with lum ≤ 0 cannot splat useful
		// energy regardless.
		const Scalar lum = 0.2126 * weighted[0] + 0.7152 * weighted[1] + 0.0722 * weighted[2];
		if( lum <= 0 ) {
			continue;
		}

		MLTStrategySplat splat;
		splat.color = weighted;

		if( cr.needsSplat ) {
			// t<=1 strategies: use the strategy's own pixel position.
			// Rasterize returns screen coordinates (y=0 at bottom);
			// convert to image buffer coordinates (y=0 at top).
			splat.rasterPos = Point2( cr.rasterPos.x,
				static_cast<Scalar>(height) - cr.rasterPos.y );
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
	const unsigned int chainIndex,
	const unsigned int width,
	const unsigned int height
	) const
{
	// Two-phase initialization that BOTH preserves the bootstrap CDF's
	// importance sampling AND keeps chains independent.
	//
	// Phase 1: seed PSSMLTSampler with seed.seed (the bootstrap index)
	// so its first iteration EXACTLY reproduces the bootstrap path the
	// CDF selected — which we know has high luminance.  This is what
	// makes "start at a bootstrap-selected high-luminance path" actually
	// true in code (the previous version hashed chainIndex into the
	// sampler seed up front, so the chain's first iteration generated
	// an arbitrary uniform-random path with no relationship to the
	// bootstrap path; the CDF's importance sampling was silently
	// thrown away even though the comment claimed otherwise).
	//
	// Phase 2: after the first Accept(), re-seed the PSSMLTSampler's
	// internal RNG to a chain-specific value.  The X vector (which
	// holds the bootstrap path's primary samples) is preserved, so the
	// chain's CURRENT state at iteration 1 is the bootstrap path.
	// Subsequent mutations use the new RNG, so two chains that picked
	// the same bootstrap index immediately diverge via different
	// proposals — no more identical Markov trajectories.
	//
	// chainRNG (used only by the host's accept/reject draw) is
	// chain-specific from the start, providing additional divergence.
	const unsigned int chainSeed     = seed.seed * 2654435761u + chainIndex;
	const unsigned int proposalSeed  = chainSeed ^ 0xA55A5AA5u;
	state.pSampler = new PSSMLTSampler( seed.seed, largeStepProb );
	state.chainRNG = RandomNumberGenerator( chainSeed );

	// Phase 1: reproduce the bootstrap path as the chain's iteration 1.
	state.pSampler->StartIteration();
	state.currentSample = EvaluateSample( scene, camera, *state.pSampler, width, height );
	state.pSampler->Accept();

	// Phase 2: re-seed the proposal RNG so subsequent mutations diverge
	// per chain.  Done AFTER Accept() so iteration 1 keeps the bootstrap
	// path; the X vector (already populated with the bootstrap path's
	// canonical randoms) is untouched.
	state.pSampler->ReSeedRNG( proposalSeed );

	// If the bootstrap path turns out invalid (rare, but possible when
	// the CDF tail is tiny), try additional large steps to find a valid
	// state.  These now use the chain-specific RNG so they don't
	// duplicate other chains' recovery sequences.
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

				if( pPixelFilter ) {
					// Fractional, filter-reconstructed splat.  Distributes
					// the contribution across the filter footprint so
					// neighbouring pixels blend smoothly and aliasing
					// disappears.
					splatFilm.SplatFiltered( s.rasterPos.x, s.rasterPos.y,
						splatColor, *pPixelFilter );
				} else {
					// No filter installed — fall back to round-to-nearest
					// point splat.  floor() here (via unsigned cast on a
					// truncation) would introduce a consistent half-pixel
					// bias; adding 0.5 before truncation rounds instead.
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

	// AttachScene creates and Prepare()s the unified LightSampler
	pCaster->AttachScene( &pScene );
	pScene.GetObjects()->PrepareForRendering();

	// Share the RayCaster's prepared LightSampler with the integrator
	const LightSampler* pLS = pCaster->GetLightSampler();
	pIntegrator->SetLightSampler( pLS );

	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Starting PSSMLT render (%ux%u)", width, height );
	GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap samples: %u, Chains: %u, Mutations/pixel: %u, Large step prob: %.2f",
		nBootstrap, nChains, nMutationsPerPixel, largeStepProb );

	//////////////////////////////////////////////////////////////////
	// Phase 1: Bootstrap
	//
	// Generate nBootstrap independent BDPT samples.  Each uses a
	// fresh PSSMLTSampler seed.  We record the luminance and seed
	// for each sample.  The same seed re-creates the same path in
	// InitChain so the CDF's importance sampling is preserved.
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

	// Bootstrap uses a PSSMLTSampler (not IndependentSampler) so the
	// path generated for seed i can be EXACTLY reproduced later by
	// InitChain creating a PSSMLTSampler with the same seed.  This
	// preserves the bootstrap CDF's importance sampling: high-luminance
	// bootstrap indices selected by the CDF turn into chains that
	// actually start at those high-luminance paths, instead of at
	// arbitrary uniform-random paths (the old IndependentSampler vs
	// PSSMLTSampler mismatch silently discarded the importance sampling).
	//
	// PSSMLTSampler's first-iteration draws are uniformly distributed
	// in [0,1) (large step overwrites the lazy-grown value with a fresh
	// random; small step mutates it once), so the b_mean estimator is
	// unbiased — it just samples a different concrete uniform sequence
	// than IndependentSampler would for the same seed.  What matters is
	// that bootstrap and chain-init agree on which sequence to use.
	for( unsigned int i = 0; i < nBootstrap; i++ )
	{
		PSSMLTSampler* pBootSampler = new PSSMLTSampler( i, largeStepProb );
		pBootSampler->StartIteration();

		MLTSample sample = EvaluateSample( pScene, *pCamera, *pBootSampler, width, height );

		bootstrapSamples[i].luminance = sample.luminance;
		bootstrapSamples[i].seed = i;
		luminanceSum += sample.luminance;

		safe_release( pBootSampler );

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

		// Target per-round wall time.  Every round ends with an
		// OutputIntermediateImage + Progress callback, which is the
		// only mechanism MLT uses to push previews to the UI.  So
		// this value IS the user-visible update cadence.
		//
		// Set to 2500 ms (instead of 5000 ms) because the bootstrap
		// estimate is systematically optimistic: MLT mutations in
		// caustic scenes cost ~2× more than bootstrap samples on
		// average.  Aiming for 2.5 s gives realistic 3–5 s rounds.
		// Combined with the adaptive adjustment after round 0 (see
		// the render loop below), the UI stays updated well inside
		// the 5–10 s target range.
		const double targetRoundMs = 2500.0;
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
		numRounds = 20;
		GlobalLog()->PrintEx( eLog_Event, "MLTRasterizer:: Bootstrap too fast to time, using %u rounds", numRounds );
	}

	unsigned int mutationsPerChainPerRound = mutationsPerChain / numRounds;

	// Ensure at least 1 mutation per chain per round
	unsigned int effectiveMutPerRound = mutationsPerChainPerRound > 0 ? mutationsPerChainPerRound : 1;

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

		InitChain( chainStates[c], pScene, *pCamera, seed, c, width, height );

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

	// Adaptive round-sizing: the bootstrap-based numRounds estimate is
	// systematically too low for caustic-heavy scenes (MLT mutations
	// cost ~2× bootstrap samples), so the user sees updates every
	// 5–10 s instead of the 2.5 s we target.  After round 0 we know
	// the real wall-time per round and can rebalance the remaining
	// rounds to hit the target.  The UI therefore converges to a
	// stable ~2.5 s update cadence after a single corrective round
	// regardless of how bad the initial estimate was.
	const double targetRoundWallMs   = 2500.0;
	unsigned int mutationsDonePerChain = 0;

	for( unsigned int round = 0; round < numRounds && !cancelled; round++ )
	{
		// For the last round, use remaining mutations to avoid rounding loss
		const unsigned int mutThisRound = ( round == numRounds - 1 )
			? ( mutationsPerChain - mutationsDonePerChain )
			: effectiveMutPerRound;

		Timer roundTimer;
		roundTimer.start();

		if( threads > 1 )
		{
			// Work-stealing chain dispatch.  The old static partition
			// (chain c → thread c / chainsPerThread) punished any
			// variance in chain cost: fast threads idled while the
			// slowest held up the whole round.  MLT chains exploring
			// caustics have highly variable per-mutation cost (different
			// path depths, different rejection rates), so static
			// partitioning lost a lot of parallelism.
			//
			// New scheme: atomic index counter plus one worker per
			// pool slot.  Each worker repeatedly fetch_add(1) and runs
			// that chain's mutations.  Fast workers scavenge extra
			// chains while slow ones finish.
			//
			// Each worker MUST flush its thread-local splat buffer
			// before returning — the round's Resolve() runs
			// immediately after the ParallelFor and would otherwise
			// miss any splats below the auto-flush threshold.

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
						RunChainSegment(
							chainStates[c], pScene, *pCamera, *pSplatFilm,
							mutThisRound, b, width, height );
					}
					FlushCallingThreadSplatBuffer();
				} );
		}
		else
		{
			// Legacy low-priority mode: lower the caller to match
			// the rest of the render.  Only apply once — QoS on
			// macOS is lower-only, and we're on the same thread
			// across rounds so repeating is harmless but wasteful.
			if( round == 0 &&
			    GlobalOptions().ReadBool( "force_all_threads_low_priority", false ) ) {
				Threading::riseSetThreadLowPriority( 0 );
			}

			// Single-threaded: run all chains sequentially
			for( unsigned int c = 0; c < effectiveChains; c++ )
			{
				RunChainSegment( chainStates[c], pScene, *pCamera, *pSplatFilm,
					mutThisRound, b, width, height );
			}
			FlushCallingThreadSplatBuffer();
		}

		// Round work complete — stop the timer BEFORE the resolve/I-O
		// so we measure pure render cost, then track how much of each
		// chain's mutation budget we've consumed.
		roundTimer.stop();
		mutationsDonePerChain += mutThisRound;
		const unsigned int roundWallMs = roundTimer.getInterval();

		//////////////////////////////////////////////////////////////
		// Adaptive round-size correction (after round 0 only).
		//
		// If the bootstrap-based estimate was off, rebalance the
		// remaining chain-mutation budget across enough additional
		// rounds to hit the ~2.5 s target.  We compute ms/mutation
		// from the first round's measured wall time (more accurate
		// than the bootstrap estimate, which ran single-threaded and
		// without the chain-context overhead).  The UI cadence
		// converges after one corrective round regardless of how
		// badly the initial estimate was off.
		//////////////////////////////////////////////////////////////
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
				// Total rounds = 1 (already done) + newRemainingRounds
				const unsigned int newNumRounds = 1 + newRemainingRounds;

				GlobalLog()->PrintEx( eLog_Event,
					"MLTRasterizer:: Adaptive round resize — round 0 took %u ms "
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

		// Fraction uses mutations-done rather than round count
		// because adaptive resize can change numRounds mid-flight,
		// which would make round/numRounds discontinuous.
		const Scalar fraction =
			static_cast<Scalar>( mutationsDonePerChain ) /
			static_cast<Scalar>( mutationsPerChain > 0 ? mutationsPerChain : 1 );
		const bool isFinalRound = ( mutationsDonePerChain >= mutationsPerChain );

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

		// Report progress using mutations-done (same reason as
		// fraction above — stable across adaptive resize).
		if( pProgressFunc ) {
			if( !pProgressFunc->Progress(
					static_cast<double>( mutationsDonePerChain ),
					static_cast<double>( mutationsPerChain ) ) ) {
				cancelled = true;
			}
		}

		// Write the final file on completion or cancellation.
		// On cancellation we still output so the user gets a
		// valid image at whatever quality was reached.
		if( isFinalRound || cancelled )
		{
#ifdef RISE_ENABLE_OIDN
			if( bDenoisingEnabled ) {
				// pImage is already the fully splatted image at this
				// point (see pSplatFilm->Resolve above), so this is the
				// pre-denoised-but-splatted snapshot the user wants to
				// compare against the denoised result.
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

void MLTRasterizer::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputImage( img, rcRegion, frame );
	}
}

void MLTRasterizer::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputPreDenoisedImage( img, rcRegion, frame );
	}
}

void MLTRasterizer::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	RasterizerOutputListType::const_iterator r, s;
	for( r=outs.begin(), s=outs.end(); r!=s; r++ ) {
		(*r)->OutputDenoisedImage( img, rcRegion, frame );
	}
}
