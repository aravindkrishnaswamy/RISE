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

AshikminShirleyAnisotropicPhongBRDF::AshikminShirleyAnisotropicPhongBRDF( const IScalarPainter& Nu_, const IScalarPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ ) :
  pNu( &Nu_ ), pNv( &Nv_ ), pRd( &Rd_ ), pRs( &Rs_ )
{
	pNu->addref();
	pNv->addref();
	pRd->addref();
	pRs->addref();
}

AshikminShirleyAnisotropicPhongBRDF::~AshikminShirleyAnisotropicPhongBRDF( )
{
	safe_release( pNu );
	safe_release( pNv );
	safe_release( pRd );
	safe_release( pRs );
}

void AshikminShirleyAnisotropicPhongBRDF::SetNu( const IScalarPainter& v ) { v.addref(); safe_release( pNu ); pNu = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetNv( const IScalarPainter& v ) { v.addref(); safe_release( pNv ); pNv = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetRd( const IPainter& v )       { v.addref(); safe_release( pRd ); pRd = &v; }
void AshikminShirleyAnisotropicPhongBRDF::SetRs( const IPainter& v )       { v.addref(); safe_release( pRs ); pRs = &v; }

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
	Vector3 k2 = Vector3Ops::Normalize(-ri.ray.Dir());
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
	// rsCol = Rs painter SAMPLED at this point — distinct from the
	// `pRs` MEMBER (the painter pointer itself).
	RISEPel	rsCol = pRs->GetColor(ri);
	RISEPel	OMRs = RISEPel(1.0,1.0,1.0) - rsCol;

	RISEPel diffuseFactor, specularFactor;
	const ScalarTriple nu = pNu->GetValuesAt(ri);
	const ScalarTriple nv = pNv->GetValuesAt(ri);
	const RISEPel NUp( nu.v[0], nu.v[1], nu.v[2] );
	const RISEPel NVp( nv.v[0], nv.v[1], nv.v[2] );
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, NUp, NVp, rsCol );

	const RISEPel diffuse = (pRd->GetColor(ri) * OMRs * diffuseFactor);
	// specularFactor already contains Fresnel F(h·k) = Rs + (1-Rs)(1-cos)^5,
	// so no extra Rs multiplication is needed (per Ashikmin-Shirley 2000 paper)
	const RISEPel specular = specularFactor;

	return diffuse + specular;
}

Scalar AshikminShirleyAnisotropicPhongBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Scalar	rsCol = pRs->GetColorNM(ri,nm);
	const Scalar	OMRs = 1.0 - rsCol;

	Scalar diffuseFactor, specularFactor;
	ComputeDiffuseSpecularFactors( diffuseFactor, specularFactor, vLightIn, ri, pNu->GetValueAtNM(ri,nm), pNv->GetValueAtNM(ri,nm), rsCol );

	const Scalar diffuse = (pRd->GetColorNM(ri,nm) * OMRs * diffuseFactor);
	// specularFactor already contains Fresnel — no extra Rs multiplication
	const Scalar specular = specularFactor;

	return diffuse + specular;
}

RISEPel AshikminShirleyAnisotropicPhongBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// AS-2000 factors as `Rd·(1-Rs)·diffuse_factor + spec_factor` where
	// the spec lobe carries Fresnel(F0=Rs) and integrates to ≈ Rs.
	// Total: Rd·(1-Rs) + Rs — symmetric in the diffuse/spec coupling.
	const RISEPel rsCol = pRs->GetColor( ri );
	return pRd->GetColor( ri ) * ( RISEPel(1,1,1) - rsCol ) + rsCol;
}
