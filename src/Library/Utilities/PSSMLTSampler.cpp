//////////////////////////////////////////////////////////////////////
//
//  PSSMLTSampler.cpp - Implementation of the Primary Sample Space
//    MLT sampler.  See PSSMLTSampler.h for overview and invariants.
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
#include <cmath>
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
  rng( PSSMLTSamplerTestHooks::Salted( seed ) )
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
  rng( PSSMLTSamplerTestHooks::Salted( seed ) )
{
}

PSSMLTSampler::~PSSMLTSampler()
{
}

// Symmetric Kelemen log-uniform perturbation in [s1, s2], wrapped to [0,1).
// Symmetry (P(x->x') == P(x'->x)) keeps the MH proposal ratio equal to 1.

Scalar PSSMLTSampler::Mutate( const Scalar value )
{
	const Scalar u = rng.CanonicalRandom();
	const Scalar delta = s2 * exp( logRatio * u );

	// Randomly add or subtract the perturbation
	Scalar result;
	if( rng.CanonicalRandom() < 0.5 )
	{
		// Positive perturbation, wrap to [0,1)
		result = value + delta;
		if( result >= 1.0 ) {
			result -= 1.0;
		}
	}
	else
	{
		// Negative perturbation, wrap to [0,1)
		result = value - delta;
		if( result < 0.0 ) {
			result += 1.0;
		}
	}
	// Sub-ULP wrap (`value - delta` in `(-0.5*ulp(1), 0)` + `1.0`) can round
	// to `1.0` exact; clamp into strict `[0, 1)` to honour the PrimarySample
	// contract (`Get1D() in [0, 1)`).
	if( result >= Scalar( 1.0 ) ) {
		result = std::nextafter( Scalar( 1.0 ), Scalar( 0.0 ) );
	} else if( result < Scalar( 0.0 ) ) {
		result = Scalar( 0.0 );
	}
	return result;
}

// Lazily grows/mutates the next primary sample in two-tier storage (X / XExtra;
// see PSSMLTSampler.h) and logs the lane in modifiedIndices for Reject().

Scalar PSSMLTSampler::Get1D()
{
	// Unconditional runtime sanity bound (not debug-only assert, which vanishes
	// under Windows Release NDEBUG) to catch runaway/uninitialized stream IDs.
	if( streamIndex < 0 || (streamIndex >= kNumStreams && streamIndex != 3072) )
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
		// Legacy flat tier (row width fixed at kLegacyNumStreams = 49).
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
		// Extra tier: independent vector per stream (O(drawn) memory).
		std::vector<PrimarySample>& streamVec = FindOrCreateExtraStream( streamIndex );

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
		const std::uint64_t nSmall = currentIteration - sample.lastModIteration;
		const std::uint64_t nMutations = nSmall > 0 ? nSmall : 1;
		for( std::uint64_t i = 0; i < nMutations; i++ )
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
}

// Begin a new proposal: pick large vs small step and reset stream/indices.
void PSSMLTSampler::StartIteration()
{
	alphaIndex = 0;
	isLargeStep = ( rng.CanonicalRandom() < largeStepProb );
	streamIndex = 0;
	sampleIndex = 0;
	modifiedIndices.clear();
}

// Commit current proposal and advance iteration counter.
void PSSMLTSampler::Accept()
{
	if( isLargeStep ) {
		lastLargeStepIteration = currentIteration;
	}
	currentIteration++;
	modifiedIndices.clear();
}

// Roll back modified lanes from backups in O(modified) time.
void PSSMLTSampler::Reject()
{
	for( unsigned int i = 0; i < modifiedIndices.size(); i++ )
	{
		const ModifiedLane& lane = modifiedIndices[i];
		PrimarySample& sample = lane.legacy
			? X[lane.legacyIdx]
			: FindOrCreateExtraStream( lane.stream )[lane.sampleIdx];
		sample.value = sample.backup;
		sample.lastModIteration = sample.backupIteration;
	}

	currentIteration++;
	modifiedIndices.clear();
}

Scalar PSSMLTSampler::GetAlpha1D()
{
    const int savedStream = streamIndex;
    const unsigned int savedIndex = sampleIndex;
    streamIndex = 3072;
    sampleIndex = alphaIndex;
    const Scalar value = Get1D();
    alphaIndex = sampleIndex;
    streamIndex = savedStream;
    sampleIndex = savedIndex;
    return value;
}
