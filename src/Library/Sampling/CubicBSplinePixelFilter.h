//////////////////////////////////////////////////////////////////////
//
//  CubicBSplinePixelFilter.h - A pixel filter which scales the values
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


#ifndef CUBICBSPLINE_PIXELFILTER_
#define CUBICBSPLINE_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

//
// Definition of the sinc pixel filter
//
namespace RISE
{
	namespace Implementation
	{
		class CubicBSplinePixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			virtual ~CubicBSplinePixelFilter( ){};

		public:
			CubicBSplinePixelFilter( )
			{
			}

			inline Scalar bsp_weight( const Scalar x ) const
			{
				if( x < -2.0 ) return 0.0;
				if( x < -1.0 ) {
					const Scalar t = 2.0+x;
					return t*t*t/6.0;
				}
				if( x < 0.0 ) return (4.0+x*x*(-6.0+x*-3.0))/6.0;
				if( x < 1.0 ) return (4.0+x*x*(-6.0+x*3.0))/6.0;
				if( x < 2.0 ) {
					const Scalar t = 2.0-x;
					return t*t*t/6.0;
				}
				return 0.0;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*4.0 - 2.0, canonical.y*4.0 - 2.0 );

				// Compute the weight
				return bsp_weight(warped.x) * bsp_weight(warped.y);				
			}
		};
	}
}

#endif
