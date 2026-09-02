//////////////////////////////////////////////////////////////////////
//
//  Function1DSpectralPainter.h - Defines a painter that is a spectral
//    painter which gets its spectral values from a Function1D
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 27, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FUNCTION1D_SPECTRAL_PAINTER_
#define FUNCTION1D_SPECTRAL_PAINTER_

#include "../Interfaces/IFunction1D.h"
#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class Function1DSpectralPainter : public Painter
		{
		protected:
			const IFunction1D&	func;
			virtual ~Function1DSpectralPainter()
			{
				func.release();
			}

		public:
			Function1DSpectralPainter( const IFunction1D& f ) : func( f )
			{
				func.addref();
			}

			RISEPel			GetColor( const RayIntersectionGeometric& ) const
			{
				return RISEPel(0,0,0);
			}

			Scalar			GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const
			{
				return func.Evaluate(nm );
			}

			//! PHYSICAL SPD -- pass through verbatim (Stage C slice 2).
			//! This painter's GetColorNM is not a Jakob-Hanika uplift of an
			//! RGB triple; it is an absolute spectral radiance/reflectance
			//! the author supplied.  Letting the IPainter default run would
			//! throw it away and re-uplift the RGB projection instead, which
			//! is exactly the class of bug the Hosek-Wilkie radiance map is
			//! kept off the painter path to avoid.
			//! Doubly load-bearing here: this painter's `GetColor` returns
			//! BLACK, so the IPainter default would make a luminaire driven
			//! by it emit exactly zero on the spectral path.
			Scalar			GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				return GetColorNM( ri, nm );
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif


