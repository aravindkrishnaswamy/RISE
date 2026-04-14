//////////////////////////////////////////////////////////////////////
//
//  Worley3DPainter.h - Declaration of a painter that uses
//  3D Worley (cellular) noise to blend between two painters.
//  Produces cell/bubble/vein patterns depending on the
//  distance metric and output mode.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORLEY_3D_PAINTER_
#define WORLEY_3D_PAINTER_

#include "Painter.h"
#include "../Noise/WorleyNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Worley3DPainter : public Painter
		{
		protected:
			virtual ~Worley3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dJitter;
			WorleyDistanceMetric	eMetric;
			WorleyOutputMode		eOutput;

			WorleyNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Worley3DPainter(
				const Scalar dJitter,
				const WorleyDistanceMetric eMetric,
				const WorleyOutputMode eOutput,
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
