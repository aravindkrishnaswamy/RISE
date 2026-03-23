//////////////////////////////////////////////////////////////////////
//
//  BioSpecDiffusionProfile.cpp - Implementation of the BioSpec
//  biophysically-parameterized BSSRDF diffusion profile.
//
//  See BioSpecDiffusionProfile.h for overview and references.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BioSpecDiffusionProfile.h"
#include "BioSpecSkinData.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

BioSpecDiffusionProfile::BioSpecDiffusionProfile(
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
	) :
  pnt_thickness_SC( thickness_SC_ ),
  pnt_thickness_epidermis( thickness_epidermis_ ),
  pnt_thickness_papillary_dermis( thickness_papillary_dermis_ ),
  pnt_thickness_reticular_dermis( thickness_reticular_dermis_ ),
  pnt_ior_SC( ior_SC_ ),
  pnt_ior_epidermis( ior_epidermis_ ),
  pnt_ior_papillary_dermis( ior_papillary_dermis_ ),
  pnt_ior_reticular_dermis( ior_reticular_dermis_ ),
  pnt_concentration_eumelanin( concentration_eumelanin_ ),
  pnt_concentration_pheomelanin( concentration_pheomelanin_ ),
  pnt_melanosomes_in_epidermis( melanosomes_in_epidermis_ ),
  pnt_hb_ratio( hb_ratio_ ),
  pnt_whole_blood_in_papillary_dermis( whole_blood_in_papillary_dermis_ ),
  pnt_whole_blood_in_reticular_dermis( whole_blood_in_reticular_dermis_ ),
  pnt_bilirubin_concentration( bilirubin_concentration_ ),
  pnt_betacarotene_concentration_SC( betacarotene_concentration_SC_ ),
  pnt_betacarotene_concentration_epidermis( betacarotene_concentration_epidermis_ ),
  pnt_betacarotene_concentration_dermis( betacarotene_concentration_dermis_ ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 ),
  pBilirubinExt( 0 ),
  pBetaCaroteneExt( 0 )
{
	// Addref all painter references
	pnt_thickness_SC.addref();
	pnt_thickness_epidermis.addref();
	pnt_thickness_papillary_dermis.addref();
	pnt_thickness_reticular_dermis.addref();
	pnt_ior_SC.addref();
	pnt_ior_epidermis.addref();
	pnt_ior_papillary_dermis.addref();
	pnt_ior_reticular_dermis.addref();
	pnt_concentration_eumelanin.addref();
	pnt_concentration_pheomelanin.addref();
	pnt_melanosomes_in_epidermis.addref();
	pnt_hb_ratio.addref();
	pnt_whole_blood_in_papillary_dermis.addref();
	pnt_whole_blood_in_reticular_dermis.addref();
	pnt_bilirubin_concentration.addref();
	pnt_betacarotene_concentration_SC.addref();
	pnt_betacarotene_concentration_epidermis.addref();
	pnt_betacarotene_concentration_dermis.addref();

	hb_concentration = SkinData::hb_concen_whole_blood;

	// Build chromophore extinction lookup tables (same as BioSpecSkinSPF)

	// Eumelanin
	{
		const int count = sizeof( SkinData::omlc_eumelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_eumelanin_wavelengths, SkinData::omlc_eumelanin_ext_mgml );
		pEumelaninExt = pFunc;
	}

	// Pheomelanin
	{
		const int count = sizeof( SkinData::omlc_pheomelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_pheomelanin_wavelengths, SkinData::omlc_pheomelanin_ext_mgml );
		pPheomelaninExt = pFunc;
	}

	// Hemoglobin (oxy + deoxy)
	{
		const int count = sizeof( SkinData::omlc_prahl_hemoglobin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pOxyFunc = 0;
		IPiecewiseFunction1D* pDeOxyFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pOxyFunc );
		RISE_API_CreatePiecewiseLinearFunction1D( &pDeOxyFunc );
		pOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths, SkinData::omlc_prahl_oxyhemoglobin );
		pDeOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths, SkinData::omlc_prahl_deoxyhemoglobin );
		pOxyHemoglobinExt = pOxyFunc;
		pDeoxyHemoglobinExt = pDeOxyFunc;
	}

	// Bilirubin
	{
		const int count = sizeof( SkinData::omlc_prahl_bilirubin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_bilirubin_wavelengths, SkinData::omlc_prahl_bilirubin );
		pBilirubinExt = pFunc;
	}

	// Beta-carotene
	{
		const int count = sizeof( SkinData::omlc_prahl_betacarotene_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_betacarotene_wavelengths, SkinData::omlc_prahl_betacarotene );
		pBetaCaroteneExt = pFunc;
	}
}

BioSpecDiffusionProfile::~BioSpecDiffusionProfile()
{
	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );

	pnt_thickness_SC.release();
	pnt_thickness_epidermis.release();
	pnt_thickness_papillary_dermis.release();
	pnt_thickness_reticular_dermis.release();
	pnt_ior_SC.release();
	pnt_ior_epidermis.release();
	pnt_ior_papillary_dermis.release();
	pnt_ior_reticular_dermis.release();
	pnt_concentration_eumelanin.release();
	pnt_concentration_pheomelanin.release();
	pnt_melanosomes_in_epidermis.release();
	pnt_hb_ratio.release();
	pnt_whole_blood_in_papillary_dermis.release();
	pnt_whole_blood_in_reticular_dermis.release();
	pnt_bilirubin_concentration.release();
	pnt_betacarotene_concentration_SC.release();
	pnt_betacarotene_concentration_epidermis.release();
	pnt_betacarotene_concentration_dermis.release();
}

//=============================================================
// Static helpers
//=============================================================

Scalar BioSpecDiffusionProfile::ComputeSkinBaselineAbsorptionCoefficient( const Scalar nm )
{
	return (0.244 + 85.3 * exp( -(nm-154.0)/66.2));
}

Scalar BioSpecDiffusionProfile::ComputeBeta(
	const Scalar lambda,
	const Scalar ior_medium
	)
{
	// Rayleigh scattering coefficient for dermal collagen fibers
	static const double ior_collagen = 1.5;
	const double ior_diff = ior_collagen / ior_medium;
	const double ior_factor = pow( ior_diff*ior_diff - 1.0, 2.0 );

	static const double PI_3 = ::pow( PI, 3.0 );

	static const double sphere_radius = 5e-6 / 2.0;
	static const double sphere_volume = 4.0/3.0 * pow(sphere_radius, 3.0) * PI;
	static const double num_fibers = 1.0 / sphere_volume * 0.21;

	double a = 8.0 * PI_3 * ior_factor;
	double b = 3.0 * pow( lambda, 4.0 );
	double c = b * num_fibers;

	return a / c;
}

Scalar BioSpecDiffusionProfile::ComputeScalingFactor( const Scalar A )
{
	// Christensen & Burley 2015 empirical fit
	const Scalar diff = A - 0.8;
	return 1.9 - A + 3.5 * diff * diff;
}

Scalar BioSpecDiffusionProfile::SchlickFresnel(
	const Scalar cosTheta,
	const Scalar eta
	)
{
	const Scalar R0 = ((1.0 - eta) / (1.0 + eta)) * ((1.0 - eta) / (1.0 + eta));
	const Scalar c = 1.0 - cosTheta;
	const Scalar c2 = c * c;
	return R0 + (1.0 - R0) * c2 * c2 * c;
}

//=============================================================
// Core: compute effective sigma_a and sigma_s' from BioSpec
//=============================================================

void BioSpecDiffusionProfile::ComputeEffectiveCoefficients(
	const Scalar nm,
	const RayIntersectionGeometric& ri,
	Scalar& sigma_a_out,
	Scalar& sigma_sp_out
	) const
{
	// Extract painter values
	const Scalar thickness_SC = pnt_thickness_SC.GetColor( ri )[0];
	const Scalar thickness_epidermis = pnt_thickness_epidermis.GetColor( ri )[0];
	const Scalar thickness_papillary_dermis = pnt_thickness_papillary_dermis.GetColor( ri )[0];
	const Scalar thickness_reticular_dermis = pnt_thickness_reticular_dermis.GetColor( ri )[0];

	const Scalar ior_papillary = pnt_ior_papillary_dermis.GetColor( ri )[0];
	const Scalar ior_reticular = pnt_ior_reticular_dermis.GetColor( ri )[0];

	const Scalar conc_eumelanin = pnt_concentration_eumelanin.GetColor( ri )[0];
	const Scalar conc_pheomelanin = pnt_concentration_pheomelanin.GetColor( ri )[0];
	const Scalar melanosomes = pnt_melanosomes_in_epidermis.GetColor( ri )[0];

	const Scalar hb_ratio = pnt_hb_ratio.GetColor( ri )[0];
	const Scalar blood_papillary = pnt_whole_blood_in_papillary_dermis.GetColor( ri )[0];
	const Scalar blood_reticular = pnt_whole_blood_in_reticular_dermis.GetColor( ri )[0];

	const Scalar bilirubin_conc = pnt_bilirubin_concentration.GetColor( ri )[0];
	const Scalar carotene_SC = pnt_betacarotene_concentration_SC.GetColor( ri )[0];
	const Scalar carotene_epidermis = pnt_betacarotene_concentration_epidermis.GetColor( ri )[0];
	const Scalar carotene_dermis = pnt_betacarotene_concentration_dermis.GetColor( ri )[0];

	const Scalar baseline = ComputeSkinBaselineAbsorptionCoefficient( nm );

	//----------------------------------------------------------
	// Per-layer absorption coefficients (same formulas as BioSpecSkinSPF)
	//----------------------------------------------------------

	// Stratum corneum
	const Scalar abs_carotene_SC = pBetaCaroteneExt->Evaluate(nm) * carotene_SC / 537.0 * log(10.0);
	const Scalar sigma_a_SC = abs_carotene_SC + baseline;

	// Epidermis
	const Scalar abs_eumelanin = pEumelaninExt->Evaluate(nm) * conc_eumelanin;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate(nm) * conc_pheomelanin;
	const Scalar abs_carotene_epi = pBetaCaroteneExt->Evaluate(nm) * carotene_epidermis / 537.0 * log(10.0);
	const Scalar sigma_a_epidermis = (abs_eumelanin + abs_pheomelanin) * melanosomes
		+ (abs_carotene_epi + baseline) * (1.0 - melanosomes);

	// Papillary dermis
	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate(nm) * hb_concentration) / 66500.0 * log(10.0);
	const Scalar abs_hb = (pDeoxyHemoglobinExt->Evaluate(nm) * hb_concentration) / 66500.0 * log(10.0);
	const Scalar abs_bilirubin = pBilirubinExt->Evaluate(nm) * bilirubin_conc / 585.0 * log(10.0);
	const Scalar abs_carotene_dermis = pBetaCaroteneExt->Evaluate(nm) * carotene_dermis / 537.0 * log(10.0);

	const Scalar sigma_a_papillary = (abs_hbo2 * hb_ratio + abs_hb * (1.0 - hb_ratio) + abs_bilirubin) * blood_papillary
		+ (abs_carotene_dermis + baseline) * (1.0 - blood_papillary);

	// Reticular dermis
	const Scalar sigma_a_reticular = (abs_hbo2 * hb_ratio + abs_hb * (1.0 - hb_ratio) + abs_bilirubin) * blood_reticular
		+ (abs_carotene_dermis + baseline) * (1.0 - blood_reticular);

	//----------------------------------------------------------
	// Per-layer reduced scattering coefficients
	//----------------------------------------------------------

	// SC and epidermis: Mie scattering is dominant.  Use an empirical
	// approximation: sigma_s' ~ 73.7 * (nm/nm_ref)^(-2.33) cm^-1
	// (Bashkatov et al. 2005 fit to Mie theory for epidermal tissue)
	const Scalar sigma_sp_SC = 73.7 * pow( nm / 500.0, -2.33 );
	const Scalar sigma_sp_epidermis = sigma_sp_SC;  // same tissue type

	// Dermis: Rayleigh scattering from collagen fibers
	// ComputeBeta gives scattering coefficient; Rayleigh has g~0 so sigma_s' ~ sigma_s
	const Scalar lambda_cm = nm * 1e-7;  // convert nm to cm
	const Scalar sigma_sp_papillary = ComputeBeta( lambda_cm, ior_papillary );
	const Scalar sigma_sp_reticular = ComputeBeta( lambda_cm, ior_reticular );

	//----------------------------------------------------------
	// Combine into effective coefficients (thickness-weighted average)
	//----------------------------------------------------------

	const Scalar total_thickness = thickness_SC + thickness_epidermis
		+ thickness_papillary_dermis + thickness_reticular_dermis;

	if( total_thickness < 1e-10 )
	{
		sigma_a_out = 0;
		sigma_sp_out = 0;
		return;
	}

	const Scalar inv_total = 1.0 / total_thickness;

	sigma_a_out = (sigma_a_SC * thickness_SC
		+ sigma_a_epidermis * thickness_epidermis
		+ sigma_a_papillary * thickness_papillary_dermis
		+ sigma_a_reticular * thickness_reticular_dermis) * inv_total;

	sigma_sp_out = (sigma_sp_SC * thickness_SC
		+ sigma_sp_epidermis * thickness_epidermis
		+ sigma_sp_papillary * thickness_papillary_dermis
		+ sigma_sp_reticular * thickness_reticular_dermis) * inv_total;
}

//=============================================================
// Profile evaluation
//=============================================================

/// Evaluates Rd(r) for a single channel given albedo, mfp, and scaling factor.
static Scalar EvaluateRdChannel(
	const Scalar r,
	const Scalar albedo,
	const Scalar mfp,
	const Scalar s
	)
{
	if( albedo < 1e-10 || mfp < 1e-10 ) return 0.0;

	const Scalar rClamped = (r < 1e-10) ? 1e-10 : r;
	const Scalar sr_over_d = s * rClamped / mfp;

	const Scalar term1 = exp( -sr_over_d );
	const Scalar term2 = exp( -sr_over_d / 3.0 );

	return albedo * s / (8.0 * PI * mfp) * (term1 + term2) / rClamped;
}

RISEPel BioSpecDiffusionProfile::EvaluateProfile(
	const Scalar r,
	const RayIntersectionGeometric& ri
	) const
{
	// Evaluate at three representative wavelengths for RGB:
	// R ~ 615 nm, G ~ 550 nm, B ~ 465 nm
	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };

	RISEPel result;
	for( int c = 0; c < 3; c++ )
	{
		Scalar sigma_a, sigma_sp;
		ComputeEffectiveCoefficients( rgb_wavelengths[c], ri, sigma_a, sigma_sp );

		const Scalar st_prime = sigma_sp + sigma_a;
		const Scalar albedo = st_prime > 1e-10 ? sigma_sp / st_prime : 0.0;
		const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
		const Scalar s = ComputeScalingFactor( albedo );

		result[c] = EvaluateRdChannel( r, albedo, mfp, s );
	}

	return result;
}

Scalar BioSpecDiffusionProfile::EvaluateProfileNM(
	const Scalar r,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	Scalar sigma_a, sigma_sp;
	ComputeEffectiveCoefficients( nm, ri, sigma_a, sigma_sp );

	const Scalar st_prime = sigma_sp + sigma_a;
	const Scalar albedo = st_prime > 1e-10 ? sigma_sp / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	return EvaluateRdChannel( r, albedo, mfp, s );
}

//=============================================================
// Importance sampling
//=============================================================

Scalar BioSpecDiffusionProfile::SampleRadius(
	const Scalar u,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };

	Scalar sigma_a, sigma_sp;
	ComputeEffectiveCoefficients( rgb_wavelengths[channel], ri, sigma_a, sigma_sp );

	const Scalar st_prime = sigma_sp + sigma_a;
	const Scalar albedo = st_prime > 1e-10 ? sigma_sp / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	if( mfp < 1e-10 || s < 1e-10 ) return 0.0;

	// 50/50 mixture of two exponentials (same as BurleyNormalizedDiffusionProfile)
	if( u < 0.5 )
	{
		const Scalar u_remapped = 2.0 * u;
		const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
		return -mfp / s * log( one_minus_u );
	}
	else
	{
		const Scalar u_remapped = 2.0 * (u - 0.5);
		const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
		return -3.0 * mfp / s * log( one_minus_u );
	}
}

Scalar BioSpecDiffusionProfile::PdfRadius(
	const Scalar r,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };

	Scalar sigma_a, sigma_sp;
	ComputeEffectiveCoefficients( rgb_wavelengths[channel], ri, sigma_a, sigma_sp );

	const Scalar st_prime = sigma_sp + sigma_a;
	const Scalar albedo = st_prime > 1e-10 ? sigma_sp / st_prime : 0.0;
	const Scalar mfp = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
	const Scalar s = ComputeScalingFactor( albedo );

	if( mfp < 1e-10 || s < 1e-10 ) return 0.0;

	const Scalar lambda1 = s / mfp;
	const Scalar lambda2 = s / (3.0 * mfp);

	return 0.5 * lambda1 * exp( -lambda1 * r )
	     + 0.5 * lambda2 * exp( -lambda2 * r );
}

//=============================================================
// Fresnel and IOR
//=============================================================

Scalar BioSpecDiffusionProfile::FresnelTransmission(
	const Scalar cosTheta,
	const RayIntersectionGeometric& ri
	) const
{
	// Use stratum corneum IOR for the outermost boundary
	const Scalar eta = pnt_ior_SC.GetColor( ri )[0];
	return 1.0 - SchlickFresnel( fabs(cosTheta), eta );
}

Scalar BioSpecDiffusionProfile::GetIOR(
	const RayIntersectionGeometric& ri
	) const
{
	return pnt_ior_SC.GetColor( ri )[0];
}

//=============================================================
// ISubSurfaceExtinctionFunction compatibility
//=============================================================

Scalar BioSpecDiffusionProfile::GetMaximumDistanceForError(
	const Scalar error
	) const
{
	if( error <= 0 ) return RISE_INFINITY;
	return -log(error) * 10.0;
}

RISEPel BioSpecDiffusionProfile::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	// Without ri, cannot evaluate accurately.  Return zero —
	// actual evaluation is done through EvaluateProfile().
	return RISEPel( 0, 0, 0 );
}
