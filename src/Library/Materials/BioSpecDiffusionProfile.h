//////////////////////////////////////////////////////////////////////
//
//  BioSpecDiffusionProfile.h - A BSSRDF diffusion profile driven
//  by BioSpec's biophysical skin parameters.
//
//  Uses per-layer multipole diffusion (Donner & Jensen 2005) to
//  compute a composite Rd(r) profile for the 4-layer skin stack.
//  The layers are composited in the Hankel (spatial frequency)
//  domain, then the result is fit to a sum of K exponentials for
//  efficient importance sampling.
//
//  The profile supports both RGB and spectral (NM) evaluation.
//
//  References:
//    Donner & Jensen 2005 — Light Diffusion in Multi-Layered
//      Translucent Materials
//    Christensen & Burley 2015 — Approximate Reflectance Profiles
//    Krishnaswamy & Baranoski 2004 — BioSpec skin model
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_DIFFUSION_PROFILE_H
#define BIOSPEC_DIFFUSION_PROFILE_H

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
		class BioSpecDiffusionProfile :
			public virtual ISubSurfaceDiffusionProfile,
			public virtual Reference
		{
		protected:
			// BioSpec biophysical parameters (IPainter references)
			const IPainter&		pnt_thickness_SC;
			const IPainter&		pnt_thickness_epidermis;
			const IPainter&		pnt_thickness_papillary_dermis;
			const IPainter&		pnt_thickness_reticular_dermis;

			const IPainter&		pnt_ior_SC;
			const IPainter&		pnt_ior_epidermis;
			const IPainter&		pnt_ior_papillary_dermis;
			const IPainter&		pnt_ior_reticular_dermis;

			const IPainter&		pnt_concentration_eumelanin;
			const IPainter&		pnt_concentration_pheomelanin;
			const IPainter&		pnt_melanosomes_in_epidermis;

			const IPainter&		pnt_hb_ratio;
			const IPainter&		pnt_whole_blood_in_papillary_dermis;
			const IPainter&		pnt_whole_blood_in_reticular_dermis;

			const IPainter&		pnt_bilirubin_concentration;
			const IPainter&		pnt_betacarotene_concentration_SC;
			const IPainter&		pnt_betacarotene_concentration_epidermis;
			const IPainter&		pnt_betacarotene_concentration_dermis;

			// Chromophore extinction lookup tables (shared with BioSpecSkinSPF)
			IFunction1D*		pEumelaninExt;
			IFunction1D*		pPheomelaninExt;
			IFunction1D*		pOxyHemoglobinExt;
			IFunction1D*		pDeoxyHemoglobinExt;
			IFunction1D*		pBilirubinExt;
			IFunction1D*		pBetaCaroteneExt;

			Scalar				hb_concentration;		///< Hemoglobin concentration in whole blood (g/L)

			//=============================================================
			// Precomputed multipole diffusion profile data
			//=============================================================

			static const int K_TERMS = 6;				///< Number of exponential terms per wavelength
			static const int NUM_RGB = 3;				///< RGB channels
			static const int NUM_SPECTRAL = 31;			///< 400-700 nm at 10 nm spacing

			/// Per-wavelength sum-of-exponentials fit for RGB mode (3 wavelengths)
			ExponentialTerm		m_rgb_terms[NUM_RGB][K_TERMS];
			Scalar				m_rgb_total_weight[NUM_RGB];		///< Sum of weights per channel (for PDF normalization)
			Scalar				m_rgb_cdf[NUM_RGB][K_TERMS];		///< CDF for mixture sampling

			/// Per-wavelength sum-of-exponentials fit for spectral mode (31 wavelengths)
			ExponentialTerm		m_spectral_terms[NUM_SPECTRAL][K_TERMS];
			Scalar				m_spectral_total_weight[NUM_SPECTRAL];

			static const Scalar	ms_rgb_wavelengths[NUM_RGB];
			static const Scalar	ms_spectral_wavelengths[NUM_SPECTRAL];

			Scalar				m_min_rate;				///< Minimum rate across all terms (for GetMaximumDistanceForError)

			virtual ~BioSpecDiffusionProfile();

			//=============================================================
			// Absorption coefficient helpers (same formulas as BioSpecSkinSPF)
			//=============================================================

			static Scalar ComputeSkinBaselineAbsorptionCoefficient( const Scalar nm );
			static Scalar ComputeBeta( const Scalar lambda, const Scalar ior_medium );
			static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta );

			/// Compute per-layer optical properties at a given wavelength.
			/// Fills 4 LayerParams (SC, epidermis, papillary dermis, reticular dermis).
			void ComputePerLayerCoefficients(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				LayerParams layers_out[4]
				) const;

			/// Precompute multipole profiles for all wavelengths.
			/// Called once from the constructor after LUT setup.
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
			BioSpecDiffusionProfile(
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
				const IPainter& betacarotene_concentration_dermis_
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
