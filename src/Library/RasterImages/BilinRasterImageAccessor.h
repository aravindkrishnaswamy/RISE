//////////////////////////////////////////////////////////////////////
//
//  BilinRasterImageAccessor.h - Implements a raster image accessor using 
//  bilinear interpolation
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 26, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BILINRASTERIMAGEACCESSOR_
#define BILINRASTERIMAGEACCESSOR_

#include "../Interfaces/IRasterImageAccessor.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		template< class C >
		class BilinRasterImageAccessor : public virtual IRasterImageAccessor, public virtual Reference
		{
		protected:
			IRasterImage&	pImage;
			int				image_width;
			int				image_height;

			virtual ~BilinRasterImageAccessor( )
			{
				pImage.release();
			}

		public:
			BilinRasterImageAccessor( IRasterImage& pImage_ ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			//
			// For satisfying the interface requirements
			//
			void		GetPEL( const Scalar x, const Scalar y, RISEColor& p ) const
			{
				C	ret;
				GetPel( x, y, ret );
				p = RISEColor( ret.base, ret.a );
			}

			void		SetPEL( const Scalar x, const Scalar y, RISEColor& p ) const
			{
				C	set = C( p.base, p.a );
				SetPel( x, y, set );
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
				Scalar	u = y * Scalar( image_width ) + 0.5;
				Scalar	v = x * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar(image_width-1) ) u = Scalar(image_width-1);
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar(image_height-1) ) v = Scalar(image_height-1);

				// Extract the integer and decimal components of the x, y co-ordinates
				double ulo, vlo;
				const double ut = modf( u, &ulo );
				const double vt = modf( v, &vlo );

				int		xlo = int( ulo );
				int		xhi = xlo+1;
				int		ylo = int( vlo );
				int		yhi = ylo+1;


				if( xhi >= int(image_width) ) xhi = int(image_width)-1;
				if( yhi >= int(image_height) ) yhi = int(image_height)-1;

				// Thus our final texel color is composed of the four texels it actually hits 
				const C	ll( pImage.GetPEL( xlo, ylo ) );
				const C	lh( pImage.GetPEL( xhi, ylo ) );
				const C	hl( pImage.GetPEL( xlo, yhi ) );
				const C	hh( pImage.GetPEL( xhi, yhi ) );
				const Scalar omut = 1.0 - ut;
				const Scalar omvt = 1.0 - vt;

				// And then our final texel color is just a linear combination of these
				// four texels
				p = ll * (omut * omvt) 
					+ hl * (omut * vt)
					+ lh * (ut * omvt) 
					+ hh * (ut * vt);
			}

			void		SetPel( const Scalar x, const Scalar y, C& p ) const
			{
				// Calculate x and y value in terms of pixels in the original 
				// image, also round up any pixel values
				Scalar	u = y * Scalar( image_width ) + 0.5;
				Scalar	v = x * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar(image_width-1) ) u = Scalar(image_width-1);
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar(image_height-1) ) v = Scalar(image_height-1);

				// Extract the integer and decimal components of the x, y co-ordinates
				double ulo, vlo;
				const double ut = modf( u, &ulo );
				const double vt = modf( v, &vlo );

				int		xlo = int( ulo );
				int		xhi = xlo+1;
				int		ylo = int( vlo );
				int		yhi = ylo+1;

				if( xhi >= int(image_width) ) xhi = int(image_width)-1;
				if( yhi >= int(image_height) ) yhi = int(image_height)-1;

				// We will write to four texels..
				const Scalar omut = 1.0 - ut;
				const Scalar omvt = 1.0 - vt;

				// And then our final texel color is just a linear combination of these
				// four texels
				pImage.SetPEL( xlo, ylo, p * (omut * omvt) );
				pImage.SetPEL( xhi, ylo, p * (omut * vt) );
				pImage.SetPEL( xlo, yhi, p * (ut * omvt) );
				pImage.SetPEL( xhi, yhi, p * (ut * vt) );
			}

			// Function2D requirements
			Scalar	Evaluate( const Scalar x, const Scalar y ) const
			{
				const C val = GetPel( x, y );
				return ColorMath::MaxValue(val.base);
			}
		};
	}
}

#endif
