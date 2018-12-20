//////////////////////////////////////////////////////////////////////
//
//  BlendPainter.h - Defines a painter that paints some
//  uniform color
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BLEND_PAINTER_
#define BLEND_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class BlendPainter : public Painter
		{
		protected:
			const IPainter&	a;
			const IPainter& b;
			const IPainter& mask;

			virtual ~BlendPainter()
			{
				a.release();
				b.release();
				mask.release();
			};

		public:

			BlendPainter( 
				const IPainter&	a_,
				const IPainter&	b_,
				const IPainter&	mask_
				) : 
			a( a_ ),
			b( b_ ),
			mask( mask_ )
			{
				a.addref();
				b.addref();
				mask.addref();
			};

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const 
			{ 
				const RISEPel ca = a.GetColor( ri );
				const RISEPel cb = b.GetColor( ri );
				const RISEPel cmask = mask.GetColor( ri );

				return ca*cmask + cb*(RISEPel(1.0,1.0,1.0)-cmask);
			}

			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const 
			{ 
				const Scalar dmask = mask.GetColorNM( ri, nm );
				return (a.GetColorNM(ri,nm)*dmask + b.GetColorNM(ri,nm)*(1.0-dmask));
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif


