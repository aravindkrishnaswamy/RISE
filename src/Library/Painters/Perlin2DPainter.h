//////////////////////////////////////////////////////////////////////
//
//  Perlin2DPainter.h - Declaration of a painter that can paint
//  a 2D perlin noise function.  The painter looks at 
//  the ray's texture co-ordinates to determine the 3D parameters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERLIN_2D_PAINTER_
#define PERLIN_2D_PAINTER_

#include "Painter.h"
#include "../Noise/PerlinNoise.h"
#include "../Utilities/SimpleInterpolators.h"

namespace RISE
{
	namespace Implementation
	{
		class Perlin2DPainter : public Painter
		{
		protected:
			virtual ~Perlin2DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector2					vScale;
			Vector2					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;

			PerlinNoise2D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Perlin2DPainter( const Scalar dPersistence, const unsigned int nOctaves, const IPainter& cA_, const IPainter& cB_, const Vector2& vScale_, const Vector2& vShift_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// For the Function2D interface
			Scalar							Evaluate( const Scalar x, const Scalar y ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
