//////////////////////////////////////////////////////////////////////
//
//  UniformColorPainter.h - Defines a painter that paints some
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

#ifndef UNIFORM_COLOR_PAINTER_
#define UNIFORM_COLOR_PAINTER_

#include "Painter.h"
#include "../Animation/KeyframableHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class UniformColorPainter : public Painter
		{
		protected:
			RISEPel	C;
			Scalar	Cnm;
			virtual ~UniformColorPainter(){};

		public:
			UniformColorPainter( const RISEPel& C_ ) : C( C_ ), Cnm( ColorMath::MaxValue(C_)){};

			RISEPel			GetColor( const RayIntersectionGeometric& ) const { return C; }
			Scalar			GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const { return Cnm; }

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{ 
				if( name == "risepel" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						IKeyframeParameter* p = new Parameter<RISEPel>( RISEPel(d), 100 );
						GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
						return p;
					}					
				}

				return 0;		
			};

			void SetIntermediateValue( const IKeyframeParameter& val )
			{
				if( val.getID() == 100 ) {
					C = *(RISEPel*)val.getValue();
					Cnm = ColorMath::MaxValue(C);
				}
			}

			void RegenerateData( ){};
		};
	}
}

#endif


