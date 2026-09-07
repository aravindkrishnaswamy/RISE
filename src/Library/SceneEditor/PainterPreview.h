//////////////////////////////////////////////////////////////////////
//
//  PainterPreview.h - The headless painter-preview engine (doc 88
//    S10, Tier 2 of the P5 human-editability contract): a small
//    RGBA8 image of what a named painter -- or, for an expression
//    painter, one of its `def` stages -- computes, without a scene
//    render.
//
//  WHY NO SCENE RENDER.  Painters are pure functions of a
//  RayIntersectionGeometric; MATERIAL_EDITOR.md's shared thumbnail
//  engine (its §4, "one engine ... path-traces for free") is the
//  right tool for a MATERIAL swatch (light transport genuinely
//  matters there), but a painter has no BSDF, no light, no camera --
//  evaluating GetColor/GetValuesAt over a synthetic grid IS the
//  correct preview, and it is orders of magnitude cheaper than
//  standing up a render pass.  This module does NOT reuse the
//  observe-toolkit's render-isolation machinery (CopyInteractiveFrame
//  et al.) for the same reason a shader unit test doesn't stand up a
//  compositor: there is no frame to isolate, only a pure evaluation
//  loop over a grid this module owns end to end.
//
//  DOMAIN CONVENTIONS (the "synthetic-but-honest" contract -- read
//  this before changing pixel (x,y) -> RayIntersectionGeometric
//  mapping, and keep PainterPreviewTest in sync with any change):
//
//    - Pixel (x,y) of a `w`x`h` patch samples at its CENTRE:
//        u = (x + 0.5) / w,   v = (y + 0.5) / h
//      Row 0 is v's low end -- NOT flipped to match "image top =
//      +Y"; this is an implementation convention, not a claim about
//      camera or texture space.  `ptCoord` (u,v) is what a
//      uv-only painter (checker, most textures) actually samples.
//    - `P` (world) and `Po` (object) both sweep a UNIT PATCH
//      centred at the origin, spanning [-0.5, 0.5] in x and y, z=0:
//        P = Po = (u - 0.5, v - 0.5, 0)
//      so a 3D-context painter (expression_painter's `P`, a
//      perlin3d/worley3d painter that samples world position) shows
//      genuine spatial structure instead of a flat/degenerate field
//      -- the P/Po/N-all-zero "dummy ri" Painter::Evaluate uses for
//      its 2D-only IFunction2D fallback would silently zero out
//      exactly this class of painter (see ExpressionPainter.h's file
//      header comment on that trap).  P == Po deliberately: there is
//      no separate object transform for a bare painter preview.
//    - `N` (shading normal) = (0, 0, 1) -- the patch faces +Z.
//    - `fw` (expression VM filter width, doc 88 S9) = 0, a POINT
//      SAMPLE, carried on a `widthValid`-true footprint (i.e. the
//      positive claim "the width is known and it is zero", not the
//      absence of a claim).  This module used to synthesize
//      1 / max(w, h) -- the patch's own pixel pitch -- on the theory
//      that a plausible footprint beats a stub 0.  It does not, for
//      two reasons.  (i) The unit patch is a SYNTHETIC domain: it is
//      one world unit across because this module chose that, with no
//      relation to the extent the painter is actually evaluated over
//      in a scene (a body authored for a 0.1 m plank sweeps the same
//      [-0.5, 0.5] here), so a length measured in the patch is not an
//      estimate of the render's footprint -- it is a length in a
//      different space.  (ii) Since 2026-09-06 that distinction has
//      teeth: fbm/turbulence/ridged rescale `fw` by their position
//      argument's compile-time domain scale (ExpressionEval.h's
//      Builder::NoiseFwScales), so the synthetic width got multiplied
//      by the body's own scale k and crossed OctaveFadeWeight's
//      hi = 0.6 at k ~= 58 -- every high-frequency body previewed as
//      one uniform square (measured: `fbm(P*k,3,0.5,2)` preview range
//      0.318 at k = 40, 0.0116 at k = 55, EXACTLY 0 at k >= 58), while
//      the real render of the same body at its own footprint is full
//      of detail.  0 is the honest value, not a fallback: this module
//      evaluates the painter at pixel CENTRES with no filtering and no
//      supersampling, which is exactly what a zero footprint declares
//      (the same statement a secondary bounce makes -- no ray carries
//      differentials after a scatter).  Callers that want a
//      footprint-faded preview must render the scene; there is no
//      preview-space value that could stand in for the render's own
//      per-hit `txFootprint`.
//    - `time` = 0.  A time-varying preview is out of scope for a
//      static thumbnail (S10); animate by advancing the scene's
//      timeline and re-requesting a fresh preview, like the film
//      preview already does for keyframed materials.
//
//  DISPLAY ENCODE.  RISEPel / expression vec3 results are Rec.709
//  LINEAR (see ExpressionPainter.h).  This module is a DEBUG/PREVIEW
//  thumbnail, not a colour-managed render: it clamps each channel to
//  [0,1] then applies a fixed gamma-2.2 encode before scaling to
//  0..255.  Values outside [0,1] (an expression painter's body is
//  deliberately allowed to produce them -- see ExpressionPainter.h)
//  clip rather than tonemap; that is an intentional simplification
//  for a small inspector swatch, not a claim of HDR fidelity.
//
//  SCALAR NORMALIZATION.  A physical scalar (IOR, scattering,
//  roughness, an expression `def`'s scalar-typed stage) has no
//  natural [0,1] range -- IOR runs ~1..2.4, scattering can be 1e6.
//  A fixed-range clamp would render most real scalar painters as a
//  flat white or black square, destroying the exact structure a
//  Tier-2 stage thumbnail exists to show.  This module instead
//  AUTO-RANGES: it evaluates the whole patch once to find
//  [dataMin, dataMax], then maps that range to grayscale [0,1]
//  (dataMin -> 0, dataMax -> 1) before the shared gamma-2.2 display
//  encode above.  The applied [dataMin, dataMax] is reported back in
//  Result -- callers that want the RAW numbers (not just "brighter =
//  higher"), or that want to detect a flat/degenerate patch
//  (dataMin == dataMax, reported as a uniform mid-gray 0.5 rather
//  than a divide-by-zero), read it there.  A colour-typed
//  (vec3-typed expression `def`, or any true IPainter) preview is
//  NOT auto-ranged -- it uses the fixed [0,1]-clamp display encode
//  above, matching how a colour swatch is understood everywhere else
//  in the editor.
//
//  PER-CHANNEL SCALAR PAINTERS (P2-4, S10 review round 1).  An
//  IScalarPainter whose HasPerChannelVariation() is true (a
//  vec3-typed `scalar_painter { expression ... }` program, or
//  RGBScalarPainter) carries a genuine per-channel triple -- e.g. an
//  RGB-dispersive IOR -- not a uniform scalar broadcast across
//  v[0..2].  RenderPainterPreview's scalar pipe renders that triple
//  as RGB (R=v[0], G=v[1], B=v[2]), matching the vec3 def-stage
//  branch's "show the true triple, don't collapse it" convention,
//  rather than reading only v[0].  Unlike a genuinely colour-typed
//  preview, the triple is still a physical scalar with no natural
//  [0,1] range, so it IS auto-ranged -- but with ONE SHARED
//  [dataMin, dataMax] computed JOINTLY across all three channels
//  (not three independent per-channel ranges).  Joint ranging
//  preserves the triple's relative channel magnitude (a dispersive
//  IOR's R < G < B ordering stays visible in the rendered swatch);
//  independent per-channel ranging would stretch each channel to
//  fill [0,1] on its own and erase exactly that relationship.  The
//  reported Result::scalarRangeMin/Max is this shared range;
//  Result::wasScalar is still true (the pipe of origin is still
//  IScalarPainter, only the ENCODE differs from the single-valued
//  grayscale case).
//
//  CONCURRENCY.  This module's static methods assume the caller
//  already holds whatever lock protects `job`'s manager graph against
//  a concurrent D2 re-derive (SceneEditController::PainterPreview /
//  RampStripPreview take that lock with the same try_lock +
//  mRenderOwnsScene discipline RefreshProperties/PipesFor use, and
//  refuse rather than block -- see PainterIntrospection.h's header
//  comment on why PipesFor is "the one place a live-object hook is
//  genuinely required").  A test that constructs its own Job and
//  calls these methods directly needs no lock -- there is no
//  concurrent mutator in that setting.
//
//  EVALUATION BUDGET (P2-1, S10 review round 1).  RenderPainterPreview
//  and RenderDefStagePreview run under that same caller-held lock, so
//  an unbounded high-resolution request against an expensive body
//  (nested fbm/turbulence octaves, worley chains) can hold it for
//  seconds -- stalling every other mutex-guarded controller getter
//  behind it (Undo included; see SceneEditController.h's own
//  concurrency notes).  Both methods therefore bound the number of
//  DISTINCT per-pixel evaluations to kMaxSampleBudget regardless of
//  the requested w*h: a request whose w*h exceeds the budget is
//  evaluated on a decimated grid (<= kMaxSampleBudget samples, same
//  aspect ratio, same pixel-centre/fw conventions scaled to the
//  grid's own resolution) and nearest-upscaled into the caller's full
//  w*h buffer.  The ABI contract is unchanged either way -- the
//  output is always exactly w*h*4 bytes -- only the number of
//  distinct evaluations is bounded.  A request at or under the budget
//  (every default-sized preview, and every panel swatch this module
//  ships with today) evaluates every pixel directly, unaffected.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PAINTERPREVIEW_
#define RISE_PAINTERPREVIEW_

#include "../Interfaces/IJobPriv.h"
#include "../Utilities/RString.h"
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	class PainterPreview
	{
	public:
		//! Hard cap on width/height -- a "preview" request for an absurd
		//! resolution (a bridge bug, a fuzzed C-ABI call) is refused rather
		//! than silently allocating gigabytes; 2048 is generous headroom
		//! over the documented default (64) and any plausible panel-strip
		//! use (§ ramp preview) while still bounding worst case to a few
		//! tens of MB.
		static const unsigned int kMaxDim = 2048;

		//! Evaluation budget (P2-1, S10 review round 1) -- see the header's
		//! EVALUATION BUDGET note above for the full rationale.  Hard cap
		//! on the number of DISTINCT per-pixel painter/expression
		//! evaluations a single RenderPainterPreview or
		//! RenderDefStagePreview call performs, independent of the
		//! requested w*h; a request over budget is evaluated on a
		//! decimated grid and nearest-upscaled into the full w*h output
		//! buffer, so the ABI contract (exactly w*h*4 bytes) never
		//! changes.  96*96 = 9216 keeps any single preview call within
		//! roughly the same order of magnitude as the documented 64x64
		//! default, however large a caller's on-screen swatch requests.
		static const unsigned int kMaxSampleBudget = 96u * 96u;

		enum class Status { Ok, Refused };

		struct Result
		{
			Status      status = Status::Refused;
			std::string refusalReason;   // set iff status == Refused

			unsigned int width  = 0;
			unsigned int height = 0;
			//! Row-major, top row (y=0, the LOW end of v -- see the header's
			//! domain-convention note) first; RGBA8, 4 bytes/pixel,
			//! size == width*height*4 iff status == Ok.
			std::vector<unsigned char> rgba;

			//! Set iff status == Ok AND the previewed stage evaluated to a
			//! SCALAR type (a scalar_painter's own output, or a scalar-typed
			//! expression def stage) -- see the header's scalar-
			//! normalization note.  Untouched (false / 0 / 0) for a
			//! colour-typed stage.  Still true for a HasPerChannelVariation()
			//! scalar painter's own preview even though `rgba` is rendered as
			//! a genuine RGB triple rather than grayscale -- see the header's
			//! PER-CHANNEL SCALAR PAINTERS note; scalarRangeMin/Max there is
			//! the ONE range shared jointly across all three channels.
			bool   wasScalar     = false;
			double scalarRangeMin = 0.0;
			double scalarRangeMax = 0.0;
		};

		//! Preview of a painter's own output (the union of the colour and
		//! scalar pipes -- whichever manager `painterName` resolves in;
		//! colour is tried first, matching PainterIntrospection::PipesFor's
		//! documented "no kind registers in both today" invariant, so the
		//! order does not paper over a real ambiguity).  `w`/`h` default to
		//! the doc 88 S10 brief's 64x64; 0 or > kMaxDim on either axis
		//! refuses.  A request whose w*h exceeds kMaxSampleBudget is
		//! evaluated on a decimated grid and nearest-upscaled into the
		//! full w*h buffer -- see the header's EVALUATION BUDGET note; the
		//! output is always exactly w*h*4 bytes regardless.  See the
		//! header comment for the full domain/encode/normalization
		//! contract, including the PER-CHANNEL SCALAR PAINTERS note for
		//! HasPerChannelVariation() scalar painters.
		static Result RenderPainterPreview( IJobPriv& job, const String& painterName,
			unsigned int w = 64, unsigned int h = 64 );

		//! Preview of expression def-slot `defIndex` (0-based, `def` line
		//! declaration order) for the expression painter named
		//! `painterName` -- colour pipe (`expression_painter`) or scalar
		//! pipe (`scalar_painter { expression ... }`), whichever resolves.
		//! Refuses when `painterName` does not resolve to either expression
		//! painter kind, or `defIndex` is outside [0, program.DefCount()).
		static Result RenderDefStagePreview( IJobPriv& job, const String& painterName,
			int defIndex, unsigned int w = 64, unsigned int h = 64 );

		//! A ramp_painter's own colour interpolation over its authored stop
		//! domain [firstStopPos, lastStopPos], as a horizontal gradient
		//! strip -- column x maps to t = firstStopPos + (x+0.5)/w *
		//! (lastStopPos - firstStopPos); every row is identical (a strip,
		//! not new data per row).  `input` (the painter that would normally
		//! drive `t` in a real render) is NOT evaluated -- see RampPainter::
		//! EvalAt's doc comment.  Refuses when `painterName` does not
		//! resolve to a ramp_painter.  `h` defaults to 1; a caller that
		//! wants a visibly-thick strip passes a larger `h` and gets the row
		//! replicated, not new samples.
		static Result RenderRampStripPreview( IJobPriv& job, const String& painterName,
			unsigned int w = 64, unsigned int h = 1 );
	};
}

#endif
