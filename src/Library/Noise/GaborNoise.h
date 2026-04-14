//////////////////////////////////////////////////////////////////////
//
//  GaborNoise.h - Defines 3D Gabor noise.
//  Sparse convolution noise using oriented Gabor kernels
//  (sinusoidal lobe modulated by Gaussian envelope).
//  Offers precise spectral control with intuitive parameters:
//  frequency, orientation, and bandwidth.
//
//  Reference: Lagae et al. 2009 "Procedural Noise using Sparse
//  Gabor Convolution" (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GABOR_NOISE_
#define GABOR_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class GaborNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~GaborNoise3D();

			Scalar		dFrequency;			///< Principal frequency of the Gabor kernel
			Scalar		dBandwidth;			///< Bandwidth (controls Gaussian width)
			Vector3		vOrientation;		///< Orientation direction (normalized)
			Scalar		dImpulseDensity;	///< Number of impulses per cell
			unsigned int nSeed;				///< Random seed for impulse placement

			/// Evaluate a single Gabor kernel at displacement (dx,dy,dz)
			inline Scalar GaborKernel( Scalar dx, Scalar dy, Scalar dz ) const;

			/// Hash function for cell-based impulse generation
			static unsigned int HashCell( int ix, int iy, int iz, unsigned int seed );

		public:
			GaborNoise3D(
				const Scalar dFrequency_,
				const Scalar dBandwidth_,
				const Vector3& vOrientation_,
				const Scalar dImpulseDensity_,
				const unsigned int nSeed_
			);

			/// Evaluates Gabor noise at (x,y,z).
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
