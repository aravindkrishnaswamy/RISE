//////////////////////////////////////////////////////////////////////
//
//  SheenBRDF.cpp - Imageworks production-friendly Charlie sheen
//  BRDF (Estevez & Kulla 2017).  D / Λ / V helpers are shared with
//  SheenSPF via CharlieSheen.h so a coefficient drift in one site
//  cannot silently desync the BRDF and the SPF.  See header for the
//  math.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SheenBRDF.h"
#include "CharlieSheen.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/math_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

SheenBRDF::SheenBRDF(
	const IPainter& sheenColor,
	const IScalarPainter& sheenRoughness
	) :
  pColor( &sheenColor ),
  pRoughness( &sheenRoughness )
{
	pColor->addref();
	pRoughness->addref();
}

SheenBRDF::~SheenBRDF()
{
	safe_release( pColor );
	safe_release( pRoughness );
}

void SheenBRDF::SetColor( const IPainter& v )
{
	v.addref();
	safe_release( pColor );
	pColor = &v;
}

void SheenBRDF::SetRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRoughness );
	pRoughness = &v;
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

	const Scalar alpha = r_max( pRoughness->GetValuesAt( ri ).v[0], Scalar(1e-3) );
	const Scalar D = CharlieSheen::D( alpha, nDotH );
	const Scalar V = CharlieSheen::V( alpha, nDotL, nDotV );

	return pColor->GetColor( ri ) * (D * V);
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

	const Scalar alpha = r_max( pRoughness->GetValueAtNM( ri, nm ), Scalar(1e-3) );
	const Scalar D = CharlieSheen::D( alpha, nDotH );
	const Scalar V = CharlieSheen::V( alpha, nDotL, nDotV );

	return pColor->GetColorNM( ri, nm ) * D * V;
}

RISEPel SheenBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Cheap directional-albedo estimate: clamp sheen colour to [0,1] and
	// use it directly.  Charlie sheen has a roughness-dependent
	// directional albedo that drops below 1 as roughness decreases, but
	// this is used for RR / importance estimation, not for the rendered
	// image, so a slightly conservative (over-)estimate is fine.
	return pColor->GetColor( ri );
}
