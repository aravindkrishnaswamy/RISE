//////////////////////////////////////////////////////////////////////
//
//  BoxPixelFilter.h - The simplest, pixel filter, all samples
//  are equally weighted, so no warping is done.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 4, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef BOXPIXELFILTER_
#define BOXPIXELFILTER_

#include "../Interfaces/IPixelFilter.h"
#include "../Utilities/Reference.h"

//
// Definition of the box pixel filter
//
namespace RISE
{
	namespace Implementation
	{
		class BoxPixelFilter : public virtual IPixelFilter, public virtual Reference
		{
		protected:
			Scalar		dKernelWidth;
			Scalar		dKernelHeight;
			Scalar		dKernelWidthOV2;
			Scalar		dKernelHeightOV2;

			virtual ~BoxPixelFilter( ){};

		public:
			BoxPixelFilter( const Scalar width, const Scalar height ) : 
			dKernelWidth( width ),
	  		dKernelHeight( height ),
			dKernelWidthOV2( width/2 ), 
			dKernelHeightOV2( height/2 )
			{
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*dKernelWidth - dKernelWidthOV2, canonical.y*dKernelHeight - dKernelHeightOV2 );
				return 1.0;
			}

			Scalar warpOnScreen( const RandomNumberGenerator&, const Point2& canonical, Point2& warped, const unsigned int x, const unsigned int y ) const
			{
				warped = Point2( canonical.x*dKernelWidth+x-dKernelWidthOV2, canonical.y*dKernelHeight+y-dKernelHeightOV2 );
				return 1.0;
			}
		};
	}
}

#endif
