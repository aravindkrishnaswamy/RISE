//////////////////////////////////////////////////////////////////////
//
//  GaussianPixelFilter.h - A pixel filter which scales the values
//  according to the normal distribution
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 9, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef GAUSSIANPIXELFILTER_
#define GAUSSIANPIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"
#include <algorithm>
#include <cmath>

//
// Definition of the gaussian(normal) pixel filter
//
namespace RISE
{
	namespace Implementation
	{
		class GaussianPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			Scalar		dSigma;
			Scalar		dTruncationBoundary;	///< exp(-K²/2) where K = radiusMax/sigma; the lower bound of the remapped uniform in the inverse-CDF radius sampler

			virtual ~GaussianPixelFilter( ){};

		public:
			GaussianPixelFilter( const Scalar size, const Scalar sigma ) :
			  dSigma( sigma )
			{
				dKernelWidth = size;
				dKernelHeight = size;

				// For the warp, we truncate the sampled radius to the
				// inscribed disk of radius min(halfW, halfH) so every
				// sample lands inside the kernel's rectangular support.
				// Pre-compute exp(-K²/2) where K is the radius bound in
				// standard-normal units; this is the lower limit of the
				// remapped uniform we feed into the radial CDF inverse
				// (see the warp() comment for the derivation).
				const Scalar radiusMax = std::min( dKernelWidth, dKernelHeight ) * Scalar( 0.5 );
				const Scalar K = ( dSigma > Scalar( 0 ) ) ? ( radiusMax / dSigma ) : Scalar( 0 );
				dTruncationBoundary = std::exp( -K * K * Scalar( 0.5 ) );
			}

			// Truncated 2D Gaussian: exp(-r²/(2σ²)) / (2πσ²) inside the
			// square support, zero outside.  Normalised as an UNtruncated
			// Gaussian for simplicity — truncating a Gaussian at ≥ 2σ
			// captures >95% of the mass, so the unit-integral assumption
			// of SplatFilm::Resolve is satisfied to within a few percent.
			// Without this override the filter returned 0 for every splat
			// whenever the kernel size exceeded ~1 pixel (which is every
			// reasonable Gaussian configuration), silently producing a
			// black image.
			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				const Scalar halfW = dKernelWidth  * 0.5;
				const Scalar halfH = dKernelHeight * 0.5;
				if( fabs( dx ) > halfW || fabs( dy ) > halfH ) {
					return 0;
				}
				const Scalar r2       = dx * dx + dy * dy;
				const Scalar twoSigma2 = 2.0 * dSigma * dSigma;
				const Scalar norm      = 1.0 / ( PI * twoSigma2 );
				return norm * exp( -r2 / twoSigma2 );
			}

			// Importance-sample the Gaussian kernel using the inverse
			// CDF of the truncated radial distribution.  Derivation:
			//
			//   A 2D isotropic N(0, σ²) has radial marginal
			//     f_r(x) = (x/σ²) exp(-x²/(2σ²)),  x ≥ 0
			//   with CDF F_r(x) = 1 - exp(-x²/(2σ²)).
			//   Inverting:  x = σ sqrt(-2 ln(1 - u))  for u ∈ [0, 1).
			//
			//   To truncate to a disk of radius R = σ·K (inscribed in
			//   the square kernel), restrict u to [0, F_r(R)] =
			//   [0, 1 - exp(-K²/2)].  Equivalently, let v = 1 - u map
			//   to [T, 1] where T = exp(-K²/2), then
			//     x = σ sqrt(-2 ln v).
			//   Remapping canonical.x ∈ [0, 1) uniformly onto [T, 1]
			//   preserves stratification.
			//
			//   Angular coordinate is uniform on [0, 2π), sourced from
			//   canonical.y to keep the warp deterministic from the
			//   caller's stratified pair.
			//
			// Returned weight is 1 because this is a perfect
			// importance-sample of the kernel — callers that combine
			// warp() with per-pixel accumulation (rather than a
			// filter-aware splat) get the filter integral estimator
			// automatically.
			//
			// The `rng` argument is unused here; the warp is driven
			// entirely by the caller's stratified canonical sample so
			// that low-discrepancy pixel samplers stay coherent.
			virtual Scalar warp( const RandomNumberGenerator& /*rng*/, const Point2& canonical, Point2& warped ) const
			{
				// Remap canonical.x from [0, 1) to [T, 1] so that
				// sqrt(-2 log v) stays bounded by K.
				// Bias canonical.x away from 1 to avoid log(0) if it
				// arrives as exactly 1; the 1 - canonical.x form maps
				// the standard [0, 1) convention onto the inverse-CDF
				// output in one step.
				const Scalar v = dTruncationBoundary +
					( Scalar( 1 ) - dTruncationBoundary ) * canonical.x;
				const Scalar vSafe = std::max( v, Scalar( 1e-30 ) );
				const Scalar r = dSigma * std::sqrt( -Scalar( 2 ) * std::log( vSafe ) );
				const Scalar theta = TWO_PI * canonical.y;

				warped.x = r * std::cos( theta );
				warped.y = r * std::sin( theta );

				return Scalar( 1 );
			}
		};
	}
}

#endif
