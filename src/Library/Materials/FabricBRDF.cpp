//////////////////////////////////////////////////////////////////////
//
//  FabricBRDF.cpp - Closed-form Charlie-sheen-over-substrate response
//    for `fabric_material`.  Shares D/Lambda/V from CharlieSheen.h and
//    E/EHatMean tables from SheenDirectionalAlbedo.h.
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
	//! Ray-facing shading normal (matches SheenBRDF / FabricSPF).
	inline RISE::Vector3 RayFacingNormal( const RISE::RayIntersectionGeometric& ri )
	{
		const bool bFrontFace = RISE::Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
		return bFrontFace ? ri.onb.w() : -ri.onb.w();
	}

	//! Ray-anchored geometric normal for horizon gating (falls back to `n` if degenerate).
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
	const bool baseScattersFullSphere,
	const bool baseSingleDiffuseLabel
	) :
  pBase( &base ),
  pSheenColor( &sheenColor ),
  pSheenRoughness( &sheenRoughness ),
  pWeaveRotation( &weaveRotation ),
  bSingleDiffuseLabel( baseSingleDiffuseLabel ),
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
	// `alpha`, `m`, and `weaveAngle` are all achromatic so Scatter's hero pdf
	// agrees with companion-wavelength Pdf() calls.
	const Scalar rawAlpha = pSheenRoughness->GetValuesAt( ri ).v[0];
	// `!(x >= lo)` catches NaN as well as below-floor (r_min/r_max pass NaN through).
	out.alpha = !( rawAlpha >= kMinSheenAlpha ) ? kMinSheenAlpha
	          : r_min( kMaxSheenAlpha, rawAlpha );

	const RISEPel rgb = ReflectanceColor( *pSheenColor, ri );
	out.m = r_min( Scalar(1), r_max( Scalar(0), ColorMath::MaxValue( rgb ) ) );

	if( nm < 0 ) {
		out.tint   = rgb;
		out.tintNM = 0;
	} else {
		out.tint   = RISEPel( 0, 0, 0 );
		// Inline white guard using `rgb` already fetched above (avoids a second GetColor).
		out.tintNM = IsUntintedWhitePainter( *pSheenColor, rgb )
			? Scalar(1)
			: r_max( Scalar(0), pSheenColor->GetColorNM( ri, nm ) );
	}

	out.weaveAngle = pWeaveRotation->GetValuesAt( ri ).v[0];
}

Scalar FabricBRDF::SheenTransmit( const Scalar alpha, const Scalar m, const Scalar cosTheta )
{
	// Clamping Ehat = min(E, 1) is load-bearing: raw E can exceed 1 near grazing
	// (cosTheta < ~0.03), which would otherwise make 1 - m*E negative.
	const Scalar eHat = r_min( Scalar(1), SheenDirectionalAlbedo::E( alpha, cosTheta ) );
	return Scalar(1) - r_min( Scalar(1), m * eHat );
}

Scalar FabricBRDF::SheenNormaliser( const Scalar alpha,
                                    const Scalar cosThetaV, const Scalar cosThetaL )
{
	// Symmetric max(1, E(v), E(l)) preserves reciprocity; == 1 outside the grazing sliver.
	const Scalar eV = SheenDirectionalAlbedo::E( alpha, cosThetaV );
	const Scalar eL = SheenDirectionalAlbedo::E( alpha, cosThetaL );
	return r_max( Scalar(1), r_max( eV, eL ) );
}

Scalar FabricBRDF::BaseScaling( const Scalar alpha, const Scalar m,
                                const Scalar cosThetaV, const Scalar cosThetaL )
{
	const Scalar denom = r_max( Scalar(1e-6),
		Scalar(1) - r_min( Scalar(1), m * SheenDirectionalAlbedo::EHatMean( alpha ) ) );
	return SheenTransmit( alpha, m, cosThetaV )
	     * SheenTransmit( alpha, m, cosThetaL )
	     / denom;
}

Scalar FabricBRDF::SheenTransmitMean( const Scalar alpha, const Scalar m )
{
	const Scalar meBar = m * SheenDirectionalAlbedo::EHatMean( alpha );
	return Scalar(1) - r_min( Scalar(1), meBar );
}

Scalar FabricBRDF::SheenSelectWeight( const Scalar alpha, const Scalar m, const Scalar cosThetaV )
{
	// Clamped Ehat keeps w in [0,1]; w steers sampling effort only (FabricSPF
	// reprices against the full mixture pdf, so any w in (0,1) is unbiased;
	// w == 1 implies SheenTransmit(v) == 0 and w == 0 implies m == 0).
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

	if( nDotV <= NEARZERO ) {
		return t;
	}

	if( nDotL <= NEARZERO )
	{
		// Opposite-hemisphere transmission (only when substrate scatters full sphere;
		// symmetric horizon exclusion band |n.l| <= NEARZERO; no geometric-horizon gate;
		// t.sheen stays 0 since Charlie sheen is reflection-only).
		if( !bBaseFullSphere || !( nDotL < -NEARZERO ) ) {
			return t;
		}
		t.scaling  = BaseScaling( p.alpha, p.m, nDotV, -nDotL );
		t.valid    = true;
		return t;
	}

	// Symmetric geometric-horizon gate (preserves reciprocity; matches SheenBRDF).
	const Vector3 geomN = GeomNormal( ri, n );
	if( Vector3Ops::Dot( l, geomN ) <= 0 || Vector3Ops::Dot( v, geomN ) <= 0 ) {
		return t;
	}

	const Vector3 h = Vector3Ops::Normalize( l + v );
	// Hoist dot product: r_max is a macro and evaluates arguments twice.
	const Scalar nDotHRaw = Vector3Ops::Dot( n, h );
	const Scalar nDotH = r_max( Scalar(0), nDotHRaw );

	const Scalar D = CharlieSheen::D( p.alpha, nDotH );
	const Scalar V = CharlieSheen::V( p.alpha, nDotL, nDotV );

	t.sheen   = D * V / SheenNormaliser( p.alpha, nDotV, nDotL );
	t.scaling = BaseScaling( p.alpha, p.m, nDotV, nDotL );
	t.valid   = true;
	return t;
}

// Keep ValueWithParams and ValueNMWithParams textually parallel (same
// parenthesization for spectral parity; transmission has t.sheen == 0).

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

	// Cap spectral tint at the achromatic budget `m` so no wavelength adds
	// back more sheen energy than `t.scaling` removed from the base.
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
	// OIDN directional albedo AOV (docs/CLOTH_FABRIC_DESIGN.md §9.2):
	// integrating BaseScaling over l cancels (1 - m*EhatMean), leaving
	//   rho(v) = baseAOV * (1 - m*Ehat(v)) + tint * Ehat(v), clamped to [0,1].
	const Vector3 n = RayFacingNormal( ri );
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = r_min( Scalar(1), r_max( Scalar(0), Vector3Ops::Dot( n, v ) ) );

	FabricParams p;
	ResolveFabric( ri, Scalar(-1), p );

	const WeaveRotatedRI weave( ri, p.weaveAngle );
	const RISEPel baseAOV = pBase->albedo( weave.Get() );

	const Scalar eHatV = r_min( Scalar(1), SheenDirectionalAlbedo::E( p.alpha, nDotV ) );
	RISEPel out = baseAOV * SheenTransmit( p.alpha, p.m, nDotV )
	            + p.tint * eHatV;

	ColorMath::Clamp( out, Scalar(0), Scalar(1) );
	return out;
}

// hemisphericalAlbedo{,NM} (docs/CLOTH_FABRIC_DESIGN.md §9.2):
//   out = R * (1 - m*EhatMean(alpha)) + sheenColor * EhatMean(alpha)
// Exact algebraically for Lambertian (~6e-4 numeric from grazing floor);
// uncorrelated-response approximation for Oren-Nayar/GGX. Not weave-rotated
// (bihemispherical average is rotation-invariant). For transmissive WeaveBRDF,
// R already reports front-hemisphere reflection only. Returns false if the
// substrate declines; clamped to [0,1] because coated_material feeds this into
// its Saunderson denominator 1 / (1 - r_i * R).

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
	out = r_min( Scalar(1), r_max( Scalar(0), out ) );
	return true;
}
