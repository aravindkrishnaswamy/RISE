//////////////////////////////////////////////////////////////////////
//
//  ImageDiffTest.cpp - Report pixel-level statistics between two PNGs.
//
//    Used to quantify rendered-image convergence — e.g., "SMS render
//    vs VCM reference should differ only by noise".
//
//    Invocation:
//      ImageDiffTest.exe <reference.png> <candidate.png>
//      ImageDiffTest.exe <reference.png> <candidate.png> <out_diff.png>
//
//    Outputs:
//      mean  per-channel RMS error
//      max   peak per-pixel L2 distance
//      mae   mean-absolute-error per channel
//      structure-similarity-ish via tiled mean difference
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IReadBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;

static bool LoadPNG( const char* path, std::vector<RISEColor>& px, unsigned int& w, unsigned int& h )
{
	IReadBuffer* pBuf = 0;
	if( !RISE_API_CreateDiskFileReadBuffer( &pBuf, path ) || !pBuf ) {
		std::fprintf( stderr, "failed to open %s\n", path );
		return false;
	}

	IRasterImageReader* pReader = 0;
	RISE_API_CreatePNGReader( &pReader, *pBuf, eColorSpace_sRGB );
	if( !pReader ) {
		pBuf->release();
		std::fprintf( stderr, "failed PNGReader for %s\n", path );
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
	for( unsigned int y = 0; y < h; y++ )
		for( unsigned int x = 0; x < w; x++ )
			px[y*w + x] = pImage->GetPEL( x, y );

	pImage->release();
	pReader->release();
	pBuf->release();
	return true;
}

int main( int argc, char** argv )
{
	if( argc < 3 ) {
		std::fprintf( stderr, "usage: %s <ref.png> <cand.png> [out_diff.png]\n", argv[0] );
		return 1;
	}

	std::vector<RISEColor> ref, cand;
	unsigned int wR, hR, wC, hC;
	if( !LoadPNG( argv[1], ref, wR, hR ) ) return 2;
	if( !LoadPNG( argv[2], cand, wC, hC ) ) return 2;

	if( wR != wC || hR != hC ) {
		std::fprintf( stderr, "size mismatch: %ux%u vs %ux%u\n", wR, hR, wC, hC );
		return 3;
	}

	const size_t N = wR * hR;
	double sumSq = 0.0;
	double sumAbs = 0.0;
	double maxDist = 0.0;
	double refMean[3] = {0, 0, 0};
	double candMean[3] = {0, 0, 0};

	for( size_t i = 0; i < N; i++ ) {
		const Scalar rR = ref[i].base[0], rG = ref[i].base[1], rB = ref[i].base[2];
		const Scalar cR = cand[i].base[0], cG = cand[i].base[1], cB = cand[i].base[2];
		const Scalar dR = cR - rR, dG = cG - rG, dB = cB - rB;
		const double sq = dR*dR + dG*dG + dB*dB;
		sumSq += sq;
		sumAbs += std::fabs(dR) + std::fabs(dG) + std::fabs(dB);
		if( sq > maxDist ) maxDist = sq;
		refMean[0] += rR; refMean[1] += rG; refMean[2] += rB;
		candMean[0] += cR; candMean[1] += cG; candMean[2] += cB;
	}

	const double rmse = std::sqrt( sumSq / (3.0 * N) );
	const double mae = sumAbs / (3.0 * N);
	const double maxL2 = std::sqrt( maxDist );
	for( int c = 0; c < 3; c++ ) { refMean[c] /= N; candMean[c] /= N; }

	std::printf( "=== image-diff: %s vs %s ===\n", argv[1], argv[2] );
	std::printf( "size:    %ux%u (%zu pixels)\n", wR, hR, N );
	std::printf( "mean(ref):  R=%.4f G=%.4f B=%.4f\n", refMean[0], refMean[1], refMean[2] );
	std::printf( "mean(cand): R=%.4f G=%.4f B=%.4f\n", candMean[0], candMean[1], candMean[2] );
	std::printf( "mean diff:  R=%+.4f G=%+.4f B=%+.4f\n",
		candMean[0]-refMean[0], candMean[1]-refMean[1], candMean[2]-refMean[2] );
	std::printf( "RMSE:    %.4f (0=identical, 1=max possible for sRGB-as-linear)\n", rmse );
	std::printf( "MAE:     %.4f\n", mae );
	std::printf( "max L2:  %.4f (worst pixel)\n", maxL2 );

	// Tile analysis: 8x8 grid of mean(|diff|) to show where the big
	// differences are.  Helps see if the diff is broadly distributed
	// (= noise) or concentrated in a specific region (= structural gap).
	std::printf( "per-tile MAE (8x8 grid):\n" );
	const unsigned int tileW = wR / 8, tileH = hR / 8;
	for( unsigned int ty = 0; ty < 8; ty++ ) {
		std::printf( "  " );
		for( unsigned int tx = 0; tx < 8; tx++ ) {
			double s = 0.0;
			unsigned int n = 0;
			for( unsigned int y = ty*tileH; y < (ty+1)*tileH; y++ ) {
				for( unsigned int x = tx*tileW; x < (tx+1)*tileW; x++ ) {
					const size_t i = y*wR + x;
					s += std::fabs( cand[i].base[0] - ref[i].base[0] )
					   + std::fabs( cand[i].base[1] - ref[i].base[1] )
					   + std::fabs( cand[i].base[2] - ref[i].base[2] );
					n++;
				}
			}
			const double tileMae = (n > 0) ? (s / (3.0 * n)) : 0.0;
			std::printf( "%5.2f ", tileMae );
		}
		std::printf( "\n" );
	}

	// Diff image output deferred — use tile grid above to localize.

	return 0;
}
