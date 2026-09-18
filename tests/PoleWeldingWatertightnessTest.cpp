//////////////////////////////////////////////////////////////////////
//
//  PoleWeldingWatertightnessTest.cpp
//
//    DL-116 red-proof.  `SphereGeometry::TessellateToMesh` used to give
//    every pole CELL its own distinct (but coincident-position) vertex
//    index instead of welding the pole into one shared vertex, so the
//    polar "quads" degenerated into zero-area triangles whose two
//    non-degenerate wedge edges were each used by only ONE triangle --
//    a genuinely closed sphere read as an OPEN SHEET under an edge-count
//    watertightness check, purely as an artifact of how the tessellator
//    indexed its poles.
//
//    TWO edge-count models are implemented here, because they disagree
//    about where the bug is observable and getting this wrong silently
//    hides the red-proof:
//
//      RawIndexEdgeCounts  -- keys the edge-adjacency map by the RAW
//        per-triangle vertex-array INDEX, no position weld at all.
//        This is the model `TriangleMeshGeometryIndexed::
//        ComputeWatertightness()` (DL-31, debt-prox, not yet on master
//        at the time this test was written) actually uses in
//        production: it recovers each triangle corner's index via
//        POINTER ARITHMETIC into `pPoints`, which is populated 1:1 from
//        the tessellator's output vertex list with no dedup pass. Under
//        THIS model the pole bug is fully visible: a detail=71 sphere
//        pre-fix reports 284 boundary edges (142 from the two
//        degenerate pole fans + 142 from the ordinary, unrelated u=0/u=1
//        seam -- see ExactWeldEdgeCounts below), matching DL-116's own
//        cited evidence exactly; the fix (this file's sibling change to
//        SphereGeometry.cpp) removes exactly the 142 pole-fan edges,
//        leaving the 142 seam-only edges untouched (see next).
//
//      ExactWeldEdgeCounts -- additionally welds vertex POSITIONS by
//        EXACT (bit-for-bit) equality before counting edges.  This
//        collapses a pole row to one canonical position EVEN ON THE
//        UNFIXED tessellator (every column at a pole is bit-identical
//        to every other one -- theta is canonicalized to 0 for the
//        whole row), so it cannot see the pole bug at all (142 -> 142,
//        unchanged by the fix) -- it isolates the SEPARATE, ordinary
//        u=0/u=1 seam residual instead, which is NOT bit-identical
//        (sin(2*pi) != sin(0) in double precision, off by ~1e-16 *
//        radius) and is INTENTIONALLY not welded by the tessellator:
//        the seam's two texcoords genuinely differ (u=0 vs u=1), and
//        DisplacedGeometry's per-vertex displacement pipeline
//        (ApplyDisplacementMapToObject / ApplyScalarHeightToObject)
//        indexes vertices[]/normals[]/coords[] by the SAME index in
//        lockstep -- welding the seam in the tessellator would either
//        duplicate a position under two different (position, uv)
//        identities or corrupt that per-vertex lookup.  See
//        docs/DL20_DL116_PATCH_CURVATURE_AND_POLE_WELDING.md for the
//        full account.  The seam residual is a job for the
//        watertightness CONSUMER's own weld (tolerance or topology
//        aware), not this tessellator -- it affects every UV-wrapped
//        closed tessellated primitive (sphere, torus, cylinder) and is
//        filed separately as DL-136.
//
//    The PRIMARY DL-116 gate below is therefore RawIndexEdgeCounts:
//    sphere boundary edges must drop from 284 (pre-fix, detail=71) to
//    exactly the seam-only baseline (142, matching ExactWeldEdgeCounts
//    exactly once poles no longer contribute) -- i.e. the pole
//    contribution is fully eliminated, not "some improvement".  Torus
//    and (capped) cylinder never had a pole in the first place (torus
//    wraps fully in both directions with no degenerate row; the
//    cylinder's caps already fan from ONE shared center vertex) --
//    their RawIndexEdgeCounts and ExactWeldEdgeCounts agree exactly
//    (poles contribute nothing to weld either way), and this file
//    asserts they are UNCHANGED by the sphere fix (a fix-didn't-spread
//    control, not a red-proof target).
//
//    Run:   ./bin/tests/PoleWeldingWatertightnessTest
//    Build: make -C build/make/rise build-test/PoleWeldingWatertightnessTest
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/CircularDiskGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Polygon.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_Passed = 0;
static int g_Failed = 0;

static void Check( bool cond, const std::string& msg )
{
	if( cond ) {
		g_Passed++;
	} else {
		g_Failed++;
		std::cout << "FAILED: " << msg << std::endl;
	}
}

struct WatertightResult
{
	unsigned int boundaryEdges;
	unsigned int nonManifoldEdges;
	unsigned int degenerateTriangles;
	unsigned int weldedVertexCount;
};

//! RAW-INDEX model: matches TriangleMeshGeometryIndexed::ComputeWatertightness's
//! actual contract (no position weld -- edges are keyed on the vertex-array
//! index recovered from live triangle topology).  See file header.
static WatertightResult RawIndexEdgeCounts(
	const IndexTriangleListType& tris,
	std::size_t                  vertexCount )
{
	WatertightResult result{ 0, 0, 0, static_cast<unsigned int>( vertexCount ) };

	std::unordered_map<std::uint64_t, int> edgeCounts;
	edgeCounts.reserve( tris.size() * 3 );

	for( const IndexedTriangle& t : tris ) {
		unsigned int idx[3] = { t.iVertices[0], t.iVertices[1], t.iVertices[2] };
		bool degenerate = ( idx[0] == idx[1] || idx[1] == idx[2] || idx[2] == idx[0] );
		if( degenerate ) {
			result.degenerateTriangles++;
			continue;
		}
		for( int k = 0; k < 3; ++k ) {
			unsigned int a = idx[k];
			unsigned int b = idx[(k + 1) % 3];
			if( a > b ) std::swap( a, b );
			const std::uint64_t key = ( (std::uint64_t)a << 32 ) | (std::uint64_t)b;
			++edgeCounts[key];
		}
	}

	for( const auto& kv : edgeCounts ) {
		if( kv.second == 1 ) result.boundaryEdges++;
		else if( kv.second > 2 ) result.nonManifoldEdges++;
	}

	return result;
}

struct Point3KeyHash
{
	std::size_t operator()( const Point3& p ) const
	{
		// Bit-exact hash: reinterpret each double's raw bits, no epsilon.
		std::uint64_t bx, by, bz;
		std::memcpy( &bx, &p.x, sizeof( bx ) );
		std::memcpy( &by, &p.y, sizeof( by ) );
		std::memcpy( &bz, &p.z, sizeof( bz ) );
		std::uint64_t h = bx;
		h ^= by + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
		h ^= bz + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
		return static_cast<std::size_t>( h );
	}
};

struct Point3KeyEqual
{
	bool operator()( const Point3& a, const Point3& b ) const
	{
		// Bit-exact compare (not a fuzzy/epsilon compare): two positions
		// weld only if they are IDENTICAL doubles in all three components.
		return std::memcmp( &a.x, &b.x, sizeof( double ) ) == 0 &&
		       std::memcmp( &a.y, &b.y, sizeof( double ) ) == 0 &&
		       std::memcmp( &a.z, &b.z, sizeof( double ) ) == 0;
	}
};

//! EXACT-WELD model: additionally welds vertex positions by bit-for-bit
//! equality before counting edges.  See file header for what this can and
//! cannot see.
static WatertightResult ExactWeldEdgeCounts(
	const IndexTriangleListType& tris,
	const VerticesListType&      vertices )
{
	WatertightResult result{ 0, 0, 0, 0 };

	std::unordered_map<Point3, unsigned int, Point3KeyHash, Point3KeyEqual> weld;
	std::vector<unsigned int> canon( vertices.size() );
	for( std::size_t i = 0; i < vertices.size(); ++i ) {
		const Point3& p = vertices[i];
		auto it = weld.find( p );
		if( it == weld.end() ) {
			unsigned int id = static_cast<unsigned int>( weld.size() );
			weld.emplace( p, id );
			canon[i] = id;
		} else {
			canon[i] = it->second;
		}
	}
	result.weldedVertexCount = static_cast<unsigned int>( weld.size() );

	std::unordered_map<std::uint64_t, int> edgeCounts;
	edgeCounts.reserve( tris.size() * 3 );

	for( const IndexedTriangle& t : tris ) {
		unsigned int idx[3] = {
			canon[ t.iVertices[0] ],
			canon[ t.iVertices[1] ],
			canon[ t.iVertices[2] ]
		};
		bool degenerate = ( idx[0] == idx[1] || idx[1] == idx[2] || idx[2] == idx[0] );
		if( degenerate ) {
			result.degenerateTriangles++;
			continue;
		}
		for( int k = 0; k < 3; ++k ) {
			unsigned int a = idx[k];
			unsigned int b = idx[(k + 1) % 3];
			if( a > b ) std::swap( a, b );
			const std::uint64_t key = ( (std::uint64_t)a << 32 ) | (std::uint64_t)b;
			++edgeCounts[key];
		}
	}

	for( const auto& kv : edgeCounts ) {
		if( kv.second == 1 ) result.boundaryEdges++;
		else if( kv.second > 2 ) result.nonManifoldEdges++;
	}

	return result;
}

static void PrintBoth( const char* label, const WatertightResult& raw, const WatertightResult& weld )
{
	std::cout << "  " << label
	          << "  raw: boundary=" << raw.boundaryEdges
	          << " nonManifold=" << raw.nonManifoldEdges
	          << " degenerate=" << raw.degenerateTriangles
	          << "   exactWeld: boundary=" << weld.boundaryEdges
	          << " nonManifold=" << weld.nonManifoldEdges
	          << " degenerate=" << weld.degenerateTriangles
	          << " welded=" << weld.weldedVertexCount
	          << std::endl;
}

//////////////////////////////////////////////////////////////////////

static void TestSphere( unsigned int detail )
{
	SphereGeometry* pSphere = new SphereGeometry( 1.0 );
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;

	bool ok = pSphere->TessellateToMesh( tris, vertices, normals, coords, detail );
	pSphere->release();
	Check( ok, "sphere TessellateToMesh succeeds at detail=" + std::to_string( detail ) );

	WatertightResult raw  = RawIndexEdgeCounts( tris, vertices.size() );
	WatertightResult weld = ExactWeldEdgeCounts( tris, vertices );
	PrintBoth( ( "sphere detail=" + std::to_string( detail ) ).c_str(), raw, weld );

	// PRIMARY DL-116 GATE: under the model the real production
	// watertightness check actually uses (raw index, no weld), a
	// tessellated sphere must have ZERO degenerate triangles and its
	// boundary-edge count must equal EXACTLY the seam-only baseline that
	// ExactWeldEdgeCounts reports (i.e. the pole's contribution -- and
	// ONLY the pole's contribution -- is fully eliminated).
	Check( raw.degenerateTriangles == 0,
		"sphere detail=" + std::to_string( detail ) + " has 0 degenerate (zero-area pole) triangles" );
	Check( raw.boundaryEdges == weld.boundaryEdges,
		"sphere detail=" + std::to_string( detail )
		+ " raw-index boundary-edge count now equals the seam-only baseline (poles contribute 0 extra)" );
	Check( raw.nonManifoldEdges == 0 && weld.nonManifoldEdges == 0,
		"sphere detail=" + std::to_string( detail ) + " has 0 non-manifold edges under either model" );
	// The remaining seam-only boundary count is real but OUT OF SCOPE for
	// this tessellator (DL-136): assert it is nonzero so a future accidental
	// "fix" that silently changes the seam's own vertex count is noticed
	// here rather than by this test going quietly green for the wrong reason.
	Check( weld.boundaryEdges == 2 * detail,
		"sphere detail=" + std::to_string( detail )
		+ " seam-only residual is exactly 2*detail (DL-136, not this row's scope)" );
}

static void TestTorus( unsigned int detail )
{
	TorusGeometry* pTorus = new TorusGeometry( 2.0, 0.5 );
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;

	bool ok = pTorus->TessellateToMesh( tris, vertices, normals, coords, detail );
	pTorus->release();
	Check( ok, "torus TessellateToMesh succeeds at detail=" + std::to_string( detail ) );

	WatertightResult raw  = RawIndexEdgeCounts( tris, vertices.size() );
	WatertightResult weld = ExactWeldEdgeCounts( tris, vertices );
	PrintBoth( ( "torus detail=" + std::to_string( detail ) ).c_str(), raw, weld );

	// Torus has NO pole (both parametric directions wrap fully; no row ever
	// collapses to one point), so DL-116's bug pattern does not apply here
	// -- this is a NOT-AFFECTED control, not a red-proof target.  raw and
	// exact-weld must agree exactly (nothing for the weld to find beyond
	// what raw-index already sees), and the residual is the u-seam AND
	// v-seam together (DL-136, same as sphere's u-seam).
	Check( raw.boundaryEdges == weld.boundaryEdges && raw.degenerateTriangles == 0,
		"torus detail=" + std::to_string( detail ) + " raw and exact-weld agree (no pole to weld)" );
	Check( raw.boundaryEdges == 4 * detail,
		"torus detail=" + std::to_string( detail ) + " residual is exactly 4*detail (u-seam + v-seam, DL-136)" );
	Check( raw.nonManifoldEdges == 0,
		"torus detail=" + std::to_string( detail ) + " has 0 non-manifold edges" );
}

static void TestCylinder( unsigned int detail, bool capped )
{
	CylinderGeometry* pCyl = new CylinderGeometry( 'z', 1.0, 2.0, capped );
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;

	bool ok = pCyl->TessellateToMesh( tris, vertices, normals, coords, detail );
	pCyl->release();
	Check( ok, std::string( "cylinder TessellateToMesh succeeds at detail=" ) + std::to_string( detail )
		+ ( capped ? " (capped)" : " (uncapped)" ) );

	WatertightResult raw  = RawIndexEdgeCounts( tris, vertices.size() );
	WatertightResult weld = ExactWeldEdgeCounts( tris, vertices );
	PrintBoth( ( std::string( "cylinder detail=" ) + std::to_string( detail ) + ( capped ? " capped" : " uncapped" ) ).c_str(),
		raw, weld );

	if( capped ) {
		// The cylinder has no pole either: its two caps already fan from ONE
		// shared center vertex (CylinderGeometry.cpp already does what
		// DL-116 asks of the sphere), so this is a NOT-AFFECTED control too
		// -- not a red-proof target.  Note raw != exactWeld HERE is expected
		// and is not the DL-116 pattern: the cap rim's fresh vertices and
		// the side wall's row at the same physical location are two
		// SEPARATE array entries that happen to be bit-identical positions
		// (same cos/theta formula evaluated twice), so exact-weld correctly
		// merges them while raw-index (no weld at all) does not -- the
		// residual in both models is the side wall's own u-seam PLUS the
		// side-to-cap crease (a genuine, load-bearing normal discontinuity:
		// the cap's normal and the side's normal legitimately differ at the
		// same position), neither of which is this tessellator's to weld.
		Check( raw.degenerateTriangles == 0 && weld.degenerateTriangles == 0,
			"capped cylinder detail=" + std::to_string( detail ) + " has 0 degenerate triangles under either model" );
		Check( raw.nonManifoldEdges == 0 && weld.nonManifoldEdges == 0,
			"capped cylinder detail=" + std::to_string( detail ) + " has 0 non-manifold edges under either model" );
	} else {
		// An uncapped cylinder is a genuine open tube -- it SHOULD report
		// boundary edges (2*detail of them, one ring at each end) PLUS the
		// side wall's own u-seam.  This is a control, not a red-proof
		// target: it demonstrates the check distinguishes a real open sheet
		// from a false-positive one.
		Check( raw.boundaryEdges == weld.boundaryEdges,
			"uncapped cylinder detail=" + std::to_string( detail ) + " raw and exact-weld agree (no pole to weld)" );
		Check( raw.nonManifoldEdges == 0,
			"uncapped cylinder detail=" + std::to_string( detail ) + " has 0 non-manifold edges" );
	}
}

static void TestDisk( unsigned int detail )
{
	CircularDiskGeometry* pDisk = new CircularDiskGeometry( 1.0, 'z' );
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;

	bool ok = pDisk->TessellateToMesh( tris, vertices, normals, coords, detail );
	pDisk->release();
	Check( ok, "disk TessellateToMesh succeeds at detail=" + std::to_string( detail ) );

	WatertightResult raw  = RawIndexEdgeCounts( tris, vertices.size() );
	WatertightResult weld = ExactWeldEdgeCounts( tris, vertices );
	PrintBoth( ( "disk detail=" + std::to_string( detail ) ).c_str(), raw, weld );

	// DL-116 sibling fix target: the disk's CENTER is the same "whole row
	// collapses to one point, same everything" pattern as the sphere's
	// poles -- confirm it is now fully welded (0 degenerate triangles) and
	// contributes NOTHING extra to the boundary count under either model.
	Check( raw.degenerateTriangles == 0 && weld.degenerateTriangles == 0,
		"disk detail=" + std::to_string( detail ) + " has 0 degenerate (zero-area center) triangles" );
	// UNLIKE sphere/torus/cylinder, a disk is a genuinely OPEN 2D sheet: its
	// outer RIM is a real, expected boundary (detail edges, one per
	// wedge), not a false positive -- so raw-index boundary count must
	// EXCEED the exact-weld baseline by nothing (center fully welded either
	// way) but both must be POSITIVE (the rim) and neither may show a
	// non-manifold edge.
	Check( raw.boundaryEdges == weld.boundaryEdges,
		"disk detail=" + std::to_string( detail ) + " raw and exact-weld agree (center fully welded)" );
	Check( raw.boundaryEdges > 0,
		"disk detail=" + std::to_string( detail ) + " has a nonzero boundary (the genuine open rim)" );
	Check( raw.nonManifoldEdges == 0 && weld.nonManifoldEdges == 0,
		"disk detail=" + std::to_string( detail ) + " has 0 non-manifold edges under either model" );
}

static void TestEllipsoid( unsigned int detail )
{
	// Scalene (all three semi-axes distinct) so this is not secretly
	// exercising the sphere code path under another name.
	EllipsoidGeometry* pEllipsoid = new EllipsoidGeometry( Vector3( 1.5, 0.75, 2.0 ) );
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;

	bool ok = pEllipsoid->TessellateToMesh( tris, vertices, normals, coords, detail );
	pEllipsoid->release();
	Check( ok, "ellipsoid TessellateToMesh succeeds at detail=" + std::to_string( detail ) );

	WatertightResult raw  = RawIndexEdgeCounts( tris, vertices.size() );
	WatertightResult weld = ExactWeldEdgeCounts( tris, vertices );
	PrintBoth( ( "ellipsoid detail=" + std::to_string( detail ) ).c_str(), raw, weld );

	// DL-116 sibling (formerly excused as out-of-scope/owned-elsewhere, closed here): structurally
	// identical to the sphere's pole bug -- EllipsoidGeometry::TessellateToMesh's
	// own `atPole` branch already canonicalizes u=0 for every column at a
	// pole row, so (like the sphere) position, normal, AND texcoord all
	// collapse to one shared value there; the pre-fix tessellator still
	// emitted `detail+1` coincident indices per pole row. Same gate as
	// TestSphere: 0 degenerate triangles, and the raw-index boundary count
	// must equal EXACTLY the seam-only baseline (poles contribute nothing
	// extra).
	Check( raw.degenerateTriangles == 0,
		"ellipsoid detail=" + std::to_string( detail ) + " has 0 degenerate (zero-area pole) triangles" );
	Check( raw.boundaryEdges == weld.boundaryEdges,
		"ellipsoid detail=" + std::to_string( detail )
		+ " raw-index boundary-edge count now equals the seam-only baseline (poles contribute 0 extra)" );
	Check( raw.nonManifoldEdges == 0 && weld.nonManifoldEdges == 0,
		"ellipsoid detail=" + std::to_string( detail ) + " has 0 non-manifold edges under either model" );
	Check( weld.boundaryEdges == 2 * detail,
		"ellipsoid detail=" + std::to_string( detail )
		+ " seam-only residual is exactly 2*detail (DL-136, not this row's scope)" );
}

int main()
{
	std::cout << "=== DL-116: pole-welding watertightness red-proof ===" << std::endl;

	std::cout << "-- Sphere (DL-116 fix target) --" << std::endl;
	TestSphere( 8 );
	TestSphere( 16 );
	TestSphere( 71 );   // the exact detail DL-116's own evidence measured (284 raw boundary edges pre-fix)

	std::cout << "-- Torus (no poles; not-affected control) --" << std::endl;
	TestTorus( 8 );
	TestTorus( 32 );

	std::cout << "-- Cylinder (no poles; caps already single-vertex fans; not-affected control) --" << std::endl;
	TestCylinder( 8, true );
	TestCylinder( 32, true );
	TestCylinder( 8, false );

	std::cout << "-- CircularDisk (DL-116 sibling fix target: single center pole) --" << std::endl;
	TestDisk( 8 );
	TestDisk( 32 );

	std::cout << "-- Ellipsoid (DL-116 sibling fix target: identical pole pattern to Sphere) --" << std::endl;
	TestEllipsoid( 8 );
	TestEllipsoid( 16 );
	TestEllipsoid( 71 );

	std::cout << "Passed: " << g_Passed << "  Failed: " << g_Failed << std::endl;
	return g_Failed == 0 ? 0 : 1;
}
