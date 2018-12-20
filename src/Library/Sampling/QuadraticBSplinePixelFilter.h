//////////////////////////////////////////////////////////////////////
//
//  QuadraticBSplinePixelFilter.h - A pixel filter which scales the values
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


#ifndef QUADRATICBSPLINE_PIXELFILTER_
#define QUADRATICBSPLINE_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Definition of the quadratic Bspline pixel filter
	//
	namespace Implementation
	{
		class QuadraticBSplinePixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			virtual ~QuadraticBSplinePixelFilter( ){};

		public:
			QuadraticBSplinePixelFilter( )
			{
			}

			inline Scalar bsp_weight( const Scalar x ) const
			{
				if( x<-1.5) return 0.0;
				if( x<-0.5) {
					const Scalar t = x+1.5;
					return 0.5*t*t;
				}
				if( x<0.5) return 0.75-x*x;
				if( x<1.5) {
					const Scalar t = x-1.5;
					return 0.5*t*t;
				}
				return 0.;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*3.0 - 1.5, canonical.y*3.0 - 1.5 );

				// Compute the weight
				return bsp_weight(warped.x) * bsp_weight(warped.y);				
			}
		};
	}
}

#endif
