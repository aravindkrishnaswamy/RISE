//////////////////////////////////////////////////////////////////////
//
//  MicrofacetUtils.h - GGX microfacet distribution and VNDF sampling
//    utilities, shared across CookTorrance, SubSurfaceScattering,
//    and other microfacet-based materials.
//
//  References:
//    - Walter et al., "Microfacet Models for Refraction through
//      Rough Surfaces", EGSR 2007
//    - Heitz, "Understanding the Masking-Shadowing Function in
//      Microfacet-Based BRDFs", JCGT 2014
//    - Dupuy & Benyoub, "Sampling Visible GGX Normals with
//      Spherical Caps", HPG 2023
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MICROFACET_UTILS_
#define MICROFACET_UTILS_

#include "Math3D/Math3D.h"
#include "OrthonormalBasis3D.h"
#include "math_utils.h"

namespace RISE
{
	namespace MicrofacetUtils
	{
		/// GGX (Trowbridge-Reitz) normal distribution function.
		/// Templated on T to support per-channel roughness (RISEPel).
		///   D(h) = alpha^2 / (PI * (cos^2(theta_h) * (alpha^2 - 1) + 1)^2)
		template< class T >
		inline T GGX_D( const T& alpha, const Scalar cosThetaH )
		{
			if( cosThetaH <= 0 ) return 0.0;
			const T a2 = alpha * alpha;
			const Scalar cos2 = cosThetaH * cosThetaH;
			const T denom = cos2 * (a2 - 1.0) + 1.0;
			return a2 / (PI * denom * denom);
		}

		/// Smith G1 masking function for GGX (scalar only).
		///   G1(v) = 2*cos(theta) / (cos(theta) + sqrt(alpha^2 + (1-alpha^2)*cos^2(theta)))
		inline Scalar GGX_G1( const Scalar alpha, const Scalar cosTheta )
		{
			if( cosTheta < 1e-10 ) return 0;
			const Scalar a2 = alpha * alpha;
			const Scalar cos2 = cosTheta * cosTheta;
			return 2.0 * cosTheta / (cosTheta + sqrt(a2 + (1.0 - a2) * cos2));
		}

		/// Smith separable masking-shadowing: G = G1(wi) * G1(wo)
		inline Scalar GGX_G( const Scalar alpha, const Scalar cosWi, const Scalar cosWo )
		{
			return GGX_G1( alpha, cosWi ) * GGX_G1( alpha, cosWo );
		}

		/// Sample a microfacet normal from the VNDF (Visible Normal
		/// Distribution Function) using the Dupuy-Benyoub spherical
		/// cap method (HPG 2023).
		///
		/// wi must point AWAY from the surface (toward the viewer).
		/// Returns the sampled micronormal in world space.
		inline Vector3 VNDF_Sample(
			const Vector3& wi,
			const OrthonormalBasis3D& onb,
			const Scalar alpha,
			const Scalar u1,
			const Scalar u2
			)
		{
			// Near-mirror: return geometric normal
			if( alpha < 1e-6 ) return onb.w();

			// Project wi into tangent space
			const Scalar wi_x = Vector3Ops::Dot( wi, onb.u() );
			const Scalar wi_y = Vector3Ops::Dot( wi, onb.v() );
			const Scalar wi_z = Vector3Ops::Dot( wi, onb.w() );

			// Stretch wi by roughness to hemisphere configuration
			const Vector3 wi_h = Vector3Ops::Normalize(
				Vector3( alpha * wi_x, alpha * wi_y, wi_z )
			);

			// Sample a spherical cap around wi_h
			const Scalar phi = TWO_PI * u1;
			const Scalar z = (1.0 - u2) * (1.0 + wi_h.z) - wi_h.z;
			const Scalar sinTheta = sqrt( r_max( 0.0, 1.0 - z * z ) );
			const Scalar x = sinTheta * cos( phi );
			const Scalar y = sinTheta * sin( phi );

			// Compute the micronormal in hemisphere configuration
			const Vector3 c( x + wi_h.x, y + wi_h.y, z + wi_h.z );

			// Unstretch back to ellipsoidal configuration
			const Vector3 m_local = Vector3Ops::Normalize(
				Vector3( alpha * c.x, alpha * c.y, c.z )
			);

			// Transform back to world space
			return Vector3Ops::Normalize(
				onb.u() * m_local.x +
				onb.v() * m_local.y +
				onb.w() * m_local.z
			);
		}

		/// Compute the VNDF-based reflection PDF for a given
		/// incoming direction wi and outgoing direction wo.
		///   pdf = D_v(m) / (4 * |wo.h|)
		/// where D_v(m) = G1(wi) * max(dot(wi,m),0) * D(m) / dot(wi,n)
		inline Scalar VNDF_Pdf(
			const Vector3& wi,
			const Vector3& wo,
			const Vector3& n,
			const Scalar alpha
			)
		{
			// Near-mirror: PDF is 0 for non-delta sampling
			if( alpha < 1e-6 ) return 0;

			const Scalar cosWi = Vector3Ops::Dot( wi, n );
			if( cosWi < 1e-10 ) return 0;

			const Vector3 h = Vector3Ops::Normalize( wi + wo );
			const Scalar cosThetaH = Vector3Ops::Dot( h, n );
			const Scalar wiH = Vector3Ops::Dot( wi, h );
			const Scalar woH = Vector3Ops::Dot( wo, h );

			if( cosThetaH <= 0 || wiH <= 0 || woH < 1e-10 ) return 0;

			const Scalar D = GGX_D<Scalar>( alpha, cosThetaH );
			const Scalar G1wi = GGX_G1( alpha, cosWi );

			// D_v(m) = G1(wi) * dot(wi,m) * D(m) / dot(wi,n)
			const Scalar Dv = G1wi * wiH * D / cosWi;

			// Reflection PDF: D_v(m) / (4 * |wo.h|)
			return Dv / (4.0 * woH);
		}
	}
}

#endif
