//////////////////////////////////////////////////////////////////////
//
//  TexturePainter.h - Defines a texture painter, which is a painter
//  that derives color from some raster image
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  Add wrapping and clamping abilities!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEXTUREPAINTER_
#define TEXTUREPAINTER_

#include "Painter.h"
#include "../Interfaces/IRasterImageAccessor.h"

namespace RISE
{
	namespace Implementation
	{
		class TexturePainter : public Painter
		{
		protected:
			IRasterImageAccessor*		pRIA;

			virtual ~TexturePainter();

		public:
			TexturePainter( IRasterImageAccessor* pRIA_ );

			RISEPel			GetColor( const RayIntersectionGeometric& ri  ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif
