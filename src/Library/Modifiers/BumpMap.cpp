//////////////////////////////////////////////////////////////////////
//
//  BumpMap.cpp - Implementation of the BumpMap ray intersection
//  modifier.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BumpMap.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

BumpMap::BumpMap( const IFunction2D& func, const Scalar dScale_, const Scalar dWindow_ ) : 
  pFunction( func ), dScale( dScale_ ), dWindow( dWindow_)
{
	pFunction.addref();
}

BumpMap::~BumpMap()
{
	pFunction.release();
}

void BumpMap::Modify( RayIntersectionGeometric& ri ) const
{
	// The bump value is the returned value multiplied by the scale
	const Scalar bumpU = (pFunction.Evaluate( ri.ptCoord.x + dWindow, ri.ptCoord.y ) * dScale) -
						 (pFunction.Evaluate( ri.ptCoord.x - dWindow, ri.ptCoord.y ) * dScale);

	const Scalar bumpV = (pFunction.Evaluate( ri.ptCoord.x , ri.ptCoord.y + dWindow ) * dScale) -
						 (pFunction.Evaluate( ri.ptCoord.x , ri.ptCoord.y - dWindow ) * dScale);



	// Perturb the normal using the surface tangent vectors from the ONB,
	// rather than static basis vectors which fail when the normal is
	// aligned with one of them (e.g. horizontal surfaces with N=(0,1,0)).
	const Vector3 vTangentU = ri.onb.u();
	const Vector3 vTangentV = ri.onb.v();

	ri.vNormal = Vector3Ops::Normalize(ri.vNormal + vTangentU * bumpU + vTangentV * bumpV);

	// Also rebuild the ONB so that SPFs (refraction/reflection) use
	// the perturbed normal, not the original geometric one.
	ri.onb.CreateFromW( ri.vNormal );
}


