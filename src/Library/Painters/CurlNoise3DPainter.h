//////////////////////////////////////////////////////////////////////
//
//  CurlNoise3DPainter.h - Declaration of a painter that uses
//  3D curl noise to blend between two painters.  Produces
//  swirling, turbulent structures resembling frozen fluid flow.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CURL_NOISE_3D_PAINTER_
#define CURL_NOISE_3D_PAINTER_

#include "Painter.h"
#include "../Noise/CurlNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class CurlNoise3DPainter : public Painter
		{
		protected:
			virtual ~CurlNoise3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;
			Scalar					dEpsilon;

			CurlNoise3D*					pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			CurlNoise3DPainter(
				const Scalar dPersistence,
				const unsigned int nOctaves,
				const Scalar dEpsilon,
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
