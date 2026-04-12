//////////////////////////////////////////////////////////////////////
//
//  SSSCoefficients.h - Utilities for converting between artist-friendly
//    subsurface scattering parameters and volumetric scattering
//    coefficients used by random-walk SSS.
//
//  Conversions:
//    ConvertBurleyToVolume:
//      From (albedo A, mean free path d, HG asymmetry g) to
//      (sigma_a, sigma_s, sigma_t) using the Christensen & Burley
//      (2015) scaling factor:
//        s = 1.9 - A + 3.5 * (A - 0.8)^2
//        sigma_t = s / d
//        sigma_s = A * sigma_t
//        sigma_a = (1 - A) * sigma_t
//
//      When g != 0, the similarity relation is applied:
//        sigma_s' = sigma_s * (1 - g)
//        sigma_t' = sigma_s' + sigma_a
//      This de-correlates the anisotropic scattering into an
//      equivalent isotropic medium with reduced scattering.
//
//  Reference:
//    Christensen, P. H. and Burley, B. (2015).
//    "Approximate Reflectance Profiles for Efficient Subsurface
//    Scattering." Pixar Technical Memo #15-04.
//
//    Chiang, M. J. and Burley, B. et al. (2016).
//    "A Practical and Controllable Hair and Fur Model for Production
//    Path Tracing." SIGGRAPH 2016.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SSS_COEFFICIENTS_
#define SSS_COEFFICIENTS_

#include "Math3D/Math3D.h"

namespace RISE
{
	namespace SSSCoefficients
	{
		/// Computes the Burley scaling factor s for a given single-scatter
		/// albedo A.  This maps [0,1] albedo to the scaling constant used
		/// in the normalized diffusion parameterization.
		///
		/// s(A) = 1.9 - A + 3.5 * (A - 0.8)^2
		///
		/// \param A  Single-scatter albedo in [0, 1]
		/// \return   Scaling factor s
		inline Scalar BurleyScalingFactor( const Scalar A )
		{
			const Scalar t = A - 0.8;
			return 1.9 - A + 3.5 * t * t;
		}

		/// Converts artist-friendly Burley parameters to volumetric
		/// scattering coefficients.
		///
		/// \param A        Single-scatter albedo (sigma_s / sigma_t) per channel
		/// \param d        Mean free path (1 / sigma_t) per channel, in world units
		/// \param g        Henyey-Greenstein asymmetry factor (-1 to 1).
		///                 When g != 0, the similarity relation is applied.
		/// \param sigma_a  [out] Absorption coefficient per channel
		/// \param sigma_s  [out] Scattering coefficient per channel
		/// \param sigma_t  [out] Extinction coefficient per channel
		inline void ConvertBurleyToVolume(
			const RISEPel& A,
			const RISEPel& d,
			const Scalar g,
			RISEPel& sigma_a,
			RISEPel& sigma_s,
			RISEPel& sigma_t
			)
		{
			for( int ch = 0; ch < 3; ch++ )
			{
				const Scalar Ach = (A[ch] < 0) ? 0 : ((A[ch] > 1.0) ? 1.0 : A[ch]);
				const Scalar dch = (d[ch] > 1e-20) ? d[ch] : 1e-20;

				const Scalar s = BurleyScalingFactor( Ach );
				const Scalar st = s / dch;
				const Scalar ss = Ach * st;
				const Scalar sa = st - ss;

				if( fabs(g) > 1e-6 )
				{
					// Similarity relation: reduce scattering by (1-g),
					// keeping absorption unchanged.  Use signed g so
					// that backward scattering (g < 0) increases the
					// reduced coefficient as physically expected.
					const Scalar ss_reduced = ss * (1.0 - g);
					sigma_s[ch] = ss_reduced;
					sigma_a[ch] = sa;
					sigma_t[ch] = ss_reduced + sa;
				}
				else
				{
					sigma_s[ch] = ss;
					sigma_a[ch] = sa;
					sigma_t[ch] = st;
				}
			}
		}

		/// Simple conversion from raw (sigma_a, sigma_s) to sigma_t.
		/// Useful when the caller already has physical coefficients.
		///
		/// \param sigma_a  Absorption coefficient per channel
		/// \param sigma_s  Scattering coefficient per channel
		/// \param sigma_t  [out] Extinction coefficient per channel
		inline void FromCoefficients(
			const RISEPel& sigma_a,
			const RISEPel& sigma_s,
			RISEPel& sigma_t
			)
		{
			sigma_t[0] = sigma_a[0] + sigma_s[0];
			sigma_t[1] = sigma_a[1] + sigma_s[1];
			sigma_t[2] = sigma_a[2] + sigma_s[2];
		}
	}
}

#endif
