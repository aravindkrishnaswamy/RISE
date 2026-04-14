//////////////////////////////////////////////////////////////////////
//
//  WorleyNoise.h - Defines 3D Worley (cellular) noise.
//  Computes distance to the Nth nearest feature point on a
//  jittered grid.  Supports F1, F2, and F2-F1 distance metrics.
//
//  Reference: Worley 1996 "A Cellular Texture Basis Function"
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORLEY_NOISE_
#define WORLEY_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		/// Distance metric used for the Worley noise output.
		enum WorleyDistanceMetric
		{
			eWorley_Euclidean = 0,		///< Standard Euclidean distance
			eWorley_Manhattan = 1,		///< Manhattan (L1) distance
			eWorley_Chebyshev = 2		///< Chebyshev (L-infinity) distance
		};

		/// Which F-value combination to use for the output.
		enum WorleyOutputMode
		{
			eWorley_F1 = 0,			///< Distance to nearest feature point
			eWorley_F2 = 1,			///< Distance to second-nearest
			eWorley_F2minusF1 = 2		///< F2 - F1 (produces vein/membrane patterns)
		};

		class WorleyNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~WorleyNoise3D();

			Scalar				dJitter;			///< Jitter amount [0,1], 1=full randomization
			WorleyDistanceMetric	eMetric;
			WorleyOutputMode		eOutput;

			/// Hash a 3D integer cell coordinate to produce a pseudo-random value in [0,1)
			static inline unsigned int HashCell( int ix, int iy, int iz );

			/// Compute distance based on the selected metric
			inline Scalar ComputeDistance( Scalar dx, Scalar dy, Scalar dz ) const;

		public:
			WorleyNoise3D(
				const Scalar dJitter_,
				const WorleyDistanceMetric eMetric_,
				const WorleyOutputMode eOutput_
			);

			/// Evaluates Worley noise at (x,y,z).
			/// Returns a value normalized to approximately [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
