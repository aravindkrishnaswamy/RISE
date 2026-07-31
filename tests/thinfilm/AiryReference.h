//////////////////////////////////////////////////////////////////////
//
//  AiryReference.h - Single-film Airy-summation reference for thin-film
//    reflectance.  Closed-form cross-check against the N-layer TMM
//    (TmmReference.h).  Ground-truth oracle for the thin-film feature
//    (docs/THIN_FILM_INTERFERENCE.md §5).  Header-only,
//    std::complex<double>, NO renderer dependency.
//
//    The Airy result for ambient(0) / film(1) / substrate(s):
//
//      r = ( r01 + r1s e^{+2iδ1} ) / ( 1 + r01 r1s e^{+2iδ1} )
//      R = |r|^2
//
//    with the per-polarization Fresnel amplitude coefficients
//
//      r_ab = (η_a - η_b) / (η_a + η_b)
//
//    and η_s = N cosθ, η_p = N / cosθ, δ1 = (2π/λ) N1 d1 cosθ1.
//
//    Those coefficients are NOT evaluated as written.  This file keeps each
//    interface as a cleared (numerator, denominator) PAIR via
//    detail::InterfaceTerms and factors the vanishing 2*N1*cos1 out of the
//    quotient -- see the DEGENERACY note below.  The cleared ratio is:
//      s: (Na cosa - Nb cosb)/(Na cosa + Nb cosb)
//      p: (Na cosb - Nb cosa)/(Na cosb + Nb cosa)
//    Algebraically identical, but finite when an ENDPOINT cosθ (ambient or
//    substrate) is exactly 0, where it gives |r| = 1 instead of
//    Inf/Inf = NaN.
//
//    DEGENERACY, and why this file does NOT divide at each interface first:
//    when the FILM's own cosθ is exactly 0 (the film at its own critical
//    angle; also exactly grazing when the film index equals the ambient's)
//    the textbook quotient (r01 + r1s φ)/(1 + r01 r1s φ) is 0/0 -- r01 == ∓1
//    exactly and δ == 0, so both sides vanish.  Dividing at each interface
//    destroys the common 2·N1·cos1 factor that makes the limit well defined,
//    and the failure is SILENT rather than loud: with any absorbing medium
//    below the film the two vanishing quantities are equal-but-nonzero, so
//    the quotient returns exactly 1 (a perfect mirror).  This file therefore
//    keeps the interfaces in cleared (numerator, denominator) form and
//    factors 2·N1·cos1 out analytically.  Pinned by ThinFilmTMMTest [8/8](b)
//    on ABSORBING substrates.
//
//    SIGN CONVENTION: e^{-iωt} time dependence (Born & Wolf / Macleod).
//    cosθ is the forward-travelling root -- the DECAYING one, so Im(δ) >= 0
//    in EVERY medium, not just an absorbing film -- and the round-trip
//    phase factor e^{+2iδ1} therefore has modulus <= 1 and decays with
//    thickness: a thick absorbing film correctly tends to the bare
//    top-interface reflectance r01, and an evanescent film (a frustrated-TIR
//    gap) tends to R = 1.  That invariant only became TRUE on 2026-07-30:
//    before then PickForwardCos tested Re(η) first with a tie-break that
//    never fired, so for an evanescent film it selected the GROWING root and
//    e^{+2iδ1} diverged (onset d ~ 4 µm).  See TmmReference.h
//    PickForwardCos.  (docs/THIN_FILM_INTERFERENCE.md §5 USED TO write the
//    e^{-2iδ} form, which belongs to the opposite phase convention and GROWS
//    for absorbing films; it was reconciled with this file on 2026-07-30 and
//    now writes e^{+2iδ}, naming e^{-2iδ} only as the wrong pairing to avoid.
//    See the TmmReference.h header for the full discussion.)
//
//    IMPORTANT: this file deliberately reuses the SAME cosθ-branch and
//    admittance helpers as TmmReference.h (detail::CosThetaInMedium,
//    detail::InterfaceTerms, detail::ExpM1OverZ) so the two implementations cannot drift apart
//    in their conventions -- the whole value of the Airy<->TMM agreement
//    test is that they share conventions but compute by different
//    algebra.  Only the e^{+2iδ} sign convention is restated here; it
//    matches the TMM layer matrix (which uses the same δ and the matching
//    -i sinδ off-diagonal sign).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINFILM_AIRYREFERENCE_H
#define THINFILM_AIRYREFERENCE_H

#include <complex>
#include <cmath>
#include <cassert>

#include "ThinFilmStack.h"
#include "TmmReference.h"		// reuse detail::CosThetaInMedium / InterfaceTerms / ExpM1OverZ / Polarization

namespace RISE
{
	namespace ThinFilmReference
	{
		//! Single-film Airy reflectance for one polarization.  `stack`
		//! must contain exactly one film (the substrate and ambient are
		//! the bounding media).  Conventions identical to TmmReference.h.
		inline double AiryReflectanceForPol(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad,
			Polarization pol )
		{
			using namespace detail;

			// The Airy closed form is single-film only (the cross-check
			// partner of the N-layer TMM).  Misuse with 0 or >1 films is a
			// caller error; fail loudly in debug builds.
			assert( stack.films.size() == 1 && "AiryReflectance requires exactly one film" );

			const Complex N0 = stack.ambientIndex;
			const Complex N1 = stack.films[0].index;
			const Complex Ns = stack.substrateIndex;
			const double  d1 = stack.films[0].thickness_nm;

			// Snell invariant s = N0 sinθ0, shared across interfaces.
			const Complex sinInvariant = N0 * std::sin( thetaI_rad );

			const Complex cos0 = CosThetaInMedium( N0, sinInvariant );
			const Complex cos1 = CosThetaInMedium( N1, sinInvariant );
			const Complex cosS = CosThetaInMedium( Ns, sinInvariant );


			// Airy summation with the vanishing cos_film FACTORED OUT rather
			// than divided away.  Dividing at each interface first (the
			// textbook r01/r1s form) destroys the common 2 N1 cos1 factor and
			// makes the quotient 0/0 when the FILM is at its own critical
			// angle -- and for any absorbing medium below the film the two
			// vanishing quantities are equal-but-nonzero, so it silently
			// returns exactly 1 (a perfect mirror) rather than NaN.
			// With (a1,b1) ambient<->film, (a2,b2) film<->substrate, (p,q)
			// ambient<->substrate, and E(z) = (e^z - 1)/z at z = 2i*delta:
			//   num = 2 N1 cos1 [ (p-q) + i kd (a2-b2)(a1+b1) E ]
			//   den = 2 N1 cos1 [ (p+q) + i kd (a1-b1)(a2-b2) E ]
			// The 2 N1 cos1 cancels exactly.
			Complex a1, b1, a2, b2, p, q;
			InterfaceTerms( N0, cos0, N1, cos1, pol, a1, b1 );
			InterfaceTerms( N1, cos1, Ns, cosS, pol, a2, b2 );
			InterfaceTerms( N0, cos0, Ns, cosS, pol, p,  q  );

			const double  kd    = PhaseCoefficient( d1, lambda_nm );
			const Complex delta = Complex( kd, 0.0 ) * N1 * cos1;
			const Complex E     = ExpM1OverZ( Complex( 0.0, 2.0 ) * delta );
			const Complex ikd( 0.0, kd );

			const Complex num = ( p - q ) + ikd * ( a2 - b2 ) * ( a1 + b1 ) * E;
			const Complex den = ( p + q ) + ikd * ( a1 - b1 ) * ( a2 - b2 ) * E;
			return std::norm( num / den );
		}

		//! s, p, and unpolarized single-film Airy reflectance.
		inline ReflectanceResult AiryReflectance(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad )
		{
			ReflectanceResult out;
			out.Rs = AiryReflectanceForPol( stack, lambda_nm, thetaI_rad, ePolS );
			out.Rp = AiryReflectanceForPol( stack, lambda_nm, thetaI_rad, ePolP );
			return out;
		}

		//! Unpolarized single-film Airy reflectance R = 1/2 (Rs + Rp).
		inline double AiryReflectanceUnpolarized(
			const Stack& stack,
			double lambda_nm,
			double thetaI_rad )
		{
			return AiryReflectance( stack, lambda_nm, thetaI_rad ).Unpolarized();
		}
	}
}

#endif
