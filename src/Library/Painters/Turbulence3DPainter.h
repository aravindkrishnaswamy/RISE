//////////////////////////////////////////////////////////////////////
//
//  Turbulence3DPainter.h - Declaration of a painter that uses
//  3D turbulence noise (absolute-value FBM) to blend between
//  two painters.  Produces wispy, smoke-like structures with
//  sharp creases where noise crosses zero.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TURBULENCE_3D_PAINTER_
#define TURBULENCE_3D_PAINTER_

#include "Painter.h"
#include "../Noise/TurbulenceNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Turbulence3DPainter : public Painter
		{
		protected:
			virtual ~Turbulence3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;

			TurbulenceNoise3D*				pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Turbulence3DPainter( const Scalar dPersistence, const unsigned int nOctaves, const IPainter& cA_, const IPainter& cB_, const Vector3& vScale_, const Vector3& vShift_ );

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
