//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongBRDF.cpp - Implements the phong BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "IsotropicPhongBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

IsotropicPhongBRDF::IsotropicPhongBRDF( const IPainter& rd, const IPainter& rs, const IPainter& exp ) : 
  refdiffuse( rd ),
  refspecular( rs ),
  exponent( exp )
{
	refdiffuse.addref();
	refspecular.addref();
	exponent.addref();
}

IsotropicPhongBRDF::~IsotropicPhongBRDF( )
{
	refdiffuse.release();
	refspecular.release();
	exponent.release();
}

template< class T >
static void ComputeDiffuseSpecularFactors( 
	T& diffuse,
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri,
	const T& exp 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Vector3& n = ri.onb.w();

	Scalar	nr = Vector3Ops::Dot(n,r);
	const Scalar	nv = Vector3Ops::Dot(n,v);

	if( (nr <= -NEARZERO) &&		// viewer is in front	
		(nv <= -NEARZERO) )			// light is in front
	{
		nr = -nr;
		Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
	else if( (nr >= NEARZERO) &&	// viewer is behind
			 (nv >= NEARZERO) )		// light is behind
	{
		Point3 incident = Point3Ops::mkPoint3( Point3( r.x, r.y, r.z ), ((nr*-2.0)*n));
		Scalar sd = Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), -v );
		if( sd > 0 ) {
			specular = pow( sd, exp ) * ((exp+2.0)/TWO_PI);
		}

		diffuse = INV_PI;
	}
}

RISEPel IsotropicPhongBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel diffuseFactor, specularFactor;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri,  exponent.GetColor(ri) );

	return ((refdiffuse.GetColor(ri) * diffuseFactor) + (refspecular.GetColor(ri)*specularFactor));
}

Scalar IsotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar diffuseFactor=0, specularFactor=0;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, exponent.GetColorNM(ri,nm) );

	return ((refdiffuse.GetColorNM(ri,nm) * diffuseFactor) + (refspecular.GetColorNM(ri,nm)*specularFactor));
}
