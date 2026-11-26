//////////////////////////////////////////////////////////////////////
//
//  DomainWarpNoise3D.cpp - Implements 3D domain-warped noise.
//  Uses nested coordinate distortion through Perlin noise to
//  create swirling, organic patterns resembling marble, lava,
//  or exotic gas formations.
//
//  Level 1: noise(p + amp * warp(p))
//  Level 2: noise(p + amp * warp(p + amp * warp(p)))
//  Level 3: noise(p + amp * warp(p + amp * warp(p + amp * warp(p))))
//
//  Each warp evaluation uses three noise lookups with distinct
//  offsets to generate a 3D displacement vector.
//
//  Reference: Inigo Quilez, "Domain Warping"
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
#include "DomainWarpNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

DomainWarpNoise3D::DomainWarpNoise3D(
	const RealSimpleInterpolator& interp,
	const Scalar dPersistence,
	const int numOctaves,
	const Scalar dWarpAmplitude_,
	const unsigned int nWarpLevels_
) :
  pNoise( new PerlinNoise3D( interp, dPersistence, numOctaves < 32 ? numOctaves : 32 ) ),
  dWarpAmplitude( dWarpAmplitude_ ),
  nWarpLevels( nWarpLevels_ > 3 ? 3 : nWarpLevels_ )
{
	GlobalLog()->PrintNew( pNoise, __FILE__, __LINE__, "perlin noise" );
}

DomainWarpNoise3D::~DomainWarpNoise3D()
{
	safe_release( pNoise );
}

Scalar DomainWarpNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	// Distinct offsets to decorrelate the three noise channels
	// used for x, y, z displacement.  These are arbitrary but
	// large enough to avoid correlation with the base noise.
	static const Scalar offX1 = 5.2, offY1 = 1.3, offZ1 = 3.7;
	static const Scalar offX2 = 1.7, offY2 = 9.2, offZ2 = 6.1;
	static const Scalar offX3 = 8.3, offY3 = 2.8, offZ3 = 4.9;

	Scalar px = x, py = y, pz = z;

	// Apply warp levels (each level nests the previous warp)
	for( unsigned int level = 0; level < nWarpLevels; level++ )
	{
		// Compute 3D displacement vector from three noise lookups
		Scalar dx = pNoise->Evaluate( px + offX1, py + offY1, pz + offZ1 );
		Scalar dy = pNoise->Evaluate( px + offX2, py + offY2, pz + offZ2 );
		Scalar dz = pNoise->Evaluate( px + offX3, py + offY3, pz + offZ3 );

		// Warp the coordinates
		px = x + dWarpAmplitude * dx;
		py = y + dWarpAmplitude * dy;
		pz = z + dWarpAmplitude * dz;
	}

	// Final noise evaluation at warped coordinates
	Scalar result = pNoise->Evaluate( px, py, pz );

	// Normalize from [-1, 1] to [0, 1]
	result = (result + 1.0) / 2.0;
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
