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

			inline Scalar weight1D_x( const Scalar x ) const
			{
				const Scalar ax = fabs(x);
				if( ax >= dKernelWidthOV2 ) return 0.0;
				if( ax < s_x ) return 1.0 - (x*x)/(s_x*dKernelWidthOV2);
				const Scalar d = dKernelWidthOV2 - ax;
				return d*d / (dKernelWidthOV2*tx_minus_sx);
			}

			inline Scalar weight1D_y( const Scalar y ) const
			{
				const Scalar ay = fabs(y);
				if( ay >= dKernelHeightOV2 ) return 0.0;
				if( ay < s_y ) return 1.0 - (y*y)/(s_y*dKernelHeightOV2);
				const Scalar d = dKernelHeightOV2 - ay;
				return d*d / (dKernelHeightOV2*ty_minus_sy);
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				return weight1D_x(dx) * weight1D_y(dy);
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*dKernelWidth - dKernelWidthOV2, canonical.y*dKernelHeight - dKernelHeightOV2 );
				return weight1D_x(warped.x) * weight1D_y(warped.y);
			}
		};
	}
}

#endif
