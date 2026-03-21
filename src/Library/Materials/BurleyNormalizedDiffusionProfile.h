//////////////////////////////////////////////////////////////////////
//
//  BurleyNormalizedDiffusionProfile.h - Burley's normalized
//  diffusion profile for subsurface scattering.
//
//  Implements the diffusion model from Christensen & Burley,
//  "Approximate Reflectance Profiles for Efficient Subsurface
//  Scattering" (SIGGRAPH 2015 Talks).
//
//  The model approximates the full volumetric BSSRDF with a sum
//  of two exponentials that accurately capture both single and
//  multiple scattering regimes:
//
//    Rd(r) = A * s / (8 * pi * d) * (exp(-s*r/d) + exp(-s*r/(3*d))) / r
//
//  where:
//    A = surface albedo (scattering / extinction after similarity)
//    d = mean free path = 1 / (sigma_s' + sigma_a)
//    s = empirical scaling factor: 1.9 - A + 3.5*(A - 0.8)^2
//
//  The profile can be importance-sampled analytically via a 50/50
//  mixture of two exponential distributions:
//    With probability 0.5: r = -d * ln(1-u) / s
//    With probability 0.5: r = -3*d * ln(1-u) / s
//
//  Parameters (sigma_a, sigma_s, g) are internally converted using
//  the similarity relation:
//    sigma_s' = sigma_s * (1 - g)
//    albedo   = sigma_s' / (sigma_s' + sigma_a)
//    mfp      = 1 / (sigma_s' + sigma_a)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BURLEY_NORMALIZED_DIFFUSION_PROFILE_H
#define BURLEY_NORMALIZED_DIFFUSION_PROFILE_H

#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class BurleyNormalizedDiffusionProfile :
			public virtual ISubSurfaceDiffusionProfile,
			public virtual Reference
		{
		protected:
			const IPainter&		ior;			///< Index of refraction at the boundary
			const IPainter&		absorption;		///< Absorption coefficient sigma_a
			const IPainter&		scattering;		///< Scattering coefficient sigma_s
			const Scalar		g;				///< HG asymmetry parameter

			virtual ~BurleyNormalizedDiffusionProfile();

			/// Computes the Burley scaling factor s from albedo A.
			/// This empirical fit controls the width of the profile:
			///   s = 1.9 - A + 3.5 * (A - 0.8)^2
			/// Higher albedo yields a wider profile (more scattering).
			static Scalar ComputeScalingFactor( const Scalar A );

			/// Schlick Fresnel reflectance from cosine and IOR
			static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta );

		public:
			BurleyNormalizedDiffusionProfile(
				const IPainter& ior_,
				const IPainter& absorption_,
				const IPainter& scattering_,
				const Scalar g_
				);

			//
			// ISubSurfaceDiffusionProfile interface
			//

			RISEPel EvaluateProfile(
				const Scalar r,
				const RayIntersectionGeometric& ri
				) const;

			Scalar EvaluateProfileNM(
				const Scalar r,
				const RayIntersectionGeometric& ri,
				const Scalar nm
				) const;

			Scalar SampleRadius(
				const Scalar u,
				const int channel,
				const RayIntersectionGeometric& ri
				) const;

			Scalar PdfRadius(
				const Scalar r,
				const int channel,
				const RayIntersectionGeometric& ri
				) const;

			Scalar FresnelTransmission(
				const Scalar cosTheta,
				const RayIntersectionGeometric& ri
				) const;

			Scalar GetIOR(
				const RayIntersectionGeometric& ri
				) const;

			//
			// ISubSurfaceExtinctionFunction interface
			//

			/// Returns the maximum distance for the given error tolerance.
			/// Beyond this distance, Rd(r) < error, so contributions can
			/// be safely ignored.  Uses the slower-decaying exponential
			/// (the exp(-s*r/(3*d)) term) to determine the cutoff.
			Scalar GetMaximumDistanceForError(
				const Scalar error
				) const;

			/// Evaluates the profile as a "total extinction" for
			/// compatibility with the existing ISubSurfaceExtinctionFunction
			/// interface used by the dipole ShaderOp.  Returns Rd(r) for
			/// each channel.
			RISEPel ComputeTotalExtinction(
				const Scalar distance
				) const;
		};
	}
}

#endif
