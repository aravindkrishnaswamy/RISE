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
#include "../Utilities/Observable.h"

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
  m_maxPolysPerNode( max_polys_per_node ),
  m_maxRecursionLevel( max_recursion_level ),
  m_bDoubleSided( bDoubleSided ),
  m_bUseBSP( bUseBSP ),
  m_bUseFaceNormals( bUseFaceNormals ),
  m_pMesh( 0 ),
  m_displacementSubscription()
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

	BuildMesh();

	// If the displacement painter exposes the Observable mixin (i.e. it derives
	// from Painter — true for every in-tree painter), subscribe so we rebuild
	// the mesh whenever the painter's keyframable state changes.  Out-of-tree
	// IFunction2D implementations that don't derive from Observable simply
	// don't subscribe; we keep today's bake-once behaviour for them.
	if( m_pDisplacement ) {
		const Observable* obs = dynamic_cast<const Observable*>( m_pDisplacement );
		if( obs ) {
			m_displacementSubscription = Subscription( obs, [this]{
				DestroyMesh();
				BuildMesh();
			} );
		}
	}
}

DisplacedGeometry::~DisplacedGeometry()
{
	// The destructor BODY runs before any member destructor.  Detach from the
	// painter's observer list first, while m_pDisplacement is still alive.
	// Otherwise, m_pDisplacement->release() below could delete the painter,
	// and the subscription's own destructor (which runs after this body) would
	// then call Detach() on freed memory.  Move-assigning an empty Subscription
	// triggers the current subscription's Detach via the assignment operator.
	m_displacementSubscription = Subscription();

	DestroyMesh();
	if( m_pDisplacement ) {
		m_pDisplacement->release();
		m_pDisplacement = 0;
	}
	if( m_pBase ) {
		m_pBase->release();
		m_pBase = 0;
	}
}

void DisplacedGeometry::BuildMesh()
{
	if( !m_pBase ) {
		return;
	}

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;

	if( !m_pBase->TessellateToMesh( tris, vertices, normals, coords, m_detail ) ) {
		GlobalLog()->Print( eLog_Error, "DisplacedGeometry: base geometry does not support tessellation (e.g. InfinitePlaneGeometry)" );
		return;
	}

	// Did we actually move any vertex positions?  With disp_scale==0 (or a null
	// displacement painter) ApplyDisplacementMapToObject is a no-op and the
	// analytical per-vertex normals coming out of TessellateToMesh are still
	// correct.  Topology-averaged normals would REPLACE those analytic normals
	// with a locally-linear approximation — fine for displaced surfaces, but
	// unnecessarily lossy when nothing was displaced.  This matters a lot for
	// SMS / Manifold-Solver tests that use disp_scale=0 as a "force tessellation
	// of an otherwise analytic shape" idiom: with this shortcut the tessellated
	// sphere's |∂N/∂u|/|∂P/∂u| ratio lands on 1/R (exactly the analytic value)
	// everywhere except the pole-cap degenerate triangles.
	const bool bVerticesDisplaced = ( m_pDisplacement && m_dispScale != 0.0 );
	if( bVerticesDisplaced ) {
		// RemapTextureCoords is a tent-fold (u → 1−2u on [0,0.5]; u → 2u−1 on
		// [0.5,1]) used ONLY to keep the displacement value consistent across
		// the u=0 / u=1 wrap seam of closed parametric surfaces (sphere,
		// torus, cylinder).  It is destructive to the linear (u, v)
		// parameterisation that the SMS / Manifold-Solver UV-Jacobian path
		// relies on: every triangle straddling u=0.5 or v=0.5 ends up with
		// a non-monotonic UV triple (tent vertex in the middle), degenerating
		// the 2×2 Jacobian.  Keep the original coords for the mesh and feed
		// a tent-remapped COPY into the displacement evaluator only.
		TexCoordsListType displacementCoords = coords;
		RemapTextureCoords( displacementCoords );
		ApplyDisplacementMapToObject( tris, vertices, normals, displacementCoords, *m_pDisplacement, m_dispScale );
	}

	if( !m_bUseFaceNormals && bVerticesDisplaced ) {
		RecomputeVertexNormalsFromTopology( tris, vertices, normals );
	}

	m_pMesh = new TriangleMeshGeometryIndexed( m_maxPolysPerNode, m_maxRecursionLevel, m_bDoubleSided, m_bUseBSP, m_bUseFaceNormals );
	GlobalLog()->PrintNew( m_pMesh, __FILE__, __LINE__, "displaced geometry internal mesh" );

	m_pMesh->BeginIndexedTriangles();
	m_pMesh->AddVertices( vertices );
	m_pMesh->AddNormals( normals );
	m_pMesh->AddTexCoords( coords );
	m_pMesh->AddIndexedTriangles( tris );
	m_pMesh->DoneIndexedTriangles();
}

void DisplacedGeometry::DestroyMesh()
{
	safe_release( m_pMesh );
	m_pMesh = 0;
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
