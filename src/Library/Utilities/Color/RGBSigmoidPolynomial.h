//////////////////////////////////////////////////////////////////////
//
//  RGBSigmoidPolynomial.h - Jakob-Hanika 2019 sigmoid spectral
//    representation.  Three floats encode an entire reflectance
//    spectrum:
//
//      S(λ) = sigmoid(c0·λ̃² + c1·λ̃ + c2)
//
//    where λ̃ = (λ_nm − 380) / 400 is the wavelength normalized to
//    [0, 1] over the visible range.  The normalization is the same
//    used by tools/JakobHanikaLUTGen.cpp (which generates the LUT
//    consumed by RGBToSpectrumTable); both ends MUST agree on the
//    convention or the LUT lookup is meaningless.
//
//    The sigmoid form `0.5 + x/(2·sqrt(1+x²))` is PBRT-v4's stable
//    variant — does not overflow for large |x|, avoids the
//    `exp(x)` denormal trap of the textbook sigmoid.
//
//    Values are intrinsically bounded in [0, 1], smooth, and
//    differentiable.  Cost per wavelength evaluation: ~6 FLOPs.
//
//    Used by:
//     - RGBAlbedoSpectrum (bounded reflectance ∈ [0, 1])
//     - RGBUnboundedSpectrum (radiance / illuminant; sigmoid · scale)
//     - RGBIlluminantSpectrum (illuminant; sigmoid · scale · refSPD,
//       where refSPD matches the LUT target's whitepoint — D65 for
//       Rec.709 post 2026-05, D50 for the historical ROMM LUT)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RGB_SIGMOID_POLYNOMIAL_
#define RGB_SIGMOID_POLYNOMIAL_

#include "../Math3D/Math3D.h"
#include <cmath>

namespace RISE
{
	struct RGBSigmoidPolynomial
	{
		Scalar c0, c1, c2;

		// Wavelength normalization MUST match tools/JakobHanikaLUTGen.cpp
		// (which writes the LUT consumed by RGBToSpectrumTable).  See
		// that file's header comment for the rationale.
		static const int kLambdaMin  = 380;
		static const int kLambdaMax  = 780;

		RGBSigmoidPolynomial() : c0(0), c1(0), c2(0) {}
		RGBSigmoidPolynomial( Scalar c0_, Scalar c1_, Scalar c2_ )
		  : c0(c0_), c1(c1_), c2(c2_) {}

		// Evaluate S(λ) at a single wavelength in nm.
		// Returns 0 outside [380, 780] — outside-range queries are
		// physically meaningless and the LUT carries no information
		// there.
		inline Scalar Eval( Scalar lambda_nm ) const
		{
			if( lambda_nm < Scalar(kLambdaMin) || lambda_nm > Scalar(kLambdaMax) ) {
				return Scalar(0);
			}
			const Scalar t = ( lambda_nm - Scalar(kLambdaMin) ) /
			                 ( Scalar(kLambdaMax - kLambdaMin) );
			const Scalar x = c0 * t * t + c1 * t + c2;
			// PBRT-v4 stable sigmoid: avoids overflow at large |x|.
			//   sigmoid(x) = 0.5 + x / (2·sqrt(1+x²))
			return Scalar(0.5) + x / (Scalar(2) * std::sqrt(Scalar(1) + x * x));
		}
	};
}

#endif
