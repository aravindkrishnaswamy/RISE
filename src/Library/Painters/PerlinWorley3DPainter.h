//////////////////////////////////////////////////////////////////////
//
//  PerlinWorley3DPainter.h - Declaration of a painter that uses
//  the Perlin-Worley hybrid noise (cloud noise) to blend between
//  two painters.  Produces puffy, cloud-like density patterns.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERLIN_WORLEY_3D_PAINTER_
#define PERLIN_WORLEY_3D_PAINTER_

#include "Painter.h"
#include "../Noise/PerlinWorleyNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class PerlinWorley3DPainter : public Painter
		{
		protected:
			virtual ~PerlinWorley3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;
			Scalar					dWorleyJitter;
			Scalar					dBlend;

			PerlinWorleyNoise3D*			pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			PerlinWorley3DPainter(
				const Scalar dPersistence,
				const unsigned int nOctaves,
				const Scalar dWorleyJitter,
				const Scalar dBlend,
				const IPainter& cA_,
				const IPainter& cB_,
				const Vector3& vScale_,
				const Vector3& vShift_
			);

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
