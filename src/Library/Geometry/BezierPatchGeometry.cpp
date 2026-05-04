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
#include "BezierTesselation.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Interfaces/ILog.h"
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

BoundingBox BezierPatchGeometry::GetElementBoundingBox( const MYOBJ elem ) const
{
	return elem.bbox;
}

char BezierPatchGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, elem.bbox );
}

void BezierPatchGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	BOX_HIT	h;
	RayBoxIntersection( ri.ray, h, elem.bbox.ll, elem.bbox.ur );

	if( !h.bHit ) return;
	// Per-element front-to-back pruning: if the bbox entry distance is already
	// beyond the closest hit, skip.  Currently often dead because BSPTreeSAHNode
	// creates a fresh RayIntersection per element (so ri.bHit is false on entry)
	// — harmless as-is, becomes effective if that allocation is ever removed.
	if( ri.bHit && h.dRange > ri.range ) return;

	BEZIER_HIT bh;
	// Seed hit.dRange with the current closest-hit distance so the analytic
	// solver can reject far (u*,v*) roots immediately inside AccumulateRoot.
	bh.dRange = ri.range;
	RayBezierPatchIntersection( ri.ray, bh, *elem.pPatch );

	if( !bh.bHit || bh.dRange >= ri.range ) return;

	// Patch-space normal from tangent cross product.  The winding (and hence
	// sign) is a parameterisation choice that varies patch-to-patch in the
	// Utah teapot file — some patches are traversed CCW, others CW — so flip
	// against the incoming ray to produce a consistent facing normal.
	Vector3 N = GeometricUtilities::BezierPatchNormalAt( *elem.pPatch, bh.u, bh.v );
	Scalar nLen = Vector3Ops::Magnitude( N );
	if( nLen < 1e-20 ) return;                  // degenerate (coincident tangents)
	N = N * ( 1.0 / nLen );

	Scalar dotND = Vector3Ops::Dot( N, ri.ray.Dir() );
	const bool bRawFront = ( dotND < 0.0 );
	if(  bRawFront && !bHitFrontFaces ) return;
	if( !bRawFront && !bHitBackFaces  ) return;

	// Make the reported normal oppose the ray — standard convention for
	// shading/reflection.  Flipping the back-face normal also gives the
	// "both sides visible" look without needing a double_sided material flag.
	if( dotND > 0.0 ) {
		N = N * -1.0;
	}

	ri.bHit           = true;
	ri.range          = bh.dRange;
	ri.ptIntersection = ri.ray.PointAtLength( bh.dRange );
	ri.ptCoord        = Point2( bh.u, bh.v );
	ri.vNormal        = N;
	ri.vGeomNormal    = N;	// analytical surface: shading == geometric
}

void BezierPatchGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool BezierPatchGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	BOX_HIT	h;
	RayBoxIntersection( ray, h, elem.bbox.ll, elem.bbox.ur );

	if( !h.bHit ) return false;

	BEZIER_HIT bh;
	// Shadow rays only need to know IF there's an occluder in [0, dHowFar].
	// Seeding hit.dRange = dHowFar lets AccumulateRoot reject any root
	// beyond the light's distance without running Newton + patch-eval on it.
	bh.dRange = dHowFar;
	RayBezierPatchIntersection( ray, bh, *elem.pPatch );
	if( !bh.bHit ) return false;
	// AccumulateRoot already applies the Bezier self-hit epsilon when
	// populating bh.dRange, so no second NEARZERO gate is needed here.
	if( bh.dRange > dHowFar ) return false;

	// Apply face culling on shadow rays too so self-shadowing respects
	// the same sidedness policy as primary rays.
	Vector3 N = GeometricUtilities::BezierPatchNormalAt( *elem.pPatch, bh.u, bh.v );
	if( Vector3Ops::SquaredModulus( N ) < 1e-40 ) return false;
	const Scalar dotND = Vector3Ops::Dot( N, ray.Dir() );
	const bool bHitFront = ( dotND < 0.0 );
	if(  bHitFront && !bHitFrontFaces ) return false;
	if( !bHitFront && !bHitBackFaces  ) return false;
	return true;
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
	const bool bUseBSP_
	) :
  pBSPTree( 0 ),
  pOctree( 0 ),
  nMaxPerOctantNode( max_patches_per_node ),
  nMaxRecursionLevel( max_recursion_level ),
  bUseBSP( bUseBSP_ )
{
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

	BoundingBox overall( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
	for( unsigned int i=0; i<patches.size(); i++ ) {
		BezierPatch* b = &patches[i];
		MYBEZIERPATCH m;
		m.pPatch = b;
		m.id = i;
		m.bbox = GeometricUtilities::BezierPatchBoundingBox( *b );
		// Bbox-grow for displacement is the wrapper's job (DisplacedGeometry
		// owns its own internal TriangleMeshGeometryIndexed and generates
		// its own bbox from the post-displacement vertices).
		overall.Include( m.bbox );
		patchptrs.push_back( m );
	}

	// We're done so stuff all the bezier patches into the bsp-tree
	safe_release( pBSPTree );
	safe_release( pOctree );

	if( bUseBSP ) {
		pBSPTree = new BSPTreeSAH<MYBEZIERPATCH>( *this, overall, nMaxPerOctantNode );
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

bool BezierPatchGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( patches.empty() || detail < 1 ) {
		return false;
	}

	// Concatenate per-patch tessellations, remapping triangle indices to the running base.
	// Note: GeneratePolygonsFromBezierPatch doesn't populate `normals`, only reserves space
	// implicitly via index references.  The caller is expected to recompute normals.
	for( BezierPatchList::const_iterator it = patches.begin(); it != patches.end(); ++it ) {
		IndexTriangleListType patchTris;
		VerticesListType      patchVerts;
		NormalsListType       patchNormals;
		TexCoordsListType     patchCoords;

		GeneratePolygonsFromBezierPatch( patchTris, patchVerts, patchNormals, patchCoords, *it, detail );

		const unsigned int vBase = static_cast<unsigned int>( vertices.size() );
		const unsigned int cBase = static_cast<unsigned int>( coords.size() );

		vertices.insert( vertices.end(), patchVerts.begin(),  patchVerts.end()  );
		coords.insert(   coords.end(),   patchCoords.begin(), patchCoords.end() );

		// The underlying tessellator uses same-index convention (normal idx == vertex idx == coord idx
		// in the local per-patch arrays).  Re-base accordingly.
		normals.resize( vertices.size(), Vector3(0.0, 0.0, 0.0) );

		for( IndexTriangleListType::const_iterator t = patchTris.begin(); t != patchTris.end(); ++t ) {
			IndexedTriangle out;
			for( int k = 0; k < 3; k++ ) {
				out.iVertices[k] = t->iVertices[k] + vBase;
				out.iNormals[k]  = t->iVertices[k] + vBase;  // same-idx convention
				out.iCoords[k]   = t->iCoords[k]   + cBase;
			}
			tris.push_back( out );
		}
	}

	// Produce topology-derived vertex normals for the entire concatenated mesh.
	RecomputeVertexNormalsFromTopology( tris, vertices, normals );

	return true;
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
	Point3	ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3	ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ;

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
	// Area-weighted surface sampling.  Required path: SSS + photon-emission
	// paths ask the geometry for uniformly-distributed surface samples.
	//
	// Per-patch area is approximated by the Jacobian magnitude |dP/du x dP/dv|
	// evaluated at the patch centre (u=v=0.5).  This is one-point quadrature
	// of the true surface integral A_i = integral_{[0,1]^2} |T_u x T_v| du dv
	// and is exact for bilinear patches and smooth for bicubics of bounded
	// curvature — the order-of-magnitude accuracy we need here for weighted
	// selection.  If a future scene proves it undersamples high-curvature
	// patches (e.g., the teapot spout bulge), swap in 2x2 Gauss-Legendre
	// (4-point) — same shape, 4x the evaluations.
	//
	// (u, v) is then drawn uniformly in [0,1]^2 within the selected patch.
	// This is uniform in parameter space, NOT uniform in surface area — a
	// Jacobian-weighted rejection loop would fix that, but for the SSS use
	// case (where points then enter a dipole/BSSRDF integrator that weights
	// by cosine anyway) single-patch parameter-space uniformity is adequate.
	if( patches.empty() ) {
		if( point )  *point  = Point3( 0, 0, 0 );
		if( normal ) *normal = Vector3( 0, 1, 0 );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	// Build area CDF across all patches.  Doing this per call is O(N) and
	// N is ~32 for the teapot / ~tens for the f16 and aphrodite — cheaper
	// than threading a cache invalidation through Prepare()/AddPatch()/etc.
	// Revisit if a scene pushes N into the thousands.
	Scalar total = 0.0;
	std::vector<Scalar> cdf;
	cdf.reserve( patches.size() );
	for( BezierPatchList::const_iterator it = patches.begin(); it != patches.end(); ++it ) {
		const Vector3 Tu = GeometricUtilities::BezierPatchTangentU( *it, 0.5, 0.5 );
		const Vector3 Tv = GeometricUtilities::BezierPatchTangentV( *it, 0.5, 0.5 );
		Scalar a = Vector3Ops::Magnitude( Vector3Ops::Cross( Tu, Tv ) );
		if( a < 1e-20 ) a = 1e-20;    // avoid zero-weight degenerate patches collapsing the CDF
		total += a;
		cdf.push_back( total );
	}

	// Draw patch via CDF search.  Linear scan is fine for ~32 patches.
	const Scalar target = prand.x * total;
	unsigned int pIdx = 0;
	while( pIdx + 1 < cdf.size() && cdf[pIdx] < target ) pIdx++;

	const BezierPatch& p = patches[pIdx];
	Scalar u = prand.y;
	Scalar v = prand.z;
	if( u < 0.0 ) u = 0.0; else if( u > 1.0 ) u = 1.0;
	if( v < 0.0 ) v = 0.0; else if( v > 1.0 ) v = 1.0;

	if( point )  *point  = GeometricUtilities::EvaluateBezierPatchAt( p, u, v );
	if( normal ) {
		Vector3 N = GeometricUtilities::BezierPatchNormalAt( p, u, v );
		const Scalar nLen = Vector3Ops::Magnitude( N );
		*normal = ( nLen > 1e-20 ) ? ( N * ( 1.0 / nLen ) ) : Vector3( 0, 1, 0 );
	}
	if( coord )  *coord  = Point2( u, v );
}

SurfaceDerivatives BezierPatchGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;
	OrthonormalBasis3D onb;
	onb.CreateFromW( objSpaceNormal );
	sd.dpdu = onb.u();
	sd.dpdv = onb.v();
	sd.dndu = Vector3( 0, 0, 0 );
	sd.dndv = Vector3( 0, 0, 0 );
	sd.uv = Point2( 0, 0 );
	sd.valid = true;
	return sd;
}

Scalar BezierPatchGeometry::GetArea( ) const
{
	// Sum of per-patch area approximations, same one-point quadrature as
	// UniformRandomPoint uses so that (1 / area) matches the implicit pdf
	// of the sampler.  Returning 1.0 (the pre-2026 placeholder) broke any
	// path that divided radiance by area — notably SSS dipole sampling —
	// because the sampler's pdf was area-weighted but the consumer's
	// inverse-area factor was hardcoded-to-unit.
	Scalar total = 0.0;
	for( BezierPatchList::const_iterator it = patches.begin(); it != patches.end(); ++it ) {
		const Vector3 Tu = GeometricUtilities::BezierPatchTangentU( *it, 0.5, 0.5 );
		const Vector3 Tv = GeometricUtilities::BezierPatchTangentV( *it, 0.5, 0.5 );
		total += Vector3Ops::Magnitude( Vector3Ops::Cross( Tu, Tv ) );
	}
	return total > 0.0 ? total : 1.0;
}
