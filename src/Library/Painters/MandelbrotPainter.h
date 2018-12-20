//////////////////////////////////////////////////////////////////////
//
//  MandelbrotPainter.h - Declaration of a painter that can paint
//  a mandelbrot fractal
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  This implementation is similar to Dan McCormick's
//				from Swish
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MANDELBROT_PAINTER_
#define MANDELBROT_PAINTER_

#include "Painter.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class MandelbrotPainter : public Painter
		{
		protected:
			const IPainter&	a;
			const IPainter&	b;

			Scalar	lower_x;
			Scalar	upper_x;
			Scalar	lower_y;
			Scalar	upper_y;

			Scalar	x_range;
			Scalar	y_range;

			Scalar	exp;

			ISimpleInterpolator<Scalar>*			pScalarInterp;
			ISimpleInterpolator<RISEPel>*			pInterp;

			virtual ~MandelbrotPainter();

			inline Scalar ComputeD( const RayIntersectionGeometric& ri ) const;

		public:
			MandelbrotPainter( 
				const IPainter& cA_,
				const IPainter& cB_,
				const Scalar lower_x_,
				const Scalar upper_x_,
				const Scalar lower_y_,
				const Scalar upper_y_, 
				const Scalar exp_
				);

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// For the Function2D interface
			Scalar							Evaluate( const Scalar x, const Scalar y ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
