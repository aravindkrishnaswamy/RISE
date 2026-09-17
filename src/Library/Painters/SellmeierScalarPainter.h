//////////////////////////////////////////////////////////////////////
//
//  SellmeierScalarPainter.h - Analytic Sellmeier IOR formula
//
//    n²(λ) = 1 + Σᵢ (Bᵢ · λ²) / (λ² - Cᵢ),  λ in micrometres
//
//  Three-term form is the textbook standard (BK7, fused silica, etc.).
//  Schott / SCHOTT, Refractiveindex.info, and most optical-glass
//  catalogs publish coefficients in this form.
//
//  RGB REPORTING (DL-82, docs/DEBT_LEDGER.md).  `GetValuesAt` returns
//  the curve evaluated at the three representative wavelengths
//  `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm -- the same
//  convention DL-29 established for `PiecewiseLinearScalarPainter` and
//  the same three `DielectricSPF`'s RGB dispersion loop refracts
//  against.  A Sellmeier curve IS a dispersion formula by construction,
//  so the case for sampling it at these three wavelengths is at least
//  as strong as for a measured file.  Read `ScalarPainterRGB::kChannelNM`'s
//  doc comment (Interfaces/IScalarPainter.h) for why a colorimetric CMF
//  integration would be WRONG here.
//
//  Before DL-82 this class broadcast a single 587.6 nm (d-line) sample
//  into all three channels and `HasPerChannelVariation()` always
//  reported false -- which also kept `DielectricSPF`/`PolishedSPF`/
//  `PerfectRefractorSPF` off their dispersion path for a Sellmeier
//  `ior`, so a BK7/fused-silica dispersion formula rendered exactly
//  achromatic under every RGB rasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SELLMEIER_SCALAR_PAINTER_
#define SELLMEIER_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class SellmeierScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const Scalar B1, B2, B3;
			const Scalar C1, C2, C3;

			//! The curve at `ScalarPainterRGB::kChannelNM`, computed ONCE
			//! at construction (the coefficients are immutable
			//! afterward).  For a single-scalar-slot view (see
			//! `MakeSingleScalarSlotView`) this is instead the green
			//! sample broadcast to all three channels.
			ScalarTriple cachedRGB;

			//! `cachedRGB`'s three channels differ.  EXACT comparison --
			//! see `PiecewiseLinearScalarPainter`'s identical field for
			//! the rationale (one curve, so a flat response gives
			//! bit-identical samples and needs no fuzzy tolerance).
			bool bHasPerChannelVariation;

			//! True for the view `MakeSingleScalarSlotView` hands a
			//! material slot that reads only `.v[0]`.
			const bool bSingleSlotView;

			virtual ~SellmeierScalarPainter() {}

			//! Guard distance: skip the singularity at λ² == Cᵢ.
			//! For standard optical-glass coefficients (Cᵢ in [0.005,
			//! 105] µm²), even UV wavelengths down to 200 nm
			//! (λ² = 0.04 µm²) are far from C₁ ≈ 0.006 µm² by enough
			//! that this guard never fires.  It's defense-in-depth
			//! for hand-authored Sellmeier coefficients.
			static constexpr Scalar kSingularityEps = Scalar( 1e-9 );

			static Scalar SafeTerm( Scalar lam2, Scalar B, Scalar C )
			{
				const Scalar denom = lam2 - C;
				if( std::fabs( denom ) < kSingularityEps ) return Scalar( 0 );
				return ( B * lam2 ) / denom;
			}

			Scalar EvalAtNM( Scalar nm ) const
			{
				// Sellmeier wants λ in micrometres; nm is nanometres.
				const Scalar lam_um = nm * Scalar( 1e-3 );
				const Scalar lam2 = lam_um * lam_um;
				const Scalar t1 = SafeTerm( lam2, B1, C1 );
				const Scalar t2 = SafeTerm( lam2, B2, C2 );
				const Scalar t3 = SafeTerm( lam2, B3, C3 );
				const Scalar n2 = Scalar( 1.0 ) + t1 + t2 + t3;
				return n2 > 0 ? std::sqrt( n2 ) : Scalar( 1.0 );
			}

		public:
			explicit SellmeierScalarPainter(
				Scalar B1_, Scalar B2_, Scalar B3_,
				Scalar C1_, Scalar C2_, Scalar C3_,
				bool bSingleSlotView_ = false
				) :
				B1( B1_ ), B2( B2_ ), B3( B3_ ),
				C1( C1_ ), C2( C2_ ), C3( C3_ ),
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

			//! Structural introspection: the six Sellmeier coefficients.
			Scalar GetB1() const { return B1; }
			Scalar GetB2() const { return B2; }
			Scalar GetB3() const { return B3; }
			Scalar GetC1() const { return C1; }
			Scalar GetC2() const { return C2; }
			Scalar GetC3() const { return C3; }

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

			//! DL-82: a Sellmeier dispersion formula bound to a
			//! single-scalar material slot keeps loading -- the slot
			//! gets the curve's green (549 nm) sample, not whichever
			//! channel lands at `.v[0]`, and the spectral path (which
			//! evaluates the exact formula at every wavelength) is
			//! untouched.  See the base declaration in
			//! Interfaces/IScalarPainter.h.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( bSingleSlotView ) return nullptr;	// already one
				return new SellmeierScalarPainter( B1, B2, B3, C1, C2, C3, true );
			}
		};
	}
}

#endif
