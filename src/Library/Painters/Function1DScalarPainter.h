//////////////////////////////////////////////////////////////////////
//
//  Function1DScalarPainter.h - Wraps an existing IFunction1D as a
//    wavelength-varying scalar painter.  Lets authors reuse RISE's
//    existing function-1d infrastructure (piecewise-linear,
//    polynomial, custom C++ subclasses, etc.) as a scalar painter.
//
//  RGB REPORTING (DL-82, docs/DEBT_LEDGER.md).  `GetValuesAt` returns
//  the wrapped function evaluated at the three representative
//  wavelengths `ScalarPainterRGB::kChannelNM` = {611, 549, 465} nm --
//  the same convention DL-29 established for
//  `PiecewiseLinearScalarPainter` and `SellmeierScalarPainter`/
//  `PolynomialScalarPainter`.  Read `ScalarPainterRGB::kChannelNM`'s
//  doc comment (Interfaces/IScalarPainter.h) for why a colorimetric
//  CMF integration would be WRONG here.  A wrapped function that
//  happens to be wavelength-independent (e.g. a constant) samples the
//  same value at all three wavelengths and correctly reports
//  `HasPerChannelVariation() == false` -- `ScalarTriple::IsUniform()`
//  is an exact comparison.
//
//  Before DL-82 this class broadcast a single 555 nm sample into all
//  three channels and `HasPerChannelVariation()` always reported
//  false -- which also kept `DielectricSPF`/`PolishedSPF`/
//  `PerfectRefractorSPF` off their dispersion path for a Function1D
//  `ior`, so a wavelength-varying wrapped function rendered exactly
//  achromatic under every RGB rasterizer.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION1D_SCALAR_PAINTER_
#define FUNCTION1D_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IFunction1D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Function1DScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			IFunction1D* const pFunc;

			//! The curve at `ScalarPainterRGB::kChannelNM`, computed ONCE
			//! at construction (the wrapped function is queried only at
			//! construction time for this cache; `GetValueAtNM` still
			//! queries it live for the spectral path).  For a
			//! single-scalar-slot view (see `MakeSingleScalarSlotView`)
			//! this is instead the green sample broadcast to all three
			//! channels.  Zero (uniform) when `pFunc` is null.
			ScalarTriple cachedRGB;

			//! `cachedRGB`'s three channels differ.  EXACT comparison --
			//! see `PiecewiseLinearScalarPainter`'s identical field for
			//! the rationale.
			bool bHasPerChannelVariation;

			//! True for the view `MakeSingleScalarSlotView` hands a
			//! material slot that reads only `.v[0]`.
			const bool bSingleSlotView;

			virtual ~Function1DScalarPainter()
			{
				if( pFunc ) pFunc->release();
			}

		public:
			explicit Function1DScalarPainter( IFunction1D* p, bool bSingleSlotView_ = false )
				: pFunc( p ),
				  bHasPerChannelVariation( false ),
				  bSingleSlotView( bSingleSlotView_ )
			{
				if( pFunc ) pFunc->addref();

				if( !pFunc ) {
					// cachedRGB stays the default zero triple -- uniform.
				} else if( bSingleSlotView ) {
					cachedRGB = ScalarTriple( pFunc->Evaluate(
						ScalarPainterRGB::kChannelNM[ ScalarPainterRGB::kSingleSampleChannel ] ) );
				} else {
					cachedRGB = ScalarTriple(
						pFunc->Evaluate( ScalarPainterRGB::kChannelNM[0] ),
						pFunc->Evaluate( ScalarPainterRGB::kChannelNM[1] ),
						pFunc->Evaluate( ScalarPainterRGB::kChannelNM[2] ) );
					bHasPerChannelVariation = !cachedRGB.IsUniform();
				}
			}

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
				return pFunc ? pFunc->Evaluate( nm ) : Scalar( 0 );
			}

			bool HasPerChannelVariation() const override { return bHasPerChannelVariation; }

			//! DL-82: a wrapped IFunction1D bound to a single-scalar
			//! material slot keeps loading -- the slot gets the curve's
			//! green (549 nm) sample, not whichever channel lands at
			//! `.v[0]`, and the spectral path is untouched.  See the
			//! base declaration in Interfaces/IScalarPainter.h.
			IScalarPainter* MakeSingleScalarSlotView() const override
			{
				if( bSingleSlotView ) return nullptr;	// already one
				return new Function1DScalarPainter( pFunc, true );
			}

			//! DL-292: the value depends on the wavelength only, never on
			//! the hit (IScalarPainter::IsPositionIndependent).
			bool IsPositionIndependent() const override { return true; }
		};
	}
}

#endif
