//////////////////////////////////////////////////////////////////////
//
//  TmmReference.h - N-layer characteristic-matrix transfer-matrix
//    method (TMM) reference for thin-film reflectance.  Ground-truth
//    oracle for the thin-film feature (docs/THIN_FILM_INTERFERENCE.md
//    §5).  Header-only, std::complex<double>, NO renderer dependency.
//
//    Reference: Macleod, "Thin-Film Optical Filters", the
//    characteristic-matrix method; Born & Wolf, "Principles of Optics".
//
//    Method (per polarization p in {s, p}):
//      Snell:        N0 sinθ0 = Nj sinθj
//      cosθj:        forward-travelling root of sqrt(1 - (N0 sinθ0/Nj)^2)
//      Admittance:   η_s = N cosθ ;  η_p = N / cosθ          (per-pol)
//      Phase:        δj = (2π/λ) Nj dj cosθj                 (complex if absorbing)
//      Layer matrix: Mj = [[ cosδj,        -i sinδj / ηj ],
//                          [ -i ηj sinδj,   cosδj        ]]
//      Stack:        M = M1 * M2 * ... * MM  (ambient -> substrate order)
//      Admittance:   [B; C] = M * [1; η_s]
//      Reflectance:  r = (η0 B - C) / (η0 B + C) ;  R = |r|^2
//
//    The reflectance is assembled in that Y-free form, and every term is
//    carried PRE-SCALED by AdmittanceScale() (1 for s, cosθ for p) so the
//    1/cosθ in η_p is cleared.  Reflectance depends only on RATIOS of
//    admittances, so the common scale cancels -- the result is
//    algebraically identical to the textbook Y = C/B form wherever that is
//    finite, and additionally FINITE where it is not (see the degeneracy
//    notes below).
//
//    With M = 1 layer this reduces algebraically to the Airy summation in
//    AiryReference.h; the test asserts they agree to ~machine epsilon,
//    which is the cross-check that catches p-polarization sign bugs a
//    single implementation cannot.
//
//    CONVENTION (e^{-iωt} time dependence; matches Born & Wolf / Macleod /
//    Byrnes' `tmm`):
//      * cosθj is the FORWARD-TRAVELLING root: Re(Nj cosθj) > 0 (tie-broken
//        by Im(Nj cosθj) > 0).  At normal incidence this keeps cosθ = +1
//        even for an absorbing medium -- it does NOT flip the sign.  This
//        is the classic bug site: a naive "Im(N cosθ) >= 0, else negate"
//        rule wrongly flips cosθ to -1 for an absorbing medium at normal
//        incidence; the forward rule does not.
//      * The forward wave accrues phase e^{+iδ} and DECAYS into an
//        absorbing layer (Im(δ) >= 0), so the round-trip Airy factor is
//        e^{+2iδ} (see AiryReference.h) and the characteristic matrix uses
//        the -i sinδ off-diagonal sign above.  Pairing the +i sinδ matrix
//        (or e^{-2iδ} Airy) with this branch gives a GROWING wave and
//        R >> 1 for absorbing films -- exactly what the TMM<->Airy
//        cross-check is built to catch.
//
//    NOTE on the design doc: docs/THIN_FILM_INTERFERENCE.md §5 literally
//    writes the +i sinδ matrix, the e^{-2iδ} Airy factor, and an
//    "Im(N cosθ) >= 0" branch.  That trio is internally INCONSISTENT (the
//    matrix/Airy signs belong to the opposite phase convention); taken
//    verbatim it diverges for absorbing films.  This reference uses the
//    self-consistent Born & Wolf / Macleod convention instead, which is
//    bit-identical to the doc's intent on all lossless cases and on every
//    real-ambient absorbing stack (the heat-tint regime).  Flagged for the
//    controller to reconcile the doc text.
//
//    NUMERICAL DOMAIN / known limits (all well outside the heat-tint
//    regime of ~5..400 nm oxide films, but documented for callers who
//    push the oracle harder):
//      * Very thick STRONGLY-absorbing films overflow this matrix form:
//        the layer-matrix entries grow like e^{|Im δ|}, and for
//        d * k / λ large enough (empirically d >~ 50 µm at k = 3) both
//        Bs and Cs overflow to inf and the r numerator/denominator become
//        NaN.  (Measured: still exactly this threshold -- finite at
//        d = 10 µm, NaN from d = 50 µm -- the scaled assembly did not
//        change it.)  The Airy form in
//        AiryReference.h stays finite there (its e^{+2iδ} underflows to 0),
//        so prefer Airy for extreme thick-absorber queries.  Single-film
//        production use lifts the Airy form anyway (see design doc §7).
//      * Exactly grazing incidence θ = 90° and exactly the critical angle
//        both drive some medium's cosθ to exactly 0, which makes the
//        UNSCALED η_p = N/cosθ infinite.  The pre-scaled assembly above
//        handles both: measured R_s = R_p = 1 (finite) at θ = 90° for a
//        bare interface in either direction, a dielectric film, a
//        frustrated-TIR gap and an absorbing film, and at exactly the
//        critical angle for both a bare interface and a stack with a film.
//        R -> 1 is the correct limit in every one of those cases.
//        RESIDUAL: a FILM sitting exactly at ITS OWN critical angle is
//        still NaN (measured) -- the layer loop's `etaj` is the one
//        admittance not carried pre-scaled, and clearing it needs a
//        running scale factor threaded through the matrix product.  No
//        test exercises that geometry.  Prior to the 2026-07-29
//        reformulation θ = 90° and the critical angle were BOTH NaN; the
//        critical-angle case was a live ThinFilmTMMTest failure once
//        -fno-finite-math-only stopped the optimiser from folding it.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINFILM_TMMREFERENCE_H
#define THINFILM_TMMREFERENCE_H

#include <complex>
#include <vector>
#include <cmath>

#include "ThinFilmStack.h"

namespace RISE
{
	namespace ThinFilmReference
	{
		//! Polarization selector.
		enum Polarization
		{
			ePolS = 0,	//!< s-polarized (TE): E perpendicular to plane of incidence
			ePolP = 1	//!< p-polarized (TM): E in the plane of incidence
		};

		//! Per-polarization reflectance result.
		struct ReflectanceResult
		{
			double	Rs;		//!< s-polarized reflectance |r_s|^2
			double	Rp;		//!< p-polarized reflectance |r_p|^2

			//! Unpolarized average R = 1/2 (Rs + Rp).
			double Unpolarized() const { return 0.5 * ( Rs + Rp ); }
		};

		namespace detail
		{
			//! Picks the FORWARD-TRAVELLING cosθ root.  A wave is forward
			//! if its tilted admittance η = N cosθ carries energy into the
			//! medium: Re(N cosθ) > 0, or (Re == 0 and Im(N cosθ) > 0) for
			//! a purely evanescent wave.  Otherwise the other root is taken.
			//!
			//! This is the convention of Byrnes' `tmm` is_forward_angle and
			//! of Born & Wolf / Macleod: at normal incidence cosθ stays +1
			//! even for an absorbing medium (Re(N) > 0), and the forward
			//! wave decays (Im(δ) >= 0).  A naive "Im(N cosθ) >= 0 else
			//! negate" rule would wrongly flip cosθ to -1 at normal
			//! incidence whenever Im(N) > 0 -- the classic absorbing-media
			//! branch bug.
			inline Complex PickForwardCos( const Complex& N, const Complex& cosCandidate )
			{
				const Complex eta = N * cosCandidate;
				const bool forward =
					( eta.real() > 0.0 ) ||
					( eta.real() == 0.0 && eta.imag() > 0.0 );
				return forward ? cosCandidate : -cosCandidate;
			}

			//! cosθ in a medium of index N given the invariant
			//! s = N0 sinθ0 (preserved across all interfaces by Snell's
			//! law).  Returns the forward-travelling branch.
			//!
			//!   sinθ = s / N ;  cosθ = sqrt(1 - sinθ^2)
			inline Complex CosThetaInMedium( const Complex& N, const Complex& sinTheta0TimesN0 )
			{
				const Complex sinTheta = sinTheta0TimesN0 / N;
				const Complex cos2 = Complex( 1.0, 0.0 ) - sinTheta * sinTheta;
				const Complex root = std::sqrt( cos2 );		// principal branch
				return PickForwardCos( N, root );
			}

			//! Tilted optical admittance for the chosen polarization.
			//!   η_s = N cosθ ;  η_p = N / cosθ
			inline Complex Admittance( const Complex& N, const Complex& cosTheta, Polarization pol )
			{
				if( pol == ePolS ) {
					return N * cosTheta;
				}
				return N / cosTheta;
			}

			//! Amplitude reflection coefficient at an a -> b interface, in a form
			//! that never divides by cosθ:
			//!   s: (Na cosa - Nb cosb) / (Na cosa + Nb cosb)
			//!   p: (Na cosb - Nb cosa) / (Na cosb + Nb cosa)
			//! Both are the admittance ratio (ηa - ηb)/(ηa + ηb) with the common
			//! cosa*cosb factor cleared, so they stay finite and correct when
			//! EITHER cosθ is exactly 0 -- i.e. exactly at the critical angle,
			//! where the result is |r| = 1 (total internal reflection).
			inline Complex InterfaceReflection(
				const Complex& Na, const Complex& cosa,
				const Complex& Nb, const Complex& cosb,
				Polarization pol )
			{
				const Complex a = ( pol == ePolS ) ? Na * cosa : Na * cosb;
				const Complex b = ( pol == ePolS ) ? Nb * cosb : Nb * cosa;
				return ( a - b ) / ( a + b );
			}

			//! Polarization scale that clears the 1/cosθ in η_p.  η * scale is
			//! finite for both polarizations:
			//!   s: scale = 1,     η*scale = N cosθ
			//!   p: scale = cosθ,  η*scale = N
			//! Used by the N-layer matrix assembly, where the admittances enter
			//! linearly and a common scale factor cancels in the final ratio.
			inline Complex AdmittanceScale( const Complex& cosTheta, Polarization pol )
			{
				return ( pol == ePolS ) ? Complex( 1.0, 0.0 ) : cosTheta;
			}

			//! η pre-multiplied by AdmittanceScale(): finite even at cosθ = 0.
			inline Complex ScaledAdmittance( const Complex& N, const Complex& cosTheta, Polarization pol )
			{
				return ( pol == ePolS ) ? N * cosTheta : N;
			}
		}

		//! Computes the single-polarization reflectance of the stack at
		//! wavelength `lambda_nm` and incidence angle `thetaI_rad` (the
		//! angle in the ambient medium, measured from the surface normal).
		inline double TmmReflectanceForPol(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad,
			Polarization pol )
		{
			using namespace detail;

			const Complex N0 = stack.ambientIndex;
			// Snell invariant s = N0 sinθ0 (the ambient is taken real, so
			// sinθ0 is real; the invariant is carried as a complex so the
			// math is uniform for all layers).
			const Complex sinInvariant = N0 * std::sin( thetaI_rad );

			// cosθ and admittance in the ambient.
			const Complex cos0 = CosThetaInMedium( N0, sinInvariant );

			// Characteristic matrix of the whole film stack, M = Π Mj,
			// accumulated in ambient -> substrate order.  Start from the
			// identity.
			Complex m00( 1.0, 0.0 ), m01( 0.0, 0.0 );
			Complex m10( 0.0, 0.0 ), m11( 1.0, 0.0 );

			const double twoPi = 2.0 * 3.14159265358979323846;

			for( size_t j = 0; j < stack.films.size(); ++j ) {
				const Complex Nj = stack.films[j].index;
				const double dj = stack.films[j].thickness_nm;

				const Complex cosj = CosThetaInMedium( Nj, sinInvariant );
				const Complex etaj = Admittance( Nj, cosj, pol );

				// Phase thickness δj = (2π/λ) Nj dj cosθj.
				const Complex delta = Complex( twoPi * dj / lambda_nm, 0.0 ) * Nj * cosj;

				const Complex cosD = std::cos( delta );
				const Complex sinD = std::sin( delta );
				const Complex negI( 0.0, -1.0 );

				// Layer (Abelès characteristic) matrix Mj, in the e^{+iδ}-
				// forward / e^{-iωt} convention:
				//   Mj = [[ cosδ,        -i sinδ / η ],
				//         [ -i η sinδ,    cosδ       ]]
				const Complex a00 = cosD;
				const Complex a01 = negI * sinD / etaj;
				const Complex a10 = negI * etaj * sinD;
				const Complex a11 = cosD;

				// M <- M * Mj (right-multiply: ambient-side layers applied
				// first accumulate on the left, which is the standard
				// product order for [B;C] = M [1; η_s]).
				const Complex n00 = m00 * a00 + m01 * a10;
				const Complex n01 = m00 * a01 + m01 * a11;
				const Complex n10 = m10 * a00 + m11 * a10;
				const Complex n11 = m10 * a01 + m11 * a11;

				m00 = n00; m01 = n01;
				m10 = n10; m11 = n11;
			}

			// Substrate admittance.
			const Complex Ns = stack.substrateIndex;
			const Complex cosS = CosThetaInMedium( Ns, sinInvariant );
			// [B; C] = M * [1; η_s], carried PRE-SCALED by AdmittanceScale(cosS)
			// so an infinite η_s -- cosθ_s = 0, i.e. EXACTLY at the critical
			// angle -- never enters the arithmetic.  Bs = B*scaleS, Cs = C*scaleS.
			const Complex scaleS = AdmittanceScale( cosS, pol );
			const Complex etaSs  = ScaledAdmittance( Ns, cosS, pol );
			const Complex Bs = m00 * scaleS + m01 * etaSs;
			const Complex Cs = m10 * scaleS + m11 * etaSs;

			// r = (η0 B - C)/(η0 B + C) -- the Y-free form -- multiplied through
			// by scale0*scaleS so no term ever needs a 1/cosθ.  That common
			// factor cancels in the ratio, so this is algebraically identical to
			// (η0 - Y)/(η0 + Y) wherever the latter is finite, and additionally
			// correct at the critical angle, where it yields |r| = 1 exactly.
			const Complex scale0 = AdmittanceScale( cos0, pol );
			const Complex eta0s  = ScaledAdmittance( N0, cos0, pol );
			const Complex num = eta0s * Bs - scale0 * Cs;
			const Complex den = eta0s * Bs + scale0 * Cs;
			const Complex r = num / den;
			return std::norm( r );		// |r|^2
		}

		//! Computes s, p, and unpolarized reflectance of the stack.
		inline ReflectanceResult TmmReflectance(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad )
		{
			ReflectanceResult out;
			out.Rs = TmmReflectanceForPol( stack, lambda_nm, thetaI_rad, ePolS );
			out.Rp = TmmReflectanceForPol( stack, lambda_nm, thetaI_rad, ePolP );
			return out;
		}

		//! Unpolarized reflectance R = 1/2 (Rs + Rp).
		inline double TmmReflectanceUnpolarized(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad )
		{
			return TmmReflectance( stack, lambda_nm, thetaI_rad ).Unpolarized();
		}
	}
}

#endif
