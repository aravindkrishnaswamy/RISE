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
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Log/Log.h"
#include "../Utilities/Profiling.h"
#include <cstdint>

using namespace RISE;
using namespace RISE::Implementation;

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
  bUseBSPtree( bUseBSPtree_ ),
  bUseOctree( bUseOctree_ ),
  nMaxObjectsPerNode( nMaxObjectsPerNode_ ),
  nMaxTreeDepth( nMaxTreeDepth_ ),
  shadowCache( new ShadowCacheSlot[kShadowCacheSlots]() )
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
	safe_release( pBVH );
	safe_release( pOctree );
	delete [] shadowCache;
}

void ObjectManager::CreateBVH() const
{
	treeCreationMutex.lock();

	// Check again if we need to create it
	if( pBVH ) {
		treeCreationMutex.unlock();
		return;
	}

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
	pBVH = newpBVH;

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

void ObjectManager::IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RISE_PROFILE_PHASE(GeomPrimary);
	RISE_PROFILE_INC(nPrimaryRays);

	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pBVH ) {
			GlobalLog()->PrintEasyWarning( "ObjectManager: BVH built lazily during IntersectRay; call PrepareForRendering() before rendering" );
			CreateBVH();
		}

		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;

		// BVH<>::IntersectRay auto-routes to BVH4 SIMD traversal when
		// the post-build collapse populated the wide nodes (the common
		// case for any non-degenerate scene).
		pBVH->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
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
				i->second.first->IntersectRay( this_ri, ri.geometric.range, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );

				if( this_ri.geometric.bHit && this_ri.geometric.range < ri.geometric.range ) {
					ri = this_ri;
				}
			}
		}
	}

	if( !ri.geometric.bHit ) {
		RISE_PROFILE_INC(nMisses);
	}
}

bool ObjectManager::IntersectShadowRay( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	RISE_PROFILE_PHASE(GeomShadow);
	RISE_PROFILE_INC(nShadowRays);

	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pBVH ) {
			CreateBVH();
		}
		return pBVH->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
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

		if( cached ) {
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

void ObjectManager::PrepareForRendering() const
{
	RISE_PROFILE_PHASE(AccelBuild);
	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) && !pBVH ) {
		CreateBVH();
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) && !pOctree ) {
		CreateOctree();
	}

	if( !shadowCache ) {
		shadowCache = new ShadowCacheSlot[kShadowCacheSlots]();
	}
}

void ObjectManager::InvalidateSpatialStructure() const
{
	if( pBVH ) {
		GlobalLog()->PrintEx( eLog_Info, "ObjectManager::InvalidateSpatialStructure:: Destroying top-level BVH for rebuild" );
		safe_release( pBVH );
	}
	if( pOctree ) {
		GlobalLog()->PrintEx( eLog_Info, "ObjectManager::InvalidateSpatialStructure:: Destroying octree for rebuild" );
		safe_release( pOctree );
	}
	// Shadow cache slots are reset but not freed — the array persists.
	if( shadowCache ) {
		memset( shadowCache, 0, sizeof(ShadowCacheSlot) * kShadowCacheSlots );
	}
}
