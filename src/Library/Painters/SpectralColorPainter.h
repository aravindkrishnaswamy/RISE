//////////////////////////////////////////////////////////////////////
//
//  SpectralColorPainter.h - Defines a painter that paints some
//  uniform color which comes from a spectrum.  This is the only
//  painter that currently properly implements the GetColorNM function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 14, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_COLOR_PAINTER_
#define SPECTRAL_COLOR_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class SpectralColorPainter : public Painter
		{
		protected:
			RISEPel					color;
			const SpectralPacket	spectrum;

			virtual ~SpectralColorPainter();

		public:
			SpectralColorPainter( const SpectralPacket& spectrum_, const Scalar scale );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ){ return 0;};
			void SetIntermediateValue( const IKeyframeParameter& val ){};
			void RegenerateData( ){};
		};
	}
}

#endif


