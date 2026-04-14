//////////////////////////////////////////////////////////////////////
//
//  PerlinWorleyNoise.h - Defines 3D Perlin-Worley hybrid noise.
//  Combines Perlin FBM with inverted Worley F1 for cloud-like
//  density patterns.  The blend factor controls the mix between
//  smooth Perlin structure and cellular Worley erosion.
//
//  Reference: Schneider & Vos 2015, "The Real-Time Volumetric
//  Cloudscapes of Horizon: Zero Dawn" (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERLIN_WORLEY_NOISE_
#define PERLIN_WORLEY_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"
#include "PerlinNoise.h"
#include "WorleyNoise.h"

namespace RISE
{
	namespace Implementation
	{
		class PerlinWorleyNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~PerlinWorleyNoise3D();

			PerlinNoise3D*		pPerlin;
			WorleyNoise3D*		pWorley;
			Scalar				dBlend;		///< Blend factor [0,1]: 0=pure Perlin, 1=pure (1-Worley)

		public:
			PerlinWorleyNoise3D(
				const RealSimpleInterpolator& interp,
				const Scalar dPersistence,
				const int numOctaves,
				const Scalar dWorleyJitter,
				const Scalar dBlend_
			);

			/// Evaluates the Perlin-Worley hybrid:
			///   result = lerp( perlin, 1-worleyF1, blend )
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
