#include <iostream>
#include <cassert>
#include <cmath>
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/IFunction2D.h"

using namespace RISE;
using namespace RISE::Implementation;

// A trivial IFunction2D that always returns the same value.
// Used to check that a known displacement produces the expected geometric offset.
class ConstFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar value;
	ConstFunction2D( const Scalar v ) : value( v ) {}
	Scalar Evaluate( const Scalar /*x*/, const Scalar /*y*/ ) const { return value; }
};

static bool IsClose( const Scalar a, const Scalar b, const Scalar eps )
{
	return std::fabs(a - b) <= eps;
}

static RayIntersectionGeometric MakeRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersectionGeometric( Ray( origin, dir ), nullRasterizerState );
}

// Build a default-parameter DisplacedGeometry wrapping an already-addref'd base.
// The DisplacedGeometry's own addref takes the count to 2; caller releases once.
static DisplacedGeometry* WrapSphere(
	IGeometry*          pBase,
	const unsigned int  detail,
	const IFunction2D*  displacement,
	const Scalar        disp_scale )
{
	return new DisplacedGeometry(
		pBase, detail, displacement, disp_scale,
		10, 8, false, true, false );
}

//-----------------------------------------------------------------------------
// Case 1: pure tessellation.  DisplacedGeometry with null displacement should
// produce a bbox within 1% of the plain sphere's analytic bbox.
//-----------------------------------------------------------------------------
static void TestPureTessellationBBox()
{
	std::cout << "Test 1: Pure tessellation bbox matches analytic sphere...\n";

	const Scalar R = 1.0;
	SphereGeometry* pSphere = new SphereGeometry( R );
	DisplacedGeometry* pDisp = WrapSphere( pSphere, 32, 0, 0.0 );

	assert( pDisp->IsValid() );

	const BoundingBox analytic = pSphere->GenerateBoundingBox();
	const BoundingBox tess     = pDisp->GenerateBoundingBox();

	// Tessellated sphere is slightly inscribed inside the analytic sphere
	// (flat facets).  At detail=32 the discrepancy should be <5%.
	const Scalar tol = 0.05 * R;
	assert( std::fabs( analytic.ll.x - tess.ll.x ) < tol );
	assert( std::fabs( analytic.ur.x - tess.ur.x ) < tol );
	assert( std::fabs( analytic.ll.y - tess.ll.y ) < tol );
	assert( std::fabs( analytic.ur.y - tess.ur.y ) < tol );

	pDisp->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 2: constant displacement pushes the surface out by k along the normal.
// Ray from far along +Z hits the displaced surface at distance ≈ (origin_z - (R+k)).
//-----------------------------------------------------------------------------
static void TestConstantDisplacement()
{
	std::cout << "Test 2: Constant displacement produces expected offset...\n";

	const Scalar R = 1.0;
	const Scalar k = 0.1;

	SphereGeometry* pSphere = new SphereGeometry( R );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );  // evaluate → 1.0

	DisplacedGeometry* pDisp = WrapSphere( pSphere, 48, pConst, k );
	assert( pDisp->IsValid() );

	// Ray from (0, 0, 10) along -Z should hit the +Z pole of the displaced sphere.
	RayIntersectionGeometric ri = MakeRI( Point3( 0.0, 0.0, 10.0 ), Vector3( 0.0, 0.0, -1.0 ) );
	pDisp->IntersectRay( ri, true, false, false );

	assert( ri.bHit );

	// Expected hit distance is (10 - (R + k)) = 10 - 1.1 = 8.9.
	// Also assert the hit is clearly FURTHER than the analytic sphere (10 - R = 9.0) by
	// at least k/2 — a regression where disp_scale silently collapsed would still land
	// near 9.0 and the loose "close to 8.9" check alone wouldn't catch it.
	const Scalar expected         = 10.0 - (R + k);
	const Scalar plain_sphere_hit = 10.0 - R;
	const Scalar tol = 0.02;
	assert( IsClose( ri.range, expected, tol ) );
	assert( ri.range < plain_sphere_hit - 0.5 * k );

	pDisp->release();
	pConst->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 3: disp_scale=0 is identical to displacement=nullptr (bbox equality).
//-----------------------------------------------------------------------------
static void TestZeroScaleInvariant()
{
	std::cout << "Test 3: disp_scale=0 matches null displacement...\n";

	const Scalar R = 1.0;

	SphereGeometry* pSphere1 = new SphereGeometry( R );
	SphereGeometry* pSphere2 = new SphereGeometry( R );
	ConstFunction2D* pConst  = new ConstFunction2D( 1.0 );

	DisplacedGeometry* pNoDisp = WrapSphere( pSphere1, 32, 0,      0.0 );
	DisplacedGeometry* pZero   = WrapSphere( pSphere2, 32, pConst, 0.0 );

	assert( pNoDisp->IsValid() );
	assert( pZero->IsValid() );

	const BoundingBox a = pNoDisp->GenerateBoundingBox();
	const BoundingBox b = pZero->GenerateBoundingBox();

	assert( IsClose( a.ll.x, b.ll.x, 1e-9 ) );
	assert( IsClose( a.ur.x, b.ur.x, 1e-9 ) );
	assert( IsClose( a.ll.y, b.ll.y, 1e-9 ) );
	assert( IsClose( a.ur.y, b.ur.y, 1e-9 ) );
	assert( IsClose( a.ll.z, b.ll.z, 1e-9 ) );
	assert( IsClose( a.ur.z, b.ur.z, 1e-9 ) );

	pZero->release();
	pNoDisp->release();
	pConst->release();
	pSphere2->release();
	pSphere1->release();
}

//-----------------------------------------------------------------------------
// A nonconstant IFunction2D used to exercise the pole-crack regression.
// Varies strongly with u, so without pole-UV canonicalization the pole-row
// vertices displace to different heights along the pole normal, fanning into
// radial spikes.
//-----------------------------------------------------------------------------
class UVaryingFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar x, const Scalar /*y*/ ) const { return x; }
};

//-----------------------------------------------------------------------------
// Case 4a: sphere pole stays closed under u-varying displacement.  Fires rays
// directly down the +Y and -Y axes — the pole vertices.  If the pole had split
// into a spike/crack, a ray from outside would either miss or hit a much
// thinner/taller silhouette than expected.  We check that the hit distance
// differs from the analytic sphere by at most the maximum displacement range.
//-----------------------------------------------------------------------------
static void TestPoleClosedUnderUVaryingDisplacement()
{
	std::cout << "Test 4a: sphere poles stay closed under u-varying displacement...\n";

	const Scalar R = 1.0;
	const Scalar k = 0.1;

	SphereGeometry* pSphere = new SphereGeometry( R );
	UVaryingFunction2D* pVar = new UVaryingFunction2D();

	DisplacedGeometry* pDisp = WrapSphere( pSphere, 64, pVar, k );
	assert( pDisp->IsValid() );

	// Fire four axis-parallel rays from above, at tiny offsets in XZ around the
	// north pole axis.  After the pole-UV-canonicalization fix every pole vertex
	// shares one UV → one displacement → the cap is a single flat disc.  All four
	// rays must therefore hit at very close to the same Y, i.e. the same range.
	//
	// Pre-fix, pole vertices displaced to different heights per azimuthal u, so
	// the cap fanned into spikes / valleys and the four rays would report wildly
	// varying ranges (up to the full disp_scale apart).  A small tolerance (much
	// tighter than k) is sufficient to distinguish the fixed from the broken case.
	const Scalar nearAxis = 0.001;  // well inside the pole-vertex cluster
	const Point3 origins[4] = {
		Point3(  nearAxis, 10.0,  0.0 ),
		Point3( -nearAxis, 10.0,  0.0 ),
		Point3(  0.0,      10.0,  nearAxis ),
		Point3(  0.0,      10.0, -nearAxis ),
	};
	Scalar ranges[4];
	for( int n = 0; n < 4; n++ ) {
		RayIntersectionGeometric ri = MakeRI( origins[n], Vector3( 0.0, -1.0, 0.0 ) );
		pDisp->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		ranges[n] = ri.range;
	}
	const Scalar rng_min = r_min( r_min( ranges[0], ranges[1] ), r_min( ranges[2], ranges[3] ) );
	const Scalar rng_max = r_max( r_max( ranges[0], ranges[1] ), r_max( ranges[2], ranges[3] ) );
	// Pole cap is uniform — max - min should be a tiny fraction of the full
	// disp_scale (pre-fix it would be comparable to k).
	assert( rng_max - rng_min < 0.1 * k );

	// Sanity: hit is somewhere reasonable (analytic sphere ± displacement range).
	assert( ranges[0] > 10.0 - (R + k) - 0.01 );
	assert( ranges[0] < 10.0 - R + 0.01 );

	// Repeat at the south pole.
	const Point3 southOrigins[4] = {
		Point3(  nearAxis, -10.0,  0.0 ),
		Point3( -nearAxis, -10.0,  0.0 ),
		Point3(  0.0,      -10.0,  nearAxis ),
		Point3(  0.0,      -10.0, -nearAxis ),
	};
	Scalar southRanges[4];
	for( int n = 0; n < 4; n++ ) {
		RayIntersectionGeometric ri = MakeRI( southOrigins[n], Vector3( 0.0, 1.0, 0.0 ) );
		pDisp->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		southRanges[n] = ri.range;
	}
	const Scalar s_min = r_min( r_min( southRanges[0], southRanges[1] ), r_min( southRanges[2], southRanges[3] ) );
	const Scalar s_max = r_max( r_max( southRanges[0], southRanges[1] ), r_max( southRanges[2], southRanges[3] ) );
	assert( s_max - s_min < 0.1 * k );

	pDisp->release();
	pVar->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 4: rays that graze the u-seam on a sphere both hit.  Without duplicated
// seam vertices, displacement could open a crack there.
//-----------------------------------------------------------------------------
static void TestSeamContinuity()
{
	std::cout << "Test 4: u-seam continuity under non-trivial displacement...\n";

	const Scalar R = 1.0;
	const Scalar k = 0.05;

	SphereGeometry* pSphere = new SphereGeometry( R );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );

	DisplacedGeometry* pDisp = WrapSphere( pSphere, 64, pConst, k );
	assert( pDisp->IsValid() );

	// Two rays: one grazes the seam at u≈0 (position on -X side), one at u≈1.
	// In the sphere's parameterization (vForward=-X), u=0 is the -X meridian.
	// Fire a ray straight into -X from +X direction — should hit at ≈ -(R+k) radial.
	{
		RayIntersectionGeometric ri = MakeRI( Point3( -10.0, 0.0, 0.0 ), Vector3( 1.0, 0.0, 0.0 ) );
		pDisp->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
		// Hit at x ≈ -(R+k), distance from origin = 10 - (R+k)
		const Scalar expected = 10.0 - (R + k);
		assert( IsClose( ri.range, expected, 0.02 ) );
	}

	// Second ray from the opposite side of the seam (slightly off meridian)
	// still hits with no sudden discontinuity.
	{
		RayIntersectionGeometric ri = MakeRI( Point3( -10.0, 0.0, 0.01 ), Vector3( 1.0, 0.0, 0.0 ) );
		pDisp->IntersectRay( ri, true, false, false );
		assert( ri.bHit );
	}

	pDisp->release();
	pConst->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 5: InfinitePlaneGeometry base.  Construction succeeds but IsValid()
// returns false; IntersectRay returns bHit=false.
//-----------------------------------------------------------------------------
static void TestInfinitePlaneFailSafe()
{
	std::cout << "Test 5: InfinitePlane base is refused loud but safe...\n";

	InfinitePlaneGeometry* pPlane = new InfinitePlaneGeometry( 1.0, 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pPlane, 16, 0, 0.0,
		10, 8, false, true, false );

	assert( !pDisp->IsValid() );

	// Ray should miss (no crash, no hit)
	RayIntersectionGeometric ri = MakeRI( Point3( 0.0, 0.0, 10.0 ), Vector3( 0.0, 0.0, -1.0 ) );
	pDisp->IntersectRay( ri, true, false, false );
	assert( !ri.bHit );

	pDisp->release();
	pPlane->release();
}

//-----------------------------------------------------------------------------
// Case 6: UniformRandomPoint lands on the displaced surface — the point's
// position is close to (R+k) from origin for a constant-displaced sphere.
//-----------------------------------------------------------------------------
static void TestUniformRandomPointOnSurface()
{
	std::cout << "Test 6: UniformRandomPoint lies on the displaced surface...\n";

	const Scalar R = 1.0;
	const Scalar k = 0.1;

	SphereGeometry* pSphere = new SphereGeometry( R );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );

	DisplacedGeometry* pDisp = WrapSphere( pSphere, 48, pConst, k );
	assert( pDisp->IsValid() );

	// Sample a few deterministic points.
	for( int i = 1; i <= 5; i++ )
	{
		const Scalar u = Scalar(i) / 6.0;
		const Scalar v = Scalar((i * 7) % 6) / 6.0;
		const Scalar w = Scalar((i * 11) % 6) / 6.0;
		Point3  pt;
		Vector3 nrm;
		Point2  uv;
		pDisp->UniformRandomPoint( &pt, &nrm, &uv, Point3( u, v, w ) );

		const Scalar r = std::sqrt( pt.x*pt.x + pt.y*pt.y + pt.z*pt.z );
		// The sampled point should be within a triangle-size tolerance of R+k.
		assert( IsClose( r, R + k, 0.05 ) );
	}

	pDisp->release();
	pConst->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 7: nested composition — DisplacedGeometry wrapping another
// DisplacedGeometry compiles and produces a valid outer mesh.
//-----------------------------------------------------------------------------
static void TestNestedComposition()
{
	std::cout << "Test 7: Nested DisplacedGeometry composition works...\n";

	const Scalar R = 1.0;

	SphereGeometry* pSphere = new SphereGeometry( R );
	ConstFunction2D* pConstInner = new ConstFunction2D( 1.0 );
	ConstFunction2D* pConstOuter = new ConstFunction2D( 1.0 );

	DisplacedGeometry* pInner = WrapSphere( pSphere, 32, pConstInner, 0.05 );
	assert( pInner->IsValid() );

	DisplacedGeometry* pOuter = WrapSphere( pInner, 16, pConstOuter, 0.05 );
	assert( pOuter->IsValid() );

	// Outer bbox should be at least as large as inner's.
	const BoundingBox innerBB = pInner->GenerateBoundingBox();
	const BoundingBox outerBB = pOuter->GenerateBoundingBox();

	assert( outerBB.ur.x >= innerBB.ur.x - 0.01 );
	assert( outerBB.ll.x <= innerBB.ll.x + 0.01 );

	pOuter->release();
	pInner->release();
	pConstOuter->release();
	pConstInner->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 8: face-normal indexed-mesh base displaces along the face normal, not +Z.
// Builds a TriangleMeshGeometryIndexed with bUseFaceNormals=true containing a
// quad in the YZ plane facing +X.  Wraps it with a constant displacement +0.1.
// The wrapped surface should sit at x = 1.1, not at z = 0.1 (which is what
// would happen if the pass-through emitted a (0,0,1) placeholder normal for
// face-normal source meshes).
//-----------------------------------------------------------------------------
static void TestFaceNormalMeshDisplacesAlongFaceNormal()
{
	std::cout << "Test 8: face-normal indexed-mesh base displaces along its face normal...\n";

	const Scalar k = 0.1;

	// Build a quad in the YZ plane at x = 1, facing +X.
	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed(
		10, 8, false, false, /*bUseFaceNormals=*/true );
	pMesh->BeginIndexedTriangles();
	pMesh->AddVertex( Point3( 1.0, -0.5, -0.5 ) );
	pMesh->AddVertex( Point3( 1.0,  0.5, -0.5 ) );
	pMesh->AddVertex( Point3( 1.0,  0.5,  0.5 ) );
	pMesh->AddVertex( Point3( 1.0, -0.5,  0.5 ) );
	pMesh->AddTexCoord( Point2( 0.0, 0.0 ) );
	pMesh->AddTexCoord( Point2( 1.0, 0.0 ) );
	pMesh->AddTexCoord( Point2( 1.0, 1.0 ) );
	pMesh->AddTexCoord( Point2( 0.0, 1.0 ) );
	// AddNormals is skipped because bUseFaceNormals=true rejects them internally.
	IndexedTriangle t1;
	t1.iVertices[0] = 0; t1.iVertices[1] = 1; t1.iVertices[2] = 2;
	t1.iCoords[0]   = 0; t1.iCoords[1]   = 1; t1.iCoords[2]   = 2;
	t1.iNormals[0]  = 0; t1.iNormals[1]  = 0; t1.iNormals[2]  = 0;
	pMesh->AddIndexedTriangle( t1 );
	IndexedTriangle t2;
	t2.iVertices[0] = 0; t2.iVertices[1] = 2; t2.iVertices[2] = 3;
	t2.iCoords[0]   = 0; t2.iCoords[1]   = 2; t2.iCoords[2]   = 3;
	t2.iNormals[0]  = 0; t2.iNormals[1]  = 0; t2.iNormals[2]  = 0;
	pMesh->AddIndexedTriangle( t2 );
	pMesh->DoneIndexedTriangles();

	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );

	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pMesh, 0, pConst, k,
		10, 8, false, true, false );
	assert( pDisp->IsValid() );

	// Ray from (10, 0, 0) toward -X should hit the displaced quad at x = 1 + k = 1.1.
	RayIntersectionGeometric ri = MakeRI( Point3( 10.0, 0.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ) );
	pDisp->IntersectRay( ri, true, false, false );
	assert( ri.bHit );
	assert( IsClose( ri.range, 10.0 - (1.0 + k), 0.01 ) );

	// Sanity: ray along -Z from (0, 0, 10) must NOT hit the quad — if the pre-fix
	// placeholder-normal bug were still present, the quad would have displaced
	// into the XY plane at z = +0.1 and intercepted this ray.
	RayIntersectionGeometric riZ = MakeRI( Point3( 0.0, 0.0, 10.0 ), Vector3( 0.0, 0.0, -1.0 ) );
	pDisp->IntersectRay( riZ, true, false, false );
	assert( !riZ.bHit );

	pDisp->release();
	pConst->release();
	pMesh->release();
}

int main()
{
	TestPureTessellationBBox();
	TestConstantDisplacement();
	TestZeroScaleInvariant();
	TestPoleClosedUnderUVaryingDisplacement();
	TestSeamContinuity();
	TestInfinitePlaneFailSafe();
	TestUniformRandomPointOnSurface();
	TestNestedComposition();
	TestFaceNormalMeshDisplacesAlongFaceNormal();

	std::cout << "All DisplacedGeometry tests passed.\n";
	return 0;
}
