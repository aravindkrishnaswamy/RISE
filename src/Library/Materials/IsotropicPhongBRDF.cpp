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

IsotropicPhongBRDF::IsotropicPhongBRDF( const IPainter& rd, const IPainter& rs, const IScalarPainter& exp ) :
  pRd( &rd ),
  pRs( &rs ),
  pExponent( &exp )
{
	pRd->addref();
	pRs->addref();
	pExponent->addref();
}

IsotropicPhongBRDF::~IsotropicPhongBRDF( )
{
	safe_release( pRd );
	safe_release( pRs );
	safe_release( pExponent );
}

void IsotropicPhongBRDF::SetRd( const IPainter& v )
{
	v.addref();
	safe_release( pRd );
	pRd = &v;
}

void IsotropicPhongBRDF::SetRs( const IPainter& v )
{
	v.addref();
	safe_release( pRs );
	pRs = &v;
}

void IsotropicPhongBRDF::SetExponent( const IScalarPainter& v )
{
	v.addref();
	safe_release( pExponent );
	pExponent = &v;
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
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

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
	const ScalarTriple e = pExponent->GetValuesAt(ri);
	const RISEPel exp( e.v[0], e.v[1], e.v[2] );
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, exp );

	return ((pRd->GetColor(ri) * diffuseFactor) + (pRs->GetColor(ri)*specularFactor));
}

Scalar IsotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar diffuseFactor=0, specularFactor=0;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, pExponent->GetValueAtNM(ri,nm) );

	return ((pRd->GetColorNM(ri,nm) * diffuseFactor) + (pRs->GetColorNM(ri,nm)*specularFactor));
}

RISEPel IsotropicPhongBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Conventional Phong reflectance: Rd + Rs (the normalized lobe
	// integrates to ≈ Rs over the hemisphere).
	return pRd->GetColor( ri ) + pRs->GetColor( ri );
}
