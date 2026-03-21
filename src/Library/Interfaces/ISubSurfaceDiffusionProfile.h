//////////////////////////////////////////////////////////////////////
//
//  ISubSurfaceDiffusionProfile.h - Interface for a subsurface
//    diffusion profile that supports both evaluation and importance
//    sampling.  Extends ISubSurfaceExtinctionFunction with methods
//    needed for BSSRDF importance sampling in path tracers.
//
//  The profile describes how light diffuses through a translucent
//  medium as a function of surface distance r between exit and
//  entry points.  Implementations provide Rd(r) evaluation, CDF
//  sampling, and PDF computation for efficient Monte Carlo
//  integration.
//
//  Current implementation:
//    BurleyNormalizedDiffusionProfile (Christensen & Burley 2015)
//  Future:
//    DipoleDiffusionProfile — wrapping the existing
//    DiffusionApproximationExtinction for use with BDPT
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISUBSURFACE_DIFFUSION_PROFILE_H
#define ISUBSURFACE_DIFFUSION_PROFILE_H

#include "ISubSurfaceExtinctionFunction.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	/// Interface for subsurface diffusion profiles with importance
	/// sampling support.  The BSSRDF factorization is:
	///   S(wo, xo, wi, xi) = C * Ft(wo) * Rd(||xo - xi||) * Ft(wi)
	/// where Rd(r) is the diffuse reflectance profile, and Ft is the
	/// Fresnel transmission factor at each boundary crossing.
	///
	/// Implementations must provide:
	///   - Rd(r) evaluation (per channel and spectral)
	///   - Importance sampling of the radius r from Rd's CDF
	///   - PDF evaluation for the sampled radius
	///   - Fresnel transmission at the surface boundary
	class ISubSurfaceDiffusionProfile :
		public virtual ISubSurfaceExtinctionFunction
	{
	public:
		/// Evaluates the diffuse reflectance profile Rd(r) for all
		/// color channels.  Returns the per-channel reflectance at
		/// surface distance r (in world units) from the illumination
		/// point.
		/// \param r Surface distance between entry and exit points
		/// \param ri Intersection data for texture coordinate lookup
		/// \return Per-channel Rd(r) value
		virtual RISEPel EvaluateProfile(
			const Scalar r,
			const RayIntersectionGeometric& ri
			) const = 0;

		/// Spectral version of EvaluateProfile for a single wavelength.
		/// \param r Surface distance between entry and exit points
		/// \param ri Intersection data for texture coordinate lookup
		/// \param nm Wavelength in nanometers
		/// \return Scalar Rd(r) value at the given wavelength
		virtual Scalar EvaluateProfileNM(
			const Scalar r,
			const RayIntersectionGeometric& ri,
			const Scalar nm
			) const = 0;

		/// Importance-samples a radius from the profile's CDF for a
		/// specific color channel.  The sampled radius is distributed
		/// according to r * Rd(r) (the 2D marginal).
		/// \param u Uniform random number in [0, 1)
		/// \param channel Color channel index (0=R, 1=G, 2=B)
		/// \param ri Intersection data for texture coordinate lookup
		/// \return Sampled radius in world units
		virtual Scalar SampleRadius(
			const Scalar u,
			const int channel,
			const RayIntersectionGeometric& ri
			) const = 0;

		/// Evaluates the sampling PDF for a given radius and channel.
		/// This is the probability density of SampleRadius returning
		/// the value r, expressed in world units (1/meter).
		/// \param r Surface distance to evaluate PDF at
		/// \param channel Color channel index (0=R, 1=G, 2=B)
		/// \param ri Intersection data for texture coordinate lookup
		/// \return PDF value in 1/world_units
		virtual Scalar PdfRadius(
			const Scalar r,
			const int channel,
			const RayIntersectionGeometric& ri
			) const = 0;

		/// Computes the Fresnel transmission factor (1 - F) at the
		/// surface boundary for a given cosine of the incident angle.
		/// \param cosTheta Cosine of angle between direction and normal
		/// \param ri Intersection data (for IOR texture lookup)
		/// \return Fresnel transmission factor in [0, 1]
		virtual Scalar FresnelTransmission(
			const Scalar cosTheta,
			const RayIntersectionGeometric& ri
			) const = 0;

		/// Returns the index of refraction at the given surface point.
		/// \param ri Intersection data (for IOR texture lookup)
		/// \return Index of refraction (eta_t / eta_i where eta_i = 1 for air)
		virtual Scalar GetIOR(
			const RayIntersectionGeometric& ri
			) const = 0;
	};
}

#endif
