//////////////////////////////////////////////////////////////////////
//
//  StochasticTilePainter.h - stochastic_tile_painter (doc 88 P3.1, S8):
//  hex-tiling with histogram-preserving blending over a source
//  painter -- one photo of bark/plaster/rust becomes an unbounded
//  non-repeating cover, with no visible tile-grid repetition.
//
//  Algorithm (Heitz & Neyret 2018, "High-Performance By-Example Noise
//  using a Histogram-Preserving Blending Operator", HPG 2018 / JCGT
//  companion; Burley 2019, "On Histogram-preserving Blending for
//  Randomized Texture Tiling", JCGT 8(4)):
//
//    1. TRIANGLE LATTICE.  `ptCoord * tile_scale` is expressed in the
//       basis of an equilateral-triangle lattice (e1 = (1,0),
//       e2 = (0.5, sqrt(3)/2)) -- the standard change of basis that
//       turns a unit-square grid into equilateral triangles (self-
//       derived from the basis-vector definition below, NOT
//       transcribed from either paper's reference shader, though it
//       is necessarily the same linear map -- there is only one
//       basis-change matrix for a given target basis).  Each query
//       point falls in one of the two triangles of its enclosing unit
//       cell; ComputeHexTiling() returns that triangle's 3 integer
//       lattice-vertex ids and their barycentric weights (sum = 1).
//       The DUAL of this lattice is hexagonal (each vertex's region
//       of local dominance is a hexagon), which is where "hex-tiling"
//       gets its name -- the code below only ever touches the
//       triangular PRIMAL lattice.
//    2. PER-VERTEX OFFSET.  Each lattice vertex id hashes (via
//       NoiseCore::WorleyHashCell, the same integer-mixing hash
//       Worley3DPainter already uses for cell ids -- see Hash01() in
//       the .cpp) to a UV offset in [0,1)^2.  The three triangle
//       vertices therefore sample three DIFFERENT, uncorrelated
//       patches of `source` (assumed periodic/tileable at integer UV
//       offsets, e.g. a wrapping image_painter) -- this is what
//       breaks the visible grid repetition.  No per-vertex rotation
//       (the papers' fuller treatment includes one) -- v1 scope is
//       offset only; a documented follow-up, not attempted this
//       slice.
//    3. WEIGHT SHARPENING.  The barycentric weights are exponentiated
//       (w_i' = w_i^blend_gamma, then renormalized to sum 1) before
//       blending -- this crisps the triangle seams (plain barycentric
//       blending reads as visibly blurred/ghosted where two samples
//       overlap).  Default blend_gamma = 7.0, matching `g_exp` in
//       Mikkelsen's "Practical Real-Time Hex-Tiling" (JCGT 11(3),
//       2022) -- a later, independently-published reproduction of
//       Heitz & Neyret's weight-sharpening step that states a
//       concrete numeric default where the original 2018 paper does
//       not.
//    4. HISTOGRAM-PRESERVING (variance-preserving) BLEND.  A plain
//       weighted average of 3 samples of the SAME textured source
//       shrinks variance (regression to the mean) -- exactly the
//       problem Heitz & Neyret's operator exists to solve.  Given
//       source mean mu (the `mean` parameter -- see below) the
//       restored output is:
//
//           out = mu + ( sum_i w_i' * (x_i - mu) ) / sqrt( sum_i w_i'^2 )
//
//       DENOMINATOR BOUND (provable, no runtime guard needed): the
//       w_i' are normalized (sum_i w_i' == 1) and non-negative, so by
//       the power-mean / Cauchy-Schwarz inequality sum_i w_i'^2 is
//       minimized when all three are equal (1/3 each), giving
//       sum = 3*(1/3)^2 = 1/3, and is at most 1 (when one weight -> 1
//       and the others -> 0).  So sqrt(sum_i w_i'^2) in [1/sqrt(3), 1]
//       always -- strictly positive, never a division by (near) zero.
//    5. MEAN.  `mean` is parser-supplied (default 0.5 0.5 0.5), NOT
//       automatically estimated from `source`.  Automatic estimation
//       (e.g. box-filtering the source's mip chain, or a one-time
//       Monte-Carlo prepass over its UV domain) is a real option in
//       principle, but no painter in this codebase has a "give me
//       your average value" prepass surface -- IPainter is a pure
//       per-hit function, not an asset with introspectable statistics
//       -- so building one is out of scope for this slice.  Documented
//       here as a possible follow-up; the workaround is what
//       uniformcolor_painter / ramp_painter already do: the AUTHOR
//       supplies the number.
//
//  Domain: UV (`ri.ptCoord`) only -- this painter never reads
//  ptIntersection / ptObjIntersec.  For world-space or triplanar
//  tiling (e.g. covering an sdf_geometry or heavily displaced mesh
//  with no authored UVs), wrap the WHOLE stochastic_tile_painter
//  chunk as the `source` of an outer `mapping_painter { projection
//  triplanar ... }` -- mapping_painter already patches `ptCoord` per
//  axis-blended sample before delegating, so composing the two gets
//  triplanar stochastic tiling for free.  This class deliberately
//  does NOT duplicate mapping_painter's projection logic.
//
//  Spectral consistency (the S2 lesson): GetColor, GetColorNM, and
//  GetSpectrum all route through ComputeHexTiling() for the triangle
//  weights/offsets, so the RGB and spectral paths can never disagree
//  on which 3 samples contributed how much.  `mean` is uplifted ONCE
//  at construction (RGBAlbedoSpectrum, Albedo kind -- same convention
//  RampPainter's stops use) so GetColorNM/GetSpectrum never uplift
//  per-sample.
//
//  GetAlpha does NOT apply the histogram-preserving restore -- alpha
//  is a coverage fraction in [0,1], not a value distribution with a
//  "histogram" to preserve, and mean/sqrt-restoring it could push it
//  outside [0,1].  It uses the SAME sharpened, normalized weights as
//  the colour path (so alpha stays spatially consistent with which
//  tile dominates) but as a plain convex combination.
//
//  txFootprint invalidation: each of the 3 samples is at an offset
//  UV unrelated to the original ptCoord's screen-space derivatives --
//  same rationale as TexCoord1Painter.h's UV1 swap and
//  MappingPainter's Proj_UV case (the S7 P1-B precedent).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-20
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STOCHASTIC_TILE_PAINTER_
#define STOCHASTIC_TILE_PAINTER_

#include "Painter.h"
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace Implementation
	{
		class StochasticTilePainter : public Painter
		{
		protected:
			const IPainter&			source;
			const Scalar				tileScale;
			const unsigned int			seed;
			const RISEPel				mean;
			const RGBAlbedoSpectrum		meanSpec;	// mean, uplifted ONCE at construction (Albedo kind)
			const Scalar				blendGamma;

			virtual ~StochasticTilePainter();

			//! Shared triangle-lattice computation: fills `w` with the 3
			//! sharpened + normalized barycentric weights (sum == 1) and
			//! `offsetUV` with the 3 per-vertex-offset sample coordinates
			//! (see the file header for the full derivation).  Called ONCE
			//! per Get* invocation; every Get* loops over the same 3
			//! samples it returns.
			void ComputeHexTiling( const Point2& coord, Scalar w[3], Point2 offsetUV[3] ) const;

		public:
			StochasticTilePainter(
				const IPainter&		source_,
				const Scalar		tileScale_,
				const unsigned int	seed_,
				const RISEPel&		mean_,
				const Scalar		blendGamma_ );

			StochasticTilePainter( const StochasticTilePainter& ) = delete;
			StochasticTilePainter& operator=( const StochasticTilePainter& ) = delete;

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			SpectralPacket	GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri ) const;

			// No animatable state (v1) -- `source`'s own painter may be
			// keyframed, which flows through automatically since we
			// re-evaluate `source` at every Get* call.
			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) {}
			void RegenerateData() {}
		};
	}
}

#endif
