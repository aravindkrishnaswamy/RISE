//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhaseFunction.h - Isotropic (uniform) phase function
//
//  Scatters light uniformly over the full sphere.  The phase function
//  value is constant:
//    p(wi, wo) = 1 / (4 * PI)
//
//  This is the simplest possible phase function and corresponds to
//  the g=0 limit of the Henyey-Greenstein phase function.  It is
//  useful for fog and other media without directional preference.
//
//  Header-only implementation — no .cpp file required.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISOTROPIC_PHASE_FUNCTION_
#define ISOTROPIC_PHASE_FUNCTION_

#include "../Interfaces/IPhaseFunction.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Utilities/ISampler.h"

namespace RISE
{
	/// \brief Isotropic phase function: scatters uniformly over the sphere
	///
	/// p(wi, wo) = 1 / (4 * PI) for all direction pairs.
	/// Sampling draws a uniformly distributed direction on the unit sphere.
	class IsotropicPhaseFunction :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
	protected:
		virtual ~IsotropicPhaseFunction()
		{
		}

	public:
		IsotropicPhaseFunction()
		{
		}

		/// \return 1 / (4 * PI) regardless of directions
		Scalar Evaluate(
			const Vector3& wi,
			const Vector3& wo
			) const
		{
			return Scalar(1.0) / FOUR_PI;
		}

		/// Sample a uniformly random direction on the unit sphere
		/// \return Normalized direction vector
		Vector3 Sample(
			const Vector3& wi,
			ISampler& sampler
			) const
		{
			// Uniform sphere sampling via inverse CDF:
			//   cos_theta = 1 - 2*xi1   (uniform in [-1, 1])
			//   phi = 2*PI*xi2
			const Scalar cos_theta = 1.0 - 2.0 * sampler.Get1D();
			const Scalar sin_theta = sqrt( fmax( 0.0, 1.0 - cos_theta * cos_theta ) );
			const Scalar phi = TWO_PI * sampler.Get1D();

			return Vector3(
				sin_theta * cos( phi ),
				sin_theta * sin( phi ),
				cos_theta );
		}

		/// \return 1 / (4 * PI) — same as Evaluate for normalized phase functions
		Scalar Pdf(
			const Vector3& wi,
			const Vector3& wo
			) const
		{
			return Scalar(1.0) / FOUR_PI;
		}
	};
}

#endif
