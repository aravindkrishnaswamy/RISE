//////////////////////////////////////////////////////////////////////
//
//  OptimalMISAccumulatorTest.cpp - Validates the OptimalMISAccumulator
//    second-moment statistics and alpha computation:
//
//    1. Equal second moments -> alpha = 0.5 (balance heuristic)
//    2. BSDF high variance -> alpha < 0.5 (less weight to BSDF)
//    3. NEE high variance -> alpha > 0.5 (more weight to BSDF)
//    4. Fallback to 0.5 with insufficient samples
//    5. Clamping to [0.05, 0.95]
//    6. Tile-based spatial binning
//    7. Zero/negative inputs ignored gracefully
//    8. Weight sum property with solved alpha
//    9. Reset clears state
//   10. AccumulateCount/Accumulate split
//   11. Zero-moment fallback (enough attempts, no hits)
//   12. DL-72: BSSRDF/volume continuation moment/count pairing --
//       unwired vs. buggy (unpaired) vs. fixed (paired), plus the
//       phasePdf/cosinePdf-invariant design property both fixes rely on
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <iomanip>

#include "../src/Library/Utilities/OptimalMISAccumulator.h"
#include "../src/Library/Utilities/MISWeights.h"

using namespace RISE;
using namespace RISE::Implementation;

static const double TOL = 1e-6;

static bool ApproxEqual(
	double a,
	double b,
	double tol
	)
{
	return fabs( a - b ) < tol;
}

static int failCount = 0;
static int passCount = 0;

static void Check(
	bool condition,
	const char* testName
	)
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Test 1: Equal second moments -> alpha = 0.5
//////////////////////////////////////////////////////////////////////
static void TestEqualMoments()
{
	std::cout << "Test 1: Equal second moments" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 16;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// Equal second moments: f^2=1, p=1 for both techniques.
	// M_nee = mean((f/p_nee)^2) = mean(1/1) = 1
	// M_bsdf = mean((f/p_bsdf)^2) = mean(1/1) = 1
	// alpha = M_nee / (M_nee + M_bsdf) = 0.5
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 1.0, 1.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 1.0, 1.0, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( ApproxEqual( alpha, 0.5, 0.01 ),
		"equal moments -> alpha ~= 0.5" );
}

//////////////////////////////////////////////////////////////////////
// Test 2: BSDF has high variance -> alpha < 0.5 (less weight to BSDF)
//////////////////////////////////////////////////////////////////////
static void TestBSDFHighVariance()
{
	std::cout << "Test 2: BSDF high variance" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// BSDF has high second moment (high variance):
	// f^2 = 100, p_bsdf = 0.1 -> (f/p_bsdf)^2 = 100/0.01 = 10000
	// NEE has low second moment:
	// f^2 = 100, p_nee = 10 -> (f/p_nee)^2 = 100/100 = 1
	// M_nee = 1, M_bsdf = 10000
	// alpha = 1 / 10001 ~= 0.0001 -> clamped to 0.05
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 100.0, 10.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 100.0, 0.1, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( alpha < 0.5,
		"BSDF high variance -> alpha < 0.5" );
	Check( alpha >= 0.05,
		"alpha clamped to min" );
}

//////////////////////////////////////////////////////////////////////
// Test 3: NEE has high variance -> alpha > 0.5 (more weight to BSDF)
//////////////////////////////////////////////////////////////////////
static void TestNEEHighVariance()
{
	std::cout << "Test 3: NEE high variance" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// NEE has high second moment:
	// f^2 = 100, p_nee = 0.1 -> (f/p_nee)^2 = 100/0.01 = 10000
	// BSDF has low second moment:
	// f^2 = 100, p_bsdf = 10 -> (f/p_bsdf)^2 = 100/100 = 1
	// M_nee = 10000, M_bsdf = 1
	// alpha = 10000 / 10001 ~= 0.999 -> clamped to 0.95
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 100.0, 0.1, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 100.0, 10.0, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( alpha > 0.5,
		"NEE high variance -> alpha > 0.5" );
	Check( alpha <= 0.95,
		"alpha clamped to max" );
}

//////////////////////////////////////////////////////////////////////
// Test 4: Insufficient samples -> fallback
//////////////////////////////////////////////////////////////////////
static void TestFallback()
{
	std::cout << "Test 4: Insufficient samples fallback" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 50;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// Only 10 samples per technique (below threshold of 50)
	for( int i = 0; i < 10; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 100.0, 0.1, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 100.0, 10.0, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( ApproxEqual( alpha, 0.5, TOL ),
		"insufficient samples -> alpha = 0.5" );

	// Also test: enough NEE but not enough BSDF -> favor NEE (alphaClampMin)
	OptimalMISAccumulator acc2;
	acc2.Initialize( 32, 32, config );
	for( int i = 0; i < 100; i++ ) {
		acc2.AccumulateCount( 8, 8, kTechniqueNEE );
		acc2.Accumulate( 8, 8, 100.0, 1.0, kTechniqueNEE );
	}
	for( int i = 0; i < 5; i++ ) {
		acc2.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc2.Accumulate( 8, 8, 100.0, 1.0, kTechniqueBSDF );
	}
	acc2.Solve();
	Check( ApproxEqual( acc2.GetAlpha( 8, 8 ), config.alphaClampMin, TOL ),
		"enough NEE but not enough BSDF -> favor NEE" );
}

//////////////////////////////////////////////////////////////////////
// Test 5: Clamping bounds
//////////////////////////////////////////////////////////////////////
static void TestClamping()
{
	std::cout << "Test 5: Clamping" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 5;
	config.alphaClampMin = 0.1;
	config.alphaClampMax = 0.9;
	acc.Initialize( 32, 32, config );

	// Extreme: NEE is great (low M), BSDF terrible (high M)
	// -> alpha near 0 -> clamped to 0.1
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 1.0, 1000.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 1.0, 0.001, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( alpha <= 0.9 + TOL,
		"alpha <= clampMax" );
	Check( alpha >= 0.1 - TOL,
		"alpha >= clampMin" );
}

//////////////////////////////////////////////////////////////////////
// Test 6: Tile-based spatial binning
//////////////////////////////////////////////////////////////////////
static void TestTileBinning()
{
	std::cout << "Test 6: Tile spatial binning" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 16;
	config.minSamplesPerTile = 5;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 64, 64, config );

	// Tile (0,0): BSDF high variance -> alpha < 0.5
	for( int i = 0; i < 50; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 100.0, 10.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 100.0, 0.1, kTechniqueBSDF );
	}

	// Tile (1,0): NEE high variance -> alpha > 0.5
	for( int i = 0; i < 50; i++ ) {
		acc.AccumulateCount( 24, 8, kTechniqueNEE );
		acc.Accumulate( 24, 8, 100.0, 0.1, kTechniqueNEE );
		acc.AccumulateCount( 24, 8, kTechniqueBSDF );
		acc.Accumulate( 24, 8, 100.0, 10.0, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alphaTile00 = acc.GetAlpha( 8, 8 );
	Scalar alphaTile10 = acc.GetAlpha( 24, 8 );

	Check( alphaTile00 < 0.5,
		"tile(0,0) BSDF high variance -> alpha < 0.5" );
	Check( alphaTile10 > 0.5,
		"tile(1,0) NEE high variance -> alpha > 0.5" );
	Check( fabs( alphaTile00 - alphaTile10 ) > 0.1,
		"different tiles have different alphas" );

	// Pixels in same tile get same alpha
	Scalar alpha00_a = acc.GetAlpha( 0, 0 );
	Scalar alpha00_b = acc.GetAlpha( 15, 15 );
	Check( ApproxEqual( alpha00_a, alpha00_b, TOL ),
		"pixels in same tile get same alpha" );
}

//////////////////////////////////////////////////////////////////////
// Test 7: Zero/negative inputs
//////////////////////////////////////////////////////////////////////
static void TestEdgeInputs()
{
	std::cout << "Test 7: Edge inputs" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 5;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// Zero f2 -> should be ignored (but count still increments)
	acc.AccumulateCount( 8, 8, kTechniqueNEE );
	acc.Accumulate( 8, 8, 0.0, 1.0, kTechniqueNEE );

	// Zero PDF -> should be ignored
	acc.AccumulateCount( 8, 8, kTechniqueNEE );
	acc.Accumulate( 8, 8, 1.0, 0.0, kTechniqueNEE );

	// Out of bounds pixel -> should be ignored
	acc.AccumulateCount( 100, 100, kTechniqueNEE );
	acc.Accumulate( 100, 100, 1.0, 1.0, kTechniqueNEE );

	acc.Solve();

	// No valid moment data -> fallback
	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( ApproxEqual( alpha, 0.5, TOL ),
		"no valid samples -> 0.5" );

	// Out of bounds query -> fallback
	Scalar alphaOOB = acc.GetAlpha( 100, 100 );
	Check( ApproxEqual( alphaOOB, 0.5, TOL ),
		"out of bounds query -> 0.5" );
}

//////////////////////////////////////////////////////////////////////
// Test 8: Weight sum property with solved alpha
//////////////////////////////////////////////////////////////////////
static void TestWeightSumWithAlpha()
{
	std::cout << "Test 8: Weight sum with solved alpha" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 5;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 50.0, 2.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 50.0, 0.5, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );

	// w_bsdf + w_nee should = 1 for any (pa, pb) pair
	double testPdfs[][2] = {
		{1.0, 1.0}, {3.0, 4.0}, {0.1, 10.0}, {10.0, 0.1}
	};

	for( int i = 0; i < 4; i++ ) {
		double pa = testPdfs[i][0];
		double pb = testPdfs[i][1];
		double wBsdf = MISWeights::OptimalMIS2Weight( pa, pb, alpha );
		double wNee = MISWeights::OptimalMIS2Weight( pb, pa, 1.0 - alpha );
		char name[128];
		snprintf( name, sizeof(name),
			"weight sum = 1: alpha=%.3f pa=%.2f pb=%.2f (sum=%.12f)",
			alpha, pa, pb, wBsdf + wNee );
		Check( ApproxEqual( wBsdf + wNee, 1.0, 1e-10 ), name );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 9: Reset clears state
//////////////////////////////////////////////////////////////////////
static void TestReset()
{
	std::cout << "Test 9: Reset" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 5;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// Accumulate and solve
	for( int i = 0; i < 50; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		acc.Accumulate( 8, 8, 100.0, 10.0, kTechniqueNEE );
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 100.0, 0.1, kTechniqueBSDF );
	}
	acc.Solve();
	Check( acc.IsReady(), "solved -> IsReady" );

	// Reset
	acc.Reset();
	Check( !acc.IsReady(), "reset -> not IsReady" );

	// After reset, GetAlpha returns 0.5
	Scalar alpha = acc.GetAlpha( 8, 8 );
	Check( ApproxEqual( alpha, 0.5, TOL ),
		"after reset -> alpha = 0.5" );
}

//////////////////////////////////////////////////////////////////////
// Test 10: AccumulateCount/Accumulate split behavior
//////////////////////////////////////////////////////////////////////
static void TestCountAccumulateSplit()
{
	std::cout << "Test 10: AccumulateCount/Accumulate split" << std::endl;

	OptimalMISAccumulator acc;
	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;
	acc.Initialize( 32, 32, config );

	// 100 NEE attempts: 50 hits (f^2=4, pdf=1), 50 misses (f=0)
	// M_nee = sum((f/p)^2) / count = (50 * 4/1) / 100 = 2.0
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueNEE );
		if( i < 50 ) {
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueNEE );
		}
		// else: miss, only count was incremented
	}

	// 100 BSDF attempts: all hits (f^2=4, pdf=1)
	// M_bsdf = sum((f/p)^2) / count = (100 * 4/1) / 100 = 4.0
	for( int i = 0; i < 100; i++ ) {
		acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueBSDF );
	}

	acc.Solve();

	Scalar alpha = acc.GetAlpha( 8, 8 );
	// M_nee = 2.0, M_bsdf = 4.0
	// alpha = M_nee / (M_nee + M_bsdf) = 2.0 / 6.0 = 0.333...
	// NEE has lower second moment (many misses dilute it), so
	// NEE gets more weight (alpha < 0.5 means less BSDF weight).
	Check( ApproxEqual( alpha, 1.0/3.0, 0.01 ),
		"miss-diluted NEE -> alpha ~= 0.333" );

	// Without the split (old behavior where count = hits only),
	// M_nee would be 4.0, same as M_bsdf, giving alpha = 0.5.
	// The count/accumulate split correctly gives alpha < 0.5
	// because NEE's second moment is diluted by the misses.
	Check( alpha < 0.45,
		"count split gives different alpha than hits-only" );
}

//////////////////////////////////////////////////////////////////////
// Test 11: Zero-moment fallback — enough attempts but no hits
//////////////////////////////////////////////////////////////////////
static void TestZeroMomentFallback()
{
	std::cout << "Test 11: Zero-moment fallback" << std::endl;

	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;

	// Case A: BSDF has enough attempts but zero moment (all misses).
	// NEE has good data.  Should favor NEE (alpha -> clampMin).
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );

		for( int i = 0; i < 50; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueNEE );
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			// No Accumulate — all BSDF attempts missed
		}
		acc.Solve();

		Scalar alpha = acc.GetAlpha( 8, 8 );
		Check( ApproxEqual( alpha, config.alphaClampMin, TOL ),
			"BSDF all-miss -> favor NEE (alphaClampMin)" );
	}

	// Case B: NEE has enough attempts but zero moment (all misses).
	// BSDF has good data.  Should favor BSDF (alpha -> clampMax).
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );

		for( int i = 0; i < 50; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			// No Accumulate — all NEE attempts missed
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueBSDF );
		}
		acc.Solve();

		Scalar alpha = acc.GetAlpha( 8, 8 );
		Check( ApproxEqual( alpha, config.alphaClampMax, TOL ),
			"NEE all-miss -> favor BSDF (alphaClampMax)" );
	}

	// Case C: Both have enough attempts but zero moment.
	// No evidence either way -> balance heuristic fallback.
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );

		for( int i = 0; i < 50; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
		}
		acc.Solve();

		Scalar alpha = acc.GetAlpha( 8, 8 );
		Check( ApproxEqual( alpha, 0.5, TOL ),
			"both all-miss -> balance heuristic (0.5)" );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 12 (DL-72, round 3): BSSRDF / volume continuation moment/count
// pairing.  DL-72's round-1 fix trained the accumulator from these two
// continuation sites with `Accumulate` calls that had NO paired
// `AccumulateCount` -- exactly the mismatch this test drives directly
// through the shared accumulator contract (PathTracingIntegrator.cpp /
// RayCaster.cpp only ever consume this same public API, so exercising it
// here proves the CONTRACT-LEVEL bug and fix without needing a full
// render). Three scenarios on the SAME base BSDF/NEE traffic:
//   "unwired"  -- the round-2 conservative fix: the SSS/volume sites
//                 contribute NEITHER a count NOR a moment (as if the
//                 continuation never happened for training purposes).
//   "buggy"    -- the round-1 bug: the SSS/volume sites contribute a
//                 moment (Accumulate) with NO matching AccumulateCount,
//                 inflating Mbsdf's numerator without growing its
//                 denominator.
//   "fixed"    -- round 3 (this pass): the SSS/volume sites contribute
//                 BOTH, paired, like every other BSDF-technique sample.
// All three are computed against a HAND-DERIVED expected alpha so this
// is a genuine numeric red-proof, not just a not-equal check.
//////////////////////////////////////////////////////////////////////
static void TestDL72PairedTrainingFires()
{
	std::cout << "Test 12: DL-72 BSSRDF/volume moment/count pairing" << std::endl;

	OptimalMISAccumulator::Config config;
	config.tileSize = 32;
	config.minSamplesPerTile = 10;
	config.alphaClampMin = 0.05;
	config.alphaClampMax = 0.95;

	const int nOrdinaryBsdf = 200;	// ordinary (non-SSS/volume) BSDF-technique hits, f2=4, pdf=1
	const int nExtra        = 50;	// simulated SSS/volume continuation hits, f2=16, pdf=1
	const int nNee          = 200;	// NEE hits, f2=4, pdf=1

	// -------------------- "unwired" (round-2 conservative fix) --------------------
	// The extra 50 samples are never fed to the accumulator at all -- matches the
	// round-2 ruling ("leave this arm UNWIRED... training at these four sites is
	// therefore NOT WIRED").  Mbsdf sees only the ordinary traffic.
	Scalar alphaUnwired;
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );
		for( int i = 0; i < nOrdinaryBsdf; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueBSDF );
		}
		for( int i = 0; i < nNee; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueNEE );
		}
		acc.Solve();
		alphaUnwired = acc.GetAlpha( 8, 8 );
	}
	// Mbsdf = 200*4/200 = 4.0, Mnee = 4.0 -> alpha = 4/(4+4) = 0.5
	Check( ApproxEqual( alphaUnwired, 0.5, 0.001 ),
		"unwired (round-2): ordinary-only traffic solves alpha=0.5" );

	// -------------------- "buggy" (round-1 pattern: Accumulate, no AccumulateCount) --------------------
	Scalar alphaBuggy;
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );
		for( int i = 0; i < nOrdinaryBsdf; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueBSDF );
		}
		for( int i = 0; i < nExtra; i++ ) {
			// Round-1 bug: moment contributed, count is NOT (this is
			// literally what DL-72's round-2 review found in
			// RayCaster.cpp's two volume sites -- "Accumulate... with NO
			// paired AccumulateCount anywhere in that file").
			acc.Accumulate( 8, 8, 16.0, 1.0, kTechniqueBSDF );
		}
		for( int i = 0; i < nNee; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueNEE );
		}
		acc.Solve();
		alphaBuggy = acc.GetAlpha( 8, 8 );
	}
	// nBsdf stays 200 (extra attempts never counted), rawBsdf = 200*4 + 50*16 = 1600.
	// Mbsdf = 1600/200 = 8.0 (inflated -- the ledger's own wording: "inflating
	// Mbsdf... and depressing alpha"), Mnee = 4.0 -> alpha = 4/(4+8) = 1/3.
	Check( ApproxEqual( alphaBuggy, 1.0/3.0, 0.001 ),
		"buggy (round-1 pattern): unpaired moment inflates Mbsdf, alpha depressed to 1/3" );
	Check( alphaBuggy < alphaUnwired,
		"buggy alpha is measurably LOWER than unwired alpha -- the exact 'depressed alpha' "
		"failure mode DL-72's round-2 review found" );

	// -------------------- "fixed" (round 3, this pass: paired) --------------------
	Scalar alphaFixed;
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );
		for( int i = 0; i < nOrdinaryBsdf; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueBSDF );
		}
		for( int i = 0; i < nExtra; i++ ) {
			// Round 3 fix: every Accumulate is paired with its own
			// AccumulateCount, exactly like the ordinary surface
			// continuation site and exactly like the fix landed in
			// PathTracingIntegrator.cpp / RayCaster.cpp this pass.
			acc.AccumulateCount( 8, 8, kTechniqueBSDF );
			acc.Accumulate( 8, 8, 16.0, 1.0, kTechniqueBSDF );
		}
		for( int i = 0; i < nNee; i++ ) {
			acc.AccumulateCount( 8, 8, kTechniqueNEE );
			acc.Accumulate( 8, 8, 4.0, 1.0, kTechniqueNEE );
		}
		acc.Solve();
		alphaFixed = acc.GetAlpha( 8, 8 );
	}
	// nBsdf = 250, rawBsdf = 200*4 + 50*16 = 1600, Mbsdf = 1600/250 = 6.4,
	// Mnee = 4.0 -> alpha = 4/(4+6.4) = 4/10.4 = 0.384615...
	Check( ApproxEqual( alphaFixed, 4.0/10.4, 0.001 ),
		"fixed (round 3): paired moment/count gives the theoretically correct alpha" );
	Check( fabs( alphaFixed - alphaUnwired ) > 0.05,
		"RED-PROVE: training now genuinely FIRES for the SSS/volume sites -- fixed alpha "
		"measurably differs from the 'sites contribute nothing' unwired baseline" );
	Check( fabs( alphaFixed - alphaBuggy ) > 0.03,
		"fixed alpha also measurably differs from the buggy (unpaired) alpha -- the pairing "
		"fix changes the solved value, not just silences a code path" );

	// -------------------- Design invariant: a perfectly self-consistent --------------------
	// -------------------- technique trains a phasePdf/cosinePdf-INVARIANT moment --------------------
	// This is the algebraic property the RayCaster.cpp volume fix and the
	// PathTracingIntegrator.cpp BSSRDF fix both rely on: when
	// `bsdfTimesCos` is constructed so that `bsdfTimesCos / bsdfPdf == 1`
	// identically (volume: IPhaseFunction::Pdf()==Evaluate(), so
	// bsdfTimesCos=phasePdf, bsdfPdf=phasePdf; BSSRDF:
	// bsdfTimesCos=(weight/weightSpatial)*cosinePdf, bsdfPdf=cosinePdf, so
	// the ratio is weight/weightSpatial independent of cosinePdf), the
	// trained moment for a fixed radiance reduces to a CONSTANT regardless
	// of the sampled pdf's actual value -- exactly what "this sampling
	// step contributes no variance of its own" means.  Three different
	// pdf values, same envRadiance, same bsdfTimesCos/bsdfPdf ratio (=1
	// here) -> identical trained moment.
	{
		OptimalMISAccumulator acc;
		acc.Initialize( 32, 32, config );
		const double envRadiance = 3.0;
		const double pdfs[3] = { 0.2, 1.0, 7.5 };
		for( int p = 0; p < 3; p++ ) {
			for( int i = 0; i < 40; i++ ) {
				acc.AccumulateCount( 16, 16, kTechniqueBSDF );
				// fLum = envRadiance * bsdfTimesCos = envRadiance * pdfs[p]
				// (bsdfTimesCos == bsdfPdf here, the ratio-of-1 case)
				const double fLum = envRadiance * pdfs[p];
				acc.Accumulate( 16, 16, fLum * fLum, pdfs[p], kTechniqueBSDF );
			}
		}
		for( int i = 0; i < 120; i++ ) {
			acc.AccumulateCount( 16, 16, kTechniqueNEE );
			acc.Accumulate( 16, 16, 9.0, 1.0, kTechniqueNEE );	// (Le/1)^2 = envRadiance^2
		}
		acc.Solve();
		const Scalar alphaInvariant = acc.GetAlpha( 16, 16 );
		// Every BSDF sample's moment reduces to envRadiance^2 = 9.0 regardless of
		// its pdf, matching NEE's own moment (also 9.0) exactly -> balance (0.5).
		Check( ApproxEqual( alphaInvariant, 0.5, 0.01 ),
			"phasePdf/cosinePdf-invariant design: moment collapses to envRadiance^2 "
			"regardless of the sampled pdf, matching balance heuristic here" );
	}
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	std::cout << "=== Optimal MIS Accumulator Test ===" << std::endl;

	TestEqualMoments();
	TestBSDFHighVariance();
	TestNEEHighVariance();
	TestFallback();
	TestClamping();
	TestTileBinning();
	TestEdgeInputs();
	TestWeightSumWithAlpha();
	TestReset();
	TestCountAccumulateSplit();
	TestZeroMomentFallback();
	TestDL72PairedTrainingFires();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	if( failCount > 0 ) {
		std::cout << "*** TEST SUITE FAILED ***" << std::endl;
		return 1;
	}

	std::cout << "All tests passed." << std::endl;
	return 0;
}
