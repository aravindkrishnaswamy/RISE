//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringSPF.h - Defines a volumetric random walk
//  subsurface scattering SPF.
//
//  This SPF models light transport through translucent media using
//  a surface-based random walk approximation.  At each surface
//  boundary the ray undergoes Fresnel reflection/refraction.  When
//  a ray traveling inside the medium hits the back surface, it may:
//    1. Fresnel-reflect back into the medium  (specular, delta)
//    2. Scatter back into the medium via the Henyey-Greenstein
//       phase function  (non-delta, enables BDPT connections)
//    3. Exit the medium through Fresnel refraction  (delta)
//
//  Beer-Lambert absorption is applied based on the distance
//  traveled through the medium.  The scattering albedo
//  (sigma_s / sigma_t) controls the balance between scattering
//  and absorption.
//
//  References:
//    - Jensen et al., "A Practical Model for Subsurface Light
//      Transport", SIGGRAPH 2001
//    - Henyey & Greenstein, "Diffuse radiation in the galaxy",
//      Astrophysical Journal 93, 1941
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
			const IPainter&		absorption;	// Absorption coefficient (per unit distance)
			const IPainter&		scattering;	// Scattering coefficient (per unit distance)
			const Scalar		g;			// Henyey-Greenstein asymmetry parameter [-1, 1]
			const Scalar		roughness;	// Surface roughness for microfacet boundary [0, 1]
			const Scalar		alpha;		// GGX alpha = roughness^2

		public:
			SubSurfaceScatteringSPF(
				const IPainter& ior_,
				const IPainter& absorption_,
				const IPainter& scattering_,
				const Scalar g_,
				const Scalar roughness_
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,						///< [in] Random number generator
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,						///< [in] Random number generator
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Evaluates the PDF for the HG scatter component.
			//! Returns non-zero only for the volumetric scatter direction (non-delta).
			Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack* const ior_stack ) const;

			//! Spectral version of Pdf evaluation
			Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar nm, const IORStack* const ior_stack ) const;
		};
	}
}

#endif
