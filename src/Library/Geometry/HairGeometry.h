//////////////////////////////////////////////////////////////////////
//
//  HairGeometry.h - A runtime curve primitive: one groom (many hair /
//    fur strands) held in shared flat arrays with an embedded segment
//    BVH.  Candidate E of docs/HAIR_FUR_DESIGN.md section 5.2.
//
//  TWO CONSTRUCTION MODES.
//
//    * EXPLICIT-STRAND (slice C1).  Strands are supplied DIRECTLY as
//      control-point data (see `StrandDesc`); the groom is built in the
//      constructor and `Realize()` is a no-op.  This is what the tests
//      and any direct C++ consumer use.
//
//    * DEFERRED GROOM (slice D).  The constructor takes a
//      `HairGroomRecipe` -- a base geometry, a few painters, numeric
//      parameters and a seed -- addrefs it, and builds NOTHING.
//      `Realize()` then runs `GenerateHairStrands` (HairGenerator.h)
//      and feeds the result through the identical build path.  This is
//      what the `hair_geometry` scene chunk produces.  Deferral is not
//      an optimisation here, it is a requirement: generation needs the
//      RESOLVED base geometry (which may itself be deferred) and can be
//      expensive, and the const hot path must never build anything.
//      Same contract DisplacedGeometry uses (IGeometry.h's Realize()).
//
//      A deferred groom that has NOT been realized is a valid, EMPTY
//      groom: it intersects nothing, reports a degenerate bounding box
//      at the object-space origin, and never crashes.  Direct
//      (non-pipeline) consumers -- unit tests, tools -- MUST call
//      Realize() before use, exactly as DisplacedGeometry requires.
//      Re-Realize() is idempotent (the second call returns immediately);
//      a groom regenerated from scratch on a later frame reproduces
//      byte-identically from the seed.
//
//  ATTRIBUTION.  The intersection strategy -- transform the segment
//  into a ray-centric frame, recursively split until the piece is
//  nearly linear, then run a 2D point-to-line test against the
//  linearly-interpolated half width -- follows the approach published
//  in PBRT's `Curve` shape (Pharr, Jakob & Humphreys, "Physically
//  Based Rendering", pbrt-v4 `src/pbrt/shapes.cpp`, Apache-2.0).  This
//  is an INDEPENDENT implementation written against RISE's conventions
//  (Catmull-Rom rather than Bezier control points, an ORTHONORMAL ray
//  frame rather than pbrt's permute+shear, RISE `Scalar`/`Vector3`
//  math, RISE's `RayIntersectionGeometric` outputs); no pbrt code is
//  reproduced.  Deviations from pbrt are called out inline where they
//  matter.  One property IS shared with pbrt rather than deviated
//  from: the reported hit is the CLOSEST APPROACH within a flattened
//  piece, not the ray's true entry into the fibre -- see the
//  `IntersectSegment` doc comment below for the two consequences this
//  has for callers (an off-surface ptIntersection, and a shadow ray's
//  potential to under-occlude).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HAIR_GEOMETRY_
#define HAIR_GEOMETRY_

#include "Geometry.h"
#include "../Acceleration/BVH.h"
#include "../Acceleration/AccelerationConfig.h"
#include "../Interfaces/ProceduralDescriptors.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//////////////////////////////////////////////////////////////
		//
		//  HairSegmentRef -- the BVH element type.
		//
		//  One record names ONE SUB-SEGMENT of one Catmull-Rom span of
		//  one strand.  Exactly 8 bytes, naturally aligned, no bit
		//  packing (the field widths ARE the packing) so there is no
		//  encode/decode arithmetic on the traversal hot path.
		//
		//  LIMITS THIS IMPOSES (all enforced with a diagnostic at
		//  construction -- see HairGeometry's ctor):
		//    * strand  : uint32_t  -> at most 4,294,967,295 strands per
		//                groom.  Not separately checked; a groom that
		//                large exhausts memory long before the index.
		//    * span    : uint16_t  -> at most 65,535 spans per strand,
		//                i.e. at most 65,536 CONTROL POINTS per strand.
		//                CHECKED: a longer strand is rejected.
		//    * subIndex: uint8_t   -> at most 256 sub-segments per span
		//                (subIndex 0..255), which bounds the build-time
		//                split depth at 8.  `kMaxBuildSplitDepth` is 3
		//                (8 sub-segments), so the field is never the
		//                binding constraint -- moot at that cap.
		//    * subDepth: uint8_t   -> the split depth itself, 0..3.
		//
		//////////////////////////////////////////////////////////////
		struct HairSegmentRef
		{
			uint32_t	strand;		///< index of the owning strand
			uint16_t	span;		///< index of the Catmull-Rom span within the strand (0 .. numCP-2)
			uint8_t		subIndex;	///< index of this sub-segment within the span, 0 .. (1<<subDepth)-1
			uint8_t		subDepth;	///< build-time split depth of the span, 0 .. kMaxBuildSplitDepth
		};

		//////////////////////////////////////////////////////////////
		//
		//  HairGeometry
		//
		//  ---- STORAGE: a deliberate, argued exception to Scalar=double
		//
		//  Control points and widths are stored as FLOAT, not `Scalar`
		//  (= double, Math3D.h).  Rationale, per HAIR_FUR_DESIGN.md
		//  section 5.2E: strand geometry is sub-millimetre, so float's
		//  ~7 significant digits sit roughly 10^4 BELOW the fibre width
		//  -- the quantisation is invisible against the primitive's own
		//  size -- while the 2x memory saving is decisive at the 1M-
		//  strand / 16M-control-point target (192 MB vs 384 MB for the
		//  positions alone).
		//
		//  The in-tree precedent is `BVH<>` itself (BVH.h:1-22, "Float
		//  vs double"): float-stored, conservatively padded AABBs drive
		//  traversal, and the LEAF test then runs in double against the
		//  original ray.  This class follows the identical discipline:
		//  floats are loaded and IMMEDIATELY widened to `Scalar`, and
		//  every ray/curve computation -- coefficients, splitting,
		//  point-to-line distance, t, normals -- runs in double.  No
		//  intersection arithmetic happens in float.
		//
		//  ---- BASIS: Catmull-Rom, with reflected phantom endpoints
		//
		//  A strand's control points are interpolated by a uniform
		//  Catmull-Rom spline, matching the convention `sweep_geometry`
		//  and `path_instances_geometry` already use.  Span j runs
		//  between CP j and CP j+1 and needs CP j-1 and CP j+2; at the
		//  two ends those do not exist.  The convention chosen here is
		//  LINEAR REFLECTION (a.k.a. the "natural"/extrapolated phantom
		//  point):
		//
		//      P[-1] := 2*P[0]   - P[1]
		//      P[n]  := 2*P[n-1] - P[n-2]
		//
		//  chosen over the clamped alternative (P[-1] := P[0]) because
		//  it makes a 2-CONTROL-POINT STRAND EXACTLY A STRAIGHT LINE:
		//  substituting the reflections into the Catmull-Rom basis
		//  cancels the u^2 and u^3 coefficients identically, leaving
		//  p(u) = P0 + (P1-P0)u.  The clamped convention does not have
		//  that property (it leaves a non-zero cubic term, so a 2-CP
		//  "straight" strand would bulge).  Strands with fewer than 2
		//  control points are rejected at construction.
		//
		//  ---- SEGMENTS AND THE EMBEDDED BVH
		//
		//  Each span is subdivided AT BUILD TIME into 2^d equal-
		//  parameter sub-segments, purely so the BVH gets tight AABBs
		//  (a curved span's own AABB is the classic thin-diagonal BVH
		//  hazard).  d is chosen per span from the span's own curvature
		//  -- see `ChooseBuildSplitDepth` in the .cpp for the bound and
		//  its derivation -- and is capped at `kMaxBuildSplitDepth`.
		//  Build-time splitting is an ACCELERATION concern only; the
		//  intersector splits further at run time as needed, so the cap
		//  costs traversal efficiency on a pathological span, never
		//  correctness.
		//
		//  The BVH is `BVH<HairSegmentRef>` with maxLeafSize 4,
		//  mirroring TriangleMeshGeometryIndexed's embedded
		//  `BVH<const PointerTriangle*>` (TriangleMeshGeometryIndexed
		//  .cpp:527-537).  This class implements the
		//  `TreeElementProcessor<HairSegmentRef>` contract for it.
		//
		//  ---- FACE FLAGS AND EXIT INFO (conventions taken)
		//
		//  bHitFrontFaces / bHitBackFaces are IGNORED.  A hair ribbon is
		//  constructed to face the ray, so it has no back side to cull:
		//  every hit is a front hit by construction.  This follows the
		//  DOUBLE-SIDED TRIANGLE MESH precedent, which overrides both
		//  flags to true at the BVH call (TriangleMeshGeometryIndexed
		//  .cpp:168, `bDoubleSided?1:bHitFrontFaces`).  The alternative
		//  -- honouring the flags by declaring the ribbon always-front
		//  and returning nothing when bHitFrontFaces is false -- was
		//  rejected because it makes fur INVISIBLE to the exit-side
		//  probes a dielectric refraction walk issues, so a groom
		//  inside glass would silently disappear.
		//  `bGeomNormalOrientedToRay` is set TRUE on every hit, which is
		//  the honest statement of what happened: the reported geometric
		//  normal was oriented to oppose the ray rather than read off a
		//  fixed surface orientation (see that field's contract in
		//  RayIntersectionGeometric.h).
		//
		//  bComputeExitInfo: a ribbon is a zero-thickness surface -- it
		//  encloses no volume, so there is no second crossing to report.
		//  EXIT IS SET EQUAL TO ENTRY (range2 = range, ptExit =
		//  ptIntersection, vNormal2 = vNormal, vGeomNormal2 =
		//  vGeomNormal).  This matches the flat-disk precedent's shape
		//  (CircularDiskGeometry reports an exit at the same crossing);
		//  the disk flips the exit normal because it is a two-sided
		//  plane whose two sides are genuinely opposed, whereas the hair
		//  ribbon's normal is ray-derived, so flipping it would fabricate
		//  a surface orientation that does not exist.
		//
		//  ---- SPACE
		//
		//  Object space, exactly like every sibling geometry: the ray
		//  arriving in `ri.ray` has already been pulled into object
		//  space by Object::IntersectRay, and normals / positions
		//  written here are transformed back out by that same caller
		//  (Object.cpp:630-745).  Nothing in this class is aware of the
		//  object's world transform.
		//
		//////////////////////////////////////////////////////////////
		class HairGeometry :
			public virtual Geometry,
			public virtual TreeElementProcessor<HairSegmentRef>
		{
		public:
			//! Cap on the BUILD-time per-span split depth.  3 => at most
			//! 8 sub-segments per span.  Chosen so the segment array
			//! stays within the design's memory budget on a groom whose
			//! strands are gently curved (the overwhelmingly common
			//! case, where the curvature bound picks d = 0 and a span
			//! costs a single 8-byte record); a pathologically kinked
			//! span merely gets a loose AABB and pays for it in
			//! traversal, never in correctness, because the intersector
			//! splits further at run time.
			static const unsigned int kMaxBuildSplitDepth = 3;

			//! Hard cap on control points per strand, imposed by
			//! HairSegmentRef::span being uint16_t.
			static const unsigned int kMaxControlPointsPerStrand = 65536;

			//! One strand of the groom, as supplied by the caller.
			//! Slice C1 is constructed from these directly; the groom
			//! generator of a later slice produces them.
			struct StrandDesc
			{
				std::vector<Point3>	controlPoints;	///< >= 2 points, root first, tip last
				Scalar				rootWidth;		///< > 0, full width (not radius) at the root
				Scalar				tipWidth;		///< > 0, full width at the tip
				Point2				rootUV;			///< the strand's UV on the base surface; reported verbatim as ri.ptCoord1

				StrandDesc() : rootWidth( 0 ), tipWidth( 0 ), rootUV( 0, 0 ) {}
			};

			//! Builds the groom.  Strands that fail validation (fewer
			//! than 2 control points, more than
			//! kMaxControlPointsPerStrand, non-positive or non-finite
			//! width, non-finite control point) are REJECTED
			//! INDIVIDUALLY with a warning naming the strand and the
			//! reason, and are excluded from the groom -- the remaining
			//! strands still build.  An all-rejected or empty input
			//! yields a valid, empty groom that intersects nothing.
			HairGeometry( const std::vector<StrandDesc>& strands );

			//! DEFERRED-GROOM construction (slice D).  Stores the recipe
			//! -- taking its OWN addref on the base geometry and every
			//! bound painter, so the caller keeps and releases its own
			//! references -- and builds nothing.  `Realize()` runs the
			//! generation.  `chunkName` is carried purely so every
			//! diagnostic points at the author's own scene chunk.
			//!
			//! The recipe is NOT validated here; call
			//! `ValidateHairGroomRecipe` (HairGenerator.h) at PARSE time
			//! so an author gets the error where they wrote the mistake.
			//! `IsValid()` re-checks the cheap half of that.
			HairGeometry( const HairGroomRecipe& recipe, const char* chunkName );

			// ---- IGeometry -------------------------------------------------

			//! Deferred-groom entry point.  A no-op in explicit-strand
			//! mode and on any second call.  Runs the generator, then
			//! the identical build path the explicit constructor uses.
			//! A failed generation leaves the groom empty (it intersects
			//! nothing) rather than throwing or leaving it half-built.
			void Realize() const override;

			//! TRUE iff this groom will ever be able to produce strands:
			//! always true in explicit-strand mode, and in deferred mode
			//! the cheap recipe check (base geometry present and
			//! tessellatable) that can be made without generating
			//! anything.  Mirrors DisplacedGeometry::IsValid().
			bool IsValid() const;

			//! Diagnostic / test accessor: has Realize() run?  Always
			//! true in explicit-strand mode.
			bool IsRealized() const { return bRealized.load( std::memory_order_acquire ); }

			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;

			//! TRUE, following the triangle-mesh precedent: a groom's
			//! bounding box is a big win as a pre-hit filter because the
			//! overwhelming majority of scene rays miss the groom
			//! entirely.
			bool DoPreHitTest() const override { return true; }

			//! FALSE.  Emissive fur is out of scope (HAIR_FUR_DESIGN.md
			//! section 5.2E); UniformRandomPoint / GetArea below cannot
			//! honour the exact-surface-sampling contract this flag
			//! gates, so the light sampler and the point-set SSS
			//! shaderops must refuse this geometry rather than sample a
			//! surface it does not offer.
			bool CanBeAreaLight() const override { return false; }

			//! FALSE.  Deliberate, per HAIR_FUR_DESIGN.md section 5.2E:
			//! a groom must NOT be eligible as a `displaced_geometry`
			//! base or a glTF-export victim.  There is no honest
			//! triangle mesh for a 1M-strand groom at any tessellation
			//! detail the rest of the pipeline could carry (candidate A
			//! of section 5.2 is rejected on arithmetic: ~9 GB of
			//! triangles), and a silently-crude one would be worse than
			//! a refusal.  Returning false here makes composites refuse
			//! at SCENE-PARSE time with a clear message rather than
			//! failing later at realize time.
			bool CanTessellate() const override { return false; }

			//! Deterministic stub.  `CanBeAreaLight()` is false, which
			//! is what keeps samplers off this path; this follows
			//! SDFGeometry's degenerate-field branch (SDFGeometry.cpp
			//! :1733-1743) in returning something stable rather than
			//! garbage should an unguarded caller arrive.
			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;

			//! 0 -- see CanBeAreaLight().  A swept-curve surface area is
			//! computable, but publishing one would imply the
			//! pdfPosition = 1/area sampling contract that
			//! UniformRandomPoint does not honour.
			Scalar GetArea() const override { return 0; }

			// ---- IKeyframable ----------------------------------------------

			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& val ) override {}
			void RegenerateData() override {}

			// ---- TreeElementProcessor<HairSegmentRef> ----------------------

			typedef HairSegmentRef	MYOBJ;

			void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;
			BoundingBox GetElementBoundingBox( const MYOBJ elem ) const override;
			bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const override;
			char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const override;

			//! Index-based, mirroring the triangle mesh's element
			//! (de)serializers (TriangleMeshGeometryIndexedSpecializations
			//! .h:527-547) -- the mesh writes a primitive INDEX and
			//! reconstructs the pointer on read.  A HairSegmentRef is
			//! already a self-describing 8-byte value, so this is a
			//! straight field write/read.  NOTE: like the mesh's, these
			//! exist to satisfy the TreeElementProcessor contract and
			//! the BVH's optional serializer; HairGeometry itself has no
			//! on-disk format in this slice, so nothing calls them yet.
			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const override;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const override;

			// ---- Introspection (tests / diagnostics) -----------------------

			unsigned int numStrands() const			{ return (unsigned int)( strandCPBegin.size() ? strandCPBegin.size() - 1 : 0 ); }
			unsigned int numRejectedStrands() const	{ return nRejectedStrands; }
			unsigned int numControlPoints() const	{ return (unsigned int)( cps.size() / 3 ); }
			unsigned int numSegments() const		{ return nSegments; }

			//! Number of control points of strand `s`.
			unsigned int numControlPointsOfStrand( unsigned int s ) const { return strandCPBegin[s+1] - strandCPBegin[s]; }
			//! Control point `i` of strand `s`, widened to Scalar.
			Point3 ControlPoint( unsigned int s, unsigned int i ) const;
			//! The strand's stored root UV (what a hit reports as ri.ptCoord1).
			Point2 StrandRootUV( unsigned int s ) const { return Point2( strandRootU[s], strandRootV[s] ); }
			//! Full width of strand `s` at arc-length fraction `sArc` in [0,1].
			Scalar StrandWidthAt( unsigned int s, const Scalar sArc ) const;
			//! Total (polyline-sampled) arc length of strand `s`.
			Scalar StrandArcLength( unsigned int s ) const;
			//! Evaluates strand `s` at GLOBAL parameter `u` in [0, numSpans],
			//! i.e. u = span index + local parameter.  Optionally returns
			//! dP/du.  Public so tests can build an independent reference.
			Point3 EvaluateStrand( unsigned int s, const Scalar u, Vector3* outDeriv ) const;
			//! Arc-length fraction in [0,1] corresponding to global parameter `u`.
			Scalar ArcFractionAt( unsigned int s, const Scalar u ) const;

			//! Rough resident size of the groom's own arrays plus the
			//! embedded BVH.  Diagnostic only.
			size_t ApproximateMemoryBytes() const;

		protected:
			virtual ~HairGeometry();

		private:
			//! ---- MUTABILITY, and why it is legitimate here
			//!
			//! Every storage member below is `mutable` because
			//! `Realize()` is const (IGeometry's contract) and must be
			//! able to materialise the groom.  This is the same
			//! lazy-build-cache pattern DisplacedGeometry uses for its
			//! `mutable m_pMesh` and ObjectManager for its `mutable
			//! pBVH`: the built arrays are a pure function of the
			//! recipe, realization is single-threaded (asserted in
			//! debug) and serialized by `realizeMutex`, and the
			//! observable surface after realization is exactly what the
			//! recipe always described.  Nothing on the const hot path
			//! ever writes them.
			//!
			//! In EXPLICIT-STRAND mode they are written once, from the
			//! constructor, and `bRealized` starts true.

			//! Control points, strand-major, 3 floats each.  See the
			//! float-storage rationale in the class comment.
			mutable std::vector<float>		cps;
			//! First control point index (in CP units) of each strand;
			//! size numStrands+1, so strand s owns
			//! [strandCPBegin[s], strandCPBegin[s+1]).
			mutable std::vector<uint32_t>	strandCPBegin;
			//! Cumulative polyline arc length from the strand root to
			//! each control point; one float per control point, same
			//! indexing as `cps`.  Sampled `kArcSamplesPerSpan` times
			//! per span at build.  Within a span, `ArcFractionAt`
			//! interpolates LINEARLY in the span parameter -- exact at
			//! span boundaries, and accurate inside to the degree the
			//! span is close to constant-speed.  This is the only
			//! approximation in the reported `s`.
			mutable std::vector<float>		cpArcCum;
			mutable std::vector<float>		strandRootWidth;
			mutable std::vector<float>		strandTipWidth;
			mutable std::vector<float>		strandRootU;
			mutable std::vector<float>		strandRootV;

			mutable BVH<HairSegmentRef>*	pSegBVH;
			mutable BoundingBox				bbox;
			mutable unsigned int			nSegments;
			mutable unsigned int			nRejectedStrands;

			//! ---- deferred-groom state (null / true in explicit mode)

			//! The recipe, heap-owned, non-null ONLY in deferred-groom
			//! mode.  Owns an addref on the base geometry and on every
			//! bound painter; released in the destructor.
			HairGroomRecipe*		pRecipe;
			//! Names the author's scene chunk in generation diagnostics.
			std::string				groomName;
			//! Set true after a successful (or failed-but-attempted)
			//! Realize(), and at construction in explicit-strand mode.
			mutable std::atomic<bool>	bRealized;
			//! Serializes the actual generation so a GUI viewport
			//! render's AttachScene cannot race a UI-thread
			//! PrepareForRendering into a double generation of the same
			//! instance -- the identical guard DisplacedGeometry keeps.
			//! Uncontended in normal use; the hot path never takes it.
			mutable std::mutex		realizeMutex;

			//! The SHARED build path: validate, pack, arc-length,
			//! sub-segment split, BVH.  Called from the explicit-strand
			//! constructor and from Realize().  `const` because it
			//! writes only the mutable storage above.
			void BuildFromStrands( const std::vector<StrandDesc>& strands ) const;

			//! Samples per span used to build `cpArcCum`.
			static const unsigned int kArcSamplesPerSpan = 16;

			// -- internals (documented at the definitions) --

			void EvalSpanCoefficients( unsigned int s, unsigned int span, Vector3 c[4] ) const;
			Scalar StrandMaxWidth( unsigned int s ) const;
			BoundingBox SegmentBoundingBox( const MYOBJ elem ) const;

			//! The single shared intersection kernel.  `outT` receives
			//! the ray parameter and `outU` the strand-global curve
			//! parameter of the CLOSEST QUALIFYING APPROACH within this
			//! sub-segment strictly inside (NEARZERO, tMax) -- NOT the
			//! ray's true geometric entry into the swept fibre.  Each
			//! recursion base case (TestFlatPiece, in the .cpp) reports
			//! the point on its piece nearest the ray axis, which can
			//! land as much as halfWidth / sin(theta) further along the
			//! ray than where the ray actually crosses the fibre's
			//! silhouette boundary (theta = angle between the ray and
			//! the fibre tangent; grazing incidence is worst-cased).
			//! This matches pbrt's Curve intersector, which has the
			//! identical property for the identical reason (closest
			//! approach within a flattened piece, not true entry).
			//!
			//! Two consequences follow from this: (1) the reported
			//! ptIntersection does not sit exactly on the fibre's
			//! silhouette surface, so a self-intersection epsilon tuned
			//! against an exact-surface primitive may need re-checking
			//! here; (2) IntersectionOnly (shadow rays) can
			//! UNDER-OCCLUDE -- a shadow ray whose dHowFar lands
			//! strictly between the true entry and this reported
			//! closest-approach point is told "no occluder" even though
			//! the ray geometrically enters the fibre before dHowFar.
			//! Returns false if there is no qualifying approach.
			bool IntersectSegment( const Ray& ray, const Scalar tMax, const MYOBJ elem,
			                       Scalar& outT, Scalar& outU ) const;

			HairGeometry( const HairGeometry& );
			HairGeometry& operator=( const HairGeometry& );
		};
	}
}

#endif
