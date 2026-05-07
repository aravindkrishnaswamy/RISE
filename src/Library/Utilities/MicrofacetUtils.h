//////////////////////////////////////////////////////////////////////
//
//  MicrofacetUtils.h - GGX microfacet distribution and VNDF sampling
//    utilities, shared across CookTorrance, GGX, SubSurfaceScattering,
//    and other microfacet-based materials.
//
//  Isotropic GGX NDF, Smith G1 (separable), and isotropic VNDF
//  sampling are used by CookTorrance.  The anisotropic extensions
//  (GGX_D_Aniso, Lambda, height-correlated G2, VNDF_Sample_Aniso)
//  are used by the dedicated GGX material.
//
//  References:
//    - Walter et al., "Microfacet Models for Refraction through
//      Rough Surfaces", EGSR 2007
//    - Heitz, "Understanding the Masking-Shadowing Function in
//      Microfacet-Based BRDFs", JCGT 3(2), 2014
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
		/// Landing 8: rotate a tangent ONB's (u, v) basis around w by
		/// `angle` radians.  Used by GGX{BRDF,SPF} to apply the
		/// KHR_materials_anisotropy `anisotropyRotation` parameter to
		/// the surface tangent frame before sampling.  Pure rotation
		/// in the surface plane — w (the normal) is unchanged, so
		/// shading-normal-dependent quantities (cos θ_v, cos θ_l)
		/// are invariant.  Only αx vs αy "directions" rotate.
		///
		/// `angle == 0` (or near-zero) is a fast-path no-op so the
		/// pre-L8 path stays bit-identical when the rotation painter
		/// resolves to zero.
		inline OrthonormalBasis3D RotateTangent( const OrthonormalBasis3D& src, Scalar angle )
		{
			if( fabs( angle ) < Scalar( 1e-9 ) ) return src;
			const Scalar c = std::cos( angle );
			const Scalar s = std::sin( angle );
			const Vector3 u = src.u();
			const Vector3 v = src.v();
			return OrthonormalBasis3D(
				u * c + v * s,
				v * c - u * s,
				src.w() );
		}

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

		// ==============================================================
		//  Anisotropic GGX and height-correlated masking-shadowing
		//
		//  These extensions support the dedicated GGX material with
		//  independent roughness in tangent u and v directions and
		//  the more physically accurate height-correlated Smith G2.
		//
		//  Reference: Heitz, JCGT 3(2), 2014, Sections 5.1-5.2
		// ==============================================================

		/// Smith Lambda function for isotropic GGX.
		///   Lambda(v) = (-1 + sqrt(1 + alpha^2 * tan^2(theta))) / 2
		inline Scalar GGX_Lambda( const Scalar alpha, const Scalar cosTheta )
		{
			if( cosTheta >= 1.0 - 1e-10 ) return 0;
			if( cosTheta < 1e-10 ) return 1e10;
			const Scalar cos2 = cosTheta * cosTheta;
			const Scalar tan2 = (1.0 - cos2) / cos2;
			return (-1.0 + sqrt( 1.0 + alpha * alpha * tan2 )) * 0.5;
		}

		/// Smith Lambda for anisotropic GGX (Heitz 2014, Eq. 86).
		/// v_local is the direction in tangent space (x=u, y=v, z=n).
		///   a2 = alpha_x^2 * v_x^2 + alpha_y^2 * v_y^2
		///   Lambda = (-1 + sqrt(1 + a2/v_z^2)) / 2
		inline Scalar GGX_Lambda_Aniso(
			const Scalar alpha_x,
			const Scalar alpha_y,
			const Vector3& v_local
			)
		{
			const Scalar vz2 = v_local.z * v_local.z;
			if( vz2 < 1e-20 ) return 1e10;
			// Clamp roughness to avoid degenerate behavior at zero
			const Scalar ax = (alpha_x < 1e-4) ? 1e-4 : alpha_x;
			const Scalar ay = (alpha_y < 1e-4) ? 1e-4 : alpha_y;
			const Scalar a2 = ax * ax * v_local.x * v_local.x
						    + ay * ay * v_local.y * v_local.y;
			return (-1.0 + sqrt( 1.0 + a2 / vz2 )) * 0.5;
		}

		/// Height-correlated Smith G2 for isotropic GGX.
		///   G2 = 1 / (1 + Lambda(wi) + Lambda(wo))
		/// More accurate than separable G1(wi)*G1(wo) because it
		/// accounts for the correlation between masking and shadowing
		/// at nearby microsurface heights (Heitz 2014, Section 5.2).
		inline Scalar GGX_G2( const Scalar alpha, const Scalar cosWi, const Scalar cosWo )
		{
			return 1.0 / (1.0 + GGX_Lambda( alpha, cosWi ) + GGX_Lambda( alpha, cosWo ));
		}

		/// Height-correlated Smith G2 for anisotropic GGX.
		/// wi_local and wo_local are directions in tangent space.
		inline Scalar GGX_G2_Aniso(
			const Scalar alpha_x,
			const Scalar alpha_y,
			const Vector3& wi_local,
			const Vector3& wo_local
			)
		{
			return 1.0 / (1.0 + GGX_Lambda_Aniso( alpha_x, alpha_y, wi_local )
						     + GGX_Lambda_Aniso( alpha_x, alpha_y, wo_local ));
		}

		/// Smith G1 for anisotropic GGX, computed from Lambda.
		///   G1(v) = 1 / (1 + Lambda(v))
		inline Scalar GGX_G1_Aniso(
			const Scalar alpha_x,
			const Scalar alpha_y,
			const Vector3& v_local
			)
		{
			return 1.0 / (1.0 + GGX_Lambda_Aniso( alpha_x, alpha_y, v_local ));
		}

		/// Anisotropic GGX (Trowbridge-Reitz) NDF.
		/// h_local is the half-vector in tangent space (x=u, y=v, z=n).
		///   D(h) = 1 / (PI * alpha_x * alpha_y * ((hx/ax)^2 + (hy/ay)^2 + hz^2)^2)
		///
		/// Templated on T to support per-channel roughness (RISEPel).
		template< class T >
		inline T GGX_D_Aniso(
			const T& alpha_x,
			const T& alpha_y,
			const Vector3& h_local
			)
		{
			if( h_local.z <= 0 ) return 0.0;
			// Clamp roughness to avoid division-by-zero for perfectly smooth surfaces
			const T ax = (alpha_x < T(1e-4)) ? T(1e-4) : alpha_x;
			const T ay = (alpha_y < T(1e-4)) ? T(1e-4) : alpha_y;
			const T hx_ax = h_local.x / ax;
			const T hy_ay = h_local.y / ay;
			const T denom = hx_ax * hx_ax + hy_ay * hy_ay + Scalar(h_local.z * h_local.z);
			return 1.0 / (PI * ax * ay * denom * denom);
		}

		/// Anisotropic VNDF sampling via the Dupuy-Benyoub spherical
		/// cap method (HPG 2023), extended to independent (alpha_x, alpha_y).
		///
		/// wi must point AWAY from the surface (toward the viewer).
		/// Returns the sampled micronormal in world space.
		inline Vector3 VNDF_Sample_Aniso(
			const Vector3& wi,
			const OrthonormalBasis3D& onb,
			const Scalar alpha_x,
			const Scalar alpha_y,
			const Scalar u1,
			const Scalar u2
			)
		{
			const Scalar alphaEff = sqrt( alpha_x * alpha_y );
			if( alphaEff < 1e-6 ) return onb.w();

			// Project wi into tangent space
			const Scalar wi_x = Vector3Ops::Dot( wi, onb.u() );
			const Scalar wi_y = Vector3Ops::Dot( wi, onb.v() );
			const Scalar wi_z = Vector3Ops::Dot( wi, onb.w() );

			// Stretch wi by anisotropic roughness to hemisphere configuration
			const Vector3 wi_h = Vector3Ops::Normalize(
				Vector3( alpha_x * wi_x, alpha_y * wi_y, wi_z )
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
				Vector3( alpha_x * c.x, alpha_y * c.y, c.z )
			);

			// Transform back to world space
			return Vector3Ops::Normalize(
				onb.u() * m_local.x +
				onb.v() * m_local.y +
				onb.w() * m_local.z
			);
		}

		/// Anisotropic VNDF-based reflection PDF.
		///   pdf = D_v(m) / (4 * |wo.h|)
		/// where D_v = G1_aniso(wi) * dot(wi,m) * D_aniso(m) / dot(wi,n)
		inline Scalar VNDF_Pdf_Aniso(
			const Vector3& wi,
			const Vector3& wo,
			const OrthonormalBasis3D& onb,
			const Scalar alpha_x,
			const Scalar alpha_y
			)
		{
			const Scalar alphaEff = sqrt( alpha_x * alpha_y );
			if( alphaEff < 1e-6 ) return 0;

			const Vector3 n = onb.w();
			const Scalar cosWi = Vector3Ops::Dot( wi, n );
			if( cosWi < 1e-10 ) return 0;

			const Vector3 h = Vector3Ops::Normalize( wi + wo );
			const Scalar wiH = Vector3Ops::Dot( wi, h );
			const Scalar woH = Vector3Ops::Dot( wo, h );
			if( wiH <= 0 || woH < 1e-10 ) return 0;

			// Half-vector and wi in tangent space
			const Vector3 h_local(
				Vector3Ops::Dot( h, onb.u() ),
				Vector3Ops::Dot( h, onb.v() ),
				Vector3Ops::Dot( h, onb.w() )
			);
			const Vector3 wi_local(
				Vector3Ops::Dot( wi, onb.u() ),
				Vector3Ops::Dot( wi, onb.v() ),
				Vector3Ops::Dot( wi, onb.w() )
			);

			if( h_local.z <= 0 ) return 0;

			const Scalar D = GGX_D_Aniso<Scalar>( alpha_x, alpha_y, h_local );
			const Scalar G1wi = GGX_G1_Aniso( alpha_x, alpha_y, wi_local );

			// D_v(m) = G1(wi) * dot(wi,m) * D(m) / dot(wi,n)
			const Scalar Dv = G1wi * wiH * D / cosWi;

			return Dv / (4.0 * woH);
		}
	}
}

#endif
