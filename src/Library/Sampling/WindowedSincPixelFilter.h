//////////////////////////////////////////////////////////////////////
//
//  WindowedSincPixelFilter.h - A pixel filter which scales the values
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


#ifndef WINDOWEDSINC_PIXELFILTER_
#define WINDOWEDSINC_PIXELFILTER_

#include "PixelFilter.h"
#include "../Utilities/Reference.h"
#include "../Functions/Polynomial.h"

namespace RISE
{
	//
	// Definition of the windowed sinc pixel filter
	//
	namespace Implementation
	{
		//! This is a helper class for all the types of windowed sinc filters
		class WindowedSincPixelFilter : public virtual PixelFilter, public virtual Reference
		{
		protected:
			const Scalar dKernelWidthOV2;
			const Scalar dKernelHeightOV2;

			WindowedSincPixelFilter( const Scalar width, const Scalar height ) : 
			dKernelWidthOV2( width/2 ),
			dKernelHeightOV2( height/2 )
			{
				dKernelWidth = width;
				dKernelHeight = height;
			}

			virtual ~WindowedSincPixelFilter( ){};

			inline void warpUniform( const Point2& canonical, Point2& warped ) const
			{
				warped = Point2( canonical.x*dKernelWidth - dKernelWidthOV2, canonical.y*dKernelHeight - dKernelHeightOV2 );
			}

			inline Point2 warpTriangle( const Point2& canonical, Point2& warped ) const 
			{		
				if (canonical.x >= 0.5) {
					warped.x = (1.5 - sqrt(2.0 - 2.0*canonical.x)) - 0.5;
				} else {
					warped.x = (-0.5 + sqrt(2.0*canonical.x)) - 0.5;
				}

				if (canonical.y >= 0.5) {
					warped.y = (1.5 - sqrt(2.0 - 2.0*canonical.y)) - 0.5;
				} else {
					warped.y = (-0.5 + sqrt(2.0*canonical.y)) - 0.5;
				}

				Point2 weight( fabs(warped.x), fabs(warped.y) );

				warped.x *= dKernelWidthOV2;
				warped.y *= dKernelHeightOV2;

				return weight;
			}

			inline Scalar sinc1D( const Scalar x ) const
			{
				if( fabs(x) < NEARZERO ) return 1.0;
				const Scalar px = PI * x;
				return sin(px) / px;
			}

		public:

		};

		// Here is a list of the all the different windowed sinc filters
		// All these windows can be found here:
		// http://graphics.ucsd.edu/~wang/graphics/filtering/filtering.html

		//////////////////////////////////
		//! Box Window
		//////////////////////////////////
		class BoxWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~BoxWindowSincPixelFilter(){};

		public:
			BoxWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				return sinc1D(dx) * sinc1D(dy);
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				return sinc1D(warped.x) * sinc1D(warped.y);
			}
		};

		//////////////////////////////////
		//! Barlett Window
		//////////////////////////////////
		class BarlettWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~BarlettWindowSincPixelFilter(){};

		public:
			BarlettWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar bx = 1.0 - fabs(dx)/dKernelWidthOV2;
				const Scalar by = 1.0 - fabs(dy)/dKernelHeightOV2;
				return (bx * sinc1D(dx)) * (by * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpTriangle( canonical, warped );
				return sinc1D(warped.x) * sinc1D(warped.y);
			}
		};

		//////////////////////////////////
		//! Welch Window
		//////////////////////////////////
		class WelchWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~WelchWindowSincPixelFilter(){};

		public:
			WelchWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar nx = fabs(dx)/dKernelWidthOV2;
				const Scalar ny = fabs(dy)/dKernelHeightOV2;
				return ((1.0-nx*nx) * sinc1D(dx)) * ((1.0-ny*ny) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = fabs(warped.x)/dKernelWidthOV2;
				const Scalar ny = fabs(warped.y)/dKernelHeightOV2;
				return ((1.0-nx*nx) * sinc1D(warped.x)) * ((1.0-ny*ny) * sinc1D(warped.y));
			}
		};

		//////////////////////////////////
		//! Hanning Window
		//////////////////////////////////
		class HanningWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~HanningWindowSincPixelFilter(){};

		public:
			HanningWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar nx = dx/dKernelWidthOV2;
				const Scalar ny = dy/dKernelHeightOV2;
				return ((0.5 + 0.5*cos(PI*nx)) * sinc1D(dx)) * ((0.5 + 0.5*cos(PI*ny)) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				return ((0.5 + 0.5*cos(PI*nx)) * sinc1D(warped.x)) * ((0.5 + 0.5*cos(PI*ny)) * sinc1D(warped.y));
			}
		};

		//////////////////////////////////
		//! Hamming Window
		//////////////////////////////////
		class HammingWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~HammingWindowSincPixelFilter(){};

		public:
			HammingWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar nx = dx/dKernelWidthOV2;
				const Scalar ny = dy/dKernelHeightOV2;
				return ((0.54 + 0.46*cos(PI*nx)) * sinc1D(dx)) * ((0.54 + 0.46*cos(PI*ny)) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				return ((0.54 + 0.46*cos(PI*nx)) * sinc1D(warped.x)) * ((0.54 + 0.46*cos(PI*ny)) * sinc1D(warped.y));
			}
		};

		//////////////////////////////////
		//! Blackman Window
		//////////////////////////////////
		class BlackmanWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~BlackmanWindowSincPixelFilter(){};

		public:
			BlackmanWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar nx = dx/dKernelWidthOV2;
				const Scalar ny = dy/dKernelHeightOV2;
				return ((0.42 + 0.5*cos(PI*nx) + 0.08*cos(TWO_PI*nx)) * sinc1D(dx)) *
					   ((0.42 + 0.5*cos(PI*ny) + 0.08*cos(TWO_PI*ny)) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				return ((0.42 + 0.5*cos(PI*nx) + 0.08*cos(TWO_PI*nx)) * sinc1D(warped.x)) *
					   ((0.42 + 0.5*cos(PI*ny) + 0.08*cos(TWO_PI*ny)) * sinc1D(warped.y));
			}
		};

		//////////////////////////////////
		//! Lanczos Window
		//////////////////////////////////
		class LanczosWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~LanczosWindowSincPixelFilter(){};

		public:
			LanczosWindowSincPixelFilter( const Scalar width, const Scalar height ) :
			WindowedSincPixelFilter( width, height )
			{
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				const Scalar nx = dx/dKernelWidthOV2;
				const Scalar ny = dy/dKernelHeightOV2;
				return (sinc1D(nx) * sinc1D(dx)) * (sinc1D(ny) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				return (sinc1D(nx) * sinc1D(warped.x)) * (sinc1D(ny) * sinc1D(warped.y));
			}
		};

		//////////////////////////////////
		//! Kaiser Window
		//////////////////////////////////
		class KaiserWindowSincPixelFilter : public virtual WindowedSincPixelFilter
		{
		protected:
			virtual ~KaiserWindowSincPixelFilter(){};

			const Scalar alpha;
			Scalar bessel_alpha;

			inline Scalar kaiser1D( const Scalar x, const Scalar halfW ) const
			{
				const Scalar n = x / halfW;
				const Scalar arg = 1.0 - n*n;
				if( arg <= 0 ) return 0.0;
				return Polynomial::bessi0( alpha*sqrt(arg) ) / bessel_alpha;
			}

		public:
			KaiserWindowSincPixelFilter( const Scalar width, const Scalar height, const Scalar alpha_ ) :
			WindowedSincPixelFilter( width, height ), alpha( alpha_ )
			{
				bessel_alpha = Polynomial::bessi0( alpha );
			}

			Scalar EvaluateFilter( const Scalar dx, const Scalar dy ) const
			{
				if( fabs(dx) > dKernelWidthOV2 || fabs(dy) > dKernelHeightOV2 ) return 0.0;
				return (kaiser1D(dx, dKernelWidthOV2) * sinc1D(dx)) *
					   (kaiser1D(dy, dKernelHeightOV2) * sinc1D(dy));
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				return (kaiser1D(warped.x, dKernelWidthOV2) * sinc1D(warped.x)) *
					   (kaiser1D(warped.y, dKernelHeightOV2) * sinc1D(warped.y));
			}
		};
	}
}

#endif
