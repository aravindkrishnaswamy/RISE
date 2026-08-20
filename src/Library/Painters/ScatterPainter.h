//////////////////////////////////////////////////////////////////////
//
//  ScatterPainter.h - scatter_painter (doc 88 P3.2, S8): texture-
//  bombing / FX-map-lite.  Stamps `source` over `background` on a
//  jittered square lattice -- the discrete-element richness (rivets,
//  leaves, scratches, stains, screws) that noise fundamentally cannot
//  produce, because noise has no notion of a single, repeated,
//  recognizable SHAPE.
//
//  Algorithm:
//
//    1. LATTICE.  `ptCoord * cell_scale` is a plain square lattice;
//       the query point's OWNING cell is floor(that).  Each cell MAY
//       contain one stamp instance, decided per-cell by a
//       deterministic hash roll against `probability` (see Hash01()
//       in the .cpp -- the identical hashing technique
//       StochasticTilePainter.cpp uses, reusing NoiseCore::
//       WorleyHashCell).
//    2. PER-CELL JITTER.  A cell's stamp instance has: a center offset
//       within the cell (up to +/- 0.5 * jitter_position cell-widths
//       per axis, so the jittered center never leaves its OWNING
//       cell), a rotation in +/- jitter_rotation degrees, and a size
//       stamp_scale * (1 + jitter_scale * u) for u in [-1,1] (so the
//       realized size is stamp_scale scaled by a factor in
//       [1 - jitter_scale, 1 + jitter_scale] -- jitter_scale is
//       validated to [0,1) at parse time, so this factor is always
//       > 0).  All four draws (position x/y, rotation, scale) plus
//       the probability roll are independent hash channels of the
//       SAME per-cell id -- see Hash01.
//    3. NEIGHBOURHOOD SEARCH.  A stamp centered in one cell can still
//       cover a query point in an ADJACENT cell (a rotated/jittered/
//       enlarged stamp near a cell border) -- FindStamp() therefore
//       checks the full 3x3 neighbourhood (the query cell + its 8
//       neighbours), not just the owning cell.
//
//       NEIGHBOURHOOD-REACH BOUND (provable, enforced at parse time,
//       see the chunk parser): a stamp's worst-case footprint is
//       bounded by the circle CIRCUMSCRIBING its (possibly rotated)
//       square, radius R = (stamp_scale * (1 + jitter_scale)) *
//       sqrt(2) / 2, centered at a point C that is always within its
//       OWNING cell (see point 2).  Two cells 2+ apart are separated
//       by at least 1 full cell-width, so a stamp owned by a cell 2+
//       away from the query cell can NEVER reach a query point in it
//       when R <= 1 -- independent of jitter_position, because C is
//       guaranteed to stay inside its own cell regardless of how that
//       parameter is set.  R <= 1 solves to:
//
//           stamp_scale * (1 + jitter_scale) <= sqrt(2)
//
//       which the parser enforces (rejects the chunk, with a
//       diagnostic, if violated) -- this is the "stamp too large"
//       check.  With that invariant held, checking the 3x3
//       neighbourhood is PROVABLY sufficient; no wider search is ever
//       needed.
//    4. RESOLUTION.  Among the (<= 9) candidate cells that (a) pass
//       their probability roll and (b) whose stamp's local frame
//       actually contains the query point after inverse-transform,
//       the NEAREST stamp CENTER wins (Euclidean distance in
//       cell-space) -- "nearer-cell-center-on-top".  Ties (a measure-
//       zero event under continuous jitter) resolve to whichever
//       candidate the fixed row-major (dj, di) scan order visits
//       first -- fully deterministic given the same UV + seed, which
//       is what "draw-order stability" means for this painter: no
//       hash-derived or floating-point-comparison-order dependency
//       beyond that fixed loop.
//    5. LOCAL FRAME.  The winning candidate's local sample coordinate
//       (in the stamp's own [0,1]^2 frame, per the file header
//       algorithm) is what `source` is evaluated at.
//    6. ALPHA-GATED COMPOSITE.  The winning stamp is composited over
//       `background` via the standard STRAIGHT-ALPHA Porter-Duff
//       "over" operator (Porter & Duff 1984), folding in BOTH the
//       stamp's own alpha (`source`'s GetAlpha) AND the background's
//       own alpha (`background`'s GetAlpha -- `background` need not be
//       opaque; the header's own "stacking scatter layers" use case
//       requires this):
//
//           out_a     = fg_a + bg_a*(1-fg_a)
//           out_color = ( fg*fg_a + bg*bg_a*(1-fg_a) ) / out_a
//
//       (out_a == 0 only when both fg_a and bg_a are 0; that case
//       falls back to the background colour, since there is nothing
//       to divide out -- see ComposeOverColor in the .cpp).  This
//       reduces exactly to out_color = bg*(1-fg_a) + fg*fg_a when
//       bg_a == 1 (an opaque background), which is the common case
//       and matches the simpler formula a reader might expect.  An
//       RGBA stamp with alpha 0 at the sampled texel therefore shows
//       pure background -- this is the texture-bombing use case (a
//       leaf/rivet/scratch PNG with an alpha cutout).  A query point
//       NOT covered by any stamp (FindStamp returns false) is pure
//       background, no compositing.
//
//  Domain: UV (`ri.ptCoord`) only, same composition-boundary
//  discipline as stochastic_tile_painter (see StochasticTilePainter.h)
//  -- wrap in an outer `mapping_painter { projection triplanar }` for
//  world-space / UV-less scattering rather than duplicating projection
//  logic here.
//
//  Spectral consistency (the S2 lesson): every Get* calls Resolve()
//  once, which is the SINGLE source of "which candidate wins, and
//  what is its alpha" -- the RGB, spectral, and alpha paths can never
//  disagree on which stamp instance (if any) is on top.
//
//  txFootprint invalidation: the winning sample's local coordinate is
//  unrelated to the original ptCoord's screen-space derivatives --
//  same rationale as StochasticTilePainter / the S7 P1-B precedent.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-20
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCATTER_PAINTER_
#define SCATTER_PAINTER_

#include "Painter.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace Implementation
	{
		class ScatterPainter : public Painter
		{
		protected:
			const IPainter&			source;			// the stamp
			const IPainter&			background;
			const Scalar				cellScale;
			const Scalar				stampScale;
			const Scalar				jitterPosition;
			const Scalar				jitterRotationDeg;
			const Scalar				jitterScale;
			const Scalar				probability;
			const unsigned int			seed;

			virtual ~ScatterPainter();

			//! 3x3-neighbourhood search for the nearest covering, active
			//! stamp instance.  Returns false (background everywhere) if
			//! none covers `coord`.  On true, `localUV` is the winning
			//! instance's sample coordinate in the stamp's own [0,1]^2
			//! frame.  See the file header for the full algorithm and the
			//! neighbourhood-reach proof.
			bool FindStamp( const Point2& coord, Point2& localUV ) const;

			//! Per-hit result shared by every Get* (the S2 lesson): whether
			//! a stamp was found, its local sample coordinate (as a full
			//! RayIntersectionGeometric copied from the query hit, with
			//! ptCoord = localUV and txFootprint invalidated), and its
			//! alpha (the Porter-Duff mix factor) -- computed ONCE so RGB/
			//! spectral/alpha paths can never disagree on which candidate
			//! is on top.  `ri2` is a plain copy of the query `ri` (NOT
			//! stamp-local) when found == false -- RayIntersectionGeometric
			//! has no default constructor, so HitInfo always carries a
			//! valid copy rather than an out-param the caller must default-
			//! construct.
			struct HitInfo
			{
				bool						found;
				Scalar						alpha;
				RayIntersectionGeometric	ri2;
				explicit HitInfo( const RayIntersectionGeometric& base ) : found( false ), alpha( 0 ), ri2( base ) {}
			};
			HitInfo Resolve( const RayIntersectionGeometric& ri ) const;

		public:
			ScatterPainter(
				const IPainter&		source_,
				const IPainter&		background_,
				const Scalar		cellScale_,
				const Scalar		stampScale_,
				const Scalar		jitterPosition_,
				const Scalar		jitterRotationDeg_,
				const Scalar		jitterScale_,
				const Scalar		probability_,
				const unsigned int	seed_ );

			ScatterPainter( const ScatterPainter& ) = delete;
			ScatterPainter& operator=( const ScatterPainter& ) = delete;

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			SpectralPacket	GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri ) const;

			// No animatable state (v1) -- `source`/`background`'s own
			// painters may be keyframed, which flows through automatically
			// since we re-evaluate them at every Get* call.
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) {}
			void RegenerateData() {}
		};
	}
}

#endif
