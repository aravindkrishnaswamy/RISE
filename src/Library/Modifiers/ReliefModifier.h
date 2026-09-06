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
//  only) and `normal_map_modifier` (a decoded image), so the same grain
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
//  This is the OPPOSITE of `bumpmap_modifier`, whose `vNormal + T*df`
//  tilts the normal TOWARD the rise, i.e. treats its field as DEPTH.
//  The migrator folds the sign (design 7.2); it is not a free choice
//  here, it is the convention every other renderer's authors expect.
//
//  DOMAIN.  `Surface` (the default) makes the field a function of the 3D
//  hit and takes the step in the tangent plane in WORLD units -- works on
//  any geometry with a normal, texcoords not required, and the
//  tangent-plane gradient of a 3D field is frame-independent so the
//  arbitrary CreateFromW tangent on tangent-less geometry is CORRECT,
//  not a compromise.  `UV` makes it a function of (u,v) with the step in
//  texture units along onb.u()/onb.v() -- byte-for-byte the legacy
//  sampling geometry, so migration off `bumpmap_modifier` is lossless
//  and image heightfields authored in UV keep working.
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
			//! texture units.  For legacy `bumpmap_modifier` migration and
			//! for image heightfields authored in UV.
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

		public:
			ReliefModifier(
				const IScalarPainter& height_,	///< [in] Height field (addref'd)
				const Scalar scale_,			///< [in] Amplitude; 0 or non-finite makes the modifier inert
				const ReliefDomain domain_,		///< [in] Surface (3D field) or UV
				const Scalar step_				///< [in] Half-step; <= 0 = auto per design 3.3
				);

			void Modify( RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
