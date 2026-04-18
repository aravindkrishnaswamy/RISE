//////////////////////////////////////////////////////////////////////
//
//  DisplacedGeometry.cpp - Implementation of DisplacedGeometry.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-18
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DisplacedGeometry.h"
#include "TriangleMeshGeometryIndexed.h"
#include "GeometryUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

DisplacedGeometry::DisplacedGeometry(
	IGeometry*          pBase,
	const unsigned int  detail,
	const IFunction2D*  displacement,
	const Scalar        disp_scale,
	const unsigned int  max_polys_per_node,
	const unsigned char max_recursion_level,
	const bool          bDoubleSided,
	const bool          bUseBSP,
	const bool          bUseFaceNormals
	) :
  m_pBase( pBase ),
  m_pDisplacement( displacement ),
  m_dispScale( disp_scale ),
  m_detail( detail ),
  m_pMesh( 0 )
{
	if( m_pBase ) {
		m_pBase->addref();
	}
	if( m_pDisplacement ) {
		m_pDisplacement->addref();
	}

	if( !m_pBase ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry is null" );
		return;
	}

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	if( !m_pBase->TessellateToMesh( tris, vertices, normals, coords, detail ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry does not support tessellation (e.g. InfinitePlaneGeometry)" );
		return;
	}

	if( m_pDisplacement ) {
		RemapTextureCoords( coords );
		ApplyDisplacementMapToObject( tris, vertices, normals, coords, *m_pDisplacement, m_dispScale );
	}

	if( !bUseFaceNormals ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
	}

	m_pMesh = new TriangleMeshGeometryIndexed( max_polys_per_node, max_recursion_level, bDoubleSided, bUseBSP, bUseFaceNormals );
	GlobalLog()->PrintNew( m_pMesh, __FILE__, __LINE__, "displaced geometry internal mesh" );

	m_pMesh->BeginIndexedTriangles();
	m_pMesh->AddVertices( vertices );
	m_pMesh->AddNormals( normals );
	m_pMesh->AddTexCoords( coords );
	m_pMesh->AddIndexedTriangles( tris );
	m_pMesh->DoneIndexedTriangles();
}

DisplacedGeometry::~DisplacedGeometry()
{
	safe_release( m_pMesh );
	if( m_pDisplacement ) {
		m_pDisplacement->release();
		m_pDisplacement = 0;
	}
	if( m_pBase ) {
		m_pBase->release();
		m_pBase = 0;
	}
}

bool DisplacedGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     /*detail*/ ) const
{
	// Re-emit the internal mesh so a DisplacedGeometry can itself be the base of another
	// DisplacedGeometry.  The detail parameter is ignored — the internal mesh was built at
	// this DisplacedGeometry's own construction-time detail.
	if( !m_pMesh ) {
		return false;
	}

	// Delegate via the internal TriangleMeshGeometryIndexed's own TessellateToMesh, which
	// is a pass-through of its stored arrays.
	return m_pMesh->TessellateToMesh( tris, vertices, normals, coords, 0 );
}

void DisplacedGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( !m_pMesh ) {
		ri.bHit = false;
		return;
	}
	m_pMesh->IntersectRay( ri, bHitFrontFaces, bHitBackFaces, bComputeExitInfo );
}

bool DisplacedGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( !m_pMesh ) {
		return false;
	}
	return m_pMesh->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
}

void DisplacedGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	if( !m_pMesh ) {
		ptCenter = Point3( 0.0, 0.0, 0.0 );
		radius   = 0.0;
		return;
	}
	m_pMesh->GenerateBoundingSphere( ptCenter, radius );
}

BoundingBox DisplacedGeometry::GenerateBoundingBox() const
{
	if( !m_pMesh ) {
		return BoundingBox( Point3( 0.0, 0.0, 0.0 ), Point3( 0.0, 0.0, 0.0 ) );
	}
	return m_pMesh->GenerateBoundingBox();
}

void DisplacedGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	if( !m_pMesh ) {
		if( point )  *point  = Point3( 0.0, 0.0, 0.0 );
		if( normal ) *normal = Vector3( 0.0, 0.0, 0.0 );
		if( coord )  *coord  = Point2( 0.0, 0.0 );
		return;
	}
	m_pMesh->UniformRandomPoint( point, normal, coord, prand );
}

Scalar DisplacedGeometry::GetArea() const
{
	if( !m_pMesh ) {
		return 0.0;
	}
	return m_pMesh->GetArea();
}

SurfaceDerivatives DisplacedGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	if( !m_pMesh ) {
		return SurfaceDerivatives();
	}
	return m_pMesh->ComputeSurfaceDerivatives( objSpacePoint, objSpaceNormal );
}
