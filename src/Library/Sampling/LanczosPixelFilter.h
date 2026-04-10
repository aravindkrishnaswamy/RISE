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

			inline Scalar lanczos1D( const Scalar x ) const
			{
				if( fabs(x) < NEARZERO ) return 1.0;
				const Scalar px = PI * x;
				const Scalar px2 = PI * (x / 2.0);
				return (sin(px) / px) * (sin(px2) / px2);
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) >= 2.0 || fabs(dy) >= 2.0 ) return 0.0;
				return lanczos1D(dx) * lanczos1D(dy);
			}

			void GetFilterSupport( Scalar& halfWidth, Scalar& halfHeight ) const
			{
				halfWidth = 2.0;
				halfHeight = 2.0;
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*4.0 - 2.0, canonical.y*4.0 - 2.0 );
				return lanczos1D(warped.x) * lanczos1D(warped.y);
			}
		};
	}
}

#endif
