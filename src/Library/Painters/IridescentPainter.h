//////////////////////////////////////////////////////////////////////
//
//  IridescentPainter.h - Declaration of a painter which returns a 
//  checker pattern
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IridescentPainter_
#define IridescentPainter_

#include "Painter.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class IridescentPainter : public Painter
		{
		protected:
			const IPainter&	a;
			const IPainter&	b;

			Scalar			bias;

			ISimpleInterpolator<Scalar>*			pScalarInterp;
			ISimpleInterpolator<RISEPel>*			pInterp;

			virtual ~IridescentPainter( );

		public:
			IridescentPainter( const IPainter& a_, const IPainter& b_, const Scalar bias_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
