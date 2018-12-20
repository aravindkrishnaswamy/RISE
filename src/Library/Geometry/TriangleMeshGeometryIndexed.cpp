//////////////////////////////////////////////////////////////////////
//
//  TriangleMeshGeometryIndexed.cpp - Implementation of the TriangleMesh
//  GeometryIndexed class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 2, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TriangleMeshGeometryIndexed.h"
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

#include "TriangleMeshGeometryIndexedSpecializations.h"

TriangleMeshGeometryIndexed::TriangleMeshGeometryIndexed(
	const unsigned int max_polys_per_node, 
	const unsigned char max_recursion_level, 
	const bool bDoubleSided_,
	const bool bUseBSP_,
	const bool bUseFaceNormals_
	) :
  nMaxPerOctantNode( max_polys_per_node ),
  nMaxRecursionLevel( max_recursion_level), 
  bDoubleSided( bDoubleSided_ ),
  bUseBSP( bUseBSP_ ),
  bUseFaceNormals( bUseFaceNormals_ ),
  pPtrOctree( 0 ),
  pPtrBSPtree( 0 )
{
}

TriangleMeshGeometryIndexed::~TriangleMeshGeometryIndexed()
{
	safe_release( pPtrOctree );
	safe_release( pPtrBSPtree );
}

void TriangleMeshGeometryIndexed::GenerateMesh( )
{
	// Hmmm....  that can't be too hard now can it ? <snicker>
}

void TriangleMeshGeometryIndexed::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
	// Triangle mesh geometry never generates exit information, it just ignores that command!

	if( bUseBSP && pPtrBSPtree ) {
		pPtrBSPtree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	} else if( pPtrOctree ) {
		pPtrOctree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	if( ri.bHit && bDoubleSided ) {
		// Flip the normal if we must
		if( Vector3Ops::Dot(ri.vNormal, ri.ray.dir) > 0 ) {
			ri.vNormal = -ri.vNormal;
		}
	}
}

bool TriangleMeshGeometryIndexed::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( bUseBSP && pPtrBSPtree ) {
		return pPtrBSPtree->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	} else if( pPtrOctree ) {
		return pPtrOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	return false;
}

void TriangleMeshGeometryIndexed::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// Find the desired triangle where the CDF is greater than the rand value
	TriangleAreasList::const_iterator it = std::lower_bound( areasCDF.begin(), areasCDF.end(), prand.z );

	int idx = areasCDF.size()-1;
	if( it != areasCDF.end() ) {
		idx = std::distance( areasCDF.begin(), it );
	}

	GeometricUtilities::PointOnTriangle( point, normal, coord, ptr_polygons[idx], prand.x, prand.y );
}

Scalar TriangleMeshGeometryIndexed::GetArea( ) const
{
	return totalArea;
}

void TriangleMeshGeometryIndexed::BeginIndexedTriangles( )
{
	safe_release( pPtrOctree );
	safe_release( pPtrBSPtree );
	areas.clear();
	areasCDF.clear();
}

void TriangleMeshGeometryIndexed::AddVertex( const Point3& point )
{
	pPoints.push_back( point );
}

void TriangleMeshGeometryIndexed::AddNormal( const Vector3& normal )
{
	if( !bUseFaceNormals ) {
		pNormals.push_back( normal );
	}
}

void TriangleMeshGeometryIndexed::AddTexCoord( const Point2& coord )
{
	pCoords.push_back( coord );
}

void TriangleMeshGeometryIndexed::AddVertices( const VerticesListType& points )
{
	pPoints.insert( pPoints.end(), points.begin(), points.end() );
}

void TriangleMeshGeometryIndexed::AddNormals( const NormalsListType& normals )
{
	if( !bUseFaceNormals ) {
		pNormals.insert( pNormals.end(), normals.begin(), normals.end() );
	}
}

void TriangleMeshGeometryIndexed::AddTexCoords( const TexCoordsListType& coords )
{
	pCoords.insert( pCoords.end(), coords.begin(), coords.end() );
}

void TriangleMeshGeometryIndexed::AddIndexedTriangle( const IndexedTriangle& tri )
{
	indexedtris.push_back( tri );
}

void TriangleMeshGeometryIndexed::AddIndexedTriangles( const IndexTriangleListType& tris )
{
	indexedtris.insert( indexedtris.end(), tris.begin(), tris.end() );
}

void TriangleMeshGeometryIndexed::ComputeAreas()
{
	// Compute triangle areas
	totalArea = 0;
	{
		MyPointerTriangleList::const_iterator i, e;
		for( i=ptr_polygons.begin(), e=ptr_polygons.end(); i!=e; i++ ) {
			const PointerTriangle&	thisTri = (*i);
			Vector3 vEdgeA = Vector3Ops::mkVector3( *thisTri.pVertices[1], *thisTri.pVertices[0] );
			Vector3 vEdgeB = Vector3Ops::mkVector3( *thisTri.pVertices[2], *thisTri.pVertices[0] );
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

void TriangleMeshGeometryIndexed::DoneIndexedTriangles( )
{
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed:: Constructing acceleration structures for %u triangles", indexedtris.size() );
	{
		// Optimize the point, normal and coord lists
		// so that they only use the amount of memory they require
		stl_utils::container_optimize< VerticesListType >( pPoints );
		if( bUseFaceNormals ) {
			stl_utils::container_erase_all< NormalsListType >( pNormals );
		} else {
			stl_utils::container_optimize< NormalsListType >( pNormals );
		}
		stl_utils::container_optimize< TexCoordsListType >( pCoords );
	}

	// Convert that indexed triangle into a pointer triangle
	{
		IndexTriangleListType::const_iterator	i, e;
		
		for( i=indexedtris.begin(), e=indexedtris.end(); i!=e; i++ )
		{
			const IndexedTriangle& tri = *i;

			PointerTriangle		myTri;

			for( int i=0; i<3; i++ )
			{
#ifdef _DEBUG
				// Check integrity
				if( tri.iVertices[i] >= pPoints.size() ||
					tri.iNormals[i] >= pNormals.size() || 
					tri.iCoords[i] >= pCoords.size()
					)
				{
					GlobalLog()->PrintEasyError( "Bad index on IndexedTriangle, bad things will happen" );
#ifdef WIN32
					// break here
					_asm int 3h;
#endif
				}
#endif

				myTri.pVertices[i] = &pPoints[tri.iVertices[i]];
				if( !bUseFaceNormals ) {
					myTri.pNormals[i] = &pNormals[tri.iNormals[i]];
				} else {
					myTri.pNormals[i] = 0;
				}
				myTri.pCoords[i] = &pCoords[tri.iCoords[i]];
			}

			ptr_polygons.push_back( myTri );
		}

		// Blow away the index triangle
		indexedtris.clear();
		stl_utils::container_erase_all< IndexTriangleListType >( indexedtris );

	}


	// We're done with all the triangles so stuff it all into an octree
	// First compute the bounds of the octree
	BoundingBox bbox( Point3( INFINITY, INFINITY, INFINITY ), Point3( -INFINITY, -INFINITY, -INFINITY ) );

	std::vector<const PointerTriangle*>	temp;

	{
		MyPointerTriangleList::iterator i, e;
		for( i=ptr_polygons.begin(), e=ptr_polygons.end(); i!=e; i++ ) {
			PointerTriangle&	mytri = (*i);
			temp.push_back( &mytri );
		}

		MyPointsList::const_iterator		m, n;
		for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ ) {
			bbox.Include( *m );
		}
	}

	if( bUseBSP ) {	
		safe_release( pPtrBSPtree );

		pPtrBSPtree = new BSPTree<const PointerTriangle*>( *this, bbox, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pPtrBSPtree, __FILE__, __LINE__, "pointers bsptree" );

		pPtrBSPtree->AddElements( temp, nMaxRecursionLevel );

//		pPtrBSPtree->DumpStatistics( eLog_Info );
	} else {
		safe_release( pPtrOctree );

		pPtrOctree = new Octree<const PointerTriangle*>( *this, bbox, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pPtrOctree, __FILE__, __LINE__, "pointers octree" );

		pPtrOctree->AddElements( temp, nMaxRecursionLevel );

//		pPtrOctree->DumpStatistics( eLog_Info );
	}

	ComputeAreas();
}

void TriangleMeshGeometryIndexed::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( INFINITY, INFINITY, INFINITY );
	Point3	ptMax( -INFINITY, -INFINITY, -INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	MyPointsList::const_iterator m, n;
	for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ )
	{
		const Point3&	pt = *m;
		if( pt.x < ptMin.x ) ptMin.x = pt.x;
		if( pt.y < ptMin.y ) ptMin.y = pt.y;
		if( pt.z < ptMin.z ) ptMin.z = pt.z;
		if( pt.x > ptMax.x ) ptMax.x = pt.x;
		if( pt.y > ptMax.y ) ptMax.y = pt.y;
		if( pt.z > ptMax.z ) ptMax.z = pt.z;
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again and calculate the radius of the sphere
	for( m=pPoints.begin(), n=pPoints.end(); m!=n; m++ ) {
		Vector3			r = Vector3Ops::mkVector3( *m, ptCenter );
		const Scalar	d = Vector3Ops::Magnitude(r);

		if( d > radius ) {
			radius = d;
		}
	}
}

BoundingBox TriangleMeshGeometryIndexed::GenerateBoundingBox( ) const
{
	if( bUseBSP && pPtrBSPtree ) {
		return pPtrBSPtree->GetBBox();
	} else if( pPtrOctree ) {
		return pPtrOctree->GetBBox();
	}
	
	return BoundingBox();
}

static const char * szSignature = "RISETMGI";
static const unsigned int cur_version = 1;

void TriangleMeshGeometryIndexed::Serialize( IWriteBuffer& buffer ) const
{
	// stuff data into the buffer

	// first write out the signature and version
	buffer.setBytes( szSignature, 8 );
	buffer.setUInt( cur_version );
	
	// put octree settings
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( char ) );
	buffer.setUInt( nMaxPerOctantNode );
	buffer.setChar( nMaxRecursionLevel );

	// put geometry settings
	buffer.setChar( bUseFaceNormals ? 1 : 0 );

	// Now put geometry data
	
	// List of points 
	{
		buffer.ResizeForMore( sizeof(Vertex)*pPoints.size() + sizeof( unsigned int ) );

		buffer.setUInt( pPoints.size() );

		MyPointsList::const_iterator	it;
		for( it=pPoints.begin(); it!=pPoints.end(); it++ ) {
			const Vertex&	v = *it;
			buffer.setDouble( v.x );
			buffer.setDouble( v.y );
			buffer.setDouble( v.z );
		}
	}

	// List of normals
	{
		buffer.ResizeForMore( sizeof(Normal)*pNormals.size() + sizeof( unsigned int ) );

		buffer.setUInt( pNormals.size() );

		MyNormalsList::const_iterator	it;
		for( it=pNormals.begin(); it!=pNormals.end(); it++ ) {
			const Normal&	n = *it;
			buffer.setDouble( n.x );
			buffer.setDouble( n.y );
			buffer.setDouble( n.z );
		}
	}

	// List of texture co-ordinates
	{
		buffer.ResizeForMore( sizeof(Normal)*pCoords.size() + sizeof( unsigned int ) );

		buffer.setUInt( pCoords.size() );

		MyCoordsList::const_iterator	it;
		for( it=pCoords.begin(); it!=pCoords.end(); it++ ) {
			const TexCoord&	c = *it;
			buffer.setDouble( c.x );
			buffer.setDouble( c.y );
		}
	}

	// List of pointer polygons (convert them to indexed polygons!)
	{
		buffer.ResizeForMore( sizeof( IndexedTriangle ) * ptr_polygons.size() + sizeof( unsigned int ) );

		buffer.setUInt( ptr_polygons.size() );

		// Do pointer arithmetic to do the conversion
		// NOTE: this only works if the points, normals and coords list
		//  are all vectors
		unsigned int vertex_ptr_begin = VoidPtrToUInt( (void*)&(*(pPoints.begin())) );
		unsigned int normal_ptr_begin = VoidPtrToUInt( (void*)&(*(pNormals.begin())) );
		unsigned int coord_ptr_begin = VoidPtrToUInt( (void*)&(*(pCoords.begin())) );

		MyPointerTriangleList::const_iterator it;
		for( it=ptr_polygons.begin(); it != ptr_polygons.end(); it++ ) {
			const PointerTriangle& ptrtri = *it;

			for( int i=0; i<3; i++ ) {
				// For each of the vertices take the pointer off set, subtract by
				// begining and divide by the size of the type
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pVertices[i]))-vertex_ptr_begin) / sizeof( Vertex ) );
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pNormals[i]))-normal_ptr_begin) / sizeof( Normal ) );
				buffer.setUInt( ((VoidPtrToUInt(ptrtri.pCoords[i]))-coord_ptr_begin) / sizeof( TexCoord ) );
			}
		}
	}

	GlobalLog()->PrintEasyInfo( "TriangleMeshGeometryIndexed:: Begining Octree serialization" );

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bDoubleSided ? 1 : 0 );

	// Write out which octree exist
	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bUseBSP ? 1 : 0 );

	if( bUseBSP ) {
		buffer.ResizeForMore( sizeof( char ) * 2 );
		buffer.setChar( pPtrBSPtree ? 1 : 0 );

		// Now serialize the bsp tree
		if( pPtrBSPtree ) {
			pPtrBSPtree->Serialize( buffer );
		}
	} else {
		buffer.ResizeForMore( sizeof( char ) * 2 );
		buffer.setChar( pPtrOctree ? 1 : 0 );

		// Now serialize the octree
		if( pPtrOctree ) {
			pPtrOctree->Serialize( buffer );
		}
	}

	// Thats it we are done!
}

void TriangleMeshGeometryIndexed::Deserialize( IReadBuffer& buffer )
{
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Begining deserialization process" );

	// First look for the triangle mesh geometry signature
	char sig[9] = {0};
	buffer.getBytes( sig, 8 );

	if( strcmp( sig, szSignature ) != 0 ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometryIndexed::Deserialize:: Signature not found.  Are you using an older format?" );
		return;
	}

	// Next check version
	const unsigned int version = buffer.getUInt();
	
	if( version != cur_version ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometryIndexed::Deserialize:: Versions don't match.  Are you using an older format?" );
		return;
	}

	// First get octree settings
	nMaxPerOctantNode = buffer.getUInt();
	nMaxRecursionLevel = buffer.getChar();

	// Get geometry settings
	bUseFaceNormals = !!buffer.getChar();

	pPoints.clear();
	pNormals.clear();
	pCoords.clear();
	ptr_polygons.clear();

	// Now get the list of points
	{
		unsigned int numpts = buffer.getUInt();
		if( numpts > 0 ) {
			// Load all the points
			pPoints.reserve( numpts );

			for( unsigned int i=0; i<numpts; i++ ) {
				Vertex	v;
				v.x = buffer.getDouble();
				v.y = buffer.getDouble();
				v.z = buffer.getDouble();
				pPoints.push_back( v );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d points", numpts );
	}

	// Get the list of normals
	{
		unsigned int numnormals = buffer.getUInt();
		if( numnormals > 0 ) {
			// Load all the normals
			pNormals.reserve( numnormals );
			for( unsigned int i=0; i<numnormals; i++ ) {
				Normal n;
				n.x = buffer.getDouble();
				n.y = buffer.getDouble();
				n.z = buffer.getDouble();
				pNormals.push_back( n );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d normals", numnormals );
	}

	if( bUseFaceNormals ) {
		stl_utils::container_erase_all< NormalsListType >( pNormals );
	}

	// Get the list of co-ordinates
	{
		unsigned int numcoords = buffer.getUInt();
		if( numcoords > 0 ) {
			// Load all the coords
			pCoords.reserve( numcoords );
			for( unsigned int i=0; i<numcoords; i++ ) {
				TexCoord tc;
				tc.x = buffer.getDouble();
				tc.y = buffer.getDouble();
				pCoords.push_back( tc );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d texture co-ordinates", numcoords );
	}

	// Get the list of indexed triangles, convert them to "pointer polygons"
	{
		unsigned int numptrpolys = buffer.getUInt();
		if( numptrpolys > 0 ) {
			// Load the pointer polygons
			ptr_polygons.reserve( numptrpolys );

			for( unsigned int j=0; j<numptrpolys; j++ ) {
				PointerTriangle		ptrtri;

				for( unsigned int i=0; i<3; i++ ) {
					ptrtri.pVertices[i] = &pPoints[buffer.getUInt()];
					
					unsigned int normal_id = buffer.getUInt();
					if( bUseFaceNormals || pNormals.size()==0 ) {
						ptrtri.pNormals[i] = 0;
					} else {
						ptrtri.pNormals[i] = &pNormals[normal_id];
					}

					ptrtri.pCoords[i] = &pCoords[buffer.getUInt()];
				}

				ptr_polygons.push_back( ptrtri );
			}
		}

		GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Read %d pointer polygons", numptrpolys );
	}

	char bdoublesided = buffer.getChar();
	bDoubleSided = !!bdoublesided;
	GlobalLog()->PrintEx( eLog_Info, "  TriangleMeshGeometryIndexed::Deserialize:: Polygons are double sided? [%s]", bDoubleSided?"YES":"NO" );

	char bsp = buffer.getChar();
	bUseBSP = !!bsp;
		
	if( bUseBSP ) {
		// Deserialize the bsp trees
		const bool bptrbsptree = !!buffer.getChar();

		if( bptrbsptree ) {
			pPtrBSPtree = new BSPTree<const PointerTriangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPtrBSPtree, __FILE__, __LINE__, "pointers bsptree" );

			// Deserialize
			pPtrBSPtree->Deserialize( buffer );
		}
	} else {
		// Deserialize the octrees
		const bool bptroctree = !!buffer.getChar();

		if( bptroctree ) {
			pPtrOctree = new Octree<const PointerTriangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPtrOctree, __FILE__, __LINE__, "pointers octree" );

			// Deserialize
			pPtrOctree->Deserialize( buffer );
		}
	}

	ComputeAreas();

	// And we're done!
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Finished deserialization", bDoubleSided );
}

void TriangleMeshGeometryIndexed::ComputeVertexNormals()
{
	pNormals.clear();
	pNormals.reserve( pPoints.size() );
	CalculateVertexNormals( indexedtris, pNormals, pPoints );
}


