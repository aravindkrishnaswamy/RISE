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
#include "../Utilities/SurfaceCurvature.h"
#include <algorithm>
#include <atomic>


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
	//
	// DL-70 P3-f: track whether the negation ACTUALLY HAPPENED, not the
	// (subtly different) `!bRawFront` predicate -- `bRawFront` is false at
	// the measure-zero `dotND == 0` edge (grazing hit) too, where this `if`
	// does NOT fire, so `!bRawFront` alone would mark the normal as flipped
	// when it never was and `UnflippedGeomNormal()` would then negate an
	// already-correct normal.
	bool bDidFlip = false;
	if( dotND > 0.0 ) {
		N = N * -1.0;
		bDidFlip = true;
	}

	ri.bHit           = true;
	ri.range          = bh.dRange;
	ri.ptIntersection = ri.ray.PointAtLength( bh.dRange );
	ri.ptCoord        = Point2( bh.u, bh.v );
	ri.vNormal        = N;
	ri.vGeomNormal    = N;	// analytical surface: shading == geometric
	// The flip above orients the normal toward the ray on back-face hits
	// -- record it so consumers needing the TRUE surface facing
	// (RayCaster's x-ray self-hit test) can recover the unflipped sign.
	ri.bGeomNormalOrientedToRay = bDidFlip;

	// DL-220: this geometry deliberately does NOT set `ri.bOpenSheet`.
	// DL-96 originally stamped `ri.bOpenSheet = bDidFlip` on the premise
	// that "a patch never encloses a volume, so a double-sided hit here
	// is always an open sheet". That premise is false for the identical
	// reason DL-157 review round 3 removed `bProvablyNoInterior` below:
	// this class is not a single patch -- `patches` is a `BezierPatchList`
	// (a vector) with a BSP/Octree over it, `AddPatch` appends, and
	// `Job.cpp`'s `.bezier` loader puts EVERY patch of a file into ONE
	// geometry (`models/raw/teapot.bezier` declares 28; `aphrodite.bezier`
	// and `f16.bezier` are closed solids). At a genuine interior exit on
	// a closed Bezier solid, `dotND > 0` and `bDidFlip` is true; stamping
	// `bOpenSheet = true` causes `RayIntersectionGeometric::BSSRDFEntryFacing()`
	// to treat the interior wall hit as an open sheet and admit it as a
	// subsurface entry seeded from inside the object. Removing the setter
	// allows Bezier geometry to fall back to the DL-70 closed-solid gate
	// (`bOpenSheet == false`), which is the correct default for any geometry
	// that might enclose a volume.
	//
	// DL-157 review round 3 (P1): this geometry deliberately does NOT set
	// `ri.bProvablyNoInterior`.  Round 2 did, on the premise that "a
	// single patch cannot enclose a volume" -- but this class is not a
	// single patch: `patches` is a `BezierPatchList` (a vector) with a
	// BSP/Octree over it, `AddPatch` appends, and `Job.cpp`'s `.bezier`
	// loader puts EVERY patch of a file into ONE geometry
	// (`models/raw/teapot.bezier` declares 28; `aphrodite.bezier` and
	// `f16.bezier` are closed solids).  At a genuine interior exit on
	// such an object `dotND > 0` and `bDidFlip` is true, so the round-2
	// stamp claimed "no interior" on exactly the hit that proves there
	// is one.  Note also that the flip above is a WINDING artifact (the
	// comment on it says so: the teapot file traverses some patches CCW
	// and others CW), which is a second reason it cannot carry a
	// topological claim.  See `RayIntersectionGeometric::
	// bProvablyNoInterior`'s contract.

	// DL-20 (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 14 item 2, closed):
	// this hit already knows exactly which PATCH and (u, v) it landed on
	// (unlike the generic `ComputeSurfaceDerivatives(point, normal)` query
	// below, which is handed only a point and cannot tell which of
	// possibly many patches -- or which of a patch's two (u, v) preimages
	// -- it came from), so the real closed-form Weingarten map is a few
	// more Bernstein-basis evaluations away.  UNGATED like the analytic
	// primitives (Sphere/Ellipsoid): this is cheap, exact, data the hit
	// already produced.  Only `scaleHint` (nothing but `curv` reads it) is
	// gated.
	//
	// CONSERVATIVE on a FLIPPED hit (`bDidFlip`): `dpdu`/`dpdv` are the
	// patch's OWN parametric tangents, un-flipped -- by construction
	// `Cross(dpdu, dpdv)` is exactly the direction `N` had BEFORE the
	// ray-facing flip above, so pairing them with the FLIPPED reported
	// `ri.vNormal` would hand a consumer a left-handed (dpdu, dpdv, n)
	// frame while claiming `valid=true`.  Re-deriving a consistent frame
	// needs a real reparametrization (e.g. swapping the u/v roles, which
	// also swaps which second derivative is which) that risks disagreeing
	// with `ptCoord`'s own (u, v) elsewhere -- out of this row's scope.
	// Reporting honest absence here is strictly better than the fabricated
	// `valid=true` this replaced (DL-20's whole point).
	if( !bDidFlip ) {
		const Vector3 dpdu = GeometricUtilities::BezierPatchTangentU( *elem.pPatch, bh.u, bh.v );
		const Vector3 dpdv = GeometricUtilities::BezierPatchTangentV( *elem.pPatch, bh.u, bh.v );
		const Vector3 d2Pduu = GeometricUtilities::BezierPatchSecondDerivUU( *elem.pPatch, bh.u, bh.v );
		const Vector3 d2Pduv = GeometricUtilities::BezierPatchSecondDerivUV( *elem.pPatch, bh.u, bh.v );
		const Vector3 d2Pdvv = GeometricUtilities::BezierPatchSecondDerivVV( *elem.pPatch, bh.u, bh.v );

		Vector3 dndu, dndv;
		if( SurfaceCurvature::ShapeOperatorFromSecondDerivatives( dpdu, dpdv, d2Pduu, d2Pduv, d2Pdvv, dndu, dndv ) ) {
			// N was NOT flipped in this branch, so dndu/dndv (derivatives of
			// Normalize(Cross(dpdu,dpdv))) already match the reported `N`
			// with no further sign correction needed.
			ri.derivatives.dpdu  = dpdu;
			ri.derivatives.dpdv  = dpdv;
			ri.derivatives.dndu  = dndu;
			ri.derivatives.dndv  = dndv;
			ri.derivatives.valid = true;
			// No independently-known texcoord chart here (ptCoord IS (u, v)
			// already -- see the `ptCoord = Point2(bh.u, bh.v)` stamp above),
			// so the identity default (`texChartValid=false`) the struct's
			// own constructor sets is left alone.
			if( SurfaceCurvatureDemand::Any() ) {
				ri.derivatives.scaleHint = SurfaceCurvature::ScaleHintFromBoundingBox( GenerateBoundingBox() );
			}
		}
	}
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
  bUseBSP( bUseBSP_ ),
  m_areaOnce( new std::once_flag ),
  m_area( 0 )
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
	m_areaOnce.reset( new std::once_flag );	// DL-459: the area table is stale
}

void BezierPatchGeometry::Prepare()
{
	// Prepare for rendering
	// Optimize the patch container
	stl_utils::container_optimize< BezierPatchList >( patches );
	m_areaOnce.reset( new std::once_flag );	// DL-459: rebuilt lazily on first surface sample

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

namespace
{
	//! Blossom of a cubic Bezier segment at (t1, t2, t3).
	inline Point3 Blossom3( const Point3 P[4], const Scalar t1, const Scalar t2, const Scalar t3 )
	{
		Point3 a[3], b[2];
		for( int k = 0; k < 3; ++k ) a[k] = Point3Ops::WeightedAverage2( P[k+1], P[k], t1 );
		for( int k = 0; k < 2; ++k ) b[k] = Point3Ops::WeightedAverage2( a[k+1], a[k], t2 );
		return Point3Ops::WeightedAverage2( b[1], b[0], t3 );
	}

	//! Control points of the cubic restricted to [s0, s1].
	inline void SubSegment( const Point3 P[4], const Scalar s0, const Scalar s1, Point3 Q[4] )
	{
		Q[0] = Blossom3( P, s0, s0, s0 );
		Q[1] = Blossom3( P, s0, s0, s1 );
		Q[2] = Blossom3( P, s0, s1, s1 );
		Q[3] = Blossom3( P, s1, s1, s1 );
	}

	//! RIGOROUS upper bound of |dP/du x dP/dv| over [u0,u1] x [v0,v1]: the
	//! cross product of the sub-patch's tangents is a bidegree-(5,5)
	//! polynomial whose Bernstein coefficients are combinations of the
	//! difference vectors' cross products; by the convex-hull property its
	//! norm is at most the largest coefficient norm.
	Scalar CellJacobianBound( const BezierPatch& p, const Scalar u0, const Scalar u1, const Scalar v0, const Scalar v1 )
	{
		static const Scalar C2[3] = { 1, 2, 1 };
		static const Scalar C3[4] = { 1, 3, 3, 1 };
		static const Scalar C5[6] = { 1, 5, 10, 10, 5, 1 };
		Point3 R[4][4], Q[4][4];
		for( int i = 0; i < 4; ++i ) {
			SubSegment( p.c[i].pts, v0, v1, R[i] );			// restrict v (index j)
		}
		for( int l = 0; l < 4; ++l ) {
			const Point3 col[4] = { R[0][l], R[1][l], R[2][l], R[3][l] };
			Point3 sub[4];
			SubSegment( col, u0, u1, sub );					// restrict u (index i)
			for( int k = 0; k < 4; ++k ) Q[k][l] = sub[k];
		}
		Vector3 du[3][4], dv[4][3];
		for( int i = 0; i < 3; ++i ) for( int j = 0; j < 4; ++j ) du[i][j] = Vector3Ops::mkVector3( Q[i+1][j], Q[i][j] );
		for( int k = 0; k < 4; ++k ) for( int l = 0; l < 3; ++l ) dv[k][l] = Vector3Ops::mkVector3( Q[k][l+1], Q[k][l] );
		Vector3 c[6][6];
		for( int a = 0; a < 6; ++a ) for( int b = 0; b < 6; ++b ) c[a][b] = Vector3( 0, 0, 0 );
		for( int i = 0; i < 3; ++i ) for( int j = 0; j < 4; ++j )
		for( int k = 0; k < 4; ++k ) for( int l = 0; l < 3; ++l ) {
			const Scalar w = ( C2[i] * C3[k] / C5[i+k] ) * ( C3[j] * C2[l] / C5[j+l] );
			c[i+k][j+l] = c[i+k][j+l] + Vector3Ops::Cross( du[i][j], dv[k][l] ) * w;
		}
		Scalar m = 0;
		for( int a = 0; a < 6; ++a ) for( int b = 0; b < 6; ++b ) m = std::max( m, Vector3Ops::Magnitude( c[a][b] ) );
		// d/ds = 3 sum du B2 B3 per local axis; d/du = (1/(u1-u0)) d/ds.
		return Scalar( 9 ) * m / ( ( u1 - u0 ) * ( v1 - v0 ) );
	}

	inline Scalar PatchJacobian( const BezierPatch& p, const Scalar u, const Scalar v )
	{
		return Vector3Ops::Magnitude( Vector3Ops::Cross(
			GeometricUtilities::BezierPatchTangentU( p, u, v ),
			GeometricUtilities::BezierPatchTangentV( p, u, v ) ) );
	}
}

void BezierPatchGeometry::EnsureAreaTable() const
{
	std::call_once( *m_areaOnce, [this]() {
		static const Scalar xg[5] = { -0.9061798459386640, -0.5384693101056831, 0.0, 0.5384693101056831, 0.9061798459386640 };
		static const Scalar wg[5] = { 0.2369268850561891, 0.4786286704993665, 0.5688888888888889, 0.4786286704993665, 0.2369268850561891 };
		const Scalar h = Scalar( 1 ) / Scalar( nAreaCells );
		m_cellCdf.clear();
		m_cellBound.clear();
		m_cellCdf.reserve( patches.size() * nAreaCells * nAreaCells );
		m_cellBound.reserve( patches.size() * nAreaCells * nAreaCells );
		Scalar area = 0, acc = 0;
		for( const BezierPatch& p : patches ) {
			for( unsigned int cj = 0; cj < nAreaCells; ++cj ) {
				for( unsigned int ci = 0; ci < nAreaCells; ++ci ) {
					const Scalar u0 = h * Scalar( ci ), v0 = h * Scalar( cj );
					Scalar cellSum = 0;
					for( int a = 0; a < 5; ++a ) {
						const Scalar u = u0 + h * Scalar( 0.5 ) * ( xg[a] + 1 );
						for( int b = 0; b < 5; ++b ) {
							const Scalar v = v0 + h * Scalar( 0.5 ) * ( xg[b] + 1 );
							cellSum += wg[a] * wg[b] * PatchJacobian( p, u, v );
						}
					}
					area += cellSum * Scalar( 0.25 ) * h * h;
					const Scalar bound = CellJacobianBound( p, u0, u0 + h, v0, v0 + h );
					m_cellBound.push_back( bound );
					acc += bound;
					m_cellCdf.push_back( acc );
				}
			}
		}
		if( acc > 0 ) {
			for( Scalar& c : m_cellCdf ) c /= acc;
			m_cellCdf.back() = 1;
		} else {
			m_cellCdf.clear();
			m_cellBound.clear();
		}
		m_area = area;
	} );
}

void BezierPatchGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// DL-459: AREA-UNIFORM, density exactly 1/GetArea().  A cell of some
	// patch is drawn from the bound-weighted CDF (prand.z), a point uniform
	// in its (u, v) square (prand.x, prand.y), and accepted with
	// probability J / bound (J = |dP/du x dP/dv|, bound rigorous -- see the
	// header); accept variates and retries come from a stream seeded by
	// prand's bits.  Pre-DL-459 this picked a patch by J at its midpoint
	// and drew (u, v) uniformly -- uniform in area only for a parallelogram.
	EnsureAreaTable();
	if( patches.empty() || m_cellCdf.empty() ) {
		if( point )  *point  = Point3( 0, 0, 0 );
		if( normal ) *normal = Vector3( 0, 1, 0 );
		if( coord )  *coord  = Point2( 0, 0 );
		return;
	}

	const unsigned int cellsPerPatch = nAreaCells * nAreaCells;
	GeometricUtilities::PrandStream stream( prand, 0x42455A /*'BEZ'*/ );
	Scalar cz = prand.z, cx = prand.x, cy = prand.y;
	std::size_t cell = 0;
	Scalar u = 0, v = 0;
	static const int kMaxCandidates = 4096;
	bool accepted = false;
	for( int attempt = 0; attempt < kMaxCandidates && !accepted; ++attempt ) {
		const std::vector<Scalar>::const_iterator it =
			std::upper_bound( m_cellCdf.begin(), m_cellCdf.end(), cz );
		cell = std::min( std::size_t( it - m_cellCdf.begin() ), m_cellCdf.size() - 1 );
		const unsigned int local = static_cast<unsigned int>( cell % cellsPerPatch );
		const Scalar h = Scalar( 1 ) / Scalar( nAreaCells );
		u = h * ( Scalar( local % nAreaCells ) + r_min( r_max( cx, Scalar( 0 ) ), Scalar( 1 ) ) );
		v = h * ( Scalar( local / nAreaCells ) + r_min( r_max( cy, Scalar( 0 ) ), Scalar( 1 ) ) );
		accepted = stream.Next() * m_cellBound[cell] < PatchJacobian( patches[cell / cellsPerPatch], u, v );
		if( !accepted ) {
			cz = stream.Next(); cx = stream.Next(); cy = stream.Next();
		}
	}
	if( !accepted ) {
		static std::atomic<bool> warned{ false };
		bool expected = false;
		if( warned.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEasyWarning( "BezierPatchGeometry:: area sampler exhausted its rejection candidates (DL-459); that sample is not area-uniform" );
		}
	}

	const BezierPatch& p = patches[cell / cellsPerPatch];
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
	// DL-20: this geometry can hold MANY patches, and a point (with no
	// accompanying (u, v) or patch index -- unlike the real ray-hit path,
	// RayElementIntersection above, which knows both for free) cannot be
	// inverted back to "which patch, which (u, v) preimage" without an
	// ambiguous, expensive re-solve.  The pre-fix body fabricated an
	// arbitrary tangent frame around `objSpaceNormal` with `dndu=dndv=0`
	// and reported it as `valid=true` -- a FLAT curvature answer for a
	// genuinely curved surface, presented as legitimate.  Conservative
	// reject instead: report "no analytical derivatives available" (the
	// default-constructed, `valid=false` struct) exactly like the base
	// IGeometry contract already documents for a geometry that cannot
	// answer.  (void)-cast the otherwise-unused parameters to keep this a
	// real body, not a signature stub.
	(void)objSpacePoint;
	(void)objSpaceNormal;
	return SurfaceDerivatives();
}

Scalar BezierPatchGeometry::GetArea( ) const
{
	// DL-459: the Gauss-Legendre integral of the SAME area element
	// UniformRandomPoint samples exactly (5x5 nodes on each of nAreaCells^2
	// cells per patch: converged to rounding for a bicubic patch with a
	// non-vanishing Jacobian), so 1/GetArea() is its density.  Pre-DL-459
	// this was a one-point midpoint estimate.  An empty or degenerate set
	// keeps the legacy 1.0 placeholder.
	EnsureAreaTable();
	return m_area > 0.0 ? m_area : 1.0;
}
