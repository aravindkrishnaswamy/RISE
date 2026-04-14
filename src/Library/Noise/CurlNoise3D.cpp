//////////////////////////////////////////////////////////////////////
//
//  CurlNoise3D.cpp - Implements 3D curl noise.
//  Uses three decorrelated Perlin noise fields as a vector
//  potential, then computes the curl via finite differences.
//  The magnitude of the curl gives a scalar suitable for
//  volume density, producing swirling turbulent structures.
//
//  Curl(F) = ( dFz/dy - dFy/dz,
//              dFx/dz - dFz/dx,
//              dFy/dx - dFx/dy )
//
//  Reference: Bridson et al. 2007 (SIGGRAPH)
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
#include "CurlNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

CurlNoise3D::CurlNoise3D(
	const RealSimpleInterpolator& interp,
	const Scalar dPersistence,
	const int numOctaves,
	const Scalar dEpsilon_
) :
  pNoiseA( new PerlinNoise3D( interp, dPersistence, numOctaves < 32 ? numOctaves : 32 ) ),
  pNoiseB( new PerlinNoise3D( interp, dPersistence, numOctaves < 32 ? numOctaves : 32 ) ),
  pNoiseC( new PerlinNoise3D( interp, dPersistence, numOctaves < 32 ? numOctaves : 32 ) ),
  dEpsilon( dEpsilon_ > 0.0 ? dEpsilon_ : 0.01 )
{
	GlobalLog()->PrintNew( pNoiseA, __FILE__, __LINE__, "curl noise A" );
	GlobalLog()->PrintNew( pNoiseB, __FILE__, __LINE__, "curl noise B" );
	GlobalLog()->PrintNew( pNoiseC, __FILE__, __LINE__, "curl noise C" );
}

CurlNoise3D::~CurlNoise3D()
{
	safe_release( pNoiseA );
	safe_release( pNoiseB );
	safe_release( pNoiseC );
}

Scalar CurlNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	const Scalar e = dEpsilon;
	const Scalar inv2e = 1.0 / (2.0 * e);

	// Use spatial offsets to decorrelate the three noise fields.
	// NoiseA is evaluated at (x, y, z)
	// NoiseB is evaluated at (x + 31.7, y + 47.3, z + 19.1)
	// NoiseC is evaluated at (x + 73.1, y + 11.9, z + 59.7)
	static const Scalar obx = 31.7, oby = 47.3, obz = 19.1;
	static const Scalar ocx = 73.1, ocy = 11.9, ocz = 59.7;

	// Compute partial derivatives via central differences
	// Fx = noiseA, Fy = noiseB, Fz = noiseC

	// dFz/dy
	Scalar dFz_dy = (pNoiseC->Evaluate(x+ocx, y+ocy+e, z+ocz) - pNoiseC->Evaluate(x+ocx, y+ocy-e, z+ocz)) * inv2e;
	// dFy/dz
	Scalar dFy_dz = (pNoiseB->Evaluate(x+obx, y+oby, z+obz+e) - pNoiseB->Evaluate(x+obx, y+oby, z+obz-e)) * inv2e;
	// dFx/dz
	Scalar dFx_dz = (pNoiseA->Evaluate(x, y, z+e) - pNoiseA->Evaluate(x, y, z-e)) * inv2e;
	// dFz/dx
	Scalar dFz_dx = (pNoiseC->Evaluate(x+ocx+e, y+ocy, z+ocz) - pNoiseC->Evaluate(x+ocx-e, y+ocy, z+ocz)) * inv2e;
	// dFy/dx
	Scalar dFy_dx = (pNoiseB->Evaluate(x+obx+e, y+oby, z+obz) - pNoiseB->Evaluate(x+obx-e, y+oby, z+obz)) * inv2e;
	// dFx/dy
	Scalar dFx_dy = (pNoiseA->Evaluate(x, y+e, z) - pNoiseA->Evaluate(x, y-e, z)) * inv2e;

	// Curl = (dFz/dy - dFy/dz, dFx/dz - dFz/dx, dFy/dx - dFx/dy)
	Scalar cx = dFz_dy - dFy_dz;
	Scalar cy = dFx_dz - dFz_dx;
	Scalar cz = dFy_dx - dFx_dy;

	// Magnitude of the curl vector
	Scalar mag = sqrt( cx*cx + cy*cy + cz*cz );

	// Normalize: typical curl magnitudes depend on noise derivatives.
	// For Perlin noise with persistence ~0.65, curl magnitudes are
	// typically in [0, ~3].  We normalize to [0, 1].
	Scalar result = mag / 3.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
