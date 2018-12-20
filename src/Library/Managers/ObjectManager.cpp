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

char ObjectManager::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, elem->getBoundingBox() );
}

void ObjectManager::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( elem->IsWorldVisible() ) {
		elem->IntersectRay( ri, INFINITY, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	}
}

void ObjectManager::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( elem->IsWorldVisible() ) {
		RayIntersection r( ri );
		RayElementIntersection( r, elem, bHitFrontFaces, bHitBackFaces, false );
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
  pBSPtree( 0 ), 
  pOctree( 0 ),
  bUseBSPtree( bUseBSPtree_ ),
  bUseOctree( bUseOctree_ ),
  nMaxObjectsPerNode( nMaxObjectsPerNode_ ),
  nMaxTreeDepth( nMaxTreeDepth_ )
{
	if( bUseBSPtree && bUseOctree ) {
		GlobalLog()->PrintEasyWarning( "ObjectManager::ObjectManager:: Can't use both Octrees and BSPtrees at the same time!" );
		bUseOctree = false;
	}

	if( bUseBSPtree ) {
		GlobalLog()->PrintEasyInfo( "ObjectManager is configured to use BSP trees for spatial partioning" );
	} else if( bUseOctree ) {
		GlobalLog()->PrintEasyInfo( "ObjectManager is configured to use Octrees for spatial partioning" );
	} else {
		GlobalLog()->PrintEasyInfo( "ObjectManager is not configured for any type of spatial acceleration" );
	}
}

ObjectManager::~ObjectManager( )
{
	safe_release( pBSPtree );
	safe_release( pOctree );
}

void ObjectManager::CreateBSPTree() const
{
	treeCreationMutex.lock();

	// Check again if we need to create it
	if( pBSPtree ) {
		treeCreationMutex.unlock();
		return;
	}

	// Construct the overall bounding box
	BoundingBox bbox( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );

	std::vector<MYOBJ> elements;

	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			bbox.Include( i->second.first->getBoundingBox() );
			elements.push_back( i->second.first );
		}
	}

	BSPTree<const IObjectPriv*>* newpBSPtree = new BSPTree<MYOBJ>( *this, bbox, nMaxObjectsPerNode );
	GlobalLog()->PrintNew( newpBSPtree, __FILE__, __LINE__, "bsptree" );
	newpBSPtree->AddElements( elements, nMaxTreeDepth );
	pBSPtree = newpBSPtree;

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
	BoundingBox bbox( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );

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
	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pBSPtree ) {
			CreateBSPTree();
		}

		ri.geometric.bHit = false;
		ri.geometric.range = INFINITY;

		pBSPtree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pOctree ) {
			CreateOctree();
		}

		ri.geometric.bHit = false;
		ri.geometric.range = INFINITY;

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
}

bool ObjectManager::IntersectShadowRay( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( bUseBSPtree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pBSPtree ) {
			CreateBSPTree();
		}

		return pBSPtree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else if( bUseOctree && (items.size() > nMaxObjectsPerNode) ) {
		if( !pOctree ) {
			CreateOctree();
		}

		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else {
		GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
		for( i=items.begin(), e=items.end(); i!=e; i++ ) {
			if( i->second.first->IsWorldVisible() && i->second.first->DoesCastShadows() ) {
				if( i->second.first->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces ) ) {
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

