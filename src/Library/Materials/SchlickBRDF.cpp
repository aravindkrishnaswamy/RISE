//////////////////////////////////////////////////////////////////////
//
//  SchlickBRDF.cpp - Implements the lambertian BRDF
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
#include "SchlickBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

SchlickBRDF::SchlickBRDF( 
	const IPainter& diffuse, 
	const IPainter& specular, 
	const IPainter& roughness,
	const IPainter& isotropy
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pRoughness( roughness ),
  pIsotropy( isotropy )
{
	pDiffuse.addref();
	pSpecular.addref();
	pRoughness.addref();
	pIsotropy.addref();
}

SchlickBRDF::~SchlickBRDF( )
{
	pDiffuse.release();
	pSpecular.release();
	pRoughness.release();
	pIsotropy.release();
}

template< class T >
static T ComputeFactor( 
	T& fresnel, 
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const T& r,
	const T& p 
	)
{
	const Vector3 l = Vector3Ops::Normalize(vLightIn); // light vector
	const Vector3 v = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Vector3 n = ri.onb.w();

	const Scalar nv = Vector3Ops::Dot(n,v);
	const Scalar nl = Vector3Ops::Dot(n,l);

	if( (nv >= NEARZERO) &&	(nl >= NEARZERO) ) {
		const Vector3 h = Vector3Ops::Normalize(l+v);
		const Scalar t = Vector3Ops::Dot(n,h);

		const Scalar hl = Vector3Ops::Dot(h,l);

		fresnel = pow<T>(1-hl,5);

		const Scalar w = Vector3Ops::Dot(ri.onb.v(),Vector3Ops::Normalize(h-(t*n)));

		const Scalar sqr_t = t*t;
		const T zdem = (r*sqr_t + 1.0) - sqr_t;
		const T Z = r / (zdem*zdem);

		const T sqr_p = p*p;
		const Scalar sqr_w = w*w;
		const T A = sqrt<T>(p/(sqr_p-sqr_p*sqr_w+sqr_w));

		return (Z*A)/(4.0*PI*nl*nv);	
	}
   
	return 0.0;
}

RISEPel SchlickBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel fresnel;
	const RISEPel factor = ComputeFactor<RISEPel>( fresnel, vLightIn, ri, pRoughness.GetColor(ri), pIsotropy.GetColor(ri) );
	if( ColorMath::MaxValue(factor) > 0 ) {
		const RISEPel rho = pSpecular.GetColor(ri);
		return (pDiffuse.GetColor(ri)*INV_PI) + ((rho + (RISEPel(1.0,1.0,1.0)-rho)*fresnel) * factor);
	}
	
	return RISEPel(0,0,0);  
}

Scalar SchlickBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar fresnel=0;
	const Scalar factor = ComputeFactor<Scalar>( fresnel, vLightIn, ri, pRoughness.GetColorNM(ri,nm), pIsotropy.GetColorNM(ri,nm) );
	if( factor > 0 ) {
		const Scalar rho = pSpecular.GetColorNM(ri,nm);
		return (pDiffuse.GetColorNM(ri,nm)*INV_PI) + (rho + (1.0-rho)*fresnel) * factor;
	}
	
	return 0;
}
