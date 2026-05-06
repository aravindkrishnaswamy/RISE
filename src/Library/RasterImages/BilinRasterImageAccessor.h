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
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		// Landing 2: stochastic mip selection PRNG.
		// Hashed-coordinate function — stable per (x,y,lod) so the
		// chosen mip is deterministic per ray and decorrelated across
		// neighbouring pixels.  Returns a uniform double in [0,1).
		inline double MipHash( double x, double y, double lod )
		{
			// xxhash-style 64-bit mixer fed deterministic bytes
			// derived from the inputs.  Cheap; no PRNG plumbing
			// required.  Sufficient for v1; integrate into the
			// per-sample Sobol budget in a follow-up.
			std::uint64_t u, v, w;
			std::memcpy( &u, &x,   sizeof(u) );
			std::memcpy( &v, &y,   sizeof(v) );
			std::memcpy( &w, &lod, sizeof(w) );
			std::uint64_t h = u * 0x9E3779B97F4A7C15ULL;
			h ^= v + 0x517CC1B727220A95ULL + (h << 6) + (h >> 2);
			h ^= w + 0x517CC1B727220A95ULL + (h << 6) + (h >> 2);
			h ^= h >> 33;
			h *= 0xff51afd7ed558ccdULL;
			h ^= h >> 33;
			return double( h >> 11 ) * (1.0 / double( 1ULL << 53 ));
		}

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

			// Landing 2: lazy mip pyramid + LOD-aware sampling.
			// mipmap_enabled is set at construction; pyramid is
			// built on first GetPELwithLOD call with lod > 0
			// (std::call_once ensures single-thread build).
			//
			// Each level stores box-filtered pixel data flat in
			// row-major C order.  The base level (mip 0) is a
			// COPY of pImage to avoid two code paths in the LOD
			// sampling routine — it could be omitted by special-
			// casing lod == 0 (saves base-image memory) but the
			// cost is one extra branch per sample.  We pay the
			// memory; it's bounded.
			struct MipLevel
			{
				std::vector<C>  pixels;
				int             w;
				int             h;
			};
			bool                            mipmap_enabled;
			// Landing 2: footprint-driven stochastic supersampling
			// mode.  Used when mipmap is undesirable (lowmem) but
			// the user still wants footprint-driven anti-aliasing.
			// Painter passes the footprint extents; accessor jitters
			// the sample position within them at base resolution.
			// Zero pyramid memory; noisier at low spp than a proper
			// pyramid but converges to the same integral.
			bool                            supersample_enabled;
			mutable std::vector<MipLevel>   mip_pyramid;
			mutable std::once_flag          pyramid_built;

			virtual ~BilinRasterImageAccessor( )
			{
				pImage.release();
			}

			// Build the box-filter mip pyramid from the base image.
			// Called once per accessor on first non-zero-LOD access.
			//
			// IMPORTANT: the pyramid stores HALF-RESOLUTION and below.
			// The full-res base level is NOT copied into the pyramid —
			// it remains in pImage and is accessed via the existing
			// GetPel path when LOD ≤ 0 (or when the stochastic mip
			// selection picks level 0).  Skipping the base copy saves
			// ~75% of pyramid memory (a 4Kx4K pyramid drops from
			// ~683 MB to ~171 MB).  Without this, NewSponza-class
			// scenes (~70 large textures) blow past system RAM and
			// the renderer slows to a crawl from swap thrashing.
			//
			// Index convention:
			//   mip_pyramid[0]   = level 1 (half-resolution)
			//   mip_pyramid[i]   = level (i+1)
			// Caller (GetPELwithLOD) maps LOD=N (N≥1) to
			// mip_pyramid[N-1] and LOD<1 to either pImage (base) or
			// mip_pyramid[0] (stochastic).
			void BuildMipPyramid() const
			{
				// Level 1 (first stored): box-filter directly from pImage.
				// Each output pixel reads 4 pImage texels, so this is the
				// only build phase that pays the per-call decode cost
				// when pImage is a ReadOnly (lowmem) image.  The GLTF
				// importer therefore avoids constructing the pyramid
				// entirely under lowmem mode (see PreDecodeTextures);
				// the lowmem path uses footprint supersampling instead.
				{
					MipLevel level1;
					level1.w = std::max( 1, image_width  / 2 );
					level1.h = std::max( 1, image_height / 2 );
					level1.pixels.resize( (size_t)level1.w * (size_t)level1.h );
					for( int y = 0; y < level1.h; ++y ) {
						for( int x = 0; x < level1.w; ++x ) {
							const int x0 = std::min( 2 * x,     image_width  - 1 );
							const int x1 = std::min( 2 * x + 1, image_width  - 1 );
							const int y0 = std::min( 2 * y,     image_height - 1 );
							const int y1 = std::min( 2 * y + 1, image_height - 1 );
							const C p00( pImage.GetPEL( x0, y0 ) );
							const C p10( pImage.GetPEL( x1, y0 ) );
							const C p01( pImage.GetPEL( x0, y1 ) );
							const C p11( pImage.GetPEL( x1, y1 ) );
							level1.pixels[ (size_t)y * (size_t)level1.w + (size_t)x ] =
								(p00 + p10 + p01 + p11) * Scalar( 0.25 );
						}
					}
					mip_pyramid.push_back( std::move( level1 ) );
				}

				// Each subsequent level is a 2:1 box filter of the
				// previous (now in-pyramid, fast access).  Stops at 1x1.
				while( mip_pyramid.back().w > 1 || mip_pyramid.back().h > 1 ) {
					const MipLevel& prev = mip_pyramid.back();
					MipLevel next;
					next.w = std::max( 1, prev.w / 2 );
					next.h = std::max( 1, prev.h / 2 );
					next.pixels.resize( (size_t)next.w * (size_t)next.h );
					for( int y = 0; y < next.h; ++y ) {
						for( int x = 0; x < next.w; ++x ) {
							const int x0 = std::min( 2 * x,     prev.w - 1 );
							const int x1 = std::min( 2 * x + 1, prev.w - 1 );
							const int y0 = std::min( 2 * y,     prev.h - 1 );
							const int y1 = std::min( 2 * y + 1, prev.h - 1 );
							const C p00 = prev.pixels[ (size_t)y0 * (size_t)prev.w + (size_t)x0 ];
							const C p10 = prev.pixels[ (size_t)y0 * (size_t)prev.w + (size_t)x1 ];
							const C p01 = prev.pixels[ (size_t)y1 * (size_t)prev.w + (size_t)x0 ];
							const C p11 = prev.pixels[ (size_t)y1 * (size_t)prev.w + (size_t)x1 ];
							next.pixels[ (size_t)y * (size_t)next.w + (size_t)x ] =
								(p00 + p10 + p01 + p11) * Scalar( 0.25 );
						}
					}
					mip_pyramid.push_back( std::move( next ) );
				}
			}

			// Bilinear sample at a specific mip level.  Mirrors the
			// existing GetPel logic but reads from mip_pyramid[level]
			// instead of pImage.
			void BilinearSampleMip( const Scalar x, const Scalar y,
			                        const int level, C& p ) const
			{
				const MipLevel& mip = mip_pyramid[ (size_t)level ];

				// Apply wrap modes (same convention as base GetPel).
				const Scalar wrappedY = ApplyWrapMode( y, wrap_s );
				const Scalar wrappedX = ApplyWrapMode( x, wrap_t );

				Scalar u = wrappedY * Scalar( mip.w ) + 0.5;
				Scalar v = wrappedX * Scalar( mip.h ) + 0.5;
				if( u < 0.0 ) u = 0.0;
				if( u > Scalar( mip.w - 1 ) ) u = Scalar( mip.w - 1 );
				if( v < 0.0 ) v = 0.0;
				if( v > Scalar( mip.h - 1 ) ) v = Scalar( mip.h - 1 );

				double ulo, vlo;
				const double ut = modf( u, &ulo );
				const double vt = modf( v, &vlo );

				int xlo = int( ulo );
				int xhi = xlo + 1;
				int ylo = int( vlo );
				int yhi = ylo + 1;

				if( xhi >= mip.w ) {
					xhi = ( wrap_s == eRasterWrap_Repeat ) ? 0 : mip.w - 1;
				}
				if( yhi >= mip.h ) {
					yhi = ( wrap_t == eRasterWrap_Repeat ) ? 0 : mip.h - 1;
				}

				const C ll = mip.pixels[ (size_t)ylo * (size_t)mip.w + (size_t)xlo ];
				const C lh = mip.pixels[ (size_t)ylo * (size_t)mip.w + (size_t)xhi ];
				const C hl = mip.pixels[ (size_t)yhi * (size_t)mip.w + (size_t)xlo ];
				const C hh = mip.pixels[ (size_t)yhi * (size_t)mip.w + (size_t)xhi ];
				const Scalar omut = 1.0 - ut;
				const Scalar omvt = 1.0 - vt;

				p = ll * (omut * omvt)
				  + hl * (omut * vt)
				  + lh * (ut * omvt)
				  + hh * (ut * vt);
			}

		public:
			BilinRasterImageAccessor( IRasterImage& pImage_ ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( eRasterWrap_ClampToEdge ), wrap_t( eRasterWrap_ClampToEdge ),
			mipmap_enabled( false ), supersample_enabled( false )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			BilinRasterImageAccessor( IRasterImage& pImage_, char wrapS, char wrapT ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( wrapS ), wrap_t( wrapT ),
			mipmap_enabled( false ), supersample_enabled( false )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			BilinRasterImageAccessor( IRasterImage& pImage_, char wrapS, char wrapT, bool mipmap ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( wrapS ), wrap_t( wrapT ),
			mipmap_enabled( mipmap ), supersample_enabled( false )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			//! Landing 2: full-control overload that selects between
			//! pyramid (mipmap=true) and stochastic supersampling
			//! (supersample=true) modes.  Both true is treated as
			//! pyramid (mipmap wins); both false is the legacy
			//! base-level bilinear (no LOD).
			BilinRasterImageAccessor( IRasterImage& pImage_, char wrapS, char wrapT,
			                          bool mipmap, bool supersample ) :
			pImage( pImage_ ), image_width( 1 ), image_height( 1 ),
			wrap_s( wrapS ), wrap_t( wrapT ),
			mipmap_enabled( mipmap ), supersample_enabled( supersample )
			{
				pImage.addref();
				image_width = pImage.GetWidth();
				image_height = pImage.GetHeight();
			}

			//
			// For satisfying the interface requirements
			//
			void		GetPEL( const Scalar x, const Scalar y, RISEColor& p ) const override
			{
				C	ret;
				GetPel( x, y, ret );
				p = RISEColor( ret.base, ret.a );
			}

			// Landing 2: report mipmap capability so painters can
			// decide whether to compute LOD.
			bool		SupportsLOD() const override { return mipmap_enabled; }
			bool		SupportsFootprint() const override { return supersample_enabled; }

			unsigned int GetWidth() const override  { return (unsigned int)image_width; }
			unsigned int GetHeight() const override { return (unsigned int)image_height; }

			// Landing 2: LOD-aware sample.  When mipmap is disabled,
			// forwards to the existing base-level bilinear path.
			// Otherwise stochastically picks one of the bracketing
			// mip levels and bilinear-samples there.
			//
			// LOD/level mapping (see BuildMipPyramid for the rationale
			// of skipping the base level in the pyramid storage):
			//   chosenLevel == 0          → sample pImage (base)
			//   chosenLevel == N (N≥1)    → sample mip_pyramid[N-1]
			// For fractional LOD between bracketing integers, the
			// stochastic pick can land on either bracket; LOD < 1 can
			// stochastically go to base or pyramid[0].
			void		GetPELwithLOD( const Scalar x, const Scalar y,
			                           const Scalar lod, RISEColor& p ) const override
			{
				if( !mipmap_enabled ) {
					GetPEL( x, y, p );
					return;
				}
				if( lod <= Scalar( 0 ) ) {
					// Below the first mip level — base sample only.
					GetPEL( x, y, p );
					return;
				}

				std::call_once( pyramid_built, [this]{ const_cast<BilinRasterImageAccessor*>(this)->BuildMipPyramid(); } );

				// Clamp LOD to the available pyramid range.  pyramid[i]
				// represents level (i+1), so the highest sampleable
				// level number is mip_pyramid.size() (which corresponds
				// to mip_pyramid[size-1]).
				const int maxLevel = (int)mip_pyramid.size();	// inclusive cap
				Scalar lodClamped = lod;
				if( lodClamped > Scalar( maxLevel ) ) lodClamped = Scalar( maxLevel );
				const int lvlFloor = (int)std::floor( lodClamped );
				const Scalar frac = lodClamped - Scalar( lvlFloor );
				const int chosenLevel =
					( lvlFloor < maxLevel && MipHash( x, y, lod ) < frac )
						? ( lvlFloor + 1 )
						: lvlFloor;

				if( chosenLevel == 0 ) {
					// Base level — sample pImage (the pyramid skips it).
					GetPEL( x, y, p );
					return;
				}

				C ret;
				BilinearSampleMip( x, y, chosenLevel - 1, ret );
				p = RISEColor( ret.base, ret.a );
			}

			// Landing 2: footprint-aware sample for lowmem mode.
			// Takes the full 2x2 footprint Jacobian
			//   ( ∂u/∂x  ∂u/∂y )
			//   ( ∂v/∂x  ∂v/∂y )
			// and jitters within the screen-pixel parallelogram its
			// columns span — i.e. the SAME parallelogram a proper mip
			// lookup would prefilter over.  At infinite spp converges
			// to the same band-limited integral with zero pyramid
			// memory.
			//
			// Earlier revisions collapsed the Jacobian to two scalar
			// extents (sqrt(dudx²+dudy²), sqrt(dvdx²+dvdy²)) and
			// sampled within the AXIS-ALIGNED bounding box of the
			// footprint.  That AABB always >= the true footprint, and
			// strictly > on rotated / sheared UV charts (e.g. Sponza
			// pillar faces, brick / tile patterns rotated 45°).  The
			// integration region was inflated, so minified textures
			// over-blurred in the lowmem path while looking sharp on
			// the same scene under the pyramid path — a tell that the
			// sampler, not the texture, was wrong.
			//
			// `jitterU` / `jitterV` are expected in [0, 1].
			void		GetPELwithFootprint( const Scalar x, const Scalar y,
			                                 const Scalar dudx, const Scalar dudy,
			                                 const Scalar dvdx, const Scalar dvdy,
			                                 const Scalar jitterU, const Scalar jitterV,
			                                 RISEColor& p ) const override
			{
				// Map jitter from [0,1]² to [-0.5, 0.5]² (one unit-area
				// screen pixel centred on the hit), then push that
				// offset through the Jacobian to reach the matching
				// point inside the texture-space parallelogram.
				const Scalar sx = jitterU - Scalar( 0.5 );
				const Scalar sy = jitterV - Scalar( 0.5 );
				const Scalar offU = sx * dudx + sy * dudy;
				const Scalar offV = sx * dvdx + sy * dvdy;
				// Axis convention from existing GetPel: x is V, y is U.
				GetPEL( x + offV, y + offU, p );
			}


			void		SetPEL( const Scalar x, const Scalar y, RISEColor& p ) const override
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
			Scalar	Evaluate( const Scalar x, const Scalar y ) const override
			{
				const C val = GetPel( x, y );
				return ColorMath::MaxValue(val.base);
			}
		};
	}
}

#endif
