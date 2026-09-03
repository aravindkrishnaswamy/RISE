//////////////////////////////////////////////////////////////////////
//
//  FibreLobeMath.h - The stateless fibre-scattering lobe primitives
//    shared by `hair_material` and `weave_material`.
//
//  ONE IMPLEMENTATION, TWO MATERIALS.  Every function below was
//  file-local to HairBSDF.cpp's anonymous namespace until
//  docs/CLOTH_FABRIC_DESIGN.md Phase 2 (slice P2-A) needed the same
//  longitudinal / azimuthal / Fresnel core for a woven-thread BSDF.
//  Rather than reproduce them -- which is precisely the RGB/NM-twin
//  drift docs/skills/audit-by-bug-pattern.md catalogues, only across
//  materials instead of across colour regimes -- they were PROMOTED
//  here verbatim and HairBSDF.cpp now consumes this header through
//  using-declarations, so its call sites are textually unchanged and
//  there is exactly one body per function.
//
//  WHY THESE AND NOT `MakeGeom` / `ComputeAp`.  The promoted set is the
//  closed subgraph reachable from the four functions Phase 2 actually
//  uses (`Mp`, `TrimmedLogistic`, `SampleTrimmedLogistic`,
//  `FrDielectric`).  `MakeGeom` and `ComputeAp` are hair-SPECIFIC: they
//  carry the fibre radius `h`, the eccentricity and the R/TT/TRT
//  multi-order energy bookkeeping, none of which a weave thread has
//  (both cloth papers model a thread as a single-order surface + volume
//  pair -- P2_ZHU_READ 4.1).  Promoting them would export hair's
//  internal geometry as if it were shared vocabulary.
//
//  WHAT THE WEAVE MODEL GETS OUT OF THEM (P2_ZHU_READ 4.1, 5):
//
//    * `Mp` is d'Eon 2011's energy-conserving longitudinal lobe, and it
//      is a STRICTLY BETTER substitute for Sadeghi 2013's plain
//      Gaussian `g(gamma, theta_h)` -- normalised at every roughness
//      rather than only in the narrow-angle limit.  Its normalisation
//
//          INT_{-pi/2}^{pi/2} Mp(theta_i, theta_o; v) cos(theta_i) dtheta_i == 1
//
//      is what makes the weave surface lobe energy-bounded BY
//      CONSTRUCTION rather than by a baked table (WeaveBRDF.h's energy
//      argument leans on it directly), and it peaks on the specular
//      cone theta_i == -theta_o, which is exactly a smooth cylinder's
//      reflection geometry.
//
//    * `TrimmedLogistic` / `SampleTrimmedLogistic` are an azimuthal
//      shape with an EXACT closed-form CDF and an EXACT inverse.
//      Neither cloth paper supplies one: Sadeghi's azimuthal factor is
//      a bare `cos(phi_d/2)` with no independently normalised sampler
//      (his own conclusion names importance sampling as open work), and
//      Zhu 2023's azimuthal behaviour is implicit inside an SGGX
//      evaluation with no separable factor at all.  RISE already had
//      the better object.
//
//    * `FrDielectric` serves both papers' Fresnel terms directly.
//
//  ALL STATELESS, ALL ACHROMATIC.  Nothing here reads a painter, a hit
//  record or any shared state, which is what makes one copy safely
//  usable from two materials on the parallel render pass.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIBRE_LOBE_MATH_
#define FIBRE_LOBE_MATH_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/FiniteMath.h"
#include <cmath>

namespace RISE
{
	namespace FibreLobeMath
	{
		inline Scalar Sqr( const Scalar x ) { return x * x; }

		inline Scalar SafeSqrt( const Scalar x ) { return x > 0 ? sqrt( x ) : Scalar( 0 ); }

		inline Scalar Clamp( const Scalar x, const Scalar lo, const Scalar hi )
		{
			return x < lo ? lo : ( x > hi ? hi : x );
		}

		inline Scalar SafeASin( const Scalar x ) { return asin( Clamp( x, -1.0, 1.0 ) ); }

		//! Modified Bessel function of the first kind, order 0.  Truncated
		//! power series -- accurate for the small arguments the non-log
		//! branch of Mp sees (v > 0.1 keeps a bounded).
		inline Scalar BesselI0( const Scalar x )
		{
			Scalar val = 0;
			Scalar x2i = 1;
			Scalar ifact = 1;
			Scalar i4 = 1;
			for( int i = 0; i < 10; i++ ) {
				if( i > 1 ) {
					ifact *= Scalar( i );
				}
				val += x2i / ( i4 * Sqr( ifact ) );
				x2i *= x * x;
				i4 *= 4;
			}
			return val;
		}

		//! log(I0(x)), evaluated asymptotically for large x.  The whole
		//! reason the low-roughness longitudinal term is numerically stable:
		//! at beta_m = 0.05 the TT variance is ~3.7e-4, so 1/v ~ 2700 and a
		//! direct sinh(1/v) / I0(a) evaluation overflows to inf/inf.
		inline Scalar LogBesselI0( const Scalar x )
		{
			if( x > 12 ) {
				// log I0(x) ~ x - 0.5 log(2 pi x) + 1/(8x) + O(1/x^2).
				// NOTE this DEVIATES from PBRT, which writes the same line as
				// `x + 0.5 * (-log(2 pi) + log(1/x) + 1/(8x))` and thereby
				// halves the 1/(8x) correction (a known slip in that source).
				// This branch's SMALLEST argument is x = 12 -- that is where
				// it is entered, by definition of the `x > 12` gate -- not
				// ~2700: a = cosThetaI*cosThetaO/v ranges over [0, ~1/v], and
				// the INTERIOR grid (beta_m down to 0.1) already reaches
				// a ~41-610, well inside this branch; ~2700 is only the
				// DEEPEST corner cell (beta_m = kMinBeta = 0.05), not the
				// smallest argument the branch sees.  At x = 12 this fixed
				// 1/(16x) term is ~0.5% of the log; for context, the
				// truncated 10-term series `BesselI0` uses BELOW this branch
				// is itself ~2% LOW at x = 12, so the seam at x = 12 carries
				// a ~2% step either way -- taking the correct constant here
				// makes the ASYMPTOTIC SIDE of that seam accurate, it does
				// not make the seam itself smaller.
				return x - 0.5 * log( TWO_PI * x ) + 1 / ( 8 * x );
			}
			return log( BesselI0( x ) );
		}

		//! d'Eon 2011 energy-conserving longitudinal scattering function.
		inline Scalar Mp(
			const Scalar cosThetaI, const Scalar cosThetaO,
			const Scalar sinThetaI, const Scalar sinThetaO,
			const Scalar v
			)
		{
			if( !( v > 0 ) ) {
				return 0;
			}
			const Scalar a = cosThetaI * cosThetaO / v;
			const Scalar b = sinThetaI * sinThetaO / v;
			// 0.6931471805599453 == log(2); folded in from the closed form.
			const Scalar mp = ( v <= 0.1 )
				? exp( LogBesselI0( a ) - b - 1 / v + 0.6931471805599453 + log( 1 / ( 2 * v ) ) )
				: ( exp( -b ) * BesselI0( a ) ) / ( sinh( 1 / v ) * 2 * v );
			return ( mp > 0 && RISE::IsFiniteDouble( mp ) ) ? mp : Scalar( 0 );
		}

		inline Scalar Logistic( Scalar x, const Scalar s )
		{
			x = fabs( x );
			const Scalar e = exp( -x / s );
			return e / ( s * Sqr( 1 + e ) );
		}

		inline Scalar LogisticCDF( const Scalar x, const Scalar s )
		{
			return 1 / ( 1 + exp( -x / s ) );
		}

		//! Logistic restricted to [a, b] and renormalised.  Chosen by
		//! Chiang et al. precisely because it is both analytically
		//! normalisable and analytically invertible, which is what gives
		//! the azimuthal term exact importance sampling.
		inline Scalar TrimmedLogistic( const Scalar x, const Scalar s, const Scalar a, const Scalar b )
		{
			if( x < a || x > b ) {
				return 0;
			}
			const Scalar norm = LogisticCDF( b, s ) - LogisticCDF( a, s );
			if( !( norm > 0 ) ) {
				return 0;
			}
			const Scalar v = Logistic( x, s ) / norm;
			return RISE::IsFiniteDouble( v ) ? v : Scalar( 0 );
		}

		//! Inverse CDF of the trimmed logistic.
		inline Scalar SampleTrimmedLogistic( const Scalar u, const Scalar s, const Scalar a, const Scalar b )
		{
			const Scalar Pa = LogisticCDF( a, s );
			const Scalar Pb = LogisticCDF( b, s );
			// Keep the remapped uniform strictly inside (0,1): the inverse
			// blows up at either end, and s can be small enough that Pa / Pb
			// round to exactly 0 / 1.
			const Scalar up = Clamp( Pa + u * ( Pb - Pa ), 1e-12, 1.0 - 1e-12 );
			const Scalar x = -s * log( 1 / up - 1 );
			return RISE::IsFiniteDouble( x ) ? Clamp( x, a, b ) : Scalar( 0 );
		}

		//! Unpolarised Fresnel reflectance at a smooth dielectric boundary.
		//! Local to the fibre models rather than routed through Optics::
		//! because the Chiang formulation is expressed in terms of a
		//! relative eta and a signed cosine, and re-deriving a direction
		//! pair just to call the shared helper would lose precision at
		//! grazing angles.
		inline Scalar FrDielectric( Scalar cosThetaI, Scalar eta )
		{
			cosThetaI = Clamp( cosThetaI, -1.0, 1.0 );
			if( cosThetaI < 0 ) {
				eta = 1 / eta;
				cosThetaI = -cosThetaI;
			}
			const Scalar sin2ThetaT = ( 1 - Sqr( cosThetaI ) ) / Sqr( eta );
			if( sin2ThetaT >= 1 ) {
				return 1;			// total internal reflection
			}
			const Scalar cosThetaT = SafeSqrt( 1 - sin2ThetaT );
			const Scalar rParl = ( eta * cosThetaI - cosThetaT ) / ( eta * cosThetaI + cosThetaT );
			const Scalar rPerp = ( cosThetaI - eta * cosThetaT ) / ( cosThetaI + eta * cosThetaT );
			return ( Sqr( rParl ) + Sqr( rPerp ) ) * 0.5;
		}
	}
}

#endif
