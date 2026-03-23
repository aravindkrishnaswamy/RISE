//////////////////////////////////////////////////////////////////////
//
//  BioSpecDiffusionProfile.cpp - Implementation of the BioSpec
//  biophysically-parameterized BSSRDF diffusion profile using
//  per-layer multipole diffusion (Donner & Jensen 2005).
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
#include "MultipoleDiffusion.h"
#include "../Utilities/HankelTransform.h"
#include "../Utilities/SumOfExponentialsFit.h"
#include "../RISE_API.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

//=============================================================
// Static wavelength tables
//=============================================================

const Scalar BioSpecDiffusionProfile::ms_rgb_wavelengths[NUM_RGB] = { 615.0, 550.0, 465.0 };

const Scalar BioSpecDiffusionProfile::ms_spectral_wavelengths[NUM_SPECTRAL] = {
	400.0, 410.0, 420.0, 430.0, 440.0, 450.0, 460.0, 470.0, 480.0, 490.0,
	500.0, 510.0, 520.0, 530.0, 540.0, 550.0, 560.0, 570.0, 580.0, 590.0,
	600.0, 610.0, 620.0, 630.0, 640.0, 650.0, 660.0, 670.0, 680.0, 690.0,
	700.0
};

//=============================================================
// Constructor / Destructor
//=============================================================

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
  pBetaCaroteneExt( 0 ),
  m_min_rate( 1.0 )
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

	// Initialize precomputed tables to zero
	memset( m_rgb_terms, 0, sizeof(m_rgb_terms) );
	memset( m_rgb_total_weight, 0, sizeof(m_rgb_total_weight) );
	memset( m_rgb_cdf, 0, sizeof(m_rgb_cdf) );
	memset( m_spectral_terms, 0, sizeof(m_spectral_terms) );
	memset( m_spectral_total_weight, 0, sizeof(m_spectral_total_weight) );

	// Run the multipole precomputation pipeline
	PrecomputeProfiles();
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
// Per-layer coefficient computation
//=============================================================

void BioSpecDiffusionProfile::ComputePerLayerCoefficients(
	const Scalar nm,
	const RayIntersectionGeometric& ri,
	LayerParams layers_out[4]
	) const
{
	// Extract painter values
	const Scalar thickness_SC = pnt_thickness_SC.GetColor( ri )[0];
	const Scalar thickness_epidermis = pnt_thickness_epidermis.GetColor( ri )[0];
	const Scalar thickness_papillary_dermis = pnt_thickness_papillary_dermis.GetColor( ri )[0];
	const Scalar thickness_reticular_dermis = pnt_thickness_reticular_dermis.GetColor( ri )[0];

	const Scalar ior_SC = pnt_ior_SC.GetColor( ri )[0];
	const Scalar ior_epidermis = pnt_ior_epidermis.GetColor( ri )[0];
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
	// Per-layer absorption coefficients
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

	const Scalar sigma_sp_SC = 73.7 * pow( nm / 500.0, -2.33 );
	const Scalar sigma_sp_epidermis = sigma_sp_SC;

	const Scalar lambda_cm = nm * 1e-7;
	const Scalar sigma_sp_papillary = ComputeBeta( lambda_cm, ior_papillary );
	const Scalar sigma_sp_reticular = ComputeBeta( lambda_cm, ior_reticular );

	//----------------------------------------------------------
	// Fill LayerParams (top to bottom)
	//----------------------------------------------------------

	// Layer 0: Stratum Corneum
	layers_out[0].sigma_a = sigma_a_SC;
	layers_out[0].sigma_sp = sigma_sp_SC;
	layers_out[0].thickness = thickness_SC;
	layers_out[0].ior = ior_SC;
	ComputeLayerDerivedParams( layers_out[0] );

	// Layer 1: Epidermis
	layers_out[1].sigma_a = sigma_a_epidermis;
	layers_out[1].sigma_sp = sigma_sp_epidermis;
	layers_out[1].thickness = thickness_epidermis;
	layers_out[1].ior = ior_epidermis;
	ComputeLayerDerivedParams( layers_out[1] );

	// Layer 2: Papillary Dermis
	layers_out[2].sigma_a = sigma_a_papillary;
	layers_out[2].sigma_sp = sigma_sp_papillary;
	layers_out[2].thickness = thickness_papillary_dermis;
	layers_out[2].ior = ior_papillary;
	ComputeLayerDerivedParams( layers_out[2] );

	// Layer 3: Reticular Dermis
	layers_out[3].sigma_a = sigma_a_reticular;
	layers_out[3].sigma_sp = sigma_sp_reticular;
	layers_out[3].thickness = thickness_reticular_dermis;
	layers_out[3].ior = ior_reticular;
	ComputeLayerDerivedParams( layers_out[3] );
}

//=============================================================
// Multipole precomputation pipeline
//=============================================================

void BioSpecDiffusionProfile::PrecomputeProfileAtWavelength(
	const Scalar nm,
	const RayIntersectionGeometric& ri,
	ExponentialTerm terms_out[K_TERMS],
	Scalar& total_weight_out,
	Scalar cdf_out[K_TERMS]
	)
{
	// Get per-layer optical properties at this wavelength
	LayerParams layers[4];
	ComputePerLayerCoefficients( nm, ri, layers );

	// Hankel frequency grid
	static const int N_FREQ = 512;
	static const int N_MULTIPOLE = 20;

	HankelGrid grid;
	grid.Create( 0.01, 1e5, N_FREQ );

	// Compute composite reflectance in Hankel domain
	double composite_R[N_FREQ];
	ComputeCompositeProfileHankel( layers, 4, grid.s, N_FREQ, N_MULTIPOLE, composite_R );

	// Inverse Hankel transform to tabulated Rd(r)
	static const int N_RADIAL = 256;
	double r_samples[N_RADIAL];
	double Rd_samples[N_RADIAL];

	const double r_min = 1e-5;		// cm
	const double r_max = 5.0;		// cm
	const double log_r_min = log( r_min );
	const double log_r_max = log( r_max );
	const double log_r_step = (log_r_max - log_r_min) / (N_RADIAL - 1);

	for( int i = 0; i < N_RADIAL; i++ )
	{
		r_samples[i] = exp( log_r_min + i * log_r_step );
		Rd_samples[i] = grid.InverseTransform( composite_R, r_samples[i] );

		// Clamp negative values (numerical noise)
		if( Rd_samples[i] < 0 ) Rd_samples[i] = 0;
	}

	// Determine rate range from per-layer sigma_tr
	double min_sigma_tr = 1e10;
	double max_sigma_tr = 0;
	for( int L = 0; L < 4; L++ )
	{
		if( layers[L].sigma_tr > 1e-10 && layers[L].sigma_tr < min_sigma_tr )
			min_sigma_tr = layers[L].sigma_tr;
		if( layers[L].sigma_tr > max_sigma_tr )
			max_sigma_tr = layers[L].sigma_tr;
	}

	// Ensure valid range
	if( min_sigma_tr > max_sigma_tr || min_sigma_tr < 1e-10 )
	{
		min_sigma_tr = 1.0;
		max_sigma_tr = 100.0;
	}

	// Extend range slightly on the high end to capture sharp peaks.
	// Do NOT extend below min_sigma_tr — the diffusion profile cannot
	// decay slower than exp(-sigma_tr * r), so slower rates are unphysical
	// and create excessively large BSSRDF search radii.
	max_sigma_tr *= 2.0;

	// Place K rates on a geometric grid
	double rates[K_TERMS];
	const double log_min_rate = log( min_sigma_tr );
	const double log_max_rate = log( max_sigma_tr );
	const double log_rate_step = (K_TERMS > 1) ? (log_max_rate - log_min_rate) / (K_TERMS - 1) : 0;

	for( int k = 0; k < K_TERMS; k++ )
	{
		rates[k] = exp( log_min_rate + k * log_rate_step );
	}

	// Fit sum of exponentials via NNLS
	FitSumOfExponentials( r_samples, Rd_samples, N_RADIAL, rates, K_TERMS, terms_out );

	// Compute total weight and CDF for mixture sampling
	total_weight_out = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		total_weight_out += terms_out[k].weight;
	}

	double cumulative = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		cumulative += terms_out[k].weight;
		cdf_out[k] = (total_weight_out > 1e-30) ? (cumulative / total_weight_out) : 0;
	}
}

void BioSpecDiffusionProfile::PrecomputeProfiles()
{
	// Create a synthetic RayIntersectionGeometric for painter evaluation.
	// For uniform painters this is arbitrary; the painters return constant values.
	Ray dummyRay( Point3(0,0,0), Vector3(0,0,1) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = Vector3(0,1,0);

	m_min_rate = 1e10;

	// Precompute RGB profiles
	for( int c = 0; c < NUM_RGB; c++ )
	{
		PrecomputeProfileAtWavelength(
			ms_rgb_wavelengths[c], ri,
			m_rgb_terms[c], m_rgb_total_weight[c], m_rgb_cdf[c] );

		for( int k = 0; k < K_TERMS; k++ )
		{
			if( m_rgb_terms[c][k].weight > 1e-20 && m_rgb_terms[c][k].rate < m_min_rate )
				m_min_rate = m_rgb_terms[c][k].rate;
		}
	}

	// Precompute spectral profiles
	for( int w = 0; w < NUM_SPECTRAL; w++ )
	{
		Scalar dummy_cdf[K_TERMS];
		PrecomputeProfileAtWavelength(
			ms_spectral_wavelengths[w], ri,
			m_spectral_terms[w], m_spectral_total_weight[w], dummy_cdf );

		for( int k = 0; k < K_TERMS; k++ )
		{
			if( m_spectral_terms[w][k].weight > 1e-20 && m_spectral_terms[w][k].rate < m_min_rate )
				m_min_rate = m_spectral_terms[w][k].rate;
		}
	}

	// Safety floor
	if( m_min_rate < 1e-10 ) m_min_rate = 0.1;

	GlobalLog()->PrintEx( eLog_Info,
		"BioSpecDiffusionProfile: Multipole precomputation complete (min_rate=%.3f cm^-1)",
		m_min_rate );
}

//=============================================================
// Sum-of-exponentials evaluation
//=============================================================

Scalar BioSpecDiffusionProfile::EvaluateSumOfExp(
	const ExponentialTerm terms[K_TERMS],
	const Scalar r
	)
{
	const Scalar rClamped = (r < 1e-10) ? 1e-10 : r;
	Scalar result = 0;

	for( int k = 0; k < K_TERMS; k++ )
	{
		if( terms[k].weight > 1e-30 )
		{
			result += terms[k].weight * exp( -terms[k].rate * rClamped );
		}
	}

	return result / rClamped;
}

//=============================================================
// Profile evaluation
//=============================================================

RISEPel BioSpecDiffusionProfile::EvaluateProfile(
	const Scalar r,
	const RayIntersectionGeometric& ri
	) const
{
	RISEPel result;
	for( int c = 0; c < NUM_RGB; c++ )
	{
		result[c] = EvaluateSumOfExp( m_rgb_terms[c], r );
	}
	return result;
}

Scalar BioSpecDiffusionProfile::EvaluateProfileNM(
	const Scalar r,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	// Find bracketing wavelengths and interpolate
	if( nm <= ms_spectral_wavelengths[0] )
		return EvaluateSumOfExp( m_spectral_terms[0], r );

	if( nm >= ms_spectral_wavelengths[NUM_SPECTRAL - 1] )
		return EvaluateSumOfExp( m_spectral_terms[NUM_SPECTRAL - 1], r );

	// Linear search (31 entries — fast enough)
	for( int w = 0; w < NUM_SPECTRAL - 1; w++ )
	{
		if( nm >= ms_spectral_wavelengths[w] && nm < ms_spectral_wavelengths[w + 1] )
		{
			const Scalar t = (nm - ms_spectral_wavelengths[w]) /
				(ms_spectral_wavelengths[w + 1] - ms_spectral_wavelengths[w]);

			const Scalar Rd_lo = EvaluateSumOfExp( m_spectral_terms[w], r );
			const Scalar Rd_hi = EvaluateSumOfExp( m_spectral_terms[w + 1], r );

			return Rd_lo * (1.0 - t) + Rd_hi * t;
		}
	}

	return EvaluateSumOfExp( m_spectral_terms[NUM_SPECTRAL / 2], r );
}

//=============================================================
// Importance sampling (K-term mixture)
//=============================================================

Scalar BioSpecDiffusionProfile::SampleRadius(
	const Scalar u,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const ExponentialTerm* terms = m_rgb_terms[channel];
	const Scalar* cdf = m_rgb_cdf[channel];
	const Scalar totalW = m_rgb_total_weight[channel];

	if( totalW < 1e-30 ) return 0.0;

	// Select which exponential term to sample from via the CDF
	int k = 0;
	for( k = 0; k < K_TERMS - 1; k++ )
	{
		if( u < cdf[k] ) break;
	}

	// Remap u into [0,1) within the selected term
	const Scalar cdf_lo = (k > 0) ? cdf[k - 1] : 0.0;
	const Scalar cdf_hi = cdf[k];
	const Scalar u_remapped = (cdf_hi > cdf_lo + 1e-30) ?
		(u - cdf_lo) / (cdf_hi - cdf_lo) : 0.0;

	// Invert exponential CDF: r = -log(1-u) / rate
	const Scalar rate = terms[k].rate;
	if( rate < 1e-30 ) return 0.0;

	const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
	return -log( one_minus_u ) / rate;
}

Scalar BioSpecDiffusionProfile::PdfRadius(
	const Scalar r,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const ExponentialTerm* terms = m_rgb_terms[channel];
	const Scalar totalW = m_rgb_total_weight[channel];

	if( totalW < 1e-30 ) return 0.0;

	// Mixture PDF: sum_k (w_k/W) * rate_k * exp(-rate_k * r)
	Scalar pdf = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		if( terms[k].weight > 1e-30 )
		{
			pdf += (terms[k].weight / totalW) * terms[k].rate * exp( -terms[k].rate * r );
		}
	}

	return pdf;
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
	// Use the slowest-decaying exponential term
	return -log(error) / m_min_rate;
}

RISEPel BioSpecDiffusionProfile::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	// Without ri, cannot evaluate accurately.  Return zero —
	// actual evaluation is done through EvaluateProfile().
	return RISEPel( 0, 0, 0 );
}
