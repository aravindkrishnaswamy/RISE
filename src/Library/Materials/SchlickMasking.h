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
#include <limits>

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
				double cFast;		//!< Eq.31 is inside the bound for every c >= cFast, at every azimuth
				bool   swapAxes;	//!< isotropy > 1: A peaks along onb.v(), not onb.u()
				bool   bounded;		//!< false: degenerate parameters, Eq.31 is returned as is
			};

			inline void PrepareUncached( Lane& L, const double roughness, const double isotropy )
			{
				L.r = roughness;
				L.sqrtR = ( roughness > 0 ) ? std::sqrt( roughness ) : 0.0;
				L.bounded = ( roughness > 0 ) && ( isotropy > 0 ) && std::isfinite( isotropy ) && std::isfinite( roughness );
				L.swapAxes = ( isotropy > 1.0 );
				const double p = L.swapAxes ? 1.0 / isotropy : isotropy;
				L.p = p;
				if( !L.bounded ) {
					L.sqrtP = L.q = L.cp = L.C2 = L.Wmax = L.Qmax = L.QmaxOver2Pi = L.WmaxOver4 = 0;
					L.cFast = 0;
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

				// cFast: with atan x <= x, E <= k Qmax at every azimuth, so
				// Eq.31 is inside the bound wherever
				//   f(c) = c (r + (1-r) c - c cp) - r (1-c^2) Qmax/(2 pi) >= 0,
				// i.e. a c^2 + r c - r Qm >= 0 with Qm = Qmax/(2 pi) and
				// a = 1 - r - cp + r Qm.  f(0) = -r Qm < 0 and
				// f(1) = 1 - cp >= 0, so {f >= 0} on (0,1] is [c_a, 1] with
				// c_a = 2 r Qm / (r + sqrt(r^2 + 4 a r Qm)) (the root that
				// is positive for a > 0 and the smaller one for a < 0,
				// written without cancellation).
				{
					const double Qm = L.QmaxOver2Pi;
					const double a = 1.0 - L.r - L.cp + L.r * Qm;
					const double disc = L.r * L.r + 4.0 * a * L.r * Qm;
					L.cFast = ( disc >= 0 ) ? 2.0 * L.r * Qm / ( L.r + std::sqrt( disc ) ) : 1.0;
					L.cFast = std::min( 1.0, std::max( 0.0, L.cFast ) );
				}
			}

			//! PrepareUncached behind a two-entry thread-local memo keyed
			//! on the exact (roughness, isotropy) pair: a material hit
			//! evaluates the same lane from value(), Scatter() and Pdf()
			//! many times, and the AGM costs a handful of square roots.
			//! Pure value key, per-thread storage, no shared mutation.
			inline void Prepare( Lane& L, const double roughness, const double isotropy )
			{
				// Isotropy exactly 1 (the common case) needs no elliptic
				// integrals: A == 1, c(p) = 1, C2 = 0, and the bound is the
				// closed-form GGX Smith G1, which Eq.31 exceeds only when
				// r < 1/4 and cos < (1-4r)/(3-4r) -- so that IS cFast.
				if( isotropy == 1.0 && roughness > 0 && std::isfinite( roughness ) ) {
					L.r = roughness;
					L.sqrtR = std::sqrt( roughness );
					L.p = L.sqrtP = 1.0;
					L.q = 0.0;
					L.cp = 1.0;
					L.C2 = 0.0;
					L.Wmax = 2.0;
					L.Qmax = 0.5 * kPi;
					L.QmaxOver2Pi = 0.25;
					L.WmaxOver4 = 0.5;
					L.cFast = ( roughness < 0.25 ) ? ( 1.0 - 4.0 * roughness ) / ( 3.0 - 4.0 * roughness ) : 0.0;
					L.swapAxes = false;
					L.bounded = true;
					return;
				}
				struct Entry { double r, p; Lane lane; bool used; };
				static thread_local Entry cache[2] = { { 0, 0, Lane(), false }, { 0, 0, Lane(), false } };
				static thread_local int next = 0;
				for( int i = 0; i < 2; i++ ) {
					if( cache[i].used && cache[i].r == roughness && cache[i].p == isotropy ) {
						L = cache[i].lane;
						return;
					}
				}
				PrepareUncached( L, roughness, isotropy );
				cache[next].r = roughness;
				cache[next].p = isotropy;
				cache[next].lane = L;
				cache[next].used = true;
				next ^= 1;
			}

			//! W(c2) = int_window A(phi) cos(phi - phi_v) dphi at
			//! c2 = cos^2(phi_v) (folded isotropy p < 1), in closed form:
			//! 2 sqrt(p) [ c2 asinh(q x/p)/(q x p) ... ] with x = sqrt(c2),
			//! y = sqrt(1 - c2).  Also returns dW/dc2.
			inline void WAndSlope( const double p, const double q, const double c2, double& W, double& dW )
			{
				const double x = std::sqrt( c2 ), y = std::sqrt( std::max( 0.0, 1.0 - c2 ) );
				const double z = q * x / p, w = q * y;
				const double sp = std::sqrt( p );
				// 1 - w^2 = 1 - (1-c2)(1-p^2) = c2 + (1-c2) p^2, without the
				// cancellation 1 - w*w suffers once q rounds to 1.
				const double oneMinusW2 = c2 + ( 1.0 - c2 ) * p * p;
				W  = 2.0 * sp * ( c2 * AsinhOverZ( z ) / p + ( 1.0 - c2 ) * AsinOverZ( w ) );
				dW = 2.0 * sp * ( ( AsinhOverZ( z ) + 1.0 / std::sqrt( 1.0 + z*z ) ) / ( 2.0 * p )
				                - ( AsinOverZ( w ) + 1.0 / std::sqrt( oneMinusW2 ) ) / 2.0 );
			}

			//! Tangent lines of W(c2) at 12 nodes c2_j = (j/11)^4.  W is
			//! concave in c2 (its rearrangement makes it the convolution
			//! of two symmetric-decreasing functions; verified numerically
			//! down to isotropy 1e-4 in docs/DL225_BOUNDED_SCHLICK.md), so
			//! every tangent lies above W and min_j over them is an UPPER
			//! bound on W -- which is all the Jensen bound needs, since
			//! W atan(k Q / W) increases in W.  Looseness <= 1.31% at
			//! isotropy 1e-4 and <= 0.53% for isotropy >= .01.  Built once
			//! per distinct isotropy per thread, only when a direction
			//! first reaches the transcendental path.
			static const int kWNodes = 12;
			struct WTable
			{
				double p;
				bool   used;
				double a[kWNodes];			//!< tangent intercepts
				double b[kWNodes];			//!< tangent slopes
			};

			inline const WTable& GetWTable( const Lane& L )
			{
				static thread_local WTable cache[4] = {};
				static thread_local int next = 0;
				for( int i = 0; i < 4; i++ ) {
					if( cache[i].used && cache[i].p == L.p ) {
						return cache[i];
					}
				}
				WTable& t = cache[next];
				next = ( next + 1 ) & 3;
				t.p = L.p;
				t.used = true;
				for( int j = 0; j < kWNodes; j++ ) {
					const double u = double( j ) / double( kWNodes - 1 );
					const double x = ( u * u ) * ( u * u );
					double W = 0, dW = 0;
					WAndSlope( L.p, L.q, x, W, dW );
					if( std::isfinite( W ) && std::isfinite( dW ) ) {
						t.a[j] = W - dW * x;
						t.b[j] = dW;
					} else {
						// An isotropy so extreme that the slope at c2 = 0
						// overflows (p^2 below the double range): drop this
						// tangent.  The rest still upper-bound the concave W.
						t.a[j] = std::numeric_limits<double>::max();
						t.b[j] = 0;
					}
				}
				return t;
			}

			//! The part of MaskDen below cFast.  Kept separate from the
			//! cheap test in MaskDen, which is what the C_D replay's inner
			//! loop runs almost always; this holds every transcendental.
			//! Returns c / m(c) >= den31 (the Eq.31 value).
			inline double MaskDenSlow( const Lane& L, const double c, const double tx, const double ty, const double den31 )
			{
				const double s = std::sqrt( std::max( 0.0, 1.0 - c*c ) );
				if( L.q == 0 ) {
					// Isotropy 1: the rearrangement/Cauchy-Schwarz bound is
					// exact, i.e. the closed-form GGX Smith G1 of Z,
					// c / m = (c + sqrt(c^2 + r s^2)) / 2.
					return std::max( den31, 0.5 * ( c + std::sqrt( c*c + L.r * s*s ) ) );
				}

				// Eq.31 is inside the bound iff c*cp + g E <= den31, with
				// g = sqrt(r) s / (2 pi).  Sufficient test first, with the
				// azimuth-free bound E <= (pi/2) Wmax (atan x <= pi/2; W is
				// maximal at the anisotropy peak): a pass returns exactly
				// what the full evaluation would.
				if( c * L.cp + L.sqrtR * s * L.WmaxOver4 <= den31 ) {
					return den31;
				}

				const double T = tx*tx + ty*ty;
				double c2 = 1.0, s2 = 0.0;
				if( T > 0 ) {
					c2 = tx*tx / T;
					s2 = 1.0 - c2;
				}
				if( L.swapAxes ) {
					std::swap( c2, s2 );
				}
				const WTable& wt = GetWTable( L );
				double W = wt.a[0] + wt.b[0] * c2;
				for( int j = 1; j < kWNodes; j++ ) {
					W = std::min( W, wt.a[j] + wt.b[j] * c2 );
				}
				const double k = L.sqrtR * s / c;
				const double g = L.sqrtR * s / ( 2.0 * kPi );		// multiplies E in c * I/nv
				const double Q = 0.5 * ( kPi * L.cp + ( c2 - s2 ) * L.C2 );
				// Same sufficient test at THIS azimuth, still atan-free:
				// W atan(k Q/W) <= min(k Q, (pi/2) W).
				if( c * L.cp + g * std::min( k * Q, 0.5 * kPi * W ) <= den31 ) {
					return den31;
				}
				double E = W * std::atan( k * Q / W );
				// The rearrangement/Cauchy-Schwarz bound matters near
				// isotropy 1 (it is exact there, and Jensen's gap reaches
				// ~3%).  Below isotropy 1/2 Jensen is within 0.5% of the
				// smaller of the two everywhere (CS was tighter in 13 of
				// 194560 swept (isotropy, azimuth, k) states, by at most
				// 0.50%; docs/DL225_BOUNDED_SCHLICK.md), so the second
				// atan is skipped there.  Either alone is a valid bound.
				if( L.p >= 0.5 ) {
					E = std::min( E, std::sqrt( M2( k, L.p, L.q ) * EIso( k ) ) );
				}
				return std::max( den31, c * L.cp + g * E );
			}

			//! c / m(c, phi) for a direction at cosine `c` from the normal
			//! whose tangential components in (onb.u, onb.v) are (tx, ty)
			//! (need not be normalized): the masking's DENOMINATOR form,
			//! so a caller forms ONE division exactly as the Eq.31 code
			//! did with (r + (1-r)nv)(r + (1-r)nl).  Positive and finite as
			//! c -> 0.  Equals Eq.31's r + (1-r)c wherever that is inside
			//! the bound, and is >= it everywhere (m never exceeds Eq.31).
			inline double MaskDen( const Lane& L, const double c, const double tx, const double ty )
			{
				const double den31 = L.r + ( 1.0 - L.r ) * c;		// c / G_Eq31(c)
				if( !L.bounded || c <= 0 || c >= 1.0 || c >= L.cFast ) {
					return den31;
				}
				return MaskDenSlow( L, c, tx, ty, den31 );
			}

			//! m(c, phi) / c = 1 / MaskDen.
			inline double MaskOverCos( const Lane& L, const double c, const double tx, const double ty )
			{
				return 1.0 / MaskDen( L, c, tx, ty );
			}
		}
	}
}

#endif
