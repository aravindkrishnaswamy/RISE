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

			// Report the real half-support (default in IPixelFilter is
			// 0.5, which only coincidentally matches width=1; without
			// the override a box filter with width>1 would appear to
			// have sub-pixel support to SplatFilm::SplatFiltered and
			// consequently short-circuit to a single-pixel point splat.
			void GetFilterSupport( Scalar& halfWidth, Scalar& halfHeight ) const
			{
				halfWidth  = dKernelWidthOV2;
				halfHeight = dKernelHeightOV2;
			}

			// Normalised box kernel: 1/(width*height) inside the support
			// so the 2D integral over [-w/2,w/2]×[-h/2,h/2] equals 1.0.
			// SplatFilm::Resolve divides by the global sample count (not
			// per-pixel weight), so the kernel MUST have unit integral
			// for splats to accumulate without a bias.
			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs( dx ) > dKernelWidthOV2 || fabs( dy ) > dKernelHeightOV2 ) {
					return 0;
				}
				return 1.0 / ( dKernelWidth * dKernelHeight );
			}
		};
	}
}

#endif
