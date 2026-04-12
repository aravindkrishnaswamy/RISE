//////////////////////////////////////////////////////////////////////
//
//  IPhaseFunction.h - Interface for phase functions used in
//    participating media scattering
//
//  Phase functions describe the angular distribution of scattered
//  light at a point in a participating medium.  They are the volume
//  analogue of a BSDF: given an incoming direction wi, they return
//  the probability density (per steradian) of scattering into the
//  outgoing direction wo.
//
//  All phase functions are normalized:
//    integral over sphere of p(wi, wo) dwo = 1
//
//  For symmetric phase functions (most physical media) the function
//  depends only on the angle between wi and wo:
//    p(wi, wo) = p(cos(theta))   where cos(theta) = dot(wi, wo)
//
//  Design note: this interface is kept thin and stateless so that
//  the same phase function instance can be shared by multiple media
//  and evaluated concurrently from different threads.  The asymmetry
//  parameter g (for Henyey-Greenstein) or other parameters are
//  baked into the concrete instance at construction time.
//
//  Aligned with Blender/Cycles ShaderVolumePhases closure pattern
//  (intern/cycles/kernel/closure/volume_util.h).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPHASEFUNCTION_
#define IPHASEFUNCTION_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	class ISampler;

	/// \brief Interface for scattering phase functions in participating media
	///
	/// A phase function p(wi, wo) gives the probability density per steradian
	/// of light scattering from direction wi into direction wo at a point
	/// inside a volume.  Concrete implementations include isotropic (uniform),
	/// Henyey-Greenstein (single-lobe anisotropic), and Rayleigh.
	class IPhaseFunction : public virtual IReference
	{
	protected:
		IPhaseFunction(){};
		virtual ~IPhaseFunction(){};

	public:

		/// Evaluate the phase function for the given pair of directions
		/// \return Phase function value p(wi, wo) [1/sr]
		virtual Scalar Evaluate(
			const Vector3& wi,							///< [in] Incoming direction (toward the scatter point)
			const Vector3& wo							///< [in] Outgoing direction (away from the scatter point)
			) const = 0;

		/// Importance-sample a scattered direction given an incoming direction
		/// \return Sampled outgoing direction (normalized)
		virtual Vector3 Sample(
			const Vector3& wi,							///< [in] Incoming direction
			ISampler& sampler							///< [in] Source of random numbers
			) const = 0;

		/// Return the PDF for sampling direction wo given incoming direction wi.
		/// For normalized phase functions this equals Evaluate().
		/// \return Sampling PDF [1/sr]
		virtual Scalar Pdf(
			const Vector3& wi,							///< [in] Incoming direction
			const Vector3& wo							///< [in] Outgoing direction
			) const = 0;

		/// Return the mean cosine (asymmetry parameter g) of the phase function.
		/// Used by volume guiding to apply the HG product to the learned
		/// distribution.  Returns 0 (isotropic) by default.
		virtual Scalar GetMeanCosine() const { return 0; }
	};
}

#endif
