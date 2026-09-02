//////////////////////////////////////////////////////////////////////
//
//  MappingPainter.h - mapping_painter (doc 88 P2.3): wraps a source
//  painter and transforms the DOMAIN it is evaluated at before
//  delegating -- the general-purpose scale/rotate/translate/reproject
//  tool for the scene language.
//
//  Design:
//
//    - `projection uv` transforms `ptCoord` (2D) before delegating --
//      this is what finally gives the scene language a general-purpose
//      UV scale/rotate/offset (UVTransformPainter already does this,
//      but it is the glTF KHR_texture_transform BRIDGE: constructed
//      only by the glTF importer, with KHR's specific rotation sign
//      convention.  mapping_painter is the author-facing tool; the two
//      are NOT merged this slice).
//    - `projection world` / `object` transform `ptIntersection` /
//      `ptObjIntersec` (3D) respectively before delegating.  Per the
//      RayIntersectionGeometric contract, a 3D-domain source painter
//      (perlin3d, worley3d, voronoi3d, ...) reads ONE of those two
//      fields (see docs/skills/procedural-textures.md's "2D vs 3D"
//      section and the voronoi3d `space` field for the one painter
//      that reads ptObjIntersec by default); `world`/`object` here
//      patch exactly the field that projection names and leave every
//      OTHER domain field (ptCoord, the field `world`/`object` did NOT
//      pick) untouched -- so wrapping a UV-domain painter in
//      `projection world` is a silent no-op (it never reads
//      ptIntersection), by design: point it at `projection uv`
//      instead.
//    - `projection triplanar` is for UV-CONSUMING sources on UV-less
//      geometry (an sdf_geometry, a heavily displaced mesh with no
//      authored UVs): it samples the source THREE times, each time
//      patching `ptCoord` from a different pair of components of the
//      (TRS-transformed) WORLD position -- (y,z) for the X-facing
//      sample, (x,z) for Y-facing, (x,y) for Z-facing -- and blends the
//      three results by |N.axis|^blend_sharpness (world shading
//      normal, `ri.vNormal`), normalized.  Triplanar always uses the
//      WORLD position/normal; there is no separate object-space
//      triplanar variant this slice.
//
//  TRS composition order (all three domains): SCALE, then ROTATE, then
//  TRANSLATE -- p' = translate + rotate(scale(p)), matching
//  UVTransformPainter's documented "T * R * S" convention.  The 3D
//  matrix is built as `Translation(translate) * XRotation(rx) *
//  YRotation(ry) * ZRotation(rz) * Stretch(scale)` -- the EXACT
//  expression shape Transformable::SetOrientation/SetPosition use to
//  combine an object's position + orientation + scale.  Because
//  Matrix4Ops::operator*'s `A * B` applies B FIRST (Transform(A*B,p)
//  == Transform(A,Transform(B,p)), i.e. the RIGHTMOST factor is
//  innermost), the actual per-point order is: Stretch first, then
//  ZRotation, then YRotation, then XRotation, then Translation last --
//  Z-then-Y-then-X, the SAME rotation order `standard_object`'s
//  `orientation` field uses (see Transformable.cpp's `orientation =
//  XRotation(x)*YRotation(y)*ZRotation(z)`).  So mapping_painter's
//  `rotate` reads exactly like an object's `orientation`.  For
//  `projection uv`, only `scale.x/y`, `rotate.z`,
//  and `translate.x/y` are meaningful -- `scale.z`, `rotate.x`,
//  `rotate.y`, and `translate.z` are IGNORED (documented, not merely
//  silently dropped): a 2D domain has no z-axis rotation-mixing
//  behaviour to inherit from the 3D matrix path, so uv uses its own
//  dedicated 2D rotation (the standard CCW `cos/-sin/sin/cos` form)
//  rather than projecting through the shared 3D matrix.
//
//  Spectral consistency (the S2 lesson): GetColor, GetColorNM,
//  GetSpectrum, and GetAlpha for `triplanar` ALL route through the
//  same ComputeTriplanar() helper for the axis weights and the three
//  ptCoord values, so the RGB and NM/spectral paths can never diverge
//  on which axes contributed how much.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-20
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAPPING_PAINTER_
#define MAPPING_PAINTER_

#include "Painter.h"
#include "../Utilities/Math3D/Math3D.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class MappingPainter : public Painter
		{
		public:
			enum Projection
			{
				Proj_UV        = 0,
				Proj_World     = 1,
				Proj_Object    = 2,
				Proj_Triplanar = 3
			};

		protected:
			const IPainter&			source;
			const Projection			projection;

			// 2D (uv) transform, precomputed at construction.  Only used
			// when projection == Proj_UV.  Standard CCW rotation, applied
			// AFTER scale, translate applied last (T * R * S order).
			Scalar						scaleU, scaleV;
			Scalar						translateU, translateV;
			Scalar						cosRz, sinRz;
			bool						isIdentityUV;

			// 3D transform, precomputed as a single combined matrix at
			// construction.  Used when projection == Proj_World, Proj_
			// Object, or Proj_Triplanar (triplanar transforms the WORLD
			// position before deriving its three axis-projected UVs).
			Matrix4						xform3D;
			bool						isIdentity3D;

			const Scalar				blendSharpness;	// triplanar only

			virtual ~MappingPainter()
			{
				source.release();
			}

			inline Point2 ApplyUV( const Point2& uv ) const
			{
				if( isIdentityUV ) return uv;
				const Scalar su = scaleU * uv.x;
				const Scalar sv = scaleV * uv.y;
				return Point2(
					cosRz * su - sinRz * sv + translateU,
					sinRz * su + cosRz * sv + translateV );
			}

			inline Point3 Apply3D( const Point3& p ) const
			{
				if( isIdentity3D ) return p;
				return Point3Ops::Transform( xform3D, p );
			}

			//! Shared triplanar weight + coordinate computation.  ALL of
			//! GetColor / GetColorNM / GetSpectrum / GetAlpha call this so
			//! the RGB, spectral, and alpha paths can never diverge on
			//! which axis contributed how much (the S2 lesson).  `w` are
			//! the three normalized blend weights (X, Y, Z axis order);
			//! `uv2` are the three per-axis UV coordinates, derived from
			//! the TRS-transformed WORLD position per the axis convention
			//! in the file header comment.
			inline void ComputeTriplanar( const RayIntersectionGeometric& ri, Scalar w[3], Point2 uv2[3] ) const
			{
				const Point3 p = Apply3D( ri.ptIntersection );
				uv2[0] = Point2( p.y, p.z );	// X-facing sample
				uv2[1] = Point2( p.x, p.z );	// Y-facing sample
				uv2[2] = Point2( p.x, p.y );	// Z-facing sample

				// P2 (S7 review round 1): axis-selection weights are
				// computed from `ri.vNormal` VERBATIM -- deliberately NOT
				// run through the TRS transform (xform3D) the way the
				// POSITION is above.  The position is remapped because
				// that is the actual sampling domain being reprojected;
				// axis DOMINANCE is a purely geometric fact about which
				// way the surface is locally facing, and stays geometric
				// regardless of how mapping_painter chooses to retile the
				// pattern painted onto it.  Rotating the axis-selection
				// normal by the same xform3D would double-apply the
				// rotation on curved/varying geometry -- the position
				// sample would spin with `rotate` (correct, that is the
				// retiling), and the axis blend would ALSO spin, so a
				// point already dominated by e.g. the Z axis could drift
				// into a different axis's basin purely from `rotate`,
				// with no relationship to the surface's actual shape.
				const Vector3& n = ri.vNormal;
				Scalar aw[3] = {
					std::pow( std::fabs( n.x ), blendSharpness ),
					std::pow( std::fabs( n.y ), blendSharpness ),
					std::pow( std::fabs( n.z ), blendSharpness )
				};
				const Scalar sum = aw[0] + aw[1] + aw[2];
				if( sum > Scalar(1e-12) ) {
					w[0] = aw[0] / sum;
					w[1] = aw[1] / sum;
					w[2] = aw[2] / sum;
				} else {
					// Degenerate normal (zero-length) -- fall back to an
					// even split rather than propagate a 0/0 NaN.
					w[0] = w[1] = w[2] = Scalar(1.0/3.0);
				}
			}

		public:
			MappingPainter(
				const IPainter&		source_,
				const Projection		projection_,
				const Vector3&			scale,
				const Vector3&			rotateDeg,
				const Vector3&			translate,
				const Scalar			blendSharpness_ );

			MappingPainter( const MappingPainter& ) = delete;
			MappingPainter& operator=( const MappingPainter& ) = delete;

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			//! SINGLE-SOURCE FORWARDER (Stage C slice 2): re-projects
			//! `ri` and forwards to the SAME source's `GetRadianceNM`.
			//! See the implementation comment for why the generic
			//! composed-GetColor default is wrong here.
			Scalar			GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
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
