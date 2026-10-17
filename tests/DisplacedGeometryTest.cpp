// This suite's assertions WRAP REAL CALLS (assert( RISE_API_CreateLatheGeometry( &p, d ) ),
// assert( geom->TessellateToMesh( ... ) )) and are its only checks, so they must run in EVERY build
// configuration: a Release build defines NDEBUG, which would compile the calls away and test nothing.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <iostream>
#include <chrono>
#include <cassert>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
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
// recompute -- see TestDisplacedOverPinchedLatheStaysClosed below and the audit
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
	// `detail` (32) now makes the LATHE's authored grid get subdivided (more rows along
	// the profile, more radial columns -- see TessellateRefined), so the baked
	// mesh is finer than the 5-row base.  Exact count expectation WITHOUT replicating
	// the builder: the vase is a closed genus-0 surface (a pole at each end), so Euler
	// gives V - E + F = 2 with E = 3F/2, i.e. V = F/2 + 2 -- true of the base
	// (98 = 192/2 + 2) and of any crack-free refinement, but NOT of one with duplicated
	// or missing seam vertices.
	assert( baseVerts.size() == baseTris.size() / 2 + 2 );
	assert( dTris.size() > baseTris.size() );
	assert( dVerts.size() > baseVerts.size() );
	assert( dVerts.size() == dTris.size() / 2 + 2 );

	// The refinement keeps every profile row (original profile points are re-emitted
	// exactly) and the angle-0 radial column, so each ORIGINAL base vertex on that
	// column and both poles must reappear displaced by exactly k along its own unit
	// normal (displaced indices are NOT preserved: TessellateToMesh re-emits in
	// first-seen order).
	Scalar worstErr = 0.0; size_t checked = 0;
	for( size_t i = 0; i < baseVerts.size(); ++i ) {
		if( !( std::fabs( baseVerts[i].x ) < 1e-9 && baseVerts[i].z >= 0.0 ) ) continue;
		++checked;
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
		<< " baked (" << baseTris.size() << " -> " << dTris.size() << " tris); " << checked << " angle-0 / pole vertices: worst |displaced - (base + k n)| = " << worstErr << " (amplitude k=" << k << ")\n";
	assert( checked >= 5 );
	assert( worstErr < 1e-9 );

	pDisp->release();
	pConst->release();
	pLathe->release();
}

//-----------------------------------------------------------------------------
// Doc 89 slice D audit: displaced_geometry over an INTERIOR-PINCH lathe used to TEAR
// at the waist.  The pinch's two poles are coincident POSITIONS with EXACTLY OPPOSITE
// normals (one vertex per adjacent band; see TestLatheInteriorPolePinch in
// ProceduralMeshTest.cpp), so displacing each along its OWN normal pushed the pair
// apart by 2*disp_scale and opened a visible crack -- documented as a limitation
// because welding the two poles in the BASE would put one band's SHADING normal in the
// wrong half-space.  Round 5 closes the crack in the DISPLACEMENT instead:
// DisplacedGeometry moves every group of base-coincident vertices to ONE shared position
// (the mean of the members' displacement vectors) while each member keeps its own
// shading normal, so the waist is watertight and both bands still shade correctly.
// This was the pinned-at-~2k gap of the earlier revisions; it is now asserted CLOSED.
//-----------------------------------------------------------------------------
static void TestDisplacedOverPinchedLatheStaysClosed()
{
	std::cout << "Test 10: displaced_geometry over an interior-pinch lathe stays CLOSED at the waist (the old ~2k tear)...\n";

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
	assert( nearAxisY.size() >= 2 && "pinch: both ex-coincident pole vertices must survive the bake (their normals differ, so they stay two vertices)" );

	Scalar loY = 1e30, hiY = -1e30;
	for( size_t i = 0; i < nearAxisY.size(); ++i ) {
		loY = std::min( loY, nearAxisY[i] );
		hiY = std::max( hiY, nearAxisY[i] );
	}
	const Scalar gap = hiY - loY;
	std::cout << "  [info] pinch gap after displacement: " << gap << " (was ~2*k = " << 2.0*k << " before the coincident-vertex weld)\n";

	// MONEY ASSERTION -- the two ex-coincident poles end at ONE position.
	assert( gap < 1e-9 );

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
// relief, and `detail 48` did nothing.  DisplacedGeometry::BuildMesh now brings a
// mesh base to spacing (base bbox diagonal / detail) before displacing: a lathe /
// sweep / skin subdivides its authored grid to that density (row and column spacing <= target;
// the quad diagonal is sqrt(2) longer), other indexed meshes subdivide uniformly.
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
		// a refined grid bounds its ROW and COLUMN spacing by the target; the quad diagonal is sqrt(2) longer
		assert( maxEdge <= 1.5 * target );
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

//-----------------------------------------------------------------------------
// A refined mesh base keeps the BASE'S smooth shading-normal field.
//
// The post-displacement normal recompute is purely geometric, and every
// vertex the refinement adds lies on its parent facet's plane.  Without the
// shading-delta correction the refined mesh shades FLAT inside each facet (the
// base's own smooth normals, which an unrefined bake interpolates across the
// facet, are thrown away) -- the base's polygonization comes back as vertical
// streaks on a displaced lathe.  Near-zero displacement isolates exactly that:
// the smooth lathe's refined mesh must NOT contain flat-shaded triangles.
//-----------------------------------------------------------------------------
static void TestRefinedMeshKeepsBaseShadingField()
{
	std::cout << "Test 10f: a refined mesh base keeps the base's smooth shading normals (no flat-per-facet shading)...\n";

	TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 48, pConst, 1e-7, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );

	size_t flat = 0, counted = 0;
	for( size_t i = 0; i < t.size(); ++i ) {
		const Vector3& a = n[t[i].iNormals[0]];
		const Vector3& b = n[t[i].iNormals[1]];
		const Vector3& d = n[t[i].iNormals[2]];
		// skip triangles that touch a pole (their corner normals legitimately differ hugely)
		const Scalar spread = std::max( Vector3Ops::Magnitude( a - b ), std::max( Vector3Ops::Magnitude( b - d ), Vector3Ops::Magnitude( a - d ) ) );
		++counted;
		if( spread < 1e-6 ) ++flat;
	}
	std::cout << "  [info] " << flat << " of " << counted << " triangles are flat-shaded (corner normals identical)\n";
	// Smooth base: essentially none.  Without the delta ~ the whole interior of
	// every long facet (measured: > 40%).
	assert( flat * 20 < counted );

	pDisp->release();
	pConst->release();
	pLathe->release();
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
	const std::chrono::steady_clock::time_point tr0 = std::chrono::steady_clock::now();
	pFn->SetValue( 3.0 );		// 0.05 -> 0.15 amplitude, through the observer path
	const double refitMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - tr0 ).count();
	std::cout << "  [info] refit of the refined mesh (re-tessellate + re-refine + displace + normals + BVH refit): " << refitMs << " ms\n";
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
// RecomputeVertexNormalsFromTopology is ANGLE-WEIGHTED, so vertex normals do
// not depend on the triangulation.  The unweighted per-face average let thin
// slivers (the adaptive mesh refinement's 1/2/3-edge splits make plenty)
// dominate a vertex's normal -- visible as streaking on a displaced vase.
//
// Two meshes of the SAME piecewise-linear surface: A is a coarse 5x5-cell grid
// over z = 0.4 sin(1.3x) cos(1.1y) (face normals differ by several degrees
// between facets); B re-triangulates every A triangle into two SLIVERS by
// inserting a point 3% from one corner along an edge (the children are
// coplanar with their parent, so their face normals are the parent's).  The
// interior angles at each ORIGINAL vertex still sum to the parent's, so the
// angle-weighted normals of A and B agree to rounding; the unweighted average
// counts a vertex once per incident child instead of once per parent and is
// several degrees off.  A degenerate triangle (a repeated index) in B must not
// produce a NaN.
//-----------------------------------------------------------------------------
static void TestAngleWeightedNormalsIgnoreTriangulation()
{
	std::cout << "Test 10e: vertex normals are angle-weighted -- independent of how a surface is triangulated...\n";

	const int N = 5;
	VerticesListType vA;
	for( int j = 0; j <= N; ++j ) {
		for( int i = 0; i <= N; ++i ) {
			const Scalar x = Scalar( i ) / N * 3.0, y = Scalar( j ) / N * 3.0;
			vA.push_back( Point3( x, y, 0.4 * std::sin( 1.3 * x ) * std::cos( 1.1 * y ) ) );
		}
	}
	IndexTriangleListType tA;
	auto idx = [&]( int i, int j ) { return (unsigned int)( j * ( N + 1 ) + i ); };
	for( int j = 0; j < N; ++j ) {
		for( int i = 0; i < N; ++i ) {
			// alternate the diagonal so the fans are irregular
			if( ( i + j ) & 1 ) {
				tA.push_back( MakeIndexedTriangleSameIdx( idx(i,j), idx(i+1,j), idx(i+1,j+1) ) );
				tA.push_back( MakeIndexedTriangleSameIdx( idx(i,j), idx(i+1,j+1), idx(i,j+1) ) );
			} else {
				tA.push_back( MakeIndexedTriangleSameIdx( idx(i,j), idx(i+1,j), idx(i,j+1) ) );
				tA.push_back( MakeIndexedTriangleSameIdx( idx(i+1,j), idx(i+1,j+1), idx(i,j+1) ) );
			}
		}
	}

	// B: slivers.
	VerticesListType vB = vA;
	IndexTriangleListType tB;
	for( size_t t = 0; t < tA.size(); ++t ) {
		const unsigned int a = tA[t].iVertices[0], b = tA[t].iVertices[1], c = tA[t].iVertices[2];
		const Scalar s = 0.97;
		const unsigned int m = (unsigned int)vB.size();
		vB.push_back( Point3( vA[a].x + s * ( vA[b].x - vA[a].x ), vA[a].y + s * ( vA[b].y - vA[a].y ), vA[a].z + s * ( vA[b].z - vA[a].z ) ) );
		tB.push_back( MakeIndexedTriangleSameIdx( a, m, c ) );
		tB.push_back( MakeIndexedTriangleSameIdx( m, b, c ) );
	}
	// a degenerate triangle on an original vertex: repeated index -> zero-length edge
	tB.push_back( MakeIndexedTriangleSameIdx( 0, 0, 1 ) );

	NormalsListType nA( vA.size(), Vector3( 0, 0, 1 ) ), nB( vB.size(), Vector3( 0, 0, 1 ) );
	RecomputeVertexNormalsFromTopology( tA, vA, nA );
	RecomputeVertexNormalsFromTopology( tB, vB, nB );

	Scalar worstDeg = 0.0;
	for( size_t i = 0; i < vA.size(); ++i ) {
		assert( std::isfinite( nB[i].x ) && std::isfinite( nB[i].y ) && std::isfinite( nB[i].z ) );
		Scalar d = Vector3Ops::Dot( nA[i], nB[i] );
		d = std::max( Scalar(-1.0), std::min( Scalar(1.0), d ) );
		worstDeg = std::max( worstDeg, std::acos( d ) * 180.0 / PI );
	}
	std::cout << "  [info] worst angle between the two triangulations' vertex normals: " << worstDeg << " deg\n";
	// angle-weighted: rounding only (red on the unweighted average: several degrees)
	assert( worstDeg < 1e-4 );
}

//-----------------------------------------------------------------------------
// Review round 1, P1-1: UV charts are interpolated LINEARLY at refinement
// midpoints, except on an axis the base DECLARES periodic.
//
// The first cut took the short way round for ANY chart delta > 0.5 on every mesh
// base, which corrupts ordinary charts: a [0,1]^2 quad (every edge spans 1), a
// tiled [0,4]^2 chart, a capped cylinder's wall (arc-length v spans 0.6), a
// sweep's planar end cap.  Wrapping is now taken only where the builder
// declared it (a full-turn lathe's u, a sweep's closed-profile u) and only for
// ring-stitch edges (both ends in the periodic region).
//-----------------------------------------------------------------------------
class UVSpyFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar /*x*/, const Scalar /*y*/ ) const { return 1.0; }
};

static TriangleMeshGeometryIndexed* MakeUVQuad( const Scalar umax )
{
	TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false );
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 1, -0.5, -0.5 ) ); m->AddVertex( Point3( 1, 0.5, -0.5 ) );
	m->AddVertex( Point3( 1, 0.5, 0.5 ) );   m->AddVertex( Point3( 1, -0.5, 0.5 ) );
	for( int i = 0; i < 4; ++i ) m->AddNormal( Vector3( 1, 0, 0 ) );
	m->AddTexCoord( Point2( 0, 0 ) ); m->AddTexCoord( Point2( umax, 0 ) );
	m->AddTexCoord( Point2( umax, umax ) ); m->AddTexCoord( Point2( 0, umax ) );
	m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( 0, 1, 2 ) );
	m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( 0, 2, 3 ) );
	m->DoneIndexedTriangles();
	return m;
}

static Scalar CircDist( const Scalar a, const Scalar b )
{
	Scalar d = std::fabs( a - b );
	d -= std::floor( d );
	return std::min( d, 1.0 - d );
}

static size_t CountZeroUVAreaTriangles( const IndexTriangleListType& t, const VerticesListType& v, const TexCoordsListType& c )
{
	size_t n = 0;
	for( size_t i = 0; i < t.size(); ++i ) {
		const Point2& a = c[t[i].iCoords[0]]; const Point2& b = c[t[i].iCoords[1]]; const Point2& d = c[t[i].iCoords[2]];
		const Scalar uvArea = 0.5 * std::fabs( ( b.x - a.x ) * ( d.y - a.y ) - ( d.x - a.x ) * ( b.y - a.y ) );
		const Vector3 cr = Vector3Ops::Cross( Vector3Ops::mkVector3( v[t[i].iVertices[1]], v[t[i].iVertices[0]] ), Vector3Ops::mkVector3( v[t[i].iVertices[2]], v[t[i].iVertices[0]] ) );
		if( uvArea < 1e-12 && Vector3Ops::Magnitude( cr ) > 1e-12 ) ++n;
	}
	return n;
}

static void TestRefinementChartInterpolation()
{
	// Each check below is SOFT (reported, tallied, asserted once at the end) so a red run
	// shows every sub-case's damage, not just the first.
	int softFails = 0;
#define SOFT( cond ) do { if( !( cond ) ) { ++softFails; std::cout << "  [FAIL] " #cond "\n"; } } while( 0 )
	std::cout << "Test 10g: refinement midpoints interpolate the UV chart linearly, wrapping only on a declared-periodic axis...\n";

	// ---- (1) plain quads: [0,1]^2 and a tiled [0,4]^2 chart -------------------
	{
		const Scalar umaxes[2] = { 1.0, 4.0 };
		for( int q = 0; q < 2; ++q ) {
			const Scalar umax = umaxes[q];
			TriangleMeshGeometryIndexed* pQuad = MakeUVQuad( umax );
			UVSpyFunction2D* pFn = new UVSpyFunction2D();
			DisplacedGeometry* pDisp = new DisplacedGeometry( pQuad, 8, pFn, 0.1, false, false, /*seamFold=*/false );
			pDisp->Realize();
			IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
			assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
			size_t bad = 0;
			for( size_t i = 0; i < v.size(); ++i ) {
				const Scalar uTrue = ( v[i].y + 0.5 ) * umax, vTrue = ( v[i].z + 0.5 ) * umax;
				if( std::fabs( c[i].x - uTrue ) > 1e-9 || std::fabs( c[i].y - vTrue ) > 1e-9 ) ++bad;
			}
			std::cout << "  [info] quad [0," << umax << "]^2: " << v.size() << " vertices after refinement, " << bad << " with a wrong UV\n";
			assert( v.size() > 4 );
			SOFT( bad == 0 );
			pDisp->release(); pFn->release(); pQuad->release();
		}
	}

	// ---- (2) capped cylinder (lathe, full turn): wall v spans 0.6; u wraps ----
	{
		const int nRadial = 24;
		static const double prof[] = { 0.0, 0.0,  1.0, 0.0,  1.0, 3.0,  0.0, 3.0 };	// arc 1 + 3 + 1: wall v = 0.2 -> 0.8
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = nRadial; ld.smooth = false;
		ITriangleMeshGeometryIndexed* pI = 0;
		assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		assert( pLathe );
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( pLathe->TessellateToMesh( bt, bv, bn, bc, 0 ) );

		UVSpyFunction2D* pFn = new UVSpyFunction2D();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 48, pFn, 1e-6, false, false, /*seamFold=*/false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		assert( t.size() > bt.size() );

		size_t wallBadV = 0, wallBadU = 0, wall = 0, capBadV = 0, capBadU = 0, cap = 0;
		for( size_t i = 0; i < v.size(); ++i ) {
			const Scalar r = std::sqrt( v[i].x * v[i].x + v[i].z * v[i].z );
			if( r > 1.0 - 1e-4 && v[i].y > 1e-3 && v[i].y < 3.0 - 1e-3 ) {		// wall
				++wall;
				if( std::fabs( c[i].y - ( 1.0 + v[i].y ) / 5.0 ) > 1e-4 ) ++wallBadV;
				const Scalar uTrue = std::atan2( v[i].x, v[i].z ) / TWO_PI;
				if( CircDist( c[i].x, uTrue ) > 1e-4 ) ++wallBadU;
			} else if( ( std::fabs( v[i].y ) < 1e-4 || std::fabs( v[i].y - 3.0 ) < 1e-4 ) && r > 1e-6 ) {	// bottom / top cap POLE FANS
				++cap;
				const Scalar vTrue = ( v[i].y < 1.0 ) ? r / 5.0 : 1.0 - r / 5.0;
				if( std::fabs( c[i].y - vTrue ) > 3e-3 ) ++capBadV;	// rim chord midpoints sit a sagitta inside the unit circle (<= 0.0017 in v)
				// u at a pole fan vertex is the vertex's own polar angle: a spoke midpoint must NOT
				// average in the pole's placeholder u (0.5), and the fan triangle across the wrap must
				// not take the far-side u.
				if( CircDist( c[i].x, std::atan2( v[i].x, v[i].z ) / TWO_PI ) > 0.02 ) ++capBadU;	// linear u over a fan triangle differs from the true polar angle by up to ~0.01 turn (3.6 deg, 24 columns) where radius varies along the edge; the placeholder bug is 0.05-0.5 turn
			}
		}
		std::cout << "  [info] capped-cylinder lathe: " << wall << " wall vertices (v wrong: " << wallBadV << ", u wrong (circular): " << wallBadU
			<< "), " << cap << " pole-fan vertices (v wrong: " << capBadV << ", u wrong (circular): " << capBadU << ")\n";
		assert( wall > 100 );
		SOFT( wallBadV == 0 );		// wall v spans 0.6 > 0.5: not a wrap
		SOFT( wallBadU == 0 );		// the declared-periodic u seam edge IS taken the short way round
		SOFT( capBadV == 0 );
		SOFT( capBadU == 0 );		// pole-fan spokes carry their ring vertex's u, not the pole placeholder's

		// circular u-continuity of every triangle (a wrong wrap midpoint is ~0.5 away)
		Scalar worstU = 0.0;
		for( size_t i = 0; i < t.size(); ++i ) {
			for( int k = 0; k < 3; ++k ) {
				const Point2& a = c[t[i].iCoords[k]]; const Point2& b = c[t[i].iCoords[( k + 1 ) % 3]];
				const unsigned int ia = t[i].iVertices[k], ib = t[i].iVertices[( k + 1 ) % 3];
				if( std::sqrt( v[ia].x * v[ia].x + v[ia].z * v[ia].z ) < 1e-6 || std::sqrt( v[ib].x * v[ib].x + v[ib].z * v[ib].z ) < 1e-6 ) continue;	// only the pole vertex itself (u is a placeholder there); every fan vertex IS checked
				worstU = std::max( worstU, CircDist( a.x, b.x ) );
			}
		}
		std::cout << "  [info] worst circular u step across any refined edge = " << worstU << " (base column pitch " << 1.0 / nRadial << ")\n";
		SOFT( worstU <= 1.0 / nRadial + 1e-9 );

		pDisp->release(); pFn->release(); pLathe->release();
	}

	// ---- (3) sweep: planar end cap is affine in position; ring u wraps; no new zero-UV-area triangles ----
	{
		const int nProfile = 28;
		double prof[ 2 * 28 ];
		for( int i = 0; i < nProfile; ++i ) {
			const double a = 2.0 * 3.14159265358979323846 * double( i ) / nProfile;
			prof[ 2*i ] = 0.5 * std::cos( a ); prof[ 2*i + 1 ] = 0.3 * std::sin( a );	// elliptic, so the cap chart is anisotropic
		}
		const double path[] = { 0.0, 0.0, 0.0,   0.0, 1.4, 0.0,   0.0, 2.8, 0.0,   0.0, 4.2, 0.0 };
		SweepDescriptor sd; sd.profilePoints = prof; sd.numProfilePoints = nProfile; sd.pathPoints = path; sd.numPathPoints = 4;
		sd.nLen = 64; sd.capStart = true; sd.capEnd = true; sd.frameHintX = 1.0;
		ITriangleMeshGeometryIndexed* pI = 0;
		assert( RISE_API_CreateSweepGeometry( &pI, sd ) );
		TriangleMeshGeometryIndexed* pSweep = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		assert( pSweep );
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( pSweep->TessellateToMesh( bt, bv, bn, bc, 0 ) );
		const size_t baseZero = CountZeroUVAreaTriangles( bt, bv, bc );

		UVSpyFunction2D* pFn = new UVSpyFunction2D();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pSweep, 48, pFn, 1e-6, false, false, /*seamFold=*/false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		assert( t.size() > bt.size() );

		// start cap: flat normal along -path tangent (-Y here) -- the UV must be an affine
		// function of position, fitted from three non-collinear vertices of that cap
		std::vector<size_t> capIdx;
		for( size_t i = 0; i < v.size(); ++i ) if( n[i].y < -0.999 && std::fabs( v[i].y ) < 1e-3 ) capIdx.push_back( i );
		assert( capIdx.size() > 12 );
		size_t i0 = capIdx[0], i1 = capIdx[0], i2 = capIdx[0];
		Scalar bestArea = 0.0;
		for( size_t a = 0; a < capIdx.size() && a < 40; ++a ) for( size_t b = a + 1; b < capIdx.size() && b < 40; ++b ) for( size_t d = b + 1; d < capIdx.size() && d < 40; ++d ) {
			const Vector3 cr = Vector3Ops::Cross( Vector3Ops::mkVector3( v[capIdx[b]], v[capIdx[a]] ), Vector3Ops::mkVector3( v[capIdx[d]], v[capIdx[a]] ) );
			const Scalar ar = Vector3Ops::Magnitude( cr );
			if( ar > bestArea ) { bestArea = ar; i0 = capIdx[a]; i1 = capIdx[b]; i2 = capIdx[d]; }
		}
		assert( bestArea > 1e-6 );
		// solve uv = M (x,z) + o on the fitted triple (cap lies in the y = 0 plane)
		const Scalar dx1 = v[i1].x - v[i0].x, dz1 = v[i1].z - v[i0].z, dx2 = v[i2].x - v[i0].x, dz2 = v[i2].z - v[i0].z;
		const Scalar det = dx1 * dz2 - dx2 * dz1;
		Scalar worstCap = 0.0;
		for( size_t k = 0; k < capIdx.size(); ++k ) {
			const size_t i = capIdx[k];
			const Scalar px = v[i].x - v[i0].x, pz = v[i].z - v[i0].z;
			const Scalar s = ( px * dz2 - pz * dx2 ) / det, tt = ( dx1 * pz - dz1 * px ) / det;	// barycentric
			const Scalar uPred = c[i0].x + s * ( c[i1].x - c[i0].x ) + tt * ( c[i2].x - c[i0].x );
			const Scalar vPred = c[i0].y + s * ( c[i1].y - c[i0].y ) + tt * ( c[i2].y - c[i0].y );
			worstCap = std::max( worstCap, std::max( std::fabs( c[i].x - uPred ), std::fabs( c[i].y - vPred ) ) );
		}
		std::cout << "  [info] sweep start cap: " << capIdx.size() << " vertices, worst deviation from an affine chart = " << worstCap << "\n";
		SOFT( worstCap < 1e-4 );

		// ring u wrap: every wall edge steps by at most one profile column (circularly)
		Scalar worstU = 0.0;
		for( size_t i = 0; i < t.size(); ++i ) {
			const unsigned int i0v = t[i].iVertices[0];
			if( std::fabs( n[i0v].y ) > 0.99 ) continue;		// cap or pole-ish
			for( int k = 0; k < 3; ++k ) {
				if( std::fabs( n[t[i].iVertices[k]].y ) > 0.99 || std::fabs( n[t[i].iVertices[( k + 1 ) % 3]].y ) > 0.99 ) continue;
				worstU = std::max( worstU, CircDist( c[t[i].iCoords[k]].x, c[t[i].iCoords[( k + 1 ) % 3]].x ) );
			}
		}
		// profile column steps are the elliptic arc fractions (<= ~ 2/nProfile at the ellipse ends)
		std::cout << "  [info] sweep wall: worst circular u step across a refined edge = " << worstU << "\n";
		SOFT( worstU < 0.06 );

		const size_t zero = CountZeroUVAreaTriangles( t, v, c );
		std::cout << "  [info] zero-UV-area triangles: base " << baseZero << ", refined " << zero << "\n";
		SOFT( zero == baseZero );

		pDisp->release(); pFn->release(); pSweep->release();
	}

	// ---- (4) path_instances_geometry of a lathe tube keeps the template's periodic u ----
	{
		static const double prof[] = { 1.0, 0.0,  1.0, 2.0 };
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 2; ld.nRadial = 16;
		ITriangleMeshGeometryIndexed* pTplI = 0;
		assert( RISE_API_CreateLatheGeometry( &pTplI, ld ) );
		static const double path[] = { 0.0, 0.0, 0.0,   0.0, 0.0, 10.0 };
		PathInstancesDescriptor pd;
		pd.pGeometry = dynamic_cast<IGeometry*>( pTplI ); pd.pathPoints = path; pd.numPathPoints = 2;
		pd.nLen = 8; pd.pitch = 20.0; pd.phase = 1.0; pd.scale = 1.0; pd.detail = 16;
		ITriangleMeshGeometryIndexed* pInstI = 0;
		assert( RISE_API_CreatePathInstancesGeometry( &pInstI, pd ) );
		TriangleMeshGeometryIndexed* pInst = dynamic_cast<TriangleMeshGeometryIndexed*>( pInstI );
		assert( pInst );
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( pInst->TessellateToMesh( bt, bv, bn, bc, 0 ) );

		UVSpyFunction2D* pFn = new UVSpyFunction2D();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pInst, 64, pFn, 0.01, false, false, /*seamFold=*/false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		assert( t.size() > bt.size() );
		size_t bad = 0; Scalar worst = 0.0;
		for( size_t i = 0; i < v.size(); ++i ) {
			Scalar best = 1e30; size_t bi = 0;
			for( size_t j = 0; j < bv.size(); ++j ) {
				const Scalar dx = v[i].x - bv[j].x, dy = v[i].y - bv[j].y, dz = v[i].z - bv[j].z;
				const Scalar dd = dx * dx + dy * dy + dz * dz;
				if( dd < best ) { best = dd; bi = j; }
			}
			const Scalar e = CircDist( c[i].x, bc[bi].x );
			if( e > 0.15 ) { ++bad; worst = std::max( worst, e ); }
		}
		std::cout << "  [info] path_instances(lathe tube) refined: " << v.size() << " vertices, " << bad << " with u more than 0.15 turn from the nearest base vertex (worst " << worst << ")\n";
		SOFT( bad == 0 );
		pDisp->release(); pFn->release(); pInst->release(); pTplI->release();
	}

	assert( softFails == 0 );
#undef SOFT
}

//-----------------------------------------------------------------------------
// Review round 1, P2-1 / P2-2: the base's shading-normal field is TRANSPORTED
// (rotated with the geometric normal), never added as a world-space delta, and
// it is applied to every smooth mesh base -- refined or not.
//
// (a) P2-1: a world-space delta pushes shading normals behind steep faces.  For
//     a table of (n_radial, displacement, frequency) configurations the count of
//     faces with a vertex shading normal BEHIND the face must not exceed the
//     count the purely geometric normals have on the same mesh.
// (b) P2-2: a base just ABOVE the refinement threshold and one just BELOW it
//     shade the same vertices within a small normal tolerance (the look must not
//     jump when refinement switches on).
//-----------------------------------------------------------------------------
class SinFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar freq;
	SinFunction2D( const Scalar f ) : freq( f ) {}
	Scalar Evaluate( const Scalar x, const Scalar y ) const { return 0.5 + 0.5 * std::sin( TWO_PI * freq * x ) * std::cos( TWO_PI * freq * y ); }
};

static size_t CountBackfacingVertexNormals( const IndexTriangleListType& t, const VerticesListType& v, const NormalsListType& n )
{
	size_t cnt = 0;
	for( size_t i = 0; i < t.size(); ++i ) {
		const Vector3 cr = Vector3Ops::Cross( Vector3Ops::mkVector3( v[t[i].iVertices[1]], v[t[i].iVertices[0]] ), Vector3Ops::mkVector3( v[t[i].iVertices[2]], v[t[i].iVertices[0]] ) );
		const Scalar m = Vector3Ops::Magnitude( cr );
		if( m < 1e-12 ) continue;
		const Vector3 nf = cr * ( 1.0 / m );
		bool bad = false;
		for( int k = 0; k < 3; ++k ) if( Vector3Ops::Dot( n[t[i].iNormals[k]], nf ) <= 0.0 ) bad = true;
		if( bad ) ++cnt;
	}
	return cnt;
}

static void TestTransportedShadingNormals()
{
	std::cout << "Test 10h: base shading normals are transported with the displaced frame and applied to refined or unrefined smooth bases alike...\n";

	// ---- (a) back-facing vertex shading normals vs the geometric-only count ----
	const int radials[3] = { 6, 12, 24 };
	const Scalar disps[3] = { 0.05, 0.1, 0.2 };
	const Scalar freqs[2] = { 2.0, 8.0 };
	size_t worstExcess = 0, totalShading = 0, totalGeom = 0;
	for( int ri = 0; ri < 3; ++ri ) for( int di = 0; di < 3; ++di ) for( int fi = 0; fi < 2; ++fi ) {
		TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( radials[ri] );
		SinFunction2D* pFn = new SinFunction2D( freqs[fi] );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 24, pFn, disps[di], false, false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		NormalsListType g( v.size(), Vector3( 0, 0, 1 ) );
		RecomputeVertexNormalsFromTopology( t, v, g );
		const size_t cs = CountBackfacingVertexNormals( t, v, n ), cg = CountBackfacingVertexNormals( t, v, g );
		totalShading += cs; totalGeom += cg;
		if( cs > cg ) worstExcess = std::max( worstExcess, cs - cg );
		if( cs > cg ) std::cout << "  [FAIL cfg] n_radial " << radials[ri] << " disp " << disps[di] << " freq " << freqs[fi] << ": shading " << cs << " > geometric " << cg << "\n";
		pDisp->release(); pFn->release(); pLathe->release();
	}
	std::cout << "  [info] 18 configurations: faces with a back-facing vertex shading normal = " << totalShading << " (geometric-only normals: " << totalGeom << ")\n";
	assert( worstExcess == 0 );

	// ---- (b) continuity across the refinement threshold ----
	{
		TriangleMeshGeometryIndexed* pLathe = MakeCoarseLathe( 24 );
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( pLathe->TessellateToMesh( bt, bv, bn, bc, 0 ) );
		const Scalar diag = BBoxDiagonal( bv ), maxEdge = MaxEdgeLength( bt, bv );
		const unsigned int below = (unsigned int)std::floor( diag / maxEdge );	// target >= longest edge: NOT refined
		const unsigned int above = below + 1;										// target just under it: refined
		SinFunction2D* pFn = new SinFunction2D( 2.0 );

		IndexTriangleListType ta, tb; VerticesListType va, vb; NormalsListType na, nb; TexCoordsListType ca, cb;
		DisplacedGeometry* pBelow = new DisplacedGeometry( pLathe, below, pFn, 0.02, false, false );
		pBelow->Realize();
		assert( pBelow->TessellateToMesh( ta, va, na, ca, 0 ) );
		DisplacedGeometry* pAbove = new DisplacedGeometry( pLathe, above, pFn, 0.02, false, false );
		pAbove->Realize();
		assert( pAbove->TessellateToMesh( tb, vb, nb, cb, 0 ) );
		std::cout << "  [info] detail " << below << " -> " << ta.size() << " tris (unrefined), detail " << above << " -> " << tb.size() << " tris (refined)\n";
		assert( ta.size() == bt.size() );
		assert( tb.size() > ta.size() );

		// compare shading normals at the vertices both bakes share (the base's own vertices)
		Scalar worstDeg = 0.0; size_t matched = 0;
		for( size_t i = 0; i < va.size(); ++i ) {
			for( size_t j = 0; j < vb.size(); ++j ) {
				const Scalar dx = va[i].x - vb[j].x, dy = va[i].y - vb[j].y, dz = va[i].z - vb[j].z;
				if( dx*dx + dy*dy + dz*dz < 1e-18 ) {
					Scalar d = Vector3Ops::Dot( na[i], nb[j] );
					d = std::max( Scalar(-1.0), std::min( Scalar(1.0), d ) );
					worstDeg = std::max( worstDeg, std::acos( d ) * 180.0 / PI );
					++matched; break;
				}
			}
		}
		std::cout << "  [info] " << matched << " of " << va.size() << " base vertices matched; worst shading-normal difference across the threshold = " << worstDeg << " deg\n";
		assert( matched == va.size() );
		assert( worstDeg < 1.0 );		// measured 0.27 deg; 1.97 deg when transport applies only to refined bases
		pBelow->release(); pAbove->release(); pFn->release(); pLathe->release();
	}
}

//-----------------------------------------------------------------------------
// Round-2 P2-2: a base that shades with FACE normals (face_normals TRUE) is
// flattened per corner by its tessellation -- every edge is already a crack, each
// triangle displacing along its own face normal -- so there is no shared topology
// to refine: it must displace exactly as before (same triangle count at any
// `detail`), not have its cracks multiplied.
//-----------------------------------------------------------------------------
static void TestFaceNormalBaseIsNotRefined()
{
	std::cout << "Test 10i: a face-normal mesh base is not refined (refining would only multiply its cracks)...\n";
	TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, /*bUseFaceNormals=*/true );
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 1, -0.5, -0.5 ) ); m->AddVertex( Point3( 1, 0.5, -0.5 ) );
	m->AddVertex( Point3( 1, 0.5, 0.5 ) );   m->AddVertex( Point3( 1, -0.5, 0.5 ) );
	for( int i = 0; i < 4; ++i ) m->AddNormal( Vector3( 1, 0, 0 ) );
	m->AddTexCoord( Point2( 0, 0 ) ); m->AddTexCoord( Point2( 1, 0 ) ); m->AddTexCoord( Point2( 1, 1 ) ); m->AddTexCoord( Point2( 0, 1 ) );
	m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( 0, 1, 2 ) );
	m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( 0, 2, 3 ) );
	m->DoneIndexedTriangles();
	assert( m->UsesFaceNormals() );
	UVSpyFunction2D* pFn = new UVSpyFunction2D();
	DisplacedGeometry* pDisp = new DisplacedGeometry( m, 64, pFn, 0.1, false, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	std::cout << "  [info] face-normal quad at detail 64: " << t.size() << " triangles, " << v.size() << " vertices (unrefined: 2 / 6)\n";
	assert( t.size() == 2 );
	pDisp->release(); pFn->release(); m->release();
}

//-----------------------------------------------------------------------------
// Round-3 P1: `detail` on a mesh base builds REGULAR meshes.
//
// The first cut split edges adaptively (per-edge 1/2/3-way splits): 48.6% of the
// triangles of a refined dome lathe had a minimum angle under 20 degrees and
// 56% of its edges ran across the (u,v) diagonal (a regular grid has 1/3), so
// directional relief (ribs / flutes) rendered zig-zag and torn at 2.4x the
// triangles of a regular grid.  Now
//   (a) a lathe / sweep / skin subdivides its AUTHORED GRID (MeshGrid: regular
//       quads, positions on the authored polygon), and
//   (b) any other indexed mesh is subdivided UNIFORMLY (whole 1->4 levels, level
//       count from the base's MEDIAN edge length, so one outlier edge cannot
//       drive it).
// The oracle for (a) is an independently built regular-grid control: the same
// profile resampled by THIS test at the same spacing, with a radial count to match,
// baked by the plain lathe factory.
//-----------------------------------------------------------------------------
struct MeshQuality
{
	size_t tris; double fracMinAngleUnder20; double fracUVDiagonal;
};

static MeshQuality MeasureLatheQuality( const IndexTriangleListType& t, const VerticesListType& v, const TexCoordsListType& c )
{
	MeshQuality q; q.tris = t.size();
	size_t under20 = 0, counted = 0, edges = 0, diag = 0;
	for( size_t i = 0; i < t.size(); ++i ) {
		Scalar mn = 1e9;
		for( int k = 0; k < 3; ++k ) {
			const Point3& a = v[t[i].iVertices[k]]; const Point3& b = v[t[i].iVertices[( k + 1 ) % 3]]; const Point3& d = v[t[i].iVertices[( k + 2 ) % 3]];
			const Vector3 e1 = Vector3Ops::mkVector3( b, a ), e2 = Vector3Ops::mkVector3( d, a );
			const Scalar l1 = Vector3Ops::Magnitude( e1 ), l2 = Vector3Ops::Magnitude( e2 );
			if( !( l1 > 0 ) || !( l2 > 0 ) ) continue;
			Scalar cs = Vector3Ops::Dot( e1, e2 ) / ( l1 * l2 );
			cs = std::max( Scalar(-1), std::min( Scalar(1), cs ) );
			mn = std::min( mn, std::acos( cs ) * 180.0 / PI );
			// UV-diagonal: an edge whose chart step changes BOTH u and v (a regular grid has 1/3 of its edges so)
			const Point2& ca = c[t[i].iCoords[k]]; const Point2& cb = c[t[i].iCoords[( k + 1 ) % 3]];
			if( std::sqrt( a.x*a.x + a.z*a.z ) < 1e-6 || std::sqrt( b.x*b.x + b.z*b.z ) < 1e-6 ) continue;	// pole spokes
			Scalar du = cb.x - ca.x; du -= std::floor( du + 0.5 );
			++edges;
			if( std::fabs( du ) > 1e-9 && std::fabs( cb.y - ca.y ) > 1e-9 ) ++diag;
		}
		if( mn < 1e8 ) { ++counted; if( mn < 20.0 ) ++under20; }
	}
	q.fracMinAngleUnder20 = counted ? double( under20 ) / double( counted ) : 0.0;
	q.fracUVDiagonal = edges ? double( diag ) / double( edges ) : 0.0;
	return q;
}

static void TestRefinementIsRegular()
{
	std::cout << "Test 10j: a refined lathe is a REGULAR grid (triangle count and quality vs an independently built control)...\n";

	// the reviewer's dome: 4-point profile, 32 radial, detail 64
	static const double prof[] = { 0.0, 0.0,  1.2, 0.3,  1.2, 1.0,  0.0, 2.2 };
	const unsigned int detail = 64;
	LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = 32; ld.smooth = true;
	ITriangleMeshGeometryIndexed* pI = 0;
	assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
	TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
	assert( pLathe );
	IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
	assert( pLathe->TessellateToMesh( bt, bv, bn, bc, 0 ) );
	const Scalar target = BBoxDiagonal( bv ) / Scalar( detail );

	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, detail, pConst, 1e-4, false, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	const MeshQuality ours = MeasureLatheQuality( t, v, c );

	// CONTROL: the same profile resampled at the target spacing by this test, radial count to match
	std::vector<double> fine; fine.push_back( prof[0] ); fine.push_back( prof[1] );
	for( int s = 0; s < 3; ++s ) {
		const double ax = prof[2*s], ay = prof[2*s+1], bx = prof[2*s+2], by = prof[2*s+3];
		const int nn = std::max( 1, (int)std::ceil( std::hypot( bx - ax, by - ay ) / target ) );
		for( int i = 1; i <= nn; ++i ) { const double f = double( i ) / nn; fine.push_back( ax + ( bx - ax ) * f ); fine.push_back( ay + ( by - ay ) * f ); }
	}
	LatheDescriptor cd; cd.profilePoints = fine.data(); cd.numProfilePoints = (unsigned int)( fine.size() / 2 );
	cd.nRadial = (int)std::ceil( 2.0 * 3.14159265358979323846 * 1.2 / target ); cd.smooth = true;
	ITriangleMeshGeometryIndexed* pCI = 0;
	assert( RISE_API_CreateLatheGeometry( &pCI, cd ) );
	TriangleMeshGeometryIndexed* pCtl = dynamic_cast<TriangleMeshGeometryIndexed*>( pCI );
	IndexTriangleListType ct; VerticesListType cv; NormalsListType cn; TexCoordsListType cc;
	assert( pCtl->TessellateToMesh( ct, cv, cn, cc, 0 ) );
	const MeshQuality ctl = MeasureLatheQuality( ct, cv, cc );

	std::cout << "  [info] ours:    " << ours.tris << " tris, " << 100 * ours.fracMinAngleUnder20 << "% under 20 deg, " << 100 * ours.fracUVDiagonal << "% UV-diagonal edges\n";
	std::cout << "  [info] control: " << ctl.tris << " tris, " << 100 * ctl.fracMinAngleUnder20 << "% under 20 deg, " << 100 * ctl.fracUVDiagonal << "% UV-diagonal edges (a regular grid has 33%)\n";
	std::cout << "  [info] (the adaptive-split first cut: 37888 tris, 48.6% under 20 deg, 56.4% UV-diagonal)\n";
	assert( ours.tris <= 1.3 * double( ctl.tris ) );							// within ~1.3x of the regular-grid triangle count
	assert( ours.fracMinAngleUnder20 <= ctl.fracMinAngleUnder20 + 0.03 );		// no sliver excess over the regular grid
	assert( ours.fracUVDiagonal <= 0.40 );										// regular: 1/3 (adaptive splitting: 56%)

	pDisp->release(); pConst->release(); pCtl->release(); pLathe->release();

	// ---- (b) the UNIFORM path for a non-builder mesh -------------------------------
	{
		// right-isosceles quad: uniform 1->4 children are SIMILAR to their parent (all 45-45-90)
		TriangleMeshGeometryIndexed* pQuad = MakeUVQuad( 1.0 );
		UVSpyFunction2D* pFn = new UVSpyFunction2D();
		DisplacedGeometry* pQD = new DisplacedGeometry( pQuad, 8, pFn, 0.1, false, false, false );
		pQD->Realize();
		IndexTriangleListType qt; VerticesListType qv; NormalsListType qn; TexCoordsListType qc;
		assert( pQD->TessellateToMesh( qt, qv, qn, qc, 0 ) );
		Scalar worstMin = 90.0;
		for( size_t i = 0; i < qt.size(); ++i ) {
			// angles in the flat (undisplaced) plane: positions' y,z only (displacement is along x)
			const Point3 p[3] = { qv[qt[i].iVertices[0]], qv[qt[i].iVertices[1]], qv[qt[i].iVertices[2]] };
			for( int k = 0; k < 3; ++k ) {
				const Scalar e1y = p[(k+1)%3].y - p[k].y, e1z = p[(k+1)%3].z - p[k].z, e2y = p[(k+2)%3].y - p[k].y, e2z = p[(k+2)%3].z - p[k].z;
				const Scalar cs = ( e1y*e2y + e1z*e2z ) / ( std::sqrt( e1y*e1y + e1z*e1z ) * std::sqrt( e2y*e2y + e2z*e2z ) );
				worstMin = std::min( worstMin, std::acos( std::max( Scalar(-1), std::min( Scalar(1), cs ) ) ) * 180.0 / PI );
			}
		}
		std::cout << "  [info] uniform (b) path on a 2-triangle quad at detail 8: " << qt.size() << " triangles, smallest angle " << worstMin << " deg (parent 45)\n";
		assert( qt.size() >= 8 );
		assert( worstMin > 44.999 );
		pQD->release(); pFn->release(); pQuad->release();
	}
	{
		// MEDIAN policy: an 8x8 grid (0.25 cells) plus ONE triangle spanning the whole grid.  The longest edge is 2.0
		// but the median is 0.25 <= target (diag / 10), so nothing is refined; a max-driven level count would refine heavily.
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false );
		m->BeginIndexedTriangles();
		for( int j = 0; j <= 8; ++j ) for( int i = 0; i <= 8; ++i ) { m->AddVertex( Point3( 0.25 * i, 0.25 * j, 0 ) ); m->AddNormal( Vector3( 0, 0, 1 ) ); m->AddTexCoord( Point2( i / 8.0, j / 8.0 ) ); }
		auto id = [&]( int i, int j ) { return (unsigned int)( j * 9 + i ); };
		for( int j = 0; j < 8; ++j ) for( int i = 0; i < 8; ++i ) {
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i,j), id(i+1,j), id(i+1,j+1) ) );
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i,j), id(i+1,j+1), id(i,j+1) ) );
		}
		m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(0,0), id(8,0), id(0,8) ) );	// the outlier: edges of length 2
		m->DoneIndexedTriangles();
		UVSpyFunction2D* pFn = new UVSpyFunction2D();
		DisplacedGeometry* pMD = new DisplacedGeometry( m, 10, pFn, 0.05, false, false, false );
		pMD->Realize();
		IndexTriangleListType mt; VerticesListType mv; NormalsListType mn; TexCoordsListType mc;
		assert( pMD->TessellateToMesh( mt, mv, mn, mc, 0 ) );
		std::cout << "  [info] median policy: grid + one 2.0-long outlier triangle, detail 10: " << mt.size() << " triangles (base 129)\n";
		assert( mt.size() == 129 );
		pMD->release(); pFn->release(); m->release();
	}
}

//-----------------------------------------------------------------------------
// A LOFT (circle -> rect morph sweep) is refined on its authored grid; an earlier
// descriptor re-bake that refined the two profiles independently left the factory's
// union resample with near-coincident parameters -- razor-thin ring rows (min
// triangle area 4e-7 against 2e-4 in the base, black dashes in the render).
//-----------------------------------------------------------------------------
static void TestLoftRefineHasNoSlivers()
{
	std::cout << "Test 10m: a refined circle->rect loft has no razor-thin ring rows...\n";
	const int nCirc = 28;
	double circ[ 2 * 28 ];
	for( int i = 0; i < nCirc; ++i ) { const double a = 2.0 * 3.14159265358979323846 * i / nCirc; circ[2*i] = 0.52 * std::cos( a ); circ[2*i+1] = 0.52 * std::sin( a ); }
	static const double rect[] = { -0.65, -0.65,  0.65, -0.65,  0.65, 0.65,  -0.65, 0.65 };
	static const double path[] = { 0, 4.2, 0,  0, 2.8, 0,  0, 1.4, 0,  0, 0.0, 0 };
	SweepDescriptor sd; sd.profilePoints = circ; sd.numProfilePoints = nCirc; sd.profile2Points = rect; sd.numProfile2Points = 4;
	sd.pathPoints = path; sd.numPathPoints = 4; sd.nLen = 64; sd.capStart = false; sd.capEnd = false; sd.frameHintX = 1.0;
	ITriangleMeshGeometryIndexed* pI = 0;
	assert( RISE_API_CreateSweepGeometry( &pI, sd ) );
	TriangleMeshGeometryIndexed* pSweep = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
	assert( pSweep );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( pSweep, 48, pConst, 1e-4, false, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	std::vector<double> areas;
	for( size_t i = 0; i < t.size(); ++i ) {
		const Vector3 cr = Vector3Ops::Cross( Vector3Ops::mkVector3( v[t[i].iVertices[1]], v[t[i].iVertices[0]] ), Vector3Ops::mkVector3( v[t[i].iVertices[2]], v[t[i].iVertices[0]] ) );
		areas.push_back( 0.5 * Vector3Ops::Magnitude( cr ) );
	}
	std::sort( areas.begin(), areas.end() );
	const double minA = areas.front(), medA = areas[areas.size() / 2];
	std::cout << "  [info] refined loft: " << t.size() << " triangles, smallest area " << minA << ", median " << medA << " (ratio " << minA / medA << ")\n";
	assert( t.size() > 1000 );
	assert( minA > 0.02 * medA );
	pDisp->release(); pConst->release(); pSweep->release();
}

//-----------------------------------------------------------------------------
// A POLAR vertex (a lathe pole: its u is a placeholder) is displaced by the AVERAGE
// of its neighbours' displacements, not by the field at the placeholder -- the
// spokes around it displace by the field at their own coordinates, and a pole at
// f(placeholder) made a star of folds at the lid.  f(u,v) = u^2: the field at the
// placeholder u = 0.5 is 0.25, the ring's mean is ~1/3, so the two differ visibly.
//-----------------------------------------------------------------------------
class USquaredFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar x, const Scalar /*y*/ ) const { return x * x; }
};

static void TestPolarVertexDisplacesByNeighbourAverage()
{
	std::cout << "Test 10n: a pole is displaced by the average of its neighbours' displacements (no lid star)...\n";
	static const double prof[] = { 0.0, 0.0,  1.2, 0.3,  1.2, 1.0,  0.0, 2.2 };
	LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = 32; ld.smooth = true;
	ITriangleMeshGeometryIndexed* pI = 0;
	assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
	TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
	USquaredFunction2D* pFn = new USquaredFunction2D();
	const Scalar k = 0.3;
	DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 64, pFn, k, false, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	size_t pole = v.size();
	for( size_t i = 0; i < v.size(); ++i ) {
		if( std::fabs( v[i].x ) < 1e-9 && std::fabs( v[i].z ) < 1e-9 && v[i].y > 1.0 && ( pole == v.size() || v[i].y > v[pole].y ) ) pole = i;
	}
	assert( pole < v.size() );
	std::vector<size_t> nb;
	for( size_t i = 0; i < t.size(); ++i ) for( int j = 0; j < 3; ++j ) if( t[i].iVertices[j] == pole ) { nb.push_back( t[i].iVertices[( j + 1 ) % 3] ); nb.push_back( t[i].iVertices[( j + 2 ) % 3] ); }
	std::sort( nb.begin(), nb.end() ); nb.erase( std::unique( nb.begin(), nb.end() ), nb.end() );
	assert( nb.size() >= 32 );
	Scalar mean = 0.0;
	for( size_t j = 0; j < nb.size(); ++j ) mean += k * c[nb[j]].x * c[nb[j]].x;
	mean /= Scalar( nb.size() );
	const Scalar poleDisp = v[pole].y - 2.2;
	std::cout << "  [info] top pole displaced by " << poleDisp << "; mean of its " << nb.size() << " neighbours' displacements " << mean
		<< " (the field at the placeholder u = 0.5 would give " << k * 0.25 << ")\n";
	assert( std::fabs( poleDisp - mean ) < 1e-6 );
	pDisp->release(); pFn->release(); pLathe->release();
}

//-----------------------------------------------------------------------------
// Round-3 P2(1): a DISPLACED base is not refined again (its inner layer already
// honoured its own detail), so displaced-of-displaced over an analytic base cannot
// pick up the placeholder pole UVs of a sphere / ellipsoid / disk tessellation.
// (First cut: 418 of 11327 vertices of such a stack got a far-side u.)
//-----------------------------------------------------------------------------
static void TestDisplacedBaseIsNotRefinedAgain()
{
	std::cout << "Test 10k: displaced-of-displaced over a sphere keeps the inner layer's mesh and UVs (inner detail governs)...\n";
	SphereGeometry* pSphere = new SphereGeometry( 1.0 );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pInner = new DisplacedGeometry( pSphere, 16, pConst, 0.05, false, false );
	pInner->Realize();
	DisplacedGeometry* pOuter = new DisplacedGeometry( pInner, 64, pConst, 0.05, false, false );
	pOuter->Realize();
	IndexTriangleListType it, ot; VerticesListType iv, ov; NormalsListType in_, on; TexCoordsListType ic, oc;
	assert( pInner->TessellateToMesh( it, iv, in_, ic, 0 ) );
	assert( pOuter->TessellateToMesh( ot, ov, on, oc, 0 ) );
	size_t bad = 0;
	for( size_t i = 0; i < ov.size(); ++i ) {
		Scalar best = 1e30; size_t bj = 0;
		for( size_t j = 0; j < iv.size(); ++j ) {
			const Scalar dx = ov[i].x - iv[j].x, dy = ov[i].y - iv[j].y, dz = ov[i].z - iv[j].z;
			const Scalar dd = dx*dx + dy*dy + dz*dz;
			if( dd < best ) { best = dd; bj = j; }
		}
		if( CircDist( oc[i].x, ic[bj].x ) > 0.15 ) ++bad;
	}
	std::cout << "  [info] inner " << it.size() << " tris / " << iv.size() << " verts, outer (detail 64) " << ot.size() << " tris / " << ov.size() << " verts; "
		<< bad << " outer vertices with u more than 0.15 turn from the nearest inner vertex\n";
	assert( ot.size() == it.size() && ov.size() == iv.size() );
	assert( bad == 0 );
	pOuter->release(); pInner->release(); pConst->release(); pSphere->release();
}

//-----------------------------------------------------------------------------
// Round-3 P2(3): the shading-normal TRANSPORT itself, not the fallback.
// A flat grid whose base shading normals are tilted 30 degrees off the geometric
// normal, displaced by a smooth field: transport rotates the shading normal with the
// geometric normal, so at EVERY vertex the angle between the displaced shading and
// geometric normals is still exactly 30 degrees, and the shading normal equals the
// independently computed rotation R(g0 -> g1) s0.  A world-space delta (the first
// cut) or any wrong rotation fails both.
//-----------------------------------------------------------------------------
static void TestShadingTransportIsRotation()
{
	std::cout << "Test 10l: the base shading normal is ROTATED with the displaced geometric normal (angle to g preserved, equals R(g0->g1) s0)...\n";
	const int N = 8;
	const Scalar tilt = 30.0 * PI / 180.0;
	const Vector3 s0( std::sin( tilt ), 0.0, std::cos( tilt ) ), g0( 0, 0, 1 );
	TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, false );
	m->BeginIndexedTriangles();
	for( int j = 0; j <= N; ++j ) for( int i = 0; i <= N; ++i ) {
		m->AddVertex( Point3( 2.0 * i / N - 1.0, 2.0 * j / N - 1.0, 0.0 ) );
		m->AddNormal( s0 );
		m->AddTexCoord( Point2( Scalar( i ) / N, Scalar( j ) / N ) );
	}
	auto id = [&]( int i, int j ) { return (unsigned int)( j * ( N + 1 ) + i ); };
	for( int j = 0; j < N; ++j ) for( int i = 0; i < N; ++i ) {
		if( ( i + j ) & 1 ) {
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i,j), id(i+1,j), id(i+1,j+1) ) );
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i,j), id(i+1,j+1), id(i,j+1) ) );
		} else {
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i,j), id(i+1,j), id(i,j+1) ) );
			m->AddIndexedTriangle( MakeIndexedTriangleSameIdx( id(i+1,j), id(i+1,j+1), id(i,j+1) ) );
		}
	}
	m->DoneIndexedTriangles();
	SinFunction2D* pFn = new SinFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( m, 1, pFn, 0.25, false, false, false );	// detail 1: no refinement
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	assert( t.size() == (size_t)( 2 * N * N ) );
	NormalsListType g1( v.size(), Vector3( 0, 0, 1 ) );
	RecomputeVertexNormalsFromTopology( t, v, g1 );

	auto rotate = []( const Vector3& a, const Vector3& b, const Vector3& x ) {		// independent Rodrigues
		const Vector3 ax = Vector3Ops::Cross( a, b );
		const Scalar s = Vector3Ops::Magnitude( ax ), cth = Vector3Ops::Dot( a, b );
		if( s < 1e-12 ) return x;
		const Vector3 k = ax * ( 1.0 / s );
		return x * cth + Vector3Ops::Cross( k, x ) * s + k * ( Vector3Ops::Dot( k, x ) * ( 1.0 - cth ) );
	};
	Scalar worstAngle = 0.0, worstVec = 0.0, maxTilt = 0.0;
	for( size_t i = 0; i < v.size(); ++i ) {
		const Scalar ang = std::acos( std::max( Scalar(-1), std::min( Scalar(1), Vector3Ops::Dot( n[i], g1[i] ) ) ) );
		worstAngle = std::max( worstAngle, std::fabs( ang - tilt ) * 180.0 / PI );
		const Vector3 want = rotate( g0, g1[i], s0 );
		worstVec = std::max( worstVec, Vector3Ops::Magnitude( n[i] - want ) );
		maxTilt = std::max( maxTilt, std::acos( std::max( Scalar(-1), std::min( Scalar(1), g1[i].z ) ) ) * 180.0 / PI );
	}
	std::cout << "  [info] displaced geometric normals tilt up to " << maxTilt << " deg off +Z; worst |angle(s1,g1) - 30 deg| = " << worstAngle
		<< " deg, worst |s1 - R(g0->g1) s0| = " << worstVec << "\n";
	assert( maxTilt > 10.0 );			// the relief really moves the geometric normal
	assert( worstAngle < 1e-6 );
	assert( worstVec < 1e-9 );
	pDisp->release(); pFn->release(); m->release();
}

//-----------------------------------------------------------------------------
// Round-4: REFINEMENT NEVER CHANGES THE AUTHORED SURFACE.
//
// The round-3 re-bake re-ran each builder's factory on a denser descriptor, which is
// a DIFFERENT surface: rows inserted on a straight profile segment got that
// segment's flat normal (a smooth lathe lost its interpolated shading: a 22 degree
// jump on the stress vase), a smooth=FALSE lathe split a crease at every inserted
// row (stair-step banding), a loft re-ran its twist alignment and centring (leg
// vertices 0.017 off the authored surface), and an authored hexagonal column turned
// round.  Refinement now subdivides the AUTHORED GRID (MeshGrid): every new vertex lies
// on the authored piecewise-linear surface, takes its normal and UV barycentrically
// from the authored vertices, and creases exist only where the author made them.
//
// The oracle is the authored (coarse) mesh itself: for every refined vertex of a
// displacement-free (disp_scale 1e-12) bake, (a) its distance to the nearest authored
// triangle is ~0 and (b) its shading normal equals the authored mesh's interpolated
// normal at that point.  (Where authored triangles coincide with different normals -- a
// cap rim against the wall -- the best-matching one counts.)
//-----------------------------------------------------------------------------
static void ClosestPointOnTriangle( const Point3& p, const Point3& a, const Point3& b, const Point3& c, Scalar& u, Scalar& v, Scalar& w, Point3& out )
{
	const Vector3 ab = Vector3Ops::mkVector3( b, a ), ac = Vector3Ops::mkVector3( c, a ), ap = Vector3Ops::mkVector3( p, a );
	const Scalar d1 = Vector3Ops::Dot( ab, ap ), d2 = Vector3Ops::Dot( ac, ap );
	if( d1 <= 0 && d2 <= 0 ) { u = 1; v = 0; w = 0; out = a; return; }
	const Vector3 bp = Vector3Ops::mkVector3( p, b );
	const Scalar d3 = Vector3Ops::Dot( ab, bp ), d4 = Vector3Ops::Dot( ac, bp );
	if( d3 >= 0 && d4 <= d3 ) { u = 0; v = 1; w = 0; out = b; return; }
	const Scalar vc = d1 * d4 - d3 * d2;
	if( vc <= 0 && d1 >= 0 && d3 <= 0 ) { const Scalar t = d1 / ( d1 - d3 ); u = 1 - t; v = t; w = 0; out = Point3( a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t ); return; }
	const Vector3 cp = Vector3Ops::mkVector3( p, c );
	const Scalar d5 = Vector3Ops::Dot( ab, cp ), d6 = Vector3Ops::Dot( ac, cp );
	if( d6 >= 0 && d5 <= d6 ) { u = 0; v = 0; w = 1; out = c; return; }
	const Scalar vb = d5 * d2 - d1 * d6;
	if( vb <= 0 && d2 >= 0 && d6 <= 0 ) { const Scalar t = d2 / ( d2 - d6 ); u = 1 - t; v = 0; w = t; out = Point3( a.x + ac.x * t, a.y + ac.y * t, a.z + ac.z * t ); return; }
	const Scalar va = d3 * d6 - d5 * d4;
	if( va <= 0 && ( d4 - d3 ) >= 0 && ( d5 - d6 ) >= 0 ) { const Scalar t = ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ); u = 0; v = 1 - t; w = t; out = Point3( b.x + ( c.x - b.x ) * t, b.y + ( c.y - b.y ) * t, b.z + ( c.z - b.z ) * t ); return; }
	const Scalar den = 1.0 / ( va + vb + vc ); v = vb * den; w = vc * den; u = 1.0 - v - w;
	out = Point3( a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w );
}

struct SurfaceCheck { size_t baseTris, outTris, outVerts; Scalar maxDist, maxAngleDeg; };

static SurfaceCheck CheckRefinementOnAuthoredSurface( TriangleMeshGeometryIndexed* pBase, const unsigned int detail )
{
	IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
	assert( pBase->TessellateToMesh( bt, bv, bn, bc, 0 ) );
	const Scalar diag = BBoxDiagonal( bv );
	ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
	DisplacedGeometry* pDisp = new DisplacedGeometry( pBase, detail, pConst, 1e-12, false, false, false );
	pDisp->Realize();
	IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
	assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
	SurfaceCheck r; r.baseTris = bt.size(); r.outTris = t.size(); r.outVerts = v.size(); r.maxDist = 0; r.maxAngleDeg = 0;
	for( size_t i = 0; i < v.size(); ++i ) {
		Scalar bestDist = 1e30, bestAngle = 1e30;
		for( size_t k = 0; k < bt.size(); ++k ) {
			const unsigned int i0 = bt[k].iVertices[0], i1 = bt[k].iVertices[1], i2 = bt[k].iVertices[2];
			Scalar u, vv, w; Point3 cp;
			ClosestPointOnTriangle( v[i], bv[i0], bv[i1], bv[i2], u, vv, w, cp );
			const Scalar d = std::sqrt( ( cp.x - v[i].x )*( cp.x - v[i].x ) + ( cp.y - v[i].y )*( cp.y - v[i].y ) + ( cp.z - v[i].z )*( cp.z - v[i].z ) );
			if( d > bestDist + 1e-9 * diag ) continue;
			Vector3 nm = bn[bt[k].iNormals[0]] * u + bn[bt[k].iNormals[1]] * vv + bn[bt[k].iNormals[2]] * w;
			const Scalar nl = Vector3Ops::Magnitude( nm );
			Scalar ang = 180.0;
			if( nl > 1e-12 ) ang = std::acos( std::max( Scalar(-1), std::min( Scalar(1), Vector3Ops::Dot( nm * ( 1.0 / nl ), n[i] ) ) ) ) * 180.0 / PI;
			if( d < bestDist - 1e-9 * diag ) { bestDist = d; bestAngle = ang; }
			else { bestDist = std::min( bestDist, d ); bestAngle = std::min( bestAngle, ang ); }
		}
		r.maxDist = std::max( r.maxDist, bestDist ); r.maxAngleDeg = std::max( r.maxAngleDeg, bestAngle );
	}
	r.maxDist /= diag;
	pDisp->release(); pConst->release();
	return r;
}

static void TestRefinementKeepsAuthoredSurface()
{
	std::cout << "Test 10o: refined vertices lie on the AUTHORED surface and shade with its interpolated normals (lathe, sweep, loft, skin)...\n";
	int softFails = 0;
#define SOFT( cond ) do { if( !( cond ) ) { ++softFails; std::cout << "  [FAIL] " #cond "\n"; } } while( 0 )
	auto report = [&]( const char* name, const SurfaceCheck& r ) {
		std::cout << "  [info] " << name << ": " << r.baseTris << " -> " << r.outTris << " tris; worst distance to the authored mesh " << r.maxDist
			<< " x diagonal, worst shading-normal difference " << r.maxAngleDeg << " deg\n";
		SOFT( r.outTris > r.baseTris );
		SOFT( r.maxDist <= 1e-9 );
		SOFT( r.maxAngleDeg <= 1.0 );
	};

	{	// smooth TRUE lathe dome (the stress vase's character)
		static const double prof[] = { 0.0, 0.0,  1.2, 0.3,  1.2, 1.0,  0.0, 2.2 };
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = 32; ld.smooth = true;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "smooth lathe dome", CheckRefinementOnAuthoredSurface( p, 64 ) );
		p->release();
	}
	{	// smooth FALSE lathe (authored creases at every profile point)
		static const double prof[] = { 0.0, 0.0,  1.0, 0.25,  1.0, 1.75,  0.0, 2.0 };
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = 24; ld.smooth = false;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "faceted (smooth FALSE) lathe", CheckRefinementOnAuthoredSurface( p, 48 ) );
		p->release();
	}
	{	// a profile_circle-10 sweep (a 10-gon column with smooth corner normals) and caps
		double circ[ 2 * 10 ];
		for( int i = 0; i < 10; ++i ) { const double a = 2.0 * 3.14159265358979323846 * i / 10; circ[2*i] = 0.9 * std::cos( a ); circ[2*i+1] = 0.9 * std::sin( a ); }
		static const double path[] = { 0, 0, 0,  0, 1.5, 0,  0, 3.0, 0 };
		SweepDescriptor sd; sd.profilePoints = circ; sd.numProfilePoints = 10; sd.pathPoints = path; sd.numPathPoints = 3; sd.nLen = 4; sd.frameHintX = 1.0;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateSweepGeometry( &pI, sd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "profile_circle-10 sweep (capped)", CheckRefinementOnAuthoredSurface( p, 48 ) );
		p->release();
	}
	{	// a loft: circle -> square, twist alignment and centring authored
		const int nCirc = 28; double circ[ 2 * 28 ];
		for( int i = 0; i < nCirc; ++i ) { const double a = 2.0 * 3.14159265358979323846 * i / nCirc; circ[2*i] = 0.52 * std::cos( a ); circ[2*i+1] = 0.52 * std::sin( a ); }
		static const double rect[] = { -0.65, -0.65,  0.65, -0.65,  0.65, 0.65,  -0.65, 0.65 };
		static const double path[] = { 0, 4.2, 0,  0, 2.8, 0,  0, 1.4, 0,  0, 0.0, 0 };
		SweepDescriptor sd; sd.profilePoints = circ; sd.numProfilePoints = nCirc; sd.profile2Points = rect; sd.numProfile2Points = 4;
		sd.pathPoints = path; sd.numPathPoints = 4; sd.nLen = 16; sd.frameHintX = 1.0;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateSweepGeometry( &pI, sd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "circle->square loft", CheckRefinementOnAuthoredSurface( p, 48 ) );
		p->release();
	}
	{	// a billowed skin
		const double railA[] = { -1.6, 0.0, 0.0,   -0.5, 0.45, 0.0,   0.6, 1.1, 0.0,   1.6, 2.0, 0.0 };
		const double railB[] = { -1.6, 0.4, -1.9,   0.0, 1.5, -1.9,   1.6, 2.6, -1.9 };
		SkinDescriptor kd; kd.railAPoints = railA; kd.numRailAPoints = 4; kd.railBPoints = railB; kd.numRailBPoints = 3;
		kd.nLen = 6; kd.nAcross = 3; kd.billow = 0.28;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateSkinGeometry( &pI, kd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "billowed skin", CheckRefinementOnAuthoredSurface( p, 48 ) );
		p->release();
	}
	{	// a skin whose rails meet at both ends (pinch fans)
		const double railA[] = { 0, 0, 0,   1, 0.4, 0,   2, 0, 0 };
		const double railB[] = { 0, 0, 0,   1, 1.2, -0.6,   2, 0, 0 };
		SkinDescriptor kd; kd.railAPoints = railA; kd.numRailAPoints = 3; kd.railBPoints = railB; kd.numRailBPoints = 3;
		kd.nLen = 6; kd.nAcross = 3; kd.billow = 0.0;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateSkinGeometry( &pI, kd ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		report( "pinched skin", CheckRefinementOnAuthoredSurface( p, 48 ) );
		p->release();
	}
	assert( softFails == 0 );
#undef SOFT
}

//-----------------------------------------------------------------------------
// Round-4 P1-2 / P2: crease rows exist only at AUTHORED profile points, and an
// authored coarse polygon stays a polygon.
//  * smooth=FALSE lathe: the refined mesh has a duplicated (split) ring only at the
//    authored profile points where two segments meet -- the first cut split a crease at
//    EVERY inserted row (stair-step banding).
//  * an authored n_radial 6 column stays a hexagon (the first cut raised the radial count
//    on the exact circle and turned it round).
//-----------------------------------------------------------------------------
static void TestCreasesOnlyWhereAuthoredAndPolygonsStayPolygons()
{
	std::cout << "Test 10p: crease rows only at authored profile points; a hexagonal column stays hexagonal...\n";
	{
		static const double prof[] = { 0.0, 0.0,  1.0, 0.25,  1.0, 1.75,  0.0, 2.0 };
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 4; ld.nRadial = 24; ld.smooth = false;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( p, 48, pConst, 1e-12, false, false, false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		// vertices on the angle-0 column (x = 0, z > 0), grouped by height
		std::vector<Scalar> ys;
		for( size_t i = 0; i < v.size(); ++i ) if( std::fabs( v[i].x ) < 1e-9 && v[i].z > 1e-6 ) ys.push_back( std::floor( v[i].y * 1e7 + 0.5 ) / 1e7 );
		std::sort( ys.begin(), ys.end() );
		size_t splitRows = 0, rows = 0;
		for( size_t i = 0; i < ys.size(); ) {
			size_t j = i; while( j < ys.size() && ys[j] == ys[i] ) ++j;
			++rows; if( j - i > 1 ) ++splitRows;
			i = j;
		}
		std::cout << "  [info] faceted lathe refined: " << rows << " distinct rows on the angle-0 column, " << splitRows << " of them split (authored creases: 2)\n";
		assert( rows > 20 );
		assert( splitRows == 2 );
		pDisp->release(); pConst->release(); p->release();
	}
	{
		static const double prof[] = { 1.0, 0.0,  1.0, 2.0 };
		LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 2; ld.nRadial = 6; ld.smooth = true;
		ITriangleMeshGeometryIndexed* pI = 0; assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
		TriangleMeshGeometryIndexed* p = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( p, 48, pConst, 1e-12, false, false, false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		Scalar rMin = 1e9, rMax = 0;
		for( size_t i = 0; i < v.size(); ++i ) { const Scalar r = std::sqrt( v[i].x*v[i].x + v[i].z*v[i].z ); rMin = std::min( rMin, r ); rMax = std::max( rMax, r ); }
		std::cout << "  [info] hexagonal column refined to " << t.size() << " triangles: radius from the axis spans [" << rMin << ", " << rMax << "] (hexagon: 0.866 at an edge middle, 1 at a corner; a circle: 1)\n";
		assert( t.size() > 100 );
		assert( rMin < 0.87 && rMax < 1.0 + 1e-9 );
		pDisp->release(); pConst->release(); p->release();
	}
}

//-----------------------------------------------------------------------------
// Round-5: refinement is crack-free (no T-junctions) and displacement is WATERTIGHT at
// authored creases, over EVERY builder variant.
//
//  P1 (T-junctions): a skin with a duplicated rail point and a nonzero billow gives the
//  coincident crease stations different normals, so the interior rows separate and the
//  zero-area test eats ONE triangle of a rail-row quad; the grid recorder left that
//  survivor unrefined while its index-sharing neighbours were subdivided -- real
//  T-junctions.  An end cap's rim (duplicate vertices) had the same problem.  The
//  detector below is the invariant: after refinement, NO vertex lies in the interior of
//  an edge that exactly one triangle uses.
//  P2 (watertight): vertices coincident in the base (cap rim vs wall rim, a duplicated
//  profile point, a lathe pinch's two poles, a skin crease station) must end at the SAME
//  displaced position.
//-----------------------------------------------------------------------------
struct CreaseFixture { const char* name; TriangleMeshGeometryIndexed* mesh; bool expectCoincident; };

static void BuildCreaseFixtures( std::vector<CreaseFixture>& out )
{
	auto add = [&]( const char* n, ITriangleMeshGeometryIndexed* p, const bool coincident ) {
		assert( p );
		TriangleMeshGeometryIndexed* m = dynamic_cast<TriangleMeshGeometryIndexed*>( p );
		assert( m );
		CreaseFixture f = { n, m, coincident };
		out.push_back( f );
	};
	const double vase[]   = { 0,0, 1.05,0.15, 1.26,0.9, 0.54,2.16, 0.66,2.7, 0,2.82 };
	const double open_[]  = { 1.0,0, 1.2,1.0, 0.8,2.0 };
	const double hour[]   = { 0.8,0, 0,1.0, 0.8,2.0 };
	const double crease[] = { 0.5,0, 1.0,1.0, 1.0,1.0, 0.6,2.0 };
	struct LV { const char* n; const double* p; int np; bool sm; double sw; bool coincident; };
	const LV lv[] = {
		{ "lathe vase smooth 360", vase, 6, true, 360, false }, { "lathe vase faceted 360", vase, 6, false, 360, true },
		{ "lathe vase smooth 200 (capped)", vase, 6, true, 200, true }, { "lathe open tube 120 (capped)", open_, 3, true, 120, true },
		{ "lathe hourglass (interior pinch) 360", hour, 3, true, 360, true }, { "lathe hourglass 90 (capped)", hour, 3, true, 90, true },
		{ "lathe dup-point crease 360", crease, 4, true, 360, true }, { "lathe dup-point crease 180 (capped)", crease, 4, true, 180, true } };
	for( size_t i = 0; i < sizeof( lv ) / sizeof( lv[0] ); ++i ) {
		LatheDescriptor d; d.profilePoints = lv[i].p; d.numProfilePoints = lv[i].np; d.nRadial = 12; d.smooth = lv[i].sm; d.sweepDegrees = lv[i].sw; d.axis = 1;
		ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateLatheGeometry( &p, d ) );
		add( lv[i].n, p, lv[i].coincident );
	}
	const double sq[]   = { -0.5,-0.5, 0.5,-0.5, 0.5,0.5, -0.5,0.5 };
	const double tri[]  = { -0.6,-0.4, 0.9,-0.4, -0.6,0.7 };
	double circ[24]; for( int i = 0; i < 12; ++i ) { const double a = 2 * PI * i / 12; circ[2*i] = 0.5 * std::cos( a ); circ[2*i+1] = 0.5 * std::sin( a ); }
	const double path[] = { 0,0,0,  0,1,0,  0.3,2,0.2 };
	const double loop[] = { 0,0,0,  2,0,0,  2,0,2,  0,0,2 };
	const double sc[] = { 1,0.4,1.3 }, scy[] = { 1,1.5,0.6 }, mo[] = { 0,0.5,1 };
	SweepDescriptor d; d.profilePoints = sq; d.numProfilePoints = 4; d.pathPoints = path; d.numPathPoints = 3; d.nLen = 6;
	d.endScaleX = 1; d.endScaleY = 1; d.capStart = true; d.capEnd = true; d.frameHintX = 1;
	{ ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, d ) ); add( "sweep square capped", p, true ); }
	{ SweepDescriptor e = d; e.capStart = e.capEnd = false; ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, e ) ); add( "sweep square uncapped", p, false ); }
	{ SweepDescriptor f = d; f.profilePoints = circ; f.numProfilePoints = 12; f.profile2Points = tri; f.numProfile2Points = 3; f.pointMorphs = mo; f.numPointMorphs = 3;
	  ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, f ) ); add( "sweep loft circle->triangle capped", p, true ); }
	{ SweepDescriptor g = d; g.pointScales = sc; g.numPointScales = 3; g.pointScalesY = scy; g.numPointScalesY = 3;
	  ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, g ) ); add( "sweep point_scale sx sy capped", p, true ); }
	{ SweepDescriptor h = d; h.pathPoints = loop; h.numPathPoints = 4; h.pathClosed = true; h.capStart = h.capEnd = false; h.nLen = 12;
	  ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, h ) ); add( "sweep closed path", p, false ); }
	{ SweepDescriptor k = d; k.endScaleX = 0.2; k.endScaleY = 0.5;
	  ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSweepGeometry( &p, k ) ); add( "sweep taper capped", p, true ); }

	const double A[]  = { -1.6,0,0, -0.5,0.45,0, 0.6,1.1,0, 1.6,2.0,0 },  Bv[] = { -1.6,0.4,-1.9, 0,1.5,-1.9, 1.6,2.6,-1.9 };
	const double Ap[] = { -1.6,0,0, 0,0.5,0, 1.6,2.0,-1 },                Bp[] = { -1.6,0,-2, 0,1.0,-2, 1.6,2.0,-1 };
	const double Ai[] = { -1.6,0,0, 0,0.5,-1, 1.6,2.0,0 },                Bi[] = { -1.6,0,-2, 0,0.5,-1, 1.6,2.0,-2 };
	const double Ac[] = { -1.6,0,0, 0,0.5,0, 0,0.5,0, 1.6,2.0,0 },        Bc[] = { -1.6,0,-2, 0,0.6,-2, 0,0.6,-2, 1.6,2.0,-2 };
	const double Ac1[] = { -1.6,0,0, 0,0.5,0, 0,0.5,0, 1.6,2.0,0 },       Bc1[] = { -1.6,0,-2, 1.6,2.0,-2 };
	struct SV { const char* n; const double* a; int na; const double* b; int nb; int across; double billow; bool coincident; };
	const SV sv[] = {
		{ "skin sail billow", A, 4, Bv, 3, 16, 0.28, false }, { "skin sail flat n_across 2", A, 4, Bv, 3, 2, 0.0, false },
		{ "skin end pinch", Ap, 3, Bp, 3, 6, 0.2, false }, { "skin interior pinch", Ai, 3, Bi, 3, 6, 0.2, true },
		{ "skin dup-point crease both rails", Ac, 4, Bc, 4, 6, 0.1, true },
		{ "skin dup-point ONE rail + billow 0.1 (reviewer P1)", Ac1, 4, Bc1, 2, 6, 0.1, true },
		{ "skin dup-point ONE rail, billow 0", Ac1, 4, Bc1, 2, 6, 0.0, true }, { "skin dup-point crease both rails, billow 0", Ac, 4, Bc, 4, 6, 0.0, true },
		{ "skin dup-point ONE rail + billow 0.28", Ac1, 4, Bc1, 2, 6, 0.28, true } };
	for( size_t i = 0; i < sizeof( sv ) / sizeof( sv[0] ); ++i ) {
		SkinDescriptor s; s.railAPoints = sv[i].a; s.numRailAPoints = sv[i].na; s.railBPoints = sv[i].b; s.numRailBPoints = sv[i].nb;
		s.nLen = 8; s.nAcross = sv[i].across; s.billow = sv[i].billow;
		ITriangleMeshGeometryIndexed* p = 0; assert( RISE_API_CreateSkinGeometry( &p, s ) );
		add( sv[i].n, p, sv[i].coincident );
	}
}

// Number of vertices lying strictly inside an edge exactly ONE triangle uses (a T-junction),
// and the longest such edge.  Coincident duplicates at an edge's ENDPOINTS do not count.
static size_t g_dbgAuthoredVerts = 0;
static size_t CountTJunctions( const IndexTriangleListType& t, const VerticesListType& v, Scalar* pLongestEdge )
{
	std::map<std::pair<unsigned int, unsigned int>, int> uses;
	for( size_t i = 0; i < t.size(); ++i ) {
		for( int k = 0; k < 3; ++k ) {
			const unsigned int a = t[i].iVertices[k], b = t[i].iVertices[( k + 1 ) % 3];
			if( a != b ) ++uses[ std::make_pair( std::min( a, b ), std::max( a, b ) ) ];
		}
	}
	const Scalar diag = BBoxDiagonal( v );
	const Scalar cell = diag / 32.0, eps = 1e-9 * diag;
	auto key = [&]( const long x, const long y, const long z ) { return ( std::uint64_t( x + 1024 ) << 42 ) ^ ( std::uint64_t( y + 1024 ) << 21 ) ^ std::uint64_t( z + 1024 ); };
	std::vector<char> used( v.size(), 0 );
	for( size_t i = 0; i < t.size(); ++i ) for( int k = 0; k < 3; ++k ) used[ t[i].iVertices[k] ] = 1;
	std::unordered_map<std::uint64_t, std::vector<unsigned int> > hash;
	for( unsigned int i = 0; i < v.size(); ++i ) if( used[i] ) hash[ key( long( std::floor( v[i].x / cell ) ), long( std::floor( v[i].y / cell ) ), long( std::floor( v[i].z / cell ) ) ) ].push_back( i );
	size_t found = 0; Scalar longest = 0;
	std::set<unsigned int> hits;
	for( std::map<std::pair<unsigned int, unsigned int>, int>::const_iterator e = uses.begin(); e != uses.end(); ++e ) {
		if( e->second != 1 ) continue;
		const Point3& a = v[e->first.first]; const Point3& b = v[e->first.second];
		const Vector3 ab = Vector3Ops::mkVector3( b, a );
		const Scalar L2 = Vector3Ops::Dot( ab, ab );
		if( L2 <= 0 ) continue;
		const int steps = int( std::sqrt( L2 ) / ( 0.5 * cell ) ) + 2;
		hits.clear();
		for( int s = 0; s <= steps; ++s ) {
			const Scalar f = Scalar( s ) / Scalar( steps );
			const long cx = long( std::floor( ( a.x + ab.x * f ) / cell ) ), cy = long( std::floor( ( a.y + ab.y * f ) / cell ) ), cz = long( std::floor( ( a.z + ab.z * f ) / cell ) );
			for( long dx = -1; dx <= 1; ++dx ) for( long dy = -1; dy <= 1; ++dy ) for( long dz = -1; dz <= 1; ++dz ) {
				const std::unordered_map<std::uint64_t, std::vector<unsigned int> >::const_iterator it = hash.find( key( cx + dx, cy + dy, cz + dz ) );
				if( it == hash.end() ) continue;
				for( size_t m = 0; m < it->second.size(); ++m ) hits.insert( it->second[m] );
			}
		}
		for( std::set<unsigned int>::const_iterator h = hits.begin(); h != hits.end(); ++h ) {
			if( *h == e->first.first || *h == e->first.second ) continue;
			const Vector3 ap = Vector3Ops::mkVector3( v[*h], a );
			const Scalar s = Vector3Ops::Dot( ap, ab ) / L2;
			if( s < 1e-6 || s > 1 - 1e-6 ) continue;
			const Vector3 d( ap.x - ab.x * s, ap.y - ab.y * s, ap.z - ab.z * s );
			if( Vector3Ops::Magnitude( d ) > eps ) continue;
			++found; longest = std::max( longest, std::sqrt( L2 ) );
			if( std::getenv( "RISE_TJ_DEBUG" ) ) {
				std::cout << "    T-junction: vertex " << *h << " (" << v[*h].x << ", " << v[*h].y << ", " << v[*h].z << ") inside edge " << e->first.first << "-" << e->first.second
					<< " (" << a.x << ", " << a.y << ", " << a.z << ")-(" << b.x << ", " << b.y << ", " << b.z << ") at s=" << s << " [authoredVerts=" << g_dbgAuthoredVerts << "]\n";
			}
		}
	}
	if( pLongestEdge ) *pLongestEdge = longest;
	return found;
}

static void TestRefinementHasNoTJunctions()
{
	std::cout << "Test 10q: refinement leaves NO T-junction (a vertex strictly inside a single-use edge) on any builder variant...\n";
	std::vector<CreaseFixture> fx; BuildCreaseFixtures( fx );
	int softFails = 0;
	for( size_t i = 0; i < fx.size(); ++i ) {
		IndexTriangleListType bt; VerticesListType bv; NormalsListType bn; TexCoordsListType bc;
		assert( fx[i].mesh->TessellateToMesh( bt, bv, bn, bc, 0 ) );
		Scalar e0 = 0, e1 = 0;
		const size_t authored = CountTJunctions( bt, bv, &e0 );
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( fx[i].mesh, 40, pConst, 1e-12, false, false, false );
		pDisp->Realize();
		IndexTriangleListType t; VerticesListType v; NormalsListType n; TexCoordsListType c;
		assert( pDisp->TessellateToMesh( t, v, n, c, 0 ) );
		g_dbgAuthoredVerts = bv.size();
		const size_t refined = CountTJunctions( t, v, &e1 );
		std::cout << "  [info] " << fx[i].name << ": " << bt.size() << " -> " << t.size() << " tris; T-junction vertices authored " << authored << ", refined " << refined << "\n";
		if( refined != 0 ) { ++softFails; std::cout << "  [FAIL] " << fx[i].name << ": " << refined << " T-junction vertices after refinement (longest edge " << e1 << ")\n"; }
		if( t.size() <= bt.size() ) { ++softFails; std::cout << "  [FAIL] " << fx[i].name << ": not refined\n"; }
		pDisp->release(); pConst->release();
	}
	for( size_t i = 0; i < fx.size(); ++i ) fx[i].mesh->release();
	assert( softFails == 0 );
}

static void TestDisplacementIsWatertightAtCreases()
{
	std::cout << "Test 10r: displacement is WATERTIGHT at authored creases (cap rims, duplicated profile points, pinch poles, crease stations)...\n";
	std::vector<CreaseFixture> fx; BuildCreaseFixtures( fx );
	int softFails = 0;
	for( size_t i = 0; i < fx.size(); ++i ) {
		IndexTriangleListType t0; VerticesListType v0; NormalsListType n0; TexCoordsListType c0;
		IndexTriangleListType t1; VerticesListType v1; NormalsListType n1; TexCoordsListType c1;
		ConstFunction2D* pConst = new ConstFunction2D( 1.0 );
		DisplacedGeometry* pFlat = new DisplacedGeometry( fx[i].mesh, 40, pConst, 1e-12, false, false, false );
		pFlat->Realize(); assert( pFlat->TessellateToMesh( t0, v0, n0, c0, 0 ) );
		const Scalar diag = BBoxDiagonal( v0 );
		SinFunction2D* pSin = new SinFunction2D( 3.0 );
		DisplacedGeometry* pDisp = new DisplacedGeometry( fx[i].mesh, 40, pSin, 0.05 * diag, false, false, false );
		pDisp->Realize(); assert( pDisp->TessellateToMesh( t1, v1, n1, c1, 0 ) );
		assert( v0.size() == v1.size() && t0.size() == t1.size() );
		// groups of vertices coincident in the UNdisplaced mesh
		std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> > groups;
		const Scalar q = 1e-6 * diag;
		for( unsigned int k = 0; k < v0.size(); ++k ) groups[ std::make_tuple( (long long)std::floor( v0[k].x / q + 0.5 ), (long long)std::floor( v0[k].y / q + 0.5 ), (long long)std::floor( v0[k].z / q + 0.5 ) ) ].push_back( k );
		size_t multi = 0; Scalar worstSpread = 0;
		for( std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> >::const_iterator g = groups.begin(); g != groups.end(); ++g ) {
			if( g->second.size() < 2 ) continue;
			++multi;
			for( size_t a = 1; a < g->second.size(); ++a ) {
				const Point3& p0 = v1[g->second[0]]; const Point3& pa = v1[g->second[a]];
				worstSpread = std::max( worstSpread, std::sqrt( ( p0.x - pa.x )*( p0.x - pa.x ) + ( p0.y - pa.y )*( p0.y - pa.y ) + ( p0.z - pa.z )*( p0.z - pa.z ) ) );
			}
		}
		Scalar maxMove = 0;
		for( size_t k = 0; k < v0.size(); ++k ) maxMove = std::max( maxMove, std::sqrt( ( v0[k].x - v1[k].x )*( v0[k].x - v1[k].x ) + ( v0[k].y - v1[k].y )*( v0[k].y - v1[k].y ) + ( v0[k].z - v1[k].z )*( v0[k].z - v1[k].z ) ) );
		std::cout << "  [info] " << fx[i].name << ": " << multi << " coincident groups, worst crack after displacement " << worstSpread / diag << " x diagonal (max move " << maxMove / diag << ")\n";
		if( !( maxMove > 1e-3 * diag ) ) { ++softFails; std::cout << "  [FAIL] " << fx[i].name << ": the displacement did not move anything (vacuous)\n"; }
		if( fx[i].expectCoincident && multi == 0 ) { ++softFails; std::cout << "  [FAIL] " << fx[i].name << ": expected coincident crease/cap vertices, found none (fixture drift)\n"; }
		if( worstSpread > 1e-9 * diag ) { ++softFails; std::cout << "  [FAIL] " << fx[i].name << ": crack " << worstSpread / diag << " x diagonal\n"; }
		pDisp->release(); pFlat->release(); pSin->release(); pConst->release();
	}
	for( size_t i = 0; i < fx.size(); ++i ) fx[i].mesh->release();
	assert( softFails == 0 );
}

//-----------------------------------------------------------------------------
// Round-6: WHICH coincident vertices the displacement welds is the BASE MESH's say
// (TriangleMeshGeometryIndexed::DisplacementWeldPolicy).
//  * a builder (lathe / sweep / skin) welds every coincident group -- its recorded creases,
//    cap rims, duplicated profile points, pinch poles;
//  * that holds at EVERY layer: a displaced base is welded again (its coincident vertices
//    leave the inner bake with different transported shading normals), and the displaced
//    chunk's own face_normals flag (how the result SHADES) does not disable it;
//  * an import / hand-built mesh welds only members whose base normals are within a crease
//    cone (< 120 deg): a folded sheet stays closed, but two coincident OPPOSITE-facing sheets
//    (a two-sided leaf) are separate surfaces and each keeps its full relief;
//  * path_instances_geometry welds nothing: touching instances are separate bodies.
//-----------------------------------------------------------------------------
class WaveFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar x, const Scalar y ) const { return 0.5 + 0.5 * std::sin( 23.0 * x ) * std::cos( 17.0 * y ); }
};

class VOnlyFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar /*x*/, const Scalar y ) const { return y; }
};

static VerticesListType BakedVertices( DisplacedGeometry* pDisp )
{
	pDisp->Realize();
	const TriangleMeshGeometryIndexed* pMesh = dynamic_cast<const TriangleMeshGeometryIndexed*>( pDisp->GetBakedMesh() );
	assert( pMesh );
	return pMesh->getVertices();
}

// Worst distance between members of one coincident group of v0 (the UNdisplaced vertices), measured
// on the same-index displaced vertices v1; `groups` receives the number of coincident groups.
static Scalar CoincidentSpread( const VerticesListType& v0, const VerticesListType& v1, size_t* groups )
{
	assert( v0.size() == v1.size() );
	const Scalar diag = BBoxDiagonal( v0 ), q = 1e-6 * diag;
	std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> > g;
	for( unsigned int k = 0; k < v0.size(); ++k ) g[ std::make_tuple( (long long)std::floor( v0[k].x / q + 0.5 ), (long long)std::floor( v0[k].y / q + 0.5 ), (long long)std::floor( v0[k].z / q + 0.5 ) ) ].push_back( k );
	size_t multi = 0; Scalar worst = 0;
	for( std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> >::const_iterator it = g.begin(); it != g.end(); ++it ) {
		if( it->second.size() < 2 ) continue;
		++multi;
		for( size_t a = 1; a < it->second.size(); ++a ) {
			const Point3& p0 = v1[it->second[0]]; const Point3& pa = v1[it->second[a]];
			worst = std::max( worst, std::sqrt( ( p0.x - pa.x )*( p0.x - pa.x ) + ( p0.y - pa.y )*( p0.y - pa.y ) + ( p0.z - pa.z )*( p0.z - pa.z ) ) );
		}
	}
	if( groups ) *groups = multi;
	return worst;
}

// As CoincidentSpread, but split by the baked mesh's weld DOMAIN (path_instances: the instance):
// the worst separation of two coincident vertices of the SAME domain and of DIFFERENT domains.
static void CoincidentSpreadByDomain( const VerticesListType& v0, const VerticesListType& v1, const std::vector<unsigned int>& dom, Scalar& within, Scalar& across, size_t& withinGroups, size_t& acrossGroups )
{
	assert( v0.size() == v1.size() && dom.size() == v0.size() );
	const Scalar diag = BBoxDiagonal( v0 ), q = 1e-6 * diag;
	std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> > g;
	for( unsigned int k = 0; k < v0.size(); ++k ) g[ std::make_tuple( (long long)std::floor( v0[k].x / q + 0.5 ), (long long)std::floor( v0[k].y / q + 0.5 ), (long long)std::floor( v0[k].z / q + 0.5 ) ) ].push_back( k );
	within = across = 0; withinGroups = acrossGroups = 0;
	for( std::map<std::tuple<long long, long long, long long>, std::vector<unsigned int> >::const_iterator it = g.begin(); it != g.end(); ++it ) {
		const std::vector<unsigned int>& m = it->second;
		bool anyWithin = false, anyAcross = false;
		for( size_t a = 0; a < m.size(); ++a ) for( size_t b = a + 1; b < m.size(); ++b ) {
			const Point3& p = v1[m[a]]; const Point3& r = v1[m[b]];
			const Scalar d = std::sqrt( ( p.x - r.x )*( p.x - r.x ) + ( p.y - r.y )*( p.y - r.y ) + ( p.z - r.z )*( p.z - r.z ) );
			if( dom[m[a]] == dom[m[b]] ) { anyWithin = true; within = std::max( within, d ); }
			else { anyAcross = true; across = std::max( across, d ); }
		}
		if( anyWithin ) ++withinGroups;
		if( anyAcross ) ++acrossGroups;
	}
}

static Scalar MaxMove( const VerticesListType& a, const VerticesListType& b )
{
	Scalar m = 0;
	for( size_t k = 0; k < a.size() && k < b.size(); ++k ) m = std::max( m, std::sqrt( ( a[k].x - b[k].x )*( a[k].x - b[k].x ) + ( a[k].y - b[k].y )*( a[k].y - b[k].y ) + ( a[k].z - b[k].z )*( a[k].z - b[k].z ) ) );
	return m;
}

static void TestWeldPoliciesPerBaseMesh()
{
	std::cout << "Test 10s: the weld follows the BASE mesh (builder: every group, at every layer, whatever face_normals; import: crease cone; path instances: none)...\n";
	int softFails = 0;
#define SOFT( cond ) do { if( !( cond ) ) { ++softFails; std::cout << "  [FAIL] " #cond "\n"; } } while( 0 )

	// ---- a 270 degree FACETED lathe with a knife-edge step: caps, creases at every profile point ----
	static const double prof[] = { 0.0,0,  1,0,  1,1.5,  0.6,1.5,  0.6,3,  0,3 };
	LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 6; ld.nRadial = 16; ld.sweepDegrees = 270; ld.smooth = false;
	ITriangleMeshGeometryIndexed* pI = 0;
	assert( RISE_API_CreateLatheGeometry( &pI, ld ) );
	TriangleMeshGeometryIndexed* pLathe = dynamic_cast<TriangleMeshGeometryIndexed*>( pI );
	assert( pLathe );
	WaveFunction2D* pWave = new WaveFunction2D();
	const Scalar ds = 0.05;

	for( int faceN = 0; faceN < 2; ++faceN ) {
		DisplacedGeometry* pFlat = new DisplacedGeometry( pLathe, 24, pWave, 1e-12, false, faceN == 1, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLathe, 24, pWave, ds, false, faceN == 1, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		size_t groups = 0; const Scalar spread = CoincidentSpread( v0, v1, &groups );
		std::cout << "  [info] lathe base, displaced chunk face_normals " << ( faceN ? "TRUE" : "FALSE" ) << ": " << groups << " coincident groups, worst crack " << spread << " (max move " << MaxMove( v0, v1 ) << ")\n";
		SOFT( groups > 0 && MaxMove( v0, v1 ) > 0.01 );
		SOFT( spread < 1e-9 );
		pFlat->release(); pDisp->release();
	}
	{	// displaced OVER displaced: the outer layer welds again (the inner bake's coincident vertices carry different shading normals)
		DisplacedGeometry* pInner = new DisplacedGeometry( pLathe, 24, pWave, 0.02, false, false, false );
		DisplacedGeometry* pFlat = new DisplacedGeometry( pInner, 24, pWave, 1e-12, false, false, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pInner, 24, pWave, ds, false, false, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		size_t groups = 0; const Scalar spread = CoincidentSpread( v0, v1, &groups );
		std::cout << "  [info] displaced over displaced lathe: " << groups << " coincident groups, worst crack " << spread << " (max move " << MaxMove( v0, v1 ) << ")\n";
		SOFT( groups > 0 && MaxMove( v0, v1 ) > 0.01 );
		SOFT( spread < 1e-9 );
		pFlat->release(); pDisp->release(); pInner->release();
	}
	pWave->release();

	// ---- imports: two coincident OPPOSITE sheets stay independent; a 90 degree fold stays closed ----
	ConstFunction2D* pOne = new ConstFunction2D( 1.0 );
	auto buildImport = [&]( const VerticesListType& V, const NormalsListType& N, const IndexTriangleListType& T ) {
		TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( true, false );
		TexCoordsListType C; for( size_t i = 0; i < V.size(); ++i ) C.push_back( TexCoord( V[i].x, V[i].z ) );
		m->BeginIndexedTriangles(); m->AddVertices( V ); m->AddNormals( N ); m->AddTexCoords( C ); m->AddIndexedTriangles( T ); m->DoneIndexedTriangles();
		return m;
	};
	auto quad = [&]( IndexTriangleListType& T, const unsigned int a, const unsigned int b, const unsigned int c, const unsigned int d ) {
		IndexedTriangle x, y;
		x.iVertices[0] = x.iNormals[0] = x.iCoords[0] = a; x.iVertices[1] = x.iNormals[1] = x.iCoords[1] = b; x.iVertices[2] = x.iNormals[2] = x.iCoords[2] = c;
		y.iVertices[0] = y.iNormals[0] = y.iCoords[0] = a; y.iVertices[1] = y.iNormals[1] = y.iCoords[1] = c; y.iVertices[2] = y.iNormals[2] = y.iCoords[2] = d;
		T.push_back( x ); T.push_back( y );
	};
	{	// double-sided leaf: two coincident quads with opposite normals
		VerticesListType V; NormalsListType N; IndexTriangleListType T;
		for( int s = 0; s < 2; ++s ) for( int j = 0; j < 2; ++j ) for( int i = 0; i < 2; ++i ) { V.push_back( Point3( i, 0, j ) ); N.push_back( Vector3( 0, s ? -1 : 1, 0 ) ); }
		quad( T, 0, 2, 3, 1 ); quad( T, 4, 5, 7, 6 );
		TriangleMeshGeometryIndexed* pLeaf = buildImport( V, N, T );
		DisplacedGeometry* pFlat = new DisplacedGeometry( pLeaf, 8, pOne, 1e-12, true, false, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pLeaf, 8, pOne, 0.1, true, false, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		Scalar maxY = 0; for( size_t k = 0; k < v1.size(); ++k ) maxY = std::max( maxY, std::fabs( v1[k].y ) );
		size_t groups = 0; const Scalar spread = CoincidentSpread( v0, v1, &groups );
		std::cout << "  [info] two-sided leaf import: max |y| displacement " << maxY << " (single sheet: 0.1), " << groups << " coincident groups, sheets end " << spread << " apart\n";
		SOFT( std::fabs( maxY - 0.1 ) < 1e-9 );		// averaging the opposite normals cancelled it to 0
		SOFT( std::fabs( spread - 0.2 ) < 1e-9 );		// +0.1 and -0.1: independent
		pFlat->release(); pDisp->release(); pLeaf->release();
	}
	{	// a 90 degree fold with duplicated edge vertices: inside the crease cone, welded
		VerticesListType V; NormalsListType N; IndexTriangleListType T;
		V.push_back( Point3( 0, 0, 0 ) ); V.push_back( Point3( 1, 0, 0 ) ); V.push_back( Point3( 1, 0, 1 ) ); V.push_back( Point3( 0, 0, 1 ) );
		for( int i = 0; i < 4; ++i ) N.push_back( Vector3( 0, 1, 0 ) );
		V.push_back( Point3( 1, 0, 0 ) ); V.push_back( Point3( 1, 1, 0 ) ); V.push_back( Point3( 1, 1, 1 ) ); V.push_back( Point3( 1, 0, 1 ) );
		for( int i = 0; i < 4; ++i ) N.push_back( Vector3( 1, 0, 0 ) );
		quad( T, 0, 1, 2, 3 ); quad( T, 4, 5, 6, 7 );
		TriangleMeshGeometryIndexed* pFold = buildImport( V, N, T );
		DisplacedGeometry* pFlat = new DisplacedGeometry( pFold, 8, pOne, 1e-12, true, false, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pFold, 8, pOne, 0.1, true, false, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		size_t groups = 0; const Scalar spread = CoincidentSpread( v0, v1, &groups );
		std::cout << "  [info] folded-sheet import: " << groups << " coincident groups, worst crack " << spread << "\n";
		SOFT( groups > 0 && spread < 1e-9 );
		pFlat->release(); pDisp->release(); pFold->release();
	}
	pOne->release();

	// ---- touching path instances: closed INSIDE each instance, independent ACROSS instances ----
	{
		static const double tube[] = { 1.0, 0.0,  1.0, 2.0 };
		LatheDescriptor td; td.profilePoints = tube; td.numProfilePoints = 2; td.nRadial = 16;
		ITriangleMeshGeometryIndexed* pTplI = 0;
		assert( RISE_API_CreateLatheGeometry( &pTplI, td ) );
		static const double path[] = { 0.0, 0.0, 0.0,   0.0, 0.0, 10.0 };
		PathInstancesDescriptor pd;
		pd.pGeometry = dynamic_cast<IGeometry*>( pTplI ); pd.pathPoints = path; pd.numPathPoints = 2;
		pd.nLen = 8; pd.pitch = 2.0; pd.phase = 1.0; pd.scale = 1.0; pd.detail = 16;		// pitch == the tube's length: instances touch rim to rim
		ITriangleMeshGeometryIndexed* pInstI = 0;
		assert( RISE_API_CreatePathInstancesGeometry( &pInstI, pd ) );
		TriangleMeshGeometryIndexed* pInst = dynamic_cast<TriangleMeshGeometryIndexed*>( pInstI );
		assert( pInst );
		SOFT( pInst->GetDisplacementWeldPolicy() == TriangleMeshGeometryIndexed::kWeldAll );	// the lathe template's own rule
		SOFT( pInst->GetDisplacementWeldDomains().size() == pInst->getVertices().size() );
		VOnlyFunction2D* pV = new VOnlyFunction2D();
		DisplacedGeometry* pFlat = new DisplacedGeometry( pInst, 16, pV, 1e-12, false, false, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pInst, 16, pV, 0.05, false, false, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		const std::vector<unsigned int> dom = dynamic_cast<const TriangleMeshGeometryIndexed*>( pDisp->GetBakedMesh() )->GetDisplacementWeldDomains();
		Scalar within = 0, across = 0; size_t wg = 0, ag = 0;
		if( dom.size() == v0.size() ) CoincidentSpreadByDomain( v0, v1, dom, within, across, wg, ag );
		else { ++softFails; std::cout << "  [FAIL] the baked mesh lost the weld domains\n"; }
		std::cout << "  [info] touching path instances: " << wg << " coincident groups inside an instance (worst " << within << "), " << ag << " across instances (worst separation " << across << ", ~disp_scale)\n";
		SOFT( ag > 0 );
		SOFT( across > 0.02 );		// each instance displaced by ITS OWN field at its rim; welding across would have fused them to 0
		pFlat->release(); pDisp->release(); pV->release(); pInst->release(); pTplI->release();
	}
	pLathe->release();
	assert( softFails == 0 );
#undef SOFT
}

//-----------------------------------------------------------------------------
// Round-7: displaced_geometry over path_instances_geometry welds WITHIN each instance (by the
// template's own rule), never across.  Round 6 gave path instances `weld nothing`, which
// cracked every crease / cap / box corner inside every instance (a faceted lathe template: all
// groups split, spread 0.24; a box template: all of them).
//-----------------------------------------------------------------------------
static void TestPathInstancesWeldWithinInstance()
{
	std::cout << "Test 10t: displaced path_instances_geometry is closed inside every instance (creased lathe and box templates)...\n";
	int softFails = 0;
#define SOFT( cond ) do { if( !( cond ) ) { ++softFails; std::cout << "  [FAIL] " #cond "\n"; } } while( 0 )
	WaveFunction2D* pWave = new WaveFunction2D();
	static const double prof[] = { 0.0,0,  1,0,  1,1.5,  0.6,1.5,  0.6,3,  0,3 };
	LatheDescriptor ld; ld.profilePoints = prof; ld.numProfilePoints = 6; ld.nRadial = 16; ld.sweepDegrees = 360; ld.smooth = false;
	ITriangleMeshGeometryIndexed* pLatheI = 0;
	assert( RISE_API_CreateLatheGeometry( &pLatheI, ld ) );
	IGeometry* pBox = 0;
	assert( RISE_API_CreateBoxGeometry( &pBox, 1, 1, 1 ) );
	static const double path[] = { 0.0, 0.0, 0.0,   20.0, 0.0, 0.0 };
	struct T { const char* name; IGeometry* tpl; } templates[] = { { "faceted lathe template", dynamic_cast<IGeometry*>( pLatheI ) }, { "box template", pBox } };
	for( size_t t = 0; t < 2; ++t ) {
		PathInstancesDescriptor pd; pd.pGeometry = templates[t].tpl; pd.pathPoints = path; pd.numPathPoints = 2; pd.pitch = 4; pd.scale = 1;
		ITriangleMeshGeometryIndexed* pInstI = 0;
		assert( RISE_API_CreatePathInstancesGeometry( &pInstI, pd ) );
		TriangleMeshGeometryIndexed* pInst = dynamic_cast<TriangleMeshGeometryIndexed*>( pInstI );
		assert( pInst );
		DisplacedGeometry* pFlat = new DisplacedGeometry( pInst, 24, pWave, 1e-12, false, false, false );
		DisplacedGeometry* pDisp = new DisplacedGeometry( pInst, 24, pWave, 0.05, false, false, false );
		const VerticesListType v0 = BakedVertices( pFlat ), v1 = BakedVertices( pDisp );
		const std::vector<unsigned int> dom = dynamic_cast<const TriangleMeshGeometryIndexed*>( pDisp->GetBakedMesh() )->GetDisplacementWeldDomains();
		Scalar within = 0, across = 0; size_t wg = 0, ag = 0;
		if( dom.size() == v0.size() ) CoincidentSpreadByDomain( v0, v1, dom, within, across, wg, ag );
		else { ++softFails; std::cout << "  [FAIL] the baked mesh lost the weld domains\n"; }
		std::cout << "  [info] path_instances of a " << templates[t].name << ": " << wg << " coincident groups inside instances, worst crack " << within << " (max move " << MaxMove( v0, v1 ) << ")\n";
		SOFT( wg > 0 && MaxMove( v0, v1 ) > 0.01 );
		SOFT( within < 1e-9 );
		pFlat->release(); pDisp->release(); pInst->release();
	}
	pBox->release(); pLatheI->release(); pWave->release();
	assert( softFails == 0 );
#undef SOFT
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
	// Debug/red-proof aid: DG_ONLY=q|r runs just the T-junction / watertight-displacement fixtures.
	if( const char* only = std::getenv( "DG_ONLY" ) ) {
		if( only[0] == 'q' ) { TestRefinementHasNoTJunctions(); return 0; }
		if( only[0] == 'r' ) { TestDisplacementIsWatertightAtCreases(); return 0; }
		if( only[0] == 's' ) { TestWeldPoliciesPerBaseMesh(); return 0; }
		if( only[0] == 't' ) { TestPathInstancesWeldWithinInstance(); return 0; }
	}
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
	TestDisplacedOverPinchedLatheStaysClosed();
	TestDisplacedBuildersShadeSmooth();
	TestDetailRefinesMeshBases();
	TestRefinedTopologyStableAcrossRefresh();
	TestAngleWeightedNormalsIgnoreTriangulation();
	TestRefinedMeshKeepsBaseShadingField();
	TestRefinementChartInterpolation();
	TestTransportedShadingNormals();
	TestFaceNormalBaseIsNotRefined();
	TestRefinementIsRegular();
	TestLoftRefineHasNoSlivers();
	TestPolarVertexDisplacesByNeighbourAverage();
	TestRefinementKeepsAuthoredSurface();
	TestCreasesOnlyWhereAuthoredAndPolygonsStayPolygons();
	TestRefinementHasNoTJunctions();
	TestDisplacementIsWatertightAtCreases();
	TestWeldPoliciesPerBaseMesh();
	TestPathInstancesWeldWithinInstance();
	TestDisplacedBaseIsNotRefinedAgain();
	TestShadingTransportIsRotation();
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
