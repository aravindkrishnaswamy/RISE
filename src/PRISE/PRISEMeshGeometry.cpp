//////////////////////////////////////////////////////////////////////
//
//  PRISEMeshGeometry.cpp - Implementation of the TriangleMesh
//  Geometry class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PRISEMeshGeometry.h"
#include "PRISEOctree.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"

using namespace Implementation;

typedef Triangle	MYTRI;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// MYTRI specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

template<>
bool PRISEOctree<MYTRI>::PRISEOctreeNode::ElementBoxIntersection( const MYTRI elem, const Point3D& ll, const Point3D& ur ) const
{
	const Triangle&	p = elem;
	//
	// Trivial acception, any of the points are inside the box
	//
	for( int j=0; j<3; j++ ) {
		if( (p.vertices[j].x >= ll.x) &&
			(p.vertices[j].x <= ur.x) &&
			(p.vertices[j].y >= ll.y) &&
			(p.vertices[j].y <= ur.y) &&
			(p.vertices[j].z >= ll.z) &&
			(p.vertices[j].z <= ur.z)
			)
		{
			// Then this polygon qualifies
			return true;
		}
	}

	//
	// Check if any of the triangle's edges intersect the box
	//

	// Edge 1
	BOX_HIT		h;
	Ray		ray;
	Scalar fEdgeLength;

	ray.origin = p.vertices[0];
	ray.dir = p.vertices[1] - p.vertices[0];
	fEdgeLength = ~ray.dir;
	ray.dir.Normalize();

	RayBoxIntersection( ray, h, ll, ur );
	if( h.bHit && h.dRange <= fEdgeLength ) {
		return true;
	}

	// Edge 2
	ray.origin = p.vertices[1];
	ray.dir = p.vertices[2] - p.vertices[1];
	fEdgeLength = ~ray.dir;
	ray.dir.Normalize();

	RayBoxIntersection( ray, h, ll, ur );
	if( h.bHit && h.dRange <= fEdgeLength ) {
		return true;
	}


	// Edge 3
	ray.origin = p.vertices[2];
	ray.dir = p.vertices[0] - p.vertices[2];
	fEdgeLength = ~ray.dir;
	ray.dir.Normalize();

	RayBoxIntersection( ray, h, ll, ur );
	if( h.bHit && h.dRange <= fEdgeLength ) {
		return true;
	}


	//
	// We know the none of the triangle's points lie in the box, we know
	// none of its edges intersect the box
	// That leaves just one more case, and that is the box cuts the triangle
	// completely internally
	//

	// Cheat and use two BBs
	Point3D triMin;
	Point3D triMax;

	triMin.x = min( p.vertices[0].x, min( p.vertices[1].x, p.vertices[2].x ) );
	triMin.y = min( p.vertices[0].y, min( p.vertices[1].y, p.vertices[2].y ) );
	triMin.z = min( p.vertices[0].z, min( p.vertices[1].z, p.vertices[2].z ) );

	triMax.x = max( p.vertices[0].x, max( p.vertices[1].x, p.vertices[2].x ) );
	triMax.y = max( p.vertices[0].y, max( p.vertices[1].y, p.vertices[2].y ) );
	triMax.z = max( p.vertices[0].z, max( p.vertices[1].z, p.vertices[2].z ) );

	// Now check the two BBs
	if( triMin.x <= ur.x && triMin.y <= ur.y && triMin.z <= ur.z &&
		triMax.x >= ll.x && triMax.y >= ll.y && triMax.z >= ll.z ) {
		return true;
	}

	//
	// No way there's an intersection
	//
	return false;

}

template<>
void PRISEOctree<MYTRI>::PRISEOctreeNode::Serialize( IWriteBuffer& buffer, const MYTRI& elem ) const
{
	for( int i=0; i<3; i++ ) {
		buffer.setDouble( elem.vertices[i].x );
		buffer.setDouble( elem.vertices[i].y );
		buffer.setDouble( elem.vertices[i].z );

		buffer.setDouble( elem.normals[i].x );
		buffer.setDouble( elem.normals[i].y );
		buffer.setDouble( elem.normals[i].z );

		buffer.setDouble( elem.coords[i].x );
		buffer.setDouble( elem.coords[i].y );
	}
}

template<>
void PRISEOctree<MYTRI>::PRISEOctreeNode::DeserializeElement( IReadBuffer& buffer, MYTRI& ret )
{
	for( int j=0; j<3; j++ ) {
		ret.vertices[j].x = buffer.getDouble();
		ret.vertices[j].y = buffer.getDouble();
		ret.vertices[j].z = buffer.getDouble();

		ret.normals[j].x = buffer.getDouble();
		ret.normals[j].y = buffer.getDouble();
		ret.normals[j].z = buffer.getDouble();

		ret.coords[j].x = buffer.getDouble();
		ret.coords[j].y = buffer.getDouble();
	}
}



PRISEMeshGeometry::PRISEMeshGeometry( const unsigned int max_polys_per_node, const unsigned char max_recursion_level, const unsigned int max_polys_level2, const unsigned char max_recur_level2 ) :
  nMaxPerOctantNode( max_polys_per_node ), nMaxRecursionLevel( max_recursion_level), pPolygonsOctree( 0 ), nMaxPerNodeLevel2( max_polys_level2 ), nMaxRecursionLevel2( max_recur_level2 )
{
}

PRISEMeshGeometry::~PRISEMeshGeometry()
{
	if( pPolygonsOctree ) {
		pPolygonsOctree->RemoveRef();
		pPolygonsOctree = 0;
	}
}

void PRISEMeshGeometry::GenerateMesh( )
{
	// Hmmm....  that can't be too hard now can it ? <snicker>
	// I'll get around to this someday
}

void PRISEMeshGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
	// mesh geometry never generates exit information, it just ignores that command!
	if( pPolygonsOctree ) {
		IMemoryBuffer*	s = (IMemoryBuffer*)ri.custom;
		if( s ) {
			// We are to continue a previous ray using the call stack in the string s
			ri.custom = 0;
			pPolygonsOctree->IntersectRayImcomplete( ri, bHitFrontFaces, bHitBackFaces, s );
		} else {
			pPolygonsOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
		}
	}
}

bool PRISEMeshGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( pPolygonsOctree ) {
		return pPolygonsOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	}

	return false;
}

void PRISEMeshGeometry::getUniformRandomPoint( Point3D* point, Vector3D* normal, Point2D* coord, const Point3D& prand ) const
{
	// We should come up with a better way of doing this!
	// Right now, I'm just going to randomly pick a triangle and
	// then pick a random point on that triangle
	unsigned int idx = (unsigned int)(prand.z * Scalar(polygons.size()-1));
	const Triangle& t = polygons[idx];
	Point2D	prand2( prand.x, prand.y );

	if( point ) {
		*point = PointOnTriangle<Point3D>( t.vertices[0], t.vertices[1], t.vertices[2], prand2 );
	}

	if( normal ) {
		*normal = PointOnTriangle<Vector3D>( t.normals[0], t.normals[1], t.normals[2], prand2 );
	}

	if( coord ) {
		*coord = PointOnTriangle<Point2D>( t.coords[0], t.coords[1], t.coords[2], prand2 );
	}
}

Scalar PRISEMeshGeometry::getArea( ) const
{
	// Sum the areas of all the triangles..
	Scalar	sum=0;
	MyTriangleList::const_iterator		i, e;

	for( i=polygons.begin(), e=polygons.end(); i!=e; i++ )
	{
		const Triangle& thisTri = (*i);
		Vector3D vEdgeA = thisTri.vertices[1] - thisTri.vertices[0];
		Vector3D vEdgeB = thisTri.vertices[2] - thisTri.vertices[0];
		sum += (~(vEdgeA*vEdgeB)) * 0.5;
	}

	return sum;
}

void PRISEMeshGeometry::BeginTriangles( )
{
	if( pPolygonsOctree ) {
		pPolygonsOctree->RemoveRef();
		pPolygonsOctree = 0;
	}
}

void PRISEMeshGeometry::AddTriangle( const Triangle& tri )
{
	// Add the triangle, precompute the stuff that needs to be precomputed
	polygons.push_back( tri );
}

template< class T >
void container_erase_all( T& v )
{
	T empty;
	v.swap( empty );
}

void PRISEMeshGeometry::DoneTriangles( )
{
	// We're done with all the triangles so stuff it all into an octree
	// First compute the bounds of the octree
	Vector3D vRootLowerLeft = Vector3D( INFINITY, INFINITY, INFINITY );
	Vector3D vRootUpperRight = Vector3D( -INFINITY, -INFINITY, -INFINITY );


	MyTriangleList::iterator i, e;
	for( i=polygons.begin(), e=polygons.end(); i!=e; i++ )
	{
		const Triangle&	p = (*i);
		for( int j=0; j<3; j++ )
		{
			if( p.vertices[j].x < vRootLowerLeft.x ) vRootLowerLeft.x = p.vertices[j].x;
			if( p.vertices[j].y < vRootLowerLeft.y ) vRootLowerLeft.y = p.vertices[j].y;
			if( p.vertices[j].z < vRootLowerLeft.z ) vRootLowerLeft.z = p.vertices[j].z;

			if( p.vertices[j].x > vRootUpperRight.x ) vRootUpperRight.x = p.vertices[j].x;
			if( p.vertices[j].y > vRootUpperRight.y ) vRootUpperRight.y = p.vertices[j].y;
			if( p.vertices[j].z > vRootUpperRight.z ) vRootUpperRight.z = p.vertices[j].z;
		}
	}

	if( pPolygonsOctree ) {
		pPolygonsOctree->RemoveRef();
		pPolygonsOctree = 0;
	}

	pPolygonsOctree = new PRISEOctree<Triangle>( vRootLowerLeft, vRootUpperRight, nMaxPerOctantNode );
	GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree" );

	pPolygonsOctree->AddElements( polygons, nMaxRecursionLevel, nMaxPerNodeLevel2, nMaxRecursionLevel2 );

//	pPolygonsOctree->DumpStatistics( eLog_Info );

	container_erase_all< MyTriangleList >( polygons );
}

void PRISEMeshGeometry::GenerateBoundingSphere( Point3D& ptCenter, Scalar& radius ) const
{
	if( pPolygonsOctree ) {
		Point3D myll, myur;
		pPolygonsOctree->GetBBox( myll, myur );

		Vector3D	vMin( myll.x, myll.y, myll.z );
		Vector3D	vMax( myur.x, myur.y, myur.z );

		// The center is the center of the minimum and maximum values of the points
		ptCenter = (vMax+vMin)*0.5f;
		radius = ~(myur-ptCenter);
	}
}

void PRISEMeshGeometry::GenerateBoundingBox( Point3D& ll, Point3D& ur ) const
{
	if( pPolygonsOctree )
	{
		Point3D myll, myur;
		pPolygonsOctree->GetBBox( myll, myur );

		if( myll.x < ll.x ) ll.x = myll.x;
		if( myll.y < ll.y ) ll.y = myll.y;
		if( myll.z < ll.z ) ll.z = myll.z;

		if( myur.x > ur.x ) ur.x = myur.x;
		if( myur.y > ur.y ) ur.y = myur.y;
		if( myur.z > ur.z ) ur.z = myur.z;
	}
}

void PRISEMeshGeometry::Serialize( IWriteBuffer& buffer ) const
{
	// stuff data into the buffer

	// first put octree settings
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( char ) );
	buffer.setUInt( nMaxPerOctantNode );
	buffer.setChar( nMaxRecursionLevel );

	GlobalLog()->PrintEasyInfo( "PRISEMeshGeometry:: Begining Octree serialization" );

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( pPolygonsOctree ? 1 : 0 );

	// Now serialize the octree
	if( pPolygonsOctree ) {
		pPolygonsOctree->Serialize( buffer );
	}

	// Thats it we are done!
}

void PRISEMeshGeometry::SerializeForCPU( IWriteBuffer& buffer, int cpu ) const
{
	// first put octree settings
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( char ) );
	buffer.setUInt( nMaxPerOctantNode );
	buffer.setChar( nMaxRecursionLevel );

	GlobalLog()->PrintEasyInfo( "PRISEMeshGeometry:: Begining Octree serialization" );

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( pPolygonsOctree ? 1 : 0 );

	// Now serialize the octree
	if( pPolygonsOctree ) {
		pPolygonsOctree->SerializeForCPU( buffer, cpu );
	}
}

int PRISEMeshGeometry::CPUFromCallStack( unsigned int nCallStack ) const
{
	if( pPolygonsOctree ) {
		return pPolygonsOctree->CPUFromCallStack( nCallStack );
	}

	return -1;
}

void PRISEMeshGeometry::Deserialize( IReadBuffer& buffer )
{
	// First get octree settings
	nMaxPerOctantNode = buffer.getUInt();
	nMaxRecursionLevel = buffer.getChar();

	polygons.clear();

	char bpolyoctree = buffer.getChar();

	// Deserialize the octrees
	if( bpolyoctree ) {
		pPolygonsOctree = new PRISEOctree<Triangle>( Vector3D(0,0,0), Vector3D(0,0,0), nMaxPerOctantNode );
		GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree" );

		// Deserialize
		pPolygonsOctree->Deserialize( buffer );
	}

	// And we're done!
}

void PRISEMeshGeometry::SegmentOctreeForCPUS( const unsigned int num_cpus )
{
	if( pPolygonsOctree ) {
		pPolygonsOctree->SegmentForCPUS( num_cpus );
	}
}
