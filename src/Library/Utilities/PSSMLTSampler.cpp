//////////////////////////////////////////////////////////////////////
//
//  PSSMLTSampler.cpp - Implementation of the Primary Sample Space
//    MLT sampler.  See PSSMLTSampler.h for algorithm overview and
//    references.
//
//  THOUGHT PROCESS:
//    The core challenge is maintaining a bijection between random
//    number sequences and paths.  BDPT consumes N random numbers
//    to build a path; we store these as the primary sample vector X.
//    To propose a new path, we mutate X and feed it back to BDPT.
//
//    Large steps (probability ~0.3) replace the entire vector with
//    fresh randoms -- this prevents the chain from getting stuck
//    in a local mode of the path space.
//
//    Small steps perturb each element by a small amount using
//    Kelemen's exponential distribution.  The distribution is
//    chosen so that the mutation is symmetric (satisfies detailed
//    balance without a Jacobian correction), and concentrates
//    most perturbations near the current value while allowing
//    occasional larger jumps.
//
//    Lazy initialization means we never need to know in advance
//    how many random numbers BDPT will consume.  If BDPT asks for
//    sample index i and we only have i-1 elements, we simply
//    append a fresh random value.
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
#include "PSSMLTSampler.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace RISE;
using namespace RISE::Implementation;

// Perturbation range constants (Kelemen et al. 2002, Section 3.2).
// s1 = minimum perturbation magnitude (~0.001)
// s2 = maximum perturbation magnitude (~0.016)
// These values are widely used in PBRT, Mitsuba, and other implementations.
const Scalar PSSMLTSampler::s1 = 1.0 / 1024.0;
const Scalar PSSMLTSampler::s2 = 1.0 / 64.0;
const Scalar PSSMLTSampler::logRatio = -log( s2 / s1 );

PSSMLTSampler::PSSMLTSampler(
	const unsigned int seed,
	const Scalar largeStepProb_
	) :
  sampleIndex( 0 ),
  currentIteration( 0 ),
  kNumStreams( kDefaultNumStreams ),
  streamIndex( 0 ),
  largeStepProb( largeStepProb_ ),
  isLargeStep( true ),
  lastLargeStepIteration( 0 ),
  rng( seed ),
  extraHint( 0 ),
  extraCursor( -1 )
{
}

PSSMLTSampler::PSSMLTSampler(
	const unsigned int seed,
	const Scalar largeStepProb_,
	const int numStreams_
	) :
  sampleIndex( 0 ),
  currentIteration( 0 ),
  kNumStreams( numStreams_ ),
  streamIndex( 0 ),
  largeStepProb( largeStepProb_ ),
  isLargeStep( true ),
  lastLargeStepIteration( 0 ),
  rng( seed ),
  extraHint( 0 ),
  extraCursor( -1 )
{
}

PSSMLTSampler::~PSSMLTSampler()
{
}

//////////////////////////////////////////////////////////////////////
// Mutate - Apply exponential perturbation to a single sample value.
//
// The mutation distribution is log-uniform between s1 and s2:
//   delta = s2 * exp(-logRatio * u),   u ~ Uniform[0,1)
// This gives equal probability mass per decade of perturbation size,
// producing a mix of tiny and moderate mutations.  The sign is
// randomized, and the result is wrapped to [0,1).
//
// This mutation is symmetric: P(x->x') = P(x'->x), which means
// the Metropolis-Hastings acceptance ratio simplifies to just
// f(x')/f(x) -- no proposal ratio correction needed.
//////////////////////////////////////////////////////////////////////

Scalar PSSMLTSampler::Mutate( const Scalar value )
{
	const Scalar u = rng.CanonicalRandom();
	const Scalar delta = s2 * exp( logRatio * u );

	// Randomly add or subtract the perturbation
	if( rng.CanonicalRandom() < 0.5 )
	{
		// Positive perturbation, wrap to [0,1)
		Scalar result = value + delta;
		if( result >= 1.0 ) {
			result -= 1.0;
		}
		return result;
	}
	else
	{
		// Negative perturbation, wrap to [0,1)
		Scalar result = value - delta;
		if( result < 0.0 ) {
			result += 1.0;
		}
		return result;
	}
}

//////////////////////////////////////////////////////////////////////
// Get1D - Return the next sample from the primary sample vector.
//
// Three cases:
// 1. Index beyond current vector size: lazily append a fresh random
//    value (this happens during the first evaluation of any path
//    that consumes more samples than we've seen before).
// 2. Large step: replace with a fresh random (backup the old value).
// 3. Small step: apply exponential perturbation (backup the old value).
//
// In all cases, the touched lane is recorded in modifiedIndices so
// that Reject() can efficiently roll back only the changed entries.
//
// STORAGE (DL-08 follow-up, 2026-09-17): two tiers -- see the class
// comment above `X`/`XExtra` in PSSMLTSampler.h for the full design
// rationale (why a single flat vector's lazy-grow loop is
// catastrophic once a reserved stream sits far above the ordinary
// ones, and why the two-tier split reproduces the pre-DL-08 base
// commit's draws bit-for-bit for streamIndex < kLegacyNumStreams).
//////////////////////////////////////////////////////////////////////

Scalar PSSMLTSampler::Get1D()
{
	// Runtime range check -- deliberately NOT a debug-only `assert`.
	// `assert` compiles to nothing when NDEBUG is defined, and NDEBUG's
	// definition is platform/config-specific: Config.OSX and
	// Config.Linux (build/make/rise/) never define it, so an `assert`
	// here would already run in EVERY mac/Linux configuration including
	// Deployment/Opto; only VS2022's Release|x64 config
	// (build/VS2022/Library/Library.vcxproj) defines NDEBUG, so an
	// `assert` guard would silently vanish specifically -- and only --
	// on Windows Release, the one configuration where a runaway
	// streamIndex would otherwise go uncaught.  This check costs one
	// branch per Get1D() call (negligible next to the
	// RandomNumberGenerator draw and PrimarySample bookkeeping this
	// function already does), so there is no performance reason to gate
	// it on a build config at all -- it fires identically everywhere.
	//
	// Note this is now a SANITY bound, not a collision-avoidance
	// requirement: with the two-tier storage below, no two distinct
	// stream numbers can ever alias, at any magnitude, so this only
	// exists to catch a programming error (a negative or absurdly large
	// streamIndex, e.g. from an uninitialized or overflowed depth
	// counter) loudly instead of silently building an unbounded
	// `XExtra` entry.
	if( streamIndex < 0 || streamIndex >= kNumStreams )
	{
		fprintf( stderr,
			"FATAL: PSSMLTSampler::Get1D: streamIndex %d is out of the "
			"sanity bound [0, %d) -- likely an uninitialized or "
			"overflowed depth counter upstream (DL-08 / debt 29 "
			"follow-up).  Raise kNumStreams if this stream number is "
			"intentional.\n", streamIndex, kNumStreams );
		abort();
	}

	PrimarySample* pSample;
	ModifiedLane lane;

	if( streamIndex < kLegacyNumStreams )
	{
		// Legacy tier: the ORIGINAL flat vector, row width pinned to
		// kLegacyNumStreams (49) forever -- bit-identical to the
		// pre-DL-08 base commit for any chain that never touches a
		// stream >= kLegacyNumStreams.
		const unsigned int idx = streamIndex + kLegacyNumStreams * sampleIndex;

		while( idx >= X.size() )
		{
			PrimarySample ps;
			ps.value = rng.CanonicalRandom();
			ps.lastModIteration = currentIteration;
			ps.backupIteration = currentIteration;
			X.push_back( ps );
		}

		pSample = &X[idx];
		lane.legacy = true;
		lane.legacyIdx = idx;
	}
	else
	{
		// Extra tier: one independently-grown vector per stream,
		// found (or created) by FindOrCreateExtraStream()'s linear
		// scan over XExtra.  Touching this stream for N samples costs
		// exactly N PrimarySample slots -- no multiplicative blow-up,
		// regardless of how large streamIndex is.
		if( extraCursor < 0 ) {
			extraCursor = static_cast<long>( FindOrCreateExtraStreamIndex( streamIndex ) );
		}
		std::vector<PrimarySample>& streamVec = XExtra[ static_cast<size_t>( extraCursor ) ].second;

		while( sampleIndex >= streamVec.size() )
		{
			PrimarySample ps;
			ps.value = rng.CanonicalRandom();
			ps.lastModIteration = currentIteration;
			ps.backupIteration = currentIteration;
			streamVec.push_back( ps );
		}

		pSample = &streamVec[sampleIndex];
		lane.legacy = false;
		lane.stream = streamIndex;
		lane.extraIdx = static_cast<size_t>( extraCursor );
		lane.sampleIdx = sampleIndex;
	}

	sampleIndex++;

	PrimarySample& sample = *pSample;

	// Save backup for potential rejection
	sample.backup = sample.value;
	sample.backupIteration = sample.lastModIteration;

	if( isLargeStep )
	{
		// Large step: complete independence -- fresh random value
		sample.value = rng.CanonicalRandom();
	}
	else
	{
		// Small step: apply exponential perturbation.
		// If this sample hasn't been touched since the last large step,
		// we first need to "catch up" by re-randomizing it -- lazy
		// large step application.
		if( sample.lastModIteration < lastLargeStepIteration )
		{
			sample.value = rng.CanonicalRandom();
			sample.lastModIteration = lastLargeStepIteration;
		}

		// Apply accumulated small mutations for all iterations this
		// sample was skipped.  If nSmall > 1, the sample wasn't
		// consumed for nSmall-1 prior iterations and needs catch-up
		// mutations to maintain the correct stationary distribution.
		const unsigned int nSmall = currentIteration - sample.lastModIteration;
		const unsigned int nMutations = nSmall > 0 ? nSmall : 1;
		for( unsigned int i = 0; i < nMutations; i++ )
		{
			sample.value = Mutate( sample.value );
		}
	}

	sample.lastModIteration = currentIteration;
	modifiedIndices.push_back( lane );

	return sample.value;
}

Point2 PSSMLTSampler::Get2D()
{
	return Point2( Get1D(), Get1D() );
}

void PSSMLTSampler::StartStream( int stream )
{
	streamIndex = stream;
	sampleIndex = 0;
	extraCursor = -1;		// resolved lazily on the first extra-tier Get1D
}

//////////////////////////////////////////////////////////////////////
// StartIteration - Begin a new mutation proposal.
//
// Decides whether this will be a large or small step based on
// largeStepProb.  Resets the consumption index so BDPT starts
// reading from position 0 again.  Clears the modification
// tracking list.
//////////////////////////////////////////////////////////////////////

void PSSMLTSampler::StartIteration()
{
	isLargeStep = ( rng.CanonicalRandom() < largeStepProb );
	streamIndex = 0;
	sampleIndex = 0;
	extraCursor = -1;
	modifiedIndices.clear();
}

//////////////////////////////////////////////////////////////////////
// Accept - Commit the current proposal as the new Markov chain state.
//
// The proposed values are already stored in X[i].value; we simply
// advance the iteration counter and, if this was a large step,
// record it as the most recent large step (for lazy catch-up in
// future small steps).
//////////////////////////////////////////////////////////////////////

void PSSMLTSampler::Accept()
{
	if( isLargeStep ) {
		lastLargeStepIteration = currentIteration;
	}
	currentIteration++;
	modifiedIndices.clear();
}

//////////////////////////////////////////////////////////////////////
// Reject - Revert all mutations made during the current proposal.
//
// Iterates through modifiedIndices and restores each sample's
// value and lastModIteration from the backup.  This is O(k) where
// k is the number of samples consumed by one BDPT evaluation,
// typically 50-200.
//////////////////////////////////////////////////////////////////////

void PSSMLTSampler::Reject()
{
	for( unsigned int i = 0; i < modifiedIndices.size(); i++ )
	{
		const ModifiedLane& lane = modifiedIndices[i];
		PrimarySample& sample = lane.legacy
			? X[lane.legacyIdx]
			: XExtra[lane.extraIdx].second[lane.sampleIdx];
		sample.value = sample.backup;
		sample.lastModIteration = sample.backupIteration;
	}

	currentIteration++;
	modifiedIndices.clear();
}
