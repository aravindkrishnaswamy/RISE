//////////////////////////////////////////////////////////////////////
//
//  BicubicRasterImageAccessor.h - Implements a raster image accessor using 
//  bilinear interpolation
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 1, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BicubicRasterImageAccessor_
#define BicubicRasterImageAccessor_

#include "../Interfaces/ICubicInterpolator.h"
#include "BilinRasterImageAccessor.h"

namespace RISE
{
	namespace Implementation
	{
		template< class C >
		class BicubicRasterImageAccessor : public virtual BilinRasterImageAccessor<C>
		{
		protected:
			const ICubicInterpolator<C>& interp;

			virtual ~BicubicRasterImageAccessor( )
			{
				interp.release();
			}

		public:
			BicubicRasterImageAccessor( IRasterImage& pImage_, const ICubicInterpolator<C>& interp_ ) :
			BilinRasterImageAccessor<C>( pImage_ ), interp( interp_ )
			{
				interp.addref();
			}

			void		GetPEL( const Scalar x, const Scalar y, RISEColor& p ) const
			{
				C	ret;
				GetPel( x, y, ret );
				p = RISEColor( ret.base, ret.a );
			}

			//
			// Actual implementation
			//

			C			GetPel( const Scalar x, const Scalar y ) const
			{
				C	ret;
				GetPel( x, y, ret );
				return ret;
			}

			void		GetPel( const Scalar x, const Scalar y, C& p ) const
			{
				// Calculate x and y value in terms of pixels in the original 
				// image, also round up any pixel values
				Scalar	u = y * Scalar( this->image_width ) + 0.5;
				Scalar	v = x * Scalar( this->image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar(this->image_width-1) ) u = Scalar(this->image_width-1);
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar(this->image_height-1) ) v = Scalar(this->image_height-1);

				// Extract the integer and decimal components of the x, y co-ordinates
				double ulo, vlo;
				const double ut = modf( u, &ulo );
				const double vt = modf( v, &vlo );

				int		xlo = int( ulo );
				int		ylo = int( vlo );

				// Now that we have the four primary pixels, we need all the pixel around that
				C pixels[4][4];
				for( int y=0; y<4; y++ ) {
					for( int x=0; x<4; x++ ) {
						int px = (xlo-1+x);
						int py = (ylo-1+y);
						if( px < 0 ) px = xlo;
						if( px >= this->image_width ) px = this->image_width-1;
						if( py < 0 ) py = ylo;
						if( py >= this->image_height ) py = this->image_height-1;
						pixels[y][x] =  this->pImage.GetPEL( px, py );
					}
				}

				// Now that we have all our pixels, run the cubic interpolator in one dimension to collapse it (we choose to collapase x)
				C pixelcol[4];
				for( int i=0; i<4; i++ ) {
					pixelcol[i] = interp.InterpolateValues( pixels[i][0], pixels[i][1], pixels[i][2], pixels[i][3], ut );
				}

				// Then collapse the y dimension for our result
				p = interp.InterpolateValues( pixelcol[0], pixelcol[1], pixelcol[2], pixelcol[3], vt );
			}
		};
	}
}

#endif
