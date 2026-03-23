//////////////////////////////////////////////////////////////////////
//
//  MultipoleDiffusion.cpp - Implementation of per-layer multipole
//  diffusion for layered translucent materials.
//
//  See MultipoleDiffusion.h for overview and references.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MultipoleDiffusion.h"
#include <cmath>

using namespace RISE;

//=============================================================
// Diffuse Fresnel reflectance
//=============================================================

double RISE::ComputeFdr( const double eta )
{
	// Egan & Hilgeman 1973 polynomial fit (Jensen 2001)
	if( eta < 1.0 )
	{
		const double eta_inv = 1.0 / eta;
		return -1.440 / (eta_inv * eta_inv) + 0.710 / eta_inv + 0.668 + 0.0636 * eta_inv;
	}

	return -1.440 / (eta * eta) + 0.710 / eta + 0.668 + 0.0636 * eta;
}

//=============================================================
// Layer derived parameters
//=============================================================

void RISE::ComputeLayerDerivedParams( LayerParams& lp )
{
	lp.sigma_t_prime = lp.sigma_a + lp.sigma_sp;

	if( lp.sigma_t_prime < 1e-20 )
	{
		lp.D = 1e10;
		lp.sigma_tr = 0;
		lp.alpha_prime = 0;
		lp.Fdr = ComputeFdr( lp.ior );
		lp.A = (1.0 + lp.Fdr) / (1.0 - lp.Fdr + 1e-30);
		lp.z_r = 1e10;
		lp.z_v = 1e10;
		lp.d_e = 2.0 * lp.A * lp.D;
		lp.slab_period = 2.0 * (lp.thickness + 2.0 * lp.d_e);
		return;
	}

	lp.D = 1.0 / (3.0 * lp.sigma_t_prime);
	lp.sigma_tr = sqrt( 3.0 * lp.sigma_a * lp.sigma_t_prime );
	lp.alpha_prime = lp.sigma_sp / lp.sigma_t_prime;

	lp.Fdr = ComputeFdr( lp.ior );
	lp.A = (1.0 + lp.Fdr) / (1.0 - lp.Fdr + 1e-30);

	lp.z_r = 1.0 / lp.sigma_t_prime;
	lp.z_v = lp.z_r + 4.0 * lp.A * lp.D;
	lp.d_e = 2.0 * lp.A * lp.D;
	lp.slab_period = 2.0 * (lp.thickness + 2.0 * lp.d_e);
}

//=============================================================
// Single-layer multipole in Hankel domain
//=============================================================

/// Hankel-domain contribution from a single dipole source.
///
/// The Hankel transform of the spatial dipole flux
///   z*(sigma_tr+1/d)*exp(-sigma_tr*d)/d^2  where d=sqrt(r^2+z^2)
/// evaluated at z=0 is simply:
///   exp(-|z| * sqrt(sigma_tr^2 + s^2))
///
/// This follows from the Sommerfeld identity: the zeroth-order Hankel
/// transform of exp(-mu*d)/d = exp(-|z|*q)/q, and differentiating
/// w.r.t. z gives the flux result.
///
/// For reflectance at z=0, a source with charge Q at position z_source
/// contributes:  Q * sign(z_source) * exp(-|z_source| * q)
///
/// For transmittance at z=d:
///   Q * sign(d - z_source) * exp(-|d - z_source| * q)
static inline double FluxHankelContrib(
	const double z_abs,			// |z_source| or |d - z_source|
	const double sigma_tr,
	const double s
	)
{
	const double q = sqrt( sigma_tr * sigma_tr + s * s );
	return exp( -z_abs * q );
}

void RISE::EvaluateMultipoleReflectanceHankel(
	const LayerParams& lp,
	const double* s_grid,
	const int N_freq,
	double* R_tilde_out,
	double* T_tilde_out,
	const int N_multipole
	)
{
	const double d = lp.thickness;
	const double prefactor = lp.alpha_prime / (4.0 * PI);

	// Extrapolated slab boundaries and period for method of images
	// Top boundary:    z = -d_e
	// Bottom boundary: z = d + d_e  (= z_bottom)
	// Effective slab:  D_slab = d + 2*d_e  (= d_eff)
	// Period:          L = 2 * D_slab
	const double z_bottom = d + lp.d_e;
	const double d_eff = d + 2.0 * lp.d_e;
	const double L = 2.0 * d_eff;		// full period

	for( int i = 0; i < N_freq; i++ )
	{
		const double s = s_grid[i];
		double R_sum = 0;
		double T_sum = 0;

		for( int j = -N_multipole; j <= N_multipole; j++ )
		{
			const double offset = j * L;

			// Method of images for a slab with extrapolated boundaries.
			//
			// Primary sources (j=0):
			//   Real:    charge +1 at z_r
			//   Virtual: charge -1 at -z_v
			//
			// Reflected about bottom boundary (z = d + d_e):
			//   Reflected real:    charge -1 at 2*(d+d_e) - z_r
			//   Reflected virtual: charge +1 at 2*(d+d_e) + z_v
			//
			// All four source types repeat with period L = 2*(d+2*d_e).
			//
			// Reflectance contribution from source at z_s with charge Q:
			//   Q * sign(z_s) * exp(-|z_s| * q)
			//
			// Transmittance contribution:
			//   Q * sign(d - z_s) * exp(-|d - z_s| * q)

			// Type A sources (same polarity as original dipole)
			{
				const double z_real = lp.z_r + offset;
				const double z_virt = -lp.z_v + offset;

				// Reflectance at z=0
				// Real (Q=+1):    +sign(z_real) * exp(-|z_real|*q)
				// Virtual (Q=-1): -sign(z_virt) * exp(-|z_virt|*q)
				const double e_real = FluxHankelContrib( fabs(z_real), lp.sigma_tr, s );
				const double e_virt = FluxHankelContrib( fabs(z_virt), lp.sigma_tr, s );

				const double sign_zr = (z_real >= 0) ? 1.0 : -1.0;
				const double sign_zv = (z_virt >= 0) ? 1.0 : -1.0;

				R_sum += sign_zr * e_real;			// Q=+1
				R_sum += -sign_zv * e_virt;			// Q=-1

				// Transmittance at z=d
				// Real (Q=+1):    +sign(d-z_real) * exp(-|d-z_real|*q)
				// Virtual (Q=-1): -sign(d-z_virt) * exp(-|d-z_virt|*q)
				const double sign_dr = (d - z_real >= 0) ? 1.0 : -1.0;
				const double sign_dv = (d - z_virt >= 0) ? 1.0 : -1.0;

				T_sum += sign_dr * FluxHankelContrib( fabs(d - z_real), lp.sigma_tr, s );
				T_sum += -sign_dv * FluxHankelContrib( fabs(d - z_virt), lp.sigma_tr, s );
			}

			// Type B sources (reflected about bottom boundary, opposite sign)
			{
				// Reflect real (Q=+1 at z_r) about z_bottom → Q=-1 at 2*z_bottom - z_r
				// Reflect virtual (Q=-1 at -z_v) about z_bottom → Q=+1 at 2*z_bottom + z_v
				const double z_real_refl = 2.0 * z_bottom - lp.z_r + offset;
				const double z_virt_refl = 2.0 * z_bottom + lp.z_v + offset;

				// Reflectance at z=0
				const double e_rr = FluxHankelContrib( fabs(z_real_refl), lp.sigma_tr, s );
				const double e_vr = FluxHankelContrib( fabs(z_virt_refl), lp.sigma_tr, s );

				const double sign_rr = (z_real_refl >= 0) ? 1.0 : -1.0;
				const double sign_vr = (z_virt_refl >= 0) ? 1.0 : -1.0;

				R_sum += -sign_rr * e_rr;			// Q=-1 (reflected real)
				R_sum += sign_vr * e_vr;			// Q=+1 (reflected virtual)

				// Transmittance at z=d
				const double sign_drr = (d - z_real_refl >= 0) ? 1.0 : -1.0;
				const double sign_dvr = (d - z_virt_refl >= 0) ? 1.0 : -1.0;

				T_sum += -sign_drr * FluxHankelContrib( fabs(d - z_real_refl), lp.sigma_tr, s );
				T_sum += sign_dvr * FluxHankelContrib( fabs(d - z_virt_refl), lp.sigma_tr, s );
			}

			// Early exit: if sources are far enough that contributions are negligible
			if( j > 0 )
			{
				const double q = sqrt( lp.sigma_tr * lp.sigma_tr + s * s );
				const double z_far = lp.z_r + j * L;
				if( exp(-z_far * q) < 1e-15 )
					break;
			}
		}

		R_tilde_out[i] = prefactor * R_sum;
		T_tilde_out[i] = prefactor * T_sum;

		// Clamp: small negative values can arise from numerical noise
		if( R_tilde_out[i] < 0 ) R_tilde_out[i] = 0;
		if( T_tilde_out[i] < 0 ) T_tilde_out[i] = 0;
	}
}

//=============================================================
// Layer stacking (adding-doubling)
//=============================================================

void RISE::StackLayersHankel(
	const double* R1, const double* T1,
	const double* R2, const double* T2,
	const double Ft_boundary,
	const int N_freq,
	double* R_out, double* T_out
	)
{
	const double Ft2 = Ft_boundary * Ft_boundary;

	for( int i = 0; i < N_freq; i++ )
	{
		const double denom = 1.0 - R1[i] * Ft2 * R2[i];

		if( fabs(denom) < 1e-30 )
		{
			R_out[i] = R1[i];
			T_out[i] = 0;
			continue;
		}

		const double inv_denom = 1.0 / denom;

		R_out[i] = R1[i] + T1[i] * Ft_boundary * R2[i] * Ft_boundary * T1[i] * inv_denom;
		T_out[i] = T1[i] * Ft_boundary * T2[i] * inv_denom;
	}
}

//=============================================================
// Composite multi-layer profile
//=============================================================

void RISE::ComputeCompositeProfileHankel(
	const LayerParams* layers,
	const int N_layers,
	const double* s_grid,
	const int N_freq,
	const int N_multipole,
	double* composite_R_out
	)
{
	if( N_layers <= 0 || N_freq <= 0 ) return;

	double* R_layer = new double[N_freq];
	double* T_layer = new double[N_freq];
	double* R_accum = new double[N_freq];
	double* T_accum = new double[N_freq];
	double* R_temp  = new double[N_freq];
	double* T_temp  = new double[N_freq];

	// Start from the bottom-most layer
	EvaluateMultipoleReflectanceHankel(
		layers[N_layers - 1], s_grid, N_freq,
		R_accum, T_accum, N_multipole );

	// Stack layers from bottom to top
	for( int L = N_layers - 2; L >= 0; L-- )
	{
		EvaluateMultipoleReflectanceHankel(
			layers[L], s_grid, N_freq,
			R_layer, T_layer, N_multipole );

		// Fresnel coupling at the boundary between layers[L] and layers[L+1]
		const double eta_ratio = layers[L + 1].ior / layers[L].ior;
		const double Fdr_boundary = ComputeFdr( eta_ratio );
		const double Ft_boundary = 1.0 - Fdr_boundary;

		StackLayersHankel(
			R_layer, T_layer,
			R_accum, T_accum,
			Ft_boundary,
			N_freq,
			R_temp, T_temp );

		double* swap;
		swap = R_accum; R_accum = R_temp; R_temp = swap;
		swap = T_accum; T_accum = T_temp; T_temp = swap;
	}

	for( int i = 0; i < N_freq; i++ )
	{
		composite_R_out[i] = R_accum[i];
	}

	delete[] R_layer;
	delete[] T_layer;
	delete[] R_accum;
	delete[] T_accum;
	delete[] R_temp;
	delete[] T_temp;
}
