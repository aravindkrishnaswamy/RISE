//////////////////////////////////////////////////////////////////////
//
//  ThinFilm.h - Header-only, inline, allocation-free thin-film
//    interference reflectance (single-film Airy + N-layer Abelès TMM)
//    in RISE Scalar (double) and std::complex<Scalar>.
//    Reference: docs/THIN_FILM_INTERFERENCE.md (§5, §7, §8).
//
//    PHYSICS / SIGN CONVENTION:
//      * Passive complex index N = n + i*k with n >= 0, k >= 0;
//        time dependence e^{-iωt} (Born & Wolf / Macleod).
//      * Forward-travelling cosθ root: decaying branch Im(N cosθ) > 0,
//        tie-broken by Re(N cosθ) > 0 for lossless propagating angles.
//        Testing Im FIRST is load-bearing (see PickForwardCos).
//      * Tilted admittance:  η_s = N cosθ ;  η_p = N / cosθ.
//      * Phase thickness:  δ = (2π/λ) N d cosθ with Im(δ) >= 0, so
//        every exponential e^{+2iδ} has modulus <= 1 (overflow-free).
//      * Single-film Airy:
//          r = ( r01 + r1s e^{+2iδ1} ) / ( 1 + r01 r1s e^{+2iδ1} )
//        evaluated in factored cleared form (InterfaceTerms + ExpM1OverZ)
//        to cancel vanishing 2*N1*cos1 analytically.  The +2iδ1 sign
//        ensures thick absorbing films decay to r01.
//      * N-layer characteristic (Abelès) matrix:
//          Mj = [[ cosδj, -i sinδj/ηj ], [ -i ηj sinδj, cosδj ]]
//
//    UNITS: wavelength and film thickness are both in nanometres (nm)
//    (only their ratio enters δ).
//
//    cosThetaI CONVENTION: cosine between the incident direction and
//    the MICROFACET HALF-VECTOR dot(wi, h) in [0,1] — NOT the
//    geometric surface normal.
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
#include "MicrofacetEnergyLUT.h"	// Kulla-Conty energy helpers
#include "GaussLegendreQuadrature.h"

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
			//! Stand-in substituted for an exact zero index (e.g. a black texel
			//! from an unvalidated `IScalarPainter`).  Folding negative components
			//! to `|n| + i|k|` enforces passive media (negative n or k amplifies
			//! and saturates the [0,1] clamp).  For `N == 0`, substituting a tiny
			//! stand-in lets the ordinary math compute the true evanescent-barrier
			//! limit in all three roles (ambient, film, substrate) without dividing
			//! by zero or inventing a wrong constant (e.g. R=1 fails for an
			//! absorbing film over a zero substrate).
			//!
			//! `1e-40` sits inside the usable window `[~1e-76, ~1e-16]` for
			//! physical optical constants.  Residual under `-ffast-math` naive
			//! complex division: overflows below `~1e-154` (Airy overflows below
			//! `~1e-77.5`, where the TMM fallback takes over) or above `~1e153.7`.
			//! Pinned by `ThinFilmProductionTest [Domain]`.
			static const Scalar kZeroIndexStandIn = Scalar(1e-40);

			//! True iff N has a non-finite component (rejected by public entry points).
			inline bool IsNonFiniteIndex( const Complex& N )
			{
				return !std::isfinite( N.real() ) || !std::isfinite( N.imag() );
			}

			//! Fallback reflectance for non-finite index inputs: stays in `[0,1]`
			//! and `> 0` so `GGXBRDF`'s `if(Rfilm > 0)` gate does not go silently
			//! black.  Engineering choice (not the exact infinite-substrate limit
			//! under an absorbing film).
			static const Scalar kNonFiniteStackReflectance = Scalar(1);

			//! Folds N onto the passive convention `|n| + i|k|`, replacing exact
			//! zero with `kZeroIndexStandIn`.  Caller must reject non-finite inputs
			//! via `IsNonFiniteIndex` first.
			inline Complex PhysicalIndex( const Complex& N )
			{
				const Scalar n = std::fabs( N.real() );
				const Scalar k = std::fabs( N.imag() );
				if( n == Scalar(0) && k == Scalar(0) ) {
					return Complex( kZeroIndexStandIn, Scalar(0) );
				}
				return Complex( n, k );
			}

			//! Builds a passive complex index from `(n, k)` via `PhysicalIndex`.
			inline Complex MakeIndex( Scalar n, Scalar k )
			{
				return PhysicalIndex( Complex( n, k ) );
			}

			//! Selects the decaying forward-travelling cosθ root: `Im(η) > 0`,
			//! tie-broken by `Re(η) > 0` where `η = N cosθ`.
			//!
			//! For a real Snell invariant `s`, `Im(η²) = 2nk >= 0` keeps `η` in
			//! quadrant I/III so Re and Im agree off-axis.  Testing Im FIRST is
			//! load-bearing and portable: `Im(η) == 0` is exact in real arithmetic
			//! for lossless propagating angles, whereas `std::sqrt` of a negative
			//! real in polar form leaves a ~`6e-17` positive Re residue, causing a
			//! Re-first rule to silently accept the growing evanescent root.
			//!
			//! Domain: passive stack with non-absorbing ambient (`k0 == 0`).  An
			//! absorbing ambient (`k0 > 0`) produces an inhomogeneous incident wave
			//! where unclamped R can exceed 1; decaying-first stays overflow-free
			//! and public entry points clamp to 1.
			inline Complex PickForwardCos( const Complex& N, const Complex& cosCandidate )
			{
				const Complex eta = N * cosCandidate;
				// Keep as literal disjunction (predicates better than ternary select);
				// `>` vs `>=` only differs at `eta == 0` where both roots are 0.
				const bool forward =
					( eta.imag() > Scalar(0) ) ||
					( eta.imag() == Scalar(0) && eta.real() > Scalar(0) );
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



			//! `sin(z)/z` with removable singularity at `z = 0` evaluated by Taylor
			//! series below `|z| = 1e-4` (exact to double precision).  Used by
			//! `ExpM1OverZ` via `sinh(w)/w = Sinc(i*w)`.
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

			//! Snell invariant `s = N0 sinθ0` via `sqrt(1 - c²)` with `cosThetaI`
			//! clamped to `[kGrazingCosFloor, 1]`.  The `1e-6` floor stays above
			//! `sqrt(eps_double) ~ 1.5e-8` so `1 - c²` does not round to `1.0`
			//! and collapse near-grazing cosines to exact grazing.
			static const Scalar kGrazingCosFloor = Scalar(1e-6);

			inline Complex SnellInvariant( const Complex& N0, Scalar cosThetaI )
			{
				// Negated comparison also clamps NaN to kGrazingCosFloor.
				Scalar c = cosThetaI;
				if( !( c > kGrazingCosFloor ) ) c = kGrazingCosFloor;
				if( c > Scalar(1) )             c = Scalar(1);
				const Scalar s0 = std::sqrt( Scalar(1) - c * c );
				return N0 * s0;
			}

			//! Phase coefficient `kd = 2π*d/λ`; non-positive or non-finite `kd`
			//! (including `d <= 0`, `λ <= 0`, `±inf`, `NaN`) maps to `0` (absent
			//! layer).  Load-bearing: `Im(δ) = kd * Im(N cosθ) >= 0 <=> kd >= 0`,
			//! which guarantees `|e^{+2iδ}| <= 1` and prevents divergence.
			//! Pinned by `ThinFilmProductionTest [Domain]`.
			inline Scalar PhaseCoefficient( Scalar thickness_nm, Scalar lambda_nm )
			{
				const Scalar kd = TWO_PI * thickness_nm / lambda_nm;
				return ( kd > Scalar(0) && std::isfinite( kd ) ) ? kd : Scalar(0);
			}

			//! The CLEARED (numerator, denominator) pair of an a -> b
			//! interface; the amplitude ratio is (a - b)/(a + b) of these.
			//! Callers keep the PAIR rather than the ratio so that a vanishing
			//! cos can be factored out BEFORE dividing -- dividing first
			//! destroys the common factor that makes the limit well defined.
			//! See AiryReflectanceForPol.
			//!
			//!   s: (Na cosa - Nb cosb)/(Na cosa + Nb cosb)
			//!   p: (Na cosb - Nb cosa)/(Na cosb + Nb cosa)
			inline void InterfaceTerms(
				const Complex& Na, const Complex& cosa,
				const Complex& Nb, const Complex& cosb,
				Polarization pol, Complex& a, Complex& b )
			{
				a = ( pol == ePolS ) ? Na * cosa : Na * cosb;
				b = ( pol == ePolS ) ? Nb * cosb : Nb * cosa;
			}

			//! Entire function `(e^z - 1)/z` (limit 1 at `z = 0`).
			//!  - `|z| > 1`: direct `(exp(z) - 1)/z`.  Callers pass `z = 2i*δ` with
			//!    `Re(z) = -2*Im(δ) <= 0`, so thick absorbing/evanescent layers decay
			//!    smoothly to `-1/z` (the `sinh` identity would form `0 * inf = NaN`).
			//!  - `|z| <= 1`: cancellation-free `e^{z/2} * Sinc(i*z/2)`.
			//! Branches agree to ~`1e-16` at `|z| = 1`; pinned by `ThinFilmProductionTest [Helpers], [Thick]`.
			inline Complex ExpM1OverZ( const Complex& z )
			{
				if( std::abs( z ) > Scalar(1) ) {
					return ( std::exp( z ) - Complex( Scalar(1), Scalar(0) ) ) / z;
				}
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

				// Factored Airy form using cleared interface pairs (a1,b1) for 0->1,
				// (a2,b2) for 1->s, and (p,q) for 0->s with 2*N1*cos1 cancelled
				// analytically.  Avoids the textbook quotient's 0/0 (which silently
				// yielded R=1 on absorbing substrates) when cos1 == 0:
				//   num = (p - q) + i*kd*(a2 - b2)*(a1 + b1)*E
				//   den = (p + q) + i*kd*(a1 - b1)*(a2 - b2)*E
				Complex a1, b1, a2, b2, p, q;
				InterfaceTerms( N0, cos0, N1, cos1, pol, a1, b1 );
				InterfaceTerms( N1, cos1, Ns, cosS, pol, a2, b2 );
				InterfaceTerms( N0, cos0, Ns, cosS, pol, p,  q  );

				const Scalar  kd    = PhaseCoefficient( thickness_nm, lambda_nm );
				const Complex delta = Complex( kd, Scalar(0) ) * N1 * cos1;
				const Complex E     = ExpM1OverZ( Complex( Scalar(0), Scalar(2) ) * delta );
				const Complex ikd( Scalar(0), kd );

				const Complex num = ( p - q ) + ikd * ( a2 - b2 ) * ( a1 + b1 ) * E;
				const Complex den = ( p + q ) + ikd * ( a1 - b1 ) * ( a2 - b2 ) * E;

				return std::norm( num / den );		// |r|²
			}

			//! Binade band `[2^-64, 2^+64]` inside which `RenormalizeLayerProduct`
			//! is a no-op (ordinary stacks never leave it; ~135 decades below
			//! `-ffast-math` naive complex-division overflow at `~1e154`).
			static const Scalar kRenormLo = Scalar(5.4210108624275222e-20);	// 2^-64
			static const Scalar kRenormHi = Scalar(1.8446744073709552e+19);	// 2^+64

			//! Rescales accumulated 2x2 product `M` in-place by an exact power of
			//! two via `frexp`/`ldexp` whenever its max-component magnitude leaves
			//! `(kRenormLo, kRenormHi)`.  Algebraically free because `r` is
			//! homogeneous of degree 0 in `M`; prevents multi-layer near-zero-index
			//! `1/|N|^2` growth from overflowing naive `-ffast-math` complex
			//! division.  Using `ldexp` directly avoids reciprocal overflow on
			//! denormals (exact for normal components).  Pinned by
			//! `ThinFilmProductionTest [Renormalize]`.
			inline void RenormalizeLayerProduct(
				Complex& m00, Complex& m01, Complex& m10, Complex& m11 )
			{
				// Max-component proxy (within sqrt(2) of true modulus) avoids 4 hypots.
				Scalar mag = std::fabs( m00.real() );
				const Scalar c[7] = { std::fabs( m00.imag() ),
				                      std::fabs( m01.real() ), std::fabs( m01.imag() ),
				                      std::fabs( m10.real() ), std::fabs( m10.imag() ),
				                      std::fabs( m11.real() ), std::fabs( m11.imag() ) };
				for( int i = 0; i < 7; ++i ) { if( c[i] > mag ) mag = c[i]; }
				if( !( mag > Scalar(0) ) || !std::isfinite( mag ) ) {
					return;
				}
				if( mag > kRenormLo && mag < kRenormHi ) {
					return;
				}
				int exponent = 0;
				std::frexp( mag, &exponent );
				m00 = Complex( std::ldexp( m00.real(), -exponent ),
				               std::ldexp( m00.imag(), -exponent ) );
				m01 = Complex( std::ldexp( m01.real(), -exponent ),
				               std::ldexp( m01.imag(), -exponent ) );
				m10 = Complex( std::ldexp( m10.real(), -exponent ),
				               std::ldexp( m10.imag(), -exponent ) );
				m11 = Complex( std::ldexp( m11.real(), -exponent ),
				               std::ldexp( m11.imag(), -exponent ) );
			}

			//! Allocation-free N-layer characteristic-matrix reflectance for one
			//! polarization (ambient -> substrate).  Reduces algebraically to
			//! `AiryReflectanceForPol` when `nFilms == 1`.
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
					// Precondition: filmIndex[j] is already passive and finite.
					const Complex Nj = filmIndex[j];
					const Scalar  dj = filmThickness_nm[j];

					const Complex cosj = CosThetaInMedium( Nj, sinInvariant );
					// Exponent-factored, singularity-free layer matrix Mhat_j:
					//  1. Avoids cosj == 0 singularities by forming delta/eta and
					//     delta*eta directly (both finite at cosj == 0 for s and p).
					//  2. Factors scalar e^{-i*delta}/2 out of Mj (cancels in
					//     homogeneous r), leaving Mhat_j = [[1+phi, -2i(delta/eta)E],
					//     [-2i(delta*eta)E, 1+phi]] with phi = e^{2i*delta} (|phi|<=1)
					//     and E = ExpM1OverZ(2i*delta), keeping entries O(1) for thick
					//     absorbing/evanescent layers.
					const Scalar  kd = PhaseCoefficient( dj, lambda_nm );
					const Complex delta = Complex( kd, Scalar(0) ) * Nj * cosj;
					const Complex z     = Complex( Scalar(0), Scalar(2) ) * delta;
					const Complex phi   = std::exp( z );		// |phi| <= 1
					const Complex E     = ExpM1OverZ( z );		// (phi - 1)/z
					const Complex cos2  = cosj * cosj;
					const Complex Nj2   = Nj * Nj;
					const Complex deltaOverEta  = ( pol == ePolS )
						? Complex( kd, Scalar(0) )
						: Complex( kd, Scalar(0) ) * cos2;
					const Complex deltaTimesEta = ( pol == ePolS )
						? Complex( kd, Scalar(0) ) * Nj2 * cos2
						: Complex( kd, Scalar(0) ) * Nj2;
					const Complex negTwoI( Scalar(0), Scalar(-2) );

					const Complex a00 = Complex( Scalar(1), Scalar(0) ) + phi;
					const Complex a01 = negTwoI * deltaOverEta  * E;
					const Complex a10 = negTwoI * deltaTimesEta * E;
					const Complex a11 = a00;

					// M <- M * Mj
					const Complex n00 = m00 * a00 + m01 * a10;
					const Complex n01 = m00 * a01 + m01 * a11;
					const Complex n10 = m10 * a00 + m11 * a10;
					const Complex n11 = m10 * a01 + m11 * a11;

					m00 = n00; m01 = n01;
					m10 = n10; m11 = n11;

					// Keep accumulated M away from naive complex-division overflow.
					RenormalizeLayerProduct( m00, m01, m10, m11 );
				}

				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );
				// Pre-scale [B; C] = M * [1; eta_s] by AdmittanceScale(cosS) and
				// clear scale0 in r = (eta0*B - C)/(eta0*B + C) so cos == 0
				// (exact critical angle) yields |r| = 1 without dividing by cos.
				const Complex scaleS = AdmittanceScale( cosS, pol );
				const Complex etaSs  = ScaledAdmittance( Ns, cosS, pol );
				const Complex Bs = m00 * scaleS + m01 * etaSs;
				const Complex Cs = m10 * scaleS + m11 * etaSs;

				const Complex scale0 = AdmittanceScale( cos0, pol );
				const Complex eta0s  = ScaledAdmittance( N0, cos0, pol );
				const Complex num = eta0s * Bs - scale0 * Cs;
				const Complex den = eta0s * Bs + scale0 * Cs;
				const Complex r = num / den;
				return std::norm( r );		// |r|²
			}

			//! Single-film reflectance for one polarization.
			//! Uses factored Airy for speed; falls back to 1-layer TMM only when
			//! Airy is non-finite (defence in depth; also handles `|N1| < ~1e-77.5`
			//! where Airy overflows but renormalized TMM succeeds).  Degeneracies
			//! (`cos1 == 0`, evanescent roots) are handled algebraically inside
			//! Airy/PickForwardCos, never via `isfinite`.
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

		//! Unpolarized reflectance `R = ½(R_s + R_p)` of an ambient / single-film /
		//! substrate stack at one wavelength (allocation-free, Airy closed form).
		//!
		//! \param cosThetaI     cosine to the microfacet half-vector `dot(wi, h)` in [0,1].
		//! \param wavelength_nm hero wavelength, nm.
		//! \param n0, k0        ambient index `N0 = n0 + i*k0` (supported domain: non-absorbing `k0 == 0`).
		//! \param n1, k1        film index `N1 = n1 + i*k1`.
		//! \param thickness_nm  physical film thickness, nm.
		//! \param n2, k2        substrate index `Ns = n2 + i*k2`.
		//! \return              unpolarized reflectance R in [0,1].
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			if( detail::IsNonFiniteIndex( Complex( n0, k0 ) )
			 || detail::IsNonFiniteIndex( Complex( n1, k1 ) )
			 || detail::IsNonFiniteIndex( Complex( n2, k2 ) ) ) {
				return detail::kNonFiniteStackReflectance;
			}
			const Complex N0 = detail::MakeIndex( n0, k0 );
			const Complex N1 = detail::MakeIndex( n1, k1 );
			const Complex Ns = detail::MakeIndex( n2, k2 );

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::SingleFilmReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolP );

			// Clamp FP boundary round-off only (passive stacks are in [0,1]).
			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//! Convenience overload taking pre-built complex indices (still validated/folded).
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& n0,
			const Complex& n1,
			Scalar thickness_nm,
			const Complex& ns )
		{
			if( detail::IsNonFiniteIndex( n0 ) || detail::IsNonFiniteIndex( n1 )
			 || detail::IsNonFiniteIndex( ns ) ) {
				return detail::kNonFiniteStackReflectance;
			}
			const Complex N0 = detail::PhysicalIndex( n0 );
			const Complex N1 = detail::PhysicalIndex( n1 );
			const Complex Ns = detail::PhysicalIndex( ns );

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

		//! General N-layer unpolarized reflectance (multi-oxide / `ar_layer` stacks).
		//! `nFilms` is clamped to `[0, kMaxFilms]`; arrays are in ambient -> substrate
		//! order.  Allocation-free; reduces algebraically to `ReflectanceConductor`
		//! when `nFilms == 1`.
		inline Scalar ReflectanceConductorStack(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& n0,
			const Complex* filmIndex,
			const Scalar* filmThickness_nm,
			int nFilms,
			const Complex& ns )
		{
			if( nFilms < 0 ) nFilms = 0;
			if( nFilms > kMaxFilms ) nFilms = kMaxFilms;

			// Validate and fold every medium once before the polarization loop.
			if( detail::IsNonFiniteIndex( n0 ) || detail::IsNonFiniteIndex( ns ) ) {
				return detail::kNonFiniteStackReflectance;
			}
			const Complex N0 = detail::PhysicalIndex( n0 );
			const Complex Ns = detail::PhysicalIndex( ns );
			Complex normFilm[kMaxFilms];
			for( int j = 0; j < nFilms; ++j ) {
				if( detail::IsNonFiniteIndex( filmIndex[j] ) ) {
					return detail::kNonFiniteStackReflectance;
				}
				normFilm[j] = detail::PhysicalIndex( filmIndex[j] );
			}

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::TmmReflectanceForPol(
				N0, normFilm, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::TmmReflectanceForPol(
				N0, normFilm, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolP );

			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//======================================================================
		//  RGB-path albedo-basis integration (PREVIEW — docs/THIN_FILM_INTERFERENCE.md §7/§8/§13)
		//======================================================================
		//
		//  Pre-integrates R(λ) over 380-780 nm against the renderer's CIE CMFs
		//  in the illuminant-free albedo basis (preview-grade; spectral path is
		//  authoritative):
		//    1. Equal-energy relative tristimulus: relC = Σ R(λ)·cmf_C(λ)·Δλ / Σ cmf_C(λ)·Δλ
		//    2. von-Kries scale E -> exact D65 white `Rec709RGBtoXYZ(1,1,1)`
		//       so a flat reflector R(λ)≡1 maps to neutral Rec.709 RGB (1,1,1).
		//  Per-shade evaluation handles spatially-varying painters; a 2D LUT for
		//  spatially-constant materials is a deferred optimization (§7/§13).

		//! Midpoint wavelength strata over 380-780 nm (allocation-free).
		static const int kRGBIntegrationSamples = 32;

		//! Dispersion-aware RGB albedo-basis reflectance at half-vector cosine
		//! `cosThetaI` and λ-independent `thickness_nm`.  `stackAt` has signature
		//! `void(Scalar nm, Scalar& n0, Scalar& k0, Scalar& n1, Scalar& k1, Scalar& n2, Scalar& k2)`
		//! (template boundary keeps painter types out of `ThinFilm.h`).
		template<class StackFn>
		inline RISEPel ReflectanceConductorRGBSpectral(
			Scalar cosThetaI,
			Scalar thickness_nm,
			const StackFn& stackAt )
		{
			const Scalar loNm = Scalar( 380 );
			const Scalar hiNm = Scalar( 780 );
			const Scalar step = ( hiNm - loNm ) / Scalar( kRGBIntegrationSamples );

			Scalar Xn = Scalar( 0 ), Yn = Scalar( 0 ), Zn = Scalar( 0 );
			Scalar Xe = Scalar( 0 ), Ye = Scalar( 0 ), Ze = Scalar( 0 );

			for( int s = 0; s < kRGBIntegrationSamples; ++s ) {
				const Scalar nm = loNm + ( Scalar( s ) + Scalar( 0.5 ) ) * step;

				XYZPel cmf;
				if( !ColorUtils::XYZFromNM( cmf, nm ) ) {
					continue;
				}

				Scalar n0, k0, n1, k1, n2, k2;
				stackAt( nm, n0, k0, n1, k1, n2, k2 );

				const bool nonFinite =
					detail::IsNonFiniteIndex( Complex( n0, k0 ) ) ||
					detail::IsNonFiniteIndex( Complex( n1, k1 ) ) ||
					detail::IsNonFiniteIndex( Complex( n2, k2 ) );
				const Complex N0 = detail::MakeIndex( n0, k0 );
				const Complex N1 = detail::MakeIndex( n1, k1 );
				const Complex Ns = detail::MakeIndex( n2, k2 );
				const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

				Scalar R;
				if( nonFinite ) {
					// Per-wavelength fallback (zero indices still take kZeroIndexStandIn).
					R = detail::kNonFiniteStackReflectance;
				} else {
					const Scalar Rs = detail::SingleFilmReflectanceForPol(
						N0, N1, Ns, thickness_nm, nm, sinInv, ePolS );
					const Scalar Rp = detail::SingleFilmReflectanceForPol(
						N0, N1, Ns, thickness_nm, nm, sinInv, ePolP );
					R = Scalar( 0.5 ) * ( Rs + Rp );
				}
				if( R < Scalar( 0 ) ) R = Scalar( 0 );
				if( R > Scalar( 1 ) ) R = Scalar( 1 );

				Xn += R * cmf.X * step;  Yn += R * cmf.Y * step;  Zn += R * cmf.Z * step;
				Xe += cmf.X * step;      Ye += cmf.Y * step;      Ze += cmf.Z * step;
			}

			// Equal-energy relative tristimulus -> exact D65 white -> linear Rec.709.
			const Scalar relX = ( Xe > Scalar( 0 ) ) ? Xn / Xe : Scalar( 0 );
			const Scalar relY = ( Ye > Scalar( 0 ) ) ? Yn / Ye : Scalar( 0 );
			const Scalar relZ = ( Ze > Scalar( 0 ) ) ? Zn / Ze : Scalar( 0 );

			const Rec709RGBPel whiteRGB( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) );
			const XYZPel whiteXYZ = ColorUtils::Rec709RGBtoXYZ( whiteRGB );

			XYZPel xyz;
			xyz.X = relX * whiteXYZ.X;
			xyz.Y = relY * whiteXYZ.Y;
			xyz.Z = relZ * whiteXYZ.Z;

			RISEPel rgb = ColorUtils::XYZtoRec709RGB( xyz );
			if( rgb.r < Scalar( 0 ) ) rgb.r = Scalar( 0 );
			if( rgb.g < Scalar( 0 ) ) rgb.g = Scalar( 0 );
			if( rgb.b < Scalar( 0 ) ) rgb.b = Scalar( 0 );
			return rgb;
		}

		//! Constant-index RGB albedo-basis reflectance wrapper over
		//! `ReflectanceConductorRGBSpectral` (bit-identical for constant n,k).
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
		
		//! Hemispherical Fresnel average `F_avg = 2 * ∫_0^1 R(μ) μ dμ` at one
		//! wavelength for the Kulla-Conty multiscatter tail
		//! (docs/THIN_FILM_INTERFERENCE.md §7).  Uses the same 21-point
		//! Gauss-Legendre rule as `MicrofacetEnergyLUT::ComputeFresnelAvg` so
		//! `film == air` matches the bare conductor identically.
		inline Scalar FresnelAvgConductor(
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			Scalar sum = Scalar( 0 );
			for( unsigned int i = 0; i < GaussLegendre21::NodeCount; ++i ) {
				const Scalar mu = GaussLegendre21::Nodes[i];
				sum += ReflectanceConductor( mu, wavelength_nm, n0, k0, n1, k1, thickness_nm, n2, k2 )
					* ( Scalar( 2 ) * mu * GaussLegendre21::Weights[i] );
			}
			return sum;
		}
		
		//! Dispersion-aware RGB hemispherical Fresnel average (preview-grade)
		//! for the Kulla-Conty multiscatter tail; uses 21-point Gauss-Legendre
		//! over `ReflectanceConductorRGBSpectral`.
		template<class StackFn>
		inline RISEPel FresnelAvgConductorRGBSpectral(
			Scalar thickness_nm,
			const StackFn& stackAt )
		{
			RISEPel sum( Scalar( 0 ), Scalar( 0 ), Scalar( 0 ) );
			for( unsigned int i = 0; i < GaussLegendre21::NodeCount; ++i ) {
				const Scalar mu = GaussLegendre21::Nodes[i];
				sum = sum + ReflectanceConductorRGBSpectral( mu, thickness_nm, stackAt )
					* ( Scalar( 2 ) * mu * GaussLegendre21::Weights[i] );
			}
			return sum;
		}

		//! Constant-index RGB hemispherical Fresnel average wrapper.
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
