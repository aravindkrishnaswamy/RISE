//////////////////////////////////////////////////////////////////////
//
//  BilinearPatchGeometry.cpp - Implementation of the sphere class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BilinearPatchGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Octree.h"
#include "../BSPTree.h"
#include "../Utilities/stl_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// BilinearPatch specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

bool BilinearPatchGeometry::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
{
	return bbox.DoIntersect( GeometricUtilities::BilinearPatchBoundingBox(*elem) );
}

char BilinearPatchGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, GeometricUtilities::BilinearPatchBoundingBox(*elem) );
}

void BilinearPatchGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// This is where we call the Ray Intersection function
	const BilinearPatch&	patch = *elem;

    BILINEAR_HIT	h;
	RayBilinearPatchIntersection( ri.ray, h, patch );

	if( h.bHit ) {
		// If there is an intersection, compute the point of intersection, the normal and the
		// texture co-ordinates
		ri.bHit = true;
		ri.range = h.dRange;
		ri.ptIntersection = ri.ray.PointAtLength( h.dRange );
		ri.ptCoord = Point2( h.u, h.v );
		ri.vNormal = Vector3Ops::Normalize(GeometricUtilities::BilinearPatchNormalAt( patch, h.u, h.v ));
	}
}

void BilinearPatchGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool BilinearPatchGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// This is where we call the Ray Intersection function
	const BilinearPatch&	patch = *elem;

    BILINEAR_HIT	h;
	RayBilinearPatchIntersection( ray, h, patch );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void BilinearPatchGeometry::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
{
	//@ TODO : to be implemented
}

void BilinearPatchGeometry::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
{
	//@ TODO : to be implemented
}

BilinearPatchGeometry::BilinearPatchGeometry(
	const unsigned int max_polys_per_node, 
	const unsigned char max_recursion_level,
	const bool bUseBSP_
	) : 
  pBSPTree( 0 ),
  pOctree( 0 ),
  nMaxPerOctantNode( max_polys_per_node ),
  nMaxRecursionLevel( max_recursion_level ),
  bUseBSP( bUseBSP_ )
{
}

BilinearPatchGeometry::~BilinearPatchGeometry( )
{
	safe_release( pBSPTree );
	safe_release( pOctree );
}

void BilinearPatchGeometry::AddPatch( const BilinearPatch& patch )
{
	patches.push_back( patch );
}

void BilinearPatchGeometry::Prepare()
{
	// Prepare for rendering
	// Optimize the patch container
	stl_utils::container_optimize< BilinearPatchList >( patches );

	// First create the pointer patches list
	BilinearPatchPtrList		patchptrs;
	patchptrs.reserve( patches.size() );		// Points to all the bilinear patches

	for( unsigned int i=0; i<patches.size(); i++ ) {
		const BilinearPatch* b = &patches[i];
		patchptrs.push_back( b );
	}

	// We're done so stuff all the bilinear patches into the bsp-tree

	// Compute the bounding box
	BoundingBox overall( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );
	BilinearPatchList::const_iterator		m, n;
	for( m=patches.begin(), n=patches.end(); m!=n; m++ ) {
		overall.Include( GeometricUtilities::BilinearPatchBoundingBox( *m ) );
	}

	safe_release( pBSPTree );
	safe_release( pOctree );

	if( bUseBSP ) {
		pBSPTree = new BSPTree<const BilinearPatch*>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pBSPTree, __FILE__, __LINE__, "bilinear patches bsptree" );

		pBSPTree->AddElements( patchptrs, nMaxRecursionLevel );
	} else {
		pOctree = new Octree<const BilinearPatch*>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pOctree, __FILE__, __LINE__, "bilinear patches octree" );

		pOctree->AddElements( patchptrs, nMaxRecursionLevel );
	}
}

void BilinearPatchGeometry::GenerateMesh( )
{

}

void BilinearPatchGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( pBSPTree ) {
		pBSPTree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	}
}

bool BilinearPatchGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( pBSPTree ) {
		return pBSPTree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	}

	return false;
}

void BilinearPatchGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( INFINITY, INFINITY, INFINITY );
	Point3	ptMax( -INFINITY, -INFINITY, -INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	BilinearPatchList::const_iterator m, n;
	for( m=patches.begin(), n=patches.end(); m!=n; m++ )
	{
		const BilinearPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			const Point3& pt = p.pts[j];
			if( pt.x < ptMin.x ) ptMin.x = pt.x;
			if( pt.y < ptMin.y ) ptMin.y = pt.y;
			if( pt.z < ptMin.z ) ptMin.z = pt.z;
			if( pt.x > ptMax.x ) ptMax.x = pt.x;
			if( pt.y > ptMax.y ) ptMax.y = pt.y;
			if( pt.z > ptMax.z ) ptMax.z = pt.z;
		}
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again and calculate the radius of the sphere
	for( m=patches.begin(), n=patches.end(); m!=n; m++ ) {
		const BilinearPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			const Point3& pt = p.pts[j];
			Vector3			r = Vector3Ops::mkVector3( pt, ptCenter );
			const Scalar	d = Vector3Ops::Magnitude(r);

			if( d > radius ) {
				radius = d;
			}
		}
	}
}

BoundingBox BilinearPatchGeometry::GenerateBoundingBox() const
{
	if( bUseBSP && pBSPTree ) {
		return pBSPTree->GetBBox();
	} else if( pOctree ) {
		return pOctree->GetBBox();
	}
	
	return BoundingBox();
}

void BilinearPatchGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	//@ To be implemented
}

Scalar BilinearPatchGeometry::GetArea( ) const
{
	//! @@ TODO, find a way to do this
	return 1.0;
}
