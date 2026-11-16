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

#include <cmath>
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

		/// Tier 1.1 transport kernel, in a local frame whose +z faces the
		/// incident medium. Both directions must be unit length and point
		/// away from the interface; wi.z > 0. eta = n_transmitted/n_incident.
		/// Alpha values are GGX widths in (0,1], not perceptual roughness. Both zero
		/// means smooth; a single zero width (a singular 1D distribution) is
		/// unsupported. This is SINGLE scattering: rejected micronormals are
		/// null events, not resampled. Multiple scattering and material/IOR
		/// stack integration are separate work; existing materials do not use
		/// this kernel yet. Shared scalar math is suitable for Pel/NM/HWSS.
		namespace RoughDielectric
		{
			struct Evaluation
			{
				Scalar f = 0;       ///< BSDF in sr^-1, including radiance scaling if requested.
				Scalar pdf = 0;     ///< Aggregate reflection/transmission solid-angle PDF.
				bool positiveDensity = false; ///< Retains positivity if conversion to Scalar underflows.
			};

			struct Sample
			{
				Vector3 wo = Vector3( 0, 0, 0 );
				Scalar f = 0;
				Scalar pdf = 0;     ///< Probability mass for a delta event, density otherwise.
				Scalar weight = 0;  ///< f * abs(wo.z) / pdf, without a second Fresnel coin.
				// Values/densities or true eta^-2 weights can exceed Scalar's
				// range. Representable throughput is evaluated independently;
				// positiveDensity keeps underflowing densities from becoming nulls.
				bool transmission = false;
				bool delta = false;
				bool valid = false;
			};

			inline bool ValidParameters( Scalar ax, Scalar ay, Scalar eta )
			{
				return std::isfinite( ax ) && std::isfinite( ay ) && std::isfinite( eta ) &&
					eta > 0 && ( ( ax > 0 && ax <= 1 && ay > 0 && ay <= 1 ) || ( ax == 0 && ay == 0 ) );
			}

			// Keep positive products as mantissa/exponent pairs until their
			// final conversion. Individual NDF factors can overflow even when
			// the final BSDF/PDF is representable. No wider arithmetic or clamps.
			struct PositiveProduct
			{
				Scalar mantissa = 1;
				int exponent = 0;
				void Multiply( Scalar value )
				{
					int e = 0, adjust = 0;
					mantissa *= std::frexp( value, &e );
					mantissa = std::frexp( mantissa, &adjust );
					exponent += e + adjust;
				}
				void Divide( Scalar value )
				{
					int e = 0, adjust = 0;
					mantissa /= std::frexp( value, &e );
					mantissa = std::frexp( mantissa, &adjust );
					exponent += adjust - e;
				}
				Scalar Value() const { return std::scalbn( mantissa, exponent ); }
			};

			inline Vector3 NormalizeScaled( const Vector3& v, const Vector3& scales )
			{
				const Scalar values[3] = { v.x, v.y, v.z };
				const Scalar factors[3] = { scales.x, scales.y, scales.z };
				Scalar parts[3] = { 0, 0, 0 };
				int powers[3] = { 0, 0, 0 }, maxPower = 0;
				bool any = false;
				for( int i = 0; i < 3; ++i ) {
					if( values[i] == 0 ) continue;
					int ev = 0, ef = 0;
					parts[i] = std::frexp( values[i], &ev ) * std::frexp( factors[i], &ef );
					powers[i] = ev + ef;
					maxPower = any ? r_max( maxPower, powers[i] ) : powers[i];
					any = true;
				}
				if( !any ) return Vector3( 0, 0, 0 );
				Scalar scaled[3];
				for( int i = 0; i < 3; ++i ) scaled[i] = std::scalbn( parts[i], powers[i] - maxPower );
				const Scalar length = std::sqrt( scaled[0]*scaled[0] + scaled[1]*scaled[1] + scaled[2]*scaled[2] );
				// Divide before the final scaling: an intermediate component
				// below denorm_min can become representable after normalization.
				return Vector3( std::scalbn(parts[0]/length,powers[0]-maxPower),
					std::scalbn(parts[1]/length,powers[1]-maxPower), std::scalbn(parts[2]/length,powers[2]-maxPower) );
			}

			inline Scalar VisibleFraction( const Vector3& w, Scalar ax, Scalar ay )
			{
				// zi/(zi+li), without rounding a subnormal li first.
				const Scalar cosine = std::abs( NormalizeScaled(w,Vector3(ax,ay,1)).z );
				return cosine/(1+cosine);
			}

			inline PositiveProduct DistributionProduct( const Vector3& m, Scalar ax, Scalar ay )
			{
				PositiveProduct result;
				if( m.z <= 0 ) { result.mantissa = 0; return result; }
				const Scalar values[3] = { std::abs(m.x), std::abs(m.y), m.z };
				const Scalar widths[3] = { ax, ay, 1 };
				Scalar parts[3] = { 0, 0, 0 };
				int powers[3] = { 0, 0, 0 }, maxPower = 0;
				for( int i = 0; i < 3; ++i ) {
					if( values[i] == 0 ) continue;
					int ev = 0, ew = 0;
					parts[i] = std::frexp( values[i], &ev ) / std::frexp( widths[i], &ew );
					powers[i] = ev - ew;
					maxPower = r_max( maxPower, powers[i] );
				}
				for( int i = 0; i < 3; ++i ) parts[i] = std::scalbn( parts[i], powers[i] - maxPower );
				const Scalar norm = std::sqrt( parts[0]*parts[0] + parts[1]*parts[1] + parts[2]*parts[2] );
				result.Multiply( INV_PI ); result.Divide( ax ); result.Divide( ay );
				for( int i = 0; i < 4; ++i ) result.Divide( norm );
				result.exponent -= 4 * maxPower;
				return result;
			}

			inline Scalar Distribution( const Vector3& m, Scalar ax, Scalar ay )
			{
				return DistributionProduct( m, ax, ay ).Value();
			}

			inline Scalar DifferenceOfProducts( Scalar a, Scalar b, Scalar c, Scalar d )
			{
				const Scalar cd = c*d;
				return std::fma(a,b,-cd) + std::fma(-c,d,cd);
			}

			inline Vector3 Cross( const Vector3& a, const Vector3& b )
			{
				return Vector3( DifferenceOfProducts(a.y,b.z,a.z,b.y),
					DifferenceOfProducts(a.z,b.x,a.x,b.z), DifferenceOfProducts(a.x,b.y,a.y,b.x) );
			}

			inline void PolarizedFresnel( Scalar a, Scalar b, Scalar& reflection, Scalar& transmission )
			{
				if( a == 0 || b == 0 ) { reflection = 1; transmission = 0; return; }
				int ea = 0, eb = 0;
				std::frexp(a,&ea); std::frexp(b,&eb);
				const int shift = 1-r_max(ea,eb);
				const Scalar sa = std::scalbn(a,shift), sb = std::scalbn(b,shift);
				const Scalar ratio = (sa-sb)/(sa+sb);
				reflection = ratio*ratio;
				// Keep tiny amplitudes out of squared denominators even when
				// fast-math reassociates ratios and products.
				PositiveProduct tail;
				tail.Multiply(4); tail.Multiply(a); tail.Multiply(b);
				tail.Divide(a+b); tail.Divide(a+b);
				transmission = tail.Value();
			}

			inline Scalar Fresnel( Scalar cosI, Scalar sinI, Scalar eta, Scalar& cosT, Scalar& transmission )
			{
				// Preserve sin(theta) independently of a cosine rounded to 1.
				// Dividing sine also avoids squaring an extreme eta.
				const Scalar sinT = sinI / eta;
				if( sinT >= 1 ) { cosT = 0; transmission = 0; return 1; }
				cosT = std::sqrt( ( 1 - sinT ) * ( 1 + sinT ) );
				// Scale each ratio by the larger index, avoiding eta*cos
				// overflow without changing the Fresnel equations.
				const Scalar a = eta >= 1 ? cosI/eta : cosI;
				const Scalar b = eta >= 1 ? cosT : eta*cosT;
				const Scalar c = eta >= 1 ? cosI : eta*cosI;
				const Scalar d = eta >= 1 ? cosT/eta : cosT;
				Scalar rs = 0, rp = 0, ts = 0, tp = 0;
				PolarizedFresnel(a,b,rs,ts); PolarizedFresnel(c,d,rp,tp);
				Scalar reflection = (rs+rp)*Scalar(0.5);
				// Compute small transmission directly; 1-R loses it when R
				// rounds to 1. Form only the larger coefficient by complement.
				transmission = (ts+tp)*Scalar(0.5);
				if( reflection <= transmission ) transmission = 1-reflection;
				else reflection = 1-transmission;
				return reflection;
			}

			// The sampler retains its micronormal: an outgoing direction
			// rounded to double can lose that information when eta is one
			// ULP from 1. Reconstructing it would create artificial nulls.
			inline Evaluation EvaluateWithNormal( const Vector3& wi, const Vector3& wo, const Vector3& m,
				Scalar ax, Scalar ay, Scalar eta, bool radiance )
			{
				Evaluation result;
				const bool reflection = wo.z > 0;
				const Scalar ci = Vector3Ops::Dot( wi, m ), co = Vector3Ops::Dot( wo, m );
				if( ci <= 0 || ( reflection ? co <= 0 : co >= 0 ) ) return result;
				Scalar cosT = 0, transmission = 0;
				const Vector3 plane = Cross(wi,m);
				const Scalar sinI = std::hypot(plane.x,std::hypot(plane.y,plane.z));
				const Scalar F = Fresnel( ci, sinI, eta, cosT, transmission );
				if( !reflection && transmission == 0 ) return result;
				PositiveProduct f = DistributionProduct( m, ax, ay ), pdf = f;
				const Scalar zi = wi.z, zo = std::abs( wo.z );
				const Scalar ai = VisibleFraction(wi,ax,ay), ao = VisibleFraction(wo,ax,ay);
				// common = (zi+li)*(zo+lo)*(ai+ao-2*ai*ao). Keep
				// these factors separate so grazing products cannot underflow.
				const Scalar mask = ai + ao - 2*ai*ao;
				if( reflection ) {
					f.Multiply(F); f.Divide(2); f.Multiply(ai); f.Divide(zi); f.Multiply(ao); f.Divide(zo); f.Divide(mask);
					pdf.Multiply(F); pdf.Divide(2); pdf.Multiply(ai); pdf.Divide(zi);
				} else {
					// Factor the absolute inverse of co+ci/eta using Snell's
					// identity, scaling by the larger index before multiplication.
					Scalar inverseDenom;
					if( eta >= 1 ) {
						const Scalar invEta = 1/eta;
						inverseDenom = (std::abs(co)+ci*invEta)/(std::abs((1-eta)/eta)*(1+invEta));
					} else {
						inverseDenom = (eta*((eta*std::abs(co)+ci)/(1+eta)))/(1-eta);
					}
					f.Multiply(2); f.Multiply(transmission); f.Multiply(ci); f.Multiply(std::abs(co));
					f.Multiply(inverseDenom); f.Multiply(inverseDenom); f.Multiply(ai); f.Divide(zi);
					pdf = f;
					f.Multiply(ao); f.Divide(zo); f.Divide(mask);
					if( radiance ) { f.Divide(eta); f.Divide(eta); }
				}
				result.f = f.Value(); result.pdf = pdf.Value();
				result.positiveDensity = pdf.mantissa > 0;
				return result;
			}

			/// Direction-only evaluation. For an actual draw, use Sample's
			/// f/pdf: when refraction's half-vector is unresolved (for example,
			/// at machine-adjacent indices), the rounded direction cannot
			/// uniquely recover the sampled micronormal or its density.
			inline Evaluation Evaluate( const Vector3& wi, const Vector3& wo,
				Scalar ax, Scalar ay, Scalar eta, bool radiance )
			{
				if( !ValidParameters( ax, ay, eta ) || wi.z <= 0 || wo.z == 0 || ax == 0 || eta == 1 ) return {};
				Vector3 m = wo.z > 0 ? wi + wo : Vector3(
					std::fma( eta, wo.x, wi.x ), std::fma( eta, wo.y, wi.y ), std::fma( eta, wo.z, wi.z ) );
				if( m.x == 0 && m.y == 0 && m.z == 0 ) return {};
				m = NormalizeScaled( m, Vector3(1,1,1) );
				if( m.z < 0 ) m = -m;
				return EvaluateWithNormal( wi, wo, m, ax, ay, eta, radiance );
			}

			inline Sample Draw( const Vector3& wi, Scalar ax, Scalar ay, Scalar eta,
				Scalar uNormal1, Scalar uNormal2, Scalar uLobe, bool radiance )
			{
				Sample result;
				if( !ValidParameters( ax, ay, eta ) || wi.z <= 0 ||
					!( uNormal1 >= 0 && uNormal1 < 1 ) || !( uNormal2 >= 0 && uNormal2 < 1 ) ||
					!( uLobe >= 0 && uLobe < 1 ) ) return result;
				result.delta = ax == 0 || eta == 1;
				Vector3 m( 0, 0, 1 );
				if( !result.delta ) {
					// Dupuy-Benyoub spherical-cap VNDF, in the local frame.
					const Vector3 vh = NormalizeScaled( wi, Vector3(ax,ay,1) );
					const Scalar z = 1 - uNormal2 * ( 1 + vh.z );
					const Scalar r = std::sqrt( r_max( Scalar(0), ( 1 - z ) * ( 1 + z ) ) );
					const Scalar phi = TWO_PI * uNormal1;
					m = NormalizeScaled( Vector3( vh.x + r * std::cos( phi ),
						vh.y + r * std::sin( phi ), ( 1 + vh.z ) * ( 1 - uNormal2 ) ), Vector3(ax,ay,1) );
				}
				const Scalar ci = Vector3Ops::Dot( wi, m );
				const Vector3 plane = Cross(wi,m);
				const Scalar sinI = std::hypot(plane.x,std::hypot(plane.y,plane.z));
				Scalar cosT = 0, transmission = 1;
				const Scalar F = eta == 1 ? Scalar(0) : Fresnel( ci, sinI, eta, cosT, transmission );
				// Put the smaller interval first so a tiny transmission tail
				// survives even when the reflection coefficient rounds to 1.
				result.transmission = transmission < F ? uLobe < transmission : uLobe >= F;
				if( eta == 1 ) {
					result.wo = -wi;
				} else if( result.transmission ) {
					// Preserve 1-eta BEFORE rounding its reciprocal. Express
					// refraction as a correction to -wi, so even normal-incidence
					// adjacent-index transmission stays exactly straight through.
					if( eta >= Scalar(0.5) && eta <= Scalar(2) ) {
						// Sterbenz's interval: subtraction from 1 is exact.
						const Scalar invEta = 1/eta;
						const Scalar contrast = ( 1 - eta ) / eta;
						const Scalar normalRatio = ( invEta + 1 ) / ( ci * invEta + cosT );
						result.wo = -wi + ( m * normalRatio - wi ) * contrast;
					} else {
						// Far from unity, use Snell's tangent/normal decomposition
						// to avoid cancelling the entire incident vector.
						const Vector3 tangent = NormalizeScaled(Cross(m,plane),Vector3(1,1,1));
						result.wo = tangent * (-sinI/eta) - m * cosT;
					}
				} else {
					result.wo = m * ( 2 * ci ) - wi;
				}
				if( result.transmission ? result.wo.z >= 0 : result.wo.z <= 0 ) return result;
				if( result.delta ) {
					result.pdf = result.transmission ? transmission : F;
					PositiveProduct weight;
					if( result.transmission && radiance ) { weight.Divide(eta); weight.Divide(eta); }
					result.weight = weight.Value();
					PositiveProduct value;
					value.Multiply(result.pdf); value.Divide(std::abs(result.wo.z));
					if( result.transmission && radiance ) { value.Divide(eta); value.Divide(eta); }
					result.f = value.Value();
					result.valid = result.pdf > 0;
				} else {
					const Evaluation evaluated = EvaluateWithNormal( wi, result.wo, m, ax, ay, eta, radiance );
					result.f = evaluated.f;
					result.pdf = evaluated.pdf;
					result.valid = evaluated.positiveDensity;
					// G2/G1(wi), after cancelling Fresnel and the refractive Jacobian.
					const Scalar ai = VisibleFraction(wi,ax,ay), ao = VisibleFraction(result.wo,ax,ay);
					result.weight = ao / ( ai + ao - 2*ai*ao );
					if( result.transmission && radiance ) {
						// Exponent scaling also prevents fast-math reassociation
						// from recreating an overflowing eta*eta intermediate.
						PositiveProduct weight;
						weight.Multiply(result.weight); weight.Divide(eta); weight.Divide(eta);
						result.weight = weight.Value();
					}
				}
				return result;
			}
		}
	}
}

#endif
