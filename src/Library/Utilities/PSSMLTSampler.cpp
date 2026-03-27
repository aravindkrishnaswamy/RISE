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
  streamIndex( 0 ),
  largeStepProb( largeStepProb_ ),
  isLargeStep( true ),
  lastLargeStepIteration( 0 ),
  rng( seed )
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
// In all cases, the modified index is recorded in modifiedIndices
// so that Reject() can efficiently roll back only the changed entries.
//////////////////////////////////////////////////////////////////////

Scalar PSSMLTSampler::Get1D()
{
	// Interleaved stream indexing: each stream gets its own contiguous
	// region of the primary sample vector so mutations to one stream
	// don't disturb others.  idx = streamIndex + kNumStreams * sampleIndex
	const unsigned int idx = streamIndex + kNumStreams * sampleIndex;
	sampleIndex++;

	// Lazy initialization: grow the vector if needed
	while( idx >= X.size() )
	{
		PrimarySample ps;
		ps.value = rng.CanonicalRandom();
		ps.lastModIteration = currentIteration;
		ps.backupIteration = currentIteration;
		X.push_back( ps );
	}

	PrimarySample& sample = X[idx];

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
	modifiedIndices.push_back( idx );

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
		PrimarySample& sample = X[modifiedIndices[i]];
		sample.value = sample.backup;
		sample.lastModIteration = sample.backupIteration;
	}

	currentIteration++;
	modifiedIndices.clear();
}
