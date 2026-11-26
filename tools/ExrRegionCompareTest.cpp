//////////////////////////////////////////////////////////////////////
//
//  ExrRegionCompareTest.cpp - Compare mean radiance of rectangular
//  regions across two EXR images.
//
//  Usage:
//    ExrRegionCompareTest.exe <a.exr> <b.exr> x0 y0 x1 y1 [label]
//
//    Reports:
//      mean_a:  linear radiance (R, G, B) in region from image A
//      mean_b:  linear radiance (R, G, B) in region from image B
//      ratio:   mean_a / mean_b per channel
//
//    Used to compare SMS vs VCM raw linear values (pre-tonemap) in a
//    specific umbra/caustic region.
//
//////////////////////////////////////////////////////////////////////

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

int main( int argc, char** argv )
{
	if( argc < 7 ) {
		std::fprintf( stderr,
			"usage: %s <a.exr> <b.exr> x0 y0 x1 y1 [label]\n", argv[0] );
		return 1;
	}

	const char* pathA = argv[1];
	const char* pathB = argv[2];
	const int x0 = std::atoi( argv[3] );
	const int y0 = std::atoi( argv[4] );
	const int x1 = std::atoi( argv[5] );
	const int y1 = std::atoi( argv[6] );
	const char* label = (argc >= 8) ? argv[7] : "region";

	std::vector<RISEColor> a, b;
	unsigned int wA, hA, wB, hB;
	if( !LoadEXR( pathA, a, wA, hA ) ) return 2;
	if( !LoadEXR( pathB, b, wB, hB ) ) return 2;

	if( wA != wB || hA != hB ) {
		std::fprintf( stderr, "size mismatch: %ux%u vs %ux%u\n", wA, hA, wB, hB );
		return 3;
	}

	if( x0 < 0 || y0 < 0 || x1 > static_cast<int>(wA) || y1 > static_cast<int>(hA) ||
		x0 >= x1 || y0 >= y1 ) {
		std::fprintf( stderr,
			"bad region (%d,%d)-(%d,%d) for image size %ux%u\n",
			x0, y0, x1, y1, wA, hA );
		return 4;
	}

	double sumA[3] = { 0.0, 0.0, 0.0 };
	double sumB[3] = { 0.0, 0.0, 0.0 };
	long long n = 0;
	for( int y = y0; y < y1; y++ ) {
		for( int x = x0; x < x1; x++ ) {
			const RISEColor& pa = a[ static_cast<size_t>(y) * wA + x ];
			const RISEColor& pb = b[ static_cast<size_t>(y) * wA + x ];
			sumA[0] += pa.base[0]; sumA[1] += pa.base[1]; sumA[2] += pa.base[2];
			sumB[0] += pb.base[0]; sumB[1] += pb.base[1]; sumB[2] += pb.base[2];
			n++;
		}
	}

	const double mA[3] = { sumA[0]/n, sumA[1]/n, sumA[2]/n };
	const double mB[3] = { sumB[0]/n, sumB[1]/n, sumB[2]/n };
	const double rR = (mB[0] > 0) ? mA[0]/mB[0] : 0;
	const double rG = (mB[1] > 0) ? mA[1]/mB[1] : 0;
	const double rB = (mB[2] > 0) ? mA[2]/mB[2] : 0;

	std::printf( "[%s] (%d,%d)-(%d,%d) n=%lld  A=(%.5f,%.5f,%.5f)  B=(%.5f,%.5f,%.5f)  ratio=(%.3f,%.3f,%.3f)\n",
		label, x0, y0, x1, y1, n,
		mA[0], mA[1], mA[2],
		mB[0], mB[1], mB[2],
		rR, rG, rB );

	return 0;
}
