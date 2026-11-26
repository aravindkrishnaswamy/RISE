//////////////////////////////////////////////////////////////////////
//
//  SimplexNoise.h - Defines 3D simplex noise.
//  Uses a tetrahedral simplex grid instead of a cubic grid,
//  reducing directional artifacts and requiring fewer gradient
//  computations than classic Perlin noise.
//
//  Reference: Perlin 2002 "Improving Noise"
//  Reference: Gustavson 2005 "Simplex noise demystified"
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMPLEX_NOISE_
#define SIMPLEX_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SimplexNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~SimplexNoise3D();

			Scalar				dPersistence;
			int					numOctaves;
			Scalar*				pAmplitudesLUT;
			Scalar*				pFrequenciesLUT;
			Scalar				dNormFactor;

			/// Evaluate a single octave of 3D simplex noise.
			/// Returns a value in approximately [-1, 1].
			static Scalar RawSimplex3D( Scalar x, Scalar y, Scalar z );

			/// Hash function for gradient index
			static inline int Hash( int i );

			/// Gradient dot product
			static Scalar GradDot( int hash, Scalar x, Scalar y, Scalar z );

		public:
			SimplexNoise3D(
				const Scalar dPersistence_,
				const int numOctaves_
			);

			/// Evaluates FBM simplex noise at (x,y,z).
			/// Returns a value normalized to [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
