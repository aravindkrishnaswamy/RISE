//////////////////////////////////////////////////////////////////////
//
//  HankelTransform.h - Utilities for zeroth-order Hankel transforms
//  used in multipole diffusion layer stacking.
//
//  Provides:
//    - J0_approx(x): Bessel function of the first kind, order zero
//    - HankelGrid: log-spaced frequency grid for Hankel transforms
//    - InverseHankelTransform: numerical inverse via trapezoidal rule
//
//  The Hankel transform of order zero is:
//    F_tilde(s) = integral_0^inf f(r) * J0(s*r) * r * dr
//  and its inverse:
//    f(r) = integral_0^inf F_tilde(s) * J0(s*r) * s * ds
//
//  Reference: Abramowitz & Stegun, Handbook of Mathematical Functions,
//  Section 9.4 (rational polynomial approximations for J0).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HANKEL_TRANSFORM_H
#define HANKEL_TRANSFORM_H

#include "Math3D/Math3D.h"
#include <cmath>

namespace RISE
{
	/// Bessel function J0(x) via rational polynomial approximation.
	/// Abramowitz & Stegun 9.4.1 (|x| <= 3) and 9.4.3 (|x| > 3).
	/// Accuracy: ~8 significant digits.
	inline Scalar J0_approx( const Scalar x )
	{
		const Scalar ax = fabs( x );

		if( ax <= 3.0 )
		{
			// A&S 9.4.1: polynomial in (x/3)^2
			const Scalar y = (x / 3.0) * (x / 3.0);
			return 1.0
				+ y * (-2.2499997
				+ y * ( 1.2656208
				+ y * (-0.3163866
				+ y * ( 0.0444479
				+ y * (-0.0039444
				+ y *   0.0002100)))));
		}
		else
		{
			// A&S 9.4.3: asymptotic expansion
			const Scalar y = 3.0 / ax;
			const Scalar f0 = 0.79788456
				+ y * (-0.00000077
				+ y * (-0.00552740
				+ y * (-0.00009512
				+ y * ( 0.00137237
				+ y * (-0.00072805
				+ y *   0.00014476)))));
			const Scalar theta0 = ax - 0.78539816
				+ y * (-0.04166397
				+ y * (-0.00003954
				+ y * ( 0.00262573
				+ y * (-0.00054125
				+ y * (-0.00029333
				+ y *   0.00013558)))));
			return f0 * cos( theta0 ) / sqrt( ax );
		}
	}

	/// Log-spaced frequency grid for Hankel transforms.
	struct HankelGrid
	{
		int		N;				///< Number of grid points
		Scalar*	s;				///< Frequency values s_i
		Scalar*	ds;				///< Integration weights (trapezoidal)

		HankelGrid() : N(0), s(0), ds(0) {}

		~HankelGrid()
		{
			delete[] s;
			delete[] ds;
		}

		/// Create a log-spaced grid from s_min to s_max with N points.
		/// Computes trapezoidal weights for numerical integration.
		void Create( const Scalar s_min, const Scalar s_max, const int numPoints )
		{
			delete[] s;
			delete[] ds;

			N = numPoints;
			s = new Scalar[N];
			ds = new Scalar[N];

			const Scalar log_min = log( s_min );
			const Scalar log_max = log( s_max );
			const Scalar log_step = (log_max - log_min) / (N - 1);

			for( int i = 0; i < N; i++ )
			{
				s[i] = exp( log_min + i * log_step );
			}

			// Trapezoidal weights on log-spaced grid:
			// ds_i = (s_{i+1} - s_{i-1}) / 2 for interior points
			ds[0] = (s[1] - s[0]) * 0.5;
			for( int i = 1; i < N - 1; i++ )
			{
				ds[i] = (s[i+1] - s[i-1]) * 0.5;
			}
			ds[N-1] = (s[N-1] - s[N-2]) * 0.5;
		}

		/// Evaluate the inverse Hankel transform at radius r:
		///   f(r) = integral_0^inf F_tilde(s) * J0(s*r) * s * ds
		/// using the trapezoidal rule on this grid.
		Scalar InverseTransform(
			const Scalar* F_tilde,
			const Scalar r
			) const
		{
			Scalar result = 0;
			for( int i = 0; i < N; i++ )
			{
				result += F_tilde[i] * J0_approx( s[i] * r ) * s[i] * ds[i];
			}
			return result;
		}

	private:
		// Non-copyable
		HankelGrid( const HankelGrid& );
		HankelGrid& operator=( const HankelGrid& );
	};
}

#endif
