//////////////////////////////////////////////////////////////////////
//
//  BezierPatchGeometry.cpp - Implementation of the sphere class
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
#include "BezierPatchGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MRUCache.h"
#include "../Utilities/stl_utils.h"

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// BezierPatch specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

bool BezierPatchGeometry::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
{
	return bbox.DoIntersect( elem.bbox );
}

char BezierPatchGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, elem.bbox );
}

void BezierPatchGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	BOX_HIT	h;
	RayBoxIntersection( ri.ray, h, elem.bbox.ll, elem.bbox.ur );

	if( h.bHit ) {
		ITriangleMeshGeometryIndexed* pGeom = cache.Get( elem );
		pGeom->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, false );
		cache.Return( elem, pGeom );
	}
}

void BezierPatchGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool BezierPatchGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	BOX_HIT	h;
	RayBoxIntersection( ray, h, elem.bbox.ll, elem.bbox.ur );

	if( h.bHit ) {
		ITriangleMeshGeometryIndexed* pGeom = cache.Get( elem );
		bool bRet = pGeom->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
		cache.Return( elem, pGeom );
		return bRet;
	}

	return false;
}

void BezierPatchGeometry::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
{
	//@ TODO : to be implemented
}

void BezierPatchGeometry::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
{
	//@ TODO : to be implemented
}

BezierPatchGeometry::BezierPatchGeometry(
	const unsigned int max_patches_per_node, 
	const unsigned char max_recursion_level,
	const bool bUseBSP_,
	const bool bAnalytic_,
	const unsigned int cache_size,
	const unsigned int max_polys_per_node, 
	const unsigned char max_poly_recursion_level, 
	const bool bDoubleSided,
	const bool bPolyUseBSP,
	const bool bUseFaceNormals,
	const unsigned int detail,
	const IFunction2D* displacement,
	const Scalar disp_scale
	) : 
  pBSPTree( 0 ),
  pOctree( 0 ),
  nMaxPerOctantNode( max_patches_per_node ),
  nMaxRecursionLevel( max_recursion_level ),
  bUseBSP( bUseBSP_ ),
  bAnalytic( bAnalytic_ ),
  generator( max_polys_per_node, max_poly_recursion_level, bDoubleSided, bPolyUseBSP, bUseFaceNormals, detail, displacement, disp_scale ),
  cache( generator, cache_size )
{
	if( bAnalytic ) {
		GlobalLog()->PrintEasyError( "BezierPatchGeometry:: Analytic rendering isn't supported yet." );
	}
}

BezierPatchGeometry::~BezierPatchGeometry( )
{
	safe_release( pBSPTree );
	safe_release( pOctree );
}

void BezierPatchGeometry::AddPatch( const BezierPatch& patch )
{
	patches.push_back( patch );
}

void BezierPatchGeometry::Prepare()
{
	// Prepare for rendering
	// Optimize the patch container
	stl_utils::container_optimize< BezierPatchList >( patches );

	// First create the pointer patches list
	BezierPatchPtrList		patchptrs;
	patchptrs.reserve( patches.size() );		// Points to all the bezier patches

	BoundingBox overall( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );
	for( unsigned int i=0; i<patches.size(); i++ ) {
		BezierPatch* b = &patches[i];
		MYBEZIERPATCH m;
		m.pPatch = b;
		m.id = i;
		m.bbox = GeometricUtilities::BezierPatchBoundingBox( *b );
		if( generator.displacement ) {
			m.bbox.Grow( generator.disp_scale );
		}
		overall.Include( m.bbox );
		patchptrs.push_back( m );
	}

	// We're done so stuff all the bezier patches into the bsp-tree
	safe_release( pBSPTree );
	safe_release( pOctree );

	if( bUseBSP ) {
		pBSPTree = new BSPTree<MYBEZIERPATCH>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pBSPTree, __FILE__, __LINE__, "bezier patches bsptree" );

		pBSPTree->AddElements( patchptrs, nMaxRecursionLevel );

//		pBSPTree->DumpStatistics( eLog_Info );
	} else {
		pOctree = new Octree<MYBEZIERPATCH>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pOctree, __FILE__, __LINE__, "bezier patches octree" );

		pOctree->AddElements( patchptrs, nMaxRecursionLevel );

//		pOctree->DumpStatistics( eLog_Info );
	}
}

void BezierPatchGeometry::GenerateMesh( )
{

}

void BezierPatchGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( pBSPTree ) {
		pBSPTree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	}
}

bool BezierPatchGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( pBSPTree ) {
		return pBSPTree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	}

	return false;
}

void BezierPatchGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( INFINITY, INFINITY, INFINITY );
	Point3	ptMax( -INFINITY, -INFINITY, -INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	BezierPatchList::const_iterator m, n;
	for( m=patches.begin(), n=patches.end(); m!=n; m++ )
	{
		const BezierPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			for( int k=0; k<4; k++ ) {
				const Point3& pt = p.c[j].pts[k];
				if( pt.x < ptMin.x ) ptMin.x = pt.x;
				if( pt.y < ptMin.y ) ptMin.y = pt.y;
				if( pt.z < ptMin.z ) ptMin.z = pt.z;
				if( pt.x > ptMax.x ) ptMax.x = pt.x;
				if( pt.y > ptMax.y ) ptMax.y = pt.y;
				if( pt.z > ptMax.z ) ptMax.z = pt.z;
			}
		}
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again and calculate the radius of the sphere
	for( m=patches.begin(), n=patches.end(); m!=n; m++ ) {
		const BezierPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			for( int k=0; k<4; k++ ) {
				const Point3& pt = p.c[j].pts[k];
				Vector3			r = Vector3Ops::mkVector3( pt, ptCenter );
				const Scalar	d = Vector3Ops::Magnitude(r);

				if( d > radius ) {
					radius = d;
				}
			}
		}
	}
}

BoundingBox BezierPatchGeometry::GenerateBoundingBox() const
{
	if( bUseBSP && pBSPTree ) {
		return pBSPTree->GetBBox();
	} else if( pOctree ) {
		return pOctree->GetBBox();
	}
	
	return BoundingBox();
}

void BezierPatchGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	//@ To be implemented
}

Scalar BezierPatchGeometry::GetArea( ) const
{
	//! @@ TODO, find a way to do this
	return 1.0;
}
