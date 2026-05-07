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
#include "SheenSPF.h"
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

	// Imageworks / Estevez-Kulla 2017 Λ polynomial fit.  This is the
	// V-cavities masking-shadowing function for the Charlie distribution,
	// fit numerically to a 5-term function whose coefficients interpolate
	// between two operating points by (1-α)².  Reference: Estevez-Kulla,
	// "Production Friendly Microfacet Sheen BRDF", §4 (SIGGRAPH 2017
	// course notes).  Same polynomial shipped in glTF-Sample-Renderer
	// and pbrt-v4.
	inline Scalar SheenLambdaHelper( const Scalar x, const Scalar alpha )
	{
		const Scalar oneMinusAlphaSq = (Scalar(1) - alpha) * (Scalar(1) - alpha);
		const Scalar a = Scalar(21.5473) + (Scalar(25.3245) - Scalar(21.5473)) * oneMinusAlphaSq;
		const Scalar b = Scalar(3.82987) + (Scalar(3.32435) - Scalar(3.82987)) * oneMinusAlphaSq;
		const Scalar c = Scalar(0.19823) + (Scalar(0.16801) - Scalar(0.19823)) * oneMinusAlphaSq;
		const Scalar d = Scalar(-1.97760) + (Scalar(-1.27393) + Scalar(1.97760)) * oneMinusAlphaSq;
		const Scalar e = Scalar(-4.32054) + (Scalar(-4.85967) + Scalar(4.32054)) * oneMinusAlphaSq;
		return a / (Scalar(1) + b * std::pow( x, c )) + d * x + e;
	}

	inline Scalar SheenLambda( const Scalar cosTheta, const Scalar alpha )
	{
		// Piecewise: the polynomial fit is accurate for cosθ ∈ [0, 0.5];
		// the upper half is reconstructed by symmetry around 0.5 to keep
		// Λ smooth across the hinge.  Without the mirror we get a visible
		// kink at cosθ = 0.5.
		const Scalar c = std::fabs( cosTheta );
		if( c < Scalar(0.5) ) {
			return std::exp( SheenLambdaHelper( c, alpha ) );
		}
		return std::exp( Scalar(2) * SheenLambdaHelper( Scalar(0.5), alpha )
		               - SheenLambdaHelper( Scalar(1) - c, alpha ) );
	}

	// V-cavities visibility for the Charlie distribution.
	//   V(ω_o, ω_i) = G2 / (4 · n·l · n·v)
	//   G2 = 1 / ((1 + Λ(n·v)) · (1 + Λ(n·l)))
	// G2 ∈ [0, 1] by construction (V-cavities masking-shadowing) but
	// V = G2 / (4·n·l·n·v) can still grow without bound as either cosine
	// approaches zero.  Clamping V at 1 — same as the glTF spec
	// reference — caps single-sample energy and matches the upper bound
	// for any physically plausible visibility term.
	//
	// Replaces the prior Ashikhmin-Neubelt analytic form, whose
	// `· n·l · n·v` denominator factor produced an unbounded
	// directional albedo at grazing (ρ = 8.7 at θ_v = 80° in the
	// Landing 6 audit — see [PHYSICALLY_BASED_PIPELINE_PLAN.md]).
	inline Scalar SheenV( const Scalar nDotL, const Scalar nDotV, const Scalar alpha )
	{
		const Scalar denom = Scalar(4) * nDotL * nDotV;
		if( denom < Scalar(1e-6) ) return Scalar(0);
		const Scalar a = r_max( alpha, Scalar(1e-3) );
		const Scalar invG2 = (Scalar(1) + SheenLambda( nDotV, a ))
		                   * (Scalar(1) + SheenLambda( nDotL, a ));
		const Scalar V = Scalar(1) / (invG2 * denom);
		if( V < Scalar(0) ) return Scalar(0);
		if( V > Scalar(1) ) return Scalar(1);
		return V;
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
	const Scalar V = SheenV( nDotL, nDotV, alpha );

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
	const Scalar V = SheenV( nDotL, nDotV, alpha );

	return pColor.GetColorNM( ri, nm ) * D * V;
}

RISEPel SheenBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Return the LUT-scaled directional reflectance (sheenColor ·
	// E_sheen) rather than the raw sheenColor painter value.
	// IBSDF::albedo is contracted to be in [0, 1] per channel for the
	// OIDN denoiser's albedo AOV (see IBSDF.h); raw sheenColor of 1
	// would over-estimate the actual reflectance — Charlie sheen's
	// integrated albedo is well below 1 — and would also break
	// CompositeBRDF::albedo's energy budget for sheen-over-base
	// (the additive AOV `topA + (1 − topL)·baseA` clamps to 1
	// exactly when topA == topL, which only holds when SheenBRDF
	// reports the LUT-scaled value).  No new state — same lookup
	// SheenSPF and CompositeSPF use.
	return GetLayerAlbedo( ri );
}

RISEPel SheenBRDF::GetLayerAlbedo( const RayIntersectionGeometric& ri ) const
{
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) return RISEPel( 0, 0, 0 );

	const Scalar alpha = r_max( ColorMath::MaxValue( pRoughness.GetColor( ri ) ), Scalar(1e-3) );
	const Scalar E = SheenSPF::AlbedoLookup( nDotV, alpha );

	RISEPel albedo = pColor.GetColor( ri ) * E;
	for( int c = 0; c < 3; ++c ) {
		if( albedo[c] < 0 ) albedo[c] = 0;
		if( albedo[c] > 1 ) albedo[c] = 1;
	}
	return albedo;
}

Scalar SheenBRDF::GetLayerAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) return 0;

	const Scalar alpha = r_max( pRoughness.GetColorNM( ri, nm ), Scalar(1e-3) );
	const Scalar E = SheenSPF::AlbedoLookup( nDotV, alpha );

	Scalar albedo = pColor.GetColorNM( ri, nm ) * E;
	if( albedo < 0 ) albedo = 0;
	if( albedo > 1 ) albedo = 1;
	return albedo;
}
