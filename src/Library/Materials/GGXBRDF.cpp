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
	const IPainter& ext,
	const FresnelMode fresnel_mode
	) :
  pDiffuse( diffuse ),
  pSpecular( specular ),
  pAlphaX( alphaX ),
  pAlphaY( alphaY ),
  pIOR( ior ),
  pExtinction( ext ),
  fresnelMode( fresnel_mode )
{
	pDiffuse.addref();
	pSpecular.addref();
	pAlphaX.addref();
	pAlphaY.addref();
	pIOR.addref();
	pExtinction.addref();
}

namespace
{
	// Hemispherical Fresnel average for Schlick's approximation:
	//   F_avg = 2 ∫₀¹ [F0 + (1-F0)(1-μ)^5] μ dμ = F0 + (1-F0)/21
	// Closed-form, no quadrature needed.  Per-channel for RISEPel.
	template< class T >
	inline T SchlickFresnelAvg( const T& F0 )
	{
		return F0 + (T(1.0) - F0) * (1.0 / 21.0);
	}
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

	RISEPel specular(0,0,0);

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector), not macrosurface normal
		if( fresnelMode == eFresnelSchlickF0 )
		{
			// Schlick: F = F0 + (1-F0)*(1-cosθ_h)^5, where specColor is F0.
			// cosθ_h = max(0, dot(wo, h)).
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const RISEPel F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( specColor, cosWoH );
			specular = F * specFactor;
		}
		else
		{
			const RISEPel ior = pIOR.GetColor(ri);
			const RISEPel ext = pExtinction.GetColor(ri);
			const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
				ri.ray.Dir(), h, RISEPel(1,1,1), ior, ext );
			specular = specColor * fresnel * specFactor;
		}
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

		if( fresnelMode == eFresnelSchlickF0 )
		{
			// Closed-form Schlick hemispherical average.  specColor is F0.
			const RISEPel F_avg = SchlickFresnelAvg<RISEPel>( specColor );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			const RISEPel ior = pIOR.GetColor(ri);
			const RISEPel ext = pExtinction.GetColor(ri);
			const RISEPel F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<RISEPel>( n, RISEPel(1,1,1), ior, ext );
			const RISEPel F_ms = MicrofacetEnergyLUT::ComputeFms<RISEPel>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	// Diffuse lobe.  In Schlick mode, modulate by (1 - max(F0)) per glTF spec
	// to enforce the (1-F) energy split between diffuse and specular.
	RISEPel diffuse = pDiffuse.GetColor(ri) * INV_PI;
	if( fresnelMode == eFresnelSchlickF0 )
	{
		const Scalar maxF0 = ColorMath::MaxValue( specColor );
		diffuse = diffuse * r_max( Scalar(0), Scalar(1.0) - maxF0 );
	}

	return diffuse + specular;
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

	Scalar specular = 0;

	if( specFactor > 0 )
	{
		// Fresnel evaluated at microfacet normal (half-vector)
		if( fresnelMode == eFresnelSchlickF0 )
		{
			const Scalar cosWoH = r_max( Scalar(0), Vector3Ops::Dot( r, h ) );
			const Scalar F = Optics::CalculateFresnelReflectanceSchlick<Scalar>( specColor, cosWoH );
			if( F > 0 ) {
				specular = F * specFactor;
			}
		}
		else
		{
			const Scalar iorVal = pIOR.GetColorNM(ri,nm);
			const Scalar extVal = pExtinction.GetColorNM(ri,nm);
			const Scalar fresnel = Optics::CalculateConductorReflectance( ri.ray.Dir(), h, 1.0, iorVal, extVal );
			if( fresnel > 0 ) {
				specular = specColor * fresnel * specFactor;
			}
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

		if( fresnelMode == eFresnelSchlickF0 )
		{
			const Scalar F_avg = SchlickFresnelAvg<Scalar>( specColor );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + F_ms * f_ms;
		}
		else
		{
			const Scalar iorVal = pIOR.GetColorNM(ri,nm);
			const Scalar extVal = pExtinction.GetColorNM(ri,nm);
			const Scalar F_avg = MicrofacetEnergyLUT::ComputeFresnelAvg<Scalar>( n, 1.0, iorVal, extVal );
			const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F_avg, Eavg );
			specular = specular + specColor * F_ms * f_ms;
		}
	}

	Scalar diffuse = pDiffuse.GetColorNM(ri,nm) * INV_PI;
	if( fresnelMode == eFresnelSchlickF0 )
	{
		// In NM, F0 is scalar; same (1-F0) split applies.
		diffuse = diffuse * r_max( Scalar(0), Scalar(1.0) - specColor );
	}

	return diffuse + specular;
}

RISEPel GGXBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Diffuse + Fresnel(cos θo) · spec, matching whichever Fresnel model
	// `value()` uses.  D / G microfacet terms shape the lobe but don't
	// change total integrated reflectance to first order.
	//
	// The Schlick branch evaluates Fresnel at the actual outgoing-cosine,
	// matching the conductor branch's intent.  Diffuse gets the
	// (1 - max(F0)) glTF-spec split.  Earlier revisions used the
	// hemispherical Schlick average here; review found it
	// overestimates near-normal and underestimates at grazing.
	const Vector3 n = ri.onb.w();
	const RISEPel specColor = pSpecular.GetColor( ri );
	const RISEPel diffColor = pDiffuse.GetColor( ri );

	if( fresnelMode == eFresnelSchlickF0 )
	{
		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar cosThetaO = r_max( Scalar(0), Vector3Ops::Dot( wo, n ) );
		const RISEPel F = Optics::CalculateFresnelReflectanceSchlick<RISEPel>( specColor, cosThetaO );
		const Scalar maxF0 = ColorMath::MaxValue( specColor );
		return diffColor * r_max( Scalar(0), Scalar(1.0) - maxF0 ) + F;
	}
	else
	{
		const RISEPel ior = pIOR.GetColor( ri );
		const RISEPel ext = pExtinction.GetColor( ri );
		const RISEPel fresnel = Optics::CalculateConductorReflectance<RISEPel>(
			ri.ray.Dir(), n, RISEPel( 1, 1, 1 ), ior, ext );
		return diffColor + specColor * fresnel;
	}
}
