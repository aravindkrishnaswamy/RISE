//////////////////////////////////////////////////////////////////////
//
//  TrianglePixelFilter.h - A pixel filter which scales
//  the weight of the values (sqrt) as they get further from 
//  the center of the pixel
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 5, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef TRIANGLEPIXELFILTER_
#define TRIANGLEPIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Definition of the triangle pixel filter
	//
	namespace Implementation
	{
		class TrianglePixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			Scalar		dKernelWidthOV2;
			Scalar		dKernelHeightOV2;

			virtual ~TrianglePixelFilter( ){};

		public:
			TrianglePixelFilter( const Scalar width, const Scalar height )
			{
				dKernelWidth = width;
				dKernelHeight = height;
				dKernelWidthOV2 = width*0.5;
				dKernelHeightOV2 = height*0.5;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				if (canonical.x >= 0.5) {
					warped.x = (1.5 - sqrt(2.0 - 2.0*canonical.x)) - 0.5;
				} else {
					warped.x = (-0.5 + sqrt(2.0*canonical.x)) - 0.5;
				}

				if (canonical.y >= 0.5) {
					warped.y = (1.5 - sqrt(2.0 - 2.0*canonical.y)) - 0.5;
				} else {
					warped.y = (-0.5 + sqrt(2.0*canonical.y)) - 0.5;
				}

				warped.x *= dKernelWidthOV2;
				warped.y *= dKernelHeightOV2;

				return 1.0;
			}

			// Separable tent: (1 - |dx|/halfW) * (1 - |dy|/halfH)
			// inside support, zero outside.  The unnormalised 2D
			// integral is halfW * halfH, so we divide by that so the
			// kernel has unit integral — required because the splat
			// film's resolve scales by a global sample count, not per-
			// pixel weight.  Before this override, EvaluateFilter fell
			// through to IPixelFilter's default (returns 0) and every
			// splat from a tent filter wider than 1 pixel was dropped
			// silently.
			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				const Scalar ax = fabs( dx );
				const Scalar ay = fabs( dy );
				if( ax >= dKernelWidthOV2 || ay >= dKernelHeightOV2 ) {
					return 0;
				}
				const Scalar tx = 1.0 - ax / dKernelWidthOV2;
				const Scalar ty = 1.0 - ay / dKernelHeightOV2;
				return ( tx * ty ) / ( dKernelWidthOV2 * dKernelHeightOV2 );
			}
		};
	}
}

#endif
