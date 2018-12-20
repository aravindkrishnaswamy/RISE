//////////////////////////////////////////////////////////////////////
//
//  Voronoi3DPainter.h - Declaration of a painter which returns a 
//  checker pattern
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 14, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef Voronoi3DPainter_
#define Voronoi3DPainter_

#include "Painter.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Voronoi3DPainter : public Painter
		{
		public:
			typedef std::pair<Point3,IPainter*> Generator;
			typedef std::vector<Generator>		GeneratorsList;

		protected:		
			const IPainter& border;
			const Scalar border_size;
			GeneratorsList	generators;

			virtual ~Voronoi3DPainter( );

			inline const IPainter& ComputeWhich( const RayIntersectionGeometric& ri ) const;

		public:
			Voronoi3DPainter( const GeneratorsList& g, const IPainter& border_, const Scalar border_size_ );

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
