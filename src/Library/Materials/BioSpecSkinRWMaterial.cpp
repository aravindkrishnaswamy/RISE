//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinRWMaterial.cpp - Implementation of the BioSpec
//  random-walk SSS skin material.
//
//  Stores chromophore extinction lookup tables and biophysical
//  parameters, computing per-wavelength walk coefficients on the
//  fly via GetRandomWalkSSSParamsNM().  See header for overview.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BioSpecSkinRWMaterial.h"
#include "BioSpecSkinData.h"
#include "BioSpecDiffusionProfile.h"
#include "../RISE_API.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

//=============================================================
// BioSpec baseline absorption (same formula as BioSpecDiffusionProfile)
//=============================================================

static Scalar ComputeSkinBaselineAbsorption( const Scalar nm )
{
	return (0.244 + 85.3 * exp( -(nm - 154.0) / 66.2 ));
}

//=============================================================
// Constructor
//=============================================================

BioSpecSkinRWMaterial::BioSpecSkinRWMaterial(
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
	) :
  iorPainter( ior_SC_ ),
  surfaceRoughness( roughness ),
  m_maxBounces( maxBounces ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 ),
  pBilirubinExt( 0 ),
  pBetaCaroteneExt( 0 )
{
	iorPainter.addref();

	// Surface BSDF/SPF: GGX microfacet with SC IOR.
	pBSDF = new SubSurfaceScatteringBSDF( ior_SC_, ior_SC_, ior_SC_, 0.0, roughness );
	GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

	pSPF = new SubSurfaceScatteringSPF( ior_SC_, ior_SC_, ior_SC_, 0.0, roughness, true );
	GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

	//=============================================================
	// Evaluate painters at a dummy intersection
	//=============================================================

	RayIntersectionGeometric dummyRI(
		Ray( Point3(0,0,0), Vector3(0,1,0) ),
		nullRasterizerState );
	dummyRI.bHit = true;
	dummyRI.ptIntersection = Point3( 0, 0, 0 );
	dummyRI.vNormal = Vector3( 0, 1, 0 );
	dummyRI.onb.CreateFromW( dummyRI.vNormal );

	m_thickness_SC        = thickness_SC_.GetColor( dummyRI )[0];
	m_thickness_epidermis = thickness_epidermis_.GetColor( dummyRI )[0];
	m_thickness_papillary = thickness_papillary_dermis_.GetColor( dummyRI )[0];
	m_thickness_reticular = thickness_reticular_dermis_.GetColor( dummyRI )[0];
	m_total_thickness     = m_thickness_SC + m_thickness_epidermis
	                      + m_thickness_papillary + m_thickness_reticular;

	m_ior_SC              = ior_SC_.GetColor( dummyRI )[0];
	m_ior_papillary       = ior_papillary_dermis_.GetColor( dummyRI )[0];
	m_ior_reticular       = ior_reticular_dermis_.GetColor( dummyRI )[0];

	m_conc_eumelanin      = concentration_eumelanin_.GetColor( dummyRI )[0];
	m_conc_pheomelanin    = concentration_pheomelanin_.GetColor( dummyRI )[0];
	m_melanosomes         = melanosomes_in_epidermis_.GetColor( dummyRI )[0];

	m_hb_ratio            = hb_ratio_.GetColor( dummyRI )[0];
	m_blood_papillary     = whole_blood_in_papillary_dermis_.GetColor( dummyRI )[0];
	m_blood_reticular     = whole_blood_in_reticular_dermis_.GetColor( dummyRI )[0];
	m_hb_concentration    = SkinData::hb_concen_whole_blood;

	m_bilirubin_conc      = bilirubin_concentration_.GetColor( dummyRI )[0];
	m_carotene_SC         = betacarotene_concentration_SC_.GetColor( dummyRI )[0];
	m_carotene_epidermis  = betacarotene_concentration_epidermis_.GetColor( dummyRI )[0];
	m_carotene_dermis     = betacarotene_concentration_dermis_.GetColor( dummyRI )[0];

	//=============================================================
	// Build chromophore extinction lookup tables
	//=============================================================

	{
		const int count = sizeof( SkinData::omlc_eumelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_eumelanin_wavelengths,
			SkinData::omlc_eumelanin_ext_mgml );
		pEumelaninExt = pFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_pheomelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_pheomelanin_wavelengths,
			SkinData::omlc_pheomelanin_ext_mgml );
		pPheomelaninExt = pFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_prahl_hemoglobin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pOxyFunc = 0;
		IPiecewiseFunction1D* pDeOxyFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pOxyFunc );
		RISE_API_CreatePiecewiseLinearFunction1D( &pDeOxyFunc );
		pOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths,
			SkinData::omlc_prahl_oxyhemoglobin );
		pDeOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths,
			SkinData::omlc_prahl_deoxyhemoglobin );
		pOxyHemoglobinExt = pOxyFunc;
		pDeoxyHemoglobinExt = pDeOxyFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_prahl_bilirubin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_bilirubin_wavelengths,
			SkinData::omlc_prahl_bilirubin );
		pBilirubinExt = pFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_prahl_betacarotene_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_betacarotene_wavelengths,
			SkinData::omlc_prahl_betacarotene );
		pBetaCaroteneExt = pFunc;
	}

	GlobalLog()->PrintEx( eLog_Info,
		"BioSpecSkinRWMaterial: spectral-only mode, ior=%.2f, maxBounces=%u",
		m_ior_SC, m_maxBounces );

	// Log key parameters and sample coefficients for debugging
	GlobalLog()->PrintEx( eLog_Info,
		"  melanosomes=%.4f, eumelanin=%.1f, pheomelanin=%.1f",
		m_melanosomes, m_conc_eumelanin, m_conc_pheomelanin );
	GlobalLog()->PrintEx( eLog_Info,
		"  thickness: SC=%.4f epi=%.4f pap=%.4f ret=%.4f total=%.4f cm",
		m_thickness_SC, m_thickness_epidermis, m_thickness_papillary, m_thickness_reticular, m_total_thickness );
	{
		Scalar sa, ssp, mf;
		ComputeEffectiveCoefficients( 550.0, sa, ssp, mf );
		const Scalar st = sa + ssp;
		GlobalLog()->PrintEx( eLog_Info,
			"  @550nm: sigma_a=%.2f sigma_sp=%.2f sigma_t=%.2f cm^-1  albedo=%.4f  melaninFilter=%.4f",
			sa, ssp, st, st > 0 ? ssp/st : 0, mf );
		ComputeEffectiveCoefficients( 450.0, sa, ssp, mf );
		GlobalLog()->PrintEx( eLog_Info,
			"  @450nm: sigma_a=%.2f sigma_sp=%.2f sigma_t=%.2f cm^-1  albedo=%.4f  melaninFilter=%.4f",
			sa, ssp, sa+ssp, (sa+ssp) > 0 ? ssp/(sa+ssp) : 0, mf );
		ComputeEffectiveCoefficients( 650.0, sa, ssp, mf );
		GlobalLog()->PrintEx( eLog_Info,
			"  @650nm: sigma_a=%.2f sigma_sp=%.2f sigma_t=%.2f cm^-1  albedo=%.4f  melaninFilter=%.4f",
			sa, ssp, sa+ssp, (sa+ssp) > 0 ? ssp/(sa+ssp) : 0, mf );
	}
}

//=============================================================
// Destructor
//=============================================================

BioSpecSkinRWMaterial::~BioSpecSkinRWMaterial()
{
	safe_release( pBSDF );
	safe_release( pSPF );
	iorPainter.release();

	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );
}

//=============================================================
// Per-wavelength coefficient computation
//=============================================================

void BioSpecSkinRWMaterial::ComputeEffectiveCoefficients(
	const Scalar nm,
	Scalar& sigma_a_eff,
	Scalar& sigma_sp_eff,
	Scalar& melaninFilterTransmittance
	) const
{
	const Scalar baseline = ComputeSkinBaselineAbsorption( nm );

	//------------------------------------------------------
	// Per-layer absorption coefficients (cm⁻¹)
	//------------------------------------------------------

	// Stratum corneum
	const Scalar abs_carotene_SC = pBetaCaroteneExt->Evaluate(nm)
		* m_carotene_SC / 537.0 * log(10.0);
	const Scalar sigma_a_SC = abs_carotene_SC + baseline;

	// Epidermis
	const Scalar abs_eumelanin = pEumelaninExt->Evaluate(nm) * m_conc_eumelanin;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate(nm) * m_conc_pheomelanin;
	const Scalar abs_carotene_epi = pBetaCaroteneExt->Evaluate(nm)
		* m_carotene_epidermis / 537.0 * log(10.0);
	const Scalar sigma_a_epidermis =
		(abs_eumelanin + abs_pheomelanin) * m_melanosomes
		+ (abs_carotene_epi + baseline) * (1.0 - m_melanosomes);

	// Dermis (shared hemoglobin/bilirubin/carotene terms)
	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate(nm) * m_hb_concentration)
		/ 66500.0 * log(10.0);
	const Scalar abs_hb = (pDeoxyHemoglobinExt->Evaluate(nm) * m_hb_concentration)
		/ 66500.0 * log(10.0);
	const Scalar abs_bilirubin = pBilirubinExt->Evaluate(nm)
		* m_bilirubin_conc / 585.0 * log(10.0);
	const Scalar abs_carotene_derm = pBetaCaroteneExt->Evaluate(nm)
		* m_carotene_dermis / 537.0 * log(10.0);

	const Scalar blood_abs = abs_hbo2 * m_hb_ratio + abs_hb * (1.0 - m_hb_ratio)
		+ abs_bilirubin + abs_carotene_derm;

	const Scalar sigma_a_papillary =
		blood_abs * m_blood_papillary + baseline * (1.0 - m_blood_papillary);
	const Scalar sigma_a_reticular =
		blood_abs * m_blood_reticular + baseline * (1.0 - m_blood_reticular);

	//------------------------------------------------------
	// Per-layer reduced scattering coefficients (cm⁻¹)
	//
	// BioSpec provides sigma_s' directly.  Under the
	// similarity principle, the walk uses sigma_s' with
	// g=0 for equivalent diffuse transport.
	//------------------------------------------------------

	// SC and epidermis: Mie-like power law
	const Scalar sigma_sp_SC = 73.7 * pow( nm / 500.0, -2.33 );
	const Scalar sigma_sp_epidermis = sigma_sp_SC;

	// Dermis: Rayleigh scattering from collagen fibers
	const Scalar lambda_cm = nm * 1e-7;
	const Scalar sigma_sp_papillary = BioSpecDiffusionProfile::ComputeBeta(
		lambda_cm, m_ior_papillary );
	const Scalar sigma_sp_reticular = BioSpecDiffusionProfile::ComputeBeta(
		lambda_cm, m_ior_reticular );

	//------------------------------------------------------
	// Effective walk coefficients
	//
	// Scattering: thickness-weighted average across all
	// layers.  This is correct because scattering is
	// spatially distributed throughout the skin volume.
	//
	// Absorption: thickness-weighted average of all layers
	// (providing baseline volumetric absorption in the
	// walk), PLUS a boundary filter for the melanin
	// double-pass through SC and epidermis.
	//
	// The boundary filter corrects for the geometric
	// constraint that all photons must transit the thin
	// melanin-rich upper layers on entry AND exit.
	// Thickness-weighted averaging dilutes the epidermis
	// contribution (it is only ~5% of total depth), but
	// physically it absorbs 100% of entering/exiting
	// photons.  The double-pass filter accounts for this
	// under-representation.
	//------------------------------------------------------

	const Scalar sigma_a_layers[4] = {
		sigma_a_SC, sigma_a_epidermis, sigma_a_papillary, sigma_a_reticular
	};
	const Scalar sigma_sp_layers[4] = {
		sigma_sp_SC, sigma_sp_epidermis, sigma_sp_papillary, sigma_sp_reticular
	};
	const Scalar thicknesses[4] = {
		m_thickness_SC, m_thickness_epidermis, m_thickness_papillary, m_thickness_reticular
	};

	sigma_a_eff = 0;
	sigma_sp_eff = 0;
	melaninFilterTransmittance = 1.0;

	if( m_total_thickness > 1e-20 )
	{
		// All layers, thickness-weighted
		for( int layer = 0; layer < 4; layer++ )
		{
			sigma_a_eff  += sigma_a_layers[layer]  * thicknesses[layer];
			sigma_sp_eff += sigma_sp_layers[layer] * thicknesses[layer];
		}
		sigma_a_eff  /= m_total_thickness;
		sigma_sp_eff /= m_total_thickness;

		// Melanin double-pass boundary filter:
		// Light entering the skin passes through SC + epidermis,
		// scatters in the dermis, then exits through epidermis +
		// SC again.  The scattered light inside the dermis has a
		// near-isotropic angular distribution, so its average path
		// length through the thin SC/epidermis slab is 2× the
		// thickness (standard diffuse-slab result).  Combined
		// with the double pass (entry + exit), the effective
		// boundary optical depth is 4× the direct OD.
		const Scalar boundaryOD = 4.0 * (sigma_a_SC * m_thickness_SC
			+ sigma_a_epidermis * m_thickness_epidermis);
		melaninFilterTransmittance = exp( -boundaryOD );
	}
}

//=============================================================
// GetRandomWalkSSSParamsNM
//=============================================================

bool BioSpecSkinRWMaterial::GetRandomWalkSSSParamsNM(
	const Scalar nm,
	RandomWalkSSSParams& params_out
	) const
{
	Scalar sigma_a_cm, sigma_sp_cm, melaninFilter;
	ComputeEffectiveCoefficients( nm, sigma_a_cm, sigma_sp_cm, melaninFilter );

	// Convert cm⁻¹ → m⁻¹ (scene units are meters)
	const Scalar sigma_a = sigma_a_cm * 100.0;
	const Scalar sigma_s = sigma_sp_cm * 100.0;
	const Scalar sigma_t = sigma_a + sigma_s;

	// Pack the scalar value into all 3 RGB channels so that
	// the walk's luminance-derived NM path (which takes
	// 0.2126*R + 0.7152*G + 0.0722*B) yields the correct
	// per-wavelength extinction.
	params_out.sigma_a = RISEPel( sigma_a, sigma_a, sigma_a );
	params_out.sigma_s = RISEPel( sigma_s, sigma_s, sigma_s );
	params_out.sigma_t = RISEPel( sigma_t, sigma_t, sigma_t );
	params_out.g = 0.0;
	params_out.ior = m_ior_SC;
	params_out.maxBounces = m_maxBounces;
	params_out.boundaryFilter = melaninFilter;

	// Limit walk depth to the total BioSpec skin thickness.
	// cm → m (scene units).
	params_out.maxDepth = m_total_thickness * 0.01;

	return true;
}
