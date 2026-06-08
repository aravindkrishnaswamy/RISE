//////////////////////////////////////////////////////////////////////
//
//  TextureScalarPainter.h - A spatially-varying scalar painter
//    backed by a raster texture.  At each surface hit, samples the
//    texture at `ri.ptCoord` and returns the chosen channel's value
//    as a pure scalar — no JH-uplift, no colorspace conversion.
//
//  Common uses:
//    - Roughness maps for GGX / Ward / etc.
//    - Spatially-varying scattering coefficient on a heterogeneous
//      dielectric.
//    - Spatially-varying IOR for "etched glass" stylised renders.
//
//  Channel selection: the user picks which channel of the texture
//  carries the scalar (default R).  Grayscale textures naturally
//  have R = G = B per pixel; this lets authors store three different
//  scalars in one RGB texture (e.g., metallicRoughness convention
//  packs metalness in B and roughness in G).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TEXTURE_SCALAR_PAINTER_
#define TEXTURE_SCALAR_PAINTER_

#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IRasterImageAccessor.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TextureScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		public:
			enum Channel { Channel_R = 0, Channel_G = 1, Channel_B = 2 };

		protected:
			IRasterImageAccessor* const pRIA;
			const Channel               channel;
			// Affine remap of the raw texel channel value:
			//   out = bias + scale * rawTexel   (rawTexel in [0,1]).
			// Defaults (scale = 1, bias = 0) make the painter return the
			// raw texel verbatim, so the no-affine constructor below
			// behaves exactly as before.  This lets a [0,1] greyscale map
			// drive a physical quantity in real units (e.g. an oxide
			// thickness map: `scale 220 bias 30` → 30..250 nm) without a
			// separate scaling painter in the chain.
			const Scalar                scale;
			const Scalar                bias;
			virtual ~TextureScalarPainter()
			{
				if( pRIA ) pRIA->release();
			}

		public:
			TextureScalarPainter( IRasterImageAccessor* pRIA_, Channel ch )
				: pRIA( pRIA_ ), channel( ch ), scale( 1 ), bias( 0 )
			{
				if( pRIA ) pRIA->addref();
			}

			TextureScalarPainter( IRasterImageAccessor* pRIA_, Channel ch,
			                      Scalar scale_, Scalar bias_ )
				: pRIA( pRIA_ ), channel( ch ), scale( scale_ ), bias( bias_ )
			{
				if( pRIA ) pRIA->addref();
			}

			ScalarTriple GetValuesAt(
				const RayIntersectionGeometric& ri
				) const override
			{
				if( !pRIA ) return ScalarTriple();
				RISEColor c;
				pRIA->GetPEL( ri.ptCoord.y, ri.ptCoord.x, c );
				const Scalar raw =
					channel == Channel_R ? c.base.r :
					channel == Channel_G ? c.base.g :
					                       c.base.b;
				const Scalar v = bias + scale * raw;
				// Replicate to three slots.  A `TextureScalarPainter`
				// represents a single scalar per UV — the user picked
				// which channel of the texture sources that scalar.
				return ScalarTriple( v );
			}

			// GetValueAtNM falls through to the default (returns v[0]).
			// Texture-driven scalars are wavelength-independent by
			// construction; for spatial × spectral, compose via
			// MultiplyScalarPainter.

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
