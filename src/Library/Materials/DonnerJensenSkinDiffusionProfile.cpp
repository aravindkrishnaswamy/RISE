//////////////////////////////////////////////////////////////////////
//
//  DonnerJensenSkinDiffusionProfile.cpp - Implementation of the
//  Donner et al. 2008 spectral skin BSSRDF diffusion profile.
//
//  See DonnerJensenSkinDiffusionProfile.h for the full list of
//  deviations from the paper and their rationale.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DonnerJensenSkinDiffusionProfile.h"
#include "BioSpecSkinData.h"
#include "MultipoleDiffusion.h"
#include "../Utilities/HankelTransform.h"
// Note: NNLS solver is implemented inline in PrecomputeProfileAtWavelength
// (same algorithm as SumOfExponentialsFit.h but with Gaussian basis functions)
#include "../RISE_API.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

//=============================================================
// Static wavelength tables
//=============================================================

const Scalar DonnerJensenSkinDiffusionProfile::ms_rgb_wavelengths[NUM_RGB] = { 615.0, 550.0, 465.0 };

const Scalar DonnerJensenSkinDiffusionProfile::ms_spectral_wavelengths[NUM_SPECTRAL] = {
	400.0, 410.0, 420.0, 430.0, 440.0, 450.0, 460.0, 470.0, 480.0, 490.0,
	500.0, 510.0, 520.0, 530.0, 540.0, 550.0, 560.0, 570.0, 580.0, 590.0,
	600.0, 610.0, 620.0, 630.0, 640.0, 650.0, 660.0, 670.0, 680.0, 690.0,
	700.0
};

//=============================================================
// Constructor / Destructor
//=============================================================

DonnerJensenSkinDiffusionProfile::DonnerJensenSkinDiffusionProfile(
	const IPainter& melanin_fraction_,
	const IPainter& melanin_blend_,
	const IPainter& hemoglobin_epidermis_,
	const IPainter& carotene_fraction_,
	const IPainter& hemoglobin_dermis_,
	const IPainter& epidermis_thickness_,
	const IPainter& ior_epidermis_,
	const IPainter& ior_dermis_,
	const IPainter& blood_oxygenation_
	) :
  pnt_melanin_fraction( melanin_fraction_ ),
  pnt_melanin_blend( melanin_blend_ ),
  pnt_hemoglobin_epidermis( hemoglobin_epidermis_ ),
  pnt_carotene_fraction( carotene_fraction_ ),
  pnt_hemoglobin_dermis( hemoglobin_dermis_ ),
  pnt_epidermis_thickness( epidermis_thickness_ ),
  pnt_ior_epidermis( ior_epidermis_ ),
  pnt_ior_dermis( ior_dermis_ ),
  pnt_blood_oxygenation( blood_oxygenation_ ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 ),
  pBetaCaroteneExt( 0 ),
  m_max_variance( 1.0 )
{
	// Addref all painter references
	pnt_melanin_fraction.addref();
	pnt_melanin_blend.addref();
	pnt_hemoglobin_epidermis.addref();
	pnt_carotene_fraction.addref();
	pnt_hemoglobin_dermis.addref();
	pnt_epidermis_thickness.addref();
	pnt_ior_epidermis.addref();
	pnt_ior_dermis.addref();
	pnt_blood_oxygenation.addref();

	hb_concentration = SkinData::hb_concen_whole_blood;

	// Build chromophore extinction lookup tables from BioSpecSkinData

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

DonnerJensenSkinDiffusionProfile::~DonnerJensenSkinDiffusionProfile()
{
	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBetaCaroteneExt );

	pnt_melanin_fraction.release();
	pnt_melanin_blend.release();
	pnt_hemoglobin_epidermis.release();
	pnt_carotene_fraction.release();
	pnt_hemoglobin_dermis.release();
	pnt_epidermis_thickness.release();
	pnt_ior_epidermis.release();
	pnt_ior_dermis.release();
	pnt_blood_oxygenation.release();
}

//=============================================================
// Static helpers
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::ComputeSkinBaselineAbsorption( const Scalar nm )
{
	// Jacques 1998 baseline skin absorption (cm^-1)
	return (0.244 + 85.3 * exp( -(nm - 154.0) / 66.2 ));
}

Scalar DonnerJensenSkinDiffusionProfile::ComputeEpidermisScattering( const Scalar nm )
{
	// Paper Eq. 13: sigma_sp(lambda) = 14.74 * lambda^(-0.22) + 2.2e11 * lambda^(-4)
	// where lambda is in nm.  This is a Mie + Rayleigh decomposition from
	// Jacques 1998.  The dermis uses half this value per the paper.
	return 14.74 * pow( nm, -0.22 ) + 2.2e11 * pow( nm, -4.0 );
}

Scalar DonnerJensenSkinDiffusionProfile::SchlickFresnel(
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
// Per-layer coefficient computation (Donner 2008 Eq. 11-13)
//=============================================================

void DonnerJensenSkinDiffusionProfile::ComputePerLayerCoefficients(
	const Scalar nm,
	const RayIntersectionGeometric& ri,
	LayerParams layers_out[2]
	) const
{
	// Extract painter values
	const Scalar C_m = pnt_melanin_fraction.GetColor( ri )[0];
	const Scalar beta_m = pnt_melanin_blend.GetColor( ri )[0];
	const Scalar C_he = pnt_hemoglobin_epidermis.GetColor( ri )[0];
	const Scalar C_bc = pnt_carotene_fraction.GetColor( ri )[0];
	const Scalar C_hd = pnt_hemoglobin_dermis.GetColor( ri )[0];
	const Scalar thickness_epi = pnt_epidermis_thickness.GetColor( ri )[0];
	const Scalar ior_epi = pnt_ior_epidermis.GetColor( ri )[0];
	const Scalar ior_derm = pnt_ior_dermis.GetColor( ri )[0];
	const Scalar gamma = pnt_blood_oxygenation.GetColor( ri )[0];

	const Scalar baseline = ComputeSkinBaselineAbsorption( nm );

	//----------------------------------------------------------
	// Chromophore absorption coefficients
	//----------------------------------------------------------

	// Melanin absorption (eumelanin + pheomelanin weighted by beta_m).
	//
	// The OMLC tabulated data gives eu/pheo extinction in cm^-1/(mg/ml).
	// To convert to melanosome-level absorption (cm^-1), we use a single
	// reference concentration C_MEL for both types.  C_MEL is calibrated
	// so that a 50/50 eu/pheo blend at 550nm matches the Jacques 1998
	// melanosome power law: mu_a_mel = 6.6e11 * lambda^(-3.33).
	//
	//   Jacques at 550nm = 514 cm^-1
	//   OMLC eu(550) = 5.76, ph(550) = 2.0 cm^-1/(mg/ml)
	//   C_MEL = 514 / (0.5 * (5.76 + 2.0)) = 132.5 mg/ml
	//
	// Using the same concentration for both types ensures that beta_m
	// linearly blends between spectral shapes without concentration
	// bias, and C_m acts as the melanosome volume fraction.
	static const Scalar C_MEL = 132.5;		// mg/ml — calibrated to Jacques 1998

	const Scalar abs_eumelanin = pEumelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_melanin = beta_m * abs_eumelanin + (1.0 - beta_m) * abs_pheomelanin;

	// Hemoglobin absorption (oxy + deoxy weighted by gamma)
	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_hb = (pDeoxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_blood = gamma * abs_hbo2 + (1.0 - gamma) * abs_hb;

	// Carotene absorption
	const Scalar abs_carotene = pBetaCaroteneExt->Evaluate( nm ) * C_bc / 537.0 * log( 10.0 );

	//----------------------------------------------------------
	// Epidermis absorption (Eq. 11)
	//----------------------------------------------------------

	// sigma_a_epi = C_m * abs_melanin + C_he * abs_blood + C_bc * abs_carotene
	//             + (1 - C_m - C_he - C_bc) * baseline
	const Scalar remainder_epi = 1.0 - C_m - C_he - C_bc;
	const Scalar sigma_a_epi = C_m * abs_melanin
		+ C_he * abs_blood
		+ abs_carotene
		+ (remainder_epi > 0 ? remainder_epi : 0) * baseline;

	//----------------------------------------------------------
	// Dermis absorption (Eq. 12)
	//----------------------------------------------------------

	// Dermis absorption per Eq. 12:
	//   sigma_a_derm = C_hd * abs_blood + (1 - C_hd) * baseline
	//
	// Note: the paper's 17.5% melanin leakage (Eq. 7-8) is a
	// spatially-varying thin absorbing layer between the scattering
	// layers, applied as a pointwise multiplication A(x,y) = exp(-P)
	// between convolution passes in the texture-space pipeline.  It
	// is NOT added to the dermis volume absorption.  In our BSSRDF
	// implementation, this inter-layer absorption is omitted; the
	// epidermis already absorbs melanin volumetrically, and the
	// Fresnel boundary coupling in the multipole layer stacking
	// handles the inter-layer transmission.
	const Scalar sigma_a_derm = C_hd * abs_blood
		+ (1.0 - C_hd) * baseline;

	//----------------------------------------------------------
	// Reduced scattering
	//----------------------------------------------------------

	const Scalar sigma_sp_epi = ComputeEpidermisScattering( nm );

	// Paper: dermis scattering = epidermis / 2.
	const Scalar sigma_sp_derm = sigma_sp_epi * 0.5;

	//----------------------------------------------------------
	// Fill LayerParams
	//----------------------------------------------------------

	// Layer 0: Epidermis
	layers_out[0].sigma_a = sigma_a_epi;
	layers_out[0].sigma_sp = sigma_sp_epi;
	layers_out[0].thickness = thickness_epi;
	layers_out[0].ior = ior_epi;
	ComputeLayerDerivedParams( layers_out[0] );

	// Layer 1: Dermis (semi-infinite, use large thickness)
	layers_out[1].sigma_a = sigma_a_derm;
	layers_out[1].sigma_sp = sigma_sp_derm;
	layers_out[1].thickness = 1.0;			// 1 cm — effectively semi-infinite for skin
	layers_out[1].ior = ior_derm;
	ComputeLayerDerivedParams( layers_out[1] );
}

//=============================================================
// Multipole precomputation pipeline
//=============================================================

void DonnerJensenSkinDiffusionProfile::PrecomputeProfileAtWavelength(
	const Scalar nm,
	const RayIntersectionGeometric& ri,
	GaussianTerm terms_out[K_TERMS],
	Scalar& total_weight_out,
	Scalar cdf_out[K_TERMS]
	)
{
	// Get per-layer optical properties at this wavelength
	LayerParams layers[2];
	ComputePerLayerCoefficients( nm, ri, layers );

	//=============================================================
	// Hankel-domain Sum-of-Gaussians fitting.
	//
	// This follows the paper's approach (Section 4.3, Eq. 4):
	// 1. Compute composite R_tilde(s) via multipole layer stacking
	//    in the Hankel domain (no inverse transform needed).
	// 2. Fit R_tilde(s) ≈ Σ c_k · exp(-σ_k² · s² / 2) directly
	//    in the Hankel domain using NNLS.
	// 3. Each Gaussian maps to the spatial domain as:
	//    Rd_k(r) = (c_k / 2πσ_k²) · exp(-r² / 2σ_k²)
	//    which is sampled via a Rayleigh distribution.
	//
	// This avoids the inverse Hankel transform entirely, since the
	// fit is performed where the profile is smooth (frequency domain).
	//=============================================================

	const LayerParams& epi = layers[0];
	const LayerParams& derm = layers[1];

	// --- Compute composite reflectance in Hankel domain ---
	static const int N_FREQ = 512;
	static const int N_MULTIPOLE = 20;

	HankelGrid grid;
	grid.Create( 0.01, 1e5, N_FREQ );

	double composite_R[N_FREQ];
	ComputeCompositeProfileHankel( layers, 2, grid.s, N_FREQ, N_MULTIPOLE, composite_R );

	// --- Choose Gaussian variances ---
	// Span from the near-field (epidermis, high sigma_tr → small σ)
	// to the far-field (dermis, lower sigma_tr → larger σ).
	// σ_min ~ 1/sigma_tr_max captures highest-frequency features.
	// σ_max ~ 3/sigma_tr_min captures broadest spatial spread.
	const double sigma_tr_max = (epi.sigma_tr > derm.sigma_tr) ? epi.sigma_tr : derm.sigma_tr;
	const double sigma_tr_min = (epi.sigma_tr < derm.sigma_tr) ? epi.sigma_tr : derm.sigma_tr;

	double sigma_min = 0.5 / sigma_tr_max;
	double sigma_max = 3.0 / sigma_tr_min;

	if( sigma_min < 1e-5 ) sigma_min = 1e-5;
	if( sigma_max < sigma_min * 2.0 ) sigma_max = sigma_min * 100.0;

	double variances[K_TERMS];
	const double log_smin = log( sigma_min );
	const double log_smax = log( sigma_max );
	const double log_sstep = (K_TERMS > 1) ? (log_smax - log_smin) / (K_TERMS - 1) : 0;

	for( int k = 0; k < K_TERMS; k++ )
	{
		const double sigma_k = exp( log_smin + k * log_sstep );
		variances[k] = sigma_k * sigma_k;
	}

	// --- NNLS fit in Hankel domain ---
	// R_tilde(s_i) ≈ Σ c_k · exp(-variance_k · s_i² / 2)
	// A[i][k] = exp(-variance_k · s_i² / 2)
	// Solve A·c = R_tilde for c ≥ 0
	{
		static const int MAX_K = 16;
		double AtA[MAX_K][MAX_K];
		double Aty[MAX_K];
		memset( AtA, 0, sizeof(AtA) );
		memset( Aty, 0, sizeof(Aty) );

		for( int i = 0; i < N_FREQ; i++ )
		{
			const double si2 = grid.s[i] * grid.s[i];
			const double yi = composite_R[i];

			double Ai[MAX_K];
			for( int k = 0; k < K_TERMS; k++ )
				Ai[k] = exp( -variances[k] * si2 * 0.5 );

			for( int k = 0; k < K_TERMS; k++ )
			{
				Aty[k] += Ai[k] * yi;
				for( int j = 0; j < K_TERMS; j++ )
					AtA[k][j] += Ai[k] * Ai[j];
			}
		}

		// NNLS: Lawson & Hanson active-set method (same as SumOfExponentialsFit.h)
		double w[MAX_K];
		bool passive[MAX_K];
		memset( w, 0, sizeof(w) );
		memset( passive, 0, sizeof(passive) );

		const int MAX_ITER = K_TERMS * 3 + 30;

		for( int iter = 0; iter < MAX_ITER; iter++ )
		{
			double grad[MAX_K];
			for( int k = 0; k < K_TERMS; k++ )
			{
				grad[k] = -Aty[k];
				for( int j = 0; j < K_TERMS; j++ )
					grad[k] += AtA[k][j] * w[j];
			}

			int best = -1;
			double bestGrad = -1e-12;
			for( int k = 0; k < K_TERMS; k++ )
			{
				if( !passive[k] && grad[k] < bestGrad )
				{
					bestGrad = grad[k];
					best = k;
				}
			}
			if( best < 0 ) break;

			passive[best] = true;

			for( int inner = 0; inner < MAX_ITER; inner++ )
			{
				int passiveIdx[MAX_K];
				int nPassive = 0;
				for( int k = 0; k < K_TERMS; k++ )
					if( passive[k] ) passiveIdx[nPassive++] = k;
				if( nPassive == 0 ) break;

				// Cholesky solve
				double subAtA[MAX_K][MAX_K], subRhs[MAX_K], L[MAX_K][MAX_K];
				for( int a = 0; a < nPassive; a++ )
				{
					subRhs[a] = Aty[passiveIdx[a]];
					for( int b = 0; b < nPassive; b++ )
						subAtA[a][b] = AtA[passiveIdx[a]][passiveIdx[b]];
				}
				memset( L, 0, sizeof(L) );
				for( int a = 0; a < nPassive; a++ )
				{
					double sum = 0;
					for( int p = 0; p < a; p++ ) sum += L[a][p] * L[a][p];
					double diag = subAtA[a][a] - sum;
					L[a][a] = (diag > 1e-30) ? sqrt(diag) : 1e-15;
					for( int b = a+1; b < nPassive; b++ )
					{
						double s = 0;
						for( int p = 0; p < a; p++ ) s += L[b][p] * L[a][p];
						L[b][a] = (subAtA[b][a] - s) / L[a][a];
					}
				}

				double z[MAX_K];
				for( int a = 0; a < nPassive; a++ )
				{
					double s = 0;
					for( int p = 0; p < a; p++ ) s += L[a][p] * z[p];
					z[a] = (subRhs[a] - s) / L[a][a];
				}

				double wSub[MAX_K];
				for( int a = nPassive-1; a >= 0; a-- )
				{
					double s = 0;
					for( int p = a+1; p < nPassive; p++ ) s += L[p][a] * wSub[p];
					wSub[a] = (z[a] - s) / L[a][a];
				}

				bool allPositive = true;
				for( int a = 0; a < nPassive; a++ )
					if( wSub[a] <= 0 ) { allPositive = false; break; }

				if( allPositive )
				{
					for( int a = 0; a < nPassive; a++ )
						w[passiveIdx[a]] = wSub[a];
					break;
				}

				double alpha = 1.0;
				for( int a = 0; a < nPassive; a++ )
				{
					if( wSub[a] <= 0 )
					{
						double t = w[passiveIdx[a]] / (w[passiveIdx[a]] - wSub[a]);
						if( t < alpha ) alpha = t;
					}
				}
				for( int a = 0; a < nPassive; a++ )
				{
					int k = passiveIdx[a];
					w[k] = w[k] + alpha * (wSub[a] - w[k]);
					if( w[k] < 1e-30 ) { w[k] = 0; passive[k] = false; }
				}
			}
		}

		for( int k = 0; k < K_TERMS; k++ )
		{
			terms_out[k].weight = (w[k] > 0) ? w[k] : 0;
			terms_out[k].variance = variances[k];
		}
	}

	// --- Build CDF for mixture sampling ---
	// Each term's weight c_k is its integrated mass (total reflectance).
	total_weight_out = 0;
	for( int k = 0; k < K_TERMS; k++ )
		total_weight_out += terms_out[k].weight;

	double cumulative = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		cumulative += terms_out[k].weight;
		cdf_out[k] = (total_weight_out > 1e-30) ? (cumulative / total_weight_out) : 0;
	}
}

void DonnerJensenSkinDiffusionProfile::PrecomputeProfiles()
{
	Ray dummyRay( Point3(0,0,0), Vector3(0,0,1) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
	ri.ptIntersection = Point3(0,0,0);
	ri.vNormal = Vector3(0,1,0);

	m_max_variance = 0;

	// Precompute RGB profiles
	for( int c = 0; c < NUM_RGB; c++ )
	{
		PrecomputeProfileAtWavelength(
			ms_rgb_wavelengths[c], ri,
			m_rgb_terms[c], m_rgb_total_weight[c], m_rgb_cdf[c] );

		for( int k = 0; k < K_TERMS; k++ )
		{
			if( m_rgb_terms[c][k].weight > 1e-20 && m_rgb_terms[c][k].variance > m_max_variance )
				m_max_variance = m_rgb_terms[c][k].variance;
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
			if( m_spectral_terms[w][k].weight > 1e-20 && m_spectral_terms[w][k].variance > m_max_variance )
				m_max_variance = m_spectral_terms[w][k].variance;
		}
	}

	if( m_max_variance < 1e-10 ) m_max_variance = 0.01;

	GlobalLog()->PrintEx( eLog_Info,
		"DonnerJensenSkinDiffusionProfile: SoG precomputation complete (max_sigma=%.4f cm)",
		sqrt(m_max_variance) );
}

//=============================================================
// Sum-of-Gaussians evaluation
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::EvaluateSumOfGaussians(
	const GaussianTerm terms[K_TERMS],
	const Scalar r
	)
{
	// Rd(r) = Σ (c_k / (2π σ_k²)) · exp(-r² / (2 σ_k²))
	Scalar result = 0;
	const Scalar r2 = r * r;

	for( int k = 0; k < K_TERMS; k++ )
	{
		if( terms[k].weight > 1e-30 && terms[k].variance > 1e-30 )
		{
			result += (terms[k].weight / (2.0 * PI * terms[k].variance)) *
				exp( -r2 / (2.0 * terms[k].variance) );
		}
	}

	return result;
}

//=============================================================
// Profile evaluation
//=============================================================

RISEPel DonnerJensenSkinDiffusionProfile::EvaluateProfile(
	const Scalar r,
	const RayIntersectionGeometric& ri
	) const
{
	RISEPel result;
	for( int c = 0; c < NUM_RGB; c++ )
		result[c] = EvaluateSumOfGaussians( m_rgb_terms[c], r );
	return result;
}

Scalar DonnerJensenSkinDiffusionProfile::EvaluateProfileNM(
	const Scalar r,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	if( nm <= ms_spectral_wavelengths[0] )
		return EvaluateSumOfGaussians( m_spectral_terms[0], r );

	if( nm >= ms_spectral_wavelengths[NUM_SPECTRAL - 1] )
		return EvaluateSumOfGaussians( m_spectral_terms[NUM_SPECTRAL - 1], r );

	for( int w = 0; w < NUM_SPECTRAL - 1; w++ )
	{
		if( nm >= ms_spectral_wavelengths[w] && nm < ms_spectral_wavelengths[w + 1] )
		{
			const Scalar t = (nm - ms_spectral_wavelengths[w]) /
				(ms_spectral_wavelengths[w + 1] - ms_spectral_wavelengths[w]);
			return EvaluateSumOfGaussians( m_spectral_terms[w], r ) * (1.0 - t)
				 + EvaluateSumOfGaussians( m_spectral_terms[w + 1], r ) * t;
		}
	}

	return EvaluateSumOfGaussians( m_spectral_terms[NUM_SPECTRAL / 2], r );
}

//=============================================================
// Importance sampling (Rayleigh mixture)
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::SampleRadius(
	const Scalar u,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const GaussianTerm* terms = m_rgb_terms[channel];
	const Scalar* cdf = m_rgb_cdf[channel];
	const Scalar totalW = m_rgb_total_weight[channel];

	if( totalW < 1e-30 ) return 0.0;

	// Select Gaussian component via CDF
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

	// Sample Rayleigh distribution: r = σ · sqrt(-2 · ln(1-u))
	const Scalar variance = terms[k].variance;
	if( variance < 1e-30 ) return 0.0;

	const Scalar one_minus_u = (u_remapped > 0.999999) ? 1e-6 : (1.0 - u_remapped);
	return sqrt( -2.0 * variance * log( one_minus_u ) );
}

Scalar DonnerJensenSkinDiffusionProfile::PdfRadius(
	const Scalar r,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const GaussianTerm* terms = m_rgb_terms[channel];
	const Scalar totalW = m_rgb_total_weight[channel];

	if( totalW < 1e-30 ) return 0.0;

	// Mixture PDF: Σ (c_k / W) · (r / σ_k²) · exp(-r² / (2 σ_k²))
	const Scalar r2 = r * r;
	Scalar pdf = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		if( terms[k].weight > 1e-30 && terms[k].variance > 1e-30 )
		{
			pdf += (terms[k].weight / totalW) *
				(r / terms[k].variance) * exp( -r2 / (2.0 * terms[k].variance) );
		}
	}

	return pdf;
}

//=============================================================
// Fresnel and IOR
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::FresnelTransmission(
	const Scalar cosTheta,
	const RayIntersectionGeometric& ri
	) const
{
	const Scalar eta = pnt_ior_epidermis.GetColor( ri )[0];
	return 1.0 - SchlickFresnel( fabs(cosTheta), eta );
}

Scalar DonnerJensenSkinDiffusionProfile::GetIOR(
	const RayIntersectionGeometric& ri
	) const
{
	return pnt_ior_epidermis.GetColor( ri )[0];
}

//=============================================================
// ISubSurfaceExtinctionFunction compatibility
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::GetMaximumDistanceForError(
	const Scalar error
	) const
{
	if( error <= 0 ) return RISE_INFINITY;
	// For a Gaussian with variance σ², exp(-r²/(2σ²)) = error
	// → r = σ · sqrt(-2 · ln(error))
	return sqrt( -2.0 * m_max_variance * log(error) );
}

RISEPel DonnerJensenSkinDiffusionProfile::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	return RISEPel( 0, 0, 0 );
}
