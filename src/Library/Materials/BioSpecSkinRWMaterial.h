//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinRWMaterial.h - A random-walk SSS skin material driven
//  by BioSpec's biophysical parameters.
//
//  Replaces BioSpecSkinBSSRDFMaterial's disk-projection sampling
//  with a volumetric random walk (Chiang & Burley, SIGGRAPH 2016).
//  The random walk naturally handles thin geometry (ears, nose,
//  fingers) where disk-projection probe rays miss the surface.
//
//  SPECTRAL-ONLY RENDERING
//
//  BioSpec is a fundamentally spectral model.  The scattering
//  coefficients vary 3:1 across the visible spectrum (σ_t ≈ 60
//  cm⁻¹ at red vs 190 cm⁻¹ at blue), which causes exponential
//  per-channel weight divergence in the RGB random walk.  This
//  material therefore only works in spectral rendering mode
//  (NM path).  GetRandomWalkSSSParams() returns NULL, disabling
//  the RGB walk path entirely.  GetRandomWalkSSSParamsNM()
//  computes per-wavelength coefficients on the fly from stored
//  chromophore extinction lookup tables.
//
//  The BioSpec 4-layer skin model (stratum corneum, epidermis,
//  papillary dermis, reticular dermis) provides per-layer optical
//  coefficients.  These are combined into effective homogeneous
//  walk parameters using thickness-weighted averaging under the
//  similarity principle:
//
//    sigma_s_walk = sigma_s' (reduced scattering, from BioSpec)
//    g_walk       = 0 (isotropic, since sigma_s' already accounts
//                      for the per-layer anisotropy)
//    sigma_a_walk = sigma_a (absorption, from BioSpec)
//
//  Per-layer anisotropy factors from literature:
//    Stratum corneum:   g = 0.917  (Krishnaswamy & Baranoski 2004)
//    Epidermis:         g = 0.781  (Krishnaswamy & Baranoski 2004)
//    Papillary dermis:  g = 0.0    (Rayleigh scattering)
//    Reticular dermis:  g = 0.0    (Rayleigh scattering)
//
//  These g values are implicitly encoded in the reduced scattering
//  coefficients that BioSpec computes: sigma_s' = sigma_s * (1-g).
//  Under the similarity principle, a random walk with (sigma_s', 0)
//  produces the same diffuse transport as (sigma_s, g).
//
//  Coefficients are computed in cm⁻¹ (BioSpec's native units) and
//  converted to scene units (meters) by multiplying by 100.  The
//  geometry must be at the correct physical scale for the walk to
//  produce correct optical thickness.
//
//  The surface boundary uses a smooth dielectric Fresnel model
//  with the stratum corneum IOR (matching the BioSpec flat
//  boundary model).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_SKIN_RW_MATERIAL_
#define BIOSPEC_SKIN_RW_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IFunction1D.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class BioSpecSkinRWMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*		pBSDF;
			SubSurfaceScatteringSPF*		pSPF;
			const IPainter&					iorPainter;
			const Scalar					surfaceRoughness;
			const unsigned int				m_maxBounces;

			//=============================================================
			// Biophysical parameters (evaluated at construction time)
			//=============================================================

			Scalar		m_ior_SC;
			Scalar		m_ior_papillary;
			Scalar		m_ior_reticular;

			Scalar		m_thickness_SC;
			Scalar		m_thickness_epidermis;
			Scalar		m_thickness_papillary;
			Scalar		m_thickness_reticular;
			Scalar		m_total_thickness;

			Scalar		m_conc_eumelanin;
			Scalar		m_conc_pheomelanin;
			Scalar		m_melanosomes;

			Scalar		m_hb_ratio;
			Scalar		m_blood_papillary;
			Scalar		m_blood_reticular;
			Scalar		m_hb_concentration;

			Scalar		m_bilirubin_conc;
			Scalar		m_carotene_SC;
			Scalar		m_carotene_epidermis;
			Scalar		m_carotene_dermis;

			//=============================================================
			// Chromophore extinction lookup tables
			//=============================================================

			IFunction1D*	pEumelaninExt;
			IFunction1D*	pPheomelaninExt;
			IFunction1D*	pOxyHemoglobinExt;
			IFunction1D*	pDeoxyHemoglobinExt;
			IFunction1D*	pBilirubinExt;
			IFunction1D*	pBetaCaroteneExt;

			//=============================================================
			// Internal helpers
			//=============================================================

			/// Compute effective walk coefficients at a single wavelength
			/// in cm⁻¹.  sigma_a_eff and sigma_sp_eff are the homogeneous
			/// walk parameters.  melaninFilterTransmittance is the
			/// double-pass melanin/SC boundary filter that must be
			/// applied as a post-walk weight multiplier.
			void ComputeEffectiveCoefficients(
				const Scalar nm,
				Scalar& sigma_a_eff,
				Scalar& sigma_sp_eff,
				Scalar& melaninFilterTransmittance
				) const;

			virtual ~BioSpecSkinRWMaterial();

		public:
			BioSpecSkinRWMaterial(
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
				const Scalar roughness,
				const unsigned int maxBounces
				);

			/// \return The BSDF for this material.
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.
			inline IEmitter* GetEmitter() const {	return 0; };

			// SSS materials scatter light diffusely through the volume.
			inline bool CouldLightPassThrough() const { return false; };

			/// Random-walk SSS — not open-medium volumetric rendering.
			inline bool IsVolumetric() const { return false; };

			/// No diffusion profile — random walk replaces disk projection.
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return 0; };

			/// RGB walk disabled — BioSpec is spectral-only.
			/// Returns NULL so the RGB integrator path skips this material.
			inline const RandomWalkSSSParams* GetRandomWalkSSSParams() const { return 0; };

			/// Compute per-wavelength random-walk SSS parameters from
			/// stored chromophore extinction lookup tables.
			bool GetRandomWalkSSSParamsNM(
				const Scalar nm,
				RandomWalkSSSParams& params_out
				) const;

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
