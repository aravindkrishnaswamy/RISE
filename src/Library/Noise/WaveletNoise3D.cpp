//////////////////////////////////////////////////////////////////////
//
//  WaveletNoise3D.cpp - Implements 3D wavelet noise.
//  Precomputes a band-limited noise tile using the wavelet
//  decomposition approach: generate random noise, downsample,
//  upsample, subtract to get the band-pass coefficients.
//
//  Reference: Cook & DeRose 2005 "Wavelet Noise" (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WaveletNoise.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

using namespace RISE;
using namespace RISE::Implementation;

// Simple LCG for reproducible random numbers
static unsigned int s_waveletSeed = 1;
static void SeedWavelet( unsigned int s ) { s_waveletSeed = s; }
static Scalar RandWavelet()
{
	s_waveletSeed = s_waveletSeed * 1103515245u + 12345u;
	return Scalar((s_waveletSeed >> 16) & 0x7FFF) / 16384.0 - 1.0;
}

// B-spline weights for downsampling (1/4, 1/2, 1/4)
static const Scalar aCoeffs[] = { 0.25, 0.5, 0.25 };

WaveletNoise3D::WaveletNoise3D(
	const unsigned int nTileSize_,
	const Scalar dPersistence_,
	const int numOctaves_
) :
  nTileSize( nTileSize_ < 16 ? 16 : (nTileSize_ > 128 ? 128 : nTileSize_) ),
  pTile( 0 ),
  dPersistence( dPersistence_ ),
  numOctaves( numOctaves_ > 16 ? 16 : numOctaves_ )
{
	// Ensure tile size is even
	if( nTileSize & 1 ) nTileSize++;

	GenerateTile();
}

WaveletNoise3D::~WaveletNoise3D()
{
	if( pTile ) {
		GlobalLog()->PrintDelete( pTile, __FILE__, __LINE__ );
		delete [] pTile;
		pTile = 0;
	}
}

void WaveletNoise3D::Downsample( const Scalar* from, Scalar* to, int n, int stride )
{
	for( int i = 0; i < n/2; i++ ) {
		to[i * stride] = 0;
		for( int k = -1; k <= 1; k++ ) {
			int idx = (2*i + k + n) % n;
			to[i * stride] += aCoeffs[k+1] * from[idx * stride];
		}
	}
}

void WaveletNoise3D::Upsample( const Scalar* from, Scalar* to, int n, int stride )
{
	// B-spline synthesis (dual of the analysis filter).
	// Insert zeros between samples, then convolve with [1/2, 1, 1/2].
	// This is equivalent to distributing each coarse sample to its
	// fine-grid neighbors with weights [1/4, 3/4, 3/4, 1/4] overlap.
	static const Scalar pCoeffs[] = { 0.25, 0.75, 0.75, 0.25 };

	for( int i = 0; i < n; i++ ) {
		to[i * stride] = 0;
	}

	for( int i = 0; i < n/2; i++ ) {
		for( int k = 0; k < 4; k++ ) {
			int idx = (2*i + k - 1 + n) % n;
			to[idx * stride] += pCoeffs[k] * from[i * stride];
		}
	}
}

void WaveletNoise3D::GenerateTile()
{
	const int n = (int)nTileSize;
	const int n3 = n * n * n;

	pTile = new Scalar[n3];
	GlobalLog()->PrintNew( pTile, __FILE__, __LINE__, "wavelet tile" );

	Scalar* temp1 = new Scalar[n3];
	Scalar* temp2 = new Scalar[n3];

	// Step 1: Fill with random noise
	SeedWavelet( 42 );
	for( int i = 0; i < n3; i++ ) {
		pTile[i] = RandWavelet();
	}

	// Step 2: For each axis, downsample then upsample to get low-pass
	memcpy( temp1, pTile, n3 * sizeof(Scalar) );

	// X-axis pass
	for( int z = 0; z < n; z++ ) {
		for( int y = 0; y < n; y++ ) {
			Downsample( &temp1[z*n*n + y*n], &temp2[z*n*n + y*n], n, 1 );
			Upsample( &temp2[z*n*n + y*n], &temp2[z*n*n + y*n], n, 1 );
		}
	}

	// Y-axis pass (stride = n for Y axis)
	memcpy( temp1, temp2, n3 * sizeof(Scalar) );
	for( int z = 0; z < n; z++ ) {
		for( int x = 0; x < n; x++ ) {
			Downsample( &temp1[z*n*n + x], &temp2[z*n*n + x], n, n );
			Upsample( &temp2[z*n*n + x], &temp2[z*n*n + x], n, n );
		}
	}

	// Z-axis pass (stride = n*n for Z axis)
	memcpy( temp1, temp2, n3 * sizeof(Scalar) );
	for( int y = 0; y < n; y++ ) {
		for( int x = 0; x < n; x++ ) {
			Downsample( &temp1[y*n + x], &temp2[y*n + x], n, n*n );
			Upsample( &temp2[y*n + x], &temp2[y*n + x], n, n*n );
		}
	}

	// Step 3: Subtract low-pass from original to get band-pass
	for( int i = 0; i < n3; i++ ) {
		pTile[i] = pTile[i] - temp2[i];
	}

	delete [] temp1;
	delete [] temp2;
}

Scalar WaveletNoise3D::EvaluateTile( Scalar x, Scalar y, Scalar z ) const
{
	const int n = (int)nTileSize;

	// Wrap coordinates to tile
	Scalar fx = fmod( x, (Scalar)n );
	if( fx < 0 ) fx += n;
	Scalar fy = fmod( y, (Scalar)n );
	if( fy < 0 ) fy += n;
	Scalar fz = fmod( z, (Scalar)n );
	if( fz < 0 ) fz += n;

	// Trilinear interpolation
	int ix = (int)floor(fx);
	int iy = (int)floor(fy);
	int iz = (int)floor(fz);

	Scalar tx = fx - ix;
	Scalar ty = fy - iy;
	Scalar tz = fz - iz;

	int ix1 = (ix + 1) % n;
	int iy1 = (iy + 1) % n;
	int iz1 = (iz + 1) % n;
	ix = ix % n;
	iy = iy % n;
	iz = iz % n;

	// 8-corner trilinear
	Scalar c000 = pTile[iz  * n*n + iy  * n + ix ];
	Scalar c100 = pTile[iz  * n*n + iy  * n + ix1];
	Scalar c010 = pTile[iz  * n*n + iy1 * n + ix ];
	Scalar c110 = pTile[iz  * n*n + iy1 * n + ix1];
	Scalar c001 = pTile[iz1 * n*n + iy  * n + ix ];
	Scalar c101 = pTile[iz1 * n*n + iy  * n + ix1];
	Scalar c011 = pTile[iz1 * n*n + iy1 * n + ix ];
	Scalar c111 = pTile[iz1 * n*n + iy1 * n + ix1];

	Scalar c00 = c000 * (1-tx) + c100 * tx;
	Scalar c10 = c010 * (1-tx) + c110 * tx;
	Scalar c01 = c001 * (1-tx) + c101 * tx;
	Scalar c11 = c011 * (1-tx) + c111 * tx;

	Scalar c0 = c00 * (1-ty) + c10 * ty;
	Scalar c1 = c01 * (1-ty) + c11 * ty;

	return c0 * (1-tz) + c1 * tz;
}

Scalar WaveletNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	Scalar total = 0.0;
	Scalar amplitude = 1.0;
	Scalar frequency = 1.0;
	Scalar normFactor = 0.0;

	for( int i = 0; i < numOctaves; i++ ) {
		total += amplitude * EvaluateTile( x * frequency, y * frequency, z * frequency );
		normFactor += amplitude;
		amplitude *= dPersistence;
		frequency *= 2.0;
	}

	// Normalize from [-normFactor, normFactor] to [0, 1]
	if( normFactor < 1e-10 ) normFactor = 1.0;
	Scalar result = (total / normFactor + 1.0) / 2.0;
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
