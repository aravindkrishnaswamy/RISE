//////////////////////////////////////////////////////////////////////
//
//  DiffusionApproximationExtinction.h - Dipole diffusion BSSRDF
//
//  Implements the dipole diffusion approximation for subsurface
//  scattering from Jensen et al., "A Practical Model for Subsurface
//  Light Transport" (SIGGRAPH 2001).
//
//  The model places two point sources — a positive "real" source at
//  depth zr below the surface and a negative "virtual" source at
//  depth zv above — to satisfy the zero-fluence boundary condition
//  at a semi-infinite half-space.  The resulting Rd(r) profile gives
//  the diffuse reflectance as a function of surface distance r.
//
//  Key equations (Jensen 2001, Section 4):
//    sigma_t' = sigma_s' + sigma_a  (reduced extinction)
//    sigma_s' = sigma_s * (1 - g)   (similarity relation)
//    D  = 1 / (3 * sigma_t')        (diffusion coefficient)
//    zr = 1 / sigma_t'              (real source depth = 1 mfp)
//    zv = zr + 4*A*D                (virtual source depth)
//    sigma_tr = sqrt(3 * sigma_a * sigma_t')  (effective transport)
//    A  = (1 + Fdr) / (1 - Fdr)     (boundary mismatch term)
//  where Fdr is the average diffuse Fresnel reflectance, approximated
//  by the polynomial fit from Egan & Hilgeman 1973.
//
//  All internal distances are in millimeters.  Scene-space distances
//  (meters) are converted via: r_mm = r_meters * 1000 * geometric_scale.
//  The geometric_scale parameter lets scene authors map real-world
//  optical properties to arbitrary mesh scales.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIFFUSION_APPROXIMATION_EXTINCTION_H
#define DIFFUSION_APPROXIMATION_EXTINCTION_H

#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Utilities/Reference.h"
#include "../../Materials/MultipoleDiffusion.h"		// ComputeFdr's eta < 1 fit (DL-291)

namespace RISE
{
	namespace Implementation
	{
		class DiffusionApproximationExtinction :
			public virtual ISubSurfaceExtinctionFunction,
			public virtual Reference
		{
		protected:
			const RISEPel absorption;
			const RISEPel scattering;
			const Scalar relative_ior;
			const Scalar g;
			const Scalar geometric_scale;

			// Precomputed values
			RISEPel s_prime;
			RISEPel t_prime;
			RISEPel alpha_prime;

			RISEPel zr;
			RISEPel zv;
			RISEPel D;			///< Diffusion coefficient (kept for per-exterior zv, DL-291)

			RISEPel tr;

			RISEPel ap_ov_four_pi;

		public:
			DiffusionApproximationExtinction(
				const RISEPel absorption_,
				const RISEPel scattering_,
				const Scalar relative_ior_,
				const Scalar g_,
				const Scalar geometric_scale_
				) :
				absorption( absorption_ ),
				scattering( scattering_ ),
				relative_ior( relative_ior_ ),
				g( g_ ),
				geometric_scale( geometric_scale_ )
			{
				// Similarity relation: reduce the scattering coefficient to account
				// for the anisotropy of the phase function (Wyman et al. 1989)
				s_prime = scattering * (1.0-g);
				t_prime = s_prime + absorption;
				alpha_prime = s_prime / t_prime;

				// Boundary condition: Fdr is the average diffuse Fresnel reflectance
				// at the surface.  The polynomial fit is from Egan & Hilgeman 1973.
				// A controls how far the virtual source sits above the surface —
				// higher IOR pushes more light back in, deepening the virtual source.
				// The value built here is for an AIR exterior; see
				// ComputeTotalExtinctionForExterior for any other (DL-291).
				// The nu >= 1 arm is the original expression, verbatim (so an
				// in-air body is bit-identical); see BoundaryA for nu < 1.
				const Scalar nu = relative_ior;
				const Scalar fresnel = -1.440/(nu*nu) + 0.710/nu + 0.668 + 0.0636*nu;
				const Scalar A = (1.0+fresnel)/(1.0-fresnel);

				const RISEPel Dcoef = RISEPel(1,1,1) / (3.0*t_prime);
				zr = RISEPel(1,1,1) / t_prime;
				zv = zr + 4.0*A*Dcoef;
				D = Dcoef;

				// Below relative index 1 the polynomial above is out of range
				// (it goes negative); BoundaryA switches to the eta < 1 fit.
				if( nu < 1.0 ) {
					zv = zr + 4.0*BoundaryA( nu )*D;
				}

				// Effective transport coefficient — controls the exponential falloff
				// rate of the Rd(r) profile.  Per-channel to produce color bleeding.
				tr = ColorMath::root(3.0*absorption*t_prime);

				ap_ov_four_pi = alpha_prime / RISEPel(FOUR_PI,FOUR_PI,FOUR_PI);
			}

			virtual ~DiffusionApproximationExtinction()
			{
			}

			/// The dipole boundary term A = (1+Fdr)/(1-Fdr) at relative
			/// index `nu` (interior / exterior).
			///
			/// nu >= 1: this class's own Egan & Hilgeman fit, exactly as it
			/// has always been evaluated (every in-air body; bit-identical).
			/// nu < 1: that polynomial is outside its range -- it goes
			/// NEGATIVE (-0.051 at 1.3 inside water, -0.375 inside 1.5 glass,
			/// against the true 0.007 / 0.036) -- so the separate eta < 1 fit
			/// (Jensen 2001; MultipoleDiffusion's ComputeFdr) is used.  Only
			/// reachable since DL-291 made nu relative to the live exterior.
			/// The two fits meet at nu = 1 to 1e-4 in Fdr (0.0016 vs 0.0017),
			/// below either fit's own error against the Fresnel integral.
			static Scalar BoundaryA( const Scalar nu )
			{
				const Scalar fresnel = ( nu >= 1.0 )
					? ( -1.440/(nu*nu) + 0.710/nu + 0.668 + 0.0636*nu )
					: Scalar( ComputeFdr( nu ) );
				return (1.0+fresnel)/(1.0-fresnel);
			}

			/// DL-291: the authored `relative_ior` is the material's index
			/// against AIR (the chunk's `ior`); the dipole's boundary term is
			/// a function of the index relative to the medium the body actually
			/// sits in, so a non-air exterior moves the virtual source.  In air
			/// this is ComputeTotalExtinction exactly.
			RISEPel ComputeTotalExtinctionForExterior(
				const Scalar distance,
				const Scalar exteriorIOR
			) const
			{
				if( exteriorIOR == 1.0 || !( exteriorIOR > 0 ) || !( exteriorIOR < RISE_INFINITY ) ) {
					return ComputeTotalExtinction( distance );
				}
				const Scalar A = BoundaryA( relative_ior / exteriorIOR );
				return EvaluateDipole( distance, zr + 4.0*A*D );
			}

			virtual Scalar GetMaximumDistanceForError(
				const Scalar error
			) const
			{
				if( error > 0 ) {
					// Rd(r) decays as exp(-sigma_tr * r), so the max distance
					// for a given error tolerance is r_max = -ln(error) / sigma_tr.
					// Convert back from mm to meters (divide by 1000*geometric_scale).
					return -log(error) / (ColorMath::MinValue(tr)*geometric_scale) / 1000.0;
				}

				return RISE_INFINITY;
			}

			/// Evaluates the dipole Rd(r) profile (Jensen 2001, Eq. 6).
			/// Returns the diffuse reflectance contribution at surface distance
			/// `distance` (in meters) from the illumination point.
			///
			/// The formula uses the gradient of the fluence from real and virtual
			/// point sources.  dr and dv are the 3D distances from the evaluation
			/// point to the real and virtual sources respectively:
			///   dr = sqrt(r^2 + zr^2),  dv = sqrt(r^2 + zv^2)
			///
			/// Rd(r) = (alpha'/4pi) * [ zr*(sigma_tr*dr+1)*exp(-sigma_tr*dr)/dr^3
			///                         + zv*(sigma_tr*dv+1)*exp(-sigma_tr*dv)/dv^3 ]
			RISEPel ComputeTotalExtinction(
				const Scalar distance
			) const
			{
				// The original evaluation, verbatim (the in-air path, DL-291);
				// EvaluateDipole below is the same formula at another zv.
				const Scalar r = distance * 1000.0 * geometric_scale;
				const Scalar r2 = r*r;
				const RISEPel sqr_r( r2, r2, r2 );

				const RISEPel one(1,1,1);
				const RISEPel negative(-1,-1,-1);

				RISEPel dv = ColorMath::root( sqr_r + (zv*zv) );
				RISEPel dr = ColorMath::root( sqr_r + (zr*zr) );

				const RISEPel inside_left = zr*(tr*dr+one) * ( ColorMath::exponential(negative*tr*dr)/(dr*dr*dr) );
				const RISEPel inside_right = zv*(tr*dv+one) * ( ColorMath::exponential(negative*tr*dv)/(dv*dv*dv) );

				return ap_ov_four_pi * (inside_left+inside_right);
			}

		protected:
			/// Jensen 2001 Eq. 6 with the given virtual-source depth.
			RISEPel EvaluateDipole(
				const Scalar distance,
				const RISEPel& zvIn
			) const
			{
				const Scalar r = distance * 1000.0 * geometric_scale;
				const Scalar r2 = r*r;
				const RISEPel sqr_r( r2, r2, r2 );

				const RISEPel one(1,1,1);
				const RISEPel negative(-1,-1,-1);

				RISEPel dv = ColorMath::root( sqr_r + (zvIn*zvIn) );
				RISEPel dr = ColorMath::root( sqr_r + (zr*zr) );

				const RISEPel inside_left = zr*(tr*dr+one) * ( ColorMath::exponential(negative*tr*dr)/(dr*dr*dr) );
				const RISEPel inside_right = zvIn*(tr*dv+one) * ( ColorMath::exponential(negative*tr*dv)/(dv*dv*dv) );

				return ap_ov_four_pi * (inside_left+inside_right);
			}
		};
	}
}

#endif
