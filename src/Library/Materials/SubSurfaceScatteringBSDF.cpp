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
#include "../Utilities/MicrofacetUtils.h"

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

/// Exact dielectric Fresnel reflectance from cosine and IOR.
/// Matches Optics::CalculateDielectricReflectance used in the SPF.
static Scalar DielectricFresnel( const Scalar cosI, const Scalar eta_i, const Scalar eta_t )
{
	const Scalar sinI2 = 1.0 - cosI * cosI;
	const Scalar sinT2 = (eta_i * eta_i) / (eta_t * eta_t) * sinI2;
	if( sinT2 >= 1.0 ) return 1.0;  // total internal reflection
	const Scalar cosT = sqrt(1.0 - sinT2);

	const Scalar rs = (eta_i * cosI - eta_t * cosT) / (eta_i * cosI + eta_t * cosT);
	const Scalar rp = (eta_t * cosI - eta_i * cosT) / (eta_t * cosI + eta_i * cosT);
	return (rs * rs + rp * rp) * 0.5;
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

		const Scalar D = MicrofacetUtils::GGX_D<Scalar>( alpha, NdotH );
		const Scalar G = MicrofacetUtils::GGX_G( alpha, NdotO, NdotI );
		const Scalar F = DielectricFresnel( OdotH, 1.0, n_ior );

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

		const Scalar D = MicrofacetUtils::GGX_D<Scalar>( alpha, NdotH );
		const Scalar G = MicrofacetUtils::GGX_G( alpha, NdotO, NdotI );
		const Scalar F = DielectricFresnel( OdotH, 1.0, n_ior );
		return D * F * G / (4.0 * NdotO * NdotI);
	}

	// Smooth surface: delta interaction, BSDF = 0 for non-specular directions
	return 0;
}

RISEPel SubSurfaceScatteringBSDF::albedo( const RayIntersectionGeometric& /*ri*/ ) const
{
	// The visible "color" for SSS materials comes from BSSRDF transport,
	// not from this BSDF's surface term (which is GGX reflection only).
	// Closed-form total reflectance under SSS isn't available here, so
	// return white — OIDN treats the surface as illumination-only,
	// which is the correct assumption for skin / wax / marble.
	return RISEPel( 1.0, 1.0, 1.0 );
}
