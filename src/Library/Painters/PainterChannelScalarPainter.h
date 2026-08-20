//////////////////////////////////////////////////////////////////////
//
//  PainterChannelScalarPainter.h - the P2.1 any-painter -> scalar
//  bridge (doc 88 S3).  scalar_painter { painter <name> channel
//  <R|G|B|A> scale <s> bias <b> } generalizes the existing raster-only
//  `texture` form of scalar_painter to ANY colour painter -- all 36
//  painter kinds plus expression_painter become bindable to every
//  physical-scalar slot with one small chunk form.
//
//  SEMANTIC CAVEAT (the same one PainterToScalarAdapter.h carries --
//  read that file's header comment for the fuller version): `source`
//  is a full `IPainter`.  `GetColor(ri)` returns a value that has
//  already gone through the colour pipe's post-colourspace Rec.709-
//  linear representation; reading a channel of it here is fine for
//  procedural masks and spatially-varying fields (roughness
//  modulation, IOR variation driven by a noise field, etc.), but a
//  spectral-file (`spectral_painter`)-backed source's RGB projection
//  has already collapsed through the same colourspace conversion any
//  RGB rendering path takes -- this is NOT a spectral-fidelity path.
//  A genuinely wavelength-varying physical scalar belongs in a native
//  IScalarPainter form (sellmeier / polynomial / piecewise-linear
//  file / scalar_painter{expression}) instead.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-19
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PAINTER_CHANNEL_SCALAR_PAINTER_
#define PAINTER_CHANNEL_SCALAR_PAINTER_

#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PainterChannelScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		public:
			enum Channel { Channel_R = 0, Channel_G = 1, Channel_B = 2, Channel_A = 3 };

		protected:
			const IPainter&  source;
			const Channel    channel;
			// Affine remap, same convention as TextureScalarPainter:
			//   out = bias + scale * rawChannel
			const Scalar     scale;
			const Scalar     bias;

			virtual ~PainterChannelScalarPainter()
			{
				source.release();
			}

			// Channel A reads GetAlpha(ri) -- the un-premultiplied per-pixel
			// alpha IPainter carries for RGBA image sources (see IPainter.h);
			// R/G/B read GetColor(ri)'s corresponding component.
			Scalar RawAt( const RayIntersectionGeometric& ri ) const
			{
				if( channel == Channel_A ) {
					return source.GetAlpha( ri );
				}
				const RISEPel c = source.GetColor( ri );
				return c[ (unsigned int)channel ];
			}

		public:
			PainterChannelScalarPainter( const IPainter& source_, const Channel channel_,
				const Scalar scale_, const Scalar bias_ )
				: source( source_ ), channel( channel_ ), scale( scale_ ), bias( bias_ )
			{
				source.addref();
			}

			// A single selected channel is replicated to all three slots --
			// this painter reads ONE channel of `source`, not three, so the
			// result is a genuinely uniform triple (matches TextureScalarPainter's
			// convention, not PainterToScalarAdapter's per-channel-triple one).
			ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
			{
				return ScalarTriple( bias + scale * RawAt( ri ) );
			}

			// GetValueAtNM falls through to the default (GetValuesAt().v[0]):
			// the selected channel is wavelength-independent by construction
			// -- same value at every nm.  For spatial x spectral, compose via
			// MultiplyScalarPainter, same as TextureScalarPainter.

			bool HasPerChannelVariation() const override { return false; }
		};
	}
}

#endif
