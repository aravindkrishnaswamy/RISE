//////////////////////////////////////////////////////////////////////
//
//  HairBSDF.cpp - Implementation of the Chiang et al. 2016 near-field
//    hair / fur BCSDF.  See HairBSDF.h for the conventions, the
//    kray / pdf reconciliation, and the caveats.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 26, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HairBSDF.h"
#include "HairMedullaProfile.h"
#include "BioSpecSkinData.h"
#include "../Utilities/FiniteMath.h"
#include "../Interfaces/ILog.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
//  File-local model math.
//
//  Everything below is achromatic except where a sigma_a / eta is
//  passed in explicitly, which is what lets the RGB path evaluate one
//  set of longitudinal / azimuthal weights and dot it against three
//  per-channel attenuation vectors.
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Highest explicitly-modelled scattering order.  p = 0 (R),
	//! 1 (TT), 2 (TRT), and p = kPMax is the RESIDUAL lobe that lumps
	//! every order above 2 -- it is what makes the model pass the white
	//! furnace test at any roughness.
	const int		kPMax = 3;

	//! The two Yan 2017 medulla-scattered lobes, appended AFTER the
	//! four Chiang orders so every pre-Phase-3 index keeps its meaning
	//! (HairBSDF.h section 3b).  `kLobeTTs` carries what the medulla
	//! scattered out of TT (p = 1), `kLobeTRTs` out of TRT (p = 2).
	const int		kLobeTTs  = kPMax + 1;		// 4
	const int		kLobeTRTs = kPMax + 2;		// 5
	//! Array extent for every per-lobe vector.  The RUNTIME loop bound
	//! is `kPMax + 1` when the medulla is off and this when it is on --
	//! never this unconditionally, or a medulla-off hit would stop
	//! being bit-identical to the pre-Phase-3 build.
	const int		kNumLobes = kPMax + 3;		// 6

	//! Upper bound on the longitudinal variance the medulla may ADD to
	//! a scattered lobe's M_p.  The baked table's largest cell is
	//! ~0.47 rad^2, so this never binds on the shipped data; it exists
	//! so a corrupt or regenerated table cannot push M_p into a regime
	//! where its series evaluation misbehaves.
	const Scalar	kMaxMedullaLongVariance = 4.0;

	//! Representative wavelengths the RGB path evaluates spectral
	//! quantities (the melanin curves) at.  RISE has no canonical
	//! RGB<->wavelength triple; these are the conventional ones and are
	//! used ONLY by colour tier 1, so RGB and spectral hair renders are
	//! driven by the same underlying curve rather than by two
	//! independently-fit constant sets.
	const Scalar	kRGBWavelengthsNM[3] = { 600.0, 550.0, 450.0 };

	//! Per-unit-concentration sigma_a at 550 nm that the OMLC melanin
	//! curves are normalised to.  These are PBRT-v4's GREEN-channel
	//! `SigmaAFromConcentration` coefficients, so `eumelanin 1.3` means
	//! "brown-black hair" exactly as it does in every other Chiang
	//! implementation.
	//!
	//! SCOPE OF THE PBRT MATCH -- exactly one anchor, not three.  We
	//! reproduce PBRT's GREEN coefficient exactly and then let the
	//! measured OMLC curve shape carry R and B.  PBRT's other two
	//! constants are a DIFFERENTLY-DERIVED RGB triple, not the same
	//! quantity read at a representative wavelength, so they do NOT
	//! agree.  Measured against `kRGBWavelengthsNM` (600 / 550 / 450):
	//!
	//!     eumelanin    R +23.5 %  (0.518 vs PBRT 0.419)
	//!                  B  -5.6 %  (1.293 vs PBRT 1.370)
	//!     pheomelanin  R +24.2 %  (0.232 vs PBRT 0.187)
	//!                  B  +1.7 %  (1.067 vs PBRT 1.050)
	//!
	//! The deviation is deliberate: spectral fidelity to the measured
	//! OMLC curve wins over matching PBRT's differently-derived constant,
	//! and it is what keeps the RGB and NM paths driven by ONE curve.  A
	//! PBRT cross-render is therefore an apples-to-apples test in G only;
	//! expect a slightly warmer R and a marginally deeper B.
	//!
	//! Also note the pheomelanin table itself has a small NON-MONOTONIC
	//! bump at 591.91 -> 600.98 nm (1.589 -> 1.756 cm^-1 mg/ml).  That is
	//! in the measured data and is kept verbatim -- it does not break the
	//! "pheomelanin reddens" behaviour, which is driven by the overall
	//! fall from ~3.0 at 550 nm to ~1.19 at 653 nm.
	const Scalar	kEumelaninSigmaAAt550   = 0.697;
	const Scalar	kPheomelaninSigmaAAt550 = 0.400;

	//! Used only if a caller misconfigures the colour tiers (an error
	//! is logged at construction).  Mid-brown.  `bColorTierValid` gates
	//! it, so a MISCONFIGURED material really does resolve to this and
	//! nothing else -- it is not a "all four painters were null" last
	//! resort (that would have made the construction-time log a lie).
	const Scalar	kFallbackSigmaA = 1.0;

	//! How close to the fibre edge a resolved h is allowed to get.
	//! See the note in HairScatteringBase::Resolve.
	const Scalar	kHEdge = 0.9995;

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
	Scalar BesselI0( const Scalar x )
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
	Scalar LogBesselI0( const Scalar x )
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
	Scalar Mp(
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
	Scalar TrimmedLogistic( const Scalar x, const Scalar s, const Scalar a, const Scalar b )
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
	Scalar SampleTrimmedLogistic( const Scalar u, const Scalar s, const Scalar a, const Scalar b )
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
	//! Local to this file rather than routed through Optics:: because
	//! the Chiang formulation is expressed in terms of a relative eta
	//! and a signed cosine, and re-deriving a direction pair just to
	//! call the shared helper would lose precision at grazing angles.
	Scalar FrDielectric( Scalar cosThetaI, Scalar eta )
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

	//! Ray-geometry terms in the fibre frame for one (h, eta) pair.
	struct Geom
	{
		Scalar	sinThetaO, cosThetaO;
		Scalar	phiO;
		Scalar	gammaO;
		Scalar	cosThetaT;
		Scalar	sinGammaT;		//!< kept for the medulla geometry only
		Scalar	cosGammaT, gammaT;
		Scalar	absorbLen;		//!< optical path length through the fibre, 2 cos(gamma_t) / cos(theta_t)
	};

	void MakeGeom( const Scalar h, const Scalar eta, const Scalar woLocal[3], Geom& g )
	{
		g.sinThetaO = Clamp( woLocal[0], -1.0, 1.0 );
		g.cosThetaO = SafeSqrt( 1 - Sqr( g.sinThetaO ) );
		g.phiO      = atan2( woLocal[2], woLocal[1] );
		g.gammaO    = SafeASin( h );

		const Scalar sinThetaT = g.sinThetaO / eta;
		g.cosThetaT = SafeSqrt( 1 - Sqr( sinThetaT ) );

		// Modified index for the projected (azimuthal) refraction.
		// Degenerate only when the ray runs exactly along the fibre
		// axis, where every lobe's contribution is ~0 anyway.
		//
		// MEASURE-ZERO DIVERGENCE FROM PBRT.  As cosThetaO -> 0 the true
		// etap -> +inf, hence sinGammaT = h/etap -> 0 and gammaT -> 0;
		// PBRT lets the IEEE division produce that limit.  RISE's finite
		// sentinels forbid an inf here (see the fast-math note in
		// CLAUDE.md), so we substitute etap = eta, which gives
		// sinGammaT = h/eta instead of 0.  It matters only on the exact
		// axial great circle, a set of measure zero that every lobe
		// weights at ~0 -- and it keeps `Pdf` and `EvalFsum` consistent
		// with each other, which is what the estimator actually needs.
		const Scalar etap = ( g.cosThetaO > 1e-9 )
			? SafeSqrt( Sqr( eta ) - Sqr( g.sinThetaO ) ) / g.cosThetaO
			: eta;
		const Scalar sinGammaT = ( etap > 1e-9 ) ? Clamp( h / etap, -1.0, 1.0 ) : Scalar( 0 );
		g.sinGammaT = sinGammaT;
		g.cosGammaT = SafeSqrt( 1 - Sqr( sinGammaT ) );
		g.gammaT    = SafeASin( sinGammaT );

		g.absorbLen = ( g.cosThetaT > 1e-9 ) ? 2 * g.cosGammaT / g.cosThetaT : Scalar( 0 );
	}

	//! The medulla geometry for one hit, plus the interpolated
	//! scattering profile.  Built ONCE per Geom and shared by the
	//! attenuation split, the lobe weights and the sampler, so the
	//! three can never disagree about which cell they read.
	//!
	//! `active == false` means "this ray never enters the medulla" --
	//! either the model is off, or the refracted chord passes outside
	//! the medulla radius.  It leaves `q1 == q2 == 1`, so the
	//! attenuation split degenerates to "all of it stays unscattered"
	//! and the two extra lobes carry exactly zero energy.
	//! Deliberately SMALL -- six scalars and a flag, no profile.  The
	//! interpolated profile is ~33 doubles and is materialised
	//! separately, by `LoadMedullaProfile`, only where it is actually
	//! read: the evaluation path always needs it, but the SAMPLING path
	//! needs it only on the hits that actually draw a scattered lobe.
	//! Keeping it out of this struct keeps `DoScatter`'s stack frame the
	//! size it was before Phase 3 -- not merely a performance nicety: a
	//! fat frame there perturbed the compiler's inlining of `EvalPdf`
	//! and cost 1 ULP on part of the group-15a golden set.
	struct Medulla
	{
		bool				active;
		Scalar				b;			//!< signed impact parameter, in medulla radii
		Scalar				tau;		//!< diametral optical depth
		Scalar				g;			//!< HG anisotropy
		Scalar				q1;			//!< ballistic survival of ONE medulla crossing
		Scalar				q2;			//!< ... of two (TRT), == q1 * q1
	};

	void MakeMedulla( const HairResolvedParams& R, const Geom& G, Medulla& M )
	{
		M.active = false;
		M.b = 0;
		M.tau = 0;
		M.g = 0;
		M.q1 = 1;
		M.q2 = 1;

		if( !R.medullaActive ) {
			return;
		}

		// Impact parameter of the REFRACTED chord relative to the
		// medulla.  The perpendicular distance from the fibre axis to
		// the chord is |sin gamma_t| (in fibre radii), so in MEDULLA
		// radii it is sin(gamma_t) / kappa.  |b| >= 1 means the chord
		// runs entirely outside the medulla -- no crossing at all.
		//
		// The sign is carried through: a chord passing one side of the
		// medulla axis produces the mirror image of the azimuthal
		// profile the other side produces, and HairMedullaLookup folds
		// that into its `mirrored` flag.
		const Scalar b = G.sinGammaT / R.kappa;
		if( !( fabs( b ) < 1.0 ) || !( G.cosThetaT > 1e-9 ) ) {
			return;
		}

		// The DIAMETRAL optical depth: sigma_m across 2*kappa fibre
		// radii, stretched by the ray's inclination inside the fibre.
		// (The actual chord's depth is this times sqrt(1 - b^2), which
		// is exactly what the ballistic survival below uses and exactly
		// what the bake's own tau means -- HairMedullaProfile.h.)
		const Scalar tau = R.sigmaM * 2 * R.kappa / G.cosThetaT;
		if( !( tau > 0 ) || !RISE::IsFiniteDouble( tau ) ) {
			return;
		}

		const Scalar q = exp( -tau * SafeSqrt( 1 - Sqr( b ) ) );
		if( !RISE::IsFiniteDouble( q ) ) {
			return;
		}
		M.q1 = Clamp( q, 0.0, 1.0 );
		M.q2 = M.q1 * M.q1;
		// A regenerated table whose bin count outgrew the fixed-capacity
		// runtime struct cannot be interpolated, and a medulla that
		// SPLITS energy away from TT/TRT but then has no profile to
		// scatter it into would LOSE that energy silently.  Refuse to
		// activate at all in that case: the model degrades to the exact
		// pre-Phase-3 lobe set, which is a visible loss of the feature
		// rather than an invisible loss of light.  (Group 19 asserts the
		// shipped table satisfies this, so it is defence in depth.)
		if( kHairMedullaPhiBins == 0 ||
		    kHairMedullaPhiBins > (unsigned int)RISE::Implementation::HairMedullaProfile::kMaxBins ) {
			M.q1 = 1;
			M.q2 = 1;
			return;
		}

		M.b = b;
		M.tau = tau;
		M.g = R.gHG;
		M.active = true;
	}

	//! Materialise the interpolated azimuthal profile for a medulla
	//! `MakeMedulla` reported active.  An inactive medulla -- or a
	//! regenerated table whose bin count outgrew the fixed-capacity
	//! struct -- yields `nBins == 0`, which every consumer reads as
	//! "no scattered lobes".
	void LoadMedullaProfile( const Medulla& M, RISE::Implementation::HairMedullaProfile& prof )
	{
		if( !M.active ) {
			prof.nBins = 0;
			prof.longVariance = 0;
			prof.mirrored = false;
			return;
		}
		RISE::Implementation::HairMedullaLookup( M.b, M.tau, M.g, prof );
	}

	//! `sum_p w[p] * ap[p]` -- the mixture dot product both `EvalFsum`
	//! and `EvalPdf` are built on.
	//!
	//! ! THE SHAPE OF THIS LOOP IS LOAD BEARING.  The first four terms
	//! are accumulated by a loop with a COMPILE-TIME bound, exactly as
	//! the pre-Phase-3 code wrote it, and the two medulla terms are
	//! appended afterwards.  Folding all six into one runtime-bounded
	//! loop is algebraically identical but lets the compiler contract
	//! the multiply-adds differently, which cost 1 ULP on 35 of the 204
	//! group-15a goldens when it was first written that way.  Keep the
	//! two halves separate.
	inline Scalar DotLobes( const Scalar w[kNumLobes], const Scalar ap[kNumLobes],
	                        const bool bMedulla )
	{
		Scalar sum = 0;
		for( int p = 0; p <= kPMax; p++ ) {
			sum += w[p] * ap[p];
		}
		if( bMedulla ) {
			sum += w[kLobeTTs]  * ap[kLobeTTs];
			sum += w[kLobeTRTs] * ap[kLobeTRTs];
		}
		return sum;
	}

	//! The longitudinal variance a medulla-scattered lobe uses:
	//! its parent order's variance, broadened by the tabulated
	//! exit-angle variance (Gaussian-convolution approximation;
	//! HairBSDF.h section 3b).
	inline Scalar MedullaLobeVariance(
		const HairResolvedParams& R,
		const RISE::Implementation::HairMedullaProfile& prof, const int parentP )
	{
		const Scalar add = Clamp( prof.longVariance, 0.0, kMaxMedullaLongVariance );
		return R.v[parentP] + add;
	}

	//! Ideal azimuthal exit angle for scattering order p.
	inline Scalar PhiForP( const int p, const Scalar gammaO, const Scalar gammaT )
	{
		return 2 * p * gammaT - 2 * gammaO + p * PI;
	}

	//! Azimuthal scattering term: the trimmed logistic wrapped around
	//! the deterministic exit angle PhiForP.
	Scalar Np( const Scalar phi, const int p, const Scalar s,
	           const Scalar gammaO, const Scalar gammaT )
	{
		Scalar dphi = phi - PhiForP( p, gammaO, gammaT );
		if( !RISE::IsFiniteDouble( dphi ) ) {
			return 0;
		}
		// Remap to [-PI, PI].  Bounded: |dphi| starts below ~7*PI.
		while( dphi > PI )  { dphi -= TWO_PI; }
		while( dphi < -PI ) { dphi += TWO_PI; }
		return TrimmedLogistic( dphi, s, -PI, PI );
	}

	//! Apparent attenuation for each scattering order at this h.
	//! ap[kPMax] is the residual (all orders > kPMax - 1) term.
	void ComputeAp( const Scalar cosThetaO, const Scalar eta, const Scalar h,
	                const Scalar T, Scalar ap[kPMax + 1] )
	{
		const Scalar cosGammaO = SafeSqrt( 1 - Sqr( h ) );
		const Scalar f = FrDielectric( cosThetaO * cosGammaO, eta );

		ap[0] = f;
		ap[1] = Sqr( 1 - f ) * T;
		for( int p = 2; p < kPMax; p++ ) {
			ap[p] = ap[p - 1] * T * f;
		}
		// MEASURE-ZERO HOLE, recorded so it is not re-derived: this
		// guard can fire for the achromatic PROXY absorption and not
		// for a wavelength (T_proxy >= T_lambda => denom_proxy <=
		// denom_lambda), giving apPDF[kPMax] == 0 with ap_lambda[kPMax]
		// > 0 -- a direction with BSDF value but no sampling density.
		// It needs f ~ 1 AND sigma_proxy ~ 0 simultaneously, where the
		// leaked term is ~(1-f)^2/denom ~ 1e-12 of the lobe.  Not worth
		// a fix; worth knowing it was looked at.
		const Scalar denom = 1 - T * f;
		ap[kPMax] = ( denom > 1e-12 ) ? ap[kPMax - 1] * f * T / denom : Scalar( 0 );
	}

	//! `ComputeAp` plus the Yan 2017 medulla SPLIT (HairBSDF.h section
	//! 3b).  When `bMedulla` is false this is `ComputeAp` and nothing
	//! else -- not "ComputeAp then multiply by ones" -- which is what
	//! keeps a medulla-off hit bit-identical.
	//!
	//! The split conserves energy EXACTLY: ap[1] + ap[kLobeTTs] and
	//! ap[2] + ap[kLobeTRTs] each reproduce the value ComputeAp wrote,
	//! so `sum_p ap[p]` is unchanged and both the furnace gate and the
	//! section-3 proxy bound carry over untouched.  (The bound needs
	//! `q` to be wavelength-independent, which holds under exactly the
	//! same non-dispersive-`ior` proviso section 3 already states -- q
	//! depends on eta through gamma_t, NOT only on the achromatically-
	//! read medulla painters.  HairBSDF.h section 3b spells this out.)
	void ComputeApWithMedulla(
		const Scalar cosThetaO, const Scalar eta, const Scalar h, const Scalar T,
		const Medulla& M, const bool bMedulla, Scalar ap[kNumLobes]
		)
	{
		ComputeAp( cosThetaO, eta, h, T, ap );
		if( !bMedulla ) {
			return;
		}
		// Order matters: ap[2] was derived FROM the unsplit ap[1], so
		// the scattered halves must be taken before either is scaled.
		ap[kLobeTTs]  = ap[1] * ( 1 - M.q1 );
		ap[kLobeTRTs] = ap[2] * ( 1 - M.q2 );
		ap[1] *= M.q1;
		ap[2] *= M.q2;
		// ap[kPMax] -- the residual bucket for every order above TRT --
		// is deliberately left whole; see HairBSDF.h section 3b.
	}

	//! The ACHROMATIC lobe-selection PMF -- the sole reason `Pdf` is
	//! wavelength independent (HairBSDF.h section 3).  `sigmaProxy` is
	//! the component-wise minimum of the RGB sigma_a triple.
	//!
	//! Returns false when every order has zero apparent energy (the
	//! caller must then produce no sample / zero density).
	//! ! THE VERBATIM PRE-PHASE-3 BODY.  `ComputeApPDF` below is its
	//! medulla-aware twin and must stay term-for-term equivalent when
	//! `bMedulla` is false -- including the sanitise-then-sum ordering,
	//! the `!(sum > 0)` gate and the divide loop.  Mirror ANY change
	//! into it.  The duplication is deliberate; see the note above
	//! `HairScatteringBase::EvalPdf` for the 1-ULP measurement that
	//! forced it.
	bool ComputeApPDFBase(
		const Geom& G, const Scalar eta, const Scalar h,
		const Scalar sigmaProxy, Scalar apPDF[kPMax + 1]
		)
	{
		Scalar ap[kPMax + 1];
		ComputeAp( G.cosThetaO, eta, h, exp( -sigmaProxy * G.absorbLen ), ap );

		Scalar sum = 0;
		for( int p = 0; p <= kPMax; p++ ) {
			if( !( ap[p] > 0 ) || !RISE::IsFiniteDouble( ap[p] ) ) {
				ap[p] = 0;
			}
			sum += ap[p];
		}
		if( !( sum > 0 ) ) {
			return false;
		}
		for( int p = 0; p <= kPMax; p++ ) {
			apPDF[p] = ap[p] / sum;
		}
		return true;
	}

	//! The medulla-on twin of `ComputeApPDFBase` -- keep the two in
	//! lockstep (see the warning on that one).
	//!
	//! FUTURE CLEANUP, deliberately not taken here: a
	//! `template< bool kMedulla >` on this function plus `EvalFsum` /
	//! `EvalPdf` would give the medulla-off instantiation the same
	//! 4-entry arrays and compile-time loop bounds the pre-Phase-3 code
	//! had, from ONE source body -- removing the hand-policed mirror.
	//! It is not done in this slice because the whole point of the
	//! duplication is that the codegen here is measurably fragile, and
	//! re-deriving bit-identity through a template instantiation is a
	//! change that deserves its own verification pass rather than
	//! riding along with the feature.
	bool ComputeApPDF(
		const Geom& G, const Medulla& M, const bool bMedulla,
		const Scalar eta, const Scalar h,
		const Scalar sigmaProxy, Scalar apPDF[kNumLobes]
		)
	{
		Scalar ap[kNumLobes];
		ComputeApWithMedulla( G.cosThetaO, eta, h, exp( -sigmaProxy * G.absorbLen ),
		                      M, bMedulla, ap );

		// Compile-time-bounded base loop plus an appended medulla tail
		// -- same reason as DotLobes above.
		Scalar sum = 0;
		for( int p = 0; p <= kPMax; p++ ) {
			if( !( ap[p] > 0 ) || !RISE::IsFiniteDouble( ap[p] ) ) {
				ap[p] = 0;
			}
			sum += ap[p];
		}
		if( bMedulla ) {
			for( int p = kLobeTTs; p <= kLobeTRTs; p++ ) {
				if( !( ap[p] > 0 ) || !RISE::IsFiniteDouble( ap[p] ) ) {
					ap[p] = 0;
				}
				sum += ap[p];
			}
		}
		if( !( sum > 0 ) ) {
			return false;
		}
		for( int p = 0; p <= kPMax; p++ ) {
			apPDF[p] = ap[p] / sum;
		}
		if( bMedulla ) {
			apPDF[kLobeTTs]  = ap[kLobeTTs]  / sum;
			apPDF[kLobeTRTs] = ap[kLobeTRTs] / sum;
		}
		return true;
	}

	//! Cuticle-tilt rotation of the OUTGOING longitudinal angle for one
	//! scattering order.  `sin2kAlpha[k] / cos2kAlpha[k]` hold
	//! sin/cos(2^k * alpha), so the three lobes get their offsets
	//!
	//!     p = 0 (R)    theta_o - 2 alpha
	//!     p = 1 (TT)   theta_o + alpha
	//!     p = 2 (TRT)  theta_o + 4 alpha
	//!
	//! from a single sin/cos pair (PBRT-identical; the classic
	//! Marschner "-alpha / +alpha/2 / +3alpha/2" figures describe the
	//! resulting HIGHLIGHT separation, not these rotation amounts).
	//! p >= kPMax (the residual lobe) is untilted.
	//!
	//! THE SINGLE SOURCE OF THIS ROTATION.  Both `LobeWeights` (the
	//! evaluation side) and `HairSPF::DoScatter` (the sampling side)
	//! call it, so the two cannot drift in sign -- which is exactly the
	//! bug a duplicated copy invites, and which
	//! `RunCuticleTiltDirection` in HairBSDFTest.cpp pins.
	inline void ApplyLobeTilt(
		const int p, const Scalar sin2kAlpha[3], const Scalar cos2kAlpha[3],
		const Scalar sinThetaO, const Scalar cosThetaO,
		Scalar& sinThetaOp, Scalar& cosThetaOp
		)
	{
		if( p == 0 ) {
			sinThetaOp = sinThetaO * cos2kAlpha[1] - cosThetaO * sin2kAlpha[1];
			cosThetaOp = cosThetaO * cos2kAlpha[1] + sinThetaO * sin2kAlpha[1];
		} else if( p == 1 ) {
			sinThetaOp = sinThetaO * cos2kAlpha[0] + cosThetaO * sin2kAlpha[0];
			cosThetaOp = cosThetaO * cos2kAlpha[0] - sinThetaO * sin2kAlpha[0];
		} else if( p == 2 ) {
			sinThetaOp = sinThetaO * cos2kAlpha[2] + cosThetaO * sin2kAlpha[2];
			cosThetaOp = cosThetaO * cos2kAlpha[2] - sinThetaO * sin2kAlpha[2];
		} else {
			sinThetaOp = sinThetaO;
			cosThetaOp = cosThetaO;
		}
		// The tilt can push cos past the pole; reflect it back.
		cosThetaOp = fabs( cosThetaOp );
	}

	//! Achromatic per-order weights: w[p] = Mp_p * Np_p for p < kPMax,
	//! w[kPMax] = Mp_kPMax / (2 pi) for the uniform-azimuth residual.
	//!
	//! BOTH the BCSDF and the sampling pdf are dot products of this
	//! vector with an attenuation vector -- fsum = dot(w, ap) and
	//! pdf = dot(w, apPDF).  That identity is what makes Scatter / Pdf /
	//! value consistent term-for-term.
	void LobeWeightsBase(
		const HairResolvedParams& R, const Geom& G,
		const Scalar sinThetaI, const Scalar cosThetaI, const Scalar phi,
		Scalar w[kPMax + 1]
		)
	{
		for( int p = 0; p < kPMax; p++ ) {
			Scalar sinThetapO, cosThetapO;
			ApplyLobeTilt( p, R.sin2kAlpha, R.cos2kAlpha,
			               G.sinThetaO, G.cosThetaO, sinThetapO, cosThetapO );

			w[p] = Mp( cosThetaI, cosThetapO, sinThetaI, sinThetapO, R.v[p] ) *
			       Np( phi, p, R.s, G.gammaO, G.gammaT );
		}
		w[kPMax] = Mp( cosThetaI, G.cosThetaO, sinThetaI, G.sinThetaO, R.v[kPMax] ) *
		           ( 1.0 / TWO_PI );
	}

	//! `LobeWeightsBase` plus the two Yan 2017 scattered lobes.  Only
	//! ever reached when the medulla is ON -- the off path calls the
	//! base directly; see the note above `HairScatteringBase::EvalPdf`.
	void LobeWeights(
		const HairResolvedParams& R, const Geom& G,
		const RISE::Implementation::HairMedullaProfile& prof,
		const Scalar sinThetaI, const Scalar cosThetaI, const Scalar phi,
		Scalar w[kNumLobes]
		)
	{
		LobeWeightsBase( R, G, sinThetaI, cosThetaI, phi, w );

		// --- the two medulla-scattered lobes (HairBSDF.h section 3b) --
		// Each sits on its PARENT order's cuticle tilt and exit
		// azimuth, with the longitudinal lobe broadened and the
		// azimuthal lobe replaced outright by the tabulated medulla
		// profile.  Both factors are normalised, so each lobe's
		// sphere integral is 1 and the furnace gate is untouched.
		w[kLobeTTs]  = 0;
		w[kLobeTRTs] = 0;
		if( prof.nBins == 0 ) {
			return;
		}
		for( int k = 0; k < 2; k++ ) {
			const int lobe   = k ? kLobeTRTs : kLobeTTs;
			const int parent = k ? 2 : 1;

			Scalar sinThetapO, cosThetapO;
			ApplyLobeTilt( parent, R.sin2kAlpha, R.cos2kAlpha,
			               G.sinThetaO, G.cosThetaO, sinThetapO, cosThetapO );

			const Scalar mp = Mp( cosThetaI, cosThetapO, sinThetaI, sinThetapO,
			                      MedullaLobeVariance( R, prof, parent ) );
			const Scalar np = prof.Eval( phi - PhiForP( parent, G.gammaO, G.gammaT ) );
			const Scalar v = mp * np;
			w[lobe] = ( v > 0 && RISE::IsFiniteDouble( v ) ) ? v : Scalar( 0 );
		}
	}

	//! Chiang's fit of the multiple-scattering reflectance denominator.
	inline Scalar ReflectanceDenom( const Scalar betaN )
	{
		const Scalar b = betaN;
		return 5.969 - 0.215 * b + 2.532 * Sqr( b ) - 10.73 * b * b * b +
		       5.574 * Sqr( Sqr( b ) ) + 0.245 * Sqr( Sqr( b ) ) * b;
	}

	//! Invert a target reflectance C to the absorption that produces it.
	Scalar SigmaAFromReflectance( const Scalar C, const Scalar D )
	{
		// C -> 0 sends sigma_a -> infinity; clamp to a very dark but
		// finite black (sigma_a ~ 1.4 at the floor for typical beta_n).
		const Scalar c = Clamp( C, 1e-3, 1.0 );
		return ( D > 1e-9 ) ? Sqr( log( c ) / D ) : Scalar( 0 );
	}

	//! Forward of SigmaAFromReflectance: the reflectance a given
	//! sigma_a implies.  Backs the OIDN albedo AOV for tiers 1 and 2.
	inline Scalar ReflectanceFromSigmaA( const Scalar sigmaA, const Scalar D )
	{
		return Clamp( exp( -SafeSqrt( sigmaA ) * D ), 0.0, 1.0 );
	}

	//! Piecewise-linear lookup with end clamping.  Matches the
	//! interpolation `BioSpecSkinSPF` builds over the very same arrays
	//! (`RISE_API_CreatePiecewiseLinearFunction1D` +
	//! `addControlPoints`), so hair and skin read the OMLC tables
	//! identically.
	Scalar Interp1D( const Scalar* xs, const Scalar* ys, const int n, const Scalar x )
	{
		if( n <= 0 )       { return 0; }
		if( x <= xs[0] )   { return ys[0]; }
		if( x >= xs[n-1] ) { return ys[n-1]; }

		int lo = 0, hi = n - 1;
		while( hi - lo > 1 ) {
			const int mid = ( lo + hi ) / 2;
			if( xs[mid] <= x ) { lo = mid; } else { hi = mid; }
		}
		const Scalar span = xs[hi] - xs[lo];
		if( !( span > 0 ) ) { return ys[lo]; }
		return ys[lo] + ( ( x - xs[lo] ) / span ) * ( ys[hi] - ys[lo] );
	}

	//! Colour tier 1: sigma_a from melanin concentrations, using the
	//! in-tree OMLC extinction tables (shared verbatim out of
	//! BioSpecSkinData.h -- NOT duplicated).  See HairBSDF.h section 4
	//! for the normalisation rationale.
	Scalar MelaninSigmaA( const Scalar ce, const Scalar cp, const Scalar nm )
	{
		const int nEu = (int)( sizeof( SkinData::omlc_eumelanin_wavelengths ) / sizeof( Scalar ) );
		const int nPh = (int)( sizeof( SkinData::omlc_pheomelanin_wavelengths ) / sizeof( Scalar ) );

		// Derived from the tables themselves rather than hard-coded, so
		// a future table correction re-normalises automatically.
		static const Scalar euScale = kEumelaninSigmaAAt550 /
			Interp1D( SkinData::omlc_eumelanin_wavelengths,
			          SkinData::omlc_eumelanin_ext_mgml, nEu, 550.0 );
		static const Scalar phScale = kPheomelaninSigmaAAt550 /
			Interp1D( SkinData::omlc_pheomelanin_wavelengths,
			          SkinData::omlc_pheomelanin_ext_mgml, nPh, 550.0 );

		Scalar sigma = 0;
		if( ce > 0 ) {
			sigma += ce * euScale * Interp1D( SkinData::omlc_eumelanin_wavelengths,
			                                  SkinData::omlc_eumelanin_ext_mgml, nEu, nm );
		}
		if( cp > 0 ) {
			sigma += cp * phScale * Interp1D( SkinData::omlc_pheomelanin_wavelengths,
			                                  SkinData::omlc_pheomelanin_ext_mgml, nPh, nm );
		}
		return sigma;
	}

	//! Fibre-frame components of a world direction.  (u, v, w) is
	//! right-handed, matching PBRT's (x, y, z) shading frame.
	inline void ToFibreFrame( const Vector3& d, const OrthonormalBasis3D& onb, Scalar out[3] )
	{
		out[0] = Vector3Ops::Dot( d, onb.u() );
		out[1] = Vector3Ops::Dot( d, onb.v() );
		out[2] = Vector3Ops::Dot( d, onb.w() );
	}
}

//////////////////////////////////////////////////////////////////////
//  HairScatteringBase
//////////////////////////////////////////////////////////////////////

const Scalar HairScatteringBase::kMinBeta = 0.05;
const Scalar HairScatteringBase::kMaxBeta = 1.0;

const Scalar HairScatteringBase::kMaxMedullaRatio        = 0.95;
const Scalar HairScatteringBase::kDefaultMedullaScatter  = 0.5;
const Scalar HairScatteringBase::kDefaultMedullaG        = 0.4;

HairScatteringBase::HairScatteringBase( const HairPainters& p ) :
  pEumelanin( p.eumelanin ),
  pPheomelanin( p.pheomelanin ),
  pSigmaA( p.sigma_a ),
  pColor( p.color ),
  pBetaM( p.beta_m ),
  pBetaN( p.beta_n ),
  pAlpha( p.alpha ),
  pIOR( p.ior ),
  pMedullaRatio( p.medulla_ratio ),
  pMedullaScatter( p.medulla_scatter ),
  pMedullaG( p.medulla_g ),
  bColorTierValid( p.ActiveColorTierCount() == 1 )
{
	if( !bColorTierValid ) {
		GlobalLog()->PrintEx( eLog_Error,
			"HairBSDF: exactly one colour tier (melanin | sigma_a | color) must be bound, %d were; "
			"falling back to a uniform mid-brown sigma_a", p.ActiveColorTierCount() );
	}
	if( !pBetaM || !pBetaN || !pAlpha || !pIOR ) {
		GlobalLog()->PrintEasyError(
			"HairBSDF: beta_m, beta_n, alpha and ior are all mandatory and must be non-null" );
	}

	if( pEumelanin )   { pEumelanin->addref(); }
	if( pPheomelanin ) { pPheomelanin->addref(); }
	if( pSigmaA )      { pSigmaA->addref(); }
	if( pColor )       { pColor->addref(); }
	if( pBetaM )       { pBetaM->addref(); }
	if( pBetaN )       { pBetaN->addref(); }
	if( pAlpha )       { pAlpha->addref(); }
	if( pIOR )         { pIOR->addref(); }
	// The three medulla slots are OPTIONAL -- a null pointer is the
	// (default) "no medulla" configuration, not an error.
	if( pMedullaRatio )   { pMedullaRatio->addref(); }
	if( pMedullaScatter ) { pMedullaScatter->addref(); }
	if( pMedullaG )       { pMedullaG->addref(); }
}

HairScatteringBase::~HairScatteringBase()
{
	safe_release( pEumelanin );
	safe_release( pPheomelanin );
	safe_release( pSigmaA );
	safe_release( pColor );
	safe_release( pBetaM );
	safe_release( pBetaN );
	safe_release( pAlpha );
	safe_release( pIOR );
	safe_release( pMedullaRatio );
	safe_release( pMedullaScatter );
	safe_release( pMedullaG );
}

void HairScatteringBase::Resolve( const RayIntersectionGeometric& ri, Resolved& R ) const
{
	// Near-field offset: PBRT's Curve convention, h = 2v - 1.
	//
	// Clamped just SHORT of the fibre edge.  At |h| == 1 exactly,
	// cos(gamma_o) == 0, so the Fresnel argument collapses to 0 and
	// F == 1: the fibre turns into a colourless white mirror (ap[0] == 1,
	// every transmissive order == 0).  That is the correct limit for a
	// grazing edge ray, but it is a TRAP for the common misconfiguration:
	// geometry with no UV channel leaves ptCoord == (0, 0), which sends
	// h == -1 over the WHOLE surface and renders the hair as flat white.
	// kHEdge keeps the model just inside the edge so such a hit still
	// carries colour, at the cost of a physically irrelevant sliver.
	R.h = Clamp( 2.0 * ri.ptCoord.y - 1.0, -kHEdge, kHEdge );

	R.betaM  = Clamp( pBetaM ? pBetaM->GetValuesAt( ri ).v[0] : 0.3, kMinBeta, kMaxBeta );
	R.betaN  = Clamp( pBetaN ? pBetaN->GetValuesAt( ri ).v[0] : 0.3, kMinBeta, kMaxBeta );

	const Scalar alphaDeg = pAlpha ? pAlpha->GetValuesAt( ri ).v[0] : 2.0;

	R.etaRef = pIOR ? pIOR->GetValuesAt( ri ).v[0] : 1.55;
	// A fibre IOR of exactly 1 (or below) collapses the refraction
	// geometry (etap -> 0) and would divide by zero; a hair fibre is a
	// dielectric by construction.
	if( !( R.etaRef > 1.0 + 1e-6 ) ) {
		R.etaRef = 1.55;
	}

	// Chiang's beta_m -> longitudinal-variance remap, and the per-lobe
	// variance ladder (TT is sharper, TRT broader).
	const Scalar bm = R.betaM;
	const Scalar bm20 = pow( bm, 20.0 );
	R.v[0] = Sqr( 0.726 * bm + 0.812 * Sqr( bm ) + 3.7 * bm20 );
	R.v[1] = 0.25 * R.v[0];
	R.v[2] = 4.0  * R.v[0];
	R.v[3] = R.v[2];		// residual lobe reuses the TRT variance

	// Chiang's beta_n -> logistic-scale remap.  0.626657069 = sqrt(pi/8).
	const Scalar bn = R.betaN;
	R.s = 0.626657069 * ( 0.265 * bn + 1.194 * Sqr( bn ) + 5.372 * pow( bn, 22.0 ) );
	if( !( R.s > 0 ) || !RISE::IsFiniteDouble( R.s ) ) {
		R.s = 1e-4;
	}

	// 2k-alpha cuticle-tilt recurrence (double-angle formulas):
	// sin2kAlpha[k] = sin(2^k * alpha), so ONE sin/cos pair serves the
	// -2 alpha / +alpha / +4 alpha lobe rotations ApplyLobeTilt applies.
	const Scalar aRad = alphaDeg * ( PI / 180.0 );
	R.sin2kAlpha[0] = sin( aRad );
	R.cos2kAlpha[0] = SafeSqrt( 1 - Sqr( R.sin2kAlpha[0] ) );
	for( int i = 1; i < kPMax; i++ ) {
		R.sin2kAlpha[i] = 2 * R.cos2kAlpha[i-1] * R.sin2kAlpha[i-1];
		R.cos2kAlpha[i] = Sqr( R.cos2kAlpha[i-1] ) - Sqr( R.sin2kAlpha[i-1] );
	}

	// --- Yan 2017 medulla (HairBSDF.h section 3b) --------------------
	// Everything above this line is the pre-Phase-3 resolution, byte
	// for byte.  Everything below only WRITES the new fields, so a
	// medulla-off hit reaches the model with exactly the parameters it
	// reached it with before Phase 3 existed.
	//
	// All three slots are read ACHROMATICALLY (`.v[0]`), never through
	// GetValueAtNM: section 3's wavelength-independent-pdf contract --
	// and with it the [0, 1] kray bound -- requires the medulla split
	// factor `q` to be the same number for every wavelength.
	R.medullaActive = false;
	R.kappa  = 0;
	R.sigmaM = 0;
	R.gHG    = 0;

	if( pMedullaRatio ) {
		const Scalar kappa = Clamp( pMedullaRatio->GetValuesAt( ri ).v[0], 0.0, kMaxMedullaRatio );
		if( kappa > 0 ) {
			const Scalar sigmaM = pMedullaScatter
				? pMedullaScatter->GetValuesAt( ri ).v[0]
				: kDefaultMedullaScatter;
			if( sigmaM > 0 && RISE::IsFiniteDouble( sigmaM ) ) {
				R.medullaActive = true;
				R.kappa  = kappa;
				R.sigmaM = sigmaM;
				// A |g| of exactly 1 is a delta phase function, which
				// the tabulated profile cannot represent.
				//
				// ! THE FINITENESS TEST IS NOT OPTIONAL, and `Clamp`
				// does not do it: `NaN < lo` and `NaN > hi` are both
				// false, so a NaN would pass straight through, poison
				// every stencil weight in `BuildStencil`, and make the
				// interpolated profile NaN.  `Eval` then returns 0, so
				// the two scattered lobes' WEIGHTS vanish while their
				// ATTENUATIONS (already taken out of TT/TRT by q) do
				// not -- silent energy loss rather than a visible NaN.
				//
				// ! ALSO NOTE THE TWO CLAMPS.  This one is the physical
				// validity range; the TABLE separately clamps to
				// +/- kHairMedullaGMax (0.8) in `BuildStencil`, so an
				// author writing `medulla_g 0.95` gets the 0.8 profile.
				// The chunk descriptor says so; this is where it
				// happens.
				const Scalar g = pMedullaG ? pMedullaG->GetValuesAt( ri ).v[0] : kDefaultMedullaG;
				R.gHG = RISE::IsFiniteDouble( g ) ? Clamp( g, -0.99, 0.99 ) : kDefaultMedullaG;
			}
		}
	}
}

void HairScatteringBase::SigmaARGB(
	const RayIntersectionGeometric& ri, const Scalar betaN, Scalar out[3]
	) const
{
	// Misconfigured tier set -> the mid-brown the constructor's error
	// message PROMISES.  Checked first (and as one cached bool) so the
	// hot path does not re-derive the tier count from four pointers.
	if( !bColorTierValid ) {
		out[0] = out[1] = out[2] = kFallbackSigmaA;
		return;
	}
	if( pColor ) {														// tier 3
		const RISEPel C = pColor->GetColor( ri );
		const Scalar D = ReflectanceDenom( betaN );
		for( int c = 0; c < 3; c++ ) {
			out[c] = SigmaAFromReflectance( C[(unsigned int)c], D );
		}
		return;
	}
	if( pSigmaA ) {														// tier 2
		const ScalarTriple t = pSigmaA->GetValuesAt( ri );
		for( int c = 0; c < 3; c++ ) {
			out[c] = t.v[c] > 0 ? t.v[c] : Scalar( 0 );
		}
		return;
	}
	// tier 1 -- the only remaining possibility once bColorTierValid holds.
	const Scalar ce = pEumelanin   ? r_max( Scalar( 0 ), pEumelanin->GetValuesAt( ri ).v[0] )   : Scalar( 0 );
	const Scalar cp = pPheomelanin ? r_max( Scalar( 0 ), pPheomelanin->GetValuesAt( ri ).v[0] ) : Scalar( 0 );
	for( int c = 0; c < 3; c++ ) {
		out[c] = MelaninSigmaA( ce, cp, kRGBWavelengthsNM[c] );
	}
}

Scalar HairScatteringBase::SigmaANM(
	const RayIntersectionGeometric& ri, const Scalar betaN, const Scalar nm
	) const
{
	// Same gate as SigmaARGB -- see the note there.
	if( !bColorTierValid ) {
		return kFallbackSigmaA;
	}
	if( pColor ) {														// tier 3
		// The uplift is legitimate here and only here: `color` IS an
		// albedo-class colour, so the JH-uplifted spectrum is the
		// physically meaningful per-wavelength reflectance to invert.
		// Guarded at the authored-white corner (IPainter.h,
		// GuardedGetColorNM): an untinted `color` must invert to
		// SigmaA == 0 at every wavelength, matching SigmaARGB's white
		// case exactly.  Pre-Stage-C (2026-09-02,
		// docs/SPECTRAL_ILLUMINANT_CONVENTION.md) the raw uplift
		// collapsed white toward ~0 above ~620 nm (CoatedLayer.h
		// PassTransmittance); post-Stage-C it is 1 - epsilon instead.
		// Either way, without the guard the raw sample would invert to
		// a spuriously large SigmaA and darken hair red-hue only in
		// spectral renders.  Same near-white discontinuity precedent as
		// CoatedLayer.h's "KNOWN RESIDUAL" block (~line 331) for a
		// textured tint straddling white.
		return SigmaAFromReflectance( GuardedGetColorNM( *pColor, ri, nm ), ReflectanceDenom( betaN ) );
	}
	if( pSigmaA ) {														// tier 2
		const Scalar s = pSigmaA->GetValueAtNM( ri, nm );
		return s > 0 ? s : Scalar( 0 );
	}
	// tier 1 -- the only remaining possibility once bColorTierValid holds.
	const Scalar ce = pEumelanin   ? r_max( Scalar( 0 ), pEumelanin->GetValueAtNM( ri, nm ) )   : Scalar( 0 );
	const Scalar cp = pPheomelanin ? r_max( Scalar( 0 ), pPheomelanin->GetValueAtNM( ri, nm ) ) : Scalar( 0 );
	return MelaninSigmaA( ce, cp, nm );
}

Scalar HairScatteringBase::SigmaAProxy(
	const RayIntersectionGeometric& ri, const Scalar betaN
	) const
{
	Scalar s[3];
	SigmaARGB( ri, betaN, s );
	// MINIMUM, not average -- and the reason is an algebraic BOUND, not
	// a heuristic (HairBSDF.h section 3):
	//
	//   sigma_proxy = min_c sigma_c  =>  T_proxy = exp(-sigma_proxy L)
	//                                            >= T_lambda  for all lambda.
	//   Every ap[p] with p >= 1 -- ap[1] = (1-f)^2 T, ap[2] = ap[1] T f,
	//   and the residual ap[3] = ap[2] f T / (1 - T f) -- is strictly
	//   INCREASING in T, and ap[0] = f is independent of T.  Hence
	//   ap_lambda[p] <= ap_proxy[p] termwise.
	//
	//   pdf = dot(w, ap_proxy) / S with S = sum_p ap_proxy[p], so
	//     fsum_lambda / pdf = S * dot(w, ap_lambda) / dot(w, ap_proxy)
	//                       <= S.
	//   And S <= 1: at T = 1 the four orders telescope to
	//   f + (1-f)^2 + (1-f)^2 f + (1-f) f^2 = f + (1-f) = 1 exactly, and
	//   S falls monotonically as T drops.
	//
	//   => kray = fsum/pdf lies in [0, 1] PROVABLY, for every RGB channel
	//   and for any non-dispersive NM wavelength.  A maximum or a mean
	//   proxy gives no such bound: a wavelength darker than the proxy
	//   would land in an under-weighted transmissive lobe and fire.
	Scalar m = s[0];
	if( s[1] < m ) { m = s[1]; }
	if( s[2] < m ) { m = s[2]; }
	return m > 0 ? m : Scalar( 0 );
}

void HairScatteringBase::ReflectanceRGB(
	const RayIntersectionGeometric& ri, Scalar out[3]
	) const
{
	const Scalar betaN = Clamp( pBetaN ? pBetaN->GetValuesAt( ri ).v[0] : 0.3, kMinBeta, kMaxBeta );

	// --- the absorption-driven (coloured) part -----------------------
	if( bColorTierValid && pColor ) {
		// Tier 3 authored the target reflectance directly.
		const RISEPel C = pColor->GetColor( ri );
		for( int c = 0; c < 3; c++ ) {
			out[c] = Clamp( C[(unsigned int)c], 0.0, 1.0 );
		}
	} else {
		Scalar sigma[3];
		SigmaARGB( ri, betaN, sigma );		// honours bColorTierValid
		const Scalar D = ReflectanceDenom( betaN );
		for( int c = 0; c < 3; c++ ) {
			out[c] = ReflectanceFromSigmaA( sigma[c], D );
		}
	}

	// --- plus the ACHROMATIC R-lobe surface reflection ---------------
	// Chiang's C <-> sigma_a fit describes the light that goes THROUGH
	// the fibre and comes back out; it says nothing about the p = 0
	// surface highlight, which is white and present at every colour.
	// Without this term `albedo()` reports 0 for black hair -- and OIDN
	// divides the noisy radiance by the albedo AOV, so a 0 there turns
	// the whole specular highlight into unguided noise.
	//
	// F_avg is approximated by the NORMAL-INCIDENCE dielectric Fresnel
	// (eta-1)^2/(eta+1)^2 (0.0465 at eta = 1.55).  The true
	// hemispherical average for eta = 1.55 is ~0.09, so the normal-
	// incidence value understates it -- NOT the safe direction for a
	// denoiser prior (OIDN divides the noisy radiance BY this albedo, so
	// understating it INCREASES the pre-divided signal, the opposite of
	// conservative).  The real defense is that a constant, achromatic
	// offset is smooth and closed-form with no fit constants, which is
	// what the denoiser prior actually needs -- not that it is a
	// numerically safe bound.  Composited as
	// C + (1 - C) * F: the surface reflects F, and the remainder is what
	// the absorption model already accounts for.
	// Same guard as Resolve: an IOR of 1 or below is not a dielectric.
	const Scalar etaRaw = pIOR ? pIOR->GetValuesAt( ri ).v[0] : Scalar( 1.55 );
	const Scalar eta = ( etaRaw > 1.0 + 1e-6 ) ? etaRaw : Scalar( 1.55 );
	const Scalar fAvg = Sqr( ( eta - 1 ) / ( eta + 1 ) );
	for( int c = 0; c < 3; c++ ) {
		out[c] = Clamp( out[c] + ( 1 - out[c] ) * fAvg, 0.0, 1.0 );
	}
}

void HairScatteringBase::EvalFsum(
	const RayIntersectionGeometric& ri, const Resolved& R, const Vector3& wi,
	const bool bNM, const Scalar nm, Scalar out[3]
	) const
{
	out[0] = out[1] = out[2] = 0;

	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wiN = Vector3Ops::Normalize( wi );

	Scalar woL[3], wiL[3];
	ToFibreFrame( wo, ri.onb, woL );
	ToFibreFrame( wiN, ri.onb, wiL );

	// Spectral eta only matters for a dispersive `ior` painter; with the
	// (default) non-dispersive one this is bit-identical to R.etaRef,
	// making the evaluation geometry and the sampling geometry the same.
	Scalar eta = R.etaRef;
	if( bNM && pIOR ) {
		const Scalar e = pIOR->GetValueAtNM( ri, nm );
		if( e > 1.0 + 1e-6 ) {
			eta = e;
		}
	}

	Geom G;
	MakeGeom( R.h, eta, woL, G );

	const Scalar sinThetaI = Clamp( wiL[0], -1.0, 1.0 );
	const Scalar cosThetaI = SafeSqrt( 1 - Sqr( sinThetaI ) );
	const Scalar phiI = atan2( wiL[2], wiL[1] );

	// ---- MEDULLA OFF: the pre-Phase-3 body, verbatim -----------------
	// Deliberately a FULL COPY rather than a special case of the medulla
	// branch -- see the same note in `EvalPdf` below.  Any change here
	// MUST be mirrored there.
	if( !R.medullaActive ) {
		Scalar w[kPMax + 1];
		LobeWeightsBase( R, G, sinThetaI, cosThetaI, phiI - G.phiO, w );

		if( bNM ) {
			const Scalar sigma = SigmaANM( ri, R.betaN, nm );
			Scalar ap[kPMax + 1];
			ComputeAp( G.cosThetaO, eta, R.h, exp( -sigma * G.absorbLen ), ap );
			Scalar sum = 0;
			for( int p = 0; p <= kPMax; p++ ) {
				sum += w[p] * ap[p];
			}
			out[0] = ( sum > 0 && RISE::IsFiniteDouble( sum ) ) ? sum : Scalar( 0 );
			return;
		}

		Scalar sigma[3];
		SigmaARGB( ri, R.betaN, sigma );
		for( int c = 0; c < 3; c++ ) {
			Scalar ap[kPMax + 1];
			ComputeAp( G.cosThetaO, eta, R.h, exp( -sigma[c] * G.absorbLen ), ap );
			Scalar sum = 0;
			for( int p = 0; p <= kPMax; p++ ) {
				sum += w[p] * ap[p];
			}
			out[c] = ( sum > 0 && RISE::IsFiniteDouble( sum ) ) ? sum : Scalar( 0 );
		}
		return;
	}

	// ---- MEDULLA ON --------------------------------------------------
	Medulla M;
	MakeMedulla( R, G, M );

	RISE::Implementation::HairMedullaProfile prof;
	LoadMedullaProfile( M, prof );

	Scalar w[kNumLobes];
	LobeWeights( R, G, prof, sinThetaI, cosThetaI, phiI - G.phiO, w );

	if( bNM ) {
		const Scalar sigma = SigmaANM( ri, R.betaN, nm );
		Scalar ap[kNumLobes];
		ComputeApWithMedulla( G.cosThetaO, eta, R.h, exp( -sigma * G.absorbLen ), M, true, ap );
		const Scalar sum = DotLobes( w, ap, true );
		out[0] = ( sum > 0 && RISE::IsFiniteDouble( sum ) ) ? sum : Scalar( 0 );
		return;
	}

	Scalar sigma[3];
	SigmaARGB( ri, R.betaN, sigma );
	for( int c = 0; c < 3; c++ ) {
		Scalar ap[kNumLobes];
		ComputeApWithMedulla( G.cosThetaO, eta, R.h, exp( -sigma[c] * G.absorbLen ), M, true, ap );
		const Scalar sum = DotLobes( w, ap, true );
		out[c] = ( sum > 0 && RISE::IsFiniteDouble( sum ) ) ? sum : Scalar( 0 );
	}
}

Scalar HairScatteringBase::EvalPdf(
	const RayIntersectionGeometric& ri, const Resolved& R, const Vector3& wi
	) const
{
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Vector3 wiN = Vector3Ops::Normalize( wi );

	Scalar woL[3], wiL[3];
	ToFibreFrame( wo, ri.onb, woL );
	ToFibreFrame( wiN, ri.onb, wiL );

	Geom G;
	MakeGeom( R.h, R.etaRef, woL, G );

	const Scalar sinThetaI = Clamp( wiL[0], -1.0, 1.0 );
	const Scalar cosThetaI = SafeSqrt( 1 - Sqr( sinThetaI ) );
	const Scalar phiI = atan2( wiL[2], wiL[1] );

	// ---- MEDULLA OFF: the pre-Phase-3 body, verbatim -----------------
	// This branch is deliberately a FULL COPY rather than a special case
	// of the one below, and the duplication is the point.  The two are
	// algebraically identical, but merely widening the arrays from 4 to
	// 6 entries and threading a `Medulla` through changed how the
	// compiler contracted `w[p] * apPDF[p]` here -- 21 of the 204
	// group-15a goldens moved by 1 ULP, which is precisely the promise
	// HairBSDF.h section 3b makes and HairBSDFTest group 15a enforces.
	// (`EvalFsum` happened to survive the shared form, but is written
	// the same way anyway rather than relying on that luck.)
	// Any change here MUST be mirrored into the medulla branch below.
	if( !R.medullaActive ) {
		Scalar w[kPMax + 1];
		LobeWeightsBase( R, G, sinThetaI, cosThetaI, phiI - G.phiO, w );

		Scalar apPDF[kPMax + 1];
		if( !ComputeApPDFBase( G, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
			return 0;
		}

		Scalar pdf = 0;
		for( int p = 0; p <= kPMax; p++ ) {
			pdf += w[p] * apPDF[p];
		}
		return ( pdf > 0 && RISE::IsFiniteDouble( pdf ) ) ? pdf : Scalar( 0 );
	}

	// ---- MEDULLA ON --------------------------------------------------
	Medulla M;
	MakeMedulla( R, G, M );

	RISE::Implementation::HairMedullaProfile prof;
	LoadMedullaProfile( M, prof );

	Scalar w[kNumLobes];
	LobeWeights( R, G, prof, sinThetaI, cosThetaI, phiI - G.phiO, w );

	Scalar apPDF[kNumLobes];
	if( !ComputeApPDF( G, M, true, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
		return 0;
	}

	Scalar pdf = 0;
	for( int p = 0; p <= kPMax; p++ ) {
		pdf += w[p] * apPDF[p];
	}
	pdf += w[kLobeTTs]  * apPDF[kLobeTTs];
	pdf += w[kLobeTRTs] * apPDF[kLobeTRTs];
	return ( pdf > 0 && RISE::IsFiniteDouble( pdf ) ) ? pdf : Scalar( 0 );
}

//////////////////////////////////////////////////////////////////////
//  HairBRDF
//////////////////////////////////////////////////////////////////////

HairBRDF::HairBRDF( const HairPainters& p ) :
  HairScatteringBase( p )
{
}

HairBRDF::~HairBRDF()
{
}

RISEPel HairBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	Resolved R;
	Resolve( ri, R );

	Scalar fsum[3];
	EvalFsum( ri, R, vLightIn, false, 0, fsum );

	// The 1/|wi . N| reconciliation -- HairBSDF.h section 2.  NOTE this
	// is the SHADING cosine (PBRT's AbsCosTheta in the shading frame),
	// NOT cos(theta_i) of the fibre inclination: theta_i is measured
	// from the normal PLANE, so cos(theta_i) never vanishes where
	// |wi . N| does.  Applied only when the cosine is nonzero, exactly
	// as PBRT does; every consumer multiplies the same cosine straight
	// back, so the cancellation is exact and the product stays finite.
	const Scalar absCos = fabs( Vector3Ops::Dot(
		Vector3Ops::Normalize( vLightIn ), ri.onb.w() ) );
	if( absCos > 0 ) {
		const Scalar inv = 1.0 / absCos;
		return RISEPel( fsum[0] * inv, fsum[1] * inv, fsum[2] * inv );
	}
	return RISEPel( fsum[0], fsum[1], fsum[2] );
}

Scalar HairBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Resolved R;
	Resolve( ri, R );

	Scalar fsum[3];
	EvalFsum( ri, R, vLightIn, true, nm, fsum );

	const Scalar absCos = fabs( Vector3Ops::Dot(
		Vector3Ops::Normalize( vLightIn ), ri.onb.w() ) );
	return ( absCos > 0 ) ? fsum[0] / absCos : fsum[0];
}

RISEPel HairBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	Scalar C[3];
	ReflectanceRGB( ri, C );
	return RISEPel( C[0], C[1], C[2] );
}

void HairBRDF::TestApAndPathLength(
	const RayIntersectionGeometric& ri, const Scalar sigmaA,
	Scalar ap[4], Scalar& absorbLen
	) const
{
	// The header cannot name kPMax (it is file-local here), so the array
	// extent is spelled out there.  Keep the two in step.
	static_assert( kPMax + 1 == 4,
		"HairBRDF::TestApAndPathLength's declared ap[4] must match kPMax + 1" );

	Resolved R;
	Resolve( ri, R );

	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	Scalar woL[3];
	ToFibreFrame( wo, ri.onb, woL );

	Geom G;
	MakeGeom( R.h, R.etaRef, woL, G );

	absorbLen = G.absorbLen;
	ComputeAp( G.cosThetaO, R.etaRef, R.h, exp( -sigmaA * G.absorbLen ), ap );
}

void HairBRDF::TestMedullaAp(
	const RayIntersectionGeometric& ri, const Scalar sigmaA,
	Scalar ap[6], int& nLobes,
	Scalar& q1, Scalar& q2,
	Scalar& medullaB, Scalar& medullaTau, Scalar& medullaLongVariance
	) const
{
	static_assert( kNumLobes == 6,
		"HairBRDF::TestMedullaAp's declared ap[6] must match kNumLobes" );

	Resolved R;
	Resolve( ri, R );

	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	Scalar woL[3];
	ToFibreFrame( wo, ri.onb, woL );

	Geom G;
	MakeGeom( R.h, R.etaRef, woL, G );

	Medulla M;
	MakeMedulla( R, G, M );

	RISE::Implementation::HairMedullaProfile prof;
	LoadMedullaProfile( M, prof );

	nLobes = R.medullaActive ? kNumLobes : ( kPMax + 1 );
	q1 = M.q1;
	q2 = M.q2;
	medullaB = M.b;
	medullaTau = M.tau;
	medullaLongVariance = prof.longVariance;

	ComputeApWithMedulla( G.cosThetaO, R.etaRef, R.h, exp( -sigmaA * G.absorbLen ),
	                      M, R.medullaActive, ap );
}

void HairBRDF::TestApplyLobeTilt(
	const RayIntersectionGeometric& ri, const int p,
	const Scalar sinThetaO, const Scalar cosThetaO,
	Scalar& sinOut, Scalar& cosOut
	) const
{
	// Route through the REAL `Resolve()` -- the same per-hit resolution
	// the renderer calls -- so `sin2kAlpha` / `cos2kAlpha` come from this
	// material's actual bound alpha painter and the actual recurrence,
	// not a private re-derivation.  A corruption anywhere in that chain
	// (painter -> Resolve() -> sin2kAlpha/cos2kAlpha -> ApplyLobeTilt) is
	// therefore visible here, matching `TestApAndPathLength` above.
	Resolved R;
	Resolve( ri, R );
	ApplyLobeTilt( p, R.sin2kAlpha, R.cos2kAlpha, sinThetaO, cosThetaO, sinOut, cosOut );
}

void HairBRDF::TestSigmaA(
	const RayIntersectionGeometric& ri, const Scalar betaN,
	Scalar rgbOut[3], Scalar& nmOut, const Scalar nm
	) const
{
	SigmaARGB( ri, betaN, rgbOut );
	nmOut = SigmaANM( ri, betaN, nm );
}

//////////////////////////////////////////////////////////////////////
//  HairSPF
//////////////////////////////////////////////////////////////////////

HairSPF::HairSPF( const HairPainters& p ) :
  HairScatteringBase( p )
{
}

HairSPF::~HairSPF()
{
}

void HairSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& /*ior_stack*/
	) const
{
	DoScatter( ri, sampler, false, 0, scattered );
}

void HairSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& /*ior_stack*/
	) const
{
	DoScatter( ri, sampler, true, nm, scattered );
}

void HairSPF::DoScatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const bool bNM,
	const Scalar nm,
	ScatteredRayContainer& scattered
	) const
{
	Resolved R;
	Resolve( ri, R );

	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	Scalar woL[3];
	ToFibreFrame( wo, ri.onb, woL );

	Geom G;
	MakeGeom( R.h, R.etaRef, woL, G );

	// `Medulla` is six scalars, NOT the profile -- the profile is only
	// materialised below, and only if a scattered lobe is actually
	// drawn.  See the note on the struct.
	Medulla M;
	MakeMedulla( R, G, M );
	const int nLobes = R.medullaActive ? kNumLobes : ( kPMax + 1 );

	// The off path calls `ComputeApPDFBase` (the verbatim pre-Phase-3
	// helper), NOT the medulla-capable wrapper -- same bit-identity
	// discipline as `EvalPdf` / `EvalFsum` above.
	Scalar apPDF[kNumLobes];
	if( R.medullaActive ) {
		if( !ComputeApPDF( G, M, true, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
			return;
		}
	} else {
		if( !ComputeApPDFBase( G, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
			return;
		}
	}

	// --- choose the scattering order p by its apparent energy --------
	Scalar uc = Clamp( sampler.Get1D(), 0.0, 1.0 - 1e-9 );
	int p = -1;
	int lastPositive = -1;
	{
		Scalar cdf = 0;
		for( int i = 0; i < nLobes; i++ ) {
			if( !( apPDF[i] > 0 ) ) {
				continue;
			}
			lastPositive = i;
			if( uc < cdf + apPDF[i] ) {
				p = i;
				// Remap the consumed uniform back to [0,1) so the
				// azimuthal sample below can reuse it -- no extra
				// sampler dimension, matching the reference.
				uc = Clamp( ( uc - cdf ) / apPDF[i], 0.0, 1.0 - 1e-9 );
				break;
			}
			cdf += apPDF[i];
		}
	}
	if( p < 0 ) {
		// The CDF fell a rounding error short of 1.  Take the last
		// order that actually carries weight; ComputeApPDF guarantees
		// at least one does.
		if( lastPositive < 0 ) {
			return;
		}
		p = lastPositive;
		uc = 1.0 - 1e-9;
	}

	// --- tilt the outgoing longitudinal angle for this lobe ----------
	// SAME helper the evaluation side uses, so the sign convention is
	// structurally shared rather than duplicated.  A medulla-scattered
	// lobe borrows its PARENT order's tilt and variance (HairBSDF.h
	// section 3b); for every pre-Phase-3 index `tiltP == p`, so this
	// ternary is a no-op there.
	const int tiltP = ( p <= kPMax ) ? p : ( ( p == kLobeTTs ) ? 1 : 2 );

	// Only a scattered lobe needs the tabulated profile, so only a
	// scattered lobe pays for the interpolation.
	RISE::Implementation::HairMedullaProfile prof;
	prof.nBins = 0;
	prof.longVariance = 0;
	prof.mirrored = false;
	if( p > kPMax ) {
		LoadMedullaProfile( M, prof );
	}

	Scalar sinThetapO, cosThetapO;
	ApplyLobeTilt( tiltP, R.sin2kAlpha, R.cos2kAlpha,
	               G.sinThetaO, G.cosThetaO, sinThetapO, cosThetapO );

	// --- sample M_p exactly (d'Eon et al. 2013) ----------------------
	const Scalar u0 = sampler.Get1D();
	const Scalar u1 = sampler.Get1D();
	const Scalar vp = ( p <= kPMax ) ? R.v[p] : MedullaLobeVariance( R, prof, tiltP );
	const Scalar cosThetaSample = 1 + vp * log( r_max( u0, Scalar( 1e-5 ) ) +
	                                            ( 1 - u0 ) * exp( -2 / vp ) );
	const Scalar sinThetaSample = SafeSqrt( 1 - Sqr( cosThetaSample ) );
	const Scalar cosPhiSample = cos( TWO_PI * u1 );

	const Scalar sinThetaI = Clamp(
		-cosThetaSample * sinThetapO + sinThetaSample * cosPhiSample * cosThetapO,
		-1.0, 1.0 );
	const Scalar cosThetaI = SafeSqrt( 1 - Sqr( sinThetaI ) );

	// --- sample N_p exactly ------------------------------------------
	//   p <  kPMax   trimmed logistic around the ideal exit angle
	//   p == kPMax   uniform azimuth (the residual bucket)
	//   p >  kPMax   the tabulated medulla profile around the PARENT
	//                order's exit angle -- `Sample` draws from exactly
	//                the density `Eval` reports, which is what keeps
	//                the mixture's Scatter / Pdf / value consistent.
	const Scalar dphi = ( p < kPMax )
		? PhiForP( p, G.gammaO, G.gammaT ) + SampleTrimmedLogistic( uc, R.s, -PI, PI )
		: ( ( p == kPMax )
			? TWO_PI * uc
			: PhiForP( tiltP, G.gammaO, G.gammaT ) + prof.Sample( uc ) );

	const Scalar phiI = G.phiO + dphi;

	// --- rebuild the world-space direction ---------------------------
	const Vector3 wi = Vector3Ops::Normalize(
		  ri.onb.u() * sinThetaI
		+ ri.onb.v() * ( cosThetaI * cos( phiI ) )
		+ ri.onb.w() * ( cosThetaI * sin( phiI ) ) );

	if( !RISE::IsFiniteDouble( wi.x ) || !RISE::IsFiniteDouble( wi.y ) ||
	    !RISE::IsFiniteDouble( wi.z ) ) {
		return;
	}

	// --- pdf of the direction we just drew ---------------------------
	// Recomputed through the SAME LobeWeights/apPDF dot product that
	// `EvalPdf` uses, so `Pdf(ri, wi)` reproduces this value exactly --
	// the contract SPFPdfConsistencyTest and MIS both depend on.
	const Scalar pdf = EvalPdf( ri, R, wi );
	if( !( pdf > 0 ) ) {
		return;
	}

	// --- throughput --------------------------------------------------
	// kray = f_RISE * |wi . N| / pdf = fsum / pdf: the 1/|wi . N|
	// inside f_RISE and the integrator's |wi . N| cancel (HairBSDF.h
	// section 2), so the shading cosine never appears here at all.
	Scalar fsum[3];
	EvalFsum( ri, R, wi, bNM, nm, fsum );

	ScatteredRay hair;
	// All hair lobes share eRayReflection so ONE existing knob
	// (max_glossy_bounce) governs fibre depth; see HairBSDF.h section 5.
	hair.type = ScatteredRay::eRayReflection;
	hair.isDelta = false;
	hair.pdf = pdf;
	hair.ray.Set( ri.ptIntersection, wi );

	if( !( fsum[0] > 0 || fsum[1] > 0 || fsum[2] > 0 ) ) {
		return;
	}

	const Scalar invPdf = 1.0 / pdf;
	if( bNM ) {
		hair.krayNM = fsum[0] * invPdf;
	} else {
		hair.kray = RISEPel( fsum[0] * invPdf, fsum[1] * invPdf, fsum[2] * invPdf );
	}

	scattered.AddScatteredRay( hair );
}

Scalar HairSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& /*ior_stack*/
	) const
{
	Resolved R;
	Resolve( ri, R );
	// NOTE the parameter name: ISPF::Pdf's `wo` is the OUTGOING
	// scattered direction, which is the model's w_i.
	return EvalPdf( ri, R, wo );
}

Scalar HairSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar /*nm*/,
	const IORStack& ior_stack
	) const
{
	// Wavelength independent by construction -- HairBSDF.h section 3.
	return Pdf( ri, wo, ior_stack );
}

Scalar HairSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& /*ior_stack*/
	) const
{
	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;			// not one of ours; let the caller fall back
	}

	Resolved R;
	Resolve( ri, R );

	// The full-BSDF-ratio route (HairBSDF.h, EvaluateKrayNM doc): the
	// sampled lobe index p is NOT recoverable from
	// (ri, outDir, rayType) -- all four lobes carry the same type tag
	// and overlap in direction space -- so we re-evaluate the whole
	// mixture.  Exact, because `EvalPdf` is wavelength independent and
	// therefore reproduces the hero's sampling pdf bit-for-bit.
	const Scalar pdf = EvalPdf( ri, R, outDir );
	if( !( pdf > 0 ) ) {
		return 0;
	}

	Scalar fsum[3];
	EvalFsum( ri, R, outDir, true, nm, fsum );
	return fsum[0] / pdf;
}
