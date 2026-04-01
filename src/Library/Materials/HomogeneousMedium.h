//////////////////////////////////////////////////////////////////////
//
//  HomogeneousMedium.h - Homogeneous participating medium
//
//  Implements a spatially uniform medium with constant absorption,
//  scattering, and emission coefficients.  Uses analytic exponential
//  sampling for free-flight distances and Beer-Lambert law for
//  transmittance evaluation.
//
//  SAMPLING ALGORITHM (exponential free-flight):
//    Given sigma_t (extinction), the distance to the next scatter
//    event is sampled as:
//      t = -ln(1 - xi) / sigma_t_max
//    where sigma_t_max = max(sigma_t.r, sigma_t.g, sigma_t.b).
//
//    If t < maxDist, a scatter event occurs.  The sampling PDF is:
//      p(t) = sigma_t_max * exp(-sigma_t_max * t)
//    and the transmittance along [0, t) is:
//      T(t) = exp(-sigma_t * t)   (per-channel Beer-Lambert)
//
//  SPECTRAL MODE:
//    In spectral (NM) mode, extinction at the given wavelength is
//    used directly — no channel selection needed.
//
//  Aligned with Blender/Cycles homogeneous volume integration in
//  volume_integrate_homogeneous()
//  (intern/cycles/kernel/integrator/shade_volume.h).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HOMOGENEOUS_MEDIUM_
#define HOMOGENEOUS_MEDIUM_

#include "../Interfaces/IMedium.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/Color/ColorMath.h"

namespace RISE
{
	/// \brief Homogeneous (spatially uniform) participating medium
	///
	/// Stores constant sigma_a, sigma_s, and optional emission.
	/// The phase function is shared via reference counting.
	class HomogeneousMedium :
		public virtual IMedium,
		public virtual Implementation::Reference
	{
	protected:
		RISEPel m_sigma_a;					///< Absorption coefficient [1/m]
		RISEPel m_sigma_s;					///< Scattering coefficient [1/m]
		RISEPel m_sigma_t;					///< Extinction = sigma_a + sigma_s
		RISEPel m_emission;					///< Volumetric emission (usually zero)
		Scalar m_sigma_t_max;				///< Max channel of sigma_t (for sampling)

		const IPhaseFunction* m_pPhase;		///< Phase function (ref-counted)

		virtual ~HomogeneousMedium();

	public:
		/// Construct a homogeneous medium with given coefficients.
		/// The phase function is addref'd and will be released on destruction.
		HomogeneousMedium(
			const RISEPel& sigma_a,			///< [in] Absorption coefficient
			const RISEPel& sigma_s,			///< [in] Scattering coefficient
			const IPhaseFunction& phase		///< [in] Phase function for scattering
			);

		/// Construct with emission
		HomogeneousMedium(
			const RISEPel& sigma_a,			///< [in] Absorption coefficient
			const RISEPel& sigma_s,			///< [in] Scattering coefficient
			const RISEPel& emission,		///< [in] Volumetric emission
			const IPhaseFunction& phase		///< [in] Phase function for scattering
			);

		MediumCoefficients GetCoefficients(
			const Point3& pt
			) const;

		MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt,
			const Scalar nm
			) const;

		const IPhaseFunction* GetPhaseFunction() const;

		Scalar SampleDistance(
			const Ray& ray,
			const Scalar maxDist,
			ISampler& sampler,
			bool& scattered
			) const;

		Scalar SampleDistanceNM(
			const Ray& ray,
			const Scalar maxDist,
			const Scalar nm,
			ISampler& sampler,
			bool& scattered
			) const;

		RISEPel EvalTransmittance(
			const Ray& ray,
			const Scalar dist
			) const;

		Scalar EvalTransmittanceNM(
			const Ray& ray,
			const Scalar dist,
			const Scalar nm
			) const;

		bool IsHomogeneous() const;
	};
}

#endif
