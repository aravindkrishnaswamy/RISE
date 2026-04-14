//////////////////////////////////////////////////////////////////////
//
//  DomainWarp3DPainter.h - Declaration of a painter that uses
//  domain-warped 3D noise to blend between two painters.
//  Produces swirling, organic marble/lava-like structures.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DOMAIN_WARP_3D_PAINTER_
#define DOMAIN_WARP_3D_PAINTER_

#include "Painter.h"
#include "../Noise/DomainWarpNoise.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class DomainWarp3DPainter : public Painter
		{
		protected:
			virtual ~DomainWarp3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			Scalar					dPersistence;
			unsigned int			nOctaves;
			Scalar					dWarpAmplitude;
			unsigned int			nWarpLevels;

			DomainWarpNoise3D*				pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			DomainWarp3DPainter(
				const Scalar dPersistence,
				const unsigned int nOctaves,
				const Scalar dWarpAmplitude,
				const unsigned int nWarpLevels,
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
