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
#include "BioSpecDiffusionProfile.h"
#include "BioSpecSkinData.h"
#include "MultipoleDiffusion.h"
#include "../Utilities/SumOfExponentialsFit.h"
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
  m_min_rate( 1.0 )
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
	// Reduced scattering for skin epidermis.
	// The paper's Eq. 13 uses a Mie+Rayleigh decomposition, but the
	// printed coefficients produce values ~10x too low compared to
	// measured skin data (Jacques 1998, Bashkatov 2005).  We use the
	// Bashkatov 2005 power-law fit instead, which matches the measured
	// range of 40-75 cm^-1 at 500nm and is the same formula used by the
	// existing BioSpec skin model.
	//
	// sigma_sp(lambda) = 73.7 * (lambda/500)^(-2.33)   [cm^-1]
	return 73.7 * pow( nm / 500.0, -2.33 );
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
	// The OMLC data is extinction in cm^-1/(mg/ml).  To get the
	// absorption coefficient of a melanosome, multiply by the melanin
	// concentration within the melanosome (typical values from medical
	// literature: 80 mg/ml for eumelanin, 12 mg/ml for pheomelanin).
	// C_m then acts as the volume fraction of melanosomes.
	static const Scalar CONC_EUMELANIN = 80.0;		// mg/ml in melanosome
	static const Scalar CONC_PHEOMELANIN = 12.0;	// mg/ml in melanosome

	const Scalar abs_eumelanin = pEumelaninExt->Evaluate( nm ) * CONC_EUMELANIN;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate( nm ) * CONC_PHEOMELANIN;
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

	// sigma_a_derm = C_hd * abs_blood + (1 - C_hd) * baseline
	//
	// DEVIATION: The paper's 17.5% melanin leakage (Eq. 7-8) is NOT
	// added to dermis sigma_a here.  It is instead applied as a
	// boundary attenuation factor A^2 = exp(-2*tau) in the composite
	// profile construction (see PrecomputeProfileAtWavelength, step 3).
	// Adding it as volume absorption would triple dermis sigma_a and
	// halve the diffusion distance, producing skin far too dark.
	const Scalar sigma_a_derm = C_hd * abs_blood
		+ (1.0 - C_hd) * baseline;

	//----------------------------------------------------------
	// Reduced scattering
	//----------------------------------------------------------

	const Scalar sigma_sp_epi = ComputeEpidermisScattering( nm );

	// DEVIATION: The paper says dermis scattering = epidermis / 2.
	// With the Bashkatov epidermis formula this gives ~30 cm^-1 at
	// 550nm.  We instead use Rayleigh scattering from collagen
	// fibers (ComputeBeta, Jacques 1996, 5um fibers at 21% volume
	// fraction), which gives ~93 cm^-1 at 550nm.  This matches
	// measured dermal scattering and is consistent with BioSpec.
	const Scalar lambda_cm = nm * 1e-7;
	const Scalar sigma_sp_derm = BioSpecDiffusionProfile::ComputeBeta( lambda_cm, ior_derm );

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
	ExponentialTerm terms_out[K_TERMS],
	Scalar& total_weight_out,
	Scalar cdf_out[K_TERMS]
	)
{
	// Get per-layer optical properties at this wavelength
	LayerParams layers[2];
	ComputePerLayerCoefficients( nm, ri, layers );

	//=============================================================
	// DEVIATION: Spatial-domain two-layer profile (instead of
	// Hankel-domain multipole + inverse transform).
	//
	// The paper computes the composite profile R_12(r) by stacking
	// layer profiles in the Hankel (spatial frequency) domain via
	// adding-doubling, then inverse-transforming to the spatial
	// domain (Donner & Jensen 2005).  The numerical Hankel inverse
	// transform produces severe J0 Bessel oscillation artifacts for
	// the thin 2-layer epidermis+dermis configuration: the profile
	// tail is inflated by orders of magnitude, corrupting the
	// exponential fit and creating visible rendering artifacts.
	//
	// Instead, we evaluate the profile directly in the spatial
	// domain:
	//   - Epidermis: spatial-domain multipole (Donner 2005 image
	//     sources evaluated at each radius r, no Hankel transform)
	//   - Dermis: Jensen 2001 dipole (semi-infinite, analytical)
	//   - Composite: R(r) = R_epi(r) + T_epi^2 * Ft * A^2 * R_derm(r)
	//
	// The convolution T_epi * R_derm * T_epi from the adding-doubling
	// is approximated by scaling R_derm with the scalar T_epi_total.
	// This is valid because the thin epidermis (~1-2 mfp) has a
	// transmittance profile sharply peaked at r~0, so convolving
	// with it is nearly the identity operation.
	//=============================================================

	const LayerParams& epi = layers[0];
	const LayerParams& derm = layers[1];

	static const int N_RADIAL = 256;
	static const int N_MULTIPOLE = 20;

	double r_samples[N_RADIAL];
	double Rd_samples[N_RADIAL];
	double R_epi_samples[N_RADIAL];
	double T_epi_samples[N_RADIAL];
	double R_derm_samples[N_RADIAL];

	const double r_min = 1e-5;		// cm
	const double r_max = 5.0;		// cm
	const double log_r_min = log( r_min );
	const double log_r_max = log( r_max );
	const double log_r_step = (log_r_max - log_r_min) / (N_RADIAL - 1);

	for( int i = 0; i < N_RADIAL; i++ )
		r_samples[i] = exp( log_r_min + i * log_r_step );

	//----------------------------------------------------------
	// 1. Epidermis: spatial-domain multipole for finite slab
	//----------------------------------------------------------

	const bool epi_is_thin = (epi.thickness < epi.z_r);

	if( epi_is_thin )
	{
		// Sub-mean-free-path slab: no diffuse scattering, Beer-Lambert only
		for( int i = 0; i < N_RADIAL; i++ )
		{
			R_epi_samples[i] = 0;
			T_epi_samples[i] = 0;	// delta function — handled via T_total below
		}
	}
	else
	{
		// Full multipole in spatial domain
		const double prefactor = epi.alpha_prime / (4.0 * PI);
		const double L = epi.slab_period;
		const double d_epi = epi.thickness;

		for( int i = 0; i < N_RADIAL; i++ )
		{
			const double r = r_samples[i];
			const double r2 = r * r;
			double R_sum = 0;
			double T_sum = 0;

			for( int j = -N_MULTIPOLE; j <= N_MULTIPOLE; j++ )
			{
				const double offset = j * L;
				const double z_real = epi.z_r + offset;		// charge +1
				const double z_virt = -epi.z_v + offset;	// charge -1

				// Reflectance at z=0: contribution = Q * sign(z_s) * |z_s| * kernel(r, z_s)
				{
					// Real source
					const double dz = fabs( z_real );
					const double dist = sqrt( r2 + dz * dz );
					const double flux = dz * (1.0 + epi.sigma_tr * dist) *
						exp( -epi.sigma_tr * dist ) / (dist * dist * dist);
					R_sum += (z_real >= 0 ? 1.0 : -1.0) * flux;

					// Virtual source (Q=-1)
					const double dz_v = fabs( z_virt );
					const double dist_v = sqrt( r2 + dz_v * dz_v );
					const double flux_v = dz_v * (1.0 + epi.sigma_tr * dist_v) *
						exp( -epi.sigma_tr * dist_v ) / (dist_v * dist_v * dist_v);
					R_sum -= (z_virt >= 0 ? 1.0 : -1.0) * flux_v;
				}

				// Transmittance at z=d_epi
				{
					const double dz_r = fabs( d_epi - z_real );
					const double dist_r = sqrt( r2 + dz_r * dz_r );
					const double flux_r = dz_r * (1.0 + epi.sigma_tr * dist_r) *
						exp( -epi.sigma_tr * dist_r ) / (dist_r * dist_r * dist_r);
					T_sum += ((d_epi - z_real) >= 0 ? 1.0 : -1.0) * flux_r;

					const double dz_v = fabs( d_epi - z_virt );
					const double dist_v = sqrt( r2 + dz_v * dz_v );
					const double flux_v = dz_v * (1.0 + epi.sigma_tr * dist_v) *
						exp( -epi.sigma_tr * dist_v ) / (dist_v * dist_v * dist_v);
					T_sum -= ((d_epi - z_virt) >= 0 ? 1.0 : -1.0) * flux_v;
				}

				// Early exit
				if( j > 0 && exp( -epi.sigma_tr * (epi.z_r + j * L) ) < 1e-15 )
					break;
			}

			R_epi_samples[i] = prefactor * R_sum;
			T_epi_samples[i] = prefactor * T_sum;
			if( R_epi_samples[i] < 0 ) R_epi_samples[i] = 0;
			if( T_epi_samples[i] < 0 ) T_epi_samples[i] = 0;
		}
	}

	//----------------------------------------------------------
	// 2. Integrate epidermis transmittance for total T_epi
	//----------------------------------------------------------

	double T_epi_total;
	if( epi_is_thin )
	{
		T_epi_total = exp( -epi.sigma_a * epi.thickness );
	}
	else
	{
		// Numerical integration: T_total = integral T_epi(r) * 2*pi*r dr
		T_epi_total = 0;
		for( int i = 0; i < N_RADIAL - 1; i++ )
		{
			const double dr = r_samples[i + 1] - r_samples[i];
			const double avg = 0.5 * (T_epi_samples[i] * r_samples[i] +
				T_epi_samples[i + 1] * r_samples[i + 1]);
			T_epi_total += avg * dr * 2.0 * PI;
		}
		// Clamp to physical range
		if( T_epi_total > 1.0 ) T_epi_total = 1.0;
		if( T_epi_total < 0.0 ) T_epi_total = 0.0;
	}

	//----------------------------------------------------------
	// 3. Fresnel and inter-layer absorption at boundary
	//----------------------------------------------------------

	const double eta_down = epi.ior / derm.ior;
	const double eta_up = derm.ior / epi.ior;
	const double Ft_boundary = (1.0 - ComputeFdr( eta_down )) * (1.0 - ComputeFdr( eta_up ));

	// Inter-layer absorbing boundary (Donner 2008 Eq. 7-8).
	// The paper places 17.5% of the epidermal melanin absorption in
	// a thin absorbing layer between the scattering layers.  This is
	// a multiplicative attenuation A = exp(-tau) applied to light
	// crossing the boundary in each direction.
	//
	// tau = 0.175 * (melanin contribution to sigma_a_epi) * d_epi
	//
	// We extract the melanin component from the total epidermis
	// absorption by subtracting the baseline and other chromophores.
	// As an approximation, we use: melanin_sigma_a ≈ C_m * abs_melanin
	// which was the dominant term added to sigma_a_epi.
	const Scalar C_m = pnt_melanin_fraction.GetColor( ri )[0];
	const Scalar beta_m = pnt_melanin_blend.GetColor( ri )[0];

	static const Scalar CONC_EU = 80.0;
	static const Scalar CONC_PH = 12.0;
	const Scalar abs_eu_local = pEumelaninExt->Evaluate( nm ) * CONC_EU;
	const Scalar abs_ph_local = pPheomelaninExt->Evaluate( nm ) * CONC_PH;
	const Scalar abs_melanin_local = beta_m * abs_eu_local + (1.0 - beta_m) * abs_ph_local;

	const double tau_boundary = 0.175 * C_m * abs_melanin_local * epi.thickness;
	const double A_boundary = exp( -tau_boundary );

	//----------------------------------------------------------
	// 4. Dermis: Jensen 2001 dipole (semi-infinite medium)
	//----------------------------------------------------------

	for( int i = 0; i < N_RADIAL; i++ )
	{
		const double r = r_samples[i];
		const double d_r = sqrt( r * r + derm.z_r * derm.z_r );
		const double d_v = sqrt( r * r + derm.z_v * derm.z_v );

		const double term_r = derm.z_r * (1.0 + derm.sigma_tr * d_r) *
			exp( -derm.sigma_tr * d_r ) / (d_r * d_r * d_r);
		const double term_v = derm.z_v * (1.0 + derm.sigma_tr * d_v) *
			exp( -derm.sigma_tr * d_v ) / (d_v * d_v * d_v);

		R_derm_samples[i] = (derm.alpha_prime / (4.0 * PI)) * (term_r + term_v);
		if( R_derm_samples[i] < 0 ) R_derm_samples[i] = 0;
	}

	//----------------------------------------------------------
	// 5. Composite: R(r) = R_epi(r) + T_epi^2 * Ft * A^2 * R_derm(r)
	//    A^2 because light crosses the absorbing boundary twice
	//    (down into dermis, then back up out of dermis)
	//----------------------------------------------------------

	const double T_factor = T_epi_total * T_epi_total * Ft_boundary * A_boundary * A_boundary;

	for( int i = 0; i < N_RADIAL; i++ )
	{
		Rd_samples[i] = R_epi_samples[i] + T_factor * R_derm_samples[i];
		if( Rd_samples[i] < 0 ) Rd_samples[i] = 0;
	}

	//----------------------------------------------------------
	// 6. Determine rate range for exponential fitting
	//----------------------------------------------------------

	// The composite profile has two distinct decay scales: a sharp
	// peak from the epidermis (rate ~ epi.sigma_tr) and a broad tail
	// from the dermis (rate ~ derm.sigma_tr).  The rate range must
	// span both to avoid smearing the near-field epidermis energy
	// into the slow dermis rate (which creates bright SSS artifacts).
	double min_rate = derm.sigma_tr * 0.5;
	double max_rate = (epi.sigma_tr > derm.sigma_tr * 2.0) ? epi.sigma_tr : derm.sigma_tr * 2.0;

	if( min_rate < 0.1 ) min_rate = 0.1;
	if( max_rate < min_rate * 2.0 ) max_rate = min_rate * 10.0;

	// Place K rates on a geometric grid
	double rates[K_TERMS];
	const double log_min_rate = log( min_rate );
	const double log_max_rate = log( max_rate );
	const double log_rate_step = (K_TERMS > 1) ? (log_max_rate - log_min_rate) / (K_TERMS - 1) : 0;

	for( int k = 0; k < K_TERMS; k++ )
	{
		rates[k] = exp( log_min_rate + k * log_rate_step );
	}

	// Fit sum of exponentials via NNLS
	FitSumOfExponentials( r_samples, Rd_samples, N_RADIAL, rates, K_TERMS, terms_out );

	// Compute total integrated mass and CDF for mixture sampling.
	// The fit model is Rd(r)*r = sum w_k exp(-rate_k * r), so the integral
	// of each component is w_k / rate_k.
	total_weight_out = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		const double mass_k = (terms_out[k].rate > 1e-30)
			? terms_out[k].weight / terms_out[k].rate : 0;
		total_weight_out += mass_k;
	}

	double cumulative = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		const double mass_k = (terms_out[k].rate > 1e-30)
			? terms_out[k].weight / terms_out[k].rate : 0;
		cumulative += mass_k;
		cdf_out[k] = (total_weight_out > 1e-30) ? (cumulative / total_weight_out) : 0;
	}
}

void DonnerJensenSkinDiffusionProfile::PrecomputeProfiles()
{
	// Create a synthetic RayIntersectionGeometric for painter evaluation.
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
		"DonnerJensenSkinDiffusionProfile: Multipole precomputation complete (min_rate=%.3f cm^-1)",
		m_min_rate );
}

//=============================================================
// Sum-of-exponentials evaluation
//=============================================================

Scalar DonnerJensenSkinDiffusionProfile::EvaluateSumOfExp(
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

RISEPel DonnerJensenSkinDiffusionProfile::EvaluateProfile(
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

Scalar DonnerJensenSkinDiffusionProfile::EvaluateProfileNM(
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

Scalar DonnerJensenSkinDiffusionProfile::SampleRadius(
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

Scalar DonnerJensenSkinDiffusionProfile::PdfRadius(
	const Scalar r,
	const int channel,
	const RayIntersectionGeometric& ri
	) const
{
	const ExponentialTerm* terms = m_rgb_terms[channel];
	const Scalar totalW = m_rgb_total_weight[channel];

	if( totalW < 1e-30 ) return 0.0;

	// Mixture PDF: sum_k (mass_k / W) * rate_k * exp(-rate_k * r)
	Scalar pdf = 0;
	for( int k = 0; k < K_TERMS; k++ )
	{
		if( terms[k].weight > 1e-30 && terms[k].rate > 1e-30 )
		{
			const Scalar mass_k = terms[k].weight / terms[k].rate;
			pdf += (mass_k / totalW) * terms[k].rate * exp( -terms[k].rate * r );
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
	// Use epidermis IOR for the outermost scattering boundary
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
	return -log(error) / m_min_rate;
}

RISEPel DonnerJensenSkinDiffusionProfile::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	return RISEPel( 0, 0, 0 );
}
