//////////////////////////////////////////////////////////////////////
//
//  MMLTSampler.cpp - Implementation of the per-depth MMLT sampler.
//    See MMLTSampler.h for algorithm overview and references.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 18, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MMLTSampler.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

MMLTSampler::MMLTSampler(
	const unsigned int seed,
	const Scalar largeStepProb_,
	const unsigned int depth_
	) :
	// Pass kMMLTNumStreams to the protected base ctor so the primary
	// sample vector lays out 51 lanes per sampleIndex (BDPT 0-47, film
	// 48, (s,t) 49, lens 50).  PSSMLTSampler's public ctor is
	// untouched and continues to use 49 lanes — see the bit-stability
	// note in MMLTSampler.h.
	PSSMLTSampler( seed, largeStepProb_, kMMLTNumStreams ),
	depth( depth_ )
{
}

MMLTSampler::~MMLTSampler()
{
}

//////////////////////////////////////////////////////////////////////
// CountStrategiesForDepth - count of valid (s,t) splits of a path
// of length d (= s+t-2) honouring maxLight/maxEye caps.
//
// Constraints:
//   s + t = d + 2     (path-length identity)
//   0 <= s <= maxLight
//   1 <= t <= maxEye  (RISE always has at least the camera vertex on
//                      the eye side; t == 0 strategies are not
//                      implemented, so they are never counted)
//
// From t = (d+2) - s and the t bounds:
//   t >= 1         <=>  s <= d + 1
//   t <= maxEye    <=>  s >= (d + 2) - maxEye
//
// Combining with s in [0, maxLight] gives the inclusive s range
// [sLo, sHi] where:
//   sLo = max(0, (d + 2) - maxEye)
//   sHi = min(maxLight, d + 1)
// The strategy count is sHi - sLo + 1 (or 0 if sHi < sLo).
//
// Edge cases handled by the int<->unsigned dance:
//   - d + 2 - maxEye can be negative when maxEye > d + 2.  Clamp to 0.
//   - sHi can be negative if (d + 1 < 0) but d is unsigned so this
//     can't happen.
//   - For very small maxEye (=0 in some degenerate test setups), the
//     sLo calculation is correctly clamped.
//////////////////////////////////////////////////////////////////////

unsigned int MMLTSampler::CountStrategiesForDepth(
	const unsigned int d,
	const unsigned int maxLight,
	const unsigned int maxEye,
	unsigned int& outSlo,
	unsigned int& outShi
	)
{
	const int sLoSigned = std::max(
		0,
		static_cast<int>( d ) + 2 - static_cast<int>( maxEye ) );
	const int sHiSigned = std::min(
		static_cast<int>( maxLight ),
		static_cast<int>( d ) + 1 );

	if( sHiSigned < sLoSigned ) {
		outSlo = 0;
		outShi = 0;
		return 0;
	}

	outSlo = static_cast<unsigned int>( sLoSigned );
	outShi = static_cast<unsigned int>( sHiSigned );
	return static_cast<unsigned int>( sHiSigned - sLoSigned + 1 );
}

//////////////////////////////////////////////////////////////////////
// PickStrategyST - draw a uniform (s,t) for this sampler's depth.
//
// The discrete index `idx in [0, nStrategies)` is computed by mapping
// a continuous primary sample u from kStreamST through floor(u * N).
// For u == 1.0 (which Get1D promises against but defending against
// numerical edge cases is cheap) we clamp to nStrategies - 1.
//
// IMPORTANT — selection PDF accounting:
//   The selection has discrete PDF = 1 / nStrategies for each (s,t).
//   The caller is responsible for multiplying the strategy's
//   contribution by nStrategies to undo this PDF.  This matches PBRT
//   v3 MLTIntegrator::L which does `return ConnectBDPT(...) *
//   nStrategies;`.  We deliberately do NOT roll the multiplier into
//   the sampler — the contribution lives in the rasterizer and the
//   sampler shouldn't know about it.
//
// Why we use a continuous primary sample for a DISCRETE choice:
//   Mutating a continuous u in [0,1) sometimes flips the mapped
//   integer index (depending on which 1/N-wide bin the perturbation
//   crosses).  This gives the chain a nonzero probability of
//   transitioning between strategies even on small steps — which is
//   what keeps the chain ergodic over the (s,t) dimension.  A
//   strictly-integer mutation would either always flip (no local
//   coherence) or never flip (no ergodicity).
//////////////////////////////////////////////////////////////////////

bool MMLTSampler::PickStrategyST(
	const unsigned int maxLight,
	const unsigned int maxEye,
	unsigned int& outS,
	unsigned int& outT,
	unsigned int& outNStrategies
	)
{
	unsigned int sLo = 0;
	unsigned int sHi = 0;
	outNStrategies = CountStrategiesForDepth( depth, maxLight, maxEye, sLo, sHi );

	if( outNStrategies == 0 ) {
		outS = 0;
		outT = 0;
		return false;
	}

	StartStream( kStreamST );
	const Scalar u = Get1D();

	unsigned int idx = static_cast<unsigned int>( u * static_cast<Scalar>( outNStrategies ) );
	if( idx >= outNStrategies ) {
		idx = outNStrategies - 1;
	}

	outS = sLo + idx;
	// t = (depth + 2) - s; depth is unsigned, depth+2 fits unsigned, outS <= depth+1 by construction.
	outT = ( depth + 2 ) - outS;

	return true;
}
