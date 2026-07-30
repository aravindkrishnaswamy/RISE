//////////////////////////////////////////////////////////////////////
//
//  ThinFilm.h - Production thin-film interference reflectance for the
//    GGX microfacet conductor (heat-tint / anodization color).  This is
//    the lifted, renderer-side evaluator of the Phase-1 validated
//    single-film Airy oracle (tests/thinfilm/AiryReference.h) and the
//    N-layer characteristic-matrix TMM (tests/thinfilm/TmmReference.h),
//    re-expressed in RISE's Scalar (double) and std::complex<Scalar>.
//
//    Header-only, inline, allocation-free.  Called per-shade per-hero-
//    wavelength on the spectral path (see docs/THIN_FILM_INTERFERENCE.md
//    §7), so the hot path performs no heap allocation and a minimal
//    number of transcendentals.
//
//    The math is N-layer-capable internally (a fixed-capacity,
//    stack-allocated characteristic-matrix product), but the shipped
//    single-film entry point ThinFilm::ReflectanceConductor() serves the
//    air / oxide / metal stack, which is the Phase-2 material.  The Airy
//    closed form is used for the single-film case (one transcendental
//    complex exp), matching exactly what was validated in Phase 1.
//
//    PHYSICS / SIGN CONVENTION (docs/THIN_FILM_INTERFERENCE.md §5, as
//    corrected and validated by the Phase-1 reference -- reproduced here
//    EXACTLY so this code is bit-faithful to the oracle):
//
//      * Complex index  N = n + i*k,  k >= 0 for an absorbing medium.
//      * Time dependence e^{-iωt} (Born & Wolf / Macleod).
//      * cosθ in each medium is the FORWARD-TRAVELLING root:
//        Re(N cosθ) > 0, tie-broken by Im(N cosθ) > 0.  At normal
//        incidence this keeps cosθ = +1 even for an absorbing medium;
//        a naive "Im(N cosθ) >= 0 else negate" rule wrongly flips it.
//      * Per-polarization tilted admittance:  η_s = N cosθ ;  η_p = N/cosθ.
//      * Phase thickness  δ = (2π/λ) N d cosθ  (complex if the film
//        absorbs); the forward wave decays, Im(δ) >= 0.
//      * Single-film Airy:
//          r = ( r01 + r1s e^{+2iδ1} ) / ( 1 + r01 r1s e^{+2iδ1} )
//        with r_ab = (η_a - η_b)/(η_a + η_b), evaluated via
//        the FACTORED form (InterfaceTerms + ExpM1OverZ), which keeps
//        each interface as a cleared (numerator, denominator) pair and
//        factors the vanishing 2*N1*cos1 out analytically.
//        The e^{+2iδ1} (NOT
//        e^{-2iδ1}) sign is the one that makes a thick absorbing film
//        decay to the bare top-interface reflectance r01.  Pairing the
//        opposite-sign factor with this forward-root branch produces a
//        GROWING wave and R >> 1 for absorbing films -- the bug the
//        Phase-1 Airy<->TMM cross-check is built to catch.
//      * N-layer characteristic (Abelès) matrix, e^{+iδ}-forward form:
//          Mj = [[ cosδj, -i sinδj/ηj ], [ -i ηj sinδj, cosδj ]]
//
//    UNITS:  wavelength and film thickness are both in nanometres (nm);
//    only their ratio enters δ, so any consistent length unit works, but
//    nm is the documented choice.
//
//    cosThetaI CONVENTION (the single most important call-site fact):
//    the input cosine is the cosine of the angle between the INCIDENT
//    direction and the MICROFACET HALF-VECTOR (the local micronormal
//    that GGX importance-samples), in [0,1].  It is NOT the cosine to the
//    geometric surface normal.  Thin-film interference is a property of
//    the local interface, whose normal is the half-vector; the GGX
//    conductor Fresnel it replaces is evaluated at the same dot(wi, h).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THIN_FILM_
#define THIN_FILM_

#include <complex>
#include <cmath>

#include "Math3D/Math3D.h"		// RISE Scalar (double)
#include "Color/Color.h"		// RISEPel / XYZPel (RGB-path albedo basis)
#include "Color/ColorUtils.h"	// ColorUtils::XYZFromNM (renderer CMFs)
#include "MicrofacetEnergyLUT.h"	// GL_N / GL_nodes / GL_weights (shared Kulla-Conty F_avg quadrature)

namespace RISE
{
	namespace ThinFilm
	{
		//! Complex optical index / amplitude type used throughout, in
		//! RISE's Scalar precision (double).
		typedef std::complex<Scalar> Complex;

		//! Polarization selector.  Matches the Phase-1 reference.
		enum Polarization
		{
			ePolS = 0,		//!< s-polarized (TE): E perpendicular to plane of incidence
			ePolP = 1		//!< p-polarized (TM): E in the plane of incidence
		};

		//! The maximum number of films the stack-allocated N-layer path
		//! supports without heap.  Single film is the shipped material;
		//! this headroom keeps the math N-layer-capable at zero cost for
		//! the documented multi-oxide extension point (§3 generality).
		static const int kMaxFilms = 8;

		namespace detail
		{
			//! Builds a complex index from real n and extinction k, forcing
			//! k non-negative (the absorbing convention N = n + i|k|).
			inline Complex MakeIndex( Scalar n, Scalar k )
			{
				return Complex( n, k >= Scalar(0) ? k : -k );
			}

			//! Picks the FORWARD-TRAVELLING cosθ root.  A wave is forward if
			//! its tilted admittance η = N cosθ carries energy into the
			//! medium: Re(N cosθ) > 0, or (Re == 0 and Im(N cosθ) > 0) for a
			//! purely evanescent wave.  Otherwise the other root is taken.
			//! (Byrnes' `tmm` is_forward_angle / Born & Wolf / Macleod.)
			inline Complex PickForwardCos( const Complex& N, const Complex& cosCandidate )
			{
				const Complex eta = N * cosCandidate;
				const bool forward =
					( eta.real() > Scalar(0) ) ||
					( eta.real() == Scalar(0) && eta.imag() > Scalar(0) );
				return forward ? cosCandidate : -cosCandidate;
			}

			//! cosθ in a medium of index N given the Snell invariant
			//! s = N0 sinθ0 (preserved across all interfaces).  Returns the
			//! forward-travelling branch.   sinθ = s/N ; cosθ = sqrt(1-sin²θ).
			inline Complex CosThetaInMedium( const Complex& N, const Complex& sinTheta0TimesN0 )
			{
				const Complex sinTheta = sinTheta0TimesN0 / N;
				const Complex cos2 = Complex( Scalar(1), Scalar(0) ) - sinTheta * sinTheta;
				const Complex root = std::sqrt( cos2 );		// principal branch
				return PickForwardCos( N, root );
			}



			//! sin(z)/z, with its REMOVABLE singularity at z = 0 evaluated
			//! analytically (the limit is exactly 1).  This is not a threshold
			//! fudge: for |z| below the cut the Taylor series IS the exact
			//! double-precision value (the first omitted term, z^6/5040, is
			//! < 1e-27 at |z| = 1e-4), so BOTH branches are exact everywhere.
			//! Needed because the layer matrix is written with sinc products
			//! instead of sin/eta quotients -- see TmmReflectanceForPol.
			inline Complex Sinc( const Complex& z )
			{
				if( std::abs( z ) < Scalar(1e-4) ) {
					const Complex z2 = z * z;
					return Complex( Scalar(1), Scalar(0) )
						- z2 / Scalar(6)
						+ ( z2 * z2 ) / Scalar(120);
				}
				return std::sin( z ) / z;
			}

			//! Polarization scale that clears the 1/cosθ in η_p.  η * scale is
			//! finite for both polarizations:
			//!   s: scale = 1,     η*scale = N cosθ
			//!   p: scale = cosθ,  η*scale = N
			//! Used by the N-layer matrix assembly, where the admittances enter
			//! linearly and a common scale factor cancels in the final ratio.
			inline Complex AdmittanceScale( const Complex& cosTheta, Polarization pol )
			{
				return ( pol == ePolS ) ? Complex( Scalar(1), Scalar(0) ) : cosTheta;
			}

			//! η pre-multiplied by AdmittanceScale(): finite even at cosθ = 0.
			inline Complex ScaledAdmittance( const Complex& N, const Complex& cosTheta, Polarization pol )
			{
				return ( pol == ePolS ) ? N * cosTheta : N;
			}

			//! The Snell invariant s = N0 sinθ0 from the ambient index and
			//! the incidence cosine.  The ambient is taken real for the
			//! shipped air/oxide/metal stack, but the math is uniform in
			//! Complex.  sinθ0 is recovered as sqrt(1 - cos²) (avoids an
			//! acos/sin round-trip on the hot path).
			//!
			//! cosThetaI is clamped to [kGrazingCosFloor, 1], a small
			//! POSITIVE floor rather than 0.
			//!
			//! HISTORICAL NOTE (updated 2026-07-29): this clamp originally
			//! existed because at EXACTLY grazing (cosθ = 0) the
			//! p-polarization admittance η_p = N/cosθ is infinite and the
			//! then-current Fresnel form (η_a-η_b)/(η_a+η_b) evaluated to
			//! Inf/Inf = NaN.  That is NO LONGER the behaviour: the Fresnel
			//! coefficients now go through the factored form (InterfaceTerms) and the
			//! N-layer assembly carries pre-scaled admittances, so cosθ = 0
			//! yields the correct R -> 1 limit directly (measured, for a
			//! bare interface either direction, a dielectric film, an
			//! FTIR gap and an absorbing film).
			//!
			//! The clamp is KEPT as belt-and-braces and for the SECOND
			//! reason below (it keeps the Snell invariant strictly < 1,
			//! which is a representability property of THIS function, not
			//! of the downstream assembly).  An edge-on microfacet has zero
			//! projected area and is rejected upstream anyway.
			//!
			//! The floor is 1e-6, NOT NEARZERO (1e-12): the invariant is
			//! built from sqrt(1 - cos²), and for cos <~ sqrt(eps_double)
			//! (~1.5e-8) the term 1 - cos² rounds to exactly 1.0, collapsing
			//! sinθ0 to 1 and cosθ in the ambient back to 0 -- which would
			//! silently return the exact-grazing limit R = 1 instead of the
			//! near-grazing value the caller asked for.  (Before the
			//! 2026-07-29 reformulation that collapse produced NaN; it is now
			//! a precision loss rather than a NaN, which is still worth the
			//! floor.)  1e-6 is comfortably above that
			//! representability threshold (so 1 - cos² stays < 1) and is
			//! still ~1 microradian from grazing, where R is already ~1.
			static const Scalar kGrazingCosFloor = Scalar(1e-6);

			inline Complex SnellInvariant( const Complex& N0, Scalar cosThetaI )
			{
				Scalar c = cosThetaI;
				if( c < kGrazingCosFloor ) c = kGrazingCosFloor;
				if( c > Scalar(1) )        c = Scalar(1);
				const Scalar s0 = std::sqrt( Scalar(1) - c * c );
				return N0 * s0;
			}

			//! Single-film Airy reflectance for one polarization.  This is
			//! the closed form lifted from AiryReference.h, the partner of
			//! the N-layer TMM (they agree to ~machine epsilon by design).

			//! The CLEARED numerator/denominator pair of an a -> b interface:
			//! The amplitude ratio is (a - b)/(a + b) of these; callers keep the
			//! PAIR rather than the ratio so a vanishing cos can be factored
			//! out before dividing (see AiryReflectanceForPol).
			//! Exposed separately so the Airy form can factor the vanishing
			//! cos out of its quotient instead of dividing first and losing
			//! the information (see AiryReflectanceForPol).
			inline void InterfaceTerms(
				const Complex& Na, const Complex& cosa,
				const Complex& Nb, const Complex& cosb,
				Polarization pol, Complex& a, Complex& b )
			{
				a = ( pol == ePolS ) ? Na * cosa : Na * cosb;
				b = ( pol == ePolS ) ? Nb * cosb : Nb * cosa;
			}

			//! (e^z - 1)/z -- entire, limit exactly 1 at z = 0.
			//!
			//! TWO forms, each used ONLY where it is both accurate and
			//! finite.  Neither alone is adequate, and this was established
			//! by measurement in both directions:
			//!
			//!   |z| <= 1 : the CANCELLATION-FREE identity
			//!             (e^z - 1)/z = e^{z/2} * sinh(z/2)/(z/2), with
			//!             sinh(w)/w = sin(iw)/(iw) = Sinc(i*w) so it reuses
			//!             the one series helper.  The naive (exp(z) - 1)/z
			//!             is unusable here: exp(z) rounds to ~1 and the
			//!             subtraction destroys the result -- measured
			//!             1.08e-12 relative error at |z| = 3e-4 versus
			//!             2.2e-16 for the identity.
			//!   |z| >  1 : the DIRECT form.  Cancellation is negligible
			//!             (relative error ~eps/|z|), and it is the only form
			//!             that survives a thick absorbing film: there Im(z)
			//!             is large, exp(z) decays to 0 and the result tends
			//!             to -1/z, whereas the identity evaluates
			//!             0 * inf = NaN.  Using the identity everywhere
			//!             turned ThinFilmProductionTest's thick-absorber
			//!             assertions RED (2 failures, deterministic).
			//!
			//! Both branches agree to ~1e-16 at the switch.  Pinned by
			//! ThinFilmTMMTest [8/8](d) (accuracy, against an 8-term
			//! long-double reference) and ThinFilmProductionTest's
			//! thick-absorber assertions (finiteness).
			inline Complex ExpM1OverZ( const Complex& z )
			{
				// |z| > 1: the direct form.  Cancellation is negligible here
				// (relative error ~eps/|z| <= 2.2e-16), and it is the ONLY
				// form that survives a thick absorbing film: there Im(z) is
				// huge, exp(z) simply DECAYS to 0 and the result tends to
				// -1/z, whereas the identity below would evaluate
				// exp(z/2) * sinh(z/2)/(z/2) as 0 * inf = NaN.
				if( std::abs( z ) > Scalar(1) ) {
					return ( std::exp( z ) - Complex( Scalar(1), Scalar(0) ) ) / z;
				}
				// |z| <= 1: the cancellation-free identity.  No overflow is
				// possible (|Im z| <= 1), and it avoids the catastrophic
				// cancellation of exp(z) - 1 for small |z|.
				const Complex half = z / Scalar(2);
				return std::exp( half ) * Sinc( Complex( Scalar(0), Scalar(1) ) * half );
			}

			inline Scalar AiryReflectanceForPol(
				const Complex& N0, const Complex& N1, const Complex& Ns,
				Scalar thickness_nm, Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Complex cos0 = CosThetaInMedium( N0, sinInvariant );
				const Complex cos1 = CosThetaInMedium( N1, sinInvariant );
				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );

				// Airy summation, with the vanishing cos_film FACTORED OUT
				// rather than divided away.
				//
				// The textbook form r = (r01 + r1s e^{2id}) / (1 + r01 r1s
				// e^{2id}) is 0/0 when the FILM's own cos is exactly 0: there
				// r01 = +-1 exactly and d = 0, so numerator and denominator
				// vanish together.  Dividing at each interface first DESTROYS
				// the common cos_film factor that makes the limit well
				// defined -- and the failure is not even loud: for any
				// absorbing medium below the film the two vanishing
				// quantities are equal-but-nonzero, so the quotient comes out
				// FINITE and exactly 1 (a perfect mirror) instead of NaN.
				// Measured before this fix: glass/air-gap/silver returned
				// 1.000 where the true value is 0.886, error up to 0.477 on
				// Ti -- silently, on the air/oxide/metal stack this feature
				// exists for.
				//
				// So keep the interfaces in CLEARED (numerator, denominator)
				// form and factor 2*N1*cos1 out of both sides analytically.
				// With (a1,b1) the ambient<->film pair, (a2,b2) the
				// film<->substrate pair, (p,q) the ambient<->substrate pair
				// (film skipped), z = 2i*delta and E(z) = (e^z - 1)/z:
				//
				//   num = 2 N1 cos1 [ (p - q) + i kd (a2-b2)(a1+b1) E ]
				//   den = 2 N1 cos1 [ (p + q) + i kd (a1-b1)(a2-b2) E ]
				//
				// The 2 N1 cos1 cancels exactly, leaving a form with NO
				// vanishing factor.  Algebraically identical to the textbook
				// quotient wherever that is defined, and correct at
				// cos_film == 0, where it agrees with the N-layer TMM.
				Complex a1, b1, a2, b2, p, q;
				InterfaceTerms( N0, cos0, N1, cos1, pol, a1, b1 );
				InterfaceTerms( N1, cos1, Ns, cosS, pol, a2, b2 );
				InterfaceTerms( N0, cos0, Ns, cosS, pol, p,  q  );

				const Scalar  kd    = TWO_PI * thickness_nm / lambda_nm;
				const Complex delta = Complex( kd, Scalar(0) ) * N1 * cos1;
				const Complex E     = ExpM1OverZ( Complex( Scalar(0), Scalar(2) ) * delta );
				const Complex ikd( Scalar(0), kd );

				const Complex num = ( p - q ) + ikd * ( a2 - b2 ) * ( a1 + b1 ) * E;
				const Complex den = ( p + q ) + ikd * ( a1 - b1 ) * ( a2 - b2 ) * E;

				return std::norm( num / den );		// |r|²
			}

			//! N-layer characteristic-matrix reflectance for one
			//! polarization.  Allocation-free: the film indices/thicknesses
			//! are passed as fixed-capacity C arrays.  With nFilms == 1 this
			//! reduces algebraically to AiryReflectanceForPol (Phase-1
			//! cross-check).  Provided so the single tested evaluator stays
			//! N-layer-capable at zero cost for the multi-oxide extension.
			inline Scalar TmmReflectanceForPol(
				const Complex& N0,
				const Complex* filmIndex, const Scalar* filmThickness_nm, int nFilms,
				const Complex& Ns,
				Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Complex cos0 = CosThetaInMedium( N0, sinInvariant );

				// Characteristic matrix M = Π Mj, ambient -> substrate order.
				Complex m00( Scalar(1), Scalar(0) ), m01( Scalar(0), Scalar(0) );
				Complex m10( Scalar(0), Scalar(0) ), m11( Scalar(1), Scalar(0) );

				for( int j = 0; j < nFilms; ++j ) {
					const Complex Nj = filmIndex[j];
					const Scalar  dj = filmThickness_nm[j];

					const Complex cosj = CosThetaInMedium( Nj, sinInvariant );
					// cos == 0 in a NON-ENDPOINT medium -- a film at its own
					// critical angle, and also exactly grazing when a film
					// index equals the ambient's -- was NaN in BOTH evaluators
					// until 2026-07-30.  In THIS (TMM) evaluator by two
					// mechanisms: the p-polarization admittance N/cos was
					// infinite, and the s-polarization one was ZERO, making
					// -i*sinD/eta a 0/0.  (The Airy form failed differently --
					// 0/0 algebraically, but a SILENT finite 1.0 on any
					// absorbing substrate; see AiryReflectanceForPol.)
					// -ffast-math had masked both.
					//
					// CLOSED here by the sinc layer matrix immediately below
					// (which removed the admittance from this loop entirely),
					// and in the single-film path by the FACTORED Airy form --
					// not by the pairing, which is defence in depth only.
					// Pinned by ThinFilmTMMTest [8/8] and
					// ThinFilmProductionTest's [Degenerate] block.
					// Layer matrix, written so that NO term divides by cos.
					//   Mj = [[ cosd, -i sind/eta ], [ -i eta sind, cosd ]]
					// with sind = delta * sinc(delta), and delta = kd * Nj * cosj
					// carrying exactly ONE factor of cos:
					//   s (eta = N cos):  delta/eta = kd          delta*eta = kd N^2 cos^2
					//   p (eta = N/cos):  delta/eta = kd cos^2    delta*eta = kd N^2
					// Both are finite for BOTH polarizations at cos == 0, so a film
					// sitting exactly at its own critical angle no longer produces
					// eta = Inf (p) or the 0/0 of -i sind/eta with eta == 0 (s).
					const Scalar  kd = TWO_PI * dj / lambda_nm;
					const Complex delta = Complex( kd, Scalar(0) ) * Nj * cosj;
					const Complex cosD  = std::cos( delta );
					const Complex sincD = Sinc( delta );
					const Complex cos2  = cosj * cosj;
					const Complex Nj2   = Nj * Nj;
					const Complex deltaOverEta  = ( pol == ePolS )
						? Complex( kd, Scalar(0) )
						: Complex( kd, Scalar(0) ) * cos2;
					const Complex deltaTimesEta = ( pol == ePolS )
						? Complex( kd, Scalar(0) ) * Nj2 * cos2
						: Complex( kd, Scalar(0) ) * Nj2;
					const Complex negI( Scalar(0), Scalar(-1) );

					const Complex a00 = cosD;
					const Complex a01 = negI * sincD * deltaOverEta;
					const Complex a10 = negI * sincD * deltaTimesEta;
					const Complex a11 = cosD;

					// M <- M * Mj
					const Complex n00 = m00 * a00 + m01 * a10;
					const Complex n01 = m00 * a01 + m01 * a11;
					const Complex n10 = m10 * a00 + m11 * a10;
					const Complex n11 = m10 * a01 + m11 * a11;

					m00 = n00; m01 = n01;
					m10 = n10; m11 = n11;
				}

				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );
				// [B; C] = M * [1; eta_s], carried PRE-SCALED by AdmittanceScale(cosS)
				// so an infinite eta_s -- cosTheta_s = 0, i.e. EXACTLY at the critical
				// angle -- never enters the arithmetic.  Bs = B*scaleS, Cs = C*scaleS.
				const Complex scaleS = AdmittanceScale( cosS, pol );
				const Complex etaSs  = ScaledAdmittance( Ns, cosS, pol );
				const Complex Bs = m00 * scaleS + m01 * etaSs;
				const Complex Cs = m10 * scaleS + m11 * etaSs;

				// r = (eta0 B - C)/(eta0 B + C) -- the Y-free form -- multiplied through
				// by scale0*scaleS so no term ever needs a 1/cosTheta.  That common
				// factor cancels in the ratio, so this is algebraically identical to
				// (eta0 - Y)/(eta0 + Y) wherever the latter is finite, and additionally
				// correct at the critical angle, where it yields |r| = 1 exactly.
				const Complex scale0 = AdmittanceScale( cos0, pol );
				const Complex eta0s  = ScaledAdmittance( N0, cos0, pol );
				const Complex num = eta0s * Bs - scale0 * Cs;
				const Complex den = eta0s * Bs + scale0 * Cs;
				const Complex r = num / den;
				return std::norm( r );		// |r|²
			}

			//! Single-film reflectance for one polarization.
			//!
			//! Primary form is the FACTORED Airy summation, which is total at
			//! cos_film == 0 (it factors the vanishing 2*N1*cos1 out of the
			//! quotient instead of dividing it away).  Airy is kept primary
			//! because the N-layer TMM overflows on thick strongly-absorbing
			//! films (measured NaN from d ~ 1.0e4 nm at k = 3) where Airy is
			//! finite past 1e6 nm.
			//!
			//! The TMM fallback is DEFENCE IN DEPTH for the case where Airy
			//! does not produce a finite value at all.  It is NOT the
			//! degeneracy fix, and it must not be relied on as one:
			//! `isfinite` was tried as the degeneracy trigger on 2026-07-30
			//! and is provably the WRONG predicate -- with the unfactored
			//! quotient and any absorbing medium below the film, the
			//! degenerate case is finite and exactly 1.0 (a perfect mirror,
			//! wrong by up to 0.477 on Ti), so the fallback never fires and
			//! the error is silent.  The fix had to be in the FORMULATION,
			//! not the trigger.  Pinned by ThinFilmTMMTest [8/8](b) and
			//! ThinFilmProductionTest [Degenerate], both of which now use
			//! ABSORBING substrates for exactly this reason.
			//!
			//! ⚠ KNOWN DEFECT, NOT GUARDED -- ROOT CAUSE IDENTIFIED
			//! (measured 2026-07-30; an earlier version of this note
			//! UNDERSTATED it, see below).
			//!
			//! PickForwardCos (:115) selects the GROWING evanescent root:
			//! for an evanescent film (ambient n > film n, beyond critical)
			//! Im(N1*cos1) < 0, so Im(delta) < 0 and the round-trip factor
			//! GROWS with thickness -- contradicting this file's own stated
			//! invariant at :34 ("the forward wave decays, Im(delta) >= 0").
			//! The Re(eta)==0 tie-break at :120 is dead code: it fired 0
			//! times in 4,000,000 evaluations (the residue is ~6.3e-17, never
			//! exactly 0).
			//!
			//! Consequences, BOTH reachable through the public API:
			//!   * NON-FINITE: 186,413 of 3,000,000 adversarial cases.  Onset
			//!     is d ~ 4 um -- NOT the 40-50 um an earlier draft of this
			//!     note claimed from a single stack.
			//!   * FINITE BUT WRONG (the dangerous class, which that draft
			//!     omitted entirely): the p-polarization Airy can return
			//!     EXACTLY 0, so the isfinite fallback below never fires.
			//!     Measured R = 0.4999999999999999 where the 120-dps truth
			//!     is 1.0.
			//!
			//! FIX (validated by review, NOT YET APPLIED): select the
			//! DECAYING root -- Im(N cos) > 0, tie-broken by Re(N cos) > 0.
			//! Measured 0 of 3,000,000 non-finite with worst-case accuracy
			//! unchanged, and all thin-film tests green.  Strictly only FILM
			//! media need the decaying rule (both the Airy quotient and the
			//! sinc layer matrix are provably invariant under cos_j -> -cos_j);
			//! endpoints need Re > 0.  Deferred as its own change because it
			//! alters branch selection for every medium.
			//!
			//! Shipped scenes use 40-220 nm films and are unaffected;
			//! triggering needs ambient n > film n AND d >~ 4 um, which
			//! nothing currently validates.
			//!
			//! Note also that the thick-absorber comparison above (TMM NaN
			//! from ~1.0e4 nm, Airy finite past 1e6 nm) is specific to the
			//! ABSORBING-film regime; it does not hold for the lossless
			//! evanescent case just described.
			inline Scalar SingleFilmReflectanceForPol(
				const Complex& N0, const Complex& N1, const Complex& Ns,
				Scalar thickness_nm, Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Scalar airy = AiryReflectanceForPol(
					N0, N1, Ns, thickness_nm, lambda_nm, sinInvariant, pol );
				if( std::isfinite( airy ) ) { return airy; }

				const Complex film[1]  = { N1 };
				const Scalar  thick[1] = { thickness_nm };
				return TmmReflectanceForPol(
					N0, film, thick, 1, Ns, lambda_nm, sinInvariant, pol );
			}
		}

		//======================================================================
		//  Public hot-path API
		//======================================================================

		//! Unpolarized reflectance R = ½(R_s + R_p) of an
		//! ambient / single-film / substrate stack at a single hero
		//! wavelength, using the validated Airy closed form.
		//!
		//! \param cosThetaI    cosine of the angle between the incident
		//!                     direction and the microfacet half-vector,
		//!                     in [0,1] (NOT the geometric-normal cosine).
		//! \param wavelength_nm hero wavelength, nm.
		//! \param n0           ambient real index (N0 = n0 + i k0; air = 1+0i).
		//! \param k0           ambient extinction.
		//! \param n1           film real index (N1 = n1 + i k1; the oxide).
		//! \param k1           film extinction (k1>0 absorbing).
		//! \param thickness_nm physical film thickness, nm.
		//! \param n2           substrate real index (Ns = n2 + i k2; the metal).
		//! \param k2           substrate extinction.
		//! \return             unpolarized reflectance R in [0,1].
		//!
		//! No heap allocation; one complex exp on the hot path.
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			const Complex N0 = detail::MakeIndex( n0, k0 );
			const Complex N1 = detail::MakeIndex( n1, k1 );
			const Complex Ns = detail::MakeIndex( n2, k2 );

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolP );

			// R is mathematically in [0,1] for a passive stack; clamp only
			// to defend against FP round-off pushing it a hair past the
			// boundary (e.g. 1+1e-16), never to mask a real overflow.
			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//! Convenience overload taking already-built complex indices.
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& N0,
			const Complex& N1,
			Scalar thickness_nm,
			const Complex& Ns )
		{
			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolP );

			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//! General N-layer unpolarized reflectance (the documented
		//! multi-oxide extension point).  `nFilms` must be in [0, kMaxFilms];
		//! `filmIndex` / `filmThickness_nm` are caller-owned arrays of that
		//! length (ambient -> substrate order).  With nFilms == 1 this is
		//! algebraically the Airy single-film result.  Allocation-free.
		inline Scalar ReflectanceConductorStack(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& N0,
			const Complex* filmIndex,
			const Scalar* filmThickness_nm,
			int nFilms,
			const Complex& Ns )
		{
			if( nFilms < 0 ) nFilms = 0;
			if( nFilms > kMaxFilms ) nFilms = kMaxFilms;

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::TmmReflectanceForPol(
				N0, filmIndex, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::TmmReflectanceForPol(
				N0, filmIndex, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolP );

			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//======================================================================
		//  RGB-path albedo-basis integration (PREVIEW — see §8)
		//======================================================================
		//
		//  The RGB rendering path has no wavelength, so the spectral
		//  interference reflectance R(λ) of the air/oxide/metal stack is
		//  pre-integrated against the renderer's CIE colour-matching
		//  functions in the ALBEDO BASIS (docs/THIN_FILM_INTERFERENCE.md §8,
		//  second bullet):
		//
		//      XYZ = Σ R(λ)·cmf(λ)·Δλ  /  Σ ȳ(λ)·Δλ
		//
		//  This is WHITE-NORMALIZED and ILLUMINANT-INDEPENDENT: a perfect
		//  reflector R(λ) ≡ 1 maps to XYZ = (white point of the equal-energy
		//  illuminant), and after XYZ→Rec.709 to neutral RGB (1,1,1) — NOT a
		//  D65-tinted colour.  It is deliberately DIFFERENT from the Phase-1
		//  swatch's D65 preview (§8, first bullet), which weights by S_D65(λ)
		//  to predict "what the surface looks like under a daylight viewer".
		//  Here the renderer multiplies this reflectance by the incident RGB
		//  radiance, so the normaliser must be illuminant-free.
		//
		//  The result is a linear Rec.709 reflectance (RISEPel).  It is
		//  PREVIEW-GRADE: three RGB point-samples cannot represent an
		//  interference integral, which is exactly why this offline
		//  pre-integration replaces a naive per-channel evaluation.  The
		//  SPECTRAL path (ReflectanceConductor per hero wavelength) carries
		//  no such approximation and is the authoritative result.
		//
		//  WHITE NORMALIZATION (the §8 subtlety that bites colour-space
		//  work).  The renderer's working space (RISEPel == Rec.709) has a
		//  D65 whitepoint, so "R(λ)≡1 → neutral RGB(1,1,1)" requires the
		//  flat reflector to integrate to the D65 white point — NOT the
		//  equal-energy point E that a bare Σ R·cmf / Σ ȳ would give (E maps
		//  to a BLUISH-TINTED Rec.709 triple, the bug an illuminant-naive
		//  normaliser introduces).  We therefore (1) form the equal-energy
		//  relative tristimulus Σ R·cmf / Σ cmf per channel (so R≡1 → E),
		//  then (2) von-Kries scale E → the matrix's exact D65 white
		//  Rec709RGBtoXYZ(1,1,1) (so R≡1 → D65 white → neutral RGB).  The
		//  scene illuminant never enters — the basis is reflectance, the
		//  renderer multiplies incident RGB radiance afterwards.
		//
		//  Per the locked design (§7/§13) this integral is the GENERATOR for
		//  a per-material (cosθ × thickness) 2D LUT; the LUT itself is a
		//  documented follow-up (it requires the substrate/film complex
		//  indices to be CONSTANT across the material, which cannot be
		//  guaranteed at the GGX layer because they arrive as possibly
		//  spatially-varying IScalarPainters).  Calling this per shade is
		//  always correct regardless of spatial variation.

		//! Number of wavelength strata for the RGB albedo-basis integral.
		//! 32 uniform samples across the visible band: above the Nyquist
		//! limit of the interference fringes for the shipped oxide thickness
		//! range (a 250 nm, n≈2.5 film has fringe spacing ~tens of nm), and
		//! a fixed compile-time count keeps the hot loop allocation-free.
		static const int kRGBIntegrationSamples = 32;

		//! RGB albedo-basis reflectance of an ambient / single-film /
		//! substrate stack at one half-vector cosine (the §8 preview
		//! integration).  See the block comment above for the convention.
		//!
		//! \param cosThetaI    cosine of the angle between the incident
		//!                     direction and the microfacet half-vector,
		//!                     in [0,1] (NOT the geometric-normal cosine) —
		//!                     identical to ReflectanceConductor.
		//! \param thickness_nm physical film thickness, nm (λ-independent).
		//! \param stackAt      per-λ optical-constant functor (see below).
		//! \return             linear Rec.709 reflectance (RISEPel); a
		//!                     perfect reflector → neutral (1,1,1).
		//!
		//! Allocation-free; kRGBIntegrationSamples Airy evaluations + CMF
		//! lookups.  Only ever called on the RGB preview path of a thin-film
		//! GGX material, so it adds zero cost to existing render paths.
		//!
		//! Per-wavelength generalisation of ReflectanceConductorRGB: the
		//! ambient / film / substrate complex indices are REBUILT INSIDE the
		//! λ loop from the caller's `stackAt` functor, so a wavelength-VARYING
		//! (file-based, e.g. Ti / TiO2) optical-constant set is honoured at
		//! EACH integration wavelength — the dispersion-correct RGB preview.
		//!
		//! `stackAt` has signature
		//!   void(Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1,
		//!        Scalar& n2, Scalar& k2)
		//! and writes the air(n0,k0) / film(n1,k1) / substrate(n2,k2) real
		//! index + extinction at the given wavelength.  Thickness is
		//! λ-independent and passed directly.
		//!
		//! ThinFilm.h stays optics-only: the functor (which closes over the
		//! IScalarPainters on the BSDF side) is the template boundary, so no
		//! painter type ever enters this header.
		//!
		//! Everything except the per-λ index construction (band, step, CMF
		//! lookup, von-Kries white normalisation, gamut clamp) is identical to
		//! ReflectanceConductorRGB; for a CONSTANT stack (stackAt returns the
		//! same n,k every wavelength) the result is BIT-IDENTICAL to the
		//! former constant-index implementation — the only change is that the
		//! loop-invariant N0/N1/Ns/sinInv are now rebuilt (to identical
		//! values) inside the loop.
		template<class StackFn>
		inline RISEPel ReflectanceConductorRGBSpectral(
			Scalar cosThetaI,
			Scalar thickness_nm,
			const StackFn& stackAt )
		{
			// Integration band matches the renderer's CIE table support.
			const Scalar loNm = Scalar( 380 );
			const Scalar hiNm = Scalar( 780 );
			const Scalar step = ( hiNm - loNm ) / Scalar( kRGBIntegrationSamples );

			// Numerator Σ R·cmf·Δλ and the PER-CHANNEL equal-energy basis
			// sums Σ cmf·Δλ.  Sharing the SAME midpoint quadrature for both
			// means a flat reflector R≡1 lands EXACTLY on the equal-energy
			// white E (Xn/Xe = Yn/Ye = Zn/Ze = 1) to quadrature error, with
			// no normaliser mismatch.
			Scalar Xn = Scalar( 0 ), Yn = Scalar( 0 ), Zn = Scalar( 0 );
			Scalar Xe = Scalar( 0 ), Ye = Scalar( 0 ), Ze = Scalar( 0 );

			for( int s = 0; s < kRGBIntegrationSamples; ++s ) {
				const Scalar nm = loNm + ( Scalar( s ) + Scalar( 0.5 ) ) * step;

				XYZPel cmf;
				if( !ColorUtils::XYZFromNM( cmf, nm ) ) {
					continue;
				}

				// Per-λ optical constants (the dispersion-correct step): for a
				// constant stack these are the same every iteration, so the
				// indices and Snell invariant below are bit-identical to the
				// old loop-invariant construction.
				Scalar n0, k0, n1, k1, n2, k2;
				stackAt( nm, n0, k0, n1, k1, n2, k2 );

				const Complex N0 = detail::MakeIndex( n0, k0 );
				const Complex N1 = detail::MakeIndex( n1, k1 );
				const Complex Ns = detail::MakeIndex( n2, k2 );
				const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

				// Goes through SingleFilmReflectanceForPol so the RGB path shares
				// the scalar path's defence-in-depth fallback.  The degeneracy
				// itself is handled inside the FACTORED Airy form; this call site
				// matters because a per-wavelength non-finite value would pass the
				// [0,1] clamp below (NaN<0 and NaN>1 are both false) and poison the
				// whole Xn/Yn/Zn accumulation.
				const Scalar Rs = detail::SingleFilmReflectanceForPol(
					N0, N1, Ns, thickness_nm, nm, sinInv, ePolS );
				const Scalar Rp = detail::SingleFilmReflectanceForPol(
					N0, N1, Ns, thickness_nm, nm, sinInv, ePolP );
				Scalar R = Scalar( 0.5 ) * ( Rs + Rp );
				if( R < Scalar( 0 ) ) R = Scalar( 0 );
				if( R > Scalar( 1 ) ) R = Scalar( 1 );

				Xn += R * cmf.X * step;  Yn += R * cmf.Y * step;  Zn += R * cmf.Z * step;
				Xe += cmf.X * step;      Ye += cmf.Y * step;      Ze += cmf.Z * step;
			}

			// Equal-energy relative tristimulus: R≡1 → (1,1,1) (white E).
			const Scalar relX = ( Xe > Scalar( 0 ) ) ? Xn / Xe : Scalar( 0 );
			const Scalar relY = ( Ye > Scalar( 0 ) ) ? Yn / Ye : Scalar( 0 );
			const Scalar relZ = ( Ze > Scalar( 0 ) ) ? Zn / Ze : Scalar( 0 );

			// von-Kries scale E → the matrix's exact D65 white so that R≡1
			// maps to neutral Rec.709.  whiteXYZ = Rec709RGBtoXYZ(1,1,1) is
			// the D65 whitepoint baked into the conversion matrix, so the
			// neutrality holds to the matrix's own precision (not an
			// approximate literal).
			const Rec709RGBPel whiteRGB( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) );
			const XYZPel whiteXYZ = ColorUtils::Rec709RGBtoXYZ( whiteRGB );

			XYZPel xyz;
			xyz.X = relX * whiteXYZ.X;
			xyz.Y = relY * whiteXYZ.Y;
			xyz.Z = relZ * whiteXYZ.Z;

			// XYZ → linear Rec.709 (RISEPel).  XYZtoRec709RGB gamut-maps,
			// keeping the preview displayable; a passive R(λ)∈[0,1] stack
			// stays inside or near the gamut so the clip is negligible.
			RISEPel rgb = ColorUtils::XYZtoRec709RGB( xyz );

			// Defend against a hair of negative from gamut mapping / FP;
			// reflectance is physically non-negative.
			if( rgb.r < Scalar( 0 ) ) rgb.r = Scalar( 0 );
			if( rgb.g < Scalar( 0 ) ) rgb.g = Scalar( 0 );
			if( rgb.b < Scalar( 0 ) ) rgb.b = Scalar( 0 );
			return rgb;
		}

		//! Constant-stack RGB albedo-basis reflectance.  THIN WRAPPER over
		//! ReflectanceConductorRGBSpectral with a constant-returning functor —
		//! bit-identical to the former standalone implementation (the indices
		//! are simply rebuilt to the same values inside the λ loop).  Kept as
		//! the convenience entry point for callers with constant n,k and for
		//! the bit-identity regression (ThinFilmRGBSpectralTest test c).
		inline RISEPel ReflectanceConductorRGB(
			Scalar cosThetaI,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			return ReflectanceConductorRGBSpectral(
				cosThetaI, thickness_nm,
				[&]( Scalar /*nm*/, Scalar& on0, Scalar& ok0, Scalar& on1, Scalar& ok1, Scalar& on2, Scalar& ok2 ) {
					on0 = n0; ok0 = k0;
					on1 = n1; ok1 = k1;
					on2 = n2; ok2 = k2;
				} );
		}
		
		//! Hemispherical Fresnel average  F_avg = 2 * integral_0^1 R(mu) mu d_mu
		//! of the air/film/substrate stack at one wavelength.  Feeds the
		//! Kulla-Conty multiscatter tail of the thin-film GGX mode: the film
		//! shifts the average reflectance by up to ~0.5 vs the bare substrate
		//! (tests/ThinFilmFurnaceTest.cpp), so the substrate average is unusable
		//! there.  Uses the SAME 21-point Gauss-Legendre rule as the conductor's
		//! MicrofacetEnergyLUT::ComputeFresnelAvg (same GL_nodes / GL_weights /
		//! 2*mu*w weighting), so at film==air this average is byte-identical to
		//! the conductor's — the additive invariant (ThinFilmBRDFTest Test D)
		//! holds in the multiscatter tail, not just the single-scatter lobe.
		//! Gauss-Legendre is also more accurate than the old midpoint rule
		//! (matches the 256-pt reference in ThinFilmFurnaceTest Gate 2 to
		//! < 2e-3).  A per-material (nm x thickness) LUT is the deferred
		//! optimisation for spatially-constant stacks; this per-shade quadrature
		//! is always correct.  docs/THIN_FILM_INTERFERENCE.md section 7.
		inline Scalar FresnelAvgConductor(
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			Scalar sum = Scalar( 0 );
			for( int i = 0; i < MicrofacetEnergyLUT::GL_N; ++i ) {
				const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
				sum += ReflectanceConductor( mu, wavelength_nm, n0, k0, n1, k1, thickness_nm, n2, k2 )
					* ( Scalar( 2 ) * mu * MicrofacetEnergyLUT::GL_weights[i] );
			}
			return sum;
		}
		
		//! RGB (no-wavelength) hemispherical Fresnel average for the thin-film
		//! multiscatter tail.  PREVIEW-grade and the costliest thin-film call
		//! (GL_N angles x the internal kRGBIntegrationSamples-lambda CMF
		//! integral); the K-C tail is a high-roughness-only correction.  Uses
		//! the SAME 21-point Gauss-Legendre rule as the spectral
		//! FresnelAvgConductor above (and the conductor ComputeFresnelAvg).
		//! Per-wavelength generalisation of FresnelAvgConductorRGB: the GL-angle
		//! hemispherical average whose inner RGB reflectance is the
		//! dispersion-correct ReflectanceConductorRGBSpectral (per-λ indices
		//! from `stackAt`).  Same 21-point Gauss-Legendre rule / `2*mu*weight`
		//! weighting as the constant variant.  For a constant stack this is
		//! BIT-IDENTICAL to FresnelAvgConductorRGB (the inner reflectance is
		//! bit-identical at every node).  Thickness is λ-independent.
		template<class StackFn>
		inline RISEPel FresnelAvgConductorRGBSpectral(
			Scalar thickness_nm,
			const StackFn& stackAt )
		{
			RISEPel sum( Scalar( 0 ), Scalar( 0 ), Scalar( 0 ) );
			for( int i = 0; i < MicrofacetEnergyLUT::GL_N; ++i ) {
				const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
				sum = sum + ReflectanceConductorRGBSpectral( mu, thickness_nm, stackAt )
					* ( Scalar( 2 ) * mu * MicrofacetEnergyLUT::GL_weights[i] );
			}
			return sum;
		}

		//! Constant-stack RGB hemispherical Fresnel average.  THIN WRAPPER over
		//! FresnelAvgConductorRGBSpectral with a constant-returning functor —
		//! bit-identical to the former standalone implementation.
		inline RISEPel FresnelAvgConductorRGB(
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			return FresnelAvgConductorRGBSpectral(
				thickness_nm,
				[&]( Scalar /*nm*/, Scalar& on0, Scalar& ok0, Scalar& on1, Scalar& ok1, Scalar& on2, Scalar& ok2 ) {
					on0 = n0; ok0 = k0;
					on1 = n1; ok1 = k1;
					on2 = n2; ok2 = k2;
				} );
		}
	}
}

#endif
