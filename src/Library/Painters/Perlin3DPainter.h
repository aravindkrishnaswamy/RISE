//////////////////////////////////////////////////////////////////////
//
//  Perlin3DPainter.h - Declaration of a painter that can paint
//  a 3D perlin noise function.  The painter looks at 
//  the ray's intersection point to determine the 3D paramters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERLIN_3D_PAINTER_
#define PERLIN_3D_PAINTER_

#include "Painter.h"
#include "../Noise/PerlinNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Perlin3DPainter : public Painter
		{
		protected:
			virtual ~Perlin3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;

			PerlinNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Perlin3DPainter( const Scalar dPersistence, const unsigned int nOctaves, const IPainter& cA_, const IPainter& cB_, const Vector3& vScale_, const Vector3& vShift_ );

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
