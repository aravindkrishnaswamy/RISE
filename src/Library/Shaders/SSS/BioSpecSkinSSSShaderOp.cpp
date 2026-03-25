//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinSSSShaderOp.cpp - Octree-based BSSRDF evaluation for
//  the BioSpec 4-layer skin model.
//
//  See BioSpecSkinSSSShaderOp.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BioSpecSkinSSSShaderOp.h"
#include "../../Materials/BioSpecSkinData.h"
#include "../../Materials/BioSpecDiffusionProfile.h"
#include "../../Materials/MultipoleDiffusion.h"
#include "../../Utilities/HankelTransform.h"
#include "../../Utilities/GeometricUtilities.h"
#include "../../Utilities/stl_utils.h"
#include "../../Sampling/HaltonPoints.h"
#include "../../RISE_API.h"
#include "../../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

//=============================================================
// BioSpecLocalProfile — stack-allocated extinction function
// for thread-safe per-pixel profile lookup.
// (Same pattern as DonnerJensenSkinSSSShaderOp's LocalProfile)
//=============================================================

class BioSpecLocalProfile : public ISubSurfaceExtinctionFunction
{
	const RISEPel*	table;
	Scalar			r2_max;
	Scalar			r2_step;
	Scalar			max_dist;
	static const int TABLE_SIZE = BioSpecSkinSSSShaderOp::TABLE_SIZE;

public:
	BioSpecLocalProfile( const RISEPel* t, Scalar r2m, Scalar r2s, Scalar md )
		: table(t), r2_max(r2m), r2_step(r2s), max_dist(md) {}

	void addref() const {}
	bool release() const { return false; }
	unsigned int refcount() const { return 1; }

	RISEPel ComputeTotalExtinction( const Scalar distance ) const
	{
		const Scalar r2 = distance * distance;
		if( r2 >= r2_max ) return RISEPel( 0, 0, 0 );
		const Scalar t = r2 / r2_step;
		const int idx = (int)t;
		const Scalar frac = t - idx;
		if( idx >= TABLE_SIZE - 1 ) return table[TABLE_SIZE - 1];
		return table[idx] * (1.0 - frac) + table[idx + 1] * frac;
	}

	Scalar GetMaximumDistanceForError( const Scalar error ) const
	{
		return max_dist;
	}
};

//=============================================================
// Static helpers
//=============================================================

Scalar BioSpecSkinSSSShaderOp::ComputeSkinBaselineAbsorption( const Scalar nm )
{
	return (0.244 + 85.3 * exp( -(nm - 154.0) / 66.2 ));
}

//=============================================================
// Per-layer coefficient computation (BioSpec 4-layer model)
//=============================================================

void BioSpecSkinSSSShaderOp::ComputePerLayerCoefficients(
	const Scalar nm,
	const Scalar mel_frac,
	const Scalar blood_pap,
	const Scalar blood_ret,
	LayerParams layers_out[4]
	) const
{
	const Scalar baseline = ComputeSkinBaselineAbsorption( nm );

	// --- Absorption ---

	// Stratum corneum
	const Scalar abs_carotene_SC = pBetaCaroteneExt->Evaluate(nm) * betacarotene_SC / 537.0 * log(10.0);
	const Scalar sigma_a_SC = abs_carotene_SC + baseline;

	// Epidermis
	const Scalar abs_eumelanin = pEumelaninExt->Evaluate(nm) * concentration_eumelanin;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate(nm) * concentration_pheomelanin;
	const Scalar abs_carotene_epi = pBetaCaroteneExt->Evaluate(nm) * betacarotene_epidermis / 537.0 * log(10.0);
	const Scalar sigma_a_epidermis = (abs_eumelanin + abs_pheomelanin) * mel_frac
		+ (abs_carotene_epi + baseline) * (1.0 - mel_frac);

	// Hemoglobin (shared by both dermal layers)
	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate(nm) * hb_concentration) / 66500.0 * log(10.0);
	const Scalar abs_hb = (pDeoxyHemoglobinExt->Evaluate(nm) * hb_concentration) / 66500.0 * log(10.0);
	const Scalar abs_bilirubin = pBilirubinExt->Evaluate(nm) * bilirubin_concentration / 585.0 * log(10.0);
	const Scalar abs_carotene_dermis = pBetaCaroteneExt->Evaluate(nm) * betacarotene_dermis / 537.0 * log(10.0);

	// Papillary dermis
	const Scalar sigma_a_papillary_val =
		(abs_hbo2 * hb_ratio + abs_hb * (1.0 - hb_ratio) + abs_bilirubin + abs_carotene_dermis) * blood_pap
		+ baseline * (1.0 - blood_pap);

	// Reticular dermis
	const Scalar sigma_a_reticular_val =
		(abs_hbo2 * hb_ratio + abs_hb * (1.0 - hb_ratio) + abs_bilirubin + abs_carotene_dermis) * blood_ret
		+ baseline * (1.0 - blood_ret);

	// --- Scattering ---
	// SC/epidermis: Bashkatov 2005
	const Scalar sigma_sp_SC = 73.7 * pow( nm / 500.0, -2.33 );
	const Scalar sigma_sp_epi = sigma_sp_SC;

	// Dermis: Rayleigh scattering from collagen (BioSpec's ComputeBeta)
	const Scalar lambda_cm = nm * 1e-7;
	const Scalar sigma_sp_pap = BioSpecDiffusionProfile::ComputeBeta( lambda_cm, ior_papillary );
	const Scalar sigma_sp_ret = BioSpecDiffusionProfile::ComputeBeta( lambda_cm, ior_reticular );

	// --- Fill layers ---
	layers_out[0].sigma_a = sigma_a_SC;
	layers_out[0].sigma_sp = sigma_sp_SC;
	layers_out[0].thickness = thickness_SC;
	layers_out[0].ior = ior_SC;
	ComputeLayerDerivedParams( layers_out[0] );

	layers_out[1].sigma_a = sigma_a_epidermis;
	layers_out[1].sigma_sp = sigma_sp_epi;
	layers_out[1].thickness = thickness_epidermis;
	layers_out[1].ior = ior_epidermis;
	ComputeLayerDerivedParams( layers_out[1] );

	layers_out[2].sigma_a = sigma_a_papillary_val;
	layers_out[2].sigma_sp = sigma_sp_pap;
	layers_out[2].thickness = thickness_papillary;
	layers_out[2].ior = ior_papillary;
	ComputeLayerDerivedParams( layers_out[2] );

	layers_out[3].sigma_a = sigma_a_reticular_val;
	layers_out[3].sigma_sp = sigma_sp_ret;
	layers_out[3].thickness = thickness_reticular;
	layers_out[3].ior = ior_reticular;
	ComputeLayerDerivedParams( layers_out[3] );
}

//=============================================================
// Profile tabulation (with hybrid Hankel+dipole correction)
//=============================================================

void BioSpecSkinSSSShaderOp::TabulateProfileAtWavelength(
	const Scalar nm,
	const int channel
	)
{
	TabulateProfileAtWavelengthInto( nm, channel,
		melanosomes_in_epidermis, whole_blood_papillary, whole_blood_reticular,
		m_Rd_table, m_table_r2_max, m_table_r2_step );
}

void BioSpecSkinSSSShaderOp::TabulateProfileAtWavelengthInto(
	const Scalar nm,
	const int channel,
	const Scalar mel_frac,
	const Scalar blood_pap,
	const Scalar blood_ret,
	RISEPel* table_out,
	Scalar table_r2_max_val,
	Scalar table_r2_step_val
	) const
{
	LayerParams layers[4];
	ComputePerLayerCoefficients( nm, mel_frac, blood_pap, blood_ret, layers );

	// --- Hankel-domain composite profile (4-layer stack) ---
	static const int N_FREQ = 512;
	static const int N_MULTIPOLE = 20;

	HankelGrid grid;
	grid.Create( 0.01, 1e5, N_FREQ );

	double composite_R[N_FREQ];
	ComputeCompositeProfileHankel( layers, 4, grid.s, N_FREQ, N_MULTIPOLE, composite_R );

	// --- Reference: reticular dermis single-layer dipole ---
	// Scaled by Beer-Lambert transmission through SC + epidermis + papillary
	const LayerParams& ret = layers[3];

	const double T_upper = exp(
		-layers[0].sigma_a * layers[0].thickness
		- layers[1].sigma_a * layers[1].thickness
		- layers[2].sigma_a * layers[2].thickness );

	// Fresnel coupling through all internal boundaries
	double Ft_composite = 1.0;
	for( int L = 0; L < 3; L++ )
	{
		const double eta_down = layers[L].ior / layers[L+1].ior;
		const double eta_up = layers[L+1].ior / layers[L].ior;
		Ft_composite *= (1.0 - ComputeFdr(eta_down)) * (1.0 - ComputeFdr(eta_up));
	}

	const double reference_scale = T_upper * Ft_composite;

	// --- Tabulate with hybrid correction ---
	for( int i = 0; i < TABLE_SIZE; i++ )
	{
		const double r2 = (i + 0.5) * table_r2_step_val;
		const double r = sqrt( r2 );

		// Hankel inverse transform
		double Rd_hankel = grid.InverseTransform( composite_R, r ) / (2.0 * PI);
		if( Rd_hankel < 0 ) Rd_hankel = 0;

		// Reference: reticular dermis dipole
		const double d_r = sqrt( r2 + ret.z_r * ret.z_r );
		const double d_v = sqrt( r2 + ret.z_v * ret.z_v );
		const double term_r = ret.z_r * (1.0 + ret.sigma_tr * d_r) *
			exp( -ret.sigma_tr * d_r ) / (d_r * d_r * d_r);
		const double term_v = ret.z_v * (1.0 + ret.sigma_tr * d_v) *
			exp( -ret.sigma_tr * d_v ) / (d_v * d_v * d_v);
		double Rd_ref = reference_scale * (ret.alpha_prime / (4.0 * PI)) * (term_r + term_v);
		if( Rd_ref < 0 ) Rd_ref = 0;

		// Hybrid: near-field uses max(Hankel, reference); far-field uses reference
		double Rd_final;
		const double r_crossover = 2.0 / ret.sigma_tr;
		if( r < r_crossover )
			Rd_final = (Rd_hankel > Rd_ref) ? Rd_hankel : Rd_ref;
		else
			Rd_final = Rd_ref;

		table_out[i][channel] = Rd_final;
	}

	// Enforce monotone decrease past the peak
	int peak = 0;
	for( int i = 1; i < TABLE_SIZE; i++ )
	{
		if( table_out[i][channel] > table_out[peak][channel] )
			peak = i;
	}
	for( int i = peak + 1; i < TABLE_SIZE; i++ )
	{
		if( table_out[i][channel] > table_out[i - 1][channel] )
			table_out[i][channel] = table_out[i - 1][channel];
	}
}

void BioSpecSkinSSSShaderOp::PrecomputeProfile()
{
	// Determine max distance from reticular dermis sigma_tr (slowest decay)
	LayerParams layers[4];
	ComputePerLayerCoefficients( 615.0, melanosomes_in_epidermis,
		whole_blood_papillary, whole_blood_reticular, layers );
	const Scalar sigma_tr_min = layers[3].sigma_tr;

	m_max_distance = (sigma_tr_min > 1e-10) ? (14.0 / sigma_tr_min) : 5.0;
	if( m_max_distance > 5.0 ) m_max_distance = 5.0;
	if( m_max_distance < 0.1 ) m_max_distance = 0.1;

	m_table_r2_max = m_max_distance * m_max_distance;
	m_table_r2_step = m_table_r2_max / TABLE_SIZE;

	for( int i = 0; i < TABLE_SIZE; i++ )
		m_Rd_table[i] = RISEPel( 0, 0, 0 );

	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };
	for( int c = 0; c < 3; c++ )
		TabulateProfileAtWavelength( rgb_wavelengths[c], c );

	GlobalLog()->PrintEx( eLog_Event,
		"BioSpecSkinSSSShaderOp: Profile tabulated (max_r=%.3f cm, table_size=%d)",
		m_max_distance, TABLE_SIZE );
}

//=============================================================
// 3D Profile LUT precomputation
//=============================================================

void BioSpecSkinSSSShaderOp::PrecomputeLUT()
{
	GlobalLog()->PrintEasyEvent( "BioSpecSkinSSSShaderOp:: Precomputing 3D profile LUT..." );

	// melanosomes: 0.001 to 0.43, log-spaced
	const Scalar mel_min = 0.001;
	const Scalar mel_max = 0.43;
	for( int i = 0; i < N_MEL; i++ )
	{
		const Scalar t = Scalar(i) / Scalar(N_MEL - 1);
		m_mel_grid[i] = mel_min * pow( mel_max / mel_min, t );
	}

	// blood papillary: 0 to 0.05, linear
	for( int i = 0; i < N_BLP; i++ )
		m_blp_grid[i] = 0.05 * Scalar(i) / Scalar(N_BLP - 1);

	// blood reticular: 0 to 0.03, linear
	for( int i = 0; i < N_BLR; i++ )
		m_blr_grid[i] = 0.03 * Scalar(i) / Scalar(N_BLR - 1);

	// Conservative max distance from lowest-absorption corner
	{
		LayerParams layers[4];
		ComputePerLayerCoefficients( 615.0, mel_min, 0.0, 0.0, layers );
		const Scalar sigma_tr_min = layers[3].sigma_tr;
		m_max_distance_lut = (sigma_tr_min > 1e-10) ? (14.0 / sigma_tr_min) : 5.0;
		if( m_max_distance_lut > 5.0 ) m_max_distance_lut = 5.0;
		if( m_max_distance_lut < 0.1 ) m_max_distance_lut = 0.1;
	}

	const Scalar lut_r2_max = m_max_distance_lut * m_max_distance_lut;
	const Scalar lut_r2_step = lut_r2_max / TABLE_SIZE;

	m_lut_tables = new RISEPel[ LUT_TOTAL * TABLE_SIZE ];
	memset( m_lut_tables, 0, sizeof(RISEPel) * LUT_TOTAL * TABLE_SIZE );

	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };

	for( int i_mel = 0; i_mel < N_MEL; i_mel++ )
	{
		for( int i_blp = 0; i_blp < N_BLP; i_blp++ )
		{
			for( int i_blr = 0; i_blr < N_BLR; i_blr++ )
			{
				RISEPel* table = &m_lut_tables[ LUTIndex(i_mel, i_blp, i_blr) ];
				for( int c = 0; c < 3; c++ )
				{
					TabulateProfileAtWavelengthInto(
						rgb_wavelengths[c], c,
						m_mel_grid[i_mel], m_blp_grid[i_blp], m_blr_grid[i_blr],
						table, lut_r2_max, lut_r2_step );
				}
			}
		}
	}

	GlobalLog()->PrintEx( eLog_Event,
		"BioSpecSkinSSSShaderOp: LUT precomputed (%dx%dx%d = %d entries, max_r=%.3f cm)",
		N_MEL, N_BLP, N_BLR, LUT_TOTAL, m_max_distance_lut );
}

//=============================================================
// Trilinear interpolation of LUT
//=============================================================

void BioSpecSkinSSSShaderOp::InterpolateProfile(
	Scalar mel, Scalar blp, Scalar blr,
	RISEPel* table_out
	) const
{
	// Clamp to grid range
	if( mel < m_mel_grid[0] ) mel = m_mel_grid[0];
	if( mel > m_mel_grid[N_MEL-1] ) mel = m_mel_grid[N_MEL-1];
	if( blp < m_blp_grid[0] ) blp = m_blp_grid[0];
	if( blp > m_blp_grid[N_BLP-1] ) blp = m_blp_grid[N_BLP-1];
	if( blr < m_blr_grid[0] ) blr = m_blr_grid[0];
	if( blr > m_blr_grid[N_BLR-1] ) blr = m_blr_grid[N_BLR-1];

	// Melanosomes (log-spaced grid)
	int i0_mel = 0;
	for( int i = 0; i < N_MEL - 1; i++ )
		if( mel >= m_mel_grid[i] && mel <= m_mel_grid[i+1] ) { i0_mel = i; break; }
	if( mel > m_mel_grid[N_MEL-2] ) i0_mel = N_MEL - 2;
	const Scalar t_mel = (m_mel_grid[i0_mel+1] > m_mel_grid[i0_mel] + 1e-30) ?
		(mel - m_mel_grid[i0_mel]) / (m_mel_grid[i0_mel+1] - m_mel_grid[i0_mel]) : 0;

	// Blood papillary (linear grid)
	int i0_blp = 0;
	for( int i = 0; i < N_BLP - 1; i++ )
		if( blp >= m_blp_grid[i] && blp <= m_blp_grid[i+1] ) { i0_blp = i; break; }
	if( blp > m_blp_grid[N_BLP-2] ) i0_blp = N_BLP - 2;
	const Scalar t_blp = (m_blp_grid[i0_blp+1] > m_blp_grid[i0_blp] + 1e-30) ?
		(blp - m_blp_grid[i0_blp]) / (m_blp_grid[i0_blp+1] - m_blp_grid[i0_blp]) : 0;

	// Blood reticular (linear grid)
	int i0_blr = 0;
	for( int i = 0; i < N_BLR - 1; i++ )
		if( blr >= m_blr_grid[i] && blr <= m_blr_grid[i+1] ) { i0_blr = i; break; }
	if( blr > m_blr_grid[N_BLR-2] ) i0_blr = N_BLR - 2;
	const Scalar t_blr = (m_blr_grid[i0_blr+1] > m_blr_grid[i0_blr] + 1e-30) ?
		(blr - m_blr_grid[i0_blr]) / (m_blr_grid[i0_blr+1] - m_blr_grid[i0_blr]) : 0;

	const RISEPel* c000 = &m_lut_tables[ LUTIndex(i0_mel,   i0_blp,   i0_blr) ];
	const RISEPel* c001 = &m_lut_tables[ LUTIndex(i0_mel,   i0_blp,   i0_blr+1) ];
	const RISEPel* c010 = &m_lut_tables[ LUTIndex(i0_mel,   i0_blp+1, i0_blr) ];
	const RISEPel* c011 = &m_lut_tables[ LUTIndex(i0_mel,   i0_blp+1, i0_blr+1) ];
	const RISEPel* c100 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_blp,   i0_blr) ];
	const RISEPel* c101 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_blp,   i0_blr+1) ];
	const RISEPel* c110 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_blp+1, i0_blr) ];
	const RISEPel* c111 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_blp+1, i0_blr+1) ];

	const Scalar w000 = (1-t_mel) * (1-t_blp) * (1-t_blr);
	const Scalar w001 = (1-t_mel) * (1-t_blp) * t_blr;
	const Scalar w010 = (1-t_mel) * t_blp     * (1-t_blr);
	const Scalar w011 = (1-t_mel) * t_blp     * t_blr;
	const Scalar w100 = t_mel     * (1-t_blp) * (1-t_blr);
	const Scalar w101 = t_mel     * (1-t_blp) * t_blr;
	const Scalar w110 = t_mel     * t_blp     * (1-t_blr);
	const Scalar w111 = t_mel     * t_blp     * t_blr;

	for( int i = 0; i < TABLE_SIZE; i++ )
	{
		table_out[i] = c000[i] * w000 + c001[i] * w001
			+ c010[i] * w010 + c011[i] * w011
			+ c100[i] * w100 + c101[i] * w101
			+ c110[i] * w110 + c111[i] * w111;
	}
}

//=============================================================
// Constructor / Destructor
//=============================================================

BioSpecSkinSSSShaderOp::BioSpecSkinSSSShaderOp(
	const unsigned int numPoints_,
	const Scalar error_,
	const unsigned int maxPointsPerNode_,
	const unsigned char maxDepth_,
	const Scalar irrad_scale_,
	const IShader& shader_,
	const bool cache_,
	const Scalar thickness_SC_,
	const Scalar thickness_epidermis_,
	const Scalar thickness_papillary_,
	const Scalar thickness_reticular_,
	const Scalar ior_SC_,
	const Scalar ior_epidermis_,
	const Scalar ior_papillary_,
	const Scalar ior_reticular_,
	const Scalar concentration_eumelanin_,
	const Scalar concentration_pheomelanin_,
	const Scalar melanosomes_in_epidermis_,
	const Scalar hb_ratio_,
	const Scalar whole_blood_papillary_,
	const Scalar whole_blood_reticular_,
	const Scalar bilirubin_concentration_,
	const Scalar betacarotene_SC_,
	const Scalar betacarotene_epidermis_,
	const Scalar betacarotene_dermis_,
	IPainter* pOffsetMelanosomes_,
	IPainter* pOffsetBloodPapillary_,
	IPainter* pOffsetBloodReticular_
	) :
  numPoints( numPoints_ ),
  error( error_ ),
  maxPointsPerNode( maxPointsPerNode_ ),
  maxDepth( maxDepth_ ),
  irrad_scale( irrad_scale_ ),
  shader( shader_ ),
  cache( cache_ ),
  thickness_SC( thickness_SC_ ),
  thickness_epidermis( thickness_epidermis_ ),
  thickness_papillary( thickness_papillary_ ),
  thickness_reticular( thickness_reticular_ ),
  ior_SC( ior_SC_ ),
  ior_epidermis( ior_epidermis_ ),
  ior_papillary( ior_papillary_ ),
  ior_reticular( ior_reticular_ ),
  concentration_eumelanin( concentration_eumelanin_ ),
  concentration_pheomelanin( concentration_pheomelanin_ ),
  melanosomes_in_epidermis( melanosomes_in_epidermis_ ),
  hb_ratio( hb_ratio_ ),
  whole_blood_papillary( whole_blood_papillary_ ),
  whole_blood_reticular( whole_blood_reticular_ ),
  bilirubin_concentration( bilirubin_concentration_ ),
  betacarotene_SC( betacarotene_SC_ ),
  betacarotene_epidermis( betacarotene_epidermis_ ),
  betacarotene_dermis( betacarotene_dermis_ ),
  pOffsetMelanosomes( pOffsetMelanosomes_ ),
  pOffsetBloodPapillary( pOffsetBloodPapillary_ ),
  pOffsetBloodReticular( pOffsetBloodReticular_ ),
  m_has_offset_painters( pOffsetMelanosomes_ || pOffsetBloodPapillary_ || pOffsetBloodReticular_ ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 ),
  pBilirubinExt( 0 ),
  pBetaCaroteneExt( 0 ),
  m_table_r2_max( 1.0 ),
  m_table_r2_step( 0.001 ),
  m_max_distance( 1.0 ),
  m_lut_tables( 0 ),
  m_max_distance_lut( 1.0 )
{
	shader.addref();
	if( pOffsetMelanosomes ) pOffsetMelanosomes->addref();
	if( pOffsetBloodPapillary ) pOffsetBloodPapillary->addref();
	if( pOffsetBloodReticular ) pOffsetBloodReticular->addref();

	hb_concentration = SkinData::hb_concen_whole_blood;

	// Build chromophore extinction LUTs (same as BioSpecDiffusionProfile)
	{
		const int count = sizeof( SkinData::omlc_eumelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_eumelanin_wavelengths, SkinData::omlc_eumelanin_ext_mgml );
		pEumelaninExt = pFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_pheomelanin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_pheomelanin_wavelengths, SkinData::omlc_pheomelanin_ext_mgml );
		pPheomelaninExt = pFunc;
	}
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
	{
		const int count = sizeof( SkinData::omlc_prahl_bilirubin_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_bilirubin_wavelengths, SkinData::omlc_prahl_bilirubin );
		pBilirubinExt = pFunc;
	}
	{
		const int count = sizeof( SkinData::omlc_prahl_betacarotene_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_betacarotene_wavelengths, SkinData::omlc_prahl_betacarotene );
		pBetaCaroteneExt = pFunc;
	}

	PrecomputeProfile();

	if( m_has_offset_painters )
		PrecomputeLUT();
}

BioSpecSkinSSSShaderOp::~BioSpecSkinSSSShaderOp()
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ )
		delete i->second;
	pointsets.clear();

	delete[] m_lut_tables;

	if( pOffsetMelanosomes ) pOffsetMelanosomes->release();
	if( pOffsetBloodPapillary ) pOffsetBloodPapillary->release();
	if( pOffsetBloodReticular ) pOffsetBloodReticular->release();

	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );
	shader.release();
}

//=============================================================
// ISubSurfaceExtinctionFunction — profile lookup
//=============================================================

RISEPel BioSpecSkinSSSShaderOp::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	const Scalar r2 = distance * distance;
	if( r2 >= m_table_r2_max ) return RISEPel( 0, 0, 0 );

	const Scalar t = r2 / m_table_r2_step;
	const int idx = (int)t;
	const Scalar frac = t - idx;

	if( idx >= TABLE_SIZE - 1 )
		return m_Rd_table[TABLE_SIZE - 1];

	return m_Rd_table[idx] * (1.0 - frac) + m_Rd_table[idx + 1] * frac;
}

Scalar BioSpecSkinSSSShaderOp::GetMaximumDistanceForError(
	const Scalar error
	) const
{
	return m_max_distance;
}

//=============================================================
// IShaderOp — two-pass BSSRDF evaluation
//=============================================================

void BioSpecSkinSSSShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	c = RISEPel( 0.0 );

	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) return;
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) return;

	if( cache )
	{
		if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) )
			return;
	}

	// --- Pass 1: Lazy octree construction ---
	PointSetMap::iterator it = pointsets.find( ri.pObject );
	PointSetOctree* ps = 0;

	if( it == pointsets.end() )
	{
		create_mutex.lock();
		PointSetMap::iterator again = pointsets.find( ri.pObject );
		if( again == pointsets.end() )
		{
			GlobalLog()->PrintEasyInfo( "BioSpecSkinSSSShaderOp:: Generating irradiance samples" );

			PointSetOctree::PointSet points;
			BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

			const Scalar objectArea = ri.pObject->GetArea();
			const Scalar dA = objectArea * irrad_scale;

			MultiHalton mh;

			for( unsigned int i = 0; i < numPoints; i++ )
			{
				PointSetOctree::SamplePoint sp;
				Vector3 normal;
				Point3 random_variables(
					mh.mod1(mh.halton(0,i)),
					mh.mod1(mh.halton(1,i)),
					mh.mod1(mh.halton(2,i)) );

				ri.pObject->UniformRandomPoint( &sp.ptPosition, &normal, 0, random_variables );

				RayIntersection newri( ri );
				newri.geometric.ray = Ray( sp.ptPosition, -normal );
				newri.geometric.bHit = true;
				newri.geometric.ptIntersection = sp.ptPosition;
				newri.geometric.vNormal = normal;
				newri.geometric.ray.Advance( 1e-8 );

				shader.Shade( rc, newri, caster, rs, sp.irrad, ior_stack );

				if( ColorMath::MaxValue(sp.irrad) > 0 )
				{
					sp.irrad = sp.irrad * dA;
					points.push_back( sp );
					bbox.Include( sp.ptPosition );
				}
			}

			if( points.size() > 0 )
			{
				bbox.EnsureBoxHasVolume();
				ps = new PointSetOctree( bbox, maxPointsPerNode );
				ps->AddElements( points, maxDepth );
			}

			pointsets[ri.pObject] = ps;
		}
		else
		{
			ps = again->second;
		}
		create_mutex.unlock();
	}
	else
	{
		ps = it->second;
	}

	// --- Pass 2: Hierarchical octree evaluation ---
	if( ps )
	{
		if( m_has_offset_painters && m_lut_tables )
		{
			Scalar mel_eff = melanosomes_in_epidermis;
			Scalar blp_eff = whole_blood_papillary;
			Scalar blr_eff = whole_blood_reticular;

			if( pOffsetMelanosomes )
				mel_eff += pOffsetMelanosomes->GetColor( ri.geometric )[0];
			if( pOffsetBloodPapillary )
				blp_eff += pOffsetBloodPapillary->GetColor( ri.geometric )[0];
			if( pOffsetBloodReticular )
				blr_eff += pOffsetBloodReticular->GetColor( ri.geometric )[0];

			if( mel_eff < 0 ) mel_eff = 0;
			if( mel_eff > 0.43 ) mel_eff = 0.43;
			if( blp_eff < 0 ) blp_eff = 0;
			if( blp_eff > 0.05 ) blp_eff = 0.05;
			if( blr_eff < 0 ) blr_eff = 0;
			if( blr_eff > 0.03 ) blr_eff = 0.03;

			RISEPel local_table[TABLE_SIZE];
			InterpolateProfile( mel_eff, blp_eff, blr_eff, local_table );

			const Scalar lut_r2_max = m_max_distance_lut * m_max_distance_lut;
			const Scalar lut_r2_step = lut_r2_max / TABLE_SIZE;
			BioSpecLocalProfile profile( local_table, lut_r2_max, lut_r2_step, m_max_distance_lut );

			ps->Evaluate( c, ri.geometric.ptIntersection, profile, error, 0, ri.geometric );
		}
		else
		{
			ps->Evaluate( c, ri.geometric.ptIntersection, *this, error, 0, ri.geometric );
		}

		c = c * (1.0 / Scalar(numPoints));
	}

	if( cache )
		rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
}

Scalar BioSpecSkinSSSShaderOp::PerformOperationNM(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum,
	const Scalar nm,
	const IORStack* const ior_stack,
	const ScatteredRayContainer* pScat
	) const
{
	return 0;
}

void BioSpecSkinSSSShaderOp::ResetRuntimeData() const
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ )
		delete i->second;
	pointsets.clear();
}
