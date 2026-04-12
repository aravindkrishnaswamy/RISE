//////////////////////////////////////////////////////////////////////
//
//  GGXBRDF.cpp - GGX microfacet BRDF implementation.
//
//  Uses anisotropic GGX NDF (Walter et al. 2007) with Smith
//  height-correlated masking-shadowing G2 (Heitz 2014) and
//  Kulla-Conty multiscattering energy compensation (2017).
//
//  The height-correlated G2 = 1/(1 + Lambda(wi) + Lambda(wo)) is
//  more accurate than the separable G1(wi)*G1(wo) used by
//  CookTorrance, because it accounts for the correlation between
//  masking and shadowing at nearby microsurface heights.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GGXBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../Utilities/math_utils.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/MicrofacetEnergyLUT.h"

using namespace RISE;
using namespace RISE::Implementation;

GGXBRDF::GGXBRDF(
	const IPainter& diffuse,
	const IPainter& specular,
	const IPainter& alphaX,
	const IPainter& alphaY,
	const IPainter& ior,
	const IPainter& ext
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pAlphaX( alphaX ),
  pAlphaY( alphaY ),
  pIOR( ior ),
  pExtinction( ext )
{
	pDiffuse.addref();
	pSpecular.addref();
	pAlphaX.addref();
	pAlphaY.addref();
	pIOR.addref();
	pExtinction.addref();
}

GGXBRDF::~GGXBRDF()
{
	pDiffuse.release();
	pSpecular.release();
	pAlphaX.release();
	pAlphaY.release();
	pIOR.release();
	pExtinction.release();
}

RISEPel GGXBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	const Vector3 n = ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );         // light direction (toward light)
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );    // view direction (toward viewer)

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return RISEPel(0,0,0);
	}

	// Read roughness parameters, clamped to avoid division-by-zero in NDF
	const Scalar alphaX = r_max( ColorMath::MaxValue( pAlphaX.GetColor(ri) ), Scalar(1e-4) );
	const Scalar alphaY = r_max( ColorMath::MaxValue( pAlphaY.GetColor(ri) ), Scalar(1e-4) );

	// Half-vector and tangent-space projections
	const Vector3 h = Vector3Ops::Normalize( v + r );
	const Vector3 h_local(
		Vector3Ops::Dot( h, ri.onb.u() ),
		Vector3Ops::Dot( h, ri.onb.v() ),
		Vector3Ops::Dot( h, ri.onb.w() )
	);
	const Vector3 wi_local(
		Vector3Ops::Dot( v, ri.onb.u() ),
		Vector3Ops::Dot( v, ri.onb.v() ),
		Vector3Ops::Dot( v, ri.onb.w() )
	);
	const Vector3 wo_local(
		Vector3Ops::Dot( r, ri.onb.u() ),
		Vector3Ops::Dot( r, ri.onb.v() ),
		Vector3Ops::Dot( r, ri.onb.w() )
	);

	// Anisotropic GGX NDF
	const Scalar D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alphaX, alphaY, h_local );

	// Height-correlated Smith G2
	const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );

	// Single-scatter specular: D * G2 / (4 * cosWi * cosWo)
	const Scalar specFactor = D * G2 / (4.0 * nv * nr);

	const RISEPel specColor = pSpecular.GetColor(ri);
	const RISEPel ior = pIOR.GetColor(ri);
	const RISEPel ext = pExtinction.GetColor(ri);

	RISEPel specular(0,0,0);

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector), not macrosurface normal
		const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
			ri.ray.Dir(), h, RISEPel(1,1,1), ior, ext );
		specular = specColor * fresnel * specFactor;
	}

	// Kulla-Conty multiscattering energy compensation
	// Use effective alpha = sqrt(alphaX * alphaY) for isotropic LUT lookup
	const Scalar alphaEff = sqrt( alphaX * alphaY );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );

	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alphaEff );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alphaEff );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
		const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
		specular = specular + specColor * F_ms * f_ms;
	}

	return pDiffuse.GetColor(ri) * INV_PI + specular;
}

Scalar GGXBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const Vector3 n = ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( vLightIn );
	const Vector3 r = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nr = Vector3Ops::Dot( n, r );
	const Scalar nv = Vector3Ops::Dot( n, v );

	if( nr < NEARZERO || nv < NEARZERO ) {
		return 0;
	}

	const Scalar alphaX = r_max( pAlphaX.GetColorNM(ri,nm), Scalar(1e-4) );
	const Scalar alphaY = r_max( pAlphaY.GetColorNM(ri,nm), Scalar(1e-4) );

	const Vector3 h = Vector3Ops::Normalize( v + r );
	const Vector3 h_local(
		Vector3Ops::Dot( h, ri.onb.u() ),
		Vector3Ops::Dot( h, ri.onb.v() ),
		Vector3Ops::Dot( h, ri.onb.w() )
	);
	const Vector3 wi_local(
		Vector3Ops::Dot( v, ri.onb.u() ),
		Vector3Ops::Dot( v, ri.onb.v() ),
		Vector3Ops::Dot( v, ri.onb.w() )
	);
	const Vector3 wo_local(
		Vector3Ops::Dot( r, ri.onb.u() ),
		Vector3Ops::Dot( r, ri.onb.v() ),
		Vector3Ops::Dot( r, ri.onb.w() )
	);

	const Scalar D = MicrofacetUtils::GGX_D_Aniso<Scalar>( alphaX, alphaY, h_local );
	const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi_local, wo_local );
	const Scalar specFactor = D * G2 / (4.0 * nv * nr);

	const Scalar specColor = pSpecular.GetColorNM(ri,nm);
	const Scalar iorVal = pIOR.GetColorNM(ri,nm);
	const Scalar extVal = pExtinction.GetColorNM(ri,nm);

	Scalar specular = 0;

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector), not macrosurface normal
		const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), h, 1.0, iorVal, extVal );
		if( fresnel > 0 ) {
			specular = specColor * fresnel * specFactor;
		}
	}

	// Kulla-Conty multiscattering
	const Scalar alphaEff = sqrt( alphaX * alphaY );
	const Scalar Eavg = MicrofacetEnergyLUT::LookupEavg( alphaEff );

	if( (1.0 - Eavg) > 1e-10 )
	{
		const Scalar Ess_o = MicrofacetEnergyLUT::LookupEss( nr, alphaEff );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEss( nv, alphaEff );
		const Scalar f_ms = (1.0 - Ess_o) * (1.0 - Ess_i) / (PI * (1.0 - Eavg));

		const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
		const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
		specular = specular + specColor * F_ms * f_ms;
	}

	return pDiffuse.GetColorNM(ri,nm) * INV_PI + specular;
}
