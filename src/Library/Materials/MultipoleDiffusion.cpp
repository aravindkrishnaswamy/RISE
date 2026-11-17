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
#include <vector>

using namespace RISE;

//=============================================================
// Diffuse Fresnel reflectance
//=============================================================

double RISE::ComputeFdr( const double eta )
{
	// Polynomial fits for diffuse Fresnel reflectance (Jensen 2001, Table 1).
	// eta = n_medium / n_outside  (ratio at the boundary).
	if( eta < 1.0 )
	{
		// For eta < 1 (going from denser to rarer medium):
		//   Fdr = -0.4399 + 0.7099/eta - 0.3319/eta^2 + 0.0636/eta^3
		const double inv  = 1.0 / eta;
		const double inv2 = inv * inv;
		const double inv3 = inv2 * inv;
		return -0.4399 + 0.7099 * inv - 0.3319 * inv2 + 0.0636 * inv3;
	}

	// For eta >= 1 (going from rarer to denser medium):
	//   Fdr = -1.4399/eta^2 + 0.7099/eta + 0.6681 + 0.0636*eta
	return -1.4399 / (eta * eta) + 0.7099 / eta + 0.6681 + 0.0636 * eta;
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
		lp.d_e_bottom = lp.d_e;
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
	lp.d_e_bottom = lp.d_e;
	lp.slab_period = 2.0 * (lp.thickness + 2.0 * lp.d_e);
}

void RISE::RelativizeLayersToExterior( LayerParams* layers, const int N_layers, const double exteriorIOR )
{
	// Air (and any non-physical exterior, which callers already map to
	// air) leaves the stack untouched -- the in-air bit-identity pin.
	if( exteriorIOR == 1.0 || !( exteriorIOR > 0.0 ) || !std::isfinite( exteriorIOR ) ) {
		return;
	}
	for( int i = 0; i < N_layers; i++ )
	{
		layers[i].ior = layers[i].ior / exteriorIOR;
		ComputeLayerDerivedParams( layers[i] );
	}
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

	// Thin-layer regime: when the slab is thinner than one transport
	// mean free path (d < z_r = 1/sigma_t'), the dipole source falls
	// outside the slab, making the multipole expansion invalid.
	// In this regime the layer is optically thin to scattering — most
	// photons traverse it without scattering.  We approximate:
	//   R ≈ 0  (negligible diffuse back-scatter)
	//   T ≈ exp(-sigma_a * d)  (Beer-Lambert absorption only)
	// This is constant in s (no lateral spread), which is correct
	// for a sub-mean-free-path slab.
	if( d < lp.z_r )
	{
		const double T_beer = exp( -lp.sigma_a * d );
		for( int i = 0; i < N_freq; i++ )
		{
			R_tilde_out[i] = 0;
			T_tilde_out[i] = T_beer;
		}
		return;
	}

	const double prefactor = lp.alpha_prime / 2.0;

	// Method of images for a slab with extrapolated boundaries at
	// z_top = -d_e  and  z_bottom = d + d_e_bottom.
	//
	// The dipole pair (real at z_r, Q=+1; virtual at -z_v, Q=-1)
	// is reflected between the two boundaries, generating image
	// pairs at offsets j*L where L = 2*(d + d_e + d_e_bottom) is the full
	// period.  Since z_v = z_r + 2*d_e, the reflected-about-bottom
	// images at offset j duplicate the primary images at offset j±1,
	// so only the primary dipole pair is needed per period.
	//
	// Reflectance at z=0 from source at z_s with charge Q:
	//   Q * sign(z_s) * exp(-|z_s| * q)
	// Transmittance at z=d:
	//   Q * sign(d - z_s) * exp(-|d - z_s| * q)
	//
	// where q = sqrt(sigma_tr^2 + s^2).

	const double d_eff = d + lp.d_e + lp.d_e_bottom;
	const double L = 2.0 * d_eff;		// full period

	for( int i = 0; i < N_freq; i++ )
	{
		const double s = s_grid[i];
		// The image series is not uniformly convergent at q=0. Its exact
		// diffusion limit partitions conservative flux between both faces.
		if( lp.sigma_tr == 0.0 && s == 0.0 ) {
			R_tilde_out[i] = lp.alpha_prime * (d + lp.d_e_bottom - lp.z_r) / d_eff;
			T_tilde_out[i] = lp.alpha_prime * (lp.z_r + lp.d_e) / d_eff;
			continue;
		}
		double R_sum = 0;
		double T_sum = 0;

		for( int j = -N_multipole; j <= N_multipole; j++ )
		{
			const double offset = j * L;

			const double z_real = lp.z_r + offset;
			const double z_virt = -lp.z_v + offset;

			// Reflectance at z=0
			const double e_real = FluxHankelContrib( fabs(z_real), lp.sigma_tr, s );
			const double e_virt = FluxHankelContrib( fabs(z_virt), lp.sigma_tr, s );

			const double sign_zr = (z_real >= 0) ? 1.0 : -1.0;
			const double sign_zv = (z_virt >= 0) ? 1.0 : -1.0;

			R_sum += sign_zr * e_real;			// Q=+1 (real)
			R_sum += -sign_zv * e_virt;			// Q=-1 (virtual)

			// Transmittance at z=d
			const double sign_dr = (d - z_real >= 0) ? 1.0 : -1.0;
			const double sign_dv = (d - z_virt >= 0) ? 1.0 : -1.0;

			T_sum += sign_dr * FluxHankelContrib( fabs(d - z_real), lp.sigma_tr, s );
			T_sum += -sign_dv * FluxHankelContrib( fabs(d - z_virt), lp.sigma_tr, s );

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
	const double Ft_down,
	const double Ft_up,
	const int N_freq,
	double* R_out, double* T_out
	)
{
	const double Ft_product = Ft_down * Ft_up;

	for( int i = 0; i < N_freq; i++ )
	{
		const double denom = 1.0 - R1[i] * Ft_product * R2[i];

		if( fabs(denom) < 1e-30 )
		{
			R_out[i] = R1[i];
			T_out[i] = 0;
			continue;
		}

		const double inv_denom = 1.0 / denom;

		// Light goes down through T1, crosses interface (Ft_down), reflects
		// off lower stack (R2), crosses interface back up (Ft_up), through T1.
		R_out[i] = R1[i] + T1[i] * Ft_down * R2[i] * Ft_up * T1[i] * inv_denom;

		// Transmittance: light goes down through T1, crosses interface (Ft_down),
		// then through the lower stack (T2).
		T_out[i] = T1[i] * Ft_down * T2[i] * inv_denom;
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

	// A matched homogeneous interface is absent physically: do not insert
	// another dipole source or a Fresnel coupling at an arbitrary split.
	std::vector<LayerParams> slabs;
	for( int i = 0; i < N_layers; ++i ) {
		if( !slabs.empty() && slabs.back().ior == layers[i].ior &&
			slabs.back().sigma_a == layers[i].sigma_a &&
			slabs.back().sigma_sp == layers[i].sigma_sp ) {
			slabs.back().thickness += layers[i].thickness;
		} else {
			slabs.push_back(layers[i]);
		}
	}
	for( size_t i = 0; i < slabs.size(); ++i ) {
		LayerParams& lp = slabs[i];
		ComputeLayerDerivedParams(lp);
		const double eta_top = lp.ior / (i ? slabs[i-1].ior : 1.0);
		const double eta_bottom = lp.ior / (i+1 < slabs.size() ? slabs[i+1].ior : 1.0);
		lp.Fdr = eta_top == 1.0 ? 0.0 : ComputeFdr(eta_top);
		lp.A = (1.0 + lp.Fdr) / (1.0 - lp.Fdr + 1e-30);
		const double fb = eta_bottom == 1.0 ? 0.0 : ComputeFdr(eta_bottom);
		lp.d_e = 2.0 * lp.A * lp.D;
		lp.d_e_bottom = 2.0 * lp.D * (1.0 + fb) / (1.0 - fb + 1e-30);
		lp.z_v = lp.z_r + 2.0 * lp.d_e;
		// Donner & Jensen 2005, Eq. 16: distinct extrapolation planes.
		lp.slab_period = 2.0 * (lp.thickness + lp.d_e + lp.d_e_bottom);
	}

	std::vector<double> rup(N_freq), tdown(N_freq), rdown(N_freq), tup(N_freq), accum(N_freq), scratch(N_freq);
	EvaluateMultipoleReflectanceHankel(slabs.back(), s_grid, N_freq,
		accum.data(), scratch.data(), N_multipole);
	for( int L = static_cast<int>(slabs.size()) - 2; L >= 0; --L ) {
		const LayerParams& lp = slabs[L];
		EvaluateMultipoleReflectanceHankel(lp, s_grid, N_freq,
			rup.data(), tdown.data(), N_multipole);
		LayerParams reversed = lp;
		reversed.d_e = lp.d_e_bottom;
		reversed.d_e_bottom = lp.d_e;
		reversed.z_v = lp.z_r + 2.0 * reversed.d_e;
		EvaluateMultipoleReflectanceHankel(reversed, s_grid, N_freq,
			rdown.data(), tup.data(), N_multipole);
		const double eta = lp.ior / slabs[L+1].ior;
		const double ft = eta == 1.0 ? 1.0 :
			(1.0 - ComputeFdr(eta)) * (1.0 - ComputeFdr(1.0/eta));
		for( int i = 0; i < N_freq; ++i ) {
			const double denom = 1.0 - rdown[i] * ft * accum[i];
			accum[i] = fabs(denom) < 1e-30 ? rup[i] :
				rup[i] + tdown[i] * ft * accum[i] * tup[i] / denom;
		}
	}
	for( int i = 0; i < N_freq; ++i ) composite_R_out[i] = accum[i];
}
