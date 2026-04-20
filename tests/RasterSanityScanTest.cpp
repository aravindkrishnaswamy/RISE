//////////////////////////////////////////////////////////////////////
//
//  RasterSanityScanTest.cpp - Unit tests for ScanRasterImageForPathologicalPixels.
//
//    Covers:
//    - Clean image (all non-negative values) reports zero negatives.
//    - Negative-channel pixel is counted exactly once and its magnitude
//      is the most-negative channel value over all pathological pixels.
//    - A pixel with multiple negative channels is still counted once
//      (per-pixel semantics).
//    - Alpha-channel negatives are ignored.
//
//    Underlying concern: pixel reconstruction filters with negative
//    side lobes (Mitchell, Lanczos) deposit small negative values at
//    pixels that happen to sit past bright neighbors in their support.
//    PNG / TGA writers clamp negatives to 0 → black pixel artifact
//    that was historically silent.  The file-write sanity scan catches
//    this class of issue before it ships to an observer.
//
//    NaN / Inf detection was deliberately NOT added because RISE
//    builds with -ffast-math which makes those predicates unreliable.
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/RasterSanityScan.h"

using namespace RISE;

namespace
{
	// Minimal in-memory raster image for testing.  Implements only
	// what the scan needs.
	class TestRasterImage : public IRasterImage
	{
	public:
		TestRasterImage( unsigned int w, unsigned int h ) :
			width( w ), height( h ), pixels( w * h )
		{
			for( auto& p : pixels ) {
				p = RISEColor( RISEPel( 0, 0, 0 ), 1.0 );
			}
		}

		RISEColor GetPEL( const unsigned int x, const unsigned int y ) const override
		{
			return pixels[y * width + x];
		}
		void SetPEL( const unsigned int x, const unsigned int y, const RISEColor& p ) override
		{
			pixels[y * width + x] = p;
		}
		void Clear( const RISEColor& c, const Rect* ) override
		{
			for( auto& p : pixels ) p = c;
		}
		void DumpImage( IRasterImageWriter* ) const override {}
		void LoadImage( IRasterImageReader* ) override {}
		unsigned int GetWidth() const override { return width; }
		unsigned int GetHeight() const override { return height; }

		// IReference — tests stack-allocate; these are no-ops.
		void addref() const override {}
		bool release() const override { return false; }
		unsigned int refcount() const override { return 1; }

	private:
		unsigned int width;
		unsigned int height;
		std::vector<RISEColor> pixels;
	};

	bool IsClose( double a, double b, double eps = 1e-9 )
	{
		return std::fabs( a - b ) < eps;
	}
}

// Clean image yields zero negatives.
static void TestCleanImage()
{
	std::cout << "Testing clean image..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.Clear( RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ), nullptr );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 0 );
	assert( r.maxNegativeMagnitude == 0.0 );
}

// One negative channel → negativeCount == 1.
static void TestSingleNegativePixel()
{
	std::cout << "Testing single negative pixel..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 1, 2, RISEColor( RISEPel( 0.3, -0.25, 0.1 ), 1.0 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 1 );
	assert( IsClose( r.maxNegativeMagnitude, 0.25 ) );
}

// Two channels of ONE pixel negative → still counts as one pixel.
static void TestMultipleNegativeChannelsOnOnePixel()
{
	std::cout << "Testing multiple negative channels on one pixel..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 0, 0, RISEColor( RISEPel( -0.1, -0.5, 0.2 ), 1.0 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 1 );
	// max magnitude is the most-negative of the two negative channels
	assert( IsClose( r.maxNegativeMagnitude, 0.5 ) );
}

// Multiple DIFFERENT pixels each with one negative channel.
static void TestMultipleNegativePixels()
{
	std::cout << "Testing multiple negative pixels..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 0, 0, RISEColor( RISEPel( -0.05, 0.2, 0.2 ), 1.0 ) );
	img.SetPEL( 2, 3, RISEColor( RISEPel( 0.1, -0.3, 0.1 ), 1.0 ) );
	img.SetPEL( 3, 0, RISEColor( RISEPel( 0.0, 0.0, -0.01 ), 1.0 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 3 );
	assert( IsClose( r.maxNegativeMagnitude, 0.3 ) );
}

// Alpha channel is ignored (negative alpha does NOT trigger any count).
static void TestAlphaIgnored()
{
	std::cout << "Testing alpha ignored..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 1, 1, RISEColor( RISEPel( 0.1, 0.1, 0.1 ), -0.5 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 0 );
}

// Small negative below rounding noise is still counted — the scan
// reports truthfully; callers decide what magnitude they care about.
static void TestTinyNegativeIsCounted()
{
	std::cout << "Testing tiny negative counted..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 0, 0, RISEColor( RISEPel( -1e-15, 0.1, 0.1 ), 1.0 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 1 );
	assert( r.maxNegativeMagnitude > 0 );
}

// Large magnitude negative is correctly reported.
static void TestLargeNegativeMagnitude()
{
	std::cout << "Testing large negative magnitude..." << std::endl;
	TestRasterImage img( 4, 4 );
	img.SetPEL( 2, 2, RISEColor( RISEPel( -42.0, 0.0, 0.0 ), 1.0 ) );
	const RasterSanityReport r = ScanRasterImageForPathologicalPixels( img );
	assert( r.negativeCount == 1 );
	assert( IsClose( r.maxNegativeMagnitude, 42.0 ) );
}

int main()
{
	TestCleanImage();
	TestSingleNegativePixel();
	TestMultipleNegativeChannelsOnOnePixel();
	TestMultipleNegativePixels();
	TestAlphaIgnored();
	TestTinyNegativeIsCounted();
	TestLargeNegativeMagnitude();
	std::cout << "All RasterSanityScan tests passed." << std::endl;
	return 0;
}
