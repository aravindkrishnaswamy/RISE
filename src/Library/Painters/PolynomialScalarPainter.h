//////////////////////////////////////////////////////////////////////
//
//  PolynomialScalarPainter.h - Polynomial function of wavelength.
//
//    f(λ) = c₀ + c₁·λ + c₂·λ² + … + cₙ·λⁿ
//
//  Useful for analytic dispersion / absorption models that are
//  expressed as low-order polynomials in λ (e.g. Cauchy's equation
//  is approximately n(λ) = A + B/λ² + C/λ⁴ — not strictly polynomial
//  but a polynomial in 1/λ²).  For Cauchy-style models, author the
//  coefficients accordingly or use SellmeierScalarPainter.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLYNOMIAL_SCALAR_PAINTER_
#define POLYNOMIAL_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class PolynomialScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			//! Coefficients c₀, c₁, … cₙ (Horner-form evaluation).
			std::vector<Scalar> coeffs;
			virtual ~PolynomialScalarPainter() {}

			static constexpr Scalar kRepresentativeNm = Scalar( 550.0 );

			Scalar EvalAtNM( Scalar nm ) const
			{
				// Empty coefficient list returns the physical default
				// (1.0).  This is a defensive backstop; the parser
				// rejects empty coefficient lists at scene-construction
				// time so this branch shouldn't be reached in practice.
				// Returning 0 (the obvious "no contribution" value) is
				// unsafe — a downstream consumer dividing by IOR would
				// hit divide-by-zero.  1.0 is the air-IOR / no-effect
				// neutral for every consumer we care about.
				if( coeffs.empty() ) return Scalar( 1 );
				// Horner's method: a₀ + λ(a₁ + λ(a₂ + λ(…)))
				Scalar acc = coeffs.back();
				for( std::ptrdiff_t i = static_cast<std::ptrdiff_t>( coeffs.size() ) - 2; i >= 0; --i ) {
					acc = acc * nm + coeffs[ static_cast<size_t>( i ) ];
				}
				return acc;
			}

		public:
			explicit PolynomialScalarPainter( std::vector<Scalar> c )
				: coeffs( std::move( c ) )
			{}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return ScalarTriple( EvalAtNM( kRepresentativeNm ) );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return EvalAtNM( nm );
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
