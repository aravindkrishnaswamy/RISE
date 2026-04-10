//////////////////////////////////////////////////////////////////////
//
//  SampledWavelengthsTest.cpp - Unit tests for the SampledWavelengths
//    class used in Hero Wavelength Spectral Sampling (HWSS).
//
//  Tests:
//    A. Equidistant spacing: companions are Delta apart (wrap-around)
//    B. Range bounds: all wavelengths fall within [lambda_min, lambda_max]
//    C. Wrap-around: hero near lambda_max wraps companions correctly
//    D. PDF values: uniform PDF = 1 / (lambda_max - lambda_min)
//    E. Termination logic: TerminateSecondary kills companions only
//    F. NumActive: tracks active count correctly
//    G. Edge cases: u=0, u near 1, single-nm range
//    H. Stratification: 4 wavelengths partition the range evenly
//
//  Build (from project root):
//    make -C build/make/rise tests
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cmath>
#include <cstdlib>

#include "../src/Library/Utilities/Color/SampledWavelengths.h"

using namespace RISE;

static const double kEps = 1e-10;

// ================================================================
// Test A: Equidistant spacing
// ================================================================

static void TestEquidistantSpacing()
{
	std::cout << "\nTest A: Equidistant spacing\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;
	const Scalar range = lambda_max - lambda_min;
	const Scalar delta = range / 4.0;

	// Test several hero positions
	const double uValues[] = { 0.0, 0.1, 0.25, 0.5, 0.75, 0.999 };
	const int nTests = sizeof(uValues) / sizeof(uValues[0]);

	for( int t = 0; t < nTests; t++ )
	{
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			uValues[t], lambda_min, lambda_max );

		// Check spacing between consecutive wavelengths (with wrap-around)
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			unsigned int j = (i + 1) % SampledWavelengths::N;
			Scalar diff = swl.lambda[j] - swl.lambda[i];
			if( diff < 0 ) diff += range;

			if( fabs( diff - delta ) > kEps )
			{
				std::cerr << "  FAIL: u=" << uValues[t]
					<< " lambda[" << i << "]=" << swl.lambda[i]
					<< " lambda[" << j << "]=" << swl.lambda[j]
					<< " diff=" << diff << " expected=" << delta << "\n";
				exit( 1 );
			}
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test B: Range bounds
// ================================================================

static void TestRangeBounds()
{
	std::cout << "\nTest B: Range bounds\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;

	// Sweep many u values
	for( int i = 0; i < 1000; i++ )
	{
		Scalar u = static_cast<Scalar>(i) / 1000.0;
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			u, lambda_min, lambda_max );

		for( unsigned int j = 0; j < SampledWavelengths::N; j++ )
		{
			if( swl.lambda[j] < lambda_min - kEps || swl.lambda[j] > lambda_max + kEps )
			{
				std::cerr << "  FAIL: u=" << u
					<< " lambda[" << j << "]=" << swl.lambda[j]
					<< " out of range [" << lambda_min << ", " << lambda_max << "]\n";
				exit( 1 );
			}
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: Wrap-around
// ================================================================

static void TestWrapAround()
{
	std::cout << "\nTest C: Wrap-around at high hero wavelength\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;

	// Hero near the end: u=0.9 → hero = 380 + 0.9*400 = 740
	SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
		0.9, lambda_min, lambda_max );

	assert( fabs( swl.lambda[0] - 740.0 ) < kEps );

	// Companion 1: 740 + 100 = 840 → wraps to 840 - 400 = 440
	// Actually: fmod(740 - 380 + 100, 400) + 380 = fmod(460, 400) + 380 = 60 + 380 = 440
	assert( fabs( swl.lambda[1] - 440.0 ) < kEps );

	// Companion 2: fmod(740 - 380 + 200, 400) + 380 = fmod(560, 400) + 380 = 160 + 380 = 540
	assert( fabs( swl.lambda[2] - 540.0 ) < kEps );

	// Companion 3: fmod(740 - 380 + 300, 400) + 380 = fmod(660, 400) + 380 = 260 + 380 = 640
	assert( fabs( swl.lambda[3] - 640.0 ) < kEps );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: PDF values
// ================================================================

static void TestPDFValues()
{
	std::cout << "\nTest D: PDF values\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;
	const Scalar expectedPdf = 1.0 / (lambda_max - lambda_min);

	SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
		0.5, lambda_min, lambda_max );

	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
	{
		if( fabs( swl.pdf[i] - expectedPdf ) > kEps )
		{
			std::cerr << "  FAIL: pdf[" << i << "]=" << swl.pdf[i]
				<< " expected=" << expectedPdf << "\n";
			exit( 1 );
		}
	}

	// Test with different range
	const Scalar narrow_min = 400.0;
	const Scalar narrow_max = 700.0;
	const Scalar narrowPdf = 1.0 / (narrow_max - narrow_min);

	SampledWavelengths swl2 = SampledWavelengths::SampleEquidistant(
		0.3, narrow_min, narrow_max );

	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
	{
		assert( fabs( swl2.pdf[i] - narrowPdf ) < kEps );
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: Termination logic
// ================================================================

static void TestTermination()
{
	std::cout << "\nTest E: Termination logic\n";

	SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
		0.5, 380.0, 780.0 );

	// Initially no companions terminated
	assert( !swl.SecondaryTerminated() );
	assert( swl.NumActive() == 4 );
	assert( !swl.terminated[0] );
	assert( !swl.terminated[1] );
	assert( !swl.terminated[2] );
	assert( !swl.terminated[3] );

	// Terminate secondaries
	swl.TerminateSecondary();

	assert( swl.SecondaryTerminated() );
	assert( swl.NumActive() == 1 );

	// Hero is never terminated
	assert( !swl.terminated[0] );
	assert( swl.terminated[1] );
	assert( swl.terminated[2] );
	assert( swl.terminated[3] );

	// Hero wavelength unchanged
	Scalar expectedHero = 380.0 + 0.5 * 400.0;
	assert( fabs( swl.HeroLambda() - expectedHero ) < kEps );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test F: NumActive tracks correctly
// ================================================================

static void TestNumActive()
{
	std::cout << "\nTest F: NumActive tracking\n";

	SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
		0.25, 380.0, 780.0 );

	assert( swl.NumActive() == 4 );

	// Manually terminate one companion
	swl.terminated[2] = true;
	assert( swl.NumActive() == 3 );
	assert( !swl.SecondaryTerminated() );

	// Terminate remaining companions
	swl.terminated[1] = true;
	swl.terminated[3] = true;
	assert( swl.NumActive() == 1 );
	assert( swl.SecondaryTerminated() );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test G: Edge cases
// ================================================================

static void TestEdgeCases()
{
	std::cout << "\nTest G: Edge cases\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;
	const Scalar range = lambda_max - lambda_min;
	const Scalar delta = range / 4.0;

	// u = 0: hero at lambda_min
	{
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			0.0, lambda_min, lambda_max );

		assert( fabs( swl.lambda[0] - lambda_min ) < kEps );
		assert( fabs( swl.lambda[1] - (lambda_min + delta) ) < kEps );
		assert( fabs( swl.lambda[2] - (lambda_min + 2.0 * delta) ) < kEps );
		assert( fabs( swl.lambda[3] - (lambda_min + 3.0 * delta) ) < kEps );
	}

	// u very close to 1: hero near lambda_max
	{
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			0.9999, lambda_min, lambda_max );

		// Hero very close to lambda_max
		assert( swl.lambda[0] >= lambda_min && swl.lambda[0] <= lambda_max );

		// All companions still in range
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			assert( swl.lambda[i] >= lambda_min - kEps );
			assert( swl.lambda[i] <= lambda_max + kEps );
		}
	}

	// Default constructor: all zeros, not terminated
	{
		SampledWavelengths swl;
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			assert( swl.lambda[i] == 0 );
			assert( swl.pdf[i] == 0 );
			assert( !swl.terminated[i] );
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test H: Stratification — 4 wavelengths partition the range
// ================================================================

static void TestStratification()
{
	std::cout << "\nTest H: Stratification covers full range\n";

	const Scalar lambda_min = 380.0;
	const Scalar lambda_max = 780.0;
	const Scalar range = lambda_max - lambda_min;
	const Scalar quarterRange = range / 4.0;

	// For any hero position, the 4 wavelengths should each land in a
	// different quarter of the range (stratified coverage).
	for( int t = 0; t < 100; t++ )
	{
		Scalar u = static_cast<Scalar>(t) / 100.0;
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			u, lambda_min, lambda_max );

		// Map each wavelength to its quarter index
		bool quarterHit[4] = { false, false, false, false };

		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			int quarter = static_cast<int>(
				(swl.lambda[i] - lambda_min) / quarterRange );
			if( quarter >= 4 ) quarter = 3;  // edge case at lambda_max
			quarterHit[quarter] = true;
		}

		// Note: when a wavelength lands exactly on a quarter boundary,
		// integer truncation may assign it to the adjacent quarter.
		// This is expected and harmless.  The stronger gap-based check
		// below verifies the equidistant property directly.
	}

	// Stronger check: sorted wavelengths span the full range.
	// The maximum gap between any two consecutive sorted wavelengths
	// (with wrap-around) should be exactly range/4.
	for( int t = 0; t < 100; t++ )
	{
		Scalar u = static_cast<Scalar>(t) / 100.0;
		SampledWavelengths swl = SampledWavelengths::SampleEquidistant(
			u, lambda_min, lambda_max );

		// Sort wavelengths
		Scalar sorted[4];
		for( unsigned int i = 0; i < 4; i++ ) sorted[i] = swl.lambda[i];
		for( int i = 0; i < 3; i++ )
		{
			for( int j = i + 1; j < 4; j++ )
			{
				if( sorted[j] < sorted[i] )
				{
					Scalar tmp = sorted[i];
					sorted[i] = sorted[j];
					sorted[j] = tmp;
				}
			}
		}

		// Check gaps (including wrap-around gap)
		for( int i = 0; i < 4; i++ )
		{
			int j = (i + 1) % 4;
			Scalar gap = sorted[j] - sorted[i];
			if( gap < 0 ) gap += range;

			if( fabs( gap - quarterRange ) > kEps )
			{
				std::cerr << "  FAIL: u=" << u
					<< " gap between sorted[" << i << "]=" << sorted[i]
					<< " and sorted[" << j << "]=" << sorted[j]
					<< " is " << gap << " expected " << quarterRange << "\n";
				exit( 1 );
			}
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================

int main( int argc, char** argv )
{
	std::cout << "=== SampledWavelengths Unit Tests ===" << std::endl;

	TestEquidistantSpacing();
	TestRangeBounds();
	TestWrapAround();
	TestPDFValues();
	TestTermination();
	TestNumActive();
	TestEdgeCases();
	TestStratification();

	std::cout << "\nAll SampledWavelengths tests passed!" << std::endl;

	return 0;
}
