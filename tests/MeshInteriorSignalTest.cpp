//////////////////////////////////////////////////////////////////////
//
//  MeshInteriorSignalTest.cpp - DL-31 red-proof + regression.
//
//  docs/DEBT_LEDGER.md DL-31: every SOLID family (sphere, box, capped
//  cylinder, the SDFs, the ellipsoid) clamps its own signed field at
//  zero -- "interpenetration is contact" -- so a point buried inside one
//  reads a genuine (negative-then-clamped, or lower-bounded) depth via
//  `IGeometry::SignedDistanceLower`, which is what `interior(r)` is built
//  on (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 5.6, 10).  A triangle mesh
//  is a SHEET: `TriangleMeshGeometryIndexed` never overrode
//  `SignedDistanceLower` at all, so the interface's own refusing default
//  fired for every mesh, closed or not -- a receiver buried inside a
//  closed, watertight mesh neighbour read `interior(r) == 0`, identical
//  to a receiver floating in open air beside it.
//
//  THE FIX (TriangleMeshGeometryIndexed.h/.cpp) adds a real
//  `SignedDistanceLower` for the CLOSED case only:
//    - WATERTIGHTNESS is checked once, at `DoneIndexedTriangles`, by the
//      textbook necessary condition for a closed 2-manifold with no
//      boundary: every edge is shared by EXACTLY two triangles.  An open
//      sheet or a mesh missing a face has boundary edges (count 1); a
//      non-manifold mesh has an edge shared by 3+.  Either way the mesh
//      stays an unsigned-only sheet, exactly as before, and a one-shot
//      diagnostic at build time says why.
//    - THE SIGN, on a certified-watertight mesh, comes from a parity
//      ray-cast against the mesh's own BVH: odd number of GEOMETRIC
//      crossings (both faces, so winding and `bDoubleSided` cannot skew
//      the count) means inside.
//    - THE MAGNITUDE reuses `DistanceToSurface`'s own answer, which this
//      family already computes EXACTLY (a bounded-radius closest-point
//      BVH traversal, not a bound), called with an unbounded budget
//      since `SignedDistanceLower`'s own contract forbids a range
//      refusal.
//
//  THE SECTIONS:
//    (a) A CLOSED, WATERTIGHT cube: interior depth at the centre equals
//        the half-size, sign negative inside, exact both ways, and the
//        answer tracks the true depth off-centre too (not just at one
//        point).
//    (b) An OPEN QUAD (a single-sided sheet, boundary on all four
//        edges): refuses SignedDistanceLower / unsigned proximity
//        unaffected -- unchanged from before this fix.
//    (c) A NON-WATERTIGHT cube (one face's two triangles removed, four
//        boundary edges around the hole): refuses the signed query,
//        unsigned answers as a sheet would -- exactly like (b), just a
//        different way of failing the same check.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( const double got, const double want, const double tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << got
			<< " want " << want << " (tol " << tol << ")" << std::endl;
	}
}

//! Eight corners of an axis-aligned cube, half-size `h`, centred at the
//! origin -- shared by every fixture below (the closed one uses all 12
//! triangles, the non-watertight one drops two of them).
static void CubeCorners( const Scalar h, std::vector<Point3>& v )
{
	v.clear();
	v.push_back( Point3( -h, -h, -h ) );	// 0
	v.push_back( Point3(  h, -h, -h ) );	// 1
	v.push_back( Point3(  h,  h, -h ) );	// 2
	v.push_back( Point3( -h,  h, -h ) );	// 3
	v.push_back( Point3( -h, -h,  h ) );	// 4
	v.push_back( Point3(  h, -h,  h ) );	// 5
	v.push_back( Point3(  h,  h,  h ) );	// 6
	v.push_back( Point3( -h,  h,  h ) );	// 7
}

//! The cube's 12 triangles, two per face -- standard, closed, manifold
//! triangulation (every edge shared by exactly two triangles: the 12
//! cube edges once each from their two adjacent faces, plus the 6
//! face-diagonals once each from the two triangles that share them).
//! Winding is not asserted anywhere in this suite (DL-31's parity test
//! is explicitly winding-independent), so any consistent choice works.
static void CubeTriangles( IndexTriangleListType& tris, bool dropTopFace )
{
	tris.clear();
	auto add = [&]( unsigned int a, unsigned int b, unsigned int c ) {
		IndexedTriangle t;
		t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
		t.iNormals[0] = a;  t.iNormals[1] = b;  t.iNormals[2] = c;
		t.iCoords[0] = a;   t.iCoords[1] = b;   t.iCoords[2] = c;
		tris.push_back( t );
	};
	// Bottom (z = -h)
	add( 0, 1, 2 ); add( 0, 2, 3 );
	// Top (z = +h) -- the face DROPPED in the non-watertight fixture
	if( !dropTopFace ) {
		add( 4, 6, 5 ); add( 4, 7, 6 );
	}
	// Front (y = -h)
	add( 0, 5, 1 ); add( 0, 4, 5 );
	// Back (y = +h)
	add( 3, 2, 6 ); add( 3, 6, 7 );
	// Left (x = -h)
	add( 0, 3, 7 ); add( 0, 7, 4 );
	// Right (x = +h)
	add( 1, 5, 6 ); add( 1, 6, 2 );
}

//! Builds a mesh from explicit corners/triangles, with a trivial
//! per-vertex normal/UV set (unused by this suite; only present because
//! DoneIndexedTriangles' DEBUG integrity check requires index-sized
//! arrays).
static TriangleMeshGeometryIndexed* BuildMesh(
	const std::vector<Point3>& corners, const IndexTriangleListType& tris )
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();

	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	for( std::size_t i = 0; i < corners.size(); ++i ) {
		vertices.push_back( corners[i] );
		normals.push_back( Vector3( 0, 0, 1 ) );	// unused placeholder
		coords.push_back( Point2( 0, 0 ) );			// unused placeholder
	}

	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();
	return mesh;
}

static void TestClosedWatertightCube()
{
	std::cout << "(a) closed, watertight cube -- interior depth, sign, exactness" << std::endl;

	const Scalar h = 2.0;
	std::vector<Point3> corners;
	CubeCorners( h, corners );
	IndexTriangleListType tris;
	CubeTriangles( tris, false );
	TriangleMeshGeometryIndexed* mesh = BuildMesh( corners, tris );

	// --- CENTRE.  True depth to the nearest face is exactly `h`.
	{
		Scalar outSigned = 0.0;
		bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(a) closed cube answers SignedDistanceLower at the centre" );
		std::cout << "    centre: reported " << (double)outSigned
			<< " (want -" << (double)h << "), exact=" << outExact << std::endl;
		Check( outSigned < Scalar( 0 ), "(a) MONEY -- centre reads NEGATIVE (inside), not the pre-fix 0" );
		CheckClose( (double)outSigned, -(double)h, 1e-9,
			"(a) MONEY -- centre depth equals the half-size exactly (-2.0)" );
		Check( outExact, "(a) MONEY -- both magnitude and sign are exact on a watertight mesh (outExact true)" );
	}

	// --- OFF-CENTRE, still inside, near one face: depth to the nearest
	// face (x = +h) is h - x.
	{
		const Scalar x = 1.5;
		Scalar outSigned = 0.0;
		bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( x, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(a) closed cube answers off-centre" );
		CheckClose( (double)outSigned, -(double)( h - x ), 1e-9,
			"(a) off-centre depth tracks the true nearest-face distance, not just a single pinned point" );
		Check( outExact, "(a) off-centre stays exact too" );
	}

	// --- OUTSIDE.  Sign flips positive; magnitude is the same exact
	// unsigned distance DistanceToSurface already gives this family.
	{
		const Scalar x = h + 3.0;
		Scalar outSigned = 0.0;
		bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( x, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(a) closed cube answers outside" );
		Check( outSigned > Scalar( 0 ), "(a) outside reads POSITIVE" );
		CheckClose( (double)outSigned, 3.0, 1e-9, "(a) outside magnitude is the true distance to the nearest face" );
		Check( outExact, "(a) outside stays exact too" );
	}

	// --- Unsigned proximity() is untouched by this fix.  A MESH IS A
	// SHEET on the unsigned side (the class's own header comment): no
	// interpenetration clamp, so the centre reads its honest distance to
	// the nearest triangle -- `h`, the SAME magnitude the new signed
	// query reports (just without the sign), not 0.
	{
		Scalar d = -1.0;
		const bool ok = mesh->DistanceToSurface( Point3( 0, 0, 0 ), Scalar( 1000 ), d );
		Check( ok, "(a) unsigned DistanceToSurface still answers at the centre" );
		CheckClose( (double)d, (double)h, 1e-9,
			"(a) unsigned query is unaffected by this fix: still the mesh family's honest "
			"nearest-triangle distance (no interpenetration clamp on the UNSIGNED side), "
			"equal in MAGNITUDE to the new signed answer" );
	}

	mesh->release();
}

static void TestOpenQuadRefuses()
{
	std::cout << "(b) an open quad (a bare sheet) -- refuses the signed query, unsigned unaffected" << std::endl;

	std::vector<Point3> corners;
	corners.push_back( Point3( -1, -1, 0 ) );
	corners.push_back( Point3(  1, -1, 0 ) );
	corners.push_back( Point3(  1,  1, 0 ) );
	corners.push_back( Point3( -1,  1, 0 ) );

	IndexTriangleListType tris;
	{
		IndexedTriangle t0; t0.iVertices[0]=0; t0.iVertices[1]=1; t0.iVertices[2]=2;
		t0.iNormals[0]=0; t0.iNormals[1]=1; t0.iNormals[2]=2;
		t0.iCoords[0]=0; t0.iCoords[1]=1; t0.iCoords[2]=2;
		IndexedTriangle t1; t1.iVertices[0]=0; t1.iVertices[1]=2; t1.iVertices[2]=3;
		t1.iNormals[0]=0; t1.iNormals[1]=2; t1.iNormals[2]=3;
		t1.iCoords[0]=0; t1.iCoords[1]=2; t1.iCoords[2]=3;
		tris.push_back( t0 ); tris.push_back( t1 );
	}
	TriangleMeshGeometryIndexed* mesh = BuildMesh( corners, tris );

	Scalar outSigned = 0.0;
	bool outExact = true;	// pre-set to catch a function that forgets to clear it
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( !ok, "(b) MONEY -- an open quad REFUSES SignedDistanceLower (unchanged from before this fix)" );
	Check( !outExact, "(b) ...and clears outExact on refusal, per the interface's own default-refusal contract" );

	// Unsigned proximity is exact and unaffected: the query point is
	// exactly on the quad's plane and inside its footprint, so the true
	// distance is 0.
	Scalar d = -1.0;
	const bool okUnsigned = mesh->DistanceToSurface( Point3( 0, 0, 0 ), Scalar( 1000 ), d );
	Check( okUnsigned, "(b) unsigned proximity still answers on the open quad" );
	CheckClose( (double)d, 0.0, 1e-9, "(b) unsigned distance at a point on the quad's own surface is 0, unaffected by this fix" );

	mesh->release();
}

static void TestNonWatertightCubeRefuses()
{
	std::cout << "(c) a cube missing one face (four boundary edges around the hole) -- "
		"refuses the signed query, unsigned unaffected, diagnostic on stderr/log" << std::endl;

	const Scalar h = 2.0;
	std::vector<Point3> corners;
	CubeCorners( h, corners );
	IndexTriangleListType tris;
	CubeTriangles( tris, /*dropTopFace=*/true );	// 10 triangles, not 12
	Check( tris.size() == 10, "(c) fixture really is missing the top face's two triangles" );

	TriangleMeshGeometryIndexed* mesh = BuildMesh( corners, tris );

	Scalar outSigned = 0.0;
	bool outExact = true;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( !ok, "(c) MONEY -- a cube with one face missing REFUSES SignedDistanceLower "
		"(not watertight: 4 boundary edges around the hole), even though it is 'closed' at every other face" );
	Check( !outExact, "(c) ...and clears outExact on refusal" );

	// Unsigned proximity is untouched: still an honest nearest-triangle
	// distance (no interpenetration clamp on this side, same as the
	// closed-cube fixture's own check) -- at the centre, that is `h`,
	// same as the closed cube (the missing top face is farther from the
	// centre than the five faces still present).
	Scalar d = -1.0;
	const bool okUnsigned = mesh->DistanceToSurface( Point3( 0, 0, 0 ), Scalar( 1000 ), d );
	Check( okUnsigned, "(c) unsigned proximity still answers on the non-watertight cube" );
	CheckClose( (double)d, (double)h, 1e-9, "(c) unsigned distance at the centre is unaffected by this fix" );

	// A diagnostic exists at build time for exactly this case -- see
	// ComputeWatertightness' own comment for why it is safe to print
	// (once per authored mesh, not once per interior() query).  Printed
	// to the global log during BuildMesh's DoneIndexedTriangles call
	// above; not independently re-captured here (this suite has no
	// log-sink hook), but its presence is exercised by construction:
	// this fixture's mesh has 4 boundary edges and the production code's
	// only path to `m_bWatertight == false` with boundaryEdges > 0 is
	// through that PrintEx call. The refusal assertions above are this
	// row's actual behavioural pin.

	mesh->release();
}

int main()
{
	std::cout << "=== MeshInteriorSignalTest (DL-31) ===" << std::endl;

	TestClosedWatertightCube();
	TestOpenQuadRefuses();
	TestNonWatertightCubeRefuses();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
