//////////////////////////////////////////////////////////////////////
//
//  SchlickMasking.h - DL-225: the bounded, reciprocal masking factor
//    shared by SchlickBRDF and SchlickSPF.
//
//  Schlick 1994 Eq.31 multiplies the specular term by G(nv)G(nl) with
//  G(c) = c/(r + (1-r)c).  Schlick's own distribution is Z(t)A(phi)/pi,
//  with Z a GGX lobe of alpha^2 = r and A his azimuthal anisotropy
//  factor.  For any microfacet distribution D the projected area of
//  the microfacets a direction v can see is
//
//      I(v) = int max(0, v.m) D(m) dm,
//
//  and the masking of that direction can never exceed nv / I(v) (the
//  visible projected microfacet area cannot exceed the macro-surface's
//  own, Heitz 2014).  Given ANY masking m with m(v) <= nv/I(v) and
//  m <= 1, and S <= 1:
//
//      rho_d(v) = int S D m(v) m(l) (h.v)/nv dh
//              <= (m(v)/nv) int max(0, h.v) D dh = m(v) I(v)/nv <= 1,
//
//  and the BRDF S D m(v) m(l) / (4 nv nl) is reciprocal because m is a
//  function of ONE direction.  Eq.31 violates the bound only near
//  grazing at low roughness (isotropically when r < 1/4 and
//  cos < (1-4r)/(3-4r)); DL-225 keeps Eq.31 everywhere else and clips
//  it to a closed-form upper bound of I(v) where it overshoots:
//
//      m(v) = min( G_Eq31(nv), nv / I_bound(v) ).
//
//  I(v)/nv = c(p) + (k / 2pi) E(phi_v), k = sqrt(r) tan(theta_v), where
//  c(p) = (1/2pi) int A dphi and
//
//      E(phi_v) = int_{-pi/2}^{pi/2} A(phi_v + x) cos x atan(k cos x) dx
//
//  (the back-facing microfacets; the Z integral is done in closed
//  form).  E is bounded two ways, and the smaller wins:
//
//    * Jensen (atan concave): E <= W atan(k Q / W), with
//        W = int_window A cos x dx  (closed form: asinh, asin)
//        Q = int_window A cos^2 x dx = (pi c(p) + cos(2 phi_v) C2(p))/2.
//      Tight to a few percent at any view azimuth.
//    * Rearrangement + Cauchy-Schwarz: E <= sqrt(M2(k,p) E_iso(k)),
//      exact at isotropy 1, where E_iso = pi (sqrt(1+k^2)-1)/k and the
//      bound reduces m to the exact isotropic Smith G1 of Z.
//
//  c(p) and C2(p) are complete elliptic integrals, evaluated by the
//  arithmetic-geometric mean.  Isotropy > 1 is Schlick's A rotated a
//  quarter turn with 1/p (A_p(phi) = A_{1/p}(phi + pi/2)).
//
//  Full derivation, the grid tables and the appearance gate:
//  docs/DL225_BOUNDED_SCHLICK.md.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SCHLICK_MASKING_H
#define RISE_SCHLICK_MASKING_H

#include <algorithm>
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		namespace SchlickMasking
		{
			static const double kPi = 3.14159265358979323846264338327950288;

			//! asinh(z)/z, stable at z -> 0.
			inline double AsinhOverZ( const double z )
			{
				return ( std::fabs( z ) < 1e-4 ) ? 1.0 - z*z/6.0 : std::asinh( z ) / z;
			}

			//! asin(z)/z, stable at z -> 0 (|z| <= 1).
			inline double AsinOverZ( const double z )
			{
				return ( std::fabs( z ) < 1e-4 ) ? 1.0 + z*z/6.0 : std::asin( z ) / z;
			}

			//! E_iso(k) = int_{-pi/2}^{pi/2} cos x atan(k cos x) dx.
			inline double EIso( const double k )
			{
				return kPi * k / ( std::sqrt( 1.0 + k*k ) + 1.0 );
			}

			//! M2(k,p) = int_{-pi/2}^{pi/2} A(x)^2 cos x atan(k cos x) dx,
			//! A centred on its peak.  Closed form
			//! (pi/q)[atan(k/q) - atan(k p/(q sqrt(1+k^2)))], written through
			//! the atan difference identity so it is exact at q -> 0.
			inline double M2( const double k, const double p, const double q )
			{
				const double s  = std::sqrt( 1.0 + k*k );
				const double xq = k * ( s - p ) / ( q*q*s + k*k*p );		// x / q
				const double x  = q * xq;
				const double atanOverX = ( x < 1e-8 ) ? 1.0 : std::atan( x ) / x;
				return kPi * xq * atanOverX;
			}

			//! Per-lane constants: one (roughness, isotropy) pair.
			struct Lane
			{
				double r;			//!< Schlick roughness (alpha^2 of Z)
				double sqrtR;
				double p;			//!< isotropy folded into (0,1]
				double sqrtP;
				double q;			//!< sqrt(1 - p^2)
				double cp;			//!< (1/2pi) int A dphi
				double C2;			//!< int_0^pi A cos(2 phi) dphi
				double Wmax;		//!< W at the anisotropy peak (the maximum over azimuth)
				double Qmax;		//!< Q at the anisotropy peak (the maximum over azimuth)
				double QmaxOver2Pi;	//!< fast-path constants
				double WmaxOver4;
				bool   swapAxes;	//!< isotropy > 1: A peaks along onb.v(), not onb.u()
				bool   bounded;		//!< false: degenerate parameters, Eq.31 is returned as is
			};

			inline void Prepare( Lane& L, const double roughness, const double isotropy )
			{
				L.r = roughness;
				L.sqrtR = ( roughness > 0 ) ? std::sqrt( roughness ) : 0.0;
				L.bounded = ( roughness > 0 ) && ( isotropy > 0 ) && std::isfinite( isotropy ) && std::isfinite( roughness );
				L.swapAxes = ( isotropy > 1.0 );
				const double p = L.swapAxes ? 1.0 / isotropy : isotropy;
				L.p = p;
				if( !L.bounded ) {
					L.sqrtP = L.q = L.cp = L.C2 = L.Wmax = L.Qmax = L.QmaxOver2Pi = L.WmaxOver4 = 0;
					return;
				}
				L.sqrtP = std::sqrt( p );
				L.q = std::sqrt( std::max( 0.0, 1.0 - p*p ) );

				// AGM(1,p), and S = sum_{n>=1} 2^n c_n^2 / q^2 (so that
				// K - E = K (q^2/2) (1 + ...) never cancels): with
				// d_n = a_n - b_n = d_{n-1}^2 / (2 (sqrt a_{n-1} + sqrt b_{n-1})^2)
				// and e_n = d_n^2 / q^2, S = sum_{m>=0} 2^(m-1) e_m.
				double a = 1.0, b = p;
				double d = 1.0 - p;
				double e = ( 1.0 - p ) / ( 1.0 + p );
				double S = 0, w = 0.5;
				for( int n = 0; n < 12; n++ ) {
					S += w * e;
					const double sa = std::sqrt( a ), sb = std::sqrt( b );
					const double sum2 = ( sa + sb ) * ( sa + sb );
					const double dn = d * d / ( 2.0 * sum2 );
					e = e * d * d / ( 4.0 * sum2 * sum2 );
					const double an = 0.5 * ( a + b ), bn = sa * sb;
					a = an; b = bn; d = dn; w *= 2.0;
					if( e * w < 1e-18 * ( S + 1e-300 ) || d == 0 ) {
						break;
					}
				}
				// Finish the AGM (it converged alongside d -> 0).
				for( int n = 0; n < 8 && a - b > 1e-16 * a; n++ ) {
					const double an = 0.5 * ( a + b ), bn = std::sqrt( a * b );
					a = an; b = bn;
				}
				L.cp = L.sqrtP / a;
				L.C2 = kPi * L.cp * S;
				L.Wmax = 2.0 * L.sqrtP * AsinhOverZ( L.q / p ) / p;
				L.Qmax = 0.5 * ( kPi * L.cp + L.C2 );
				L.QmaxOver2Pi = L.Qmax / ( 2.0 * kPi );
				L.WmaxOver4 = 0.25 * L.Wmax;
			}

			//! m(c, phi) / c for a direction at cosine `c` from the normal
			//! whose tangential components in (onb.u, onb.v) are (tx, ty)
			//! (need not be normalized).  Finite as c -> 0.  Equals Eq.31's
			//! G(c)/c = 1/(r + (1-r)c) wherever that is inside the bound.
			inline double MaskOverCos( const Lane& L, const double c, const double tx, const double ty )
			{
				const double den31 = L.r + ( 1.0 - L.r ) * c;		// c / G_Eq31(c)
				if( !L.bounded || c <= 0 || c >= 1.0 ) {
					return 1.0 / den31;
				}

				// Eq.31 is inside the bound iff c*cp + g E <= den31, with
				// g = sqrt(r) s / (2 pi) and E <= W atan(k Q / W) (Jensen).
				// Three progressively tighter, progressively costlier
				// sufficient tests run first; each upper-bounds the Jensen
				// bound at EVERY azimuth (W, Q are maximal at the anisotropy
				// peak and W atan(k Q/W) increases in both), so a pass
				// returns exactly what the full evaluation would.
				//   (a) atan x <= x:    c cp + r s^2 Qmax / (2 pi c) <= den31
				if( c * ( den31 - c * L.cp ) >= L.r * ( 1.0 - c*c ) * L.QmaxOver2Pi ) {
					return 1.0 / den31;
				}
				const double s = std::sqrt( std::max( 0.0, 1.0 - c*c ) );
				//   (b) atan x <= pi/2: c cp + sqrt(r) s Wmax / 4 <= den31
				if( c * L.cp + L.sqrtR * s * L.WmaxOver4 <= den31 ) {
					return 1.0 / den31;
				}
				const double k = L.sqrtR * s / c;
				const double g = L.sqrtR * s / ( 2.0 * kPi );		// multiplies E in c * I/nv
				//   (c) the Jensen bound at the anisotropy peak.
				if( c * L.cp + g * L.Wmax * std::atan( k * L.Qmax / L.Wmax ) <= den31 ) {
					return 1.0 / den31;
				}

				double smith;
				if( L.q == 0 ) {
					// Isotropy 1: the rearrangement/Cauchy-Schwarz bound is
					// exact, i.e. the closed-form GGX Smith G1 of Z,
					// m/c = 2 / (c + sqrt(c^2 + r s^2)).
					smith = 2.0 / ( c + std::sqrt( c*c + L.r * s*s ) );
				} else {
					const double T = tx*tx + ty*ty;
					double c2 = 1.0, s2 = 0.0;
					if( T > 0 ) {
						c2 = tx*tx / T;
						s2 = 1.0 - c2;
					}
					if( L.swapAxes ) {
						std::swap( c2, s2 );
					}
					const double W = 2.0 * L.sqrtP * (
						c2 * AsinhOverZ( L.q * std::sqrt( c2 ) / L.p ) / L.p +
						s2 * AsinOverZ( L.q * std::sqrt( s2 ) ) );
					const double Q = 0.5 * ( kPi * L.cp + ( c2 - s2 ) * L.C2 );
					const double eJensen = W * std::atan( k * Q / W );
					const double eCS = std::sqrt( M2( k, L.p, L.q ) * EIso( k ) );
					smith = 1.0 / ( c * L.cp + g * std::min( eJensen, eCS ) );
				}
				return std::min( 1.0 / den31, smith );
			}
		}
	}
}

#endif
