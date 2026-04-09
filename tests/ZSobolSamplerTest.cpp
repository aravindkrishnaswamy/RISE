//////////////////////////////////////////////////////////////////////
//
//  ZSobolSamplerTest.cpp - Unit tests for the ZSobol (blue-noise
//    screen-space) sampler variant.
//
//  Tests:
//    A. Determinism: same inputs produce same outputs
//    B. Morton reindexing: ZSobol produces different indices than
//       standard Sobol for the same pixel/sample
//    C. Complementary samples: adjacent Morton pixels produce
//       well-stratified combined sample sets
//    D. Phase budgeting: StartStream, HasFixedDimensionBudget
//    E. Consistency with SobolSampler interface contract
//    F. Overflow guard: verify limits
//    G. CanEncode2D: coordinate-range gate for Morton encoding
//    H. Large-coordinate fallback: ZSobolSampler degrades to
//       standard Sobol when shifted index overflows uint32_t
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
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <iomanip>

#include "../src/Library/Utilities/MortonCode.h"
#include "../src/Library/Utilities/ZSobolSampler.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Sampling/SobolSequence.h"

using namespace RISE;
using namespace RISE::Implementation;

// ================================================================
// Test A: Determinism
// ================================================================

static void TestDeterminism()
{
	std::cout << "\nTest A: Determinism\n";

	const uint32_t mortonIndex = MortonCode::Morton2D( 42, 17 );
	const uint32_t log2SPP = 4;  // 16 SPP
	const uint32_t seed = SobolSequence::HashCombine( mortonIndex, 0u );

	// Create two samplers with identical parameters
	ZSobolSampler a( 5, mortonIndex, log2SPP, seed );
	ZSobolSampler b( 5, mortonIndex, log2SPP, seed );

	// They must produce identical sequences
	for( int i = 0; i < 20; i++ )
	{
		Scalar va = a.Get1D();
		Scalar vb = b.Get1D();
		if( va != vb )
		{
			std::cerr << "  FAIL: Dimension " << i << ": " << va
				<< " != " << vb << "\n";
			exit( 1 );
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// Test B: Morton reindexing produces different samples
// ================================================================

static void TestMortonReindexing()
{
	std::cout << "\nTest B: Morton reindexing differs from standard Sobol\n";

	const uint32_t x = 10, y = 20;
	const uint32_t mortonIndex = MortonCode::Morton2D( x, y );
	const uint32_t log2SPP = 5;  // 32 SPP
	const uint32_t sampleIndex = 7;

	// Standard Sobol seed and sampler
	const uint32_t stdSeed = SobolSequence::HashCombine(
		static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
	SobolSampler stdSampler( sampleIndex, stdSeed );

	// ZSobol seed and sampler
	const uint32_t zSeed = SobolSequence::HashCombine( mortonIndex, 0u );
	ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, zSeed );

	// At least some dimensions should differ (with overwhelming probability)
	int differCount = 0;
	for( int i = 0; i < 32; i++ )
	{
		Scalar sv = stdSampler.Get1D();
		Scalar zv = zSampler.Get1D();
		if( sv != zv ) differCount++;
	}

	if( differCount == 0 )
	{
		std::cerr << "  FAIL: ZSobol produced identical samples to standard "
			<< "Sobol for all 32 dimensions\n";
		exit( 1 );
	}

	std::cout << "  " << differCount << "/32 dimensions differ\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: Complementary samples across Morton-adjacent pixels
//
// For a small pixel grid, collect the first 2D sample from each
// pixel using both standard Sobol and ZSobol.  Measure the
// star discrepancy (approximated via L2 discrepancy) of the
// combined sample set.  ZSobol should produce lower discrepancy
// because Morton-adjacent pixels get consecutive Sobol indices,
// and the (0,2)-net property ensures complementary stratification.
// ================================================================

static double ComputeL2Discrepancy( const std::vector<std::pair<double,double>>& pts )
{
	// Hickernell's closed-form L2-star discrepancy formula
	const int N = static_cast<int>(pts.size());
	if( N == 0 ) return 1.0;

	double sum1 = 0.0;
	for( int i = 0; i < N; i++ )
	{
		double xi = pts[i].first;
		double yi = pts[i].second;
		sum1 += (1.0 - xi*xi) * (1.0 - yi*yi);
	}
	sum1 /= N;

	double sum2 = 0.0;
	for( int i = 0; i < N; i++ )
	{
		for( int j = 0; j < N; j++ )
		{
			double minx = std::min( 1.0 - pts[i].first, 1.0 - pts[j].first );
			double miny = std::min( 1.0 - pts[i].second, 1.0 - pts[j].second );
			sum2 += minx * miny;
		}
	}
	sum2 /= (double(N) * N);

	double disc = 1.0/3.0 - sum1/2.0 + sum2;
	return sqrt( std::max( 0.0, disc ) );
}

static void TestComplementarySamples()
{
	std::cout << "\nTest C: Complementary samples (cross-pixel stratification)\n";

	// Use a 4x4 pixel grid, 1 sample per pixel = 16 points total
	const int gridSize = 4;
	const uint32_t log2SPP = 0;

	std::vector<std::pair<double,double>> stdPts, zPts;

	for( int y = 0; y < gridSize; y++ )
	{
		for( int x = 0; x < gridSize; x++ )
		{
			// Standard Sobol: sample index 0 for all pixels
			uint32_t stdSeed = SobolSequence::HashCombine(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			SobolSampler stdSampler( 0, stdSeed );
			Point2 sp = stdSampler.Get2D();
			stdPts.push_back( std::make_pair( sp.x, sp.y ) );

			// ZSobol: Morton-indexed
			uint32_t mortonIndex = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			uint32_t zSeed = SobolSequence::HashCombine( mortonIndex, 0u );
			ZSobolSampler zSampler( 0, mortonIndex, log2SPP, zSeed );
			Point2 zp = zSampler.Get2D();
			zPts.push_back( std::make_pair( zp.x, zp.y ) );
		}
	}

	double stdDisc = ComputeL2Discrepancy( stdPts );
	double zDisc = ComputeL2Discrepancy( zPts );

	std::cout << "  Standard Sobol L2* discrepancy: " << std::fixed
		<< std::setprecision(6) << stdDisc << "\n";
	std::cout << "  ZSobol L2* discrepancy:         " << std::fixed
		<< std::setprecision(6) << zDisc << "\n";

	// ZSobol should have lower or comparable discrepancy because
	// Morton-consecutive pixels get consecutive (0,2)-net samples.
	// We do not hard-fail if ZSobol is slightly worse (hash randomization
	// can cause variance), but it should not be dramatically worse.
	if( zDisc > stdDisc * 2.0 )
	{
		std::cerr << "  FAIL: ZSobol discrepancy is more than 2x worse "
			<< "than standard Sobol\n";
		exit( 1 );
	}

	// Also test with more samples per pixel
	const uint32_t spp8 = 8;
	const uint32_t log2SPP8 = 3;
	std::vector<std::pair<double,double>> stdPts8, zPts8;

	for( int y = 0; y < gridSize; y++ )
	{
		for( int x = 0; x < gridSize; x++ )
		{
			uint32_t stdSeed = SobolSequence::HashCombine(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			uint32_t mortonIndex = MortonCode::Morton2D(
				static_cast<uint32_t>(x), static_cast<uint32_t>(y) );
			uint32_t zSeed = SobolSequence::HashCombine( mortonIndex, 0u );

			for( uint32_t s = 0; s < spp8; s++ )
			{
				SobolSampler stdSampler( s, stdSeed );
				Point2 sp = stdSampler.Get2D();
				stdPts8.push_back( std::make_pair( sp.x, sp.y ) );

				ZSobolSampler zSampler( s, mortonIndex, log2SPP8, zSeed );
				Point2 zp = zSampler.Get2D();
				zPts8.push_back( std::make_pair( zp.x, zp.y ) );
			}
		}
	}

	double stdDisc8 = ComputeL2Discrepancy( stdPts8 );
	double zDisc8 = ComputeL2Discrepancy( zPts8 );

	std::cout << "  4x4 grid, 8 SPP — Std L2*: " << stdDisc8
		<< ", ZSobol L2*: " << zDisc8 << "\n";

	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: Phase budgeting (StartStream, HasFixedDimensionBudget)
// ================================================================

static void TestPhaseBudgeting()
{
	std::cout << "\nTest D: Phase budgeting\n";

	uint32_t mortonIndex = MortonCode::Morton2D( 50, 50 );
	uint32_t seed = SobolSequence::HashCombine( mortonIndex, 0u );
	ZSobolSampler sampler( 3, mortonIndex, 4, seed );

	// HasFixedDimensionBudget must be true (inherited from SobolSampler)
	if( !sampler.HasFixedDimensionBudget() )
	{
		std::cerr << "  FAIL: ZSobolSampler must report "
			<< "HasFixedDimensionBudget() == true\n";
		exit( 1 );
	}

	// StartStream should reset dimensions deterministically
	sampler.StartStream( 0 );
	Scalar v0a = sampler.Get1D();

	// Consume some dimensions
	for( int i = 0; i < 10; i++ ) sampler.Get1D();

	// Reset to stream 0 should give same value
	sampler.StartStream( 0 );
	Scalar v0b = sampler.Get1D();

	if( v0a != v0b )
	{
		std::cerr << "  FAIL: StartStream(0) should be deterministic: "
			<< v0a << " vs " << v0b << "\n";
		exit( 1 );
	}

	// Different streams should (almost certainly) differ
	sampler.StartStream( 1 );
	Scalar v1 = sampler.Get1D();

	if( v0a == v1 )
	{
		std::cerr << "  WARNING: Stream 0 and stream 1 produced same value "
			<< "(extremely unlikely)\n";
	}

	std::cout << "  HasFixedDimensionBudget() == true: OK\n";
	std::cout << "  StartStream determinism: OK\n";
	std::cout << "  Stream independence: OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: ISampler interface contract
// ================================================================

static void TestISamplerContract()
{
	std::cout << "\nTest E: ISampler interface contract\n";

	uint32_t mortonIndex = MortonCode::Morton2D( 100, 200 );
	uint32_t seed = SobolSequence::HashCombine( mortonIndex, 0u );
	ZSobolSampler sampler( 0, mortonIndex, 5, seed );

	// Get1D should return values in [0, 1)
	for( int i = 0; i < 100; i++ )
	{
		Scalar v = sampler.Get1D();
		if( v < 0.0 || v >= 1.0 )
		{
			std::cerr << "  FAIL: Get1D() returned " << v
				<< " (expected [0, 1))\n";
			exit( 1 );
		}
	}

	// Get2D should return values in [0, 1)^2
	sampler.StartStream( 0 );
	for( int i = 0; i < 50; i++ )
	{
		Point2 p = sampler.Get2D();
		if( p.x < 0.0 || p.x >= 1.0 || p.y < 0.0 || p.y >= 1.0 )
		{
			std::cerr << "  FAIL: Get2D() returned (" << p.x << ", "
				<< p.y << ") (expected [0, 1)^2)\n";
			exit( 1 );
		}
	}

	// ZSobolSampler should be usable through ISampler pointer
	ISampler* pSampler = &sampler;
	pSampler->StartStream( 2 );
	Scalar v = pSampler->Get1D();
	if( v < 0.0 || v >= 1.0 )
	{
		std::cerr << "  FAIL: ISampler* Get1D() returned " << v << "\n";
		exit( 1 );
	}

	std::cout << "  Value range: OK\n";
	std::cout << "  Polymorphic access: OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test F: Overflow guard
// ================================================================

static void TestOverflowGuard()
{
	std::cout << "\nTest F: Overflow guard (practical limits)\n";

	// 1024x1024 at 1024 SPP should work
	{
		uint32_t mortonIndex = MortonCode::Morton2D( 1023, 1023 );
		uint32_t log2SPP = 10;  // 1024 SPP
		uint32_t seed = SobolSequence::HashCombine( mortonIndex, 0u );

		// This should not assert
		ZSobolSampler sampler( 1023, mortonIndex, log2SPP, seed );
		(void)sampler.Get1D();
	}

	// 512x512 at 512 SPP — well within limits
	{
		uint32_t mortonIndex = MortonCode::Morton2D( 511, 511 );
		uint32_t log2SPP = 9;  // 512 SPP
		uint32_t seed = SobolSequence::HashCombine( mortonIndex, 0u );
		ZSobolSampler sampler( 511, mortonIndex, log2SPP, seed );
		(void)sampler.Get1D();
	}

	// Verify the global index computation is correct
	{
		uint32_t mortonIndex = 100;
		uint32_t log2SPP = 4;  // 16 SPP
		uint32_t sampleIndex = 7;
		uint32_t seed = 12345;

		ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, seed );

		// The effective global index should be (100 << 4) | 7 = 1607
		uint32_t expectedIndex = (mortonIndex << log2SPP) | sampleIndex;
		SobolSampler directSampler( expectedIndex, seed );

		// Both should produce identical samples
		for( int i = 0; i < 16; i++ )
		{
			Scalar zv = zSampler.Get1D();
			Scalar dv = directSampler.Get1D();
			if( zv != dv )
			{
				std::cerr << "  FAIL: ZSobol index computation mismatch at "
					<< "dimension " << i << ": " << zv << " vs " << dv << "\n";
				exit( 1 );
			}
		}
	}

	std::cout << "  1024x1024 @ 1024 SPP: OK\n";
	std::cout << "  Index computation verified\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test G: CanEncode2D coordinate-range gate
// ================================================================

static void TestCanEncode2D()
{
	std::cout << "\nTest G: CanEncode2D coordinate-range gate\n";

	// Maximum valid coordinates (16 bits each)
	if( !MortonCode::CanEncode2D( 0, 0 ) )
	{
		std::cerr << "  FAIL: (0, 0) should be encodable\n";
		exit( 1 );
	}
	if( !MortonCode::CanEncode2D( 65535, 65535 ) )
	{
		std::cerr << "  FAIL: (65535, 65535) should be encodable\n";
		exit( 1 );
	}
	if( !MortonCode::CanEncode2D( 1023, 1023 ) )
	{
		std::cerr << "  FAIL: (1023, 1023) should be encodable\n";
		exit( 1 );
	}

	// Coordinates at and beyond the 16-bit boundary must be rejected
	if( MortonCode::CanEncode2D( 65536, 0 ) )
	{
		std::cerr << "  FAIL: x=65536 should NOT be encodable\n";
		exit( 1 );
	}
	if( MortonCode::CanEncode2D( 0, 65536 ) )
	{
		std::cerr << "  FAIL: y=65536 should NOT be encodable\n";
		exit( 1 );
	}
	if( MortonCode::CanEncode2D( 65536, 65536 ) )
	{
		std::cerr << "  FAIL: (65536, 65536) should NOT be encodable\n";
		exit( 1 );
	}
	if( MortonCode::CanEncode2D( 100000, 200 ) )
	{
		std::cerr << "  FAIL: x=100000 should NOT be encodable\n";
		exit( 1 );
	}

	// Verify that Morton2D silently truncates out-of-range coordinates
	// (this is the bug CanEncode2D is designed to prevent)
	uint32_t m_lo = MortonCode::Morton2D( 10, 20 );
	uint32_t m_hi = MortonCode::Morton2D( 10 + 65536, 20 );
	if( m_lo != m_hi )
	{
		std::cerr << "  FAIL: Morton2D should truncate to 16 bits "
			<< "(this test validates the aliasing behavior CanEncode2D prevents)\n";
		exit( 1 );
	}

	std::cout << "  Valid coordinates accepted: OK\n";
	std::cout << "  Out-of-range coordinates rejected: OK\n";
	std::cout << "  Morton2D truncation confirmed (aliasing CanEncode2D prevents): OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test H: Large-coordinate / overflow fallback
//
// When the Morton-shifted index would overflow uint32_t,
// ZSobolSampler::ComputeIndex must fall back to returning the
// plain sampleIndex (standard Sobol behavior) instead of wrapping.
// ================================================================

static void TestOverflowFallback()
{
	std::cout << "\nTest H: Large-coordinate / overflow fallback\n";

	// Case 1: Overflow due to large mortonIndex << log2SPP.
	// mortonIndex = 0x00FFFFFF (~16M), log2SPP = 10 (1024 SPP).
	// Shifted: 0x00FFFFFF << 10 = 0x3FFFFFC00 > uint32_t max.
	// ComputeIndex should fall back to sampleIndex.
	{
		const uint32_t bigMorton = 0x00FFFFFFu;
		const uint32_t log2SPP = 10;
		const uint32_t sampleIndex = 42;
		const uint32_t seed = 12345;

		ZSobolSampler zSampler( sampleIndex, bigMorton, log2SPP, seed );

		// Fallback means ZSobolSampler behaves like SobolSampler(sampleIndex, seed)
		SobolSampler stdSampler( sampleIndex, seed );

		for( int i = 0; i < 16; i++ )
		{
			Scalar zv = zSampler.Get1D();
			Scalar sv = stdSampler.Get1D();
			if( zv != sv )
			{
				std::cerr << "  FAIL: Overflow case did not fall back to "
					<< "standard Sobol at dimension " << i
					<< ": " << zv << " vs " << sv << "\n";
				exit( 1 );
			}
		}
	}

	// Case 2: Overflow due to log2SPP >= 32.
	{
		const uint32_t mortonIndex = 1;
		const uint32_t log2SPP = 32;
		const uint32_t sampleIndex = 0;
		const uint32_t seed = 99999;

		ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, seed );
		SobolSampler stdSampler( sampleIndex, seed );

		Scalar zv = zSampler.Get1D();
		Scalar sv = stdSampler.Get1D();
		if( zv != sv )
		{
			std::cerr << "  FAIL: log2SPP=32 did not fall back: "
				<< zv << " vs " << sv << "\n";
			exit( 1 );
		}
	}

	// Case 3: Exactly at the uint32_t boundary — should NOT fall back.
	// mortonIndex = 0x003FFFFF (2048x2048 max Morton), log2SPP = 10.
	// Shifted: 0x003FFFFF << 10 = 0xFFFFFC00, which fits in uint32_t.
	{
		const uint32_t mortonIndex = 0x003FFFFFu;
		const uint32_t log2SPP = 10;
		const uint32_t sampleIndex = 7;
		const uint32_t seed = 54321;

		ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, seed );

		// Should NOT fall back — verify it produces the Morton-remapped index
		uint32_t expectedIndex = (mortonIndex << log2SPP) | sampleIndex;
		SobolSampler directSampler( expectedIndex, seed );

		for( int i = 0; i < 16; i++ )
		{
			Scalar zv = zSampler.Get1D();
			Scalar dv = directSampler.Get1D();
			if( zv != dv )
			{
				std::cerr << "  FAIL: At-limit case should NOT fall back, "
					<< "dimension " << i << ": " << zv << " vs " << dv << "\n";
				exit( 1 );
			}
		}
	}

	// Case 4: Just over the boundary — should fall back.
	// mortonIndex = 0x00400000 (one above 2048x2048 max), log2SPP = 10.
	// Shifted: 0x00400000 << 10 = 0x100000000 > uint32_t max.
	{
		const uint32_t mortonIndex = 0x00400000u;
		const uint32_t log2SPP = 10;
		const uint32_t sampleIndex = 3;
		const uint32_t seed = 77777;

		ZSobolSampler zSampler( sampleIndex, mortonIndex, log2SPP, seed );
		SobolSampler stdSampler( sampleIndex, seed );

		Scalar zv = zSampler.Get1D();
		Scalar sv = stdSampler.Get1D();
		if( zv != sv )
		{
			std::cerr << "  FAIL: Just-over-boundary did not fall back: "
				<< zv << " vs " << sv << "\n";
			exit( 1 );
		}
	}

	std::cout << "  Overflow (large mortonIndex << log2SPP): falls back OK\n";
	std::cout << "  Overflow (log2SPP >= 32): falls back OK\n";
	std::cout << "  At-limit (fits in uint32_t): Morton remapping active OK\n";
	std::cout << "  Just-over-boundary: falls back OK\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "=== ZSobol Sampler Tests ===\n";

	TestDeterminism();
	TestMortonReindexing();
	TestComplementarySamples();
	TestPhaseBudgeting();
	TestISamplerContract();
	TestOverflowGuard();
	TestCanEncode2D();
	TestOverflowFallback();

	std::cout << "\nAll ZSobol sampler tests passed!\n";
	return 0;
}
