#include <iostream>
#include <cassert>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/IFunction2D.h"
// Doc 89 slice D (displaced-detail composition audit): builds a lathe_geometry
// base mesh via the same RISE_API factory the scene parser uses, so the
// composition tests below exercise the REAL builder output, not a hand-rolled
// stand-in.
#include "../src/Library/RISE_API.h"
#include <algorithm>

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
	// Tier A2 cleanup (2026-04-27): max_polys/max_recur/bUseBSP are gone.
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pBase, detail, displacement, disp_scale,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	// DEFERRED REALIZATION (2026-06-13): the mesh is no longer baked in the
	// constructor.  Direct (non-render-pipeline) consumers like this test must
	// Realize() before querying geometry — the render pipeline does this in
	// RayCaster::AttachScene.  Single-threaded here, so it is safe.
	pDisp->Realize();
	return pDisp;
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
// Case 5: InfinitePlaneGeometry base.  An infinite plane cannot be
// tessellated (IGeometry::CanTessellate()==false), so DisplacedGeometry
// REFUSES it as an invalid recipe at construction (IsValid()==false) — the
// cheap parse-time validation that honors "validate cheaply at parse, defer
// only the bake".  The production factory (RISE_API_CreateDisplacedGeometry)
// surfaces this as a parse error.  Even if a caller ignores the refusal and
// forces Realize() anyway, every query guard-fails safely (no crash, no hit).
//-----------------------------------------------------------------------------
static void TestInfinitePlaneFailSafe()
{
	std::cout << "Test 5: InfinitePlane base is refused loud but safe...\n";

	InfinitePlaneGeometry* pPlane = new InfinitePlaneGeometry( 1.0, 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pPlane, 16, 0, 0.0,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );

	// The base reports it cannot tessellate, so the recipe is REFUSED at parse.
	assert( !pPlane->CanTessellate() );
	assert( !pDisp->IsValid() );

	// Defensive: even if a caller ignores IsValid() and forces Realize() + a
	// ray, it must not crash and must miss (the release-mode guard-and-fail).
	pDisp->Realize();
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
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/true );
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
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	assert( pDisp->IsValid() );
	pDisp->Realize();	// deferred bake (see WrapSphere note)

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

//-----------------------------------------------------------------------------
// Doc 89 slice D audit: displaced_geometry composed over a BUILDER mesh
// (lathe_geometry), not just an analytic primitive.  This is the audit's
// headline PASS case -- derive, bake, and confirm the displacement actually
// moved vertices, bounded by the painter's own amplitude.  A ConstFunction2D
// of value 1.0 displaces every non-degenerate vertex by disp_scale along its
// OWN per-vertex normal (the smooth lathe bake's normal, copied verbatim by
// TessellateToMesh's pass-through BEFORE any post-displacement normal
// recompute -- see TestDisplacedOverPinchedLatheTears below and the audit
// report for what that recompute does to SHADING, which this test does not
// exercise), so the max (and min) position deviation across the whole mesh
// should land close to disp_scale.
//-----------------------------------------------------------------------------
static void TestDisplacedOverLatheAppliesDisplacement()
{
	std::cout << "Test 9: displaced_geometry composes over a lathe_geometry builder mesh...\n";

	// Simple UNPINCHED vase profile (the only r=0 points are the two ends,
	// which are ordinary poles, not an INTERIOR pinch -- see the pinch test
	// below for that case).
	const double prof[] = { 0.0, 0.0,  0.7, 0.2,  0.9, 1.0,  0.5, 1.8,  0.0, 2.0 };
	LatheDescriptor ld;
	ld.profilePoints = prof; ld.numProfilePoints = 5;
	ld.nRadial = 32;
	ld.smooth = true;

	ITriangleMeshGeometryIndexed* pLatheI = 0;
	assert( RISE_API_CreateLatheGeometry( &pLatheI, ld ) );
	TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pLatheI );
	assert( pLathe );

	// Snapshot the BASE mesh's own vertex positions (pre-displacement) via
	// TessellateToMesh -- the exact call DisplacedGeometry::BuildMesh makes.
	IndexTriangleListType baseTris; VerticesListType baseVerts; NormalsListType baseNorms; TexCoordsListType baseCoords;
	assert( pLathe->TessellateToMesh( baseTris, baseVerts, baseNorms, baseCoords, 0 ) );
	assert( !baseVerts.empty() );

	const Scalar k = 0.15;	// constant displacement amplitude
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pLathe, 32, pConst, k,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	assert( pDisp->IsValid() );
	pDisp->Realize();

	IndexTriangleListType dTris; VerticesListType dVerts; NormalsListType dNorms; TexCoordsListType dCoords;
	assert( pDisp->TessellateToMesh( dTris, dVerts, dNorms, dCoords, 0 ) );
	assert( dVerts.size() == baseVerts.size() );

	Scalar maxDev = 0.0, minDev = 1e30;
	for( size_t i = 0; i < dVerts.size(); ++i ) {
		const Scalar dx = dVerts[i].x - baseVerts[i].x;
		const Scalar dy = dVerts[i].y - baseVerts[i].y;
		const Scalar dz = dVerts[i].z - baseVerts[i].z;
		const Scalar dev = std::sqrt( dx*dx + dy*dy + dz*dz );
		maxDev = std::max( maxDev, dev );
		minDev = std::min( minDev, dev );
	}
	std::cout << "  [info] lathe displacement deviation: min=" << minDev << " max=" << maxDev << " (amplitude k=" << k << ")\n";
	// Displacement actually applied (not a no-op)...
	assert( maxDev > 0.0 );
	// ...and bounded by the painter's own amplitude (every vertex moves by
	// disp_scale along a UNIT normal; 5% slack for FP/normal-magnitude noise).
	assert( maxDev <= k * 1.05 );
	assert( minDev >= k * 0.95 );

	pDisp->release();
	pConst->release();
	pLathe->release();
}

//-----------------------------------------------------------------------------
// Doc 89 slice D audit: displaced_geometry over an INTERIOR-PINCH lathe TEARS
// at the waist.  This is documented -- and deliberately NOT "fixed" -- at
// ProceduralDescriptors.h's LatheDescriptor comment: the pinch's two poles
// are coincident POSITIONS with EXACTLY OPPOSITE normals (one vertex per
// adjacent band; see TestLatheInteriorPolePinch in ProceduralMeshTest.cpp),
// so displacing each pole along its OWN normal pushes the pair apart by
// 2*disp_scale and opens a visible crack.  Welding the two poles to close
// the crack would instead put one of the two bands' SHADING normal in the
// wrong half-space -- the defect the split exists to prevent -- so the
// documented guidance is "displace an unpinched profile, or keep disp_scale
// small ... rather than fixing it".
//
// This is the STRONGEST FAIL cell the doc 89 slice D audit found (matrix
// criterion 3, "renders sanely / no holes").  Pinned here quantitatively so
// the limitation stays load-bearing: a future change that welds the pinch
// (intentionally or as a side effect of some other normal-recompute fix)
// will fail this assertion and must consciously revisit both this test and
// the LatheDescriptor comment, instead of the crack being silently
// reintroduced or silently "fixed" without anyone noticing the tradeoff.
//-----------------------------------------------------------------------------
static void TestDisplacedOverPinchedLatheTears()
{
	std::cout << "Test 10: displaced_geometry over an interior-pinch lathe TEARS at the waist (documented limitation)...\n";

	const double R = 1.0;
	const double prof[] = { R, 0.0,   0.0, 1.0,   R, 2.0 };	// hourglass, pinched at h=1 (TestLatheInteriorPolePinch's profile)
	LatheDescriptor ld;
	ld.profilePoints = prof; ld.numProfilePoints = 3;
	ld.nRadial = 24;

	ITriangleMeshGeometryIndexed* pLatheI = 0;
	assert( RISE_API_CreateLatheGeometry( &pLatheI, ld ) );
	TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pLatheI );
	assert( pLathe );

	const Scalar k = 0.2;
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pLathe, 24, pConst, k,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	assert( pDisp->IsValid() );
	pDisp->Realize();	// must NOT crash on the pinch -- the render-sanity half of the audit

	IndexTriangleListType dTris; VerticesListType dVerts; NormalsListType dNorms; TexCoordsListType dCoords;
	assert( pDisp->TessellateToMesh( dTris, dVerts, dNorms, dCoords, 0 ) );
	assert( !dVerts.empty() );

	// Find the ex-pinch vertices: pre-displacement they sat exactly on the
	// axis (x=z=0, y=1); a pure +/-Y pole normal keeps them on the axis after
	// displacement too, landing near y = 1 +/- k.
	std::vector<Scalar> nearAxisY;
	for( size_t i = 0; i < dVerts.size(); ++i ) {
		const Scalar rxz = std::sqrt( dVerts[i].x * dVerts[i].x + dVerts[i].z * dVerts[i].z );
		if( rxz < Scalar(1e-6) && std::fabs( dVerts[i].y - Scalar(1.0) ) < Scalar(0.5) ) {
			nearAxisY.push_back( dVerts[i].y );
		}
	}
	std::cout << "  [info] " << nearAxisY.size() << " near-axis vertices near the waist after displacement\n";
	assert( nearAxisY.size() >= 2 && "pinch: both ex-coincident pole vertices must survive the bake" );

	Scalar loY = 1e30, hiY = -1e30;
	for( size_t i = 0; i < nearAxisY.size(); ++i ) {
		loY = std::min( loY, nearAxisY[i] );
		hiY = std::max( hiY, nearAxisY[i] );
	}
	const Scalar gap = hiY - loY;
	std::cout << "  [info] pinch gap after displacement: " << gap << " (expect ~2*k = " << 2.0*k << ")\n";

	// MONEY ASSERTION -- the documented crack, pinned quantitatively.
	assert( gap > Scalar(1.5) * k && gap < Scalar(2.5) * k );

	pDisp->release();
	pConst->release();
	pLathe->release();
}

//-----------------------------------------------------------------------------
// Concurrency guard (review Finding 1): many threads calling Realize() at once
// must BuildMesh() the instance EXACTLY once.  Models a GUI viewport render's
// AttachScene racing a UI-thread PrepareForRendering/picking on the same, not-
// yet-realized geometry.  Without the realize mutex, several threads pass the
// !m_bRealized check and double-build (leak / refcount corruption).
//-----------------------------------------------------------------------------
static void TestConcurrentRealize()
{
	std::cout << "Test: concurrent Realize() bakes exactly once (thread-safety)...\n";

	const Scalar R = 1.0;
	SphereGeometry*    pSphere = new SphereGeometry( R );
	ConstFunction2D*   pConst  = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp   = new DisplacedGeometry(
		pSphere, 24, pConst, 0.1, /*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	// NOT realized yet -- the threads below race the first Realize().

	DisplacedGeometry::ResetBuildMeshCount();

	const int N = 32;
	std::atomic<int>  ready( 0 );
	std::atomic<bool> go( false );
	std::vector<std::thread> threads;
	for( int i = 0; i < N; ++i ) {
		threads.emplace_back( [&]{
			ready.fetch_add( 1 );
			while( !go.load() ) { }      // spin-barrier: maximise overlap
			pDisp->Realize();
		} );
	}
	while( ready.load() < N ) { }    // wait until all threads are armed
	go.store( true );
	for( std::thread& t : threads ) { t.join(); }

	const unsigned int builds = DisplacedGeometry::GetBuildMeshCount();
	assert( builds == 1 && "concurrent Realize() must BuildMesh exactly once" );
	std::cout << "  [ok] " << N << " concurrent Realize() -> " << builds << " build (want 1)\n";

	pDisp->release();
	pConst->release();
	pSphere->release();
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
	TestDisplacedOverLatheAppliesDisplacement();
	TestDisplacedOverPinchedLatheTears();
	TestConcurrentRealize();

	std::cout << "All DisplacedGeometry tests passed.\n";
	return 0;
}
