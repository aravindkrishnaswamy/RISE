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

namespace RISE
{
	class IObjectPriv;

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

		//! LIGHT-VISIBILITY query: any-hit test against every object that is
		//! both IsWorldVisible() AND DoesCastShadows().  An object authored
		//! with `casts_shadows FALSE` is INVISIBLE to this query by design --
		//! it does not block light, so NEE / light-sampling visibility tests
		//! (the intended callers) must not see it as an occluder.
		//!
		//! Do NOT reuse this for a geometry-presence / occlusion query (e.g.
		//! ambient occlusion): such a query wants to know whether there is
		//! GEOMETRY in the way, independent of whether that geometry casts
		//! shadows.  Use IntersectOcclusionRay for that -- see its contract
		//! below.  (2026-09-07: this exact confusion was a real bug --
		//! docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md section 8.1.)
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

		//! Eagerly builds spatial acceleration structures before rendering begins.
		//! Must be called before multi-threaded rendering to avoid lazy init during render.
		virtual void PrepareForRendering() const = 0;

		//! Destroys the current spatial acceleration structure so it can be rebuilt.
		//! Used when animated object transforms invalidate the world-space bounding
		//! boxes that the BSP/octree was built from. Call PrepareForRendering() after
		//! this to rebuild with current transforms.
		virtual void InvalidateSpatialStructure() const = 0;

		// ---- Authored scene graph (docs/agentic-redesign/87-recursive-scene-graph.md)
		//
		// TWO REPRESENTATIONS, SEPARATE JOBS.  The manager's item map is the
		// FLAT RENDER LIST: every entry carries a fully composed world
		// transform, and it is what the TLAS is built over and what all nine
		// world-visible enumeration consumers walk.  The parent links below are
		// the AUTHORED GRAPH: a side index that says nothing about rendering
		// and exists so ComposeWorldTransforms can bake the flat list's world
		// transforms, and so the outliner can show a tree.
		//
		// Links are stored BY NAME, one direction only (child -> parent name).
		// No pointer is retained, so no lifetime cycle is possible and a
		// removed parent degrades to "treat the child as a root" rather than
		// to a dangling deref.

		//! Record `child`'s parent.  A null / empty `parent` DETACHES the child
		//! (it becomes a root).  Refuses -- with a diagnostic, changing
		//! nothing -- when the child or the parent is not a registered object,
		//! when they are the same object, or when the link would close a cycle.
		//! \return TRUE if the link was recorded (or cleared), FALSE if refused.
		//!
		//! NOT pure bookkeeping on the DETACH path.  An implementation must
		//! re-compose the ex-child there and invalidate, because the ex-child
		//! has just left the link map: no later walk can find it, and it would
		//! otherwise render at its ex-parent's composed pose behind a TLAS leaf
		//! bounding it at that old position -- unhittable and unpickable.  A
		//! RE-parent needs neither: the new link is visible to the next
		//! per-frame compose.  Callers that own a Scene still owe the
		//! LIGHT-TOPOLOGY bump on a real link change; this interface cannot
		//! reach one.  See 87 §5 step 2.
		//!
		//! ASYMMETRY WORTH KNOWING: a DETACH is accepted on a CSG operand,
		//! while every parenting form on that same node is refused.  That is
		//! deliberate, not a missed guard -- an operand can only ever be
		//! holding a link through a path that bypassed those refusals, and the
		//! detach is precisely the repair for it.  Refusing here would make
		//! such a node unrepairable.
		virtual bool SetObjectParent(
			const char* child,							///< [in] Name of the child object
			const char* parent							///< [in] Name of the parent object, or null / "" to detach
			) = 0;

		//! The recorded parent name for `child`, or "" when it is a root or
		//! not a registered object.  The returned pointer is owned by the
		//! manager and is invalidated by anything that changes the link map --
		//! SetObjectParent, and also RemoveItem, which erases the removed
		//! object's row AND re-roots every orphan.  Consume it immediately.
		virtual const char* GetObjectParent(
			const char* child							///< [in] Name of the child object
			) const = 0;

		//! Does any registered object name `parent` as its parent?  Cheap query
		//! over the link map.  Removing such an object MOVES its children (they
		//! fall back to their own local transforms), which is a spatial and
		//! potentially a light-topology change that no per-object check can
		//! see -- the children are not the object being removed.
		virtual bool HasChildren(
			const char* parent							///< [in] Name of the candidate parent
			) const = 0;

		//! Walk the authored graph and bake `world = parent.world * local` into
		//! every object, in parent-before-child order, via
		//! ITransformable::FinalizeTransformations( parentWorld ).  Idempotent:
		//! composition is an ARGUMENT, never a stack push, so running this
		//! twice yields the same matrices.  Objects with no parent link are
		//! finalized against identity, which is exactly what they did before
		//! hierarchy existed.
		//! \return TRUE if ANY object's world matrix actually changed.  The CST
		//! incremental apply uses this as its spatial gate: a re-parented or
		//! re-posed container moves DESCENDANTS that the edit closure never
		//! contains, so their bounding boxes cannot be compared pairwise -- this
		//! answer is what makes the TLAS invalidation sound.
		//!
		//! THE SIGNAL IS CONSUMED BY WHOEVER CALLS THIS.  A second call returns
		//! FALSE because the first one already did the work -- so an
		//! implementation that composes INTERNALLY (SetObjectParent's detach
		//! path does) owes the invalidation itself; its caller's gate will
		//! correctly see "nothing changed" and skip.  That is not a subtlety,
		//! it is how a detached object went unhittable once.
		virtual bool ComposeWorldTransforms() const = 0;

		//! Monotonic counter advanced every time InvalidateSpatialStructure() runs.
		//! A consumer reads it across an edit to confirm whether the top-level
		//! acceleration was invalidated: the CST incremental apply's closure-gated
		//! invariant pass leaves it UNCHANGED for a non-spatial edit (material/painter
		//! value -- object bboxes unchanged) and advances it for a spatial edit
		//! (geometry-extent / transform change), which is exactly the "non-spatial
		//! edit skips the TLAS" property (docs/agentic-redesign/21-stable-apply-and-
		//! resolver.md slices 3-4).
		virtual unsigned long long GetSpatialStructureGeneration() const = 0;

		//! 87 step 3: record that `entry` was SYNTHESIZED -- produced by an
		//! expansion rather than by a chunk of its own name.
		//! `instancingChunk` is the name on the chunk an author can edit;
		//! `sourceNode` is the object that chunk instanced.  "" is accepted and
		//! means "no source OBJECT"; no expansion produces that today (87 step
		//! 3d deleted the one that did), so it is a tolerated input, not a
		//! live case.
		//!
		//! Appended at the interface TAIL, with its getter, because this repo
		//! pins vtable order (tests/IJobVtableManifest.txt pins IJob; the
		//! convention is append-only everywhere).
		//! \return FALSE for a null/empty `entry`.
		virtual bool SetObjectProvenance(
			const char* entry,							///< [in] Name of the synthesized entry
			const char* instancingChunk,				///< [in] Name on the chunk that produced it
			const char* sourceNode						///< [in] Object it instanced, or null / "" if none
			) = 0;

		//! Where did `entry` come from?  FALSE -- and both out-pointers
		//! untouched -- when `entry` is an ordinary authored object, which is
		//! the overwhelmingly common answer.
		//!
		//! THE ONLY SANCTIONED WAY to map a rendered entry back to an editable
		//! chunk.  Callers must NOT reconstruct the relationship from the name
		//! itself (splitting on `.`, probing for a `[`): a synthesized name's
		//! spelling belongs to the expansion that made it.
		//!
		//! The returned pointers are owned by the manager and are invalidated
		//! by anything that changes the provenance map -- SetObjectProvenance,
		//! RemoveItem, Shutdown.  Consume them immediately.
		virtual bool GetObjectProvenance(
			const char* entry,							///< [in] Name of the entry to trace
			const char** outInstancingChunk,			///< [out] Chunk name; may be null if not wanted
			const char** outSourceNode					///< [out] Source object name ("" if none); may be null
			) const = 0;

		//! GEOMETRY-PRESENCE query: any-hit test against every object that
		//! IsWorldVisible(), IGNORING DoesCastShadows() entirely.  This is
		//! the occlusion contract an ambient-occlusion-style estimator
		//! wants -- "is there geometry in the way", not "does light reach
		//! past this object" -- so an object authored with `casts_shadows
		//! FALSE` still occludes here (it has not stopped existing; it has
		//! only stopped blocking light for NEE purposes).
		//!
		//! Sibling of IntersectShadowRay above, which answers the OPPOSITE
		//! question (light visibility, `casts_shadows`-gated) for NEE /
		//! light-sampling callers.  Added 2026-09-07 after
		//! AmbientOcclusionShaderOp was found reusing IntersectShadowRay
		//! for a query it is not -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
		//! section 8.1 and IntersectShadowRay's comment above.
		//!
		//! Appended at the interface TAIL, matching this file's own
		//! append-only convention for prior additions (SetObjectParent,
		//! ComposeWorldTransforms, GetSpatialStructureGeneration,
		//! SetObjectProvenance, GetObjectProvenance above) -- ObjectManager
		//! is this interface's sole in-tree implementer, always obtained
		//! through RISE's own factories (RISE_API_CreateObjectManager) and
		//! never constructed by out-of-tree callers, so a new pure virtual
		//! here is source-breaking (a hypothetical external IObjectManager
		//! subclass fails to compile until it implements this too) but not
		//! a silent binary-vtable hazard the way the same addition would be
		//! on IScene/IMaterial, which ARE meant to be reached through
		//! caller-supplied instances.
		virtual bool IntersectOcclusionRay(
			const Ray& ray,								///< [in] The ray to process the intersection from
			const Scalar dHowFar,						///< [in] Maximum distance to travel along that ray (optimization parameter)
			const bool bHitFrontFaces,					///< [in] Should we process the intersection if the element is front facing?
			const bool bHitBackFaces					///< [in] Should we process the intersection if the element is back facing?
			) const = 0;

		//! SHORTEST DISTANCE from `ptWorld` to the surface of any OTHER
		//! world-visible, non-emissive object -- the query behind the
		//! `proximity(r)` texture builtin
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).
		//!
		//! CASTS NO RAY AND EVALUATES NO PAINTER.  It walks the top-level
		//! BVH as a point query where one exists (a cached world-AABB
		//! snapshot on the small-scene fallback), asks each surviving candidate for its own
		//! closed-form (or bounded) point-to-surface distance in that
		//! object's own space, and keeps the minimum.  That is what makes
		//! it safe from every render thread by the same argument
		//! `IntersectShadowRay` makes -- read-only against geometry and
		//! transforms that are immutable for the pass -- and what makes it
		//! reentrancy-free where a ray-based estimator would not be.
		//!
		//! WHY A DISTANCE AND NOT A RAY FAN.  A thin object resting on a
		//! plane subtends grazing directions only, so every hemisphere
		//! estimator reads a two-to-ten-pixel band rather than a seam
		//! (measured: docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §8.1).  The
		//! seam IS the set of points within a millimetre or two of another
		//! surface, which only a true distance function reports.
		//!
		//! THREE EXCLUSIONS, each deliberate:
		//!   * `self` never contributes -- its own surface is at distance
		//!     zero from every point on it.  Identity, not name: two
		//!     INSTANCED COPIES of one geometry are different objects and
		//!     do count against each other.
		//!   * objects failing `IsWorldVisible()`, which is exactly the
		//!     filter `IntersectOcclusionRay` uses and is what excludes CSG
		//!     operands (they ARE registered in the manager's map; only the
		//!     composite is world-visible).  `casts_shadows FALSE` does NOT
		//!     exempt anything -- this is geometry presence, the same
		//!     contract `IntersectOcclusionRay` was split out for.
		//!   * EMITTERS (`GetMaterial() && GetMaterial()->GetEmitter()`,
		//!     the predicate LuminaryManager uses to decide what NEE
		//!     samples).  A light panel parked two millimetres off a wall
		//!     must not paint grime on it.  Unreal's distance-field set and
		//!     Blender's proximity target contain no lights either.  The
		//!     cost is that a DECORATIVE emitter -- a lava pool, a glowing
		//!     rune -- also stops collecting contact dirt; disclosed in the
		//!     design's §10, not fixed in v1.
		//!
		//! DIRECTION OF ERROR, where a family cannot be exact: the answer
		//! is an UPPER bound on the true distance, never a lower one.  So
		//! `proximity` may UNDER-paint a seam and can never paint one that
		//! is not there.  A candidate that cannot answer contributes
		//! nothing (it is treated as far) and says so ONCE PER REFUSING
		//! OBJECT in the log -- honest absence over a wrong distance.  The
		//! latch is `IObject::NoteDistanceRefusal`; the line is printed by
		//! the manager, which is the only party that knows the chunk's
		//! name; and the refusal is re-confirmed at an UNBOUNDED radius
		//! before printing, because an SDF's lower-bound early-out also
		//! returns false for a neighbour that is merely out of range and
		//! must not be reported as unable to answer.  THAT CONFIRM REMOVES
		//! ONLY THE RADIUS-DEPENDENT false positives; it cannot tell a family
		//! that never answers apart from a one-off per-point solver failure
		//! (an SDF bracket that did not close), so the printed line claims
		//! only "could not answer this one query, even unbounded" -- see
		//! `ObjectManager::LogDistanceRefusal` and
		//! docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2 for which families
		//! never answer versus which (SDFs) this diagnostic is best-effort
		//! for.
		//!
		//! \return TRUE and writes `outDist` (in **[0, maxDistWorld)**) when
		//!         some candidate answered within the radius; FALSE with
		//!         `outDist` untouched when nothing did -- which is also
		//!         the answer for a non-finite point, a non-positive or
		//!         non-finite radius, and an empty scene.
		//!
		//! BOTH ENDS OF THAT INTERVAL ARE THE CODE'S, not a convention:
		//! zero is ATTAINED, because interpenetration is contact and the
		//! solid families clamp their signed field at zero rather than
		//! taking its absolute value; and `maxDistWorld` is EXCLUDED,
		//! because a candidate is only accepted on `d < best` with `best`
		//! starting at the radius.  A neighbour exactly `maxDistWorld` away
		//! is therefore not found, and `proximity` reads its neutral 0
		//! there -- which is the same number `1 - d/r` would have given, so
		//! the signal is continuous across the cut-off either way.
		virtual bool NearestOtherSurface(
			const Point3& ptWorld,						///< [in] The world-space point to measure from
			const IObject* self,						///< [in] The object the point belongs to; never contributes.  May be null
			const Scalar maxDistWorld,					///< [in] Search radius, a WORLD LENGTH; candidates beyond it are skipped
			Scalar& outDist								///< [out] Shortest distance found, world units
			) const = 0;

		//! HOW DEEP INSIDE ANOTHER OBJECT is `ptWorld`?  The signed half of
		//! the query above, and what `interior(r)` reads
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).
		//!
		//! THE SAME CANDIDATE SET, by the same three exclusions -- self by
		//! identity, `IsWorldVisible()` (which is what keeps CSG operands
		//! out), and emitters.  Three things differ, each for a reason:
		//!
		//!   * THE RUNNING QUANTITY IS A MAXIMUM, not a minimum.  If a
		//!     point is inside two overlapping solids, LEAVING their union
		//!     needs at least the larger of the two depths, so the max is
		//!     still a lower bound on the true depth -- the under-paint
		//!     direction, as for `proximity`.
		//!   * THERE IS NO DISTANCE-BASED PRUNE.  `NearestOtherSurface`
		//!     shrinks its budget as it finds closer neighbours; a maximum
		//!     cannot.  The only prune is the candidate's cached world box:
		//!     a box that does not contain the point cannot contain it,
		//!     and unlike the proximity scan the test is ORDINARY
		//!     containment with no radius expansion.
		//!   * IT READS THE SAME CANDIDATE SOURCE AS `NearestOtherSurface`:
		//!     the top-level BVH where one exists (`BVH::ForEachContainingPoint`
		//!     descends only the nodes whose box contains the point -- a
		//!     containment walk needs no distance prune, so a maximum is as
		//!     cheap to collect as a minimum) and the AABB snapshot only on
		//!     the small-scene fallback.  So it is stale EXACTLY as the render
		//!     and `proximity` are: an object added without an invalidate is
		//!     invisible to all three until the structure is rebuilt
		//!     (ProximitySignalTest (g2) pins all three together).  Phase 3's
		//!     first cut read the count-checked snapshot instead and was
		//!     visible to the add alone; review round 1 closed that.
		//!   * IT DOES NOT LOG REFUSALS.  Every SHEET family refuses
		//!     containment at every point BY DESIGN -- a plane, a disk, an
		//!     open cylinder, a mesh, a patch and hair have no inside to
		//!     be in -- so a shared latch would print the proximity
		//!     message for every mesh and every plane in the scene.
		//!     Refusal here is silent, and disclosed in §10.
		//!
		//! The per-object call is `IObject::SignedDistanceLower`, and only
		//! a NEGATIVE answer counts: a positive one says the point is
		//! outside that candidate, which is the ordinary case.
		//!
		//! \return TRUE and writes `outDepth` (> 0) when some candidate
		//!         contained the point; FALSE with `outDepth` untouched
		//!         otherwise -- including for a non-finite point, a
		//!         non-positive or non-finite `maxDepthWorld`, and an
		//!         empty scene.  `interior` reads FALSE as its neutral 0.
		virtual bool DeepestOtherContainment(
			const Point3& ptWorld,						///< [in] The world-space point to test
			const IObject* self,						///< [in] The object the point belongs to; never contributes.  May be null
			const Scalar maxDepthWorld,					///< [in] Effort budget, a WORLD LENGTH (the caller's radius)
			Scalar& outDepth							///< [out] Deepest containment found, world units
			) const = 0;
	};
}

#include "IObjectPriv.h"

#endif
