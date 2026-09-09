//////////////////////////////////////////////////////////////////////
//
//  ObjectManager.h - Declaration of the ObjectManager class which
//    helps scenes manage various geometric objects.  It builds a
//    top-level BVH4 acceleration structure (TLAS) over the objects,
//    with a linear-loop fallback for tiny scenes; see
//    Job::SetPrimaryAcceleration and docs/ARCHITECTURE.md "Top-Level
//    Acceleration (TLAS)".
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 16, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OBJECTMANAGER_
#define OBJECTMANAGER_

#include "../Interfaces/IObjectManager.h"
#include "../Utilities/Threads/Threads.h"
#include "GenericManager.h"
#include "../Acceleration/BVH.h"
#include "../Octree.h"

#include <map>
#include <set>
#include <atomic>	// the proximity AABB snapshot's publish/read edge

namespace RISE
{
	namespace Implementation
	{
		class ObjectManager : 
			public virtual IObjectManager, 
			public virtual GenericManager<IObjectPriv>,
			public virtual TreeElementProcessor<const IObjectPriv*>
		{
		protected:
			virtual ~ObjectManager();

			// Top-level acceleration: SAH BVH (BVH4-collapsed, SIMD AABB
			// test) over scene objects.  bUseBSPtree is the constructor
			// gate name kept for back-compat; semantically it now means
			// "build a top-level BVH".
			mutable BVH<const IObjectPriv*>*    pBVH;
			mutable Octree<const IObjectPriv*>* pOctree;
			mutable unsigned long long          mSpatialGen;   //!< advanced on every InvalidateSpatialStructure (see IObjectManager)

			bool bUseBSPtree;
			bool bUseOctree;
			const unsigned int nMaxObjectsPerNode;
			const unsigned int nMaxTreeDepth;

			RMutex treeCreationMutex;

			//! The AUTHORED graph: child name -> parent name.  Only non-root
			//! objects appear.  See IObjectManager's block comment for why this
			//! is by name and one-directional.
			//!
			//! LIVES AND DIES WITH THE MANAGER.  Two consequences worth knowing
			//! before 87 step 3 (instancing) makes them hotter:
			//!  - Job::SetPrimaryAcceleration REPLACES the manager, so it drops
			//!    every link along with every object.  Its existing contract
			//!    ("call before adding objects") already covers this.
			//!  - Scene::RestoreFromSnapshot (marked EXPERIMENTAL, not a
			//!    production path) clears the manager name-by-name and re-adds
			//!    clones, so the links do not survive it.  The clones carry the
			//!    right composed matrices, so the scene LOOKS right until the
			//!    next compose finds every node a root and flattens the
			//!    hierarchy to local poses.  Restoring the link map alongside
			//!    the objects is the fix if that path is ever productionised.
			//!    87 STEP 3b SCALES THE BLAST RADIUS, and `provenanceByName`
			//!    (below) is dropped by the same path with the same effect.
			//!    Before 3b a lost link stranded ONE authored child; a `source`
			//!    instance of a subtree produces a whole synthesized SUBTREE
			//!    whose members exist only as links to their clone-parents, so
			//!    the flatten moves every one of them at once.  It is worst for
			//!    a member whose LOCAL transform is identity -- a cloned
			//!    `rect_light` places its panel through `center` / `facing`, not
			//!    through a local matrix, so once its link to the instance root
			//!    is gone it collapses to the WORLD ORIGIN rather than merely
			//!    losing the instance's offset.  Losing `provenanceByName` on
			//!    top of that leaves those entries unattributable: the editor
			//!    can no longer answer "which chunk do I move to move this?"
			//!    and the gizmo's own refusal message has nothing to name.
			//!    Documented rather than fixed, deliberately -- this is not a
			//!    production path, and the fix belongs with productionising it.
			//!    A THIRD 3b CONSEQUENCE ON THE SAME PATH, this one FIXED rather
			//!    than documented: `Scene::CreateSnapshot` clones every manager
			//!    item BY NAME, so a `csg_object`'s operands are cloned once on
			//!    their own account as well as underneath the composite's clone.
			//!    Once being an operand became a CONSUMPTION COUNT (87 step 3b)
			//!    rather than a cleared visibility flag, and since
			//!    `Object::CopySnapshotStateInto` deliberately does not copy that
			//!    count, the standalone clone came out WORLD-VISIBLE -- a second,
			//!    renderable copy of a shape that has no standalone existence
			//!    (zero such clones before 3b, one per operand after).  That
			//!    helper now copies the COMPOSED `IsWorldVisible()` rather than
			//!    the base flag, which restores the pre-3b outcome exactly; see
			//!    the comment at the assignment.  Restoring the link map remains
			//!    the outstanding item above.
			//!  - RemoveItem + a re-add under the SAME name leaves the ex-
			//!    children rooted: the removal retires their links by design,
			//!    and the re-add cannot know they were there.
			std::map<String,String> parentByName;

			//! 87 step 3: where a SYNTHESIZED entry came from.
			//! `entry name -> (instancing chunk name, source node name)`.
			//!
			//! Populated by the derive whenever an object is produced by an
			//! expansion rather than by a chunk of its own name -- i.e. by a
			//! `standard_object` carrying `source`: the collapse case (step 3a,
			//! entry name == instancing chunk name), each subtree clone `I.X`
			//! (step 3b) and each repetition `I[i,j]` / `I[i,j].X` (step 3c).
			//! The `sourceNode` field is always populated now that step 3d has
			//! deleted the one expansion that had no source OBJECT.
			//!
			//! This is the ONLY sanctioned way to get from a rendered entry
			//! back to the chunk an author can edit.  Every consumer does a MAP
			//! LOOKUP -- nobody may split the name on `.` or probe for a `[`,
			//! because a synthesized name is an opaque token whose spelling is
			//! the expansion's business and nobody else's.
			//!
			//! Lives and dies with the manager, exactly like `parentByName`
			//! above, and is retired by the same two hooks (`RemoveItem` for one
			//! entry, `Shutdown` for the lot).
			std::map<String, std::pair<String,String> > provenanceByName;

			//! Did the LAST walk compose anything against a parent LINK?  (Not
			//! "against a non-identity matrix" -- a link whose parent happens
			//! to be identity still counts.  Conservative in the safe
			//! direction: it can only cause an extra walk, never skip a needed
			//! one.)  This is what makes the "no links -> no
			//! work" fast path SOUND.  Without it, un-parenting the last child
			//! (a detach, or removing its parent) would empty `parentByName`
			//! and the fast path would then skip the one walk that still had to
			//! run -- leaving the ex-child rendering at its old composed pose
			//! forever.
			mutable bool anyComposedAgainstParent;

			//! Names whose recorded parent has since disappeared from the
			//! manager.  Warned about once each (a dangling link is treated as
			//! "root"), so a per-frame compose cannot turn one authoring
			//! mistake into a log flood.
			mutable std::set<String> danglingParentWarned;

			//! One-shot latch for RebakeHierarchy's "composed N of M" diagnostic.
			//! A dedicated flag rather than a sentinel key in the set above: that
			//! set is keyed by OBJECT NAME, so a sentinel shares a keyspace with
			//! real names -- an object actually so named would silence the
			//! diagnostic, and RemoveItem's erase-by-name would re-arm it.
			mutable bool rebakeIncompleteWarned;

			// Shadow cache: per-slot last occluder, padded to avoid false sharing.
			// Each thread hashes into a slot; collisions are harmless (just a stale hint).
			static const unsigned int kShadowCacheSlots = 64;
			struct ShadowCacheSlot {
				const IObjectPriv* pOccluder;
				char pad[64 - sizeof(const IObjectPriv*)];
			};
			mutable ShadowCacheSlot* shadowCache;

			//! THE WORLD-AABB SNAPSHOT the cross-object proximity query
			//! scans (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2).
			//!
			//! WHY IT IS NEW WORK RATHER THAN A FREE BYPRODUCT of the TLAS.
			//! World AABBs are computed INSIDE CreateBVH() / CreateOctree(),
			//! which run only when `items.size() > nMaxObjectsPerNode`, so a
			//! four-object scene never builds one -- and `Object::
			//! getBoundingBox()` transforms eight corners through the world
			//! matrix on EVERY call (tens of nanoseconds plus a virtual), so
			//! a naive per-query scan of a 405-object scene is 20-40 us, not
			//! ~1 us.  At tens of millions of hits per frame that is the
			//! difference between a few percent and a multiple.
			//!
			//! IMMUTABLE ONCE PUBLISHED, which is the whole thread-safety
			//! argument: it is built whole, published behind ONE pointer
			//! exactly as `pBVH` is, never grown and never edited in place,
			//! and released at the same call sites as `pBVH` (there under
			//! `treeCreationMutex`, which `pBVH` does not need) -- so a query copies the
			//! pointer once and reads lock-free, with no reallocation
			//! hazard.
			//!
			//! WHAT IT DOES ADD, said plainly rather than waved away as "no
			//! race class `pBVH` does not already have" -- which is FALSE
			//! for a small scene.  `pBVH` is built only when
			//! `items.size() > nMaxObjectsPerNode` (4); at or below that
			//! threshold there is no TLAS at all, so the manager's "never
			//! invalidate during a pass" contract has never had anything to
			//! protect there.  THIS snapshot IS built for a four-object
			//! scene, and `InvalidateSpatialStructure` deletes it at the
			//! same call site where it releases `pBVH` (under
			//! `treeCreationMutex`, unlike `pBVH`, because EnsureBoxSnapshot
			//! retires and republishes inside that lock).  So the
			//! honest statement is: the snapshot EXTENDS the existing
			//! "never during a pass" contract to scenes small enough that
			//! it used to be vacuous.  It is the same contract, newly
			//! load-bearing on a population that had been exempt.
			//!
			//! The pointer is `std::atomic` for the publish/read edge that
			//! IS in scope: `EnsureBoxSnapshot` fills a fresh snapshot and
			//! then stores it, and a query on another thread loads it and
			//! dereferences immediately.  A plain pointer makes that a data
			//! race by the language's definition and leaves the store free
			//! to be seen before the writes that filled the vector; the
			//! release store / acquire load pair is what makes "published
			//! whole" true at the memory model rather than only in prose.
			//! Relaxed would not do: it orders nothing.
			//!
			//! Holds EVERY registered object, world-invisible ones
			//! included; the query filters.  Filtering at build time would
			//! be faster and is deliberately not done: visibility is
			//! immutable within a pass but this snapshot can outlive one
			//! (nothing drops it unless the spatial structure is
			//! invalidated), and a stale visibility filter baked into it
			//! would be invisible.
			struct ObjectBoxSnapshot
			{
				struct Entry
				{
					BoundingBox			box;
					const IObjectPriv*	pObj;
				};
				std::vector<Entry>	entries;
			};
			mutable std::atomic<const ObjectBoxSnapshot*> pBoxes;

			//! SUPERSEDED snapshots, kept alive until the next real
			//! invalidate.
			//!
			//! `EnsureBoxSnapshot` rebuilds when the object COUNT has moved
			//! under it (see its own comment for why that case exists at
			//! all), and it can be reached from `IntersectRay` -- i.e.
			//! potentially while other render threads are mid-scan of the
			//! snapshot it is replacing.  DELETING the old one there would
			//! be a use-after-free, and a strictly worse hazard than the
			//! staleness it is fixing.  So a replaced snapshot is RETIRED
			//! rather than freed: the new pointer is published, the old
			//! object stays valid for anyone still reading it, and the
			//! whole retired set is freed in `InvalidateSpatialStructure`
			//! and the destructor -- both of which already carry the "never
			//! during a pass" contract that makes freeing safe.
			//!
			//! Bounded by the number of structural mutations between two
			//! invalidates, which is small: every Job-level edit that MOVES
			//! anything invalidates, and object adds -- the case this
			//! exists for -- come in batches at scene build.
			mutable std::vector<const ObjectBoxSnapshot*> retiredBoxes;

			//! Builds `pBoxes` if it is null OR its entry count no longer
			//! matches the manager's.  Mutex-serialized and double-checked,
			//! exactly like CreateBVH().
			void EnsureBoxSnapshot() const;

			//! Print the design's promised once-per-refusing-object line for
			//! `obj`, naming the scene chunk and its geometry kind.  Called
			//! only after `IObject::NoteDistanceRefusal()` has said this
			//! call won the object's latch, so the O(n) reverse name lookup
			//! it does runs at most once per object for the life of that
			//! object -- never on the hot path.
			void LogDistanceRefusal( const IObjectPriv* obj ) const;

			//! ONE CANDIDATE's half of `NearestOtherSurface`: the exclusion
			//! rules (self by identity, world-invisible, emitters), the call
			//! into the object's own distance, and the once-per-object
			//! refusal diagnostic with its unbounded-radius confirm.
			//!
			//! It exists so the FLAT SCAN and the TLAS POINT QUERY cannot
			//! drift: those two differ only in WHICH objects they hand to
			//! this function, and every rule the design states about which
			//! neighbours count lives here, once.
			//!
			//! `budget` is the caller's running best, passed straight through
			//! as the geometry's search radius so a close neighbour found
			//! early still prunes the expensive families.
			//! \return the candidate's world distance, or `RISE_INFINITY`
			//!         when it does not count or could not answer -- a value
			//!         that can never beat any admissible `budget`.
			Scalar ProximityCandidateDistance(
				const IObjectPriv* obj,
				const IObject* self,
				const Point3& ptWorld,
				const Scalar budget ) const;

			// Realize all objects' deferred geometry (idempotent) before any bbox/
			// TLAS query.  Called from PrepareForRendering AND CreateBVH/CreateOctree.
			void RealizeAllObjects() const;
			void CreateBVH() const;
			void CreateOctree() const;

		public:
			ObjectManager(
				const bool bUseBSPtree,
				const bool bUseOctree,
				const unsigned int nMaxObjectsPerNode,
				const unsigned int nMaxTreeDepth
				);

			void IntersectRay(
				RayIntersection& ri,
				const bool bHitFrontFaces,
				const bool bHitBackFaces,
				const bool bComputeExitInfo
				) const;

			bool IntersectShadowRay(
				const Ray& ray,
				const Scalar dHowFar,
				const bool bHitFrontFaces,
				const bool bHitBackFaces
				) const;

			//! See IObjectManager::IntersectOcclusionRay's contract comment.
			//! Same three-path traversal (BVH / octree / linear) as
			//! IntersectShadowRay, but filtered on IsWorldVisible() only --
			//! no DoesCastShadows() gate, and no shadow cache (a separate,
			//! smaller-volume query; not worth a second cache).
			bool IntersectOcclusionRay(
				const Ray& ray,
				const Scalar dHowFar,
				const bool bHitFrontFaces,
				const bool bHitBackFaces
				) const;

			//! See IObjectManager::NearestOtherSurface's contract comment.
			bool NearestOtherSurface(
				const Point3& ptWorld,
				const IObject* self,
				const Scalar maxDistWorld,
				Scalar& outDist
				) const;

			//! TEST-ONLY: has the proximity AABB snapshot been built?
			//!
			//! Exists for the demand-gate test, which has no other way to
			//! observe that a scene with no live `proximity()` consumer
			//! never pays for the snapshot -- the gate is a NEGATIVE
			//! (work not done), and a timing assertion would be a flake.
			//! Reads the published pointer, nothing else.
			bool ForTest_HasBoxSnapshot() const
			{
				return pBoxes.load( std::memory_order_acquire ) != 0;
			}

			void EnumerateObjects( IEnumCallback<IObject>& pFunc ) const;
			void EnumerateObjects( IEnumCallback<IObjectPriv>& pFunc ) const;

			// From TreeElementProcessor
			typedef const IObjectPriv*	MYOBJ;
				void RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				void RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const;
				bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const;
				BoundingBox GetElementBoundingBox( const MYOBJ elem ) const;
				bool ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const;
				char WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const;

			void SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const;
			void DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const;

			//! Tells all the objects to reset any runtime data
			void ResetRuntimeData() const;

			bool SetObjectParent( const char* child, const char* parent );
			const char* GetObjectParent( const char* child ) const;
			bool HasChildren( const char* parent ) const;
			bool SetObjectProvenance( const char* entry, const char* instancingChunk, const char* sourceNode );
			bool GetObjectProvenance( const char* entry, const char** outInstancingChunk, const char** outSourceNode ) const;
			bool ComposeWorldTransforms() const;

			//! The PER-FRAME half of composition: re-bake ONLY the nodes that are
			//! part of a hierarchy, and leave every unparented, childless object
			//! alone.
			//!
			//! ComposeWorldTransforms walks EVERY registered object, which is right
			//! for the structural calls (a derive, a detach, a removal -- where an
			//! object that just LEFT the graph still has to be put back on identity)
			//! and wrong once per render pass: a single `parent` line anywhere made
			//! a 16k-object scene re-finalize all 16k every pass, measured at
			//! +3.5 ms/pass, with nothing amortizing it -- the TLAS rebuild that was
			//! supposed to dwarf it is exactly what does NOT run when the walk
			//! reports no change.
			//!
			//! Sized by the number of LINKS, not the number of objects: one link in
			//! a 16k scene touches two nodes.  Skipping the rest is not an
			//! approximation -- an unparented, childless object composes against
			//! identity, which is what its own last finalize already used, so the
			//! full walk's visit to it is a no-op by construction (its before/after
			//! compare never fires).
			//!
			//! DEPENDS on every structural site continuing to call
			//! ComposeWorldTransforms: a detach removes the ex-child from the link
			//! map, so this walk can no longer see it, and it is the full walk that
			//! puts it back on identity.
			bool RebakeHierarchy() const;

			//! Removing an object also retires its place in the authored graph:
			//! its own parent link goes, and any object that named it as parent
			//! is RE-ROOTED.  Doing it here rather than leaving the link to
			//! dangle keeps `parentByName` an exact description of the live
			//! object set -- which is what lets ComposeWorldTransforms treat a
			//! dangling link as a genuine anomaly worth a warning.
			bool RemoveItem( const char* szName );

			//! The authored graph dies with the objects.  RemoveItem retires
			//! links one at a time; this is the same rule for the wholesale
			//! clear, and it is the ONE manager-lifecycle hook RemoveItem's
			//! retirement does not otherwise cover.  Unreachable as a bug
			//! today -- the sole caller (Job::SetPrimaryAcceleration) releases
			//! the manager on the next line -- but a future "clear the scene in
			//! place" that reused the manager would otherwise resurrect stale
			//! parenting the moment a removed name was re-added.
			void Shutdown();

			void PrepareForRendering() const;
			void InvalidateSpatialStructure() const;
			unsigned long long GetSpatialStructureGeneration() const { return mSpatialGen; }
		};
	}
}

#endif
