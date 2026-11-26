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
#include "../Utilities/FiniteMath.h"
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

			//! Structural introspection: source painter + channel/scale/bias.
			const IPainter& GetSource() const { return source; }
			Channel GetChannel() const { return channel; }
			Scalar GetScale() const { return scale; }
			Scalar GetBias() const { return bias; }

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
			bool ReadsWorldPosition() const override { return source.ReadsWorldPosition(); }
			unsigned int GetPathDependence() const override { return source.GetPathDependence(); }
		};

		//! Full 3-channel RGB bridge from any IPainter to IScalarPainter
		//! (`scalar_painter { painter NAME channel RGB scale S bias B }`
		//! and `RISE_API_CreatePainterRGBScalarPainter`).
		//!
		//! Unlike `PainterChannelScalarPainter` (which replicates one selected
		//! channel across all 3 lanes), `PainterRGBScalarPainter` evaluates
		//! `source.GetColor(ri)` and forwards `(bias + scale*R, bias + scale*G,
		//! bias + scale*B)` as a per-channel `ScalarTriple` — enabling spatially
		//! varying tinted physical-scalar slots such as dielectric `tau`.
		//!
		//! `GetValueAtNM` uses the same nominal-wavelength piecewise-linear
		//! interpolation as `RGBScalarPainter` (B=450 nm, G=550 nm, R=650 nm,
		//! clamped outside `[450, 650]`).  Same post-colourspace caveat as
		//! `PainterChannelScalarPainter`.
		class PainterRGBScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			const IPainter& source;
			const Scalar    scale;
			const Scalar    bias;

			static constexpr Scalar kNmR = Scalar( 650.0 );
			static constexpr Scalar kNmG = Scalar( 550.0 );
			static constexpr Scalar kNmB = Scalar( 450.0 );

			virtual ~PainterRGBScalarPainter()
			{
				source.release();
			}

		public:
			PainterRGBScalarPainter( const IPainter& source_, const Scalar scale_, const Scalar bias_ )
				: source( source_ ), scale( scale_ ), bias( bias_ )
			{
				source.addref();
			}

			const IPainter& GetSource() const { return source; }
			Scalar GetScale() const { return scale; }
			Scalar GetBias() const { return bias; }

			ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
			{
				const RISEPel c = source.GetColor( ri );
				const Scalar r = bias + scale * c[0];
				const Scalar g = bias + scale * c[1];
				const Scalar b = bias + scale * c[2];
				return ScalarTriple(
					RISE::IsFiniteDouble( static_cast<double>( r ) ) ? r : Scalar( 0 ),
					RISE::IsFiniteDouble( static_cast<double>( g ) ) ? g : Scalar( 0 ),
					RISE::IsFiniteDouble( static_cast<double>( b ) ) ? b : Scalar( 0 ) );
			}

			Scalar GetValueAtNM( const RayIntersectionGeometric& ri, Scalar nm ) const override
			{
				const ScalarTriple v = GetValuesAt( ri );
				if( !RISE::IsFiniteDouble( static_cast<double>( nm ) ) ) {
					return v.v[1];
				}
				if( nm <= kNmB ) return v.v[2];
				if( nm >= kNmR ) return v.v[0];
				if( nm <= kNmG ) {
					const Scalar t = ( nm - kNmB ) / ( kNmG - kNmB );
					return v.v[2] + t * ( v.v[1] - v.v[2] );
				} else {
					const Scalar t = ( nm - kNmG ) / ( kNmR - kNmG );
					return v.v[1] + t * ( v.v[0] - v.v[1] );
				}
			}

			bool HasPerChannelVariation() const override { return true; }
			bool ReadsWorldPosition() const override { return source.ReadsWorldPosition(); }
			unsigned int GetPathDependence() const override { return source.GetPathDependence(); }
		};
	}
}

#endif
