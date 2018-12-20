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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar wx = warped.x*PI;
				const Scalar wy = warped.y*PI;
				const Scalar sinc_x = sin(wx) / wx;
				const Scalar sinc_y = sin(wy) / wy;
				return
					(sinc_x) *
					(sinc_y);
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpTriangle( canonical, warped );
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return sinc_x * sinc_y;
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = fabs(warped.x)/dKernelWidthOV2;
				const Scalar ny = fabs(warped.y)/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					((1.0-nx*nx) * sinc_x) *
					((1.0-ny*ny) * sinc_y);
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					((0.5 + 0.5 * cos(PI*nx)) * sinc_x) *
					((0.5 + 0.5 * cos(PI*ny)) * sinc_y);
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					((0.54 + 0.46 * cos(PI*nx)) * sinc_x) *
					((0.54 + 0.46 * cos(PI*ny)) * sinc_y);
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					((0.42 + 0.5 * cos(PI*nx) + 0.08 * cos(TWO_PI*nx)) * sinc_x) *
					((0.42 + 0.5 * cos(PI*ny) + 0.08 * cos(TWO_PI*ny)) * sinc_y);
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

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					(sin(PI*nx)/(PI*nx) * sinc_x) *
					(sin(PI*ny)/(PI*ny) * sinc_y);
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

		public:
			KaiserWindowSincPixelFilter( const Scalar width, const Scalar height, const Scalar alpha_ ) : 
			WindowedSincPixelFilter( width, height ), alpha( alpha_ )
			{
				bessel_alpha = Polynomial::bessi0( alpha );
			}

			Scalar warp( const RandomNumberGenerator&, const Point2& canonical, Point2& warped ) const
			{
				warpUniform( canonical, warped );
				const Scalar nx = warped.x/dKernelWidthOV2;
				const Scalar ny = warped.y/dKernelHeightOV2;
				const Scalar sinc_x = sin(PI*warped.x) / (PI*warped.x);
				const Scalar sinc_y = sin(PI*warped.y) / (PI*warped.y);
				return
					(((Polynomial::bessi0( alpha*sqrt(1.0-nx*nx)) )/bessel_alpha) * sinc_x) *
					(((Polynomial::bessi0( alpha*sqrt(1.0-ny*ny)) )/bessel_alpha) * sinc_y);
			}
		};
	}
}

#endif
