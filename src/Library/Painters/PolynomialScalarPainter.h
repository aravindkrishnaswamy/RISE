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
//  RGB REPORTING (DL-82, docs/DEBT_LEDGER.md).  `GetValuesAt` returns
//  the curve evaluated at the three representative wavelengths
//  `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm -- the same
//  convention DL-29 established for `PiecewiseLinearScalarPainter` and
//  `SellmeierScalarPainter`.  Read `ScalarPainterRGB::kChannelNM`'s doc
//  comment (Interfaces/IScalarPainter.h) for why a colorimetric CMF
//  integration would be WRONG here.  A CONSTANT polynomial (only c0
//  nonzero, or every higher-order coefficient zero) samples the same
//  value at all three wavelengths and correctly reports
//  `HasPerChannelVariation() == false` -- `ScalarTriple::IsUniform()`
//  is an exact comparison, so a genuinely flat curve is unaffected by
//  this change.
//
//  Before DL-82 this class broadcast a single 550 nm sample into all
//  three channels and `HasPerChannelVariation()` always reported
//  false -- which also kept `DielectricSPF`/`PolishedSPF`/
//  `PerfectRefractorSPF` off their dispersion path for a polynomial
//  `ior`, so a wavelength-varying polynomial dispersion formula
//  rendered exactly achromatic under every RGB rasterizer.
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

			//! The curve at `ScalarPainterRGB::kChannelNM`, computed ONCE
			//! at construction (the coefficients are immutable
			//! afterward).  For a single-scalar-slot view (see
			//! `MakeSingleScalarSlotView`) this is instead the green
			//! sample broadcast to all three channels.
			ScalarTriple cachedRGB;

			//! `cachedRGB`'s three channels differ.  EXACT comparison --
			//! see `PiecewiseLinearScalarPainter`'s identical field for
			//! the rationale.
			bool bHasPerChannelVariation;

			//! True for the view `MakeSingleScalarSlotView` hands a
			//! material slot that reads only `.v[0]`.
			const bool bSingleSlotView;

			virtual ~PolynomialScalarPainter() {}

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
			explicit PolynomialScalarPainter(
				std::vector<Scalar> c,
				bool bSingleSlotView_ = false
				)
				: coeffs( std::move( c ) ),
				  bHasPerChannelVariation( false ),
				  bSingleSlotView( bSingleSlotView_ )
			{
				if( bSingleSlotView ) {
					cachedRGB = ScalarTriple( EvalAtNM(
						ScalarPainterRGB::kChannelNM[ ScalarPainterRGB::kSingleSampleChannel ] ) );
					bHasPerChannelVariation = false;
				} else {
					cachedRGB = ScalarTriple(
						EvalAtNM( ScalarPainterRGB::kChannelNM[0] ),
						EvalAtNM( ScalarPainterRGB::kChannelNM[1] ),
						EvalAtNM( ScalarPainterRGB::kChannelNM[2] ) );
					bHasPerChannelVariation = !cachedRGB.IsUniform();
				}
			}

			//! Structural introspection: the coefficient list.
			const std::vector<Scalar>& GetCoeffs() const { return coeffs; }

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				return cachedRGB;
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return EvalAtNM( nm );
			}

			bool HasPerChannelVariation() const override { return bHasPerChannelVariation; }

			//! DL-82: a polynomial dispersion formula bound to a
			//! single-scalar material slot keeps loading -- the slot
			//! gets the curve's green (549 nm) sample, not whichever
			//! channel lands at `.v[0]`, and the spectral path is
			//! untouched.  See the base declaration in
			//! Interfaces/IScalarPainter.h.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( bSingleSlotView ) return nullptr;	// already one
				return new PolynomialScalarPainter( coeffs, true );
			}

			//! DL-292: the value depends on the wavelength only, never on
			//! the hit (IScalarPainter::IsPositionIndependent).
			bool IsPositionIndependent() const override { return true; }
		};
	}
}

#endif
