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
//  Reduced scattering per Equation 13.
//  Two-layer multipole diffusion (Donner & Jensen 2005) with
//  Sum-of-Gaussians fitting in the Hankel domain (Section 4.3).
//
//  References:
//    Donner, Weyrich, d'Eon, Ramamoorthi, Rusinkiewicz 2008 —
//      A Layered, Heterogeneous Reflectance Model for Acquiring
//      and Rendering Human Skin
//    Donner & Jensen 2005 — Light Diffusion in Multi-Layered
//      Translucent Materials
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
//     fit the composite Hankel-domain profile as a Sum-of-Gaussians
//     (same as the paper), then evaluate and importance-sample the
//     resulting spatial-domain Gaussians in the BDPT integrator.
//     This means we cannot support spatially-varying heterogeneous
//     inter-layer absorption (the paper's key rendering contribution).
//     Uniform skin appearance works well; effects like veins,
//     freckles, and tattoos via parameter maps would require the
//     texture-space convolution pipeline.
//
//  2. PROFILE FITTING: The paper fits Sum-of-Gaussians to the
//     Hankel-domain composite profile R_tilde(s), which we follow
//     faithfully.  The paper then uses the Gaussians for separable
//     2D texture-space blurs.  We instead convert each Gaussian to
//     its spatial-domain form Rd_k(r) = (c_k/2piSigma_k^2) *
//     exp(-r^2/2Sigma_k^2) and importance-sample via Rayleigh
//     distributions.  This avoids the inverse Hankel transform
//     entirely (which produces severe J0 ringing for the thin
//     2-layer configuration).
//
//  3. MELANIN ABSORPTION UNITS: The paper's sigma_a^em and
//     sigma_a^pm are absorption coefficients of melanin pigment in
//     cm^-1 at the melanosome level.  Our chromophore data (OMLC)
//     is tabulated as extinction in cm^-1/(mg/ml).  We convert to
//     melanosome-level absorption using a single reference
//     concentration C_MEL = 132.5 mg/ml for both eu and pheo,
//     calibrated so that a 50/50 blend at 550nm matches the
//     Jacques 1998 melanosome power law (6.6e11 * lambda^-3.33).
//     C_m then acts as the melanosome volume fraction.
//
//  4. SCATTERING FORMULA: The paper's Eq. 13 prints:
//       sigma_sp = 14.74*lambda^(-0.22) + 2.2e11*lambda^(-4)
//     which gives only ~6 cm^-1 at 550nm.  The Jacques 1998
//     reference that the paper cites gives ~37 cm^-1.  The
//     Rayleigh coefficient appears to be 10x too small (2.2e11
//     vs 2e12 from OMLC) and the Mie exponent differs (-0.22
//     vs -1.5).  At moderate-to-high melanin, the low scattering
//     causes albedo < 10%, where the diffusion approximation
//     breaks down and produces spectral artifacts (blue tinting).
//     We use the Jacques/OMLC formula instead:
//       sigma_sp = 2e12*lambda^(-4) + 2e5*lambda^(-1.5)
//     which gives ~37 cm^-1 at 550nm — consistent with measured
//     skin optical properties and valid for the diffusion model
//     across the full melanin range.
//
//  5. INTER-LAYER ABSORPTION: The paper's thin absorbing layer
//     (Eq. 7-8, A(x,y) = exp(-P(x,y))) is a spatially-varying
//     pointwise boundary attenuation applied between convolution
//     passes in the texture-space pipeline.  In our point-sampled
//     BSSRDF, we do not implement this absorbing layer.  The
//     Fresnel coupling between epidermis and dermis is handled by
//     the adding-doubling layer stacking (Ft_down, Ft_up), and
//     the epidermis already absorbs melanin volumetrically.  The
//     17.5% melanin leakage is omitted.  Supporting heterogeneous
//     inter-layer absorption would require the full texture-space
//     convolution pipeline described in Section 4.3.
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
			// Precomputed profile data (Sum-of-Gaussians)
			//
			// The composite two-layer reflectance is fit in the Hankel
			// (spatial frequency) domain as a Sum-of-Gaussians:
			//   R_tilde(s) ≈ Σ c_k * exp(-σ_k² * s² / 2)
			//
			// which corresponds in the spatial domain to:
			//   Rd(r) = Σ (c_k / (2π σ_k²)) * exp(-r² / (2 σ_k²))
			//
			// Each Gaussian component is importance-sampled via its
			// Rayleigh distribution: r = σ_k * sqrt(-2 ln(1-u)).
			//=============================================================

			/// A single Gaussian term in the diffusion profile.
			struct GaussianTerm
			{
				double	weight;			///< c_k: integrated mass (total hemispherical reflectance of this term)
				double	variance;		///< σ_k²: Gaussian variance in cm²
			};

			static const int K_TERMS = 6;
			static const int NUM_RGB = 3;
			static const int NUM_SPECTRAL = 31;			///< 400-700 nm at 10 nm spacing

			GaussianTerm		m_rgb_terms[NUM_RGB][K_TERMS];
			Scalar				m_rgb_total_weight[NUM_RGB];		///< Σ c_k per channel
			Scalar				m_rgb_cdf[NUM_RGB][K_TERMS];

			GaussianTerm		m_spectral_terms[NUM_SPECTRAL][K_TERMS];
			Scalar				m_spectral_total_weight[NUM_SPECTRAL];

			static const Scalar	ms_rgb_wavelengths[NUM_RGB];
			static const Scalar	ms_spectral_wavelengths[NUM_SPECTRAL];

			Scalar				m_max_variance;		///< Largest variance across all terms (for GetMaximumDistanceForError)

			virtual ~DonnerJensenSkinDiffusionProfile();

			//=============================================================
			// Absorption / scattering helpers
			//=============================================================

			static Scalar ComputeSkinBaselineAbsorption( const Scalar nm );
			static Scalar ComputeEpidermisScattering( const Scalar nm );
			static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta );

			void ComputePerLayerCoefficients(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				LayerParams layers_out[2]
				) const;

			void PrecomputeProfiles();

			void PrecomputeProfileAtWavelength(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				GaussianTerm terms_out[K_TERMS],
				Scalar& total_weight_out,
				Scalar cdf_out[K_TERMS]
				);

			/// Evaluate the K-term Sum-of-Gaussians at radius r.
			static Scalar EvaluateSumOfGaussians(
				const GaussianTerm terms[K_TERMS],
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
