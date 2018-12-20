//////////////////////////////////////////////////////////////////////
//
//  OrenNayarBRDF.cpp - Implements the lambertian BRDF
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
#include "OrenNayarBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

OrenNayarBRDF::OrenNayarBRDF( 
	const IPainter& reflectance, 
	const IPainter& roughness
	) :
  pReflectance( reflectance ),
  pRoughness( roughness )
{
	pReflectance.addref();
	pRoughness.addref();
}

OrenNayarBRDF::~OrenNayarBRDF( )
{
	pReflectance.release();
	pRoughness.release();
}

template< class T >
void OrenNayarBRDF::ComputeFactor( 
	T& L1, 
	T& L2, 
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const Vector3& n, 
	const T& roughness 
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.dir); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) &&	(nv >= NEARZERO) ) {
		const T sqr_r = roughness*roughness;
		const Scalar cos_phi_diff = Vector3Ops::Dot(
			Vector3Ops::Normalize(r-(n*nr)),
			Vector3Ops::Normalize(v-(n*nv))
			);

		const Scalar theta_i = acos(nv);
		const Scalar theta_r = acos(nr);

		const Scalar alpha = r_max(theta_i,theta_r);
		const Scalar beta = r_min(theta_i,theta_r);

		const T C1 = 1.0 - 0.5*(sqr_r / (sqr_r + 0.33));
		const T C2 = 0.45 * (sqr_r / (sqr_r + 0.09)) *	(sin(alpha) - ((cos_phi_diff >= 0)? 0 : pow(2.0*beta/PI,3.0)));
		const Scalar t = (4.0*alpha*beta/(PI*PI));
		const T C3 = 0.125 * (sqr_r / (sqr_r + 0.09)) * (t*t);
		L1 = ( C1 + cos_phi_diff * C2 * tan(beta) + (1.0 - fabs(cos_phi_diff)) * C3 * tan((alpha+beta)/2.0) );
		const Scalar u = (2.0*beta)/PI;
		L2 = 0.17 * (sqr_r / (sqr_r + 0.13)) * (1.0 - cos_phi_diff * (u*u));
	}
}

RISEPel OrenNayarBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel L1, L2;
	ComputeFactor<RISEPel>( L1, L2, vLightIn, ri, ri.onb.w(), pRoughness.GetColor(ri) );
	const RISEPel rho = pReflectance.GetColor(ri);

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}

Scalar OrenNayarBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar L1=0, L2=0;
	ComputeFactor<Scalar>( L1, L2, vLightIn, ri, ri.onb.w(), pRoughness.GetColorNM(ri,nm) );
	const Scalar rho = pReflectance.GetColorNM(ri,nm);

	return (L1*INV_PI*rho) + (L2*INV_PI*(rho*rho));
}
