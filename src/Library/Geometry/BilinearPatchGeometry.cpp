//////////////////////////////////////////////////////////////////////
//
//  BilinearPatchGeometry.cpp - Implementation of the sphere class
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
#include "BilinearPatchGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Interfaces/ILog.h"
#include "../Octree.h"
#include "../BSPTreeSAH.h"
#include "../Utilities/stl_utils.h"
#include <algorithm>   // std::lower_bound for the area-CDF search

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// BilinearPatch specialization required for the octree
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

bool BilinearPatchGeometry::ElementBoxIntersection( const MYOBJ elem, const BoundingBox& bbox ) const
{
	return bbox.DoIntersect( GeometricUtilities::BilinearPatchBoundingBox(*elem) );
}

BoundingBox BilinearPatchGeometry::GetElementBoundingBox( const MYOBJ elem ) const
{
	return GeometricUtilities::BilinearPatchBoundingBox(*elem);
}

char BilinearPatchGeometry::WhichSideofPlaneIsElement( const MYOBJ elem, const Plane& plane ) const
{
	return GeometricUtilities::WhichSideOfPlane( plane, GeometricUtilities::BilinearPatchBoundingBox(*elem) );
}

void BilinearPatchGeometry::RayElementIntersection( RayIntersectionGeometric& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// This is where we call the Ray Intersection function
	const BilinearPatch&	patch = *elem;

    BILINEAR_HIT	h;
	RayBilinearPatchIntersection( ri.ray, h, patch );

	if( h.bHit ) {
		// If there is an intersection, compute the point of intersection, the normal and the
		// texture co-ordinates
		ri.bHit = true;
		ri.range = h.dRange;
		ri.ptIntersection = ri.ray.PointAtLength( h.dRange );
		ri.ptCoord = Point2( h.u, h.v );
		ri.vNormal = Vector3Ops::Normalize(GeometricUtilities::BilinearPatchNormalAt( patch, h.u, h.v ));
		ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric
	}
}

void BilinearPatchGeometry::RayElementIntersection( RayIntersection& ri, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	RayElementIntersection( ri.geometric, elem, bHitFrontFaces, bHitBackFaces );
}

bool BilinearPatchGeometry::RayElementIntersection_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const MYOBJ elem, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// This is where we call the Ray Intersection function
	const BilinearPatch&	patch = *elem;

    BILINEAR_HIT	h;
	RayBilinearPatchIntersection( ray, h, patch );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void BilinearPatchGeometry::SerializeElement( IWriteBuffer& buffer, const MYOBJ elem ) const
{
	//@ TODO : to be implemented
}

void BilinearPatchGeometry::DeserializeElement( IReadBuffer& buffer, MYOBJ& ret ) const
{
	//@ TODO : to be implemented
}

BilinearPatchGeometry::BilinearPatchGeometry(
	const unsigned int max_polys_per_node, 
	const unsigned char max_recursion_level,
	const bool bUseBSP_
	) : 
  pBSPTree( 0 ),
  pOctree( 0 ),
  nMaxPerOctantNode( max_polys_per_node ),
  nMaxRecursionLevel( max_recursion_level ),
  bUseBSP( bUseBSP_ ),
  dTotalArea( 0 )
{
}

BilinearPatchGeometry::~BilinearPatchGeometry( )
{
	safe_release( pBSPTree );
	safe_release( pOctree );
}

void BilinearPatchGeometry::AddPatch( const BilinearPatch& patch )
{
	patches.push_back( patch );
}

void BilinearPatchGeometry::Prepare()
{
	// Prepare for rendering
	// Optimize the patch container
	stl_utils::container_optimize< BilinearPatchList >( patches );

	// First create the pointer patches list
	BilinearPatchPtrList		patchptrs;
	patchptrs.reserve( patches.size() );		// Points to all the bilinear patches

	for( unsigned int i=0; i<patches.size(); i++ ) {
		const BilinearPatch* b = &patches[i];
		patchptrs.push_back( b );
	}

	// We're done so stuff all the bilinear patches into the bsp-tree

	// Compute the bounding box
	BoundingBox overall( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );
	BilinearPatchList::const_iterator		m, n;
	for( m=patches.begin(), n=patches.end(); m!=n; m++ ) {
		overall.Include( GeometricUtilities::BilinearPatchBoundingBox( *m ) );
	}

	safe_release( pBSPTree );
	safe_release( pOctree );

	if( bUseBSP ) {
		pBSPTree = new BSPTreeSAH<const BilinearPatch*>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pBSPTree, __FILE__, __LINE__, "bilinear patches bsptree" );

		pBSPTree->AddElements( patchptrs, nMaxRecursionLevel );
	} else {
		pOctree = new Octree<const BilinearPatch*>( *this, overall, nMaxPerOctantNode );
		GlobalLog()->PrintNew( pOctree, __FILE__, __LINE__, "bilinear patches octree" );

		pOctree->AddElements( patchptrs, nMaxRecursionLevel );
	}

	// Build the area-light sampling tables.  Done ONCE here rather than per
	// UniformRandomPoint call: the table is O(patches * nAreaCells^2) to
	// build, and NEE calls the sampler once per shadow ray.
	vAreaCDF.clear();
	vAreaCDF.reserve( patches.size() * nAreaCells * nAreaCells );
	dTotalArea = 0;
	for( BilinearPatchList::const_iterator p = patches.begin(); p != patches.end(); ++p ) {
		// CANONICAL RISE CORNER MAPPING -- pts[] is NOT row-major:
		//   pts[0] at (u=0, v=0)    pts[1] at (u=0, v=1)
		//   pts[2] at (u=1, v=0)    pts[3] at (u=1, v=1)
		// as implemented by GeometricUtilities::EvaluateBilinearPatchAt,
		// RayBilinearPatchIntersection and TessellateToMesh.  The helper
		// takes (c00, c10, c11, c01), so pts[1] and pts[2] SWAP here.
		// Getting this wrong samples a different surface than IntersectRay
		// traces on any non-planar patch -- the exact defect the pre-fix
		// row-major tessellation had (see GeometryUVRoundtripTest's header).
		dTotalArea += GeometricUtilities::AccumulateBilinearAreaCDF(
			p->pts[0], p->pts[2], p->pts[3], p->pts[1],
			nAreaCells, dTotalArea, vAreaCDF );
	}
}

bool BilinearPatchGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( patches.empty() || detail < 1 ) {
		return false;
	}

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int rowStride = nU + 1;

	for( BilinearPatchList::const_iterator it = patches.begin(); it != patches.end(); ++it ) {
		const BilinearPatch& patch = *it;

		const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );

		for( unsigned int j = 0; j <= nV; j++ ) {
			const Scalar v = Scalar(j) / Scalar(nV);
			for( unsigned int i = 0; i <= nU; i++ ) {
				const Scalar u = Scalar(i) / Scalar(nU);

				// Match the canonical RISE bilinear convention used by
				// GeometricUtilities::EvaluateBilinearPatchAt and by
				// RayBilinearPatchIntersection (and asserted by
				// tests/GeometricUtilitiesTest.cpp):
				//   pts[0] at (u=0, v=0)
				//   pts[1] at (u=0, v=1)
				//   pts[2] at (u=1, v=0)
				//   pts[3] at (u=1, v=1)
				//
				// Pre-fix this body used the row-major convention (pts[1] at
				// (1, 0), pts[2] at (1, 1)), which gave a different surface
				// from what IntersectRay traces — so IntersectRay's (u, v)
				// did not roundtrip through this forward formula.
				const Scalar w00 = (1.0 - u) * (1.0 - v);  // pts[0] at (0, 0)
				const Scalar w01 = (1.0 - u) *        v;   // pts[1] at (0, 1)
				const Scalar w10 =        u  * (1.0 - v);  // pts[2] at (1, 0)
				const Scalar w11 =        u  *        v;   // pts[3] at (1, 1)

				const Point3 pos(
					w00 * patch.pts[0].x + w01 * patch.pts[1].x + w10 * patch.pts[2].x + w11 * patch.pts[3].x,
					w00 * patch.pts[0].y + w01 * patch.pts[1].y + w10 * patch.pts[2].y + w11 * patch.pts[3].y,
					w00 * patch.pts[0].z + w01 * patch.pts[1].z + w10 * patch.pts[2].z + w11 * patch.pts[3].z );

				vertices.push_back( pos );
				normals.push_back( Vector3(0.0, 0.0, 0.0) );  // re-averaged below
				coords.push_back( Point2( u, v ) );
			}
		}

		for( unsigned int j = 0; j < nV; j++ ) {
			for( unsigned int i = 0; i < nU; i++ ) {
				const unsigned int a = baseIdx + j     * rowStride + i;
				const unsigned int b = baseIdx + j     * rowStride + (i + 1);
				const unsigned int c = baseIdx + (j+1) * rowStride + i;
				const unsigned int d = baseIdx + (j+1) * rowStride + (i + 1);

				tris.push_back( MakeIndexedTriangleSameIdx( a, b, c ) );
				tris.push_back( MakeIndexedTriangleSameIdx( b, d, c ) );
			}
		}
	}

	RecomputeVertexNormalsFromTopology( tris, vertices, normals );

	return true;
}

void BilinearPatchGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( pBSPTree ) {
		pBSPTree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		pOctree->IntersectRay( ri, bHitFrontFaces, bHitBackFaces );
	}
}

bool BilinearPatchGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( pBSPTree ) {
		return pBSPTree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	} else if( pOctree ) {
		return pOctree->IntersectRay_IntersectionOnly( ray, dHowFar, bHitFrontFaces, bHitBackFaces );
	}

	return false;
}

void BilinearPatchGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	Point3	ptMin( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3	ptMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ;

	// Go through all the points and calculate the minimum and maximum values from the
	// entire set.
	BilinearPatchList::const_iterator m, n;
	for( m=patches.begin(), n=patches.end(); m!=n; m++ )
	{
		const BilinearPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			const Point3& pt = p.pts[j];
			if( pt.x < ptMin.x ) ptMin.x = pt.x;
			if( pt.y < ptMin.y ) ptMin.y = pt.y;
			if( pt.z < ptMin.z ) ptMin.z = pt.z;
			if( pt.x > ptMax.x ) ptMax.x = pt.x;
			if( pt.y > ptMax.y ) ptMax.y = pt.y;
			if( pt.z > ptMax.z ) ptMax.z = pt.z;
		}
	}

	// The center is the center of the minimum and maximum values of the points
	ptCenter = Point3Ops::WeightedAverage2( ptMin, ptMax, 0.5 );
	radius = 0;

	// Go through all the points again and calculate the radius of the sphere
	for( m=patches.begin(), n=patches.end(); m!=n; m++ ) {
		const BilinearPatch&	p = *m;
		for( int j=0; j<4; j++ ) {
			const Point3& pt = p.pts[j];
			Vector3			r = Vector3Ops::mkVector3( pt, ptCenter );
			const Scalar	d = Vector3Ops::Magnitude(r);

			if( d > radius ) {
				radius = d;
			}
		}
	}
}

BoundingBox BilinearPatchGeometry::GenerateBoundingBox() const
{
	if( bUseBSP && pBSPTree ) {
		return pBSPTree->GetBBox();
	} else if( pOctree ) {
		return pOctree->GetBBox();
	}
	
	return BoundingBox();
}

void BilinearPatchGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// Area-proportional sampling over every (patch, cell) pair via the single
	// global CDF built in Prepare().  Consumers (NEE, photon emission, SSS)
	// use 1/GetArea() as the reciprocal position PDF, so selection MUST be
	// proportional to area -- selecting patches or cells uniformly by count
	// would reproduce exactly the BoxGeometry defect fixed in 098d2565.
	//
	// Was previously an empty stub: it wrote nothing, so with Point3's
	// zero-initialising default constructor every caller received the local
	// origin.  A `bilinearpatch_geometry` bound to an emissive material was
	// therefore a point-mass emitter, not a surface one -- the same pathology
	// as the VCM torus-arealight deficit.  CanBeAreaLight() now also refuses
	// the degenerate cases this function can no longer serve.
	if( patches.empty() || vAreaCDF.empty() || dTotalArea <= 0 ) {
		if( point )  *point  = Point3( 0, 0, 0 );
		if( normal ) *normal = Vector3( 0, 1, 0 );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	// prand.z selects (patch, cell); prand.x / prand.y place the point within
	// that cell -- the same variate roles BoxGeometry and the triangle meshes
	// use, so samplers that stratify a particular dimension behave alike.
	const Scalar target = prand.z * dTotalArea;
	std::vector<Scalar>::const_iterator it =
		std::lower_bound( vAreaCDF.begin(), vAreaCDF.end(), target );
	size_t flat = ( it == vAreaCDF.end() )
		? vAreaCDF.size() - 1
		: static_cast<size_t>( std::distance( vAreaCDF.begin(), it ) );

	const size_t cellsPerPatch = static_cast<size_t>( nAreaCells ) * nAreaCells;
	const size_t patchIdx = flat / cellsPerPatch;
	const size_t cellIdx  = flat % cellsPerPatch;
	if( patchIdx >= patches.size() ) {
		if( point )  *point  = Point3( 0, 0, 0 );
		if( normal ) *normal = Vector3( 0, 1, 0 );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	const BilinearPatch& bp = patches[patchIdx];
	Scalar u = 0, v = 0;
	GeometricUtilities::BilinearCellToUV(
		static_cast<unsigned int>( cellIdx ), nAreaCells, prand.x, prand.y, u, v );

	if( point ) {
		// Same canonical corner mapping as Prepare() -- pts[1]/pts[2] swap.
		*point = GeometricUtilities::BilinearForward(
			bp.pts[0], bp.pts[2], bp.pts[3], bp.pts[1], u, v );
	}

	if( normal ) {
		const Vector3 Tu = GeometricUtilities::BilinearTangentU(
			bp.pts[0], bp.pts[2], bp.pts[3], bp.pts[1], v );
		const Vector3 Tv = GeometricUtilities::BilinearTangentV(
			bp.pts[0], bp.pts[2], bp.pts[3], bp.pts[1], u );
		const Vector3 N = Vector3Ops::Cross( Tu, Tv );
		const Scalar nLen = Vector3Ops::Magnitude( N );
		*normal = ( nLen > NEARZERO ) ? ( N * ( 1.0 / nLen ) ) : Vector3( 0, 1, 0 );
	}

	if( coord ) {
		*coord = Point2( u, v );
	}
}

SurfaceDerivatives BilinearPatchGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
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

Scalar BilinearPatchGeometry::GetArea( ) const
{
	// The cached total from Prepare(), computed by the SAME quadrature the
	// sampler's CDF uses -- so 1/GetArea() is exactly UniformRandomPoint's
	// density.  That identity, not the absolute accuracy of the quadrature,
	// is what area-light unbiasedness depends on.
	//
	// Previously a hardcoded 1.0, which made a patch's emitted power wrong by
	// its true area (and, since consumers divide by it, silently rescaled
	// every estimator built on this geometry).
	return dTotalArea;
}
