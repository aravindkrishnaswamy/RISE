//////////////////////////////////////////////////////////////////////
//
//  BicubicRasterImageAccessor.h - Implements a raster image accessor using
//  bicubic interpolation
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

			BicubicRasterImageAccessor( IRasterImage& pImage_, const ICubicInterpolator<C>& interp_, char wrapS, char wrapT ) :
			BilinRasterImageAccessor<C>( pImage_, wrapS, wrapT ), interp( interp_ )
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
				// Defensive: a zero-dimension image (corrupt PNG decoded
				// to 0×0, or a freshly-constructed unloaded image) would
				// hit modulo-by-zero in the Repeat/MirroredRepeat
				// boundary path below.  Return a default-initialised
				// (zero) pel.  The Bilin and NNB GetPel paths carry the
				// same guard (theirs protects against the clamp bounds
				// going negative into the unchecked raster GetPEL).
				if( this->image_width <= 0 || this->image_height <= 0 ) {
					p = C();
					return;
				}

				// Apply per-axis wrap before pixel-coord scaling.  See
				// BilinRasterImageAccessor for the rationale and the
				// wrap_s ↔ y / wrap_t ↔ x axis convention.
				const Scalar wrappedY = ApplyWrapMode( y, this->wrap_s );
				const Scalar wrappedX = ApplyWrapMode( x, this->wrap_t );

				// Calculate x and y value in terms of pixels in the original
				// image, also round up any pixel values
				Scalar	u = wrappedY * Scalar( this->image_width ) + 0.5;
				Scalar	v = wrappedX * Scalar( this->image_height ) + 0.5;

				// Clamp u and v to [0, dim-1].  NOTE this is NOT a no-op
				// for Repeat / MirroredRepeat: u = wrapped*W + 0.5
				// exceeds W-1 whenever wrapped > 1 - 1.5/W, so the top
				// ~1.5 texels of every tile pin u to W-1 (making the
				// interpolation parameter 0 there).  Deliberate pre-clamp
				// design — do not "fix" (see the Bilin accessor).  For
				// ClampToEdge the clamp does the actual saturation.
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

				// Bicubic samples a 4x4 footprint around (xlo, ylo).
				//   - Repeat: off-image neighbour wraps around to the
				//     opposite edge (modulo W / H).
				//   - MirroredRepeat: off-image neighbour mirrors —
				//     fold into [0, 2*W) then reflect into [0, W) so
				//     adjacent tiles meet seam-free.
				//   - ClampToEdge: saturate the neighbour index at the
				//     boundary texel, matching Bilin's xhi/yhi clamp.
				//     (The low branch writes xlo/ylo, but px < 0 implies
				//     xlo == 0, so both branches land on the true edge
				//     texel — textbook clamp-to-edge.)
				auto wrapIndex = []( int p, int N, char mode ) -> int {
					if( mode == eRasterWrap_Repeat ) {
						return ((p % N) + N) % N;
					}
					if( mode == eRasterWrap_MirroredRepeat ) {
						const int twoN = 2 * N;
						int q = ((p % twoN) + twoN) % twoN;	// [0, 2N)
						return ( q < N ) ? q : ( twoN - 1 - q );
					}
					// ClampToEdge handled inline by the caller (saturates at the boundary texel)
					return p;
				};
				C pixels[4][4];
				const int W = (int)this->image_width;
				const int H = (int)this->image_height;
				for( int y=0; y<4; y++ ) {
					for( int x=0; x<4; x++ ) {
						int px = (xlo-1+x);
						int py = (ylo-1+y);
						// Axis convention (same as the wrapped-UV code
						// above and Bilin's xhi/yhi seam partners): the
						// width-axis index px comes from `u` (input `y`,
						// wrap_s); the height-axis index py comes from
						// `v` (input `x`, wrap_t).
						if( this->wrap_s != eRasterWrap_ClampToEdge ) {
							px = wrapIndex( px, W, this->wrap_s );
						} else {
							if( px < 0 ) px = xlo;
							if( px >= W ) px = W-1;
						}
						if( this->wrap_t != eRasterWrap_ClampToEdge ) {
							py = wrapIndex( py, H, this->wrap_t );
						} else {
							if( py < 0 ) py = ylo;
							if( py >= H ) py = H-1;
						}
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
