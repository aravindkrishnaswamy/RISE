//////////////////////////////////////////////////////////////////////
//
//  RampPainter.h - ramp_painter (doc 88 P2.2, S3): the universal
//  scalar -> colour remap.  Drives a multi-stop colour ramp from a
//  source painter's channel (or alpha), with linear / constant /
//  smooth interpolation between stops.
//
//  Design (see the S3 slice brief for the full rationale):
//
//    - `input` supplies the driving scalar `t` via one channel
//      (R/G/B/A) of its GetColor/GetAlpha at the hit; `t` is clamped
//      to [firstStopPos, lastStopPos] before locating the bracketing
//      pair of stops.
//    - Stops are eagerly JH-uplifted ONCE at construction (the same
//      route UniformColorPainter uses, Albedo kind) -- GetColorNM /
//      GetSpectrum never uplift per-sample; they linearly combine the
//      STOPS' precomputed spectral Eval(nm) by the same fractional
//      weight `u` the colour path uses (a linear combination of two
//      RGBAlbedoSpectrum Eval() outputs is the same technique
//      BlendPainter::GetColorNM already uses to blend two operand
//      spectra by a mask fraction).
//    - `Locate()` is one shared bracket-and-weight computation for
//      all three interpolation modes: `constant` forces the blend
//      weight to 0 (always the lower-index stop's exact value --
//      "hold-left" until the next stop's position, where the bracket
//      search itself has already advanced to that stop, so the value
//      jumps to it exactly AT that stop, not one stop later);
//      `smooth` reshapes the linear fraction with a smoothstep;
//      `linear` uses the fraction as-is.  All three modes therefore
//      return the exact stop colour/spectrum at every stop position
//      by construction (u == 0 or the bracket collapses to a single
//      stop).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-19
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAMP_PAINTER_
#define RAMP_PAINTER_

#include "Painter.h"
#include "../Utilities/Color/RGBSpectra.h"
#include <cstddef>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class RampPainter : public Painter
		{
		public:
			enum Channel { Channel_R = 0, Channel_G = 1, Channel_B = 2, Channel_A = 3 };
			enum Interpolation { Interp_Linear = 0, Interp_Constant = 1, Interp_Smooth = 2 };

			//! One AUTHORED colour stop, as parsed from the scene (position
			//! along `t` + an RGB colour already resolved to RISEPel by the
			//! chunk parser's `color_space` handling -- mirrors
			//! uniformcolor_painter's own colour-space convention).  Passed
			//! to the constructor; the class stores its own eagerly-uplifted
			//! internal representation (`StopEval`, below) built from these.
			struct Stop
			{
				Scalar  pos;
				RISEPel color;
				Stop() : pos( 0 ) {}
				Stop( const Scalar p, const RISEPel& c ) : pos( p ), color( c ) {}
			};

		protected:
			//! Internal per-stop representation: the authored (pos, color)
			//! plus the ONE-TIME Jakob-Hanika uplift (Albedo kind, matching
			//! UniformColorPainter's default -- ramp_painter's typical use is
			//! reflectance-like fields: terrain colour, tint gradients, mask
			//! visualisation).
			struct StopEval
			{
				Scalar             pos;
				RISEPel            color;
				RGBAlbedoSpectrum  spec;
				explicit StopEval( const Stop& s )
					: pos( s.pos ), color( s.color ), spec( RGBAlbedoSpectrum::FromRGB( s.color ) )
				{}
			};

			const IPainter&        input;
			const Channel           channel;
			const Interpolation     interp;
			std::vector<StopEval>   stops;      // sorted ascending by pos, size >= 2 (validated by the parser before construction)

			virtual ~RampPainter();

			//! Reads the driving channel of `input` at `ri` and clamps to
			//! the authored stop range.
			Scalar SampleT( const RayIntersectionGeometric& ri ) const;

			//! Shared bracket-and-weight computation for GetColor /
			//! GetColorNM / GetSpectrum.  `i0`/`i1` are stop indices
			//! (i0 <= i1; i0 == i1 at either end of the range or under
			//! `constant` mode); `u` is the blend weight toward `i1`
			//! (already reshaped for the active interpolation mode).
			void Locate( const Scalar t, std::size_t& i0, std::size_t& i1, Scalar& u ) const;

		public:
			RampPainter( const IPainter& input_, const Channel channel_,
				const Interpolation interp_, const std::vector<Stop>& stops_ );

			RampPainter( const RampPainter& ) = delete;
			RampPainter& operator=( const RampPainter& ) = delete;

			RISEPel        GetColor( const RayIntersectionGeometric& ri ) const override;
			Scalar         GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const override;
			SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const override;

			//! S10 (doc 88): read-only accessors for the Tier-2 ramp
			//! gradient-strip preview (PainterPreview.cpp).  Expose just
			//! enough to evaluate the ramp's OWN colour interpolation over
			//! its authored stop domain -- as if some hidden driving painter
			//! swept `t` across [firstStopPos, lastStopPos] -- WITHOUT
			//! touching `input` (the preview shows the ramp, not whatever
			//! painter happens to be driving it in the live scene).
			std::size_t StopCount() const { return stops.size(); }
			Scalar StopPos( std::size_t i ) const { return stops[i].pos; }
			RISEPel StopColor( std::size_t i ) const { return stops[i].color; }
			Interpolation GetInterpolation() const { return interp; }

			//! Evaluate the ramp's own colour interpolation at `t` (clamped
			//! to [StopPos(0), StopPos(StopCount()-1)]), reusing the SAME
			//! `Locate()` bracket/weight computation `GetColor` uses -- so
			//! the strip preview and a real render agree on interpolation
			//! mode exactly, not approximately.  Precondition: StopCount()
			//! >= 2 (guaranteed by the constructor's parser-validated input;
			//! calling this on a default-constructed/empty ramp is undefined,
			//! same precondition GetColor already carries).
			RISEPel EvalAt( Scalar t ) const;

			// No animatable state (v1) -- `input`'s own painter may be
			// keyframed (e.g. an expression_painter driving `t` via `time`),
			// which flows through automatically since we re-evaluate `input`
			// at every GetColor/GetColorNM/GetSpectrum call.
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) override {}
			void RegenerateData() override {}
		};
	}
}

#endif
