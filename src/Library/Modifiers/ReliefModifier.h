//////////////////////////////////////////////////////////////////////
//
//  ReliefModifier.h - Painter-driven shading-normal micro-relief.
//
//  The height field is ANY `IScalarPainter` -- an expression, a voronoi
//  cell field, a ramp, a texture, or any colour painter through the
//  existing `scalar_painter { painter X channel R }` bridge -- evaluated
//  as a 3D field by central difference in the hit's tangent plane.  That
//  is the whole point: RISE's procedural texture stack authors fields
//  that already drive albedo and every microsurface scalar on any
//  geometry with no texcoords, and before this modifier none of them
//  could tilt the shading normal.  The only two normal-perturbing
//  modifiers were `bumpmap_modifier` (an IFunction2D at ri.ptCoord, UV
//  only; REMOVED 2026-09-06, this class is its replacement) and
//  `normal_map_modifier` (a decoded image), so the same grain
//  field that darkened and roughened a surface could not also emboss it,
//  and authored variation read as paint on plastic.
//
//  See docs/RELIEF_MODIFIER_DESIGN.md for the full design; 3.2 is the
//  perturbation and 3.3 the step rule, both restated at their sites in
//  ReliefModifier.cpp.
//
//  SIGN.  Positive height RISES ALONG +N (Blinn 1978; PBRT-v4 10.3):
//
//      N' = normalize( N - scale * ( h_T * T + h_B * B ) )
//
//  This is the OPPOSITE of the removed `bumpmap_modifier`, whose
//  `vNormal + T*df` tilted the normal TOWARD the rise, i.e. treated its
//  field as DEPTH.  The migrator folds the sign (design 7.2), as does the
//  ABI-frozen `RISE_API_CreateBumpMapModifier` shim; it is not a free choice
//  here, it is the convention every other renderer's authors expect.
//
//  SLOPE CLAMP (`maxSlope`, opt-in, default 0 = off).  The perturbation
//  above is UNBOUNDED in the field's slope: `scale * |grad h|` is a
//  tangent-plane tilt, and a fine fbm field whose slope is O(10) at a
//  `scale` large enough to be legible tilts `N'` to within a degree or
//  two of the surface plane.  `N'` never crosses the plane -- the
//  perturbation is perpendicular to `N`, so `N'.N = 1/sqrt(1+|g|^2) > 0`
//  always -- but it does cross the GEOMETRIC HORIZON as seen from the ray
//  or from the light, and the materials' geometric-horizon gates (see
//  CookTorranceSPF::Scatter and its siblings) then reject nearly every
//  sampled direction and the surface shades BLACK.  That was the measured
//  practical failure on `weathered_workbench`'s grain: invisible at a
//  `scale` small enough to be safe, black speckle at a `scale` large
//  enough to read.  With `maxSlope > 0` the scaled gradient is rescaled
//  to that magnitude when it exceeds it -- DIRECTION PRESERVED, magnitude
//  bounded -- so the tilt is capped at `atan(maxSlope)` and the shading
//  and geometric hemispheres always overlap by at least
//  `90deg - atan(maxSlope)`.  `scale` then sets the amplitude the shallow
//  parts of the field get and the clamp holds the peaks, which is what
//  makes a fine field legible at all.  0 keeps the legacy unbounded
//  behaviour (and is what the migrator writes).
//
//  THIS BOUND IS PER MODIFIER; STACKED RELIEFS ADD THEIR TILTS.  The
//  overlap guarantee above bounds the tilt THIS application adds relative
//  to the vNormal it receives, not the total against the original
//  geometric normal.  Under ModifierStack (design 4), two relief modifiers
//  each at maxSlope 0.30 compose to a total tilt of up to 2*atan(0.30),
//  not the atan(0.30) a single clamp promises -- see
//  docs/RELIEF_MODIFIER_DESIGN.md 3.2 and ReliefModifierTest's test 11g.
//

//  DOMAIN.  `Surface` (the default) makes the field a function of the 3D
//  hit and takes the step in the tangent plane in WORLD units -- works on
//  any geometry with a normal, texcoords not required, and the
//  tangent-plane gradient of a 3D field is frame-independent so the
//  arbitrary CreateFromW tangent on tangent-less geometry is CORRECT,
//  not a compromise.  `UV` makes it a function of (u,v) with the step in
//  texture units along onb.u()/onb.v() -- byte-for-byte the legacy
//  sampling geometry, so migration off the removed `bumpmap_modifier`
//  is lossless and image heightfields authored in UV keep working.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 5, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RELIEF_MODIFIER_
#define RELIEF_MODIFIER_

#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		//! Which domain the height field is a function of, and therefore
		//! which point field the central difference steps in.
		enum class ReliefDomain
		{
			//! The 3D hit.  Step is taken in the tangent plane in WORLD
			//! units; ptIntersection, ptObjIntersec and (when derivatives
			//! are available) ptCoord all move consistently.  The default,
			//! and the recommended mode.
			Surface,

			//! The (u,v) texcoord.  Step is taken on ptCoord only, in
			//! texture units.  For scenes migrated off the removed
			//! `bumpmap_modifier`, and for image heightfields authored in UV.
			UV
		};

		class ReliefModifier :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~ReliefModifier( );

			const IScalarPainter&	height;		///< The height field; `.v[0]` is read (requireSingle at the parser)
			Scalar					dScale;		///< Amplitude: field units -> world units (surface) / UV units (uv)
			ReliefDomain			domain;		///< Which point domain the field and the step live in
			Scalar					dStep;		///< Central-difference half-step; <= 0 selects the automatic rule (design 3.3)
			Scalar					dMaxSlope;	///< Upper bound on |scale*grad h|; <= 0 or non-finite = unclamped

		public:
			ReliefModifier(
				const IScalarPainter& height_,	///< [in] Height field (addref'd)
				const Scalar scale_,			///< [in] Amplitude; 0 or non-finite makes the modifier inert
				const ReliefDomain domain_,		///< [in] Surface (3D field) or UV
				const Scalar step_,				///< [in] Half-step; <= 0 = auto per design 3.3
				const Scalar maxSlope_			///< [in] Tangent-plane tilt bound (a slope: 1 = 45deg); <= 0 = no clamp
				);

			void Modify( RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
