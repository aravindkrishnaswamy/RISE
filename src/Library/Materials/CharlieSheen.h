//////////////////////////////////////////////////////////////////////
//
//  CharlieSheen.h - Shared Charlie / Imageworks production-friendly
//  sheen distribution + shadowing-masking helpers used by SheenBRDF
//  and SheenSPF.  Single source of truth so a coefficient drift in
//  one site cannot silently desync the BRDF and the SPF.
//
//  References:
//    Estevez & Kulla 2017, "Production Friendly Microfacet Sheen
//      BRDF" (Imageworks technical brief)
//    Khronos glTF Sample Renderer `lambdaSheenNumericHelper` GLSL
//      port (the canonical KHR_materials_sheen reference impl)
//
//  All helpers are inline; this header has no .cpp counterpart.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CHARLIE_SHEEN_
#define CHARLIE_SHEEN_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/math_utils.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		namespace CharlieSheen
		{
			// Charlie microfacet distribution
			//   D(α, n·h) = (2 + 1/α) / (2π) · sin(θ_h)^(1/α)
			// where sin(θ_h) = sqrt(1 - (n·h)²).
			inline Scalar D( const Scalar alpha, const Scalar nDotH )
			{
				const Scalar a = r_max( alpha, Scalar(1e-3) );
				const Scalar invA = Scalar(1) / a;
				const Scalar sin2 = r_max( Scalar(0), Scalar(1) - nDotH * nDotH );
				const Scalar sinTh = std::sqrt( sin2 );
				// pow(0, x) is 0 for x>0; pow(1e-30, ...) yields 0 in
				// practice and the result is multiplied by sheenColor
				// which can be 0 too, so this is safe at grazing angles.
				const Scalar p = std::pow( sinTh, invA );
				return (Scalar(2) + invA) * p / (Scalar(2) * PI);
			}

			// Imageworks "Production-Friendly" Charlie shadowing-masking
			// (Estevez & Kulla 2017).
			//
			//   Λ_charlie(α, x) = a/(1 + b·xᶜ) + d·x + e   (x = cos θ ∈ [0, 1])
			//   coefficients a..e are interpolated between the α=0 and α=1
			//   endpoints from Tab. 1 by the weight w = (1 - α)², matching
			//   the Khronos glTF Sample Renderer `lambdaSheenNumericHelper`
			//   convention `mix(α=1-end, α=0-end, (1-α)²)`.  Linear-in-α
			//   blends produce a ~3-8 % albedo drift at mid roughness vs
			//   the reference renderer.
			//
			//   For x < 0.5 we exponentiate the polynomial directly; for
			//   x ≥ 0.5 we use the mid-point reflection
			//     Λ(x ≥ 0.5) = exp(2·L(0.5) − L(1 − x))
			//   so the function is C¹-continuous at x = 0.5.
			//
			//   V(α, n·l, n·v) = 1 / ((1 + Λ(α, n·l) + Λ(α, n·v)) · 4·n·l·n·v)
			//
			// Energy-bounded by construction; no per-sample clamps.
			inline Scalar LambdaPoly( const Scalar a, const Scalar b, const Scalar c,
			                          const Scalar d, const Scalar e, const Scalar x )
			{
				return a / ( Scalar(1) + b * std::pow( x, c ) ) + d * x + e;
			}

			inline Scalar Lambda( const Scalar alpha, const Scalar cosTheta )
			{
				// Coefficients from Estevez & Kulla 2017 Tab. 1:
				//   α = 0:  (a, b, c, d, e) = (25.3245,  3.32435, 0.16801, -1.27393, -4.85967)
				//   α = 1:  (a, b, c, d, e) = (21.5473,  3.82987, 0.19823, -1.97760, -4.32054)
				// Khronos blends them with weight w = (1 - α)² on the
				// α = 0 endpoint:  coeff = w·(α=0) + (1-w)·(α=1)
				//   α = 0 → w = 1 → α=0 coefficients (25.3245, ...)
				//   α = 1 → w = 0 → α=1 coefficients (21.5473, ...)
				//   α = 0.5 → w = 0.25 → 25 % toward α=0 endpoint
				const Scalar t = r_max( Scalar(0), r_min( Scalar(1), alpha ) );
				const Scalar w = (Scalar(1) - t) * (Scalar(1) - t);
				const Scalar oneMinusW = Scalar(1) - w;
				const Scalar a = Scalar( 25.3245 ) * w + Scalar( 21.5473 ) * oneMinusW;
				const Scalar b = Scalar(  3.32435) * w + Scalar(  3.82987) * oneMinusW;
				const Scalar c = Scalar(  0.16801) * w + Scalar(  0.19823) * oneMinusW;
				const Scalar d = Scalar( -1.27393) * w + Scalar( -1.97760) * oneMinusW;
				const Scalar e = Scalar( -4.85967) * w + Scalar( -4.32054) * oneMinusW;

				const Scalar x = r_max( Scalar(0), r_min( Scalar(1), cosTheta ) );
				const Scalar L = ( x < Scalar(0.5) )
					? LambdaPoly( a, b, c, d, e, x )
					: ( Scalar(2) * LambdaPoly( a, b, c, d, e, Scalar(0.5) )
					  - LambdaPoly( a, b, c, d, e, Scalar(1) - x ) );
				return std::exp( L );
			}

			inline Scalar V( const Scalar alpha, const Scalar nDotL, const Scalar nDotV )
			{
				const Scalar cosProd = nDotL * nDotV;
				if( cosProd < Scalar(1e-6) ) return Scalar(0);
				const Scalar denom = ( Scalar(1) + Lambda( alpha, nDotL ) + Lambda( alpha, nDotV ) )
				                   * Scalar(4) * cosProd;
				if( denom < Scalar(1e-6) ) return Scalar(0);
				return Scalar(1) / denom;
			}
		}
	}
}

#endif
