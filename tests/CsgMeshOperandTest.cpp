//////////////////////////////////////////////////////////////////////
//
//  CsgMeshOperandTest.cpp - DL-621 review round (findings 10-13).
//
//  CSG over mesh / displaced operands after DL-621 (d47c91cac):
//
//   A. exit completion (`RecoverContainedOperand`) re-casts only an
//      operand that publishes no exit of its own (mesh, displaced, patch
//      set); an analytic leaf is cast exactly once per query and keeps
//      the exit it published (finding 10);
//   B. `AdvanceOperandPast` is not called for a single-lobe (convex
//      analytic) operand, so the common "intervals in sequence" miss
//      costs no extra re-cast, in the radiance and the shadow query
//      (finding 11);
//   C. a mesh takes a SHEET role only when its free boundary is
//      substantial: a closed mesh left open by a pinhole or an unwelded
//      T-junction is UNDECIDED, so intersection( near-closed mesh, ball )
//      is the solid lens, not a capless skin (finding 12);
//   D. two open sheets welded into a zero-volume "pillow" are not a
//      closed solid (the DL-197 coplanar-opposed guard), while a closed
//      cube with a flat pleat stays closed (finding 13);
//   E. a first openness query from many threads at once builds the
//      topology once (finding 13).
//
//  Red-proof: against d47c91cac 34 passed, 12 failed (sections B-E).
//  Section A is a PIN, green on d47c91cac too: an analytic leaf
//  publishes its exit, `Object::IntersectRay` sets `hasBoundaryRange2`,
//  and `RecoverContainedOperand` returns before any completion -- the
//  explicit gate makes that structural.  B's last row pins the one
//  rendered difference DL-621 makes on the shipped CSG scenes.
//
//////////////////////////////////////////////////////////////////////

#define main DomainFixtureMain
#include "SMSDomainReplayTest.cpp"
#undef main

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/Library/Polygon.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////

//! A sphere that counts how often it is intersected.
class CountingSphere : public SphereGeometry
{
public:
	mutable std::atomic<unsigned int> casts{ 0 };
	explicit CountingSphere( Scalar r ) : SphereGeometry( r ) {}
	void IntersectRay( RayIntersectionGeometric& ri, const bool f, const bool b, const bool e ) const override
	{
		casts.fetch_add( 1, std::memory_order_relaxed );
		SphereGeometry::IntersectRay( ri, f, b, e );
	}
protected:
	virtual ~CountingSphere() {}
};

struct CountedCsg
{
	CountingSphere* gA = nullptr;
	CountingSphere* gB = nullptr;
	CSGObject* csg = nullptr;
	void Reset() { gA->casts = 0; gB->casts = 0; }
	~CountedCsg() { safe_release( csg ); }
};

static void MakeCountedCsg( CountedCsg& c, const CSG_OP op, const Point3& pA, const Point3& pB, Scalar rA = 1, Scalar rB = 1 )
{
	c.gA = new CountingSphere( rA );
	c.gB = new CountingSphere( rB );
	Object* oA = new Object( c.gA );
	Object* oB = new Object( c.gB );
	oA->SetPosition( pA );
	oB->SetPosition( pB );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();
	c.csg = new CSGObject( op );
	Check( c.csg->AssignObjects( oA, oB ), "fixture: composite takes its operands" );
	c.csg->FinalizeTransformations();
	safe_release( oA );
	safe_release( oB );
	// c.gA / c.gB stay alive through the operands' references.
	c.gA->release();
	c.gB->release();
}

static RayIntersection CastObj( const IObject& o, const Point3& origin, const Vector3& dir )
{
	RayIntersection hit( Ray( origin, dir ), nullRasterizerState );
	o.IntersectRay( hit, RISE_INFINITY, true, true, false );
	return hit;
}

//! Lat-long triangle sphere, outward wound, single pole vertices.
//! `skipTri` >= 0 drops that one triangle (a pinhole).
static std::vector<Point3> LatLongSphereCorners( int rings, int segs, Scalar r, int skipTri,
	std::vector<Point3>* outVerts = nullptr, std::vector<int>* outTris = nullptr )
{
	std::vector<Point3> v;
	v.push_back( Point3( 0, 0, r ) );
	for( int i = 1; i < rings; ++i ) {
		const Scalar th = PI * Scalar( i ) / Scalar( rings );
		for( int j = 0; j < segs; ++j ) {
			const Scalar ph = TWO_PI * Scalar( j ) / Scalar( segs );
			v.push_back( Point3( r * std::sin( th ) * std::cos( ph ), r * std::sin( th ) * std::sin( ph ), r * std::cos( th ) ) );
		}
	}
	v.push_back( Point3( 0, 0, -r ) );
	const int south = static_cast<int>( v.size() ) - 1;
	auto ring = [&]( int i, int j ) { return 1 + ( i - 1 ) * segs + ( ( j % segs ) + segs ) % segs; };
	std::vector<int> t;
	for( int j = 0; j < segs; ++j ) {
		t.insert( t.end(), { 0, ring( 1, j ), ring( 1, j + 1 ) } );
	}
	for( int i = 1; i < rings - 1; ++i ) {
		for( int j = 0; j < segs; ++j ) {
			t.insert( t.end(), { ring( i, j ), ring( i + 1, j ), ring( i + 1, j + 1 ) } );
			t.insert( t.end(), { ring( i, j ), ring( i + 1, j + 1 ), ring( i, j + 1 ) } );
		}
	}
	for( int j = 0; j < segs; ++j ) {
		t.insert( t.end(), { south, ring( rings - 1, j + 1 ), ring( rings - 1, j ) } );
	}
	std::vector<int> kept;
	for( std::size_t k = 0; k < t.size() / 3; ++k ) {
		if( static_cast<int>( k ) == skipTri ) {
			continue;
		}
		kept.insert( kept.end(), { t[3 * k], t[3 * k + 1], t[3 * k + 2] } );
	}
	std::vector<Point3> corners;
	for( int idx : kept ) {
		corners.push_back( v[idx] );
	}
	if( outVerts ) *outVerts = v;
	if( outTris ) *outTris = kept;
	return corners;
}

static std::string IndexedMeshText( const std::string& name, const std::vector<Point3>& v, const std::vector<int>& t )
{
	std::ostringstream s;
	s.precision( 17 );
	s << "indexedmesh_geometry\n{\n name " << name << "\n double_sided TRUE\n face_normals TRUE\n";
	for( const Point3& p : v ) {
		s << " vertex " << p.x << ' ' << p.y << ' ' << p.z << '\n';
	}
	for( std::size_t i = 0; i < v.size(); ++i ) {
		s << " uv 0 0\n";
	}
	for( std::size_t k = 0; k < t.size() / 3; ++k ) {
		s << " triangle " << t[3 * k] << ' ' << t[3 * k + 1] << ' ' << t[3 * k + 2] << '\n';
	}
	return s.str() + "}\n";
}

static void PushTri( std::vector<Point3>& c, const Point3& a, const Point3& b, const Point3& d )
{
	c.push_back( a ); c.push_back( b ); c.push_back( d );
}

//! Unit cube [-1,1]^3, outward wound, as 12 corner triangles; when
//! `tJunction`, the +X face's top edge carries a midpoint vertex the +Z
//! face does not share (an unwelded T-junction: three free edges that
//! run back along each other).
static std::vector<Point3> CubeCorners( bool tJunction )
{
	const Point3 v[8] = { Point3(-1,-1,-1), Point3(1,-1,-1), Point3(1,1,-1), Point3(-1,1,-1),
		Point3(-1,-1,1), Point3(1,-1,1), Point3(1,1,1), Point3(-1,1,1) };
	const int tris[12][3] = { {0,2,1},{0,3,2},{4,5,6},{4,6,7},
		{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5} };
	std::vector<Point3> c;
	for( int k = 0; k < 12; ++k ) {
		if( tJunction && ( k == 10 || k == 11 ) ) {
			continue;
		}
		PushTri( c, v[tris[k][0]], v[tris[k][1]], v[tris[k][2]] );
	}
	if( tJunction ) {
		// +X face (1,2,6,5) re-triangulated around M, the midpoint of the
		// top edge 5-6 that the +Z face (4,5,6,7) keeps whole.
		const Point3 m( 1, 0, 1 );
		PushTri( c, v[1], v[2], m );
		PushTri( c, v[2], v[6], m );
		PushTri( c, v[1], m, v[5] );
	}
	return c;
}

//////////////////////////////////////////////////////////////////////
// A. exit completion only for operands that publish no exit
//////////////////////////////////////////////////////////////////////

static void TestAnalyticOperandsCastOnce()
{
	std::cout << "A. analytic operands: no exit-completion re-cast" << std::endl;
	const Vector3 down( 0, 0, -1 );
	// Overlapping spheres: the ray crosses both, the composite hits.
	for( const CSG_OP op : { CSG_UNION, CSG_INTERSECTION, CSG_SUBTRACTION } ) {
		CountedCsg c;
		MakeCountedCsg( c, op, Point3( 0, 0, 0.5 ), Point3( 0, 0, -0.5 ) );
		c.Reset();
		const RayIntersection h = CastObj( *c.csg, Point3( 0.1, 0.05, 5 ), down );
		Check( h.geometric.bHit, "A: overlapping-sphere composite is hit" );
		Check( c.gA->casts == 1 && c.gB->casts == 1,
			"A: each analytic operand is intersected exactly once per radiance query (op " +
			std::to_string( int( op ) ) + ": " + std::to_string( c.gA->casts ) + "/" + std::to_string( c.gB->casts ) + ")" );
		c.Reset();
		const bool occ = c.csg->IntersectRay_IntersectionOnly( Ray( Point3( 0.1, 0.05, 5 ), down ), RISE_INFINITY, true, true );
		Check( occ, "A: overlapping-sphere composite occludes" );
		Check( c.gA->casts == 1 && c.gB->casts == 1,
			"A: each analytic operand is intersected exactly once per shadow query" );
		// The exit published by the composite is the sphere's own.
		if( op == CSG_UNION ) {
			Check( std::fabs( h.geometric.range2 - ( 5 + 0.5 + std::sqrt( 1 - 0.0125 ) ) ) < 1e-6,
				"A: union exit is the lower sphere's published exit" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// B. no AdvanceOperandPast re-cast past a single-lobe operand
//////////////////////////////////////////////////////////////////////

static void TestNoAdvancePastSingleLobe()
{
	std::cout << "B. single-lobe operands: no advance re-cast on the sequential miss" << std::endl;
	const Vector3 down( 0, 0, -1 );
	const Point3 o( 0.1, 0.05, 10 );

	// Intersection, A's interval [7.x, 9.x] wholly before B's [11.x, 13.x].
	{
		CountedCsg c;
		MakeCountedCsg( c, CSG_INTERSECTION, Point3( 0, 0, 2 ), Point3( 0, 0, -2 ) );
		c.Reset();
		const RayIntersection h = CastObj( *c.csg, o, down );
		Check( !h.geometric.bHit, "B: disjoint-in-sequence intersection misses" );
		Check( c.gA->casts == 1 && c.gB->casts == 1,
			"B: intersection miss costs one cast per operand (A " + std::to_string( c.gA->casts ) +
			", B " + std::to_string( c.gB->casts ) + ")" );
		c.Reset();
		const bool occ = c.csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true );
		Check( !occ, "B: disjoint-in-sequence intersection does not occlude" );
		Check( c.gA->casts == 1 && c.gB->casts == 1, "B: intersection shadow miss costs one cast per operand" );
	}
	// Same with the operands swapped (B's interval first).
	{
		CountedCsg c;
		MakeCountedCsg( c, CSG_INTERSECTION, Point3( 0, 0, -2 ), Point3( 0, 0, 2 ) );
		c.Reset();
		const RayIntersection h = CastObj( *c.csg, o, down );
		Check( !h.geometric.bHit && c.gA->casts == 1 && c.gB->casts == 1,
			"B: swapped intersection miss costs one cast per operand" );
	}
	// Subtraction, the subtracted B's interval wholly before A's: the
	// composite is A, unchanged.
	{
		CountedCsg c;
		MakeCountedCsg( c, CSG_SUBTRACTION, Point3( 0, 0, -2 ), Point3( 0, 0, 2 ) );
		c.Reset();
		const RayIntersection h = CastObj( *c.csg, o, down );
		Check( h.geometric.bHit && std::fabs( h.geometric.range - ( 12 - std::sqrt( 1 - 0.0125 ) ) ) < 1e-6,
			"B: subtraction with B before A is A" );
		Check( c.gA->casts == 1 && c.gB->casts == 1,
			"B: subtraction pre-pass costs no re-cast of a single-lobe B (B " + std::to_string( c.gB->casts ) + ")" );
		c.Reset();
		const bool occ = c.csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true );
		Check( occ && c.gA->casts == 1 && c.gB->casts == 1, "B: subtraction shadow pre-pass costs no re-cast" );
	}
	// The one place the shipped CSG scenes render differently from
	// pre-DL-621 master (irradiance_cache_torture, csg_comprehensive): a
	// ray that STARTS inside the subtracted B, leaves B, and only then
	// meets A.  The composite there is A alone; the pre-DL-621 algebra
	// reported a miss (a hole through A wherever B protrudes from it).
	{
		CountedCsg c;
		MakeCountedCsg( c, CSG_SUBTRACTION, Point3( 0, 0, -2 ), Point3( 0, 0, 10 ) );
		const RayIntersection h = CastObj( *c.csg, o, down );
		Check( h.geometric.bHit && std::fabs( h.geometric.range - ( 12 - std::sqrt( 1 - 0.0125 ) ) ) < 1e-6,
			"B: a ray leaving the subtracted B before it meets A hits A" );
		Check( c.csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true ),
			"B: ... and is occluded by A" );
	}
}

//////////////////////////////////////////////////////////////////////
// C. a near-closed mesh is not a sheet
//////////////////////////////////////////////////////////////////////

static void TestNearClosedMeshRole()
{
	std::cout << "C. near-closed meshes: pinhole / T-junction are undecided, real sheets stay sheets" << std::endl;
	// Classification.
	{
		const std::vector<Point3> closed = LatLongSphereCorners( 16, 32, 1, -1 );
		Check( GeometricUtilities::TriangleMeshOpenness( closed ) == 0, "C: closed lat-long sphere is closed (control)" );
		// Drop one quad-half on ring 7 (near the equator): a pinhole of
		// ~1/1000 of the area.
		const std::vector<Point3> pinhole = LatLongSphereCorners( 16, 32, 1, 32 + 2 * 32 * 6 + 3 );
		Check( GeometricUtilities::TriangleMeshOpenness( pinhole ) == -1,
			"C: sphere with a one-triangle pinhole is undecided (-1), not a sheet (got " +
			std::to_string( GeometricUtilities::TriangleMeshOpenness( pinhole ) ) + ")" );
		const std::vector<Point3> tj = CubeCorners( true );
		Check( GeometricUtilities::TriangleMeshOpenness( tj ) == -1,
			"C: cube with an unwelded T-junction is undecided (-1), not a sheet (got " +
			std::to_string( GeometricUtilities::TriangleMeshOpenness( tj ) ) + ")" );
		Check( GeometricUtilities::TriangleMeshOpenness( CubeCorners( false ) ) == 0, "C: closed cube is closed (control)" );

		// Controls that ARE sheets: a hemispherical bowl, a cube missing
		// one face, a flat quad.
		std::vector<Point3> bowl;
		{
			const std::vector<Point3> all = LatLongSphereCorners( 16, 32, 1, -1 );
			for( std::size_t k = 0; k < all.size(); k += 3 ) {
				if( all[k].z + all[k + 1].z + all[k + 2].z > 0 ) {
					PushTri( bowl, all[k], all[k + 1], all[k + 2] );
				}
			}
		}
		Check( GeometricUtilities::TriangleMeshOpenness( bowl ) == 1, "C: hemispherical bowl is a sheet (control)" );
		std::vector<Point3> openBox = CubeCorners( false );
		openBox.erase( openBox.begin() + 6, openBox.begin() + 12 );	// drop the +Z face (triangles 2, 3)
		Check( GeometricUtilities::TriangleMeshOpenness( openBox ) == 1, "C: cube missing a face is a sheet (control)" );
		std::vector<Point3> quad;
		PushTri( quad, Point3( -1, -1, 0 ), Point3( 1, -1, 0 ), Point3( 1, 1, 0 ) );
		PushTri( quad, Point3( -1, -1, 0 ), Point3( 1, 1, 0 ), Point3( -1, 1, 0 ) );
		Check( GeometricUtilities::TriangleMeshOpenness( quad ) == 1, "C: flat quad is a sheet (control)" );
		// Two flat quads touching at ONE vertex and wound against each
		// other: each is a sheet, and their boundary loops must not cancel
		// just because they meet at a vertex.
		std::vector<Point3> bow;
		PushTri( bow, Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ) );
		PushTri( bow, Point3( 0, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 ) );
		PushTri( bow, Point3( 0, 0, 0 ), Point3( -1, -1, 0 ), Point3( -1, 0, 0 ) );
		PushTri( bow, Point3( 0, 0, 0 ), Point3( 0, -1, 0 ), Point3( -1, -1, 0 ) );
		Check( GeometricUtilities::TriangleMeshOpenness( bow ) == 1,
			"C: two opposite-wound quads touching at a vertex are a sheet (got " +
			std::to_string( GeometricUtilities::TriangleMeshOpenness( bow ) ) + ")" );
	}

	// Composite: intersection( pinhole sphere mesh, ball centred on its
	// surface ).  The lens they share is solid; trimmed as a sheet, the
	// ray at x = 0.8 would see no surface of the mesh inside the ball and
	// miss outright.
	{
		std::vector<Point3> v;
		std::vector<int> t;
		LatLongSphereCorners( 16, 32, 1, 32 + 2 * 32 * 6 + 3, &v, &t );
		std::string scene = Materials( false ) + IndexedMeshText( "pinsphere", v, t ) +
			"sphere_geometry\n{\n name ball\n radius 0.5\n}\n"
			"standard_object\n{\n name o_pin\n geometry pinsphere\n material glass\n}\n"
			"standard_object\n{\n name o_ball\n geometry ball\n material glass\n position 1 0 0\n}\n"
			"csg_object\n{\n name lens\n obja o_pin\n objb o_ball\n operation intersection\n material glass\n}\n";
		LoadedScene loaded( scene );
		const CSGObject* lens = dynamic_cast<const CSGObject*>( loaded.Object( "lens" ) );
		Check( lens != nullptr, "C: near-closed lens fixture loads" );
		if( lens ) {
			Check( !lens->TrimsSheet(), "C: intersection( pinhole sphere, ball ) does not trim the mesh as a sheet" );
			const RayIntersection h = CastObj( *lens, Point3( 0.8, 0, 5 ), Vector3( 0, 0, -1 ) );
			const Scalar expect = 5 - std::sqrt( 0.25 - 0.04 );
			Check( h.geometric.bHit && std::fabs( h.geometric.range - expect ) < 1e-6,
				"C: the ray through the lens enters it at the ball's surface (got hit=" +
				std::to_string( h.geometric.bHit ) + " range=" + std::to_string( h.geometric.range ) + ")" );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// D. zero-volume pillow is not closed
//////////////////////////////////////////////////////////////////////

static void TestPillowRole()
{
	std::cout << "D. pillow of two coincident sheets is not a closed solid" << std::endl;
	// Two unit quads 1e-12 apart (inside the weld epsilon), split on
	// DIFFERENT diagonals and wound oppositely, so after the weld every
	// edge is shared by exactly two triangles and no two triangles share
	// all three corners -- the 2-triangle pillow test cannot see it.
	const Scalar g = 1e-12;
	std::vector<Point3> pillow;
	PushTri( pillow, Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ) );
	PushTri( pillow, Point3( 0, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 ) );
	PushTri( pillow, Point3( 0, 0, g ), Point3( 0, 1, g ), Point3( 1, 0, g ) );
	PushTri( pillow, Point3( 1, 0, g ), Point3( 0, 1, g ), Point3( 1, 1, g ) );
	Check( GeometricUtilities::TriangleMeshOpenness( pillow ) == -1,
		"D: welded zero-volume pillow is undecided (-1), not closed (got " +
		std::to_string( GeometricUtilities::TriangleMeshOpenness( pillow ) ) + ")" );

	// Control: a closed cube whose bottom face carries a flat pleat (a
	// coplanar fold inside a face) encloses volume and stays closed.
	std::vector<Point3> pleat = CubeCorners( false );
	pleat.erase( pleat.begin(), pleat.begin() + 6 );	// drop the -Z face
	const Point3 a( -1, -1, -1 ), b( 1, -1, -1 ), c( 1, 1, -1 ), d( -1, 1, -1 );
	const Point3 p8( 0, -0.90, -1 ), p9( 0, -0.95, -1 );
	PushTri( pleat, a, p8, b );
	PushTri( pleat, a, p9, p8 );
	PushTri( pleat, p8, p9, b );
	PushTri( pleat, a, c, p9 );
	PushTri( pleat, p9, c, b );
	PushTri( pleat, a, d, c );
	Check( GeometricUtilities::TriangleMeshOpenness( pleat ) == 0, "D: pleated closed cube stays closed (control)" );

	// Composite: the pillow is no longer a closed operand, so a sheet
	// unioned with it is not given the sheet-union-solid path.
	TriangleMeshGeometry* gp = new TriangleMeshGeometry( true );
	gp->BeginTriangles();
	for( std::size_t k = 0; k < pillow.size(); k += 3 ) {
		Triangle tri;
		for( int m = 0; m < 3; ++m ) {
			tri.vertices[m] = pillow[k + m];
			tri.normals[m] = Vector3( 0, 0, 1 );
			tri.coords[m] = Point2( 0, 0 );
		}
		gp->AddTriangle( tri );
	}
	gp->DoneTriangles();
	Check( gp->TopologicalOpenness() == -1, "D: TriangleMeshGeometry pillow reports TopologicalOpenness -1" );
	safe_release( gp );
}

//////////////////////////////////////////////////////////////////////
// E. concurrent first query builds once
//////////////////////////////////////////////////////////////////////

static TriangleMeshGeometry* MakeGridSheet( int n )
{
	TriangleMeshGeometry* g = new TriangleMeshGeometry( true );
	g->BeginTriangles();
	for( int i = 0; i < n; ++i ) {
		for( int j = 0; j < n; ++j ) {
			const Point3 p00( i, j, 0 ), p10( i + 1, j, 0 ), p11( i + 1, j + 1, 0 ), p01( i, j + 1, 0 );
			const Point3 q[2][3] = { { p00, p10, p11 }, { p00, p11, p01 } };
			for( int s = 0; s < 2; ++s ) {
				Triangle tri;
				for( int m = 0; m < 3; ++m ) {
					tri.vertices[m] = q[s][m];
					tri.normals[m] = Vector3( 0, 0, 1 );
					tri.coords[m] = Point2( 0, 0 );
				}
				g->AddTriangle( tri );
			}
		}
	}
	g->DoneTriangles();
	return g;
}

static double ProcessCpuSeconds()
{
	return double( std::clock() ) / double( CLOCKS_PER_SEC );
}

static void TestConcurrentFirstQuery()
{
	std::cout << "E. concurrent first openness query builds the topology once" << std::endl;
	const int n = 500;	// 500000 triangles
	TriangleMeshGeometry* single = MakeGridSheet( n );
	const double c0 = ProcessCpuSeconds();
	const int r0 = single->TopologicalOpenness();
	const double oneBuild = ProcessCpuSeconds() - c0;
	safe_release( single );

	TriangleMeshGeometry* shared = MakeGridSheet( n );
	const unsigned int nThreads = 8;
	std::atomic<unsigned int> ready{ 0 };
	std::atomic<bool> go{ false };
	std::vector<int> results( nThreads, -3 );
	std::vector<std::thread> threads;
	const double c1 = ProcessCpuSeconds();
	for( unsigned int k = 0; k < nThreads; ++k ) {
		threads.emplace_back( [&, k]() {
			ready.fetch_add( 1 );
			while( !go.load() ) {
				std::this_thread::yield();
			}
			results[k] = shared->TopologicalOpenness();
		} );
	}
	while( ready.load() < nThreads ) {
		std::this_thread::yield();
	}
	const double c2 = ProcessCpuSeconds();
	go.store( true );
	for( std::thread& t : threads ) {
		t.join();
	}
	const double concurrent = ProcessCpuSeconds() - c2;
	(void)c1;
	safe_release( shared );
	bool agree = ( r0 == 1 );
	for( const int r : results ) {
		agree = agree && ( r == r0 );
	}
	std::cout << "    one build " << oneBuild * 1e3 << " ms CPU; " << nThreads << " concurrent first queries "
		<< concurrent * 1e3 << " ms CPU" << std::endl;
	Check( agree, "E: every thread reads the same openness (+1 sheet)" );
	Check( concurrent < Scalar( 3 ) * oneBuild + 0.02,
		"E: concurrent first queries cost about one build, not one per thread" );
}

//////////////////////////////////////////////////////////////////////
// F. subtraction from inside B through all of A
//////////////////////////////////////////////////////////////////////

static Object* MakeSphereObject( Scalar r, const Point3& p )
{
	SphereGeometry* g = new SphereGeometry( r );
	Object* o = new Object( g );
	safe_release( g );
	o->SetPosition( p );
	o->FinalizeTransformations();
	return o;
}

static void TestInsideBThroughA()
{
	std::cout << "F. subtraction: a ray inside B that crosses all of A before leaving B" << std::endl;
	const Vector3 down( 0, 0, -1 );
	const Point3 o( 0.1, 0.05, 4 );
	const Scalar h = std::sqrt( 1 - 0.0125 );
	// A: unit sphere at the origin (z in [-1, 1]); B: radius 4 at z = 2,
	// containing the ray origin and all of A along the ray.  A - B is
	// empty along the ray.
	{
		Object* oA = MakeSphereObject( 1, Point3( 0, 0, 0 ) );
		Object* oB = MakeSphereObject( 4, Point3( 0, 0, 2 ) );
		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		csg->AssignObjects( oA, oB );
		csg->FinalizeTransformations();
		safe_release( oA );
		safe_release( oB );
		const RayIntersection hit = CastObj( *csg, o, down );
		Check( !hit.geometric.bHit, "F: A wholly inside B along the ray: A - B is missed (got range " +
			std::to_string( hit.geometric.range ) + ")" );
		Check( !csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true ),
			"F: ... and does not occlude" );
		// Control: B ends inside A (B radius 2.5 -> exit at z = -0.5).
		safe_release( csg );
	}
	{
		Object* oA = MakeSphereObject( 1, Point3( 0, 0, 0 ) );
		Object* oB = MakeSphereObject( 2.5, Point3( 0, 0, 2 ) );
		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		csg->AssignObjects( oA, oB );
		csg->FinalizeTransformations();
		safe_release( oA );
		safe_release( oB );
		const RayIntersection hit = CastObj( *csg, o, down );
		const Scalar bExit = 2 + std::sqrt( 6.25 - 0.0125 );
		Check( hit.geometric.bHit && std::fabs( hit.geometric.range - bExit ) < 1e-6,
			"F: B exits inside A: the composite starts at B's exit (control)" );
		Check( csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true ),
			"F: ... and occludes (control)" );
		safe_release( csg );
	}
	// A two-lobe A (a union: lobes z in [-1,1] and [-9,-7]); the second
	// lobe lies past B's exit, so it is the composite's surface.
	{
		Object* l0 = MakeSphereObject( 1, Point3( 0, 0, 0 ) );
		Object* l1 = MakeSphereObject( 1, Point3( 0, 0, -8 ) );
		CSGObject* uni = new CSGObject( CSG_UNION );
		uni->AssignObjects( l0, l1 );
		uni->FinalizeTransformations();
		safe_release( l0 );
		safe_release( l1 );
		Object* oB = MakeSphereObject( 4, Point3( 0, 0, 2 ) );
		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		csg->AssignObjects( uni, oB );
		csg->FinalizeTransformations();
		safe_release( uni );
		safe_release( oB );
		const RayIntersection hit = CastObj( *csg, o, down );
		const Scalar expect = 4 + 8 - h;
		Check( hit.geometric.bHit && std::fabs( hit.geometric.range - expect ) < 1e-6,
			"F: two-lobe A: the lobe past B's exit is hit (got hit=" + std::to_string( hit.geometric.bHit ) +
			" range " + std::to_string( hit.geometric.range ) + ")" );
		Check( csg->IntersectRay_IntersectionOnly( Ray( o, down ), RISE_INFINITY, true, true ),
			"F: two-lobe A: the lobe past B's exit occludes" );
		Check( !csg->IntersectRay_IntersectionOnly( Ray( o, down ), Scalar( 10 ), true, true ),
			"F: two-lobe A: ... but not before it" );
		safe_release( csg );
	}
}

//////////////////////////////////////////////////////////////////////
// G. a mesh edit re-derives the parent composite's cached roles
//////////////////////////////////////////////////////////////////////

static void FillMesh( TriangleMeshGeometry* g, const std::vector<Point3>& corners )
{
	g->BeginTriangles();
	for( std::size_t k = 0; k < corners.size(); k += 3 ) {
		Triangle tri;
		const Vector3 n = Vector3Ops::Normalize( Vector3Ops::Cross(
			Vector3Ops::mkVector3( corners[k + 1], corners[k] ), Vector3Ops::mkVector3( corners[k + 2], corners[k] ) ) );
		for( int m = 0; m < 3; ++m ) {
			tri.vertices[m] = corners[k + m];
			tri.normals[m] = n;
			tri.coords[m] = Point2( 0, 0 );
		}
		g->AddTriangle( tri );
	}
	g->DoneTriangles();
}

static void TestMeshEditRefreshesComposite()
{
	std::cout << "G. a mesh edit re-derives the composite's cached operand roles" << std::endl;
	// Start as the cube [-1,1]^3 without its +Z face (an open box: a
	// sheet), intersected with a ball; then append the two +Z triangles
	// (`FillMesh` appends -- BeginTriangles does not clear) so the same
	// geometry becomes the closed cube.
	const std::vector<Point3> cube = CubeCorners( false );
	std::vector<Point3> openBox( cube.begin(), cube.begin() + 6 );
	openBox.insert( openBox.end(), cube.begin() + 12, cube.end() );
	const std::vector<Point3> lid( cube.begin() + 6, cube.begin() + 12 );
	TriangleMeshGeometry* g = new TriangleMeshGeometry( true );
	FillMesh( g, openBox );
	Object* oMesh = new Object( g );
	oMesh->FinalizeTransformations();
	Object* oBall = MakeSphereObject( 1, Point3( 0, 0, 0 ) );
	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	csg->AssignObjects( oMesh, oBall );
	csg->FinalizeTransformations();
	safe_release( oMesh );
	safe_release( oBall );
	csg->Realize();
	Check( csg->TrimsSheet(), "G: open box ∩ ball trims the sheet" );
	// Edit: close the box (no Realize).
	FillMesh( g, lid );
	Check( !csg->TrimsSheet() && csg->IsTopologicallyClosed(),
		"G: after the mesh becomes closed, the composite re-derives its roles without a Realize" );
	const RayIntersection hit = CastObj( *csg, Point3( 0.1, 0.05, 5 ), Vector3( 0, 0, -1 ) );
	Check( hit.geometric.bHit && std::fabs( hit.geometric.range - ( 5 - std::sqrt( 1 - 0.0125 ) ) ) < 1e-6,
		"G: ... and renders the cube ∩ ball solid (enters at the ball)" );
	safe_release( g );
	safe_release( csg );
}

//////////////////////////////////////////////////////////////////////
// H. shadow rays respect their length from inside a composite
//////////////////////////////////////////////////////////////////////

//! The shadow query must agree with the radiance query: it occludes
//! exactly when the composite's first boundary lies before `far`.
static void CheckShadowAgreesWithHit( const CSGObject& csg, const Point3& o, const Vector3& d,
	const std::string& label, Scalar expectRange )
{
	const RayIntersection hit = CastObj( csg, o, d );
	Check( hit.geometric.bHit && std::fabs( hit.geometric.range - expectRange ) < 1e-6,
		"H: " + label + ": radiance first boundary at the closed form (got " + std::to_string( hit.geometric.range ) + ")" );
	for( const Scalar f : { Scalar( 0.25 ) * expectRange, Scalar( 0.75 ) * expectRange,
			Scalar( 1.25 ) * expectRange, Scalar( 3 ) * expectRange } ) {
		const bool occ = csg.IntersectRay_IntersectionOnly( Ray( o, d ), f, true, true );
		Check( occ == ( f > expectRange ),
			"H: " + label + ": shadow ray of length " + std::to_string( f ) + ( occ ? " occluded" : " not occluded" ) +
			" (boundary at " + std::to_string( expectRange ) + ")" );
	}
}

static CSGObject* MakeTwoSphereCsg( const CSG_OP op, Scalar rA, const Point3& pA, Scalar rB, const Point3& pB )
{
	Object* oA = MakeSphereObject( rA, pA );
	Object* oB = MakeSphereObject( rB, pB );
	CSGObject* csg = new CSGObject( op );
	csg->AssignObjects( oA, oB );
	csg->FinalizeTransformations();
	safe_release( oA );
	safe_release( oB );
	return csg;
}

static void TestShadowLengthFromInside()
{
	std::cout << "H. shadow rays from inside a composite respect their length" << std::endl;
	const Scalar r2 = 0.0125;	// (0.1, 0.05) offset squared
	// Subtraction, origin inside A (r 3), outside the small cavity B
	// (r 0.5 at z = -2).  Down: first boundary is the cavity's entry.
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_SUBTRACTION, 3, Point3( 0, 0, 0 ), 0.5, Point3( 0, 0, -2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 1 ), Vector3( 0, 0, -1 ),
			"A - B from inside A toward B", 3 - std::sqrt( 0.25 - r2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 1 ), Vector3( 0, 0, 1 ),
			"A - B from inside A away from B", std::sqrt( 9 - r2 ) - 1 );
		safe_release( csg );
	}
	// Union of overlapping unit spheres at z = 0 and z = -1.5.
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_UNION, 1, Point3( 0, 0, 0 ), 1, Point3( 0, 0, -1.5 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 0 ), Vector3( 0, 0, -1 ),
			"A u B from inside A only", 1.5 + std::sqrt( 1 - r2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, -0.75 ), Vector3( 0, 0, -1 ),
			"A u B from inside both", 0.75 + std::sqrt( 1 - r2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 5 ), Vector3( 0, 0, -1 ),
			"A u B from outside (control)", 5 - std::sqrt( 1 - r2 ) );
		safe_release( csg );
	}
	// Union coverage: inside A with B past a gap; inside B only; a ray
	// that meets only one operand.
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_UNION, 1, Point3( 0, 0, 0 ), 1, Point3( 0, 0, -3 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 0 ), Vector3( 0, 0, -1 ),
			"A u B from inside A, B past a gap", std::sqrt( 1 - r2 ) );
		safe_release( csg );
	}
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_UNION, 1, Point3( 0, 0, -1.5 ), 1, Point3( 0, 0, 0 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 0.5 ), Vector3( 0, 0, -1 ),
			"A u B from inside B only", 2 + std::sqrt( 1 - r2 ) );
		safe_release( csg );
	}
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_UNION, 1, Point3( 0, 0, 0 ), 1, Point3( 5, 0, 0 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 5 ), Vector3( 0, 0, -1 ),
			"A u B, the ray meets A only", 5 - std::sqrt( 1 - r2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 0 ), Vector3( 0, 0, -1 ),
			"A u B, from inside A, the ray meets A only", std::sqrt( 1 - r2 ) );
		safe_release( csg );
	}
	// Subtraction from inside B: B (r 2.5 at z = 2) ends inside A.
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_SUBTRACTION, 1, Point3( 0, 0, 0 ), 2.5, Point3( 0, 0, 2 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 4 ), Vector3( 0, 0, -1 ),
			"A - B from inside B", 2 + std::sqrt( 6.25 - r2 ) );
		safe_release( csg );
	}
	// Multi-lobe A (a union composite, so the advance paths run):
	// lobes r 0.5 at the origin and r 1 at z = zLobe; B r 4 at z = 2
	// contains the ray origin and all of the first lobe.
	for( const Scalar zLobe : { Scalar( -2.5 ), Scalar( -8 ) } ) {
		Object* l0 = MakeSphereObject( 0.5, Point3( 0, 0, 0 ) );
		Object* l1 = MakeSphereObject( 1, Point3( 0, 0, zLobe ) );
		CSGObject* uni = new CSGObject( CSG_UNION );
		uni->AssignObjects( l0, l1 );
		uni->FinalizeTransformations();
		safe_release( l0 );
		safe_release( l1 );
		Object* oB = MakeSphereObject( 4, Point3( 0, 0, 2 ) );
		CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
		csg->AssignObjects( uni, oB );
		csg->FinalizeTransformations();
		safe_release( uni );
		safe_release( oB );
		const Scalar bExit = 2 + std::sqrt( 16 - r2 );
		if( zLobe > -4 ) {
			// The second lobe [5.5, 7.5] straddles B's exit (~6): the
			// surface is B's exit, closed by the lobe's exit.
			CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 4 ), Vector3( 0, 0, -1 ),
				"multi-lobe A - B, second lobe straddles B's exit", bExit );
			const RayIntersection h = CastObj( *csg, Point3( 0.1, 0.05, 4 ), Vector3( 0, 0, -1 ) );
			Check( h.geometric.bHit && std::fabs( h.geometric.range2 - ( 4 - zLobe + std::sqrt( 1 - r2 ) ) ) < 1e-6,
				"H: multi-lobe A - B, straddling lobe: the interval closes at the lobe's exit (got " +
				std::to_string( h.geometric.range2 ) + ")" );
			Check( h.geometric.bHit && h.geometric.vNormal.z > 0,
				"H: multi-lobe A - B, straddling lobe: B's exit normal faces back into the cavity" );
		} else {
			CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 4 ), Vector3( 0, 0, -1 ),
				"multi-lobe A - B, second lobe past B's exit", 4 - zLobe - std::sqrt( 1 - r2 ) );
		}
		safe_release( csg );
	}
	// Intersection and subtraction from outside (controls).
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_INTERSECTION, 1, Point3( 0, 0, 0 ), 1, Point3( 0, 0, -1.5 ) );
		CheckShadowAgreesWithHit( *csg, Point3( 0.1, 0.05, 5 ), Vector3( 0, 0, -1 ),
			"A n B from outside (control)", 5 + 1.5 - std::sqrt( 1 - r2 ) );
		safe_release( csg );
	}
}

//////////////////////////////////////////////////////////////////////
// I. subtraction from inside a multi-lobe B (a torus)
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Geometry/TorusGeometry.h"

//! A = union of two spheres on the x axis, given as ray-parameter
//! intervals [lo, hi] along the ray from (-3.5, 0, 0) toward +x.
static CSGObject* MakeTorusSubtraction( Scalar lo0, Scalar hi0, Scalar lo1, Scalar hi1 )
{
	const Scalar ox = -3.5;
	Object* l0 = MakeSphereObject( 0.5 * ( hi0 - lo0 ), Point3( ox + 0.5 * ( lo0 + hi0 ), 0, 0 ) );
	Object* l1 = MakeSphereObject( 0.5 * ( hi1 - lo1 ), Point3( ox + 0.5 * ( lo1 + hi1 ), 0, 0 ) );
	CSGObject* uni = new CSGObject( CSG_UNION );
	uni->AssignObjects( l0, l1 );
	uni->FinalizeTransformations();
	safe_release( l0 );
	safe_release( l1 );
	TorusGeometry* gt = new TorusGeometry( 3, 1 );
	Object* oB = new Object( gt );
	safe_release( gt );
	oB->FinalizeTransformations();
	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	csg->AssignObjects( uni, oB );
	csg->FinalizeTransformations();
	safe_release( uni );
	safe_release( oB );
	return csg;
}

static void TestSubtractionInsideMultiLobeB()
{
	std::cout << "I. subtraction from inside a torus B (B exits 1.5, re-enters 5.5, exits 7.5)" << std::endl;
	const Point3 o( -3.5, 0, 0 );
	const Vector3 dx( 1, 0, 0 );
	// Precondition: the torus intervals along this ray.
	{
		TorusGeometry* gt = new TorusGeometry( 3, 1 );
		Object* oB = new Object( gt );
		safe_release( gt );
		oB->FinalizeTransformations();
		const RayIntersection h0 = CastObj( *oB, o, dx );
		const RayIntersection h1 = CastObj( *oB, Point3( -1, 0, 0 ), dx );
		Check( h0.geometric.bHit && std::fabs( h0.geometric.range - 1.5 ) < 1e-6 &&
			h1.geometric.bHit && std::fabs( h1.geometric.range - 3.0 ) < 1e-6,
			"I: (precondition) torus R 3 r 1 is crossed at 1.5 and re-entered at 5.5 along the ray" );
		safe_release( oB );
	}
	// Phantom: A's lobes [0.7, 1.1] and [5.7, 6.3] both lie inside B.
	{
		CSGObject* csg = MakeTorusSubtraction( 0.7, 1.1, 5.7, 6.3 );
		const RayIntersection h = CastObj( *csg, o, dx );
		Check( !h.geometric.bHit, "I: A - torus with every A lobe inside B is missed (got range " +
			std::to_string( h.geometric.range ) + ")" );
		Check( !csg->IntersectRay_IntersectionOnly( Ray( o, dx ), 20, true, true ),
			"I: ... and does not occlude" );
		safe_release( csg );
	}
	// Straddle: lobe [1.3, 6.0] straddles B's exit (1.5) and B re-enters
	// (5.5) inside it -- the interval is [1.5, 5.5].
	{
		CSGObject* csg = MakeTorusSubtraction( 0.7, 1.1, 1.3, 6.0 );
		CheckShadowAgreesWithHit( *csg, o, dx, "A - torus, straddling lobe", 1.5 );
		const RayIntersection h = CastObj( *csg, o, dx );
		Check( h.geometric.bHit && std::fabs( h.geometric.range2 - 5.5 ) < 1e-6,
			"I: A - torus, straddling lobe: the interval closes at B's re-entry (got " +
			std::to_string( h.geometric.range2 ) + ")" );
		Check( h.geometric.bHit && h.geometric.vNormal2.x > 0,
			"I: A - torus, straddling lobe: the closing normal points along the ray" );
		safe_release( csg );
	}
	// A entry outside B, B covering its tail: lobe [2.0, 6.0] -> [2.0, 5.5].
	{
		CSGObject* csg = MakeTorusSubtraction( 0.7, 1.1, 2.0, 6.0 );
		CheckShadowAgreesWithHit( *csg, o, dx, "A - torus, lobe entered after B's exit", 2.0 );
		const RayIntersection h = CastObj( *csg, o, dx );
		Check( h.geometric.bHit && std::fabs( h.geometric.range2 - 5.5 ) < 1e-6,
			"I: A - torus, lobe after B's exit: the interval closes at B's re-entry (got " +
			std::to_string( h.geometric.range2 ) + ")" );
		safe_release( csg );
	}
	// A lobe past B's second lobe: [7.8, 8.4] is the surface.
	{
		CSGObject* csg = MakeTorusSubtraction( 0.7, 1.1, 7.8, 8.4 );
		CheckShadowAgreesWithHit( *csg, o, dx, "A - torus, lobe past B's second lobe", 7.8 );
		safe_release( csg );
	}
}

//////////////////////////////////////////////////////////////////////
// J. subtraction from inside both; budget exhaustion; touching lobes
//////////////////////////////////////////////////////////////////////

static Object* MakeTorusObject()
{
	TorusGeometry* gt = new TorusGeometry( 3, 1 );
	Object* o = new Object( gt );
	safe_release( gt );
	o->FinalizeTransformations();
	return o;
}

static CSGObject* Subtract( IObjectPriv* a, IObjectPriv* b )
{
	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	csg->AssignObjects( a, b );
	csg->FinalizeTransformations();
	safe_release( a );
	safe_release( b );
	return csg;
}

static void CheckMiss( const CSGObject& csg, const Point3& o, const Vector3& d, const std::string& label )
{
	const RayIntersection h = CastObj( csg, o, d );
	Check( !h.geometric.bHit, "J: " + label + ": no hit (got range " + std::to_string( h.geometric.range ) + ")" );
	Check( !csg.IntersectRay_IntersectionOnly( Ray( o, d ), 20, true, true ), "J: " + label + ": no occlusion" );
}

static void TestSubtractionInsideBoth()
{
	std::cout << "J. subtraction from inside both / budget / touching lobes" << std::endl;
	const Point3 o( -3.5, 0, 0 );
	const Vector3 dx( 1, 0, 0 );
	// P2: inside both, multi-lobe A (lobes [-1, 1.0] and [1.3, 6.0]) and
	// torus B (exits 1.5, re-enters 5.5): A's first lobe ends inside B,
	// its second straddles B's exit -> surface at 1.5, closed at 5.5.
	{
		CSGObject* csg = MakeTorusSubtraction( -1, 1.0, 1.3, 6.0 );
		CheckShadowAgreesWithHit( *csg, o, dx, "inside both, A's second lobe straddles B's exit", 1.5 );
		const RayIntersection h = CastObj( *csg, o, dx );
		Check( h.geometric.bHit && std::fabs( h.geometric.range2 - 5.5 ) < 1e-6,
			"J: inside both, straddle: interval closes at B's re-entry (got " + std::to_string( h.geometric.range2 ) + ")" );
		safe_release( csg );
	}
	// P3-1: inside both, B exits first (1.5) and re-enters (5.5) before
	// A's exit (6.0): interval [1.5, 5.5].
	{
		CSGObject* csg = Subtract( MakeSphereObject( 3.5, Point3( -1, 0, 0 ) ), MakeTorusObject() );
		CheckShadowAgreesWithHit( *csg, o, dx, "inside both, B exits first", 1.5 );
		const RayIntersection h = CastObj( *csg, o, dx );
		Check( h.geometric.bHit && std::fabs( h.geometric.range2 - 5.5 ) < 1e-6,
			"J: inside both, B exits first: interval closes at B's re-entry (got " + std::to_string( h.geometric.range2 ) + ")" );
		Check( h.geometric.bHit && h.geometric.vNormal2.x > 0,
			"J: inside both, B exits first: the closing normal points along the ray" );
		safe_release( csg );
	}
	// P3-2: the re-cast allowance runs out after A's advance; B's second
	// lobe (which hides A's second lobe) is never visited -> conservative miss.
	{
		CSGObject::SetAdvanceBudgetForTesting( 1 );
		CSGObject* csg = MakeTorusSubtraction( 0.7, 1.1, 5.7, 6.3 );
		CheckMiss( *csg, o, dx, "budget exhausted after A's advance" );
		safe_release( csg );
		CSGObject::SetAdvanceBudgetForTesting( 0 );
		CSGObject* full = MakeTorusSubtraction( 0.7, 1.1, 7.8, 8.4 );
		CheckShadowAgreesWithHit( *full, o, dx, "full budget restored (control)", 7.8 );
		safe_release( full );
	}
	// P3-3: B is two spheres touching on the ray at t = 1 (a union, so
	// the ray starting inside the first reports its exit at the seam).
	// A (t in [-1.5, 2]) lies wholly inside B -> nothing.
	{
		Object* b0 = MakeSphereObject( 1, Point3( -3.5, 0, 0 ) );
		Object* b1 = MakeSphereObject( 1, Point3( -1.5, 0, 0 ) );
		CSGObject* uniB = new CSGObject( CSG_UNION );
		uniB->AssignObjects( b0, b1 );
		uniB->FinalizeTransformations();
		safe_release( b0 );
		safe_release( b1 );
		CSGObject* csg = Subtract( MakeSphereObject( 1.75, Point3( -3.25, 0, 0 ) ), uniB );
		CheckMiss( *csg, o, dx, "B continues through a touching lobe past A" );
		safe_release( csg );
	}
	// P3-4 pins: single-lobe operands through the new top logic.
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_SUBTRACTION, 1, Point3( -3.5, 0, 0 ), 2, Point3( -3.5, 0, 0 ) );
		CheckMiss( *csg, o, dx, "inside both single-lobe, A left first" );
		safe_release( csg );
	}
	{
		CSGObject* csg = Subtract( MakeSphereObject( 0.2, Point3( -2.6, 0, 0 ) ), MakeTorusObject() );
		CheckMiss( *csg, o, dx, "single-lobe A inside torus B" );
		safe_release( csg );
	}
	{
		CSGObject* csg = MakeTwoSphereCsg( CSG_SUBTRACTION, 2, Point3( -3.5, 0, 0 ), 1, Point3( -3.5, 0, 0 ) );
		CheckShadowAgreesWithHit( *csg, o, dx, "inside both single-lobe, B left first", 1.0 );
		safe_release( csg );
	}
}

int main()
{
	std::cout << "=== CsgMeshOperandTest (DL-621 review findings 10-13) ===" << std::endl;
	TestAnalyticOperandsCastOnce();
	TestNoAdvancePastSingleLobe();
	TestNearClosedMeshRole();
	TestPillowRole();
	TestConcurrentFirstQuery();
	TestInsideBThroughA();
	TestMeshEditRefreshesComposite();
	TestShadowLengthFromInside();
	TestSubtractionInsideMultiLobeB();
	TestSubtractionInsideBoth();
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
