//////////////////////////////////////////////////////////////////////
//
//  MitchellNetravaliPixelFilter.h - A pixel filter which scales the values
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


#ifndef MITCHELLNETRAVALI_PIXELFILTER_
#define MITCHELLNETRAVALI_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	//
	// Definition of the mithcell-netravali pixel filter
	//
	namespace Implementation
	{
		class MitchellNetravaliPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			const Scalar	b;
			const Scalar	c;

			Scalar			ua;
			Scalar			ub;
			Scalar			uc;

			Scalar			va;
			Scalar			vb;
			Scalar			vc;
			Scalar			vd;

			virtual ~MitchellNetravaliPixelFilter( ){};

		public:
			MitchellNetravaliPixelFilter( const Scalar b_, const Scalar c_ ) : 
			b( b_ ),
			c( c_ )
			{
				ua = 12.0 - 9.0*b - 6.0*c;
				ub = -18.0 + 12.0*b + 6.0*c;
				uc = 6.0 - 2.0*b;

				va = -b - 6.0*c;
				vb = 6.0*b + 30.0*c;
				vc = -12.0*b - 48.0*c;
				vd = 8.0*b + 24.0*c;
			}

			inline Scalar ComputeWeight( const Scalar x ) const
			{
				const Scalar x2 = x*x;
				const Scalar x3 = x2*x;

				if( x <= 1.0 ) {
					return (1.0/6.0)*(ua*x3 + ub*x2 + uc);
				}

				return (1.0/6.0)*(va*x3 + vb*x2 + vc*x + vd);
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*4.0 - 2.0, canonical.y*4.0 - 2.0 );

				// Compute the weight
				return ComputeWeight(fabs(warped.x)) * ComputeWeight(fabs(warped.y));
					
			}
		};
	}
}

#endif
