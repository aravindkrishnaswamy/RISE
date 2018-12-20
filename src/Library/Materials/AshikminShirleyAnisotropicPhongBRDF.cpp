//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongBRDF.cpp - Implementation of the class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AshikminShirleyAnisotropicPhongBRDF.h"
#include "../Utilities/Optics.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

AshikminShirleyAnisotropicPhongBRDF::AshikminShirleyAnisotropicPhongBRDF( const IPainter& Nu_, const IPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ ) : 
  Nu( Nu_ ), Nv( Nv_ ), Rd( Rd_ ), Rs( Rs_ )
{
	Nu.addref();
	Nv.addref();
	Rd.addref();
	Rs.addref();
}

AshikminShirleyAnisotropicPhongBRDF::~AshikminShirleyAnisotropicPhongBRDF( )
{
	Nu.release();
	Nv.release();
	Rd.release();
	Rs.release();
}

template< class T >
void AshikminShirleyAnisotropicPhongBRDF::ComputeDiffuseSpecularFactors( 
	T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri,
	const T& NU, 
	const T& NV, 
	const T& Rs 
	)
{
	diffuse = specular = 0;

	Vector3 k1 = Vector3Ops::Normalize(vLightIn);
	Vector3 k2 = Vector3Ops::Normalize(-ri.ray.dir);
	Vector3 h = Vector3Ops::Normalize(k1+k2);

	const Vector3& k = k2;
	const Vector3& n = ri.onb.w();
	const Vector3& u = ri.onb.u();
	const Vector3& v = ri.onb.v();

	const Scalar hdotk = Vector3Ops::Dot(h,k);
	Scalar ndotk1 = Vector3Ops::Dot(n,k1);
	Scalar ndotk2 = Vector3Ops::Dot(n,k2);

	if( ndotk2 < NEARZERO ) {
		return;
	}

	if( ndotk1 < NEARZERO ) {
		ndotk1 = 0;
	}

	Scalar	fromK1 = 1.0 - pow(1.0 - ((ndotk1)*0.5), 5.0);
	Scalar	fromK2 = 1.0 - pow(1.0 - ((ndotk2)*0.5), 5.0);

	static const Scalar energyConservation = 28.0 / (23.0 * PI);

	diffuse = energyConservation * fromK1 * fromK2;

	// Compute specular
	const T NuNvFactor = sqrt((NU+1.0)*(NV+1.0));
	const T rhoSconst = NuNvFactor / (8.0 * PI);
	const T fresnel = Optics::CalculateFresnelReflectanceSchlick( Rs, hdotk );
	
	const Scalar hn = Vector3Ops::Dot(h,n);
	const Scalar hu = Vector3Ops::Dot(h,u);
	const Scalar hv = Vector3Ops::Dot(h,v);
	const T exponent = (( NU*hu*hu ) + ( NV*hv*hv )) / (1 - (hn*hn));
	
	const T num = pow( hn, exponent );
	const Scalar den = (hdotk) * r_max( ndotk1, ndotk2 );

	specular = rhoSconst*(num/den)*fresnel;
}

RISEPel AshikminShirleyAnisotropicPhongBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel	pRs = Rs.GetColor(ri);
	RISEPel	OMRs = RISEPel(1.0,1.0,1.0) - pRs;

	RISEPel diffuseFactor, specularFactor;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, Nu.GetColor(ri), Nv.GetColor(ri), pRs );

	const RISEPel diffuse = (Rd.GetColor(ri) * OMRs * diffuseFactor);
	const RISEPel specular = specularFactor * pRs;

	return diffuse + specular;
}

Scalar AshikminShirleyAnisotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar	pRs = Rs.GetColorNM(ri,nm);
	const Scalar	OMRs = 1.0 - pRs;

	Scalar diffuseFactor, specularFactor;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, Nu.GetColorNM(ri,nm), Nv.GetColorNM(ri,nm), pRs );

	const Scalar diffuse = (Rd.GetColorNM(ri,nm) * OMRs * diffuseFactor);
	const Scalar specular = specularFactor * pRs;

	return diffuse + specular;
}
