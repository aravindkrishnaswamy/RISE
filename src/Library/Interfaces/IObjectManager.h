//////////////////////////////////////////////////////////////////////
//
//  IObjectManager.h - Interface to the object manager, which manages
//    all objects in a scene
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOBJECTMANAGER_
#define IOBJECTMANAGER_

#include "IManager.h"
#include "IEnumCallback.h"
#include "../Intersection/RayIntersection.h"
#include <cstddef>
#include <utility>
#include <vector>

namespace RISE
{
	class ICamera;
	class IObjectPriv;
	class ISampler;

	class IObjectManager : public virtual IManager<IObjectPriv>
	{
	protected:
		IObjectManager(){};
		virtual ~IObjectManager(){};

	public:
		//
		// Object specific
		//

		//! Intersects a ray with the object
		virtual void IntersectRay( 
			RayIntersection& ri,						///< [in/out] Intersection details at point of intersection if there is an intersection
			const bool bHitFrontFaces,					///< [in] Should front facing hits be processed?
			const bool bHitBackFaces,					///< [in] Should back facing hits be processed?
			const bool bComputeExitInfo					///< [in] Should exit information be computed (the ray continues until exiting the object) in addition of initial intersection information?
			) const = 0;

		//! Light-visibility any-hit query: tests only `IsWorldVisible() && DoesCastShadows()`.
		//! Use IntersectOcclusionRay() for geometry-presence queries (e.g. AO).
		virtual bool IntersectShadowRay(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Enumerates the objects with normal interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObject>& pFunc				///< [in] Callback functor interface that accepts normal IObject interface
			) const = 0;

		//! Enumerates the objects with their priviledged interfaces
		virtual void EnumerateObjects( 
			IEnumCallback<IObjectPriv>& pFunc			///< [in] Callback functor interface that accepts the IObjectPriv interface
			) const = 0;

		//! Tells all the objects to reset any runtime data
		virtual void ResetRuntimeData() const = 0;

		//! Eagerly composes transforms, realizes deferred geometry, validates path-dependence
		//! invariants (e.g. SMS exclusions / one-shot warning), and builds the TLAS before
		//! multi-threaded rendering.
		virtual void PrepareForRendering() const = 0;

		//! Destroys the spatial acceleration structure (and swept motion bounds) so the next
		//! PrepareForRendering() rebuilds from current transforms.
		virtual void InvalidateSpatialStructure() const = 0;

		// ---- Motion blur (single-threaded between frames) ----
		// Bracket shutter sampling with BeginMotionSweep / AccumulateMotionBounds / EndMotionSweep
		// so the TLAS bounds each object across the whole exposure (padded for curved motion).

		//! Clears swept motion boxes; next build uses current static bounds.
		virtual void ClearMotionBounds() const {}

		//! Starts a shutter sweep, clearing prior swept bounds and tracking moved parents.
		virtual void BeginMotionSweep() const {}

		//! Unions every object's current world bbox (finite axes) into its swept box.
		virtual void AccumulateMotionBounds() const {}

		//! Ends a sweep and narrows per-sample RecomposeAnimatedHierarchy() to moved subtrees.
		virtual void EndMotionSweep() const {}

		//! Re-composes animated parented hierarchies without touching the TLAS.
		virtual void RecomposeAnimatedHierarchy() const {}

		//! Parent-before-child (node, parent) re-compose plan for per-thread motion-blur clones;
		//! returns false if no sweep narrowed the walk (caller must stay single-threaded).
		virtual bool GetPerSampleRecomposePlan( std::vector< std::pair<IObjectPriv*, IObjectPriv*> >& out ) const { out.clear(); return true; }

		// ---- Authored scene graph (name-based child->parent side index; no pointer cycles) ----

		//! Records or clears (null/"") `child`'s parent; refuses unknown names, self-links, or
		//! cycles. Detaching immediately recomposes the ex-child and invalidates the TLAS
		//! (accepted on CSG operands as a repair path).
		virtual bool SetObjectParent(
			const char* child,							///< [in] Name of the child object
			const char* parent							///< [in] Name of the parent object, or null / "" to detach
			) = 0;

		//! Parent name for `child` (or ""); manager-owned pointer invalidated by link-map mutations.
		virtual const char* GetObjectParent(
			const char* child							///< [in] Name of the child object
			) const = 0;

		//! True if any registered object names `parent` as its parent.
		virtual bool HasChildren(
			const char* parent							///< [in] Name of the candidate parent
			) const = 0;

		//! Idempotently bakes `world = parent.world * local` parent-before-child via
		//! FinalizeTransformations(). Returns true iff any world matrix changed (consumed once).
		virtual bool ComposeWorldTransforms() const = 0;

		//! Monotonic counter incremented on every InvalidateSpatialStructure() call.
		virtual unsigned long long GetSpatialStructureGeneration() const = 0;

		// Append-only interface tail below (preserves vtable layout).

		//! Records provenance for synthesized expansion `entry`; returns false if `entry` is empty.
		virtual bool SetObjectProvenance(
			const char* entry,							///< [in] Name of the synthesized entry
			const char* instancingChunk,				///< [in] Name on the chunk that produced it
			const char* sourceNode						///< [in] Object it instanced, or null / "" if none
			) = 0;

		//! Maps a synthesized entry back to its editable chunk/source (false for authored objects).
		//! Returned pointers are manager-owned and invalidated by provenance/item mutations.
		virtual bool GetObjectProvenance(
			const char* entry,							///< [in] Name of the entry to trace
			const char** outInstancingChunk,			///< [out] Chunk name; may be null if not wanted
			const char** outSourceNode					///< [out] Source object name ("" if none); may be null
			) const = 0;

		//! Geometry-presence any-hit query: tests every `IsWorldVisible()` object, ignoring
		//! `DoesCastShadows()` (for AO/occlusion callers; sibling of IntersectShadowRay).
		virtual bool IntersectOcclusionRay(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Shortest distance (upper bound, in [0, maxDistWorld)) from `ptWorld` to any other
		//! world-visible, non-emissive object (`self` excluded by identity). Casts no rays and
		//! evaluates no painters; logs once per refusing object; optional TLAS diagnostic counters
		//! are incremented.
		virtual bool NearestOtherSurface(
			const Point3& ptWorld,						///< [in] The world-space point to measure from
			const IObject* self,						///< [in] The object the point belongs to; never contributes.  May be null
			const Scalar maxDistWorld,					///< [in] Search radius, a WORLD LENGTH; candidates beyond it are skipped
			Scalar& outDist,							///< [out] Shortest distance found, world units
			std::size_t* outNodesVisited = 0,			///< [in,out] Diagnostic: incremented by BVH nodes visited (TLAS path only)
			std::size_t* outCandidatesVisited = 0		///< [in,out] Diagnostic: incremented by candidate objects evaluated (TLAS path only)
			) const = 0;

		//! Deepest containment lower bound (> 0) across all other world-visible, non-emissive
		//! objects containing `ptWorld` (via SignedDistanceLower; sheet families refuse silently).
		//! Uses the same TLAS/snapshot candidate source as NearestOtherSurface.
		virtual bool DeepestOtherContainment(
			const Point3& ptWorld,						///< [in] The world-space point to test
			const IObject* self,						///< [in] The object the point belongs to; never contributes.  May be null
			const Scalar maxDepthWorld,					///< [in] Effort budget, a WORLD LENGTH (the caller's radius)
			Scalar& outDepth,							///< [out] Deepest containment found, world units
			std::size_t* outNodesVisited = 0,			///< [in,out] Diagnostic: incremented by BVH nodes visited (TLAS path only)
			std::size_t* outCandidatesVisited = 0		///< [in,out] Diagnostic: incremented by candidate objects evaluated (TLAS path only)
			) const = 0;

		//! Shadow any-hit query excluding objects whose material reports HasDeltaPassThrough()
		//! (objects without an own pass-through material are treated as opaque).
		virtual bool IntersectShadowRayOpaque(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! Returns a world-visible object (other than `self`) with random-walk SSS whose interior
		//! contains `ptWorld`, or null. Tests SignedDistanceLower first, falling back to two-way
		//! crossing parity along +/-`probeDir`; rejects `bProvablyNoInterior` / ray-derived hits.
		virtual const IObject* RandomWalkObjectContaining(
			const Point3& ptWorld,						///< [in] The world-space point to test
			const Vector3& probeDir,					///< [in] Unit direction of the containment ray
			const IObject* self							///< [in] Never counts.  May be null
			) const = 0;

		//! Lists up to `maxOut` world-visible objects (other than `self`, with a material)
		//! whose world box lies within `radius` of `ptWorld`; returns count written.
		virtual unsigned int ObjectsNearPoint(
			const Point3& ptWorld,						///< [in] The world-space point
			const Scalar radius,						///< [in] World distance from the point to a candidate's box
			const IObject* self,						///< [in] Never counts.  May be null
			const IObject** out,						///< [out] Candidates
			const unsigned int maxOut					///< [in] Capacity of `out`
			) const = 0;

		//! Containment predicate for `pObject` (same SignedDistanceLower / two-way parity rule
		//! as RandomWalkObjectContaining).
		virtual bool ObjectContainsPoint(
			const IObject* pObject,						///< [in] The object asked
			const Point3& ptWorld,						///< [in] The world-space point
			const Vector3& probeDir						///< [in] Unit direction of the parity rays
			) const = 0;

		//! Intersects a single scene object `pObject` (both faces, within `dHowFar`) and stamps
		//! cross-object shading signals identically to a scene cast.
		virtual void IntersectSingleObject(
			RayIntersection& ri,						///< [in/out] The ray in, the hit (if any) out
			const IObject* pObject,						///< [in] The only object tested
			const Scalar dHowFar						///< [in] Farthest accepted range
			) const = 0;
        //! Coverage-aware transport traversal. Skips are not path vertices;
        //! the published ray and range still describe the complete segment.
        //! With records, traversal spans [startDistance,maxDistance); only
        //! [occlusionStart,occlusionEnd) may return an occluder. A record is
        //! the single accepted event, in original-ray coordinates, even when
        //! a DL-05 caller resumes after a previous accepted interface.
        void IntersectRaySampled(RayIntersection& ri, ISampler& sampler,
            bool front = true, bool back = true, bool exit = false,
            Scalar maxDistance = RISE_INFINITY, bool shadows = false, MediumBoundaryHits* boundaries = nullptr, bool boundariesOnly = false, Scalar occlusionEnd = RISE_INFINITY, Scalar occlusionStart = 0, Scalar startDistance = 0) const;
        //! Collect accepted medium boundaries over the full segment, ignoring ordinary occluders.
        void CollectMediumBoundaryHitsSampled(const Ray& ray, Scalar distance, ISampler& sampler, MediumBoundaryHits& boundaries, const ICamera* pCamera = nullptr) const;
        bool IntersectShadowRaySampled(const Ray& ray, Scalar distance, ISampler& sampler, MediumBoundaryHits* boundaries = nullptr, Scalar physicalDistance = -1, Scalar occlusionStart = 0, const ICamera* pCamera = nullptr) const;
    };
}

#include "IObjectPriv.h"

#endif
