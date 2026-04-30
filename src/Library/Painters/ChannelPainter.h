//////////////////////////////////////////////////////////////////////
//
//  ChannelPainter.h - Extracts a single R/G/B channel from another
//  painter, optionally rescaled and biased, and broadcasts it as a
//  greyscale RGB triple.
//
//  Designed for glTF metallic-roughness texture decomposition: the
//  metallic factor lives in the .B channel of the MR texture and the
//  roughness factor in .G; routing each through a `channel_painter`
//  yields the per-pixel scalar input that `ggx_material`'s `rs` /
//  `alphax` / `alphay` parameters expect.  See docs/GLTF_IMPORT.md §4
//  (PBR mapping) for the full graph.
//
//  Spectral path: NM lookups pass through to the source painter and
//  then sample the chosen channel's "narrow-band slice" by treating
//  the source's spectral value as if it were already RGB-decomposed.
//  This is a coarse approximation (channel selection isn't a meaningful
//  spectral op) but it lets the painter participate in spectral
//  rasterizers without crashing; users authoring spectral PBR scenes
//  should source roughness / metallic factors from scalar painters
//  (e.g. uniformcolor_painter) rather than channel-extracted RGB.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CHANNEL_PAINTER_
#define CHANNEL_PAINTER_

#include "Painter.h"

namespace RISE
{
	namespace Implementation
	{
		class ChannelPainter : public Painter
		{
		public:
			enum Channel { CHAN_R = 0, CHAN_G = 1, CHAN_B = 2 };

		protected:
			const IPainter&		source;
			Channel				channel;
			Scalar				scale;
			Scalar				bias;

			virtual ~ChannelPainter()
			{
				source.release();
			}

		public:
			ChannelPainter(
				const IPainter& src,
				Channel         chan,
				Scalar          scale_,
				Scalar          bias_ ) :
			  source( src ),
			  channel( chan ),
			  scale( scale_ ),
			  bias( bias_ )
			{
				source.addref();
			}

			RISEPel GetColor( const RayIntersectionGeometric& ri ) const
			{
				const RISEPel s = source.GetColor( ri );
				Scalar v = Scalar( 0 );
				switch( channel ) {
					case CHAN_R: v = s.r; break;
					case CHAN_G: v = s.g; break;
					case CHAN_B: v = s.b; break;
				}
				const Scalar out = scale * v + bias;
				return RISEPel( out, out, out );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				// Channel selection has no meaningful spectral analogue;
				// pass the source's narrow-band value through the
				// scale/bias affinity.  Acceptable for PBR scenes that
				// drive roughness / metallic from scalar (uniformcolor)
				// painters; questionable for ones that try to spectralise
				// a per-channel RGB texture.  See header.
				const Scalar v = source.GetColorNM( ri, nm );
				return scale * v + bias;
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) { (void)name; (void)value; return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ) { (void)val; };
			void RegenerateData( ) {};
		};
	}
}

#endif
