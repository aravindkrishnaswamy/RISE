//////////////////////////////////////////////////////////////////////
//
//  IMedium.h - Interface for participating media
//
//  A medium describes the volumetric scattering and absorption
//  properties of a region of space.  It replaces the deprecated
//  IAtmosphere interface and aligns with Blender/Cycles' volume
//  architecture (VolumeShaderCoefficients, volume stack, etc.) for
//  future plugin compatibility.
//
//  Media can be attached to objects (interior medium) or to the
//  scene (global/world medium).  The ray caster uses the IORStack
//  to determine which medium a ray is currently traveling through.
//
//  Two concrete implementations are planned:
//    HomogeneousMedium  - constant coefficients (Stage 5A)
//    HeterogeneousMedium - spatially varying via IVolume (Stage 5B)
//
//  CORE PROPERTIES:
//    sigma_a  - absorption coefficient [1/m]
//    sigma_s  - scattering coefficient [1/m]
//    sigma_t  - extinction = sigma_a + sigma_s [1/m]
//    phase    - scattering direction distribution (IPhaseFunction)
//
//  KEY OPERATIONS:
//    GetCoefficients()     - query medium properties at a point
//    SampleDistance()       - free-flight distance sampling
//    EvalTransmittance()   - Beer-Lambert transmittance along a ray
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IMEDIUM_
#define IMEDIUM_

#include "IReference.h"
#include "IPhaseFunction.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Ray.h"

namespace RISE
{
	class ISampler;

	/// Coefficients at a point in the medium.
	/// Aligned with Cycles' VolumeShaderCoefficients
	/// (intern/cycles/kernel/integrator/shade_volume.h).
	struct MediumCoefficients
	{
		RISEPel sigma_t;		///< Extinction coefficient (absorption + scattering)
		RISEPel sigma_s;		///< Scattering coefficient
		RISEPel emission;		///< Volumetric emission (zero for non-emissive media)
	};

	/// Spectral (single-wavelength) variant of MediumCoefficients
	struct MediumCoefficientsNM
	{
		Scalar sigma_t;			///< Extinction at this wavelength
		Scalar sigma_s;			///< Scattering at this wavelength
		Scalar emission;		///< Emission at this wavelength
	};

	/// \brief Interface for participating media (volumes)
	///
	/// Replaces the deprecated IAtmosphere.  Media provide absorption,
	/// scattering, emission, and phase function information needed by
	/// the ray caster to perform volumetric path tracing.
	class IMedium : public virtual IReference
	{
	protected:
		IMedium(){};
		virtual ~IMedium(){};

	public:

		/// Query medium coefficients at a world-space point.
		/// Homogeneous media ignore the point argument.
		virtual MediumCoefficients GetCoefficients(
			const Point3& pt						///< [in] World-space position
			) const = 0;

		/// Spectral variant: coefficients at a point for a single wavelength
		virtual MediumCoefficientsNM GetCoefficientsNM(
			const Point3& pt,						///< [in] World-space position
			const Scalar nm							///< [in] Wavelength in nanometers
			) const = 0;

		/// Get the phase function for scattering in this medium
		/// \return Pointer to the phase function (never NULL for valid media)
		virtual const IPhaseFunction* GetPhaseFunction() const = 0;

		/// Sample a free-flight distance along a ray from origin.
		/// If the sampled distance is less than maxDist, a scatter event
		/// occurs and scattered is set to true.  Otherwise the ray
		/// passes through without scattering and scattered is false.
		/// \return Sampled distance along the ray
		virtual Scalar SampleDistance(
			const Ray& ray,							///< [in] Ray to sample along
			const Scalar maxDist,					///< [in] Maximum distance (e.g. to next surface)
			ISampler& sampler,						///< [in] Random number source
			bool& scattered							///< [out] True if scatter event occurred
			) const = 0;

		/// Spectral variant of SampleDistance
		virtual Scalar SampleDistanceNM(
			const Ray& ray,							///< [in] Ray to sample along
			const Scalar maxDist,					///< [in] Maximum distance
			const Scalar nm,						///< [in] Wavelength in nanometers
			ISampler& sampler,						///< [in] Random number source
			bool& scattered							///< [out] True if scatter event occurred
			) const = 0;

		/// Evaluate transmittance along a ray segment [0, dist).
		/// For homogeneous media: T = exp(-sigma_t * dist).
		/// \return Per-channel transmittance in [0, 1]
		virtual RISEPel EvalTransmittance(
			const Ray& ray,							///< [in] Ray direction (origin used for heterogeneous)
			const Scalar dist						///< [in] Distance along the ray
			) const = 0;

		/// Spectral variant of EvalTransmittance
		virtual Scalar EvalTransmittanceNM(
			const Ray& ray,							///< [in] Ray direction
			const Scalar dist,						///< [in] Distance along the ray
			const Scalar nm							///< [in] Wavelength in nanometers
			) const = 0;

		/// Is this medium spatially uniform?
		/// Returns true for homogeneous media.  Analogous to Cycles'
		/// volume_is_homogeneous() check on SD_HETEROGENEOUS_VOLUME flag.
		virtual bool IsHomogeneous() const = 0;
	};
}

#endif
