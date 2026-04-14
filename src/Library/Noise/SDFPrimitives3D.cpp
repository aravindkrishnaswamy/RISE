//////////////////////////////////////////////////////////////////////
//
//  SDFPrimitives3D.cpp - Implements 3D signed distance field
//  primitives.  Evaluates SDF for geometric shapes and converts
//  the distance to a density value suitable for volume rendering.
//
//  Reference: Inigo Quilez, "Distance Functions"
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
#include "SDFPrimitives.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

SDFPrimitive3D::SDFPrimitive3D(
	const SDFPrimitiveType eType_,
	const Scalar dParam1_,
	const Scalar dParam2_,
	const Scalar dParam3_,
	const Scalar dShellThickness_,
	const Scalar dNoiseAmplitude_,
	const Scalar dNoiseFrequency_,
	const RealSimpleInterpolator& interp
) :
  eType( eType_ ),
  dParam1( dParam1_ ),
  dParam2( dParam2_ ),
  dParam3( dParam3_ ),
  dShellThickness( dShellThickness_ ),
  dNoiseAmplitude( dNoiseAmplitude_ ),
  dNoiseFrequency( dNoiseFrequency_ ),
  pNoise( 0 )
{
	if( dNoiseAmplitude > 0.0 ) {
		pNoise = new PerlinNoise3D( interp, 0.5, 4 );
		GlobalLog()->PrintNew( pNoise, __FILE__, __LINE__, "sdf noise" );
	}
}

SDFPrimitive3D::~SDFPrimitive3D()
{
	safe_release( pNoise );
}

Scalar SDFPrimitive3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	Scalar dist;

	switch( eType )
	{
	case eSDF_Sphere:
		{
			// Sphere SDF: |p| - radius
			dist = sqrt( x*x + y*y + z*z ) - dParam1;
		}
		break;

	case eSDF_Box:
		{
			// Box SDF with half-extents (dParam1, dParam2, dParam3)
			Scalar dx = fabs(x) - dParam1;
			Scalar dy = fabs(y) - dParam2;
			Scalar dz = fabs(z) - dParam3;
			Scalar mx = dx > 0.0 ? dx : 0.0;
			Scalar my = dy > 0.0 ? dy : 0.0;
			Scalar mz = dz > 0.0 ? dz : 0.0;
			Scalar outside = sqrt( mx*mx + my*my + mz*mz );
			Scalar dmax = dx;
			if( dy > dmax ) dmax = dy;
			if( dz > dmax ) dmax = dz;
			Scalar inside = dmax < 0.0 ? dmax : 0.0;
			dist = outside + inside;
		}
		break;

	case eSDF_Torus:
		{
			// Torus SDF: major radius = dParam1, minor radius = dParam2
			// Torus lies in XZ plane
			Scalar qx = sqrt( x*x + z*z ) - dParam1;
			dist = sqrt( qx*qx + y*y ) - dParam2;
		}
		break;

	case eSDF_Cylinder:
		{
			// Cylinder SDF: radius = dParam1, half-height = dParam2
			// Cylinder aligned along Y axis
			Scalar dr = sqrt( x*x + z*z ) - dParam1;
			Scalar dy = fabs(y) - dParam2;
			Scalar mr = dr > 0.0 ? dr : 0.0;
			Scalar my = dy > 0.0 ? dy : 0.0;
			Scalar outside = sqrt( mr*mr + my*my );
			Scalar dmax = dr > dy ? dr : dy;
			Scalar inside = dmax < 0.0 ? dmax : 0.0;
			dist = outside + inside;
		}
		break;

	default:
		dist = sqrt( x*x + y*y + z*z ) - dParam1;
		break;
	}

	// Apply noise displacement (pyroclastic effect)
	if( pNoise && dNoiseAmplitude > 0.0 ) {
		Scalar noiseVal = pNoise->Evaluate( x * dNoiseFrequency, y * dNoiseFrequency, z * dNoiseFrequency );
		dist += noiseVal * dNoiseAmplitude;
	}

	// If shell mode, use abs(dist) - thickness/2
	if( dShellThickness > 0.0 ) {
		dist = fabs(dist) - dShellThickness * 0.5;
	}

	// Convert distance to density using smooth exponential falloff.
	// Negative distance = inside = high density.
	// Uses exp(-dist*dist / (2*sigma^2)) Gaussian falloff
	// for a much softer, more natural cloud-like boundary.
	// The falloff width is proportional to the sphere radius (dParam1)
	// so larger shapes have proportionally wider soft edges.
	Scalar sigma = dParam1 > 0.01 ? dParam1 * 0.8 : 0.1;
	Scalar density;
	if( dist < 0.0 ) {
		// Inside: full density, slight falloff toward center for variety
		density = 1.0;
	} else {
		// Outside: smooth Gaussian falloff
		density = exp( -(dist * dist) / (2.0 * sigma * sigma) );
	}

	return density;
}
