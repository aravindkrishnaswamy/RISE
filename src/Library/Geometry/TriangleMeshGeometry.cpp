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
// BSPTreeSAH / Octree headers are still needed in the .cpp because v2/v3
// .risemesh files have BSP/octree byte blocks that we read into local
// temporaries during Deserialize so the byte stream advances correctly,
// even though the parsed trees are never used (BVH owns intersection now).
// They are NOT pulled into the .h — this class no longer carries any
// BSP/octree state, only the ability to consume the legacy bytes.
#include "../Octree.h"
#include "../BSPTreeSAH.h"
#include <cmath>

inline unsigned int VoidPtrToUInt( const void* v )
{
	return (unsigned int)*((unsigned int*)(&v));
}

using namespace RISE;
using namespace RISE::Implementation;

#include "TriangleMeshGeometrySpecializations.h"

TriangleMeshGeometry::TriangleMeshGeometry(
	const bool bDoubleSided_
	) :
  bDoubleSided( bDoubleSided_ ),
  pPolygonsBVH( 0 )
{
}

TriangleMeshGeometry::~TriangleMeshGeometry()
{
	safe_release( pPolygonsBVH );
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

	// Cleanup §3+§4: BVH is the only active path; legacy BSP/octree
	// members preserved on the class (for v1/v2 .risemesh deserialize
	// compat) but never reachable at runtime.
	if( pPolygonsBVH ) {
		pPolygonsBVH->IntersectRay( ri, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
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
	// Cleanup §3+§4: BVH-only.
	if( pPolygonsBVH ) {
		return pPolygonsBVH->IntersectRay_IntersectionOnly( ray, dHowFar, bDoubleSided?1:bHitFrontFaces, bDoubleSided?1:bHitBackFaces );
	}
	return false;
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
	safe_release( pPolygonsBVH );
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
	// Self-clearing: the inner loops both push_back, so we MUST clear
	// before recomputing or the CDF grows quadratically across repeated
	// calls.  No observer path on the non-indexed mesh today, but the
	// invariant is shared with TriangleMeshGeometryIndexed::ComputeAreas
	// for defense in depth.  See Tier A2 review notes, 2026-04-27.
	areas.clear();
	areasCDF.clear();
	totalArea = 0;

	// Compute triangle areas
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
	if( totalArea > 0 ) {
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

	// Tier 1 §4: BVH replaces BSP/octree as the active acceleration
	// structure for non-indexed meshes too.  (Cleanup §3+§4: env-var
	// escape hatch and BSP/octree fallback both removed; pPolygonsBSPtree
	// / pPolygonsOctree members are kept for v1/v2 .risemesh deserialize
	// compat but are never built or consulted at runtime.)
	safe_release( pPolygonsBVH );

	AccelerationConfig cfg;
	cfg.maxLeafSize            = 4;
	cfg.binCount               = 32;
	cfg.sahTraversalCost       = 1.0;
	cfg.sahIntersectionCost    = 1.0;
	cfg.doubleSided            = bDoubleSided;
	cfg.buildSBVH              = false;
	cfg.sbvhDuplicationBudget  = 0.30;

	pPolygonsBVH = new BVH<const Triangle*>( *this, temp, bbox, cfg );
	GlobalLog()->PrintNew( pPolygonsBVH, __FILE__, __LINE__, "polygons BVH" );

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
	// Cleanup §3+§4: BVH-only.
	if( pPolygonsBVH ) {
		return pPolygonsBVH->GetBBox();
	}
	return BoundingBox();
}

static const char * szSignature = "RISE_TMG";
static const unsigned int cur_version = 4;
//
// Version history (non-indexed mesh):
//   2 — original layout: octree settings + polygons + bDoubleSided +
//       BSP/octree byte block.
//   3 — same layout (Tier 1 §4 added a runtime BVH but didn't change
//       the on-disk format; v3 readers built BVH at load-time).
//   4 — Tier A2 cleanup (2026-04-27): drops the legacy on-disk fields
//       (nMaxPerOctantNode, nMaxRecursionLevel, bUseBSP, bptrXX, tree
//       bytes).  Layout: signature(8) + version(4) + numpolys(4) +
//       Triangle data + bDoubleSided(1).  No BVH cache yet — non-
//       indexed meshes still build BVH at load time post-Deserialize.

void TriangleMeshGeometry::Serialize( IWriteBuffer& buffer ) const
{
	// stuff data into the buffer

	// first write out the signature and version
	buffer.setBytes( szSignature, 8 );
	buffer.setUInt( cur_version );

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

	buffer.ResizeForMore( sizeof( char ) );
	buffer.setChar( bDoubleSided ? 1 : 0 );

	// Tier A2 (.risemesh v4): no more BSP/octree byte block.  BVH is
	// built at load time in Deserialize.  A future commit could add
	// a v5 BVH cache here, mirroring the indexed mesh's cache, when
	// non-indexed mesh load time becomes a measurable bottleneck.
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

	if( version < 2 || version > cur_version ) {
		GlobalLog()->PrintEx( eLog_Error,
			"TriangleMeshGeometry::Deserialize:: Unsupported .risemesh version %u (this build understands v2..v%u)",
			version, cur_version );
		return;
	}

	// Pre-v4 layouts started with octree settings (nMaxPerOctantNode +
	// nMaxRecursionLevel) before the polygon data.  Read+discard them.
	if( version < 4 ) {
		(void)buffer.getUInt();   // legacy nMaxPerOctantNode
		(void)buffer.getChar();   // legacy nMaxRecursionLevel
	}

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

	// Pre-v4 files have a BSP/octree block here that the live class no
	// longer carries.  Read+discard via Deserialize-local temporaries so
	// the byte stream advances past the block; the parsed trees are
	// validated for "structurally non-broken" so we can warn loudly on
	// truly corrupt files, but the BVH is the sole live acceleration
	// structure regardless.
	if( version < 4 ) {
		const bool legacyUseBSP = !!buffer.getChar();
		const bool legacyHaveTree = !!buffer.getChar();

		if( legacyHaveTree ) {
			if( legacyUseBSP ) {
				BSPTreeSAH<const Triangle*>* pLegacyBSP =
					new BSPTreeSAH<const Triangle*>(
						*this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), 1 );
				GlobalLog()->PrintNew( pLegacyBSP, __FILE__, __LINE__, "legacy polygons BSP (read+discard)" );
				pLegacyBSP->Deserialize( buffer );
				safe_release( pLegacyBSP );
			} else {
				Octree<const Triangle*>* pLegacyOct =
					new Octree<const Triangle*>(
						*this, BoundingBox(Point3(0,0,0), Point3(0,0,0)), 1 );
				GlobalLog()->PrintNew( pLegacyOct, __FILE__, __LINE__, "legacy polygons Octree (read+discard)" );
				pLegacyOct->Deserialize( buffer );
				safe_release( pLegacyOct );
			}
		}
	}

	// Build the BVH from the loaded polygon data.  Non-indexed mesh
	// .risemesh files don't yet carry a BVH cache, so we always build
	// at load time (cheap for the small number of non-indexed meshes
	// in the asset library).
	safe_release( pPolygonsBVH );

	if( polygons.size() > 0 ) {
		GlobalLog()->PrintEx( eLog_Info, "TriangleMeshGeometry::Deserialize:: Building BVH from %u polygons", (unsigned)polygons.size() );

		BoundingBox bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ), Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		std::vector<const Triangle*> temp;
		MyTriangleList::iterator i, e;
		for( i=polygons.begin(), e=polygons.end(); i!=e; i++ ) {
			temp.push_back( &(*i) );
			for( int j=0; j<3; j++ ) {
				bbox.Include( i->vertices[j] );
			}
		}

		AccelerationConfig cfg;
		cfg.maxLeafSize            = 4;
		cfg.binCount               = 32;
		cfg.sahTraversalCost       = 1.0;
		cfg.sahIntersectionCost    = 1.0;
		cfg.doubleSided            = bDoubleSided;
		cfg.buildSBVH              = false;
		cfg.sbvhDuplicationBudget  = 0.30;

		pPolygonsBVH = new BVH<const Triangle*>( *this, temp, bbox, cfg );
		GlobalLog()->PrintNew( pPolygonsBVH, __FILE__, __LINE__, "polygons BVH (built post-deserialize)" );
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

	// Strategy (mirrors TriangleMeshGeometryIndexedSpecializations.h /
	// TriangleMeshGeometryIndexed::ComputeTriangleDerivatives): invert the
	// 2×2 UV Jacobian using the per-vertex texture coordinates so the
	// derivatives are in the stored (u,v) parameterisation.  For
	// TessellateToMesh-produced meshes those UVs are the analytical
	// shape's own parameterisation, so the frame aligns with and scales
	// like the analytical dpdu/dpdv.
	// Fall back to the barycentric-edge frame when |det|<ε.
	const Vector3 e1 = Vector3Ops::mkVector3( tri.vertices[1], tri.vertices[0] );
	const Vector3 e2 = Vector3Ops::mkVector3( tri.vertices[2], tri.vertices[0] );

	const Scalar duA = tri.coords[1].x - tri.coords[0].x;
	const Scalar duB = tri.coords[2].x - tri.coords[0].x;
	const Scalar dvA = tri.coords[1].y - tri.coords[0].y;
	const Scalar dvB = tri.coords[2].y - tri.coords[0].y;
	const Scalar uvDet = duA * dvB - duB * dvA;
	const bool useUVJacobian = ( fabs( uvDet ) > NEARZERO );

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

	// Project into the shading-normal tangent plane, per
	// docs/GEOMETRY_DERIVATIVES.md tangency convention.
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

	// Raw normal-diff derivatives in barycentric (A, B).
	const Vector3 dNraw_dA = Vector3(
		tri.normals[1].x - tri.normals[0].x,
		tri.normals[1].y - tri.normals[0].y,
		tri.normals[1].z - tri.normals[0].z );
	const Vector3 dNraw_dB = Vector3(
		tri.normals[2].x - tri.normals[0].x,
		tri.normals[2].y - tri.normals[0].y,
		tri.normals[2].z - tri.normals[0].z );

	// Apply the same UV-Jacobian inversion to the normal derivatives
	// so they live in the same parameterisation as dpdu/dpdv.
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

	const Vector3 Nraw = Vector3(
		tri.normals[0].x + barU * dNraw_dA.x + barV * dNraw_dB.x,
		tri.normals[0].y + barU * dNraw_dA.y + barV * dNraw_dB.y,
		tri.normals[0].z + barU * dNraw_dA.z + barV * dNraw_dB.z );
	const Scalar len = Vector3Ops::Magnitude( Nraw );
	const Scalar invLen = (len > NEARZERO) ? 1.0 / len : 0.0;

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
