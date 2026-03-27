//////////////////////////////////////////////////////////////////////
//
//  DonnerJensenSkinSSSShaderOp.cpp - Octree-based BSSRDF evaluation
//  for the Donner et al. 2008 spectral skin model.
//
//  See DonnerJensenSkinSSSShaderOp.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DonnerJensenSkinSSSShaderOp.h"
#include "../../Materials/BioSpecSkinData.h"
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
// LocalProfile — stack-allocated ISubSurfaceExtinctionFunction
// wrapper for thread-safe per-pixel profile lookup.
//
// When offset painters produce a spatially-varying diffusion
// profile, each thread's PerformOperation call interpolates
// the 3D LUT into a stack-allocated table, wraps it in this
// class, and passes it to PointSetOctree::Evaluate().
//
// Thread safety: each thread has its own stack frame, so no
// shared mutable state.  No-op addref/release since the
// lifetime is bounded by the PerformOperation scope.
//=============================================================

class LocalProfile : public ISubSurfaceExtinctionFunction
{
	const RISEPel*	table;
	Scalar			r2_max;
	Scalar			r2_step;
	Scalar			max_dist;
	static const int TABLE_SIZE = DonnerJensenSkinSSSShaderOp::TABLE_SIZE;

public:
	LocalProfile( const RISEPel* t, Scalar r2m, Scalar r2s, Scalar md )
		: table(t), r2_max(r2m), r2_step(r2s), max_dist(md) {}

	// No-op ref counting: stack-allocated, lifetime bounded by PerformOperation scope
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

		if( idx >= TABLE_SIZE - 1 )
			return table[TABLE_SIZE - 1];

		return table[idx] * (1.0 - frac) + table[idx + 1] * frac;
	}

	Scalar GetMaximumDistanceForError( const Scalar error ) const
	{
		return max_dist;
	}
};

//=============================================================
// Static helpers (same formulas as DonnerJensenSkinDiffusionProfile)
//=============================================================

Scalar DonnerJensenSkinSSSShaderOp::ComputeSkinBaselineAbsorption( const Scalar nm )
{
	return (0.244 + 85.3 * exp( -(nm - 154.0) / 66.2 ));
}

Scalar DonnerJensenSkinSSSShaderOp::ComputeEpidermisScattering( const Scalar nm )
{
	// Jacques 1998 / OMLC reduced scattering for skin (cm^-1):
	//   mu_sp = 2e12 * nm^(-4) + 2e5 * nm^(-1.5)
	// See DonnerJensenSkinDiffusionProfile.h deviation #4 for why
	// we use this instead of the paper's Eq. 13.
	return 2.0e12 * pow( nm, -4.0 ) + 2.0e5 * pow( nm, -1.5 );
}

//=============================================================
// Per-layer coefficient computation (Donner 2008 Eq. 11-13)
//=============================================================

void DonnerJensenSkinSSSShaderOp::ComputePerLayerCoefficients(
	const Scalar nm,
	LayerParams layers_out[2]
	) const
{
	const Scalar baseline = ComputeSkinBaselineAbsorption( nm );

	// Melanin (calibrated to Jacques 1998 melanosome power law)
	static const Scalar C_MEL = 132.5;
	const Scalar abs_eumelanin = pEumelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_melanin = melanin_blend * abs_eumelanin + (1.0 - melanin_blend) * abs_pheomelanin;

	// Hemoglobin
	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_hb = (pDeoxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_blood = blood_oxygenation * abs_hbo2 + (1.0 - blood_oxygenation) * abs_hb;

	// Carotene
	const Scalar abs_carotene = pBetaCaroteneExt->Evaluate( nm ) * carotene_fraction / 537.0 * log( 10.0 );

	// Epidermis (Eq. 11)
	const Scalar remainder_epi = 1.0 - melanin_fraction - hemoglobin_epidermis - carotene_fraction;
	const Scalar sigma_a_epi = melanin_fraction * abs_melanin
		+ hemoglobin_epidermis * abs_blood
		+ abs_carotene
		+ (remainder_epi > 0 ? remainder_epi : 0) * baseline;

	// Dermis (Eq. 12)
	const Scalar sigma_a_derm = hemoglobin_dermis * abs_blood
		+ (1.0 - hemoglobin_dermis) * baseline;

	// Scattering (Eq. 13)
	const Scalar sigma_sp_epi = ComputeEpidermisScattering( nm );
	const Scalar sigma_sp_derm = sigma_sp_epi * 0.5;

	// Epidermis
	layers_out[0].sigma_a = sigma_a_epi;
	layers_out[0].sigma_sp = sigma_sp_epi;
	layers_out[0].thickness = epidermis_thickness;
	layers_out[0].ior = ior_epidermis;
	ComputeLayerDerivedParams( layers_out[0] );

	// Dermis (semi-infinite)
	layers_out[1].sigma_a = sigma_a_derm;
	layers_out[1].sigma_sp = sigma_sp_derm;
	layers_out[1].thickness = 1.0;
	layers_out[1].ior = ior_dermis;
	ComputeLayerDerivedParams( layers_out[1] );
}

//=============================================================
// Profile tabulation
//=============================================================

void DonnerJensenSkinSSSShaderOp::TabulateProfileAtWavelength(
	const Scalar nm,
	const int channel
	)
{
	LayerParams layers[2];
	ComputePerLayerCoefficients( nm, layers );

	const LayerParams& epi = layers[0];
	const LayerParams& derm = layers[1];

	// --- Hankel-domain composite profile ---
	static const int N_FREQ = 512;
	static const int N_MULTIPOLE = 20;

	HankelGrid grid;
	grid.Create( 0.01, 1e5, N_FREQ );

	double composite_R[N_FREQ];
	ComputeCompositeProfileHankel( layers, 2, grid.s, N_FREQ, N_MULTIPOLE, composite_R );

	// --- Spatial-domain reference (dermis dipole) ---
	// Used to correct Hankel ringing at medium-large r.
	const double T_epi_beer = exp( -epi.sigma_a * epi.thickness * 2.0 );
	const double eta_down = epi.ior / derm.ior;
	const double eta_up = derm.ior / epi.ior;
	const double Ft = (1.0 - ComputeFdr(eta_down)) * (1.0 - ComputeFdr(eta_up));
	const double reference_scale = T_epi_beer * Ft;

	// --- Tabulate Rd(r) with hybrid correction ---
	for( int i = 0; i < TABLE_SIZE; i++ )
	{
		const double r2 = (i + 0.5) * m_table_r2_step;
		const double r = sqrt( r2 );

		// Hankel inverse transform
		double Rd_hankel = grid.InverseTransform( composite_R, r ) / (2.0 * PI);
		if( Rd_hankel < 0 ) Rd_hankel = 0;

		// Reference: dermis dipole scaled by epidermis transmission
		const double d_r = sqrt( r2 + derm.z_r * derm.z_r );
		const double d_v = sqrt( r2 + derm.z_v * derm.z_v );
		const double term_r = derm.z_r * (1.0 + derm.sigma_tr * d_r) *
			exp( -derm.sigma_tr * d_r ) / (d_r * d_r * d_r);
		const double term_v = derm.z_v * (1.0 + derm.sigma_tr * d_v) *
			exp( -derm.sigma_tr * d_v ) / (d_v * d_v * d_v);
		double Rd_ref = reference_scale * (derm.alpha_prime / (4.0 * PI)) * (term_r + term_v);
		if( Rd_ref < 0 ) Rd_ref = 0;

		// Hybrid: use Hankel where it's higher (captures inter-layer coupling
		// in the near-field), fall back to reference where Hankel undershoots
		// or at large r where ringing dominates.
		double Rd_final;
		const double r_crossover = 2.0 / derm.sigma_tr;
		if( r < r_crossover )
		{
			// Near-field: use max of Hankel and reference
			Rd_final = (Rd_hankel > Rd_ref) ? Rd_hankel : Rd_ref;
		}
		else
		{
			// Far-field: use reference (Hankel is unreliable here)
			Rd_final = Rd_ref;
		}

		m_Rd_table[i][channel] = Rd_final;
	}

	// Enforce monotone decrease past the peak
	int peak = 0;
	for( int i = 1; i < TABLE_SIZE; i++ )
	{
		if( m_Rd_table[i][channel] > m_Rd_table[peak][channel] )
			peak = i;
	}
	for( int i = peak + 1; i < TABLE_SIZE; i++ )
	{
		if( m_Rd_table[i][channel] > m_Rd_table[i - 1][channel] )
			m_Rd_table[i][channel] = m_Rd_table[i - 1][channel];
	}
}

void DonnerJensenSkinSSSShaderOp::PrecomputeProfile()
{
	// Determine max distance from the dermis sigma_tr (slowest decay)
	LayerParams layers[2];
	ComputePerLayerCoefficients( 615.0, layers );  // red has lowest sigma_tr
	const Scalar sigma_tr_min = layers[1].sigma_tr;

	// r_max where exp(-sigma_tr * r) ≈ 1e-6
	m_max_distance = (sigma_tr_min > 1e-10) ? (14.0 / sigma_tr_min) : 5.0;
	if( m_max_distance > 5.0 ) m_max_distance = 5.0;
	if( m_max_distance < 0.1 ) m_max_distance = 0.1;

	m_table_r2_max = m_max_distance * m_max_distance;
	m_table_r2_step = m_table_r2_max / TABLE_SIZE;

	// Initialize table to zero
	for( int i = 0; i < TABLE_SIZE; i++ )
		m_Rd_table[i] = RISEPel( 0, 0, 0 );

	// Tabulate at RGB wavelengths: R=615nm, G=550nm, B=465nm
	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };
	for( int c = 0; c < 3; c++ )
	{
		TabulateProfileAtWavelength( rgb_wavelengths[c], c );
	}

	GlobalLog()->PrintEx( eLog_Info,
		"DonnerJensenSkinSSSShaderOp: Profile tabulated (max_r=%.3f cm, table_size=%d)",
		m_max_distance, TABLE_SIZE );
}

//=============================================================
// Parameterized profile tabulation (for LUT entries)
//=============================================================

void DonnerJensenSkinSSSShaderOp::TabulateProfileAtWavelengthInto(
	const Scalar nm,
	const int channel,
	const Scalar mel_frac,
	const Scalar hb_epi,
	const Scalar hb_derm,
	RISEPel* table_out,
	Scalar table_r2_max_val,
	Scalar table_r2_step_val
	) const
{
	// Compute layer params with the given parameter values
	const Scalar baseline = ComputeSkinBaselineAbsorption( nm );

	static const Scalar C_MEL = 132.5;
	const Scalar abs_eumelanin = pEumelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_pheomelanin = pPheomelaninExt->Evaluate( nm ) * C_MEL;
	const Scalar abs_melanin = melanin_blend * abs_eumelanin + (1.0 - melanin_blend) * abs_pheomelanin;

	const Scalar abs_hbo2 = (pOxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_hb_deoxy = (pDeoxyHemoglobinExt->Evaluate( nm ) * hb_concentration) / 66500.0 * log( 10.0 );
	const Scalar abs_blood = blood_oxygenation * abs_hbo2 + (1.0 - blood_oxygenation) * abs_hb_deoxy;
	const Scalar abs_carotene = pBetaCaroteneExt->Evaluate( nm ) * carotene_fraction / 537.0 * log( 10.0 );

	const Scalar remainder_epi = 1.0 - mel_frac - hb_epi - carotene_fraction;
	const Scalar sigma_a_epi = mel_frac * abs_melanin
		+ hb_epi * abs_blood
		+ abs_carotene
		+ (remainder_epi > 0 ? remainder_epi : 0) * baseline;

	const Scalar sigma_a_derm = hb_derm * abs_blood
		+ (1.0 - hb_derm) * baseline;

	const Scalar sigma_sp_epi = ComputeEpidermisScattering( nm );
	const Scalar sigma_sp_derm = sigma_sp_epi * 0.5;

	LayerParams layers[2];
	layers[0].sigma_a = sigma_a_epi;
	layers[0].sigma_sp = sigma_sp_epi;
	layers[0].thickness = epidermis_thickness;
	layers[0].ior = ior_epidermis;
	ComputeLayerDerivedParams( layers[0] );

	layers[1].sigma_a = sigma_a_derm;
	layers[1].sigma_sp = sigma_sp_derm;
	layers[1].thickness = 1.0;
	layers[1].ior = ior_dermis;
	ComputeLayerDerivedParams( layers[1] );

	// Hankel composite
	static const int N_FREQ = 512;
	static const int N_MULTIPOLE = 20;
	HankelGrid grid;
	grid.Create( 0.01, 1e5, N_FREQ );

	double composite_R[N_FREQ];
	ComputeCompositeProfileHankel( layers, 2, grid.s, N_FREQ, N_MULTIPOLE, composite_R );

	// Reference dipole
	const LayerParams& epi = layers[0];
	const LayerParams& derm = layers[1];
	const double T_epi_beer = exp( -epi.sigma_a * epi.thickness * 2.0 );
	const double eta_down = epi.ior / derm.ior;
	const double eta_up = derm.ior / epi.ior;
	const double Ft = (1.0 - ComputeFdr(eta_down)) * (1.0 - ComputeFdr(eta_up));
	const double reference_scale = T_epi_beer * Ft;

	// Tabulate with hybrid correction
	for( int i = 0; i < TABLE_SIZE; i++ )
	{
		const double r2 = (i + 0.5) * table_r2_step_val;
		const double r = sqrt( r2 );

		double Rd_hankel = grid.InverseTransform( composite_R, r ) / (2.0 * PI);
		if( Rd_hankel < 0 ) Rd_hankel = 0;

		const double d_r = sqrt( r2 + derm.z_r * derm.z_r );
		const double d_v = sqrt( r2 + derm.z_v * derm.z_v );
		const double term_r = derm.z_r * (1.0 + derm.sigma_tr * d_r) *
			exp( -derm.sigma_tr * d_r ) / (d_r * d_r * d_r);
		const double term_v = derm.z_v * (1.0 + derm.sigma_tr * d_v) *
			exp( -derm.sigma_tr * d_v ) / (d_v * d_v * d_v);
		double Rd_ref = reference_scale * (derm.alpha_prime / (4.0 * PI)) * (term_r + term_v);
		if( Rd_ref < 0 ) Rd_ref = 0;

		double Rd_final;
		const double r_crossover = 2.0 / derm.sigma_tr;
		if( r < r_crossover )
			Rd_final = (Rd_hankel > Rd_ref) ? Rd_hankel : Rd_ref;
		else
			Rd_final = Rd_ref;

		table_out[i][channel] = Rd_final;
	}

	// Enforce monotone decrease past peak
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

//=============================================================
// 3D Profile LUT precomputation
//=============================================================

void DonnerJensenSkinSSSShaderOp::PrecomputeLUT()
{
	GlobalLog()->PrintEasyEvent( "DonnerJensenSkinSSSShaderOp:: Precomputing 3D profile LUT..." );

	// Build grid axes (log-spaced for melanin, linear for hemoglobin)
	// Melanin: 0 to 0.5, log-spaced with a small floor
	const Scalar mel_min = 0.001;
	const Scalar mel_max = 0.5;
	for( int i = 0; i < N_MEL; i++ )
	{
		const Scalar t = Scalar(i) / Scalar(N_MEL - 1);
		m_mel_grid[i] = mel_min * pow( mel_max / mel_min, t );
	}

	// Hemoglobin epidermis: 0 to 0.05, linear
	for( int i = 0; i < N_HBE; i++ )
		m_hbe_grid[i] = 0.05 * Scalar(i) / Scalar(N_HBE - 1);

	// Hemoglobin dermis: 0 to 0.1, linear
	for( int i = 0; i < N_HBD; i++ )
		m_hbd_grid[i] = 0.1 * Scalar(i) / Scalar(N_HBD - 1);

	// Determine a conservative max distance across the full parameter range.
	// Use the lowest-absorption corner (min melanin, min hemoglobin) which
	// gives the largest diffusion distance.
	{
		LayerParams layers[2];
		// Temporarily override members for computation
		const Scalar saved_mel = melanin_fraction;
		const Scalar saved_hbe = hemoglobin_epidermis;
		const Scalar saved_hbd = hemoglobin_dermis;

		// Use const_cast for temporary override during precomputation only
		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->melanin_fraction) = mel_min;
		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->hemoglobin_epidermis) = 0.0;
		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->hemoglobin_dermis) = 0.0;
		ComputePerLayerCoefficients( 615.0, layers );

		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->melanin_fraction) = saved_mel;
		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->hemoglobin_epidermis) = saved_hbe;
		const_cast<Scalar&>(const_cast<DonnerJensenSkinSSSShaderOp*>(this)->hemoglobin_dermis) = saved_hbd;

		const Scalar sigma_tr_min = layers[1].sigma_tr;
		m_max_distance_lut = (sigma_tr_min > 1e-10) ? (14.0 / sigma_tr_min) : 5.0;
		if( m_max_distance_lut > 5.0 ) m_max_distance_lut = 5.0;
		if( m_max_distance_lut < 0.1 ) m_max_distance_lut = 0.1;
	}

	const Scalar lut_r2_max = m_max_distance_lut * m_max_distance_lut;
	const Scalar lut_r2_step = lut_r2_max / TABLE_SIZE;

	// Allocate LUT storage
	m_lut_tables = new RISEPel[ LUT_TOTAL * TABLE_SIZE ];
    memset( (void*)m_lut_tables, 0, sizeof(RISEPel) * LUT_TOTAL * TABLE_SIZE );

	static const Scalar rgb_wavelengths[3] = { 615.0, 550.0, 465.0 };

	// Fill each grid point
	for( int i_mel = 0; i_mel < N_MEL; i_mel++ )
	{
		for( int i_hbe = 0; i_hbe < N_HBE; i_hbe++ )
		{
			for( int i_hbd = 0; i_hbd < N_HBD; i_hbd++ )
			{
				RISEPel* table = &m_lut_tables[ LUTIndex(i_mel, i_hbe, i_hbd) ];

				for( int c = 0; c < 3; c++ )
				{
					TabulateProfileAtWavelengthInto(
						rgb_wavelengths[c], c,
						m_mel_grid[i_mel], m_hbe_grid[i_hbe], m_hbd_grid[i_hbd],
						table, lut_r2_max, lut_r2_step );
				}
			}
		}
	}

	GlobalLog()->PrintEx( eLog_Event,
		"DonnerJensenSkinSSSShaderOp: LUT precomputed (%dx%dx%d = %d entries, max_r=%.3f cm)",
		N_MEL, N_HBE, N_HBD, LUT_TOTAL, m_max_distance_lut );
}

//=============================================================
// Trilinear interpolation of LUT into caller's buffer
//=============================================================

void DonnerJensenSkinSSSShaderOp::InterpolateProfile(
	Scalar mel, Scalar hbe, Scalar hbd,
	RISEPel* table_out
	) const
{
	// Clamp to grid range
	if( mel < m_mel_grid[0] ) mel = m_mel_grid[0];
	if( mel > m_mel_grid[N_MEL-1] ) mel = m_mel_grid[N_MEL-1];
	if( hbe < m_hbe_grid[0] ) hbe = m_hbe_grid[0];
	if( hbe > m_hbe_grid[N_HBE-1] ) hbe = m_hbe_grid[N_HBE-1];
	if( hbd < m_hbd_grid[0] ) hbd = m_hbd_grid[0];
	if( hbd > m_hbd_grid[N_HBD-1] ) hbd = m_hbd_grid[N_HBD-1];

	// Find grid cell + interpolation weights for melanin (log-spaced grid)
	int i0_mel = 0;
	for( int i = 0; i < N_MEL - 1; i++ )
	{
		if( mel >= m_mel_grid[i] && mel <= m_mel_grid[i+1] ) { i0_mel = i; break; }
	}
	if( mel > m_mel_grid[N_MEL-2] ) i0_mel = N_MEL - 2;
	const Scalar t_mel = (m_mel_grid[i0_mel+1] > m_mel_grid[i0_mel] + 1e-30) ?
		(mel - m_mel_grid[i0_mel]) / (m_mel_grid[i0_mel+1] - m_mel_grid[i0_mel]) : 0;

	// Hemoglobin epidermis (linear grid)
	int i0_hbe = 0;
	for( int i = 0; i < N_HBE - 1; i++ )
	{
		if( hbe >= m_hbe_grid[i] && hbe <= m_hbe_grid[i+1] ) { i0_hbe = i; break; }
	}
	if( hbe > m_hbe_grid[N_HBE-2] ) i0_hbe = N_HBE - 2;
	const Scalar t_hbe = (m_hbe_grid[i0_hbe+1] > m_hbe_grid[i0_hbe] + 1e-30) ?
		(hbe - m_hbe_grid[i0_hbe]) / (m_hbe_grid[i0_hbe+1] - m_hbe_grid[i0_hbe]) : 0;

	// Hemoglobin dermis (linear grid)
	int i0_hbd = 0;
	for( int i = 0; i < N_HBD - 1; i++ )
	{
		if( hbd >= m_hbd_grid[i] && hbd <= m_hbd_grid[i+1] ) { i0_hbd = i; break; }
	}
	if( hbd > m_hbd_grid[N_HBD-2] ) i0_hbd = N_HBD - 2;
	const Scalar t_hbd = (m_hbd_grid[i0_hbd+1] > m_hbd_grid[i0_hbd] + 1e-30) ?
		(hbd - m_hbd_grid[i0_hbd]) / (m_hbd_grid[i0_hbd+1] - m_hbd_grid[i0_hbd]) : 0;

	// 8 corner tables for trilinear interpolation
	const RISEPel* c000 = &m_lut_tables[ LUTIndex(i0_mel,   i0_hbe,   i0_hbd) ];
	const RISEPel* c001 = &m_lut_tables[ LUTIndex(i0_mel,   i0_hbe,   i0_hbd+1) ];
	const RISEPel* c010 = &m_lut_tables[ LUTIndex(i0_mel,   i0_hbe+1, i0_hbd) ];
	const RISEPel* c011 = &m_lut_tables[ LUTIndex(i0_mel,   i0_hbe+1, i0_hbd+1) ];
	const RISEPel* c100 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_hbe,   i0_hbd) ];
	const RISEPel* c101 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_hbe,   i0_hbd+1) ];
	const RISEPel* c110 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_hbe+1, i0_hbd) ];
	const RISEPel* c111 = &m_lut_tables[ LUTIndex(i0_mel+1, i0_hbe+1, i0_hbd+1) ];

	// Trilinear interpolation weights
	const Scalar w000 = (1-t_mel) * (1-t_hbe) * (1-t_hbd);
	const Scalar w001 = (1-t_mel) * (1-t_hbe) * t_hbd;
	const Scalar w010 = (1-t_mel) * t_hbe     * (1-t_hbd);
	const Scalar w011 = (1-t_mel) * t_hbe     * t_hbd;
	const Scalar w100 = t_mel     * (1-t_hbe) * (1-t_hbd);
	const Scalar w101 = t_mel     * (1-t_hbe) * t_hbd;
	const Scalar w110 = t_mel     * t_hbe     * (1-t_hbd);
	const Scalar w111 = t_mel     * t_hbe     * t_hbd;

	// Interpolate all TABLE_SIZE entries
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

DonnerJensenSkinSSSShaderOp::DonnerJensenSkinSSSShaderOp(
	const unsigned int numPoints_,
	const Scalar error_,
	const unsigned int maxPointsPerNode_,
	const unsigned char maxDepth_,
	const Scalar irrad_scale_,
	const IShader& shader_,
	const bool cache_,
	const Scalar melanin_fraction_,
	const Scalar melanin_blend_,
	const Scalar hemoglobin_epidermis_,
	const Scalar carotene_fraction_,
	const Scalar hemoglobin_dermis_,
	const Scalar epidermis_thickness_,
	const Scalar ior_epidermis_,
	const Scalar ior_dermis_,
	const Scalar blood_oxygenation_,
	IPainter* pOffsetMelanin_,
	IPainter* pOffsetHbEpidermis_,
	IPainter* pOffsetHbDermis_
	) :
  numPoints( numPoints_ ),
  error( error_ ),
  maxPointsPerNode( maxPointsPerNode_ ),
  maxDepth( maxDepth_ ),
  irrad_scale( irrad_scale_ ),
  shader( shader_ ),
  cache( cache_ ),
  melanin_fraction( melanin_fraction_ ),
  melanin_blend( melanin_blend_ ),
  hemoglobin_epidermis( hemoglobin_epidermis_ ),
  carotene_fraction( carotene_fraction_ ),
  hemoglobin_dermis( hemoglobin_dermis_ ),
  epidermis_thickness( epidermis_thickness_ ),
  ior_epidermis( ior_epidermis_ ),
  ior_dermis( ior_dermis_ ),
  blood_oxygenation( blood_oxygenation_ ),
  pOffsetMelanin( pOffsetMelanin_ ),
  pOffsetHbEpidermis( pOffsetHbEpidermis_ ),
  pOffsetHbDermis( pOffsetHbDermis_ ),
  m_has_offset_painters( pOffsetMelanin_ || pOffsetHbEpidermis_ || pOffsetHbDermis_ ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 ),
  pBetaCaroteneExt( 0 ),
  m_table_r2_max( 1.0 ),
  m_table_r2_step( 0.001 ),
  m_max_distance( 1.0 ),
  m_lut_tables( 0 ),
  m_max_distance_lut( 1.0 )
{
	shader.addref();
	if( pOffsetMelanin ) pOffsetMelanin->addref();
	if( pOffsetHbEpidermis ) pOffsetHbEpidermis->addref();
	if( pOffsetHbDermis ) pOffsetHbDermis->addref();

	hb_concentration = SkinData::hb_concen_whole_blood;

	// Build chromophore extinction LUTs
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
		const int count = sizeof( SkinData::omlc_prahl_betacarotene_wavelengths ) / sizeof( Scalar );
		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );
		pFunc->addControlPoints( count, SkinData::omlc_prahl_betacarotene_wavelengths, SkinData::omlc_prahl_betacarotene );
		pBetaCaroteneExt = pFunc;
	}

	// Precompute the tabulated profile (base, for uniform case)
	PrecomputeProfile();

	// If offset painters are present, precompute the 3D LUT
	if( m_has_offset_painters )
	{
		PrecomputeLUT();
	}
}

DonnerJensenSkinSSSShaderOp::~DonnerJensenSkinSSSShaderOp()
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ )
		delete i->second;
	pointsets.clear();

	delete[] m_lut_tables;

	if( pOffsetMelanin ) pOffsetMelanin->release();
	if( pOffsetHbEpidermis ) pOffsetHbEpidermis->release();
	if( pOffsetHbDermis ) pOffsetHbDermis->release();

	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBetaCaroteneExt );
	shader.release();
}

//=============================================================
// ISubSurfaceExtinctionFunction — profile lookup for PointSetOctree
//=============================================================

RISEPel DonnerJensenSkinSSSShaderOp::ComputeTotalExtinction(
	const Scalar distance
	) const
{
	const Scalar r2 = distance * distance;
	if( r2 >= m_table_r2_max ) return RISEPel( 0, 0, 0 );

	// Linear interpolation in the r²-indexed table
	const Scalar t = r2 / m_table_r2_step;
	const int idx = (int)t;
	const Scalar frac = t - idx;

	if( idx >= TABLE_SIZE - 1 )
		return m_Rd_table[TABLE_SIZE - 1];

	return m_Rd_table[idx] * (1.0 - frac) + m_Rd_table[idx + 1] * frac;
}

Scalar DonnerJensenSkinSSSShaderOp::GetMaximumDistanceForError(
	const Scalar error
	) const
{
	return m_max_distance;
}

//=============================================================
// IShaderOp — two-pass BSSRDF evaluation
//=============================================================

void DonnerJensenSkinSSSShaderOp::PerformOperation(
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

	// Only on normal pass for view rays
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) return;

	// State cache check
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
			GlobalLog()->PrintEasyInfo( "DonnerJensenSkinSSSShaderOp:: Generating irradiance samples" );

			PointSetOctree::PointSet points;
			BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY),
				Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

			// Compute area weight for Monte Carlo integration.
			// The BSSRDF integral is:
			//   L(xo) = (1/pi) * integral Rd(|xo-xi|) * E(xi) * dA(xi)
			//
			// shader.Shade() returns exitant radiance L_shade.  For a
			// Lambertian surface with reflectance rho:
			//   L_shade = (rho/pi) * E  ->  E = pi * L_shade / rho
			//
			// The Monte Carlo estimate is:
			//   L ≈ (1/pi) * (1/N) * sum Rd * (pi * L_shade / rho) * A
			//     = (A / (N * rho)) * sum Rd * L_shade
			//
			// The 1/N factor is applied after the octree sum (line below).
			// Here we pre-multiply each sample by A * irrad_scale.  With
			// the skin material set to reflectance 1.0, rho cancels.
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

			// Store even if null — prevents repeated generation attempts
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
			// Spatially-varying: evaluate offset painters at shading point,
			// interpolate LUT into a stack-allocated table, wrap in LocalProfile.
			// Each thread gets its own stack frame — fully thread-safe.
			Scalar mel_eff = melanin_fraction;
			Scalar hbe_eff = hemoglobin_epidermis;
			Scalar hbd_eff = hemoglobin_dermis;

			if( pOffsetMelanin )
				mel_eff += pOffsetMelanin->GetColor( ri.geometric )[0];
			if( pOffsetHbEpidermis )
				hbe_eff += pOffsetHbEpidermis->GetColor( ri.geometric )[0];
			if( pOffsetHbDermis )
				hbd_eff += pOffsetHbDermis->GetColor( ri.geometric )[0];

			// Clamp to physical range
			if( mel_eff < 0 ) mel_eff = 0;
			if( mel_eff > 0.5 ) mel_eff = 0.5;
			if( hbe_eff < 0 ) hbe_eff = 0;
			if( hbe_eff > 0.05 ) hbe_eff = 0.05;
			if( hbd_eff < 0 ) hbd_eff = 0;
			if( hbd_eff > 0.1 ) hbd_eff = 0.1;

			// Stack-allocated interpolated table: 1024 × 24 bytes = 24 KB
			RISEPel local_table[TABLE_SIZE];
			InterpolateProfile( mel_eff, hbe_eff, hbd_eff, local_table );

			const Scalar lut_r2_max = m_max_distance_lut * m_max_distance_lut;
			const Scalar lut_r2_step = lut_r2_max / TABLE_SIZE;
			LocalProfile profile( local_table, lut_r2_max, lut_r2_step, m_max_distance_lut );

			ps->Evaluate( c, ri.geometric.ptIntersection, profile, error, 0, ri.geometric );
		}
		else
		{
			// Uniform skin: use base table via *this, zero overhead
			ps->Evaluate( c, ri.geometric.ptIntersection, *this, error, 0, ri.geometric );
		}

		// Normalize by sample count (each sample's irradiance * dA already applied)
		c = c * (1.0 / Scalar(numPoints));
	}

	if( cache )
		rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
}

Scalar DonnerJensenSkinSSSShaderOp::PerformOperationNM(
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
	// TODO: spectral version
	return 0;
}

void DonnerJensenSkinSSSShaderOp::ResetRuntimeData() const
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ )
		delete i->second;
	pointsets.clear();
}
