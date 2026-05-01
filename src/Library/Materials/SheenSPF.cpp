//////////////////////////////////////////////////////////////////////
//
//  SheenSPF.cpp - Sampling counterpart to SheenBRDF.  Cosine-weighted
//    hemisphere sampling; the PDF mismatch with the Charlie BRDF
//    distribution is small for the typical sheen roughness range
//    [0.1, 1] and gets cleaned up by MIS in the path tracer.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SheenSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/math_utils.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Charlie distribution + Neubelt visibility (replicated from
	// SheenBRDF — kept private here so SheenSPF can compute kray =
	// f(wo) · cosθ_o / pdf(wo) without circular includes).
	inline Scalar SheenD( const Scalar alpha, const Scalar nDotH )
	{
		const Scalar a = r_max( alpha, Scalar(1e-3) );
		const Scalar invA = Scalar(1) / a;
		const Scalar sin2 = r_max( Scalar(0), Scalar(1) - nDotH * nDotH );
		const Scalar sinTh = std::sqrt( sin2 );
		const Scalar p = std::pow( sinTh, invA );
		return (Scalar(2) + invA) * p / (Scalar(2) * PI);
	}

	inline Scalar SheenV( const Scalar nDotL, const Scalar nDotV )
	{
		// See SheenBRDF.cpp's SheenV — must match exactly.  The
		// `· n·l · n·v` factor is required for the Khronos /
		// Estevez-Kulla form to integrate to a sensible directional
		// albedo.
		const Scalar cosProd = nDotL * nDotV;
		const Scalar denom = Scalar(4) * ( nDotL + nDotV - cosProd ) * cosProd;
		if( denom < Scalar(1e-6) ) return Scalar(0);
		return Scalar(1) / denom;
	}
}

SheenSPF::SheenSPF(
	const IPainter& sheenColor,
	const IPainter& sheenRoughness
	) :
  pColor( sheenColor ),
  pRoughness( sheenRoughness )
{
	pColor.addref();
	pRoughness.addref();
}

SheenSPF::~SheenSPF()
{
	pColor.release();
	pRoughness.release();
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

	const Scalar alpha = r_max( ColorMath::MaxValue( pRoughness.GetColor( ri ) ), Scalar(1e-3) );
	const Scalar D = SheenD( alpha, nDotH );
	const Scalar V = SheenV( nDotL, nDotV );

	// kray = f(wo) · cosθ_o / pdf(wo); pdf for cosine-hemisphere is
	// cosθ_o / π, so kray = f · π.
	const RISEPel kray = pColor.GetColor( ri ) * (D * V * PI);

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

	const Scalar alpha = r_max( pRoughness.GetColorNM( ri, nm ), Scalar(1e-3) );
	const Scalar D = SheenD( alpha, nDotH );
	const Scalar V = SheenV( nDotL, nDotV );

	ScatteredRay s;
	s.type = ScatteredRay::eRayDiffuse;
	s.ray.Set( ri.ptIntersection, wo );
	s.krayNM = pColor.GetColorNM( ri, nm ) * D * V * PI;
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
