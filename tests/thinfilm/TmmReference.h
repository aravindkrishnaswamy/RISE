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
//      * cosθj is the FORWARD-TRAVELLING root: the DECAYING one,
//        Im(Nj cosθj) > 0, tie-broken by Re(Nj cosθj) > 0 for a lossless
//        medium at a propagating angle.  Testing Im first is load-bearing
//        (the Im == 0 tie-break is exact; Re == 0 never fires) -- see
//        PickForwardCos.  At normal incidence this keeps cosθ = +1 even for
//        an absorbing medium -- it does NOT flip the sign, because
//        Im(η) = k > 0 there.  (This file used to add that a naive
//        "Im(N cosθ) >= 0, else negate" rule "wrongly flips cosθ to -1"
//        there.  That is FALSE and was never measured -- for N = 2.5 + 3i
//        that rule also keeps +1.  Production ThinFilm.h retracted the
//        same claim on 2026-07-31; this copy was missed, which is the
//        third time a correction landed on one twin and not the other.)
//      * The forward wave accrues phase e^{+iδ} and DECAYS into an
//        absorbing layer (Im(δ) >= 0), so the round-trip Airy factor is
//        e^{+2iδ} (see AiryReference.h) and the characteristic matrix uses
//        the -i sinδ off-diagonal sign above.  Pairing the +i sinδ matrix
//        (or e^{-2iδ} Airy) with this branch gives a GROWING wave and
//        R >> 1 for absorbing films -- exactly what the TMM<->Airy
//        cross-check is built to catch.
//
//    NOTE on the design doc: docs/THIN_FILM_INTERFERENCE.md §5 was
//    reconciled with this file on 2026-07-30 (it had literally written the
//    +i sinδ matrix, the e^{-2iδ} Airy factor, and an "Im(N cosθ) >= 0"
//    branch -- a trio that is internally INCONSISTENT and diverges for
//    absorbing films taken verbatim).  This reference uses the
//    self-consistent Born & Wolf / Macleod convention.
//
//    NUMERICAL DOMAIN / known limits (all well outside the heat-tint
//    regime of ~5..400 nm oxide films, but documented for callers who
//    push the oracle harder):
//      * THICK-ABSORBER OVERFLOW -- CLOSED 2026-07-30, and the note that
//        stood here is now HISTORY, not a live caveat.  It read: "very
//        thick strongly-absorbing films overflow this matrix form; the
//        layer-matrix entries grow like e^{|Im δ|}, cliff ~1.0e4 nm
//        (~10.25 µm) at k = 3 under the shipped -ffast-math flags; the
//        2026-07-29 scaled assembly REDUCED that headroom by roughly
//        |eta0| (37 of 800k random stacks regressed from finite to NaN);
//        prefer the Airy form for extreme thick-absorber queries."  All of
//        that described the cosδ / sinδ layer matrix.  The exponent-
//        factored matrix below never forms a quantity of modulus > ~1 per
//        layer, so within the measured envelope there is no cliff and no
//        headroom to lose: finite for BOTH forms over 4,000,000 random
//        stacks with |N| in 1e-3..1e3 and thickness to 1e12 nm (0
//        non-finite, versus 1,553,854 for the old matrix on the same
//        inputs).  Airy remains the single-film production form for COST,
//        not range (design doc §7).
//        ⚠ THE ENVELOPE IS THE CLAIM -- "total" is NOT.  Far outside the
//        range of real optical constants a cliff remains, from a different
//        mechanism: -ffast-math implies -fcx-limited-range, so std::complex
//        division is the naive (ac+bd, bc-ad)/(c²+d²) and that c²+d² term
//        can overflow; the same happens for a very small index, whose
//        interface products grow like 1/n.  The failure count out there
//        depends strongly on how the stack is sampled -- two defensible
//        readings of the same experiment differed by more than 10x, and an
//        earlier version of this note stated one reading's numbers as fact
//        AND asserted "0 under strict IEEE", which a second reading
//        contradicted.  No table is quoted here for that reason.  What is
//        stable across every sampling tried: the shipped-index envelope
//        (|N| in 1e-3..1e3, thickness to 1e12 nm) is CLEAN, both forms,
//        both flag sets.  `ar_layer` bounds n > 0, thickness > 0 and the
//        layer count, but places no MAGNITUDE cap, so the region outside is
//        reachable in principle by a pathological scene and unreachable by
//        a physical one.
//      * Exactly grazing incidence θ = 90° and exactly the critical angle
//        both drive some medium's cosθ to exactly 0, which makes the
//        UNSCALED η_p = N/cosθ infinite.  The pre-scaled assembly above
//        handles both: measured R_s = R_p = 1 (finite) at θ = 90° for a
//        bare interface in either direction, a dielectric film, a
//        frustrated-TIR gap and an absorbing film, and at exactly the
//        critical angle for both a bare interface and a stack with a film.
//        R -> 1 is the correct limit in every one of those cases.
//        DEGENERACY (closed 2026-07-30): cos == 0 in a NON-ENDPOINT
//        medium -- a film at its own critical angle, and also exactly
//        grazing when a film index equals the ambient's -- was NaN in both
//        evaluators, by two mechanisms (the p admittance N/cos was
//        infinite; the s one was ZERO, making -i sinD/eta a 0/0).  Closed
//        by the layer matrix below, whose delta/eta and delta*eta products
//        are finite for both polarizations at cos == 0.  The Airy form in
//        AiryReference.h was ALSO 0/0 there while it divided at each
//        interface first (r01 == -r1s, delta == 0); it now factors the
//        vanishing 2*N1*cos1 out of the quotient and is total too.  An
//        earlier note here claimed that cancellation was "structural, so
//        no factoring removes it" -- that was WRONG, disproved
//        constructively.  Both forms now agree at the degeneracy
//        (ThinFilmTMMTest [8/8](b), on ABSORBING substrates).  Before the
//        2026-07-29 reformulation θ = 90° and the critical angle were BOTH
//        NaN; the critical-angle case was a live ThinFilmTMMTest failure
//        once -fno-finite-math-only stopped the optimiser folding it.
//      * EVANESCENT BRANCH -- CLOSED 2026-07-30.  PickForwardCos used to
//        test Re(η) first, with a tie-break that never fired, so for an
//        evanescent medium it selected the GROWING root and |e^{+2iδ}|
//        diverged with thickness (onset d ~ 4 µm).  See PickForwardCos.
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
			//! Picks the FORWARD-TRAVELLING cosθ root: the DECAYING one.
			//! Kept BIT-IDENTICAL to production ThinFilm.h PickForwardCos,
			//! whose comment carries the full derivation, the measurement and
			//! the history.  In brief:
			//!
			//! A passive medium cannot amplify, so the forward root is the one
			//! with Im(η) > 0 for η = N cosθ.  Im(η) is exactly 0 only for a
			//! lossless medium at a propagating angle, where neither root
			//! decays and the forward one is instead Re(η) > 0.  Testing Im
			//! FIRST is load-bearing: the `Im(η) == 0` tie-break is EXACT
			//! (1,224,959 firings in 4,000,000 evaluations), while
			//! `Re(η) == 0` never fires for an EVANESCENT medium -- the case a
			//! Re-first rule delegates to it -- because std::sqrt of a
			//! negative real leaves a ~6.1e-17 real residue (0 of the same
			//! 4,000,000; it fires only when η is exactly 0, at a medium's own
			//! critical angle, where both roots are 0 anyway).  So a Re-first
			//! rule decides the evanescent branch on round-off noise and
			//! selects the GROWING root.  That is what this did before
			//! 2026-07-30.
			//!
			//! At normal incidence cosθ stays +1 even for an absorbing medium,
			//! because Im(η) = k > 0.  An earlier note here claimed a naive
			//! "Im(N cosθ) >= 0 else negate" rule differs by negating the
			//! exact-zero propagating case; measured, it does not -- that rule
			//! keeps both the absorbing normal-incidence root and the
			//! exact-zero propagating one.  Claim withdrawn rather than
			//! replaced with another unverified taxonomy.
			inline Complex PickForwardCos( const Complex& N, const Complex& cosCandidate )
			{
				const Complex eta = N * cosCandidate;
				const bool forward =
					( eta.imag() > 0.0 ) ||
					( eta.imag() == 0.0 && eta.real() > 0.0 );
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



			//! The CLEARED (numerator, denominator) pair of an a -> b
			//! interface; the amplitude ratio is (a-b)/(a+b) of these.
			//! Exposed so the Airy form can factor the vanishing cos out of
			//! its quotient instead of dividing it away first.
			inline void InterfaceTerms(
				const Complex& Na, const Complex& cosa,
				const Complex& Nb, const Complex& cosb,
				Polarization pol, Complex& a, Complex& b )
			{
				a = ( pol == ePolS ) ? Na * cosa : Na * cosb;
				b = ( pol == ePolS ) ? Nb * cosb : Nb * cosa;
			}


			//! sin(z)/z with its REMOVABLE singularity at z = 0 evaluated
			//! analytically (the limit is exactly 1).  Not a threshold fudge:
			//! below the cut the Taylor series IS the exact double value
			//! (first omitted term z^6/5040 < 1e-27 at |z| = 1e-4), so both
			//! branches are exact.  Sole consumer is ExpM1OverZ, via
			//! sinh(w)/w = Sinc(i*w); the layer matrix used to call it
			//! directly, before the 2026-07-30 exponent-factored form.
			inline Complex Sinc( const Complex& z )
			{
				if( std::abs( z ) < 1e-4 ) {
					const Complex z2 = z * z;
					return Complex( 1.0, 0.0 ) - z2 / 6.0 + ( z2 * z2 ) / 120.0;
				}
				return std::sin( z ) / z;
			}

			//! Phase-thickness coefficient kd = 2*pi*d/lambda, normalized so a
			//! layer whose phase is not a POSITIVE FINITE number contributes
			//! none.  kd >= 0 is exactly the condition under which the
			//! decaying root gives Im(delta) >= 0 and hence
			//! |e^{+2i delta}| <= 1, which is what keeps both evaluators
			//! overflow-free.  Kept in step with production ThinFilm.h
			//! PhaseCoefficient, whose comment carries the derivation.
			//!
			//! The test is on kd, NOT on thickness.  An earlier version here
			//! normalized only a negative thickness while citing production's
			//! rule -- which by then had already been corrected, because
			//! lambda_nm is just as unvalidated and lambda < 0 flips the sign
			//! of kd exactly as d < 0 does.  Measured on this oracle before
			//! the correction: air / (2.1+0.6i, 300 nm) / silver at
			//! cosTheta = 0.7 and lambda = -550 returned R = 8.3737 -- the
			//! growing-wave failure the condition exists to prevent, from the
			//! file that is supposed to be the ground truth.
			inline double PhaseCoefficient( double thickness_nm, double lambda_nm )
			{
				const double kd = 2.0 * 3.14159265358979323846 * thickness_nm / lambda_nm;
				return ( kd > 0.0 && std::isfinite( kd ) ) ? kd : 0.0;
			}

			//! (e^z - 1)/z, entire, limit exactly 1 at z = 0.  TWO forms, and
			//! the split matters in both directions -- see ThinFilm.h for the
			//! full note and the measurements:
			//!   |z| <= 1 : the cancellation-free identity
			//!             (e^z-1)/z = e^{z/2} sinh(z/2)/(z/2) with
			//!             sinh(w)/w = Sinc(i*w).  The naive (exp(z)-1)/z
			//!             loses the result to cancellation for small |z|
			//!             (8.05e-13 vs 4.5e-16 at |z| = 3e-4, measured).
			//!   |z| >  1 : the DIRECT form -- the only one that survives a
			//!             thick absorbing or evanescent layer, where Re(z)
			//!             is large and negative and the identity would
			//!             evaluate 0 * inf = NaN.
			inline Complex ExpM1OverZ( const Complex& z )
			{
				// See ThinFilm.h for the full note: direct form above |z| = 1
				// (the only one that survives a thick absorbing film, where
				// the identity would give 0 * inf = NaN), cancellation-free
				// identity below it.
				if( std::abs( z ) > 1.0 ) {
					return ( std::exp( z ) - Complex( 1.0, 0.0 ) ) / z;
				}
				const Complex half = z / 2.0;
				return std::exp( half ) * Sinc( Complex( 0.0, 1.0 ) * half );
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

			for( size_t j = 0; j < stack.films.size(); ++j ) {
				const Complex Nj = stack.films[j].index;
				const double dj = stack.films[j].thickness_nm;

				const Complex cosj = CosThetaInMedium( Nj, sinInvariant );
				// Layer matrix in the EXPONENT-FACTORED form -- see production
				// ThinFilm.h TmmReflectanceForPol for the full derivation.  It
				// is total against both degeneracies of this loop:
				//   (1) cos == 0 in a non-endpoint medium (a film at its own
				//       critical angle).  delta = kd*Nj*cosj carries exactly
				//       ONE factor of cos, so only these appear:
				//         s (eta = N cos):  delta/eta = kd        delta*eta = kd N^2 cos^2
				//         p (eta = N/cos):  delta/eta = kd cos^2  delta*eta = kd N^2
				//       both finite at cos == 0 for both polarizations, where
				//       the textbook sind/eta form gave Inf (p) or 0/0 (s).
				//   (2) OVERFLOW for a thick absorbing / evanescent layer:
				//       cosd and sind grow like e^{|Im delta|}.  Writing
				//       Mj = (e^{-i delta}/2) * Mhat_j with phi = e^{+2i delta}
				//       (|phi| <= 1 for the decaying forward root) and
				//         Mhat_j = [[ 1+phi, (1-phi)/eta ], [ eta(1-phi), 1+phi ]]
				//       drops a scalar that cancels exactly in r (which is
				//       homogeneous of degree 0 in M), leaving every entry
				//       O(1) at any thickness.  (1-phi) = -z E(z) at
				//       z = 2i delta, E = ExpM1OverZ.
				const double  kd = PhaseCoefficient( dj, lambda_nm );
				const Complex delta = Complex( kd, 0.0 ) * Nj * cosj;
				const Complex z     = Complex( 0.0, 2.0 ) * delta;
				const Complex phi   = std::exp( z );		// |phi| <= 1
				const Complex E     = ExpM1OverZ( z );		// (phi - 1)/z
				const Complex cos2  = cosj * cosj;
				const Complex Nj2   = Nj * Nj;
				const Complex deltaOverEta  = ( pol == ePolS ) ? Complex( kd, 0.0 )
				                                              : Complex( kd, 0.0 ) * cos2;
				const Complex deltaTimesEta = ( pol == ePolS ) ? Complex( kd, 0.0 ) * Nj2 * cos2
				                                              : Complex( kd, 0.0 ) * Nj2;
				const Complex negTwoI( 0.0, -2.0 );

				const Complex a00 = Complex( 1.0, 0.0 ) + phi;
				const Complex a01 = negTwoI * deltaOverEta  * E;
				const Complex a10 = negTwoI * deltaTimesEta * E;
				const Complex a11 = a00;

				// M <- M * Mj (right-multiply: ambient-side layers applied
				// first accumulate on the left, which is the standard
				// product order for [B;C] = M [1; η_s]).
				const Complex n00 = m00 * a00 + m01 * a10;
				const Complex n01 = m00 * a01 + m01 * a11;
				const Complex n10 = m10 * a00 + m11 * a10;
				const Complex n11 = m10 * a01 + m11 * a11;

				m00 = n00; m01 = n01;
				m10 = n10; m11 = n11;

				// Renormalize to O(1) by an exact power of two -- r is
				// homogeneous of degree 0 in M, so this changes no result.
				// Kept in step with production ThinFilm.h, whose comment
				// carries the derivation and the measured overflow it closes.
				{
					double mag = std::abs( m00 );
					if( std::abs( m01 ) > mag ) mag = std::abs( m01 );
					if( std::abs( m10 ) > mag ) mag = std::abs( m10 );
					if( std::abs( m11 ) > mag ) mag = std::abs( m11 );
					if( mag > 0.0 && std::isfinite( mag ) ) {
						int exponent = 0;
						std::frexp( mag, &exponent );
						// ldexp on each VALUE, not multiplication by a
						// precomputed 2^-exponent: for a denormal mag that
						// reciprocal overflows to inf.  See production
						// ThinFilm.h.
						m00 = Complex( std::ldexp( m00.real(), -exponent ),
						               std::ldexp( m00.imag(), -exponent ) );
						m01 = Complex( std::ldexp( m01.real(), -exponent ),
						               std::ldexp( m01.imag(), -exponent ) );
						m10 = Complex( std::ldexp( m10.real(), -exponent ),
						               std::ldexp( m10.imag(), -exponent ) );
						m11 = Complex( std::ldexp( m11.real(), -exponent ),
						               std::ldexp( m11.imag(), -exponent ) );
					}
				}
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
