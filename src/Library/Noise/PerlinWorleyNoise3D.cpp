//////////////////////////////////////////////////////////////////////
//
//  PerlinWorleyNoise3D.cpp - Implements 3D Perlin-Worley hybrid.
//  Blends Perlin FBM with inverted Worley F1 noise for puffy,
//  cloud-like density patterns.
//
//  Formula: result = lerp( perlinNorm, 1 - worleyF1, blend )
//  where perlinNorm remaps Perlin [-1,1] to [0,1].
//
//  Reference: Schneider & Vos 2015 (SIGGRAPH)
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
#include "PerlinWorleyNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

PerlinWorleyNoise3D::PerlinWorleyNoise3D(
	const RealSimpleInterpolator& interp,
	const Scalar dPersistence,
	const int numOctaves,
	const Scalar dWorleyJitter,
	const Scalar dBlend_
) :
  pPerlin( new PerlinNoise3D( interp, dPersistence, numOctaves < 32 ? numOctaves : 32 ) ),
  pWorley( new WorleyNoise3D( dWorleyJitter, eWorley_Euclidean, eWorley_F1 ) ),
  dBlend( dBlend_ )
{
	GlobalLog()->PrintNew( pPerlin, __FILE__, __LINE__, "perlin noise" );
	GlobalLog()->PrintNew( pWorley, __FILE__, __LINE__, "worley noise" );
}

PerlinWorleyNoise3D::~PerlinWorleyNoise3D()
{
	safe_release( pPerlin );
	safe_release( pWorley );
}

Scalar PerlinWorleyNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	// Perlin FBM returns [-1, 1], normalize to [0, 1]
	Scalar perlinVal = (pPerlin->Evaluate( x, y, z ) + 1.0) / 2.0;
	if( perlinVal < 0.0 ) perlinVal = 0.0;
	if( perlinVal > 1.0 ) perlinVal = 1.0;

	// Worley F1 returns [0, 1] where 0 = at feature point, 1 = far from feature point.
	// Invert it: (1 - F1) gives density = 1 at feature point, 0 far away.
	// This creates puffy, cloud-like blobs.
	Scalar worleyVal = 1.0 - pWorley->Evaluate( x, y, z );

	// Blend: 0 = pure Perlin, 1 = pure inverted Worley
	Scalar result = perlinVal * (1.0 - dBlend) + worleyVal * dBlend;

	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
