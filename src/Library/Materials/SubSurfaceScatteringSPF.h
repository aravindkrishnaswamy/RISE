//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringSPF.h - Defines the surface scattering
//  probability function for BSSRDF-based subsurface scattering.
//
//  With the BSSRDF approach, all subsurface light transport is
//  handled analytically by the diffusion profile (evaluated in the
//  integrator via probe ray sampling).  The SPF is responsible only
//  for surface interactions at the boundary:
//
//  From outside (front face):
//    - GGX-sampled reflection (non-delta when rough, delta when smooth)
//    - The refracted (entry) ray is NOT generated here — the BDPT
//      integrator samples entry points via BSSRDF importance sampling.
//      The SPF only emits a surface reflection ray.
//
//  From inside (back face):
//    - Should not occur with BSSRDF (no volumetric random walk).
//      If hit from inside (e.g. BDPT light subpath), emit a delta
//      Fresnel reflection back into the medium.
//
//  References:
//    - Christensen & Burley, "Approximate Reflectance Profiles for
//      Efficient Subsurface Scattering", SIGGRAPH 2015
//    - Walter et al., "Microfacet Models for Refraction through
//      Rough Surfaces", EGSR 2007
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_SPF_
#define SUBSURFACE_SCATTERING_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			virtual ~SubSurfaceScatteringSPF();

			const IPainter&		ior;		// Index of refraction
			const IPainter&		absorption;	// Absorption coefficient (kept for parameter storage)
			const IPainter&		scattering;	// Scattering coefficient (kept for parameter storage)
			const Scalar		g;			// HG asymmetry parameter (kept for parameter storage)
			const Scalar		roughness;	// Surface roughness for microfacet boundary [0, 1]
			const Scalar		alpha;		// GGX alpha = roughness^2
			const bool			bAbsorbBackFace;	// If true, back-face hits are absorbed (no scattering)

		public:
			SubSurfaceScatteringSPF(
				const IPainter& ior_,
				const IPainter& absorption_,
				const IPainter& scattering_,
				const Scalar g_,
				const Scalar roughness_,
				const bool bAbsorbBackFace_ = false
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,										///< [in] Sampler
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,										///< [in] Sampler
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Evaluates the PDF for the scattered direction.
			//! Returns GGX reflection PDF for front face hits, 0 otherwise.
			Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack& ior_stack ) const;

			//! Spectral version of Pdf evaluation
			Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar nm, const IORStack& ior_stack ) const;
		};
	}
}

#endif
