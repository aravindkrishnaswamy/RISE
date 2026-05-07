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
#include <random>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// Charlie distribution + V-cavities Λ-based visibility (replicated
	// from SheenBRDF — kept private here so SheenSPF can compute
	// kray = f(wo) · cosθ_o / pdf(wo) without circular includes).
	// The two namespaces MUST stay in lock-step or MIS weights between
	// SPF::Scatter (forward sampling) and BRDF::value (light sampling)
	// silently diverge.
	inline Scalar SheenD( const Scalar alpha, const Scalar nDotH )
	{
		const Scalar a = r_max( alpha, Scalar(1e-3) );
		const Scalar invA = Scalar(1) / a;
		const Scalar sin2 = r_max( Scalar(0), Scalar(1) - nDotH * nDotH );
		const Scalar sinTh = std::sqrt( sin2 );
		const Scalar p = std::pow( sinTh, invA );
		return (Scalar(2) + invA) * p / (Scalar(2) * PI);
	}

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
		const Scalar c = std::fabs( cosTheta );
		if( c < Scalar(0.5) ) {
			return std::exp( SheenLambdaHelper( c, alpha ) );
		}
		return std::exp( Scalar(2) * SheenLambdaHelper( Scalar(0.5), alpha )
		               - SheenLambdaHelper( Scalar(1) - c, alpha ) );
	}

	inline Scalar SheenV( const Scalar nDotL, const Scalar nDotV, const Scalar alpha )
	{
		// See SheenBRDF.cpp's SheenV — V-cavities masking-shadowing for
		// the Charlie distribution, capped at 1.  Replaces the prior
		// Ashikhmin-Neubelt form whose `· n·l · n·v` denominator factor
		// produced unbounded directional albedo at grazing.
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

	// ----------------------------------------------------------------
	// Sheen directional-albedo LUT (Khronos / Imageworks form)
	// ----------------------------------------------------------------
	//
	// E_sheen(μ_v, α) = ∫_Ω+ f_sheen(ω_i, ω_v) · (n·ω_i) dω_i
	//
	// where f_sheen is the V-cavities Charlie BRDF defined above.
	// This LUT drives the Khronos additive layered composition in
	// `CompositeSPF` (and `CompositeBRDF`) when a sheen-like top
	// layer sits over a base material:
	//
	//     f_layered = f_sheen + f_base · (1 − sheenColor · E_sheen)
	//
	// Without this composition, a sheen top whose outgoing rays
	// always go UP (cosine-hemisphere sampling on the front face)
	// short-circuits CompositeSPF's random walk, dropping the base
	// layer's contribution entirely — see Landing 6 §"Finding E".
	//
	// Implementation: 32×32 table over (μ_v, α) ∈ [0, 1]² built
	// once at process startup via deterministic Monte-Carlo
	// integration.  Bilinear interpolation at lookup time.
	// Build cost is ~0.1 s; the result is shared across all
	// SheenSPF / SheenBRDF / CompositeSPF queries in the process.

	static constexpr int kSheenLUTSize = 32;

	class SheenAlbedoTable
	{
	public:
		SheenAlbedoTable() { Build(); }

		Scalar Lookup( Scalar mu, Scalar alpha ) const
		{
			mu    = std::min( std::max( mu,    Scalar(0) ), Scalar(1) );
			alpha = std::min( std::max( alpha, Scalar(0) ), Scalar(1) );

			const Scalar fmu    = mu    * Scalar(kSheenLUTSize - 1);
			const Scalar falpha = alpha * Scalar(kSheenLUTSize - 1);
			const int i0 = static_cast<int>( fmu );
			const int j0 = static_cast<int>( falpha );
			const int i1 = std::min( i0 + 1, kSheenLUTSize - 1 );
			const int j1 = std::min( j0 + 1, kSheenLUTSize - 1 );
			const Scalar tu = fmu    - Scalar(i0);
			const Scalar tv = falpha - Scalar(j0);

			const Scalar a = values[i0][j0] * (Scalar(1) - tu) + values[i1][j0] * tu;
			const Scalar b = values[i0][j1] * (Scalar(1) - tu) + values[i1][j1] * tu;
			return a * (Scalar(1) - tv) + b * tv;
		}

	private:
		Scalar values[kSheenLUTSize][kSheenLUTSize];

		void Build()
		{
			// Deterministic MC seed so the LUT is bit-identical across
			// processes / runs / platforms.  4096 samples per cell gives
			// ~1.5 % relative noise at the lowest E values, which is
			// inside the Run()-side tolerance band on the audit and well
			// below visible thresholds in renders.
			static constexpr int kSamples = 4096;
			std::mt19937 rng( 0x5EE7Au );		// arbitrary fixed seed
			std::uniform_real_distribution<Scalar> dist( Scalar(0), Scalar(1) );

			for( int i = 0; i < kSheenLUTSize; ++i )
			{
				const Scalar mu_v = (kSheenLUTSize > 1)
					? Scalar(i) / Scalar(kSheenLUTSize - 1)
					: Scalar(0.5);
				const Scalar sin_v = std::sqrt( std::max( Scalar(0), Scalar(1) - mu_v * mu_v ) );
				// Pick wo = (sin_v, 0, mu_v).  The integral is
				// rotationally symmetric around the surface normal so
				// the choice of phi is arbitrary.
				const Scalar wo_x = sin_v;
				const Scalar wo_z = mu_v;

				for( int j = 0; j < kSheenLUTSize; ++j )
				{
					Scalar alpha = Scalar(j) / Scalar(kSheenLUTSize - 1);
					alpha = std::min( std::max( alpha, Scalar(1e-3) ), Scalar(1) );

					Scalar sum = 0;
					for( int k = 0; k < kSamples; ++k )
					{
						// Cosine-weighted hemisphere sample for ω_i.
						const Scalar u1 = dist( rng );
						const Scalar u2 = dist( rng );
						const Scalar phi    = Scalar(2) * PI * u1;
						const Scalar cos_t  = std::sqrt( u2 );
						const Scalar sin_t  = std::sqrt( std::max( Scalar(0), Scalar(1) - u2 ) );
						const Scalar wi_x   = std::cos( phi ) * sin_t;
						const Scalar wi_y   = std::sin( phi ) * sin_t;
						const Scalar wi_z   = cos_t;

						const Scalar nDotL = wi_z;
						const Scalar nDotV = mu_v;
						if( nDotL <= NEARZERO || nDotV <= NEARZERO ) {
							continue;
						}

						// h = normalize(wi + wo)
						const Scalar h_x = wi_x + wo_x;
						const Scalar h_y = wi_y;
						const Scalar h_z = wi_z + wo_z;
						const Scalar h_len = std::sqrt( h_x*h_x + h_y*h_y + h_z*h_z );
						if( h_len < NEARZERO ) {
							continue;
						}
						const Scalar nDotH = h_z / h_len;

						const Scalar D = SheenD( alpha, nDotH );
						const Scalar V = SheenV( nDotL, nDotV, alpha );

						// MC estimator for ∫ f·cos·dωi with cosine-weighted sampling:
						//   E[BRDF · cos / pdf] = E[BRDF · cos · π / cos] = E[BRDF · π]
						sum += D * V * PI;
					}
					Scalar E = sum / Scalar(kSamples);
					// Clamp to [0, 1].  V-cavities is energy-conserving by
					// construction so E ≤ 1, but MC noise can push the
					// last LSB above unity at corners; clamping keeps the
					// downstream attenuation `(1 − E)` non-negative.
					values[i][j] = std::min( std::max( E, Scalar(0) ), Scalar(1) );
				}
			}
		}
	};

	const SheenAlbedoTable& GetSheenLUT()
	{
		// Function-local static — guaranteed thread-safe initialization
		// in C++11+ ("magic statics"), built on first call, kept for
		// the lifetime of the process.
		static const SheenAlbedoTable lut;
		return lut;
	}
}

Scalar SheenSPF::AlbedoLookup( Scalar nDotV, Scalar alpha )
{
	return GetSheenLUT().Lookup( nDotV, alpha );
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
	const Scalar V = SheenV( nDotL, nDotV, alpha );

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
	const Scalar V = SheenV( nDotL, nDotV, alpha );

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

RISEPel SheenSPF::GetLayerAlbedo(
	const RayIntersectionGeometric& ri,
	const IORStack& /*ior_stack*/
	) const
{
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) return RISEPel( 0, 0, 0 );

	const Scalar alpha = r_max( ColorMath::MaxValue( pRoughness.GetColor( ri ) ), Scalar(1e-3) );
	const Scalar E = AlbedoLookup( nDotV, alpha );

	// f_combined = f_top + f_base · (1 − sheenColor · E_sheen).  Per-channel
	// `sheenColor · E` is the fraction of incident light captured by the
	// sheen lobe; the complement is what reaches the base.  Clamp the
	// product so a high-key sheen colour (sheenColor > 1, valid in
	// linear painters) can't drive the base attenuation negative.
	RISEPel albedo = pColor.GetColor( ri ) * E;
	for( int c = 0; c < 3; ++c ) {
		if( albedo[c] < 0 ) albedo[c] = 0;
		if( albedo[c] > 1 ) albedo[c] = 1;
	}
	return albedo;
}

Scalar SheenSPF::GetLayerAlbedoNM(
	const RayIntersectionGeometric& ri,
	const IORStack& /*ior_stack*/,
	const Scalar nm
	) const
{
	const bool bFrontFace = Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= NEARZERO;
	const Vector3 n = bFrontFace ? ri.onb.w() : -ri.onb.w();
	const Vector3 v = Vector3Ops::Normalize( -ri.ray.Dir() );
	const Scalar nDotV = Vector3Ops::Dot( v, n );
	if( nDotV <= NEARZERO ) return 0;

	const Scalar alpha = r_max( pRoughness.GetColorNM( ri, nm ), Scalar(1e-3) );
	const Scalar E = AlbedoLookup( nDotV, alpha );

	Scalar albedo = pColor.GetColorNM( ri, nm ) * E;
	if( albedo < 0 ) albedo = 0;
	if( albedo > 1 ) albedo = 1;
	return albedo;
}
