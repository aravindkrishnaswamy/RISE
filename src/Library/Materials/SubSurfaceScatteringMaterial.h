//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringMaterial.h - Defines a material that
//  simulates subsurface scattering via a BSSRDF (Bidirectional
//  Surface Scattering Reflectance Distribution Function).
//
//  The subsurface light transport is modeled analytically using
//  a diffusion profile (Burley's normalized diffusion) that
//  evaluates Rd(r) as a function of surface distance.  The BDPT
//  integrator importance-samples entry points using probe rays,
//  weighted by the profile.
//
//  The SPF handles surface reflection only (no volumetric random
//  walk).  The BSDF evaluates surface reflection for BDPT
//  connection strategies.
//
//  Parameters:
//    ior     - Index of refraction at the surface boundary
//    sigma_a - Absorption coefficient (per unit distance, per channel)
//    sigma_s - Scattering coefficient (per unit distance, per channel)
//    g       - Henyey-Greenstein asymmetry parameter (-1 to 1)
//    roughness - Surface roughness for microfacet boundary [0, 1]
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_MATERIAL_
#define SUBSURFACE_SCATTERING_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"
#include "BurleyNormalizedDiffusionProfile.h"

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*				pBSDF;
			SubSurfaceScatteringSPF*				pSPF;
			BurleyNormalizedDiffusionProfile*		pProfile;

			virtual ~SubSurfaceScatteringMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
				safe_release( pProfile );
			}

		public:
			SubSurfaceScatteringMaterial(
				const IPainter& ior,
				const IPainter& absorption,
				const IPainter& scattering,
				const Scalar g,
				const Scalar roughness
				)
			{
				pBSDF = new SubSurfaceScatteringBSDF( ior, absorption, scattering, g, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior, absorption, scattering, g, roughness, true );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				pProfile = new BurleyNormalizedDiffusionProfile( ior, absorption, scattering, g );
				GlobalLog()->PrintNew( pProfile, __FILE__, __LINE__, "DiffusionProfile" );
			}

			/// \return The BSDF for this material.  NULL If there is no BSDF
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			// SSS materials scatter light diffusely through the volume,
			// so straight-line camera connections through them are unphysical.
			inline bool CouldLightPassThrough() const { return false; };

			/// BSSRDF-based SSS does not use volumetric random walk.
			/// All subsurface transport is handled analytically by the
			/// diffusion profile — no interior vertices are created.
			inline bool IsVolumetric() const { return false; };

			/// \return The diffusion profile for BSSRDF importance sampling.
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return pProfile; };
		};
	}
}

#endif
