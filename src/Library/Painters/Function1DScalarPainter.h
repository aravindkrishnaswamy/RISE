//////////////////////////////////////////////////////////////////////
//
//  Function1DScalarPainter.h - Wraps an existing IFunction1D as a
//    wavelength-varying scalar painter.  Lets authors reuse RISE's
//    existing function-1d infrastructure (piecewise-linear,
//    polynomial, custom C++ subclasses, etc.) as a scalar painter.
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
			virtual ~Function1DScalarPainter()
			{
				if( pFunc ) pFunc->release();
			}

			static constexpr Scalar kRepresentativeNm = Scalar( 555.0 );

		public:
			explicit Function1DScalarPainter( IFunction1D* p ) : pFunc( p )
			{
				if( pFunc ) pFunc->addref();
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& /*ri*/
				) const override
			{
				if( !pFunc ) return ScalarTriple();
				return ScalarTriple( pFunc->Evaluate( kRepresentativeNm ) );
			}

			Scalar GetValueAtNM(
				const RayIntersectionGeometric& /*ri*/,
				Scalar nm
				) const override
			{
				return pFunc ? pFunc->Evaluate( nm ) : Scalar( 0 );
			}

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
