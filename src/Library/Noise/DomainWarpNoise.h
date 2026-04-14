//////////////////////////////////////////////////////////////////////
//
//  DomainWarpNoise.h - Defines 3D domain-warped noise.
//  Evaluates noise at distorted coordinates:
//    noise(p + amplitude * vec3(noise(p+off1), noise(p+off2), noise(p+off3)))
//  Nesting produces increasingly organic, swirling structures.
//
//  Reference: Inigo Quilez, "Domain Warping"
//  https://iquilezles.org/articles/warp/
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DOMAIN_WARP_NOISE_
#define DOMAIN_WARP_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"
#include "PerlinNoise.h"

namespace RISE
{
	namespace Implementation
	{
		class DomainWarpNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~DomainWarpNoise3D();

			PerlinNoise3D*		pNoise;
			Scalar				dWarpAmplitude;		///< How much to displace coordinates
			unsigned int		nWarpLevels;		///< Number of nested warp levels (1-3)

		public:
			DomainWarpNoise3D(
				const RealSimpleInterpolator& interp,
				const Scalar dPersistence,
				const int numOctaves,
				const Scalar dWarpAmplitude_,
				const unsigned int nWarpLevels_
			);

			/// Evaluates domain-warped noise.
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
