//////////////////////////////////////////////////////////////////////
//
//  CheckerPainter.h - Declaration of a painter which returns a 
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

#ifndef CHECKERPAINTER_
#define CHECKERPAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class CheckerPainter : public Painter
		{
		protected:
			const IPainter&	a;
			const IPainter&	b;
			Scalar			dSize;

			virtual ~CheckerPainter( );

			inline const IPainter& ComputeWhich( const RayIntersectionGeometric& ri ) const;

		public:
			CheckerPainter( Scalar dSize_, const IPainter& a_, const IPainter& b_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
