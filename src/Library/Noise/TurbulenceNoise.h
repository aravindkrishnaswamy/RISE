//////////////////////////////////////////////////////////////////////
//
//  TurbulenceNoise.h - Defines turbulence noise functions.
//  Turbulence is the sum of absolute values of noise octaves,
//  producing sharp creases where noise crosses zero.  This
//  gives a wispy, smoke-like character distinct from smooth
//  Perlin FBM.
//
//  Reference: Perlin 1985 "An Image Synthesizer"
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TURBULENCE_NOISE_
#define TURBULENCE_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "InterpolatedNoise.h"

namespace RISE
{
	namespace Implementation
	{
		class TurbulenceNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~TurbulenceNoise3D();

			InterpolatedNoise3D*		noise;
			Scalar						persistence;
			int							numOctaves;
			int							n;
			Scalar*						pAmplitudesLUT;
			Scalar						dNormFactor;

		public:
			TurbulenceNoise3D( const RealSimpleInterpolator& interp, const Scalar persistence_, const int numOctaves_ );

			/// Evaluates turbulence: sum of |noise| at each octave,
			/// normalized to [0, 1] by dividing by the sum of amplitudes.
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
