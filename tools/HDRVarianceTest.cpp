//////////////////////////////////////////////////////////////////////
//
//  HDRVarianceTest.cpp - Compute per-pixel variance across N HDR
//    renders.  Used to quantify how much rendering noise / inter-run
//    variance two integrator configurations produce — e.g.,
//    "OpenPGL with rrAffectsDirectContribution=true vs =false at
//    same SPP".
//
//    Reads K linear-space OpenEXR (.exr) files (must be same size),
//    computes per-pixel sample mean μ(x,y) and unbiased sample
//    variance σ²(x,y), and reports image-aggregate statistics.
//
//    EXR is preferred over Radiance .hdr because RISE's HDRReader
//    has a known bug in old-format RLE decoding (uses stale `buf`
//    instead of just-read `col` at HDRReader.cpp:200-203), causing
//    pixel exponents to be wrong.  EXR roundtrips cleanly.
//
//    Two modes:
//
//      HDRVarianceTest.exe <run0.exr> <run1.exr> [<run2.exr> ...]
//        Pure variance across N independent runs (K must be >= 2).
//        Reports image-mean variance — the standard noise metric.
//
//      HDRVarianceTest.exe --ref <reference.exr> <run0.exr> ...
//        RMSE-vs-reference: each run's pixels compared to the
//        reference (assumed converged ground truth at high SPP).
//        Reports mean and per-run RMSE values.
//
//    All math is done on linear RGB floats (no sRGB encoding /
//    quantization), so the noise floor is the actual rendering
//    variance, not 8-bit PNG stair-stepping.
//
//    Author: Aravind Krishnaswamy
//    Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IReadBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;

static bool LoadHDR( const char* path, std::vector<RISEColor>& px,
	unsigned int& w, unsigned int& h )
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
		std::fprintf( stderr, "failed EXRReader for %s\n", path );
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
	double sumDbg = 0;
	for( unsigned int y = 0; y < h; y++ )
		for( unsigned int x = 0; x < w; x++ ) {
			RISEColor c = pImage->GetPEL( x, y );
			px[y*w + x] = c;
			sumDbg += c.base[0] + c.base[1] + c.base[2];
		}
	if( const char* dbg = std::getenv( "HDRVAR_DEBUG" ) ) {
		(void)dbg;
		RISEColor cc = pImage->GetPEL( w/2, h/2 );
		std::fprintf( stderr, "  [debug] %s: %ux%u sumRGB=%.6e center=(%.6e,%.6e,%.6e)\n",
			path, w, h, sumDbg, cc.base[0], cc.base[1], cc.base[2] );
	}

	pImage->release();
	pReader->release();
	pBuf->release();
	return true;
}

static int RunVarianceMode( int argc, char** argv )
{
	const int K = argc - 1;
	std::vector<std::vector<RISEColor>> runs( K );
	unsigned int W = 0, H = 0;

	for( int k = 0; k < K; k++ ) {
		unsigned int w, h;
		if( !LoadHDR( argv[1 + k], runs[k], w, h ) ) {
			return 2;
		}
		if( k == 0 ) { W = w; H = h; }
		else if( w != W || h != H ) {
			std::fprintf( stderr,
				"size mismatch: %s is %ux%u, expected %ux%u\n",
				argv[1+k], w, h, W, H );
			return 3;
		}
	}

	const size_t N = static_cast<size_t>(W) * H;
	const double invK = 1.0 / static_cast<double>(K);
	const double invKm1 = 1.0 / static_cast<double>(K - 1);

	// Per-pixel sample mean & unbiased variance, reduced to image
	// aggregates as we go (no need to keep per-pixel arrays).
	double sumVar = 0.0;          // Σ_pixels Σ_channels σ²(x,y,c)
	double sumMeanSq = 0.0;       // Σ μ²(x,y,c) — for SNR-ish metric
	double sumMean = 0.0;         // Σ μ(x,y,c)
	double maxPixVar = 0.0;       // peak per-pixel variance (max over channels)
	std::vector<double> perPixVar; perPixVar.reserve( N );

	double channelMean[3] = {0,0,0};

	for( size_t i = 0; i < N; i++ ) {
		double mean[3] = {0,0,0};
		for( int k = 0; k < K; k++ ) {
			mean[0] += runs[k][i].base[0];
			mean[1] += runs[k][i].base[1];
			mean[2] += runs[k][i].base[2];
		}
		mean[0] *= invK; mean[1] *= invK; mean[2] *= invK;

		double var[3] = {0,0,0};
		for( int k = 0; k < K; k++ ) {
			const double dR = runs[k][i].base[0] - mean[0];
			const double dG = runs[k][i].base[1] - mean[1];
			const double dB = runs[k][i].base[2] - mean[2];
			var[0] += dR*dR;
			var[1] += dG*dG;
			var[2] += dB*dB;
		}
		var[0] *= invKm1; var[1] *= invKm1; var[2] *= invKm1;

		const double pixVar = var[0] + var[1] + var[2];
		const double pixVarMax = std::max( {var[0], var[1], var[2]} );
		sumVar += pixVar;
		if( pixVarMax > maxPixVar ) maxPixVar = pixVarMax;
		perPixVar.push_back( pixVarMax );

		sumMean += mean[0] + mean[1] + mean[2];
		sumMeanSq += mean[0]*mean[0] + mean[1]*mean[1] + mean[2]*mean[2];

		channelMean[0] += mean[0];
		channelMean[1] += mean[1];
		channelMean[2] += mean[2];
	}

	channelMean[0] /= N; channelMean[1] /= N; channelMean[2] /= N;
	const double meanPerPixVar = sumVar / (3.0 * N);
	const double meanPerPixSigma = std::sqrt( meanPerPixVar );
	const double meanIntensity = sumMean / (3.0 * N);
	const double relSigma = meanIntensity > 0 ? meanPerPixSigma / meanIntensity : 0.0;

	std::sort( perPixVar.begin(), perPixVar.end() );
	const double medianPixVar  = perPixVar[ perPixVar.size() / 2 ];
	const double p95PixVar     = perPixVar[ std::min<size_t>( perPixVar.size()-1,
		static_cast<size_t>( 0.95 * perPixVar.size() ) ) ];
	const double p99PixVar     = perPixVar[ std::min<size_t>( perPixVar.size()-1,
		static_cast<size_t>( 0.99 * perPixVar.size() ) ) ];

	std::printf( "=== HDR variance: K=%d runs ===\n", K );
	for( int k = 0; k < K; k++ ) std::printf( "  in[%d]: %s\n", k, argv[1+k] );
	std::printf( "size:                       %ux%u (%zu pixels)\n", W, H, N );
	std::printf( "channel mean(R,G,B):        %.4f %.4f %.4f\n",
		channelMean[0], channelMean[1], channelMean[2] );
	std::printf( "\n" );
	std::printf( "mean per-pixel σ²:          %.6e (linear-space variance, summed over channels then averaged)\n",
		meanPerPixVar );
	std::printf( "mean per-pixel σ:           %.6e (sqrt — comparable to per-pixel RMSE between runs / √2)\n",
		meanPerPixSigma );
	std::printf( "relative noise σ/μ:         %.4f%%\n", 100.0 * relSigma );
	std::printf( "\n" );
	std::printf( "distribution of per-pixel σ²(max channel):\n" );
	std::printf( "  median:                   %.6e\n", medianPixVar );
	std::printf( "  95th percentile:          %.6e\n", p95PixVar );
	std::printf( "  99th percentile:          %.6e\n", p99PixVar );
	std::printf( "  max:                      %.6e\n", maxPixVar );

	return 0;
}

static int RunReferenceMode( int argc, char** argv )
{
	// argv[1] == "--ref", argv[2] == reference path, argv[3..] == run paths
	std::vector<RISEColor> ref;
	unsigned int W = 0, H = 0;
	if( !LoadHDR( argv[2], ref, W, H ) ) return 2;

	const int K = argc - 3;
	const size_t N = static_cast<size_t>(W) * H;

	std::vector<double> runRmses;
	double sumRunMSE = 0.0;

	std::printf( "=== HDR RMSE vs reference: K=%d runs ===\n", K );
	std::printf( "ref:                        %s (%ux%u)\n", argv[2], W, H );

	for( int k = 0; k < K; k++ ) {
		std::vector<RISEColor> cand;
		unsigned int w, h;
		if( !LoadHDR( argv[3 + k], cand, w, h ) ) return 2;
		if( w != W || h != H ) {
			std::fprintf( stderr, "size mismatch on %s\n", argv[3+k] );
			return 3;
		}

		double sumSq = 0.0;
		for( size_t i = 0; i < N; i++ ) {
			const double dR = cand[i].base[0] - ref[i].base[0];
			const double dG = cand[i].base[1] - ref[i].base[1];
			const double dB = cand[i].base[2] - ref[i].base[2];
			sumSq += dR*dR + dG*dG + dB*dB;
		}
		const double mse = sumSq / (3.0 * N);
		const double rmse = std::sqrt( mse );
		runRmses.push_back( rmse );
		sumRunMSE += mse;
		std::printf( "  run[%d] %s: MSE=%.6e RMSE=%.6e\n",
			k, argv[3+k], mse, rmse );
	}

	const double meanMSE = sumRunMSE / static_cast<double>(K);
	const double meanRMSE = std::sqrt( meanMSE );
	double minR = runRmses[0], maxR = runRmses[0];
	for( double r : runRmses ) { minR = std::min(minR,r); maxR = std::max(maxR,r); }

	std::printf( "\n" );
	std::printf( "mean MSE across runs:       %.6e\n", meanMSE );
	std::printf( "mean RMSE across runs:      %.6e\n", meanRMSE );
	std::printf( "min/max per-run RMSE:       %.6e / %.6e\n", minR, maxR );

	return 0;
}

int main( int argc, char** argv )
{
	if( argc < 3 ) {
		std::fprintf( stderr,
			"usage:\n"
			"  variance:      %s <run0.hdr> <run1.hdr> [<run2.hdr> ...]\n"
			"  vs reference:  %s --ref <reference.hdr> <run0.hdr> [<run1.hdr> ...]\n",
			argv[0], argv[0] );
		return 1;
	}

	if( std::strcmp( argv[1], "--ref" ) == 0 ) {
		if( argc < 4 ) {
			std::fprintf( stderr, "--ref needs <reference.hdr> and at least one run\n" );
			return 1;
		}
		return RunReferenceMode( argc, argv );
	}

	return RunVarianceMode( argc, argv );
}
