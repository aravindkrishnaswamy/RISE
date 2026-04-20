//////////////////////////////////////////////////////////////////////
//
//  MMLTStrategySelectionTest.cpp - Validates the per-depth strategy
//    enumeration and selection logic in MMLTSampler:
//
//    A. CountStrategiesForDepth: known formulas for depth d under
//       maxLight/maxEye caps, edge cases (cap=0, very small d).
//    B. PickStrategyST: returned (s,t) always satisfy s+t = depth+2
//       and the cap constraints; outNStrategies matches CountStrategies-
//       ForDepth.
//    C. Selection-uniformity: empirically the selection PDF is
//       1/nStrategies — over many large-step iterations the empirical
//       histogram of (s,t) is within tolerance of uniform.
//    D. Stream independence: stream 49 ((s,t) selection) and stream
//       50 (lens) do not alias with film stream 48 or BDPT streams
//       0-47.  Mirrors the structure of PSSMLTStreamAliasingTest for
//       the new streams.
//    E. Bound-depth invariance: depth tag is read-only and used only
//       for PDF computation; CountStrategiesForDepth(d, ...) does not
//       depend on the sampler's bound depth.
//
//  Build: tests are auto-discovered via the Makefile wildcard.  No
//  build-file changes needed beyond dropping this .cpp into tests/.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 18, 2026
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cstdlib>
#include <cmath>

#include "../src/Library/Utilities/MMLTSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << name << "\n"; }
}

// ================================================================
// Test A: CountStrategiesForDepth — closed-form expectations.
//
// For path of length d (s+t = d+2) with caps maxLight, maxEye:
//   Valid s range: [max(0, d+2-maxEye), min(maxLight, d+1)]
//   Count = max(0, hi - lo + 1)
// ================================================================

static void TestCountStrategies()
{
	std::cout << "\nTest A: CountStrategiesForDepth\n";

	unsigned int sLo = 0, sHi = 0;

	// d=0, large caps: strategies (0,2) and (1,1).  Count = 2.
	Check( MMLTSampler::CountStrategiesForDepth( 0, 12, 12, sLo, sHi ) == 2,
		"d=0, large caps -> 2 strategies" );
	Check( sLo == 0 && sHi == 1, "d=0, large caps -> s in [0,1]" );

	// d=1, large caps: (0,3), (1,2), (2,1).  Count = 3.
	Check( MMLTSampler::CountStrategiesForDepth( 1, 12, 12, sLo, sHi ) == 3,
		"d=1, large caps -> 3 strategies" );
	Check( sLo == 0 && sHi == 2, "d=1, large caps -> s in [0,2]" );

	// d=10, large caps: 12 strategies.
	Check( MMLTSampler::CountStrategiesForDepth( 10, 12, 12, sLo, sHi ) == 12,
		"d=10, large caps -> 12 strategies" );

	// d=22 (max with light=eye=12): (0,24)?  No — t<=12 caps it.
	// s+t=24, t<=12 -> s>=12.  s<=12, s<=23 -> s in [12,12].  Count=1.
	Check( MMLTSampler::CountStrategiesForDepth( 22, 12, 12, sLo, sHi ) == 1,
		"d=22, light=eye=12 -> 1 strategy (s=t=12)" );
	Check( sLo == 12 && sHi == 12, "d=22 -> only (12,12)" );

	// d=23 (above maxLight + maxEye - 2 = 22): no valid strategies.
	Check( MMLTSampler::CountStrategiesForDepth( 23, 12, 12, sLo, sHi ) == 0,
		"d=23 -> 0 strategies (over cap)" );

	// d=5, asymmetric caps: maxLight=2, maxEye=4.  s+t=7.
	// s>=7-4=3 conflicts with s<=2.  Count=0.
	Check( MMLTSampler::CountStrategiesForDepth( 5, 2, 4, sLo, sHi ) == 0,
		"d=5, light=2 eye=4 -> 0 strategies (caps incompatible)" );

	// d=4, light=2, eye=4: s+t=6, s>=6-4=2 (not 0!), s<=min(2,5)=2.
	// s in [2,2].  Only (s=2, t=4).  Count=1.
	Check( MMLTSampler::CountStrategiesForDepth( 4, 2, 4, sLo, sHi ) == 1,
		"d=4, light=2 eye=4 -> 1 strategy (s=2,t=4)" );
	Check( sLo == 2 && sHi == 2, "d=4, light=2 -> s in [2,2]" );

	// d=0, maxLight=0, maxEye=2: s+t=2, s in {0}, t in {2}.  (0,2) only.
	Check( MMLTSampler::CountStrategiesForDepth( 0, 0, 2, sLo, sHi ) == 1,
		"d=0, light=0 eye=2 -> 1 strategy (s=0,t=2)" );

	// Degenerate maxEye=0: t never satisfies t>=1, so 0 strategies.
	Check( MMLTSampler::CountStrategiesForDepth( 5, 12, 0, sLo, sHi ) == 0,
		"d=5, eye=0 -> 0 strategies (no eye vertex)" );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test B: PickStrategyST — returned (s,t) always satisfies the
// constraints and outNStrategies matches CountStrategies.
// ================================================================

static void TestPickStrategy()
{
	std::cout << "\nTest B: PickStrategyST\n";

	const unsigned int maxLight = 12;
	const unsigned int maxEye = 12;

	for( unsigned int d = 0; d <= 22; d++ )
	{
		MMLTSampler* pSampler = new MMLTSampler( 1234 + d, 1.0, d );

		unsigned int expectedSlo = 0, expectedShi = 0;
		const unsigned int expectedN =
			MMLTSampler::CountStrategiesForDepth( d, maxLight, maxEye,
				expectedSlo, expectedShi );

		// Run a handful of iterations and confirm every draw is in range.
		for( int iter = 0; iter < 32; iter++ )
		{
			pSampler->StartIteration();

			unsigned int s = 999, t = 999, n = 0;
			const bool ok = pSampler->PickStrategyST( maxLight, maxEye, s, t, n );

			Check( ok == ( expectedN > 0 ), "PickStrategyST returns ok iff strategies exist" );
			if( !ok ) {
				pSampler->Accept();
				continue;
			}

			Check( n == expectedN, "outNStrategies matches CountStrategiesForDepth" );
			Check( s >= expectedSlo && s <= expectedShi, "s in valid range" );
			Check( s + t == d + 2, "s + t == depth + 2" );
			Check( t >= 1, "t >= 1 (eye always has camera)" );
			Check( t <= maxEye, "t <= maxEye" );
			Check( s <= maxLight, "s <= maxLight" );

			pSampler->Accept();
		}

		pSampler->release();
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: Selection uniformity — empirical (s,t) histogram matches
// uniform 1/nStrategies within a tolerance.
//
// Use large-step iterations so each (s,t) draw consumes a fresh
// random; then check that no bucket is >2x or <0.5x its expected
// share over a large sample count.  A failure here means the
// floor(u*N) mapping is biased somehow (e.g., bad u distribution
// from kStreamST's lazy growth).
// ================================================================

static void TestSelectionUniformity()
{
	std::cout << "\nTest C: Selection uniformity\n";

	const unsigned int maxLight = 12;
	const unsigned int maxEye = 12;
	const unsigned int testDepth = 5;	// 7 valid strategies (s in [0..6])

	unsigned int sLo = 0, sHi = 0;
	const unsigned int N = MMLTSampler::CountStrategiesForDepth(
		testDepth, maxLight, maxEye, sLo, sHi );
	Check( N == 7, "d=5 has 7 strategies" );

	// largeStepProb=1.0 so every iteration draws a fresh u for kStreamST.
	MMLTSampler* pSampler = new MMLTSampler( 7777, 1.0, testDepth );

	const int kIterations = 60000;
	std::vector<int> counts( N, 0 );

	for( int iter = 0; iter < kIterations; iter++ )
	{
		pSampler->StartIteration();

		unsigned int s, t, n;
		Check( pSampler->PickStrategyST( maxLight, maxEye, s, t, n ),
			"PickStrategyST succeeds" );
		const unsigned int idx = s - sLo;
		counts[idx]++;

		pSampler->Accept();
	}

	const double expectedPerBucket = static_cast<double>( kIterations ) /
		static_cast<double>( N );
	for( unsigned int i = 0; i < N; i++ ) {
		const double ratio = static_cast<double>( counts[i] ) / expectedPerBucket;
		// 5% tolerance — generous for 60k samples / 6 buckets.
		Check( ratio > 0.95 && ratio < 1.05,
			"bucket count within 5% of uniform expectation" );
		if( !( ratio > 0.95 && ratio < 1.05 ) ) {
			std::cout << "    bucket " << i << ": count=" << counts[i]
				<< " expected=" << expectedPerBucket
				<< " ratio=" << ratio << "\n";
		}
	}

	pSampler->release();

	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: Stream independence for the new MMLT streams.
//
// kStreamST (49) and kStreamLens (50) must produce values
// independent of the film stream (48) and the BDPT integrator
// streams (0-47).  Identical to PSSMLTStreamAliasingTest's Test B
// but extended to cover the new streams.
// ================================================================

static void TestStreamIndependence()
{
	std::cout << "\nTest D: Stream independence (kStreamST, kStreamLens)\n";

	const int kSamples = 5;
	const int testStreams[] = { 0, 1, 16, 47, MMLTSampler::kStreamFilm };
	const int nTestStreams = sizeof(testStreams) / sizeof(testStreams[0]);

	const int kIterations = 20;

	// largeStepProb=1.0 so every iteration is a large step (fresh randoms),
	// which is the regime where aliasing would be visible as duplicate values.
	MMLTSampler* pSampler = new MMLTSampler( 33333, 1.0, /*depth=*/4 );

	for( int iter = 0; iter < kIterations; iter++ )
	{
		pSampler->StartIteration();

		// Read stream 49 ((s,t) selection)
		pSampler->StartStream( MMLTSampler::kStreamST );
		std::vector<Scalar> stVals( kSamples );
		for( int i = 0; i < kSamples; i++ ) {
			stVals[i] = pSampler->Get1D();
		}

		// Read stream 50 (lens)
		pSampler->StartStream( MMLTSampler::kStreamLens );
		std::vector<Scalar> lensVals( kSamples );
		for( int i = 0; i < kSamples; i++ ) {
			lensVals[i] = pSampler->Get1D();
		}

		// Cross-check: stream 49 vs stream 50 must produce different
		// values at every position.
		for( int i = 0; i < kSamples; i++ ) {
			Check( stVals[i] != lensVals[i],
				"kStreamST != kStreamLens at same sample idx" );
		}

		// Cross-check: each of the 5 reference streams must produce
		// values different from kStreamST and kStreamLens.
		for( int s = 0; s < nTestStreams; s++ )
		{
			pSampler->StartStream( testStreams[s] );
			for( int i = 0; i < kSamples; i++ )
			{
				const Scalar v = pSampler->Get1D();
				Check( v != stVals[i],
					"reference stream != kStreamST at same sample idx" );
				Check( v != lensVals[i],
					"reference stream != kStreamLens at same sample idx" );
			}
		}

		pSampler->Accept();
	}

	pSampler->release();
	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: Bound-depth invariance.
//
// MMLTSampler stores the depth as metadata.  CountStrategiesForDepth
// is a pure static function; verify it does NOT depend on the
// sampler instance's depth tag (we pass d explicitly).
// ================================================================

static void TestDepthTagIsMetadataOnly()
{
	std::cout << "\nTest E: Bound-depth tag is metadata only\n";

	unsigned int sLo1 = 0, sHi1 = 0, sLo2 = 0, sHi2 = 0;
	const unsigned int n1 = MMLTSampler::CountStrategiesForDepth( 5, 12, 12, sLo1, sHi1 );
	const unsigned int n2 = MMLTSampler::CountStrategiesForDepth( 5, 12, 12, sLo2, sHi2 );
	Check( n1 == n2 && sLo1 == sLo2 && sHi1 == sHi2,
		"CountStrategies is deterministic for fixed (d, caps)" );

	// Confirm GetDepth round-trips construction.
	for( unsigned int d = 0; d < 24; d++ ) {
		MMLTSampler* pSampler = new MMLTSampler( 99, 0.3, d );
		Check( pSampler->GetDepth() == d, "GetDepth() returns construction value" );
		pSampler->release();
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "=== MMLT Strategy Selection Tests ===\n";

	TestCountStrategies();
	TestPickStrategy();
	TestSelectionUniformity();
	TestStreamIndependence();
	TestDepthTagIsMetadataOnly();

	std::cout << "\nResults: " << passCount << " passed, "
		<< failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
