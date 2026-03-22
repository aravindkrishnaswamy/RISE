//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringBSDF.cpp - Implementation of the SSS BSDF
//  for direct lighting evaluation in BDPT.
//
//  With the BSSRDF approach, only surface reflection is evaluated:
//
//  Both outside (front/front): GGX microfacet BRDF for rough
//    surfaces, diffuse Fresnel approximation for smooth surfaces.
//  All other cases: return 0.  Subsurface transport is handled
//    by the diffusion profile, not the BSDF connection.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubSurfaceScatteringBSDF.h"

using namespace RISE;
using namespace RISE::Implementation;

SubSurfaceScatteringBSDF::SubSurfaceScatteringBSDF(
	const IPainter& ior_,
	const IPainter& absorption_,
	const IPainter& scattering_,
	const Scalar g_,
	const Scalar roughness_
	) :
  ior( ior_ ),
  absorption( absorption_ ),
  scattering( scattering_ ),
  g( g_ ),
  roughness( roughness_ ),
  alpha( roughness_ * roughness_ )
{
	ior.addref();
	absorption.addref();
	scattering.addref();
}

SubSurfaceScatteringBSDF::~SubSurfaceScatteringBSDF()
{
	ior.release();
	absorption.release();
	scattering.release();
}

/// GGX normal distribution function
static Scalar GGX_D( const Scalar NdotH, const Scalar alpha )
{
	const Scalar a2 = alpha * alpha;
	const Scalar cos2 = NdotH * NdotH;
	const Scalar denom = cos2 * (a2 - 1.0) + 1.0;
	return a2 / (PI * denom * denom);
}

/// Smith G1 for GGX
static Scalar GGX_G1( const Scalar NdotV, const Scalar alpha )
{
	const Scalar a2 = alpha * alpha;
	const Scalar cos2 = NdotV * NdotV;
	return 2.0 * NdotV / (NdotV + sqrt(a2 + (1.0 - a2) * cos2));
}

/// Smith G masking-shadowing for GGX
static Scalar GGX_G( const Scalar NdotV, const Scalar NdotL, const Scalar alpha )
{
	return GGX_G1( NdotV, alpha ) * GGX_G1( NdotL, alpha );
}

/// Schlick Fresnel from cosine and IOR
static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta_i, const Scalar eta_t )
{
	const Scalar R0 = ((eta_i - eta_t) / (eta_i + eta_t)) * ((eta_i - eta_t) / (eta_i + eta_t));
	const Scalar c = 1.0 - cosTheta;
	const Scalar c2 = c * c;
	return R0 + (1.0 - R0) * c2 * c2 * c;
}

RISEPel SubSurfaceScatteringBSDF::value(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri
	) const
{
	const Vector3 n = ri.onb.w();
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wi = Vector3Ops::Normalize( vLightIn );
	const Scalar NdotO = Vector3Ops::Dot( n, wo );
	const Scalar NdotI = Vector3Ops::Dot( n, wi );

	// Only evaluate when both directions are on the outside (front face).
	// All other cases return 0 — the subsurface transport is handled
	// by the diffusion profile in the integrator, not by BSDF connections.
	if( NdotO <= 0 || NdotI <= 0 )
		return RISEPel( 0, 0, 0 );

	const Scalar n_ior = ior.GetColor( ri )[0];

	if( alpha > 1e-6 )
	{
		// Rough surface: GGX microfacet BRDF (Walter et al. 2007)
		Vector3 h = Vector3Ops::Normalize( wo + wi );
		if( Vector3Ops::Dot( h, n ) < 0 ) {
			h = -h;
		}
		const Scalar NdotH = Vector3Ops::Dot( n, h );
		const Scalar OdotH = Vector3Ops::Dot( wo, h );
		if( NdotH <= 0 || OdotH <= 0 ) return RISEPel(0,0,0);

		const Scalar D = GGX_D( NdotH, alpha );
		const Scalar G = GGX_G( NdotO, NdotI, alpha );
		const Scalar F = SchlickFresnel( OdotH, 1.0, n_ior );

		const Scalar val = D * F * G / (4.0 * NdotO * NdotI);
		return RISEPel( val, val, val );
	}

	// Smooth surface: perfect specular (delta) reflection only.
	// The delta interaction is handled by the SPF's kray; the BSDF
	// must return 0 for all non-specular direction pairs to stay
	// consistent with the SPF's Pdf() which returns 0 (delta).
	return RISEPel( 0, 0, 0 );
}

Scalar SubSurfaceScatteringBSDF::valueNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	const Vector3 n = ri.onb.w();
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wi = Vector3Ops::Normalize( vLightIn );
	const Scalar NdotO = Vector3Ops::Dot( n, wo );
	const Scalar NdotI = Vector3Ops::Dot( n, wi );

	if( NdotO <= 0 || NdotI <= 0 )
		return 0;

	const Scalar n_ior = ior.GetColorNM( ri, nm );

	if( alpha > 1e-6 )
	{
		Vector3 h = Vector3Ops::Normalize( wo + wi );
		if( Vector3Ops::Dot( h, n ) < 0 ) h = -h;
		const Scalar NdotH = Vector3Ops::Dot( n, h );
		const Scalar OdotH = Vector3Ops::Dot( wo, h );
		if( NdotH <= 0 || OdotH <= 0 ) return 0;

		const Scalar D = GGX_D( NdotH, alpha );
		const Scalar G = GGX_G( NdotO, NdotI, alpha );
		const Scalar F = SchlickFresnel( OdotH, 1.0, n_ior );
		return D * F * G / (4.0 * NdotO * NdotI);
	}

	// Smooth surface: delta interaction, BSDF = 0 for non-specular directions
	return 0;
}
