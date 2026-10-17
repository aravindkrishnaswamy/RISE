//////////////////////////////////////////////////////////////////////
//
//  DisplacedGeometry.h - Composite geometry that wraps any IGeometry,
//  tessellates it via IGeometry::TessellateToMesh, and applies a
//  displacement map along the vertex normals.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DISPLACED_GEOMETRY_
#define DISPLACED_GEOMETRY_

#include "Geometry.h"
#include "ClippedPlaneGeometry.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ITriangleMeshGeometry.h"
#include "../Utilities/Observable.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace RISE
{
	namespace Implementation
	{
		// Wraps any IGeometry (including another DisplacedGeometry), tessellates it via the
		// base's TessellateToMesh, and displaces every vertex along its normal by
		// `displacement(u,v) * disp_scale`.  The resulting mesh is built into an internal
		// TriangleMeshGeometryIndexed and used for all intersection / sampling queries.
		//
		// MESH BASES ARE DISPLACED AT `detail`.  A base that is already a mesh (a
		// TriangleMeshGeometryIndexed -- lathe / sweep / skin / the indexed-mesh imports;
		// NOT the legacy non-indexed TriangleMeshGeometry, whose tessellation is
		// per-corner) ignores `detail` in its own TessellateToMesh, which would leave it
		// displaced only at its own vertices.  TessellateAndRefine() therefore brings it
		// to spacing (base bbox diagonal / detail).
		//
		// REFINEMENT NEVER CHANGES THE AUTHORED SURFACE.  It only adds SAMPLES for the
		// displacement to act on: every new vertex lies on the authored piecewise-linear
		// surface (a chord between authored vertices, a point inside an authored
		// triangle), takes a normal interpolated (lerp + normalize) from the authored
		// vertex normals and a UV interpolated from the authored UVs (respecting the
		// chart's periodic / polar bits); creases exist only where the author made them
		// (crease / cap / duplicated-point vertices stay separate vertices) and the
		// authored correspondence (loft pairing, caps) is kept.  The SILHOUETTE IS THE
		// AUTHORED POLYGON -- a coarse lathe stays a polygon; what the author wants
		// rounder, the author makes denser.  At disp_scale -> 0 the displaced geometry
		// therefore renders as the undisplaced base up to one stated tolerance: a refined
		// vertex inside a NON-PLANAR authored quad is taken on the triangle (of the fold
		// q0-q2) that covers it, so refined sub-triangles can cross that fold and sit up
		// to ~0.1% of the quad's diagonal off the authored pair of triangles.
		//   (a) a lathe / sweep / skin RECORDS ITS AUTHORED GRID (TriangleMeshGeometryIndexed::
		//       MeshGrid: quads between line FAMILIES, fans around poles / pinches) and
		//       TessellateRefined() subdivides each family by its own length, so shared edges
		//       agree, quads stay regular quads (near-square cells, clean ribs, about the
		//       triangle count of a hand-authored grid at that spacing) and position / normal
		//       / UV are interpolated barycentrically inside the AUTHORED triangles.  Any
		//       authored triangle no cell replaced -- an END CAP, or a skin triangle the
		//       builder's zero-area test half-ate -- that touches an edge a cell subdivided
		//       subdivides that edge the same way (found by position, so a cap's duplicate
		//       rim vertices match the wall's) and is closed by a fan to its centroid (a
		//       collinear one by a fan from a corner): the refined mesh has no T-junction,
		//       and a cap's rim matches the wall's rim vertex for vertex.  A cap's interior
		//       carries no authored vertices, so it is flat except for its centroid.
		//   (b) any other indexed mesh is subdivided UNIFORMLY (whole 1->4 levels, level
		//       count from the base's MEDIAN edge length, midpoints shared per unordered
		//       combined-index pair, linear positions, lerped normals and UVs) -- except
		//       across a chart axis the base DECLARES periodic (a ring-stitch edge is taken
		//       the short way round) and at a POLAR vertex (a placeholder u / v: an edge to a
		//       normal vertex takes the normal vertex's value).
		// Capped at 1.5M triangles (6 levels for (b)); the cap bounds the grid cells, the
		// triangles added around caps are a small extra.  A pole / pinch is DISPLACED by
		// the average of its neighbours' displacements, not by the field at its
		// placeholder UV.  Analytic and SDF bases already honour `detail` and are
		// untouched; so is a DISPLACED base (already baked at its own detail -- the inner
		// layer governs); and any bake with disp_scale == 0.  A base that shades with
		// FACE normals (face_normals TRUE) is flattened per corner by its tessellation --
		// every edge of it is already a crack -- so it is not refined and displaces along
		// each face normal as it always did.
		//
		// WATERTIGHT AT CREASES (see BaseWeldPolicy for the full rule).  Crease / cap / pinch /
		// UV-seam vertices are separate vertices in the base (that is what keeps a crease) --
		// coincident in position, different in normal and UV.  Displacing each along its own
		// normal by its own height tore them apart (cap rims off the walls, a pinch's two poles
		// by 2*disp_scale, crease rows).  WeldCoincidentDisplacement moves the groups the BASE
		// MESH says are one surface to ONE shared position, the mean of the members'
		// displacement vectors, each keeping its own shading normal: a lathe / sweep / skin
		// welds every coincident group (its recorded creases, cap rims, duplicated profile
		// points, pinch poles, skin crease stations); an import / hand-built mesh welds members
		// whose base normals are within a 120-degree crease cone, so coincident OPPOSITE-facing
		// sheets (leaf, cloth) stay independent and keep their full relief; path_instances_
		// geometry welds WITHIN each instance by its template's own rule and never across
		// instances (touching instances are separate bodies).  A displaced base is
		// welded again by its base's policy.  Consequences: a pinch ring displaces to ~zero
		// relief (its poles are averaged against each other); knife-edge opposite sheets may
		// separate.  Smooth-normal TriangleMeshGeometryIndexed bases only -- a face-normal
		// base, an analytic base used directly and an SDF base are not touched (a box's per-face
		// corner duplicates still crack: a pre-existing limitation of that tessellation, absent
		// when the box is a path_instances template, whose instances weld by the crease cone) -- and
		// gated on the BASE's normal source, not this chunk's own face_normals flag.
		//
		// SHADING.  For every smooth-normal mesh base that is displaced (refined or
		// not) the base's own shading-normal field is carried through the
		// displacement: each vertex's base shading normal is rotated by the rotation
		// the displacement made to its geometric normal (ApplyTransportedShading), so
		// the look is continuous across the refinement threshold and the relief
		// perturbs the base's smooth field instead of replacing it.  (Geometric normals on
		// every bake -- an analytic base included -- are ANGLE-weighted per vertex, see
		// RecomputeVertexNormalsFromTopology: ~0.2 deg mean difference from the old
		// unweighted average on a sphere, up to tens of degrees only at a collapsed fan.)
		//
		// REFIT.  An animated displacement refits the baked mesh in place
		// (RefreshMeshVertices) -- unless the refinement came out different (it is size
		// driven, so a moving base can change topology at an unchanged vertex count),
		// detected by a topology signature, in which case the mesh is rebuilt from
		// scratch.  Cost: every painter notification re-tessellates and re-refines the
		// base (O(triangles), allocation-heavy): measured ~6-8 ms for the 10-11k-triangle
		// stress-scene vase / leg, ~3 ms for the sail, against ~0.3 ms for an unrefined
		// small mesh -- a keyframed displacement pays it per frame.
		//
		// TWO HEIGHT ROUTES, MUTUALLY EXCLUSIVE (2026-09-06).  The offset scalar comes
		// from EXACTLY ONE of:
		//   (a) `displacement`, an IFunction2D evaluated as `f(u, v)` -- the original
		//       route, byte-for-byte unchanged; or
		//   (b) `height`, an IScalarPainter evaluated as a FIELD at the vertex through a
		//       synthetic RayIntersectionGeometric (GeometryUtilities::
		//       ApplyScalarHeightToObject).  This is what lets ONE `scalar_painter`
		//       drive coarse `displaced_geometry` shape AND fine `relief_modifier`
		//       normal perturbation -- docs/RELIEF_MODIFIER_DESIGN.md section 5.3.
		// `disp_scale` multiplies either one identically.  Passing both is refused at
		// the construction boundary (Job::AddDisplacedGeometryWithHeight).
		//
		// OBJECT SPACE, NOT WORLD (route (b)).  The bake happens before the geometry is
		// bound to an object, so there is no object-to-world transform to apply: the
		// synthetic hit carries the vertex's OBJECT-space position in BOTH `P`
		// (ptIntersection) and `Po` (ptObjIntersec), which therefore COINCIDE.  A field
		// authored against `P` does NOT follow the object's placement.  Author against
		// `Po`.
		//
		// DEFERRED REALIZATION (2026-06-13): the tessellate + displace + mesh-build work
		// (BuildMesh) is NOT done in the constructor.  It is deferred to Realize(), which the
		// render pipeline calls once, single-threaded, from RayCaster::AttachScene BEFORE the
		// parallel rasterize.  This means a displaced geometry that is never bound to a
		// rendered object is never baked (e.g. the GuillocheWatch dial has 6 displaced dials
		// but only 1 is active — only that 1 bakes).  Direct (non-pipeline) consumers — unit
		// tests, tools — MUST call Realize() after construction before using the geometry; it
		// is single-threaded-safe to do so.
		//
		// If the base geometry's TessellateToMesh returns false (e.g. InfinitePlaneGeometry),
		// Realize() logs an error and leaves the internal mesh null; all IGeometry query
		// methods then degrade to "miss" / empty behavior (the release-mode guard-and-fail).
		// IsValid() reports RECIPE validity (base non-null), known at construction.
		class DisplacedGeometry : public Geometry
		{
		protected:
			IGeometry*                       m_pBase;
			const IFunction2D*               m_pDisplacement;
			const IScalarPainter*            m_pHeight;		//!< route (b); mutually exclusive with m_pDisplacement (see the class comment)
			Scalar                           m_dispScale;
			unsigned int                     m_detail;
			bool                             m_bDoubleSided;
			bool                             m_bUseFaceNormals;
			bool                             m_bSeamFold;	//!< tent-fold the UV before evaluating displacement (closed wrap-seam surfaces); FALSE = raw UV (open Cartesian fields)
			const ClippedPlaneGeometry*      m_pPlaneBase;	//!< DL-345: m_pBase when it is a clipped plane (owned via m_pBase's reference), else null

			// The baked mesh and the realized flag are the deferred-realization
			// state.  They are `mutable` because Realize() is a const method
			// (it materializes a build-time cache that is a pure function of
			// this geometry's recipe — base + displacement + scale + detail —
			// and does not change the observable surface; same legitimate
			// `mutable` lazy-cache pattern as ObjectManager's mutable pBVH).
			// m_pMesh is the SOLE OWNER of the mesh (freed in DestroyMesh + the
			// dtor).  Realization is single-threaded (the freeze guard asserts
			// this in debug), so the unlocked mutable write is safe.
			mutable ITriangleMeshGeometryIndexed*    m_pMesh;
			mutable std::atomic<bool>                m_bRealized;
			// Serializes the actual bake so a GUI viewport render's AttachScene
			// cannot race a UI-thread PrepareForRendering/picking into a double
			// BuildMesh() of the same instance.  Uncontended in normal use; the
			// hot path (IntersectRay) never takes it (reads the post-bake m_pMesh).
			mutable std::mutex                       m_realizeMutex;

			// Subscription to the height source's Observable -- the
			// IFunction2D `displacement` or the IScalarPainter `height`,
			// whichever this geometry was given (they are mutually exclusive,
			// so there is only ever one).  When it notifies (e.g. a keyframed
			// `time` parameter changed), the callback rebuilds m_pMesh.
			//
			// Destruction ordering note: the destructor BODY runs before any
			// member destructors, so the subscription's own destructor would
			// otherwise fire AFTER m_pDisplacement->release() has potentially
			// deleted the painter.  ~DisplacedGeometry resets this member
			// explicitly at the top of the dtor body, before any release, so
			// Detach executes while the subject is still alive.
			Subscription                     m_displacementSubscription;

			virtual ~DisplacedGeometry();

			// Tessellate base + apply displacement + build internal indexed
			// mesh.  Idempotent: safe to call repeatedly via DestroyMesh()/
			// BuildMesh() to refresh after the displacement painter changes.
			// `const` (mutates only the mutable mesh members) so it can be
			// invoked from the const Realize() path.  CASCADES: realizes the
			// base before tessellating it (a displaced base must be baked
			// before TessellateToMesh re-emits its mesh).
			void BuildMesh() const;
			void DestroyMesh() const;

			// TRUE iff a height route is bound AND disp_scale != 0 -- i.e. the
			// vertices will actually move (the one predicate shared by
			// ApplyHeightField's shortcut and the refinement gate).
			bool WillDisplaceVertices() const;

			// Tessellate the base via TessellateToMesh and, when the base is a
			// MESH whose own tessellation ignores `detail` (a
			// TriangleMeshGeometryIndexed -- lathe / sweep / skin / imported
			// mesh -- or another DisplacedGeometry re-emitting its baked mesh)
			// AND vertices will be displaced, refine it so its longest edges
			// are <= (base bbox diagonal / m_detail): without this a mesh base
			// is displaced only at its OWN vertices and `detail` has no effect.
			// Bases that already honour `detail` (spheres, boxes, SDFs, ...)
			// are never touched.  Shared by BuildMesh and RefreshMeshVertices
			// so the two cannot drift; bLog suppresses the "refinement capped"
			// warning on the refit path.  Returns FALSE iff the base cannot
			// tessellate.
			//
			// shading0 / geom0: for a MESH base that will be displaced (refined or
			// not), the base's per-vertex shading normals and its angle-weighted
			// geometric normals on the UNdisplaced mesh; empty otherwise.  The
			// post-displacement normal recompute is purely geometric, and every
			// vertex a refinement adds lies on its parent facet's plane, so without
			// carrying the base's shading field through the displacement a refined
			// mesh would shade FLAT per facet (re-introducing the base's
			// polygonization).  ApplyTransportedShading rotates each base shading
			// normal by the rotation g0 -> g1 the displacement made to the geometric
			// normal, so the relief perturbs the base's own smooth field.
			// uvFlags: per-vertex chart-periodicity bits (TriangleMeshGeometryIndexed::
			// kUPeriodic / kVPeriodic) parallel to `vertices` (empty for a non-mesh base).
			bool TessellateAndRefine(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords,
				const bool             bLog,
				NormalsListType&       shading0,
				NormalsListType&       geom0,
				std::vector<unsigned char>& uvFlags,
				std::vector<unsigned int>&  weldDomains ) const;

			// WATERTIGHT DISPLACEMENT at creases.  A mesh base keeps a crease / cap / pinch /
			// UV seam as SEPARATE vertices -- coincident in position, different in normal and
			// UV -- and each would displace along its own normal by its own height, tearing the
			// crease open.  The vertices that are coincident in the UNdisplaced mesh (positions
			// welded by GeometricUtilities::WeldVertexPositions) are therefore moved to ONE
			// shared position, the mean of the group's displacement vectors; each member keeps
			// its own shading normal (the crease still shades as a crease).  WHICH coincident
			// groups are welded is the BASE MESH's own say (TriangleMeshGeometryIndexed::
			// DisplacementWeldPolicy): a lathe / sweep / skin welds every group (they are its
			// recorded creases, cap rims, duplicated profile points, pinch poles, crease
			// stations); an import or hand-built mesh welds only members whose base normals lie
			// within a crease cone (< 120 deg) -- coincident OPPOSITE-facing sheets (a two-sided
			// leaf, cloth) are separate surfaces and stay independent, so each keeps its full
			// relief -- and path_instances_geometry welds WITHIN each instance by its template's
			// own rule (every instance is a weld domain) and never across instances (touching
			// instances are separate bodies).  A cone-rule group clusters by the FIRST member's
			// normal (dot > -1/2).  The crease-cone rule needs the base normals: without them
			// the weld is skipped.  Consequences: a pinch's two poles stay together and the relief
			// averages to ~zero on that ring; opposite sheets may separate under displacement.
			// A displaced base is welded again (its baked mesh inherits the policy of ITS base),
			// because its coincident vertices leave the inner bake with different transported
			// shading normals.  Gated on the BASE MESH's normal source (a face-normal base is
			// flattened per corner by design and is not welded), NOT on this geometry's own
			// face_normals flag, which only chooses how the result SHADES.  Analytic and SDF
			// bases are not touched (a box's per-face corner duplicates still crack under
			// displacement: a pre-existing limitation of those tessellations).
			// Returns the base's policy, or -1 for no weld.
			int  BaseWeldPolicy() const;
			void WeldCoincidentDisplacement( const VerticesListType& undisplaced, const NormalsListType& baseNormals, const std::vector<unsigned int>& domains, const int policy, VerticesListType& displaced ) const;

			// Topology signature (FNV-1a over the triangle index list and vertex
			// count) of the mesh the current m_pMesh was built from.
			// RefreshMeshVertices re-tessellates (and re-refines) the base and
			// refits only when the signature matches -- the mesh-base refinement
			// is edge-length driven, so a base whose vertices moved (an animated
			// displaced base) could otherwise change topology while keeping the
			// same vertex COUNT, which UpdateVertices cannot detect.
			mutable std::uint64_t                    m_topologySignature;

			// The ONE place either height route touches vertex positions --
			// shared by BuildMesh() and RefreshMeshVertices() so the
			// `disp_scale == 0` shortcut, the `uv_seam_fold` tent treatment
			// and the route choice cannot drift between the bake path and
			// the animation-refit path.  Returns TRUE iff vertices actually
			// moved (the caller's gate on recomputing topology normals).
			bool ApplyHeightField(
				IndexTriangleListType& tris,
				VerticesListType&      vertices,
				NormalsListType&       normals,
				TexCoordsListType&     coords ) const;

			// Tier 1 §3 animation refit path: re-tessellate base + re-apply
			// displacement, then call m_pMesh->UpdateVertices() to swap
			// vertex/normal storage in place and refit the BVH instead of
			// rebuilding.  Falls back to DestroyMesh+BuildMesh if there
			// is no current mesh or topology has changed (which is not
			// currently possible — m_detail is fixed at construction).
			// Single-threaded editor / observer path only.
			void RefreshMeshVertices();

		public:
			DisplacedGeometry(
				IGeometry*            pBase,
				const unsigned int    detail,
				const IFunction2D*    displacement,
				const Scalar          disp_scale,
				const bool            bDoubleSided,
				const bool            bUseFaceNormals,
				const bool            bSeamFold = true,	//!< FALSE for open Cartesian displacement fields (no UV mirror)
				const IScalarPainter* pHeight = 0 );	//!< route (b); pass at most one of `displacement` / `pHeight`

			DisplacedGeometry( const DisplacedGeometry& ) = delete;
			DisplacedGeometry& operator=( const DisplacedGeometry& ) = delete;

			// True iff the RECIPE is valid (base geometry is non-null), known
			// at construction.  This is now a cheap recipe check, NOT a
			// mesh-presence check — the mesh is built lazily by Realize(), so
			// post-construction (pre-realize) the mesh is null but IsValid()
			// is already true.  (A base that can NEVER tessellate, e.g.
			// InfinitePlaneGeometry, makes IsValid() FALSE — refused at parse via
			// the cheap CanTessellate() capability check, not deferred to bake.)
			bool IsValid() const { return m_pBase != 0 && m_pBase->CanTessellate(); }

			//! True when the baked mesh shades with computed face normals (the ctor's bUseFaceNormals).
			bool UsesFaceNormals() const { return m_bUseFaceNormals; }

			//! Direct access to base geometry (e.g. for base curvature queries, DL-13).
			const IGeometry* GetBaseGeometry() const { return m_pBase; }

			// IRealizable (IGeometry): deferred-realization entry point.  Realize()
			// bakes the mesh once (idempotent via the internal m_bRealized flag).  A
			// failed bake (base TessellateToMesh false) leaves m_pMesh null and every
			// query guard-fails to miss/zero, so callers must null-check the mesh.
			void Realize() const override;

			// A displaced geometry tessellates (re-emits its baked mesh) iff its
			// base can — nested displaced-of-non-tessellatable is refused at parse.
			bool CanTessellate() const override { return m_pBase != 0 && m_pBase->CanTessellate(); }

			// Diagnostic: process-wide count of BuildMesh() invocations (every
			// actual tessellate + bake).  Used by DeferredRealizeTest to prove
			// the realize pass bakes only render-reachable displaced
			// geometries (the GuillocheWatch dial: 1 baked, not 6) and skips
			// the unbound ones.  Atomic so the count is well-defined even if a
			// future caller realizes off-thread; in practice realization is
			// single-threaded.  Same lightweight static-counter pattern as
			// TriangleMeshGeometryIndexed's s_nextGeometryId.
            bool HasUncertainSMSNormalOrientation() const;
            //! DL-382: neither certified closed nor certified open (the
            //! realized mesh's own certificates, or DL-345's convex-plane
            //! base). Set allowGeneralProof=false for legacy-only material consumers.
            //! Such an uncertified transmissive surface keeps the IOR-stack rule.
            bool HasUncertifiedOpenness(bool allowGeneralProof=true) const;
            bool IsCertifiedWatertight() const;
            bool IsProvablyOpenSheet(bool allowGeneralProof=true) const;
            //! DL-621(a): topological openness (see GeometricUtilities::TriangleMeshOpenness).
            int TopologicalOpenness() const;
            bool HasLegacyOpenSheetProof() const;
            bool HasNegativeOpenSignedVolume() const;
			static unsigned int GetBuildMeshCount();
			static void         ResetBuildMeshCount();

			//! Diagnostic: process-wide count of TessellateToMeshEx calls (the full-mesh COPY a
			//! consumer pays by tessellating a displaced geometry).  Lets a test prove a
			//! consumer reads the baked mesh in place instead (GetBakedMesh).
			static unsigned int GetTessellateCopyCount();
			static void         ResetTessellateCopyCount();

			//! The baked (refined, displaced) mesh, or NULL before Realize() / after a failed
			//! bake.  A read-only view for a consumer that needs the surface's vertices
			//! without TessellateToMesh's full copy of every array -- the agent's isolate
			//! surface-point gather stride-reads it, so its memory stays bounded however
			//! fine `detail` made the bake (up to the 1.5M-triangle refinement cap).  Not
			//! synchronized against RefreshMeshVertices' in-place refit (nor is
			//! TessellateToMeshEx, nor the render's IntersectRay): call it between renders.
			const ITriangleMeshGeometryIndexed* GetBakedMesh() const { return m_pMesh; }

			// IGeometry
			bool TessellateToMesh( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail ) const override;

			//! TessellateToMesh plus the per-output-vertex chart-periodicity bits of the
			//! baked mesh (see TriangleMeshGeometryIndexed::SetUVPeriodicRegion) -- how a
			//! displaced-of-displaced base keeps its wrap through the bake.
			bool TessellateToMeshEx( IndexTriangleListType& tris, VerticesListType& vertices, NormalsListType& normals, TexCoordsListType& coords, const unsigned int detail, std::vector<unsigned char>* pUVPeriodic, std::vector<unsigned int>* pWeldDomains = 0 ) const;
			void IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override;
			bool IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const override;

			void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override;
			BoundingBox GenerateBoundingBox() const override;
			inline bool DoPreHitTest() const override { return true; }

			void UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const override;
			Scalar GetArea() const override;
			//! DL-320: the internal mesh flips back-face hits when m_bDoubleSided (see IGeometry).
			bool IsDoubleSided() const override { return m_bDoubleSided; }

			SurfaceDerivatives ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const override;

			//! IGeometry::DistanceToSurface -- DELEGATE to the baked mesh,
			//! exactly as IntersectRay does, so `proximity(r)` measures the
			//! DISPLACED surface and not the smooth base
			//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2, Phase 2).  This
			//! is the whole point of the row: a heightfield that lifts the
			//! surface by `disp_scale` moves contact by `disp_scale`, and
			//! answering from the base would under-report the gap by exactly
			//! that -- the forbidden direction.
			//!
			//! It calls `Realize()` first, which the ray forwarders
			//! deliberately do NOT: they are the hot path and pay for a bake
			//! that `RayCaster::AttachScene` has already done, while this
			//! query is rare, heavy, and reachable from a test or a tool that
			//! never went through AttachScene.  On an already-realized
			//! geometry Realize() is one acquire load and returns before its
			//! own render-freeze assert, so the production path costs that
			//! load and nothing else.
			//!
			//! Refuses (like every other forwarder here) when the bake failed
			//! and left no mesh.  The mesh's own SHEET semantics apply -- see
			//! TriangleMeshGeometryIndexed::DistanceToSurface.
			bool DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const override;

			//! IGeometry::SignedDistanceLower -- DL-31 sibling.  Same
			//! DELEGATE-to-the-baked-mesh shape as `DistanceToSurface` right
			//! above, added when `TriangleMeshGeometryIndexed` gained a real
			//! `SignedDistanceLower` for certified-watertight meshes: without
			//! this forwarder, `interior(r)` could never see a displaced
			//! surface even when its baked mesh IS watertight (a displaced
			//! sphere's topology is unchanged by displacement -- only vertex
			//! POSITIONS move), because the base `IGeometry::SignedDistanceLower`
			//! refuses unconditionally and `DisplacedGeometry` never overrode
			//! it. `Realize()`'s cost/rarity argument is identical to
			//! `DistanceToSurface`'s -- see that method's comment.
			bool SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
				Scalar& outSigned, bool& outExact ) const override;

			//! IGeometry::SelfHitRootFloor -- DELEGATE, exactly as IntersectRay
			//! does: this geometry's rays are answered by the baked mesh, so
			//! the mesh owns the gate.  Before the bake (or after a failed one)
			//! there is no surface to re-hit and the base default is the honest
			//! answer.
			Scalar SelfHitRootFloor( const Point3& localOrigin, const Vector3& localDir, const Vector3& localNormal ) const override
			{
				return m_pMesh
					? m_pMesh->SelfHitRootFloor( localOrigin, localDir, localNormal )
					: IGeometry::SelfHitRootFloor( localOrigin, localDir, localNormal );
			}

			//! Smoothing-aware analytical query.  Composes the BASE geometry's
			//! analytical derivatives with the displacement painter via the
			//! standard chain rule, scaling `disp_scale` by `(1 - smoothing)`.
			//! BOTH height routes are handled: the IFunction2D one central-
			//! differences the function in (u, v); the IScalarPainter one
			//! central-differences the FIELD along the base's own object-space
			//! `dpdu`/`dpdv` (first-order `P(u ± eps) ~= P_b ± eps*dpdu`), so no
			//! additional base evaluations are needed, and steps `ptCoord` in
			//! lockstep so a UV-reading field differentiates identically on
			//! either route.
			//! At smoothing=1 the displacement contribution vanishes and the
			//! result equals the base's analytical (recursing for nested
			//! displaceds) — exposing the base surface curvature.
			//! At smoothing=0 the result matches the actual displaced surface
			//! (what ray intersections and expression curv/curvR report by default).
			//! Used by SMS two-stage solver — see `docs/SMS_TWO_STAGE_SOLVER.md`.
			bool ComputeAnalyticalDerivatives(
				const Point2& uv,
				Scalar        smoothing,
				Point3&       outPosition,
				Vector3&      outNormal,
				Vector3&      outDpdu,
				Vector3&      outDpdv,
				Vector3&      outDndu,
				Vector3&      outDndv
				) const override;

			// Keyframable — static after construction; no keyframeable parameters.
			IKeyframeParameter* KeyframeFromParameters( const String& /*name*/, const String& /*value*/ ) override { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& /*val*/ ) override {}
			void RegenerateData() override {}
		};
	}
}

#endif
