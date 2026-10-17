#include <iostream>
#include <cassert>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Observable.h"
// Doc 89 slice D (displaced-detail composition audit): builds a lathe_geometry
// base mesh via the same RISE_API factory the scene parser uses, so the
// composition tests below exercise the REAL builder output, not a hand-rolled
// stand-in.
#include "../src/Library/RISE_API.h"
#include <algorithm>
// The `height` scalar-field route (2026-09-06, docs/RELIEF_MODIFIER_DESIGN.md
// section 5.3): the tests below drive it through the REAL scene chunk so the
// descriptor, the CST resolver's scalar-painter closure and
// Job::AddDisplacedGeometryWithHeight are all under test, not just the
// geometry class.
#include "../src/Library/Geometry/GeometryUtilities.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#ifdef _WIN32
	#include <io.h>
	#include <process.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

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
	// `detail` (32) now REFINES a mesh base whose edges exceed bbox-diagonal /
	// detail, so the baked mesh is finer than the 4-ring-row base.  The
	// original vertices keep indices [0, baseVerts.size()); midpoints are
	// appended after them.  Exact count expectation WITHOUT replicating the
	// algorithm: the vase is a closed genus-0 surface (a pole at each end), so
	// Euler gives V - E + F = 2 with E = 3F/2, i.e. V = F/2 + 2 -- true of the
	// base (98 = 192/2 + 2) and true of a crack-free refinement, but NOT of one
	// that duplicates a midpoint per incident triangle or leaves T-junctions.
	assert( baseVerts.size() == baseTris.size() / 2 + 2 );
	assert( dTris.size() > baseTris.size() );
	assert( dVerts.size() > baseVerts.size() );
	assert( dVerts.size() == dTris.size() / 2 + 2 );

	// Every ORIGINAL base vertex must reappear (displaced indices are NOT
	// preserved: TessellateToMesh re-emits in first-seen order over the refined
	// triangles) displaced by exactly k along its own unit normal.
	Scalar worstErr = 0.0;
	for( size_t i = 0; i < baseVerts.size(); ++i ) {
		const Vector3 n = Vector3Ops::Normalize( baseNorms[i] );
		const Point3 expected( baseVerts[i].x + n.x * k, baseVerts[i].y + n.y * k, baseVerts[i].z + n.z * k );
		Scalar best = 1e30;
		for( size_t j = 0; j < dVerts.size(); ++j ) {
			const Scalar dx = dVerts[j].x - expected.x, dy = dVerts[j].y - expected.y, dz = dVerts[j].z - expected.z;
			best = std::min( best, std::sqrt( dx*dx + dy*dy + dz*dz ) );
		}
		worstErr = std::max( worstErr, best );
	}
	std::cout << "  [info] lathe: " << baseVerts.size() << " base vertices -> " << dVerts.size()
		<< " baked (" << baseTris.size() << " -> " << dTris.size() << " tris); worst |displaced - (base + k n)| = " << worstErr << " (amplitude k=" << k << ")\n";
	assert( worstErr < 1e-9 );

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
// Builder-base SMOOTHNESS (the fix for the "displaced lathe/sweep/skin renders
// FACETED" defect found by the doc 89 slice D audit).
//
// ROOT CAUSE it pins: TriangleMeshGeometryIndexed::TessellateToMesh used to
// flatten every triangle corner into its own (pos, normal, uv) tuple -- 3N
// vertices for N triangles -- so DisplacedGeometry's post-displacement
// RecomputeVertexNormalsFromTopology (which averages face normals per vertex
// INDEX) saw every index belong to exactly one triangle and returned that
// triangle's flat face normal.  It now assigns ONE output index per distinct
// ORIGINAL (position, normal, uv) index triple, reproducing the builder's
// shared topology exactly (hard edges / caps / pinch poles stay split because
// their normal indices differ).
//
// Three witnesses, each of which FAILS on the flattening and PASSES now:
//  (a) the baked mesh shares vertices (V ~ F/2, it was 3F) and the `detail`
//      refinement of the mesh base is crack-free (Euler characteristic equals
//      the base's);
//  (b) no two baked vertices share a POSITION (they all did: ~6 per interior
//      vertex) -- for builders with no intentional duplicate (a seamless
//      lathe, a capless sweep, a skin sheet);
//  (c) the post-displacement SHADING normal is not the face normal: the mean
//      angle between a corner's stored normal and its triangle's geometric
//      normal is well above zero (it was exactly zero -- flat shading).
//-----------------------------------------------------------------------------
// V - E + F over the REFERENCED vertices and unordered index-pair edges.
static long long EulerCharacteristic( const IndexTriangleListType& tris )
{
	std::vector<unsigned int> verts;
	std::vector< std::pair<unsigned int,unsigned int> > edges;
	for( size_t t = 0; t < tris.size(); ++t ) {
		for( int k = 0; k < 3; ++k ) {
			const unsigned int a = tris[t].iVertices[k], b = tris[t].iVertices[(k+1)%3];
			verts.push_back( a );
			edges.push_back( a < b ? std::make_pair( a, b ) : std::make_pair( b, a ) );
		}
	}
	std::sort( verts.begin(), verts.end() ); verts.erase( std::unique( verts.begin(), verts.end() ), verts.end() );
	std::sort( edges.begin(), edges.end() ); edges.erase( std::unique( edges.begin(), edges.end() ), edges.end() );
	return (long long)verts.size() - (long long)edges.size() + (long long)tris.size();
}

static void CheckDisplacedBuilderIsSmooth(
	const char*                  name,
	TriangleMeshGeometryIndexed* pBase,
	const Scalar                 k,
	const unsigned int           detail )
{
	IndexTriangleListType bTris; VerticesListType bVerts; NormalsListType bNorms; TexCoordsListType bCoords;
	assert( pBase->TessellateToMesh( bTris, bVerts, bNorms, bCoords, 0 ) );
	assert( !bTris.empty() );

	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pBase, detail, pConst, k,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	assert( pDisp->IsValid() );
	pDisp->Realize();

	IndexTriangleListType dTris; VerticesListType dVerts; NormalsListType dNorms; TexCoordsListType dCoords;
	assert( pDisp->TessellateToMesh( dTris, dVerts, dNorms, dCoords, 0 ) );
	assert( dVerts.size() == dNorms.size() && dVerts.size() == dCoords.size() );

	// (a) shared topology.  The old flattening gave 3 vertices per triangle; a
	// shared-topology bake of ANY manifold-ish mesh has V ~ F/2.  And, because
	// `detail` now refines a mesh base, the CRACK-FREE property of that
	// refinement: edge splitting never changes the Euler characteristic
	// V - E + F, whereas a midpoint duplicated per incident triangle (or a
	// T-junction) would.
	std::cout << "  [info] " << name << ": base " << bTris.size() << " tris / " << pBase->numPoints()
		<< " vertices -> baked " << dTris.size() << " tris / " << dVerts.size()
		<< " vertices (flattened would be " << 3*dTris.size() << ")\n";
	assert( dVerts.size() * 2 < dTris.size() * 3 );
	assert( EulerCharacteristic( dTris ) == EulerCharacteristic( bTris ) );

	// (b) no coincident positions (quantised to 1e-7).
	{
		typedef std::pair< long long, std::pair< long long, long long > > Key;
		std::vector< Key > keys;
		keys.reserve( dVerts.size() );
		const Scalar q = 1e7;
		for( size_t i = 0; i < dVerts.size(); ++i ) {
			keys.push_back( Key( (long long)std::floor( dVerts[i].x * q + 0.5 ),
				std::make_pair( (long long)std::floor( dVerts[i].y * q + 0.5 ),
				                (long long)std::floor( dVerts[i].z * q + 0.5 ) ) ) );
		}
		std::sort( keys.begin(), keys.end() );
		size_t coincident = 0;
		for( size_t i = 1; i < keys.size(); ++i ) {
			if( keys[i] == keys[i-1] ) ++coincident;
		}
		std::cout << "  [info] " << name << ": " << coincident << " vertices coincide in position with another\n";
		assert( coincident == 0 );
	}

	// (c) smooth shading: stored corner normal vs the triangle's own face normal.
	{
		Scalar sumAngle = 0.0; size_t n = 0;
		for( size_t t = 0; t < dTris.size(); ++t ) {
			const IndexedTriangle& tri = dTris[t];
			const Vector3 e1 = Vector3Ops::mkVector3( dVerts[tri.iVertices[1]], dVerts[tri.iVertices[0]] );
			const Vector3 e2 = Vector3Ops::mkVector3( dVerts[tri.iVertices[2]], dVerts[tri.iVertices[0]] );
			const Vector3 c = Vector3Ops::Cross( e1, e2 );
			const Scalar m = Vector3Ops::Magnitude( c );
			if( m < 1e-12 ) continue;	// degenerate (pole fan) -- no face normal to compare
			const Vector3 fn = c * (1.0 / m);
			for( int j = 0; j < 3; ++j ) {
				Scalar d = Vector3Ops::Dot( fn, dNorms[tri.iNormals[j]] );
				d = std::max( Scalar(-1.0), std::min( Scalar(1.0), d ) );
				sumAngle += std::acos( d );
				++n;
			}
		}
		const Scalar meanDeg = ( n ? sumAngle / Scalar(n) : 0.0 ) * 180.0 / PI;
		std::cout << "  [info] " << name << ": mean corner-normal vs face-normal angle = " << meanDeg << " deg (0 = flat shaded)\n";
		assert( meanDeg > 0.5 );
	}

	pDisp->release();
	pConst->release();
}

static void TestDisplacedBuildersShadeSmooth()
{
	std::cout << "Test 10b: displaced_geometry over lathe / sweep / skin builder meshes keeps SMOOTH shading (no per-corner flattening)...\n";

	// LATHE -- Test 9's unpinched vase (seamless full turn, one pole vertex per end).
	{
		const double prof[] = { 0.0, 0.0,  0.7, 0.2,  0.9, 1.0,  0.5, 1.8,  0.0, 2.0 };
		LatheDescriptor ld;
		ld.profilePoints = prof; ld.numProfilePoints = 5;
		ld.nRadial = 32;
		ld.smooth = true;
		ITriangleMeshGeometryIndexed* pI = 0;
		assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		assert( p );
		CheckDisplacedBuilderIsSmooth( "lathe", p, 0.15, 32 );
		p->release();
	}

	// SWEEP -- a 16-gon section swept along a bent 3-point path, NO end caps
	// (caps are deliberately duplicated hard-edge vertices, so they would
	// legitimately coincide in position with the tube's rim).
	{
		double prof[ 32 ];
		for( int i = 0; i < 16; ++i ) {
			const double a = 2.0 * 3.14159265358979323846 * double( i ) / 16.0;
			prof[ 2*i ] = 0.4 * std::cos( a );
			prof[ 2*i + 1 ] = 0.4 * std::sin( a );
		}
		const double path[] = { 0.0, 0.0, 0.0,   0.0, 1.0, 0.3,   0.4, 2.0, 0.3 };
		SweepDescriptor sd;
		sd.profilePoints = prof; sd.numProfilePoints = 16;
		sd.pathPoints = path;    sd.numPathPoints = 3;
		sd.nLen = 24;
		sd.capStart = false; sd.capEnd = false;
		ITriangleMeshGeometryIndexed* pI = 0;
		assert( RISE_API_CreateSweepGeometry( &pI, sd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		assert( p );
		CheckDisplacedBuilderIsSmooth( "sweep", p, 0.05, 24 );
		p->release();
	}

	// SKIN -- a billowed sheet between two rails.
	{
		const double railA[] = { -1.6, 0.0, 0.0,   -0.5, 0.45, 0.0,   0.6, 1.1, 0.0,   1.6, 2.0, 0.0 };
		const double railB[] = { -1.6, 0.4, -1.9,   0.0, 1.5, -1.9,   1.6, 2.6, -1.9 };
		SkinDescriptor kd;
		kd.railAPoints = railA; kd.numRailAPoints = 4;
		kd.railBPoints = railB; kd.numRailBPoints = 3;
		kd.nLen = 20; kd.nAcross = 10; kd.billow = 0.28;
		ITriangleMeshGeometryIndexed* pI = 0;
		assert( RISE_API_CreateSkinGeometry( &pI, kd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		assert( p );
		CheckDisplacedBuilderIsSmooth( "skin", p, 0.04, 24 );
		p->release();
	}
}

static Scalar MaxEdgeLength( const IndexTriangleListType& tris, const VerticesListType& verts )
{
	Scalar m = 0.0;
	for( size_t t = 0; t < tris.size(); ++t ) {
		for( int k = 0; k < 3; ++k ) {
			const Point3& a = verts[ tris[t].iVertices[k] ];
			const Point3& b = verts[ tris[t].iVertices[(k+1)%3] ];
			m = std::max( m, std::sqrt( (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z) ) );
		}
	}
	return m;
}

static Scalar BBoxDiagonal( const VerticesListType& verts )
{
	Point3 lo( 1e30, 1e30, 1e30 ), hi( -1e30, -1e30, -1e30 );
	for( size_t i = 0; i < verts.size(); ++i ) {
		lo.x = std::min( lo.x, verts[i].x ); lo.y = std::min( lo.y, verts[i].y ); lo.z = std::min( lo.z, verts[i].z );
		hi.x = std::max( hi.x, verts[i].x ); hi.y = std::max( hi.y, verts[i].y ); hi.z = std::max( hi.z, verts[i].z );
	}
	return std::sqrt( (hi.x-lo.x)*(hi.x-lo.x) + (hi.y-lo.y)*(hi.y-lo.y) + (hi.z-lo.z)*(hi.z-lo.z) );
}

static TriangleMeshGeometryIndexed* MakeCoarseLathe( const int nRadial )
{
	// FOUR profile points -> three bands: a coarse tank-shaped lathe whose
	// own vertical resolution is the profile point count and nothing else.
	static const double prof[] = { 0.0, 0.0,  1.0, 0.25,  1.0, 1.75,  0.0, 2.0 };
	LatheDescriptor ld;
	ld.profilePoints = prof; ld.numProfilePoints = 4;
	ld.nRadial = nRadial;
	ld.smooth = true;
	ITriangleMeshGeometryIndexed* pI = 0;
	assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
	TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
	assert( p );
	return p;
}

//-----------------------------------------------------------------------------
// `detail` REFINES a mesh base.  A lathe/sweep/skin/imported mesh ignores
// `detail` in its own TessellateToMesh, so it used to be displaced only at its
// OWN vertices: a 4-point-profile lathe has 4 vertex rows however fine the
// relief, and `detail 48` did nothing.  DisplacedGeometry::BuildMesh now splits
// every edge longer than (base bbox diagonal / detail) before displacing.
//
// RED on the unrefined bake (vertical spacing = the profile rows, ~1.5 vs a
// 0.072 target); GREEN now.  Also pins: crack-free (Euler characteristic of
// the base kept), the pinch/crease vertices stay split (no position weld --
// see Test 10), and analytic/SDF-style bases that already honour `detail`
// are NOT touched.
//-----------------------------------------------------------------------------
static void TestDetailRefinesMeshBases()
{
	std::cout << "Test 10c: `detail` refines a coarse mesh base (lathe vertical resolution) and leaves detail-honouring bases alone...\n";

	// ---- (1) the coarse lathe, detail 48 --------------------------------
	{
		TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
		IndexTriangleListType bTris; VerticesListType bVerts; NormalsListType bNorms; TexCoordsListType bCoords;
		assert( pLathe->TessellateToMesh( bTris, bVerts, bNorms, bCoords, 0 ) );
		const unsigned int detail = 48;
		const Scalar target = BBoxDiagonal( bVerts ) / Scalar( detail );
		const Scalar baseMaxEdge = MaxEdgeLength( bTris, bVerts );
		std::cout << "  [info] base: " << bTris.size() << " tris, longest edge " << baseMaxEdge << ", target (diag/detail) " << target << "\n";
		assert( baseMaxEdge > 5.0 * target );		// the premise: the base really is far coarser than `detail`

		const Scalar k = 0.002;						// tiny: edge lengths are measured in the displaced frame
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, detail, pConst, k, false, false );
		pDisp->Realize();

		IndexTriangleListType dTris; VerticesListType dVerts; NormalsListType dNorms; TexCoordsListType dCoords;
		assert( pDisp->TessellateToMesh( dTris, dVerts, dNorms, dCoords, 0 ) );
		const Scalar maxEdge = MaxEdgeLength( dTris, dVerts );
		std::vector<Scalar> ys;
		for( size_t i = 0; i < dVerts.size(); ++i ) ys.push_back( dVerts[i].y );
		std::sort( ys.begin(), ys.end() );
		Scalar maxGap = 0.0;
		for( size_t i = 1; i < ys.size(); ++i ) maxGap = std::max( maxGap, ys[i] - ys[i-1] );
		std::cout << "  [info] displaced: " << dTris.size() << " tris, longest edge " << maxEdge << ", largest vertical gap between vertices " << maxGap << "\n";

		// MONEY ASSERTIONS (red before the refinement: maxEdge ~ 1.5, maxGap ~ 1.5).
		assert( maxEdge <= 1.3 * target );
		assert( maxGap  <= 1.3 * target );
		// crack-free: refinement preserves the Euler characteristic of the base.
		assert( EulerCharacteristic( dTris ) == EulerCharacteristic( bTris ) );
		assert( dVerts.size() == dTris.size() / 2 + 2 );	// closed genus-0 (pole at each end)
		// the refined, displaced mesh is still a valid, queryable mesh: a ray
		// from outside along +X at mid-height hits the displaced wall near r = 1 + k.
		RayIntersectionGeometric ri = MakeRI( Point3( 5.0, 1.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ) );
		pDisp->IntersectRay( ri, true, true, false );
		std::cout << "  [info] ray hit=" << ri.bHit << " at x=" << 5.0 - ri.range << " (wall r = " << 1.0 + k << ")\n";
		assert( ri.bHit );
		assert( std::fabs( ( 5.0 - ri.range ) - ( 1.0 + k ) ) < 0.02 );

		pDisp->release();
		pConst->release();
		pLathe->release();
	}

	// ---- (2) bases that already honour `detail` are untouched -----------
	{
		SphereGeometry* pSphere = new SphereGeometry( 1.0 );
		const unsigned int detail = 16;
		IndexTriangleListType sT; VerticesListType sV; NormalsListType sN; TexCoordsListType sC;
		assert( pSphere->TessellateToMesh( sT, sV, sN, sC, detail ) );

		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pSphere, detail, pConst, 0.05, false, false );
		pDisp->Realize();
		IndexTriangleListType dT; VerticesListType dV; NormalsListType dN; TexCoordsListType dC;
		assert( pDisp->TessellateToMesh( dT, dV, dN, dC, 0 ) );
		std::cout << "  [info] sphere detail " << detail << ": base tessellation " << sT.size() << " tris / " << sV.size()
			<< " verts, displaced " << dT.size() << " tris / " << dV.size() << " verts\n";
		assert( dT.size() == sT.size() );
		assert( dV.size() == sV.size() );

		pDisp->release();
		pConst->release();
		pSphere->release();
	}
	{
		BoxGeometry* pBox = new BoxGeometry( 2.0, 2.0, 2.0 );
		const unsigned int detail = 12;
		IndexTriangleListType bT; VerticesListType bV; NormalsListType bN; TexCoordsListType bC;
		assert( pBox->TessellateToMesh( bT, bV, bN, bC, detail ) );
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pBox, detail, pConst, 0.05, false, false );
		pDisp->Realize();
		IndexTriangleListType dT; VerticesListType dV; NormalsListType dN; TexCoordsListType dC;
		assert( pDisp->TessellateToMesh( dT, dV, dN, dC, 0 ) );
		std::cout << "  [info] box detail " << detail << ": base tessellation " << bT.size() << " tris, displaced " << dT.size() << " tris\n";
		assert( dT.size() == bT.size() );
		assert( dV.size() == bV.size() );
		pDisp->release();
		pConst->release();
		pBox->release();
	}

	// ---- (3) disp_scale 0 ("force tessellation" idiom) is not refined ---
	{
		TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
		IndexTriangleListType bTris; VerticesListType bVerts; NormalsListType bNorms; TexCoordsListType bCoords;
		assert( pLathe->TessellateToMesh( bTris, bVerts, bNorms, bCoords, 0 ) );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 48, 0, 0.0, false, false );
		pDisp->Realize();
		IndexTriangleListType dT; VerticesListType dV; NormalsListType dN; TexCoordsListType dC;
		assert( pDisp->TessellateToMesh( dT, dV, dN, dC, 0 ) );
		assert( dT.size() == bTris.size() && dV.size() == bVerts.size() );
		pDisp->release();
		pLathe->release();
	}

	// ---- (4) the hard cap: absurd detail stops at the level/triangle cap
	{
		TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 100000, pConst, 0.002, false, false );
		pDisp->Realize();
		IndexTriangleListType dT; VerticesListType dV; NormalsListType dN; TexCoordsListType dC;
		assert( pDisp->TessellateToMesh( dT, dV, dN, dC, 0 ) );
		std::cout << "  [info] detail 100000 capped at " << dT.size() << " triangles\n";
		assert( dT.size() <= 1500000 );
		assert( dT.size() > 1000 );
		pDisp->release();
		pConst->release();
		pLathe->release();
	}
}

// An IFunction2D that is also Observable (like every in-tree painter), so a
// value change drives DisplacedGeometry's animation REFIT path.
class ObservableConstFunction2D : public virtual IFunction2D, public virtual Reference, public Observable
{
public:
	Scalar value;
	ObservableConstFunction2D( const Scalar v ) : value( v ) {}
	Scalar Evaluate( const Scalar /*x*/, const Scalar /*y*/ ) const { return value; }
	void SetValue( const Scalar v ) { value = v; NotifyObservers(); }
};

//-----------------------------------------------------------------------------
// The refined topology is DETERMINISTIC and STABLE across refreshes: a painter
// notification re-tessellates + re-refines and refits the existing mesh in
// place (no rebuild: BuildMesh count unchanged), the triangle/vertex counts
// stay put, and the vertices move by the new amplitude.
//-----------------------------------------------------------------------------
static void TestRefinedTopologyStableAcrossRefresh()
{
	std::cout << "Test 10d: a painter-driven refresh refits the refined mesh in place (stable topology)...\n";

	TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
	ObservableConstFunction2D* pFn = new ObservableConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 32, pFn, 0.05, false, false );
	pDisp->Realize();

	IndexTriangleListType t0; VerticesListType v0; NormalsListType n0; TexCoordsListType c0;
	assert( pDisp->TessellateToMesh( t0, v0, n0, c0, 0 ) );

	DisplacedGeometry::ResetBuildMeshCount();
	pFn->SetValue( 3.0 );		// 0.05 -> 0.15 amplitude, through the observer path
	assert( DisplacedGeometry::GetBuildMeshCount() == 0 && "refresh must REFIT, not rebuild" );

	IndexTriangleListType t1; VerticesListType v1; NormalsListType n1; TexCoordsListType c1;
	assert( pDisp->TessellateToMesh( t1, v1, n1, c1, 0 ) );
	std::cout << "  [info] " << t0.size() << " tris / " << v0.size() << " verts before, " << t1.size() << " / " << v1.size() << " after the refresh\n";
	assert( t1.size() == t0.size() && v1.size() == v0.size() );

	// A wall vertex (r = 1 band, |y - 1| small) moved outward by the extra 0.10.
	Scalar maxR0 = 0.0, maxR1 = 0.0;
	for( size_t i = 0; i < v0.size(); ++i ) maxR0 = std::max( maxR0, std::sqrt( v0[i].x*v0[i].x + v0[i].z*v0[i].z ) );
	for( size_t i = 0; i < v1.size(); ++i ) maxR1 = std::max( maxR1, std::sqrt( v1[i].x*v1[i].x + v1[i].z*v1[i].z ) );
	std::cout << "  [info] max radius " << maxR0 << " -> " << maxR1 << "\n";
	assert( maxR1 > maxR0 + 0.05 );

	pDisp->release();
	pFn->release();
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

//=============================================================================
//  The `height` SCALAR-FIELD route (2026-09-06).
//
//  `displaced_geometry` now takes EITHER the legacy `displacement`
//  (IFunction2D, sampled f(u,v)) or `height` (IScalarPainter, evaluated as a
//  3D FIELD at each vertex's OBJECT-space position).  These cases pin, in
//  order: the field's exactness and its object-space convention; that the
//  IFunction2D route is byte-for-byte what it was; the mutual-exclusion parse
//  error; the scalar-pipe diagnostic; and UV parity between the two routes
//  (which is what proves the synthesized `ptCoord` -- seam fold included -- is
//  right).
//=============================================================================

namespace {

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_dispheight_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << "RISE ASCII SCENE 7\n" << body;
	f.close();
	return path;
}

//! Load a scene body with stdout captured -- GlobalLog's eLog_Console sink
//! includes eLog_Error, so Job::AddDisplacedGeometryWithHeight's diagnostics
//! land there.  Same fd-dup technique as ReliefModifierTest::ParseCapturing.
bool ParseCapturing( const std::string& tag, const std::string& body,
                     IJobPriv& job, std::string& captured )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_dispheight_stdout_" + tag + "_" + pidbuf + ".txt";

	const std::string path = WriteTempScene( tag, body );

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	captured.clear();
	{
		std::ifstream in( capPath.c_str(), std::ios::binary );
		std::string line;
		while( std::getline( in, line ) ) { captured += line; captured += "\n"; }
	}
	std::remove( capPath.c_str() );
	std::remove( path.c_str() );
	return ok;
}

bool Contains( const std::string& hay, const char* needle )
{
	return hay.find( needle ) != std::string::npos;
}

//! Re-emit a realized geometry's baked mesh as a flat vertex list.  Both
//! routes go through the same TriangleMeshGeometryIndexed re-emit, so two
//! lists produced this way are directly comparable element-for-element.
bool EmitVertices( const IGeometry& g, VerticesListType& out )
{
	IndexTriangleListType tris;
	NormalsListType       normals;
	TexCoordsListType     coords;
	out.clear();
	return g.TessellateToMesh( tris, out, normals, coords, 0 );
}

const unsigned int kDetail = 24;

} // namespace

//-----------------------------------------------------------------------------
// Case 13: a `height` field is evaluated at the vertex's OBJECT-space
// position, and the vertex moves along its normal by EXACTLY height*disp_scale.
//
// `expression Po.x` is chosen because it is the sharpest possible probe of the
// two things that could be wrong: if the field went through Painter::Evaluate's
// fake hit (the pre-2026-09-06 behaviour for a 3D painter in this slot) every
// vertex would get the SAME value, and if the synthesized hit carried anything
// but the vertex position, the per-vertex value would not be the vertex's own
// x.  Exactness is 1e-9: this is one multiply and one add, not an integration.
//
// RED-PROOF (2026-09-06): replacing the GetValuesAt(ri) call in
// GeometryUtilities::ApplyScalarHeightToObject with a constant
// (`height.GetValuesAt(ri).v[0]` -> `0.5`, i.e. the constant path this route
// exists to escape) fails this case at the first non-pole vertex.
//-----------------------------------------------------------------------------
static void TestScalarHeightIsObjectSpaceAndExact()
{
	std::cout << "Test 13: `height` evaluates the field at the vertex's object-space position, exactly...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	const bool ok = ParseCapturing( "exact",
		"scalar_painter\n{\n\tname h_pox\n\texpression Po.x\n}\n",
		*job, log );
	assert( ok );

	IScalarPainter* pHeight = job->GetScalarPainters()->GetItem( "h_pox" );
	assert( pHeight != 0 );

	// Tessellate a plain sphere, snapshot the pre-displacement state, then run
	// the SAME applier DisplacedGeometry::ApplyHeightField runs.
	SphereGeometry* pSphere = new SphereGeometry( 1.0 );
	IndexTriangleListType tris;
	VerticesListType      verts;
	NormalsListType       normals;
	TexCoordsListType     coords;
	assert( pSphere->TessellateToMesh( tris, verts, normals, coords, kDetail ) );

	const VerticesListType before  = verts;
	const NormalsListType  nBefore = normals;

	// disp_scale is deliberately NOT 1: a route that applied the scale twice
	// (the plausible bug when a new parameter is threaded through two layers)
	// is invisible at 1.0 but gives 0.5625 instead of 0.75 here.
	const Scalar scale = 0.75;
	ApplyScalarHeightToObject( tris, verts, normals, coords, *pHeight, scale );

	// Every vertex: moved along its OWN pre-displacement normal by exactly
	// its OWN object-space x, times disp_scale.  (Vertices not referenced by
	// any triangle are untouched by both the applier and this expectation.)
	std::vector<bool> touched( before.size(), false );
	for( size_t t = 0; t < tris.size(); ++t ) {
		for( int j = 0; j < 3; ++j ) touched[ tris[t].iVertices[j] ] = true;
	}

	Scalar maxErr = 0.0, minDisp = 1e30, maxDisp = -1e30;
	for( size_t i = 0; i < before.size(); ++i ) {
		if( !touched[i] ) continue;
		const Scalar expectedDisp = before[i].x * scale;
		const Point3 expected = Point3Ops::mkPoint3( before[i], nBefore[i] * expectedDisp );
		maxErr = std::max( maxErr, std::fabs( verts[i].x - expected.x ) );
		maxErr = std::max( maxErr, std::fabs( verts[i].y - expected.y ) );
		maxErr = std::max( maxErr, std::fabs( verts[i].z - expected.z ) );
		minDisp = std::min( minDisp, expectedDisp );
		maxDisp = std::max( maxDisp, expectedDisp );
	}
	std::cout << "    max per-vertex error = " << maxErr
	          << ", displacement range = [" << minDisp << ", " << maxDisp << "]\n";
	assert( maxErr <= 1e-9 );

	// The constant-path killer: on a unit sphere `Po.x` genuinely spans
	// [-1, 1], so at scale 0.75 the displacement spans ~1.5.  A fake-hit /
	// constant evaluation would collapse this range to zero.
	assert( maxDisp - minDisp > 1.4 );

	pSphere->release();
	safe_release( job );
}

//-----------------------------------------------------------------------------
// Case 14: the `height` route drives the REAL chunk end to end -- descriptor,
// CST scalar-painter closure, Job resolution, deferred realize, ray query.
//
// With `height = Po.x` and disp_scale 1 on a unit sphere, the +X side inflates
// to |x| ~ 2 and the -X side collapses toward the origin.  A ray down -X from
// (10,0,0) therefore hits at ~8, a long way from the undisplaced 9.
//-----------------------------------------------------------------------------
static void TestScalarHeightThroughTheChunk()
{
	std::cout << "Test 14: `displaced_geometry { height ... }` parses, realizes and displaces...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	const bool ok = ParseCapturing( "chunk",
		"scalar_painter\n{\n\tname h_pox\n\texpression Po.x\n}\n"
		"sphere_geometry\n{\n\tname base\n\tradius 1.0\n}\n"
		"displaced_geometry\n{\n\tname bumpy\n\tbase_geometry base\n\tdetail 48\n"
		"\theight h_pox\n\tdisp_scale 1.0\n}\n",
		*job, log );
	assert( ok );

	IGeometry* pDisp = job->GetGeometries()->GetItem( "bumpy" );
	assert( pDisp != 0 );
	pDisp->Realize();

	RayIntersectionGeometric ri = MakeRI( Point3( 10.0, 0.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ) );
	pDisp->IntersectRay( ri, true, false, false );
	assert( ri.bHit );
	std::cout << "    +X hit range = " << ri.range << " (undisplaced would be 9.0)\n";
	assert( IsClose( ri.range, 8.0, 0.05 ) );

	safe_release( job );
}

//-----------------------------------------------------------------------------
// Case 15: the IFunction2D `displacement` route is BYTE-FOR-BYTE unchanged.
//
// Rather than pin an opaque vertex dump, this rebuilds the pre-2026-09-06
// pipeline by hand -- tessellate, tent-fold a COPY of the coords,
// ApplyDisplacementMapToObject, RecomputeVertexNormalsFromTopology, feed an
// indexed mesh -- and requires EXACT equality with what DisplacedGeometry
// produces today.  A u-varying function is used so the seam fold is load-
// bearing; a constant would pass even if the fold were dropped.
//
// NOTE: the "hand-rolled" replica below CALLS the same
// ApplyDisplacementMapToObject / RecomputeVertexNormalsFromTopology
// functions DisplacedGeometry itself calls (GeometryUtilities.h) -- it is
// not an independent re-implementation.  So this test pins the COMPOSITION
// (tessellate -> tent-fold -> apply -> recompute normals -> raw coords,
// in that order, with no extra step in between), not a byte-for-byte check
// against an oracle that computes the displaced mesh some other way.  A bug
// inside either shared function would pass here undetected on both sides.
//-----------------------------------------------------------------------------
static void TestFunction2DRouteUnchanged()
{
	std::cout << "Test 15: the `displacement` (IFunction2D) route is byte-identical to the hand-rolled legacy pipeline...\n";

	const Scalar scale = 0.3;

	// `UVaryingFunction2D` (declared above for the pole-crack case) evaluates
	// to `u`, so the tent fold is load-bearing here: a constant would pass
	// even if the fold were dropped.
	SphereGeometry*     pSphere = new SphereGeometry( 1.0 );
	UVaryingFunction2D* pFunc   = new UVaryingFunction2D();

	// (a) Through DisplacedGeometry.
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pSphere, kDetail, pFunc, scale, /*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	pDisp->Realize();
	VerticesListType actual;
	assert( EmitVertices( *pDisp, actual ) );

	// (b) The legacy pipeline, by hand.
	IndexTriangleListType tris;
	VerticesListType      verts;
	NormalsListType       normals;
	TexCoordsListType     coords;
	assert( pSphere->TessellateToMesh( tris, verts, normals, coords, kDetail ) );
	TexCoordsListType folded = coords;
	RemapTextureCoords( folded );
	ApplyDisplacementMapToObject( tris, verts, normals, folded, *pFunc, scale );
	RecomputeVertexNormalsFromTopology( tris, verts, normals );

	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( false, false );
	pMesh->BeginIndexedTriangles();
	pMesh->AddVertices( verts );
	pMesh->AddNormals( normals );
	pMesh->AddTexCoords( coords );
	pMesh->AddIndexedTriangles( tris );
	pMesh->DoneIndexedTriangles();
	VerticesListType expected;
	assert( EmitVertices( *pMesh, expected ) );

	assert( actual.size() == expected.size() );
	assert( !actual.empty() );
	size_t mismatches = 0;
	for( size_t i = 0; i < actual.size(); ++i ) {
		if( actual[i].x != expected[i].x || actual[i].y != expected[i].y || actual[i].z != expected[i].z ) {
			++mismatches;
		}
	}
	std::cout << "    " << actual.size() << " vertices compared, " << mismatches << " differ (exact equality)\n";
	assert( mismatches == 0 );

	pMesh->release();
	pDisp->release();
	pFunc->release();
	pSphere->release();
}

//-----------------------------------------------------------------------------
// Case 16: spelling BOTH `displacement` and `height` is a parse error that
// names both -- not a silent preference for one.
//-----------------------------------------------------------------------------
static void TestBothHeightRoutesRefused()
{
	std::cout << "Test 16: `displacement` + `height` together is refused, naming both...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	ParseCapturing( "both",
		"expression_function2d\n{\n\tname f_uv\n\texpr u*v\n}\n"
		"scalar_painter\n{\n\tname h_pox\n\texpression Po.x\n}\n"
		"sphere_geometry\n{\n\tname base\n\tradius 1.0\n}\n"
		"displaced_geometry\n{\n\tname bad\n\tbase_geometry base\n"
		"\tdisplacement f_uv\n\theight h_pox\n}\n",
		*job, log );

	assert( Contains( log, "displaced_geometry `bad`" ) );
	assert( Contains( log, "`displacement`" ) && Contains( log, "`f_uv`" ) );
	assert( Contains( log, "`height`" ) && Contains( log, "`h_pox`" ) );
	assert( Contains( log, "mutually exclusive" ) );
	assert( job->GetGeometries()->GetItem( "bad" ) == 0 );

	safe_release( job );
}

//-----------------------------------------------------------------------------
// Case 17: a COLOUR painter bound to `height` gets the standing scalar-pipe
// diagnostic (kScalarBoundToIPainterFmt), matched as substrings so a re-word
// does not silently un-test it.  Height is a LENGTH; routing it through JH
// spectral uplift is exactly what the scalar pipe exists to prevent.
//-----------------------------------------------------------------------------
static void TestColourPainterOnHeightRefused()
{
	std::cout << "Test 17: a colour painter on `height` yields the scalar-pipe diagnostic...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	ParseCapturing( "colour",
		"uniformcolor_painter\n{\n\tname cp\n\tcolor 0.5 0.5 0.5\n}\n"
		"sphere_geometry\n{\n\tname base\n\tradius 1.0\n}\n"
		"displaced_geometry\n{\n\tname bad2\n\tbase_geometry base\n\theight cp\n}\n",
		*job, log );

	assert( Contains( log, "displaced_geometry `bad2`" ) );
	assert( Contains( log, "is bound to `IPainter` chunk `cp`" ) );
	assert( Contains( log, "`scalar_painter`" ) && Contains( log, "no JH spectral uplift" ) );
	assert( job->GetGeometries()->GetItem( "bad2" ) == 0 );

	safe_release( job );
}

//-----------------------------------------------------------------------------
// Case 18: UV PARITY.  The same body `u*v` bound as an
// `expression_function2d` through `displacement`, and as a `scalar_painter`
// through `height`, must produce the SAME mesh -- exactly.
//
// This is the test that proves the synthesized hit's `ptCoord` is right,
// INCLUDING the `uv_seam_fold` tent treatment: the fold is applied in the
// shared DisplacedGeometry::ApplyHeightField, and if the scalar route saw raw
// UV while the function route saw folded UV, every vertex off u=v=0.5 would
// differ.  Both `uv_seam_fold` settings are checked, since only the TRUE case
// exercises the fold and only the FALSE case proves the fold is not applied
// unconditionally.
//-----------------------------------------------------------------------------
static void TestUVParityBetweenRoutes()
{
	std::cout << "Test 18: `u*v` gives the same mesh through `displacement` and through `height`...\n";

	const char* const kFold[] = { "TRUE", "FALSE" };
	for( int f = 0; f < 2; ++f ) {
		IJobPriv* job = 0;
		RISE_CreateJobPriv( &job );
		assert( job != 0 );

		std::string log;
		std::string body =
			"expression_function2d\n{\n\tname f_uv\n\texpr u*v\n}\n"
			"scalar_painter\n{\n\tname h_uv\n\texpression u*v\n}\n"
			"sphere_geometry\n{\n\tname base\n\tradius 1.0\n}\n"
			"displaced_geometry\n{\n\tname via_func\n\tbase_geometry base\n\tdetail 24\n"
			"\tdisplacement f_uv\n\tdisp_scale 0.4\n\tuv_seam_fold ";
		body += kFold[f];
		body += "\n}\n"
			"displaced_geometry\n{\n\tname via_field\n\tbase_geometry base\n\tdetail 24\n"
			"\theight h_uv\n\tdisp_scale 0.4\n\tuv_seam_fold ";
		body += kFold[f];
		body += "\n}\n";

		const bool ok = ParseCapturing( std::string("parity_") + kFold[f], body, *job, log );
		assert( ok );

		IGeometry* gFunc  = job->GetGeometries()->GetItem( "via_func" );
		IGeometry* gField = job->GetGeometries()->GetItem( "via_field" );
		assert( gFunc != 0 && gField != 0 );
		gFunc->Realize();
		gField->Realize();

		VerticesListType a, b;
		assert( EmitVertices( *gFunc,  a ) );
		assert( EmitVertices( *gField, b ) );
		assert( a.size() == b.size() && !a.empty() );

		Scalar maxErr = 0.0, maxSpread = 0.0;
		const Point3 origin( 0.0, 0.0, 0.0 );
		for( size_t i = 0; i < a.size(); ++i ) {
			maxErr = std::max( maxErr, std::fabs( a[i].x - b[i].x ) );
			maxErr = std::max( maxErr, std::fabs( a[i].y - b[i].y ) );
			maxErr = std::max( maxErr, std::fabs( a[i].z - b[i].z ) );
			maxSpread = std::max( maxSpread,
				std::fabs( Vector3Ops::Magnitude( Vector3Ops::mkVector3( a[i], origin ) ) - 1.0 ) );
		}
		std::cout << "    uv_seam_fold " << kFold[f] << ": max |func - field| = " << maxErr
		          << " over " << a.size() << " vertices (displacement spread " << maxSpread << ")\n";
		assert( maxErr <= 1e-12 );
		// Guard against a vacuous pass: the field must actually have moved
		// vertices off the unit sphere, or "identical" would mean nothing.
		assert( maxSpread > 0.05 );

		safe_release( job );
	}
}

//-----------------------------------------------------------------------------
// Case 19: ANALYTICAL-DERIVATIVE parity between the two routes.
//
// `ComputeAnalyticalDerivatives` (the SMS two-stage solver's query) has its
// own displacement evaluation, separate from the bake: the IFunction2D branch
// central-differences f in (u,v); the IScalarPainter branch central-
// differences the FIELD along the base's object-space dpdu/dpdv (first order,
// `P(u +/- eps) ~= P_b +/- eps*dpdu`) and steps `ptCoord` in lockstep with the
// same tent fold.  For a field that is a pure function of (u, v) those two
// must therefore agree -- which is what pins the lockstep stepping and the
// fold in the ANALYTICAL path, exactly as case 18 pins them in the BAKE path.
// Without this, the branch would be reachable only through SMS and only on
// scenes nobody runs in CI.
//-----------------------------------------------------------------------------
static void TestAnalyticalDerivativeParity()
{
	std::cout << "Test 19: ComputeAnalyticalDerivatives agrees between the two routes on a UV field...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	const bool ok = ParseCapturing( "deriv",
		"expression_function2d\n{\n\tname f_uv\n\texpr u*v\n}\n"
		"scalar_painter\n{\n\tname h_uv\n\texpression u*v\n}\n"
		// An ELLIPSOID, not a sphere: `ComputeAnalyticalDerivatives` is
		// implemented by EllipsoidGeometry (SphereGeometry has no analytical
		// query, so a sphere base makes both routes return false and the
		// comparison vacuous -- which is what the `probed == 4` guard below
		// catches).
		"ellipsoid_geometry\n{\n\tname base\n\tradii 1.0 1.0 1.0\n}\n"
		"displaced_geometry\n{\n\tname via_func\n\tbase_geometry base\n\tdetail 24\n"
		"\tdisplacement f_uv\n\tdisp_scale 0.4\n}\n"
		"displaced_geometry\n{\n\tname via_field\n\tbase_geometry base\n\tdetail 24\n"
		"\theight h_uv\n\tdisp_scale 0.4\n}\n",
		*job, log );
	assert( ok );

	IGeometry* gFunc  = job->GetGeometries()->GetItem( "via_func" );
	IGeometry* gField = job->GetGeometries()->GetItem( "via_field" );
	assert( gFunc != 0 && gField != 0 );

	// Sample away from the u=0.5 / v=0.5 tent vertices (where BOTH branches
	// document a sign flip) and away from the poles.
	const Scalar kUV[][2] = { {0.20,0.30}, {0.35,0.65}, {0.72,0.24}, {0.81,0.77} };
	Scalar worst = 0.0;
	unsigned int probed = 0;
	for( size_t i = 0; i < sizeof(kUV)/sizeof(kUV[0]); ++i ) {
		const Point2 uv( kUV[i][0], kUV[i][1] );
		Point3  Pa, Pb;
		Vector3 Na, Nb, dua, dub, dva, dvb, dnua, dnub, dnva, dnvb;
		const bool oka = gFunc->ComputeAnalyticalDerivatives(  uv, 0.0, Pa, Na, dua, dva, dnua, dnva );
		const bool okb = gField->ComputeAnalyticalDerivatives( uv, 0.0, Pb, Nb, dub, dvb, dnub, dnvb );
		assert( oka == okb );
		if( !oka ) continue;
		++probed;
		const Vector3 dP( Pa.x-Pb.x, Pa.y-Pb.y, Pa.z-Pb.z );
		worst = std::max( worst, Vector3Ops::Magnitude( dP ) );
		worst = std::max( worst, Vector3Ops::Magnitude( Na - Nb ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dua - dub ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dva - dvb ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dnua - dnub ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dnva - dnvb ) );
	}
	std::cout << "    " << probed << " uv probes, worst |func - field| across P/N/dpdu/dpdv/dndu/dndv = " << worst << "\n";
	assert( probed == 4 );
	// Both branches take the SAME eps and the SAME tent fold, so for a
	// (u,v)-only field the only difference is the scalar route's extra
	// first-order position step -- which the field ignores.  Agreement is
	// therefore to round-off, not merely to FD order.
	assert( worst <= 1e-9 );

	safe_release( job );
}

//-----------------------------------------------------------------------------
// Case 20: CLOSED-FORM check of the scalar-field branch's POSITION step.
//
// Test 19 uses `u*v`, a pure-(u,v) field: EvalHeightField's return value does
// not depend on the object-space POSITION argument at all, so the
// `dpdu_b * epsP` / `dpdv_b * epsP` position probes in the `IScalarPainter`
// branch of DisplacedGeometry::ComputeAnalyticalDerivatives are computed but
// never exercised -- deleting the position step and passing eps=0 (or any
// other constant) at both `f_uplus`/`f_uminus` sites leaves test 19 green.
//
// This test uses `height = Po.x` instead: a field that is a function of
// POSITION only and does not read (u,v) at all, so it can only be
// differentiated correctly by actually stepping the position.  Because
// `f(P) = P.x` is exactly linear, the central difference the scalar branch
// takes is exact (not merely FD-accurate): stepping to `P_b +/- eps*dpdu_b`
// and evaluating `f` there gives `P_b.x +/- eps*dpdu_b.x` with no truncation
// term, so `df/du = dpdu_b.x` and `df/dv = dpdv_b.x` to round-off.
// Substituting into DisplacedGeometry's own documented chain rule --
//   P_d    = P_b + s*f*N_b
//   dpdu_d = dpdu_b + s*(df/du*N_b + f*dndu_b)     (mirror for dpdv_d)
// -- gives a closed form this test computes INDEPENDENTLY, by querying the
// BASE ellipsoid's own ComputeAnalyticalDerivatives directly (not by
// re-running DisplacedGeometry's FD), and compares against what the
// `height`-routed DisplacedGeometry actually returns.
//
// Red-proofed 2026-09-06 against DisplacedGeometry.cpp's scalar branch
// (src/Library/Geometry/DisplacedGeometry.cpp): (a) zeroing the position
// step (evaluating all four probes at `P_b` instead of `P_b +/- eps*dpdu_b`
// / `P_b +/- eps*dpdv_b`) collapsed `df/du`/`df/dv` to 0 and failed this
// test's `worst <= 1e-9` assert, while leaving test 19 (`u*v`) green; (b)
// swapping `dpdu_b`/`dpdv_b` between the u- and v-probes also failed this
// test (df/du and df/dv traded values) while leaving test 19 green -- `u*v`
// can't distinguish either mutation because its position argument is
// ignored.  Both mutations were reverted after confirming the failure; the
// source is unchanged by this test's existence.
//-----------------------------------------------------------------------------
static void TestScalarHeightPositionStepClosedForm()
{
	std::cout << "Test 20: ComputeAnalyticalDerivatives' scalar-field position step matches a closed form on `Po.x`...\n";

	IJobPriv* job = 0;
	RISE_CreateJobPriv( &job );
	assert( job != 0 );

	std::string log;
	const bool ok = ParseCapturing( "posstep",
		"scalar_painter\n{\n\tname h_pox\n\texpression Po.x\n}\n"
		"ellipsoid_geometry\n{\n\tname base\n\tradii 1.0 1.0 1.0\n}\n"
		"displaced_geometry\n{\n\tname via_field\n\tbase_geometry base\n\tdetail 24\n"
		"\theight h_pox\n\tdisp_scale 0.4\n}\n",
		*job, log );
	assert( ok );

	IGeometry* gBase  = job->GetGeometries()->GetItem( "base" );
	IGeometry* gField = job->GetGeometries()->GetItem( "via_field" );
	assert( gBase != 0 && gField != 0 );

	// Same four probe points as case 19: away from the u=0.5/v=0.5 tent
	// vertices and away from the poles.
	const Scalar kUV[][2] = { {0.20,0.30}, {0.35,0.65}, {0.72,0.24}, {0.81,0.77} };
	const Scalar s = 0.4;
	Scalar worst = 0.0;
	unsigned int probed = 0;
	for( size_t i = 0; i < sizeof(kUV)/sizeof(kUV[0]); ++i ) {
		const Point2 uv( kUV[i][0], kUV[i][1] );

		Point3  Pb, Pd;
		Vector3 Nb, Nd, dpdu_b, dpdv_b, dndu_b, dndv_b, dpdu_d, dpdv_d, dndu_d, dndv_d;
		const bool okBase  = gBase->ComputeAnalyticalDerivatives(  uv, 0.0, Pb, Nb, dpdu_b, dpdv_b, dndu_b, dndv_b );
		const bool okField = gField->ComputeAnalyticalDerivatives( uv, 0.0, Pd, Nd, dpdu_d, dpdv_d, dndu_d, dndv_d );
		assert( okBase == okField );
		if( !okBase ) continue;
		++probed;

		// f = Po.x = Pb.x exactly; df/du = dpdu_b.x, df/dv = dpdv_b.x
		// exactly, both to round-off (see comment above).
		const Scalar f    = Pb.x;
		const Scalar dfdu = dpdu_b.x;
		const Scalar dfdv = dpdv_b.x;

		const Point3 expectedP = Point3Ops::mkPoint3( Pb, Nb * (s * f) );
		const Vector3 expectedDpdu(
			dpdu_b.x + s * ( dfdu * Nb.x + f * dndu_b.x ),
			dpdu_b.y + s * ( dfdu * Nb.y + f * dndu_b.y ),
			dpdu_b.z + s * ( dfdu * Nb.z + f * dndu_b.z ) );
		const Vector3 expectedDpdv(
			dpdv_b.x + s * ( dfdv * Nb.x + f * dndv_b.x ),
			dpdv_b.y + s * ( dfdv * Nb.y + f * dndv_b.y ),
			dpdv_b.z + s * ( dfdv * Nb.z + f * dndv_b.z ) );

		const Vector3 dP( Pd.x - expectedP.x, Pd.y - expectedP.y, Pd.z - expectedP.z );
		worst = std::max( worst, Vector3Ops::Magnitude( dP ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dpdu_d - expectedDpdu ) );
		worst = std::max( worst, Vector3Ops::Magnitude( dpdv_d - expectedDpdv ) );
	}
	std::cout << "    " << probed << " uv probes, worst |actual - closed form| across P/dpdu/dpdv = " << worst << "\n";
	assert( probed == 4 );
	assert( worst <= 1e-9 );

	safe_release( job );
}

//-----------------------------------------------------------------------------
// Builds a manually-WELDED watertight cube mesh: one shared vertex per
// corner (8 total), the standard 12-triangle closed triangulation. Used
// as a `DisplacedGeometry` base below instead of `BoxGeometry` because
// `BoxGeometry::TessellateToMesh` tessellates its six faces
// INDEPENDENTLY -- each face gets its own vertex grid with no seam
// welding to its neighbours -- so a plain tessellated box is ALSO not
// watertight by DL-31's edge-count check (measured: 96 boundary edges
// at detail=4, one full unshared perimeter per face). That is the exact
// same underlying pattern as DL-116 (SphereGeometry's unwelded poles),
// just at every seam rather than only the two poles; recorded as part
// of DL-116's own scope rather than a second new row, since it is the
// same tessellator-does-not-weld-seams mechanism, not a different bug.
// `TriangleMeshGeometryIndexed::TessellateToMesh` is a pass-through
// (one output vertex per distinct source index triple), so handing DisplacedGeometry an already-welded mesh as
// its base is what actually isolates THIS test's own subject -- the
// SignedDistanceLower forwarding -- from that separate tessellator gap.
static TriangleMeshGeometryIndexed* BuildWeldedCubeForDisplacement( const Scalar h )
{
	VerticesListType vertices;
	vertices.push_back( Point3( -h, -h, -h ) );	// 0
	vertices.push_back( Point3(  h, -h, -h ) );	// 1
	vertices.push_back( Point3(  h,  h, -h ) );	// 2
	vertices.push_back( Point3( -h,  h, -h ) );	// 3
	vertices.push_back( Point3( -h, -h,  h ) );	// 4
	vertices.push_back( Point3(  h, -h,  h ) );	// 5
	vertices.push_back( Point3(  h,  h,  h ) );	// 6
	vertices.push_back( Point3( -h,  h,  h ) );	// 7

	NormalsListType normals( vertices.size(), Vector3( 0, 0, 1 ) );	// unused by this test
	TexCoordsListType coords( vertices.size(), Point2( 0, 0 ) );		// unused by this test

	IndexTriangleListType tris;
	auto add = [&]( unsigned int a, unsigned int b, unsigned int c ) {
		IndexedTriangle t;
		t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
		t.iNormals[0] = a;  t.iNormals[1] = b;  t.iNormals[2] = c;
		t.iCoords[0] = a;   t.iCoords[1] = b;   t.iCoords[2] = c;
		tris.push_back( t );
	};
	add( 0, 1, 2 ); add( 0, 2, 3 );	// bottom (z=-h)
	add( 4, 6, 5 ); add( 4, 7, 6 );	// top (z=+h)
	add( 0, 5, 1 ); add( 0, 4, 5 );	// front (y=-h)
	add( 3, 2, 6 ); add( 3, 6, 7 );	// back (y=+h)
	add( 0, 3, 7 ); add( 0, 7, 4 );	// left (x=-h)
	add( 1, 5, 6 ); add( 1, 6, 2 );	// right (x=+h)

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//-----------------------------------------------------------------------------
// DL-31 sibling: DisplacedGeometry::DistanceToSurface already delegates to
// its baked mesh (the row above this one, "docs/CROSS_OBJECT_PROXIMITY_
// DESIGN.md 5.2, Phase 2"); SignedDistanceLower did not at all -- fixed
// to forward the same way.
//
// HISTORY (DL-143 made this pin LIVE): a `DisplacedGeometry`'s baked mesh
// comes from `m_pBase->TessellateToMesh(...)`, and when the base is
// ITSELF a `TriangleMeshGeometryIndexed` (as it is here -- a hand-welded,
// genuinely watertight cube, 8 vertices / 12 triangles), that class's OWN
// `TessellateToMesh` re-emits one vertex per distinct original
// (position, normal, uv) index triple.  HISTORICALLY it flattened every
// triangle corner to its own independent tuple, so the baked mesh had
// 36 vertices for 12 triangles, not 8 (that flattening is what made
// displaced lathe/sweep/skin bases render faceted -- see
// TestDisplacedBuildersShadeSmooth); it now preserves the source's
// sharing, so this cube bakes to exactly its own 8 vertices (asserted
// below).  Originally (`ComputeWatertightness` keyed by raw
// POSITION-ARRAY INDEX) the 36-vertex bake made every edge look like it
// was used by exactly one triangle, so the watertightness check read it
// as an open sheet even though the ORIGINAL base was perfectly closed --
// this test used to be a CONSISTENCY PIN on the refusal, not a red-proof
// of a successful signed answer, and said so.
//
// DL-143 fixed `ComputeWatertightness` to weld corners by POSITION before
// counting edges, so even a bake that duplicates corners (the old
// flattening, or any per-corner pipeline) welds right back down to the
// original 8 physical positions and is read as watertight; with sharing
// preserved the bake is closed under the raw index check too. This is
// therefore a REAL red/green assertion, not a pin: pre-DL-143 this
// test's own `!ok` assertion passed (the baked mesh refused); on this
// tree `ok` is now `true` (measured: `SignedDistanceLower` on the baked
// mesh answers `-half`, exact) -- the forwarder added by this row is
// exercised end-to-end for the first time, through a REAL bake, not just
// by inspection of its textual shape against `DistanceToSurface`'s
// already-proven forwarder.
//-----------------------------------------------------------------------------
static void TestSignedDistanceLowerForwardsToBakedMesh()
{
	std::cout << "Test 21: DisplacedGeometry::SignedDistanceLower forwards to the real baked mesh (DL-31 sibling)...\n";

	const Scalar half = 2.0;
	TriangleMeshGeometryIndexed* pWeldedCube = BuildWeldedCubeForDisplacement( half );

	// Sanity: the STANDALONE base really is watertight and answers the
	// signed query correctly on its own -- isolating "the base mesh is
	// fine" from whatever the bake does to it.
	{
		Scalar s = 0.0; bool e = false;
		const bool ok = pWeldedCube->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), s, e );
		assert( ok );
		assert( std::fabs( (double)s - ( -(double)half ) ) < 1e-9 );
		assert( e );
	}

	// Null displacement: no vertex positions move, only TessellateToMesh's
	// own re-emission runs.
	DisplacedGeometry* pDisp = new DisplacedGeometry(
		pWeldedCube, /*detail=*/4, /*displacement=*/0, /*disp_scale=*/0.0,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false );
	pDisp->Realize();
	assert( pDisp->IsValid() );

	// Shared topology survives the bake: 8 vertices, not 3 x 12 = 36.
	{
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( pDisp->TessellateToMesh( bt, bv, bn, bc, 0 ) );
		assert( bt.size() == 12 );
		assert( bv.size() == 8 );
	}

	const Point3 centre( 0, 0, 0 );

	// The pre-existing, already-working forward: proves a real mesh was
	// baked and DistanceToSurface genuinely reaches it (not refused, not
	// zero-by-coincidence).
	Scalar dUnsigned = -1.0;
	const bool okUnsigned = pDisp->DistanceToSurface( centre, Scalar( 1000 ), dUnsigned );
	assert( okUnsigned );
	assert( std::fabs( (double)dUnsigned - (double)half ) < 1e-6 );

	// The NEW forward: reaches the SAME mesh, which (per the comment
	// above) keeps the cube's own 8 shared vertices through
	// TessellateToMesh (and would weld right back to them under DL-143's
	// position-keyed watertightness check even if it did not), so it
	// correctly ANSWERS the signed query --
	// the true depth to the nearest face at the centre is exactly
	// `-half`, same as the standalone base mesh's own answer above.
	Scalar outSigned = 12345.0;	// sentinel: must be overwritten on success
	bool outExact = false;
	const bool ok = pDisp->SignedDistanceLower( centre, Scalar( 1000 ), outSigned, outExact );
	assert( ok );
	assert( outExact );
	assert( std::fabs( (double)outSigned - ( -(double)half ) ) < 1e-6 );

	pDisp->release();
	pWeldedCube->release();
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
	TestDisplacedBuildersShadeSmooth();
	TestDetailRefinesMeshBases();
	TestRefinedTopologyStableAcrossRefresh();
	TestConcurrentRealize();
	TestScalarHeightIsObjectSpaceAndExact();
	TestScalarHeightThroughTheChunk();
	TestFunction2DRouteUnchanged();
	TestBothHeightRoutesRefused();
	TestColourPainterOnHeightRefused();
	TestUVParityBetweenRoutes();
	TestAnalyticalDerivativeParity();
	TestScalarHeightPositionStepClosedForm();
	TestSignedDistanceLowerForwardsToBakedMesh();

	std::cout << "All DisplacedGeometry tests passed.\n";
	return 0;
}
