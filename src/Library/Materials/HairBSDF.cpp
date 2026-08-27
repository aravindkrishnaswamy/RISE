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

	//! Representative wavelengths the RGB path evaluates spectral
	//! quantities (the melanin curves) at.  RISE has no canonical
	//! RGB<->wavelength triple; these are the conventional ones and are
	//! used ONLY by colour tier 1, so RGB and spectral hair renders are
	//! driven by the same underlying curve rather than by two
	//! independently-fit constant sets.
	const Scalar	kRGBWavelengthsNM[3] = { 600.0, 550.0, 450.0 };

	//! Per-unit-concentration sigma_a at 550 nm that the OMLC melanin
	//! curves are normalised to.  These are PBRT-v4's green-channel
	//! `SigmaAFromConcentration` coefficients, which are the
	//! RGB-projection of the same OMLC spectroscopy; anchoring to them
	//! keeps `eumelanin 1.3` meaning "brown-black hair" exactly as it
	//! does in every other Chiang implementation, and makes a
	//! cross-check against a PBRT render an apples-to-apples test.
	const Scalar	kEumelaninSigmaAAt550   = 0.697;
	const Scalar	kPheomelaninSigmaAAt550 = 0.400;

	//! Used only if a caller misconfigures the colour tiers (an error
	//! is logged at construction).  Mid-brown.
	const Scalar	kFallbackSigmaA = 1.0;

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
			return x + 0.5 * ( -log( TWO_PI ) + log( 1 / x ) + 1 / ( 8 * x ) );
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
		const Scalar etap = ( g.cosThetaO > 1e-9 )
			? SafeSqrt( Sqr( eta ) - Sqr( g.sinThetaO ) ) / g.cosThetaO
			: eta;
		const Scalar sinGammaT = ( etap > 1e-9 ) ? Clamp( h / etap, -1.0, 1.0 ) : Scalar( 0 );
		g.cosGammaT = SafeSqrt( 1 - Sqr( sinGammaT ) );
		g.gammaT    = SafeASin( sinGammaT );

		g.absorbLen = ( g.cosThetaT > 1e-9 ) ? 2 * g.cosGammaT / g.cosThetaT : Scalar( 0 );
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
		const Scalar denom = 1 - T * f;
		ap[kPMax] = ( denom > 1e-12 ) ? ap[kPMax - 1] * f * T / denom : Scalar( 0 );
	}

	//! The ACHROMATIC lobe-selection PMF -- the sole reason `Pdf` is
	//! wavelength independent (HairBSDF.h section 3).  `sigmaProxy` is
	//! the component-wise minimum of the RGB sigma_a triple.
	//!
	//! Returns false when every order has zero apparent energy (the
	//! caller must then produce no sample / zero density).
	bool ComputeApPDF(
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

	//! Achromatic per-order weights: w[p] = Mp_p * Np_p for p < kPMax,
	//! w[kPMax] = Mp_kPMax / (2 pi) for the uniform-azimuth residual.
	//!
	//! BOTH the BCSDF and the sampling pdf are dot products of this
	//! vector with an attenuation vector -- fsum = dot(w, ap) and
	//! pdf = dot(w, apPDF).  That identity is what makes Scatter / Pdf /
	//! value consistent term-for-term.
	void LobeWeights(
		const HairResolvedParams& R, const Geom& G,
		const Scalar sinThetaI, const Scalar cosThetaI, const Scalar phi,
		Scalar w[kPMax + 1]
		)
	{
		for( int p = 0; p < kPMax; p++ ) {
			// Cuticle-tilt rotation of the outgoing longitudinal angle.
			// The 2k-alpha recurrence gives the -alpha / +alpha/2 /
			// +3alpha/2 lobe offsets without a trig call per lobe.
			Scalar sinThetapO, cosThetapO;
			if( p == 0 ) {
				sinThetapO = G.sinThetaO * R.cos2kAlpha[1] - G.cosThetaO * R.sin2kAlpha[1];
				cosThetapO = G.cosThetaO * R.cos2kAlpha[1] + G.sinThetaO * R.sin2kAlpha[1];
			} else if( p == 1 ) {
				sinThetapO = G.sinThetaO * R.cos2kAlpha[0] + G.cosThetaO * R.sin2kAlpha[0];
				cosThetapO = G.cosThetaO * R.cos2kAlpha[0] - G.sinThetaO * R.sin2kAlpha[0];
			} else {
				sinThetapO = G.sinThetaO * R.cos2kAlpha[2] + G.cosThetaO * R.sin2kAlpha[2];
				cosThetapO = G.cosThetaO * R.cos2kAlpha[2] - G.sinThetaO * R.sin2kAlpha[2];
			}
			// The tilt can push cos past the pole; reflect it back.
			cosThetapO = fabs( cosThetapO );

			w[p] = Mp( cosThetaI, cosThetapO, sinThetaI, sinThetapO, R.v[p] ) *
			       Np( phi, p, R.s, G.gammaO, G.gammaT );
		}
		w[kPMax] = Mp( cosThetaI, G.cosThetaO, sinThetaI, G.sinThetaO, R.v[kPMax] ) *
		           ( 1.0 / TWO_PI );
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

HairScatteringBase::HairScatteringBase( const HairPainters& p ) :
  pEumelanin( p.eumelanin ),
  pPheomelanin( p.pheomelanin ),
  pSigmaA( p.sigma_a ),
  pColor( p.color ),
  pBetaM( p.beta_m ),
  pBetaN( p.beta_n ),
  pAlpha( p.alpha ),
  pIOR( p.ior )
{
	if( p.ActiveColorTierCount() != 1 ) {
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
}

void HairScatteringBase::Resolve( const RayIntersectionGeometric& ri, Resolved& R ) const
{
	// Near-field offset: PBRT's Curve convention, h = 2v - 1.
	R.h = Clamp( 2.0 * ri.ptCoord.y - 1.0, -1.0, 1.0 );

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

	// 2k-alpha cuticle-tilt recurrence (double-angle formulas), so the
	// three lobes get their -alpha, +alpha/2, +3alpha/2 offsets from one
	// sin/cos pair.
	const Scalar aRad = alphaDeg * ( PI / 180.0 );
	R.sin2kAlpha[0] = sin( aRad );
	R.cos2kAlpha[0] = SafeSqrt( 1 - Sqr( R.sin2kAlpha[0] ) );
	for( int i = 1; i < kPMax; i++ ) {
		R.sin2kAlpha[i] = 2 * R.cos2kAlpha[i-1] * R.sin2kAlpha[i-1];
		R.cos2kAlpha[i] = Sqr( R.cos2kAlpha[i-1] ) - Sqr( R.sin2kAlpha[i-1] );
	}
}

void HairScatteringBase::SigmaARGB(
	const RayIntersectionGeometric& ri, const Scalar betaN, Scalar out[3]
	) const
{
	if( pColor ) {														// tier 3
		const RISEPel C = pColor->GetColor( ri );
		const Scalar D = ReflectanceDenom( betaN );
		for( unsigned int c = 0; c < 3; c++ ) {
			out[c] = SigmaAFromReflectance( C[c], D );
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
	if( pEumelanin || pPheomelanin ) {									// tier 1
		const Scalar ce = pEumelanin   ? r_max( Scalar( 0 ), pEumelanin->GetValuesAt( ri ).v[0] )   : Scalar( 0 );
		const Scalar cp = pPheomelanin ? r_max( Scalar( 0 ), pPheomelanin->GetValuesAt( ri ).v[0] ) : Scalar( 0 );
		for( int c = 0; c < 3; c++ ) {
			out[c] = MelaninSigmaA( ce, cp, kRGBWavelengthsNM[c] );
		}
		return;
	}
	out[0] = out[1] = out[2] = kFallbackSigmaA;
}

Scalar HairScatteringBase::SigmaANM(
	const RayIntersectionGeometric& ri, const Scalar betaN, const Scalar nm
	) const
{
	if( pColor ) {														// tier 3
		// The uplift is legitimate here and only here: `color` IS an
		// albedo-class colour, so the JH-uplifted spectrum is the
		// physically meaningful per-wavelength reflectance to invert.
		return SigmaAFromReflectance( pColor->GetColorNM( ri, nm ), ReflectanceDenom( betaN ) );
	}
	if( pSigmaA ) {														// tier 2
		const Scalar s = pSigmaA->GetValueAtNM( ri, nm );
		return s > 0 ? s : Scalar( 0 );
	}
	if( pEumelanin || pPheomelanin ) {									// tier 1
		const Scalar ce = pEumelanin   ? r_max( Scalar( 0 ), pEumelanin->GetValueAtNM( ri, nm ) )   : Scalar( 0 );
		const Scalar cp = pPheomelanin ? r_max( Scalar( 0 ), pPheomelanin->GetValueAtNM( ri, nm ) ) : Scalar( 0 );
		return MelaninSigmaA( ce, cp, nm );
	}
	return kFallbackSigmaA;
}

Scalar HairScatteringBase::SigmaAProxy(
	const RayIntersectionGeometric& ri, const Scalar betaN
	) const
{
	Scalar s[3];
	SigmaARGB( ri, betaN, s );
	// MINIMUM, not average: see HairBSDF.h section 3.  The minimum is
	// the maximum transmittance, so the resulting PMF over-weights the
	// transmissive lobes relative to every wavelength and f/pdf can
	// never blow up because a wavelength landed in a starved lobe.
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

	if( pColor ) {
		// Tier 3 authored the target reflectance directly.
		const RISEPel C = pColor->GetColor( ri );
		for( unsigned int c = 0; c < 3; c++ ) {
			out[c] = Clamp( C[c], 0.0, 1.0 );
		}
		return;
	}

	Scalar sigma[3];
	SigmaARGB( ri, betaN, sigma );
	const Scalar D = ReflectanceDenom( betaN );
	for( int c = 0; c < 3; c++ ) {
		out[c] = ReflectanceFromSigmaA( sigma[c], D );
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

	Scalar w[kPMax + 1];
	LobeWeights( R, G, sinThetaI, cosThetaI, phiI - G.phiO, w );

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

	Scalar w[kPMax + 1];
	LobeWeights( R, G, sinThetaI, cosThetaI, phiI - G.phiO, w );

	Scalar apPDF[kPMax + 1];
	if( !ComputeApPDF( G, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
		return 0;
	}

	Scalar pdf = 0;
	for( int p = 0; p <= kPMax; p++ ) {
		pdf += w[p] * apPDF[p];
	}
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

	// The 1/|cos theta_i| reconciliation -- HairBSDF.h section 2.
	// Applied only when the cosine is nonzero, exactly as PBRT does;
	// every consumer multiplies the same cosine straight back, so the
	// cancellation is exact and the product stays finite.
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

	Scalar apPDF[kPMax + 1];
	if( !ComputeApPDF( G, R.etaRef, R.h, SigmaAProxy( ri, R.betaN ), apPDF ) ) {
		return;
	}

	// --- choose the scattering order p by its apparent energy --------
	Scalar uc = Clamp( sampler.Get1D(), 0.0, 1.0 - 1e-9 );
	int p = -1;
	int lastPositive = -1;
	{
		Scalar cdf = 0;
		for( int i = 0; i <= kPMax; i++ ) {
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
	Scalar sinThetapO, cosThetapO;
	if( p == 0 ) {
		sinThetapO = G.sinThetaO * R.cos2kAlpha[1] - G.cosThetaO * R.sin2kAlpha[1];
		cosThetapO = G.cosThetaO * R.cos2kAlpha[1] + G.sinThetaO * R.sin2kAlpha[1];
	} else if( p == 1 ) {
		sinThetapO = G.sinThetaO * R.cos2kAlpha[0] + G.cosThetaO * R.sin2kAlpha[0];
		cosThetapO = G.cosThetaO * R.cos2kAlpha[0] - G.sinThetaO * R.sin2kAlpha[0];
	} else if( p == 2 ) {
		sinThetapO = G.sinThetaO * R.cos2kAlpha[2] + G.cosThetaO * R.sin2kAlpha[2];
		cosThetapO = G.cosThetaO * R.cos2kAlpha[2] - G.sinThetaO * R.sin2kAlpha[2];
	} else {
		sinThetapO = G.sinThetaO;
		cosThetapO = G.cosThetaO;
	}
	cosThetapO = fabs( cosThetapO );

	// --- sample M_p exactly (d'Eon et al. 2013) ----------------------
	const Scalar u0 = sampler.Get1D();
	const Scalar u1 = sampler.Get1D();
	const Scalar vp = R.v[p];
	const Scalar cosThetaSample = 1 + vp * log( r_max( u0, Scalar( 1e-5 ) ) +
	                                            ( 1 - u0 ) * exp( -2 / vp ) );
	const Scalar sinThetaSample = SafeSqrt( 1 - Sqr( cosThetaSample ) );
	const Scalar cosPhiSample = cos( TWO_PI * u1 );

	const Scalar sinThetaI = Clamp(
		-cosThetaSample * sinThetapO + sinThetaSample * cosPhiSample * cosThetapO,
		-1.0, 1.0 );
	const Scalar cosThetaI = SafeSqrt( 1 - Sqr( sinThetaI ) );

	// --- sample N_p exactly (trimmed logistic, or uniform residual) --
	const Scalar dphi = ( p < kPMax )
		? PhiForP( p, G.gammaO, G.gammaT ) + SampleTrimmedLogistic( uc, R.s, -PI, PI )
		: TWO_PI * uc;

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
	// kray = f_RISE * |cos theta_i| / pdf = fsum / pdf: the 1/|cos|
	// inside f_RISE and the integrator's |cos| cancel (HairBSDF.h
	// section 2), so the cosine never appears here at all.
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
