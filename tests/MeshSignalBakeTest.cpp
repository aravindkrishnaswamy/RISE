//////////////////////////////////////////////////////////////////////
//
//  MeshSignalBakeTest.cpp - Phase-3 exit gate for the geometry-derived
//  shading signals arc (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §13).
//
//  Phase 3 answers the SAME two builtins -- `occlusion(radius)` and
//  `thickness(radius)` -- on indexed triangle meshes, out of a LAZY
//  per-vertex bake owned by the geometry.
//
//  What this pins, and why each one is here:
//
//    (a) LAZINESS, proved by counter rather than asserted.  Building a
//        mesh and firing thousands of rays at it builds NOTHING.  This
//        is the mechanical half of the draft-mode guarantee: draft
//        renders execute no material shading, so they reach no
//        expression, so they reach no provider -- and a bake that only
//        exists on provider READ therefore cannot fire.
//    (b) The first painter-driven query builds EXACTLY ONE table, and
//        every later query on the same (geometry, radius) builds none.
//    (c) Eight threads racing the first query still build exactly one:
//        the find-or-build is serialized.
//    (d) A FRESH geometry builds fresh -- the drop-and-recreate
//        invalidation the incremental derive path relies on (§7.2), and
//        the reason the cache is keyed on the geometry and never on an
//        IObject* (whose address is reused in place).
//    (e) MONEY, occlusion: a floor vertex jammed against a wall reads
//        substantially more occluded than one 2 units away, on ONE mesh
//        carrying both controls.
//    (f) MONEY, thickness: a tessellated slab of width w read at radius
//        R reads ~w/R -- a NUMBER, not an ordering.  Catches both the
//        "lift the ray outward and re-enter through your own face"
//        error (would read ~0) and the "sample the full inward
//        hemisphere" error (a cosine-weighted mean of 1/cos is exactly
//        2, so it would read ~2w/R).
//    (g) The CONSTANT-RADIUS precondition (§7.1): a computed radius is
//        REFUSED -- neutral fallback, and no bake built for it -- while
//        the identical literal is answered.
//    (h) A SECOND distinct literal radius gets its OWN table (the
//        bounded map), and the cap refuses past it rather than growing
//        without limit.
//    (i) UpdateVertices (vertex-level animation -- the one case §7.2's
//        free-invalidation argument does NOT cover) drops the bakes.
//    (j) A hit carrying no triangle (a hand-built record, primId -1)
//        refuses rather than interpolating garbage.
//    (l) INSTANCING: two objects at different world scales sharing ONE
//        mesh geometry read the same signal, the mesh-family twin of
//        SurfaceSignalsTest (i).  This is what makes a geometry-owned
//        (rather than object-owned) bake correct in the first place.
//    (m) N threads each asking for a DIFFERENT literal radius converge
//        on the bounded map: one table per distinct radius, no more.
//    (n) The drop-and-recreate invalidation of §7.2 through the REAL
//        `Cst::DeriveToJobIncremental` path -- (d) proves the property
//        on hand-constructed geometries, this one proves the derive
//        path actually recreates.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Geometry/MeshSignalBake.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IObjectManager.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( Scalar got, Scalar want, Scalar tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want
			<< "  |d| " << std::fabs( got - want ) << "  (tol " << tol << ")" << std::endl;
	}
}

static unsigned int Builds()
{
	return MeshSignalBake::BuildCounter().load( std::memory_order_relaxed );
}

//======================================================================
// Mesh builders
//
// Every face carries its OWN vertex positions -- no sharing across
// faces.  That is deliberate: a shared corner would accumulate the
// average of several face normals, and these tests want to measure the
// bake, not the vertex-normal blending.
//======================================================================

//! Emits an axis-aligned quad, tessellated `n` x `n`, into `mesh`, with a
//! constant normal.  `o` is a corner; `du` / `dv` are the full edge
//! vectors.  Returns nothing -- indices are self-contained.
static void AddGrid( TriangleMeshGeometryIndexed* mesh, unsigned int& nextIndex,
	const Point3& o, const Vector3& du, const Vector3& dv, const Vector3& n, const int cells )
{
	const int side = cells + 1;
	const unsigned int base = nextIndex;
	for( int j = 0; j < side; ++j ) {
		for( int i = 0; i < side; ++i ) {
			const Scalar fu = Scalar(i) / Scalar(cells);
			const Scalar fv = Scalar(j) / Scalar(cells);
			mesh->AddVertex( Point3( o.x + du.x*fu + dv.x*fv,
			                         o.y + du.y*fu + dv.y*fv,
			                         o.z + du.z*fu + dv.z*fv ) );
			mesh->AddNormal( n );
			mesh->AddTexCoord( Point2( fu, fv ) );
			++nextIndex;
		}
	}
	for( int j = 0; j < cells; ++j ) {
		for( int i = 0; i < cells; ++i ) {
			const unsigned int a = base + (unsigned int)( j*side + i );
			const unsigned int b = a + 1;
			const unsigned int c = a + (unsigned int)side;
			const unsigned int d = c + 1;
			IndexedTriangle t1, t2;
			t1.iVertices[0] = a; t1.iVertices[1] = b; t1.iVertices[2] = d;
			t2.iVertices[0] = a; t2.iVertices[1] = d; t2.iVertices[2] = c;
			for( int k = 0; k < 3; ++k ) {
				t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
				t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
			}
			mesh->AddIndexedTriangle( t1 );
			mesh->AddIndexedTriangle( t2 );
		}
	}
}

//! A right-angle TRENCH: a floor quad in z = 0 spanning x in [-1,1] and
//! y in [-0.9, 1], plus a wall quad in the plane y = -1 rising to z = 2.
//! One mesh, both controls: the floor strip at y ~ -0.9 sits 0.1 from the
//! wall, the strip at y ~ +1 is 2 units clear of it.
//!
//! The floor deliberately STOPS SHORT of the wall plane rather than
//! meeting it.  A vertex lying exactly IN an occluder's plane cannot be
//! occluded by it -- every ray from there either lies in the plane or
//! travels away from it -- so a floor row at y = -1 would read ~1 and
//! look like a broken bake when it is in fact correct geometry.  (A real
//! crease shares its vertices, which gives the bisector normal and the
//! familiar ~0.5; that is a different configuration, not this one.)
static TriangleMeshGeometryIndexed* BuildTrench()
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->BeginIndexedTriangles();
	unsigned int next = 0;
	// floor, normal +Z
	AddGrid( mesh, next, Point3( -1, -0.9, 0 ), Vector3( 2, 0, 0 ), Vector3( 0, 1.9, 0 ), Vector3( 0, 0, 1 ), 8 );
	// wall at y = -1, normal +Y (facing the floor's open side)
	AddGrid( mesh, next, Point3( -1, -1, 0 ), Vector3( 2, 0, 0 ), Vector3( 0, 0, 2 ), Vector3( 0, 1, 0 ), 8 );
	mesh->DoneIndexedTriangles();
	return mesh;
}

//! A closed, tessellated SLAB of thickness `w`, spanning x,y in [-2,2] and
//! z in [0,w].  Six faces, each with its own vertices, so a top-face
//! vertex's normal is exactly +Z and its inward trace is exactly -Z.
static TriangleMeshGeometryIndexed* BuildSlab( const Scalar w )
{
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->BeginIndexedTriangles();
	unsigned int next = 0;
	const Scalar E = 2.0;
	// top (+Z) and bottom (-Z)
	AddGrid( mesh, next, Point3( -E, -E, w ), Vector3( 2*E, 0, 0 ), Vector3( 0, 2*E, 0 ), Vector3( 0, 0, 1 ), 4 );
	AddGrid( mesh, next, Point3( -E, -E, 0 ), Vector3( 2*E, 0, 0 ), Vector3( 0, 2*E, 0 ), Vector3( 0, 0,-1 ), 4 );
	// four sides
	AddGrid( mesh, next, Point3( -E, -E, 0 ), Vector3( 2*E, 0, 0 ), Vector3( 0, 0, w ), Vector3( 0,-1, 0 ), 2 );
	AddGrid( mesh, next, Point3( -E,  E, 0 ), Vector3( 2*E, 0, 0 ), Vector3( 0, 0, w ), Vector3( 0, 1, 0 ), 2 );
	AddGrid( mesh, next, Point3( -E, -E, 0 ), Vector3( 0, 2*E, 0 ), Vector3( 0, 0, w ), Vector3(-1, 0, 0 ), 2 );
	AddGrid( mesh, next, Point3(  E, -E, 0 ), Vector3( 0, 2*E, 0 ), Vector3( 0, 0, w ), Vector3( 1, 0, 0 ), 2 );
	mesh->DoneIndexedTriangles();
	return mesh;
}

static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! Evaluate `body` at the first hit of a world-space ray against `obj`,
//! through the SAME ExprEvalContext the painters build.
static bool EvalAtHit( const Object* obj, const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	if( !CompileWithContext( body, prog ) ) return false;

	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection ri = MkRI( origin, dir );
	const bool hit = HitObject( obj, ri );
	if( hit ) {
		outValue = painter->GetValuesAt( ri.geometric ).v[0];
	}
	painter->release();
	return hit;
}

//======================================================================
// (a) Intersection alone builds NOTHING -- the draft-mode guarantee
//======================================================================

static void TestIntersectionBuildsNothing()
{
	std::cout << "(a) building + intersecting a mesh triggers no bake" << std::endl;

	const unsigned int before = Builds();

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// Thousands of hits, exactly as a render would produce them -- and
	// exactly as a `quality:"draft"` preview produces them, because draft
	// differs from a production render only in what it does AFTER the hit
	// (it ignores authored materials entirely, so it never evaluates an
	// expression, so it never reaches a provider).
	int hits = 0;
	for( int i = 0; i < 40; ++i ) {
		for( int j = 0; j < 40; ++j ) {
			const Scalar x = Scalar(-0.95) + Scalar(1.9) * Scalar(i) / Scalar(39);
			const Scalar y = Scalar(-0.95) + Scalar(1.9) * Scalar(j) / Scalar(39);
			RayIntersection ri = MkRI( Point3( x, y, 5 ), Vector3( 0, 0, -1 ) );
			if( HitObject( o, ri ) ) {
				++hits;
				// The stamp itself must be there -- laziness must come from
				// nobody CALLING the provider, not from the channel being
				// absent.
				if( i == 0 && j == 0 ) {
					Check( ri.geometric.signals.pProvider != 0, "(a) mesh hit publishes a provider" );
					Check( ri.geometric.signals.primId >= 0, "(a) mesh hit stamps a triangle id" );
				}
			}
		}
	}
	Check( hits > 1000, "(a) the sweep actually hit the mesh" );
	Check( Builds() == before, "(a) MONEY -- 1600 intersections, ZERO bakes built" );

	o->release();
}

//======================================================================
// (b) First query builds exactly one; later queries build none
// (c) Eight threads racing the first query still build exactly one
//======================================================================

static void TestBuildsExactlyOnce()
{
	std::cout << "(b) first production query builds exactly one table; (c) under contention, still one" << std::endl;

	{
		TriangleMeshGeometryIndexed* mesh = BuildTrench();
		Object* o = new Object( mesh );
		mesh->release();
		o->FinalizeTransformations();

		const unsigned int before = Builds();
		Scalar v = 0;
		Check( EvalAtHit( o, Point3( 0, 0.9, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", v ),
			"(b) the query ray hits" );
		Check( Builds() == before + 1, "(b) MONEY -- the first occlusion() query builds exactly one table" );

		const unsigned int after1 = Builds();
		for( int i = 0; i < 50; ++i ) {
			Scalar ignored = 0;
			EvalAtHit( o, Point3( 0, 0.9, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", ignored );
		}
		Check( Builds() == after1, "(b) 50 further queries at the same radius build nothing" );

		o->release();
	}

	{
		// Fresh geometry, so the cache starts empty again.
		TriangleMeshGeometryIndexed* mesh = BuildTrench();
		Object* o = new Object( mesh );
		mesh->release();
		o->FinalizeTransformations();

		const unsigned int before = Builds();
		std::atomic<int> answered( 0 );
		std::vector<std::thread> threads;
		for( int t = 0; t < 8; ++t ) {
			threads.push_back( std::thread( [o, &answered]() {
				for( int i = 0; i < 25; ++i ) {
					Scalar v = 0;
					if( EvalAtHit( o, Point3( 0, 0.9, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", v ) ) {
						++answered;
					}
				}
			} ) );
		}
		for( size_t t = 0; t < threads.size(); ++t ) threads[t].join();

		Check( answered.load() == 200, "(c) all 200 concurrent queries answered" );
		Check( Builds() == before + 1, "(c) MONEY -- 8 threads racing the first query build exactly ONE table" );

		o->release();
	}
}

//======================================================================
// (d) A fresh geometry builds fresh -- drop-and-recreate invalidation
//======================================================================

static void TestFreshGeometryRebuilds()
{
	std::cout << "(d) a second geometry instance of the same mesh builds its own bake" << std::endl;

	unsigned int builds[2] = { 0, 0 };
	Scalar values[2] = { 0, 0 };
	for( int pass = 0; pass < 2; ++pass ) {
		TriangleMeshGeometryIndexed* mesh = BuildTrench();
		Object* o = new Object( mesh );
		mesh->release();
		o->FinalizeTransformations();

		const unsigned int before = Builds();
		EvalAtHit( o, Point3( 0, -0.9, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", values[pass] );
		builds[pass] = Builds() - before;
		o->release();
	}
	Check( builds[0] == 1 && builds[1] == 1,
		"(d) MONEY -- each geometry instance builds its own table (a re-derived geometry is NOT stale)" );
	// And the two builds agree bit for bit: the bake is deterministic, so
	// a re-derive cannot shift the look of the render.
	Check( values[0] == values[1], "(d) the two independently built tables agree exactly (deterministic bake)" );
}

//======================================================================
// (e) MONEY: occlusion discriminates the corner from the open floor
//======================================================================

static void TestOcclusionDiscriminates()
{
	std::cout << "(e) trench: corner reads occluded, open floor reads unoccluded" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	Scalar aoCorner = 0, aoOpen = 0;
	Check( EvalAtHit( o, Point3( 0, -0.88, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", aoCorner ),
		"(e) corner ray hits the floor" );
	Check( EvalAtHit( o, Point3( 0,  0.90, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", aoOpen ),
		"(e) open-floor ray hits the floor" );

	Check( aoCorner >= 0 && aoCorner <= 1, "(e) corner occlusion is in [0,1]" );
	Check( aoOpen >= 0 && aoOpen <= 1, "(e) open-floor occlusion is in [0,1]" );
	// The query radius (0.3 of a 3.46-unit diagonal, so ~1.04 units) does
	// not reach the wall from y = +0.9, which is 1.9 units away.
	CheckClose( aoOpen, 1.0, 0.05, "(e) open floor reads ~unoccluded" );
	Check( aoCorner < Scalar( 0.75 ),
		"(e) MONEY -- the floor strip beside the wall reads substantially occluded" );

	// And the signal is a GRADIENT, not a two-state flag: occlusion rises
	// monotonically with distance from the wall.  This is what makes it
	// usable as a mask rather than as a selector, and it is the property a
	// per-vertex bake plus barycentric interpolation is supposed to deliver.
	const Scalar ys[4] = { -0.88, -0.6, -0.3, 0.4 };
	Scalar prev = -1;
	bool monotone = true;
	for( int i = 0; i < 4; ++i ) {
		Scalar v = 0;
		if( !EvalAtHit( o, Point3( 0, ys[i], 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", v ) ) {
			monotone = false;
			break;
		}
		if( v <= prev ) monotone = false;
		prev = v;
	}
	Check( monotone, "(e) occlusion increases monotonically with distance from the wall" );

	o->release();
}

//======================================================================
// (f) MONEY: thickness reads ~w/R on a slab -- a number, not an order
//======================================================================

static void TestThicknessIsQuantitative()
{
	std::cout << "(f) tessellated slab: thickness(R) ~= w/R" << std::endl;

	const Scalar w = 0.2;
	TriangleMeshGeometryIndexed* mesh = BuildSlab( w );
	Object* o = new Object( mesh );
	o->FinalizeTransformations();

	// Object-space bbox diagonal -- the quantity the radius is a FRACTION
	// of.  Computed here rather than hardcoded so the expectation stays
	// honest if the slab's extents change.
	const BoundingBox bb = mesh->GenerateBoundingBox();
	const Vector3 d = Vector3Ops::mkVector3( bb.ur, bb.ll );
	const Scalar diag = Vector3Ops::Magnitude( d );

	RayIntersection ri = MkRI( Point3( 0.13, -0.07, 5 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( o, ri ), "(f) the slab's top face is hit" );
	Check( ri.geometric.signals.pProvider != 0, "(f) the slab hit publishes a provider" );

	if( ri.geometric.signals.pProvider ) {
		// R = 4w, so the slab is a QUARTER of the query radius thick.
		//
		// Deliberately NOT the natural-looking w/R = 0.5.  At a half, an
		// INVERTED convention (reporting `1 - w/R`) reads 0.485 against the
		// correct 0.515 -- and any tolerance loose enough to admit the
		// cone's own ~3 % admits that too, so the primary assertion would
		// pass on a sign error and only the saturation check below would
		// catch it.  At a quarter the two candidate answers are 0.258 and
		// 0.742, and this one assertion carries the convention on its own.
		const Scalar R  = 4 * w;
		const Scalar rf = R / diag;
		Scalar th = -1;
		Check( ri.geometric.signals.pProvider->ComputeThickness(
			ri.geometric.signals, rf, true, th ), "(f) ComputeThickness answers" );
		// Expected 0.25, plus the ~3.1 % the 20-degree inward cone adds over
		// a straight-down probe (see MeshSignalBake::kThicknessConeHalfAngle).
		CheckClose( th, 0.25 * 1.031, 0.04, "(f) MONEY -- thickness reads w/R, not 1-w/R (inverted), not 0 (outward lift) and not 2w/R (hemisphere)" );

		// And it SATURATES: read at a radius smaller than the slab is
		// thick, the answer is a flat 1 (= thick), matching the SDF
		// estimator's own saturation convention.
		Scalar thSat = -1;
		Check( ri.geometric.signals.pProvider->ComputeThickness(
			ri.geometric.signals, ( w * Scalar(0.5) ) / diag, true, thSat ), "(f) ComputeThickness answers at the small radius" );
		CheckClose( thSat, 1.0, 1e-9, "(f) thickness saturates at 1 when the query radius is inside the solid" );
	}

	mesh->release();
	o->release();
}

//======================================================================
// (g) The constant-radius precondition
//======================================================================

static void TestComputedRadiusRefused()
{
	std::cout << "(g) a COMPUTED radius is refused (neutral), the same literal is answered" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// Prime the literal-0.3 table first, so the computed-radius query below
	// has a table it COULD have wrongly substituted.
	Scalar aoLiteral = 0;
	EvalAtHit( o, Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", aoLiteral );
	Check( aoLiteral < Scalar( 0.95 ), "(g) the literal query reads a real (occluded) value" );

	const unsigned int before = Builds();
	Scalar aoComputed = -1;
	// `0.15*2` is exactly 0.3 and is genuinely constant -- and the compiler
	// still does not PROVE it, which is the honest boundary of a token
	// peek.  The baked path must treat unproven as dynamic: neutral, not
	// the 0.3 table sitting right there.
	Check( EvalAtHit( o, Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.15*2)", aoComputed ),
		"(g) the computed-radius ray hits" );
	CheckClose( aoComputed, 1.0, 1e-9,
		"(g) MONEY -- a computed radius reads the NEUTRAL fallback, never the baked table" );
	Check( Builds() == before, "(g) and it builds nothing (an unbounded per-value bake is the failure this prevents)" );

	// Same for thickness.
	Scalar thComputed = -1;
	EvalAtHit( o, Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ), "thickness(0.1+0.0)", thComputed );
	CheckClose( thComputed, 1.0, 1e-9, "(g) computed-radius thickness reads the neutral fallback (1 = thick)" );

	o->release();
}

//======================================================================
// (h) The bounded radius map, and its cap
//======================================================================

static void TestRadiusMapAndCap()
{
	std::cout << "(h) distinct literal radii get their own tables, up to a bounded cap" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	o->FinalizeTransformations();

	RayIntersection ri = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( o, ri ), "(h) the query ray hits" );

	if( ri.geometric.signals.pProvider )
	{
		const ISurfaceSignalProvider* p = ri.geometric.signals.pProvider;

		unsigned int before = Builds();
		Scalar a = 0, b = 0;
		Check( p->ComputeOcclusion( ri.geometric.signals, Scalar( 0.30 ), true, a ), "(h) radius 0.30 answers" );
		Check( p->ComputeOcclusion( ri.geometric.signals, Scalar( 0.10 ), true, b ), "(h) radius 0.10 answers" );
		Check( Builds() == before + 2, "(h) two distinct radii built two tables" );
		Check( a != b, "(h) and they are genuinely different measurements, not one table answering both" );

		// A radius within the matching tolerance of an existing one reuses
		// its table; one outside it does not.
		before = Builds();
		Scalar c = 0;
		Check( p->ComputeOcclusion( ri.geometric.signals, Scalar( 0.30 ) * ( Scalar(1) + Scalar(1e-6) ), true, c ),
			"(h) a radius inside the tolerance answers" );
		Check( Builds() == before, "(h) ...from the existing table (no rebuild)" );
		CheckClose( c, a, 1e-12, "(h) ...and gives that table's value" );

		// Fill to the cap, then confirm the next distinct radius REFUSES
		// rather than growing the map without limit.
		const Scalar more[6] = { 0.11, 0.12, 0.13, 0.14, 0.15, 0.16 };
		for( int i = 0; i < 6; ++i ) {
			Scalar v = 0;
			Check( p->ComputeOcclusion( ri.geometric.signals, more[i], true, v ),
				"(h) filling the radius map answers" );
		}
		before = Builds();
		Scalar over = -1;
		Check( !p->ComputeOcclusion( ri.geometric.signals, Scalar( 0.42 ), true, over ),
			"(h) MONEY -- past the cap, a further distinct radius REFUSES" );
		Check( over == Scalar( -1 ), "(h) ...leaving outValue untouched" );
		Check( Builds() == before, "(h) ...and builds nothing" );

		// The refusal is the neutral fallback at the wrapper level, not an
		// error: this is the same honest-absence convention the whole
		// design uses.
		CheckClose( ri.geometric.signals.Occlusion( Scalar( 0.42 ), true ), 1.0, 1e-12,
			"(h) a refused radius reads NeutralOcclusion() through the wrapper" );

		// Thickness keeps its OWN map -- the cap is per signal.
		before = Builds();
		Scalar th = 0;
		Check( p->ComputeThickness( ri.geometric.signals, Scalar( 0.42 ), true, th ),
			"(h) thickness at the same radius still answers (per-signal map)" );
		Check( Builds() == before + 1, "(h) ...having built its own table" );
	}

	mesh->release();
	o->release();
}

//======================================================================
// (i) UpdateVertices invalidates
//======================================================================

static void TestUpdateVerticesInvalidates()
{
	std::cout << "(i) vertex-level animation (UpdateVertices) drops the bakes" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	o->FinalizeTransformations();

	Scalar first = 0;
	EvalAtHit( o, Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", first );

	// Same vertices, same normals -- the point is the INVALIDATION, not the
	// deformation; a shape change would confound "did it rebuild" with
	// "did the value move".
	VerticesListType verts = mesh->getVertices();
	NormalsListType  norms = mesh->getNormals();
	mesh->UpdateVertices( verts, norms );

	const unsigned int before = Builds();
	Scalar second = 0;
	EvalAtHit( o, Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", second );
	Check( Builds() == before + 1, "(i) MONEY -- the post-UpdateVertices query REBUILDS (the old table is gone)" );
	CheckClose( second, first, 1e-12, "(i) and the rebuilt table matches, the geometry being unchanged" );

	mesh->release();
	o->release();
}

//======================================================================
// (j) A record with no triangle refuses
//======================================================================

static void TestNoTriangleRefuses()
{
	std::cout << "(j) a hit carrying no triangle refuses rather than interpolating garbage" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	o->FinalizeTransformations();

	RayIntersection ri = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( o, ri ), "(j) the query ray hits" );

	if( ri.geometric.signals.pProvider ) {
		SurfaceSignalInfo noTri = ri.geometric.signals;
		noTri.primId = -1;			// as a hand-built record (PathVertexEval, LightSampler) leaves it
		Scalar v = -1;
		Check( !ri.geometric.signals.pProvider->ComputeOcclusion( noTri, Scalar( 0.3 ), true, v ),
			"(j) ComputeOcclusion refuses without a triangle" );
		Check( v == Scalar( -1 ), "(j) ...leaving outValue untouched" );
		CheckClose( noTri.Occlusion( Scalar( 0.3 ), true ), 1.0, 1e-12,
			"(j) ...and the wrapper reads NeutralOcclusion()" );

		SurfaceSignalInfo badTri = ri.geometric.signals;
		badTri.primId = 1000000;
		Scalar v2 = -1;
		Check( !ri.geometric.signals.pProvider->ComputeOcclusion( badTri, Scalar( 0.3 ), true, v2 ),
			"(j) an out-of-range triangle id refuses too" );
	}

	mesh->release();
	o->release();
}

//======================================================================
// (k) CSG carries the mesh payload
//======================================================================

static void TestCsgCarriesMeshPayload()
{
	std::cout << "(k) a CSG composite with a mesh operand carries the triangle payload" << std::endl;

	// A UNION of the trench mesh with a small sphere placed well away from
	// the query ray.  The union's algebra reports the MESH's own surface
	// there, so this checks the one thing Phase 3 adds to the CSG contract:
	// `AdoptCsgSurfacePayload` copies `signals` as a whole struct, so the
	// new primId / barycentric fields ride along with the provider pointer
	// and the composite can still answer a baked query.
	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* oA = new Object( mesh );
	mesh->release();
	oA->FinalizeTransformations();

	SphereGeometry* sph = new SphereGeometry( 0.2 );
	Object* oB = new Object( sph );
	sph->release();
	oB->TranslateObject( Vector3( 10, 10, 10 ) );
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( oA, oB ), "(k) composite takes the mesh / sphere operands" );
	csg->FinalizeTransformations();

	RayIntersection ri = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( csg, ri ), "(k) the composite is hit on its mesh operand" );
	Check( ri.geometric.signals.pProvider != 0, "(k) the composite hit carries the mesh's provider" );
	Check( ri.geometric.signals.primId >= 0, "(k) MONEY -- and the triangle id survives the composite" );

	if( ri.geometric.signals.pProvider ) {
		Scalar viaCsg = -1;
		Check( ri.geometric.signals.pProvider->ComputeOcclusion(
			ri.geometric.signals, Scalar( 0.3 ), true, viaCsg ), "(k) the composite hit answers occlusion" );

		// Identical to the same ray against the bare mesh object: the CSG
		// layer must not perturb a baked read at all.
		RayIntersection riBare = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( oA, riBare ), "(k) the bare mesh object is hit by the same ray" );
		Scalar viaBare = -2;
		if( riBare.geometric.signals.pProvider ) {
			riBare.geometric.signals.pProvider->ComputeOcclusion(
				riBare.geometric.signals, Scalar( 0.3 ), true, viaBare );
		}
		CheckClose( viaCsg, viaBare, 1e-12, "(k) and reads exactly what the bare mesh reads" );
	}

	csg->release();
	oA->release();
	oB->release();
}

//======================================================================
// (l) INSTANCING: one geometry, two objects at different world scales
//======================================================================

static void TestInstancesShareOneBake()
{
	std::cout << "(l) two instances of ONE mesh geometry share its bake and read identically" << std::endl;

	// The mesh-family twin of SurfaceSignalsTest (i).  It is the reason the
	// cache is owned by the GEOMETRY: a bake is object-space, so it is
	// simultaneously correct for every instance, and `occlusion(0.3)` means
	// "30 % of the object" on each of them no matter what world size they
	// are drawn at.
	{
		TriangleMeshGeometryIndexed* mesh = BuildTrench();

		Object* oA = new Object( mesh );
		oA->FinalizeTransformations();

		Object* oB = new Object( mesh );
		oB->SetScale( 4.0 );
		oB->FinalizeTransformations();
		mesh->release();

		const unsigned int before = Builds();
		Scalar aoA = 0, aoB = 0;
		Check( EvalAtHit( oA, Point3( 0, -0.88, 5 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", aoA ),
			"(l) instance A corner hit" );
		Check( Builds() == before + 1, "(l) instance A built the one table" );

		// The corresponding place on the 4x instance: the same object-space
		// point, four times further out in the world.
		const unsigned int afterA = Builds();
		Check( EvalAtHit( oB, Point3( 0, -0.88*4, 20 ), Vector3( 0, 0, -1 ), "occlusion(0.3)", aoB ),
			"(l) instance B corner hit" );
		Check( Builds() == afterA,
			"(l) MONEY -- instance B builds NOTHING: the bake belongs to the geometry, not the object" );

		Check( aoA < Scalar( 0.9 ), "(l) instance A really is occluded (non-vacuous)" );
		CheckClose( aoB, aoA, 1e-12, "(l) MONEY -- occlusion IDENTICAL at scale 1 and scale 4" );

		oA->release();
		oB->release();
	}

	// Thickness too: it is normalized by the same object-space fraction, so
	// it must be scale-invariant for the same reason.
	{
		const Scalar w = 0.2;
		TriangleMeshGeometryIndexed* mesh = BuildSlab( w );

		Object* oA = new Object( mesh );
		oA->FinalizeTransformations();

		Object* oB = new Object( mesh );
		oB->SetScale( 3.0 );
		oB->FinalizeTransformations();
		mesh->release();

		Scalar thA = 0, thB = 0;
		Check( EvalAtHit( oA, Point3( 0.13, -0.07, 5 ), Vector3( 0, 0, -1 ), "thickness(0.14)", thA ),
			"(l) slab instance A hit" );
		Check( EvalAtHit( oB, Point3( 0.13*3, -0.07*3, 15 ), Vector3( 0, 0, -1 ), "thickness(0.14)", thB ),
			"(l) slab instance B hit" );
		Check( thA > Scalar( 0 ) && thA < Scalar( 1 ),
			"(l) slab instance A thickness is non-saturated (a saturated 1 would compare constants)" );
		CheckClose( thB, thA, 1e-12, "(l) MONEY -- thickness IDENTICAL at scale 1 and scale 3" );

		oA->release();
		oB->release();
	}
}

//======================================================================
// (m) Concurrent DISTINCT radii: the map converges, one table each
//======================================================================

static void TestConcurrentDistinctRadii()
{
	std::cout << "(m) eight threads, eight different radii: exactly eight tables" << std::endl;

	// (c) races threads onto ONE radius and proves they build once.  This is
	// the other half: threads that each MISS force eight serialized builds
	// through the same lock, and the map must end up holding exactly the
	// eight of them -- no duplicate table for a radius another thread was
	// already building, and no lost insert.
	TriangleMeshGeometryIndexed* mesh = BuildTrench();
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// Exactly kMaxTablesPerKind distinct radii, so every one of them is
	// expected to be ACCEPTED -- a cap refusal here would look like a lost
	// build and confuse the count.
	const size_t kThreads = MeshSignalBakeCache::kMaxTablesPerKind;
	std::vector<Scalar> radii;
	for( size_t t = 0; t < kThreads; ++t ) {
		radii.push_back( Scalar( 0.10 ) + Scalar( 0.05 ) * Scalar( t ) );
	}

	const unsigned int before = Builds();
	std::atomic<int> answered( 0 );
	std::atomic<int> mismatched( 0 );
	std::vector<Scalar> firstAnswer( kThreads, Scalar( -1 ) );

	std::vector<std::thread> threads;
	for( size_t t = 0; t < kThreads; ++t ) {
		threads.push_back( std::thread( [o, t, &radii, &answered, &mismatched, &firstAnswer]() {
			for( int i = 0; i < 20; ++i ) {
				RayIntersection ri = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
				if( !HitObject( o, ri ) || !ri.geometric.signals.pProvider ) {
					continue;
				}
				Scalar v = -1;
				if( !ri.geometric.signals.pProvider->ComputeOcclusion(
						ri.geometric.signals, radii[t], true, v ) ) {
					continue;
				}
				++answered;
				// Only this thread touches its own slot.
				if( firstAnswer[t] < Scalar( 0 ) ) {
					firstAnswer[t] = v;
				} else if( firstAnswer[t] != v ) {
					++mismatched;		// one radius answering two ways
				}
			}
		} ) );
	}
	for( size_t t = 0; t < threads.size(); ++t ) threads[t].join();

	Check( answered.load() == (int)( kThreads * 20 ), "(m) every concurrent query at every radius answered" );
	Check( mismatched.load() == 0, "(m) each radius answered consistently across its 20 queries" );
	Check( Builds() == before + (unsigned int)kThreads,
		"(m) MONEY -- N concurrent DISTINCT radii build exactly N tables (no duplicate, no lost insert)" );

	// And the map really is full rather than merely N-built: the next
	// distinct radius is refused by the cap.
	{
		RayIntersection ri = MkRI( Point3( 0, -0.85, 5 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( o, ri ), "(m) the cap-probe ray hits" );
		if( ri.geometric.signals.pProvider ) {
			Scalar over = -1;
			Check( !ri.geometric.signals.pProvider->ComputeOcclusion(
					ri.geometric.signals, Scalar( 0.77 ), true, over ),
				"(m) the map is FULL at the cap -- a ninth distinct radius refuses" );
		}
	}

	o->release();
}

//======================================================================
// (n) Invalidation through the REAL incremental derive path
//======================================================================

static void TestIncrementalDerivePathRebuilds()
{
	std::cout << "(n) a geometry edit through Cst::DeriveToJobIncremental bakes fresh" << std::endl;

	// (d) proves the property on hand-constructed geometries: a second
	// geometry has its own cache because the cache is a member subobject.
	// That argument is sound, and it is still an argument about the OBJECTS
	// -- it says nothing about whether the derive path actually recreates
	// the geometry rather than mutating one in place.  §7.2's whole
	// free-invalidation claim rests on it doing so, so this drives a real
	// edit through the real path and watches the build counter.
	//
	// `lathe_geometry` is the mesh chosen because it is authorable inline
	// (no asset file) and lands on TriangleMeshGeometryIndexed, which is the
	// family that bakes.
	const std::string kScene =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"lathe_geometry\n{\nname g\naxis y\nn_radial 16\nsmooth TRUE\n"
			"profile_point 0.0 0.0\nprofile_point 0.5 0.0\n"
			"profile_point 0.5 1.0\nprofile_point 0.0 1.0\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\n}\n";

	Cst::Document doc = Cst::ParseToCst( kScene );
	Job* job = new Job();
	std::vector<std::string> d0;
	Cst::DeriveToJob( doc, *job, &d0 );

	IObjectPriv* obj = job->GetObjects() ? job->GetObjects()->GetItem( "o" ) : 0;
	Check( obj != 0, "(n) the derived scene has the lathe object" );

	Scalar firstValue = -1;
	if( obj ) {
		const unsigned int before = Builds();
		RayIntersection ri = MkRI( Point3( 0, 0.5, 5 ), Vector3( 0, 0, -1 ) );
		obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
		Check( ri.geometric.bHit, "(n) the lathe's wall is hit" );
		Check( ri.geometric.signals.pProvider != 0, "(n) and it publishes a mesh provider" );
		if( ri.geometric.signals.pProvider ) {
			Check( ri.geometric.signals.pProvider->ComputeOcclusion(
				ri.geometric.signals, Scalar( 0.3 ), true, firstValue ), "(n) the first query answers" );
		}
		Check( Builds() == before + 1, "(n) the first query built the bake" );
	}

	// The EDIT: change the lathe's radial resolution, which is a genuine
	// shape change, and re-apply only that chunk's closure.
	const Cst::NodeId gId = Cst::DocFindByName( doc, "lathe_geometry/g" );
	Cst::Document docE = Cst::DocSetParamValue( doc, gId, "n_radial", 0, "24" );
	std::vector<Cst::NodeId> closure = Cst::DocEditClosure( docE, gId );
	std::vector<std::string> di;
	const int applied = Cst::DeriveToJobIncremental( docE, *job, closure, &di );
	Check( applied > 0 && di.empty(), "(n) the geometry edit applied (not refused)" );

	IObjectPriv* obj2 = job->GetObjects() ? job->GetObjects()->GetItem( "o" ) : 0;
	Check( obj2 != 0, "(n) the object survives the re-derive" );
	if( obj2 ) {
		const unsigned int before = Builds();
		RayIntersection ri = MkRI( Point3( 0, 0.5, 5 ), Vector3( 0, 0, -1 ) );
		obj2->IntersectRay( ri, RISE_INFINITY, true, true, false );
		Check( ri.geometric.bHit, "(n) the re-derived lathe is hit" );
		if( ri.geometric.signals.pProvider ) {
			Scalar secondValue = -1;
			Check( ri.geometric.signals.pProvider->ComputeOcclusion(
				ri.geometric.signals, Scalar( 0.3 ), true, secondValue ), "(n) the post-edit query answers" );
			Check( secondValue >= 0 && secondValue <= 1, "(n) ...with a value in range" );
		}
		Check( Builds() == before + 1,
			"(n) MONEY -- the re-derived geometry bakes FRESH (the derive path really does recreate)" );
	}

	job->release();
}

//======================================================================
// (o) MONEY, convexity: a baked box mesh reads 0 on a face, ~0.5 on an
//     arris and ~0.75 on a corner -- the same closed forms the SDF family
//     hits, reached by a completely different mechanism
//
// The mesh cannot answer convexity out of the OCCLUSION bake: that one
// integrates a cosine-weighted OUTWARD hemisphere, which saturates at 1
// for a plane and for every convex feature alike and so carries no
// convexity information at all.  eConvexity is therefore its own table,
// traced over the FULL sphere uniformly -- the mesh's form of the same
// accessibility `A` the SDF family measures by ball volume.  On a wedge of
// solid dihedral angle theta both give A = 1 - theta/(2*pi), so the two
// families agree on exactly the features the signal exists to find:
//     face   theta = pi   -> A = 1/2  -> convexity 0
//     arris  theta = pi/2 -> A = 3/4  -> convexity 0.5
//     corner theta = pi/4 -> A = 7/8  -> convexity 0.75
//
// Bands are looser than SurfaceSignalsTest's because a mesh answers at
// VERTICES and interpolates barycentrically to the hit, and because the
// bake's rays start `originEpsilon` off the surface (see (p) below).
//======================================================================

static void TestConvexityDiscriminatesOnMesh()
{
	std::cout << "(o) baked box mesh: convexity 0 on a face, ~0.5 on an arris, ~0.75 at a corner" << std::endl;

	// A closed 4 x 4 x 1 slab: its top face is planar, its top edges are
	// 90-degree arrises and its top corners are three-face corners.
	TriangleMeshGeometryIndexed* mesh = BuildSlab( 1.0 );
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// bbox 4 x 4 x 1, diagonal 5.745; 0.15 of it is a query radius of 0.86
	// -- big enough that the arris is locally a wedge, small enough that
	// the far side of the slab is not inside the ball.
	const char* rf = "0.15";

	Scalar cxFace = -1, cxArris = -1, cxCorner = -1;
	Check( EvalAtHit( o, Point3( 0.0, 0.0, 5 ), Vector3( 0, 0, -1 ),
		std::string( "convexity(" ) + rf + ")", cxFace ), "(o) face ray hits the top" );
	// 0.01 inside the edge / corner, so the barycentric read is dominated by
	// the edge (resp. corner) vertex rather than averaging in its neighbour.
	Check( EvalAtHit( o, Point3( 1.99, 0.0, 5 ), Vector3( 0, 0, -1 ),
		std::string( "convexity(" ) + rf + ")", cxArris ), "(o) arris ray hits the top" );
	Check( EvalAtHit( o, Point3( 1.99, 1.99, 5 ), Vector3( 0, 0, -1 ),
		std::string( "convexity(" ) + rf + ")", cxCorner ), "(o) corner ray hits the top" );

	CheckClose( cxFace, 0.0, 0.02, "(o) the flat top face reads convexity ~0 (see (p) for the bound)" );
	CheckClose( cxArris, 0.5, 0.15, "(o) MONEY -- a 90-degree arris reads ~0.5 (closed form)" );
	CheckClose( cxCorner, 0.75, 0.20, "(o) MONEY -- a three-face corner reads ~0.75 (closed form)" );
	Check( cxCorner > cxArris + Scalar( 0.1 ),
		"(o) ORDERING -- a corner is more convex than an arris, by a clear margin" );
	Check( cxArris > cxFace + Scalar( 0.3 ),
		"(o) ORDERING -- an arris is far more convex than the flat face" );

	// The pairing the whole design rests on: none of these three is a
	// cavity, so occlusion must read 1 at all of them.  A convex arris that
	// darkened occlusion is exactly the bug the SDF family had.
	Scalar aoFace = -1, aoArris = -1, aoCorner = -1;
	Check( EvalAtHit( o, Point3( 0.0, 0.0, 5 ), Vector3( 0, 0, -1 ),
		std::string( "occlusion(" ) + rf + ")", aoFace ), "(o) face occlusion evaluates" );
	Check( EvalAtHit( o, Point3( 1.99, 0.0, 5 ), Vector3( 0, 0, -1 ),
		std::string( "occlusion(" ) + rf + ")", aoArris ), "(o) arris occlusion evaluates" );
	Check( EvalAtHit( o, Point3( 1.99, 1.99, 5 ), Vector3( 0, 0, -1 ),
		std::string( "occlusion(" ) + rf + ")", aoCorner ), "(o) corner occlusion evaluates" );
	CheckClose( aoFace, 1.0, 0.02, "(o) flat top face reads occlusion 1" );
	CheckClose( aoArris, 1.0, 0.02, "(o) convex arris reads occlusion 1" );
	CheckClose( aoCorner, 1.0, 0.02, "(o) convex corner reads occlusion 1" );

	o->release();
}

//======================================================================
// (p) the convexity bake's ONE documented bias, pinned at its bound
//
// Bake rays start at `vertex + n * originEpsilon` -- they have to, or the
// triangles incident on the vertex answer "hit" for free.  On a flat
// surface a ray aimed just below the horizon then escapes instead of
// hitting, whenever |cos theta| < originEpsilon / R.  So a flat mesh reads
// A ~= 1/2 + originEpsilon/R rather than exactly 1/2, i.e. a small
// POSITIVE convexity where the truth is 0.
//
// It is a bias, not noise: one-sided (never negative, so a convexity mask
// on flat mesh lights nothing), bounded by originEpsilon/R, and it does
// not average away.  This pins it at that bound rather than leaving it to
// be rediscovered.  With kOriginEpsilonFraction = 1e-4 of the diagonal and
// a query radius of 0.05 of the diagonal, the bound is 2e-3.
//======================================================================

static void TestConvexityFlatBias()
{
	std::cout << "(p) convexity's origin-epsilon bias on flat mesh stays inside its bound" << std::endl;

	TriangleMeshGeometryIndexed* mesh = BuildSlab( 1.0 );
	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	// Several radii: the bias scales as originEpsilon/R, so a SMALLER
	// radius is the harder case and must still land under the bound.
	const char* radii[3] = { "0.05", "0.10", "0.20" };
	const Scalar bounds[3] = { Scalar( 0.006 ), Scalar( 0.004 ), Scalar( 0.003 ) };

	for( int i = 0; i < 3; ++i ) {
		Scalar cx = -1;
		Check( EvalAtHit( o, Point3( 0.05, -0.07, 5 ), Vector3( 0, 0, -1 ),
			std::string( "convexity(" ) + radii[i] + ")", cx ),
			std::string( "(p) flat-face convexity evaluates at r=" ) + radii[i] );
		Check( cx >= Scalar( 0 ), std::string( "(p) the bias is ONE-SIDED (never negative) at r=" ) + radii[i] );
		Check( cx <= bounds[i],
			std::string( "(p) flat-face convexity stays under its originEpsilon/R bound at r=" ) + radii[i] );
	}

	o->release();
}

//======================================================================

int main()
{
	std::cout << "=== MeshSignalBakeTest (design doc Phase 3: mesh occlusion / thickness bakes) ===" << std::endl;

	TestIntersectionBuildsNothing();
	TestBuildsExactlyOnce();
	TestFreshGeometryRebuilds();
	TestOcclusionDiscriminates();
	TestThicknessIsQuantitative();
	TestComputedRadiusRefused();
	TestRadiusMapAndCap();
	TestUpdateVerticesInvalidates();
	TestNoTriangleRefuses();
	TestCsgCarriesMeshPayload();
	TestInstancesShareOneBake();
	TestConcurrentDistinctRadii();
	TestIncrementalDerivePathRebuilds();
	TestConvexityDiscriminatesOnMesh();
	TestConvexityFlatBias();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
