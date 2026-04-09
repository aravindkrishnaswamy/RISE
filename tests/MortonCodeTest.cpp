//////////////////////////////////////////////////////////////////////
//
//  MortonCodeTest.cpp - Unit tests for 2D Morton code utilities.
//
//  Tests:
//    A. Known values: verify specific Morton codes
//    B. Round-trip: InverseMorton2D(Morton2D(x,y)) == (x,y)
//    C. Z-order adjacency: 2x2 blocks produce consecutive codes
//    D. RoundUpPow2 correctness
//    E. Log2Int correctness
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

#include "../src/Library/Utilities/MortonCode.h"

using namespace RISE;

// ================================================================
// Test A: Known Morton code values
// ================================================================

static void TestKnownValues()
{
	std::cout << "\nTest A: Known Morton code values\n";

	// Morton2D interleaves x (even bits) and y (odd bits):
	//   (0,0) = 0b00 = 0
	//   (1,0) = 0b01 = 1
	//   (0,1) = 0b10 = 2
	//   (1,1) = 0b11 = 3
	//   (2,0) = 0b0100 = 4
	//   (0,2) = 0b1000 = 8
	//   (2,2) = 0b1100 = 12
	//   (3,3) = 0b1111 = 15
	assert( MortonCode::Morton2D( 0, 0 ) == 0 );
	assert( MortonCode::Morton2D( 1, 0 ) == 1 );
	assert( MortonCode::Morton2D( 0, 1 ) == 2 );
	assert( MortonCode::Morton2D( 1, 1 ) == 3 );
	assert( MortonCode::Morton2D( 2, 0 ) == 4 );
	assert( MortonCode::Morton2D( 0, 2 ) == 8 );
	assert( MortonCode::Morton2D( 2, 2 ) == 12 );
	assert( MortonCode::Morton2D( 3, 3 ) == 15 );

	// Larger values
	assert( MortonCode::Morton2D( 4, 0 ) == 16 );
	assert( MortonCode::Morton2D( 0, 4 ) == 32 );
	assert( MortonCode::Morton2D( 255, 255 ) == 65535 );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test B: Round-trip InverseMorton2D(Morton2D(x,y)) == (x,y)
// ================================================================

static void TestRoundTrip()
{
	std::cout << "\nTest B: Round-trip encoding/decoding\n";

	// Exhaustive for small coordinates
	for( uint32_t y = 0; y < 64; y++ )
	{
		for( uint32_t x = 0; x < 64; x++ )
		{
			uint32_t code = MortonCode::Morton2D( x, y );
			uint32_t rx, ry;
			MortonCode::InverseMorton2D( code, rx, ry );
			if( rx != x || ry != y )
			{
				std::cerr << "  FAIL: Morton2D(" << x << ", " << y
					<< ") = " << code << ", InverseMorton2D gives ("
					<< rx << ", " << ry << ")\n";
				exit( 1 );
			}
		}
	}

	// Spot-check larger coordinates
	const uint32_t testCoords[] = { 0, 1, 127, 128, 255, 256, 511, 1023,
		1024, 2047, 4095, 8191, 16383, 32767, 65535 };
	const int N = sizeof(testCoords) / sizeof(testCoords[0]);

	for( int i = 0; i < N; i++ )
	{
		for( int j = 0; j < N; j++ )
		{
			uint32_t x = testCoords[i];
			uint32_t y = testCoords[j];
			uint32_t code = MortonCode::Morton2D( x, y );
			uint32_t rx, ry;
			MortonCode::InverseMorton2D( code, rx, ry );
			if( rx != x || ry != y )
			{
				std::cerr << "  FAIL: Morton2D(" << x << ", " << y
					<< ") = " << code << ", InverseMorton2D gives ("
					<< rx << ", " << ry << ")\n";
				exit( 1 );
			}
		}
	}

	std::cout << "  Tested " << (64*64 + N*N) << " round-trips\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test C: Z-order adjacency — 2x2 blocks are consecutive
// ================================================================

static void TestZOrderAdjacency()
{
	std::cout << "\nTest C: Z-order adjacency (2x2 blocks consecutive)\n";

	// In Morton order, the four pixels of any aligned 2x2 block
	// (bx*2, by*2) through (bx*2+1, by*2+1) should produce four
	// consecutive Morton codes: base, base+1, base+2, base+3.
	int failures = 0;

	for( uint32_t by = 0; by < 32; by++ )
	{
		for( uint32_t bx = 0; bx < 32; bx++ )
		{
			uint32_t m00 = MortonCode::Morton2D( bx*2,   by*2 );
			uint32_t m10 = MortonCode::Morton2D( bx*2+1, by*2 );
			uint32_t m01 = MortonCode::Morton2D( bx*2,   by*2+1 );
			uint32_t m11 = MortonCode::Morton2D( bx*2+1, by*2+1 );

			if( m10 != m00+1 || m01 != m00+2 || m11 != m00+3 )
			{
				std::cerr << "  FAIL at block (" << bx << ", " << by
					<< "): codes = " << m00 << ", " << m10 << ", "
					<< m01 << ", " << m11 << "\n";
				failures++;
				if( failures > 5 ) exit( 1 );
			}
		}
	}

	if( failures > 0 ) exit( 1 );

	std::cout << "  Tested " << (32*32) << " blocks\n";
	std::cout << "  Passed!\n";
}

// ================================================================
// Test D: RoundUpPow2 correctness
// ================================================================

static void TestRoundUpPow2()
{
	std::cout << "\nTest D: RoundUpPow2\n";

	assert( MortonCode::RoundUpPow2( 0 ) == 1 );
	assert( MortonCode::RoundUpPow2( 1 ) == 1 );
	assert( MortonCode::RoundUpPow2( 2 ) == 2 );
	assert( MortonCode::RoundUpPow2( 3 ) == 4 );
	assert( MortonCode::RoundUpPow2( 4 ) == 4 );
	assert( MortonCode::RoundUpPow2( 5 ) == 8 );
	assert( MortonCode::RoundUpPow2( 7 ) == 8 );
	assert( MortonCode::RoundUpPow2( 8 ) == 8 );
	assert( MortonCode::RoundUpPow2( 9 ) == 16 );
	assert( MortonCode::RoundUpPow2( 16 ) == 16 );
	assert( MortonCode::RoundUpPow2( 17 ) == 32 );
	assert( MortonCode::RoundUpPow2( 255 ) == 256 );
	assert( MortonCode::RoundUpPow2( 256 ) == 256 );
	assert( MortonCode::RoundUpPow2( 1000 ) == 1024 );
	assert( MortonCode::RoundUpPow2( 1024 ) == 1024 );
	assert( MortonCode::RoundUpPow2( 1025 ) == 2048 );
	assert( MortonCode::RoundUpPow2( 65536 ) == 65536 );

	std::cout << "  Passed!\n";
}

// ================================================================
// Test E: Log2Int correctness
// ================================================================

static void TestLog2Int()
{
	std::cout << "\nTest E: Log2Int\n";

	assert( MortonCode::Log2Int( 0 ) == 0 );
	assert( MortonCode::Log2Int( 1 ) == 0 );
	assert( MortonCode::Log2Int( 2 ) == 1 );
	assert( MortonCode::Log2Int( 3 ) == 1 );
	assert( MortonCode::Log2Int( 4 ) == 2 );
	assert( MortonCode::Log2Int( 7 ) == 2 );
	assert( MortonCode::Log2Int( 8 ) == 3 );
	assert( MortonCode::Log2Int( 15 ) == 3 );
	assert( MortonCode::Log2Int( 16 ) == 4 );
	assert( MortonCode::Log2Int( 31 ) == 4 );
	assert( MortonCode::Log2Int( 32 ) == 5 );
	assert( MortonCode::Log2Int( 256 ) == 8 );
	assert( MortonCode::Log2Int( 1024 ) == 10 );
	assert( MortonCode::Log2Int( 65536 ) == 16 );

	// Verify consistency: Log2Int(RoundUpPow2(n)) == log2 of
	// the rounded-up power of 2
	for( uint32_t n = 1; n <= 2048; n++ )
	{
		uint32_t p = MortonCode::RoundUpPow2( n );
		uint32_t l = MortonCode::Log2Int( p );
		// p should be a power of 2, so 1 << l == p
		if( (1u << l) != p )
		{
			std::cerr << "  FAIL: RoundUpPow2(" << n << ") = " << p
				<< ", Log2Int(" << p << ") = " << l
				<< ", but 1<<" << l << " = " << (1u << l) << "\n";
			exit( 1 );
		}
	}

	std::cout << "  Passed!\n";
}

// ================================================================
// main
// ================================================================

int main( int /*argc*/, char** /*argv*/ )
{
	std::cout << "=== Morton Code Tests ===\n";

	TestKnownValues();
	TestRoundTrip();
	TestZOrderAdjacency();
	TestRoundUpPow2();
	TestLog2Int();

	std::cout << "\nAll Morton code tests passed!\n";
	return 0;
}
