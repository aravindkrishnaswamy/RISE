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
//    (d) COST, reported not asserted (wall-clock is machine-dependent):
//        ns/query for the parity ray-cast on a ~10k-triangle tessellated
//        sphere, since `interior(r)` is a per-shading-point query and
//        the design's own cost sections (docs/CROSS_OBJECT_PROXIMITY_
//        DESIGN.md §5.3, §8.3) measure everything else on this signal
//        in exactly this unit.
//
//  DL-143 (review P1 on the DL-31 fix above): `ComputeWatertightness`
//  keyed edges by the raw POSITION-ARRAY INDEX, which is only the same
//  thing as "the same physical vertex" on a hand-welded mesh -- section
//  (a)'s 8-vertex cube is exactly that, and it is the ONLY shape this
//  file built before this row.  Every per-corner import/tessellation
//  path (`GLTFSceneImporter::BuildGeometryFromPrimitive`'s one-AddVertex-
//  per-glTF-vertex convention, every engine `TessellateToMesh` producer)
//  gives the SAME physical corner a DIFFERENT array slot per triangle
//  that touches it, so DL-31's own edge-count check read a genuinely
//  closed solid as an open sheet on essentially every shipped or
//  imported mesh.  Fixed by welding vertices to a shared id by POSITION
//  (`WeldVertexPositions` in the .cpp) before counting edges.  New
//  sections:
//    (e) MONEY.  A hand-built, FLAT-SHADED 24-vertex cube (6 faces x 4
//        corners, no vertex shared across faces -- the same convention
//        `Box.glb` and every triangle-mesh export with per-face normals
//        uses) reads OPEN under the old index-keyed check and WATERTIGHT
//        under the new position-welded one.
//    (f) DL-116 CLOSED (debt-geom2, 2026-09-17).  This section originally
//        pinned DL-116 as "refined, not closed": at the time it was
//        written (debt-prox, branched before the fix landed),
//        `SphereGeometry::TessellateToMesh` still emitted `detail+1`
//        coincident-position vertices per pole, so DL-143's own
//        position-weld correctly identified each pole ring as ONE
//        vertex, which turned the pole-cell triangles (always zero-area)
//        into DEGENERATE ones -- a more precisely diagnosed refusal, but
//        still a refusal.  `SphereGeometry`/`EllipsoidGeometry::
//        TessellateToMesh` now weld their own pole row to a single
//        shared (position, normal, texcoord) index and skip the wedge
//        triangle that entry would make degenerate (see
//        docs/DL20_DL116_PATCH_CURVATURE_AND_POLE_WELDING.md), so the
//        raw triangle list fed into this mesh no longer contains ANY
//        degenerate triangles at either pole -- DL-143's position-weld
//        (whose tolerance also absorbs the ordinary u-seam's ~1e-16
//        float noise, see DL-136) now finds a genuinely closed
//        2-manifold with zero boundary edges, and `SignedDistanceLower`
//        ANSWERS instead of refusing.  Both `SphereGeometry` and
//        `EllipsoidGeometry` are exercised here.
//    (g) `BoxGeometry::TessellateToMesh` (independently tessellated
//        per-face, no cross-seam sharing -- 96 boundary edges pre-fix at
//        detail=4) DOES weld back to watertight: its face seams agree to
//        float-noise precision, well inside this fix's tolerance.
//    (h) ONE SHIPPED ASSET, end to end: `Box.glb` loaded through the
//        real `GLTFSceneImporter`, answering a real signed interior
//        distance through the exact mechanism `interior(r)` reads from
//        (`SignedDistanceLower`) -- not a synthetic fixture.  Of the
//        four assets named in the review (Avocado, DragonAttenuation,
//        SheenChair, NormalTangentTest), NONE become fully watertight
//        after welding: see this row's own commit message / the ledger
//        for the measured per-primitive boundary/non-manifold counts --
//        each has a genuine remaining defect or open boundary in the
//        source asset itself (a real, not a testing, residual). `Box.glb`
//        and `BoxTextured.glb` (both 24 vertices / 12 triangles, the
//        same per-face convention as (e)) DO close, and are used here.
//
//  DL-136 CLOSURE EVIDENCE (debt-dlweld, 2026-09-17): DL-116's own fix
//  comment predicted that DL-143's `1e-6*bboxDiagonal` weld tolerance,
//  being many orders of magnitude coarser than a UV seam's ~1e-16-scale
//  float noise, would ALSO weld the ordinary (non-pole) u=0/u=1 seam on
//  every tessellated closed UV-wrapped primitive -- sphere, torus, capped
//  cylinder -- even though `PoleWeldingWatertightnessTest.cpp`'s own
//  EXACT-bit-equality local model cannot see that (by design: it isolates
//  the seam residual FROM the pole fix, so it deliberately does not weld
//  it). Sections (i)-(k) verify that prediction against the PRODUCTION
//  path this whole file already exercises (`BeginIndexedTriangles` /
//  `DoneIndexedTriangles` / `SignedDistanceLower`), not a local
//  reimplementation:
//    (i) `TorusGeometry::TessellateToMesh` (no pole at all -- its own
//        u-seam AND v-seam are the entire DL-136 residual) now answers.
//    (j) `CylinderGeometry::TessellateToMesh` (capped; no pole either --
//        its own side-wall u-seam is the residual) now answers.
//    (k) The SAME three geometries wrapped in `DisplacedGeometry` at
//        ZERO displacement: `DisplacedGeometry::SignedDistanceLower`
//        forwards to its own internally-baked `TriangleMeshGeometryIndexed`
//        (`m_pMesh`), built by the identical `TessellateToMesh` call this
//        file already exercises directly, so this exercises DL-136's
//        closure through the SAME wrapper `docs/DL20_DL116_PATCH_
//        CURVATURE_AND_POLE_WELDING.md`'s own sibling audit already
//        traced for DL-143 (`DisplacedGeometry::BuildMesh` -> `m_pBase->
//        TessellateToMesh` -> position-verbatim flattening).
//    (l) A SCALED variant (vertices pre-scaled by 1000x, moving the bbox
//        diagonal and hence `eps` in lockstep) confirms the weld is
//        RELATIVE, not an absolute-epsilon artifact of the specific unit
//        scale these fixtures happen to use.
//
//  DL-150 (found reviewing DL-143's own closure, `debt-dlweld`,
//  2026-09-17): the position weld's `eps` is relative to the WHOLE mesh's
//  bounding box, so it can exceed the physical gap between two genuinely
//  INDEPENDENT open sheets whenever anything else in the same mesh
//  inflates that bbox -- welding them into one false, confidently-signed
//  2-manifold.  Section (m) is the reviewer's own repro (two independently
//  triangulated, opposite-winding quads 0.001 apart, plus one remote
//  vertex that inflates the bbox diagonal to ~1732 so `eps` ~1.7e-3 >
//  0.001): pre-fix this FALSELY certifies watertight and answers a wrong
//  signed depth in the sliver between the quads; post-fix, an
//  orientation-consistency discriminator in `ComputeWatertightness`
//  (see that function's own comment) refuses it.  Section (n) is the
//  DOCUMENTED residual the discriminator cannot catch (two sheets facing
//  the SAME way): a control, not a red-proof target -- it stays falsely
//  certified either way, matching the design doc's own stated limit.
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Importers/GLTFSceneImporter.h"

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

//! Round-2 delta-review follow-up (debt-geom2 @ ebf69c4e): `ComputeWatertightness`
//! has TWO distinct early-return refusal branches -- the boundary/non-manifold
//! edge tally (b)/(c) above exercise, and a SEPARATE, EARLIER one that
//! fires the moment any triangle has a repeated (degenerate, zero-area)
//! corner, before the edge tally even runs (see its own `if( a == b ) {
//! degenerate = true; continue; }` / `if( degenerate ) { ...; return; }`).
//! Before this fixture, the ONLY place that branch was reached in
//! `tests/` at all was through the sphere/disk/ellipsoid pole fans, via
//! this file's own section (f)/(f2) -- and this slice rewrote those to
//! assert SUCCESS (the entire point of the DL-116 fix being that
//! post-weld there are no more degenerate triangles fed into the mesh),
//! which left the degenerate-triangle branch itself with ZERO test
//! coverage anywhere. This fixture pins it directly, independent of
//! DL-116 or any tessellator.
//!
//! Deliberately minimal (3 vertices, 2 triangles) rather than reusing
//! the cube, because a SINGLE degenerate triangle does not actually
//! isolate this branch: its lone self-loop edge (a,a) gets exactly ONE
//! increment, which the edge tally would ALSO flag as a boundary edge
//! even if the `a==b` guard were removed -- so disabling the guard would
//! not change the refusal outcome, and the test would stay green for
//! the wrong reason (see the file's own audit-by-bug-pattern discipline:
//! a red-proof that cannot go red proves nothing). TWO degenerate
//! triangles sharing the SAME repeated vertex make every edge the guard
//! would otherwise ignore pair up to count exactly 2 (self-loop (0,0)
//! from each triangle; (0,1) traversed twice by triangle A alone; (0,2)
//! traversed twice by triangle B alone) -- i.e. with the guard disabled
//! this two-triangle "mesh" reads as a perfectly closed 2-manifold by
//! the edge-count heuristic alone, genuinely flipping `SignedDistanceLower`
//! from refusal to a (bogus) success. That is the real red-proof this
//! row's recipe asks for, verified below by literally disabling the
//! guard and rebuilding (see the commit message).
static void TestDegenerateTriangleRefuses()
{
	std::cout << "(c2) two triangles sharing one repeated vertex -- refuses via ComputeWatertightness's OTHER early-return branch (degenerate triangle), distinct from (b)/(c)'s boundary-edge refusal" << std::endl;

	std::vector<Point3> corners;
	corners.push_back( Point3( 0, 0, 0 ) );	// 0 -- the repeated vertex
	corners.push_back( Point3( 1, 0, 0 ) );	// 1
	corners.push_back( Point3( 0, 1, 0 ) );	// 2

	IndexTriangleListType tris;
	{
		// Triangle A: (0, 0, 1) -- corners 0 and 1 of THIS triangle are
		// both vertex index 0 (a real, in-range index -- this triangle
		// survives DoneIndexedTriangles' _DEBUG bounds check, it is not
		// a hand-forged out-of-range index).
		IndexedTriangle a;
		a.iVertices[0] = 0; a.iVertices[1] = 0; a.iVertices[2] = 1;
		a.iNormals[0]  = 0; a.iNormals[1]  = 0; a.iNormals[2]  = 1;
		a.iCoords[0]   = 0; a.iCoords[1]   = 0; a.iCoords[2]   = 1;
		tris.push_back( a );

		// Triangle B: (0, 0, 2) -- same repeated vertex 0, different
		// third corner. Its own self-loop (0,0) is what pairs up with
		// triangle A's self-loop to count exactly 2 once the guard is
		// disabled (see the fixture's own header comment above).
		IndexedTriangle b;
		b.iVertices[0] = 0; b.iVertices[1] = 0; b.iVertices[2] = 2;
		b.iNormals[0]  = 0; b.iNormals[1]  = 0; b.iNormals[2]  = 2;
		b.iCoords[0]   = 0; b.iCoords[1]   = 0; b.iCoords[2]   = 2;
		tris.push_back( b );
	}

	TriangleMeshGeometryIndexed* mesh = BuildMesh( corners, tris );

	Scalar outSigned = 12345.0;
	bool outExact = true;	// pre-set to catch a function that forgets to clear it
	const bool ok = mesh->SignedDistanceLower( Point3( 0.25, 0.25, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( !ok, "(c2) MONEY -- a degenerate (repeated-vertex) triangle refuses SignedDistanceLower "
		"via ComputeWatertightness's degenerate-triangle branch, not its boundary-edge branch" );
	Check( !outExact, "(c2) ...and clears outExact on refusal" );

	// Unsigned proximity is untouched by this refusal path -- both
	// triangles still answer `PointTriangleDistance`, just via its own
	// degenerate-input behaviour rather than a clean point-to-segment
	// fallback: with a==b, `ab = b-a` is the ZERO vector, which makes the
	// algorithm's "edge region AB" test (a genuine 1-D region on a
	// non-degenerate triangle) fire on this zero-length "edge" instead of
	// falling through to the real (a,c) edge, and its own `den == 0`
	// guard then collapses the projection parameter to `v=0` -- i.e. the
	// closest point it reports is exactly vertex `a` = (0,0,0), not the
	// nearer point on segment (0,0,0)-(1,0,0) a non-degenerate point-to-
	// segment distance would give. This is a pre-existing, documented
	// characteristic of `PointTriangleDistance` on a coincident-corner
	// input (out of this row's scope to change), not a defect this
	// fixture introduces -- pinned here as the actual closed-form value
	// so a future change to that behaviour is noticed rather than
	// silently accepted: distance from (0.25, 0.25, 0) to (0, 0, 0) is
	// sqrt(0.125) = 0.3535533905932738.
	Scalar d = -1.0;
	const bool okUnsigned = mesh->DistanceToSurface( Point3( 0.25, 0.25, 0 ), Scalar( 1000 ), d );
	Check( okUnsigned, "(c2) unsigned proximity still answers with degenerate triangles present" );
	CheckClose( (double)d, 0.3535533905932738, 1e-9,
		"(c2) unsigned distance matches PointTriangleDistance's own coincident-corner behaviour (collapses to vertex 0)" );

	mesh->release();
}

//! DL-143 MONEY red-proof.  Builds a FLAT-SHADED cube: 6 faces, 4
//! vertices EACH (24 total), no vertex shared across faces -- the same
//! per-corner convention `Box.glb` and any per-face-normal export use.
//! Every physical corner of the cube is touched by 3 faces, so it gets 3
//! DISTINCT array slots here, all at the identical position.  Pre-DL-143
//! (position-index keying) every one of the cube's 12 true edges is split
//! into two DIFFERENT keys (one per adjacent face's own local slots),
//! each used once -- 24 "boundary" edges, an open sheet.  Post-DL-143
//! (position-welded keying) the two slots per shared corner collapse back
//! to one id, the two faces' contributions to each true edge become the
//! SAME key, and the mesh reads correctly as the closed cube it always
//! was.
static void TestFlatShadedPerCornerCubeWelds()
{
	std::cout << "(e) MONEY -- a flat-shaded, per-corner 24-vertex cube (Box.glb's own convention) welds to watertight" << std::endl;

	const Scalar h = 2.0;
	std::vector<Point3> corners;
	CubeCorners( h, corners );

	std::vector<Point3> vertices;
	IndexTriangleListType tris;
	auto face = [&]( unsigned int c0, unsigned int c1, unsigned int c2, unsigned int c3 ) {
		// Four BRAND NEW array slots for this face, copied from the
		// shared corner positions -- never reused by any other face,
		// exactly like a per-face-normal glTF export.
		const unsigned int base = (unsigned int)vertices.size();
		vertices.push_back( corners[c0] );
		vertices.push_back( corners[c1] );
		vertices.push_back( corners[c2] );
		vertices.push_back( corners[c3] );
		auto addTri = [&]( unsigned int a, unsigned int b, unsigned int c ) {
			IndexedTriangle t;
			t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
			t.iNormals[0] = a;  t.iNormals[1] = b;  t.iNormals[2] = c;
			t.iCoords[0] = a;   t.iCoords[1] = b;   t.iCoords[2] = c;
			tris.push_back( t );
		};
		addTri( base + 0, base + 1, base + 2 );
		addTri( base + 0, base + 2, base + 3 );
	};
	// Same 8 corner indices and face membership as CubeTriangles above,
	// just each face getting its OWN 4 slots instead of sharing the 8.
	face( 0, 1, 2, 3 );	// bottom (z=-h)
	face( 4, 5, 6, 7 );	// top (z=+h)
	face( 0, 1, 5, 4 );	// front (y=-h)
	face( 3, 2, 6, 7 );	// back (y=+h)
	face( 0, 3, 7, 4 );	// left (x=-h)
	face( 1, 2, 6, 5 );	// right (x=+h)

	Check( vertices.size() == 24, "(e) fixture really has 24 (6x4) per-face vertex slots" );
	Check( tris.size() == 12, "(e) fixture really has 12 triangles" );

	TriangleMeshGeometryIndexed* mesh = BuildMesh( vertices, tris );

	Scalar outSigned = 12345.0;
	bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(e) MONEY -- the flat-shaded per-corner cube now ANSWERS SignedDistanceLower "
		"(pre-DL-143 this refused: 24 boundary edges from an entirely closed solid)" );
	CheckClose( (double)outSigned, -(double)h, 1e-9,
		"(e) MONEY -- centre depth equals the half-size exactly, same as the hand-welded (a) fixture" );
	Check( outExact, "(e) MONEY -- exact, same as the hand-welded (a) fixture" );

	mesh->release();
}

//! DL-116 CLOSED (debt-geom2, 2026-09-17): `SphereGeometry`/
//! `EllipsoidGeometry::TessellateToMesh` now weld every pole ROW to a
//! single shared (position, normal, texcoord) index and skip the one
//! wedge triangle per pole cell that entry would make degenerate (see
//! docs/DL20_DL116_PATCH_CURVATURE_AND_POLE_WELDING.md) -- so the raw
//! triangle list fed into this mesh contains zero degenerate triangles
//! at either pole, DL-143's position-weld finds a genuinely closed
//! 2-manifold (its tolerance also swallows the ordinary u-seam's
//! ~1e-16-scale float noise, DL-136), and `SignedDistanceLower` now
//! ANSWERS instead of refusing on a tessellated sphere or ellipsoid.
//! The magnitude check is a LOOSE bound (matching (d)'s own rationale):
//! the mesh is a polyhedral approximation, not the analytic surface, so
//! its closest-point answer at the centre approaches -- but does not
//! exactly equal -- the true minimum semi-axis / radius.
static void TestSphereTessellationNowWatertight()
{
	std::cout << "(f) DL-116 CLOSED -- SphereGeometry::TessellateToMesh now welds watertight" << std::endl;

	SphereGeometry* pSphere = new SphereGeometry( 3.0 );
	pSphere->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pSphere->TessellateToMesh( tris, vertices, normals, coords, 71 );
	Check( built, "(f) SphereGeometry::TessellateToMesh(detail=71) succeeds" );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(f) MONEY -- SphereGeometry::TessellateToMesh's own output now ANSWERS "
		"SignedDistanceLower post-weld (pre-fix: refused on degenerate pole triangles)" );
	CheckClose( (double)outSigned, -3.0, 0.01 * 3.0,
		"(f) tessellated-sphere centre depth is close to the analytic sphere's -R" );
	Check( outExact, "(f) exact (closed 2-manifold, parity ray-cast sign + exact closest-point magnitude)" );

	mesh->release();
	pSphere->release();
}

//! DL-116 sibling (same fix, same commit, see above): `EllipsoidGeometry::
//! TessellateToMesh` had the IDENTICAL per-pole-row coincident-vertex
//! pattern as `SphereGeometry` (its own comment pointed at Sphere's for
//! the rationale) and is welded the same way here. Scalene (a != b != c)
//! semi-axes so this is not secretly exercising the sphere code path.
//! The origin's true nearest surface point on an ellipsoid is exactly
//! `min(a, b, c)` away (attained along the shortest semi-axis) -- the
//! same closed-form argument as a sphere's `-R`, just anisotropic.
static void TestEllipsoidTessellationNowWatertight()
{
	std::cout << "(f2) DL-116 sibling -- EllipsoidGeometry::TessellateToMesh now welds watertight" << std::endl;

	const Scalar a = 1.5, b = 0.75, c = 2.0;
	EllipsoidGeometry* pEllipsoid = new EllipsoidGeometry( Vector3( a, b, c ) );
	pEllipsoid->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pEllipsoid->TessellateToMesh( tris, vertices, normals, coords, 71 );
	Check( built, "(f2) EllipsoidGeometry::TessellateToMesh(detail=71) succeeds" );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(f2) MONEY -- EllipsoidGeometry::TessellateToMesh's own output now ANSWERS "
		"SignedDistanceLower post-weld (pre-fix: refused on degenerate pole triangles)" );
	const Scalar minAxis = std::min( a, std::min( b, c ) );
	CheckClose( (double)outSigned, -(double)minAxis, 0.02 * (double)minAxis,
		"(f2) tessellated-ellipsoid centre depth is close to the analytic min(a,b,c)" );
	Check( outExact, "(f2) exact (closed 2-manifold, parity ray-cast sign + exact closest-point magnitude)" );

	mesh->release();
	pEllipsoid->release();
}

//! `BoxGeometry::TessellateToMesh` tessellates its six faces
//! INDEPENDENTLY -- each face computes its own vertex grid from its own
//! (origin, edgeU, edgeV) basis, with no explicit seam sharing -- so
//! pre-DL-143 it read as open (96 boundary edges at detail=4, one full
//! unshared perimeter per face: this is the SAME underlying pattern as
//! (e) and DL-116, just at every seam rather than only the two poles).
//! The seam positions agree to float-noise precision (both sides compute
//! the same real-valued point from consistent inputs), well inside
//! DL-143's `1e-6 * bboxDiagonal` weld tolerance, so this DOES close.
static void TestBoxTessellationWelds()
{
	std::cout << "(g) BoxGeometry::TessellateToMesh (independently-tessellated faces) welds to watertight" << std::endl;

	const Scalar halfExtent = 2.0;	// BoxGeometry(w,h,d) takes FULL extents
	BoxGeometry* pBox = new BoxGeometry( 2.0 * halfExtent, 2.0 * halfExtent, 2.0 * halfExtent );
	pBox->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pBox->TessellateToMesh( tris, vertices, normals, coords, 4 );
	Check( built, "(g) BoxGeometry::TessellateToMesh(detail=4) succeeds" );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(g) MONEY -- the independently-tessellated box now ANSWERS SignedDistanceLower "
		"(pre-DL-143: 96 boundary edges, one unshared perimeter per face)" );
	CheckClose( (double)outSigned, -(double)halfExtent, 1e-6,
		"(g) centre depth equals the half-extent (tessellation is exact for a box, no polyhedral-approximation slack)" );
	Check( outExact, "(g) exact" );

	mesh->release();
	pBox->release();
}

//! (h) ONE SHIPPED ASSET, end to end.  `Box.glb` (the same asset
//! `GLTFLoaderTest` already covers for its OWN, unrelated reasons: 24
//! vertices, 6 faces x 4 corners, no vertex shared between faces -- see
//! that test's own comment) is loaded through the REAL importer, and the
//! resulting mesh answers a REAL signed interior distance through the
//! exact mechanism `interior(r)` is built on (`SignedDistanceLower`),
//! not a synthetic fixture built by this test file. Of the four assets
//! named in the DL-143 review, NONE fully weld to watertight -- each has
//! a genuine remaining defect (measured, not asserted here, since these
//! are real third-party asset properties, not this fix's contract):
//!   Avocado             38 boundary edges  (was 124 pre-weld)
//!   DragonAttenuation (Cloth Backdrop, mesh 0)  617 boundary edges (a
//!                        real open cloth sheet -- correctly stays open)
//!   DragonAttenuation (Dragon, mesh 1)          0 boundary, 6 NON-
//!                        MANIFOLD edges (a genuine small defect in the
//!                        source asset -- refuses for a real reason)
//!   SheenChair (4 primitives)   400 / 528 / 384 / 32 boundary edges
//!   NormalTangentTest    128 boundary edges (UNCHANGED from pre-weld --
//!                        this asset's 128 boundary edges are not a
//!                        welding artifact at all; it stays open either
//!                        way)
//! `Box.glb` and `BoxTextured.glb`, by contrast, use the exact per-face
//! convention (e) tests and both close.
static void TestShippedGltfBoxEndToEnd()
{
	std::cout << "(h) Box.glb loaded through GLTFSceneImporter -- end-to-end interior() through a real shipped asset" << std::endl;

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();

	GLTFSceneImporter imp( "scenes/Tests/Geometry/assets/Box.glb" );
	Check( imp.IsValid(), "(h) Box.glb parses -- is the asset committed, and is the test running from the repo root?" );

	const bool built = imp.BuildGeometryFromPrimitive( mesh, 0, 0, false );
	Check( built, "(h) Box.glb builds into a TriangleMeshGeometryIndexed" );

	if( built ) {
		// Box.glb is the standard glTF-Sample-Assets unit cube: half-
		// extent 0.5, centred at the origin.
		Scalar outSigned = 12345.0; bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(h) MONEY -- a REAL shipped glTF asset, loaded through the REAL importer, "
			"now answers a real interior() signed distance end to end" );
		CheckClose( (double)outSigned, -0.5, 1e-6,
			"(h) centre depth equals the unit cube's half-extent (-0.5)" );
		Check( outExact, "(h) exact" );

		// Off-centre, still inside: the same shape of check section (a)
		// runs on the hand-built cube, now through a real import.
		Scalar outSigned2 = 12345.0; bool outExact2 = false;
		const bool ok2 = mesh->SignedDistanceLower( Point3( 0.3, 0, 0 ), Scalar( 1000 ), outSigned2, outExact2 );
		Check( ok2, "(h) off-centre interior query on the real asset answers" );
		CheckClose( (double)outSigned2, -(0.5 - 0.3), 1e-6,
			"(h) off-centre depth on the real asset tracks the true nearest-face distance" );
		Check( outExact2, "(h) off-centre stays exact too" );
	}

	mesh->release();
}

//! DL-136 CLOSURE, section (i).  `TorusGeometry::TessellateToMesh` has NO
//! pole at all -- both parametric directions wrap fully -- so its entire
//! DL-136 residual is the ordinary u-seam + v-seam (`sin(2*pi) != sin(0)`
//! at ~1e-16 relative scale), a genuine texcoord discontinuity but not a
//! genuine POSITION discontinuity.  DL-143's weld tolerance
//! (`1e-6*bboxDiagonal`) is many orders of magnitude coarser than that
//! float noise, so it should weld the seam too -- verified here through
//! the PRODUCTION path (`SignedDistanceLower`), not
//! `PoleWeldingWatertightnessTest.cpp`'s own local, deliberately-exact-bit
//! edge-count model (which is why that file's own torus/cylinder sections
//! still correctly report a nonzero seam-only residual: they are pinning
//! a DIFFERENT, EXACT-equality model, not this one).  Probe point is the
//! tube's own central-circle point `(R, 0, 0)`, whose true nearest surface
//! distance is exactly the tube radius `r` (see
//! `TorusGeometry::DistanceToSurface`'s own comment for the ring-in-XZ,
//! tube-along-Y convention this matches); the tessellated mesh's answer is
//! a polyhedral approximation (inscribed-polygon apothem), hence the loose
//! tolerance.
static void TestTorusTessellationWatertight()
{
	std::cout << "(i) DL-136 CLOSED -- TorusGeometry::TessellateToMesh welds its u/v seam to watertight" << std::endl;

	const Scalar R = 2.0, r = 0.5;
	TorusGeometry* pTorus = new TorusGeometry( R, r );
	pTorus->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pTorus->TessellateToMesh( tris, vertices, normals, coords, 32 );
	Check( built, "(i) TorusGeometry::TessellateToMesh(detail=32) succeeds" );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( R, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(i) MONEY -- TorusGeometry::TessellateToMesh's own output ANSWERS SignedDistanceLower "
		"through the PRODUCTION weld (DL-136 residual on this primitive is the seam only, which welds)" );
	CheckClose( (double)outSigned, -(double)r, 0.05 * (double)r,
		"(i) tessellated-torus tube-centre depth is close to the analytic tube radius" );
	Check( outExact, "(i) exact (closed 2-manifold, parity ray-cast sign + exact closest-point magnitude)" );

	mesh->release();
	pTorus->release();
}

//! DL-136 CLOSURE, section (j).  `CylinderGeometry::TessellateToMesh`
//! (capped) has no pole either -- its two caps already fan from one
//! shared center vertex (pre-existing, not a DL-116 target) -- so its
//! DL-136 residual is the side wall's own u-seam plus the side-to-cap
//! crease (a genuine, load-bearing NORMAL discontinuity, but not a
//! position one: same `cos`/`sin` formula on both sides of the crease).
//! Probe point is the axis origin; the cylinder is built radius=1,
//! height=2 (axis symmetric about the origin, `CylinderGeometry`'s own
//! convention), so the analytic nearest surface from the origin is
//! `min(radius, height/2) = 1.0`, attained by BOTH the side wall and the
//! caps -- the polyhedral side wall under-reads this by the inscribed-
//! apothem factor `cos(pi/detail)`, hence the loose tolerance.
static void TestCappedCylinderTessellationWatertight()
{
	std::cout << "(j) DL-136 CLOSED -- capped CylinderGeometry::TessellateToMesh welds its seam to watertight" << std::endl;

	const Scalar radius = 1.0, height = 2.0;
	const unsigned int detail = 32;
	CylinderGeometry* pCyl = new CylinderGeometry( 'z', radius, height, /*capped=*/true );
	pCyl->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pCyl->TessellateToMesh( tris, vertices, normals, coords, detail );
	Check( built, "(j) capped CylinderGeometry::TessellateToMesh(detail=32) succeeds" );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(j) MONEY -- capped CylinderGeometry::TessellateToMesh's own output ANSWERS SignedDistanceLower "
		"through the PRODUCTION weld (DL-136 residual on this primitive is the side u-seam + cap crease, which weld)" );
	const Scalar apothem = radius * std::cos( PI / (Scalar)detail );
	const Scalar expected = std::min( apothem, height / Scalar( 2 ) );
	CheckClose( (double)outSigned, -(double)expected, 0.02 * (double)radius,
		"(j) tessellated-cylinder centre depth tracks min(inscribed side apothem, half-height)" );
	Check( outExact, "(j) exact (closed 2-manifold, parity ray-cast sign + exact closest-point magnitude)" );

	mesh->release();
	pCyl->release();
}

//! A displacement of exactly zero, used to wrap sphere/torus/cylinder in
//! `DisplacedGeometry` for section (k) without perturbing their positions
//! at all -- the point is to exercise DL-136's closure through the
//! WRAPPER's own baked mesh (`DisplacedGeometry::SignedDistanceLower`
//! forwards to `m_pMesh->SignedDistanceLower`), not to test displacement.
class ZeroFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar /*x*/, const Scalar /*y*/ ) const { return Scalar( 0 ); }
};

//! DL-136 CLOSURE, section (k).  Wraps each of Sphere/Torus/(capped)
//! Cylinder in `DisplacedGeometry` at zero displacement and confirms the
//! WRAPPER answers a real signed interior distance too -- not just the
//! bare tessellator this file already tests directly in (f)/(i)/(j).
//! `DisplacedGeometry::BuildMesh` bakes via `m_pBase->TessellateToMesh`
//! and copies positions VERBATIM (see `DisplacedGeometry.h`'s own class
//! comment and `docs/DL20_DL116_PATCH_CURVATURE_AND_POLE_WELDING.md`'s
//! sibling-audit correction on this exact point), so the baked mesh's
//! topology and DL-136 residual are identical to the un-wrapped case --
//! this section exists to prove that identity holds through the real
//! wrapper class, not to re-derive it.
static void TestDisplacedWrapsWatertight()
{
	std::cout << "(k) DL-136 CLOSED -- DisplacedGeometry(disp=0) wrapping sphere/torus/cylinder welds to watertight" << std::endl;

	ZeroFunction2D zeroFn;
	zeroFn.addref();

	{
		SphereGeometry* pBase = new SphereGeometry( 3.0 );
		pBase->addref();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pBase, 31, &zeroFn, Scalar( 0 ), false, false );
		pDisp->addref();
		pDisp->Realize();

		Scalar outSigned = 12345.0; bool outExact = false;
		const bool ok = pDisp->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(k) MONEY -- DisplacedGeometry(sphere, disp=0) ANSWERS SignedDistanceLower through its own baked mesh" );
		CheckClose( (double)outSigned, -3.0, 0.05,
			"(k) displaced-sphere-wrap centre depth is close to the analytic sphere's -R" );
		Check( outExact, "(k) sphere wrap exact" );

		pDisp->release();
		pBase->release();
	}
	{
		const Scalar R = 2.0, r = 0.5;
		TorusGeometry* pBase = new TorusGeometry( R, r );
		pBase->addref();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pBase, 32, &zeroFn, Scalar( 0 ), false, false );
		pDisp->addref();
		pDisp->Realize();

		Scalar outSigned = 12345.0; bool outExact = false;
		const bool ok = pDisp->SignedDistanceLower( Point3( R, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(k) MONEY -- DisplacedGeometry(torus, disp=0) ANSWERS SignedDistanceLower through its own baked mesh" );
		CheckClose( (double)outSigned, -(double)r, 0.05 * (double)r,
			"(k) displaced-torus-wrap tube-centre depth is close to the analytic tube radius" );
		Check( outExact, "(k) torus wrap exact" );

		pDisp->release();
		pBase->release();
	}
	{
		const Scalar radius = 1.0, height = 2.0;
		const unsigned int detail = 32;
		CylinderGeometry* pBase = new CylinderGeometry( 'z', radius, height, /*capped=*/true );
		pBase->addref();
		DisplacedGeometry* pDisp = new DisplacedGeometry( pBase, detail, &zeroFn, Scalar( 0 ), false, false );
		pDisp->addref();
		pDisp->Realize();

		Scalar outSigned = 12345.0; bool outExact = false;
		const bool ok = pDisp->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(k) MONEY -- DisplacedGeometry(capped cylinder, disp=0) ANSWERS SignedDistanceLower through its own baked mesh" );
		const Scalar apothem = radius * std::cos( PI / (Scalar)detail );
		const Scalar expected = std::min( apothem, height / Scalar( 2 ) );
		CheckClose( (double)outSigned, -(double)expected, 0.02 * (double)radius,
			"(k) displaced-cylinder-wrap centre depth tracks min(inscribed side apothem, half-height)" );
		Check( outExact, "(k) cylinder wrap exact" );

		pDisp->release();
		pBase->release();
	}

	zeroFn.release();
}

//! DL-136 CLOSURE, section (l).  Confirms the weld is genuinely RELATIVE
//! to the mesh's own scale, not an absolute-epsilon artifact that happens
//! to work at the unit scale every other section here uses: the SAME
//! sphere tessellation, with every vertex position pre-scaled by 1000x
//! (moving the bbox diagonal -- and hence `eps = max(1e-9,
//! 1e-6*diagonal)` -- by the same factor), must weld its seam and answer
//! SignedDistanceLower at the correspondingly-scaled depth.  Normals are
//! untouched (a uniform position scale does not change unit face
//! directions), matching how `TessellateToMesh` itself would produce them
//! at that scale.
static void TestScaledMeshWeldsRelatively()
{
	std::cout << "(l) DL-136 sibling -- a 1000x-scaled tessellated sphere still welds (relative epsilon)" << std::endl;

	const Scalar R = 3.0;
	const Scalar kScale = 1000.0;
	SphereGeometry* pSphere = new SphereGeometry( R );
	pSphere->addref();
	IndexTriangleListType tris;
	VerticesListType vertices;
	NormalsListType normals;
	TexCoordsListType coords;
	const bool built = pSphere->TessellateToMesh( tris, vertices, normals, coords, 31 );
	Check( built, "(l) SphereGeometry::TessellateToMesh(detail=31) succeeds" );

	for( std::size_t i = 0; i < vertices.size(); ++i ) {
		vertices[i] = Point3( vertices[i].x * kScale, vertices[i].y * kScale, vertices[i].z * kScale );
	}

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000000 ), outSigned, outExact );
	Check( ok, "(l) MONEY -- a 1000x-scaled tessellated sphere still ANSWERS SignedDistanceLower "
		"(eps scales with the mesh's own bbox diagonal, not a fixed absolute constant)" );
	CheckClose( (double)outSigned, -(double)( R * kScale ), 0.01 * (double)( R * kScale ),
		"(l) scaled-sphere centre depth tracks -R*scale" );
	Check( outExact, "(l) exact at the scaled size too" );

	mesh->release();
	pSphere->release();
}

//! DL-150.  Two independently-triangulated QUADS with OPPOSITE winding
//! (quad A faces +Z, quad B faces -Z -- i.e. they face EACH OTHER across
//! a 0.001-unit gap), plus one remote vertex far away that inflates this
//! mesh's own bounding-box diagonal to ~1732 -- so DL-143's weld epsilon
//! (`1e-6*diagonal` ~= 1.7e-3) EXCEEDS the physical gap between the two
//! quads, which are otherwise totally unrelated open sheets.  Each quad's
//! own 4 boundary edges pair up 1:1 with the other's after the weld
//! (their shared diagonal stays internal to each quad, unaffected), so
//! the pre-DL-150 edge-count check alone reads this as a genuinely closed
//! 2-manifold and `SignedDistanceLower` answers a confident WRONG signed
//! depth for a point in the sliver between the quads.  The remote vertex
//! is folded into one degenerate (zero-area) triangle with two of quad
//! A's own vertices purely to keep it part of THIS mesh's position array
//! (and hence its bounding box) without adding any real edges of its own
//! to the watertightness count -- `ComputeWatertightness`'s existing
//! degenerate-triangle branch would normally refuse outright on that, so
//! this fixture instead gives the remote point ITS OWN small but genuinely
//! non-degenerate closed tetrahedron, which contributes 0 boundary/
//! non-manifold edges of its own and does not interact with the quads'
//! edges at all (disjoint vertex ids) -- exactly the reviewer's own repro
//! shape (two false-stitched quads sharing a mesh with an unrelated,
//! genuinely closed remote component).
static void AddTri( IndexTriangleListType& tris, unsigned int a, unsigned int b, unsigned int c )
{
	IndexedTriangle t;
	t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
	t.iNormals[0]  = a; t.iNormals[1]  = b; t.iNormals[2]  = c;
	t.iCoords[0]   = a; t.iCoords[1]   = b; t.iCoords[2]   = c;
	tris.push_back( t );
}

//! Two SINGLE triangles (no internal diagonal, hence no way for this
//! fixture to trip the PRE-EXISTING DL-31 edge-count check by accident --
//! that confound is exactly why an earlier revision of this fixture used
//! two-triangle QUADS sharing a diagonal, discovered on review to
//! already refuse via DL-31 alone before DL-150's own discriminator ever
//! runs, which made it a red-proof of the WRONG mechanism).  Triangle A
//! at z=0; triangle B at z=`gap`, over the SAME (x,y) positions, so after
//! the weld its 3 vertices resolve to the EXACT SAME post-weld ids as
//! A's -- literally the coincident-triangle signature `ComputeWatertightness`
//! now looks for.  `sameWinding=false` (default) reverses B's vertex
//! order (the reviewer's own "opposite winding" repro: an unordered-set
//! match only, cosine -1 under the REMOVED orientation check); `true`
//! keeps B's order identical to A's (an exact ORDERED match -- two
//! literally duplicate triangles, closing the prior orientation
//! discriminator's "same-facing" residual too).  Either way each lone
//! triangle's own 3 edges are all boundary (count 1) before the weld, and
//! become count 2 (A+B) after it -- 0 boundary, 0 non-manifold, so the
//! edge-count check alone falsely certifies this as a closed 2-manifold
//! with NO other structure to confound the read.
static bool BuildCoincidentTriangleWithRemoteTetrahedron( const Scalar gap,
	IndexTriangleListType& tris, VerticesListType& vertices, bool sameWinding = false )
{
	tris.clear();
	vertices.clear();

	const unsigned int a0 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 0, 0 ) );
	const unsigned int a1 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 0, 0 ) );
	const unsigned int a2 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 1, 0 ) );
	AddTri( tris, a0, a1, a2 );

	const unsigned int b0 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 0, gap ) );
	const unsigned int b1 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 0, gap ) );
	const unsigned int b2 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 1, gap ) );
	if( sameWinding ) {
		AddTri( tris, b0, b1, b2 );	// same order as A -- literally duplicate post-weld
	} else {
		AddTri( tris, b0, b2, b1 );	// reversed -- same SET, opposite winding
	}

	// A remote, genuinely closed tetrahedron (4 vertices, 4 triangular
	// faces, each edge shared by exactly two of them) far from the two
	// triangles, purely to inflate the mesh's own bounding-box diagonal
	// to ~1732 -- matching the reviewer's own repro numbers (eps ~1.7e-3,
	// between the 0.001 gap that should falsely weld and the 0.01 gap
	// that should correctly refuse).
	const Point3 tCenter( 1000, 1000, 1000 );
	const unsigned int t0 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( 0, 0, 1 ) ) );
	const unsigned int t1 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( 1, 0, -1 ) ) );
	const unsigned int t2 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( -1, 1, -1 ) ) );
	const unsigned int t3 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( -1, -1, -1 ) ) );
	AddTri( tris, t0, t1, t2 );
	AddTri( tris, t0, t2, t3 );
	AddTri( tris, t0, t3, t1 );
	AddTri( tris, t1, t3, t2 );

	return true;
}

//! DL-150's own DOCUMENTED RESIDUAL fixture: two INDEPENDENTLY
//! tessellated quads (2 triangles each, split along DIFFERENT internal
//! diagonals) facing each other -- after the weld only the 4 PERIMETER
//! edges are shared between them (each quad's own diagonal stays
//! internal to itself, at a DIFFERENT post-weld vertex pair than the
//! other quad's), so no two triangles ever resolve to the same 3
//! vertices.  Still a false stitch by the same physical mechanism as the
//! coincident-triangle fixture above, but structurally invisible to a
//! check that only looks for exact triangle coincidence -- see
//! `ComputeWatertightness`'s own comment for why this residual is
//! accepted rather than chased further.
static bool BuildOffsetTessellationQuadsWithRemoteTetrahedron( const Scalar gap,
	IndexTriangleListType& tris, VerticesListType& vertices )
{
	tris.clear();
	vertices.clear();

	const unsigned int a0 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 0, 0 ) );
	const unsigned int a1 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 0, 0 ) );
	const unsigned int a2 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 1, 0 ) );
	const unsigned int a3 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 1, 0 ) );
	AddTri( tris, a0, a1, a2 );	// diagonal a0-a2
	AddTri( tris, a0, a2, a3 );

	const unsigned int b0 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 0, gap ) );
	const unsigned int b1 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 0, gap ) );
	const unsigned int b2 = (unsigned int)vertices.size(); vertices.push_back( Point3( 1, 1, gap ) );
	const unsigned int b3 = (unsigned int)vertices.size(); vertices.push_back( Point3( 0, 1, gap ) );
	AddTri( tris, b1, b3, b2 );	// diagonal b1-b3, opposite winding -- faces quad A
	AddTri( tris, b1, b0, b3 );

	const Point3 tCenter( 1000, 1000, 1000 );
	const unsigned int t0 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( 0, 0, 1 ) ) );
	const unsigned int t1 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( 1, 0, -1 ) ) );
	const unsigned int t2 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( -1, 1, -1 ) ) );
	const unsigned int t3 = (unsigned int)vertices.size(); vertices.push_back( Point3Ops::mkPoint3( tCenter, Vector3( -1, -1, -1 ) ) );
	AddTri( tris, t0, t1, t2 );
	AddTri( tris, t0, t2, t3 );
	AddTri( tris, t0, t3, t1 );
	AddTri( tris, t1, t3, t2 );

	return true;
}

//! Builds a `TriangleMeshGeometryIndexed` from the given index/vertex
//! lists and runs `SignedDistanceLower` at `queryPoint`.  Normals are a
//! trivial per-vertex placeholder (DL-150's coincident-triangle
//! discriminator, unlike the orientation-based one it replaced, reads
//! POSITIONS only -- see `ComputeWatertightness`'s own comment): any
//! authored value works, `DoneIndexedTriangles`' DEBUG check just needs
//! an index-sized array to exist (same convention as `BuildMesh` above).
static bool RunSignedDistanceFixture( const IndexTriangleListType& tris, const VerticesListType& vertices,
	const Point3& queryPoint, Scalar& outSigned, bool& outExact )
{
	NormalsListType normals( vertices.size(), Vector3( 0, 0, 1 ) );
	TexCoordsListType coords( vertices.size(), Point2( 0, 0 ) );

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	outSigned = 12345.0; outExact = false;
	const bool ok = mesh->SignedDistanceLower( queryPoint, Scalar( 1000 ), outSigned, outExact );
	mesh->release();
	return ok;
}

static void TestOpposedFacingSheetsRefuseViaDiscriminator()
{
	std::cout << "(m) DL-150 MONEY -- two coincident triangles, welded by a remote-inflated bbox, "
		"REFUSE via the coincident-triangle discriminator" << std::endl;

	{
		// Control at the SAME shape, gap=0.01 > eps: unaffected by DL-150,
		// the two triangles never weld to each other at all and each
		// keeps its own 3 boundary edges (6 total) -- refuses via the
		// pre-existing DL-31 edge-count check alone, exactly as the
		// ledger row states.
		IndexTriangleListType tris; VerticesListType vertices;
		BuildCoincidentTriangleWithRemoteTetrahedron( Scalar( 0.01 ), tris, vertices );
		Scalar outSigned; bool outExact;
		const bool ok = RunSignedDistanceFixture( tris, vertices, Point3( 0.3, 0.3, 0.005 ), outSigned, outExact );
		Check( !ok, "(m) control -- gap=0.01 > eps never welds the two triangles together; refuses via the ordinary boundary-edge count" );
	}
	{
		// MONEY: gap=0.001 < eps (~1.7e-3), opposite winding.  Pre-DL-150
		// (verified against master commit 4b692be6, before ANY DL-150
		// code existed) this falsely certifies watertight with a
		// confidently wrong signed depth in the sliver between the two
		// triangles; the coincident-triangle discriminator now refuses it.
		IndexTriangleListType tris; VerticesListType vertices;
		BuildCoincidentTriangleWithRemoteTetrahedron( Scalar( 0.001 ), tris, vertices, /*sameWinding=*/false );
		Scalar outSigned; bool outExact;
		const bool ok = RunSignedDistanceFixture( tris, vertices, Point3( 0.3, 0.3, 0.0005 ), outSigned, outExact );
		Check( !ok, "(m) MONEY -- DL-150: gap=0.001 < eps welds two opposite-winding coincident triangles; "
			"the discriminator refuses the false 2-manifold instead of answering a wrong signed depth" );
	}
}

//! DL-150 review round 2 (2026-09-18): the SAME-winding case is no longer
//! a documented residual -- it is now CAUGHT too, and by a STRONGER
//! signal than the opposite-winding case: with matching winding, the two
//! triangles resolve to the exact same ORDERED vertex triple (not just
//! the same unordered set), i.e. two literally duplicate triangles.  The
//! orientation-based discriminator this replaced could not see this case
//! at all (cosine +1, indistinguishable from a genuine seam by
//! orientation alone); the coincident-triangle check does not consult
//! orientation in the first place, so it treats both cases identically.
static void TestSameWindingCoincidentTriangleAlsoRefuses()
{
	std::cout << "(n) DL-150 -- same-winding coincident triangles ALSO refuse (closes the prior "
		"orientation-discriminator residual)" << std::endl;

	IndexTriangleListType tris; VerticesListType vertices;
	BuildCoincidentTriangleWithRemoteTetrahedron( Scalar( 0.001 ), tris, vertices, /*sameWinding=*/true );
	Scalar outSigned; bool outExact;
	const bool ok = RunSignedDistanceFixture( tris, vertices, Point3( 0.3, 0.3, 0.0005 ), outSigned, outExact );
	Check( !ok, "(n) MONEY -- same-winding coincident triangles weld into two LITERALLY duplicate "
		"triangles; the coincident-triangle discriminator refuses regardless of orientation" );
}

//! DL-150's own DOCUMENTED RESIDUAL (per `ComputeWatertightness`'s own
//! comment and the ledger row): two independently-tessellated quads on
//! DIFFERENT internal diagonals still weld into an equally false
//! 2-manifold that this check cannot see (every edge still reads count
//! 2; no two triangles share all three vertices).  This is a CONTROL,
//! not a red-proof target: it is expected to stay falsely certified, and
//! exists so a future, stronger check has a known, honestly-stated
//! starting point.
static void TestOffsetTessellationResidualUncaught()
{
	std::cout << "(o) DL-150 documented residual -- independently-tessellated (different-diagonal) quads "
		"still falsely certify (no coincident triangles for this check to find)" << std::endl;

	IndexTriangleListType tris; VerticesListType vertices;
	BuildOffsetTessellationQuadsWithRemoteTetrahedron( Scalar( 0.001 ), tris, vertices );
	Scalar outSigned; bool outExact;
	const bool ok = RunSignedDistanceFixture( tris, vertices, Point3( 0.5, 0.5, 0.0005 ), outSigned, outExact );
	Check( ok, "(o) DOCUMENTED RESIDUAL -- independently-tessellated (different-diagonal) facing sheets "
		"still falsely certify watertight; see DL-150's own stated limit" );
}

//! Appends ONE triangle with brand-new, per-corner vertex slots and a
//! REAL flat face normal computed from its own winding (`cross(p1-p0,
//! p2-p0)`, normalized) -- the exact per-face-flat-shaded convention
//! `Box.glb`/section (e) use, but built explicitly here (not through
//! `BuildMesh()`'s placeholder-normal helper) so the caller's own winding
//! determines a REAL outward direction.  Used by the wedge/prism fixture
//! below, whose whole point is to exercise a genuine sharp CONVEX crease
//! with authored normals that actually vary face to face -- the exact
//! thing `BuildMesh()`'s `(0,0,1)`-for-every-vertex placeholder could
//! never exercise (see `ComputeWatertightness`'s own comment on why the
//! removed orientation discriminator's flat-cube control was a red
//! herring for this reason).
static void AddFlatTriFace( VerticesListType& vertices, NormalsListType& normals, IndexTriangleListType& tris,
	const Point3& p0, const Point3& p1, const Point3& p2 )
{
	const Vector3 e1 = Vector3Ops::mkVector3( p1, p0 );	// p1 - p0
	const Vector3 e2 = Vector3Ops::mkVector3( p2, p0 );	// p2 - p0
	const Vector3 n = Vector3Ops::Normalize( Vector3Ops::Cross( e1, e2 ) );
	const unsigned int base = (unsigned int)vertices.size();
	vertices.push_back( p0 ); vertices.push_back( p1 ); vertices.push_back( p2 );
	normals.push_back( n ); normals.push_back( n ); normals.push_back( n );
	AddTri( tris, base, base + 1, base + 2 );
}

//! A CLOSED triangular-prism wedge with apex angle `apexDeg` at the edge
//! `A0-A1` (the edge shared by the two slanted side faces), REAL per-face
//! flat normals throughout.  Apex at x=0; base (the wide face opposite
//! the apex) at x=1, spanning y in `[-t,t]` where `t=tan(apexDeg/2)`;
//! prism axis along Z, length H.  8 triangles: 2 end caps + 3 side
//! quads (each split into 2), EVERY winding independently verified by
//! hand (see the review-round-2 fix commit message for the full
//! derivation) to produce the correct OUTWARD normal -- in particular,
//! the two slanted faces' outward normals are `(-t,+-1,0)` (unnormalized),
//! whose cosine is EXACTLY `-cos(apexDeg)`: this is what makes a sharp
//! wedge (small `apexDeg`) read a cosine near -1, indistinguishable from
//! the false-stitch signature the REMOVED orientation discriminator
//! looked for, and why that discriminator was wrong.
static void BuildWedge( const Scalar apexDeg, const Scalar H,
	VerticesListType& vertices, NormalsListType& normals, IndexTriangleListType& tris )
{
	vertices.clear(); normals.clear(); tris.clear();

	const Scalar t = std::tan( apexDeg * PI / Scalar( 360 ) );	// tan(apexDeg/2 in radians)
	const Point3 A0( 0, 0, 0 ),      B0( 1, t, 0 ),      C0( 1, -t, 0 );
	const Point3 A1( 0, 0, H ),      B1( 1, t, H ),      C1( 1, -t, H );

	AddFlatTriFace( vertices, normals, tris, A0, B0, C0 );	// cap z=0, outward -Z
	AddFlatTriFace( vertices, normals, tris, A1, C1, B1 );	// cap z=H, outward +Z
	AddFlatTriFace( vertices, normals, tris, A0, B1, B0 );	// face AB, outward (-t,+1,0)
	AddFlatTriFace( vertices, normals, tris, A0, A1, B1 );
	AddFlatTriFace( vertices, normals, tris, A0, C0, C1 );	// face AC, outward (-t,-1,0)
	AddFlatTriFace( vertices, normals, tris, A0, C1, A1 );
	AddFlatTriFace( vertices, normals, tris, B0, B1, C1 );	// face BC (the wide base), outward (+1,0,0)
	AddFlatTriFace( vertices, normals, tris, B0, C1, C0 );
}

//! DL-150 review round 2 MONEY: a genuinely closed, correctly-wound
//! wedge/prism with a sharp CONVEX crease must certify watertight and
//! answer the correct signed depth at EVERY apex angle, including ones
//! sharp enough (<60 degrees) that the REMOVED orientation discriminator
//! (threshold cosine < -0.5) falsely refused it (red on the parent
//! commit 1c0bf3ae: apex 30/50/58 degrees refused, only 60/70 passed,
//! since cosine = -cos(apexDeg) crosses -0.5 exactly at apexDeg=60).
//! Query point is on the prism's central axis at half-height; its true
//! nearest surface (verified analytically for every angle tested here)
//! is always one of the two slanted side faces, at closed-form distance
//! `0.5*t/sqrt(t*t+1)` where `t=tan(apexDeg/2)` -- the mesh is exactly
//! flat (no polyhedral-approximation slack), so this is checked tight.
static void TestWedgeSharpCreaseCertifiesAtEveryAngle()
{
	std::cout << "(p) DL-150 MONEY -- a closed, real-normal wedge certifies at every apex angle, "
		"including sharp convex creases the removed orientation check falsely refused" << std::endl;

	const Scalar apexAnglesDeg[] = { 30, 50, 58, 60, 70 };
	const Scalar H = 2.0;
	for( const Scalar apexDeg : apexAnglesDeg ) {
		VerticesListType vertices; NormalsListType normals; IndexTriangleListType tris;
		BuildWedge( apexDeg, H, vertices, normals, tris );
		TexCoordsListType coords( vertices.size(), Point2( 0, 0 ) );

		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
		mesh->addref();
		mesh->BeginIndexedTriangles();
		mesh->AddVertices( vertices );
		mesh->AddNormals( normals );
		mesh->AddTexCoords( coords );
		mesh->AddIndexedTriangles( tris );
		mesh->DoneIndexedTriangles();

		const std::string tag = "(p) apex=" + std::to_string( (int)apexDeg ) + "deg";
		Scalar outSigned = 12345.0; bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( 0.5, 0, H / 2 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, tag + " -- MONEY: certifies watertight (a genuine sharp convex crease is not a false stitch)" );
		if( ok ) {
			const Scalar t = std::tan( apexDeg * PI / Scalar( 360 ) );
			const Scalar expected = Scalar( 0.5 ) * t / std::sqrt( t * t + Scalar( 1 ) );
			CheckClose( (double)outSigned, -(double)expected, 1e-9, tag + " -- signed depth matches the closed-form nearest-slanted-face distance" );
			Check( outExact, tag + " -- exact" );
		}

		mesh->release();
	}
}

//! A hand-built, per-face-duplicated (REAL per-face flat normals, not
//! `BuildMesh()`'s `(0,0,1)` placeholder) closed cube -- the regression
//! `ComputeWatertightness`'s own review-round-2 correction calls for: the
//! three mutually orthogonal face normals meeting at any corner have
//! cosine exactly 0 (a bounded, ordinary angle no discriminator should
//! ever flag), confirmed here through REAL authored normals rather than
//! the placeholder that let the removed orientation check's own control
//! pass for the wrong reason.
static void TestFlatShadedCubeWithRealNormalsCertifies()
{
	std::cout << "(q) DL-150 sibling -- a flat-shaded cube with REAL per-face normals still certifies" << std::endl;

	const Scalar h = 2.0;
	std::vector<Point3> corners;
	CubeCorners( h, corners );

	// Same 12-triangle winding `CubeTriangles()` uses (see that function's
	// own comment), so the per-face normals computed by `AddFlatTriFace`
	// really do point outward.
	VerticesListType vertices; NormalsListType normals; IndexTriangleListType tris;
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[1], corners[2] );	// bottom (z=-h)
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[2], corners[3] );
	AddFlatTriFace( vertices, normals, tris, corners[4], corners[6], corners[5] );	// top (z=+h)
	AddFlatTriFace( vertices, normals, tris, corners[4], corners[7], corners[6] );
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[5], corners[1] );	// front (y=-h)
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[4], corners[5] );
	AddFlatTriFace( vertices, normals, tris, corners[3], corners[2], corners[6] );	// back (y=+h)
	AddFlatTriFace( vertices, normals, tris, corners[3], corners[6], corners[7] );
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[3], corners[7] );	// left (x=-h)
	AddFlatTriFace( vertices, normals, tris, corners[0], corners[7], corners[4] );
	AddFlatTriFace( vertices, normals, tris, corners[1], corners[5], corners[6] );	// right (x=+h)
	AddFlatTriFace( vertices, normals, tris, corners[1], corners[6], corners[2] );

	TexCoordsListType coords( vertices.size(), Point2( 0, 0 ) );
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(q) MONEY -- flat-shaded cube with REAL (non-placeholder) per-face normals certifies watertight" );
	CheckClose( (double)outSigned, -(double)h, 1e-9, "(q) centre depth equals the half-size exactly" );
	Check( outExact, "(q) exact" );

	mesh->release();
}

//! A genuinely closed, valid thin box (a real slab) whose two large
//! faces are thicker apart than `eps` -- confirms the position weld does
//! NOT merge them (they stay two distinct, non-coincident sheets of the
//! SAME closed solid) and the mesh still certifies via the ordinary
//! DL-31/DL-143 path, unaffected by DL-150's mechanism either way.
static void TestThinButValidSolidThickerThanEpsCertifies()
{
	std::cout << "(r) DL-150 sibling -- a genuinely closed thin slab (thickness >> eps) certifies normally" << std::endl;

	// Half-thickness 0.05 in a unit-scale box: bbox diagonal ~sqrt(2^2+2^2+0.1^2)~2.83,
	// eps~2.83e-6 -- far below the 0.1 physical thickness, so the two
	// large faces are never welded to each other.
	const Scalar halfW = 1.0, halfD = 1.0, halfThick = 0.05;
	std::vector<Point3> corners;
	corners.push_back( Point3( -halfW, -halfD, -halfThick ) );	// 0
	corners.push_back( Point3(  halfW, -halfD, -halfThick ) );	// 1
	corners.push_back( Point3(  halfW,  halfD, -halfThick ) );	// 2
	corners.push_back( Point3( -halfW,  halfD, -halfThick ) );	// 3
	corners.push_back( Point3( -halfW, -halfD,  halfThick ) );	// 4
	corners.push_back( Point3(  halfW, -halfD,  halfThick ) );	// 5
	corners.push_back( Point3(  halfW,  halfD,  halfThick ) );	// 6
	corners.push_back( Point3( -halfW,  halfD,  halfThick ) );	// 7

	IndexTriangleListType tris;
	CubeTriangles( tris, /*dropTopFace=*/false );
	TriangleMeshGeometryIndexed* mesh = BuildMesh( corners, tris );

	Scalar outSigned = 12345.0; bool outExact = false;
	const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
	Check( ok, "(r) MONEY -- a genuinely closed thin (but > eps) slab certifies watertight normally" );
	CheckClose( (double)outSigned, -(double)halfThick, 1e-9, "(r) centre depth equals the slab's own half-thickness (nearest surface, not half-width)" );
	Check( outExact, "(r) exact" );

	mesh->release();
}

//! A hand-built, WELDED UV-sphere: one shared vertex at each pole (a fan
//! of triangles there), ordinary quads split into two triangles on the
//! `n` interior latitude rings.  Deliberately NOT
//! `SphereGeometry::TessellateToMesh` -- when this fixture was written,
//! that tessellator gave each pole CELL its own distinct
//! (but coincident-position) vertex, so the polar "quads" degenerated to
//! zero-area triangles whose wedge edges were each used by only one
//! triangle: a 10082-triangle `TessellateToMesh` sphere measured 284
//! boundary edges under DL-31's watertightness check, a real closed
//! sphere reading as an open sheet purely because of how the tessellator
//! indexed its poles.  Filed as DL-116; CLOSED 2026-09-17 (debt-geom2,
//! see (f) above, which now exercises `SphereGeometry::TessellateToMesh`'s
//! own output directly and gets the same watertight answer).  This
//! fixture is kept as-is regardless -- it gives exact, reproducible
//! control over the triangle count (10000 exactly) this section's timing
//! wants, independent of which tessellator's convention happens to
//! produce it.
static bool BuildWeldedUVSphere( const Scalar R, const unsigned int n, const unsigned int m,
	IndexTriangleListType& tris, VerticesListType& vertices )
{
	tris.clear();
	vertices.clear();
	if( n < 1 || m < 3 ) { return false; }

	const unsigned int northIdx = 0;
	const unsigned int ringBase = 1;						// ring i (1..n), column j (0..m-1) -> ringBase + (i-1)*m + j
	const unsigned int southIdx = ringBase + n * m;

	vertices.resize( southIdx + 1 );
	vertices[northIdx] = Point3( 0, R, 0 );
	vertices[southIdx] = Point3( 0, -R, 0 );
	for( unsigned int i = 1; i <= n; ++i ) {
		const Scalar theta = PI * (Scalar)i / (Scalar)( n + 1 );	// strictly between 0 and PI: never at a pole
		const Scalar y = R * std::cos( theta );
		const Scalar ringR = R * std::sin( theta );
		for( unsigned int j = 0; j < m; ++j ) {
			const Scalar phi = TWO_PI * (Scalar)j / (Scalar)m;
			vertices[ringBase + (i - 1) * m + j] = Point3( ringR * std::cos( phi ), y, ringR * std::sin( phi ) );
		}
	}

	auto addTri = [&]( unsigned int a, unsigned int b, unsigned int c ) {
		IndexedTriangle t;
		t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
		t.iNormals[0] = a;  t.iNormals[1] = b;  t.iNormals[2] = c;
		t.iCoords[0] = a;   t.iCoords[1] = b;   t.iCoords[2] = c;
		tris.push_back( t );
	};
	auto ring = [&]( unsigned int i, unsigned int j ) { return ringBase + (i - 1) * m + ( j % m ); };

	// North cap: a fan from the pole to ring 1.
	for( unsigned int j = 0; j < m; ++j ) {
		addTri( northIdx, ring( 1, j ), ring( 1, j + 1 ) );
	}
	// Interior bands: ring i to ring i+1, one shared diagonal per quad.
	for( unsigned int i = 1; i < n; ++i ) {
		for( unsigned int j = 0; j < m; ++j ) {
			const unsigned int a = ring( i, j ), b = ring( i, j + 1 );
			const unsigned int c = ring( i + 1, j ), d = ring( i + 1, j + 1 );
			addTri( a, c, d );
			addTri( a, d, b );
		}
	}
	// South cap: a fan from ring n to the pole.
	for( unsigned int j = 0; j < m; ++j ) {
		addTri( southIdx, ring( n, j + 1 ), ring( n, j ) );
	}
	return true;
}

//! (d) COST.  A ~10k-triangle watertight sphere, timed over many random
//! interior/exterior queries.  Reported, not gated -- wall clock is
//! machine-dependent -- but printed in the same ns/query unit the
//! design doc's own cost sections use, so a reader can compare directly.
static void TestParityCost()
{
	std::cout << "(d) cost -- parity ray-cast + closest-point on a ~10k-triangle watertight sphere" << std::endl;

	// n=50 interior rings x m=100 columns: 2*100 (cap fans) +
	// 2*100*49 (interior bands) = 200 + 9800 = 10000 triangles exactly.
	const unsigned int n = 50, m = 100;
	const Scalar R = 3.0;
	IndexTriangleListType tris;
	VerticesListType vertices;
	const bool built = BuildWeldedUVSphere( R, n, m, tris, vertices );
	Check( built, "(d) welded UV-sphere construction succeeds" );
	if( !built ) { return; }
	std::cout << "    " << tris.size() << " triangles, " << vertices.size() << " vertices" << std::endl;

	NormalsListType normals( vertices.size(), Vector3( 0, 1, 0 ) );	// unused placeholder
	TexCoordsListType coords( vertices.size(), Point2( 0, 0 ) );		// unused placeholder

	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->BeginIndexedTriangles();
	mesh->AddVertices( vertices );
	mesh->AddNormals( normals );
	mesh->AddTexCoords( coords );
	mesh->AddIndexedTriangles( tris );
	mesh->DoneIndexedTriangles();

	// A sanity check that this fixture actually exercises the fixed
	// path, not a silent refusal: pin one interior depth against the
	// sphere's own closed form before timing.
	{
		Scalar outSigned = 0.0; bool outExact = false;
		const bool ok = mesh->SignedDistanceLower( Point3( 0, 0, 0 ), Scalar( 1000 ), outSigned, outExact );
		Check( ok, "(d) the tessellated sphere is certified watertight (SignedDistanceLower answers)" );
		// Tessellation is a polyhedral approximation of the sphere, not
		// the sphere itself, so this is a loose bound (1% of R), not the
		// tight tolerances used on the exact cube fixtures above.
		CheckClose( (double)outSigned, -(double)R, 0.01 * (double)R,
			"(d) tessellated-sphere centre depth is close to the analytic sphere's -R" );
	}

	std::mt19937 rng( 12345 );
	std::uniform_real_distribution<double> unit( -1.0, 1.0 );
	const int N = 2000;
	std::vector<Point3> points;
	points.reserve( N );
	for( int i = 0; i < N; ++i ) {
		// A mix of interior and exterior points, spanning roughly [0, 2R]
		// from the centre so both the parity cast and the closest-point
		// traversal do real work either way.
		const Scalar x = (Scalar)( unit( rng ) * 2.0 * (double)R );
		const Scalar y = (Scalar)( unit( rng ) * 2.0 * (double)R );
		const Scalar z = (Scalar)( unit( rng ) * 2.0 * (double)R );
		points.push_back( Point3( x, y, z ) );
	}

	const auto t0 = std::chrono::steady_clock::now();
	long long sink = 0;
	for( int i = 0; i < N; ++i ) {
		Scalar outSigned = 0.0; bool outExact = false;
		if( mesh->SignedDistanceLower( points[i], Scalar( 1000 ), outSigned, outExact ) ) {
			sink += outSigned < Scalar( 0 ) ? 1 : 0;
		}
	}
	const auto t1 = std::chrono::steady_clock::now();
	const double ns = std::chrono::duration<double, std::nano>( t1 - t0 ).count();
	const double nsPerQuery = ns / (double)N;
	std::cout << "    " << N << " SignedDistanceLower queries (parity + closest-point): "
		<< nsPerQuery << " ns/query (interior hits counted: " << sink << " of " << N << ")" << std::endl;

	// Decomposed: the unsigned closest-point half ALONE (the same
	// traversal `proximity()` already paid before this row), so the
	// parity ray-cast's own marginal cost is visible rather than folded
	// into one number.
	{
		const auto u0 = std::chrono::steady_clock::now();
		long long sink2 = 0;
		for( int i = 0; i < N; ++i ) {
			Scalar d = 0.0;
			if( mesh->DistanceToSurface( points[i], Scalar( 1000 ), d ) ) { sink2 += 1; }
		}
		const auto u1 = std::chrono::steady_clock::now();
		const double nsUnsigned = std::chrono::duration<double, std::nano>( u1 - u0 ).count() / (double)N;
		std::cout << "    " << N << " DistanceToSurface (unsigned, pre-existing) queries: "
			<< nsUnsigned << " ns/query (answered " << sink2 << " of " << N << "); "
			<< "parity ray-cast's own marginal cost ~= " << ( nsPerQuery - nsUnsigned ) << " ns/query" << std::endl;
	}

	mesh->release();
}

int main()
{
	std::cout << "=== MeshInteriorSignalTest (DL-31, DL-143, DL-136, DL-150) ===" << std::endl;

	TestClosedWatertightCube();
	TestOpenQuadRefuses();
	TestNonWatertightCubeRefuses();
	TestDegenerateTriangleRefuses();
	TestFlatShadedPerCornerCubeWelds();
	TestSphereTessellationNowWatertight();
	TestEllipsoidTessellationNowWatertight();
	TestBoxTessellationWelds();
	TestShippedGltfBoxEndToEnd();
	TestTorusTessellationWatertight();
	TestCappedCylinderTessellationWatertight();
	TestDisplacedWrapsWatertight();
	TestScaledMeshWeldsRelatively();
	TestOpposedFacingSheetsRefuseViaDiscriminator();
	TestSameWindingCoincidentTriangleAlsoRefuses();
	TestOffsetTessellationResidualUncaught();
	TestWedgeSharpCreaseCertifiesAtEveryAngle();
	TestFlatShadedCubeWithRealNormalsCertifies();
	TestThinButValidSolidThickerThanEpsCertifies();
	TestParityCost();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
