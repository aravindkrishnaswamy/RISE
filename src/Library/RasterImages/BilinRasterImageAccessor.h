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
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		// Apply a wrap mode to a normalized UV component (typically in
		// [0, 1] but tiled assets may pass any value).  Returns the
		// equivalent in-range coordinate per the mode.  Inlined in the
		// accessor headers because it's the per-sample inner loop.
		inline Scalar ApplyWrapMode( Scalar uv, char wrapMode )
		{
			switch( wrapMode ) {
				case eRasterWrap_Repeat:
					// Wrap to [0, 1).  std::floor handles negatives so a
					// UV of -0.25 maps to 0.75 (one full tile back).
					return uv - std::floor( uv );
				case eRasterWrap_MirroredRepeat: {
					// Reduce to [0, 2), then mirror around 1 so the
					// resulting value is in [0, 1].  Visually: each
					// integer crossing flips orientation, so adjacent
					// tiles meet seam-free.
					Scalar f = uv - 2.0 * std::floor( uv * 0.5 );
					return ( f > 1.0 ) ? ( 2.0 - f ) : f;
				}
				default:
					// eRasterWrap_ClampToEdge — leave the UV alone; the
					// accessor's existing per-pixel clamp handles values
					// outside [0, 1] by saturating at the boundary texel.
					return uv;
			}
		}

		template< class C >
		class BilinRasterImageAccessor : public virtual IRasterImageAccessor, public virtual Reference
		{
		protected:
			IRasterImage&	pImage;
			int				image_width;
			int				image_height;
			char			wrap_s;		// see eRasterWrapMode in IRasterImageAccessor.h
			char			wrap_t;

			virtual ~BilinRasterImageAccessor( )
			{
				pImage.release();
			}

		public:
			BilinRasterImageAccessor( IRasterImage& pImage_ ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( eRasterWrap_ClampToEdge ), wrap_t( eRasterWrap_ClampToEdge )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			BilinRasterImageAccessor( IRasterImage& pImage_, char wrapS, char wrapT ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( wrapS ), wrap_t( wrapT )
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
				// Apply per-axis wrap mode BEFORE pixel-coordinate scaling.
				// (The existing variable mapping inverts width/height vs.
				// the conventional U/V — `u` here is the horizontal pixel
				// coord computed from `y`, `v` the vertical from `x` —
				// so wrap_s applies to the AXIS whose UV is `y` and
				// wrap_t to the AXIS whose UV is `x`, matching the
				// long-standing accessor convention.)  ClampToEdge is a
				// no-op here; the existing per-pixel clamp at lines
				// below handles it.
				const Scalar wrappedY = ApplyWrapMode( y, wrap_s );
				const Scalar wrappedX = ApplyWrapMode( x, wrap_t );

				// Calculate x and y value in terms of pixels in the original
				// image, also round up any pixel values
				Scalar	u = wrappedY * Scalar( image_width ) + 0.5;
				Scalar	v = wrappedX * Scalar( image_height ) + 0.5;

				// Clamp u and v values to between 0 and the image size.
				// For Repeat / MirroredRepeat the wrapped UV is already in
				// [0, 1] so this clamp is normally a no-op (only catches
				// fp-rounding overshoot near the upper edge).  For
				// ClampToEdge the clamp does the actual saturation.
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

				// Boundary handling for the lerp partner.
				//   - Repeat: the upper neighbour wraps to texel 0 — so
				//     adjacent tiles share a seamless lerp across the
				//     seam (the texel at UV=1.0 blends into the texel at
				//     UV=0+ε of the next tile).
				//   - MirroredRepeat: the upper neighbour mirrors back
				//     to texel image_width-1 (the SAME edge texel as
				//     xlo) — that's the seam-free reflection: the
				//     texture reads back from the same edge in the next
				//     tile.  Using `0` here would jump to the opposite
				//     side of the image instead of mirroring.
				//   - ClampToEdge: clip to the last texel (legacy
				//     behaviour, matches the lower-bound clamp above).
				if( xhi >= int(image_width) ) {
					xhi = ( wrap_s == eRasterWrap_Repeat )
						? 0
						: int(image_width)-1;	// clamp OR mirrored
				}
				if( yhi >= int(image_height) ) {
					yhi = ( wrap_t == eRasterWrap_Repeat )
						? 0
						: int(image_height)-1;	// clamp OR mirrored
				}

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
