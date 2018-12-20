//////////////////////////////////////////////////////////////////////
//
//  LambertianBRDF.cpp - Implements the lambertian BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LambertianBRDF.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

LambertianBRDF::LambertianBRDF( const IPainter& reflectance ) :
  pReflectance( reflectance )
{
	pReflectance.addref();
}

LambertianBRDF::~LambertianBRDF( )
{
	pReflectance.release();
}

static bool ShouldReflect( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr <= -NEARZERO) &&		// viewer is in front	
		(nv <= -NEARZERO) ) {		// light is in front
		return true;
	} else if( (nr >= NEARZERO) &&	// viewer is behind
				(nv >= NEARZERO) ) {	// light is behind
		return true;
	}
   
	return false;
}

RISEPel LambertianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	if( ShouldReflect( vLightIn, ri, ri.onb.w() ) ) {
		return pReflectance.GetColor(ri) * INV_PI;
	}
   
	return RISEPel(0,0,0);
}

Scalar LambertianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	if( ShouldReflect( vLightIn, ri, ri.onb.w() ) ) {
		return pReflectance.GetColorNM(ri,nm) * INV_PI;
	}
	
	return 0;
}
