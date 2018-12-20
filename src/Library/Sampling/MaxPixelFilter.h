//////////////////////////////////////////////////////////////////////
//
//  MaxPixelFilter.h - A pixel filter which scales the values
//  according to a sinc filter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 2, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef MAXPIXELFILTER_
#define MAXPIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Definition of the sinc pixel filter
	//
	namespace Implementation
	{
		class MaxPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			Scalar		dKernelWidthOV2;
			Scalar		dKernelHeightOV2;

			Scalar		s_x;
			Scalar		s_y;

			Scalar		tx_minus_sx;
			Scalar		ty_minus_sy;

			virtual ~MaxPixelFilter( ){};

		public:
			MaxPixelFilter( const Scalar width, const Scalar height, const Scalar sx, const Scalar sy ) : 
			dKernelWidthOV2( width/2 ),
			dKernelHeightOV2( height/2 ),
			s_x( sx ),
			s_y( sy )
			{
				dKernelWidth = width;
				dKernelHeight = height;

				tx_minus_sx = dKernelWidthOV2 - s_x;
				ty_minus_sy = dKernelHeightOV2 - s_y;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*dKernelWidth - dKernelWidthOV2, canonical.y*dKernelHeight - dKernelHeightOV2 );

				// Compute the weight
				const Scalar weight_x = fabs(warped.x) < s_x ? (1.0 - (warped.x*warped.x)/(s_x*dKernelWidthOV2)) : ((dKernelWidthOV2-fabs(warped.x))*(dKernelWidthOV2-fabs(warped.x)) / (dKernelWidthOV2*tx_minus_sx));
				const Scalar weight_y = fabs(warped.y) < s_y ? (1.0 - (warped.y*warped.y)/(s_x*dKernelHeightOV2)) : ((dKernelHeightOV2-fabs(warped.y))*(dKernelHeightOV2-fabs(warped.y)) / (dKernelHeightOV2*ty_minus_sy));
				return weight_x * weight_y;
			}
		};
	}
}

#endif
