//////////////////////////////////////////////////////////////////////
//
//  WorleyNoise3D.cpp - Implements 3D Worley (cellular) noise.
//  Uses a jittered grid with one feature point per cell and
//  searches the 3x3x3 neighborhood to find nearest distances.
//
//  Reference: Worley 1996 "A Cellular Texture Basis Function"
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
#include "WorleyNoise.h"
#include <math.h>
#include <float.h>

using namespace RISE;
using namespace RISE::Implementation;

WorleyNoise3D::WorleyNoise3D(
	const Scalar dJitter_,
	const WorleyDistanceMetric eMetric_,
	const WorleyOutputMode eOutput_
) :
  dJitter( dJitter_ ),
  eMetric( eMetric_ ),
  eOutput( eOutput_ )
{
}

WorleyNoise3D::~WorleyNoise3D()
{
}

/// Hash function using large primes to generate pseudo-random values
/// from integer cell coordinates.  Returns a 32-bit hash.
/// Uses unsigned arithmetic throughout to avoid signed overflow UB.
unsigned int WorleyNoise3D::HashCell( int ix, int iy, int iz )
{
	// Cast to unsigned before multiplication to make wrapping well-defined
	unsigned int n = (unsigned int)ix * 702395077u + (unsigned int)iy * 915488749u + (unsigned int)iz * 2120969693u;
	n = (n << 13) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return n;
}

Scalar WorleyNoise3D::ComputeDistance( Scalar dx, Scalar dy, Scalar dz ) const
{
	switch( eMetric )
	{
	case eWorley_Manhattan:
		return fabs(dx) + fabs(dy) + fabs(dz);
	case eWorley_Chebyshev:
		{
			Scalar ax = fabs(dx), ay = fabs(dy), az = fabs(dz);
			Scalar m = ax > ay ? ax : ay;
			return m > az ? m : az;
		}
	case eWorley_Euclidean:
	default:
		return sqrt( dx*dx + dy*dy + dz*dz );
	}
}

Scalar WorleyNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	// Determine which cell we're in
	int ix = (int)floor(x);
	int iy = (int)floor(y);
	int iz = (int)floor(z);

	// Fractional part within the cell
	Scalar fx = x - ix;
	Scalar fy = y - iy;
	Scalar fz = z - iz;

	Scalar f1 = 1e20;	// Distance to nearest feature point
	Scalar f2 = 1e20;	// Distance to second nearest

	// Search 3x3x3 neighborhood
	for( int dz_ = -1; dz_ <= 1; dz_++ )
	{
		for( int dy_ = -1; dy_ <= 1; dy_++ )
		{
			for( int dx_ = -1; dx_ <= 1; dx_++ )
			{
				int cx = ix + dx_;
				int cy = iy + dy_;
				int cz = iz + dz_;

				// Generate feature point position within this cell
				unsigned int h = HashCell( cx, cy, cz );

				// Convert hash to [0,1) range for x,y,z offsets.
				// Use high bits (>> 16) for better LCG quality.
				Scalar px = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
				h = h * 1103515245u + 12345u;	// Advance the hash for y
				Scalar py = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
				h = h * 1103515245u + 12345u;	// Advance the hash for z
				Scalar pz = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;

				// Apply jitter (0 = center of cell, 1 = fully random)
				px = 0.5 + dJitter * (px - 0.5);
				py = 0.5 + dJitter * (py - 0.5);
				pz = 0.5 + dJitter * (pz - 0.5);

				// Distance from the sample point to this feature point
				Scalar ddx = (dx_ + px) - fx;
				Scalar ddy = (dy_ + py) - fy;
				Scalar ddz = (dz_ + pz) - fz;

				Scalar dist = ComputeDistance( ddx, ddy, ddz );

				// Insertion sort into f1, f2
				if( dist < f1 )
				{
					f2 = f1;
					f1 = dist;
				}
				else if( dist < f2 )
				{
					f2 = dist;
				}
			}
		}
	}

	// Produce output based on selected mode
	Scalar result;
	switch( eOutput )
	{
	case eWorley_F2:
		result = f2;
		break;
	case eWorley_F2minusF1:
		result = f2 - f1;
		break;
	case eWorley_F1:
	default:
		result = f1;
		break;
	}

	// Normalize to [0,1] range.  The normalization scale depends on
	// both the distance metric and the output mode.  These values
	// are empirically chosen to map the typical range of each
	// (metric, output) combination to fill [0,1] without excessive
	// clipping.  With full jitter, worst-case distances can exceed
	// these estimates, so we clamp afterwards.
	static const Scalar normTable[3][3] = {
		// Euclidean:  F1,  F2,   F2-F1
		{             1.0, 1.5,  0.75 },
		// Manhattan:  F1,  F2,   F2-F1
		{             1.5, 2.5,  1.2  },
		// Chebyshev:  F1,  F2,   F2-F1
		{             0.6, 1.0,  0.5  }
	};

	unsigned int mi = (unsigned int)eMetric;
	unsigned int oi = (unsigned int)eOutput;
	if( mi > 2 ) mi = 0;
	if( oi > 2 ) oi = 0;
	Scalar normScale = normTable[mi][oi];

	result = result / normScale;

	// Clamp to [0, 1]
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
