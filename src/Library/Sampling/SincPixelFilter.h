//////////////////////////////////////////////////////////////////////
//
//  SincPixelFilter.h - A pixel filter which scales the values
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


#ifndef SINCPIXELFILTER_
#define SINCPIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"

namespace RISE
{
	//
	// Definition of the sinc pixel filter
	//
	namespace Implementation
	{
		class SincPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:

			const Scalar scale;

			virtual ~SincPixelFilter( );

		public:
			SincPixelFilter( const Scalar window, const Scalar scale_ );

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				const Scalar sx = dx * scale;
				const Scalar sy = dy * scale;
				const Scalar sinc_x = (fabs(sx) < NEARZERO) ? 1.0 : sin(sx) / sx;
				const Scalar sinc_y = (fabs(sy) < NEARZERO) ? 1.0 : sin(sy) / sy;
				return sinc_x * sinc_y;
			}

			virtual Scalar warp( const RandomNumberGenerator& random, const Point2& canonical, Point2& warped ) const;
		};
	}
}

#endif


