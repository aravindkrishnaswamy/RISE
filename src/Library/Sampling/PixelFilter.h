//////////////////////////////////////////////////////////////////////
//
//  PixelFilter.h - Contains implementation help for pixel filters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PIXELFILTER_
#define PIXELFILTER_

#include "../Interfaces/IPixelFilter.h"

namespace RISE
{
	namespace Implementation
	{
		class PixelFilter : public IPixelFilter
		{
		protected:
			Scalar			dKernelWidth;
			Scalar			dKernelHeight;

			virtual ~PixelFilter( ){};

		public:
			virtual Scalar warpOnScreen( const RandomNumberGenerator& random, const Point2& canonical, Point2& warped, const unsigned int x, const unsigned int y ) const
			{
				Point2 w;
				const Scalar weight = warp( random, canonical, w );
				warped = Point2( w.x+x, w.y+y );
				return weight;
			}
		};
	}
}

#endif
