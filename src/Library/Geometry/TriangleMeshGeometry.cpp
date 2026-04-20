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
#include "../Utilities/OrthonormalBasis3D.h"
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

bool TriangleMeshGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/ ) const
{
	// Pass-through: emit stored triangles as indexed geometry.  Each Triangle contributes
	// three fresh vertices (no merging) so per-triangle UV/normal discontinuities are preserved.
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );

	for( MyTriangleList::const_iterator it = polygons.begin(); it != polygons.end(); ++it ) {
		const Triangle& tri = *it;
		const unsigned int a = static_cast<unsigned int>( vertices.size() );

		for( int k = 0; k < 3; k++ ) {
			vertices.push_back( tri.vertices[k] );
			normals.push_back(  tri.normals[k] );
			coords.push_back(   tri.coords[k] );
		}

		tris.push_back( MakeIndexedTriangleSameIdx( a, a + 1, a + 2 ) );
	}

	return static_cast<unsigned int>( vertices.size() ) > baseIdx;
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
		if( Vector3Ops::Dot(ri.vNormal, ri.ray.Dir()) > 0 ) {
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

	int idx = static_cast<int>(areasCDF.size())-1;
	if( it != areasCDF.end() ) {
		idx = static_cast<int>(std::distance( areasCDF.begin(), it ));
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
	BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );

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

		pPolygonsBSPtree = new BSPTreeSAH<const Triangle*>( *this, bbox, nMaxPerOctantNode );
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
	Point3	ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3	ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ;

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
static const unsigned int cur_version = 3;

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
		buffer.ResizeForMore( static_cast<unsigned int>(sizeof( Triangle ) * polygons.size() + sizeof( unsigned int )) );

		buffer.setUInt( static_cast<unsigned int>(polygons.size()) );

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
	
	if( version != 2 && version != cur_version ) {
		GlobalLog()->PrintEasyError( "TriangleMeshGeometry::Deserialize:: Versions don't match.  Are you using an older format?" );
		return;
	}

	// First get octree settings
	nMaxPerOctantNode = buffer.getUInt();
	nMaxRecursionLevel = buffer.getChar();

	polygons.clear();
	safe_release( pPolygonsOctree );
	safe_release( pPolygonsBSPtree );

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
	bool bTreeValid = false;
		
	if( bUseBSP ) {
		// Deserialize the bsp trees
		const bool bpolybsptree = !!buffer.getChar();

		if( bpolybsptree ) {
				if( version == cur_version ) {
					pPolygonsBSPtree = new BSPTreeSAH<const Triangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
					GlobalLog()->PrintNew( pPolygonsBSPtree, __FILE__, __LINE__, "polygons bsptree" );

				// Deserialize
				pPolygonsBSPtree->Deserialize( buffer );

				BoundingBox treeBBox = pPolygonsBSPtree->GetBBox();
				Vector3 extents = treeBBox.GetExtents();
				if( std::isfinite(treeBBox.ll.x) && std::isfinite(treeBBox.ll.y) && std::isfinite(treeBBox.ll.z) &&
					std::isfinite(treeBBox.ur.x) && std::isfinite(treeBBox.ur.y) && std::isfinite(treeBBox.ur.z) &&
					std::abs(extents.x) < 1e10 && std::abs(extents.y) < 1e10 && std::abs(extents.z) < 1e10 )
				{
					bTreeValid = true;
				} else {
					GlobalLog()->PrintEasyWarning( "TriangleMeshGeometry::Deserialize:: Deserialized BSP tree has invalid bounding box, will rebuild" );
					safe_release( pPolygonsBSPtree );
					pPolygonsBSPtree = 0;
				}
				} else {
					GlobalLog()->PrintEasyWarning( "TriangleMeshGeometry::Deserialize:: Legacy BSP serialization detected, rebuilding SAH tree from polygon data" );
				}
			}
	} else {
		// Deserialize the octrees
		const bool bpolyoctree = !!buffer.getChar();

		if( bpolyoctree ) {
			pPolygonsOctree = new Octree<const Triangle*>( *this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree" );

			// Deserialize
			pPolygonsOctree->Deserialize( buffer );
			bTreeValid = true;
		}
	}

	if( !bTreeValid && polygons.size() > 0 ) {
		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometry::Deserialize:: Rebuilding spatial structure from %u polygons", polygons.size() );

		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		std::vector<const Triangle*> temp;
		MyTriangleList::iterator i, e;
		for( i=polygons.begin(), e=polygons.end(); i!=e; i++ ) {
			temp.push_back( &(*i) );
			for( int j=0; j<3; j++ ) {
				bbox.Include( i->vertices[j] );
			}
		}

		if( bUseBSP ) {
			pPolygonsBSPtree = new BSPTreeSAH<const Triangle*>( *this, bbox, nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPolygonsBSPtree, __FILE__, __LINE__, "polygons bsptree (rebuilt)" );
			pPolygonsBSPtree->AddElements( temp, nMaxRecursionLevel );
		} else {
			pPolygonsOctree = new Octree<const Triangle*>( *this, bbox, nMaxPerOctantNode );
			GlobalLog()->PrintNew( pPolygonsOctree, __FILE__, __LINE__, "polygons octree (rebuilt)" );
			pPolygonsOctree->AddElements( temp, nMaxRecursionLevel );
		}
	}

	ComputeAreas();

	// And we're done!
	GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometry::Deserialize:: Finished deserialization", bDoubleSided );
}

// Barycentric coords for the non-indexed mesh case (shares logic with
// the indexed variant but works off Triangle rather than PointerTriangle).
static void TMBarycentric(
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

// Compute SMS-style per-triangle derivatives for a non-indexed Triangle.
// See TriangleMeshGeometryIndexed::ComputeTriangleDerivatives for the
// mathematical derivation; this version works directly off the inline
// vertices/normals stored per-triangle (no pointer indirection).
static SurfaceDerivatives TMComputeTriangleDerivatives(
	const Triangle& tri,
	const Vector3& objSpaceNormal,
	const Scalar barU,
	const Scalar barV )
{
	SurfaceDerivatives sd;
	sd.valid = true;

	// Project triangle edges into the shading-normal tangent plane, per
	// docs/GEOMETRY_DERIVATIVES.md tangency convention.
	const Vector3 e1 = Vector3Ops::mkVector3( tri.vertices[1], tri.vertices[0] );
	const Vector3 e2 = Vector3Ops::mkVector3( tri.vertices[2], tri.vertices[0] );
	const Scalar e1_dot_n = Vector3Ops::Dot( e1, objSpaceNormal );
	const Scalar e2_dot_n = Vector3Ops::Dot( e2, objSpaceNormal );
	sd.dpdu = Vector3(
		e1.x - objSpaceNormal.x * e1_dot_n,
		e1.y - objSpaceNormal.y * e1_dot_n,
		e1.z - objSpaceNormal.z * e1_dot_n );
	sd.dpdv = Vector3(
		e2.x - objSpaceNormal.x * e2_dot_n,
		e2.y - objSpaceNormal.y * e2_dot_n,
		e2.z - objSpaceNormal.z * e2_dot_n );

	const Vector3 dNraw_du = Vector3(
		tri.normals[1].x - tri.normals[0].x,
		tri.normals[1].y - tri.normals[0].y,
		tri.normals[1].z - tri.normals[0].z );
	const Vector3 dNraw_dv = Vector3(
		tri.normals[2].x - tri.normals[0].x,
		tri.normals[2].y - tri.normals[0].y,
		tri.normals[2].z - tri.normals[0].z );
	const Vector3 Nraw = Vector3(
		tri.normals[0].x + barU * dNraw_du.x + barV * dNraw_dv.x,
		tri.normals[0].y + barU * dNraw_du.y + barV * dNraw_dv.y,
		tri.normals[0].z + barU * dNraw_du.z + barV * dNraw_dv.z );
	const Scalar len = Vector3Ops::Magnitude( Nraw );
	const Scalar invLen = (len > NEARZERO) ? 1.0 / len : 0.0;
	const Vector3 N = Nraw * invLen;

	// Use objSpaceNormal (shading normal from caller) as the reference
	// for perpendicularity — matches TriangleMeshGeometryIndexed.
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

	// Enforce right-handed frame
	const Vector3 cross = Vector3Ops::Cross( sd.dpdu, sd.dpdv );
	if( Vector3Ops::Dot( cross, objSpaceNormal ) < 0.0 ) {
		sd.dpdv = Vector3( -sd.dpdv.x, -sd.dpdv.y, -sd.dpdv.z );
		sd.dndv = Vector3( -sd.dndv.x, -sd.dndv.y, -sd.dndv.z );
	}

	sd.uv = Point2( barU, barV );
	return sd;
}

SurfaceDerivatives TriangleMeshGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	// See TriangleMeshGeometryIndexed::ComputeSurfaceDerivatives for the
	// rationale.  O(n) walk, sufficient for tests and one-off queries.
	const Scalar epsTol = 1e-3;

	const Triangle* bestTri = 0;
	Scalar bestU = 0, bestV = 0;
	Scalar bestOutOfPlane = RISE_INFINITY;

	for( MyTriangleList::const_iterator it = polygons.begin();
		it != polygons.end(); ++it )
	{
		const Triangle& tri = *it;
		Scalar u = 0, v = 0;
		bool inside = false;
		TMBarycentric( tri.vertices[0], tri.vertices[1], tri.vertices[2],
			objSpacePoint, u, v, inside, epsTol );
		if( !inside ) continue;

		const Vector3 e1 = Vector3Ops::mkVector3( tri.vertices[1], tri.vertices[0] );
		const Vector3 e2 = Vector3Ops::mkVector3( tri.vertices[2], tri.vertices[0] );
		Vector3 faceN = Vector3Ops::Cross( e1, e2 );
		const Scalar faceNLen = Vector3Ops::Magnitude( faceN );
		if( faceNLen < NEARZERO ) continue;
		faceN = faceN * (1.0 / faceNLen);
		const Vector3 delta = Vector3Ops::mkVector3( objSpacePoint, tri.vertices[0] );
		const Scalar oop = std::fabs( Vector3Ops::Dot( faceN, delta ) );
		if( oop < bestOutOfPlane ) {
			bestTri = &tri;
			bestU = u;
			bestV = v;
			bestOutOfPlane = oop;
		}
	}

	if( !bestTri ) {
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

	return TMComputeTriangleDerivatives( *bestTri, objSpaceNormal, bestU, bestV );
}
