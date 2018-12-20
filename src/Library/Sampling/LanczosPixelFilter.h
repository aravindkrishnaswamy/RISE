//////////////////////////////////////////////////////////////////////
//
//  LanczosPixelFilter.h - A pixel filter which scales the values
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


#ifndef LANCZOS_PIXELFILTER_
#define LANCZOS_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Definition of the lanczos pixel filter
	//
	namespace Implementation
	{
		class LanczosPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			virtual ~LanczosPixelFilter( ){};

		public:
			LanczosPixelFilter( )
			{
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*4.0 - 2.0, canonical.y*4.0 - 2.0 );

				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				const Scalar sinc_x2 = sin(PI*(warped.x/2)) / (PI*(warped.x/2));
				const Scalar sinc_y2 = sin(PI*(warped.y/2)) / (PI*(warped.y/2));

				// Compute the weight
				return 
					(sinc_x*sinc_x2) *
					(sinc_y*sinc_y2);
					
			}
		};
	}
}

#endif
