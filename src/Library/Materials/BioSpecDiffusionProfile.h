//////////////////////////////////////////////////////////////////////
//
//  BioSpecDiffusionProfile.h - A BSSRDF diffusion profile driven
//  by BioSpec's biophysical skin parameters.
//
//  Computes per-layer absorption coefficients from BioSpec's
//  chromophore formulas (melanin, hemoglobin, bilirubin,
//  beta-carotene, baseline) and combines them with Rayleigh
//  scattering coefficients to produce effective (sigma_a, sigma_s')
//  for the full skin stack.  These feed into a Burley normalized
//  diffusion profile for Rd(r) evaluation and importance sampling.
//
//  The profile supports both RGB and spectral (NM) evaluation.
//
//  References:
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

			virtual ~BioSpecDiffusionProfile();

			//=============================================================
			// Absorption coefficient helpers (same formulas as BioSpecSkinSPF)
			//=============================================================

			static Scalar ComputeSkinBaselineAbsorptionCoefficient( const Scalar nm );
			static Scalar ComputeBeta( const Scalar lambda, const Scalar ior_medium );
			static Scalar ComputeScalingFactor( const Scalar A );
			static Scalar SchlickFresnel( const Scalar cosTheta, const Scalar eta );

			/// Compute effective sigma_a and sigma_s' for the full skin stack
			/// at a given wavelength, using BioSpec biophysical parameters.
			void ComputeEffectiveCoefficients(
				const Scalar nm,
				const RayIntersectionGeometric& ri,
				Scalar& sigma_a_out,
				Scalar& sigma_sp_out
				) const;

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
