//////////////////////////////////////////////////////////////////////
//
//  GaborNoise3D.cpp - Implements 3D Gabor noise via sparse
//  convolution.  Distributes Gabor kernels (oriented sinusoid
//  modulated by Gaussian) at random positions within a grid,
//  summing their contributions at each evaluation point.
//
//  Reference: Lagae et al. 2009 (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GaborNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

GaborNoise3D::GaborNoise3D(
	const Scalar dFrequency_,
	const Scalar dBandwidth_,
	const Vector3& vOrientation_,
	const Scalar dImpulseDensity_,
	const unsigned int nSeed_
) :
  dFrequency( dFrequency_ ),
  dBandwidth( dBandwidth_ > 0.01 ? dBandwidth_ : 0.01 ),
  vOrientation( vOrientation_ ),
  dImpulseDensity( dImpulseDensity_ > 0.1 ? dImpulseDensity_ : 0.1 ),
  nSeed( nSeed_ )
{
	// Normalize orientation
	Scalar len = sqrt( vOrientation.x*vOrientation.x + vOrientation.y*vOrientation.y + vOrientation.z*vOrientation.z );
	if( len > 1e-10 ) {
		vOrientation.x /= len;
		vOrientation.y /= len;
		vOrientation.z /= len;
	} else {
		vOrientation = Vector3( 1, 0, 0 );
	}
}

GaborNoise3D::~GaborNoise3D()
{
}

unsigned int GaborNoise3D::HashCell( int ix, int iy, int iz, unsigned int seed )
{
	unsigned int n = (unsigned int)ix * 702395077u + (unsigned int)iy * 915488749u + (unsigned int)iz * 2120969693u + seed * 1136930381u;
	n = (n << 13) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return n;
}

Scalar GaborNoise3D::GaborKernel( Scalar dx, Scalar dy, Scalar dz ) const
{
	// Gaussian envelope
	Scalar a = M_PI * dBandwidth;	// Gaussian width parameter
	Scalar r2 = dx*dx + dy*dy + dz*dz;
	Scalar gaussian = exp( -a * a * r2 );

	// Oriented sinusoid: cos(2*pi*frequency * dot(orientation, displacement))
	Scalar dotProd = vOrientation.x * dx + vOrientation.y * dy + vOrientation.z * dz;
	Scalar sinusoid = cos( 2.0 * M_PI * dFrequency * dotProd );

	return gaussian * sinusoid;
}

Scalar GaborNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	// Determine the effective radius of the Gabor kernel
	// (3 standard deviations of the Gaussian)
	Scalar a = M_PI * dBandwidth;
	Scalar kernelRadius = 3.0 / a;
	int searchRadius = (int)ceil( kernelRadius );
	if( searchRadius < 1 ) searchRadius = 1;
	if( searchRadius > 3 ) searchRadius = 3;	// Cap for performance

	int ix = (int)floor(x);
	int iy = (int)floor(y);
	int iz = (int)floor(z);

	Scalar total = 0.0;

	// Search neighborhood cells
	for( int dz_ = -searchRadius; dz_ <= searchRadius; dz_++ )
	{
		for( int dy_ = -searchRadius; dy_ <= searchRadius; dy_++ )
		{
			for( int dx_ = -searchRadius; dx_ <= searchRadius; dx_++ )
			{
				int cx = ix + dx_;
				int cy = iy + dy_;
				int cz = iz + dz_;

				// Generate impulses for this cell
				unsigned int h = HashCell( cx, cy, cz, nSeed );

				// Number of impulses in this cell (Poisson-like via hash)
				unsigned int numImpulses = (unsigned int)(dImpulseDensity);
				if( (h & 0xFF) < (unsigned int)((dImpulseDensity - numImpulses) * 256.0) )
					numImpulses++;

				for( unsigned int imp = 0; imp < numImpulses; imp++ )
				{
					// Generate impulse position within cell
					h = h * 1103515245u + 12345u;
					Scalar px = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
					h = h * 1103515245u + 12345u;
					Scalar py = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
					h = h * 1103515245u + 12345u;
					Scalar pz = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;

					// Generate random weight [-1, 1]
					h = h * 1103515245u + 12345u;
					Scalar weight = Scalar( (h >> 16) & 0xFFFF ) / 32768.0 - 1.0;

					// Displacement from evaluation point to impulse
					Scalar ddx = x - (cx + px);
					Scalar ddy = y - (cy + py);
					Scalar ddz = z - (cz + pz);

					total += weight * GaborKernel( ddx, ddy, ddz );
				}
			}
		}
	}

	// Normalize to [0, 1]
	// The expected variance of Gabor noise depends on impulse density
	// and kernel parameters.  We use a simple empirical normalization.
	Scalar normScale = 2.0 * sqrt( dImpulseDensity );
	if( normScale < 1.0 ) normScale = 1.0;

	Scalar result = (total / normScale + 1.0) / 2.0;
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
