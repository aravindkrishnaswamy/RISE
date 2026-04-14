//////////////////////////////////////////////////////////////////////
//
//  ReactionDiffusion3DPainter.h - Painter using 3D reaction-diffusion
//  (Turing) patterns for biological/organic volume structures.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef REACTION_DIFFUSION_3D_PAINTER_
#define REACTION_DIFFUSION_3D_PAINTER_

#include "Painter.h"
#include "../Noise/ReactionDiffusion.h"
#include "../Interfaces/ISimpleInterpolator.h"

namespace RISE
{
	namespace Implementation
	{
		class ReactionDiffusion3DPainter : public Painter
		{
		protected:
			virtual ~ReactionDiffusion3DPainter();

			const IPainter&			a;
			const IPainter&			b;

			Vector3					vScale;
			Vector3					vShift;

			unsigned int			nGridSize;
			Scalar					dDa;
			Scalar					dDb;
			Scalar					dFeed;
			Scalar					dKill;
			unsigned int			nIterations;

			ReactionDiffusion3D*			pFunc;
			ISimpleInterpolator<Scalar>*	pInterp;
			ISimpleInterpolator<RISEPel>*	pColorInterp;

		public:
			ReactionDiffusion3DPainter(
				const unsigned int nGridSize,
				const Scalar dDa,
				const Scalar dDb,
				const Scalar dFeed,
				const Scalar dKill,
				const unsigned int nIterations,
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
