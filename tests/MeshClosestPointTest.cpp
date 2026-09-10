//////////////////////////////////////////////////////////////////////
//
//  MeshClosestPointTest.cpp - Phase 2 of the CROSS-OBJECT proximity
//  signal (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §8): the MESH family's
//  `DistanceToSurface`, answered by a bounded-radius closest-point
//  traversal of the mesh's own BVH.
//
//  WHY A DIFFERENTIAL AND NOT A LIST OF NUMBERS.  Phase 1's families are
//  closed forms, so `ProximitySignalTest` can check each one against
//  algebra written by hand.  A mesh has no closed form: the answer is
//  `min` over tens of thousands of triangles, and the only honest
//  reference for it is that same minimum computed the slow way.  So the
//  centre of this suite is a DIFFERENTIAL against brute force over EVERY
//  triangle, driving the SAME point-triangle formula
//  (`TriangleMeshGeometryIndexed::PointTriangleDistance`, public and
//  static precisely so this test can call it).  What that pins is the
//  TRAVERSAL -- the pruning, the ordering, the radius cut-off -- which is
//  the only thing Phase 2 added.  It deliberately does NOT re-derive the
//  point-triangle formula: two independent implementations would disagree
//  in the last ulp on edge-region ties and the suite would be measuring
//  its own reference instead of the code.
//
//  ...and then ONE closed form on top of it, because a differential
//  against a shared formula cannot catch a formula that is wrong the same
//  way twice.  Section (b) checks a tessellated sphere against `|p| - R`
//  and BOUNDS the tessellation's own error by measuring it, rather than
//  quoting a chord-error formula nobody re-derives.
//
//  THE SECTIONS:
//    (a) DIFFERENTIAL vs brute force over every triangle, >= 10^5 random
//        points across four meshes: an engine-tessellated sphere, the
//        bunny asset, a two-triangle sliver, and a mesh whose triangles
//        are DEGENERATE (zero area -- collinear and coincident).
//    (b) CLOSED FORM: the tessellated sphere against `|p| - R`, inside a
//        bound derived from the mesh itself (see the section header).
//    (c) The `maxDist` CUT-OFF is exactly at r -- the design's stated
//        half-open `[0, maxDist)` range.
//    (d) The DISPLACED-GEOMETRY forward: a `DisplacedGeometry` over a
//        sphere with a constant height answers the DISPLACED mesh's
//        distance, not the smooth base's.  Answering from the base would
//        under-report the gap by the whole displacement.
//    (e) The RAW (non-indexed) family still REFUSES -- the design's table
//        says it has no BVH and must contribute nothing.
//    (f) EIGHT THREADS querying one mesh concurrently agree with serial,
//        bit for bit.  The traversal is `const` over immutable post-build
//        state with a thread_local scratch stack, and this is what says so.
//    (h) THE TWO CANDIDATE SOURCES AGREE: `NearestOtherSurface` walks the
//        top-level BVH as a point query when the manager has one and scans
//        the flat AABB snapshot when it does not; both are driven over one
//        SHARED object set and compared bit for bit.
//    (g) SCENE D's placement, COMPUTED HERE.  The scene file records the
//        y offsets that put the bunny's lowest vertex on the plane and the
//        dragon's on the bunny; this section re-derives them from the
//        assets and asserts them at 1e-6, so a replaced asset FAILS rather
//        than silently faking contact.  It then drives the §8 Phase-2 gate
//        on the loaded scene: >= 0.9 within 2 mm of the bunny's footprint,
//        0 under the light panel, and the `casts_shadows FALSE` sphere
//        counting.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <filesystem>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/InfinitePlaneGeometry.h"
#include "../src/Library/Functions/ConstantFunctions.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include <cstdio>
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace fs = std::filesystem;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( const Scalar got, const Scalar want, const Scalar tol, const std::string& name )
{
	if( std::fabs( (double)( got - want ) ) <= (double)tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << (double)got
			<< " want " << (double)want << " (tol " << (double)tol << ")" << std::endl;
	}
}

//======================================================================
// Repo / asset location
//======================================================================

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "models" / "risemesh" / "bunny.risemesh" ) ) {
			return p;
		}
	}
	return fs::path();
}

//! Load a `.risemesh` through the SAME deserialize the `risemesh_geometry`
//! chunk uses (Job::AddRISEMeshTriangleMeshGeometry's load-into-memory
//! branch), so section (g)'s placement numbers are derived from exactly
//! the vertex data the scene will render.
static TriangleMeshGeometryIndexed* LoadRiseMesh( const fs::path& file )
{
	IMemoryBuffer* pBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pBuffer, file.string().c_str() );
	if( !pBuffer || pBuffer->Size() == 0 ) {
		if( pBuffer ) pBuffer->release();
		return 0;
	}
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->Deserialize( *pBuffer );
	pBuffer->release();
	if( mesh->numPoints() == 0 ) {
		mesh->release();
		return 0;
	}
	return mesh;
}

//! A tessellated sphere built through the ENGINE's own TessellateToMesh --
//! not a hand-rolled UV grid -- so the fixture is the same triangle soup a
//! `displaced_geometry` or a tessellating importer would produce.
static TriangleMeshGeometryIndexed* BuildTessellatedSphere( const Scalar R, const unsigned int detail )
{
	SphereGeometry* sph = new SphereGeometry( R );
	sph->addref();

	IndexTriangleListType tris;
	VerticesListType      vertices;
	NormalsListType       normals;
	TexCoordsListType     coords;
	const bool ok = sph->TessellateToMesh( tris, vertices, normals, coords, detail );
	sph->release();
	if( !ok ) return 0;

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//! TWO TRIANGLES, both slivers: long, thin, nearly collinear vertices.
//! The point-triangle region method's edge branches are where a sliver
//! lives -- almost no query lands in its interior region -- so this is the
//! fixture that exercises them, and the traversal's leaf handling on a
//! tree with a single node.
static TriangleMeshGeometryIndexed* BuildSliver()
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertex( Point3( -5.0,  0.0,  0.0 ) );
	mesh->AddVertex( Point3(  5.0,  0.0,  0.0 ) );
	mesh->AddVertex( Point3(  0.0,  1e-4,  0.0 ) );
	mesh->AddVertex( Point3(  0.0, -1e-4,  0.3 ) );
	mesh->AddNormal( Vector3( 0, 0, 1 ) );
	mesh->AddTexCoord( Point2( 0, 0 ) );
	IndexedTriangle t;
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
	t.iNormals[0]  = 0; t.iNormals[1]  = 0; t.iNormals[2]  = 0;
	t.iCoords[0]   = 0; t.iCoords[1]   = 0; t.iCoords[2]   = 0;
	mesh->AddIndexedTriangle( t );
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 3;
	mesh->AddIndexedTriangle( t );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//! DEGENERATE triangles: one with three COLLINEAR vertices, one with two
//! COINCIDENT vertices, one with all three coincident (a point), plus one
//! ordinary triangle so the mesh has a real surface to be nearest to.
//! Every one of the degenerate three has zero area, which drives
//! PointTriangleDistance's `denom <= 0` fallback.  Without that fallback
//! the barycentric divide is 0/0 and the answer is NaN -- and a NaN loses
//! every comparison in the traversal, so it would vanish silently rather
//! than fail loudly.  This fixture is what makes the fallback observable.
static TriangleMeshGeometryIndexed* BuildDegenerate()
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertex( Point3(  0.0, 0.0, 0.0 ) );	// 0
	mesh->AddVertex( Point3(  1.0, 0.0, 0.0 ) );	// 1  (0,1,2 collinear)
	mesh->AddVertex( Point3(  2.0, 0.0, 0.0 ) );	// 2
	mesh->AddVertex( Point3(  0.0, 2.0, 0.0 ) );	// 3
	mesh->AddVertex( Point3(  3.0, 3.0, 1.0 ) );	// 4  (an ordinary triangle with 5,6)
	mesh->AddVertex( Point3(  4.0, 3.0, 1.0 ) );	// 5
	mesh->AddVertex( Point3(  3.5, 4.0, 1.0 ) );	// 6
	mesh->AddNormal( Vector3( 0, 0, 1 ) );
	mesh->AddTexCoord( Point2( 0, 0 ) );

	IndexedTriangle t;
	t.iNormals[0] = 0; t.iNormals[1] = 0; t.iNormals[2] = 0;
	t.iCoords[0]  = 0; t.iCoords[1]  = 0; t.iCoords[2]  = 0;
	// collinear
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2; mesh->AddIndexedTriangle( t );
	// two coincident
	t.iVertices[0] = 3; t.iVertices[1] = 3; t.iVertices[2] = 0; mesh->AddIndexedTriangle( t );
	// all three coincident -- a point
	t.iVertices[0] = 3; t.iVertices[1] = 3; t.iVertices[2] = 3; mesh->AddIndexedTriangle( t );
	// one honest triangle
	t.iVertices[0] = 4; t.iVertices[1] = 5; t.iVertices[2] = 6; mesh->AddIndexedTriangle( t );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//======================================================================
// The brute-force reference
//======================================================================

//! `min` over EVERY triangle, under the SAME point-triangle formula the
//! traversal's leaves use.  Returns false when nothing is strictly within
//! `maxDist`, matching the traversal's half-open contract exactly (the
//! design's §10: the reported range is `[0, maxDist)`).
static bool BruteForceDistance(
	const TriangleMeshGeometryIndexed* mesh, const Point3& p,
	const Scalar maxDist, Scalar& outDist )
{
	const PointerTriangleListType& faces = mesh->getFaces();
	Scalar best = maxDist;
	bool found = false;
	for( std::size_t i = 0; i < faces.size(); ++i ) {
		const Scalar d = TriangleMeshGeometryIndexed::PointTriangleDistance(
			p, *faces[i].pVertices[0], *faces[i].pVertices[1], *faces[i].pVertices[2] );
		if( d < best ) { best = d; found = true; }
	}
	if( !found ) return false;
	outDist = best;
	return true;
}

//======================================================================
// (a) THE DIFFERENTIAL
//======================================================================

//! Run `nPoints` uniformly random points in a box that is the mesh's own
//! bounding box inflated by `pad`, and compare the BVH traversal against
//! brute force at three radii: one that admits everything, one at roughly
//! the box scale, and one deliberately tight so most points REFUSE.  The
//! refusals matter as much as the answers -- a traversal that pruned too
//! eagerly would agree on the near points and silently drop the far ones.
struct DiffResult
{
	unsigned int nCompared    = 0;	//!< points where both answered
	unsigned int nBothRefused = 0;
	unsigned int nDisagreeAns = 0;	//!< both answered, different value
	unsigned int nDisagreeYN  = 0;	//!< one answered, the other refused
	double       maxAbsDiff   = 0.0;
	unsigned int nExactBits   = 0;	//!< of nCompared, how many matched bit for bit
};

static void RunDifferential(
	const TriangleMeshGeometryIndexed* mesh, const char* label,
	const unsigned int nPoints, const Scalar pad, const unsigned int seed,
	DiffResult& acc )
{
	const BoundingBox bb = mesh->GenerateBoundingBox();
	const Point3 lo( bb.ll.x - pad, bb.ll.y - pad, bb.ll.z - pad );
	const Point3 hi( bb.ur.x + pad, bb.ur.y + pad, bb.ur.z + pad );

	const Scalar diag = Vector3Ops::Magnitude( Vector3Ops::mkVector3( bb.ur, bb.ll ) );
	const Scalar radii[3] = { RISE_INFINITY, diag, diag * Scalar( 0.02 ) };

	RandomNumberGenerator rng( seed );

	DiffResult local;
	for( unsigned int i = 0; i < nPoints; ++i ) {
		const Point3 p(
			lo.x + ( hi.x - lo.x ) * rng.CanonicalRandom(),
			lo.y + ( hi.y - lo.y ) * rng.CanonicalRandom(),
			lo.z + ( hi.z - lo.z ) * rng.CanonicalRandom() );

		const Scalar r = radii[ i % 3 ];

		Scalar dFast = Scalar( 0 ), dSlow = Scalar( 0 );
		const bool okFast = mesh->DistanceToSurface( p, r, dFast );
		const bool okSlow = BruteForceDistance( mesh, p, r, dSlow );

		if( okFast != okSlow ) {
			++local.nDisagreeYN;
			continue;
		}
		if( !okFast ) { ++local.nBothRefused; continue; }

		++local.nCompared;
		const double diff = std::fabs( (double)( dFast - dSlow ) );
		if( diff > local.maxAbsDiff ) local.maxAbsDiff = diff;
		if( dFast == dSlow ) ++local.nExactBits;
		else ++local.nDisagreeAns;
	}

	std::cout << "      " << label
		<< ": " << mesh->getFaces().size() << " tris, " << nPoints << " points"
		<< " -- answered " << local.nCompared
		<< ", refused-both " << local.nBothRefused
		<< ", bit-identical " << local.nExactBits
		<< ", value-mismatch " << local.nDisagreeAns
		<< ", yes/no-mismatch " << local.nDisagreeYN
		<< ", max|diff| " << local.maxAbsDiff << std::endl;

	Check( local.nDisagreeYN == 0,
		std::string( "(a) " ) + label + ": traversal and brute force agree on WHETHER anything is in range" );
	Check( local.nDisagreeAns == 0,
		std::string( "(a) " ) + label + ": every answered distance is BIT-IDENTICAL to brute force" );

	acc.nCompared    += local.nCompared;
	acc.nBothRefused += local.nBothRefused;
	acc.nDisagreeAns += local.nDisagreeAns;
	acc.nDisagreeYN  += local.nDisagreeYN;
	acc.nExactBits   += local.nExactBits;
	if( local.maxAbsDiff > acc.maxAbsDiff ) acc.maxAbsDiff = local.maxAbsDiff;
}

static void TestDifferential(
	const TriangleMeshGeometryIndexed* sphere,
	const TriangleMeshGeometryIndexed* bunny,
	const TriangleMeshGeometryIndexed* sliver,
	const TriangleMeshGeometryIndexed* degen )
{
	std::cout << "(a) DIFFERENTIAL vs brute force over every triangle" << std::endl;

	DiffResult acc;
	RunDifferential( sphere, "tessellated sphere (engine TessellateToMesh, detail 48)",
		40000, Scalar( 0.75 ), 12345u, acc );
	if( bunny ) {
		RunDifferential( bunny, "bunny.risemesh",
			40000, Scalar( 0.4 ), 22222u, acc );
	}
	RunDifferential( sliver, "two-triangle sliver",
		15000, Scalar( 1.0 ), 33333u, acc );
	RunDifferential( degen, "degenerate (collinear / coincident) triangles",
		15000, Scalar( 1.0 ), 44444u, acc );

	std::cout << "      TOTAL: " << ( acc.nCompared + acc.nBothRefused + acc.nDisagreeYN )
		<< " points, " << acc.nCompared << " answered, "
		<< acc.nExactBits << " bit-identical, max|diff| " << acc.maxAbsDiff << std::endl;

	const unsigned int total = acc.nCompared + acc.nBothRefused + acc.nDisagreeYN;
	Check( total >= 100000u, "(a) the differential ran on at least 10^5 random points" );
	Check( acc.nCompared > 0, "(a) ...and a non-trivial share of them were actually ANSWERED" );
	Check( acc.nBothRefused > 0, "(a) ...and a non-trivial share REFUSED, so the cut-off was exercised too" );
	Check( acc.maxAbsDiff == 0.0, "(a) MONEY: max |traversal - brute force| over every point is EXACTLY 0" );
	Check( acc.nExactBits == acc.nCompared, "(a) ...every single answered point, not just on average" );
}

//======================================================================
// (b) THE CLOSED FORM
//======================================================================

//! A tessellated sphere is an INSCRIBED polyhedron: every vertex is on the
//! true sphere, every face chord is inside it.  So for a point at radius
//! `t > R` the exact mesh distance `d` is bracketed with NO appeal to a
//! chord-error formula:
//!
//!   lower: the mesh is inside the ball of radius R, so  d >= t - R.
//!   upper: the mesh is a closed star-shaped surface about the origin, so
//!          the ray from the origin through p meets it at some q with
//!          |q| >= rho, hence  d <= |p - q| = t - |q| <= t - rho,
//!          where rho = min over the MESH of |q| -- the polyhedron's
//!          inradius.
//!
//! `rho` is not quoted, it is MEASURED here with the same point-triangle
//! formula (it is exactly the distance from the origin to the mesh), so
//! the bound `R - rho` reported below is a property of the fixture that
//! was actually built, not of one somebody assumed.
static void TestSphereClosedForm( const TriangleMeshGeometryIndexed* sphere, const Scalar R )
{
	std::cout << "(b) closed form: the tessellated sphere against |p| - R" << std::endl;

	Scalar rho = Scalar( 0 );
	const bool okRho = BruteForceDistance( sphere, Point3( 0, 0, 0 ), RISE_INFINITY, rho );
	Check( okRho, "(b) the polyhedron's inradius rho is measurable from the mesh itself" );

	const Scalar sag = R - rho;
	std::cout << "      R = " << (double)R << ", measured inradius rho = " << (double)rho
		<< "  ->  tessellation bound R - rho = " << (double)sag << std::endl;
	Check( sag > Scalar( 0 ) && sag < Scalar( 0.01 ) * R,
		"(b) the bound is positive (an inscribed mesh really is inside) and under 1% of R at detail 48" );

	// Six directions, three radii each: the bracket must hold everywhere,
	// not only on an axis where a vertex happens to sit.
	const Vector3 dirs[6] = {
		Vector3( 1, 0, 0 ), Vector3( -1, 0, 0 ),
		Vector3( 0, 1, 0 ), Vector3( 0, -1, 0 ),
		Vector3( 0, 0, 1 ), Vector3( 0.5773502691896258, 0.5773502691896258, 0.5773502691896258 ) };
	const Scalar ts[3] = { R * Scalar( 1.5 ), R * Scalar( 3 ), R * Scalar( 10 ) };

	double worst = 0.0;
	bool allIn = true;
	for( int i = 0; i < 6; ++i ) {
		for( int j = 0; j < 3; ++j ) {
			const Point3 p( dirs[i].x * ts[j], dirs[i].y * ts[j], dirs[i].z * ts[j] );
			Scalar d = Scalar( 0 );
			if( !sphere->DistanceToSurface( p, RISE_INFINITY, d ) ) { allIn = false; continue; }
			const Scalar exact = ts[j] - R;
			const double over = (double)( d - exact );
			if( over < -1e-12 || over > (double)sag + 1e-12 ) allIn = false;
			if( over > worst ) worst = over;
		}
	}
	std::cout << "      worst over-report against |p| - R across 18 probes: " << worst
		<< "  (bound " << (double)sag << ")" << std::endl;
	Check( allIn, "(b) MONEY: every probe lands in [ |p| - R , |p| - R + (R - rho) ]" );
	Check( worst <= (double)sag,
		"(b) ...and the worst one is inside the measured tessellation bound" );
	Check( worst > 0.0,
		"(b) ...and it is NOT zero, i.e. the fixture really is a polyhedron and the bound is doing work" );
}

//======================================================================
// (c) THE RADIUS CUT-OFF
//======================================================================

//! The design's §10 fixes the reported range at `[0, maxDist)`: zero is
//! attained, the radius itself is EXCLUDED.  This drives that boundary
//! from both sides at the exact value, using a distance the test first
//! measures so the boundary really is AT r and not near it.
static void TestRadiusCutoff( const TriangleMeshGeometryIndexed* sphere )
{
	std::cout << "(c) the maxDist cut-off, exactly at r" << std::endl;

	const Point3 p( 0, 0, 5 );
	Scalar D = Scalar( 0 );
	Check( sphere->DistanceToSurface( p, RISE_INFINITY, D ), "(c) the probe point has a measurable distance" );

	Scalar d = Scalar( 0 );
	Check( !sphere->DistanceToSurface( p, D, d ),
		"(c) MONEY: maxDist EXACTLY equal to the distance REFUSES (the range is half-open)" );

	const Scalar justOver = std::nextafter( (double)D, 1e300 );
	Check( sphere->DistanceToSurface( p, justOver, d ),
		"(c) ...and one ulp above it answers" );
	CheckClose( d, D, Scalar( 0 ), "(c) ...with the same value it gave unbounded" );

	const Scalar justUnder = std::nextafter( (double)D, -1e300 );
	Check( !sphere->DistanceToSurface( p, justUnder, d ),
		"(c) ...and one ulp below it refuses" );

	Check( !sphere->DistanceToSurface( p, Scalar( 0 ), d ),
		"(c) a zero radius refuses rather than answering 0" );
	Check( !sphere->DistanceToSurface( p, Scalar( -1 ), d ),
		"(c) a negative radius refuses" );
}

//======================================================================
// (d) THE DISPLACED-GEOMETRY FORWARD
//======================================================================

//! The row that makes the mesh path worth having on procedural geometry:
//! `displaced_geometry` bakes an internal mesh, and the query must measure
//! THAT mesh.  With a CONSTANT height of 1 and `disp_scale = h`, every
//! vertex of a tessellated sphere of radius R moves radially out to R + h,
//! so a probe at radius t reads `t - (R + h)` and not `t - R`.  The two
//! differ by exactly h, which is the whole error an author would eat if
//! this forwarded to the base.
static void TestDisplacedForward()
{
	std::cout << "(d) the displaced-geometry forward" << std::endl;

	const Scalar R = Scalar( 1 );
	const Scalar h = Scalar( 0.25 );

	SphereGeometry* base = new SphereGeometry( R );
	base->addref();
	ConstantFunction2D* height = new ConstantFunction2D( Scalar( 1 ) );
	height->addref();

	DisplacedGeometry* disp = new DisplacedGeometry(
		base, 48u, height, h, false, false, true, 0 );
	disp->addref();

	const Point3 p( 0, 0, 5 );
	Scalar d = Scalar( 0 );
	const bool ok = disp->DistanceToSurface( p, RISE_INFINITY, d );
	Check( ok, "(d) the displaced geometry answers (and realized itself to do it)" );

	const Scalar wantDisp = Scalar( 5 ) - ( R + h );
	const Scalar wantBase = Scalar( 5 ) - R;
	std::cout << "      displaced answers " << (double)d
		<< "  (displaced surface " << (double)wantDisp
		<< ", smooth base would be " << (double)wantBase << ")" << std::endl;

	// Tolerance is the tessellation sagitta of the DISPLACED sphere, the
	// same inscribed-polyhedron argument as (b), generously rounded up at
	// detail 48 -- the point of the check is the 0.25 separation, not the
	// third decimal.
	Check( std::fabs( (double)( d - wantDisp ) ) < 0.01,
		"(d) MONEY: it answers the DISPLACED radius R + h" );
	Check( std::fabs( (double)( d - wantBase ) ) > 0.2,
		"(d) ...and is nowhere near the smooth base's answer, which would under-report by h" );

	disp->release();
	height->release();
	base->release();
}

//======================================================================
// (e) THE RAW FAMILY STILL REFUSES
//======================================================================

static void TestRawRefuses()
{
	std::cout << "(e) the RAW (non-indexed) mesh family still refuses" << std::endl;

	TriangleMeshGeometry* raw = new TriangleMeshGeometry( false );
	raw->addref();
	raw->BeginTriangles();
	Triangle t;
	t.vertices[0] = Point3( 0, 0, 0 );
	t.vertices[1] = Point3( 1, 0, 0 );
	t.vertices[2] = Point3( 0, 1, 0 );
	t.normals[0] = t.normals[1] = t.normals[2] = Vector3( 0, 0, 1 );
	t.coords[0] = t.coords[1] = t.coords[2] = Point2( 0, 0 );
	raw->AddTriangle( t );
	raw->DoneTriangles();

	Scalar d = Scalar( 0 );
	Check( !raw->DistanceToSurface( Point3( 0.25, 0.25, 1.0 ), Scalar( 10 ), d ),
		"(e) MONEY: a RAW mesh with a real triangle a known 1.0 away STILL refuses -- the "
		"Phase-2 row is the INDEXED family only, and the design's table says so" );

	// ...and the point really was in range for the indexed twin, so the
	// refusal above is about the family and not about the probe.
	TriangleMeshGeometryIndexed* idx = new TriangleMeshGeometryIndexed( false, false );
	idx->addref();
	idx->BeginIndexedTriangles();
	idx->AddVertex( Point3( 0, 0, 0 ) );
	idx->AddVertex( Point3( 1, 0, 0 ) );
	idx->AddVertex( Point3( 0, 1, 0 ) );
	idx->AddNormal( Vector3( 0, 0, 1 ) );
	idx->AddTexCoord( Point2( 0, 0 ) );
	IndexedTriangle it;
	it.iVertices[0] = 0; it.iVertices[1] = 1; it.iVertices[2] = 2;
	it.iNormals[0]  = 0; it.iNormals[1]  = 0; it.iNormals[2]  = 0;
	it.iCoords[0]   = 0; it.iCoords[1]   = 0; it.iCoords[2]   = 0;
	idx->AddIndexedTriangle( it );
	idx->DoneIndexedTriangles();

	Scalar di = Scalar( 0 );
	Check( idx->DistanceToSurface( Point3( 0.25, 0.25, 1.0 ), Scalar( 10 ), di ),
		"(e) ...the identical triangle in the INDEXED family answers" );
	CheckClose( di, Scalar( 1 ), Scalar( 1e-12 ), "(e) ...at exactly 1.0" );

	idx->release();
	raw->release();
}

//======================================================================
// (f) CONCURRENCY
//======================================================================

//! Eight threads over the same immutable mesh, each re-running a serial
//! reference set.  The traversal is `const`, mutates nothing, and its
//! scratch stack is thread_local; a shared stack (the obvious way to write
//! it) would corrupt exactly here and nowhere else.
static void TestConcurrency( const TriangleMeshGeometryIndexed* mesh )
{
	std::cout << "(f) eight threads on one mesh agree with serial" << std::endl;

	const unsigned int N = 4000;
	std::vector<Point3> pts;
	pts.reserve( N );
	RandomNumberGenerator rng( 98765u );
	const BoundingBox bb = mesh->GenerateBoundingBox();
	for( unsigned int i = 0; i < N; ++i ) {
		pts.push_back( Point3(
			bb.ll.x - 0.5 + ( bb.ur.x - bb.ll.x + 1.0 ) * rng.CanonicalRandom(),
			bb.ll.y - 0.5 + ( bb.ur.y - bb.ll.y + 1.0 ) * rng.CanonicalRandom(),
			bb.ll.z - 0.5 + ( bb.ur.z - bb.ll.z + 1.0 ) * rng.CanonicalRandom() ) );
	}

	std::vector<Scalar> serial( N, Scalar( -1 ) );
	for( unsigned int i = 0; i < N; ++i ) {
		Scalar d = Scalar( 0 );
		if( mesh->DistanceToSurface( pts[i], RISE_INFINITY, d ) ) serial[i] = d;
	}

	const unsigned int nThreads = 8;
	std::vector< std::vector<Scalar> > out( nThreads, std::vector<Scalar>( N, Scalar( -1 ) ) );
	std::vector<std::thread> ts;
	for( unsigned int t = 0; t < nThreads; ++t ) {
		ts.emplace_back( [&, t]() {
			for( unsigned int i = 0; i < N; ++i ) {
				Scalar d = Scalar( 0 );
				if( mesh->DistanceToSurface( pts[i], RISE_INFINITY, d ) ) out[t][i] = d;
			}
		} );
	}
	for( std::size_t i = 0; i < ts.size(); ++i ) ts[i].join();

	unsigned int mismatches = 0;
	for( unsigned int t = 0; t < nThreads; ++t ) {
		for( unsigned int i = 0; i < N; ++i ) {
			if( out[t][i] != serial[i] ) ++mismatches;
		}
	}
	std::cout << "      " << nThreads << " x " << N << " concurrent queries, "
		<< mismatches << " mismatches against serial" << std::endl;
	Check( mismatches == 0,
		"(f) MONEY: every concurrent answer is BIT-IDENTICAL to the serial one" );
}

//======================================================================
// (h) THE TWO CANDIDATE SOURCES AGREE
//======================================================================

//! `ObjectManager::NearestOtherSurface` has TWO ways to find the objects
//! near a point: a TLAS POINT QUERY when the manager built a top-level BVH,
//! and a FLAT SCAN over the AABB snapshot otherwise.  Everything downstream
//! of the choice is shared (`ProximityCandidateDistance`), but the choice
//! itself decides WHICH objects are asked, and a traversal that pruned one
//! subtree too eagerly would return a larger distance -- an UNDER-painted
//! seam, silent by construction.
//!
//! So this drives BOTH, on the SAME objects, and compares bit for bit.
//! Two managers are built over one shared object set, differing only in
//! `bUseBSPtree`: the tree-backed one walks the TLAS, the other cannot
//! build one and takes the flat scan.  Sharing the objects (rather than
//! rebuilding them) is what makes the comparison exact -- there is no
//! second construction for a float to differ in.
static void TestCandidateSourcesAgree()
{
	std::cout << "(h) the TLAS point query and the flat scan agree, object for object" << std::endl;

	IObjectManager* mgrTree = 0;
	IObjectManager* mgrFlat = 0;
	// nMaxObjectsPerNode 4 with well over 4 objects => the first builds a
	// TLAS.  bUseBSPtree FALSE on the second => it never can.
	Check( RISE_API_CreateObjectManager( &mgrTree, true,  false, 4, 32 ), "(h) a TLAS-backed manager" );
	Check( RISE_API_CreateObjectManager( &mgrFlat, false, false, 4, 32 ), "(h) a scan-only manager" );
	if( !mgrTree || !mgrFlat ) return;

	std::vector<Object*> objs;

	// A spread of families, so the comparison covers a refusing candidate
	// (the mesh past its radius), an exact one, and a solid one.
	for( int i = 0; i < 10; ++i ) {
		SphereGeometry* g = new SphereGeometry( Scalar( 0.4 + 0.05 * i ) );
		Object* o = new Object( g );
		g->release();
		o->SetPosition( Point3( Scalar( i ) * Scalar( 1.3 ) - Scalar( 6 ),
		                        Scalar( ( i % 3 ) ) * Scalar( 0.7 ),
		                        Scalar( ( i % 4 ) ) * Scalar( -0.9 ) ) );
		o->FinalizeTransformations();
		objs.push_back( o );
	}
	TriangleMeshGeometryIndexed* sphereMesh = BuildTessellatedSphere( Scalar( 0.8 ), 24u );
	if( sphereMesh ) {
		Object* o = new Object( sphereMesh );
		o->SetPosition( Point3( 2.5, 1.1, 1.4 ) );
		o->FinalizeTransformations();
		objs.push_back( o );
	}

	// --- FOUR MORE FAMILIES, added so this shared-object differential also
	// covers the neighbour KINDS the design calls out by name rather than
	// only spheres and a mesh (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.2,
	// and ObjectManager::ProximityCandidateDistance's exclusion rules).
	// Agreement between the TLAS point query and the flat scan is close to
	// STRUCTURAL for any single object -- both call the exact same
	// `ProximityCandidateDistance` on the exact same `Object*` -- so what
	// each of these actually exercises is a DIFFERENT hazard:
	Object* infPlaneObj = 0;
	Object* emitterObj  = 0;
	Object* invisObj    = 0;
	Object* anisoObj    = 0;
	// Positions are tracked here rather than read back from the objects
	// (Object has no GetPosition accessor) -- and chosen at z = -10, well
	// clear of the infinite plane's world z = 0 and of the dense sphere
	// cluster's z in {0, -0.9, -1.8, -2.7}, so the targeted probes below
	// exercise exactly one hazard each without the plane or a stray sphere
	// competing as an unintended nearer candidate.
	const Point3 kEmitterPos( -6, 0, -10 );
	const Point3 kInvisPos( -4.7, 0, -10 );
	const Point3 kAnisoPos( 4, -1, -10 );
	{
		// An INFINITE box (+/-RISE_INFINITY, see InfinitePlaneGeometry::
		// GenerateBoundingBox / BoundingBox's default ctor) is the case the
		// TLAS's SAH build could mishandle -- a node straddling +/-inf on
		// every axis is never smaller than any query's running best, so
		// this object must never be skippable by ClosestPointDistance's
		// prune test on ANY node it lands in.  Positioned at world z = 0,
		// inside the probe volume's z range (-4..4), so a meaningful
		// fraction of probes have it as the genuine nearest candidate
		// (distance |z|) rather than a permanent refusal that a broken
		// prune could never be caught missing.
		InfinitePlaneGeometry* g = new InfinitePlaneGeometry( Scalar( 1 ), Scalar( 1 ) );
		infPlaneObj = new Object( g );
		g->release();
		infPlaneObj->FinalizeTransformations();
		objs.push_back( infPlaneObj );

		// An EMITTER, which `ProximityCandidateDistance` must exclude on
		// BOTH sources (docs 5.2's "emitters never count").  Placed to
		// overlap the dense sphere cluster above so it is often the
		// CLOSEST geometry in world space -- if either source's exclusion
		// slipped, that source alone would answer with the emitter's
		// distance and the cross-check below would catch it directly
		// rather than by chance in the N-probe loop.
		SphereGeometry* eg = new SphereGeometry( Scalar( 0.3 ) );
		IMaterial* pBase = 0;
		IPainter* pRad = 0;
		RISE_API_CreateUniformColorPainter( &pRad, RISEPel( 1, 1, 1 ) );
		RISE_API_CreateLambertianMaterial( &pBase, *pRad );
		IMaterial* pLum = 0;
		if( pBase ) {
			RISE_API_CreateLambertianLuminaireMaterial( &pLum, *pRad, *pBase, Scalar( 1 ) );
		}
		emitterObj = new Object( eg );
		eg->release();
		emitterObj->SetPosition( kEmitterPos );
		emitterObj->FinalizeTransformations();
		if( pLum ) emitterObj->AssignMaterial( *pLum );
		safe_release( pLum );
		safe_release( pBase );
		safe_release( pRad );
		objs.push_back( emitterObj );

		// An object hidden via `SetWorldVisible( false )` -- the same
		// exclusion rule the CSG-operand case in ProximitySignalTest (c)
		// exercises, but asked here through BOTH candidate sources over
		// ONE shared object rather than only the flat-scan-only fixture
		// scenes use.
		SphereGeometry* vg = new SphereGeometry( Scalar( 0.3 ) );
		invisObj = new Object( vg );
		vg->release();
		invisObj->SetPosition( kInvisPos );
		invisObj->FinalizeTransformations();
		invisObj->SetWorldVisible( false );
		objs.push_back( invisObj );

		// An ANISOTROPICALLY SCALED sphere: `Object::DistanceToSurface`
		// answers it through the Frobenius/determinant sigma-pair upper
		// bound (see that function's comment) rather than exactly, and
		// that bound is a property of the OBJECT, not of which manager
		// asks -- this is the sphere ProximitySignalTest (d) already
		// checks against the closed form (scale 2 1 1 -> true 2, reported
		// ~4.899), added here so the shared-object cross-check covers the
		// same non-exact family the differential in (a) never touches
		// (that section is mesh-only).
		SphereGeometry* ag = new SphereGeometry( Scalar( 1 ) );
		anisoObj = new Object( ag );
		ag->release();
		anisoObj->SetPosition( kAnisoPos );
		anisoObj->SetStretch( Vector3( 2, 1, 1 ) );
		anisoObj->FinalizeTransformations();
		objs.push_back( anisoObj );
	}

	for( std::size_t i = 0; i < objs.size(); ++i ) {
		char name[32];
		snprintf( name, sizeof( name ), "o%u", (unsigned)i );
		mgrTree->AddItem( objs[i], name );
		mgrFlat->AddItem( objs[i], name );
	}
	mgrTree->PrepareForRendering();
	mgrFlat->PrepareForRendering();

	const unsigned int N = 6000;
	RandomNumberGenerator rng( 5150u );
	unsigned int mismatch = 0, agreedAnswered = 0, agreedFar = 0;
	const Scalar radii[3] = { Scalar( 0.25 ), Scalar( 1.0 ), Scalar( 4.0 ) };
	for( unsigned int i = 0; i < N; ++i ) {
		const Point3 p(
			-8.0 + 14.0 * rng.CanonicalRandom(),
			-2.0 +  6.0 * rng.CanonicalRandom(),
			-4.0 +  8.0 * rng.CanonicalRandom() );
		// Rotate `self` through the set too, so the self-exclusion rule is
		// exercised on both paths and not only the geometry lookup.
		const IObject* self = objs[ i % objs.size() ];
		const Scalar r = radii[ i % 3 ];

		Scalar dT = Scalar( 0 ), dF = Scalar( 0 );
		const bool okT = mgrTree->NearestOtherSurface( p, self, r, dT );
		const bool okF = mgrFlat->NearestOtherSurface( p, self, r, dF );
		if( okT != okF ) { ++mismatch; continue; }
		if( !okT ) { ++agreedFar; continue; }
		if( dT != dF ) ++mismatch; else ++agreedAnswered;
	}
	std::cout << "      " << N << " probes: " << agreedAnswered << " agreed-answered, "
		<< agreedFar << " agreed-far, " << mismatch << " mismatches" << std::endl;
	Check( mismatch == 0,
		"(h) MONEY: every probe reads the SAME distance through the TLAS point query and "
		"the flat scan -- the traversal prunes nothing a linear scan would have found" );
	Check( agreedAnswered > 500 && agreedFar > 500,
		"(h) ...and both outcomes are well represented, so the agreement is not vacuous" );

	// --- FOUR TARGETED PROBES, one per family added above, so each one's
	// specific hazard is checked directly rather than trusted to show up by
	// chance in the N-probe loop.
	{
		// The infinite plane is z = 0 in world space (identity transform;
		// see InfinitePlaneGeometry::DistanceToSurface's own comment: "the
		// plane is z = 0, so the answer is |z|").  (0, 0, 0.05) is a
		// genuine, non-refusing answer at 0.05 -- and BOTH sources must
		// find it: a TLAS build that mishandled its +/-inf box would prune
		// it out of some node and answer with something else (or refuse).
		const Point3 pNearPlane( 0, 0, 0.05 );
		Scalar dT = 0, dF = 0;
		const bool okT = mgrTree->NearestOtherSurface( pNearPlane, objs[0], Scalar( 0.2 ), dT );
		const bool okF = mgrFlat->NearestOtherSurface( pNearPlane, objs[0], Scalar( 0.2 ), dF );
		Check( okT && okF, "(h) the infinite plane answers on BOTH sources (never pruned)" );
		Check( okT && okF && std::fabs( (double)( dT - Scalar( 0.05 ) ) ) < 1e-9,
			"(h) ...at its closed form (|z|), not merely SOME answer" );
		Check( okT && okF && dT == dF,
			"(h) ...and the TLAS and flat-scan answers are bit-identical" );
	}
	{
		// The emitter sits at kEmitterPos, off in its own corner of the
		// probe volume (z = -10) so nothing else is a candidate there; if
		// either source's emitter exclusion were missing, THAT source alone
		// would answer this probe with the emitter's own distance (0, the
		// probe point is its centre) instead of refusing outright.
		Scalar dT = 0, dF = 0;
		const bool okT = mgrTree->NearestOtherSurface( kEmitterPos, objs[0], Scalar( 0.31 ), dT );
		const bool okF = mgrFlat->NearestOtherSurface( kEmitterPos, objs[0], Scalar( 0.31 ), dF );
		Check( !okT && !okF,
			"(h) MONEY: an emitter excludes on BOTH sources -- neither answers from ON TOP of it "
			"at a radius only the emitter itself could satisfy" );
	}
	{
		// Same shape as the emitter check, for `SetWorldVisible( false )`.
		Scalar dT = 0, dF = 0;
		const bool okT = mgrTree->NearestOtherSurface( kInvisPos, objs[0], Scalar( 0.31 ), dT );
		const bool okF = mgrFlat->NearestOtherSurface( kInvisPos, objs[0], Scalar( 0.31 ), dF );
		Check( !okT && !okF,
			"(h) MONEY: a SetWorldVisible(false) object excludes on BOTH sources" );
	}
	{
		// The anisotropic sphere (radius 1, stretch (2,1,1)) probed 3 units
		// out along its LONG axis: the world-space ellipsoid extends 2 units
		// that way (radius 1 x stretch 2), so the TRUE gap is exactly 1, and
		// `Object::DistanceToSurface`'s sigma-pair bound over-reports it --
		// the exact factor does not matter here (ProximitySignalTest (d)
		// pins that number against the closed form for a different sphere);
		// what matters is that BOTH sources read the SAME over-reported
		// bound, because it is a property of the Object, not of which
		// manager asked.  Radius 5 stays well clear of the plane (10 away
		// at kAnisoPos's z) and of every other object in the set.
		const Point3 pAniso( kAnisoPos.x + 3, kAnisoPos.y, kAnisoPos.z );
		Scalar dT = 0, dF = 0;
		const bool okT = mgrTree->NearestOtherSurface( pAniso, objs[0], Scalar( 5 ), dT );
		const bool okF = mgrFlat->NearestOtherSurface( pAniso, objs[0], Scalar( 5 ), dF );
		Check( okT && okF && dT == dF,
			"(h) the anisotropic sphere's sigma-pair upper bound is bit-identical on both sources" );
		Check( okT && dT > Scalar( 0.99 ),
			"(h) ...and it is a real over-report (>= the true 1.0 gap), not an accidental exact hit" );
	}

	mgrFlat->release();
	mgrTree->release();
	if( sphereMesh ) sphereMesh->release();
}

//======================================================================
// (g) SCENE D
//======================================================================

//! THE NUMBERS THE SCENE FILE RECORDS.  Keep these in lockstep with the
//! header of scenes/Tests/Signals/proximity_mesh_contact.RISEscene: the
//! whole point of this section is that a replaced or re-exported asset
//! FAILS here rather than quietly floating the bunny above the plane while
//! every proximity check still passes for the wrong reason.
//!
//! THE METHOD, for each of the two:
//!   BUNNY   scale 1, so its lowest VERTEX touches the plane y = 0 at
//!           `y = -bbox.ll.y`.  x and z are 0 -- the bunny is not moved
//!           laterally, so its contact footprint is wherever its lowest
//!           vertex happens to be, which the test computes rather than
//!           assumes (a bunny's lowest vertex is a foot, not the centre).
//!   DRAGON  scale 0.35, placed so its LOWEST VERTEX lands exactly on the
//!           bunny's HIGHEST VERTEX in world space.  Not "bbox top to bbox
//!           bottom": that only makes the two boxes touch, and leaves the
//!           two SURFACES an unknown distance apart everywhere.  Vertex to
//!           vertex is a single point of genuine contact whose distance is
//!           a closed form -- zero -- which is what lets station 6 assert a
//!           number instead of an ordering.
static const Scalar kBunnyScale    = Scalar( 1.0 );
static const Scalar kBunnyY        = Scalar( -0.0329874 );
static const Scalar kDragonScale   = Scalar( 0.35 );
static const Scalar kDragonX       = Scalar( -0.0318315 );
static const Scalar kDragonY       = Scalar( 0.135862595 );
static const Scalar kDragonZ       = Scalar( -0.014760295 );

struct SceneD
{
	Job*			job = 0;
	IObjectManager*	mgr = 0;
	IObjectPriv*	plane = 0;
};

static bool LoadSceneD( SceneD& out )
{
	const fs::path root = FindRepoRoot();
	if( root.empty() ) return false;
	const fs::path scenePath = root / "scenes" / "Tests" / "Signals" / "proximity_mesh_contact.RISEscene";
	std::ifstream in( scenePath );
	if( !in ) return false;
	std::stringstream ss;
	ss << in.rdbuf();

	Cst::Document doc = Cst::ParseToCst( ss.str() );
	out.job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *out.job, &diags );
	for( std::size_t i = 0; i < diags.size(); ++i ) {
		std::cout << "  scene diagnostic: " << diags[i] << std::endl;
	}
	Check( diags.empty(), "(g) scene D derives with NO diagnostics" );

	out.mgr = out.job->GetObjects();
	if( !out.mgr ) return false;
	out.mgr->PrepareForRendering();
	out.plane = out.mgr->GetItem( "plane" );
	return out.plane != 0;
}

static Scalar ProxAt( const SceneD& s, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo si;
	si.pScene  = s.mgr;
	si.pSelf   = self;
	si.ptWorld = p;
	ExpressionMemo::Invalidate();
	return si.Proximity( r );
}

static void TestSceneD( const fs::path& root )
{
	std::cout << "(g) scene D -- the placement this test OWNS, and the Phase-2 gate" << std::endl;

	// --- The placement, re-derived from the assets themselves.
	TriangleMeshGeometryIndexed* bunny  = LoadRiseMesh( root / "models" / "risemesh" / "bunny.risemesh" );
	TriangleMeshGeometryIndexed* dragon = LoadRiseMesh( root / "models" / "risemesh" / "dragon_small.risemesh" );
	Check( bunny != 0,  "(g) bunny.risemesh loads" );
	Check( dragon != 0, "(g) dragon_small.risemesh loads" );
	if( !bunny || !dragon ) {
		if( bunny ) bunny->release();
		if( dragon ) dragon->release();
		return;
	}

	const BoundingBox bbB = bunny->GenerateBoundingBox();
	const BoundingBox bbD = dragon->GenerateBoundingBox();
	std::cout.precision( 17 );
	std::cout << "      bunny  bbox y [" << (double)bbB.ll.y << ", " << (double)bbB.ur.y << "]" << std::endl;
	std::cout << "      dragon bbox y [" << (double)bbD.ll.y << ", " << (double)bbD.ur.y << "]" << std::endl;

	// The bunny's lowest vertex sits ON the plane y = 0:  y = -S * bbox.ll.y.
	const Scalar derivedBunnyY = -kBunnyScale * bbB.ll.y;
	std::cout << "      derived bunny y  = " << (double)derivedBunnyY
		<< "   (header " << (double)kBunnyY << ")" << std::endl;
	CheckClose( derivedBunnyY, kBunnyY, Scalar( 1e-6 ),
		"(g) MONEY: the bunny's y offset re-derived from the ASSET matches the scene header's number" );

	// The extreme VERTICES -- the lowest of the bunny (its contact with the
	// plane) and the highest of the bunny (the dragon's contact with it).
	// Computed from the vertex array, not the bbox: a bbox corner is not a
	// point ON the surface, and placing a mesh by one puts it wherever the
	// box happens to reach rather than where the model does.
	Point3 bLowest( 0, RISE_INFINITY, 0 );
	Point3 bHighest( 0, -RISE_INFINITY, 0 );
	{
		const VerticesListType& vs = bunny->getVertices();
		for( std::size_t i = 0; i < vs.size(); ++i ) {
			if( vs[i].y < bLowest.y )  bLowest  = vs[i];
			if( vs[i].y > bHighest.y ) bHighest = vs[i];
		}
	}
	Point3 dLowest( 0, RISE_INFINITY, 0 );
	{
		const VerticesListType& vs = dragon->getVertices();
		for( std::size_t i = 0; i < vs.size(); ++i ) {
			if( vs[i].y < dLowest.y ) dLowest = vs[i];
		}
	}

	const Point3 contactWorld(
		kBunnyScale * bLowest.x,
		kBunnyScale * bLowest.y + kBunnyY,
		kBunnyScale * bLowest.z );
	std::cout << "      bunny lowest vertex -> world contact ("
		<< (double)contactWorld.x << ", " << (double)contactWorld.y << ", "
		<< (double)contactWorld.z << ")" << std::endl;
	CheckClose( contactWorld.y, Scalar( 0 ), Scalar( 1e-15 ),
		"(g) ...and that vertex lands EXACTLY on the plane y = 0" );
	// The offset above was derived from GenerateBoundingBox(); this says the
	// box's floor really IS the lowest vertex and not a float-padded value
	// near it, which is what makes the two derivations the same number.
	CheckClose( bLowest.y, bbB.ll.y, Scalar( 1e-15 ),
		"(g) ...and the mesh's bounding box floor IS that vertex, not a padded approximation" );

	// The dragon's translation puts ITS lowest vertex on the bunny's highest.
	const Point3 bunnyTopWorld(
		kBunnyScale * bHighest.x,
		kBunnyScale * bHighest.y + kBunnyY,
		kBunnyScale * bHighest.z );
	const Scalar derivedDragonX = bunnyTopWorld.x - kDragonScale * dLowest.x;
	const Scalar derivedDragonY = bunnyTopWorld.y - kDragonScale * dLowest.y;
	const Scalar derivedDragonZ = bunnyTopWorld.z - kDragonScale * dLowest.z;
	std::cout << "      derived dragon position = ("
		<< (double)derivedDragonX << ", " << (double)derivedDragonY << ", "
		<< (double)derivedDragonZ << ")" << std::endl;
	std::cout << "      header dragon position  = ("
		<< (double)kDragonX << ", " << (double)kDragonY << ", " << (double)kDragonZ << ")" << std::endl;
	CheckClose( derivedDragonX, kDragonX, Scalar( 1e-6 ),
		"(g) MONEY: the dragon's x offset re-derived from the ASSETS matches the scene header" );
	CheckClose( derivedDragonY, kDragonY, Scalar( 1e-6 ),
		"(g) MONEY: ...its y offset too (its lowest vertex on the bunny's highest)" );
	CheckClose( derivedDragonZ, kDragonZ, Scalar( 1e-6 ),
		"(g) MONEY: ...and its z offset" );

	std::cout.precision( 6 );
	const Point3 meshContactWorld = bunnyTopWorld;

	bunny->release();
	dragon->release();

	// --- The scene, and the gate.
	SceneD s;
	if( !LoadSceneD( s ) ) {
		Check( false, "(g) scene D loads and exposes its `plane` receiver" );
		if( s.job ) s.job->release();
		return;
	}

	const Scalar r = Scalar( 0.02 );	// 2 cm; the gate is stated at 2 mm

	// Station 1: on the plane, 1 mm outside the contact footprint, in four
	// directions.  The gate is >= 0.9 within 2 mm of the footprint, which
	// for a foot resting on a plane the local model reads as ~1 at 1 mm.
	const Scalar mm = Scalar( 0.001 );
	const Vector3 outs[4] = {
		Vector3( 1, 0, 0 ), Vector3( -1, 0, 0 ), Vector3( 0, 0, 1 ), Vector3( 0, 0, -1 ) };
	Scalar worstNear = Scalar( 1 );
	for( int i = 0; i < 4; ++i ) {
		const Point3 p( contactWorld.x + outs[i].x * mm, Scalar( 0 ), contactWorld.z + outs[i].z * mm );
		const Scalar v = ProxAt( s, p, s.plane, r );
		std::cout << "      station 1 (" << (double)outs[i].x << "," << (double)outs[i].z
			<< ") 1 mm out: proximity = " << (double)v << std::endl;
		if( v < worstNear ) worstNear = v;
	}
	Check( worstNear >= Scalar( 0.9 ),
		"(g) MONEY (Phase-2 gate): the plane reads >= 0.9 at 1 mm outside the bunny's footprint" );

	// Station 2: the same four directions at 2 mm, the gate's stated
	// distance -- still >= 0.9 (a 2 mm offset from a resting foot is well
	// under the 20 mm radius).
	Scalar worst2mm = Scalar( 1 );
	for( int i = 0; i < 4; ++i ) {
		const Point3 p( contactWorld.x + outs[i].x * 2 * mm, Scalar( 0 ), contactWorld.z + outs[i].z * 2 * mm );
		const Scalar v = ProxAt( s, p, s.plane, r );
		if( v < worst2mm ) worst2mm = v;
	}
	std::cout << "      station 2, worst of four at 2 mm out: " << (double)worst2mm << std::endl;
	Check( worst2mm >= Scalar( 0.9 ),
		"(g) ...and still >= 0.9 at the gate's stated 2 mm" );

	// Station 3: THE LIGHT PANEL.  A rect_light parked 2 mm above the plane
	// is the fixture for the design's "lights never count" rule.  Directly
	// under it the plane must read 0 -- and the number that makes this
	// check meaningful is what it WOULD read if the panel counted: with the
	// panel 2 mm up and r = 20 mm, that is 1 - 0.002/0.02 = 0.9.
	{
		const Point3 p = Point3( Scalar( 0.6 ), Scalar( 0 ), Scalar( 0 ) );
		const Scalar v = ProxAt( s, p, s.plane, r );
		std::cout << "      station 3, under the light panel: proximity = " << (double)v
			<< "  (would be 0.9 if emitters counted)" << std::endl;
		Check( v == Scalar( 0 ),
			"(g) MONEY: the plane reads EXACTLY 0 under the emissive panel -- lights never count" );
	}

	// Station 4: the `casts_shadows FALSE` sphere DOES count.  It sits
	// 3 mm above the plane, so directly under it the plane reads
	// 1 - 0.003/0.02 = 0.85.
	{
		const Point3 p = Point3( Scalar( -0.6 ), Scalar( 0 ), Scalar( 0 ) );
		const Scalar v = ProxAt( s, p, s.plane, r );
		std::cout << "      station 4, under the casts_shadows FALSE sphere: proximity = "
			<< (double)v << "  (closed form 0.85)" << std::endl;
		CheckClose( v, Scalar( 0.85 ), Scalar( 1e-6 ),
			"(g) MONEY: a casts_shadows FALSE neighbour counts, at its exact closed form" );
	}

	// Station 5: open plane, far from everything -- the neutral.
	{
		const Point3 p = Point3( Scalar( 1.8 ), Scalar( 0 ), Scalar( 1.8 ) );
		const Scalar v = ProxAt( s, p, s.plane, r );
		Check( v == Scalar( 0 ), "(g) the open plane reads 0" );
	}

	// Station 6: MESH ON MESH -- the row only a mesh-family query can
	// answer, and the one that pins the DRAGON's placement rather than the
	// bunny's.
	//
	// A TRAP WORTH NAMING, because the first version of this section fell
	// straight into it.  The dragon's lowest vertex sits ON the bunny's
	// highest, so that world point lies on BOTH surfaces.  Asking there with
	// the DRAGON as `self` reads 1 -- but so would asking with the dragon
	// absent, deleted, or collapsed by a degenerate transform, because the
	// BUNNY is also at distance 0 there.  The check would have passed while
	// proving nothing about the dragon at all.  (It did: an early draft
	// wrote the dragon's `scale` as one number into a per-axis DoubleVec3
	// slot, which made it degenerate and invisible, and this station still
	// read 1.0.)
	//
	// So `self` is the BUNNY: with the bunny excluded, the only surface at
	// that point is the dragon's, and a missing or misplaced dragon reads
	// the PLANE 0.135 m below -- far outside the radius, hence 0.
	{
		IObjectPriv* dragonObj = s.mgr->GetItem( "dragon" );
		IObjectPriv* bunnyObj  = s.mgr->GetItem( "bunny" );
		Check( dragonObj != 0, "(g) the dragon object is in the scene" );
		Check( bunnyObj  != 0, "(g) the bunny object is in the scene" );
		if( dragonObj && bunnyObj ) {
			const Scalar v = ProxAt( s, meshContactWorld, bunnyObj, r );
			std::cout << "      station 6, mesh-on-mesh (self = BUNNY, so the answer is the DRAGON): "
				<< "proximity = " << (double)v << std::endl;
			Check( v >= Scalar( 0.999 ),
				"(g) MONEY: with the bunny excluded, the DRAGON is at distance 0 at the shared vertex" );

			// ...and 5 mm BELOW that point the dragon's nearest surface is
			// that same lowest vertex (every other dragon point is higher),
			// so the closed form is exactly 1 - 0.005/0.02 = 0.75.  A
			// signal that read 1 everywhere passes the check above and
			// fails this one.
			const Point3 below( meshContactWorld.x, meshContactWorld.y - Scalar( 0.005 ), meshContactWorld.z );
			const Scalar vBelow = ProxAt( s, below, bunnyObj, r );
			std::cout << "      station 6b, 5 mm below it: proximity = " << (double)vBelow
				<< "  (closed form 0.75)" << std::endl;
			CheckClose( vBelow, Scalar( 0.75 ), Scalar( 1e-6 ),
				"(g) ...and 5 mm below it reads the dragon's own lowest vertex, at its closed form" );

			// And the symmetric reading, with the DRAGON excluded: the
			// bunny is at 0 there too.  Together the two say both meshes
			// really are at that point, which neither alone can.
			const Scalar vSym = ProxAt( s, meshContactWorld, dragonObj, r );
			Check( vSym >= Scalar( 0.999 ),
				"(g) ...and with the dragon excluded the BUNNY is at 0 there, so both meshes are present" );
		}
	}

	s.job->release();
}

//======================================================================

int main( int, char** )
{
	std::cout << "MeshClosestPointTest -- the mesh family's closest-point query "
		<< "(CROSS_OBJECT_PROXIMITY_DESIGN.md Phase 2)" << std::endl;

	const fs::path root = FindRepoRoot();
	if( root.empty() ) {
		std::cout << "  FATAL: cannot locate the repo root (models/risemesh/bunny.risemesh)" << std::endl;
		return 1;
	}

	const Scalar R = Scalar( 1 );
	TriangleMeshGeometryIndexed* sphere = BuildTessellatedSphere( R, 48u );
	if( !sphere ) {
		std::cout << "  FATAL: the engine's sphere tessellation refused" << std::endl;
		return 1;
	}
	TriangleMeshGeometryIndexed* bunny  = LoadRiseMesh( root / "models" / "risemesh" / "bunny.risemesh" );
	TriangleMeshGeometryIndexed* sliver = BuildSliver();
	TriangleMeshGeometryIndexed* degen  = BuildDegenerate();

	Check( bunny != 0, "(a) bunny.risemesh loads for the differential" );

	TestDifferential( sphere, bunny, sliver, degen );
	TestSphereClosedForm( sphere, R );
	TestRadiusCutoff( sphere );
	TestDisplacedForward();
	TestRawRefuses();
	TestConcurrency( bunny ? bunny : sphere );
	TestCandidateSourcesAgree();
	TestSceneD( root );

	if( bunny ) bunny->release();
	sliver->release();
	degen->release();
	sphere->release();

	std::cout << std::endl;
	std::cout << "MeshClosestPointTest: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
