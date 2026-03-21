//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringBSDF.cpp - Implementation of the SSS BSDF
//  for direct lighting evaluation in BDPT.
//
//  When the surface is hit from outside (front face), we return
//  a diffuse Fresnel approximation using Schlick's R0.
//
//  When the surface is hit from inside (back face, representing
//  a volumetric scatter point), we evaluate the Henyey-Greenstein
//  phase function scaled by the scattering albedo.
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

/// Computes the HG phase function value
static Scalar HGPhase( const Scalar g, const Scalar cosTheta )
{
	const Scalar denom = 1.0 + g*g - 2.0*g*cosTheta;
	return (1.0 - g*g) / (4.0 * PI * denom * sqrt(denom));
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

	const Scalar n_ior = ior.GetColor( ri )[0];

	// Compute scattering albedo (used for interior evaluations)
	const RISEPel sa = absorption.GetColor( ri );
	const RISEPel ss = scattering.GetColor( ri );
	RISEPel albedo;
	for( int c=0; c<3; c++ ) {
		const Scalar st = sa[c] + ss[c];
		albedo[c] = st > 1e-10 ? ss[c] / st : 0.0;
	}

	const bool viewerOutside = (NdotO > 0);
	const bool lightOutside  = (NdotI < 0);

	if( viewerOutside || lightOutside )
	{
		if( alpha > 1e-6 )
		{
			// GGX microfacet evaluation
			const Scalar absNdotO = fabs( NdotO );
			const Scalar absNdotI = fabs( NdotI );

			if( viewerOutside && lightOutside )
			{
				// Both outside: microfacet reflection BRDF
				// f = D(h) * F * G / (4 * |n.wo| * |n.wi|)
				// wo and wi both point away from surface
				Vector3 h = Vector3Ops::Normalize( wo + wi );
				if( Vector3Ops::Dot( h, n ) < 0 ) {
					h = -h;
				}
				const Scalar NdotH = Vector3Ops::Dot( n, h );
				const Scalar OdotH = Vector3Ops::Dot( wo, h );
				if( NdotH <= 0 || OdotH <= 0 ) return RISEPel(0,0,0);

				const Scalar D = GGX_D( NdotH, alpha );
				const Scalar G = GGX_G( absNdotO, absNdotI, alpha );
				const Scalar F = SchlickFresnel( OdotH, 1.0, n_ior );

				const Scalar val = D * F * G / (4.0 * absNdotO * absNdotI);
				return RISEPel( val, val, val );
			}
			// Transmission (one inside, one outside): return 0.
			// The BSDF cannot evaluate transmission for SSS because it
			// has no knowledge of the medium thickness. All SSS transport
			// is handled by the SPF random walk.
		}

		// Smooth surface fallback: Schlick approximation
		const Scalar R0 = ((n_ior - 1.0) / (n_ior + 1.0)) * ((n_ior - 1.0) / (n_ior + 1.0));
		return RISEPel( R0, R0, R0 ) * INV_PI;
	}
	else
	{
		// Both inside: HG phase function evaluation
		const Scalar cosAngle = Vector3Ops::Dot( wi, wo );
		const Scalar phase = HGPhase( g, cosAngle );
		return albedo * phase;
	}
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

	const Scalar n_ior = ior.GetColorNM( ri, nm );

	const Scalar sa = absorption.GetColorNM( ri, nm );
	const Scalar ss = scattering.GetColorNM( ri, nm );
	const Scalar st = sa + ss;
	const Scalar albedoVal = st > 1e-10 ? ss / st : 0.0;

	const bool viewerOutside = (NdotO > 0);
	const bool lightOutside  = (NdotI < 0);

	if( viewerOutside || lightOutside )
	{
		if( alpha > 1e-6 )
		{
			const Scalar absNdotO = fabs( NdotO );
			const Scalar absNdotI = fabs( NdotI );

			if( viewerOutside && lightOutside )
			{
				Vector3 h = Vector3Ops::Normalize( wo + wi );
				if( Vector3Ops::Dot( h, n ) < 0 ) h = -h;
				const Scalar NdotH = Vector3Ops::Dot( n, h );
				const Scalar OdotH = Vector3Ops::Dot( wo, h );
				if( NdotH <= 0 || OdotH <= 0 ) return 0;

				const Scalar D = GGX_D( NdotH, alpha );
				const Scalar G = GGX_G( absNdotO, absNdotI, alpha );
				const Scalar F = SchlickFresnel( OdotH, 1.0, n_ior );
				return D * F * G / (4.0 * absNdotO * absNdotI);
			}
			// No transmission evaluation — handled by SPF random walk
		}

		const Scalar R0 = ((n_ior - 1.0) / (n_ior + 1.0)) * ((n_ior - 1.0) / (n_ior + 1.0));
		return R0 * INV_PI;
	}
	else
	{
		const Scalar cosAngle = Vector3Ops::Dot( wi, wo );
		const Scalar phase = HGPhase( g, cosAngle );
		return albedoVal * phase;
	}
}
