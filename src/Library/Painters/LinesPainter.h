//////////////////////////////////////////////////////////////////////
//
//  LinesPainter.h - Declaration of a painter which returns a 
//  a pattern of lines
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LINESPAINTER_
#define LINESPAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class LinesPainter : public Painter
		{
		protected:
			const IPainter&	a;
			const IPainter&	b;
			Scalar			dSize;
			const bool		bVertical;

			virtual ~LinesPainter( );

			inline const IPainter& ComputeWhich( const RayIntersectionGeometric& ri ) const;

		public:
			LinesPainter( Scalar dSize_, const IPainter& a_, const IPainter& b_, bool bVertical_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			SpectralPacket					GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			//! SINGLE-SOURCE SELECTOR (Stage C slice 2): `ComputeWhich`
			//! picks ONE child and this forwards to that child's
			//! `GetRadianceNM`.  The generic composed-`GetColor` default is
			//! for painters that BLEND several sources; a selector emits
			//! exactly one child's spectrum, so forwarding preserves a
			//! physical SPD bound to an emissive slot (the default would
			//! re-uplift its RGB projection -- and emit BLACK for a
			//! `piecewise_linear_function`-backed child).
			Scalar							GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
