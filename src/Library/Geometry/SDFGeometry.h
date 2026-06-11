//////////////////////////////////////////////////////////////////////
//
//  SDFGeometry.h - Signed-distance-field (implicit) geometry.
//
//  A list of transformed primitives (sphere / box / round box /
//  cylinder / torus / capsule / round cone) composed with hard- or
//  SMOOTH-MINIMUM boolean operations, ray-traced by sphere tracing.
//  This is the primitive for melded / filleted organic shapes that the
//  analytic primitives + hard-boolean CSG cannot express -- e.g. watch
//  lugs flowing into a bezel with a real fillet, or fat-in-the-middle /
//  tapering hands.
//
//  Each part is brought into its own local frame
//    lp = Rinv * (p - pos);  ls = lp / scale
//  the primitive distance is evaluated, and multiplied by min|scale|
//  (a conservative Lipschitz factor that keeps the sphere-trace from
//  overshooting under non-uniform scale).
//
//  Reference: Inigo Quilez, "Distance Functions" / "Smooth minimum".
//  https://iquilezles.org/articles/distfunctions/
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SDF_GEOMETRY_
#define SDF_GEOMETRY_

#include "Geometry.h"
#include <vector>
#include <mutex>		// std::once_flag for the lazily-built surface-sampling structure

namespace RISE
{
	namespace Implementation
	{
		class SDFGeometry : public Geometry
		{
		public:
			//! Primitive shapes (all centred at the part local origin).
			enum SDFPrim
			{
				ePrimSphere   = 0,	//!< a = radius
				ePrimBox      = 1,	//!< a,b,c = half-extents
				ePrimRoundBox = 2,	//!< a,b,c = half-extents, round = corner radius
				ePrimCylinder = 3,	//!< a = radius, b = half-height (axis = local Y)
				ePrimTorus    = 4,	//!< a = major radius, b = tube radius (ring in local XZ, around Y)
				ePrimCapsule  = 5,	//!< a = radius, b = half-height of the core segment (axis = local Y)
				ePrimRoundCone= 6	//!< a = base radius (at y=0), b = tip radius (at y=c), c = height (axis = local Y)
			};

			//! Boolean op used to fold a part into the running field.
			enum SDFOp
			{
				eOpUnion     = 0,	//!< hard union (min)
				eOpSmin      = 1,	//!< smooth union (polynomial smin, radius k)
				eOpSubtract  = 2,	//!< smooth subtraction of this part (radius k)
				eOpIntersect = 3	//!< smooth intersection with this part (radius k)
			};

			//! One transformed primitive in the field.
			struct Part
			{
				SDFPrim  type;
				SDFOp    op;
				Scalar   k;			//!< smin blend radius (object-space units); 0 = hard
				Point3   pos;		//!< part origin in object space
				Vector3  cx, cy, cz;//!< columns of the local->object rotation; Rinv*v = (cx.v, cy.v, cz.v)
				Vector3  scale;		//!< per-axis scale
				Vector3  invScale;	//!< 1/scale (precomputed)
				Scalar   minScale;	//!< min(|scale.x|,|scale.y|,|scale.z|)
				Scalar   a, b, c;	//!< primitive size params (see SDFPrim)
				Scalar   round;		//!< extra rounding radius
			};

			//! Build a Part from human-friendly inputs: euler angles in
			//! DEGREES (applied Rz*Ry*Rx), per-axis scale.  Precomputes the
			//! rotation columns + inverse scale.
			static Part MakePart(
				const SDFPrim type, const SDFOp op, const Scalar k,
				const Point3& pos, const Scalar exDeg, const Scalar eyDeg, const Scalar ezDeg,
				const Vector3& scale, const Scalar a, const Scalar b, const Scalar c, const Scalar round );

			//! Parses newline-separated part lines -- the ONE grammar shared by
			//! the scene chunk's inline `part` parameter and external parts files:
			//!     <prim> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <m n o>  <round>
			//! Blank lines and `#` comments are skipped.  Unknown / malformed /
			//! trailing tokens hard-fail with `szContext` + 1-based line number in
			//! the log (no silent fallback).  Appends parsed parts to `out`.
			//! Future part-grammar extensions land HERE; the chunk parser forwards
			//! lines verbatim and needs no change.
			static bool ParsePartLines(
				const char* szSource,	///< [in] Newline-separated part lines
				const char* szContext,	///< [in] Label for diagnostics (file path or "<inline part list>")
				std::vector<Part>& out );

			SDFGeometry( const std::vector<Part>& parts, const unsigned int maxSteps, const Scalar surfaceEpsilonFraction, const unsigned int samplingDetail = 64 );

		protected:
			virtual ~SDFGeometry();

			std::vector<Part>  m_parts;
			unsigned int       m_maxSteps;		//!< sphere-trace step cap
			Scalar             m_epsFrac;		//!< surface epsilon as a fraction of the bbox diagonal
			Scalar             m_eps;			//!< absolute surface epsilon (object-space units)
			BoundingBox        m_bbox;			//!< precomputed AABB (union of parts, blend-expanded)
			Scalar             m_diagonal;		//!< bbox diagonal length

			Scalar  Map( const Point3& p ) const;				//!< composed signed distance at p
			Vector3 GradientNormal( const Point3& p ) const;	//!< unit gradient (outward) normal
			//! March along (o + t*dir) from tStart, within [.., t1], to the
			//! next surface crossing.  Returns true + tHit on a hit.
			bool    March( const Point3& o, const Vector3& dir, const Scalar tStart, const Scalar t1, Scalar& tHit ) const;
			void    ComputeBounds();

			//! Newton-projects p onto the zero set: p -= Map(p) * GradientNormal(p),
			//! two iterations.  Marching-tet vertices and surface samples go through
			//! this so they lie ON the sphere-traced surface (within ~m_eps), not on
			//! the linear-interpolated tessellation of it.
			Point3  ProjectToSurface( const Point3& p ) const;

			//! Core mesher: marching TETRAHEDRA (Freudenthal 6-tet cube split -- face-
			//! consistent across the grid, no ambiguous cases, watertight by
			//! construction) over the padded bbox at `cells` cells along the longest
			//! axis.  Emits an edge-welded indexed triangle mesh with projected
			//! vertices, wound so geometric normals face OUTWARD (away from the
			//! inside corners of the generating tet).  Appends to the outputs.
			void    GenerateSurfaceMesh( const unsigned int cells, std::vector<Point3>& verts, std::vector<unsigned int>& triIndices ) const;

			//! One triangle of the lazily-built surface-sampling structure.
			struct SampleTri
			{
				Point3  a, b, c;
				Scalar  cumArea;	//!< cumulative area up to and including this triangle
			};

			//! Builds m_sampleTris / m_surfaceArea from GenerateSurfaceMesh at
			//! m_samplingDetail.  Thread-safe (std::call_once); derived-immutable
			//! cache, so conceptually const.  Cost is paid only by SDFs that are
			//! actually surface-sampled (emitters, point-set SSS).
			void    EnsureSamplingStructure() const;

			unsigned int                    m_samplingDetail;	//!< cells along the longest bbox axis for the sampling mesh
			mutable std::once_flag          m_samplingOnce;
			mutable std::vector<SampleTri>  m_sampleTris;
			mutable Scalar                  m_surfaceArea;
			mutable unsigned int            m_missedFeatureCells = 0;	//!< definite-miss cells found by EnsureSamplingStructure's detector

		public:
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const;
			BoundingBox GenerateBoundingBox() const;
			inline bool DoPreHitTest() const { return true; }

			//! Tessellates the SDF via marching tetrahedra.  `detail` = cells along
			//! the longest bbox axis (clamped to [8, 512]; expect ~detail^2 * 4
			//! triangles).  Vertices are Newton-projected onto the zero set and
			//! normals come from the exact field gradient, so the mesh converges to
			//! the sphere-traced surface much faster than raw marching output.
			//! Makes the SDF a valid base for DisplacedGeometry and mesh export.
			bool TessellateToMesh(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords,
				const unsigned int     detail ) const;

			//! TRUE first-class area-light support: UniformRandomPoint / GetArea are
			//! backed by a lazily-built tessellation of the field (Jacobian-weighted
			//! triangle CDF, samples projected onto the zero set).  Returns honest
			//! false when the field tessellates to nothing (zero surface area) OR
			//! when the missed-feature detector PROVED renderable surface absent
			//! from the sampling structure (SuspectedMissedFeatureCells() > 0): in
			//! that state the sampling contract "UniformRandomPoint covers the
			//! renderable surface" does not hold, and registering as a luminary
			//! anyway re-introduces the phantom-NEE-pdf MIS bias on exactly the
			//! missed component (NEE can never sample it while BSDF-hit emission
			//! is down-weighted as if it could).  False routes every consumer --
			//! LuminaryManager registration, the PT / EmissionShaderOp emission-MIS
			//! gates, the SSS shaderops -- to the consistent fallback (full-weight
			//! BSDF-hit emission, no NEE, no SSS): unbiased but noisy.  The cure is
			//! a higher sampling_detail; the build-time warning names the first
			//! missed location.
			//! NB: first call may build the sampling structure (thread-safe).
			inline bool CanBeAreaLight() const { return GetArea() > 0 && SuspectedMissedFeatureCells() == 0; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const;
			Scalar GetArea() const;

			//! Number of grid cells where the sampling mesher PROVABLY missed
			//! surface (all 8 cell corners on one side of the zero set, cell
			//! center on the other -- marching tets only see corner sign
			//! changes, so such a cell renders via sphere tracing but is
			//! absent from GetArea / UniformRandomPoint).  Best-effort lower
			//! bound: features that evade the center probe too stay
			//! undetected.  > 0 means raise sampling_detail before using the
			//! SDF as an emitter / SSS source.  Builds the sampling structure
			//! on first call.
			unsigned int SuspectedMissedFeatureCells() const;

			// Keyframable interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData();
		};
	}
}

#endif
