//////////////////////////////////////////////////////////////////////
//
//  FabricBRDF.cpp - Closed-form Charlie-sheen-over-substrate response
//    for `fabric_material`.  See FabricBRDF.h for the model, the
//    roughness floor's measurement, and the weave-rotation contract.
//
//    The D / Lambda / V helpers are CharlieSheen.h's, unchanged and
//    shared with SheenBRDF / SheenSPF, so a coefficient drift in one
//    site cannot silently desync the lobe across materials.  The E and
//    EHatMean tables are SheenDirectionalAlbedo.h's, baked from the very
//    same D / V.  (A third table, S, existed until round 5 and is
//    retired -- see that header.)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FabricBRDF.h"
#include "CharlieSheen.h"
#include "SheenDirectionalAlbedo.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

const Scalar FabricBRDF::kMinSheenAlpha = Scalar( 0.04 );
const Scalar FabricBRDF::kMaxSheenAlpha = Scalar( 1.0 );

namespace
{
	//! The ray-facing shading normal.  Identical construction to
	//! SheenBRDF::value's back-face flip and to FabricSPF's, so
	//! evaluator and sampler share one frame.
	inline RISE::Vector3 RayFacingNormal( const RISE::RayIntersectionGeometric& ri )
	{
		const bool bFrontFace = RISE::Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
		return bFrontFace ? ri.onb.w() : -ri.onb.w();
	}

	//! Ray-anchored geometric normal for the horizon gate.  Degenerate
	//! `vGeomNormal` falls back to the shading normal, making the gate a
	//! no-op (matches SheenBRDF / SheenSPF / CoatedSPF).
	inline RISE::Vector3 GeomNormal( const RISE::RayIntersectionGeometric& ri, const RISE::Vector3& n )
	{
		const RISE::Vector3& raw = ( RISE::Vector3Ops::SquaredModulus( ri.vGeomNormal ) > RISE::Scalar(1e-12) )
			? ri.vGeomNormal : n;
		return ( RISE::Vector3Ops::Dot( raw, ri.ray.Dir() ) < 0 ) ? raw : -raw;
	}
}

FabricBRDF::FabricBRDF(
	const IBSDF& base,
	const IPainter& sheenColor,
	const IScalarPainter& sheenRoughness,
	const IScalarPainter& weaveRotation,
	const bool baseScattersFullSphere
	) :
  pBase( &base ),
  pSheenColor( &sheenColor ),
  pSheenRoughness( &sheenRoughness ),
  pWeaveRotation( &weaveRotation ),
  bBaseFullSphere( baseScattersFullSphere )
{
	pBase->addref();
	pSheenColor->addref();
	pSheenRoughness->addref();
	pWeaveRotation->addref();
}

FabricBRDF::~FabricBRDF()
{
	safe_release( pWeaveRotation );
	safe_release( pSheenRoughness );
	safe_release( pSheenColor );
	safe_release( pBase );
}

void FabricBRDF::SetSheenColor( const IPainter& v )
{
	v.addref();
	safe_release( pSheenColor );
	pSheenColor = &v;
}

void FabricBRDF::SetSheenRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pSheenRoughness );
	pSheenRoughness = &v;
}

void FabricBRDF::SetWeaveRotation( const IScalarPainter& v )
{
	v.addref();
	safe_release( pWeaveRotation );
	pWeaveRotation = &v;
}

void FabricBRDF::ResolveFabric( const RayIntersectionGeometric& ri, const Scalar nm, FabricParams& out ) const
{
	// ACHROMATIC in both regimes -- see the long note on the
	// declaration.  `alpha` feeds the mixture density, so a
	// wavelength-dependent value would desync Scatter's stored hero pdf
	// from a companion-wavelength Pdf() call.
	const Scalar rawAlpha = pSheenRoughness->GetValuesAt( ri ).v[0];
	// NaN GUARD.  `r_min`/`r_max` are comparison macros, so a NaN
	// `rawAlpha` survives BOTH clamps (every comparison against NaN is
	// false) and propagates through E -> the whole BRDF, turning a
	// mis-authored painter into NaN pixels rather than a visibly wrong
	// but finite surface.  `!(x >= lo)` is true for NaN as well as for
	// below-range, so this one test covers both and needs no isnan.
	// (The repo has this class of hole broadly; fixing it HERE is cheap
	// because there is exactly one place alpha enters.)
	out.alpha = !( rawAlpha >= kMinSheenAlpha ) ? kMinSheenAlpha
	          : r_min( kMaxSheenAlpha, rawAlpha );

	// `m` comes from the AUTHORED RGB triple in BOTH regimes.  See the
	// long note on the declaration in FabricBRDF.h: the base scaling and
	// the lobe-selection weight are achromatic by construction, and
	// making them wavelength-dependent would desync Scatter's stored
	// hero pdf from a companion-wavelength Pdf() call.
	const RISEPel rgb = pSheenColor->GetColor( ri );
	out.m = r_min( Scalar(1), r_max( Scalar(0), ColorMath::MaxValue( rgb ) ) );

	if( nm < 0 ) {
		out.tint   = rgb;
		out.tintNM = 0;
	} else {
		out.tint   = RISEPel( 0, 0, 0 );
		// The white guard, INLINED against the `rgb` sample already in
		// hand.  `GuardedGetColorNM` would re-sample `GetColor(ri)` just
		// to run `IsUntintedWhite` on it, which on this path is a second
		// painter evaluation for a value we already have.  Semantics are
		// identical -- same predicate, same fallback (IPainter.h).
		out.tintNM = IsUntintedWhite( rgb )
			? Scalar(1)
			: pSheenColor->GetColorNM( ri, nm );
	}

	// requireSingle on the descriptor, and ACHROMATIC in both regimes:
	// this angle picks the FRAME the substrate's Pdf is evaluated in, so
	// a wavelength-dependent value would mean the sampler and the
	// density disagree about the tangent basis.  See the declaration.
	out.weaveAngle = pWeaveRotation->GetValuesAt( ri ).v[0];
}

Scalar FabricBRDF::SheenTransmit( const Scalar alpha, const Scalar m, const Scalar cosTheta )
{
	// `Ehat = min(E, 1)`, and the clamp is LOAD-BEARING, not defensive.
	// With the E table's cosTheta axis warped toward grazing
	// (2026-09-02) the resolved lobe reaches E = 1.152 even above the
	// roughness floor, inside the sliver cosTheta < ~0.03 -- so an
	// unclamped `1 - m*E` really would go negative there and the base
	// term really would subtract energy.  The floor keeps E <= 1 for
	// cosTheta >= 0.03; this keeps the rest bounded.  See the banner.
	const Scalar eHat = r_min( Scalar(1), SheenDirectionalAlbedo::E( alpha, cosTheta ) );
	return Scalar(1) - r_min( Scalar(1), m * eHat );
}

Scalar FabricBRDF::SheenNormaliser( const Scalar alpha,
                                    const Scalar cosThetaV, const Scalar cosThetaL )
{
	// RAW E on both arms, and a MAX rather than either arm alone: the
	// result must be symmetric under an l/v swap or the BRDF stops being
	// reciprocal, which is the property the whole round-5 form exists to
	// keep.  Exactly 1 -- i.e. bit-identically absent -- wherever the
	// lobe is already energy-bounded, so nothing outside the grazing
	// sliver moves.
	const Scalar eV = SheenDirectionalAlbedo::E( alpha, cosThetaV );
	const Scalar eL = SheenDirectionalAlbedo::E( alpha, cosThetaL );
	return r_max( Scalar(1), r_max( eV, eL ) );
}

Scalar FabricBRDF::BaseScaling( const Scalar alpha, const Scalar m,
                                const Scalar cosThetaV, const Scalar cosThetaL )
{
	// The adding-doubling sum.  `1 - m*EhatMean` is bounded well away
	// from zero over the whole shipped table -- EhatMean runs 0.004766
	// (alpha 1e-3) to 0.299393 (alpha 1), so the denominator is >= 0.70
	// at m = 1 -- but the floor makes the division unconditionally safe
	// rather than safe-by-table-inspection, which is a property a future
	// re-bake should not be able to take away silently.
	const Scalar denom = r_max( Scalar(1e-6),
		Scalar(1) - r_min( Scalar(1), m * SheenDirectionalAlbedo::EHatMean( alpha ) ) );
	return SheenTransmit( alpha, m, cosThetaV )
	     * SheenTransmit( alpha, m, cosThetaL )
	     / denom;
}

Scalar FabricBRDF::SheenTransmitMean( const Scalar alpha, const Scalar m )
{
	// The hemispherical mean of SheenTransmit, which is what survives
	// the double integral in `hemisphericalAlbedo` -- see the long
	// derivation above that method.  `EHatMean` is already the mean of
	// the CLAMPED lobe (the table bakes min(E,1), not E), so this is the
	// exact companion of SheenTransmit and not an approximation of it.
	const Scalar meBar = m * SheenDirectionalAlbedo::EHatMean( alpha );
	return Scalar(1) - r_min( Scalar(1), meBar );
}

Scalar FabricBRDF::SheenSelectWeight( const Scalar alpha, const Scalar m, const Scalar cosThetaV )
{
	// `Ehat`, not raw E: inside the grazing sliver the raw lobe exceeds
	// 1, and a selection weight above 1 would starve the base branch
	// entirely at exactly the directions where the base still carries
	// most of the energy.
	//
	// THIS WEIGHT UNDER-SELECTS THE SHEEN BRANCH NEAR GRAZING, AND THAT
	// IS VARIANCE, NOT BIAS.  `w` is the sheen's TABULATED directional
	// albedo, while its actual share of rho at a near-grazing view can
	// be higher; the sampler then picks the base branch more often than
	// the energy split warrants, which shows up as extra noise on
	// silhouettes rather than as a wrong mean.  The estimator stays
	// unbiased for ANY w in (0,1) because `Scatter` reprices every
	// sample against the FULL mixture density (FabricSPF's
	// sample-then-reprice recipe), so `w` steers effort only.
	//
	// The endpoints are safe rather than merely unreached: w == 1
	// implies m*Ehat(v) == 1 implies SheenTransmit(v) == 0, so the base
	// term is EXACTLY zero and the branch it starves carries no energy;
	// w == 0 implies m == 0 implies an unlit sheen lobe.
	//
	// The gap was much larger before the E table's cosTheta floor
	// (SheenDirectionalAlbedo.h): the same under-read that broke the
	// energy identity also fed this weight, so `w` read 0.36 where the
	// sheen's true share was 61 %.  With the floor, `w` tracks the
	// share closely and no separate remedy is warranted -- a w_min
	// override would have to be mirrored in `Pdf` to keep
	// Scatter<->Pdf agreement, buying noise reduction at the cost of
	// the one invariant gate 6 exists to protect.
	const Scalar eHat = r_min( Scalar(1), SheenDirectionalAlbedo::E( alpha, cosThetaV ) );
	return r_min( Scalar(1), r_max( Scalar(0), m * eHat ) );
}

FabricBRDF::FabricTerms FabricBRDF::ComputeTerms(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const FabricParams& p ) const
{
	FabricTerms t;
	t.valid = false;
	t.sheen = 0;
	t.scaling = 0;

	const Vector3 n = RayFacingNormal( ri );
	const Vector3 l = Vector3Ops::Normalize( vLightIn );
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nDotL = Vector3Ops::Dot( n, l );
	const Scalar nDotV = Vector3Ops::Dot( n, v );

	// The VIEW must be on the shading-normal side.  `n` is the ray-facing
	// normal, so this is a degeneracy guard rather than a real gate.
	if( nDotV <= NEARZERO ) {
		return t;
	}

	if( nDotL <= NEARZERO )
	{
		// TRANSMISSION (R8 P1.1; see FabricBRDF.h's transmission
		// section for the derivation and the energy claim).
		//
		// BIT-IDENTICAL TO THE COMMITTED CODE when the substrate cannot
		// transmit: `bBaseFullSphere` is false for every Phase-1
		// substrate and for a `transmission none` weave, so the whole
		// block collapses to the original `nDotL <= NEARZERO -> return
		// t` early-out, with `t` still the all-zero, invalid record it
		// was initialised to.  Nothing below is reached, and no
		// substrate call is made.
		//
		// The `< -NEARZERO` band around the horizon is deliberate and
		// symmetric with the `<= NEARZERO` above: a direction sitting
		// exactly IN the surface plane belongs to neither lobe, and
		// admitting it would hand `BaseScaling` a `|n.l|` of ~0, where
		// `Ehat` is at its grazing peak, for a direction that carries no
		// cosine weight anyway.
		if( !bBaseFullSphere || !( nDotL < -NEARZERO ) ) {
			return t;
		}

		// NO GEOMETRIC-HORIZON GATE.  This IS the below-horizon
		// transport `ScattersFullSphere()` exists to admit -- the same
		// call `WeaveBRDF::ValueWithParams`'s transmit branch and
		// `HairBSDF` both make for the far side.
		//
		// `t.sheen` stays 0: the Charlie lobe is reflection-only, so it
		// contributes nothing here.  Leaving it at 0 (rather than
		// branching in the two `*WithParams` bodies) is what keeps the
		// RGB and NM twins TEXTUALLY PARALLEL -- `tint * t.sheen`
		// against `tintNMCapped * t.sheen`, both exactly zero -- which
		// is the structural half of the twin discipline those bodies
		// document.
		// The two crossings of the fuzz layer, over the one
		// adding-doubling normaliser.  Symmetric under an l/v swap by
		// construction (the arms exchange, `|.|` is even), which is the
		// cross-hemisphere reciprocity argument.
		t.scaling  = BaseScaling( p.alpha, p.m, nDotV, -nDotL );
		t.valid    = true;
		return t;
	}

	// Geometric-horizon gate, symmetric in l and v (so it cannot break
	// reciprocity).  Mirrors SheenBRDF::value's gate exactly; every
	// allowlisted substrate applies the same one internally, so
	// returning zero here loses nothing the base would have returned.
	const Vector3 geomN = GeomNormal( ri, n );
	if( Vector3Ops::Dot( l, geomN ) <= 0 || Vector3Ops::Dot( v, geomN ) <= 0 ) {
		return t;
	}

	const Vector3 h = Vector3Ops::Normalize( l + v );
	// `r_max` is a MACRO (math_utils.h), so passing a call expression
	// evaluates it TWICE.  Hoist the dot product into a local first.
	const Scalar nDotHRaw = Vector3Ops::Dot( n, h );
	const Scalar nDotH = r_max( Scalar(0), nDotHRaw );

	const Scalar D = CharlieSheen::D( p.alpha, nDotH );
	const Scalar V = CharlieSheen::V( p.alpha, nDotL, nDotV );

	// The symmetric normaliser: exactly 1 outside the grazing sliver, so
	// this division is bit-identically absent there.
	t.sheen   = D * V / SheenNormaliser( p.alpha, nDotV, nDotL );
	// The product of the two arms over `1 - m*EhatMean`.  Swapping l and
	// v exchanges the arms and leaves the product unchanged, which is
	// the reciprocity argument; the denominator is direction-independent
	// and therefore cannot break it.
	t.scaling = BaseScaling( p.alpha, p.m, nDotV, nDotL );
	t.valid   = true;
	return t;
}

//////////////////////////////////////////////////////////////////////
// value / valueNM, and their `*WithParams` forms.
//
// ONE geometry body (ComputeTerms) serves both colour regimes, so the
// RGB and spectral twins cannot drift on the geometry, the gates, the
// normaliser or the scaling -- the only thing that differs is WHICH
// tint multiplies the sheen term and WHICH substrate entry point
// supplies the base.  That is the structural half of
// docs/skills/audit-by-bug-pattern.md's RGB/NM discipline.
//
// The two lines below are kept TEXTUALLY PARALLEL, parenthesisation
// included: `tint * t.sheen` against `tintNM * t.sheen`.  Gate 8
// (spectral parity at an authored-white dye) measures their agreement
// in ulps, so an association difference here would surface there as a
// guard failure it is not.
//
// THE TRANSMISSION CASE NEEDS NO BRANCH HERE, and that is by design.
// `ComputeTerms` reports an opposite-hemisphere pair with `t.sheen == 0`
// and `t.scaling` carrying the two-crossing attenuation, so the SAME
// expression evaluates to `f_base(l,v) * scale(l,v)` -- exactly the form
// FabricBRDF.h's transmission section derives.  One body, both sides of
// the surface, no second place for the RGB and NM twins to drift.
//////////////////////////////////////////////////////////////////////

RISEPel FabricBRDF::ValueWithParams(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const FabricParams& p,
	const RayIntersectionGeometric& weaveRi ) const
{
	const FabricTerms t = ComputeTerms( vLightIn, ri, p );
	if( !t.valid ) {
		return RISEPel( 0, 0, 0 );
	}
	return p.tint * t.sheen + pBase->value( vLightIn, weaveRi ) * t.scaling;
}

Scalar FabricBRDF::ValueNMWithParams(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const FabricParams& p,
	const RayIntersectionGeometric& weaveRi ) const
{
	const FabricTerms t = ComputeTerms( vLightIn, ri, p );
	if( !t.valid ) {
		return 0;
	}

	// THE SPECTRAL TINT IS CAPPED AT THE ACHROMATIC ENERGY BUDGET.
	//
	// `m` is the RGB max3 and is what suppresses the base
	// (`t.scaling`), while `tintNM` is the hero-wavelength sample.  If a
	// painter's value at some wavelength exceeded its own RGB max3, the
	// sheen term would be added back at more than the base gave up, and
	// this single wavelength's rho could exceed 1 -- an over-unity BRDF
	// on the spectral pipe only, invisible to every RGB gate.
	//
	// `hemisphericalAlbedoNM` documents the same hazard and clamps its
	// RESULT; a BRDF cannot do that (the result is a density, not an
	// albedo), so the bound goes on the INPUT instead: no wavelength may
	// claim more of the sheen lobe than the achromatic budget `m` paid
	// for.  Where the two agree -- which is everywhere `GuardedGetColorNM`
	// returns its guarded 1.0 for an authored white, and everywhere a
	// painter's spectrum sits under its RGB envelope -- this is a
	// bit-identical no-op, so gate 8's spectral parity is unaffected.
	const Scalar tintNMCapped = r_min( p.tintNM, p.m );
	return tintNMCapped * t.sheen + pBase->valueNM( vLightIn, weaveRi, nm ) * t.scaling;
}

RISEPel FabricBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	FabricParams p;
	ResolveFabric( ri, Scalar(-1), p );
	const WeaveRotatedRI weave( ri, p.weaveAngle );
	return ValueWithParams( vLightIn, ri, p, weave.Get() );
}

Scalar FabricBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	FabricParams p;
	ResolveFabric( ri, nm, p );
	const WeaveRotatedRI weave( ri, p.weaveAngle );
	return ValueNMWithParams( vLightIn, ri, nm, p, weave.Get() );
}

RISEPel FabricBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// OIDN AOV (9.2): a genuinely noise-free directional estimate --
	// the substrate's own AOV attenuated by the same scaling factor
	// `value()` applies at the view direction, plus the sheen lobe's
	// TABULATED directional albedo.  That is strictly better than
	// SheenBRDF::albedo's clamp-to-its-own-colour, which reports the
	// tint regardless of roughness or angle.
	const Vector3 n = RayFacingNormal( ri );
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = r_min( Scalar(1), r_max( Scalar(0), Vector3Ops::Dot( n, v ) ) );

	FabricParams p;
	ResolveFabric( ri, Scalar(-1), p );

	// The substrate's AOV is a DIRECTIONAL quantity evaluated in the
	// shading frame, exactly like `value()`, so it gets the same rotated
	// record the six evaluation entry points get.  (Every allowlisted
	// substrate's `albedo` happens to read only `onb.w()`, which the
	// rotation leaves alone -- but relying on that would make this site
	// silently wrong for a future frame-reading substrate.)
	const WeaveRotatedRI weave( ri, p.weaveAngle );
	const RISEPel baseAOV = pBase->albedo( weave.Get() );

	// THE SINGLE ARM, NOT THE JOINT SCALE -- and that is a DERIVATION,
	// not a simplification.  `albedo` is a directional-hemispherical
	// quantity, i.e. the joint kernel already integrated over l:
	//
	//   rho(v) = INT [ f_base(l,v) * scale(l,v) ] (n.l) dl
	//          = (1 - m*Ehat(v))/(1 - m*EhatMean) * INT f_base (1-m*Ehat(l)) (n.l) dl
	//
	// and for a base whose response is uncorrelated with Ehat(l) that
	// inner integral is `albedo_base * (1 - m*EhatMean)` -- because
	// (1/pi) INT Ehat(l) (n.l) dl IS EhatMean, by its definition.  The
	// `1/(1 - m*EhatMean)` therefore CANCELS EXACTLY and the AOV carries
	// the single arm:
	//
	//   rho(v) = albedo_base * (1 - m*Ehat(v)) + sheenColor * Ehat(v)
	//
	// For a white Lambertian base at m = 1 with an achromatic dye this
	// is Ehat(v) + 1 - Ehat(v) = 1 identically, which is the same
	// identity the furnace's Lambertian rows measure -- so the AOV
	// agrees with the rendered image about how much light the surface
	// returns, which is the property OIDN's cleanAux mode depends on.
	//
	// `Ehat`, NOT raw E, on both terms: inside the grazing sliver the
	// raw lobe exceeds 1, and using it here would make the AOV claim
	// the surface returns more than it receives -- exactly what the
	// [0,1] contract below forbids, and it would then be the CLAMP
	// rather than the model keeping the AOV legal.  With Ehat the sum
	// is in range by construction and the clamp only ever covers an
	// author binding a sheen painter above 1 (which is legal --
	// nothing else refuses it).
	const Scalar eHatV = r_min( Scalar(1), SheenDirectionalAlbedo::E( p.alpha, nDotV ) );
	RISEPel out = baseAOV * SheenTransmit( p.alpha, p.m, nDotV )
	            + p.tint * eHatV;

	// IBSDF::albedo's contract is explicit that the AOV must be in
	// [0,1] per channel so OIDN can run with cleanAux=true.
	ColorMath::Clamp( out, Scalar(0), Scalar(1) );
	return out;
}

//////////////////////////////////////////////////////////////////////
// hemisphericalAlbedo{,NM} -- ROUTE 1 of docs/CLOTH_FABRIC_DESIGN.md
// 9.2 as amended by round 5, with its exactness class stated rather
// than glossed.
//
//   out = substrate.hemisphericalAlbedo() * (1 - m*EhatMean(alpha))
//       + sheenColor * EhatMean(alpha)
//
// NO KERNEL TABLE.  Round 4 needed a baked 2-D `S(alpha, m)` here,
// because a `min` DOES NOT FACTOR into (a function of l) x (a function
// of v) and its double integral had to be tabulated.  The product form
// factors by construction, so the whole S table is RETIRED (it is gone
// from SheenDirectionalAlbedo.{h,cpp} and from the generator).  The
// derivation is two lines:
//
//   INT INT f_base * (1-m*Ehat(v))(1-m*Ehat(l))/(1-m*EhatMean) (n.l)(n.v) dl dv
//     = rho_base * (1 - m*EhatMean)^2 / (1 - m*EhatMean)
//     = rho_base * (1 - m*EhatMean)
//
// using (1/pi) INT Ehat(mu) mu dmu == EhatMean twice, once per arm --
// which is what kEHatMeanTable bakes: the mean of the CLAMPED lobe
// rather than of the raw one, so both sides of the identity use the
// same quantity.
//
// ONE CAVEAT, worth stating rather than leaving to be rediscovered.
// The table clamps at the NODES and then interpolates in alpha ("mean
// of clamps"), whereas `value()` clamps the INTERPOLATED E ("clamp of
// means").  The two commute only where no bracketing node straddles
// E == 1, so this closed form is not bit-exactly `value()`'s
// denominator in the narrow alpha band where they differ.  Measured at
// 2.7e-4 absolute (worst |table - dense numeric| = 2.665e-4 at
// alpha 0.765, table consistently LOW), and gate 5b bounds the total
// route-1 error at 5 % regardless -- but "exactly" would be the wrong
// word for it.
//
// A SECOND contributor to that same 2.7e-4, worth naming because it is
// NOT the clamp: the generator trapezoids LINEAR IN mu between the
// warped nodes while `E()` interpolates LINEAR IN sqrt(mu) -- two
// different interpolation bases over the same nodes.  Its effect here
// is |R - tint| * 2.7e-4, hence EXACTLY ZERO for the white-base /
// m = 1 / white-tint case the furnace measures, which is why no gate
// sees it.  Note also that TestEHatMeanConsistency cannot: it
// reproduces the generator's own trapezoid, so it pins
// table-vs-generator agreement, not table-vs-truth.  Adding the sheen lobe's own bihemispherical
// share, sheenColor * EhatMean, gives
// the line above.
//
// EXACTNESS CLASS.  Pulling the substrate's own albedo out of the joint
// (l, v) integral treats `f_base` and the kernel as UNCORRELATED, which
// requires `f_base` to be CONSTANT in the integration variables.  So
// this is:
//
//   * EXACT ALGEBRAICALLY for a Lambertian substrate, and now in CLOSED
//     FORM rather than through an interpolated table.  NUMERICALLY it
//     lands +3.1e-4 to +6.0e-4 HIGH against a brute-force
//     bihemispherical average of the actual f (closed form 1.000000 vs
//     brute 0.99950 / 0.99947 / 0.99940 / 0.99962 / 0.99969 at alpha
//     0.04 / 0.08 / 0.2 / 0.5 / 1.0).  The ALGEBRA is exact; the model
//     underneath it is not -- the cosTheta floor loses energy below mu1
//     and the sliver just above gains ~1.7 %, and the cosine weighting
//     very nearly, but not quite, cancels the two.  So "exact for
//     Lambertian" means "to 6e-4", not "to floating point".
//   * An UNCORRELATED-RESPONSE APPROXIMATION for Oren-Nayar (which
//     couples l and v through max/min(theta_l, theta_v) and the
//     azimuthal difference) and for GGX (which couples them through the
//     half-vector).  Azimuthal symmetry is NOT sufficient and is the
//     wrong test.  The error is measured -- not assumed -- by
//     tests/FabricMaterialChunkTest.cpp's brute-force quadrature check
//     (9.9 gate 5b).
//
// It is NOT "closed form for an arbitrary substrate" and must not be
// described as such.
//
// A SEPARATE, LARGER ERROR LIVES ONE LAYER DOWN AND IS NOT OURS.  What
// this method returns also inherits whatever the SUBSTRATE's own
// `hemisphericalAlbedo` reports.  `OrenNayarBRDF::hemisphericalAlbedo`
// returns `Rd` verbatim -- documented in OrenNayarBRDF.cpp:148-190 as
// measured 12.6 % high at roughness 0.5 and 25.6 % high at roughness 1
// -- and GGX's is likewise an estimate that runs high at grazing.  Gate
// 5b prints both errors separately for exactly this reason: no change
// here can fix that one, and `coated_material`'s recycling denominator
// already inherits the same debt.
//
// A TRANSMISSIVE SUBSTRATE NEEDS NOTHING EXTRA HERE (R8 P1.1), and the
// reason is on the substrate's side rather than ours:
// `WeaveBRDF::hemisphericalAlbedo` already reports the FRONT-hemisphere
// REFLECT budget only -- its own `volumeScale = 1 - transmit_k` excludes
// the share diverted to the diffuse transmission lobe, and the delta gap
// lobe never entered that number at all.  So `R` below is still "what
// the substrate returns to the front", which is exactly the quantity the
// product form's derivation integrates, and adding the transmitted share
// to it would make this method claim energy that never comes back up.
// `coated_material`'s Saunderson recycling denominator consumes this
// number and would be actively wrong if it did.
//
// DELIBERATELY NOT WEAVE-ROTATED.  Unlike the six evaluation entry
// points, this quantity is a bihemispherical average, which a rotation
// about `w` cannot change; passing the caller's own record is both
// correct and free.
//
// Returns FALSE when the substrate declines, rather than substituting a
// stand-in.  Unreachable through the shipping paths -- the allowlist is
// enforced at parse time AND at construction time and every allowlisted
// BSDF implements it -- but declining is the honest answer for a future
// substrate that does not, and the contract explicitly permits it.
// Note the contrast with CoatedBRDF::SubstrateAlbedo, which falls back
// to zero: there the number feeds a recycling denominator that must
// still produce SOMETHING, whereas here it IS the answer.
//////////////////////////////////////////////////////////////////////

bool FabricBRDF::hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const
{
	RISEPel R;
	if( !pBase->hemisphericalAlbedo( ri, R ) ) {
		return false;
	}

	FabricParams p;
	ResolveFabric( ri, Scalar(-1), p );

	out = R * SheenTransmitMean( p.alpha, p.m )
	    + p.tint * SheenDirectionalAlbedo::EHatMean( p.alpha );
	// Clamped, like `albedo`.  A reflectance under a uniform field above
	// 1 is not merely cosmetic: `coated_material` puts this number in a
	// Saunderson recycling denominator 1/(1 - r_i*R), which an R > 1
	// drives toward zero or negative.  Reachable when an author binds a
	// sheen painter above 1 (legal -- nothing refuses it).
	ColorMath::Clamp( out, Scalar(0), Scalar(1) );
	return true;
}

bool FabricBRDF::hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const
{
	Scalar R = 0;
	if( !pBase->hemisphericalAlbedoNM( ri, nm, R ) ) {
		return false;
	}

	FabricParams p;
	ResolveFabric( ri, nm, p );

	out = R * SheenTransmitMean( p.alpha, p.m )
	    + p.tintNM * SheenDirectionalAlbedo::EHatMean( p.alpha );
	// Clamped for the same reason as the RGB twin -- and this path can
	// exceed 1 for an extra reason the RGB one cannot: `m` is the
	// ACHROMATIC max3 while `tintNM` is the hero-wavelength sample, so a
	// painter whose value at one wavelength exceeds its RGB max3 leaves
	// the base under-suppressed relative to the sheen term added back.
	out = r_min( Scalar(1), r_max( Scalar(0), out ) );
	return true;
}
