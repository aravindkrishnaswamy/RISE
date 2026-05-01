//////////////////////////////////////////////////////////////////////
//
//  SheenBRDF.cpp - Implementation of the Charlie / Neubelt sheen
//  BRDF; see header for the math.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SheenBRDF.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Charlie microfacet distribution
	//   D(α, n·h) = (2 + 1/α) / (2π) · sin(θ_h)^(1/α)
	// where sin(θ_h) = sqrt(1 - (n·h)²).
	inline Scalar SheenD( const Scalar alpha, const Scalar nDotH )
	{
		const Scalar a = r_max( alpha, Scalar(1e-3) );
		const Scalar invA = Scalar(1) / a;
		const Scalar sin2 = r_max( Scalar(0), Scalar(1) - nDotH * nDotH );
		const Scalar sinTh = std::sqrt( sin2 );
		// pow(0, x) is 0 for x>0; pow(1e-30, ...) yields 0 in practice and
		// the result is multiplied by sheenColor which can be 0 too, so this
		// is safe for grazing angles.
		const Scalar p = std::pow( sinTh, invA );
		return (Scalar(2) + invA) * p / (Scalar(2) * PI);
	}

	// Ashikhmin / Neubelt visibility (Khronos KHR_materials_sheen formulation):
	//   V(n·l, n·v) = 1 / (4 · (n·l + n·v − n·l·n·v) · n·l · n·v)
	// The `· n·l · n·v` factor in the denominator is what gives Charlie its
	// characteristic grazing-bright / centre-dark profile; an earlier
	// revision dropped it and produced over-bright sheen at normal incidence.
	// Bounded above for grazing directions to avoid blowups.
	inline Scalar SheenV( const Scalar nDotL, const Scalar nDotV )
	{
		const Scalar cosProd = nDotL * nDotV;
		const Scalar denom = Scalar(4) * ( nDotL + nDotV - cosProd ) * cosProd;
		if( denom < Scalar(1e-6) ) return Scalar(0);
		return Scalar(1) / denom;
	}
}

SheenBRDF::SheenBRDF(
	const IPainter& sheenColor,
	const IPainter& sheenRoughness
	) :
  pColor( sheenColor ),
  pRoughness( sheenRoughness )
{
	pColor.addref();
	pRoughness.addref();
}

SheenBRDF::~SheenBRDF()
{
	pColor.release();
	pRoughness.release();
}

RISEPel SheenBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	// Flip the shading normal for hits on the back face so the BRDF stays
	// consistent with SheenSPF::Pdf / Scatter (both flip the ONB when the
	// incoming ray pierces the surface from inside).  Without this, MIS
	// weights mismatch when sheen is layered above a transparent base
	// (composite + dielectric) and back-face hits silently return zero.
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 l = Vector3Ops::Normalize( vLightIn );
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nDotL = Vector3Ops::Dot( n, l );
	const Scalar nDotV = Vector3Ops::Dot( n, v );
	if( nDotL <= NEARZERO || nDotV <= NEARZERO ) {
		return RISEPel( 0, 0, 0 );
	}

	const Vector3 h = Vector3Ops::Normalize( l + v );
	const Scalar nDotH = r_max( Scalar(0), Vector3Ops::Dot( n, h ) );

	const Scalar alpha = r_max( ColorMath::MaxValue( pRoughness.GetColor( ri ) ), Scalar(1e-3) );
	const Scalar D = SheenD( alpha, nDotH );
	const Scalar V = SheenV( nDotL, nDotV );

	return pColor.GetColor( ri ) * (D * V);
}

Scalar SheenBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Same back-face flip as SheenBRDF::value.
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 l = Vector3Ops::Normalize( vLightIn );
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );

	const Scalar nDotL = Vector3Ops::Dot( n, l );
	const Scalar nDotV = Vector3Ops::Dot( n, v );
	if( nDotL <= NEARZERO || nDotV <= NEARZERO ) {
		return 0;
	}

	const Vector3 h = Vector3Ops::Normalize( l + v );
	const Scalar nDotH = r_max( Scalar(0), Vector3Ops::Dot( n, h ) );

	const Scalar alpha = r_max( pRoughness.GetColorNM( ri, nm ), Scalar(1e-3) );
	const Scalar D = SheenD( alpha, nDotH );
	const Scalar V = SheenV( nDotL, nDotV );

	return pColor.GetColorNM( ri, nm ) * D * V;
}

RISEPel SheenBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Cheap directional-albedo estimate: clamp sheen colour to [0,1] and
	// use it directly.  Charlie sheen has a roughness-dependent
	// directional albedo that drops below 1 as roughness decreases, but
	// this is used for RR / importance estimation, not for the rendered
	// image, so a slightly conservative (over-)estimate is fine.
	return pColor.GetColor( ri );
}
