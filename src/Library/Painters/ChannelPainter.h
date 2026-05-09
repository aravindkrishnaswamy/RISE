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
//  Spectral path: a channel extraction selects ONE scalar from the
//  source's RGB triple — the result is wavelength-independent by
//  construction (a roughness or metallic value doesn't have a
//  spectrum).  GetColorNM therefore samples the source's RGB,
//  extracts the requested channel, and returns that scalar at every
//  wavelength.  Earlier revisions called source.GetColorNM(ri, nm)
//  and ignored the channel selector, which produced wrong
//  per-wavelength scalars for the standard glTF metallicRoughness
//  texture layout (R=AO, G=roughness, B=metallic, all packed in one
//  RGB texture): in spectral mode the metallic painter would
//  silently sample the JH-uplifted spectrum of (R, G, B) instead of
//  just the B value, treating organic surfaces as ~17 % metallic
//  and skewing diffuse / Fresnel splits warm.  The fix matches the
//  RGB path: pull the scalar from source.GetColor(ri).
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
			//! Channel selector.  CHAN_R / G / B read from the source's
			//! `GetColor` RGB triple.  CHAN_A reads from `GetAlpha`, which
			//! is the un-premultiplied A channel of an RGBA texture
			//! (default 1.0 for painters that don't expose alpha) -- used
			//! by the glTF importer's alphaMode = MASK / BLEND wiring.
			enum Channel { CHAN_R = 0, CHAN_G = 1, CHAN_B = 2, CHAN_A = 3 };

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
				Scalar v = Scalar( 0 );
				if( channel == CHAN_A ) {
					v = source.GetAlpha( ri );
				} else {
					const RISEPel s = source.GetColor( ri );
					switch( channel ) {
						case CHAN_R: v = s.r; break;
						case CHAN_G: v = s.g; break;
						case CHAN_B: v = s.b; break;
						case CHAN_A: break;	// handled above (unreachable here)
					}
				}
				Scalar out = scale * v + bias;
				// Alpha is a coverage scalar; downstream consumers
				// (BlendPainter mask, AlphaTestShaderOp cutoff,
				// TransparencyShaderOp factor) all assume [0,1].
				// Clamp here so a stray scale/bias from a hand-authored
				// scene cannot produce extrapolating blends.
				if( channel == CHAN_A ) {
					out = (out < Scalar(0)) ? Scalar(0)
					    : (out > Scalar(1)) ? Scalar(1)
					    : out;
				}
				return RISEPel( out, out, out );
			}

			Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
			{
				// Mirror the RGB path: pull the scalar channel from the
				// source's RGB output — a roughness / metallic / AO
				// scalar is wavelength-independent, so we return the
				// same value at every nm.  `nm` is intentionally unused.
				(void)nm;
				Scalar v = Scalar( 0 );
				if( channel == CHAN_A ) {
					v = source.GetAlpha( ri );
				} else {
					const RISEPel s = source.GetColor( ri );
					switch( channel ) {
						case CHAN_R: v = s.r; break;
						case CHAN_G: v = s.g; break;
						case CHAN_B: v = s.b; break;
						case CHAN_A: break;	// handled above (unreachable here)
					}
				}
				Scalar out = scale * v + bias;
				if( channel == CHAN_A ) {
					out = (out < Scalar(0)) ? Scalar(0)
					    : (out > Scalar(1)) ? Scalar(1)
					    : out;
				}
				return out;
			}

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) { (void)name; (void)value; return 0; };
			void SetIntermediateValue( const IKeyframeParameter& val ) { (void)val; };
			void RegenerateData( ) {};
		};
	}
}

#endif
