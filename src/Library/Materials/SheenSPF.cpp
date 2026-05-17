//////////////////////////////////////////////////////////////////////
//
//  SheenSPF.cpp - Sampling counterpart to SheenBRDF.  Cosine-weighted
//    hemisphere sampling; the PDF mismatch with the Charlie BRDF
//    distribution is small for the typical sheen roughness range
//    [0.1, 1] and gets cleaned up by MIS in the path tracer.
//
//    D / Λ / V helpers are shared with SheenBRDF via CharlieSheen.h
//    so a coefficient drift in one site cannot silently desync the
//    BRDF and the SPF.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SheenSPF.h"
#include "CharlieSheen.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/math_utils.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

SheenSPF::SheenSPF(
	const IPainter& sheenColor,
	const IScalarPainter& sheenRoughness
	) :
  pColor( &sheenColor ),
  pRoughness( &sheenRoughness )
{
	pColor->addref();
	pRoughness->addref();
}

SheenSPF::~SheenSPF()
{
	safe_release( pColor );
	safe_release( pRoughness );
}

void SheenSPF::SetColor( const IPainter& v )
{
	v.addref();
	safe_release( pColor );
	pColor = &v;
}

void SheenSPF::SetRoughness( const IScalarPainter& v )
{
	v.addref();
	safe_release( pRoughness );
	pRoughness = &v;
}

void SheenSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& /*ior_stack*/
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 n = myonb.w();

	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
	const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );

	const Scalar nDotL = Vector3Ops::Dot( wo, n );
	if( nDotL <= NEARZERO ) {
		return;
	}

	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) {
		return;
	}

	const Vector3 h = Vector3Ops::Normalize( wo + v );
	const Scalar nDotH = r_max( Scalar(0), Vector3Ops::Dot( n, h ) );

	const Scalar alpha = r_max( pRoughness->GetValuesAt( ri ).v[0], Scalar(1e-3) );
	const Scalar D = CharlieSheen::D( alpha, nDotH );
	const Scalar V = CharlieSheen::V( alpha, nDotL, nDotV );

	// kray = f(wo) · cosθ_o / pdf(wo); pdf for cosine-hemisphere is
	// cosθ_o / π, so kray = f · π.
	const RISEPel kray = pColor->GetColor( ri ) * (D * V * PI);

	ScatteredRay s;
	s.type = ScatteredRay::eRayDiffuse;
	s.ray.Set( ri.ptIntersection, wo );
	s.kray = kray;
	s.pdf = nDotL * INV_PI;
	s.isDelta = false;
	scattered.AddScatteredRay( s );
}

void SheenSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& /*ior_stack*/
	) const
{
	OrthonormalBasis3D myonb = ri.onb;
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) {
		myonb.FlipW();
	}
	const Vector3 n = myonb.w();

	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
	const Vector3 wo = GeometricUtilities::CreateDiffuseVector( myonb, ptrand );

	const Scalar nDotL = Vector3Ops::Dot( wo, n );
	if( nDotL <= NEARZERO ) return;

	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) return;

	const Vector3 h = Vector3Ops::Normalize( wo + v );
	const Scalar nDotH = r_max( Scalar(0), Vector3Ops::Dot( n, h ) );

	const Scalar alpha = r_max( pRoughness->GetValueAtNM( ri, nm ), Scalar(1e-3) );
	const Scalar D = CharlieSheen::D( alpha, nDotH );
	const Scalar V = CharlieSheen::V( alpha, nDotL, nDotV );

	ScatteredRay s;
	s.type = ScatteredRay::eRayDiffuse;
	s.ray.Set( ri.ptIntersection, wo );
	s.krayNM = pColor->GetColorNM( ri, nm ) * D * V * PI;
	s.pdf = nDotL * INV_PI;
	s.isDelta = false;
	scattered.AddScatteredRay( s );
}

Scalar SheenSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& /*ior_stack*/
	) const
{
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Scalar cosTheta = bFrontFace
		? Vector3Ops::Dot( wo, ri.onb.w() )
		: -Vector3Ops::Dot( wo, ri.onb.w() );
	return (cosTheta > 0) ? cosTheta * INV_PI : 0;
}

Scalar SheenSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar /*nm*/,
	const IORStack& ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}
