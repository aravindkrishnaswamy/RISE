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
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		// Forward-declared in BilinRasterImageAccessor.h; declared here too
		// to keep the header self-contained for NNB users who don't
		// transitively include the Bilin header.
		inline Scalar ApplyWrapModeNNB( Scalar uv, char wrapMode )
		{
			switch( wrapMode ) {
				case eRasterWrap_Repeat:
					return uv - std::floor( uv );
				case eRasterWrap_MirroredRepeat: {
					Scalar f = uv - 2.0 * std::floor( uv * 0.5 );
					return ( f > 1.0 ) ? ( 2.0 - f ) : f;
				}
				default:
					return uv;
			}
		}

		template< class C >
		class NNBRasterImageAccessor : public virtual IRasterImageAccessor, public virtual Reference
		{
		protected:
			IRasterImage&	pImage;
			Scalar			image_width;
			Scalar			image_height;
			char			wrap_s;		// see eRasterWrapMode
			char			wrap_t;

			virtual ~NNBRasterImageAccessor( )
			{
				pImage.release();
			}

		public:
			NNBRasterImageAccessor( IRasterImage& pImage_ ) :
			pImage( pImage_ ), image_width( 1.0 ), image_height( 1.0 ),
			wrap_s( eRasterWrap_ClampToEdge ), wrap_t( eRasterWrap_ClampToEdge )
			{
				pImage.addref();
				image_width = (Scalar)pImage.GetWidth();
				image_height = (Scalar)pImage.GetHeight();
			}

			NNBRasterImageAccessor( IRasterImage& pImage_, char wrapS, char wrapT ) :
			pImage( pImage_ ), image_width( 1.0 ), image_height( 1.0 ),
			wrap_s( wrapS ), wrap_t( wrapT )
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
				// Apply per-axis wrap before the pixel-coord scale (see
				// BilinRasterImageAccessor for the rationale and the
				// `wrap_s ↔ y / wrap_t ↔ x` axis convention).
				const Scalar wrappedY = ApplyWrapModeNNB( y, wrap_s );
				const Scalar wrappedX = ApplyWrapModeNNB( x, wrap_t );

				Scalar	u = wrappedY * Scalar( image_width ) + 0.5;
				Scalar	v = wrappedX * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size.
				// For Repeat / MirroredRepeat the wrapped UV is in [0,1]
				// so this is a no-op except for fp-rounding overshoot.
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
