//////////////////////////////////////////////////////////////////////
//
//  SDFGeometry.h - Signed-distance-field (implicit) geometry.
//
//  A list of transformed primitives (sphere / box / round box /
//  cylinder / torus / capsule / round cone / superellipsoid) composed
//  with hard- or SMOOTH-MINIMUM boolean operations, ray-traced by
//  sphere tracing.
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
		//! Also an ISurfaceSignalProvider: the distance field it already
		//! carries answers `occlusion(radius)` and `thickness(radius)`
		//! DIRECTLY, with no rays, no bake, no scene access and no locks --
		//! which is why the SDF family is the one this arc lights up first
		//! (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.2).  Meshes reach the
		//! same two builtin names in Phase 3 through the same interface, out
		//! of a per-vertex bake.
		//! NOTE the plain (non-virtual, non-refcounted) second base: a
		//! provider pointer is a borrowed back-pointer whose lifetime is the
		//! geometry's, never an owning handle -- nothing deletes through it,
		//! so there is no diamond and no refcount to share.
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
				//! SUPERELLIPSOID (Barr superquadric), pole axis = local Y.
				//! a = radius, b = e1 (NORTH-SOUTH / latitude exponent), c = e2
				//! (EAST-WEST / longitude exponent); `round` is UNUSED (author 0),
				//! as on roundcone.  Ellipsoidal PROPORTIONS come from the part's
				//! own <sx sy sz> scale, whose conservative minScale Lipschitz
				//! factor partEval already applies -- the primitive itself is
				//! always radius-uniform.  The continuum: e1 = e2 = 1 is a SPHERE
				//! (exactly -- the field reduces to sdSphere); both -> 0 is a BOX;
				//! e1 -> 0 with e2 = 1 is a CYLINDER about local Y; e1 = e2 = 2 is
				//! an OCTAHEDRON; in between lie the cushions, rounded boxes and
				//! bicones.  BOTH exponents are CLAMPED to [0.1, 2] (kSEMinExp /
				//! kSEMaxExp in SDFGeometry.cpp): above 2 the solid stops being
				//! convex and the distance bound stops being CONSERVATIVE (a
				//! sphere-trace overshoot); below 0.1 the shape is already within
				//! 3.5 % of the box that `box` renders exactly and more cheaply.
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

			//! Heightfield mode: the exact analytic surface z = scale*field(u,v) over the
			//! square [-radius,radius]^2 (u=(x+R)/2R, v=(y+R)/2R), sphere-traced -- no
			//! tessellation, O(1) memory.  The exact-geometry ground truth twin of
			//! DisplacedGeometry.
			SDFGeometry( const IFunction2D* field, const Scalar radius, const Scalar scale,
			             const unsigned int maxSteps, const Scalar surfaceEpsilonFraction,
			             const unsigned int samplingDetail = 64 );

			//! Creature scaffold slice (2026-08-25): the FULLY DERIVED part
			//! list this object was built from -- cx/cy/cz rotation columns,
			//! minScale, invScale and every other RecomputePartDerived field
			//! already resolved, exactly as ParsePartLines/MakePart left
			//! them.  Read-only; empty for the heightfield-mode constructor
			//! (no parts).  Exists so a caller that only has a DERIVED Job
			//! (a skeleton_geometry chunk's own expansion is never written
			//! back to the CST text -- see SkeletonGeometryAsciiChunkParser's
			//! own doc, "this text is never saved to disk or shown to an
			//! author") can still recover the EXACT engine-computed parts a
			//! chunk expanded to, e.g. to run the shared blend-scale scan
			//! (ScanSdfGeometryBlendScaleOffenders_ in AgentSession.cpp)
			//! against a skeleton's real bones instead of re-deriving the
			//! expansion by hand in a second, driftable copy.
			const std::vector<Part>& GetParts() const { return m_parts; }

			//! Blend-domain-control slice (2026-08-25): the composed signed
			//! distance of an ARBITRARY parts list at an arbitrary point,
			//! WITHOUT constructing an SDFGeometry -- the exact same
			//! sequential fold `Map()` runs (union: hard min; smin/subtract/
			//! intersect: the polynomial blend at that part's own `k`), just
			//! exposed as a public static so a test can evaluate a
			//! skeleton_geometry's DERIVED parts (from a Job's
			//! GetSdfGeometryParts-equivalent, or hand-built for a fixture)
			//! at a probe point -- e.g. "does the gap between two
			//! deliberately-close joints still read as two distinct
			//! surfaces, or did an unrelated part's blend radius bridge
			//! them" -- without a full render.  `Map()` itself now forwards
			//! to this (see its own body) so the two can never drift; empty
			//! `parts` returns the same +1e30 "nothing here" sentinel the
			//! fold's own running-field seed uses.
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

			//! DIVERGENCE OF THE UNIT NORMAL FIELD at an on-surface point,
			//! `div n_hat = k1 + k2 = 2H` -- the one-sided finite difference
			//! that has been in this file since the curvature-corrected
			//! sampling weights were added (it was the `jacobianAt` lambda's
			//! inner math; extracted so the sampling structure and the
			//! intersection-time `curv` signal share ONE implementation
			//! rather than two copies of the same stencil).
			//!
			//! Costs 3 extra GradientNormal calls == ~18 extra Map()
			//! evaluations, each O(#parts).  That is why the intersection-time
			//! caller gates on SurfaceCurvatureDemand.
			//!
			//! `n` must be GradientNormal(y) -- passed in rather than
			//! recomputed because every caller already has it.  `hfd` is the
			//! FD step; use CurvatureFDStep() unless you have a reason not to.
			//! Returns 0 on a degenerate step, which reads as "flat" -- the
			//! honest neutral answer, never a NaN.
			Scalar  DivergenceOfUnitNormal( const Point3& y, const Vector3& n, const Scalar hfd ) const;

			//! The FD step the curvature stencil uses: comfortably above the
			//! surface epsilon (so the two GradientNormal evaluations are not
			//! differencing numerical noise) and a fixed fraction of the bbox
			//! diagonal (so it scales with the object).  Shared by the
			//! sampling-weight path and the intersection-time signal so a
			//! future tuning change moves both together.
			Scalar  CurvatureFDStep() const;
			//! March along (o + t*dir) from tStart, within [.., t1], to the
			//! next surface crossing.  Returns true + tHit on a hit.
			bool    March( const Point3& o, const Vector3& dir, const Scalar tStart, const Scalar t1, Scalar& tHit ) const;
			void    ComputeBounds();

			//! Recomputes a part's DERIVED fields (rotation columns cx/cy/cz from
			//! pt.euler; invScale + minScale from pt.scale) from its RAW authoring
			//! fields.  The single source of truth for the rotation/scale math --
			//! MakePart (build time) and SetIntermediateValue (keyframing rotation /
			//! scale) both route through it, so the two paths cannot drift.
			static void RecomputePartDerived( Part& pt );

			//! (Re)computes m_hfLip, the safe-sphere-trace Lipschitz bound for the
			//! heightfield, from m_pHeightfield / m_hfScale / m_hfRadius.  Shared by
			//! the heightfield ctor and RegenerateData (keyframing heightfield_scale).
			void    ComputeHeightfieldLipschitz();

			//! Drops the lazily-built surface-sampling cache so the NEXT GetArea /
			//! UniformRandomPoint rebuilds it against the current (animated) field.
			//! Called from RegenerateData, which runs single-threaded BETWEEN frames,
			//! so swapping in a fresh once_flag cannot race the in-render lazy build.
			void    InvalidateSamplingStructure();

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
			mutable std::unique_ptr<std::once_flag>  m_samplingOnce;	//!< reset by InvalidateSamplingStructure when an animated field changes the surface
			mutable std::vector<SampleTri>  m_sampleTris;
			mutable Scalar                  m_surfaceArea;
			mutable unsigned int            m_missedFeatureCells = 0;	//!< definite-miss cells found by EnsureSamplingStructure's detector

			// Heightfield mode (the analytic exact-surface twin of DisplacedGeometry).
			// Declared LAST among data members so the heightfield ctor's init list
			// (which leaves m_parts empty and sets only these) stays in declaration
			// order without disturbing the parts-ctor's order above.
			bool                m_isHeightfield = false;
			const IFunction2D*  m_pHeightfield  = 0;	//!< height field f(u,v) in [0,1]; addref'd
			Scalar              m_hfRadius      = 0;	//!< R: radius of the disk domain, centred at the origin in local XY (object units)
			Scalar              m_hfScale       = 0;	//!< world amplitude: surface z = m_hfScale*f(u,v)
			Scalar              m_hfLip         = 2;	//!< Lipschitz bound sqrt(1+maxslope^2) for safe sphere-tracing

		public:
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			//! Shared preamble for both signal estimators: validates the
			//! query and resolves the two quantities they both need -- the
			//! query radius in this geometry's own object-space units, and
			//! the sphere-trace band residual `Map(hit)` that every sample
			//! and every march is measured against.
			//!
			//! \return FALSE (outputs untouched) on a degenerate box, a
			//!         non-positive radius, or heightfield mode -- whose
			//!         globally-scaled field would make the occlusion march
			//!         step by the wrong length everywhere the local slope is
			//!         gentle.
			bool PrepareSignalQuery( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, Scalar& outR, Scalar& outD0 ) const;

			//! ISurfaceSignalProvider -- the `occlusion(radius)` builtin
			//! (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.2 for the channel,
			//! docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md for the estimator).
			//!
			//! DIRECTIONAL VISIBILITY over the outward hemisphere -- of 12
			//! cosine-weighted directions, spun about the normal by one of 32
			//! pre-built rotations picked per hit, the fraction that escape a
			//! distance `radiusFraction * m_diagonal` without entering the
			//! solid.  Bit for bit the question the mesh family's bake asks
			//! with rays -- including the per-hit spin, which the mesh bake
			//! has always applied per VERTEX (`GoldenRotation`) -- which is
			//! what makes "the same builtin on meshes and SDFs" a portability
			//! claim rather than a naming coincidence.
			//!
			//! A plane reads EXACTLY 1 and so does every CONVEX feature (all
			//! outward directions escape, so no normalisation is needed at
			//! all) -- the property the retired Evans normal-line estimator
			//! could not deliver: it returned `cos(gamma)` at a convex CSG
			//! edge of half-angle `gamma`, i.e. 0.707 at a plain 90-degree
			//! arris, and returned that SAME 0.707 at a concave 90-degree
			//! valley.  A wedge of empty opening `alpha <= pi` reads exactly
			//! `alpha/pi`; slots, folds and pockets go dark.
			//!
			//! Costs one sphere trace per outward direction (12), each
			//! bounded to 24 steps and averaging ~7.6 of them -- so ~91 field
			//! evaluations, the expensive one of the two signals, and the
			//! reason the .cpp explains at length why the cheap ball-volume
			//! measure cannot stand in for it.  The traces run in LOCKSTEP
			//! across the directions rather than one ray to completion, which
			//! is worth ~1.8x on its own (see the note at the loop).
			//!
			//! `radiusFraction` is a fraction of m_diagonal (this geometry's
			//! bounding-box diagonal), so the answer is invariant across
			//! instances at different world scales.
			//!
			//! `bRadiusIsConstant` is IGNORED here, and that is the whole
			//! M2/M3 asymmetry in one line: a live field can answer ANY
			//! radius per hit, so `occlusion(fbm(P)*0.1)` is a legal knob on
			//! the SDF family and a refusal on the baked mesh family
			//! (design doc §7.1's closing paragraph).
			bool ComputeOcclusion( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! ISurfaceSignalProvider -- the `convexity(radius)` builtin.
			//!
			//! `clamp(2A - 1, 0, 1)` for the BALL-VOLUME accessibility A --
			//! the fraction of the ball of radius `radiusFraction *
			//! m_diagonal` about the hit lying outside the solid, which a
			//! centrally-symmetric point set puts at EXACTLY 1/2 on a plane
			//! at any orientation.  32 point samples (16 mirrored pairs, the
			//! set rotated per hit like occlusion's), no marching, no rays,
			//! no tangent frame: roughly a third of occlusion's cost.
			//!
			//! Deliberately a DIFFERENT measure from occlusion's, and the
			//! .cpp argues both halves of why: volume cannot see a slot
			//! (which is why occlusion marches), and solid angle cannot see
			//! the convexity of a smooth body at all (from a point on a
			//! sphere of ANY radius the solid subtends exactly a hemisphere),
			//! which is why convexity does not.  On WEDGES -- edges, creases,
			//! corners -- the two agree exactly.  0 on a plane
			//! and in every cavity, **0.5 at a 90-degree arris**, **0.75 at a
			//! three-face corner**, approaching 1 at a knife edge; on a convex
			//! sphere of radius rho it is exactly `3R/(8*rho)`.  Those values
			//! mean the same thing on every object at every scene scale, which
			//! is what `curv` -- an unbounded differential quantity whose
			//! useful thresholds are per-object -- structurally cannot offer.
			//! `bRadiusIsConstant` is ignored, as for ComputeOcclusion.
			bool ComputeConvexity( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			//! ISurfaceSignalProvider -- the `thickness(radius)` builtin.
			//! Marches INWARD along -n to the far zero crossing and normalizes
			//! by the query radius: a slab of width w read at radius R gives
			//! min(w/R, 1), and anything at least as thick as the query radius
			//! reads a flat 1.  The SDF family is the one that has "distance
			//! to the other side" genuinely in hand.
			//! `bRadiusIsConstant` is ignored, as for ComputeOcclusion.
			bool ComputeThickness( const SurfaceSignalInfo& hit,
				const Scalar radiusFraction, const bool bRadiusIsConstant, Scalar& outValue ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest() const override { return true; }

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
				const unsigned int     detail ) const override;

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
			inline bool CanBeAreaLight() const override { return GetArea() > 0 && SuspectedMissedFeatureCells() == 0; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;

			//! IGeometry::SelfHitRootFloor -- the sphere-tracer has no root
			//! gate; what it has instead is `March`'s step-off: a ray whose
			//! ORIGIN reads |Map(o)| <= 2*m_eps is treated as spawned ON the
			//! surface and marched forward in 4*m_eps steps until it clears
			//! that band, which walks straight PAST the face a probe was
			//! standing off from.  So the smallest standoff that still
			//! re-hits the intended face is the band itself, 2*m_eps.
			//! (m_eps is set at construction and refreshed in the realize
			//! pass; before that it carries its constructed value, never zero.)
			//!
			//! That band is a PERPENDICULAR distance -- the field's value is --
			//! while the interface answers in RANGE along `localDir`, so it is
			//! divided by the incidence cosine exactly as
			//! `BoxGeometry::SelfHitRootFloor` divides its own plane-distance
			//! band, with the same 1/20 grazing clamp.  It used to be returned
			//! direction-independently on the argument that a perpendicular
			//! distance is an upper bound on the range, which is BACKWARDS: at
			//! `theta` off the normal a ray must travel `d / cos(theta)` of
			//! range to clear `d` of perpendicular distance, so the plain band
			//! UNDER-states, and without bound.  Measured against the bisected
			//! gate on a single uniform sphere R=3, BEFORE the divide:
			//! claim/gate 1.00 at normal incidence, 0.866 at 30 deg, 0.707 at
			//! 45, 0.500 at 60, 0.26 at 75.  After it the claim tracks the gate
			//! (1.00 through 75 deg, 1.003 at 84) until the 1/20 clamp binds --
			//! which it does past 87.1 deg, where cos falls below 0.05 and the
			//! divisor stops following it: claim/gate 0.72 at 88 deg and 0.39 at
			//! 89.  That residual under-statement at near-tangency is the price
			//! of a finite answer there, and it is the graceful direction (the
			//! probe misses and takes its entry-payload fallback).
			//!
			//! BUT the band is 2*m_eps in FIELD VALUE, not in DISTANCE, and
			//! `Map` is only 1-Lipschitz -- it may report LESS than the true
			//! distance (adversarial review of 4b141ad3, P1-2).  `partEval`
			//! evaluates each primitive in its own unit-scaled frame and
			//! multiplies by the part's conservative `minScale` Lipschitz
			//! factor, so along the part's LARGEST-scaled axis the field grows
			//! at only `minScale / maxScale` per unit of object-space travel.
			//! A probe standing `d` off the surface therefore reads
			//! |Map| ~= (minScale/maxScale) * d, and the step-off keeps firing
			//! until d >= 2*m_eps * maxScale/minScale -- measured EXACTLY 1/0.15
			//! times the plain 2*m_eps for a sphere part scaled (0.15, 1, 1), i.e.
			//! the old claim under-stated the real gate by 6.7x and a probe that
			//! trusted it was marched straight past the face it was aiming at.
			//!
			//! So divide by that ratio -- but by the ratio of the part(s) that
			//! actually OWN the queried point, not by the global minimum over
			//! every part (adversarial review of 7923bf2f, P2-2).  The global
			//! minimum charges the whole field the worst squash any lobe of it
			//! applies ANYWHERE: measured on
			//! `union(sphere R=3 uniform, sphere R=3 scaled (0.02,1,1) at x=8)`,
			//! the exit face of the UNIFORM lobe claimed 1.39e-2 against a
			//! bisected gate of 2.79e-4 -- a 50x over-statement (6x past the
			//! contract's 8x bound), and through the probe's 2x margin plus 10 %
			//! slack a same-face acceptance window of ~2.9e-2 WORLD UNITS, which
			//! is the decoy-face trade CsgSurfacePayloadTest Test 15 bounds,
			//! re-opened at a scale where a whole second lobe fits inside it.
			//! The ratio is <= 1 by construction (minScale <= maxScale), so it
			//! only ever widens the claim; a uniformly-scaled owner contributes
			//! exactly 1 and leaves the answer at 2*m_eps.  Note that the plain
			//! `minScale` (rather than the ratio) would be WRONG in the other
			//! direction for a part scaled up non-uniformly -- e.g. (2, 3, 3)
			//! has minScale 2 > 1 yet still shrinks the field by 2/3.
			//! Heightfield mode has no parts and keeps the bare band.
			//! Both ratios are taken from scale magnitudes FLOORED at 1e-9, the
			//! flooring `RecomputePartDerived` already applies before deriving
			//! the `minScale` `partEval` multiplies by -- so a zero-scale axis
			//! contributes its real 1e-9/maxScale shrink instead of being
			//! skipped as unusable, which is what reading `pt.scale` raw did.
			//!
			//! OWNER CRITERION (see the definition in SDFGeometry.cpp for the
			//! band's derivation): a part owns the point when its OWN signed
			//! distance there, `partEval`, is within a band of zero wide enough
			//! to cover twice the widest floor this function could return.
			//! Exclusion is what has to be sound, and it is: `partEval` is a
			//! CONSERVATIVE under-estimate of the true distance to that part's
			//! surface, so `|partEval| > band` PROVES the part's surface is more
			//! than `band` away and therefore cannot become the field's arg-min
			//! anywhere along a standoff this function would sanction.  With no
			//! qualifying part (a point on a blend seam, where the fold's value
			//! belongs to no single part) it falls back to the global minimum,
			//! i.e. exactly the previous, safely-conservative behaviour.
			//!
			//! CAPPED AT HALF THE FIELD'S BOUNDING-BOX DIAGONAL (adversarial
			//! review of 384e3752, P1-2).  Every widening above is a RATIO --
			//! the band over a Lipschitz shrink over an incidence cosine -- and
			//! a ratio is unbounded: a part authored `scale (1,1,0)` floors its
			//! shrink at 1e-9 and a field 2.83 units across claimed 5.66e4
			//! (6.9e-5 with a uniform scale).  A floor larger than the object
			//! sanctions a standoff outside the field, and
			//! `CSGObject::SelfHitRootFloor` reads `2 * floorChild` as an
			//! ownership-ray REACH, so such a number charges this field's floor
			//! on geometry tens of units away.  The cap is orders above every
			//! non-degenerate configuration (see the derivation in the .cpp);
			//! where it bites it UNDER-states, which is the graceful direction.
			//! The authoring surfaces refuse the input as well -- both
			//! `ParsePartLines` and the `scale` keyframe setter clamp a
			//! sub-1e-6 magnitude to 1e-6 with a warning, and reject a
			//! non-finite one (from scene text the strict keyframe parser
			//! already refuses `nan` / `inf` before the setter runs, so the
			//! setter's own check covers only a hand-built or interpolated
			//! keyframe) -- but the cap is what makes the floor sound for
			//! a field built through the constructor directly.
			//!
			//! P2-2 (accepted, documented): this is a RELATIVE window, unlike every
			//! other geometry's ulp-scale gate.  m_eps is `m_epsFrac` of the bbox
			//! diagonal, so the window is `2 * m_epsFrac / lipschitz` OF THE SHAPE
			//! ITSELF -- 0.01 % at the 5e-5 scene default with uniform parts, ~0.07 %
			//! at the 0.15 shrink BoxGeometryTest's row uses -- and it is
			//! SCALE-INVARIANT, so it does not tighten on a bigger SDF.  A SECOND
			//! lobe of the field lying within that window of the intended face
			//! cannot be told apart from it by the probe, and would be adopted as
			//! the same face.  There is
			//! no cheaper discriminator available at this layer (the sphere-tracer
			//! has no notion of primitive identity along a ray), and the outcome
			//! is a payload from a face a fraction of a percent away rather than a
			//! wrong-side antipodal one, so it is accepted rather than guarded.
			//! Not accounted for: `sminP`/`smaxP` blending can flatten the field's
			//! gradient further near a seam (two opposed unit gradients average
			//! toward zero), widening the band there beyond what this bound
			//! predicts.  A probe on such a seam misses and takes the probe's own
			//! graceful entry-payload fallback -- quality, never a wrong-face
			//! adoption -- which is why no blend term is charged here.
			//! (Defined in SDFGeometry.cpp -- it needs `partEval`, the
			//! translation-unit-local per-part field evaluator the owner
			//! criterion is built on.)
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override;

			//! Number of authored SDF primitives folded into this geometry's
			//! field.  Exact and blend-independent (unlike GetArea(), which a
			//! small smin-blended duplicate primitive can shift by less than
			//! tessellation noise) -- the natural diagnostic for the O(parts)
			//! per-march-step cost noted above, and for regression-guarding
			//! "did this emit exactly the primitives it should have".
			//! NB: the heightfield constructor leaves m_parts empty by design
			//! (its field comes from the sampled height grid, not from
			//! authored primitives), so NumParts() == 0 there even though the
			//! geometry itself is non-empty -- don't read 0 as "no surface".
			inline std::size_t NumParts() const { return m_parts.size(); }

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

			// Keyframable interface.  Animate the field itself -- the timeline
			// targets the GEOMETRY (`element_type geometry`), not the wrapping
			// standard_object's rigid transform.  Recognized `param` names:
			//   part<i>.position   vec3   part i origin (object space)
			//   part<i>.rotation   vec3   Euler degrees (Rz*Ry*Rx)
			//   part<i>.scale      vec3   per-axis scale
			//   part<i>.size       vec3   primitive size params (a,b,c -- see SDFPrim)
			//   part<i>.blend      scalar smooth-min/boolean radius k
			//   part<i>.round      scalar extra rounding radius
			//   heightfield_scale  scalar heightfield-mode displacement amplitude
			// `<i>` is the 0-based part index in authoring order.  Per-frame the
			// animator calls SetIntermediateValue once per animated param, then
			// RegenerateData once (rebuilds bounds / Lipschitz / sampling cache).
			// op and primitive TYPE are deliberately NOT animatable -- changing
			// them would break the "first part is union/smin" + order-fold bbox
			// invariants mid-animation.
			//
			// EMITTER CAVEAT: per frame the area-light sampling CDF is rebuilt
			// against the current surface (RegenerateData -> InvalidateSampling-
			// Structure), so NEE position/pdf track the animation.  But an SDF
			// emitter's luminary-set MEMBERSHIP / CanBeAreaLight() verdict is
			// fixed at the t=0 surface (LuminaryManager enumerates once, before
			// the frame loop -- like the photon maps).  An SDF that is zero-area
			// at t=0 and GROWS into an emitter is never NEE-sampled (full-weight
			// BSDF-hit emission only: unbiased, noisier); one that SHRINKS to
			// zero stays registered but GetArea()->0 is guarded at the consumers
			// (no divide-by-zero).  Keep emissive SDFs non-degenerate at t=0, or
			// animate only non-emissive SDFs, for clean NEE.
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData() override;
		};
	}
}

#endif
