//////////////////////////////////////////////////////////////////////
//
//  HairGeneratorTest.cpp - Contract test for the painter-driven GROOM
//    GENERATOR and the `hair_geometry` scene chunk (slice D of the
//    hair/fur arc, docs/HAIR_FUR_DESIGN.md section 5.3).
//
//  WHAT THIS TEST OWNS.  The RECIPE -> STRANDS transformation and the
//  chunk that authors it.  It does NOT re-test the curve primitive's
//  intersector or its curve math -- HairGeometryTest.cpp owns those --
//  and it does not test the BCSDF (HairBSDFTest.cpp) or the material
//  chunk (HairMaterialChunkTest.cpp).
//
//  The nine groups, and the specific regression each one buys:
//
//    1. DETERMINISM.  Two grooms built from the same recipe and seed
//       are bit-identical in every control point, width and root UV;
//       changing ONLY the seed changes them.  This is the contract that
//       makes an animated groom stable frame to frame, and it is the
//       one a "just use GlobalRNG" simplification would silently break.
//
//    2. ROOT PLACEMENT.  Roots land ON the tessellated base surface,
//       and they are AREA-weighted rather than triangle-weighted --
//       proved on a two-triangle base whose areas are exactly 9:1,
//       against a binomial tolerance.  A per-triangle-uniform sampler
//       would put 50/50 here and fail by ~50 standard deviations.
//
//    3. DENSITY MASK, AND STREAM KEYING.  A step-function density
//       painter over the base UV leaves EXACTLY zero roots on its masked
//       half -- not "few".  And the strands it does NOT remove are
//       BIT-IDENTICAL to the same groom grown with no density painter at
//       all: placement is keyed per candidate, so painting a bald patch
//       on one ear cannot silently rearrange the hair on the other.
//
//    4. LENGTH PAINTER + WIDTH LERP.  A 0.5 length painter halves the
//       measured arc length of every strand (checked on the UNBENT
//       configuration so arc length is exactly the authored length),
//       and the reported width at s=0 / s=1 is the authored root / tip.
//
//    5. CLUMPING.  Turning clumping on strictly reduces the mean
//       distance from a strand's tip to its clump centre's tip, at the
//       same seed.  (The test reproduces the DOCUMENTED cell rule --
//       first strand in a cell of side `clump_size` is that cell's
//       centre -- so it also pins that rule, not just "tips moved".)
//       Then, on a base of THREE well-separated islands, the exact
//       per-cell lerp target is pinned: a clumped tip lands exactly
//       `clump` of the way to its OWN cell centre's tip and nowhere
//       near any other island's, which is what separates the documented
//       spatial partition from an index-strided one that would drag
//       strands across the whole surface.
//
//    6. FRIZZ / CURL / GRAVITY, AND STREAM ISOLATION.  Non-zero
//       parameters move control points off the straight-growth
//       baseline; each strand's jitter is INDEPENDENT of its
//       neighbours'; and adding gravity to a frizzed groom displaces
//       every control point by exactly the documented gravity term and
//       nothing else -- the effects compose without perturbing each
//       other's random stream.
//
//    7. THE REALIZE CONTRACT.  A deferred groom before Realize() is
//       defined and EMPTY -- no strands, no hits, no crash; after
//       Realize() it has strands and is hit; a second Realize() changes
//       nothing.
//
//    8. THE CHUNK.  A minimal `hair_geometry` chunk parses and
//       registers a geometry; a missing `base_geometry`, an unknown
//       base name, and each out-of-range numeric are all REJECTED with
//       a diagnostic naming the offending parameter.  And four fields
//       (`segments`, `seed`, `gravity`, `frizz`) are pinned
//       BEHAVIOURALLY through the chunk -- two scenes differing in one
//       line, measured in the resulting groom.  Parsing alone only
//       proves the DESCRIPTOR declares a parameter; it cannot catch a
//       Finalize that drops it or reads the neighbouring one.
//
//    9. COMB AND THE ROOT TANGENT FRAME.  A constant comb painter
//       sweeps every strand the SAME way and by the documented amount
//       (a fraction of strand length, weighted t^2), across a base
//       whose two triangles have different first edges -- which is the
//       assertion that separates a UV-derived tangent frame from the
//       per-triangle one, and that catches a sign flip in the decode.
//       Red combs along +dP/du, green along the bitangent, and pure
//       blue does nothing at all (it is a flow map, not a normal map).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
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

#include "../src/Library/RISE_API.h"
#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Geometry/HairGenerator.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

namespace {

//! Takes ownership of the reference a `new` / `RISE_API_Create*` hands
//! back (RISE's Reference base starts at refcount 1), rather than
//! addref-ing on top of it.
template<class T>
class Owned
{
public:
	explicit Owned( T* q = 0 ) : p( q ) {}
	~Owned() { if( p ) p->release(); }
	void reset( T* q ) { if( p ) p->release(); p = q; }
	T*   release_ownership() { T* q = p; p = 0; return q; }
	T*   operator->() const { return p; }
	T&   operator*()  const { return *p; }
	T*   get()        const { return p; }
	explicit operator bool() const { return p != 0; }
private:
	T* p;
	Owned( const Owned& );
	Owned& operator=( const Owned& );
};

//////////////////////////////////////////////////////////////////////
// Test-local painters
//////////////////////////////////////////////////////////////////////

//! Density mask: 0 on the u < 0.5 half of the base UV domain, 1 on the
//! other.  A STEP, deliberately -- "exactly zero roots in the masked
//! half" is a far sharper assertion than "fewer roots".
class UVStepScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	virtual ~UVStepScalarPainter() {}
public:
	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		return ScalarTriple( ri.ptCoord.x < Scalar(0.5) ? Scalar(0) : Scalar(1) );
	}
	bool HasPerChannelVariation() const override { return false; }
};

//! Density mask that keeps a NAMED half of the base UV domain, so the
//! stream-keying test can mask out a chosen region and check that the
//! survivors elsewhere did not move.
class UVBandScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	const Scalar cut;
	virtual ~UVBandScalarPainter() {}
public:
	explicit UVBandScalarPainter( Scalar cutU ) : cut( cutU ) {}
	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		return ScalarTriple( ri.ptCoord.x < cut ? Scalar(0) : Scalar(1) );
	}
	bool HasPerChannelVariation() const override { return false; }
};

//! Constant scalar, for the length-multiplier slot.
class ConstScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	const Scalar v;
	virtual ~ConstScalarPainter() {}
public:
	explicit ConstScalarPainter( Scalar value ) : v( value ) {}
	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ) const override { return ScalarTriple( v ); }
	bool HasPerChannelVariation() const override { return false; }
};

//! A constant RGB colour painter for the `comb` slot.  This is the
//! SHIPPING UniformColorPainter rather than a test-local stub, because
//! the comb decode reads `GetColor` and the point of the test is that
//! the value an author writes in a `uniformcolor_painter` chunk means
//! what the descriptor says it means.
IPainter* MakeConstColorPainter( const double r, const double g, const double b )
{
	IPainter* p = 0;
	if( !RISE_API_CreateUniformColorPainter( &p, RISEPel( r, g, b ) ) ) { return 0; }
	return p;
}

//////////////////////////////////////////////////////////////////////
// Base-geometry fixtures
//////////////////////////////////////////////////////////////////////

//! The unit square in the z = 0 plane, two triangles, normals +Z, and
//! UV == (x, y).  Making the UV equal the position is what lets the
//! density test assert on ROOT POSITIONS while the painter is really
//! reading UVs.
//!
//! It is also the fixture the COMB test needs, because its two
//! triangles have DELIBERATELY DIFFERENT FIRST EDGES: (0,0)->(1,0) for
//! the lower one and (0,0)->(1,1) for the upper.  An edge-derived
//! tangent frame therefore differs by 45 degrees between them while a
//! UV-derived one (dP/du = (1,0,0) on both, since UV == position) does
//! not -- which is exactly the difference group 9 measures.
ITriangleMeshGeometryIndexed* MakeUnitQuad()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) {
		return 0;
	}
	m->BeginIndexedTriangles();
	m->AddVertex( Point3( 0, 0, 0 ) );
	m->AddVertex( Point3( 1, 0, 0 ) );
	m->AddVertex( Point3( 1, 1, 0 ) );
	m->AddVertex( Point3( 0, 1, 0 ) );
	for( int i = 0; i < 4; ++i ) { m->AddNormal( Vector3( 0, 0, 1 ) ); }
	m->AddTexCoord( Point2( 0, 0 ) );
	m->AddTexCoord( Point2( 1, 0 ) );
	m->AddTexCoord( Point2( 1, 1 ) );
	m->AddTexCoord( Point2( 0, 1 ) );

	IndexedTriangle t;
	t.iVertices[0] = 0; t.iVertices[1] = 1; t.iVertices[2] = 2;
	t.iNormals[0]  = 0; t.iNormals[1]  = 1; t.iNormals[2]  = 2;
	t.iCoords[0]   = 0; t.iCoords[1]   = 1; t.iCoords[2]   = 2;
	m->AddIndexedTriangle( t );
	t.iVertices[0] = 0; t.iVertices[1] = 2; t.iVertices[2] = 3;
	t.iNormals[0]  = 0; t.iNormals[1]  = 2; t.iNormals[2]  = 3;
	t.iCoords[0]   = 0; t.iCoords[1]   = 2; t.iCoords[2]   = 3;
	m->AddIndexedTriangle( t );
	m->DoneIndexedTriangles();
	return m;
}

//! Two DISJOINT triangles in the z = 0 plane whose areas are exactly
//! 9 : 1 (4.5 and 0.5), separated along x so a root can be classified
//! by position alone.  Normals +Z.
ITriangleMeshGeometryIndexed* MakeNineToOne()
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) {
		return 0;
	}
	m->BeginIndexedTriangles();
	// big: (0,0) (3,0) (0,3) -> area 4.5
	m->AddVertex( Point3(  0, 0, 0 ) );
	m->AddVertex( Point3(  3, 0, 0 ) );
	m->AddVertex( Point3(  0, 3, 0 ) );
	// small: (10,0) (11,0) (10,1) -> area 0.5
	m->AddVertex( Point3( 10, 0, 0 ) );
	m->AddVertex( Point3( 11, 0, 0 ) );
	m->AddVertex( Point3( 10, 1, 0 ) );
	for( int i = 0; i < 6; ++i ) {
		m->AddNormal( Vector3( 0, 0, 1 ) );
		m->AddTexCoord( Point2( 0.75, 0.75 ) );		// unmasked everywhere
	}
	IndexedTriangle t;
	for( int k = 0; k < 3; ++k ) { t.iVertices[k] = k; t.iNormals[k] = k; t.iCoords[k] = k; }
	m->AddIndexedTriangle( t );
	for( int k = 0; k < 3; ++k ) { t.iVertices[k] = 3+k; t.iNormals[k] = 3+k; t.iCoords[k] = 3+k; }
	m->AddIndexedTriangle( t );
	m->DoneIndexedTriangles();
	return m;
}

//! THREE well-separated square islands in the z = 0 plane (side
//! `side`, origins `gap` apart along +x), normals +Z, per-island UV.
//! The clump test grows on this so that a clump cell of side gap/2
//! contains exactly one island: a strand's own clump centre is then a
//! near neighbour and every OTHER group's centre is `gap` away, which
//! is what makes "did it converge on the right centre" a question with
//! two visibly different answers.
ITriangleMeshGeometryIndexed* MakeThreeIslands( const double side, const double gap )
{
	ITriangleMeshGeometryIndexed* m = 0;
	if( !RISE_API_CreateTriangleMeshGeometryIndexed( &m, true, false ) || !m ) {
		return 0;
	}
	m->BeginIndexedTriangles();
	for( int i = 0; i < 3; ++i ) {
		const double x0 = (double)i * gap;
		m->AddVertex( Point3( x0,        0,    0 ) );
		m->AddVertex( Point3( x0 + side, 0,    0 ) );
		m->AddVertex( Point3( x0 + side, side, 0 ) );
		m->AddVertex( Point3( x0,        side, 0 ) );
		for( int k = 0; k < 4; ++k ) { m->AddNormal( Vector3( 0, 0, 1 ) ); }
		m->AddTexCoord( Point2( 0, 0 ) );
		m->AddTexCoord( Point2( 1, 0 ) );
		m->AddTexCoord( Point2( 1, 1 ) );
		m->AddTexCoord( Point2( 0, 1 ) );

		const int v = i * 4;
		IndexedTriangle t;
		t.iVertices[0] = v+0; t.iVertices[1] = v+1; t.iVertices[2] = v+2;
		t.iNormals[0]  = v+0; t.iNormals[1]  = v+1; t.iNormals[2]  = v+2;
		t.iCoords[0]   = v+0; t.iCoords[1]   = v+1; t.iCoords[2]   = v+2;
		m->AddIndexedTriangle( t );
		t.iVertices[0] = v+0; t.iVertices[1] = v+2; t.iVertices[2] = v+3;
		t.iNormals[0]  = v+0; t.iNormals[1]  = v+2; t.iNormals[2]  = v+3;
		t.iCoords[0]   = v+0; t.iCoords[1]   = v+2; t.iCoords[2]   = v+3;
		m->AddIndexedTriangle( t );
	}
	m->DoneIndexedTriangles();
	return m;
}

//////////////////////////////////////////////////////////////////////
// Groom helpers
//////////////////////////////////////////////////////////////////////

//! The PRODUCTION cell key, mirrored: a 3-integer tuple with
//! lexicographic ordering, exactly what HairGenerator.cpp's `CellKey`
//! is.  Deliberately NOT a hash-combine of the three coordinates into
//! one integer -- that is what this test used to do, and it can collide
//! two genuinely different cells into one, which would silently weaken
//! every assertion built on the partition.
struct TestCellKey
{
	long long x, y, z;
	bool operator<( const TestCellKey& o ) const
	{
		if( x != o.x ) return x < o.x;
		if( y != o.y ) return y < o.y;
		return z < o.z;
	}
};

TestCellKey MakeTestCellKey( const Point3& p, const double cell )
{
	TestCellKey k;
	k.x = (long long)floor( (double)p.x / cell );
	k.y = (long long)floor( (double)p.y / cell );
	k.z = (long long)floor( (double)p.z / cell );
	return k;
}

//! The plain, unstyled recipe every group starts from: straight quills
//! along the surface normal, no painters, nothing enabled.
HairGroomRecipe PlainRecipe( IGeometry* base, unsigned int count, unsigned int segments = 6 )
{
	HairGroomRecipe r;
	r.pBase        = base;
	r.p.count      = count;
	r.p.segments   = segments;
	r.p.seed       = 7;
	r.p.baseDetail = 4;
	r.p.length     = 0.5;
	r.p.widthRoot  = 0.02;
	r.p.widthTip   = 0.006;
	return r;
}

//! Builds AND realizes a groom.  Returns null if construction was
//! refused (which several negative fixtures below expect).
HairGeometry* BuildAndRealize( const HairGroomRecipe& r )
{
	IGeometry* g = 0;
	if( !RISE_API_CreateHairGeometryGroom( &g, r, "test_groom" ) || !g ) {
		return 0;
	}
	g->Realize();
	// dynamic_cast, not static_cast: Geometry is a VIRTUAL base of
	// HairGeometry, and a downcast through a virtual base is only
	// expressible dynamically.
	HairGeometry* h = dynamic_cast<HairGeometry*>( g );
	if( !h ) { g->release(); }
	return h;
}

//! True iff two grooms are IDENTICAL in every observable: strand count,
//! per-strand control-point count, every control point (compared with
//! `==`, i.e. bitwise on the widened float storage), both widths and
//! the root UV.
bool GroomsIdentical( const HairGeometry& a, const HairGeometry& b )
{
	if( a.numStrands() != b.numStrands() ) return false;
	for( unsigned int s = 0; s < a.numStrands(); ++s ) {
		if( a.numControlPointsOfStrand( s ) != b.numControlPointsOfStrand( s ) ) return false;
		for( unsigned int k = 0; k < a.numControlPointsOfStrand( s ); ++k ) {
			const Point3 pa = a.ControlPoint( s, k );
			const Point3 pb = b.ControlPoint( s, k );
			if( !( pa.x == pb.x && pa.y == pb.y && pa.z == pb.z ) ) return false;
		}
		if( a.StrandWidthAt( s, 0 ) != b.StrandWidthAt( s, 0 ) ) return false;
		if( a.StrandWidthAt( s, 1 ) != b.StrandWidthAt( s, 1 ) ) return false;
		const Point2 ua = a.StrandRootUV( s );
		const Point2 ub = b.StrandRootUV( s );
		if( !( ua.x == ub.x && ua.y == ub.y ) ) return false;
	}
	return true;
}

Point3 StrandTip( const HairGeometry& g, unsigned int s )
{
	return g.ControlPoint( s, g.numControlPointsOfStrand( s ) - 1 );
}

//////////////////////////////////////////////////////////////////////
// Scene plumbing (HairMaterialChunkTest's pattern, unchanged)
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_hairgen_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                          std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_hairgen_stdout_" + tag + "_" + pidbuf + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = ParseBodyInto( tag, body, job );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
	remove( capPath.c_str() );

	return ok;
}

} // namespace

// ============================================================
//  1. Determinism
// ============================================================

static void RunDeterminism()
{
	std::cout << "=== 1. Determinism ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	HairGroomRecipe r = PlainRecipe( base.get(), 400 );

	Owned<HairGeometry> a( BuildAndRealize( r ) );
	Owned<HairGeometry> b( BuildAndRealize( r ) );
	if( !a || !b ) { Check( false, "two grooms built from the same recipe" ); return; }

	std::cout << "  strands (seed 7)           : " << a->numStrands() << std::endl;

	Check( a->numStrands() > 0, "the plain recipe actually grows strands" );
	Check( GroomsIdentical( *a, *b ),
	       "MONEY: the same recipe + seed produces BIT-IDENTICAL strands across two independent builds" );

	// A second Realize() on an already-realized groom is a no-op, not a
	// regeneration -- the observable groom is unchanged.
	const unsigned int before = a->numStrands();
	const Point3 tipBefore = StrandTip( *a, 0 );
	a->Realize();
	Check( a->numStrands() == before, "a second Realize() leaves the strand count unchanged" );
	const Point3 tipAfter = StrandTip( *a, 0 );
	Check( tipBefore.x == tipAfter.x && tipBefore.y == tipAfter.y && tipBefore.z == tipAfter.z,
	       "a second Realize() leaves the strands themselves unchanged (idempotent)" );

	r.p.seed = 8;
	Owned<HairGeometry> c( BuildAndRealize( r ) );
	if( !c ) { Check( false, "groom built at a different seed" ); return; }
	Check( !GroomsIdentical( *a, *c ), "changing ONLY the seed changes the groom" );
}

// ============================================================
//  2. Root placement + area weighting
// ============================================================

static void RunRootPlacement()
{
	std::cout << "=== 2. Root placement and area weighting ===" << std::endl;

	// -- roots lie exactly ON the tessellated base (a flat z = 0 quad),
	//    and inside its footprint.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
		if( !base ) { Check( false, "unit-quad base built" ); return; }

		HairGroomRecipe r = PlainRecipe( base.get(), 500 );
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g ) { Check( false, "quad groom built" ); return; }

		double worstOffPlane = 0;
		int outsideFootprint = 0;
		int badRootUV        = 0;
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			const Point3 root = g->ControlPoint( s, 0 );
			worstOffPlane = std::max( worstOffPlane, fabs( (double)root.z ) );
			if( root.x < -1e-9 || root.x > 1.0 + 1e-9 ||
			    root.y < -1e-9 || root.y > 1.0 + 1e-9 ) ++outsideFootprint;
			// UV == position on this fixture, so the stored root UV must
			// agree with the root's own (x, y) -- this is what a
			// hair_material reads as ri.ptCoord1.
			const Point2 uv = g->StrandRootUV( s );
			if( fabs( (double)uv.x - (double)root.x ) > 1e-5 ||
			    fabs( (double)uv.y - (double)root.y ) > 1e-5 ) ++badRootUV;
		}
		std::cout << "  strands                    : " << g->numStrands() << std::endl;
		std::cout << "  worst |z| at a root        : " << worstOffPlane << std::endl;

		Check( g->numStrands() == 500,   "with no density mask every candidate becomes a strand" );
		Check( worstOffPlane < 1e-9,     "every root lies ON the tessellated base surface" );
		Check( outsideFootprint == 0,    "every root lies inside the base's footprint" );
		Check( badRootUV == 0,           "the stored root UV is the base surface's UV at the root" );
	}

	// -- AREA weighting, on a 9:1 pair of triangles.  A per-triangle-
	//    uniform sampler would put ~50/50 here.
	{
		Owned<ITriangleMeshGeometryIndexed> base( MakeNineToOne() );
		if( !base ) { Check( false, "9:1 base built" ); return; }

		const unsigned int N = 4000;
		HairGroomRecipe r = PlainRecipe( base.get(), N );
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g ) { Check( false, "9:1 groom built" ); return; }

		unsigned int big = 0, small = 0;
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			( g->ControlPoint( s, 0 ).x < 5.0 ? big : small )++;
		}
		// Binomial(N, 0.9): mean 3600, sd = sqrt(4000*0.9*0.1) = 19.0.
		// A 5-sigma window is ~95 either side; a 50/50 sampler would
		// land 1600 away, i.e. 84 sigma out.
		const double expected = 0.9 * (double)N;
		const double sd       = sqrt( (double)N * 0.9 * 0.1 );
		std::cout << "  big/small triangle roots   : " << big << " / " << small
		          << "  (expected " << expected << " +/- " << 5.0*sd << ")" << std::endl;

		Check( big + small == N, "every candidate landed on one of the two triangles" );
		Check( fabs( (double)big - expected ) < 5.0 * sd,
		       "MONEY: roots are AREA-weighted (9:1 triangles give a 9:1 split, within 5 sigma)" );
	}
}

// ============================================================
//  3. Density mask
// ============================================================

static void RunDensityMask()
{
	std::cout << "=== 3. Density mask and stream keying ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	Owned<UVStepScalarPainter> density( new UVStepScalarPainter() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const unsigned int N = 2000;
	HairGroomRecipe r = PlainRecipe( base.get(), N );
	r.pDensity = density.get();

	Owned<HairGeometry> g( BuildAndRealize( r ) );
	if( !g ) { Check( false, "masked groom built" ); return; }

	int inMaskedHalf = 0;
	for( unsigned int s = 0; s < g->numStrands(); ++s ) {
		if( g->ControlPoint( s, 0 ).x < 0.5 ) ++inMaskedHalf;
	}
	const double sd = sqrt( (double)N * 0.5 * 0.5 );
	std::cout << "  strands surviving          : " << g->numStrands()
	          << "  (expected ~" << N/2 << " +/- " << 5.0*sd << ")" << std::endl;
	std::cout << "  strands in the masked half : " << inMaskedHalf << std::endl;

	Check( inMaskedHalf == 0,
	       "MONEY: a step-function density painter leaves EXACTLY zero roots on its masked half" );
	Check( g->numStrands() > 0, "the unmasked half still grows hair" );
	Check( fabs( (double)g->numStrands() - 0.5*(double)N ) < 5.0*sd,
	       "`count` is a PRE-mask budget: about half the candidates survive a half-masked painter" );

	// -- STREAM KEYING.  Editing `density` must not re-roll the strands
	//    it does not remove.  This is the property that makes grooming
	//    iterative: an author paints a bald patch over one ear, and the
	//    hair on the other ear must not move.  It only holds because
	//    each candidate's placement draws come from a stream keyed on
	//    its OWN ordinal; a single stream walked across the loop would
	//    make every strand's position depend on how many draws the
	//    candidates before it happened to consume, and binding a density
	//    painter at all changes that count (three draws per candidate
	//    become four).
	HairGroomRecipe rNone = PlainRecipe( base.get(), N );		// no density painter
	Owned<HairGeometry> none( BuildAndRealize( rNone ) );

	Owned<ConstScalarPainter> ones( new ConstScalarPainter( 1.0 ) );
	HairGroomRecipe rOnes = PlainRecipe( base.get(), N );
	rOnes.pDensity = ones.get();								// bound, but rejects nothing
	Owned<HairGeometry> neutral( BuildAndRealize( rOnes ) );

	if( !none || !neutral ) { Check( false, "unmasked and neutral-density grooms built" ); return; }

	Check( none->numStrands() == N, "the unmasked groom keeps every candidate" );
	Check( GroomsIdentical( *none, *neutral ),
	       "MONEY: BINDING a density painter that rejects nothing changes nothing -- "
	       "not one strand moves, though it consumes an extra draw per candidate" );

	// Every survivor of the MASKED groom must be bit-identical to its
	// counterpart in the unmasked one.  Matched by root position, which
	// is unique per candidate and compares exactly (identical bits, not
	// nearby values, is the whole claim).
	std::map<std::pair<double,double>, unsigned int> byRoot;
	for( unsigned int s = 0; s < none->numStrands(); ++s ) {
		const Point3 root = none->ControlPoint( s, 0 );
		byRoot[ std::make_pair( (double)root.x, (double)root.y ) ] = s;
	}
	unsigned int matched = 0, unmatched = 0, drifted = 0;
	for( unsigned int s = 0; s < g->numStrands(); ++s ) {
		const Point3 root = g->ControlPoint( s, 0 );
		std::map<std::pair<double,double>, unsigned int>::const_iterator it =
			byRoot.find( std::make_pair( (double)root.x, (double)root.y ) );
		if( it == byRoot.end() ) { ++unmatched; continue; }
		++matched;
		const unsigned int t = it->second;
		if( g->numControlPointsOfStrand( s ) != none->numControlPointsOfStrand( t ) ) { ++drifted; continue; }
		for( unsigned int k = 0; k < g->numControlPointsOfStrand( s ); ++k ) {
			const Point3 a = g->ControlPoint( s, k );
			const Point3 b = none->ControlPoint( t, k );
			if( !( a.x == b.x && a.y == b.y && a.z == b.z ) ) { ++drifted; break; }
		}
	}
	std::cout << "  survivors matched / drifted: " << matched << " / " << drifted << std::endl;
	Check( unmatched == 0,
	       "every strand the mask kept exists at the identical root in the unmasked groom" );
	Check( matched > 0 && drifted == 0,
	       "MONEY: masking out half the candidates leaves the OTHER half BIT-IDENTICAL "
	       "(placement is keyed per candidate, so a density edit is local)" );
}

// ============================================================
//  4. Length painter and width lerp
// ============================================================

static void RunLengthAndWidth()
{
	std::cout << "=== 4. Length painter and width lerp ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	// The UNBENT configuration: no comb / gravity / curl / frizz, so a
	// strand is exactly a straight segment of the authored length and
	// its arc length is exact rather than shortened by a bend.
	HairGroomRecipe r = PlainRecipe( base.get(), 200 );

	Owned<HairGeometry> plain( BuildAndRealize( r ) );
	if( !plain ) { Check( false, "unbent groom built" ); return; }

	double worstLenErr = 0;
	double worstRootW  = 0;
	double worstTipW   = 0;
	for( unsigned int s = 0; s < plain->numStrands(); ++s ) {
		worstLenErr = std::max( worstLenErr, fabs( (double)plain->StrandArcLength( s ) - r.p.length ) );
		worstRootW  = std::max( worstRootW,  fabs( (double)plain->StrandWidthAt( s, 0 ) - r.p.widthRoot ) );
		worstTipW   = std::max( worstTipW,   fabs( (double)plain->StrandWidthAt( s, 1 ) - r.p.widthTip ) );
	}
	std::cout << "  worst arc-length error     : " << worstLenErr << std::endl;

	Check( worstLenErr < 1e-6, "an unbent strand's arc length is exactly the authored `length`" );
	Check( worstRootW < 1e-9,  "the reported width at s = 0 is `width_root`" );
	Check( worstTipW  < 1e-9,  "the reported width at s = 1 is `width_tip`" );

	// Half-length painter: every strand halves.
	Owned<ConstScalarPainter> half( new ConstScalarPainter( 0.5 ) );
	r.pLengthScale = half.get();
	Owned<HairGeometry> scaled( BuildAndRealize( r ) );
	if( !scaled ) { Check( false, "length-scaled groom built" ); return; }

	double worstScaledErr = 0;
	for( unsigned int s = 0; s < scaled->numStrands(); ++s ) {
		worstScaledErr = std::max( worstScaledErr,
			fabs( (double)scaled->StrandArcLength( s ) - 0.5 * r.p.length ) );
	}
	std::cout << "  worst scaled-length error  : " << worstScaledErr << std::endl;
	Check( worstScaledErr < 1e-6,
	       "MONEY: a 0.5 length painter halves every strand's measured arc length" );
	Check( scaled->numStrands() == plain->numStrands(),
	       "a length painter changes lengths, not which candidates survive" );

	// A ZERO length painter drops every strand rather than emitting
	// degenerate zero-length hair.
	Owned<ConstScalarPainter> zero( new ConstScalarPainter( 0.0 ) );
	r.pLengthScale = zero.get();
	Owned<HairGeometry> none( BuildAndRealize( r ) );
	if( !none ) { Check( false, "zero-length groom built" ); return; }
	Check( none->numStrands() == 0, "a zero length painter drops every strand (no zero-length hair)" );
}

// ============================================================
//  5. Clumping
// ============================================================

static void RunClumping()
{
	std::cout << "=== 5. Clumping ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const Scalar kCell = 0.25;

	HairGroomRecipe r = PlainRecipe( base.get(), 600, 8 );
	// Frizz, so the strands are NOT all parallel and clumping has
	// something to converge.  Perfectly parallel quills on a flat base
	// all point the same way, so their tips are already as close as the
	// roots are and a clump pull would be a near-no-op -- the test would
	// pass without proving anything.
	r.p.frizz = 0.4;

	Owned<HairGeometry> off( BuildAndRealize( r ) );

	r.p.clump     = 0.9;
	r.p.clumpSize = kCell;
	Owned<HairGeometry> on( BuildAndRealize( r ) );

	if( !off || !on ) { Check( false, "clumped and unclumped grooms built" ); return; }
	Check( off->numStrands() == on->numStrands(), "clumping does not change which candidates survive" );

	// Reproduce the DOCUMENTED cell rule so this measures the contract,
	// not merely "the tips moved": roots are quantised onto a grid of
	// side `clump_size`; each occupied cell's LOWEST-INDEX strand is
	// that cell's centre.  Roots are identical in both grooms (clumping
	// never moves a root), so one assignment serves both.
	std::map<TestCellKey, unsigned int> centreOfCell;
	std::vector<unsigned int> centre( off->numStrands(), 0 );
	std::vector<bool> hasCentre( off->numStrands(), false );
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		const TestCellKey key = MakeTestCellKey( off->ControlPoint( s, 0 ), (double)kCell );
		std::map<TestCellKey, unsigned int>::const_iterator it = centreOfCell.find( key );
		if( it == centreOfCell.end() ) { centreOfCell[key] = s; }
		else if( it->second != s )     { centre[s] = it->second; hasCentre[s] = true; }
	}

	double sumOff = 0, sumOn = 0;
	unsigned int n = 0;
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		if( !hasCentre[s] ) continue;
		sumOff += Point3Ops::Distance( StrandTip( *off, s ), StrandTip( *off, centre[s] ) );
		sumOn  += Point3Ops::Distance( StrandTip( *on,  s ), StrandTip( *on,  centre[s] ) );
		++n;
	}
	Check( n > 20, "the clump-cell partition actually put strands into shared cells" );
	if( n > 0 ) {
		std::cout << "  clumped strands            : " << n << " of " << off->numStrands() << std::endl;
		std::cout << "  mean tip-to-centre, off    : " << sumOff / (double)n << std::endl;
		std::cout << "  mean tip-to-centre, on     : " << sumOn  / (double)n << std::endl;
		Check( sumOn < sumOff,
		       "MONEY: clumping strictly reduces the mean tip-to-clump-centre distance at the same seed" );
	}

	// Roots never move, however hard the clump pulls.
	double worstRootMove = 0;
	for( unsigned int s = 0; s < off->numStrands(); ++s ) {
		worstRootMove = std::max( worstRootMove,
			(double)Point3Ops::Distance( off->ControlPoint( s, 0 ), on->ControlPoint( s, 0 ) ) );
	}
	Check( worstRootMove == 0.0, "clumping never moves a root (the t^2 weight is exactly 0 there)" );

	// clump_size 0 disables clumping entirely, whatever `clump` says.
	r.p.clumpSize = 0.0;
	Owned<HairGeometry> disabled( BuildAndRealize( r ) );
	if( disabled ) {
		Check( GroomsIdentical( *off, *disabled ),
		       "clump_size = 0 disables clumping entirely, even at clump = 0.9" );
	} else {
		Check( false, "clump_size = 0 groom built" );
	}

	// ---------------------------------------------------------------
	//  THE PARTITION ITSELF, on three well-separated islands.
	//
	//  "Tips got closer to their centre" is satisfied by any scheme
	//  that pulls tips together, INCLUDING the index-strided one the
	//  design explicitly rejects (every Nth generated root is a centre
	//  -- which, because roots come out of an area-weighted sampler,
	//  would pull strands across the entire surface).  Three islands
	//  `kGap` apart make the two schemes give visibly different
	//  answers, and the assertion is the EXACT documented target rather
	//  than an inequality: a clumped tip is a lerp of `clump` from its
	//  own tip toward its OWN cell centre's tip, so its distance to
	//  that centre must come out at exactly (1 - clump) of what it was.
	// ---------------------------------------------------------------
	{
		const double kSide  = 0.2;
		const double kGap   = 10.0;
		const double kCell3 = 5.0;			// >> island, << gap: one cell per island
		const double kClump = 0.6;

		Owned<ITriangleMeshGeometryIndexed> islands( MakeThreeIslands( kSide, kGap ) );
		if( !islands ) { Check( false, "three-island base built" ); return; }

		HairGroomRecipe ri = PlainRecipe( islands.get(), 600, 8 );
		ri.p.frizz = 0.4;					// so tips are not already coincident
		Owned<HairGeometry> before( BuildAndRealize( ri ) );
		ri.p.clump     = kClump;
		ri.p.clumpSize = kCell3;
		Owned<HairGeometry> after( BuildAndRealize( ri ) );
		if( !before || !after ) { Check( false, "three-island grooms built" ); return; }

		std::map<TestCellKey, unsigned int> cells;
		std::vector<unsigned int> own( before->numStrands(), 0 );
		std::vector<bool> owned( before->numStrands(), false );
		for( unsigned int s = 0; s < before->numStrands(); ++s ) {
			const TestCellKey key = MakeTestCellKey( before->ControlPoint( s, 0 ), kCell3 );
			std::map<TestCellKey, unsigned int>::const_iterator it = cells.find( key );
			if( it == cells.end() ) { cells[key] = s; }
			else if( it->second != s ) { own[s] = it->second; owned[s] = true; }
		}
		std::cout << "  island cells occupied      : " << cells.size() << std::endl;
		Check( cells.size() == 3,
		       "three separated islands, one clump cell each (the documented spatial partition)" );

		// The centres themselves are never touched.
		unsigned int centresMoved = 0;
		for( std::map<TestCellKey, unsigned int>::const_iterator it = cells.begin(); it != cells.end(); ++it ) {
			if( Point3Ops::Distance( StrandTip( *before, it->second ), StrandTip( *after, it->second ) ) != 0.0 ) {
				++centresMoved;
			}
		}
		Check( centresMoved == 0, "a clump centre is never itself pulled" );

		// EXACT per-cell lerp target, and no cross-island attraction.
		double worstTargetErr = 0;
		double worstCrossRatio = 0;			// smallest on/off ratio to a FOREIGN centre
		bool   haveCross = false;
		unsigned int nOwned = 0;
		for( unsigned int s = 0; s < before->numStrands(); ++s ) {
			if( !owned[s] ) continue;
			++nOwned;
			const Point3 cTip = StrandTip( *before, own[s] );
			const double dOff = Point3Ops::Distance( StrandTip( *before, s ), cTip );
			const double dOn  = Point3Ops::Distance( StrandTip( *after,  s ), cTip );
			worstTargetErr = std::max( worstTargetErr, fabs( dOn - ( 1.0 - kClump ) * dOff ) );

			// ... and against every OTHER island's centre, the distance
			//     must be essentially unchanged: the pull is local.
			for( std::map<TestCellKey, unsigned int>::const_iterator it = cells.begin(); it != cells.end(); ++it ) {
				if( it->second == own[s] ) continue;
				const Point3 fTip = StrandTip( *before, it->second );
				const double fOff = Point3Ops::Distance( StrandTip( *before, s ), fTip );
				const double fOn  = Point3Ops::Distance( StrandTip( *after,  s ), fTip );
				if( fOff > 0 ) {
					const double ratio = fOn / fOff;
					if( !haveCross || ratio < worstCrossRatio ) { worstCrossRatio = ratio; haveCross = true; }
				}
			}
		}
		std::cout << "  clumped strands (islands)  : " << nOwned << std::endl;
		std::cout << "  worst |d_on - (1-c)*d_off| : " << worstTargetErr << std::endl;
		std::cout << "  worst cross-island ratio   : " << worstCrossRatio << std::endl;

		Check( nOwned > 100, "the island fixture actually put many strands into shared cells" );
		// The tolerance is float-storage quantisation, not slack: the
		// third island sits at x ~ 20, where a float coordinate carries
		// ~2e-6 of quantisation, and three independently-quantised
		// points enter each distance.  The signal it has to separate is
		// enormous by comparison -- converging on the WRONG island's
		// centre would miss by ~10 scene units.
		Check( worstTargetErr < 5e-5,
		       "MONEY: a clumped tip lands EXACTLY `clump` of the way to its OWN cell centre's tip" );
		Check( haveCross && worstCrossRatio > 0.95,
		       "MONEY: no strand converges on a FOREIGN island's centre -- the partition is spatial, "
		       "not index-strided" );
	}
}

// ============================================================
//  6. Frizz and curl
// ============================================================

static void RunFrizzAndCurl()
{
	std::cout << "=== 6. Frizz and curl ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	const HairGroomRecipe plain = PlainRecipe( base.get(), 300, 10 );
	Owned<HairGeometry> baseline( BuildAndRealize( plain ) );
	if( !baseline ) { Check( false, "baseline groom built" ); return; }

	// The straight-growth baseline really is straight: every control
	// point sits on the surface normal through its own root.
	{
		double worstLateral = 0;
		for( unsigned int s = 0; s < baseline->numStrands(); ++s ) {
			const Point3 root = baseline->ControlPoint( s, 0 );
			for( unsigned int k = 0; k < baseline->numControlPointsOfStrand( s ); ++k ) {
				const Point3 p = baseline->ControlPoint( s, k );
				worstLateral = std::max( worstLateral,
					sqrt( ( p.x - root.x )*( p.x - root.x ) + ( p.y - root.y )*( p.y - root.y ) ) );
			}
		}
		Check( worstLateral < 1e-9, "with every styling knob off, strands grow straight along the normal" );
	}

	// CROSS-EFFECT STREAM ISOLATION.  Adding gravity to an ALREADY
	// FRIZZED groom must displace every control point by exactly the
	// documented gravity term -- world -Y, gravity * length * t^2 --
	// and by nothing else.  Two things fail at once if it does not: the
	// gravity formula, and the independence of the effects (a gravity
	// path that drew from, or perturbed, the frizz stream would shift
	// the jitter as well, showing up as a non-zero x / z delta).
	//
	// This replaces an earlier check that set frizz / curl / gravity /
	// clump explicitly to 0.0 and asserted the groom matched the
	// baseline.  That check could not fail: the baseline recipe already
	// held those defaults, so it compared two BYTE-IDENTICAL recipes and
	// re-tested nothing but group 1's determinism.
	{
		HairGroomRecipe rf = plain;
		rf.p.frizz = 0.5;
		HairGroomRecipe rg = rf;
		rg.p.gravity = 0.3;

		Owned<HairGeometry> f( BuildAndRealize( rf ) );
		Owned<HairGeometry> fg( BuildAndRealize( rg ) );
		if( !f || !fg ) { Check( false, "frizz and frizz+gravity grooms built" ); return; }

		Check( f->numStrands() == fg->numStrands(), "adding gravity does not change the strand count" );

		double worstLateral = 0;			// x / z must not move at all
		double worstDropErr = 0;			// y must move by exactly the gravity term
		for( unsigned int s = 0; s < f->numStrands() && s < fg->numStrands(); ++s ) {
			const unsigned int n = f->numControlPointsOfStrand( s );
			for( unsigned int k = 0; k < n; ++k ) {
				const Point3 a = f->ControlPoint( s, k );
				const Point3 b = fg->ControlPoint( s, k );
				const double t = (double)k / (double)( n - 1 );
				const double expectedDrop = rg.p.gravity * rg.p.length * t * t;
				worstLateral = std::max( worstLateral,
					std::max( fabs( (double)b.x - (double)a.x ), fabs( (double)b.z - (double)a.z ) ) );
				worstDropErr = std::max( worstDropErr,
					fabs( ( (double)a.y - (double)b.y ) - expectedDrop ) );
			}
		}
		std::cout << "  gravity: worst lateral drift: " << worstLateral << std::endl;
		std::cout << "  gravity: worst drop error   : " << worstDropErr << std::endl;
		Check( worstLateral < 1e-7,
		       "MONEY: gravity does not disturb the frizz stream (x and z are untouched)" );
		Check( worstDropErr < 1e-6,
		       "MONEY: gravity drops every control point by exactly gravity * length * t^2 in world -Y" );
	}

	// PER-STRAND JITTER INDEPENDENCE.  Each strand's frizz comes from
	// its own stream, keyed on its own ordinal.  A single stream shared
	// across the groom, or a stream keyed on something constant, would
	// give every strand the SAME jitter -- which reads as a coherent
	// wave through the whole coat rather than as roughness, and which
	// no "frizz moves the strands" assertion can see.
	{
		HairGroomRecipe r = plain;
		r.p.frizz = 0.5;
		Owned<HairGeometry> f( BuildAndRealize( r ) );
		if( !f ) { Check( false, "jitter-independence groom built" ); return; }

		// At a fixed segment index the ONLY thing separating two strands
		// is their jitter draw: growth is along +Z from the root by the
		// same arc on this flat base.
		std::set< std::pair<double,double> > distinct;
		const unsigned int kProbe = 1;
		for( unsigned int s = 0; s < f->numStrands(); ++s ) {
			if( f->numControlPointsOfStrand( s ) <= kProbe ) continue;
			const Point3 root = f->ControlPoint( s, 0 );
			const Point3 p    = f->ControlPoint( s, kProbe );
			distinct.insert( std::make_pair( (double)p.x - (double)root.x,
			                                 (double)p.y - (double)root.y ) );
		}
		std::cout << "  distinct jitters at cp 1   : " << distinct.size()
		          << " of " << f->numStrands() << " strands" << std::endl;
		Check( distinct.size() > f->numStrands() / 2,
		       "MONEY: frizz is drawn PER STRAND -- strands at the same segment index jitter differently" );
	}

	// Frizz moves control points, but never the root.
	{
		HairGroomRecipe r = plain;
		r.p.frizz = 0.5;
		Owned<HairGeometry> f( BuildAndRealize( r ) );
		if( !f ) { Check( false, "frizzed groom built" ); return; }
		Check( !GroomsIdentical( *baseline, *f ), "non-zero frizz moves the strands off the baseline" );
		Check( f->numStrands() == baseline->numStrands(), "frizz does not change the strand count" );
		double worstRootMove = 0;
		for( unsigned int s = 0; s < f->numStrands(); ++s ) {
			worstRootMove = std::max( worstRootMove,
				(double)Point3Ops::Distance( f->ControlPoint( s, 0 ), baseline->ControlPoint( s, 0 ) ) );
		}
		Check( worstRootMove == 0.0, "frizz never moves a root" );
	}

	// Curl moves control points, but never the root, and its lateral
	// excursion is bounded by curl_radius.
	{
		HairGroomRecipe r = plain;
		r.p.curlRadius = 0.05;
		r.p.curlStep   = 0.2;
		Owned<HairGeometry> c( BuildAndRealize( r ) );
		if( !c ) { Check( false, "curled groom built" ); return; }
		Check( !GroomsIdentical( *baseline, *c ), "non-zero curl moves the strands off the baseline" );

		double worstRootMove = 0, worstLateral = 0;
		for( unsigned int s = 0; s < c->numStrands(); ++s ) {
			const Point3 root = c->ControlPoint( s, 0 );
			worstRootMove = std::max( worstRootMove,
				(double)Point3Ops::Distance( root, baseline->ControlPoint( s, 0 ) ) );
			for( unsigned int k = 0; k < c->numControlPointsOfStrand( s ); ++k ) {
				const Point3 p = c->ControlPoint( s, k );
				worstLateral = std::max( worstLateral,
					sqrt( ( p.x - root.x )*( p.x - root.x ) + ( p.y - root.y )*( p.y - root.y ) ) );
			}
		}
		std::cout << "  worst curl lateral offset  : " << worstLateral
		          << "  (curl_radius " << r.p.curlRadius << ")" << std::endl;
		Check( worstRootMove == 0.0, "curl never moves a root (the helix opens out of the follicle)" );
		// The bound is checked to FLOAT precision, not double: control
		// points are stored as float (HairGeometry's documented
		// storage exception), so a coordinate near 1 carries ~1e-7 of
		// quantisation, and both the root and the control point are
		// quantised independently.
		Check( worstLateral > 1e-6 && worstLateral <= r.p.curlRadius + 1e-5,
		       "the curl's lateral excursion is non-zero and bounded by curl_radius" );
	}

	// Gravity pulls tips down in world -Y and nothing else.
	{
		HairGroomRecipe r = plain;
		r.p.gravity = 0.5;
		Owned<HairGeometry> gv( BuildAndRealize( r ) );
		if( !gv ) { Check( false, "gravity groom built" ); return; }
		int wrongWay = 0;
		for( unsigned int s = 0; s < gv->numStrands(); ++s ) {
			const Point3 a = StrandTip( *gv, s );
			const Point3 b = StrandTip( *baseline, s );
			if( !( a.y < b.y - 1e-9 ) ) ++wrongWay;
		}
		Check( wrongWay == 0, "gravity moves every tip DOWN in world -Y" );
	}
}

// ============================================================
//  7. The Realize contract
// ============================================================

static void RunRealizeContract()
{
	std::cout << "=== 7. The Realize contract ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	HairGroomRecipe r = PlainRecipe( base.get(), 300 );
	r.p.widthRoot = 0.05;
	r.p.widthTip  = 0.03;

	IGeometry* raw = 0;
	if( !RISE_API_CreateHairGeometryGroom( &raw, r, "realize_groom" ) || !raw ) {
		Check( false, "deferred groom constructed" );
		return;
	}
	Owned<HairGeometry> g( dynamic_cast<HairGeometry*>( raw ) );

	Check( g->IsValid(),    "a deferred groom with a tessellatable base is valid at construction" );
	Check( !g->IsRealized(), "a deferred groom is NOT realized at construction" );

	// -- DOCUMENTED pre-Realize behaviour: defined and EMPTY.  No
	//    strands, a degenerate box, and every ray misses.  Not a crash,
	//    and not a silent partial groom.
	Check( g->numStrands()  == 0, "a groom before Realize() has no strands" );
	Check( g->numSegments() == 0, "a groom before Realize() has no segments" );

	RandomNumberGenerator rng( 4242u );
	int hitsBefore = 0;
	std::vector<Ray> probes;
	for( int i = 0; i < 400; ++i ) {
		const Point3 o( rng.CanonicalRandom(), rng.CanonicalRandom(), -1.0 );
		const Vector3 d = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
		probes.push_back( Ray( o, d ) );
	}
	for( size_t i = 0; i < probes.size(); ++i ) {
		RayIntersectionGeometric ri( probes[i], nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( ri.bHit ) ++hitsBefore;
		if( g->IntersectRay_IntersectionOnly( probes[i], RISE_INFINITY, true, true ) ) ++hitsBefore;
	}
	Check( hitsBefore == 0, "MONEY: a groom before Realize() intersects NOTHING (defined and empty, no crash)" );

	g->Realize();
	Check( g->IsRealized(), "Realize() marks the groom realized" );
	Check( g->numStrands() > 0, "Realize() actually generated strands" );

	int hitsAfter = 0;
	for( size_t i = 0; i < probes.size(); ++i ) {
		RayIntersectionGeometric ri( probes[i], nullRasterizerState );
		g->IntersectRay( ri, true, true, false );
		if( ri.bHit ) ++hitsAfter;
	}
	std::cout << "  hits before / after        : " << 0 << " / " << hitsAfter << std::endl;
	Check( hitsAfter > 0, "after Realize() the groom is hit by rays that previously missed" );

	// A groom is never an area light and never tessellates, deferred or
	// not -- the composites that would misuse it must refuse at parse.
	Check( !g->CanBeAreaLight(), "a groom refuses to serve as an area light" );
	Check( !g->CanTessellate(),  "a groom refuses to tessellate (so it cannot be a displaced_geometry base)" );
}

// ============================================================
//  8. The chunk
// ============================================================

static void RunChunk()
{
	std::cout << "=== 8. The hair_geometry chunk ===" << std::endl;

	const std::string kBase =
		"sphere_geometry\n{\n\tname\tcc_base\n\tradius\t1.0\n}\n";

	// -- minimal chunk parses and registers a geometry.
	{
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (minimal)" ); return; }
		const std::string body = kBase +
			"hair_geometry\n{\n\tname\tcc_groom\n\tbase_geometry\tcc_base\n\tcount\t200\n\tlength\t0.1\n}\n";
		const bool ok = ParseBodyInto( "minimal", body, *job );
		Check( ok, "a minimal hair_geometry chunk parses" );
		IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "cc_groom" ) : 0;
		Check( geom != 0, "the minimal chunk registers a geometry under its own name" );
		if( geom ) {
			HairGeometry* hg = dynamic_cast<HairGeometry*>( geom );
			Check( hg != 0, "the registered geometry really is a HairGeometry" );
			if( hg ) {
				Check( !hg->IsRealized(), "the chunk does NOT generate at parse time (generation is deferred)" );
				hg->Realize();
				Check( hg->numStrands() > 0, "realizing the parsed groom grows strands on the sphere" );
			}
		}
		safe_release( job );
	}

	// -- a fully-styled chunk parses, with every optional parameter bound
	//    at once.  This proves each field is DECLARED (an undeclared one
	//    is a hard parse error) and that a full styling set resolves its
	//    painter references and grows a groom.  It does NOT prove
	//    Finalize reads any of them under the declared name -- a dropped
	//    or transposed `bag.Get*` parses exactly as cleanly.  The
	//    behavioural probes below cover `segments`, `seed`, `gravity` and
	//    `frizz`; the rest are pinned through the direct recipe API in
	//    groups 3-6 and 9 instead.
	{
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (styled)" ); return; }
		const std::string body = kBase +
			"scalar_painter\n{\n\tname\tcc_dens\n\tvalue\t0.8\n}\n"
			"scalar_painter\n{\n\tname\tcc_len\n\tvalue\t0.7\n}\n"
			"uniformcolor_painter\n{\n\tname\tcc_comb\n\tcolor\t0.9 0.5 0.5\n}\n"
			"hair_geometry\n{\n"
			"\tname\tcc_groom\n\tbase_geometry\tcc_base\n\tcount\t300\n\tlength\t0.12\n"
			"\tsegments\t10\n\twidth_root\t0.004\n\twidth_tip\t0.001\n\tseed\t3\n\tbase_detail\t16\n"
			"\tdensity\tcc_dens\n\tlength_painter\tcc_len\n\tcomb\tcc_comb\n"
			"\tgravity\t0.2\n\tfrizz\t0.1\n\tclump\t0.5\n\tclump_size\t0.05\n"
			"\tcurl_radius\t0.01\n\tcurl_step\t0.05\n}\n";
		std::string captured;
		const bool ok = ParseBodyCapturing( "styled", body, *job, captured );
		Check( ok, "a fully-styled hair_geometry chunk parses (every optional parameter bound)" );
		IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "cc_groom" ) : 0;
		Check( geom != 0, "the styled chunk registers its geometry" );
		if( geom ) {
			geom->Realize();
			HairGeometry* hg = dynamic_cast<HairGeometry*>( geom );
			Check( hg && hg->numStrands() > 0, "the styled groom grows strands" );
		}
		safe_release( job );
	}

	// -- FOUR fields REACH the generator through the chunk.  Parsing
	//    successfully only proves the DESCRIPTOR declares a parameter; it
	//    does not prove Finalize reads it under the same name, nor that
	//    it hands it to the right `HairGroomParams` slot -- a typo would
	//    silently keep the default forever, and a transposition (frizz
	//    into gravity) would parse just as happily.  Each probe below
	//    grows two grooms from scenes differing in exactly ONE LINE and
	//    measures the difference in the resulting strands, which is the
	//    only construction that catches either failure.
	{
		auto GroomWith = []( const char* tag, const std::string& base, const std::string& extra,
		                     unsigned segments, unsigned seed,
		                     std::vector<Point3>& outPoints, unsigned& outCPs ) -> bool {
			IJobPriv* job = 0;
			if( !RISE_CreateJobPriv( &job ) || !job ) { return false; }
			std::ostringstream oss;
			oss << base
			    << "hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n"
			    << "\tcount\t120\n\tlength\t0.1\n\tsegments\t" << segments
			    << "\n\tseed\t" << seed << "\n" << extra << "}\n";
			bool good = false;
			if( ParseBodyInto( tag, oss.str(), *job ) ) {
				IGeometry* geom = job->GetGeometries() ? job->GetGeometries()->GetItem( "g" ) : 0;
				HairGeometry* hg = geom ? dynamic_cast<HairGeometry*>( geom ) : 0;
				if( hg ) {
					hg->Realize();
					outCPs = hg->numStrands() ? hg->numControlPointsOfStrand( 0 ) : 0;
					for( unsigned s = 0; s < hg->numStrands(); ++s ) {
						for( unsigned k = 0; k < hg->numControlPointsOfStrand( s ); ++k ) {
							outPoints.push_back( hg->ControlPoint( s, k ) );
						}
					}
					good = !outPoints.empty();
				}
			}
			safe_release( job );
			return good;
		};

		// `segments` and `seed`.
		std::vector<Point3> ptsA, ptsB, ptsC;
		unsigned cpsA = 0, cpsB = 0, cpsC = 0;
		const bool a = GroomWith( "seg4_seed1", kBase, "", 4, 1, ptsA, cpsA );
		const bool b = GroomWith( "seg4_seed2", kBase, "", 4, 2, ptsB, cpsB );
		const bool c = GroomWith( "seg9_seed1", kBase, "", 9, 1, ptsC, cpsC );

		Check( a && b && c, "the three seed/segments probe grooms all built" );
		if( a && b && c ) {
			Check( cpsA == 4 && cpsC == 9,
			       "MONEY: `segments` reaches the generator through the chunk (4 and 9 control points per strand)" );
			bool same = ( ptsA.size() == ptsB.size() );
			for( size_t i = 0; same && i < ptsA.size(); ++i ) {
				same = ( ptsA[i].x == ptsB[i].x && ptsA[i].y == ptsB[i].y && ptsA[i].z == ptsB[i].z );
			}
			Check( !same, "MONEY: `seed` reaches the generator through the chunk (two seeds give different strands)" );
		}

		// `gravity`.  Two scenes differing in one line; every tip must
		// end up measurably LOWER, and the roots must not move at all
		// (gravity is a styling knob applied after placement).
		std::vector<Point3> ptsG0, ptsG1;
		unsigned cpsG0 = 0, cpsG1 = 0;
		const bool g0 = GroomWith( "grav0", kBase, "\tgravity\t0.0\n", 6, 5, ptsG0, cpsG0 );
		const bool g1 = GroomWith( "grav5", kBase, "\tgravity\t0.5\n", 6, 5, ptsG1, cpsG1 );
		Check( g0 && g1 && ptsG0.size() == ptsG1.size() && cpsG0 == 6,
		       "the two gravity probe grooms built with matching topology" );
		if( g0 && g1 && ptsG0.size() == ptsG1.size() && cpsG0 == 6 ) {
			unsigned notLower = 0, rootsMoved = 0;
			double worstDrop = 0;
			for( size_t i = 0; i < ptsG0.size(); ++i ) {
				const unsigned k = (unsigned)( i % cpsG0 );
				if( k == 0 ) {
					if( Point3Ops::Distance( ptsG0[i], ptsG1[i] ) != 0.0 ) ++rootsMoved;
				} else if( k == cpsG0 - 1 ) {
					const double drop = (double)ptsG0[i].y - (double)ptsG1[i].y;
					if( !( drop > 1e-6 ) ) ++notLower;
					worstDrop = std::max( worstDrop, drop );
				}
			}
			std::cout << "  gravity probe: largest tip drop: " << worstDrop << std::endl;
			Check( rootsMoved == 0, "`gravity` through the chunk never moves a root" );
			Check( notLower == 0,
			       "MONEY: `gravity` reaches the generator through the chunk (every tip is measurably lower)" );
		}

		// `frizz`.  Same construction: one line's difference must show
		// up in the control points, and again not in the roots.
		std::vector<Point3> ptsF0, ptsF1;
		unsigned cpsF0 = 0, cpsF1 = 0;
		const bool f0 = GroomWith( "frizz0", kBase, "\tfrizz\t0.0\n", 6, 5, ptsF0, cpsF0 );
		const bool f1 = GroomWith( "frizz3", kBase, "\tfrizz\t0.3\n", 6, 5, ptsF1, cpsF1 );
		Check( f0 && f1 && ptsF0.size() == ptsF1.size() && cpsF0 == 6,
		       "the two frizz probe grooms built with matching topology" );
		if( f0 && f1 && ptsF0.size() == ptsF1.size() && cpsF0 == 6 ) {
			unsigned rootsMoved = 0;
			double worstDeviation = 0;
			for( size_t i = 0; i < ptsF0.size(); ++i ) {
				const double d = Point3Ops::Distance( ptsF0[i], ptsF1[i] );
				if( (unsigned)( i % cpsF0 ) == 0 ) { if( d != 0.0 ) ++rootsMoved; }
				else                               { worstDeviation = std::max( worstDeviation, d ); }
			}
			std::cout << "  frizz probe: largest deviation : " << worstDeviation << std::endl;
			Check( rootsMoved == 0, "`frizz` through the chunk never moves a root" );
			Check( worstDeviation > 1e-4,
			       "MONEY: `frizz` reaches the generator through the chunk (control points deviate)" );
		}
	}

	// -- negative fixtures.  Each must be REJECTED, with a diagnostic
	//    naming the offending parameter.
	struct Neg { const char* tag; const char* body; const char* mustSay; };
	const std::string missingBase =
		"hair_geometry\n{\n\tname\tg\n\tcount\t10\n\tlength\t0.1\n}\n";
	const std::string unknownBase =
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tno_such_thing\n\tcount\t10\n\tlength\t0.1\n}\n";
	const std::string missingCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tlength\t0.1\n}\n";
	const std::string missingLength = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n}\n";
	const std::string zeroCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t0\n\tlength\t0.1\n}\n";
	const std::string hugeCount = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t9000000\n\tlength\t0.1\n}\n";
	const std::string oneSegment = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\tsegments\t1\n}\n";
	const std::string zeroLength = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0\n}\n";
	const std::string zeroWidth = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\twidth_root\t0\n}\n";
	const std::string curlNoStep = kBase +
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tcc_base\n\tcount\t10\n\tlength\t0.1\n\tcurl_radius\t0.01\n}\n";
	// An infinite plane cannot tessellate, so it cannot carry hair.
	const std::string badBaseKind =
		"infiniteplane_geometry\n{\n\tname\tflat\n}\n"
		"hair_geometry\n{\n\tname\tg\n\tbase_geometry\tflat\n\tcount\t10\n\tlength\t0.1\n}\n";

	const Neg negs[] = {
		{ "missing_base",   missingBase.c_str(),   "base_geometry" },
		{ "unknown_base",   unknownBase.c_str(),   "no_such_thing" },
		{ "missing_count",  missingCount.c_str(),  "count" },
		{ "missing_length", missingLength.c_str(), "length" },
		{ "zero_count",     zeroCount.c_str(),     "count" },
		{ "huge_count",     hugeCount.c_str(),     "count" },
		{ "one_segment",    oneSegment.c_str(),    "segments" },
		{ "zero_length",    zeroLength.c_str(),    "length" },
		{ "zero_width",     zeroWidth.c_str(),     "width_root" },
		{ "curl_no_step",   curlNoStep.c_str(),    "curl_step" },
		{ "bad_base_kind",  badBaseKind.c_str(),   "tessellat" },
	};

	for( size_t i = 0; i < sizeof(negs)/sizeof(negs[0]); ++i ) {
		IJobPriv* job = 0;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (negative)" ); return; }
		std::string captured;
		const bool ok = ParseBodyCapturing( negs[i].tag, negs[i].body, *job, captured );
		Check( !ok, std::string( "negative fixture `" ) + negs[i].tag + "` is REJECTED" );
		Check( captured.find( negs[i].mustSay ) != std::string::npos,
		       std::string( "negative fixture `" ) + negs[i].tag +
		       "`: the diagnostic names `" + negs[i].mustSay + "`" );
		safe_release( job );
	}
}

// ============================================================
//  9. Comb and the root tangent frame
// ============================================================

static void RunComb()
{
	std::cout << "=== 9. Comb and the root tangent frame ===" << std::endl;

	Owned<ITriangleMeshGeometryIndexed> base( MakeUnitQuad() );
	if( !base ) { Check( false, "unit-quad base built" ); return; }

	// The fixture's UV is exactly its position, so dP/du is (1,0,0)
	// everywhere and the surface normal is +Z -- which makes the whole
	// expected strand shape closed-form:
	//
	//     cp(k) = root + (0,0,1) * length * t  +  dWorld * length * t^2
	//
	// with dWorld the comb direction decoded from the painter's RGB.
	// The t^2 weight is the documented tip weighting, and `length` is
	// the documented magnitude scale (a unit-length d sweeps the tip a
	// full strand-length).
	const double kLen = 0.5;					// PlainRecipe's `length`

	struct Case { const char* what; double r, g, b; double ex, ey, ez; };
	const Case cases[] = {
		// red = +1 along dP/du, the surface tangent
		{ "red combs along +dP/du",      1.0, 0.5, 0.5,   1, 0, 0 },
		// green = +1 along the bitangent, N x T = (0,0,1) x (1,0,0) = (0,1,0)
		{ "green combs along the bitangent", 0.5, 1.0, 0.5,   0, 1, 0 },
		// the opposite red combs the opposite way -- a sign flip in the
		// 2*rgb-1 decode inverts this and nothing else
		{ "0 0.5 0.5 combs along -dP/du", 0.0, 0.5, 0.5,  -1, 0, 0 },
	};

	for( size_t ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ++ci )
	{
		const Case& C = cases[ci];
		Owned<IPainter> comb( MakeConstColorPainter( C.r, C.g, C.b ) );
		if( !comb ) { Check( false, "comb painter built" ); return; }

		HairGroomRecipe r = PlainRecipe( base.get(), 400, 6 );
		r.pComb = comb.get();
		Owned<HairGeometry> g( BuildAndRealize( r ) );
		if( !g ) { Check( false, "combed groom built" ); return; }

		double worstErr = 0;
		unsigned int lowerTri = 0, upperTri = 0;
		double minTipAlong = 0;
		bool   haveTip = false;
		for( unsigned int s = 0; s < g->numStrands(); ++s ) {
			const Point3 root = g->ControlPoint( s, 0 );
			// Which of the two triangles this root sits on -- the split
			// runs along y = x.  Both must be populated or the coherence
			// claim below is vacuous.
			( (double)root.y < (double)root.x ? lowerTri : upperTri )++;

			const unsigned int n = g->numControlPointsOfStrand( s );
			for( unsigned int k = 0; k < n; ++k ) {
				const Point3 p = g->ControlPoint( s, k );
				const double t = (double)k / (double)( n - 1 );
				const double bend = kLen * t * t;
				const double expX = C.ex * bend;
				const double expY = C.ey * bend;
				const double expZ = C.ez * bend + kLen * t;		// growth along +Z
				worstErr = std::max( worstErr, fabs( (double)p.x - (double)root.x - expX ) );
				worstErr = std::max( worstErr, fabs( (double)p.y - (double)root.y - expY ) );
				worstErr = std::max( worstErr, fabs( (double)p.z - (double)root.z - expZ ) );
			}
			// The tip's displacement along the intended comb axis, so a
			// sign flip is called out as a SIGN failure, not just as a
			// large residual.
			const Point3 tip = StrandTip( *g, s );
			const double along = ( (double)tip.x - (double)root.x ) * C.ex
			                   + ( (double)tip.y - (double)root.y ) * C.ey;
			if( !haveTip || along < minTipAlong ) { minTipAlong = along; haveTip = true; }
		}
		std::cout << "  [" << C.what << "] worst error: " << worstErr
		          << "  min tip displacement along the comb axis: " << minTipAlong
		          << "  (" << lowerTri << " / " << upperTri << " strands per triangle)" << std::endl;

		Check( lowerTri > 20 && upperTri > 20,
		       std::string( "both triangles of the quad carry strands (" ) + C.what + ")" );
		Check( haveTip && minTipAlong > 0.9 * kLen,
		       std::string( "MONEY: the comb pushes the tip a full strand-length the RIGHT way -- " ) + C.what );
		// One tolerance for every strand on BOTH triangles is what makes
		// this a coherence assertion as well as a magnitude one: an
		// edge-derived tangent frame differs by 45 degrees between the
		// quad's two triangles, so half the strands would miss by
		// ~0.3 * length -- five orders of magnitude outside this bound.
		Check( worstErr < 1e-6,
		       std::string( "MONEY: EVERY strand, on BOTH triangles, is combed by exactly "
		                    "length * t^2 along one shared direction -- " ) + C.what );
	}

	// PURE BLUE IS NOT A COMB.  The encoding borrows the normal map's
	// 2*rgb-1 remap and nothing else: the component along the normal is
	// projected out, so a normal map's `flat` pixel 0.5 0.5 1.0 decodes
	// to d = (0,0,1) and combs NOTHING.  Authors reach for a normal map
	// here, so the behaviour is worth pinning rather than leaving to the
	// descriptor's prose.
	{
		HairGroomRecipe rPlain = PlainRecipe( base.get(), 300, 6 );
		Owned<HairGeometry> uncombed( BuildAndRealize( rPlain ) );

		Owned<IPainter> blue( MakeConstColorPainter( 0.5, 0.5, 1.0 ) );
		Owned<IPainter> grey( MakeConstColorPainter( 0.5, 0.5, 0.5 ) );
		if( !uncombed || !blue || !grey ) { Check( false, "flow-map convention grooms built" ); return; }

		HairGroomRecipe rBlue = rPlain;  rBlue.pComb = blue.get();
		HairGroomRecipe rGrey = rPlain;  rGrey.pComb = grey.get();
		Owned<HairGeometry> gBlue( BuildAndRealize( rBlue ) );
		Owned<HairGeometry> gGrey( BuildAndRealize( rGrey ) );
		if( !gBlue || !gGrey ) { Check( false, "flow-map convention grooms realized" ); return; }

		Check( GroomsIdentical( *uncombed, *gGrey ),
		       "neutral grey (0.5 0.5 0.5) is exactly no comb" );
		Check( GroomsIdentical( *uncombed, *gBlue ),
		       "MONEY: it is a FLOW map, not a normal map -- a normal map's flat pixel "
		       "(0.5 0.5 1.0) combs nothing at all, because the blue channel is projected out" );
	}
}

// ============================================================
//  main
// ============================================================

int main()
{
	std::cout << "===== HairGeneratorTest =====" << std::endl << std::endl;

	RunDeterminism();      std::cout << std::endl;
	RunRootPlacement();    std::cout << std::endl;
	RunDensityMask();      std::cout << std::endl;
	RunLengthAndWidth();   std::cout << std::endl;
	RunClumping();         std::cout << std::endl;
	RunFrizzAndCurl();     std::cout << std::endl;
	RunRealizeContract();  std::cout << std::endl;
	RunChunk();            std::cout << std::endl;
	RunComb();             std::cout << std::endl;

	std::cout << "===== Summary =====" << std::endl;
	std::cout << "checks run:    " << g_checks << std::endl;
	std::cout << "checks failed: " << g_failures << std::endl;

	if( g_failures > 0 ) {
		std::cout << std::endl << "HairGeneratorTest FAILED" << std::endl;
		return 1;
	}
	std::cout << std::endl << "All hair generator tests passed!" << std::endl;
	return 0;
}
