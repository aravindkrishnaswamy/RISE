//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometry.cpp - Implementation of the TriangleMesh
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
#include "TriangleMeshGeometry.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "GeometryUtilities.h"
#include "../Utilities/stl_utils.h"

inline unsigned int VoidPtrToUInt( const void* v )
{
	return (unsigned int)*((unsigned int*)(&v));
}

using namespace RISE;
using namespace RISE::Implementation;

#include "TriangleMeshGeometrySpecializations.h"

TriangleMeshGeometry::TriangleMeshGeometry(
	const unsigned int max_polys_per_node, 
	const unsigned char max_recursion_level, 
	const bool bDoubleSided_,
	const bool bUseBSP_
	) :
  nMaxPerOctantNode( max_polys_per_node ),
  nMaxRecursionLevel( max_recursion_level), 
  bDoubleSided( bDoubleSided_ ),
  bUseBSP( bUseBSP_ ),
  pPolygonsOctree( 0 ), 
  pPolygonsBSPtree( 0 )
{
}

TriangleMeshGeometry::~TriangleMeshGeometry()
{
	safe_release( pPolygonsOctree );
	safe_release( pPolygonsBSPtree );
}

void TriangleMeshGeometry::GenerateMesh( )
{
	// Hmmm....  that can't be too hard now can it ? <snicker>
}

void TriangleMeshGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
	// Triangle mesh geometry never generates exit information, it just ignores that command!

	if( bUseBSP && pPolygonsBSPtree ) {
		pPolygonsBSPtree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	} else if( pPolygonsOctree ) {
		pPolygonsOctree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	if( ri.bHit && bDoubleSided ) {
		// Flip the normal if we must
		if( Vector3Ops::Dot(ri.vNormal, ri.ray.dir) > 0 ) {
			ri.vNormal = -ri.vNormal;
		}
	}
}

bool TriangleMeshGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	bool bHit = false;

	if( bUseBSP && pPolygonsBSPtree) {
		return pPolygonsBSPtree->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	} else if( pPolygonsOctree ) {
		return pPolygonsOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	return bHit;
}

void TriangleMeshGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// Find the desired triangle where the CDF is greater than the rand value
	TriangleAreasList::const_iterator it = std::lower_bound( areasCDF.begin(), areasCDF.end(), prand.z );

	int idx = areasCDF.size()-1;
	if( it != areasCDF.end() ) {
		idx = std::distance( areasCDF.begin(), it );
	}

	GeometricUtilities::PointOnTriangle( point, normal, coord, polygons[idx], prand.x, prand.y );
}

Scalar TriangleMeshGeometry::GetArea( ) const
{
	return totalArea;
}

void TriangleMeshGeometry::BeginTriangles( )
{
	safe_release( pPolygonsOctree );
	safe_release( pPolygonsBSPtree );
	areas.clear();
	areasCDF.clear();
}

void TriangleMeshGeometry::AddTriangle( const Triangle& tri )
{
	// Add the triangle, precompute the stuff that needs to be precompute
	polygons.push_back( tri );
}

void TriangleMeshGeometry::ComputeAreas()
{
	// Compute triangle areas
	totalArea = 0;
	{
		MyTriangleList::const_iterator i, e;
		for( i=polygons.begin(), e=polygons.end(); i!=e; i++ ) {
			const Triangle& thisTri = (*i);
			Vector3 vEdgeA = Vector3Ops::mkVector3( thisTri.vertices[1], thisTri.vertices[0] );
			Vector3 vEdgeB = Vector3Ops::mkVector3( thisTri.vertices[2], thisTri.vertices[0] );
			const Scalar thisArea = (Vector3Ops::Magnitude(Vector3Ops::Cross(vEdgeA,vEdgeB))) * 0.5;
			totalArea += thisArea;
			areas.push_back( thisArea );
		}
	}
	// Compute the areas CDF
	{
		const Scalar invArea = 1.0 / totalArea;
		Scalar sum = 0;

		TriangleAreasList::const_iterator i, e;
		for( i=areas.begin(), e=areas.end(); i!=e; i++ ) {
			sum += (*i) * invArea;
			areasCDF.push_back( sum );
		}
	}
}

void TriangleMeshGeometry::DoneTriangles( )
{
	// We're done with all the triangles so stuff it all into an octree
	// First compute the bounds of the octree
	BoundingBox bbox( Point3( INFINITY, INFINITY, INFINITY ), Point3( -INFINITY, -INFINITY, -INFINITY ) );

	std::vector<const Triangle*>	temp;

	{
		MyTriangleList::iterator i, e;
		for( i=polygons.begin(), e=polygons.end(); i!=e; i++ )
		{
			const Triangle& p = (*i);
			for( int j=0; j<3; j++ ) {
				bbox.Include( p.vertices[j] );
			}

			temp.push_back( &p );
		}
	}

	if( bUseBSP ) {
		safe_release( pPolygonsBSPtree );

		pPolygonsBSPtree = new BSPTree<const Triangle*>( *this, bbox, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons bsptree" );

		pPolygonsBSPtree->AddElements( temp, nMaxRecursionLevel );

//		pPolygonsBSPtree->DumpStatistics( eLog_Info );
	} else {
		safe_release( pPolygonsOctree );

		pPolygonsOctree = new Octree<const Triangle*>( *this, bbox, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree" );

		pPolygonsOctree->AddElements( temp, nMaxRecursionLevel );

//		pPolygonsOctree->DumpStatistics( eLog_Info );
	}

	ComputeAreas();
}

void TriangleMeshGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( INFINITY, INFINITY, INFINITY );
	Point3	ptMax( -INFINITY, -INFINITY, -INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	MyTriangleList::const_iterator		i, e;
	for( i=polygons.begin(), e=polygons.end(); i!=e; i++ )
	{
		const Triangle& p = *i;

		for( int j=0; j<3; j++ )
		{
			if( p.vertices[j].x < ptMin.x ) ptMin.x = p.vertices[j].x;
			if( p.vertices[j].y < ptMin.y ) ptMin.y = p.vertices[j].y;
			if( p.vertices[j].z < ptMin.z ) ptMin.z = p.vertices[j].z;
			if( p.vertices[j].x > ptMax.x ) ptMax.x = p.vertices[j].x;
			if( p.vertices[j].y > ptMax.y ) ptMax.y = p.vertices[j].y;
			if( p.vertices[j].z > ptMax.z ) ptMax.z = p.vertices[j].z;
		}
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again, and calculate the radius of the sphere
	// Which is the largest magnitude of the vector from the center to each point
	for( i=polygons.begin(), e=polygons.end(); i!=e; i++ )
	{
		const Triangle& p = *i;

		for( int j=0; j<3; j++ ) {
			Vector3			r = Vector3Ops::mkVector3( p.vertices[j], ptCenter );
			const Scalar	d = Vector3Ops::Magnitude(r);

			if( d > radius ) {
				radius = d;
			}
		}
	}
}

BoundingBox TriangleMeshGeometry::GenerateBoundingBox( ) const
{
	if( bUseBSP && pPolygonsBSPtree ) {
		return pPolygonsBSPtree->GetBBox();
	} else if( pPolygonsOctree ) {
		return pPolygonsOctree->GetBBox();
	}
	
	return BoundingBox();
}

static const char * szSignature = "RISE_TMG";
static const unsigned int cur_version = 2;

void TriangleMeshGeometry::Serialize( IWriteBuffer& buffer ) const
{
	// stuff data into the buffer

	// first write out the signature and version
	buffer.setBytes( szSignature, 8 );
	buffer.setUInt( cur_version );
	
	// put octree settings
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( char ) );
	buffer.setUInt( nMaxPerOctantNode );
	buffer.setChar( nMaxRecursionLevel );

	// Now put geometry data
	{
		buffer.ResizeForMore( sizeof( Triangle ) * polygons.size() + sizeof( unsigned int ) );

		buffer.setUInt( polygons.size() );

		MyTriangleList::const_iterator		it;
		for( it=polygons.begin(); it!=polygons.end(); it++ ) {
			const Triangle&	tri = *it;

			for( int i=0; i<3; i++ ) {
				buffer.setDouble( tri.vertices[i].x );
				buffer.setDouble( tri.vertices[i].y );
				buffer.setDouble( tri.vertices[i].z );

				buffer.setDouble( tri.normals[i].x );
				buffer.setDouble( tri.normals[i].y );
				buffer.setDouble( tri.normals[i].z );

				buffer.setDouble( tri.coords[i].x );
				buffer.setDouble( tri.coords[i].y );
			}
		}
	}

	GlobalLog()->PrintEasyInfo( "TriangleMeshGeometry:: Begining Octree serialization" );

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bDoubleSided ? 1 : 0 );

	// Write out which octree exist
	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bUseBSP ? 1 : 0 );

	if( bUseBSP ) {
		buffer.ResizeForMore( sizeof( char ) * 2 );
		buffer.setChar( pPolygonsBSPtree ? 1 : 0 );

		// Now serialize the octree
		if( pPolygonsBSPtree ) {
			pPolygonsBSPtree->Serialize( buffer );
		}
	} else {
		buffer.ResizeForMore( sizeof( char ) * 2 );
		buffer.setChar( pPolygonsOctree ? 1 : 0 );

		// Now serialize the octree
		if( pPolygonsOctree ) {
			pPolygonsOctree->Serialize( buffer );
		}
	}

	// Thats it we are done!
}

void TriangleMeshGeometry::Deserialize( IReadBuffer& buffer )
{
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometry::Deserialize:: Begining deserialization process" );

	// First look for the triangle mesh geometry signature
	char sig[9] = {0};
	buffer.getBytes( sig, 8 );

	if( strcmp( sig, szSignature ) != 0 ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometry::Deserialize:: Signature not found.  Are you using an older format?" );
		return;
	}

	// Next check version
	const unsigned int version = buffer.getUInt();
	
	if( version != cur_version ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometry::Deserialize:: Versions don't match.  Are you using an older format?" );
		return;
	}

	// First get octree settings
	nMaxPerOctantNode = buffer.getUInt();
	nMaxRecursionLevel = buffer.getChar();

	polygons.clear();

	// Get the list of pure triangles
	{
		unsigned int numpolys = buffer.getUInt();
		if( numpolys > 0 ) {
			// Load the pure triangles
			polygons.reserve( numpolys );

			for( unsigned int i=0; i<numpolys; i++ ) {
				Triangle tri;

				for( unsigned int j=0; j<3; j++ ) {
					tri.vertices[j].x = buffer.getDouble();
					tri.vertices[j].y = buffer.getDouble();
					tri.vertices[j].z = buffer.getDouble();

					tri.normals[j].x = buffer.getDouble();
					tri.normals[j].y = buffer.getDouble();
					tri.normals[j].z = buffer.getDouble();

					tri.coords[j].x = buffer.getDouble();
					tri.coords[j].y = buffer.getDouble();
				}

				polygons.push_back( tri );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometry::Deserialize:: Read %d pure triangles", numpolys );
	}

	char bdoublesided = buffer.getChar();
	bDoubleSided = !!bdoublesided;
	GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometry::Deserialize:: Polygons are double sided? [%s]", bDoubleSided?"YES":"NO" );

	char bsp = buffer.getChar();
	bUseBSP = !!bsp;
		
	if( bUseBSP ) {
		// Deserialize the bsp trees
		const bool bpolybsptree = !!buffer.getChar();

		if( bpolybsptree ) {
			pPolygonsBSPtree = new BSPTree<const Triangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons bsptree" );

			// Deserialize
			pPolygonsOctree->Deserialize( buffer );
		}
	} else {
		// Deserialize the octrees
		const bool bpolyoctree = !!buffer.getChar();

		if( bpolyoctree ) {
			pPolygonsOctree = new Octree<const Triangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree" );

			// Deserialize
			pPolygonsOctree->Deserialize( buffer );
		}
	}

	ComputeAreas();

	// And we're done!
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometry::Deserialize:: Finished deserialization", bDoubleSided );
}


