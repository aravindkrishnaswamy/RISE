//////////////////////////////////////////////////////////////////////
//
//  CurlNoise.h - Defines 3D curl noise.
//  Computes the curl of a 3D noise potential field using finite
//  differences.  The magnitude of the resulting divergence-free
//  vector field is used as the output scalar, producing swirling,
//  turbulent structures.
//
//  Reference: Bridson et al. 2007 "Curl-noise for procedural
//  fluid flow" (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CURL_NOISE_
#define CURL_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"
#include "PerlinNoise.h"

namespace RISE
{
	namespace Implementation
	{
		class CurlNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~CurlNoise3D();

			PerlinNoise3D*		pNoiseA;	///< Potential field component A
			PerlinNoise3D*		pNoiseB;	///< Potential field component B
			PerlinNoise3D*		pNoiseC;	///< Potential field component C
			Scalar				dEpsilon;	///< Finite difference step size

		public:
			CurlNoise3D(
				const RealSimpleInterpolator& interp,
				const Scalar dPersistence,
				const int numOctaves,
				const Scalar dEpsilon_
			);

			/// Evaluates curl noise magnitude at (x,y,z).
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
