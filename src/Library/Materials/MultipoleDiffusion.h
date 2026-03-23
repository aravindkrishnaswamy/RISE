//////////////////////////////////////////////////////////////////////
//
//  MultipoleDiffusion.h - Per-layer multipole diffusion for layered
//  translucent materials (Donner & Jensen 2005).
//
//  Provides:
//    - LayerParams: per-layer optical properties and derived values
//    - Multipole reflectance/transmittance in Hankel (frequency) domain
//    - Adding-doubling layer stacking in Hankel domain
//    - Composite profile pipeline for multi-layer skin stacks
//
//  The multipole method generalizes the Jensen 2001 dipole to finite
//  slabs by placing multiple dipole pairs at periodic mirror image
//  positions.  Layer stacking is performed in the zeroth-order Hankel
//  transform domain where convolution becomes multiplication.
//
//  References:
//    Donner & Jensen 2005 — Light Diffusion in Multi-Layered
//      Translucent Materials
//    Jensen et al. 2001 — A Practical Model for Subsurface Light
//      Transport
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MULTIPOLE_DIFFUSION_H
#define MULTIPOLE_DIFFUSION_H

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	/// Optical properties for a single homogeneous slab layer.
	struct LayerParams
	{
		// Primary parameters (set by caller)
		double	sigma_a;		///< Absorption coefficient (cm^-1)
		double	sigma_sp;		///< Reduced scattering coefficient (cm^-1)
		double	thickness;		///< Layer thickness (cm)
		double	ior;			///< Index of refraction

		// Derived parameters (computed by ComputeLayerDerivedParams)
		double	sigma_t_prime;	///< sigma_a + sigma_sp
		double	D;				///< Diffusion coefficient = 1/(3*sigma_t')
		double	sigma_tr;		///< Effective transport = sqrt(3*sigma_a*sigma_t')
		double	alpha_prime;	///< Reduced albedo = sigma_sp / sigma_t'
		double	Fdr;			///< Diffuse Fresnel reflectance at boundary
		double	A;				///< Boundary condition = (1+Fdr)/(1-Fdr)
		double	z_r;			///< Real source depth = 1/sigma_t'
		double	z_v;			///< Virtual source depth = z_r + 4*A*D
		double	d_e;			///< Extrapolated boundary = 2*A*D
		double	slab_period;	///< 2*(thickness + 2*d_e) — multipole spacing
	};

	/// Compute derived parameters from primary parameters.
	void ComputeLayerDerivedParams( LayerParams& lp );

	/// Compute diffuse Fresnel reflectance for a given IOR ratio.
	/// Uses Egan & Hilgeman 1973 polynomial fit (same as Jensen 2001).
	double ComputeFdr( double eta );

	/// Evaluate single-layer multipole reflectance and transmittance
	/// in the Hankel domain at each frequency point.
	///
	/// For reflectance, the profile is evaluated at z=0 (top surface).
	/// For transmittance, it is evaluated at z=thickness (bottom surface).
	///
	/// Each dipole pair (real at z_r_j, virtual at z_v_j) contributes
	/// to R_tilde(s) via the analytic Hankel transform:
	///   z * exp(-|z| * sqrt(sigma_tr^2 + s^2))
	///
	/// \param lp            Layer with derived params already computed
	/// \param s_grid        Hankel frequency grid, length N_freq
	/// \param N_freq        Number of frequency samples
	/// \param R_tilde_out   Output reflectance in Hankel domain, length N_freq
	/// \param T_tilde_out   Output transmittance in Hankel domain, length N_freq
	/// \param N_multipole   Number of multipole pairs per side (total = 2*N+1)
	void EvaluateMultipoleReflectanceHankel(
		const LayerParams& lp,
		const double* s_grid,
		int N_freq,
		double* R_tilde_out,
		double* T_tilde_out,
		int N_multipole
		);

	/// Stack two layers in the Hankel domain using the adding-doubling
	/// recurrence.  Accounts for Fresnel coupling at the internal
	/// boundary via separate downward and upward diffuse Fresnel
	/// transmission factors.
	///
	/// The combined reflectance and transmittance are:
	///   R = R1 + T1 * Ft_down * R2 * Ft_up * T1 / denom
	///   T = T1 * Ft_down * T2 / denom
	///   denom = 1 - R1 * Ft_down * Ft_up * R2
	///
	/// Ft_down = 1 - Fdr(n_top / n_bottom)  (light exiting top layer downward)
	/// Ft_up   = 1 - Fdr(n_bottom / n_top)  (light exiting bottom layer upward)
	///
	/// \param R1, T1        Top layer Hankel R/T, length N_freq
	/// \param R2, T2        Bottom layer Hankel R/T, length N_freq
	/// \param Ft_down       Diffuse Fresnel transmission, top→bottom
	/// \param Ft_up         Diffuse Fresnel transmission, bottom→top
	/// \param N_freq        Number of frequency samples
	/// \param R_out, T_out  Combined R/T output, length N_freq
	void StackLayersHankel(
		const double* R1, const double* T1,
		const double* R2, const double* T2,
		double Ft_down,
		double Ft_up,
		int N_freq,
		double* R_out, double* T_out
		);

	/// Compute the composite reflectance profile for a multi-layer
	/// stack in the Hankel domain.
	///
	/// Stacks layers from bottom to top using adding-doubling.
	/// The outermost (top) layer is layers[0], innermost (bottom)
	/// is layers[N_layers-1].
	///
	/// \param layers          Array of layer params (top to bottom)
	/// \param N_layers        Number of layers (typically 4 for skin)
	/// \param s_grid          Hankel frequency grid, length N_freq
	/// \param N_freq          Number of frequency samples
	/// \param N_multipole     Multipole pairs per side
	/// \param composite_R_out Composite reflectance in Hankel domain, length N_freq
	void ComputeCompositeProfileHankel(
		const LayerParams* layers,
		int N_layers,
		const double* s_grid,
		int N_freq,
		int N_multipole,
		double* composite_R_out
		);
}

#endif
