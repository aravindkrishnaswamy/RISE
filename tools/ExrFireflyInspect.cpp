//////////////////////////////////////////////////////////////////////
//
//  ExrFireflyInspect.cpp - Find and report firefly pixels in an EXR.
//
//  Usage:
//    ExrFireflyInspect.exe <image.exr> [topN]
//
//  Reports the top-N pixels by linear-luminance ratio to neighborhood
//  median.  Shows actual HDR values (not 8-bit-clamped), so we can
//  tell "barely over 1.0" (expected MLT variance) from "10000.0"
//  (genuine bug producing huge splats).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IReadBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;

int main( int argc, char** argv )
{
	if( argc < 2 ) {
		std::fprintf( stderr, "usage: %s <image.exr> [topN]\n", argv[0] );
		std::fprintf( stderr, "       %s <image.exr> --probe x1,y1 x2,y2 ...\n", argv[0] );
		return 1;
	}
	const char* path = argv[1];
	int topN = ( argc > 2 ) ? std::atoi( argv[2] ) : 30;
	bool probeMode = ( argc > 2 && std::strcmp( argv[2], "--probe" ) == 0 );

	IReadBuffer* pBuf = 0;
	if( !RISE_API_CreateDiskFileReadBuffer( &pBuf, path ) || !pBuf ) {
		std::fprintf( stderr, "failed to open %s\n", path );
		return 1;
	}
	IRasterImageReader* pReader = 0;
	RISE_API_CreateEXRReader( &pReader, *pBuf, eColorSpace_Rec709RGB_Linear );
	if( !pReader ) {
		pBuf->release();
		return 1;
	}
	IRasterImage* pImage = 0;
	if( !RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor( 0, 0, 0, 0 ) ) ||
	    !pImage ) {
		pReader->release(); pBuf->release();
		return 1;
	}
	pImage->LoadImage( pReader );
	const unsigned int W = pImage->GetWidth();
	const unsigned int H = pImage->GetHeight();
	std::vector<double> L( static_cast<size_t>( W ) * H, 0.0 );
	double maxL = 0;
	double sumL = 0;
	for( unsigned int y = 0; y < H; y++ ) {
		for( unsigned int x = 0; x < W; x++ ) {
			RISEColor c = pImage->GetPEL( x, y );
			double lum = 0.2126 * c.base[0] + 0.7152 * c.base[1] + 0.0722 * c.base[2];
			L[y * W + x] = lum;
			if( lum > maxL ) maxL = lum;
			sumL += lum;
		}
	}
	const double meanL = sumL / ( static_cast<double>( W ) * H );

	std::printf( "Image: %s  (%ux%u)\n", path, W, H );
	std::printf( "  meanL = %.4f   maxL = %.4f   max/mean = %.1fx\n",
		meanL, maxL, maxL / std::max( meanL, 1e-9 ) );

	// Probe mode: print pixel values at requested (x, y) coordinates.
	if( probeMode ) {
		for( int i = 3; i < argc; i++ ) {
			int px = -1, py = -1;
			if( std::sscanf( argv[i], "%d,%d", &px, &py ) == 2 &&
			    px >= 0 && py >= 0 &&
			    static_cast<unsigned int>( px ) < W &&
			    static_cast<unsigned int>( py ) < H ) {
				RISEColor c = pImage->GetPEL( px, py );
				const double lum = L[py * W + px];
				std::printf( "  (%4d, %4d) RGB=(%7.4f, %7.4f, %7.4f)  L=%7.4f\n",
					px, py, c.base[0], c.base[1], c.base[2], lum );
			}
		}
		pImage->release(); pReader->release(); pBuf->release();
		return 0;
	}
	pImage->release(); pReader->release(); pBuf->release();

	// Top-N pixels by absolute luminance.
	struct LumPx { unsigned int x, y; double L; };
	std::vector<LumPx> sortedLum;
	sortedLum.reserve( static_cast<size_t>( W ) * H );
	for( unsigned int y = 0; y < H; y++ ) {
		for( unsigned int x = 0; x < W; x++ ) {
			sortedLum.push_back( { x, y, L[y * W + x] } );
		}
	}
	std::partial_sort( sortedLum.begin(),
		sortedLum.begin() + std::min<size_t>( topN, sortedLum.size() ),
		sortedLum.end(),
		[]( const LumPx& a, const LumPx& b ) { return a.L > b.L; } );
	std::printf( "Top %d pixels by absolute luminance:\n", topN );
	std::printf( "%4s  %4s  %12s\n", "x", "y", "L_pixel" );
	for( int i = 0; i < topN && i < static_cast<int>( sortedLum.size() ); i++ ) {
		std::printf( "%4u  %4u  %12.3f\n",
			sortedLum[i].x, sortedLum[i].y, sortedLum[i].L );
	}
	std::printf( "\n" );

	// Find pixels whose luminance is dramatically above neighborhood median.
	struct Hit { unsigned int x, y; double cur, med, ratio; };
	std::vector<Hit> hits;
	const int radius = 2;
	std::vector<double> nbr( ( 2 * radius + 1 ) * ( 2 * radius + 1 ) - 1 );
	for( unsigned int y = radius; y + radius < H; y++ ) {
		for( unsigned int x = radius; x + radius < W; x++ ) {
			const double cur = L[y * W + x];
			if( cur < 0.01 ) continue;	// too dim to be an interesting firefly
			int n = 0;
			for( int dy = -radius; dy <= radius; dy++ ) {
				for( int dx = -radius; dx <= radius; dx++ ) {
					if( dx == 0 && dy == 0 ) continue;
					nbr[n++] = L[( y + dy ) * W + ( x + dx )];
				}
			}
			std::sort( nbr.begin(), nbr.begin() + n );
			const double med = std::max( nbr[n / 2], 1e-9 );
			const double ratio = cur / med;
			if( ratio >= 4.0 ) {
				hits.push_back( { x, y, cur, med, ratio } );
			}
		}
	}
	std::sort( hits.begin(), hits.end(),
		[]( const Hit& a, const Hit& b ) { return a.ratio > b.ratio; } );
	if( static_cast<int>( hits.size() ) > topN ) hits.resize( topN );

	std::printf( "Found %zu firefly pixels (luminance >= 4x neighborhood median)\n",
		hits.size() );
	std::printf( "%4s  %4s  %12s  %12s  %10s\n", "x", "y", "L_pixel", "L_med", "ratio" );
	for( const Hit& h : hits ) {
		std::printf( "%4u  %4u  %12.3f  %12.4f  %10.1f\n",
			h.x, h.y, h.cur, h.med, h.ratio );
	}
	return 0;
}
