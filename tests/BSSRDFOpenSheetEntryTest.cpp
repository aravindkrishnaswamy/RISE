//////////////////////////////////////////////////////////////////////
//
//  BSSRDFOpenSheetEntryTest.cpp - Regression guard for DL-96:
//    DL-70's BSSRDF front-face gate (`PathTracingIntegrator.cpp` x2,
//    `BDPTIntegrator.cpp` x4) chose CLOSED-SOLID semantics -- only the
//    face agreeing with a surface's single TRUE, fixed outward normal
//    admits BSSRDF entry -- which silently drops subsurface entry from
//    the SECOND face of an OPEN double-sided diffusing sheet (a leaf,
//    a cloth card), where both faces are legitimate physical entry
//    points.
//
//  THE FIX
//
//    A new `RayIntersectionGeometric::bOpenSheet` flag, stamped by the
//    geometry that produced a hit, distinguishes "this double-sided
//    surface is a closed solid" (bOpenSheet == false: exactly one true
//    outward face) from "this double-sided surface is an open sheet"
//    (bOpenSheet == true: both faces legitimate).  The six BSSRDF gate
//    call sites now read `ri.geometric.BSSRDFEntryFacing(wo)` instead
//    of `TrueGeomFacing(wo)`: for a closed solid this is IDENTICAL to
//    the pre-fix DL-70 behaviour (admit only the true outward face);
//    for an open sheet it instead uses the RAY-FACING (reported)
//    normal, which is always >= 0 for whichever side the ray actually
//    struck -- so BOTH faces of an open sheet now attempt BSSRDF
//    entry.
//
//    WHICH GEOMETRIES SET `bOpenSheet` (see the field's own doc
//    comment in RayIntersectionGeometric.h for the full contract):
//      - `TriangleMeshGeometryIndexed`: `bDoubleSided && !m_bWatertight`
//        (DL-143's build-time position-weld watertightness check).
//      - `TriangleMeshGeometry` (non-indexed): `bDoubleSided`
//        unconditionally -- this class has no watertightness
//        certification at all.
//      - `ClippedPlaneGeometry` / `BezierPatchGeometry`: on a
//        BACK-FACE hit (same condition each already uses for
//        `bGeomNormalOrientedToRay`) -- a plane/patch never encloses
//        a volume, so a double-sided hit is always an open sheet.
//      - `HairGeometry` does NOT set it (already excluded from the
//        recovery via `bGeomNormalRayDerived`, an orthogonal reason).
//      - `CSGObject::AdoptCsgSurfacePayload` forwards whichever
//        operand's surface is actually being reported, same category
//        as `bGeomNormalOrientedToRay`/`bGeomNormalRayDerived`.
//
//  WHAT EACH PART OF THIS FILE PROVES
//
//    PART A (mechanism level, RED-PROOF BY CONSTRUCTION -- `bOpenSheet`
//    and `BSSRDFEntryFacing()` do not exist on the unfixed library, so
//    this whole file fails to COMPILE against it, the same red-proof
//    shape DL-186's brand-new-ABI tests use).  Confirms every
//    geometry's stamping rule against real production `IntersectRay`
//    calls, and confirms the SIGN of `BSSRDFEntryFacing` diverges from
//    `TrueGeomFacing` at exactly the sites the fix targets (an open
//    sheet's back-face hit) and agrees everywhere else (closed solids,
//    single-sided geometry, front-face hits).
//
//    PART B (render level, END-TO-END).  A double-sided open
//    `clippedplane_geometry` sheet with a `subsurfacescattering_material`
//    (defaults), lit from ONLY the +Z side by a directional light.  A
//    FRONT camera views the lit (+Z) face directly; a BACK camera
//    views the unlit (-Z) face, which can only receive energy via
//    BSSRDF transport finding a lit entry point nearby on the SAME
//    sheet.  Pre-fix the back view's exit hit is REJECTED by the gate
//    (money number below) -- BSSRDF is never attempted, so the back
//    view renders to the scene's own residual floor (whatever the
//    surface's own specular/Fresnel component contributes with no
//    light behind it, near zero).  Post-fix the back view is ADMITTED
//    and renders a genuine, non-negligible glow.  Exercised under PT
//    RGB, PT spectral (hero wavelength), and BDPT -- the same fix
//    lands in both integrators' gate sites.  A SINGLE-SIDED control
//    (the same scene with `doublesided FALSE`) is unaffected either
//    way: a single-sided plane simply has no back face to hit at all.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `dl96`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/BezierPatchGeometry.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

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

//////////////////////////////////////////////////////////////////////
// PART A -- mechanism-level fixtures
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Eight corners of an axis-aligned cube, half-size `h`.
	void CubeCorners( const Scalar h, std::vector<Point3>& v )
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

	//! The cube's 12 triangles (or 10, dropping the top face), matching
	//! tests/MeshInteriorSignalTest.cpp's `CubeTriangles` shape -- a
	//! manifold, position-weldable triangulation when complete, and a
	//! genuine open boundary (missing top face) when `dropTopFace`.
	void CubeTriangles( IndexTriangleListType& tris, bool dropTopFace )
	{
		tris.clear();
		auto add = [&]( unsigned int a, unsigned int b, unsigned int c ) {
			IndexedTriangle t;
			t.iVertices[0] = a; t.iVertices[1] = b; t.iVertices[2] = c;
			t.iNormals[0] = a;  t.iNormals[1] = b;  t.iNormals[2] = c;
			t.iCoords[0] = a;   t.iCoords[1] = b;   t.iCoords[2] = c;
			tris.push_back( t );
		};
		add( 0, 1, 2 ); add( 0, 2, 3 );				// bottom (z=-h)
		if( !dropTopFace ) { add( 4, 6, 5 ); add( 4, 7, 6 ); }	// top (z=+h)
		add( 0, 5, 1 ); add( 0, 4, 5 );				// front (y=-h)
		add( 3, 2, 6 ); add( 3, 6, 7 );				// back (y=+h)
		add( 0, 3, 7 ); add( 0, 7, 4 );				// left (x=-h)
		add( 1, 5, 6 ); add( 1, 6, 2 );				// right (x=+h)
	}

	TriangleMeshGeometryIndexed* BuildIndexedCube( bool bDoubleSided, bool dropTopFace )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( bDoubleSided, false );
		mesh->addref();

		std::vector<Point3> corners;
		CubeCorners( 2.0, corners );
		IndexTriangleListType tris;
		CubeTriangles( tris, dropTopFace );

		VerticesListType vertices;
		NormalsListType normals;
		TexCoordsListType coords;
		for( std::size_t i = 0; i < corners.size(); ++i ) {
			vertices.push_back( corners[i] );
			normals.push_back( Vector3( 0, 0, 1 ) );	// unused placeholder
			coords.push_back( Point2( 0, 0 ) );		// unused placeholder
		}

		mesh->BeginIndexedTriangles();
		mesh->AddVertices( vertices );
		mesh->AddNormals( normals );
		mesh->AddTexCoords( coords );
		mesh->AddIndexedTriangles( tris );
		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! A broad double-sided (or single-sided) flat quad through the
	//! non-indexed `TriangleMeshGeometry`, authored normal +Z on both
	//! triangles -- mirrors tests/BSSRDFPlanarProbeReachTest.cpp's
	//! `MakeDoubleSidedQuadMeshGeneric`.
	TriangleMeshGeometry* BuildFlatQuadNonIndexed( bool bDoubleSided )
	{
		TriangleMeshGeometry* pGeo = new TriangleMeshGeometry( bDoubleSided );
		pGeo->addref();

		const Scalar half = 100.0;
		const Point3 verts[4] = {
			Point3( -half, -half, 0 ), Point3(  half, -half, 0 ),
			Point3(  half,  half, 0 ), Point3( -half,  half, 0 ) };
		const Point2 uv[4] = { Point2(0,0), Point2(1,0), Point2(1,1), Point2(0,1) };
		const Vector3 normal( 0, 0, 1 );

		Triangle t1, t2;
		const int idx1[3] = { 0, 1, 2 };
		const int idx2[3] = { 0, 2, 3 };
		for( int k = 0; k < 3; k++ ) {
			t1.vertices[k] = verts[ idx1[k] ]; t1.normals[k] = normal; t1.coords[k] = uv[ idx1[k] ];
			t2.vertices[k] = verts[ idx2[k] ]; t2.normals[k] = normal; t2.coords[k] = uv[ idx2[k] ];
		}
		pGeo->AddTriangle( t1 );
		pGeo->AddTriangle( t2 );
		pGeo->DoneTriangles();
		return pGeo;
	}

	//! A wide, straight hair strand along X through the origin -- the
	//! `MakeWideHairStrand` pattern from tests/HairSSSEntryNormalTest.cpp.
	HairGeometry* BuildWideStrand()
	{
		std::vector<HairGeometry::StrandDesc> strands( 1 );
		strands[0].controlPoints.push_back( Point3( -2, 0, 0 ) );
		strands[0].controlPoints.push_back( Point3(  2, 0, 0 ) );
		strands[0].rootWidth = 1.0;
		strands[0].tipWidth  = 1.0;
		strands[0].rootUV    = Point2( 0, 0 );
		return new HairGeometry( strands );
	}
}

//////////////////////////////////////////////////////////////////////
// A1/A2: TriangleMeshGeometryIndexed -- watertight vs non-watertight.
//////////////////////////////////////////////////////////////////////
static void TestIndexedMeshWatertightVsOpen()
{
	std::cout << "Part A1: TriangleMeshGeometryIndexed, watertight double-sided cube" << std::endl;
	{
		TriangleMeshGeometryIndexed* mesh = BuildIndexedCube( /*bDoubleSided=*/true, /*dropTopFace=*/false );

		// Outside hit: ray from far -X travelling +X hits the -X face
		// from outside (no flip -- the face's own outward normal is
		// -X, dot(-X,+X) = -1 < 0).
		{
			RayIntersectionGeometric ri( Ray( Point3( -10, 0, 0 ), Vector3( 1, 0, 0 ) ), nullRasterizerState );
			mesh->IntersectRay( ri, true, true, false );
			Check( ri.bHit, "A1: outside ray hits the watertight cube" );
			Check( ri.bOpenSheet == false, "A1 MONEY: watertight double-sided cube, OUTSIDE hit, bOpenSheet == false" );
		}
		// Inside hit: ray from the centre travelling +X hits a face
		// from the inside.  `CubeTriangles`' own winding is not
		// consistently outward-facing (it is only asserted watertight,
		// per its own doc comment and MeshInteriorSignalTest.cpp's),
		// so whether THIS specific hit triggers the flip depends on
		// that face's authored winding, not on "inside vs outside" --
		// the MONEY property under test (bOpenSheet stays false on a
		// watertight double-sided mesh) holds regardless of the flip,
		// so it is asserted unconditionally rather than gated on it.
		{
			RayIntersectionGeometric ri( Ray( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) ), nullRasterizerState );
			mesh->IntersectRay( ri, true, true, false );
			Check( ri.bHit, "A1: inside ray hits the watertight cube" );
			std::cout << "    (diagnostic) inside hit bGeomNormalOrientedToRay=" << ri.bGeomNormalOrientedToRay << std::endl;
			Check( ri.bOpenSheet == false, "A1 MONEY: watertight double-sided cube, INSIDE hit, bOpenSheet == false (regardless of this face's own winding)" );
		}
		safe_release( mesh );
	}

	std::cout << "Part A2: TriangleMeshGeometryIndexed, NON-watertight (missing face) double-sided box" << std::endl;
	{
		TriangleMeshGeometryIndexed* mesh = BuildIndexedCube( /*bDoubleSided=*/true, /*dropTopFace=*/true );

		// Hit the (still fully triangulated) -X face from outside --
		// individually this face looks "closed", but the WHOLE MESH's
		// watertightness certification fails because the top face is
		// missing, so bOpenSheet must read true here too.
		RayIntersectionGeometric ri( Ray( Point3( -10, 0, 0 ), Vector3( 1, 0, 0 ) ), nullRasterizerState );
		mesh->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "A2: outside ray hits the open box" );
		Check( ri.bOpenSheet == true, "A2 MONEY: non-watertight double-sided box, bOpenSheet == true (even on an individually-closed-looking face)" );
		safe_release( mesh );
	}
}

//////////////////////////////////////////////////////////////////////
// A3: TriangleMeshGeometry (non-indexed) -- always an open sheet when
// double-sided (no watertightness concept at all); single-sided is
// untouched (bOpenSheet stays default false).
//////////////////////////////////////////////////////////////////////
static void TestNonIndexedMeshAlwaysOpen()
{
	std::cout << "Part A3: TriangleMeshGeometry (non-indexed), double-sided flat quad" << std::endl;
	{
		TriangleMeshGeometry* pGeo = BuildFlatQuadNonIndexed( true );

		RayIntersectionGeometric front( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		pGeo->IntersectRay( front, true, true, false );
		Check( front.bHit, "A3: front ray hits the double-sided quad" );
		Check( front.bOpenSheet == true, "A3 MONEY: non-indexed double-sided quad, FRONT hit, bOpenSheet == true" );

		RayIntersectionGeometric back( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		pGeo->IntersectRay( back, true, true, false );
		Check( back.bHit, "A3: back ray hits the double-sided quad" );
		Check( back.bOpenSheet == true, "A3 MONEY: non-indexed double-sided quad, BACK hit, bOpenSheet == true" );

		safe_release( pGeo );
	}

	std::cout << "Part A3b: TriangleMeshGeometry (non-indexed), SINGLE-sided quad control" << std::endl;
	{
		TriangleMeshGeometry* pGeo = BuildFlatQuadNonIndexed( false );

		RayIntersectionGeometric front( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		pGeo->IntersectRay( front, true, true, false );
		Check( front.bHit, "A3b: front ray hits the single-sided quad" );
		Check( front.bOpenSheet == false, "A3b control: single-sided quad, FRONT hit, bOpenSheet stays false (default)" );

		// `TriangleMeshGeometry::IntersectRay` passes the CALLER's own
		// `bHitFrontFaces`/`bHitBackFaces` straight to the BVH whenever
		// `bDoubleSided` is false (`bDoubleSided?1:bHitBackFaces`) --
		// single/double-sidedness culling for this class is a caller
		// (Object/material) responsibility, not something the geometry
		// enforces on its own.  So a back hit DOES occur here (we pass
		// bHitBackFaces=true); what matters for DL-96 is that the
		// flip/bOpenSheet-stamping block (`if bHit && bDoubleSided`)
		// never runs, so bOpenSheet stays default false either way.
		RayIntersectionGeometric back( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		pGeo->IntersectRay( back, true, true, false );
		Check( back.bHit, "A3b control: (this class culls sidedness at the caller, not here) back ray still hits" );
		Check( back.bOpenSheet == false, "A3b control: single-sided quad, BACK hit, bOpenSheet stays false (the flip block never runs)" );

		safe_release( pGeo );
	}
}

//////////////////////////////////////////////////////////////////////
// A4: ClippedPlaneGeometry -- open only on a back-face hit, and the
// GATE MECHANISM itself diverges there (the actual thing DL-96 fixes).
//////////////////////////////////////////////////////////////////////
static void TestClippedPlaneGateDivergence()
{
	std::cout << "Part A4: ClippedPlaneGeometry double-sided, front vs back, and the gate divergence" << std::endl;

	const Scalar half = 50.0;
	const Point3 corners[4] = {
		Point3( -half, -half, 0 ), Point3( half, -half, 0 ),
		Point3( half, half, 0 ), Point3( -half, half, 0 ) };
	ClippedPlaneGeometry* pGeo = new ClippedPlaneGeometry( corners, /*bDoubleSided_=*/true );
	pGeo->addref();

	// FRONT hit (from +Z, travelling -Z): no flip, bOpenSheet stays
	// false -- but this is HARMLESS, since BSSRDFEntryFacing() and
	// TrueGeomFacing() are IDENTICAL whenever bGeomNormalOrientedToRay
	// is false (both just read the raw, unflipped facing).
	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		pGeo->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "A4: front ray hits the double-sided clipped plane" );
		Check( ri.bOpenSheet == false, "A4: FRONT hit, bOpenSheet == false" );

		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar gated = ri.BSSRDFEntryFacing( wo );
		const Scalar closedSolid = ri.TrueGeomFacing( wo );
		Check( std::fabs( gated - closedSolid ) < 1e-9,
			"A4: FRONT hit, BSSRDFEntryFacing() agrees with TrueGeomFacing() (open/closed makes no difference here)" );
		Check( gated > 0, "A4: FRONT hit, gate admits BSSRDF entry (both before and after DL-96)" );
	}

	// BACK hit (from -Z, travelling +Z): flips, bOpenSheet == true --
	// THIS is the site DL-96 fixes.  Pre-fix (TrueGeomFacing, the
	// closed-solid recovery) rejects; post-fix (BSSRDFEntryFacing, the
	// open-sheet ray-facing admission) accepts.
	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		pGeo->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "A4: back ray hits the double-sided clipped plane" );
		Check( ri.bGeomNormalOrientedToRay, "A4: (sanity) the back hit really is a flip" );
		Check( ri.bOpenSheet == true, "A4 MONEY: BACK hit, bOpenSheet == true" );

		const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
		const Scalar gated = ri.BSSRDFEntryFacing( wo );
		const Scalar closedSolid = ri.TrueGeomFacing( wo );
		std::cout << "    BACK hit: BSSRDFEntryFacing=" << (double)gated
			<< "  TrueGeomFacing (pre-fix behaviour)=" << (double)closedSolid << std::endl;
		Check( gated > 0, "A4 MONEY: BACK hit, BSSRDFEntryFacing() ADMITS entry (the DL-96 fix)" );
		Check( closedSolid < 0, "A4 MONEY: BACK hit, TrueGeomFacing() would have REJECTED entry (the pre-fix DL-70 behaviour)" );
	}

	safe_release( pGeo );

	std::cout << "Part A4b: ClippedPlaneGeometry SINGLE-sided control" << std::endl;
	{
		ClippedPlaneGeometry* single = new ClippedPlaneGeometry( corners, /*bDoubleSided_=*/false );
		single->addref();

		RayIntersectionGeometric front( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		single->IntersectRay( front, true, true, false );
		Check( front.bHit, "A4b: front ray hits the single-sided clipped plane" );
		Check( front.bOpenSheet == false, "A4b control: FRONT hit, bOpenSheet stays false" );

		RayIntersectionGeometric back( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		single->IntersectRay( back, true, true, false );
		Check( back.bHit == false, "A4b control: single-sided plane has no back face to hit at all (unaffected by DL-96 either way)" );

		safe_release( single );
	}
}

//////////////////////////////////////////////////////////////////////
// A5: BezierPatchGeometry -- same shape as ClippedPlaneGeometry (a
// flat patch, so a back-face hit is unambiguous).
//////////////////////////////////////////////////////////////////////
static void TestBezierPatchOpenSheet()
{
	std::cout << "Part A5: BezierPatchGeometry, flat patch, front vs back" << std::endl;

	// A genuinely FLAT 4x4-control-point patch at z=0, X,Y in [-1.5,1.5]
	// (the TestBezierSaddle() pattern from tests/PatchCurvatureTest.cpp
	// with k=0, i.e. no curvature at all).
	BezierPatchGeometry* g = new BezierPatchGeometry( 10, 8, false );
	g->addref();
	BezierPatch patch;
	for( int i = 0; i < 4; i++ ) {
		const Scalar X = -1.5 + Scalar(i);
		for( int j = 0; j < 4; j++ ) {
			const Scalar Y = -1.5 + Scalar(j);
			patch.c[i].pts[j] = Point3( X, Y, 0.0 );
		}
	}
	g->AddPatch( patch );
	g->Prepare();

	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "A5: front ray hits the flat Bezier patch" );
		Check( ri.bOpenSheet == false, "A5: FRONT hit, bOpenSheet == false" );
	}
	{
		RayIntersectionGeometric ri( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		Check( ri.bHit, "A5: back ray hits the flat Bezier patch" );
		Check( ri.bGeomNormalOrientedToRay, "A5: (sanity) the back hit really is a flip" );
		Check( ri.bOpenSheet == true, "A5 MONEY: BACK hit, bOpenSheet == true" );
	}

	safe_release( g );
}

//////////////////////////////////////////////////////////////////////
// A6: HairGeometry -- does NOT set bOpenSheet (excluded for the
// orthogonal bGeomNormalRayDerived reason, not this one).
//////////////////////////////////////////////////////////////////////
static void TestHairDoesNotSetOpenSheet()
{
	std::cout << "Part A6: HairGeometry does not set bOpenSheet" << std::endl;

	HairGeometry* hair = BuildWideStrand();
	hair->addref();

	RayIntersectionGeometric ri( Ray( Point3( 0, 0, -5 ), Vector3( 0, 0, 1 ) ), nullRasterizerState );
	hair->IntersectRay( ri, true, true, false );
	Check( ri.bHit, "A6: ray hits the hair strand" );
	Check( ri.bGeomNormalRayDerived, "A6: (sanity) hair reports a ray-derived normal" );
	Check( ri.bOpenSheet == false, "A6 MONEY: HairGeometry never sets bOpenSheet (default stays false)" );

	safe_release( hair );
}

//////////////////////////////////////////////////////////////////////
// PART B -- render-level end-to-end money test
//////////////////////////////////////////////////////////////////////

namespace
{
	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		unsigned int width;
		unsigned int height;

		CapturingRasterizerOutput() : width(0), height(0) {}

	protected:
		virtual ~CapturingRasterizerOutput() {}

	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

		virtual void OutputImage(
			const IRasterImage& pImage,
			const Rect*,
			const unsigned int ) override
		{
			width = pImage.GetWidth();
			height = pImage.GetHeight();
			pixels.resize( width * height );
			for( unsigned int y = 0; y < height; y++ ) {
				for( unsigned int x = 0; x < width; x++ ) {
					pixels[y * width + x] = pImage.GetPEL( x, y );
				}
			}
		}
	};

	struct RenderResult
	{
		double mean[3];
		bool   valid;
	};

	std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
	{
		char path[512];
		std::snprintf( path, sizeof(path),
			"/tmp/bssrdf_open_sheet_%s_%d.RISEscene",
			tag, static_cast<int>(::getpid()) );
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return std::string();
		ofs << sceneText;
		ofs.close();
		return std::string( path );
	}

	RenderResult RenderAndComputeMean( const std::string& scenePath, unsigned int seed )
	{
		RenderResult result{ {0,0,0}, false };

		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return result;

		if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			safe_release( pJob );
			return result;
		}

		pJob->RemoveRasterizerOutputs();

		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );

		std::srand( seed );
		const bool bRendered = pJob->Rasterize();
		if( !bRendered || pCap->pixels.empty() ) {
			safe_release( pCap );
			safe_release( pJob );
			return result;
		}

		bool allFinite = true;
		double sum[3] = {0,0,0};
		for( const RISEColor& c : pCap->pixels ) {
			const double cov = c.a;
			const double r = c.base.r * cov, g = c.base.g * cov, b = c.base.b * cov;
			if( !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ) { allFinite = false; break; }
			sum[0] += r; sum[1] += g; sum[2] += b;
		}
		if( allFinite ) {
			const double n = double( pCap->pixels.size() );
			result.mean[0] = sum[0] / n;
			result.mean[1] = sum[1] / n;
			result.mean[2] = sum[2] / n;
			result.valid = true;
		}

		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	//! Shared scene fragment: the open-sheet (or single-sided, via
	//! `doubleSided`) SSS receiver plus a delta-position `omni_light`
	//! placed IN FRONT of the sheet (+Z side, between the sheet and the
	//! front camera).  A point light has no line of sight around an
	//! opaque quad to its own back, so this illuminates the +Z face
	//! only and leaves -Z dark -- exactly like the `directional_light`
	//! this fragment used originally, but `directional_light` was
	//! swapped out after discovering (empirically, via a standalone
	//! omni_light+lambertian control that DID render nonzero, and a
	//! directional_light+lambertian control that did not) that BDPT
	//! does not support directional lights at all -- a pre-existing,
	//! orthogonal gap (matching CLAUDE.md's documented "VCM has no
	//! directional-light sampling" note; BDPT shares it), unrelated to
	//! DL-96.  `omni_light` is a delta light with no surface, so it is
	//! never directly visible to a camera ray either, unlike a mesh
	//! area emitter that would have to be carefully kept out of both
	//! cameras' frustums.  `subsurfacescattering_material` is used at
	//! its own documented physically-reasonable defaults (ior 1.3,
	//! absorption 0.1, scattering 1.0, g 0, roughness 0).
	std::string SceneCommon( bool doubleSided )
	{
		std::string s;
		s += "omni_light\n{\n\tname l_omni\n\tpower 40.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.0 0.0 6.0\n}\n\n";
		s += "subsurfacescattering_material\n{\n\tname mat_sss\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname quad_sss\n";
		s += "\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n";
		s += std::string("\tdoublesided ") + ( doubleSided ? "TRUE" : "FALSE" ) + "\n";
		s += "}\n\n";
		s += "standard_object\n{\n\tname obj_sss\n\tgeometry quad_sss\n\tmaterial mat_sss\n}\n";
		return s;
	}

	std::string CameraBlock( bool front )
	{
		std::string s = "film\n{\n\twidth 16\n\theight 16\n}\n\n";
		s += "pinhole_camera\n{\n";
		s += front ? "\tlocation 0 0 8\n" : "\tlocation 0 0 -8\n";
		s += "\tlookat 0 0 0\n\tup 0 1 0\n\tfov 20.0\n}\n\n";
		return s;
	}

	const char* kRasterizerPT =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 128\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern rendered/dl96_pt_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	const char* kRasterizerPTSpectral =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_spectral_rasterizer\n{\n\tsamples 128\n\trr_min_depth 8\n\thwss FALSE\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern rendered/dl96_ptspec_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	const char* kRasterizerBDPT =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 128\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"file_rasterizeroutput\n{\n\tpattern rendered/dl96_bdpt_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	double Luma( const double m[3] ) { return m[0] + m[1] + m[2]; }
}

static void TestOpenSheetFrontBackSymmetry_PT()
{
	std::cout << "Part B1: open-sheet SSS quad, front vs back view, PT RGB" << std::endl;

	const std::string frontScene = std::string("RISE ASCII SCENE 7\n") + kRasterizerPT + CameraBlock(true)  + SceneCommon(true);
	const std::string backScene  = std::string("RISE ASCII SCENE 7\n") + kRasterizerPT + CameraBlock(false) + SceneCommon(true);

	const std::string frontPath = WriteSceneToTempFile( frontScene, "pt_front" );
	const std::string backPath  = WriteSceneToTempFile( backScene,  "pt_back"  );
	Check( !frontPath.empty() && !backPath.empty(), "B1: scene temp files written" );

	const RenderResult front = RenderAndComputeMean( frontPath, 4001 );
	const RenderResult back  = RenderAndComputeMean( backPath,  4002 );
	std::remove( frontPath.c_str() );
	std::remove( backPath.c_str() );

	Check( front.valid, "B1: front render produced a finite image" );
	Check( back.valid,  "B1: back render produced a finite image" );
	if( !front.valid || !back.valid ) return;

	const double frontLuma = Luma( front.mean );
	const double backLuma  = Luma( back.mean );
	std::cout << "    front mean=(" << front.mean[0] << "," << front.mean[1] << "," << front.mean[2] << ")  luma=" << frontLuma << std::endl;
	std::cout << "    back  mean=(" << back.mean[0]  << "," << back.mean[1]  << "," << back.mean[2]  << ")  luma=" << backLuma  << std::endl;

	Check( frontLuma > 1e-3, "B1: (sanity) the directly-lit front face is non-trivially bright" );
	Check( backLuma > 1e-4,
		"B1 MONEY: PT RGB -- the UNLIT back face of an open sheet gets non-negligible radiance via BSSRDF transport (pre-fix this was rejected at the gate and reads ~0)" );
}

static void TestOpenSheetFrontBackSymmetry_PTSpectral()
{
	std::cout << "Part B2: open-sheet SSS quad, front vs back view, PT spectral (hero)" << std::endl;

	const std::string frontScene = std::string("RISE ASCII SCENE 7\n") + kRasterizerPTSpectral + CameraBlock(true)  + SceneCommon(true);
	const std::string backScene  = std::string("RISE ASCII SCENE 7\n") + kRasterizerPTSpectral + CameraBlock(false) + SceneCommon(true);

	const std::string frontPath = WriteSceneToTempFile( frontScene, "ptspec_front" );
	const std::string backPath  = WriteSceneToTempFile( backScene,  "ptspec_back"  );
	Check( !frontPath.empty() && !backPath.empty(), "B2: scene temp files written" );

	const RenderResult front = RenderAndComputeMean( frontPath, 4101 );
	const RenderResult back  = RenderAndComputeMean( backPath,  4102 );
	std::remove( frontPath.c_str() );
	std::remove( backPath.c_str() );

	Check( front.valid, "B2: front render produced a finite image" );
	Check( back.valid,  "B2: back render produced a finite image" );
	if( !front.valid || !back.valid ) return;

	const double frontLuma = Luma( front.mean );
	const double backLuma  = Luma( back.mean );
	std::cout << "    front luma=" << frontLuma << "  back luma=" << backLuma << std::endl;

	Check( frontLuma > 1e-3, "B2: (sanity) the directly-lit front face is non-trivially bright" );
	Check( backLuma > 1e-4,
		"B2 MONEY: PT spectral (hero) -- the UNLIT back face gets non-negligible radiance via BSSRDF transport" );
}

static void TestOpenSheetFrontBackSymmetry_BDPT()
{
	std::cout << "Part B3: open-sheet SSS quad, front vs back view, BDPT" << std::endl;

	const std::string frontScene = std::string("RISE ASCII SCENE 7\n") + kRasterizerBDPT + CameraBlock(true)  + SceneCommon(true);
	const std::string backScene  = std::string("RISE ASCII SCENE 7\n") + kRasterizerBDPT + CameraBlock(false) + SceneCommon(true);

	const std::string frontPath = WriteSceneToTempFile( frontScene, "bdpt_front" );
	const std::string backPath  = WriteSceneToTempFile( backScene,  "bdpt_back"  );
	Check( !frontPath.empty() && !backPath.empty(), "B3: scene temp files written" );

	const RenderResult front = RenderAndComputeMean( frontPath, 4201 );
	const RenderResult back  = RenderAndComputeMean( backPath,  4202 );
	std::remove( frontPath.c_str() );
	std::remove( backPath.c_str() );

	Check( front.valid, "B3: front render produced a finite image" );
	Check( back.valid,  "B3: back render produced a finite image" );
	if( !front.valid || !back.valid ) return;

	const double frontLuma = Luma( front.mean );
	const double backLuma  = Luma( back.mean );
	std::cout << "    front luma=" << frontLuma << "  back luma=" << backLuma << std::endl;

	Check( frontLuma > 1e-3, "B3: (sanity) the directly-lit front face is non-trivially bright" );
	Check( backLuma > 1e-4,
		"B3 MONEY: BDPT -- the UNLIT back face gets non-negligible radiance via BSSRDF transport (the same fix lands in BDPTIntegrator.cpp's 4 gate sites)" );
}

//! CONTROL: the identical scene with `doublesided FALSE`.  A
//! single-sided plane has no back face to hit at all, so the back
//! camera must see background (near-zero) both before and after
//! DL-96 -- this is NOT the money assertion, it is the "did we
//! accidentally change single-sided behaviour" guard.
static void TestSingleSidedControlUnaffected()
{
	std::cout << "Part B4: SINGLE-SIDED control -- back view stays at background level" << std::endl;

	const std::string backScene = std::string("RISE ASCII SCENE 7\n") + kRasterizerPT + CameraBlock(false) + SceneCommon(false);
	const std::string backPath = WriteSceneToTempFile( backScene, "pt_single_back" );
	Check( !backPath.empty(), "B4: scene temp file written" );

	const RenderResult back = RenderAndComputeMean( backPath, 4301 );
	std::remove( backPath.c_str() );

	Check( back.valid, "B4: render produced a finite image" );
	if( !back.valid ) return;

	const double backLuma = Luma( back.mean );
	std::cout << "    single-sided back luma=" << backLuma << std::endl;
	Check( backLuma < 1e-4, "B4 control: a single-sided sheet's back view stays at background level (no back face to hit)" );
}

int main()
{
	std::cout << "=== BSSRDFOpenSheetEntryTest (DL-96) ===" << std::endl;

	TestIndexedMeshWatertightVsOpen();
	TestNonIndexedMeshAlwaysOpen();
	TestClippedPlaneGateDivergence();
	TestBezierPatchOpenSheet();
	TestHairDoesNotSetOpenSheet();

	TestOpenSheetFrontBackSymmetry_PT();
	TestOpenSheetFrontBackSymmetry_PTSpectral();
	TestOpenSheetFrontBackSymmetry_BDPT();
	TestSingleSidedControlUnaffected();

	std::cout << "\nPassed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
