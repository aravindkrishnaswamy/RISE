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
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"

using namespace RISE;
using namespace RISE::Implementation;

static inline Scalar ToScalarAlpha( const Scalar a ) { return a; }
static inline Scalar ToScalarAlpha( const RISEPel& a ) { return a[0]; }

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
T CookTorranceBRDF::ComputeFactor( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& alpha )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	Scalar nr = Vector3Ops::Dot(n,r);
	Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) && (nv >= NEARZERO) ) {
		const Vector3 h = Vector3Ops::Normalize(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

		// GGX NDF (templated for per-channel roughness)
		const T D = MicrofacetUtils::GGX_D<T>( alpha, hn );

		// Smith masking-shadowing (scalar, geometry-only)
		const Scalar scalarAlpha = ToScalarAlpha( alpha );
		const Scalar G = MicrofacetUtils::GGX_G( scalarAlpha, nv, nr );

		return D * G / (4.0 * nr * nv);
	}

	return 0.0;
}

RISEPel CookTorranceBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const Vector3 n = ri.onb.w();
	const RISEPel alphaColor = pMasking.GetColor(ri);
	const Scalar scalarAlpha = ColorMath::MaxValue( alphaColor );

	const RISEPel factor = ComputeFactor<RISEPel>( vLightIn, ri, n, alphaColor );

	const RISEPel specColor = pSpecular.GetColor(ri);
	const RISEPel ior = pIOR.GetColor(ri);
	const RISEPel ext = pExtinction.GetColor(ri);

	RISEPel specular(0,0,0);

	if( ColorMath::MinValue(factor) > 0 ) {
		const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>( ri.ray.Dir(), n, RISEPel(1,1,1), ior, ext );
		specular = specColor * fresnel * factor;
	}

	// Kulla-Conty multiscattering energy compensation
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( scalarAlpha );
	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar cosWo = Vector3Ops::Dot( n, Vector3Ops::Normalize(-ri.ray.Dir()) );
		const Scalar cosWi = Vector3Ops::Dot( n, Vector3Ops::Normalize(vLightIn) );
		if( cosWo > 0 && cosWi > 0 )
		{
			const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosWo, scalarAlpha );
			const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, scalarAlpha );
			const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

			const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	return pDiffuse.GetColor(ri)*INV_PI + specular;
}

Scalar CookTorranceBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Vector3 n = ri.onb.w();
	const Scalar alpha = pMasking.GetColorNM(ri,nm);
	const Scalar specColor = pSpecular.GetColorNM(ri,nm);
	const Scalar iorVal = pIOR.GetColorNM(ri,nm);
	const Scalar extVal = pExtinction.GetColorNM(ri,nm);

	Scalar specular = 0;

	const Scalar factor = ComputeFactor<Scalar>( vLightIn, ri, n, alpha );
	if( factor > 0 ) {
		const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), n, 1.0, iorVal, extVal );
		if( fresnel > 0 ) {
			specular = specColor * fresnel * factor;
		}
	}

	// Kulla-Conty multiscattering energy compensation
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alpha );
	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar cosWo = Vector3Ops::Dot( n, Vector3Ops::Normalize(-ri.ray.Dir()) );
		const Scalar cosWi = Vector3Ops::Dot( n, Vector3Ops::Normalize(vLightIn) );
		if( cosWo > 0 && cosWi > 0 )
		{
			const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( cosWo, alpha );
			const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );
			const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

			const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	return pDiffuse.GetColorNM(ri,nm)*INV_PI + specular;
}
