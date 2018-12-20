//////////////////////////////////////////////////////////////////////
//
//  CatmullRomPixelFilter.h - A pixel filter which scales the values
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


#ifndef CATMULLROM_PIXELFILTER_
#define CATMULLROM_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

//
// Definition of the catmull rom spline pixel filter
//
namespace RISE
{
	namespace Implementation
	{
		class CatmullRomPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			virtual ~CatmullRomPixelFilter( ){};

		public:
			CatmullRomPixelFilter( )
			{
			}

			inline Scalar crs_weight( const Scalar x ) const
			{
				if( x < -2.0 ) return 0.0;
				if( x < -1.0 ) return 0.5 * ( 4.0+x*(8.0+x*(5.0+x)) );
				if( x < 0.0 ) return 0.5 * ( 2.0+x*x*(-5.0+x*-3.0) );
				if( x < 1.0 ) return 0.5 * ( 2.0+x*x*(-5.0+x*3.0) );
				if( x < 2.0 ) return 0.5 * ( 4.0+x*(-8.0+x*(5.0-x)) );
				return 0.;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*4.0 - 2.0, canonical.y*4.0 - 2.0 );

				// Compute the weight
				return crs_weight(warped.x) * crs_weight(warped.y);				
			}
		};
	}
}

#endif
