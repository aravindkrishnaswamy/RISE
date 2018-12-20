//////////////////////////////////////////////////////////////////////
//
//  CookTorranceBRDF.cpp - Implements the lambertian BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CookTorranceBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

CookTorranceBRDF::CookTorranceBRDF( 
	const IPainter& diffuse, 
	const IPainter& specular, 
	const IPainter& masking,
	const IPainter& ior,
	const IPainter& ext
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pMasking( masking ),
  pIOR( ior ),
  pExtinction( ext )
{
	pDiffuse.addref();
	pSpecular.addref();
	pMasking.addref();
	pIOR.addref();
	pExtinction.addref();
}

CookTorranceBRDF::~CookTorranceBRDF( )
{
	pDiffuse.release();
	pSpecular.release();
	pMasking.release();
	pIOR.release();
	pExtinction.release();
}

template< class T >
inline T ComputeFacetDistribution( const Scalar alpha, const T& m )
{
	const T left = 1.0 / (m*m * pow<T>(cos(alpha),4));
	const T inside = tan(alpha)/m;
	const T right = exp<T>(-1.0*(inside*inside));
	return left * right;
}

template< class T >
T CookTorranceBRDF::ComputeFactor( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& m )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	Scalar nr = Vector3Ops::Dot(n,r);
	Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) &&	(nv >= NEARZERO) ) {
		const Vector3 h = Vector3Ops::Normalize(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

		const T facet = ComputeFacetDistribution<T>( acos(hn), m );

		// Compute the geometric masking term
		const Scalar vh = Vector3Ops::Dot(v,h);
		const Scalar first = (2.0 * hn * nv) / vh;
		const Scalar second = (2.0 * hn * nr) / vh;

		const Scalar masking = r_min( 1.0, r_min( first, second ) );

		return (facet*masking) / (nr*nv);
	}
   
	return 0.0;
}

RISEPel CookTorranceBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const RISEPel factor = ComputeFactor<RISEPel>( vLightIn, ri, ri.onb.w(), pMasking.GetColor(ri) );
	if( ColorMath::MinValue(factor) > 0 ) {
		const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>( ri.ray.dir, ri.onb.w(), RISEPel(1,1,1), pIOR.GetColor(ri), pExtinction.GetColor(ri) );
		return pDiffuse.GetColor(ri)*INV_PI + (pSpecular.GetColor(ri) * INV_PI * fresnel * factor);
	}
	
	return pDiffuse.GetColor(ri)*INV_PI;
}

Scalar CookTorranceBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar factor = ComputeFactor<Scalar>( vLightIn, ri, ri.onb.w(), pMasking.GetColorNM(ri,nm) );
	if( factor > 0 ) {
		const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.dir, ri.onb.w(), 1.0, pIOR.GetColorNM(ri,nm), pExtinction.GetColorNM(ri,nm) );
		if( fresnel > 0 ) {
			return pDiffuse.GetColorNM(ri,nm)*INV_PI + (pSpecular.GetColorNM(ri,nm) * INV_PI * fresnel * factor);
		}
	}
	
	return pDiffuse.GetColorNM(ri,nm)*INV_PI;
}
