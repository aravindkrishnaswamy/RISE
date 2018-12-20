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
			Scalar		dKernelSizeSigmaRatioExp;
			Scalar		dSigma;

			virtual ~GaussianPixelFilter( ){};

		public:

			// We choose to perform the most basic form of the 
			// Box-Muller transform.  I realize we could use the polar form of the transform...

			GaussianPixelFilter( const Scalar size, const Scalar sigma ) : 
			  dSigma( sigma )
			{
				dKernelWidth = size;
				dKernelHeight = size;

				Scalar	dKernelSizeSigmaRatio = size / dSigma;

				dKernelSizeSigmaRatioExp = exp( dKernelSizeSigmaRatio*dKernelSizeSigmaRatio*(-0.5) );
			}

			virtual Scalar warp( const RandomNumberGenerator& rng, const Point2& canonical, Point2& warped ) const
			{
				Scalar r = rng.CanonicalRandom() * dKernelSizeSigmaRatioExp;
				Scalar a = rng.CanonicalRandom();

				Scalar sqrt_log = sqrt(-2.0*log(r));
				Scalar two_pi_a = TWO_PI*a;

				warped.x = dSigma * (sqrt_log * cos(two_pi_a));
				warped.y = dSigma * (sqrt_log * sin(two_pi_a));

				/* Polar Box-Muller with two independant variables with sigma of 1
				Scalar x1, x2, w, y1, y2;
				do {
					 x1 = 2.0 * rng.CanonicalRandom() - 1.0;
					 x2 = 2.0 * rng.CanonicalRandom() - 1.0;
					 w = x1 * x1 + x2 * x2;
				} while ( w >= 1.0 );

				w = sqrt( (-2.0 * log( w ) ) / w );

				warped.x = canonical.x * w;
				warped.y = canonical.y * w;
				*/

				return 1.0;
			}
		};
	}
}

#endif
