//////////////////////////////////////////////////////////////////////
//
//  VertexColorPainter.h - A painter that returns the per-vertex color
//  interpolated by the geometry at the hit point (`ri.vColor`).  Falls
//  back to a configured default color for hits on geometry that does
//  not carry vertex colors (every IGeometry except a coloured indexed
//  triangle mesh today).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VERTEX_COLOR_PAINTER_
#define VERTEX_COLOR_PAINTER_

#include "Painter.h"
#include "../Animation/KeyframableHelper.h"

namespace RISE
{
	namespace Implementation
	{
		class VertexColorPainter : public Painter
		{
		protected:
			RISEPel	Cdefault;	// Fallback color when ri.bHasVertexColor is false
			Scalar	CnmDefault;	// Cached per-wavelength scalar for the fallback path
			virtual ~VertexColorPainter(){};

		public:
			VertexColorPainter( const RISEPel& fallback ) :
			  Cdefault( fallback ),
			  CnmDefault( ColorMath::MaxValue( fallback ) )
			{};

			RISEPel	GetColor( const RayIntersectionGeometric& ri ) const
			{
				return ri.bHasVertexColor ? ri.vColor : Cdefault;
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar /*nm*/ ) const
			{
				return ri.bHasVertexColor
					? ColorMath::MaxValue( ri.vColor )
					: CnmDefault;
			}

			// Keyframable interface — drives the fallback color over time.
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value )
			{
				if( name == "fallback_risepel" ) {
					double d[3];
					if( sscanf( value.c_str(), "%lf %lf %lf", &d[0], &d[1], &d[2] ) == 3 ) {
						IKeyframeParameter* p = new Parameter<RISEPel>( RISEPel(d), 200 );
						GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
						return p;
					}
				}
				return 0;
			};

			void SetIntermediateValue( const IKeyframeParameter& val )
			{
				if( val.getID() == 200 ) {
					Cdefault = *(RISEPel*)val.getValue();
					CnmDefault = ColorMath::MaxValue( Cdefault );
				}
			}

			void RegenerateData( ){};
		};
	}
}

#endif
