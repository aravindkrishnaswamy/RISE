//////////////////////////////////////////////////////////////////////
//
//  Painter.h - Implementation help for painters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PAINTER_
#define PAINTER_

#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Painter : public virtual IPainter, public virtual Reference
		{
		protected:
			SpectralPacket dummy_spectrum;

			Painter() : dummy_spectrum(400,700,1) {};
			virtual ~Painter(){};

		public:
			virtual Scalar						GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual SpectralPacket				GetSpectrum( const RayIntersectionGeometric& ri ) const;

			// For the Function2D interface
			virtual Scalar			Evaluate( const Scalar x, const Scalar y ) const;
		};
	}
}

#endif
