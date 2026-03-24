//////////////////////////////////////////////////////////////////////
//
//  DonnerJensenSkinDiffusionProfile.h - A BSSRDF diffusion profile
//  implementing the Donner et al. 2008 spectral skin model.
//
//  Uses a two-layer (epidermis + dermis) model driven by 6 intuitive
//  physiological parameters:
//    C_m     - Melanin fraction (volume fraction of melanosomes)
//    beta_m  - Melanin type blend (eumelanin vs pheomelanin)
//    C_he    - Hemoglobin fraction (epidermis)
//    C_bc    - Carotene fraction
//    C_hd    - Hemoglobin fraction (dermis)
//    rho_s   - Oiliness / surface roughness
//
//  Spectral absorption per Equations 11-12 of the paper.
//  Inter-layer melanin absorption per Equations 7-8.
//
//  References:
//    Donner, Weyrich, d'Eon, Ramamoorthi, Rusinkiewicz 2008 —
//      A Layered, Heterogeneous Reflectance Model for Acquiring
//      and Rendering Human Skin
//    Donner & Jensen 2005 — Light Diffusion in Multi-Layered
//      Translucent Materials
//    Jensen, Marschner, Levoy, Hanrahan 2001 — A Practical Model
//      for Subsurface Light Transport
//
//  ================================================================
//  Deviations from the paper
//  ================================================================
//
//  The paper was designed for GPU texture-space irradiance
//  convolution (Section 4.3).  This implementation adapts the
//  paper's spectral model for RISE's Monte Carlo BDPT renderer
//  using point-sampled BSSRDF importance sampling.  The following
//  deviations were necessary:
//
//  1. RENDERING METHOD: The paper convolves irradiance with
//     Sum-of-Gaussians profiles in UV texture space (Eq. 4, 9),
//     with pointwise inter-layer absorption (Eq. 7-8).  We instead
//     precompute a radial diffusion profile Rd(r), fit it to a sum
//     of exponentials, and importance-sample it in the BDPT
//     integrator.  This means we cannot support spatially-varying
//     heterogeneous inter-layer absorption (the paper's key
//     rendering contribution).  Uniform skin appearance works well;
//     effects like veins, freckles, and tattoos via parameter maps
//     would require the texture-space convolution pipeline.
//
//  2. PROFILE COMPUTATION: The paper uses Hankel-domain multipole
//     layer stacking (Donner & Jensen 2005) with inverse Hankel
//     transform.  We found that the numerical Hankel transform
//     produces severe J0 oscillation artifacts for the thin 2-layer
//     epidermis+dermis configuration, corrupting the profile tail.
//     Instead, we evaluate the profile directly in the spatial
//     domain: the epidermis slab uses a spatial-domain multipole
//     (same image sources as Donner 2005, evaluated at each radius),
//     and the dermis uses the Jensen 2001 semi-infinite dipole.
//     The two-layer composite approximates the adding-doubling
//     convolution by scaling the dermis profile with the epidermis
//     hemispherical transmittance (valid because the thin epidermis
//     transmittance profile is sharply peaked at r~0).
//
//  3. SCATTERING FORMULA: The paper's Eq. 13 reduced scattering
//     formula (14.74*lambda^(-0.22) + 2.2e11*lambda^(-4)) produces
//     values ~10x too low compared to measured skin data (~6 cm^-1
//     at 550nm vs expected ~60 cm^-1).  We use the Bashkatov 2005
//     power-law fit (73.7*(lambda/500)^(-2.33)) for the epidermis
//     and Rayleigh collagen scattering (Jacques 1996) for the
//     dermis, which match the measured range and are consistent
//     with the existing BioSpec skin model.
//
//  4. MELANIN ABSORPTION UNITS: The paper's sigma_a^em and
//     sigma_a^pm are absorption coefficients of melanin pigment in
//     cm^-1.  Our chromophore data (OMLC) is tabulated as
//     extinction in cm^-1/(mg/ml).  We multiply by reference
//     melanosome concentrations (80 mg/ml eumelanin, 12 mg/ml
//     pheomelanin) to obtain absorption per melanosome, then C_m
//     acts as the melanosome volume fraction.
//
//  5. INTER-LAYER ABSORPTION: The paper's thin absorbing layer
//     (Eq. 7-8, A(x,y) = exp(-P(x,y))) is a pointwise boundary
//     attenuation in the texture-space convolution.  We apply it
//     as a scalar boundary transmission factor A^2 (squared for
//     the round-trip through the boundary) that scales the dermis
//     contribution: T_factor = T_epi^2 * Ft_boundary * A^2.
//     This captures the homogeneous (spatially uniform) effect but
//     not the heterogeneous variation that requires texture-space
//     convolution.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DONNER_JENSEN_SKIN_DIFFUSION_PROFILE_H
#define DONNER_JENSEN_SKIN_DIFFUSION_PROFILE_H

#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IFunction1D.h"
#include "../Utilities/Reference.h"
#include "../Utilities/SumOfExponentialsFit.h"
#include "MultipoleDiffusion.h"

namespace RISE
{
	namespace Implementation
	{
		class DonnerJensenSkinDiffusionProfile :
			public virtual ISubSurfaceDiffusionProfile,
			public virtual Reference
		{
		protected:
			// Donner 2008 biophysical parameters (IPainter references)
			const IPainter&		pnt_melanin_fraction;		///< C_m: melanin volume fraction (0-0.5)
			const IPainter&		pnt_melanin_blend;			///< beta_m: eumelanin vs pheomelanin (0-1)
			const IPainter&		pnt_hemoglobin_epidermis;	///< C_he: hemoglobin fraction in epidermis (0-0.05)
			const IPainter&		pnt_carotene_fraction;		///< C_bc: carotene fraction (0-0.05)
			const IPainter&		pnt_hemoglobin_dermis;		///< C_hd: hemoglobin fraction in dermis (0-0.1)
			const IPainter&		pnt_epidermis_thickness;	///< Epidermis thickness (cm), default 0.025
			const IPainter&		pnt_ior_epidermis;			///< IOR of epidermis, default 1.4
			const IPainter&		pnt_ior_dermis;				///< IOR of dermis, default 1.38
			const IPainter&		pnt_blood_oxygenation;		///< gamma: blood oxygenation (0-1), default 0.7

			// Chromophore extinction lookup tables
			IFunction1D*		pEumelaninExt;
			IFunction1D*		pPheomelaninExt;
			IFunction1D*		pOxyHemoglobinExt;
			IFunction1D*		pDeoxyHemoglobinExt;
			IFunction1D*		pBetaCaroteneExt;

			Scalar				hb_concentration;			///< Hemoglobin concentration in whole blood (g/L)

			//=============================================================
			// Precomputed multipole diffusion profile data
			//=============================================================

			static const int K_TERMS = 6;
			static const int NUM_RGB = 3;
			static const int NUM_SPECTRAL = 31;			///< 400-700 nm at 10 nm spacing

			ExponentialTerm		m_rgb_terms[NUM_RGB][K_TERMS];
			Scalar				m_rgb_total_weight[NUM_RGB];
			Scalar				m_rgb_cdf[NUM_RGB][K_TERMS];

			ExponentialTerm		m_spectral_terms[NUM_SPECTRAL][K_TERMS];
			Scalar				m_spectral_total_weight[NUM_SPECTRAL];

			static const Scalar	ms_rgb_wavelengths[NUM_RGB];
			static const Scalar	ms_spectral_wavelengths[NUM_SPECTRAL];

			Scalar				m_min_rate;

			virtual ~DonnerJensenSkinDiffusionProfile();

			//=============================================================
			// Absorption / scattering helpers
			//=============================================================

			/// Baseline skin absorption (Eq. 11/12 remainder term).
			/// From Jacques 1998: sigma_a_baseline = 0.244 + 85.3 * exp(-(nm-154)/66.2)
			static Scalar ComputeSkinBaselineAbsorption( const Scalar nm );

			/// Reduced scattering coefficient per Eq. 13.
			/// sigma_sp(lambda) = 14.74 * lambda^(-0.22) + 2.2e11 * lambda^(-4)
			/// where lambda is in nm.
			static Scalar ComputeEpidermisScattering( const Scalar nm );

			/// Schlick Fresnel approximation.
			static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta );

			/// Compute per-layer optical properties at a given wavelength.
			/// Fills 2 LayerParams: [0]=epidermis, [1]=dermis.
			void ComputePerLayerCoefficients(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				LayerParams layers_out[2]
				) const;

			/// Precompute multipole profiles for all wavelengths.
			void PrecomputeProfiles();

			/// Run the multipole pipeline for a single wavelength.
			void PrecomputeProfileAtWavelength(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				ExponentialTerm terms_out[K_TERMS],
				Scalar& total_weight_out,
				Scalar cdf_out[K_TERMS]
				);

			/// Evaluate the K-term sum-of-exponentials at radius r.
			static Scalar EvaluateSumOfExp(
				const ExponentialTerm terms[K_TERMS],
				const Scalar r
				);

		public:
			DonnerJensenSkinDiffusionProfile(
				const IPainter& melanin_fraction_,
				const IPainter& melanin_blend_,
				const IPainter& hemoglobin_epidermis_,
				const IPainter& carotene_fraction_,
				const IPainter& hemoglobin_dermis_,
				const IPainter& epidermis_thickness_,
				const IPainter& ior_epidermis_,
				const IPainter& ior_dermis_,
				const IPainter& blood_oxygenation_
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

			Scalar GetMaximumDistanceForError(
				const Scalar error
				) const;

			RISEPel ComputeTotalExtinction(
				const Scalar distance
				) const;
		};
	}
}

#endif
