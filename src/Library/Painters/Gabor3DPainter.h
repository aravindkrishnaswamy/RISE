//////////////////////////////////////////////////////////////////////
//
//  Gabor3DPainter.h - Painter using 3D Gabor noise for
//  anisotropic, directionally-structured volumes.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GABOR_3D_PAINTER_
#define GABOR_3D_PAINTER_

#include "Painter.h"
#include "../Noise/GaborNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class Gabor3DPainter : public Painter
		{
		protected:
			virtual ~Gabor3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dFrequency;
			Scalar					dBandwidth;
			Vector3					vOrientation;
			Scalar					dImpulseDensity;

			GaborNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			Gabor3DPainter(
				const Scalar dFrequency,
				const Scalar dBandwidth,
				const Vector3& vOrientation,
				const Scalar dImpulseDensity,
				const IPainter& cA_,
				const IPainter& cB_,
				const Vector3& vScale_,
				const Vector3& vShift_
			);

			RISEPel							GetColor( const RayIntersectionGeometric& ri  ) const;
			Scalar							GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif
