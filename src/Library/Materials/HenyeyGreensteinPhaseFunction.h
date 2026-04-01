//////////////////////////////////////////////////////////////////////
//
//  HenyeyGreensteinPhaseFunction.h - Henyey-Greenstein phase function
//
//  The Henyey-Greenstein (HG) phase function is the most widely used
//  single-parameter model for anisotropic scattering in participating
//  media.  It is parameterized by an asymmetry factor g in [-1, 1]:
//
//    p(cos_theta) = (1 - g^2) / (4*PI * (1 + g^2 - 2*g*cos_theta)^(3/2))
//
//  where cos_theta = dot(wi, wo).
//
//    g > 0  :  forward scattering (common in fog, clouds, skin)
//    g = 0  :  isotropic (reduces to 1/(4*PI))
//    g < 0  :  backward scattering
//
//  Sampling uses the closed-form inverse CDF:
//    cos_theta = (1/(2g)) * (1 + g^2 - ((1-g^2)/(1-g+2g*xi))^2)
//
//  This implementation is the canonical HG code in RISE.  The
//  static helper SampleWithG() is provided for callers that need
//  HG sampling with a per-call g parameter (e.g. BioSpec tissue
//  scattering where g varies with wavelength).
//
//  Aligned with Blender/Cycles HG implementation in
//  intern/cycles/kernel/closure/volume_util.h.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HENYEY_GREENSTEIN_PHASE_FUNCTION_
#define HENYEY_GREENSTEIN_PHASE_FUNCTION_

#include "../Interfaces/IPhaseFunction.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/ISampler.h"

namespace RISE
{
	/// \brief Henyey-Greenstein anisotropic scattering phase function
	///
	/// Provides Evaluate, Sample, and Pdf for a fixed asymmetry factor g.
	/// Also exposes static helpers (EvaluateWithG, SampleWithG) for callers
	/// that need the HG formula with a varying g parameter.
	class HenyeyGreensteinPhaseFunction :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
	protected:
		const Scalar m_g;		///< Asymmetry factor [-1, 1]

		virtual ~HenyeyGreensteinPhaseFunction();

	public:
		/// Construct with asymmetry factor g.
		/// g=0 gives isotropic, g>0 forward, g<0 backward scattering.
		HenyeyGreensteinPhaseFunction(
			const Scalar g
			);

		Scalar Evaluate(
			const Vector3& wi,
			const Vector3& wo
			) const;

		Vector3 Sample(
			const Vector3& wi,
			ISampler& sampler
			) const;

		Scalar Pdf(
			const Vector3& wi,
			const Vector3& wo
			) const;

		//
		// Static helpers for callers that need HG with a per-call g value.
		// These are the canonical implementations — the instance methods
		// above delegate to them.
		//

		/// Evaluate the HG phase function for arbitrary g and cos_theta.
		/// \return p(cos_theta, g) = (1-g^2) / (4*PI * (1+g^2-2g*cos_theta)^1.5)
		static Scalar EvaluateWithG(
			const Scalar cosTheta,
			const Scalar g
			);

		/// Sample a scattered direction using the HG inverse CDF with arbitrary g.
		/// \return Sampled outgoing direction (normalized)
		static Vector3 SampleWithG(
			const Vector3& wi,
			ISampler& sampler,
			const Scalar g
			);
	};
}

#endif
