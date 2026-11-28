//////////////////////////////////////////////////////////////////////
//
//  SDFGeometry.h - Signed-distance-field (implicit) geometry.
//
//  Composed from transformed primitives via hard/smooth-minimum booleans
//  and sphere-traced. Each part transforms into its local frame:
//    lp = Rinv * (p - pos);  ls = lp / scale
//  and scales its distance by min|scale| (conservative Lipschitz factor).
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
#include "../Interfaces/IFunction2D.h"	// heightfield mode field source (addref'd)
#include "../Interfaces/ISurfaceSignalProvider.h"	// occlusion()/thickness() dispatch (design doc Phase 2)
#include <vector>
#include <cstddef>		// std::size_t (NumParts() et al.) -- <vector> is not required to declare it
#include <mutex>		// std::once_flag for the lazily-built surface-sampling structure
#include <memory>		// std::unique_ptr<std::once_flag> -- resettable for animated fields
#include <algorithm>	// std::min / std::max (SelfHitRootFloor's Lipschitz scan)
#include <cmath>		// std::fabs (same)

namespace RISE
{
	namespace Implementation
	{
		//! Also implements ISurfaceSignalProvider (plain non-virtual, non-refcounted
		//! borrowed back-pointer; lifetime is the geometry's) to answer occlusion(),
		//! convexity(), and thickness() directly from the distance field.
		class SDFGeometry : public Geometry, public ISurfaceSignalProvider
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
				ePrimRoundCone= 6,	//!< a = base radius (at y=0), b = tip radius (at y=c), c = height (axis = local Y)
				//!< Barr superquadric (pole axis = local Y): a = radius, b = e1 (latitude),
				//!< c = e2 (longitude), round unused (0). Exponents clamped to [0.1, 2]
				//!< to preserve convexity and a conservative Lipschitz distance bound.
				ePrimSuperellipsoid = 7
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
				Vector3  euler;		//!< RAW authoring rotation (Euler degrees, Rz*Ry*Rx) -- the source for cx/cy/cz; kept so keyframing rotation can re-derive the columns
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

			//! Parses newline-separated part lines into `out` (skipping blanks and `#` comments):
			//!   <prim> <op> <k>  <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <m n o>  <round>
			//! Unknown, malformed, or trailing tokens hard-fail with `szContext` and 1-based line number.
			static bool ParsePartLines(
				const char* szSource,	///< [in] Newline-separated part lines
				const char* szContext,	///< [in] Label for diagnostics (file path or "<inline part list>")
				std::vector<Part>& out );

			SDFGeometry( const std::vector<Part>& parts, const unsigned int maxSteps, const Scalar surfaceEpsilonFraction, const unsigned int samplingDetail = 64 );

			//! Heightfield mode: analytic surface z = scale*field(u,v) over [-radius,radius]^2
			//! (u=(x+R)/2R, v=(y+R)/2R), sphere-traced with O(1) memory.
			SDFGeometry( const IFunction2D* field, const Scalar radius, const Scalar scale,
			             const unsigned int maxSteps, const Scalar surfaceEpsilonFraction,
			             const unsigned int samplingDetail = 64 );

			//! Read-only derived part list (rotation columns, invScale, minScale resolved);
			//! empty in heightfield mode.
			const std::vector<Part>& GetParts() const { return m_parts; }

			//! Unpadded object-space bounding points of thin additive (union/smin) parts
			//! whose thickness is below `thinThickness` (omits subtract/intersect parts;
			//! empty in heightfield mode).
			void GetThinPartPoints( const Scalar thinThickness, std::vector<Point3>& out ) const;

			//! Evaluates the sequential fold of `parts` at `p` (same math as Map());
			//! empty `parts` returns +1e30.
			static Scalar EvaluateParts( const std::vector<Part>& parts, const Point3& p );

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

			//! One-sided finite-difference divergence of the unit normal (`div n_hat = k1+k2 = 2H`).
			//! Costs ~18 Map() calls (gated on SurfaceCurvatureDemand at hit time); returns 0 on degenerate step.
			Scalar  DivergenceOfUnitNormal( const Point3& y, const Vector3& n, const Scalar hfd ) const;

			//! Shared finite-difference step for curvature (above m_eps, proportional to m_diagonal).
			Scalar  CurvatureFDStep() const;
			//! Sphere-trace along (o + t*dir) from tStart within [.., t1]; returns true + tHit on hit.
			bool    March( const Point3& o, const Vector3& dir, const Scalar tStart, const Scalar t1, Scalar& tHit ) const;
			void    ComputeBounds();

			//! Recomputes cx/cy/cz from pt.euler and invScale/minScale from pt.scale.
			static void RecomputePartDerived( Part& pt );

			//! Computes m_hfLip (safe sphere-trace Lipschitz bound) for heightfield mode.
			void    ComputeHeightfieldLipschitz();

			//! Resets the lazy surface-sampling cache (called single-threaded from RegenerateData).
			void    InvalidateSamplingStructure();

			//! Two-step Newton projection onto the zero set: p -= Map(p) * GradientNormal(p).
			Point3  ProjectToSurface( const Point3& p ) const;

			//! Area ratio dA_surface / dA_chord = |m.n| / (1 + d*(k1+k2)) clamped to [0.5, 2];
			//! writes projected point `y` and normal `n`.
			Scalar  ProjectionJacobian( const Point3& x, const Vector3& m, Point3& y, Vector3& n, bool* clamped = 0 ) const;

			//! Freudenthal 6-tet marching tetrahedra over the padded bbox (`cells` along longest axis);
			//! emits outward-wound edge-welded triangles with Newton-projected vertices.
			void    GenerateSurfaceMesh( const unsigned int cells, std::vector<Point3>& verts, std::vector<unsigned int>& triIndices ) const;

			//! One triangle of the lazily-built surface-sampling structure.
			struct SampleTri
			{
				Point3  a, b, c;
				Vector3 m;			//!< unit chord-triangle normal
				Scalar  jBound;		//!< bound of ProjectionJacobian over the triangle
				Scalar  cumArea;	//!< cumulative proposal weight (chord area * jBound)
			};

			//! Thread-safe (std::call_once) lazy build of m_sampleTris / m_surfaceArea.
			void    EnsureSamplingStructure() const;

			unsigned int                    m_samplingDetail;	//!< cells along longest bbox axis for sampling mesh
			mutable std::unique_ptr<std::once_flag>  m_samplingOnce;
			mutable std::vector<SampleTri>  m_sampleTris;
			mutable Scalar                  m_surfaceArea;
			mutable Scalar                  m_proposalTotal = 0;	//!< total proposal weight (CDF sum)
			mutable unsigned int            m_missedFeatureCells = 0;	//!< definite-miss cells detected during sampling build

			// Heightfield mode members (kept last to preserve constructor init order).
			bool                m_isHeightfield = false;
			const IFunction2D*  m_pHeightfield  = 0;	//!< height field f(u,v) in [0,1]; addref'd
			Scalar              m_hfRadius      = 0;	//!< disk domain radius in local XY (object units)
			Scalar              m_hfScale       = 0;	//!< world amplitude: z = m_hfScale * f(u,v)
			Scalar              m_hfLip         = 2;	//!< Lipschitz bound sqrt(1 + maxslope^2)

		public:
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			//! Validates signal query and resolves object-space radius `outR` and residual `outD0 = Map(hit)`.
			//! Returns false on degenerate bounds, non-positive radius, or heightfield mode.
			bool PrepareSignalQuery( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, Scalar& outR, Scalar& outD0 ) const;

			//! Upper bound on distance via bracketed sign change (never raw 1-Lipschitz Map,
			//! which can under-read). Early-outs when Map(p) > maxDistObject; refuses if no
			//! sign flip occurs within budget or in heightfield mode.
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! Returns raw 1-Lipschitz Map(p) as signed lower bound (exact sign: <= 0 iff inside;
			//! outExact is always false). Refuses in heightfield mode.
			bool SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
				Scalar& outSigned, bool& outExact ) const override;

			//! Directional hemisphere visibility: fraction of 12 cosine-weighted outward rays
			//! escaping distance `radiusFraction * m_diagonal`. Planar/convex features read 1;
			//! ignores `bRadiusIsConstant`.
			bool ComputeOcclusion( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! Ball-volume convexity: `clamp(2A - 1, 0, 1)` via 32 mirrored samples (0 = flat/concave,
			//! 0.5 = 90-deg arris, 0.75 = 3-face corner, 3R/(8*rho) on sphere). Ignores `bRadiusIsConstant`.
			bool ComputeConvexity( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! Inward march along -n to far zero crossing, normalized by query radius: min(w/R, 1).
			//! Ignores `bRadiusIsConstant`.
			bool ComputeThickness( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest() const override { return true; }

			//! Tessellates via marching tetrahedra (`detail` clamped to [8, 512]) with
			//! Newton-projected vertices and exact field-gradient normals.
			bool TessellateToMesh(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords,
				const unsigned int     detail ) const override;

			//! True when GetArea() > 0 and SuspectedMissedFeatureCells() == 0 (missed features
			//! would break the NEE sampling contract and introduce MIS bias). May lazily build
			//! the sampling structure on first call (thread-safe).
			inline bool CanBeAreaLight() const override { return GetArea() > 0 && SuspectedMissedFeatureCells() == 0; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;

			//! Standoff floor for March()'s 2*m_eps step-off band: divided by incidence cosine
			//! (clamped at 0.05) and the owning part's local minScale/maxScale Lipschitz ratio
			//! (floored at 1e-9; global min fallback on blend seams), capped at 0.5 * m_diagonal.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override;

			//! Number of authored primitives (0 in heightfield mode even though non-empty).
			inline std::size_t NumParts() const { return m_parts.size(); }

			//! Lower bound on grid cells where marching tets missed renderable surface (all 8
			//! corners same sign, center opposite). Triggers lazy sampling build on first call.
			unsigned int SuspectedMissedFeatureCells() const;

			// Keyframable params (`<i>` = 0-based part index):
			//   part<i>.{position,rotation,scale,size} (vec3), part<i>.{blend,round} (scalar),
			//   heightfield_scale (scalar). op/type are not animatable.
			// Note: RegenerateData() rebuilds bounds/Lipschitz/sampling CDF per frame, but
			// luminary-set membership (CanBeAreaLight) is fixed at t=0.
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData() override;
		};
	}
}

#endif
