//////////////////////////////////////////////////////////////////////
//
//  FindFireflyTest.cpp - Scan an EXR for firefly pixels.
//
//  For every pixel, computes the local-neighborhood (default 5x5)
//  median luminance, then reports the top-N pixels ranked by the
//  ratio pixel_luminance / neighborhood_median.  The per-pixel ratio
//  is what makes a firefly visible — a bright pixel surrounded by
//  other bright pixels (e.g. a legitimate caustic) has a ratio near
//  1, while an outlier sample isolated against a darker neighborhood
//  has a large ratio.  Median is more robust than mean against
//  clustered fireflies.
//
//  Usage:
//    FindFireflyTest <a.exr> [top_n=20] [win=5] [min_lum=0.5]
//
//    top_n    number of fireflies to report
//    win      odd window size for the local neighborhood
//    min_lum  absolute luminance floor; pixels dimmer than this are
//             skipped so we don't rank noise in dark regions
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IReadBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;

static bool LoadEXR(
	const char* path,
	std::vector<RISEColor>& px,
	unsigned int& w,
	unsigned int& h )
{
	IReadBuffer* pBuf = 0;
	if( !RISE_API_CreateDiskFileReadBuffer( &pBuf, path ) || !pBuf ) {
		std::fprintf( stderr, "failed to open %s\n", path );
		return false;
	}

	IRasterImageReader* pReader = 0;
	RISE_API_CreateEXRReader( &pReader, *pBuf, eColorSpace_Rec709RGB_Linear );
	if( !pReader ) {
		pBuf->release();
		return false;
	}

	IRasterImage* pImage = 0;
	if( !RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) ) || !pImage ) {
		pReader->release();
		pBuf->release();
		return false;
	}
	pImage->LoadImage( pReader );

	w = pImage->GetWidth();
	h = pImage->GetHeight();
	px.resize( w * h );
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			px[y*w + x] = pImage->GetPEL( x, y );
		}
	}

	pImage->release();
	pReader->release();
	pBuf->release();
	return true;
}

static inline double Luminance( const RISEColor& c )
{
	// Rec.709 luminance
	return 0.2126 * c.base[0] + 0.7152 * c.base[1] + 0.0722 * c.base[2];
}

struct Firefly
{
	int x;
	int y;
	double lum;
	double median;
	double ratio;
	double r;
	double g;
	double b;
};

int main( int argc, char** argv )
{
	if( argc < 2 ) {
		std::fprintf( stderr,
			"usage: %s <a.exr> [top_n=20] [win=5] [min_lum=0.5]\n", argv[0] );
		return 1;
	}

	const char* path   = argv[1];
	const int topN     = (argc >= 3) ? std::atoi( argv[2] ) : 20;
	const int win      = (argc >= 4) ? std::atoi( argv[3] ) : 5;
	const double minLum = (argc >= 5) ? std::atof( argv[4] ) : 0.5;

	if( win < 3 || (win % 2) == 0 ) {
		std::fprintf( stderr, "win must be an odd integer >= 3 (got %d)\n", win );
		return 1;
	}

	std::vector<RISEColor> img;
	unsigned int w = 0, h = 0;
	if( !LoadEXR( path, img, w, h ) ) return 2;

	const int half = win / 2;

	// Precompute luminance grid for faster neighborhood lookups
	std::vector<double> lum( w * h );
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			lum[ y * w + x ] = Luminance( img[ y * w + x ] );
		}
	}

	std::vector<Firefly> candidates;
	candidates.reserve( 1024 );

	std::vector<double> neigh;
	neigh.reserve( win * win );

	for( int y = 0; y < static_cast<int>(h); y++ ) {
		for( int x = 0; x < static_cast<int>(w); x++ ) {
			const double l = lum[ y * w + x ];
			if( l < minLum ) continue;

			neigh.clear();
			for( int dy = -half; dy <= half; dy++ ) {
				const int ny = y + dy;
				if( ny < 0 || ny >= static_cast<int>(h) ) continue;
				for( int dx = -half; dx <= half; dx++ ) {
					const int nx = x + dx;
					if( nx < 0 || nx >= static_cast<int>(w) ) continue;
					if( dx == 0 && dy == 0 ) continue;
					neigh.push_back( lum[ ny * w + nx ] );
				}
			}
			if( neigh.empty() ) continue;

			const size_t m = neigh.size() / 2;
			std::nth_element( neigh.begin(), neigh.begin() + m, neigh.end() );
			const double median = neigh[ m ];
			if( median <= 0 ) continue;

			const double ratio = l / median;
			if( ratio < 2.0 ) continue;

			Firefly f;
			f.x = x;
			f.y = y;
			f.lum = l;
			f.median = median;
			f.ratio = ratio;
			const RISEColor& c = img[ y * w + x ];
			f.r = c.base[0]; f.g = c.base[1]; f.b = c.base[2];
			candidates.push_back( f );
		}
	}

	std::sort( candidates.begin(), candidates.end(),
		[]( const Firefly& a, const Firefly& b ) { return a.ratio > b.ratio; } );

	const int reportN = std::min( topN, static_cast<int>( candidates.size() ) );
	std::printf( "# %s %ux%u  candidates=%zu  reporting top %d (win=%d, min_lum=%.2f)\n",
		path, w, h, candidates.size(), reportN, win, minLum );
	std::printf( "# rank  x   y   lum     median  ratio  RGB\n" );
	for( int i = 0; i < reportN; i++ ) {
		const Firefly& f = candidates[i];
		std::printf( "%4d  %3d %3d  %7.3f  %6.3f  %6.2fx  (%.3f, %.3f, %.3f)\n",
			i + 1, f.x, f.y, f.lum, f.median, f.ratio, f.r, f.g, f.b );
	}

	return 0;
}
