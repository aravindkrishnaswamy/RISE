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
//        InterfaceReflection (the same ratio with the cosθ factors
//        cleared, so it stays finite when a cosθ is exactly 0).
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
			//! coefficients now go through InterfaceReflection and the
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
			inline Scalar AiryReflectanceForPol(
				const Complex& N0, const Complex& N1, const Complex& Ns,
				Scalar thickness_nm, Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Complex cos0 = CosThetaInMedium( N0, sinInvariant );
				const Complex cos1 = CosThetaInMedium( N1, sinInvariant );
				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );

				// Fresnel amplitude reflection coefficients at each interface.
				// InterfaceReflection is the admittance ratio (η_a - η_b)/(η_a + η_b)
				// with the cosθ factors cleared, so a medium sitting exactly at the
				// critical angle (cosθ = 0, infinite η_p) gives |r| = 1 instead of NaN.
				const Complex r01 = InterfaceReflection( N0, cos0, N1, cos1, pol );
				const Complex r1s = InterfaceReflection( N1, cos1, Ns, cosS, pol );

				// Phase thickness δ1 = (2π/λ) N1 d1 cosθ1.
				const Complex delta = Complex( TWO_PI * thickness_nm / lambda_nm, Scalar(0) ) * N1 * cos1;

				// e^{+2iδ1}: forward-decaying round-trip phase (see header).
				const Complex i( Scalar(0), Scalar(1) );
				const Complex phase = std::exp( Scalar(2) * i * delta );

				const Complex r = ( r01 + r1s * phase ) /
					( Complex( Scalar(1), Scalar(0) ) + r01 * r1s * phase );
				return std::norm( r );		// |r|²
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
					// KNOWN RESIDUAL (2026-07-29, characterisation CORRECTED
					// 2026-07-30 after adversarial review -- the earlier
					// version of this comment was wrong in four ways):
					//
					// A medium OTHER than the ambient/substrate endpoints
					// having cos == 0 exactly still yields NaN.  That happens
					// when a FILM sits at ITS OWN critical angle, and also at
					// EXACTLY grazing whenever a film index equals the
					// ambient's.
					//
					// SCOPE -- BOTH production paths are affected, not just
					// this one.  ReflectanceConductor (the single-film Airy
					// form that GGX `fresnel_mode thinfilm` calls) NaNs on the
					// same geometry, by a DIFFERENT mechanism: at cos1 == 0,
					// InterfaceReflection gives r01 == -r1s exactly and
					// delta == 0, so the Airy quotient below is 0/0.  The NaN
					// then survives the [0,1] guards in ReflectanceConductor
					// (NaN < 0 and NaN > 1 are both false) and reaches the
					// BRDF; through ReflectanceConductorRGBSpectral one bad
					// wavelength poisons the whole XYZ accumulation.
					//
					// MECHANISM here is polarization-dependent: for p, etaj is
					// infinite (N/0); for s, etaj is ZERO (N*0) and the NaN is
					// a01 = -i*sinD/etaj = 0/0 below.  Pre-scaling etaj is
					// therefore a NO-OP for s and cannot fix half the symptom.
					//
					// FIX (validated by review, not yet applied): write the
					// off-diagonals as products with sinc(delta) = sin(delta)/
					// delta rather than quotients -- delta carries exactly one
					// factor of cos, so delta/eta and delta*eta are both
					// finite for both polarizations at cos == 0, with no
					// division by cos and NO running scale factor (the
					// "running scale factor" the earlier comment claimed was
					// required is not).  Measured: matches the current form to
					// 8.9e-16 and makes the degenerate geometries finite AND
					// continuous with their two-sided limits.
					//
					// Reachability is narrow (a nextafter scan found 3 NaN in
					// 8001 consecutive cosI doubles around the critical value)
					// but nonzero, and ambientIOR is not pinned to 1 for
					// nested dielectrics.  No test exercises the geometry yet.
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

			//! Single-film reflectance for one polarization: the Airy closed
			//! form, falling back to the 1-layer sinc TMM where Airy is
			//! UNDEFINED.
			//!
			//! A threshold-free pairing of two EXACT forms, not a fudge.
			//! Their failure modes are disjoint and each covers the other's
			//! (both measured):
			//!
			//!   * Airy is 0/0 at EXACTLY the film's own critical angle
			//!     (cos_film == 0 makes r01 == -r1s and delta == 0; the
			//!     cancellation is structural, so no factoring removes it).
			//!     It is otherwise accurate right up to that point --
			//!     agreement with the TMM is 1e-16..1e-11 for cos offsets
			//!     from 1e-2 down to 1e-14 -- so there is no wide
			//!     ill-conditioned band that would need a threshold.
			//!   * The TMM layer matrix grows like e^{|Im delta|}, so it
			//!     overflows on thick strongly-absorbing films (measured NaN
			//!     from d ~ 2e4 nm at k = 3) where Airy stays finite past
			//!     1e6 nm.  So the TMM must NOT become the primary form.
			//!
			//! Hence: try Airy; if it did not produce a finite value the
			//! geometry is the degenerate one, where the TMM is defined.
			//!
			//! NOTE std::isfinite is load-bearing here and WORKS -- macOS
			//! pairs -fno-finite-math-only since 2026-07-29.  Under the old
			//! bare -ffast-math it would have been folded to `true` and this
			//! fallback would have been DEAD CODE.
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

				const Scalar Rs = detail::AiryReflectanceForPol(
					N0, N1, Ns, thickness_nm, nm, sinInv, ePolS );
				const Scalar Rp = detail::AiryReflectanceForPol(
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
