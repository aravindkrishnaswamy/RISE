//////////////////////////////////////////////////////////////////////
//
//  NNBRasterImageAccessor.h - Implements a raster image accessor using 
//  nearest neighbor interpolation.
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

#ifndef NNBRASTERIMAGEACCESSOR_
#define NNBRASTERIMAGEACCESSOR_

#include "../Interfaces/IRasterImageAccessor.h"

namespace RISE
{
	namespace Implementation
	{
		template< class C >
		class NNBRasterImageAccessor : public virtual IRasterImageAccessor, public virtual Reference
		{
		protected:
			IRasterImage&	pImage;
			Scalar			image_width;
			Scalar			image_height;

			virtual ~NNBRasterImageAccessor( )
			{
				pImage.release();
			}

		public:
			NNBRasterImageAccessor( IRasterImage& pImage_ ) :
			pImage( pImage_ ), image_width( 1.0 ), image_height( 1.0 )
			{
				pImage.addref();
				image_width = (Scalar)pImage.GetWidth();
				image_height = (Scalar)pImage.GetHeight();
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
				Scalar	u = y * Scalar( image_width ) + 0.5;
				Scalar	v = x * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar(image_width-1) ) u = Scalar(image_width-1);
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar(image_height-1) ) v = Scalar(image_height-1);

				p = pImage.GetPEL( (unsigned int)u, (unsigned int)v );
			}

			void		SetPel( const Scalar x, const Scalar y, C& p ) const
			{
				Scalar	u = y * Scalar( image_width ) + 0.5;
				Scalar	v = x * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar(image_width-1) ) u = Scalar(image_width-1);
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar(image_height-1) ) v = Scalar(image_height-1);

				pImage.SetPEL( (unsigned int)u, (unsigned int)v, p );
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
