//////////////////////////////////////////////////////////////////////
//
//  ObjectManager.cpp - Implementation of the ObjectManager class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 16, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ObjectManager.h"
#include "../Utilities/ExpressionMemo.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Log/Log.h"
#include "../Utilities/Profiling.h"
#include "../Objects/CSGObject.h"   // telling a CSG operand from a container node (both are hidden)
#include "../Interfaces/ISurfaceSignalProvider.h"	// ProximityDemand: the snapshot's cost gate
#include <atomic>
#include <typeinfo>	// LogDistanceRefusal names the refusing geometry's kind
#include <cstdint>
#include <vector>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

namespace {

// Process-wide monotonic source for spatial-structure generation values.
// Each ObjectManager seeds its generation here at construction and draws
// every InvalidateSpatialStructure advance from the same counter, so a
// generation value is NEVER repeated across manager instances.  This is
// what makes (Scene*, generation) a safe identity key for consumers that
// cache scene-derived state across whole-scene rebuilds (the interactive
// depth view's extent cache): Job::ClearAll can hand back a recycled
// Scene address whose fresh manager would otherwise restart at 0 and
// falsely validate the stale cache.  The IObjectManager contract
// ("advanced every time InvalidateSpatialStructure() runs", equality
// stable across non-spatial edits) is preserved -- consumers compare for
// equality/inequality, never arithmetic.
//! A hidden node is either a 87 CONTAINER (a pure transform: no geometry, no
//! operands) or a CSG OPERAND (hidden by its composite).  Only the first is a
//! scene-graph node.  A CSGObject also has null geometry -- its shape comes
//! from its operands, and nested CSG makes an inner composite a legitimate
//! operand -- so "no geometry" alone does not identify a container.
bool IsContainerNode_( const IObjectPriv* obj )
{
	if( !obj || obj->GetGeometry() ) return false;
	return dynamic_cast<const Implementation::CSGObject*>( obj ) == 0;
}

unsigned long long NextSpatialGeneration()
{
	static std::atomic<unsigned long long> sCounter{ 0 };
	return sCounter.fetch_add( 1, std::memory_order_relaxed ) + 1;
}

//! GEOMETRY-PRESENCE sibling of ObjectManager's own TreeElementProcessor
//! implementation.  Forwards every method to the real ObjectManager
//! UNCHANGED except RayElementIntersection_IntersectionOnly, which drops
//! the DoesCastShadows() gate that ObjectManager::RayElementIntersection_
//! IntersectionOnly applies -- see IObjectManager::IntersectOcclusionRay's
//! contract comment (Interfaces/IObjectManager.h) for why.
//!
//! This lets ObjectManager::IntersectOcclusionRay traverse the SAME
//! already-built top-level BVH/octree the shadow-ray path uses, via the
//! `epOverride` parameter added to BVH<>::IntersectRay_IntersectionOnly /
//! Octree<>::IntersectRay_IntersectionOnly, rather than building a second
//! tree or duplicating either traversal -- the shadow-ray hot loop (NEE)
//! is untouched.
//!
//! Constructed on the stack per call and handed to the tree by reference,
//! never addref'd/stored: BVH<>'s `ep` / Octree<>'s `ep` are plain C++
//! references, not Reference<T>, and the traversal never calls
//! addref/release/refcount on them (confirmed by inspection of BVH<>'s
//! and Octree<>'s constructors).  Those three IReference methods below
//! are therefore unreachable no-op stubs, present only because
//! TreeElementProcessor<T> inherits IReference.
class OcclusionElementProcessor : public RISE::TreeElementProcessor<const IObjectPriv*>
{
public:
	explicit OcclusionElementProcessor( const ObjectManager& mgr_ ) : mgr( mgr_ ) {}

	void RayElementIntersection( RayIntersectionGeometric& ri, const IObjectPriv* elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override
	{
		mgr.RayElementIntersection( ri, elem, bHitFrontFaces, bHitBackFaces );
	}

	void RayElementIntersection( RayIntersection& ri, const IObjectPriv* elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const override
	{
		mgr.RayElementIntersection( ri, elem, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	}

	//! The ONE line that differs from ObjectManager::RayElementIntersection_
	//! IntersectionOnly: IsWorldVisible() only, no DoesCastShadows() gate.
	bool RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const IObjectPriv* elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const override
	{
		if( elem->IsWorldVisible() ) {
			return elem->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
		}
		return false;
	}

	BoundingBox GetElementBoundingBox( const IObjectPriv* elem ) const override
	{
		return mgr.GetElementBoundingBox( elem );
	}

	bool ElementBoxIntersection( const IObjectPriv* elem, const BoundingBox& bbox ) const override
	{
		return mgr.ElementBoxIntersection( elem, bbox );
	}

	char WhichSideofPlaneIsElement( const IObjectPriv* elem, const Plane& plane ) const override
	{
		return mgr.WhichSideofPlaneIsElement( elem, plane );
	}

	void SerializeElement( IWriteBuffer& buffer, const IObjectPriv* elem ) const override
	{
		mgr.SerializeElement( buffer, elem );
	}

	void DeserializeElement( IReadBuffer& buffer, const IObjectPriv*& elem ) const override
	{
		mgr.DeserializeElement( buffer, elem );
	}

	void addref() const override {}
	bool release() const override { return false; }
	unsigned int refcount() const override { return 1; }

private:
	const ObjectManager& mgr;
};

}

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// MYOBJ specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

bool ObjectManager::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
{
	return bbox.DoIntersect( elem->getBoundingBox() );
}

BoundingBox ObjectManager::GetElementBoundingBox( const MYOBJ elem ) const
{
	return elem->getBoundingBox();
}

char ObjectManager::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, elem->getBoundingBox() );
}

void ObjectManager::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RISE_PROFILE_INC(nObjectIntersectionTests);
	if( !elem->IsWorldVisible() ) {
		return;
	}

	// Native closest-hit semantics — required by the BVH<> leaf
	// intersection contract.  The BVH calls this with a SHARED `ri`
	// across all leaf primitives, expecting the processor to only
	// overwrite when its hit is strictly closer than the running
	// ri.range.  Without this guard, the LAST-tested object in a
	// leaf wins regardless of distance, which manifests as missing
	// hits / wrong colours after the TLAS migration.  (BSPTreeSAH
	// did the local-myRI dance externally in its node code, so the
	// processor itself didn't have to be defensive — see the BVH
	// retrospective Tier A cleanup §2 for the analogous fix on
	// TriangleMeshGeometryIndexed::RayElementIntersection.)
	//
	// Uses a local RayIntersection so the IObject's own ray-into-
	// object-space transform doesn't pollute the shared ri.geometric
	// (Object::IntersectRay mutates ri.geometric.ray for the duration
	// of the call and restores at exit, but the call may early-return
	// without restore on the box-prehit miss path).
	RayIntersection myRI( ri.geometric.ray, ri.geometric.rast );
	// Per-cast inputs (glossyFilterWidth, wire-edge request) must survive
	// the fresh-record dance -- see PropagateCastInputs' doc.
	myRI.geometric.PropagateCastInputs( ri.geometric );
	elem->IntersectRay( myRI, ri.geometric.range, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	if( myRI.geometric.bHit && myRI.geometric.range < ri.geometric.range ) {
		ri = myRI;
		RISE_PROFILE_INC(nObjectIntersectionHits);
	}
}

void ObjectManager::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !elem->IsWorldVisible() ) {
		return;
	}

	// Same closest-hit semantics as the full overload above.
	RayIntersection myRI( ri.ray, ri.rast );
	myRI.geometric.range = ri.range;
	myRI.geometric.PropagateCastInputs( ri );
	elem->IntersectRay( myRI, ri.range, bHitFrontFaces, bHitBackFaces, false );
	if( myRI.geometric.bHit && myRI.geometric.range < ri.range ) {
		ri = myRI.geometric;
	}
}

bool ObjectManager::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( elem->IsWorldVisible() && elem->DoesCastShadows() ) {
		return elem->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	}
	return false;
}

void ObjectManager::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
{
}

void ObjectManager::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
{
}



ObjectManager::ObjectManager(
			const bool bUseBSPtree_,
			const bool bUseOctree_,
			const unsigned int nMaxObjectsPerNode_,
			const unsigned int nMaxTreeDepth_
			) :
  pBVH( 0 ),
  pOctree( 0 ),
  mSpatialGen( NextSpatialGeneration() ),
  bUseBSPtree( bUseBSPtree_ ),
  bUseOctree( bUseOctree_ ),
  nMaxObjectsPerNode( nMaxObjectsPerNode_ ),
  nMaxTreeDepth( nMaxTreeDepth_ ),
  anyComposedAgainstParent( false ),
  rebakeIncompleteWarned( false ),
  shadowCache( new ShadowCacheSlot[kShadowCacheSlots]() ),
  pBoxes( 0 )
{
	if( bUseBSPtree && bUseOctree ) {
		GlobalLog()->PrintEasyWarning( "ObjectManager::ObjectManager:: Can't use both Octrees and BVH at the same time!" );
		bUseOctree = false;
	}

	if( bUseBSPtree ) {
		GlobalLog()->PrintEasyInfo( "ObjectManager is configured to use top-level BVH (SAH BVH4) for spatial partitioning" );
	} else if( bUseOctree ) {
		GlobalLog()->PrintEasyInfo( "ObjectManager is configured to use Octrees for spatial partioning" );
	} else {
		GlobalLog()->PrintEasyInfo( "ObjectManager is not configured for any type of spatial acceleration" );
	}
}

ObjectManager::~ObjectManager( )
{
	// NO MUTEX HERE, unlike InvalidateSpatialStructure's identical cleanup of
	// `retiredBoxes` -- and that is not an oversight, it is the destructor
	// contract: by the time this body runs the object is being torn down,
	// which requires every other party (every render thread, every caller
	// that might reach EnsureBoxSnapshot) to already be done with it. A
	// concurrent `EnsureBoxSnapshot().push_back()` racing a destructor is a
	// use-after-free of the ObjectManager ITSELF, independent of whether
	// this loop takes `treeCreationMutex` -- locking here would silence
	// nothing, and would risk locking a mutex mid-destruction that a
	// stalled other thread still holds. `pBVH`/`pOctree`/`pBoxes` right
	// above follow the same convention for the same reason.
	{
		// `pBVH` is now `std::atomic<T*>`, which `safe_release`'s `T*&`
		// parameter cannot bind to directly -- relaxed load/store here
		// matches the "no mutex, nothing else can be racing" contract this
		// destructor already states for `pOctree` / `pBoxes` beside it.
		BVH<const IObjectPriv*>* localBVH = pBVH.load( std::memory_order_relaxed );
		safe_release( localBVH );
		pBVH.store( localBVH, std::memory_order_relaxed );
	}
	safe_release( pOctree );
	delete pBoxes.load( std::memory_order_relaxed );
	pBoxes.store( 0, std::memory_order_relaxed );
	for( std::size_t k = 0; k < retiredBoxes.size(); ++k ) {
		delete retiredBoxes[k];
	}
	retiredBoxes.clear();
	delete [] shadowCache;
}

void ObjectManager::RealizeAllObjects() const
{
	// Realize deferred geometry (e.g. DisplacedGeometry's mesh bake) BEFORE any
	// bbox / acceleration-structure query.  Called from PrepareForRendering AND
	// from CreateBVH/CreateOctree, so even the lazy IntersectRay build path --
	// which bypasses PrepareForRendering -- builds from realized, non-zero
	// bounds.  Object::Realize() is const, idempotent, and mutex-serialized for
	// the deferred geometries, so repeated/concurrent calls are safe no-ops.
	GenericManager<IObjectPriv>::ItemListType::const_iterator i, e;
	for( i=items.begin(), e=items.end(); i!=e; ++i ) {
		i->second.first->Realize();
	}
}

void ObjectManager::CreateBVH() const
{
	treeCreationMutex.lock();

	// Check again if we need to create it.  Relaxed: `treeCreationMutex`
	// already serializes every WRITER against every other writer (this
	// function is the only one that stores a non-null `pBVH`), so this
	// re-check only needs to see this thread's own prior work, never a
	// concurrent one's -- the ordering that matters for UNLOCKED readers is
	// the release store below, not this guard.
	if( pBVH.load( std::memory_order_relaxed ) ) {
		treeCreationMutex.unlock();
		return;
	}

	// Realize deferred geometry first, so the BVH is built from real bounds
	// even on the lazy IntersectRay path that skipped PrepareForRendering.
	RealizeAllObjects();

	// Construct the overall bounding box
	BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

	std::vector<MYOBJ> elements;

	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			bbox.Include( i->second.first->getBoundingBox() );
			elements.push_back( i->second.first );
		}
	}

	// Top-level AccelerationConfig.  Each leaf "primitive" here is a
	// whole IObject, and a leaf intersection means descending into the
	// per-mesh BVH (or evaluating an analytic primitive).  That's much
	// more expensive than a single triangle test, so we bias the SAH
	// toward more aggressive splitting via a higher intersection cost.
	// 8.0 is conservative — it produces small leaves (~1–4 objects)
	// without tipping the build into pathological deep splits on
	// scenes whose object AABBs overlap heavily (Sponza-class).
	AccelerationConfig cfg{};
	cfg.maxLeafSize          = nMaxObjectsPerNode;
	cfg.binCount             = 32;
	cfg.sahTraversalCost     = 1.0;
	cfg.sahIntersectionCost  = 8.0;
	cfg.doubleSided          = true;

	BVH<MYOBJ>* newpBVH = new BVH<MYOBJ>( *this, elements, bbox, cfg );
	GlobalLog()->PrintNew( newpBVH, __FILE__, __LINE__, "top-level bvh" );
	// RELEASE: publishes the fully-built `BVH` -- every write the
	// constructor above made -- before the pointer becomes visible to an
	// unlocked reader's `memory_order_acquire` load.  This is the half of
	// the double-checked-locking fix that actually matters; the relaxed
	// re-check above only protects this function against itself.
	pBVH.store( newpBVH, std::memory_order_release );

	treeCreationMutex.unlock();
}

void ObjectManager::CreateOctree() const
{
	treeCreationMutex.lock();

	// Check again if we need to create it
	if( pOctree ) {
		treeCreationMutex.unlock();
		return;
	}

	// Realize deferred geometry first (see CreateBVH).
	RealizeAllObjects();

	// Construct the overall bounding box
	BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

	std::vector<MYOBJ> elements;

	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			bbox.Include( i->second.first->getBoundingBox() );
			elements.push_back( i->second.first );
		}
	}

	Octree<const IObjectPriv*>* newpOctree = new Octree<MYOBJ>( *this, bbox, nMaxObjectsPerNode );
	GlobalLog()->PrintNew( newpOctree, __FILE__, __LINE__, "octree" );
	newpOctree->AddElements( elements, nMaxTreeDepth );
	pOctree = newpOctree;

	treeCreationMutex.unlock();
}

void ObjectManager::EnsureBoxSnapshot() const
{
	// A COUNT CHECK BESIDE THE NULL CHECK, and it closes a hazard this
	// snapshot has that `pBVH` does not.
	//
	// The manager's contract is "InvalidateSpatialStructure, then
	// PrepareForRendering, after any structural change", and `pBVH` relies
	// on it entirely.  But `pBVH` is only BUILT when
	// `items.size() > nMaxObjectsPerNode`; below that threshold
	// IntersectRay walks `items` LIVE, so the contract has never actually
	// been load-bearing for a small scene.  This snapshot IS built there --
	// it has to be, since a four-object scene needs its boxes as much as a
	// four-hundred-object one -- which would make a small scene the first
	// place a missed invalidate produces a wrong answer rather than a
	// stale one: an object added after Prepare would RENDER (the linear
	// loop sees it) and be INVISIBLE to every proximity query (the
	// snapshot would not).
	//
	// So the entry count is compared as well.  IT EXISTS FOR ADDS, and
	// that is the honest scope: `Job::AddObject` on a scene of four or
	// fewer objects neither builds a TLAS nor invalidates anything, so
	// without this check the added object would render and be invisible to
	// every proximity query.  A REMOVAL is caught only incidentally -- in
	// this tree every removal path calls `InvalidateSpatialStructure`,
	// which drops the snapshot outright and makes the count check
	// redundant there.  It does NOT catch an add and a removal in the same
	// gap (the count is unchanged), and it does not pretend to; for that,
	// and for anything that MOVES an object, the invalidate contract is
	// still the mechanism.  One `size()` compare on a std::map is O(1) and
	// sits beside a null check that was already there.
	{
		const ObjectBoxSnapshot* const cur = pBoxes.load( std::memory_order_acquire );
		if( cur && cur->entries.size() == items.size() ) {
			return;
		}
	}

	treeCreationMutex.lock();
	{
		const ObjectBoxSnapshot* const cur = pBoxes.load( std::memory_order_acquire );
		if( cur && cur->entries.size() == items.size() ) {
			treeCreationMutex.unlock();
			return;
		}
	}
	// A stale-by-count snapshot is RETIRED, never deleted here.  This
	// function is reachable from IntersectRay, so another render thread may
	// be mid-scan of the very object being replaced -- freeing it would be
	// a use-after-free, and a strictly worse hazard than the staleness the
	// count check exists to fix.  The retired set is freed in
	// InvalidateSpatialStructure and the destructor, which already carry
	// the "never during a pass" contract that makes freeing safe.  See the
	// field's doc comment in ObjectManager.h.
	//
	// WHAT RETIRING DOES *NOT* PROTECT, stated so nobody reads more into it
	// than it carries: it keeps the ARRAY alive, not the objects the array
	// points at.  The entries hold raw `const IObjectPriv*`, un-addrefed, so
	// a snapshot that outlives a removed object still names it.  In this
	// tree that gap is closed by the removal paths themselves, every one of
	// which calls InvalidateSpatialStructure -- which frees the retired set
	// too, under the same "never during a pass" contract.  Retiring buys
	// safety for the ADD case above, where nothing invalidates.
	// Realize deferred geometry first, exactly as CreateBVH does: an
	// unrealized DisplacedGeometry reports a ZERO bounding box, and a
	// snapshot built from it would exclude that object from every proximity
	// query for the life of the snapshot.
	RealizeAllObjects();

	ObjectBoxSnapshot* snap = new ObjectBoxSnapshot();
	snap->entries.reserve( items.size() );

	// EVERY registered object, world-invisible ones included -- the query
	// applies the visibility and emitter filters, so nothing about a
	// filtering decision is baked into a structure that can outlive the
	// pass that built it.  `getBoundingBox()` is the expensive call this
	// whole snapshot exists to hoist out of the per-query loop.
	GenericManager<IObjectPriv>::ItemListType::const_iterator i, e;
	for( i = items.begin(), e = items.end(); i != e; ++i ) {
		ObjectBoxSnapshot::Entry entry;
		entry.pObj = i->second.first;
		entry.box  = entry.pObj->getBoundingBox();
		snap->entries.push_back( entry );
	}

	// PUBLISHED WHOLE, behind one pointer, and never touched again -- see
	// the field's doc comment in ObjectManager.h for why that is the entire
	// thread-safety argument.
	//
	// THE SWAP HAPPENS LAST, after the new snapshot is complete, and the
	// old one is only RETIRED (not freed) here.  Both halves of that
	// matter: clearing `pBoxes` before the build would leave a concurrent
	// reader holding null for the duration and reading its neutral -- a
	// transient under-paint for no reason -- and freeing the old object
	// would be a use-after-free for a reader still scanning it.  So the
	// old snapshot stays published and valid right up to this single
	// store, and stays alive past it.
	//
	// The store is a RELEASE and every read is an ACQUIRE, which is what
	// makes "published whole" true at the memory model and not merely in
	// this comment: without the pair, a reader is free to observe the
	// pointer before the vector writes that filled it.
	{
		const ObjectBoxSnapshot* const old = pBoxes.load( std::memory_order_relaxed );
		if( old ) {
			retiredBoxes.push_back( old );
		}
	}
	pBoxes.store( snap, std::memory_order_release );

	treeCreationMutex.unlock();
}

void ObjectManager::LogDistanceRefusal( const IObjectPriv* obj ) const
{
	if( !obj ) {
		return;
	}

	// THE REVERSE NAME LOOKUP.  O(n) over the item map, and affordable
	// exactly because the caller has already won the object's one-shot
	// latch: this runs at most once per object, ever, and never again on
	// the per-candidate path.  The manager owns this map, which is why the
	// printing lives here and the latch lives on the object.
	const char* name = "(unnamed)";
	GenericManager<IObjectPriv>::ItemListType::const_iterator i, e;
	for( i = items.begin(), e = items.end(); i != e; ++i ) {
		if( i->second.first == obj ) {
			name = i->first.c_str();
			break;
		}
	}

	// AND THE GEOMETRY KIND, because "which chunk" and "why" are different
	// questions and an author needs both.  `typeid(...).name()` is a
	// mangled string on this toolchain (the same form
	// PixelBasedRasterizerHelper's ForTest_SamplingKernelName already
	// prints), but the class name is legible inside it -- "SDFGeometry",
	// "BezierPatchGeometry" -- which is all a diagnostic needs.  A null
	// geometry is itself a refusal reason and is named as one.
	const IGeometry* const geom = obj->GetGeometry();
	const char* kind = geom ? typeid( *geom ).name() : "(no geometry)";

	GlobalLog()->PrintEx( eLog_Info,
		"ObjectManager::NearestOtherSurface:: object `%s` (geometry %s) could not answer a "
		"distance query at one queried point even with the search radius removed, so it "
		"contributed nothing to proximity() there and is treated as FAR (under-paints contact, "
		"never invents it).  Reported once per object.  This may be a per-point solver failure "
		"(an SDF bracket that did not close) rather than a family that never answers -- the "
		"families that never answer are listed in docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2.",
		name, kind );
}

//! See the header for the contract.  This is every rule the design states
//! about WHICH neighbours count, in one place, so the flat scan and the
//! TLAS point query cannot drift apart on any of them.
Scalar ObjectManager::ProximityCandidateDistance(
	const IObjectPriv* obj, const IObject* self,
	const Point3& ptWorld, const Scalar budget ) const
{
	// SELF, by identity.  Two INSTANCED COPIES of one geometry are
	// different objects and do count against each other, which is the
	// behaviour the design wants and a name- or geometry-keyed test
	// would get wrong.
	if( obj == self ) {
		return RISE_INFINITY;
	}
	// The same filter IntersectOcclusionRay uses -- and what excludes
	// CSG operands, which ARE registered here; only the composite is
	// world-visible.  `casts_shadows FALSE` is deliberately NOT
	// consulted: this is geometry presence, not light visibility.
	//
	// It is asked LIVE rather than trusted from the TLAS's build-time
	// membership, so an object that became invisible after the tree was
	// built stops counting immediately.  (The reverse -- one that became
	// visible -- is not in the tree at all, and is the staleness the
	// caller's comment discloses.)
	if( !obj->IsWorldVisible() ) {
		return RISE_INFINITY;
	}
	// EMITTERS never count: a light panel parked millimetres off a wall
	// must not paint grime on it.  Same predicate LuminaryManager uses
	// to decide what NEE samples, so "is a light" means one thing in
	// the engine.
	const IMaterial* const mat = obj->GetMaterial();
	if( mat && mat->GetEmitter() ) {
		return RISE_INFINITY;
	}

	Scalar d = Scalar( 0 );
	if( obj->DistanceToSurface( ptWorld, budget, d ) ) {
		return d;
	}

	// THE PROMISED LINE.  The design's 2 says a neighbour that cannot
	// answer "says so once in the log", and until Phase 1 nothing printed
	// it -- three surfaces claimed a diagnostic that did not exist.  The
	// latch lives on the object (so this is one relaxed load per candidate
	// per hit after the first refusal) and the printing lives in the
	// manager (so it can name the chunk, which an Object cannot).  That
	// promise is honest for the families that never answer (5.2); for an
	// SDF it is BEST-EFFORT ONLY -- see the confirm below, which removes
	// the radius-dependent false positives but cannot tell a genuine "this
	// family never answers" refusal apart from a one-off per-point solver
	// failure (an unclosed bracket, a stalled descent, a fabricated
	// (0,1,0) gradient at a flat blend seam).  An SDF's one-shot latch is
	// very often spent on exactly that kind of per-point failure, not on
	// family membership.
	//
	// AND IT IS CONFIRMED BEFORE IT IS PRINTED, because `false` above does
	// NOT only mean "I cannot answer".  `SDFGeometry`'s step-1 lower-bound
	// early-out (`Map(p) > maxDist` proves the true distance is past the
	// radius) also returns false, and that is a perfectly healthy outcome
	// that happens at the far corners of every SDF's expanded box.
	// Printing "this object cannot answer" there would be a false
	// statement about a geometry that answers fine.  The MESH family added
	// in Phase 2 refuses the same benign way -- nothing within the radius
	// -- so this confirm is what keeps every mesh in Sponza out of the log.
	//
	// So: take the one-shot latch first (cheap, and it bounds everything
	// below to once per object for the life of the object), then ask the
	// SAME object again with an UNBOUNDED radius.  A family with no closed
	// form -- a patch, a RAW mesh, a CSG composite, a heightfield SDF, a
	// degenerate transform -- refuses that too, immediately and in O(1).  A
	// non-heightfield SDF is NOT O(1) here: the confirm re-runs the full
	// bracket search (descent + doubling probe, ~80 `Map()` evaluations x
	// O(parts), 5.2) at unbounded radius, once per object, and can still
	// return false for a healthy field that merely hit its solver budget or
	// landed on a degenerate gradient -- which is exactly the
	// per-point-failure case the message states explicitly rather than
	// branding the object as a member of a family that never answers.  An
	// SDF that was merely out of range answers it, and says nothing.  An
	// indexed MESH is not O(1) either: the confirm re-runs
	// `BVH::ClosestPointDistance` unbounded (`maxDist = RISE_INFINITY`),
	// once per mesh, ever (the one-shot latch above).  REASONED BOUND, not
	// measured (the `RISE_PROX_FORCE` hook that would make a fresh Sponza
	// number cheap was removed with the rest of that measurement's
	// instrumentation -- see docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §8.3):
	// an earlier draft of this comment claimed the unbounded call "visits
	// the whole tree", which OVERSTATES it.  `ClosestPointDistance` starts
	// `best` at `maxDist` and prunes any subtree whose box-to-point
	// distance is `>= best`; with `best` seeded at infinity the FIRST leaf
	// primitive the nearest-first descent reaches collapses `best` to a
	// real, finite number (any primitive's true distance is finite), and
	// every node visited after that is pruned exactly as a normally-bounded
	// query would prune it.  So the unbounded confirm costs about the same
	// as one ORDINARY closest-point query against that mesh -- not a
	// full-tree scan -- unless the tree's own build quality is degenerate
	// enough that the nearest-first ordering fails to reach a good
	// candidate quickly, which is a BVH-quality question, not a property of
	// this being an unbounded call.
	//
	// WHAT THIS DELIBERATELY GIVES UP: if an object's FIRST refusal is the
	// benign far one, the latch is spent and a later bracket-budget failure
	// on the same object stays silent.  That is the right trade -- the
	// design's promise is about neighbours that CANNOT answer, and this one
	// can; the budget failure is separately disclosed as a residual (10).
	if( obj->NoteDistanceRefusal() ) {
		Scalar confirm = Scalar( 0 );
		if( !obj->DistanceToSurface( ptWorld, RISE_INFINITY, confirm ) ) {
			LogDistanceRefusal( obj );
		}
	}
	return RISE_INFINITY;
}

bool ObjectManager::NearestOtherSurface(
	const Point3& ptWorld, const IObject* self, const Scalar maxDistWorld, Scalar& outDist ) const
{
	// A non-finite point or an unusable radius is a refusal, not a
	// zero-distance answer: `proximity` reads a refusal as its neutral 0
	// (paint nothing), which is the honest answer to a nonsense query.
	if( !RISE::IsFiniteDouble( (double)ptWorld.x )
	 || !RISE::IsFiniteDouble( (double)ptWorld.y )
	 || !RISE::IsFiniteDouble( (double)ptWorld.z ) ) {
		return false;
	}
	if( !( maxDistWorld > Scalar( 0 ) ) || !RISE::IsFiniteDouble( (double)maxDistWorld ) ) {
		return false;
	}

	// UNCONDITIONAL, even though `snap` below is read ONLY by the flat-scan
	// branch further down and never by the TLAS branch (review round 1,
	// item 6d) -- on a TLAS-backed scene this call's `snap` result is
	// simply discarded a few lines later.  Kept anyway because
	// `EnsureBoxSnapshot` carries `RealizeAllObjects()` (see its own
	// comment), which this function needs on EITHER path: a query reached
	// through the lazy `IntersectRay` self-heal (no prior
	// `PrepareForRendering`) must not ask a still-unrealized
	// `DisplacedGeometry` for its distance, exactly the hazard `CreateBVH`
	// guards against for its own path.  `Realize()` is idempotent and
	// one-shot per geometry, so calling it here when the TLAS build already
	// did is a cheap no-op, not double work.
	EnsureBoxSnapshot();
	// ONE copy of the pointer, then a lock-free read of an immutable
	// object.  Re-reading `pBoxes` inside the loop would reintroduce
	// exactly the race the immutability buys away.
	const ObjectBoxSnapshot* const snap = pBoxes.load( std::memory_order_acquire );
	if( !snap ) {
		return false;
	}

	Scalar best = maxDistWorld;
	bool found = false;

	// THE CANDIDATE SOURCE, and the ONE decision this function makes.
	//
	// The TOP-LEVEL BVH, when the manager has one, is walked as a POINT
	// query: the same bounded closest-point traversal the mesh family uses
	// on its own triangles, one level up, with "the primitive's distance"
	// being the whole per-object query below.  It visits the two children
	// of each node nearest-box-first and prunes any subtree whose box is
	// already further than the running best, so a query at 4 cm touches a
	// handful of nodes instead of every object in the scene.
	//
	// The FLAT SCAN over the AABB snapshot is the fallback, and it is not
	// vestigial: the manager builds no TLAS at all for a scene of
	// `nMaxObjectsPerNode` (4) objects or fewer, nor when `bUseBSPtree` is
	// off, and those scenes -- every fixture in `ProximitySignalTest`
	// included -- still have to answer.
	//
	// WHAT CHANGES WITH THE TLAS, said plainly because it is a contract
	// shift and not only a speed-up.  The snapshot carries a COUNT CHECK
	// (see EnsureBoxSnapshot) that rebuilds it when an object was ADDED
	// without an invalidate; the TLAS has no such check, so on a
	// TLAS-backed scene a proximity query is now exactly as stale as the
	// RENDER is -- `ObjectManager::IntersectRay` walks that same tree, so
	// an object invisible to this query is equally invisible to the
	// picture.  Consistent, and strictly closer to "the signal measures the
	// scene you are looking at" than the previous state, where proximity
	// could see a neighbour the frame did not.  The case §8.1's count check
	// was actually written for -- an add on a scene of four or fewer
	// objects, where the linear IntersectRay loop WOULD render the new
	// object -- keeps the flat scan and keeps the check.  Pinned by
	// `ProximitySignalTest`'s `TestTLASStalenessViaJobAddObject` (g2):
	// a 7th object added via `Job::AddObject` on a 6-object (TLAS-backed)
	// scene is invisible to BOTH `NearestOtherSurface` and `IntersectRay`
	// until `InvalidateSpatialStructure` + a rebuild, never one before the
	// other.
	// ACQUIRE: pairs with CreateBVH's / InvalidateSpatialStructure's release
	// stores -- this is the read half of the fix, and unlike IntersectRay's
	// reader below, this one has NO self-heal (a null read here is a
	// legitimate "no TLAS at this object count" case, not a "build one
	// lazily" case).
	const BVH<const IObjectPriv*>* const tlas =
		( bUseBSPtree && items.size() > nMaxObjectsPerNode )
			? pBVH.load( std::memory_order_acquire ) : 0;

	if( tlas && tlas->numPrims() > 0 ) {
		// The traversal owns `best`; this mirror exists only to hand the
		// running budget down as each geometry's search radius, which is
		// what lets a close neighbour found early prune the expensive
		// families.  It sees every leaf evaluation in the same order, so it
		// tracks the traversal's own value exactly.
		Scalar budget = maxDistWorld;
		Scalar d = Scalar( 0 );
		const bool ok = tlas->ClosestPointDistance(
			ptWorld, maxDistWorld,
			[&]( const IObjectPriv* obj, const Point3& p ) -> Scalar {
				const Scalar dist = ProximityCandidateDistance( obj, self, p, budget );
				if( dist < budget ) budget = dist;
				return dist;
			},
			d,
			// useElementBoxTest = TRUE: a TLAS leaf's `primDist` is the WHOLE
			// per-object query above, which for a mesh object recurses into
			// that mesh's own inner BVH -- worth one object-box test to skip
			// (see BVH.h ClosestPointDistance's doc comment; measured on
			// Sponza, docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §8.3).
			true );
		if( ok ) {
			best  = d;
			found = true;
		}
	} else {
		for( std::size_t k = 0; k < snap->entries.size(); ++k ) {
			const ObjectBoxSnapshot::Entry& entry = snap->entries[k];
			const IObjectPriv* const obj = entry.pObj;

			// BOX REJECTION, against the RUNNING best rather than the
			// original radius, so a close neighbour found early prunes the
			// rest.
			//
			// ORDINARY CONTAINMENT, and it must stay ordinary: an infinite
			// plane's box is +/-RISE_INFINITY (= DBL_MAX, which
			// IsFiniteDouble reports FINITE), and under a rotation some
			// axes overflow to +/-inf.  Either way the expanded box
			// contains every point and the plane is admitted by the same
			// test as everything else.  DO NOT add a "skip non-finite
			// boxes" guard to dodge NaN -- it would make every infinite
			// plane in the scene invisible to this query.  A NaN bound
			// (reachable from an inf*0 in a rotated corner transform) makes
			// both comparisons false and so ADMITS the candidate, which is
			// the safe direction: the geometry is then asked, and answers
			// correctly or refuses.
			const Point3& ll = entry.box.ll;
			const Point3& ur = entry.box.ur;
			if( ptWorld.x < ll.x - best || ptWorld.x > ur.x + best ) continue;
			if( ptWorld.y < ll.y - best || ptWorld.y > ur.y + best ) continue;
			if( ptWorld.z < ll.z - best || ptWorld.z > ur.z + best ) continue;

			const Scalar d = ProximityCandidateDistance( obj, self, ptWorld, best );
			if( d < best ) {
				best = d;
				found = true;
				if( best <= Scalar( 0 ) ) {
					// Touching or interpenetrating: nothing can beat 0, and
					// the remaining candidates cannot change the answer.
					break;
				}
			}
		}
	}

	if( !found ) {
		return false;
	}
	outDist = best;
	return true;
}

void ObjectManager::IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RISE_PROFILE_PHASE(GeomPrimary);
	RISE_PROFILE_INC(nPrimaryRays);

	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		// ACQUIRE, re-loaded after a self-heal build: this is the DCLP
		// reader half -- see pBVH's declaration comment in ObjectManager.h.
		// The local copy (not a second bare `pBVH->...` after the guard) is
		// what keeps this call correct if a concurrent thread's
		// InvalidateSpatialStructure runs between the guard and the use;
		// it does not, on its own, extend that BVH's lifetime past a
		// concurrent release (same residual the header comment states).
		BVH<const IObjectPriv*>* localBVH = pBVH.load( std::memory_order_acquire );
		if( !localBVH ) {
			GlobalLog()->PrintEasyWarning( "ObjectManager: BVH built lazily during IntersectRay; call PrepareForRendering() before rendering" );
			CreateBVH();
			localBVH = pBVH.load( std::memory_order_acquire );
		}

		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;

		// BVH<>::IntersectRay auto-routes to BVH4 SIMD traversal when
		// the post-build collapse populated the wide nodes (the common
		// case for any non-degenerate scene).
		localBVH->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pOctree ) {
			GlobalLog()->PrintEasyWarning( "ObjectManager: Octree built lazily during IntersectRay; call PrepareForRendering() before rendering" );
			CreateOctree();
		}

		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;

		pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	} else {
		GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
		for( i=items.begin(), e=items.end(); i!=e; i++ )
		{
			if( i->second.first->IsWorldVisible() )
			{
				RayIntersection		this_ri( ri.geometric.ray, ri.geometric.rast );
				this_ri.geometric.PropagateCastInputs( ri.geometric );
				i->second.first->IntersectRay( this_ri, ri.geometric.range, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );

				if( this_ri.geometric.bHit && this_ri.geometric.range < ri.geometric.range ) {
					ri = this_ri;
				}
			}
		}
	}

	// THE CROSS-OBJECT SIGNAL STAMP (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
	// §5.1).  ONE site, after traversal, on the WINNING record -- all three
	// branches above (BVH4, octree, linear) fall through to here, and the
	// per-candidate `myRI` copy-back happens INSIDE traversal, so nothing
	// downstream can overwrite what is written here.  `pSelf` is copied from
	// `ri.pObject` rather than recomputed, so the two identities can never
	// disagree: `Object::IntersectRay` and `CSGObject::IntersectRay` both set
	// `pObject` to themselves, so a CSG hit names the COMPOSITE and an operand
	// -- never reached by the manager -- can never be `pSelf`.
	//
	// Written unconditionally, miss included: on a miss `pObject` is null and
	// `ptIntersection` is whatever the record was constructed with, and a
	// consumer of a missed record has nothing to read anyway.  Unconditional
	// keeps this off the branch-predictor and matches the way the record's own
	// per-hit signal payload is stamped (SurfaceSignalInfo's doc comment: a
	// pointer plus a few scalars is cheaper to always write than to gate).
	//
	// `signals.time` is NOT stamped here -- the manager does not know an
	// evaluating painter's time.  ExpressionPainter::BuildContext stamps it on
	// its own copy; see the field's doc comment.
	//
	// THE SNAPSHOT THE STAMP PROMISES.  Stamping `pScene` publishes this
	// manager to a painter that may call `proximity()`, so the AABB snapshot
	// that query scans has to exist by then.  PrepareForRendering builds it
	// eagerly; this covers the callers that skip PrepareForRendering
	// entirely -- the same population the lazy `CreateBVH()` above exists
	// for, plus the <=4-object linear branch, which has no lazy build of its
	// own and is exactly what a small test scene uses.  One predictable
	// not-null branch per primary ray after the first, against a traversal
	// that just walked a BVH.  (NearestOtherSurface calls it again, and that
	// belt-and-braces is deliberate: an out-of-tree caller could reach the
	// query through a hit record this function never produced.)
	//
	// GATED ON DEMAND, and that gate is the point of the call being here at
	// all being affordable.  Unconditionally this was a load, a compare and
	// a heap indirection on EVERY primary and secondary ray, charged to
	// every scene in the tree including the overwhelming majority that
	// never mention `proximity()`.  `ProximityDemand::Any()` is one relaxed
	// atomic load and answers "does any live compiled painter call the
	// builtin"; when it does not, no snapshot is needed and none is kept
	// alive.  Correctness does not rest on the counter -- with the eager
	// and the per-ray build both skipped, `NearestOtherSurface` still
	// builds lazily under the tree mutex on its first call.
	if( ProximityDemand::Any() ) {
		EnsureBoxSnapshot();
	}

	ri.geometric.signals.pScene  = this;
	ri.geometric.signals.pSelf   = ri.pObject;
	ri.geometric.signals.ptWorld = ri.geometric.ptIntersection;

	if( !ri.geometric.bHit ) {
		RISE_PROFILE_INC(nMisses);
	}
}

bool ObjectManager::IntersectShadowRay( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	RISE_PROFILE_PHASE(GeomShadow);
	RISE_PROFILE_INC(nShadowRays);

	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		// Same DCLP-correct acquire/self-heal/re-load pattern as
		// IntersectRay above; see pBVH's declaration comment.
		BVH<const IObjectPriv*>* localBVH = pBVH.load( std::memory_order_acquire );
		if( !localBVH ) {
			CreateBVH();
			localBVH = pBVH.load( std::memory_order_acquire );
		}
		return localBVH->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pOctree ) {
			CreateOctree();
		}

		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else {
		// Shadow cache: hash the stack address to pick a per-thread slot.
		// Each slot occupies its own cache line to avoid false sharing.
		int dummy;
		const unsigned int slot = (unsigned int)(reinterpret_cast<uintptr_t>(&dummy) >> 12) & (kShadowCacheSlots - 1);
		const IObjectPriv* cached = shadowCache[slot].pOccluder;

		// Re-check the cached occluder's flags before trusting it: the same
		// IsWorldVisible() && DoesCastShadows() gate the slow path applies when caching
		// (below).  An object whose casts_shadows / world-visible flag was flipped AFTER
		// being cached (e.g. a non-spatial edit that re-points it in place but does not
		// invalidate the spatial structure -- the CST stable-object apply) must NOT keep
		// occluding from a stale cache hit.
		if( cached && cached->IsWorldVisible() && cached->DoesCastShadows() ) {
			if( cached->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces ) ) {
				RISE_PROFILE_INC(nShadowCacheHits);
				return true;
			}
		}

		RISE_PROFILE_INC(nShadowCacheMisses);

		GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
		for( i=items.begin(), e=items.end(); i!=e; i++ ) {
			const IObjectPriv* obj = i->second.first;
			if( obj == cached ) {
				continue;  // Already tested above
			}
			if( obj->IsWorldVisible() && obj->DoesCastShadows() ) {
				if( obj->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces ) ) {
					shadowCache[slot].pOccluder = obj;
					return true;
				}
			}
		}

		return false;
	}
}

//! See IObjectManager::IntersectOcclusionRay's contract comment
//! (Interfaces/IObjectManager.h): geometry-presence any-hit, filtered on
//! IsWorldVisible() only -- no DoesCastShadows() gate, unlike
//! IntersectShadowRay above.
//!
//! Same three-path traversal as IntersectShadowRay (BVH / octree /
//! linear), sharing the SAME already-built tree via `OcclusionElement-
//! Processor` + the `epOverride` parameter on BVH<>:: / Octree<>::
//! IntersectRay_IntersectionOnly, so this does not build a second tree.
//! The linear-loop fallback path deliberately has NO shadow cache: AO-
//! style occlusion queries are lower call volume than NEE shadow rays and
//! a second cache (with its own staleness-on-flag-flip bookkeeping) is
//! not worth it for that traffic.
bool ObjectManager::IntersectOcclusionRay( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		// Same DCLP-correct acquire/self-heal/re-load pattern as
		// IntersectRay above; see pBVH's declaration comment.
		BVH<const IObjectPriv*>* localBVH = pBVH.load( std::memory_order_acquire );
		if( !localBVH ) {
			CreateBVH();
			localBVH = pBVH.load( std::memory_order_acquire );
		}
		const OcclusionElementProcessor occlusionEp( *this );
		return localBVH->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces, &occlusionEp );
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pOctree ) {
			CreateOctree();
		}
		const OcclusionElementProcessor occlusionEp( *this );
		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces, &occlusionEp );
	} else {
		GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
		for( i=items.begin(), e=items.end(); i!=e; i++ ) {
			const IObjectPriv* obj = i->second.first;
			if( obj->IsWorldVisible() ) {
				if( obj->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces ) ) {
					return true;
				}
			}
		}
		return false;
	}
}

void ObjectManager::EnumerateObjects( IEnumCallback<IObject>& pFunc ) const
{
	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			pFunc( *i->second.first );
		}
	}
}

void ObjectManager::EnumerateObjects( IEnumCallback<IObjectPriv>& pFunc ) const
{
	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			pFunc( *i->second.first );
		}
	}
}

void ObjectManager::ResetRuntimeData() const
{
	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		i->second.first->ResetRuntimeData();
	}
}

bool ObjectManager::SetObjectParent( const char* child, const char* parent )
{
	if( !child || !child[0] ) {
		GlobalLog()->PrintEasyError( "ObjectManager::SetObjectParent:: child name is empty" );
		return false;
	}
	const String childName( child );
	if( !GetItem( child ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"ObjectManager::SetObjectParent:: `%s` is not a registered object", child );
		return false;
	}

	// Detach.  Re-compose IMMEDIATELY, and with the FULL walk -- this is the one
	// mutation the per-frame RebakeHierarchy structurally cannot repair.  The
	// ex-child has just left `parentByName`, so the narrow walk can no longer
	// SEE it, while its `m_mxParentWorld` still holds the parent it just left:
	// it would render at its ex-parent's composed pose forever.  Measured
	// exactly that way through `IJob::SetObjectParent( name, 0 )`, whose own doc
	// advertises detach as a public operation and says nothing about owing a
	// compose afterwards.
	//
	// Doing it here rather than asking callers makes the manager
	// self-consistent: RemoveItem already composes for the same reason (its
	// orphans also leave the map), and the ADD direction needs no help because a
	// new link IS visible to the narrow walk.  A detach is a structural edit, so
	// paying one full walk for it is not a per-frame cost.
	if( !parent || !parent[0] ) {
		const bool had = ( parentByName.erase( childName ) > 0 );
		if( had && ComposeWorldTransforms() ) {
			// AND INVALIDATE.  Composing here CONSUMES the "moved" signal: the
			// caller's own ComposeObjectHierarchy now correctly answers "nothing
			// changed", and RebakeHierarchy cannot see the ex-child either, so
			// without this line NOTHING invalidates -- the object keeps a stale
			// TLAS leaf at its old world position and becomes unhittable and
			// unpickable for the rest of the session.  Measured exactly that
			// way.  RemoveItem's Job-layer caller compensates for the same
			// swallowed signal with an unconditional invalidate; this path had
			// no such compensation, so it does it itself.
			InvalidateSpatialStructure();
		}
		return true;
	}

	// A CSG OPERAND is not a scene-graph node.  CSGObject::IntersectRay
	// transforms the world ray into ITS frame and only then hands it to each
	// operand, so an operand's `m_mxFinalTrans` is CSG-LOCAL, not world.
	// Parenting TO one would place the child as if the CSG's own transform were
	// identity; parenting one would bake a world matrix into a slot the CSG
	// re-interprets as local, silently pushing the ancestor's translation
	// through the CSG's transform.  Neither is what anyone means.  An operand
	// is identifiable without a back-pointer: it is the one node that is hidden
	// yet HAS geometry (a container is hidden and has none).
	//
	// Parenting to the csg_object ITSELF is fine and is NOT refused here -- a
	// CSG composite is world-visible and its final matrix really is its world
	// transform.
	{
		const IObjectPriv* childObj = GetItem( child );
		if( childObj && !childObj->IsWorldVisible() && !IsContainerNode_( childObj ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"ObjectManager::SetObjectParent:: `%s` is a CSG operand, whose transform is interpreted in "
				"its csg_object's frame, not the world's; it cannot take a `parent`.  Parent the csg_object "
				"instead.", child );
			return false;
		}
	}

	const String parentName( parent );
	if( parentName == childName ) {
		GlobalLog()->PrintEx( eLog_Error,
			"ObjectManager::SetObjectParent:: `%s` cannot be its own parent", child );
		return false;
	}
	{
		const IObjectPriv* parentObj = GetItem( parent );
		if( parentObj && !parentObj->IsWorldVisible() && !IsContainerNode_( parentObj ) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"ObjectManager::SetObjectParent:: `%s` is a CSG operand -- its transform is interpreted in "
				"its csg_object's frame, not the world's, so anything parented to it would be placed as if "
				"that csg_object had no transform.  Parent to the csg_object instead.", parent );
			return false;
		}
	}
	if( !GetItem( parent ) ) {
		// Declare-before-use.  At parse time this IS the cycle guard: a cycle
		// needs a link back to something not yet declared, and a reference the
		// engine has not registered yet cannot resolve.  The ancestor walk
		// below is the guard for a RUNTIME reparent, where both endpoints
		// already exist.
		GlobalLog()->PrintEx( eLog_Error,
			"ObjectManager::SetObjectParent:: parent `%s` of `%s` is not a registered OBJECT "
			"-- a `parent` must name an object (not a material, painter, geometry, ...) and it must "
			"be DECLARED BEFORE the object that names it", parent, child );
		return false;
	}

	// Cycle guard: walk up from the PROPOSED parent.  If we reach the child,
	// the link would close a loop and ComposeWorldTransforms would never
	// terminate (or, with the reachability fallback, would silently drop the
	// whole loop to root).  Bounded by the link count, so a pre-existing loop
	// in the map cannot hang this walk either.
	{
		String cursor = parentName;
		size_t guard = parentByName.size() + 1;
		while( guard-- > 0 ) {
			if( cursor == childName ) {
				GlobalLog()->PrintEx( eLog_Error,
					"ObjectManager::SetObjectParent:: parenting `%s` to `%s` would close a cycle "
					"(`%s` is already a descendant of `%s`); link refused", child, parent, parent, child );
				return false;
			}
			const std::map<String,String>::const_iterator up = parentByName.find( cursor );
			if( up == parentByName.end() ) break;   // reached a root: no cycle
			cursor = up->second;
		}
	}

	parentByName[childName] = parentName;
	return true;
}

bool ObjectManager::HasChildren( const char* parent ) const
{
	if( !parent || !parent[0] ) return false;
	const String parentName( parent );
	for( std::map<String,String>::const_iterator i = parentByName.begin(); i != parentByName.end(); ++i ) {
		if( i->second == parentName ) return true;
	}
	return false;
}

bool ObjectManager::SetObjectProvenance( const char* entry, const char* instancingChunk, const char* sourceNode )
{
	if( !entry || !entry[0] ) return false;
	provenanceByName[ String( entry ) ] =
		std::make_pair( String( instancingChunk ? instancingChunk : "" ), String( sourceNode ? sourceNode : "" ) );
	return true;
}

bool ObjectManager::GetObjectProvenance( const char* entry, const char** outInstancingChunk, const char** outSourceNode ) const
{
	if( !entry || !entry[0] ) return false;
	const std::map<String, std::pair<String,String> >::const_iterator i = provenanceByName.find( String( entry ) );
	if( i == provenanceByName.end() ) return false;
	if( outInstancingChunk ) *outInstancingChunk = i->second.first.c_str();
	if( outSourceNode )      *outSourceNode      = i->second.second.c_str();
	return true;
}

void ObjectManager::Shutdown()
{
	parentByName.clear();
	provenanceByName.clear();
	danglingParentWarned.clear();
	anyComposedAgainstParent = false;
	// The latch used to live INSIDE danglingParentWarned and was cleared for
	// free; moving it out of that keyspace lost the reset, so restore it here.
	rebakeIncompleteWarned = false;
	// `pBoxes` / `retiredBoxes` name the very objects `GenericManager::Shutdown`
	// (below) is about to release -- raw `const IObjectPriv*` entries, un-
	// addrefed, exactly as the snapshot's own doc comment already says for
	// the object-removal case.  Left uncleared here they hold dangling
	// pointers into freed objects, the same lifecycle gap this function
	// exists to close for `parentByName` above; a future "clear the scene in
	// place" caller (this function's own doc comment) would resurrect stale
	// boxes the moment a name were re-added, exactly as it would resurrect
	// stale parenting without the clears above.  Guarded by the same mutex
	// EnsureBoxSnapshot/InvalidateSpatialStructure use for this state.
	treeCreationMutex.lock();
	delete pBoxes.load( std::memory_order_relaxed );
	pBoxes.store( 0, std::memory_order_relaxed );
	for( std::size_t k = 0; k < retiredBoxes.size(); ++k ) {
		delete retiredBoxes[k];
	}
	retiredBoxes.clear();
	treeCreationMutex.unlock();
	GenericManager<IObjectPriv>::Shutdown();
}

bool ObjectManager::RemoveItem( const char* szName )
{
	const bool ok = GenericManager<IObjectPriv>::RemoveItem( szName );
	if( !ok || !szName ) return ok;

	const String gone( szName );
	parentByName.erase( gone );
	provenanceByName.erase( gone );   // 87 step 3: the entry is gone, so is the record of where it came from
	danglingParentWarned.erase( gone );

	// Re-root the orphans.  Their world transforms change (they lose the
	// removed node's contribution), which is the only possible answer once the
	// parent is gone -- but say so once, because the alternative reading is
	// that they moved for no reason.
	unsigned int orphans = 0;
	for( std::map<String,String>::iterator i = parentByName.begin(); i != parentByName.end(); ) {
		if( i->second == gone ) { parentByName.erase( i++ ); ++orphans; }
		else                    { ++i; }
	}
	if( orphans ) {
		// RE-COMPOSE, do not merely re-book.  Erasing the links above changes
		// only `parentByName`; each orphan's world matrix, inverse, and all
		// three Object caches still hold the composition against the object
		// that was just deleted.  Nothing else would fix it on this path --
		// Job::RemoveObject does not compose, and the per-frame re-bake is 87
		// step 2 -- so `remove object` from the console would leave the
		// orphans rendering at their old pose indefinitely.  This makes the
		// log line below true rather than aspirational.
		ComposeWorldTransforms();
		GlobalLog()->PrintEx( eLog_Info,
			"ObjectManager::RemoveItem:: `%s` was the parent of %u object(s); they are now roots "
			"and no longer carry its transform", szName, orphans );
	}
	return ok;
}

const char* ObjectManager::GetObjectParent( const char* child ) const
{
	if( !child ) return "";
	const std::map<String,String>::const_iterator i = parentByName.find( String( child ) );
	return ( i == parentByName.end() ) ? "" : i->second.c_str();
}

bool ObjectManager::RebakeHierarchy() const
{
	// See the header for why this exists next to ComposeWorldTransforms.
	if( parentByName.empty() ) return false;

	// Both containers are sized by the LINK count, not the object count.
	std::map<String, std::vector<std::pair<unsigned long long, String> > > childrenOf;
	std::set<String> isChild;
	std::map<String, IObjectPriv*> members;

	for( std::map<String,String>::const_iterator it = parentByName.begin(); it != parentByName.end(); ++it ) {
		const GenericManager<IObjectPriv>::ItemListType::const_iterator ci = items.find( it->first );
		if( ci == items.end() ) continue;   // the child itself is gone; the structural path owns that
		const GenericManager<IObjectPriv>::ItemListType::const_iterator pi = items.find( it->second );
		if( pi == items.end() ) {
			// Dangling parent.  Warned once per name -- the same dedupe the full
			// walk uses, and for the same reason: this runs every frame.
			if( danglingParentWarned.insert( it->first ).second ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"ObjectManager::RebakeHierarchy:: object `%s` names parent `%s`, which is no longer a "
					"registered object; it is composed as a root", it->first.c_str(), it->second.c_str() );
			}
			// COMPOSE IT ANYWAY, against identity, exactly as the full walk does
			// -- do not `continue`.  Its m_mxParentWorld still holds the parent
			// that vanished, and nothing else will clear it; skipping here on
			// the grounds that "its own finalize already put it on identity"
			// would be the same false assumption that made a detached ex-child
			// render at its ex-parent's pose.  It is a member with no link, so
			// the seeding loop below treats it as a root.
			members[it->first] = ci->second.first;
			continue;
		}
		childrenOf[it->second].push_back( std::make_pair( GetItemSerial( it->first.c_str() ), it->first ) );
		isChild.insert( it->first );
		members[it->first]  = ci->second.first;
		members[it->second] = pi->second.first;
	}
	// NOT `childrenOf.empty()`: a scene whose ONLY link is a dangling one has no
	// parent->child edges at all, and returning here would discard the very node
	// the dangling branch above just added to `members` -- the case that branch
	// exists for.
	if( members.empty() ) return false;
	for( std::map<String, std::vector<std::pair<unsigned long long, String> > >::iterator c = childrenOf.begin();
		c != childrenOf.end(); ++c ) {
		std::sort( c->second.begin(), c->second.end() );
	}

	// Seed from the hierarchy's own roots -- members that are nobody's child.
	std::vector<String> stack;
	for( std::map<String, IObjectPriv*>::const_iterator m = members.begin(); m != members.end(); ++m ) {
		if( isChild.find( m->first ) == isChild.end() ) stack.push_back( m->first );
	}
	std::reverse( stack.begin(), stack.end() );   // pop in name order, matching the full walk's determinism

	std::set<String> visited;
	bool anyChanged = false;
	size_t composed = 0;
	while( !stack.empty() ) {
		const String name = stack.back();
		stack.pop_back();
		if( !visited.insert( name ).second ) continue;   // cycle bound, same as the full walk
		const std::map<String, IObjectPriv*>::const_iterator mi = members.find( name );
		if( mi == members.end() || !mi->second ) continue;
		IObjectPriv* node = mi->second;

		Matrix4 parentWorld = Matrix4Ops::Identity();
		const std::map<String,String>::const_iterator link = parentByName.find( name );
		if( link != parentByName.end() ) {
			const GenericManager<IObjectPriv>::ItemListType::const_iterator p = items.find( link->second );
			if( p != items.end() ) parentWorld = p->second.first->GetFinalTransformMatrix();
		}

		// EXACT compare -- the caller decides whether to throw away the TLAS on
		// this answer, and a tolerance would let a genuine sub-epsilon move keep
		// a stale acceleration structure.
		const Matrix4 before = node->GetFinalTransformMatrix();
		node->FinalizeTransformations( parentWorld );
		const Matrix4 after = node->GetFinalTransformMatrix();
		const Scalar* b = &before._00;
		const Scalar* a2 = &after._00;
		for( int k = 0; k < 16; ++k ) {
			if( b[k] != a2[k] ) {
				anyChanged = true;
				node->ResetRuntimeData();   // the caches this node's new matrix invalidates
				break;
			}
		}
		++composed;

		// Children are held BY NAME, so pushing them is a copy -- this walk is
		// name-keyed throughout, which is also what lets `visited` bound a cycle
		// without a second pointer-keyed map.  Reversed so the LIFO stack pops
		// them in registration-serial order, matching the full walk.
		const std::map<String, std::vector<std::pair<unsigned long long, String> > >::const_iterator kids =
			childrenOf.find( name );
		if( kids != childrenOf.end() ) {
			for( std::vector<std::pair<unsigned long long, String> >::const_reverse_iterator k = kids->second.rbegin();
				k != kids->second.rend(); ++k ) {
				stack.push_back( k->second );
			}
		}
	}

	if( composed != members.size() ) {
		// Only reachable through a cycle that evaded SetObjectParent's guard.
		// Deduped by the SAME set the dangling-parent warning uses -- this runs
		// once per frame, and the full walk's equivalent diagnostic is not
		// deduped, which would write one line per frame for a whole animation.
		if( !rebakeIncompleteWarned ) {
			rebakeIncompleteWarned = true;
			GlobalLog()->PrintEx( eLog_Error,
				"ObjectManager::RebakeHierarchy:: composed %u of %u hierarchy nodes -- a cycle in the "
				"link map is the expected cause; the unreached nodes keep the world transform they last had",
				static_cast<unsigned int>( composed ), static_cast<unsigned int>( members.size() ) );
		}
	}

	if( anyChanged ) anyComposedAgainstParent = true;
	return anyChanged;
}

bool ObjectManager::ComposeWorldTransforms() const
{
	// FAST PATH: a flat scene is the overwhelmingly common case and must cost
	// nothing -- every object's parent is identity, which is what its last
	// finalize already used, so there is nothing to re-bake.  The second term
	// is load-bearing: "no links" is only equivalent to "nothing to do" once a
	// walk has actually PUT every node back on identity.  Detaching the last
	// child, or removing its parent, empties the map while the ex-child is
	// still carrying its old composed parent matrix.
	if( parentByName.empty() && !anyComposedAgainstParent ) {
		return false;
	}

	// Children lists, ordered by REGISTRATION SERIAL.  On a full derive that IS
	// document order, i.e. 87's "child order for display comes from declaration
	// order"; after an INCREMENTAL apply it is not, because a re-pointed object
	// keeps its old serial while an inserted one gets a fresh serial at the
	// end, so a child inserted mid-file sorts last among its siblings until the
	// next full derive.  Composition is indifferent to sibling order -- each
	// child composes against its parent alone -- so this matters only to the
	// tree UI (87 section 5 step 4), which should read order from the CST
	// document if it needs to be exact across a reload.  A stable, meaningful
	// order is still what a tree UI needs, and deriving it here means the UI
	// does not have to keep a parallel index.
	std::map<String, std::vector<std::pair<unsigned long long, IObjectPriv*> > > childrenOf;
	std::vector<IObjectPriv*> roots;
	std::map<const IObjectPriv*, String> nameOf;

	GenericManager<IObjectPriv>::ItemListType::const_iterator i, e;
	for( i=items.begin(), e=items.end(); i!=e; ++i ) {
		IObjectPriv* obj = i->second.first;
		nameOf[obj] = i->first;
		const std::map<String,String>::const_iterator link = parentByName.find( i->first );
		if( link == parentByName.end() ) {
			roots.push_back( obj );
			continue;
		}
		if( items.find( link->second ) == items.end() ) {
			// The recorded parent is gone (removed from the manager after the
			// link was made).  Treat the child as a root rather than dropping
			// it from the render list -- it is still a real object -- and warn
			// once per name so a per-frame compose cannot flood the log.
			if( danglingParentWarned.insert( i->first ).second ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"ObjectManager::ComposeWorldTransforms:: object `%s` names parent `%s`, which is "
					"no longer a registered object; treating `%s` as a root",
					i->first.c_str(), link->second.c_str(), i->first.c_str() );
			}
			roots.push_back( obj );
			continue;
		}
		childrenOf[link->second].push_back( std::make_pair( GetItemSerial( i->first.c_str() ), obj ) );
	}
	for( std::map<String, std::vector<std::pair<unsigned long long, IObjectPriv*> > >::iterator c = childrenOf.begin();
		c != childrenOf.end(); ++c ) {
		std::sort( c->second.begin(), c->second.end() );
	}

	// Iterative parent-before-child walk.  `visited` also bounds the walk
	// against a cycle that somehow evaded SetObjectParent's guard (e.g. links
	// recorded through a future path that forgets to call it): a node is
	// composed at most once, so a loop terminates instead of hanging.
	std::set<const IObjectPriv*> visited;
	std::vector<IObjectPriv*> stack( roots.rbegin(), roots.rend() );
	size_t composed = 0;
	bool anyChanged = false;
	bool sawParented = false;
	while( !stack.empty() ) {
		IObjectPriv* node = stack.back();
		stack.pop_back();
		if( !visited.insert( node ).second ) continue;

		// A root composes against identity -- byte-identical to the pre-
		// hierarchy behaviour.  A child composes against its parent's ALREADY
		// FINAL world matrix, which the parent-before-child order guarantees
		// is current.
		const std::map<const IObjectPriv*, String>::const_iterator nm = nameOf.find( node );
		const std::map<String,String>::const_iterator link =
			( nm == nameOf.end() ) ? parentByName.end() : parentByName.find( nm->second );
		Matrix4 parentWorld = Matrix4Ops::Identity();
		if( link != parentByName.end() ) {
			const GenericManager<IObjectPriv>::ItemListType::const_iterator p = items.find( link->second );
			if( p != items.end() ) parentWorld = p->second.first->GetFinalTransformMatrix();
		}
		// EXACT comparison, deliberately: the caller uses this to decide
		// whether to throw away the TLAS, and a tolerance would let a genuine
		// sub-epsilon move keep a stale acceleration structure.  Same rule as
		// the incremental apply's own BBoxEqual gate.
		const Matrix4 before = node->GetFinalTransformMatrix();
		if( link != parentByName.end() ) sawParented = true;
		node->FinalizeTransformations( parentWorld );
		{
			const Matrix4 after = node->GetFinalTransformMatrix();
			const Scalar* b = &before._00;
			const Scalar* a2 = &after._00;
			bool moved = false;
			for( int k = 0; k < 16; ++k ) { if( b[k] != a2[k] ) { moved = true; break; } }
			if( moved ) {
				anyChanged = true;
				// A node the walk MOVED needs its per-object runtime caches
				// dropped, exactly as the edited node does in
				// SceneEditor::RunObjectInvariantChain.  The editor only knows
				// about the node it was handed; the descendants it moved
				// through this walk are ours to reset.
				node->ResetRuntimeData();
			}
		}
		++composed;

		if( nm != nameOf.end() ) {
			const std::map<String, std::vector<std::pair<unsigned long long, IObjectPriv*> > >::const_iterator kids =
				childrenOf.find( nm->second );
			if( kids != childrenOf.end() ) {
				// Pushed in reverse so the LIFO stack pops them in declaration order.
				for( std::vector<std::pair<unsigned long long, IObjectPriv*> >::const_reverse_iterator k = kids->second.rbegin();
					k != kids->second.rend(); ++k ) {
					stack.push_back( k->second );
				}
			}
		}
	}

	if( composed != items.size() ) {
		// A cycle in the link map is the expected cause -- SetObjectParent
		// refuses those, so this guards a future writer that bypasses it.  The
		// count can also fall short if one object were ever registered under
		// two names (the walk visits by POINTER, the count is over NAMES), so
		// the message reports the observed fact and names the likely cause
		// rather than asserting it.  Either way the unreached nodes keep
		// whatever world transform they last had, which is worth saying loudly.
		GlobalLog()->PrintEx( eLog_Error,
			"ObjectManager::ComposeWorldTransforms:: composed %u of %u registered objects -- the "
			"remainder were not reached from any root, so their world transforms are STALE.  The usual "
			"cause is a cycle in the parent links; a single object registered under two names would "
			"also do it.",
			(unsigned int)composed, (unsigned int)items.size() );
	}

	anyComposedAgainstParent = sawParented;
	return anyChanged;
}

void ObjectManager::PrepareForRendering() const
{
	RISE_PROFILE_PHASE(AccelBuild);

	// EXPRESSION MEMO (Utilities/ExpressionMemo.h), the FIRST of two bumps
	// around this function; the TAIL one is the load-bearing half and
	// carries the argument for the pair.
	//
	// THIS ONE IS DEFENCE IN DEPTH.  It used to be justified by "the
	// realize pass below evaluates painters, and a displacement painter
	// can be an expression that reads geometry signals" -- but that
	// mechanism cannot reach a stale entry.  Realize-time displacement has
	// exactly two evaluators, and neither can: GeometryUtilities.cpp's
	// ApplyScalarHeightToObject builds its hit record with `ri.signals`
	// DEFAULT-constructed (its own comment spells out that the builtins
	// therefore read their neutral fallback), as does HairGenerator's
	// MakeRootRi; and ApplyDisplacementMapToObject takes an IFunction2D
	// and calls `Evaluate(u,v)`, which has no hit record at all.  So none
	// of them ever keys an L1 entry against a provider; what they can read
	// is the null-provider NEUTRAL, a constant.  L2 cannot be stale for
	// them either: `m_time` is in the key, and a painter's `param`s are
	// compile-time constants folded into a program whose id is minted
	// fresh by every Finalize.  Kept anyway
	// -- one atomic increment at a seam that is about to rebuild a TLAS,
	// and cover for a future realize-time consumer that DOES carry a
	// provider -- but nothing today depends on it.  (ExpressionMemo.h's
	// Invalidate() comment is the single account.)
	ExpressionMemo::Invalidate();

	// EXPRESSION MEMO, the SECOND bump, and the reason there are two.
	// Objects are about to be realized and the hierarchy re-baked, so any
	// geometry a memo entry was keyed against may move or be rebuilt by
	// the work below.  The bump above covers the mutation that happened
	// BEFORE we were called; this one covers the mutation this function
	// IS, and closes the window in which another thread adopts the top
	// bump's generation, misses, reads a geometry mid-realize, and stamps
	// that answer with a generation nothing will ever drop.
	//
	// A SCOPE GUARD rather than a trailing statement, because the work
	// between here and the end can THROW -- the realize pass runs arbitrary
	// geometry build code and CreateBVH allocates -- and an exception would
	// skip a trailing `Invalidate();`, leaving exactly the state the
	// argument above says must not exist.  RayCaster::AttachScene and
	// Scene::SetSceneTime use the same guard for the same reason.
	const ExpressionMemo::DropOnScopeExit memoDropOnExit;

	// Realize deferred geometry BEFORE building the TLAS from object bounding
	// boxes (an unrealized DisplacedGeometry reports a ZERO bbox, and the BVH
	// built from it is KEPT by the `!pBVH` guard below -> displaced objects
	// vanish / become unpickable).  Direct callers (the GUI production-render +
	// picking paths) reach PrepareForRendering before RayCaster::AttachScene's
	// realize pass, and this is the funnel they share.  Idempotent.
	RealizeAllObjects();

	// 87 step 2: RE-BAKE the hierarchy, every frame.  This is the whole of
	// hierarchical animation.  On the animation path EvaluateAtTime runs
	// immediately before every call to this function, moving whatever this
	// frame's keyframes drive; each of those nodes finalized against the parent
	// world it was LAST handed, which is the PREVIOUS frame's.  Re-walking here
	// recomposes every child against its parent's CURRENT world, so a timeline
	// on a parent carries its whole subtree with no animation code of its own --
	// `Transformable` already implements IKeyframable, so a container node is
	// keyframable for free.
	//
	// It also stops the stored parent world being a LATCH.  Under step 1 an
	// interactive edit while the user was parked at an animated t=T wrote
	// parentWorld(T) into every child and left it there, so scrubbing back
	// displaced the subtree for the rest of the session.  Recomputing it every
	// frame is the fix, and it is why no narrower one was worth having.
	//
	// NOT a structural re-flatten, per 87 §5 step 2: the object list and the
	// name map are untouched.  This walks into already-allocated entries.
	//
	// A scene with no `parent` links pays one predicate for this --
	// ComposeWorldTransforms' fast path returns immediately unless a link has
	// actually been composed against.  A scene WITH links pays a full walk per
	// frame; that is bounded by the TLAS rebuild happening a few lines below,
	// which is strictly more expensive.
	if( RebakeHierarchy() ) {
		// The walk MOVED something, so any acceleration structure built from
		// the old bounding boxes is stale.  The animation loop invalidates
		// before calling us and has usually done this already; repeating it
		// makes the postcondition -- "on return, the spatial structure matches
		// the world transforms" -- true for EVERY caller, including the GUI
		// production-render and picking paths that arrive here directly.  It
		// costs nothing when nothing moved: the walk's change test is an exact
		// 16-word matrix compare, so a static parented scene answers false on
		// every frame after the first.
		InvalidateSpatialStructure();
	}

	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) && !pBVH.load( std::memory_order_acquire ) ) {
		CreateBVH();
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) && !pOctree ) {
		CreateOctree();
	}

	if( !shadowCache ) {
		shadowCache = new ShadowCacheSlot[kShadowCacheSlots]();
	}

	// THE WORLD-AABB SNAPSHOT for the cross-object proximity query
	// (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.2), built LAST and GATED ON
	// DEMAND -- see below for why it is neither unconditional nor keyed to
	// the TLAS's own `items.size()` gate.
	//
	// LAST because everything above it can move an object: the re-bake
	// composes hierarchy transforms and, when it changes anything, calls
	// InvalidateSpatialStructure -- which drops this snapshot along with
	// pBVH.  Building before that would publish boxes from the PREVIOUS
	// frame's poses and then have them thrown away, or worse, kept.
	//
	// UNCONDITIONALLY, unlike the TLAS build above it, because its
	// `items.size() > nMaxObjectsPerNode` gate is a property of the TLAS
	// and not of this: a four-object scene has no top-level BVH and still
	// needs its boxes.
	//
	// GATED ON DEMAND, though -- see the call in IntersectRay for the full
	// argument.  A scene with no live `proximity()` consumer builds no
	// snapshot at all, which is observable (and observed:
	// `ForTest_HasBoxSnapshot` stays false after a render), and costs it
	// nothing but one relaxed atomic load per pass.
	if( ProximityDemand::Any() ) {
		EnsureBoxSnapshot();
	}

	// EXPRESSION MEMO: the trailing bump is taken by `memoDropOnExit`,
	// declared at the top of this function -- see its comment for why it
	// is a scope guard and not a statement here.
}

void ObjectManager::InvalidateSpatialStructure() const
{
	mSpatialGen = NextSpatialGeneration();   // observable: a non-spatial incremental edit must NOT reach here (slice 3 closure gate)
	{
		// RELEASE store: a concurrent unlocked reader's acquire load must
		// see either the fully-live old `BVH` or the null that means
		// "self-heal via CreateBVH", never a torn or reordered half-state.
		// This still does NOT make the release itself safe against a
		// reader mid-traversal of the OLD tree -- see pBVH's declaration
		// comment in ObjectManager.h for that residual.
		BVH<const IObjectPriv*>* localBVH = pBVH.load( std::memory_order_relaxed );
		if( localBVH ) {
			GlobalLog()->PrintEx( eLog_Info, "ObjectManager::InvalidateSpatialStructure:: Destroying top-level BVH for rebuild" );
			safe_release( localBVH );
			pBVH.store( localBVH, std::memory_order_release );
		}
	}
	if( pOctree ) {
		GlobalLog()->PrintEx( eLog_Info, "ObjectManager::InvalidateSpatialStructure:: Destroying octree for rebuild" );
		safe_release( pOctree );
	}
	// The proximity query's world-AABB snapshot dies here beside pBVH,
	// under the same "never during a pass" contract those already carry --
	// it is derived from exactly the world transforms this call is
	// declaring stale.  A plain delete, not safe_release: the snapshot is
	// an owned POD, not a refcounted engine object.
	//
	// BOTH the live pointer and the retired set are handled UNDER
	// treeCreationMutex, and the scope has to cover both: EnsureBoxSnapshot
	// (the sole other writer) loads the live pointer, pushes it onto
	// `retiredBoxes` and publishes its replacement inside ONE critical
	// section, so an unlocked delete-and-clear here could interleave with
	// that -- free the pointer EnsureBoxSnapshot has just loaded, watch it
	// push the dangling value onto the retired set and re-publish a fresh
	// snapshot over the null this call wrote (resurrecting a snapshot after
	// an invalidate), and then free the same pointer a second time from the
	// retired set below.  "Never during a pass" bounds WHEN this runs, not
	// what serializes it against a same-instant EnsureBoxSnapshot at a pass
	// boundary; the mutex is what rules that out.  `retiredBoxes` is a plain
	// `std::vector` with no thread-safety story of its own, which is the
	// second reason the lock is needed.  This is the ONE place, besides the
	// destructor (which runs after every thread is gone and so takes no
	// lock -- see ~ObjectManager) and Shutdown(), where a snapshot is
	// actually freed.
	treeCreationMutex.lock();
	{
		const ObjectBoxSnapshot* const cur = pBoxes.load( std::memory_order_relaxed );
		if( cur ) {
			delete cur;
			pBoxes.store( 0, std::memory_order_relaxed );
		}
	}
	for( std::size_t k = 0; k < retiredBoxes.size(); ++k ) {
		delete retiredBoxes[k];
	}
	retiredBoxes.clear();
	treeCreationMutex.unlock();
	// Shadow cache slots are reset but not freed — the array persists.
	if( shadowCache ) {
		memset( shadowCache, 0, sizeof(ShadowCacheSlot) * kShadowCacheSlots );
	}
}
