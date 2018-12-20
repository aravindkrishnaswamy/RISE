//////////////////////////////////////////////////////////////////////
//
//  CookPixelFilter.h - A pixel filter which scales the values
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


#ifndef COOKPIXELFILTER_
#define COOKPIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

//
// Definition of the cook pixel filter
//
namespace RISE
{
	namespace Implementation
	{
		class CookPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			Scalar		dKernelWidthOV2;
			Scalar		dKernelHeightOV2;

			Scalar		dExpWidthSq;
			Scalar		dExpHeightSq;

			virtual ~CookPixelFilter( ){};

		public:
			CookPixelFilter( const Scalar width, const Scalar height ) : 
			dKernelWidthOV2( width/2 ),
			dKernelHeightOV2( height/2 )
			{
				dKernelWidth = width;
				dKernelHeight = height;
			
				dExpWidthSq = exp(-(dKernelWidthOV2*dKernelWidthOV2));
				dExpHeightSq = exp(-(dKernelHeightOV2*dKernelHeightOV2));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*dKernelWidth - dKernelWidthOV2, canonical.y*dKernelHeight - dKernelHeightOV2 );

				// Compute the weight
				return 
					(exp(-(warped.x*warped.x)) - dExpWidthSq) * 
					(exp(-(warped.y*warped.y)) - dExpHeightSq);
			}
		};
	}
}

#endif
