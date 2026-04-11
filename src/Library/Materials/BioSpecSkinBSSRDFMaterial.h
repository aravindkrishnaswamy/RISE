//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinBSSRDFMaterial.h - A BSSRDF-based skin material
//  driven by BioSpec's biophysical parameters.
//
//  Combines a surface-reflection BSDF and SPF (for specular
//  reflection at the skin boundary) with a BioSpec-parameterized
//  diffusion profile (for subsurface scattering).  This enables
//  BDPT compatibility: the integrator uses the diffusion profile
//  for BSSRDF importance sampling while the BSDF/SPF handle
//  surface reflection for connection strategies.
//
//  The surface boundary uses a smooth dielectric Fresnel model
//  with the stratum corneum IOR (roughness = 0 gives perfect
//  specular reflection, matching BioSpec's flat boundary model).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_SKIN_BSSRDF_MATERIAL_
#define BIOSPEC_SKIN_BSSRDF_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"
#include "BioSpecDiffusionProfile.h"

namespace RISE
{
	namespace Implementation
	{
		class BioSpecSkinBSSRDFMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*		pBSDF;
			SubSurfaceScatteringSPF*		pSPF;
			BioSpecDiffusionProfile*		pProfile;
			const IPainter&					iorPainter;
			const Scalar					surfaceRoughness;

			virtual ~BioSpecSkinBSSRDFMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
				safe_release( pProfile );
				iorPainter.release();
			}

		public:
			BioSpecSkinBSSRDFMaterial(
				const IPainter& thickness_SC_,
				const IPainter& thickness_epidermis_,
				const IPainter& thickness_papillary_dermis_,
				const IPainter& thickness_reticular_dermis_,
				const IPainter& ior_SC_,
				const IPainter& ior_epidermis_,
				const IPainter& ior_papillary_dermis_,
				const IPainter& ior_reticular_dermis_,
				const IPainter& concentration_eumelanin_,
				const IPainter& concentration_pheomelanin_,
				const IPainter& melanosomes_in_epidermis_,
				const IPainter& hb_ratio_,
				const IPainter& whole_blood_in_papillary_dermis_,
				const IPainter& whole_blood_in_reticular_dermis_,
				const IPainter& bilirubin_concentration_,
				const IPainter& betacarotene_concentration_SC_,
				const IPainter& betacarotene_concentration_epidermis_,
				const IPainter& betacarotene_concentration_dermis_,
				const IPainter& folds_aspect_ratio_,
				const bool bSubdermalLayer,
				const Scalar roughness
				) :
			iorPainter( ior_SC_ ),
			surfaceRoughness( roughness )
			{
				iorPainter.addref();
				// The BSDF and SPF need IOR, absorption, and scattering painters.
				// For the surface boundary, we use the SC IOR.  The absorption and
				// scattering painters are not used by the surface BSDF/SPF for
				// BSSRDF materials (all subsurface transport is in the profile),
				// but we pass the SC IOR as a dummy for both since the interface
				// requires them.  The g parameter is 0 (isotropic, irrelevant for
				// surface-only evaluation).
				pBSDF = new SubSurfaceScatteringBSDF( ior_SC_, ior_SC_, ior_SC_, 0.0, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior_SC_, ior_SC_, ior_SC_, 0.0, roughness, true );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				pProfile = new BioSpecDiffusionProfile(
					thickness_SC_,
					thickness_epidermis_,
					thickness_papillary_dermis_,
					thickness_reticular_dermis_,
					ior_SC_,
					ior_epidermis_,
					ior_papillary_dermis_,
					ior_reticular_dermis_,
					concentration_eumelanin_,
					concentration_pheomelanin_,
					melanosomes_in_epidermis_,
					hb_ratio_,
					whole_blood_in_papillary_dermis_,
					whole_blood_in_reticular_dermis_,
					bilirubin_concentration_,
					betacarotene_concentration_SC_,
					betacarotene_concentration_epidermis_,
					betacarotene_concentration_dermis_
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

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = iorPainter.GetColor( ri )[0];
				info.valid = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = iorPainter.GetColorNM( ri, nm );
				info.valid = true;
				return info;
			}
		};
	}
}

#endif
