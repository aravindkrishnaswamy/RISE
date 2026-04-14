//////////////////////////////////////////////////////////////////////
//
//  SDF3DPainter.h - Declaration of a painter that uses signed
//  distance field primitives to blend between two painters.
//  Creates geometric volume shapes with optional noise displacement.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SDF_3D_PAINTER_
#define SDF_3D_PAINTER_

#include "Painter.h"
#include "../Noise/SDFPrimitives.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class SDF3DPainter : public Painter
		{
		protected:
			virtual ~SDF3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			SDFPrimitiveType		eType;
			Scalar					dParam1;
			Scalar					dParam2;
			Scalar					dParam3;
			Scalar					dShellThickness;
			Scalar					dNoiseAmplitude;
			Scalar					dNoiseFrequency;

			SDFPrimitive3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			SDF3DPainter(
				const SDFPrimitiveType eType,
				const Scalar dParam1,
				const Scalar dParam2,
				const Scalar dParam3,
				const Scalar dShellThickness,
				const Scalar dNoiseAmplitude,
				const Scalar dNoiseFrequency,
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
