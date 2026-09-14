//////////////////////////////////////////////////////////////////////
//
//  UVGeneratorObjectSpaceInputTest.cpp - Regression guard for DL-95:
//    `Object::IntersectRay` used to hand an overriding UV generator
//    `ri.geometric.ptIntersection` roughly 380 lines BEFORE that field
//    is written for THIS hit -- the write happens only at the general-
//    path stamp near the end of the function, well after the UV
//    generator has already run and returned.
//
//  THE BUG PATTERN, IN ONE SENTENCE
//
//    A handful of analytic geometries (SphereGeometry, BoxGeometry,
//    TorusGeometry, CircularDiskGeometry, ClippedPlaneGeometry,
//    BilinearPatchGeometry, DisplacedGeometry, HairGeometry,
//    GeometryUtilities' bake-time synthetic hit) stamp an OBJECT-space
//    `ri.ptIntersection` inside their OWN `IntersectRay`, which
//    happens to make the UV-generator call read a live, correct,
//    object-space point for THEM -- but `TriangleMeshGeometry{,Indexed}`
//    stamp neither `ptIntersection` nor `ptObjIntersec` at all (their
//    position is reconstructed generically, later, by
//    `Object::IntersectRay` itself), so on a MESH the UV-generator call
//    read whatever stale point happened to already be sitting in the
//    shared `RayIntersection` record: `(0, 0, 0)` in a freshly
//    constructed one, or the PREVIOUS object's hit point in a real
//    render loop that reuses one record across a BVH traversal.
//
//  THE FIX
//
//    `Object::IntersectRay` now computes the canonical `ptObjIntersec`
//    (object-space ray, evaluated at the object-space range, the exact
//    expression the pre-existing general-path stamp a few hundred
//    lines below already used to build `ptIntersection`'s WORLD-space
//    value) BEFORE calling the UV generator, and hands the generator
//    THAT field instead of `ptIntersection`.  This is the frame a UV
//    generator is authored in (the object's own local box/cylinder/
//    sphere dimensions) and the frame every analytic stamper already
//    used, so those primitives are unaffected (sub-test 3, a
//    consistency pin); it is the fix for meshes (sub-tests 1 and 2,
//    genuine red-proofs).
//
//  WHAT EACH SUB-TEST PROVES
//
//    Sub-test 1 (fresh-record flavour).  Two different points on the
//      same face of a double-sided cube MESH, each hit with its own
//      freshly-constructed `RayIntersection` (so `ptIntersection`
//      starts at the default `(0, 0, 0)`), under a `BoxUVGenerator`.
//      Pre-fix both charts collapse to `BoxUVGenerator`'s output for
//      `ptIntersection == (0, 0, 0)` -- `(0.5, 0.5)` -- regardless of
//      where the ray actually struck.  Money assertion: the two charts
//      differ, and match the closed-form box projection of the ACTUAL
//      hit points.
//
//    Sub-test 2 (previous-object flavour, END-TO-END).  The exact
//      "real render loop" case the ledger row names: one
//      `RayIntersection` record is used to hit a `ClippedPlaneGeometry`
//      object first (which DOES stamp its own point, landing a known,
//      unrelated point in the record), then reused, unmodified between
//      hits exactly as `Object::IntersectRay` leaves it, to hit a
//      double-sided cube MESH carrying a `BoxUVGenerator`.  Pre-fix the
//      mesh's chart follows the FIRST object's leftover point, not the
//      cube's own hit point.  Money assertion: the mesh's chart matches
//      its own hit point and is independent of what the previous object
//      left behind.
//
//    Sub-test 3 (consistency pin, NOT a red-proof -- green before and
//      after).  For each analytic geometry that self-stamps
//      `ptIntersection` (sphere, box, torus, disk, clipped-plane,
//      bilinear-patch), a `RecordingUVGenerator` echoes whatever point
//      it is handed straight into `ptCoord` (u = x, v = y in the
//      OBJECT-space point), so its output is a direct probe of "what
//      point did the UV generator see" without needing to invert any
//      per-shape projection.  Comparing that probe (via a real,
//      identity-transform `Object`) against the SAME geometry's own
//      `ptIntersection`, read straight off a bare, un-wrapped
//      `IntersectRay` call (bypassing `Object` entirely -- the true,
//      un-mediated ground truth), pins that the fix does not move the
//      analytic primitives' UV input: both routes agree to within the
//      1e-9 tolerance that comfortably contains the `SURFACE_INTERSEC_ERROR`
//      (1e-12) back-off `ptObjIntersec` applies and the self-stamp does
//      not.
//
//  CSG NOTE (sibling audit, not a red-proof here)
//
//    `CSGObject::IntersectRay` never calls `pUVGenerator->GenerateUV`
//    at all -- grep confirms zero occurrences in CSGObject.cpp -- so an
//    overriding UV generator `SetUVGenerator`'d directly onto a
//    `csg_object` is silently never invoked at the composite level
//    (an operand Object's OWN UV generator, if it has one, still fires
//    inside that operand's own `Object::IntersectRay`, in that
//    operand's own object space, and its `ptCoord` survives upward
//    through `AdoptCsgSurfacePayload` unchanged).  This is a different
//    bug PATTERN (a missing call, not a stale-read ordering defect) and
//    is out of scope for DL-95; recorded as DL-107 in the ledger.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl95`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/BilinearPatchGeometry.h"
#include "../src/Library/Geometry/BoxUVGenerator.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Interfaces/IUVGenerator.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Polygon.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static void CheckClose( double got, double want, double tol, const char* testName )
{
	const bool ok = std::fabs( got - want ) <= tol;
	if( ok ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName
			<< "  (got " << got << ", want " << want << " +/- " << tol << ")" << std::endl;
	}
}

namespace
{
	Vector3 AxisVector( int axis, Scalar value )
	{
		Vector3 v( 0, 0, 0 );
		if( axis == 0 ) v.x = value;
		else if( axis == 1 ) v.y = value;
		else v.z = value;
		return v;
	}

	//! Mirrors GeomNormalOrientationSitesTest.cpp's `BuildCube`: a
	//! closed, axis-aligned cube spanning [-halfSize, halfSize]^3, six
	//! flat-shaded unshared-vertex quads.  `TriangleMeshGeometryIndexed`
	//! never stamps `ptIntersection` / `ptObjIntersec` itself -- that is
	//! exactly the DL-95 gap.
	TriangleMeshGeometryIndexed* BuildCube( bool bDoubleSided, Scalar halfSize )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( bDoubleSided, false );
		mesh->BeginIndexedTriangles();

		unsigned int nextVertex = 0;
		auto addFace = [&]( int axis, Scalar sign )
		{
			const Vector3 outward = AxisVector( axis, sign );
			const int uAxis = (axis + 1) % 3;
			const int vAxis = (axis + 2) % 3;
			const unsigned int base = nextVertex;
			for( int su = -1; su <= 1; su += 2 ) {
				for( int sv = -1; sv <= 1; sv += 2 ) {
					Point3 p( 0, 0, 0 );
					p[axis] = sign * halfSize;
					p[uAxis] = static_cast<Scalar>( su ) * halfSize;
					p[vAxis] = static_cast<Scalar>( sv ) * halfSize;
					mesh->AddVertex( p );
					mesh->AddNormal( outward );
					mesh->AddTexCoord( Point2( 0, 0 ) );
				}
			}
			IndexedTriangle t1, t2;
			t1.iVertices[0] = base + 0; t1.iVertices[1] = base + 1; t1.iVertices[2] = base + 2;
			t2.iVertices[0] = base + 1; t2.iVertices[1] = base + 3; t2.iVertices[2] = base + 2;
			for( int k = 0; k < 3; k++ ) {
				t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
				t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
			}
			mesh->AddIndexedTriangle( t1 );
			mesh->AddIndexedTriangle( t2 );
			nextVertex += 4;
		};

		addFace( 0, +1 ); addFace( 0, -1 );
		addFace( 1, +1 ); addFace( 1, -1 );
		addFace( 2, +1 ); addFace( 2, -1 );

		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! Production intersection call: front + back faces, exit info, real
	//! `Object::IntersectRay`.  Deliberately does NOT touch
	//! `ri.geometric.ptIntersection` between calls -- a caller that
	//! reuses one `ri` across two `Hit()` calls on two different objects
	//! is exactly the "real render loop reuses one record across a BVH
	//! traversal" scenario DL-95 names.
	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}

	//! Echoes the point it is handed straight into (u, v) = (x, y), so
	//! its output is a direct, per-shape-projection-free probe of
	//! "what point did the generator see".
	class RecordingUVGenerator : public virtual IUVGenerator, public virtual Reference
	{
	protected:
		virtual ~RecordingUVGenerator() {}
	public:
		virtual void GenerateUV( const Point3& ptIntersection, const Vector3&, Point2& uv ) const
		{
			uv = Point2( ptIntersection.x, ptIntersection.y );
		}
	};
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: fresh-record flavour -- two different points on the
//  same mesh face, each with its own freshly-constructed
//  RayIntersection.
//////////////////////////////////////////////////////////////////////
static void TestMeshTwoPointsProduceDifferentUV()
{
	std::cout << "Sub-test 1: two different points on the same mesh face chart to different UVs"
		<< std::endl;

	TriangleMeshGeometryIndexed* g = BuildCube( /*bDoubleSided*/true, 1.0 );
	Object* o = new Object( g );  safe_release( g );
	BoxUVGenerator* uvg = new BoxUVGenerator( 2.0, 2.0, 2.0 );
	uvg->addref();
	o->SetUVGenerator( *uvg );
	o->FinalizeTransformations();

	// Two points on the +Z face (halfSize 1): (0.3, 0.1, 1) and
	// (-0.3, 0.4, 1).  Closed-form BoxUVGenerator side 5 (the +Z side):
	// u = (x + w/2)/w, v = 1 - (y + h/2)/h, w = h = 2.
	const Ray rayA( Point3( 0.3, 0.1, 3 ), Vector3( 0, 0, -1 ) );
	const Ray rayB( Point3( -0.3, 0.4, 3 ), Vector3( 0, 0, -1 ) );

	RayIntersection riA( rayA, nullRasterizerState );  Hit( o, rayA, riA );
	RayIntersection riB( rayB, nullRasterizerState );  Hit( o, rayB, riB );

	Check( riA.geometric.bHit && riB.geometric.bHit, "fixture: both rays hit the mesh" );

	if( riA.geometric.bHit && riB.geometric.bHit )
	{
		// MONEY: the two charts must differ.  Pre-fix BOTH read
		// BoxUVGenerator's output for ptIntersection == (0, 0, 0)
		// (the default-constructed record's stale value): side 5,
		// u = (0+1)*0.5 = 0.5, v = 1-(0+1)*0.5 = 0.5 -- (0.5, 0.5)
		// for BOTH hits, so this check is red pre-fix.
		Check( std::fabs( riA.geometric.ptCoord.x - riB.geometric.ptCoord.x ) > 0.01 ||
			   std::fabs( riA.geometric.ptCoord.y - riB.geometric.ptCoord.y ) > 0.01,
			"MONEY: two different hit points chart to different UVs" );

		CheckClose( riA.geometric.ptCoord.x, 0.65, 1e-9, "point A charts to u=0.65" );
		CheckClose( riA.geometric.ptCoord.y, 0.45, 1e-9, "point A charts to v=0.45" );
		CheckClose( riB.geometric.ptCoord.x, 0.35, 1e-9, "point B charts to u=0.35" );
		CheckClose( riB.geometric.ptCoord.y, 0.30, 1e-9, "point B charts to v=0.30" );
	}

	uvg->release();
	o->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: previous-object flavour, END-TO-END -- one
//  RayIntersection record reused across two different objects.
//////////////////////////////////////////////////////////////////////
static void TestMeshUVIndependentOfPreviousObjectHit()
{
	std::cout << "Sub-test 2: a mesh's UV chart does not follow a PREVIOUS object's leftover hit point"
		<< std::endl;

	// The "previous object": a plane, far from the mesh under test,
	// that DOES self-stamp its own ptIntersection.  No UV generator on
	// this one -- it exists only to plant a known, unrelated point in
	// the shared record.
	const Point3 farCorners[4] = {
		Point3( -100, -100, 50 ), Point3( 100, -100, 50 ),
		Point3( 100, 100, 50 ), Point3( -100, 100, 50 )
	};
	ClippedPlaneGeometry* gPrev = new ClippedPlaneGeometry( farCorners, /*bDoubleSided*/true );
	Object* oPrev = new Object( gPrev );  safe_release( gPrev );
	oPrev->FinalizeTransformations();

	const Ray rayPrev( Point3( 2, 3, 60 ), Vector3( 0, 0, -1 ) );

	// The mesh under test, with a BoxUVGenerator.
	TriangleMeshGeometryIndexed* g = BuildCube( /*bDoubleSided*/true, 1.0 );
	Object* o = new Object( g );  safe_release( g );
	BoxUVGenerator* uvg = new BoxUVGenerator( 2.0, 2.0, 2.0 );
	uvg->addref();
	o->SetUVGenerator( *uvg );
	o->FinalizeTransformations();

	const Ray rayMesh( Point3( 0.3, 0.1, 3 ), Vector3( 0, 0, -1 ) );

	// ONE record, hit the previous object first, then the mesh --
	// exactly as Object::IntersectRay leaves ri.geometric.ptIntersection
	// between two calls in a real render loop.
	RayIntersection ri( rayPrev, nullRasterizerState );
	Hit( oPrev, rayPrev, ri );
	Check( ri.geometric.bHit, "fixture: the previous object is hit" );
	Check( std::fabs( ri.geometric.ptIntersection.x - 2.0 ) < 1e-9 &&
		   std::fabs( ri.geometric.ptIntersection.y - 3.0 ) < 1e-9 &&
		   std::fabs( ri.geometric.ptIntersection.z - 50.0 ) < 1e-9,
		"fixture: the previous object's hit point is the known, unrelated (2, 3, 50)" );

	Hit( o, rayMesh, ri );
	Check( ri.geometric.bHit, "fixture: the mesh is hit" );

	if( ri.geometric.bHit )
	{
		// MONEY: the mesh's own UV chart, not the (2, 3, 50) BoxUVGenerator
		// would read pre-fix (side 5: u=(2+1)*0.5=1.5, v=1-(3+1)*0.5=-1.0 --
		// both clearly outside [0, 1], and clearly not this hit's own
		// (0.65, 0.45)).
		CheckClose( ri.geometric.ptCoord.x, 0.65, 1e-9,
			"MONEY: mesh charts to its OWN hit point's u, not the previous object's" );
		CheckClose( ri.geometric.ptCoord.y, 0.45, 1e-9,
			"MONEY: mesh charts to its OWN hit point's v, not the previous object's" );
	}

	uvg->release();
	o->release();
	oPrev->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3: consistency pin -- analytic primitives are unaffected.
//////////////////////////////////////////////////////////////////////
namespace
{
	//! Runs `geom` through a bare, un-wrapped IntersectRay (ground
	//! truth: exactly what the geometry itself believes its object-space
	//! intersection point is, with no Object-level mediation at all),
	//! then through a real, identity-transform Object with a
	//! RecordingUVGenerator attached (the code path under test), and
	//! checks the two agree.
	void CheckAnalyticPrimitiveUnaffected( IGeometry* geom, const Ray& ray, const char* name )
	{
		RayIntersectionGeometric riGround( ray, nullRasterizerState );
		geom->IntersectRay( riGround, true, true, false );

		std::string groundHitMsg = std::string( name ) + ": fixture ray hits the bare geometry";
		Check( riGround.bHit, groundHitMsg.c_str() );
		if( !riGround.bHit ) { return; }

		Object* o = new Object( geom );
		RecordingUVGenerator* rec = new RecordingUVGenerator();
		rec->addref();
		o->SetUVGenerator( *rec );
		o->FinalizeTransformations();

		RayIntersection ri( ray, nullRasterizerState );
		Hit( o, ray, ri );

		std::string wrappedHitMsg = std::string( name ) + ": fixture ray hits the Object-wrapped geometry";
		Check( ri.geometric.bHit, wrappedHitMsg.c_str() );
		if( ri.geometric.bHit )
		{
			std::string ux = std::string( name ) + ": UV-generator input x unaffected by the fix";
			std::string uy = std::string( name ) + ": UV-generator input y unaffected by the fix";
			CheckClose( ri.geometric.ptCoord.x, riGround.ptIntersection.x, 1e-9, ux.c_str() );
			CheckClose( ri.geometric.ptCoord.y, riGround.ptIntersection.y, 1e-9, uy.c_str() );
		}

		rec->release();
		o->release();
	}
}

static void TestAnalyticPrimitivesUnaffected()
{
	std::cout << "Sub-test 3: analytic primitives' UV-generator input is unaffected by the fix (consistency pin)"
		<< std::endl;

	{
		SphereGeometry* g = new SphereGeometry( 2.0 );
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 0.3, 0.2, 10 ), Vector3( 0, 0, -1 ) ), "sphere" );
		g->release();
	}
	{
		BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 0.3, 0.2, 10 ), Vector3( 0, 0, -1 ) ), "box" );
		g->release();
	}
	{
		TorusGeometry* g = new TorusGeometry( 2.0, 0.5 );
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 2.0, 0.0, 10 ), Vector3( 0, 0, -1 ) ), "torus" );
		g->release();
	}
	{
		CircularDiskGeometry* g = new CircularDiskGeometry( 2.0, 'z' );
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 0.3, 0.2, 5 ), Vector3( 0, 0, -1 ) ), "disk" );
		g->release();
	}
	{
		const Point3 corners[4] = {
			Point3( -1, -1, 1 ), Point3( 1, -1, 1 ), Point3( 1, 1, 1 ), Point3( -1, 1, 1 )
		};
		ClippedPlaneGeometry* g = new ClippedPlaneGeometry( corners, /*bDoubleSided*/true );
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 0.3, 0.1, 3 ), Vector3( 0, 0, -1 ) ), "clipped-plane" );
		g->release();
	}
	{
		BilinearPatchGeometry* g = new BilinearPatchGeometry( 10, 8, /*bUseBSP=*/false );
		BilinearPatch patch;
		patch.pts[0] = Point3( -1, -1, 0 );
		patch.pts[1] = Point3( -1, 1, 0 );
		patch.pts[2] = Point3( 1, -1, 0 );
		patch.pts[3] = Point3( 1, 1, 0.2 );
		g->AddPatch( patch );
		g->Prepare();
		CheckAnalyticPrimitiveUnaffected( g, Ray( Point3( 0.2, 0.1, 5 ), Vector3( 0, 0, -1 ) ), "bilinear-patch" );
		g->release();
	}
}

int main()
{
	std::cout << "UVGeneratorObjectSpaceInputTest (DL-95)" << std::endl;
	std::cout << "========================================" << std::endl;

	TestMeshTwoPointsProduceDifferentUV();
	TestMeshUVIndependentOfPreviousObjectHit();
	TestAnalyticPrimitivesUnaffected();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
