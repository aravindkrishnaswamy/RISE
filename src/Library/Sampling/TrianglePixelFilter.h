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
		};
	}
}

#endif
