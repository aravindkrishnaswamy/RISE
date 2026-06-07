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
//    SIGN CONVENTION: e^{-iωt} time dependence (Born & Wolf / Macleod).
//    cosθ is the forward-travelling root (Im(δ1) >= 0 for an absorbing
//    film), so the round-trip phase factor e^{+2iδ1} DECAYS with
//    thickness -- a thick absorbing film correctly tends to the bare
//    top-interface reflectance r01.  The e^{-2iδ} form written in the
//    design doc belongs to the opposite phase convention and GROWS for
//    absorbing films; see TmmReference.h header for the full discussion.
//
//    IMPORTANT: this file deliberately reuses the SAME cosθ-branch and
//    admittance helpers as TmmReference.h (detail::CosThetaInMedium,
//    detail::Admittance) so the two implementations cannot drift apart
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
#include "TmmReference.h"		// reuse detail::CosThetaInMedium / Admittance / Polarization

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

			const Complex eta0 = Admittance( N0, cos0, pol );
			const Complex eta1 = Admittance( N1, cos1, pol );
			const Complex etaS = Admittance( Ns, cosS, pol );

			// Fresnel amplitude reflection coefficients at each interface,
			// in admittance form r_ab = (η_a - η_b)/(η_a + η_b).
			const Complex r01 = ( eta0 - eta1 ) / ( eta0 + eta1 );
			const Complex r1s = ( eta1 - etaS ) / ( eta1 + etaS );

			// Phase thickness δ1 = (2π/λ) N1 d1 cosθ1 (matches the TMM
			// layer matrix).
			const double twoPi = 2.0 * 3.14159265358979323846;
			const Complex delta = Complex( twoPi * d1 / lambda_nm, 0.0 ) * N1 * cos1;

			// e^{+2iδ1} (forward-decaying round-trip phase; see header).
			const Complex i( 0.0, 1.0 );
			const Complex phase = std::exp( 2.0 * i * delta );

			const Complex r = ( r01 + r1s * phase ) / ( Complex( 1.0, 0.0 ) + r01 * r1s * phase );
			return std::norm( r );
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
