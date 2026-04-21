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
#include "../Utilities/OrthonormalBasis3D.h"
#include "GeometryUtilities.h"
#include "../Utilities/stl_utils.h"
#include <cmath>
#ifdef RISE_ENABLE_MAILBOXING
#include <atomic>
#include <unordered_map>
#endif

inline unsigned int VoidPtrToUInt( const void* v )
{
	return (unsigned int)*((unsigned int*)(&v));
}

using namespace RISE;
using namespace RISE::Implementation;

#ifdef RISE_ENABLE_MAILBOXING
namespace {
	static std::atomic<unsigned int> s_nextGeometryId(1);

	struct MailboxState {
		unsigned int rayId;
		std::vector<unsigned int> stamps;
	};

	static thread_local std::unordered_map<unsigned int, MailboxState> tl_mailboxes;

	static inline MailboxState& GetMailbox(unsigned int geomId, size_t numTris)
	{
		MailboxState& mb = tl_mailboxes[geomId];
		if( mb.stamps.size() < numTris ) {
			mb.stamps.assign(numTris, 0);
			mb.rayId = 0;
		}
		return mb;
	}
}
#endif

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
#ifdef RISE_ENABLE_MAILBOXING
  , geometryId( s_nextGeometryId.fetch_add(1) )
#endif
{
}

TriangleMeshGeometryIndexed::~TriangleMeshGeometryIndexed()
{
	safe_release( pPtrOctree );
	safe_release( pPtrBSPtree );
}

bool TriangleMeshGeometryIndexed::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/ ) const
{
	// Pass-through: emit triangles with same-index (iVertices == iNormals == iCoords)
	// semantics.  The source mesh stores pPoints, pNormals, pCoords as INDEPENDENTLY
	// indexed attribute arrays (a position can be shared across faces with distinct
	// normals/UVs), but downstream displacement code (`ApplyDisplacementMapToObject`)
	// indexes vNormals and vCoords by the VERTEX index — so we flatten each triangle
	// corner to its own (pos, normal, uv) tuple here.  Also handles face-normal source
	// meshes, which store no normals (pNormals empty, src.pNormals[k]==null): we emit a
	// placeholder (0,0,1) that the caller's RecomputeVertexNormalsFromTopology will
	// overwrite.
	//
	// DoneIndexedTriangles() clears indexedtris after converting to pointer-triangle
	// form, so ptr_polygons is the authoritative source.
	if( ptr_polygons.empty() ) {
		return false;
	}

	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );

	for( MyPointerTriangleList::const_iterator it = ptr_polygons.begin(); it != ptr_polygons.end(); ++it ) {
		const PointerTriangle& src = *it;

		// For face-normals source meshes, pNormals is empty and every src.pNormals[k]
		// is null.  We can't emit a constant placeholder like +Z — downstream code
		// (DisplacedGeometry) displaces vertices ALONG the stored normal BEFORE any
		// later normal recompute, so a constant placeholder would push every corner
		// along +Z regardless of the triangle's actual orientation.  Compute the
		// true face normal from the triangle's vertex positions and emit it for
		// all three corners.  Guarded against degenerate triangles (zero-area).
		Vector3 faceNormal( 0.0, 0.0, 1.0 );
		if( !src.pNormals[0] ) {
			const Vector3 e1 = Vector3Ops::mkVector3( *src.pVertices[1], *src.pVertices[0] );
			const Vector3 e2 = Vector3Ops::mkVector3( *src.pVertices[2], *src.pVertices[0] );
			const Vector3 cross = Vector3Ops::Cross( e1, e2 );
			const Scalar mag = Vector3Ops::Magnitude( cross );
			if( mag > NEARZERO ) {
				faceNormal = cross * (1.0 / mag);
			}
		}

		IndexedTriangle t;
		for( int k = 0; k < 3; k++ ) {
			const unsigned int localIdx = static_cast<unsigned int>( vertices.size() );
			vertices.push_back( *src.pVertices[k] );
			normals.push_back( src.pNormals[k] ? *src.pNormals[k] : faceNormal );
			coords.push_back( *src.pCoords[k] );
			t.iVertices[k] = localIdx;
			t.iNormals[k]  = localIdx;
			t.iCoords[k]   = localIdx;
		}
		tris.push_back( t );
	}

	(void)baseIdx;  // keep variable visible in logs / future debugging
	return true;
}

void TriangleMeshGeometryIndexed::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool /*bComputeExitInfo*/ ) const
{
	// Triangle mesh geometry never generates exit information, it just ignores that command!

	// Bump the mailbox ray ID so duplicate triangles in multiple BSP leaves are skipped
#ifdef RISE_ENABLE_MAILBOXING
	{
		MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
		++mb.rayId;
	}
#endif

	if( bUseBSP && pPtrBSPtree ) {
		pPtrBSPtree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	} else if( pPtrOctree ) {
		pPtrOctree->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}

	if( ri.bHit && bDoubleSided ) {
		// Flip the normal if we must
		if( Vector3Ops::Dot(ri.vNormal, ri.ray.Dir()) > 0 ) {
			ri.vNormal = -ri.vNormal;
		}
	}
}

bool TriangleMeshGeometryIndexed::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
#ifdef RISE_ENABLE_MAILBOXING
	{
		MailboxState& mb = GetMailbox(geometryId, ptr_polygons.size());
		++mb.rayId;
	}
#endif

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

	int idx = static_cast<int>(areasCDF.size())-1;
	if( it != areasCDF.end() ) {
		idx = static_cast<int>(std::distance( areasCDF.begin(), it ));
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
					__debugbreak();
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
	BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );

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

		pPtrBSPtree = new BSPTreeSAH<const PointerTriangle*>( *this, bbox, nMaxPerOctantNode );
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
	Point3	ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3	ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ;

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
static const unsigned int cur_version = 2;

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
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Vertex)*pPoints.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pPoints.size()) );

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
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Normal)*pNormals.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pNormals.size()) );

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
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof(Normal)*pCoords.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(pCoords.size()) );

		MyCoordsList::const_iterator	it;
		for( it=pCoords.begin(); it!=pCoords.end(); it++ ) {
			const TexCoord&	c = *it;
			buffer.setDouble( c.x );
			buffer.setDouble( c.y );
		}
	}

	// List of pointer polygons (convert them to indexed polygons!)
	{
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof( IndexedTriangle ) * ptr_polygons.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(ptr_polygons.size()) );

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
	
	if( version != 1 && version != cur_version ) {
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
	safe_release( pPtrOctree );
	safe_release( pPtrBSPtree );

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
		
	// Try to deserialize the spatial acceleration structure, but validate 
	// the bounding box afterwards. If it's corrupted, rebuild from scratch.
	bool bTreeValid = false;

	if( bUseBSP ) {
		// Deserialize the bsp trees
		const bool bptrbsptree = !!buffer.getChar();

		if( bptrbsptree ) {
			if( version == cur_version ) {
				pPtrBSPtree = new BSPTreeSAH<const PointerTriangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
				GlobalLog()->PrintNew( pPtrBSPtree, __FILE__, __LINE__, "pointers bsptree" );

				// Deserialize
				pPtrBSPtree->Deserialize( buffer );

				// Validate the deserialized bounding box
				BoundingBox treeBBox = pPtrBSPtree->GetBBox();
				Vector3 extents = treeBBox.GetExtents();
				if( std::isfinite(treeBBox.ll.x) && std::isfinite(treeBBox.ll.y) && std::isfinite(treeBBox.ll.z) &&
					std::isfinite(treeBBox.ur.x) && std::isfinite(treeBBox.ur.y) && std::isfinite(treeBBox.ur.z) &&
					std::abs(extents.x) < 1e10 && std::abs(extents.y) < 1e10 && std::abs(extents.z) < 1e10 )
				{
					bTreeValid = true;
				} else {
					GlobalLog()->PrintEasyWarning( "TriangleMeshGeometryIndexed::Deserialize:: Deserialized BSP tree has invalid bounding box, will rebuild" );
					safe_release( pPtrBSPtree );
					pPtrBSPtree = 0;
				}
			} else {
				GlobalLog()->PrintEasyWarning( "TriangleMeshGeometryIndexed::Deserialize:: Legacy BSP serialization detected, rebuilding SAH tree from polygon data" );
			}
		}
	} else {
		// Deserialize the octrees
		const bool bptroctree = !!buffer.getChar();

		if( bptroctree ) {
			pPtrOctree = new Octree<const PointerTriangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPtrOctree, __FILE__, __LINE__, "pointers octree" );

			// Deserialize
			pPtrOctree->Deserialize( buffer );

			// Validate the deserialized bounding box
			BoundingBox treeBBox = pPtrOctree->GetBBox();
			Vector3 extents = treeBBox.GetExtents();
			if( std::isfinite(treeBBox.ll.x) && std::isfinite(treeBBox.ll.y) && std::isfinite(treeBBox.ll.z) &&
				std::isfinite(treeBBox.ur.x) && std::isfinite(treeBBox.ur.y) && std::isfinite(treeBBox.ur.z) &&
				std::abs(extents.x) < 1e10 && std::abs(extents.y) < 1e10 && std::abs(extents.z) < 1e10 )
			{
				bTreeValid = true;
			} else {
				GlobalLog()->PrintEasyWarning( "TriangleMeshGeometryIndexed::Deserialize:: Deserialized Octree has invalid bounding box, will rebuild" );
				safe_release( pPtrOctree );
				pPtrOctree = 0;
			}
		}
	}

	// If the deserialized tree was invalid, rebuild from the loaded polygon data
	if( !bTreeValid && ptr_polygons.size() > 0 ) {
		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Rebuilding spatial structure from %u polygons", ptr_polygons.size() );

		// Compute bounding box from vertex data
		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		std::vector<const PointerTriangle*> temp;

		MyPointerTriangleList::iterator pi, pe;
		for( pi=ptr_polygons.begin(), pe=ptr_polygons.end(); pi!=pe; pi++ ) {
			temp.push_back( &(*pi) );
		}

		MyPointsList::const_iterator mi, mn;
		for( mi=pPoints.begin(), mn=pPoints.end(); mi!=mn; mi++ ) {
			bbox.Include( *mi );
		}

		if( bUseBSP ) {
			pPtrBSPtree = new BSPTreeSAH<const PointerTriangle*>( *this, bbox, nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPtrBSPtree, __FILE__, __LINE__, "pointers bsptree (rebuilt)" );
			pPtrBSPtree->AddElements( temp, nMaxRecursionLevel );
		} else {
			pPtrOctree = new Octree<const PointerTriangle*>( *this, bbox, nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPtrOctree, __FILE__, __LINE__, "pointers octree (rebuilt)" );
			pPtrOctree->AddElements( temp, nMaxRecursionLevel );
		}

		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometryIndexed::Deserialize:: Spatial structure rebuilt successfully" );
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

// Derivatives for a triangle in barycentric parameterization.
//
// Parameterize the triangle by p(u,v) = v0 + u*(v1-v0) + v*(v2-v0) with
// (u, v) in the standard barycentric domain {(u,v) : u,v >= 0, u+v <= 1}.
// Then dp/du and dp/dv are the triangle edge vectors e1 = v1-v0 and
// e2 = v2-v0 respectively — constant across the triangle, so the triangle
// is actually a flat patch in parameter space.
//
// For smooth-shaded triangles the normal is barycentric-interpolated:
//   N_raw(u,v) = n0 + u*(n1-n0) + v*(n2-n0)
//   N(u,v)     = N_raw / |N_raw|
// Differentiating the normalized expression gives:
//   dN/du = (dN_raw/du - N*dot(N, dN_raw/du)) / |N_raw|
//
// The returned (dpdu, dpdv, n) may be LEFT-HANDED depending on the
// triangle winding vs its per-vertex normals.  We flip dpdv's sign (and
// dndv accordingly) if needed so the frame is always right-handed, per
// docs/GEOMETRY_DERIVATIVES.md.
static SurfaceDerivatives ComputeTriangleDerivatives(
	const PointerTriangle& tri,
	const Vector3& objSpaceNormal,
	const Point3& objSpacePoint,
	const Scalar barU,
	const Scalar barV,
	bool hasVertexNormals )
{
	SurfaceDerivatives sd;
	sd.valid = true;

	// Strategy (mirrors TriangleMeshGeometryIndexedSpecializations.h):
	// invert the 2×2 UV Jacobian using the stored per-vertex texture
	// coordinates so derivatives come out in the stored parameterisation.
	// For TessellateToMesh-produced meshes (sphere, torus, ellipsoid,
	// cylinder) those UVs ARE the analytical shape's own (u,v), so the
	// frame is aligned with the analytical parameterisation and scales
	// match the analytical |dpdu|, |dpdv|.
	//
	// Fall back to the barycentric-edge frame when UVs are missing or
	// degenerate (|det|<ε: pole triangle or collinear-UV triangle).
	const Vector3 e1 = Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] );
	const Vector3 e2 = Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] );

	bool useUVJacobian = false;
	Scalar uvDet = 0.0;
	Scalar duA = 0.0, duB = 0.0, dvA = 0.0, dvB = 0.0;
	if( tri.pCoords[0] && tri.pCoords[1] && tri.pCoords[2] ) {
		duA = tri.pCoords[1]->x - tri.pCoords[0]->x;
		duB = tri.pCoords[2]->x - tri.pCoords[0]->x;
		dvA = tri.pCoords[1]->y - tri.pCoords[0]->y;
		dvB = tri.pCoords[2]->y - tri.pCoords[0]->y;
		uvDet = duA * dvB - duB * dvA;
		if( fabs( uvDet ) > NEARZERO ) {
			useUVJacobian = true;
		}
	}

	Vector3 dpdu_raw, dpdv_raw;
	if( useUVJacobian ) {
		const Scalar invDet = 1.0 / uvDet;
		dpdu_raw = Vector3(
			( e1.x * dvB - e2.x * dvA ) * invDet,
			( e1.y * dvB - e2.y * dvA ) * invDet,
			( e1.z * dvB - e2.z * dvA ) * invDet );
		dpdv_raw = Vector3(
			( e2.x * duA - e1.x * duB ) * invDet,
			( e2.y * duA - e1.y * duB ) * invDet,
			( e2.z * duA - e1.z * duB ) * invDet );
	} else {
		dpdu_raw = e1;
		dpdv_raw = e2;
	}

	// Project dpdu, dpdv into the shading-normal tangent plane so
	// dpdu·n ≈ 0 even for smooth-shaded triangles whose shading normal
	// differs from the face normal.  (See docs/GEOMETRY_DERIVATIVES.md.)
	const Scalar dpdu_dot_n = Vector3Ops::Dot( dpdu_raw, objSpaceNormal );
	const Scalar dpdv_dot_n = Vector3Ops::Dot( dpdv_raw, objSpaceNormal );
	sd.dpdu = Vector3(
		dpdu_raw.x - objSpaceNormal.x * dpdu_dot_n,
		dpdu_raw.y - objSpaceNormal.y * dpdu_dot_n,
		dpdu_raw.z - objSpaceNormal.z * dpdu_dot_n );
	sd.dpdv = Vector3(
		dpdv_raw.x - objSpaceNormal.x * dpdv_dot_n,
		dpdv_raw.y - objSpaceNormal.y * dpdv_dot_n,
		dpdv_raw.z - objSpaceNormal.z * dpdv_dot_n );

	if( hasVertexNormals && tri.pNormals[0] && tri.pNormals[1] && tri.pNormals[2] ) {
		// Raw normal-diff derivatives (in barycentric (A, B)).
		const Vector3 dNraw_dA = Vector3(
			tri.pNormals[1]->x - tri.pNormals[0]->x,
			tri.pNormals[1]->y - tri.pNormals[0]->y,
			tri.pNormals[1]->z - tri.pNormals[0]->z );
		const Vector3 dNraw_dB = Vector3(
			tri.pNormals[2]->x - tri.pNormals[0]->x,
			tri.pNormals[2]->y - tri.pNormals[0]->y,
			tri.pNormals[2]->z - tri.pNormals[0]->z );

		// Apply the same UV-Jacobian inversion to the normal derivatives
		// as to dpdu/dpdv, so the resulting dN/du, dN/dv live in the
		// SAME parameterisation as dpdu/dpdv.
		Vector3 dNraw_du, dNraw_dv;
		if( useUVJacobian ) {
			const Scalar invDet = 1.0 / uvDet;
			dNraw_du = Vector3(
				( dNraw_dA.x * dvB - dNraw_dB.x * dvA ) * invDet,
				( dNraw_dA.y * dvB - dNraw_dB.y * dvA ) * invDet,
				( dNraw_dA.z * dvB - dNraw_dB.z * dvA ) * invDet );
			dNraw_dv = Vector3(
				( dNraw_dB.x * duA - dNraw_dA.x * duB ) * invDet,
				( dNraw_dB.y * duA - dNraw_dA.y * duB ) * invDet,
				( dNraw_dB.z * duA - dNraw_dA.z * duB ) * invDet );
		} else {
			dNraw_du = dNraw_dA;
			dNraw_dv = dNraw_dB;
		}

		// Reconstruct |N_raw| at this (u,v) using the interpolated
		// unnormalized normal. The caller-supplied objSpaceNormal is
		// already normalized, so we can use the raw barycentric
		// interpolation (using dNraw_dA, dNraw_dB, the original basis —
		// barU/barV are in barycentric space, not (u,v) space).
		const Vector3 Nraw = Vector3(
			tri.pNormals[0]->x + barU * dNraw_dA.x + barV * dNraw_dB.x,
			tri.pNormals[0]->y + barU * dNraw_dA.y + barV * dNraw_dB.y,
			tri.pNormals[0]->z + barU * dNraw_dA.z + barV * dNraw_dB.z );
		const Scalar len = Vector3Ops::Magnitude( Nraw );
		const Scalar invLen = (len > NEARZERO) ? 1.0 / len : 0.0;

		// dN/d* = projection perpendicular to N, divided by |N_raw|.
		// Use the caller-supplied objSpaceNormal (which is the shading
		// normal the RISE runtime attached to this hit), not the
		// reconstructed N, so that our dn/d* is rigorously perpendicular
		// to the same normal a downstream consumer will use.
		const Scalar dot_u = Vector3Ops::Dot( objSpaceNormal, dNraw_du );
		const Scalar dot_v = Vector3Ops::Dot( objSpaceNormal, dNraw_dv );
		sd.dndu = Vector3(
			(dNraw_du.x - objSpaceNormal.x * dot_u) * invLen,
			(dNraw_du.y - objSpaceNormal.y * dot_u) * invLen,
			(dNraw_du.z - objSpaceNormal.z * dot_u) * invLen );
		sd.dndv = Vector3(
			(dNraw_dv.x - objSpaceNormal.x * dot_v) * invLen,
			(dNraw_dv.y - objSpaceNormal.y * dot_v) * invLen,
			(dNraw_dv.z - objSpaceNormal.z * dot_v) * invLen );
	} else {
		// Face-normal mesh: flat face, no per-vertex normal variation
		sd.dndu = Vector3( 0, 0, 0 );
		sd.dndv = Vector3( 0, 0, 0 );
	}

	// Handedness: (dpdu × dpdv) · n must be > 0.  If the triangle
	// winding gave the wrong sign w.r.t. the shading normal, flip dpdv
	// and dndv.
	const Vector3 cross = Vector3Ops::Cross( sd.dpdu, sd.dpdv );
	if( Vector3Ops::Dot( cross, objSpaceNormal ) < 0.0 ) {
		sd.dpdv = Vector3( -sd.dpdv.x, -sd.dpdv.y, -sd.dpdv.z );
		sd.dndv = Vector3( -sd.dndv.x, -sd.dndv.y, -sd.dndv.z );
	}

	sd.uv = Point2( barU, barV );
	return sd;
}

// Compute barycentric coords of a point in a triangle (ignoring
// out-of-plane component).  Returns (u, v) such that
//   p ≈ v0 + u*(v1-v0) + v*(v2-v0).
// `inside` is set to true iff (u,v) is inside the triangle (with
// tolerance epsTol).  The "containment" test is the only way to
// disambiguate which triangle a point belongs to.
static void BarycentricCoords(
	const Point3& v0, const Point3& v1, const Point3& v2,
	const Point3& p,
	Scalar& u, Scalar& v, bool& inside,
	const Scalar epsTol = 1e-4 )
{
	const Vector3 e1 = Vector3Ops::mkVector3( v1, v0 );
	const Vector3 e2 = Vector3Ops::mkVector3( v2, v0 );
	const Vector3 pv = Vector3Ops::mkVector3( p, v0 );
	const Scalar a = Vector3Ops::Dot( e1, e1 );
	const Scalar b = Vector3Ops::Dot( e1, e2 );
	const Scalar c = Vector3Ops::Dot( e2, e2 );
	const Scalar d = Vector3Ops::Dot( e1, pv );
	const Scalar e = Vector3Ops::Dot( e2, pv );
	const Scalar det = a*c - b*b;
	if( std::fabs( det ) < NEARZERO ) {
		u = 0; v = 0; inside = false;
		return;
	}
	u = (c*d - b*e) / det;
	v = (a*e - b*d) / det;
	inside = (u >= -epsTol) && (v >= -epsTol) && (u + v <= 1.0 + epsTol);
}

SurfaceDerivatives TriangleMeshGeometryIndexed::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	// Walk the triangle list to find which triangle contains the point.
	// This is O(n_triangles) — fine for tests and rare one-off queries.
	// SMS and other frequent callers should read derivatives directly
	// from the RayIntersectionGeometric record populated during
	// IntersectRay (stage 1.1 of the SMS work), avoiding this walk.
	const Scalar epsTol = 1e-3;

	const PointerTriangle* bestTri = 0;
	Scalar bestU = 0, bestV = 0;
	Scalar bestOutOfPlane = RISE_INFINITY;

	for( MyPointerTriangleList::const_iterator it = ptr_polygons.begin();
		it != ptr_polygons.end(); ++it )
	{
		const PointerTriangle& tri = *it;
		Scalar u = 0, v = 0;
		bool inside = false;
		BarycentricCoords( *tri.pVertices[0], *tri.pVertices[1], *tri.pVertices[2],
			objSpacePoint, u, v, inside, epsTol );
		if( !inside ) continue;

		// Verify the point is actually on the triangle's plane (not just
		// inside its prism).
		const Vector3 e1 = Vector3Ops::mkVector3( *tri.pVertices[1], *tri.pVertices[0] );
		const Vector3 e2 = Vector3Ops::mkVector3( *tri.pVertices[2], *tri.pVertices[0] );
		Vector3 faceN = Vector3Ops::Cross( e1, e2 );
		const Scalar faceNLen = Vector3Ops::Magnitude( faceN );
		if( faceNLen < NEARZERO ) continue;
		faceN = faceN * (1.0 / faceNLen);
		const Vector3 delta = Vector3Ops::mkVector3( objSpacePoint, *tri.pVertices[0] );
		const Scalar oop = std::fabs( Vector3Ops::Dot( faceN, delta ) );
		if( oop < bestOutOfPlane ) {
			bestTri = &tri;
			bestU = u;
			bestV = v;
			bestOutOfPlane = oop;
		}
	}

	if( !bestTri ) {
		// Fallback: no triangle found containing the point.  Return a
		// reasonable tangent frame from the normal so callers don't NaN.
		SurfaceDerivatives sd;
		OrthonormalBasis3D onb;
		onb.CreateFromW( objSpaceNormal );
		sd.dpdu = onb.u();
		sd.dpdv = onb.v();
		sd.dndu = Vector3( 0, 0, 0 );
		sd.dndv = Vector3( 0, 0, 0 );
		sd.uv = Point2( 0, 0 );
		sd.valid = false;
		return sd;
	}

	return ComputeTriangleDerivatives( *bestTri, objSpaceNormal, objSpacePoint,
		bestU, bestV, !bUseFaceNormals );
}
