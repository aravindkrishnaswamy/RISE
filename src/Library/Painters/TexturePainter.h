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

			// Landing 2: cached dispatch decision.  Eliminates 2-3
			// virtual calls per per-sample GetColor invocation by
			// resolving the LOD strategy at construction.
			//   Mode_Base       — no LOD support; use GetPEL
			//   Mode_Pyramid    — accessor has mip pyramid
			//   Mode_Supersample— accessor uses footprint stochastic
			//                     supersampling at base (lowmem path)
			enum eFilterMode
			{
				Mode_Base       = 0,
				Mode_Pyramid    = 1,
				Mode_Supersample = 2
			};
			eFilterMode					filter_mode;

			//! Shared LOD-aware sample: dispatches on the cached
			//! filter_mode.  Used by both GetColor (returns .base)
			//! and GetAlpha (returns .a) so the alpha path picks
			//! the same LOD as the colour path — important for
			//! alphaMode = BLEND consistency under minification.
			RISEColor		SampleTextured( const RayIntersectionGeometric& ri ) const;

			virtual ~TexturePainter();

		public:
			TexturePainter( IRasterImageAccessor* pRIA_ );

			RISEPel			GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri  ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif
