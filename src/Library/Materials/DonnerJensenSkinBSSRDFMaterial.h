//////////////////////////////////////////////////////////////////////
//
//  DonnerJensenSkinBSSRDFMaterial.h - A BSSRDF-based skin material
//  implementing the Donner et al. 2008 spectral skin model.
//
//  Combines a surface-reflection BSDF and SPF (for specular
//  reflection at the skin boundary) with a Donner-Jensen
//  diffusion profile (for subsurface scattering).  This enables
//  BDPT compatibility: the integrator uses the diffusion profile
//  for BSSRDF importance sampling while the BSDF/SPF handle
//  surface reflection for connection strategies.
//
//  References:
//    Donner, Weyrich, d'Eon, Ramamoorthi, Rusinkiewicz 2008 —
//      A Layered, Heterogeneous Reflectance Model for Acquiring
//      and Rendering Human Skin
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DONNER_JENSEN_SKIN_BSSRDF_MATERIAL_
#define DONNER_JENSEN_SKIN_BSSRDF_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"
#include "DonnerJensenSkinDiffusionProfile.h"

namespace RISE
{
	namespace Implementation
	{
		class DonnerJensenSkinBSSRDFMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*				pBSDF;
			SubSurfaceScatteringSPF*				pSPF;
			DonnerJensenSkinDiffusionProfile*		pProfile;

			virtual ~DonnerJensenSkinBSSRDFMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
				safe_release( pProfile );
			}

		public:
			DonnerJensenSkinBSSRDFMaterial(
				const IPainter& melanin_fraction_,
				const IPainter& melanin_blend_,
				const IPainter& hemoglobin_epidermis_,
				const IPainter& carotene_fraction_,
				const IPainter& hemoglobin_dermis_,
				const IPainter& epidermis_thickness_,
				const IPainter& ior_epidermis_,
				const IPainter& ior_dermis_,
				const IPainter& blood_oxygenation_,
				const Scalar roughness
				)
			{
				// Surface BSDF/SPF use epidermis IOR for Fresnel reflection.
				// The absorption and scattering painters are not used by the
				// surface BSDF/SPF for BSSRDF materials, so we pass the IOR
				// painter as a dummy.
				pBSDF = new SubSurfaceScatteringBSDF( ior_epidermis_, ior_epidermis_, ior_epidermis_, 0.0, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior_epidermis_, ior_epidermis_, ior_epidermis_, 0.0, roughness, true );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				pProfile = new DonnerJensenSkinDiffusionProfile(
					melanin_fraction_,
					melanin_blend_,
					hemoglobin_epidermis_,
					carotene_fraction_,
					hemoglobin_dermis_,
					epidermis_thickness_,
					ior_epidermis_,
					ior_dermis_,
					blood_oxygenation_
					);
				GlobalLog()->PrintNew( pProfile, __FILE__, __LINE__, "DiffusionProfile" );
			}

			/// \return The BSDF for this material.
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			// SSS materials scatter light diffusely through the volume,
			// so straight-line camera connections through them are unphysical.
			inline bool CouldLightPassThrough() const { return false; };

			/// BSSRDF-based SSS does not use volumetric random walk.
			inline bool IsVolumetric() const { return false; };

			/// \return The diffusion profile for BSSRDF importance sampling.
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return pProfile; };
		};
	}
}

#endif
